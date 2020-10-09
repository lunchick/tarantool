/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2020, Tarantool AUTHORS, please see AUTHORS file.
 */

#include <dlfcn.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include "assoc.h"
#include "diag.h"
#include "error.h"
#include "errinj.h"
#include "fiber.h"
#include "port.h"

#include "box/error.h"
#include "lua/utils.h"
#include "libeio/eio.h"

#include "module_cache.h"

/** Modules name to descriptor hash. */
static struct mh_strnptr_t *mod_hash = NULL;

/**
 * Parse function descriptor from a string.
 */
void
parse_func_name(const char *str, struct func_name_desc *d)
{
	d->package = str;
	d->package_end = strrchr(str, '.');
	if (d->package_end != NULL) {
		/* module.submodule.function => module.submodule, function */
		d->sym = d->package_end + 1; /* skip '.' */
	} else {
		/* package == function => function, function */
		d->sym = d->package;
		d->package_end = str + strlen(str);
	}
}

/**
 * Look up a module in the modules cache.
 */
static struct module *
module_cache_find(const char *name, const char *name_end)
{
	mh_int_t e = mh_strnptr_find_inp(mod_hash, name, name_end - name);
	if (e == mh_end(mod_hash))
		return NULL;
	return mh_strnptr_node(mod_hash, e)->val;
}

/**
 * Save a module to the modules cache.
 */
static int
module_cache_add(struct module *module)
{
	size_t package_len = strlen(module->package);
	const struct mh_strnptr_node_t nd = {
		.str	= module->package,
		.len	= package_len,
		.hash	= mh_strn_hash(module->package, package_len),
		.val	= module,
	};

	if (mh_strnptr_put(mod_hash, &nd, NULL, NULL) == mh_end(mod_hash)) {
		diag_set(OutOfMemory, sizeof(nd), "malloc",
			 "module cache node");
		return -1;
	}
	return 0;
}

/**
 * Delete a module from the modules cache.
 */
static void
module_cache_del(const char *name, const char *name_end)
{
	mh_int_t e = mh_strnptr_find_inp(mod_hash, name, name_end - name);
	if (e != mh_end(mod_hash))
		mh_strnptr_del(mod_hash, e, NULL);
}

/**
 * Arguments for luaT_module_find used by lua_cpcall().
 */
struct module_find_ctx {
	const char *package;
	const char *package_end;
	char *path;
	size_t path_len;
};

/**
 * A cpcall() helper for module_find().
 */
static int
luaT_module_find(lua_State *L)
{
	struct module_find_ctx *ctx = (void *)lua_topointer(L, 1);

	/*
	 * Call package.searchpath(name, package.cpath) and use
	 * the path to the function in dlopen().
	 */
	lua_getglobal(L, "package");
	lua_getfield(L, -1, "search");

	/* Argument of search: name */
	lua_pushlstring(L, ctx->package, ctx->package_end - ctx->package);

	lua_call(L, 1, 1);
	if (lua_isnil(L, -1))
		return luaL_error(L, "module not found");

	/* Convert path to absolute */
	char resolved[PATH_MAX];
	if (realpath(lua_tostring(L, -1), resolved) == NULL) {
		diag_set(SystemError, "realpath");
		return luaT_error(L);
	}

	snprintf(ctx->path, ctx->path_len, "%s", resolved);
	return 0;
}

/**
 * Find a path to a module using Lua's package.cpath.
 *
 * @param ctx module find context
 * @param[out] ctx->path path to shared library.
 * @param[out] ctx->path_len size of @a ctx->path buffer.
 *
 * @return 0 on succes, -1 otherwise, diag is set.
 */
static int
module_find(struct module_find_ctx *ctx)
{
	lua_State *L = tarantool_L;
	int top = lua_gettop(L);
	if (luaT_cpcall(L, luaT_module_find, ctx) != 0) {
		diag_set(ClientError, ER_LOAD_MODULE,
			 (int)(ctx->package_end - ctx->package),
			 ctx->package, lua_tostring(L, -1));
		lua_settop(L, top);
		return -1;
	}
	assert(top == lua_gettop(L)); /* cpcall discard results */
	return 0;
}

/**
 * Load dynamic shared object, ie module library.
 *
 * Create a new symlink based on temporary directory
 * and try to load via this symink to load a dso twice
 * for cases of a function reload.
 */
static struct module *
module_load(const char *package, const char *package_end)
{
	char path[PATH_MAX];
	struct module_find_ctx ctx = {
		.package	= package,
		.package_end	= package_end,
		.path		= path,
		.path_len	= sizeof(path),
	};
	if (module_find(&ctx) != 0)
		return NULL;

	int package_len = package_end - package;
	struct module *module = malloc(sizeof(*module) + package_len + 1);
	if (module == NULL) {
		diag_set(OutOfMemory, sizeof(*module) + package_len + 1,
			 "malloc", "struct module");
		return NULL;
	}
	memcpy(module->package, package, package_len);
	module->package[package_len] = 0;
	rlist_create(&module->mod_sym_head);
	module->calls = 0;

	const char *tmpdir = getenv("TMPDIR");
	if (tmpdir == NULL)
		tmpdir = "/tmp";

	char dir_name[PATH_MAX];
	int rc = snprintf(dir_name, sizeof(dir_name), "%s/tntXXXXXX", tmpdir);
	if (rc < 0 || (size_t)rc >= sizeof(dir_name)) {
		diag_set(SystemError, "failed to generate path to tmp dir");
		goto error;
	}

	if (mkdtemp(dir_name) == NULL) {
		diag_set(SystemError, "failed to create unique dir name: %s",
			 dir_name);
		goto error;
	}

	char load_name[PATH_MAX];
	rc = snprintf(load_name, sizeof(load_name), "%s/%.*s." TARANTOOL_LIBEXT,
		      dir_name, package_len, package);
	if (rc < 0 || (size_t)rc >= sizeof(dir_name)) {
		diag_set(SystemError, "failed to generate path to DSO");
		goto error;
	}

	struct stat st;
	if (stat(path, &st) < 0) {
		diag_set(SystemError, "failed to stat() module %s", path);
		goto error;
	}

	int source_fd = open(path, O_RDONLY);
	if (source_fd < 0) {
		diag_set(SystemError, "failed to open module %s", path);
		goto error;
	}

	int dest_fd = open(load_name, O_WRONLY | O_CREAT | O_TRUNC,
			   st.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO));
	if (dest_fd < 0) {
		diag_set(SystemError, "failed to open file %s for writing ",
			 load_name);
		close(source_fd);
		goto error;
	}

	off_t ret = eio_sendfile_sync(dest_fd, source_fd, 0, st.st_size);
	close(source_fd);
	close(dest_fd);
	if (ret != st.st_size) {
		diag_set(SystemError, "failed to copy DSO %s to %s",
			 path, load_name);
		goto error;
	}

	module->handle = dlopen(load_name, RTLD_NOW | RTLD_LOCAL);
	if (unlink(load_name) != 0)
		say_warn("failed to unlink dso link %s", load_name);
	if (rmdir(dir_name) != 0)
		say_warn("failed to delete temporary dir %s", dir_name);
	if (module->handle == NULL) {
		diag_set(ClientError, ER_LOAD_MODULE, package_len,
			  package, dlerror());
		goto error;
	}

	struct errinj *e = errinj(ERRINJ_DYN_MODULE_COUNT, ERRINJ_INT);
	if (e != NULL)
		++e->iparam;
	return module;

error:
	free(module);
	return NULL;
}

/**
 * Delete a module.
 */
static void
module_delete(struct module *module)
{
	struct errinj *e = errinj(ERRINJ_DYN_MODULE_COUNT, ERRINJ_INT);
	if (e != NULL)
		--e->iparam;
	dlclose(module->handle);
	TRASH(module);
	free(module);
}

/**
 * Check if a module is unused and delete it then.
 */
static void
module_gc(struct module *module)
{
	if (rlist_empty(&module->mod_sym_head) && module->calls == 0)
		module_delete(module);
}

/**
 * Import a function from a module.
 */
static box_function_f
module_sym(struct module *module, const char *name)
{
	box_function_f f = dlsym(module->handle, name);
	if (f == NULL) {
		diag_set(ClientError, ER_LOAD_FUNCTION, name, dlerror());
		return NULL;
	}
	return f;
}

/**
 * Load a new module symbol.
 */
int
module_sym_load(struct module_sym *mod_sym)
{
	assert(mod_sym->addr == NULL);

	struct func_name_desc d;
	parse_func_name(mod_sym->name, &d);

	struct module *module = module_cache_find(d.package, d.package_end);
	if (module == NULL) {
		module = module_load(d.package, d.package_end);
		if (module == NULL)
			return -1;
		if (module_cache_add(module)) {
			module_delete(module);
			return -1;
		}
	}

	mod_sym->addr = module_sym(module, d.sym);
	if (mod_sym->addr == NULL)
		return -1;

	mod_sym->module = module;
	rlist_add(&module->mod_sym_head, &mod_sym->list);
	return 0;
}

/**
 * Unload a module's symbol.
 */
void
module_sym_unload(struct module_sym *mod_sym)
{
	if (mod_sym->module == NULL)
		return;

	rlist_del(&mod_sym->list);
	if (rlist_empty(&mod_sym->module->mod_sym_head)) {
		struct func_name_desc d;
		parse_func_name(mod_sym->name, &d);
		module_cache_del(d.package, d.package_end);
	}
	module_gc(mod_sym->module);

	mod_sym->module = NULL;
	mod_sym->addr = NULL;
}

/**
 * Execute module symbol (run a function).
 */
int
module_sym_call(struct module_sym *mod_sym, struct port *args,
		struct port *ret)
{
	if (mod_sym->addr == NULL) {
		if (module_sym_load(mod_sym) != 0)
			return -1;
	}

	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);

	uint32_t data_sz;
	const char *data = port_get_msgpack(args, &data_sz);
	if (data == NULL)
		return -1;

	port_c_create(ret);
	box_function_ctx_t ctx = {
		.port = ret,
	};

	/*
	 * Module can be changed after function reload. Also
	 * keep in mind that stored C procedure may yield inside.
	 */
	struct module *module = mod_sym->module;
	assert(module != NULL);
	++module->calls;
	int rc = mod_sym->addr(&ctx, data, data + data_sz);
	--module->calls;
	module_gc(module);
	region_truncate(region, region_svp);

	if (rc != 0) {
		if (diag_last_error(&fiber()->diag) == NULL) {
			/* Stored procedure forget to set diag  */
			diag_set(ClientError, ER_PROC_C, "unknown error");
		}
		port_destroy(ret);
		return -1;
	}

	return rc;
}

/**
 * Reload a module and all associated symbols.
 */
int
module_reload(const char *package, const char *package_end,
	      struct module **module)
{
	struct module *old = module_cache_find(package, package_end);
	if (old == NULL) {
		/* Module wasn't loaded - do nothing. */
		*module = NULL;
		return 0;
	}

	struct module *new = module_load(package, package_end);
	if (new == NULL)
		return -1;

	struct module_sym *mod_sym, *tmp;
	rlist_foreach_entry_safe(mod_sym, &old->mod_sym_head, list, tmp) {
		struct func_name_desc d;
		parse_func_name(mod_sym->name, &d);

		mod_sym->addr = module_sym(new, d.sym);
		if (mod_sym->addr == NULL) {
			say_error("module: reload %s, symbol %s not found",
				  package, d.sym);
			goto restore;
		}

		mod_sym->module = new;
		rlist_move(&new->mod_sym_head, &mod_sym->list);
	}

	module_cache_del(package, package_end);
	if (module_cache_add(new) != 0)
		goto restore;

	module_gc(old);
	*module = new;
	return 0;

restore:
	/*
	 * Some old-dso func can't be load from new module,
	 * restore old functions.
	 */
	do {
		struct func_name_desc d;
		parse_func_name(mod_sym->name, &d);
		mod_sym->addr = module_sym(old, d.sym);
		if (mod_sym->addr == NULL) {
			/*
			 * Something strange was happen, an early loaden
			 * function was not found in an old dso.
			 */
			panic("Can't restore module function, "
			      "server state is inconsistent");
		}
		mod_sym->module = old;
		rlist_move(&old->mod_sym_head, &mod_sym->list);
	} while (mod_sym != rlist_first_entry(&old->mod_sym_head,
					      struct module_sym,
					      list));
	assert(rlist_empty(&new->mod_sym_head));
	module_delete(new);
	return -1;
}

/**
 * Initialize modules subsystem.
 */
int
module_init(void)
{
	mod_hash = mh_strnptr_new();
	if (mod_hash == NULL) {
		diag_set(OutOfMemory, sizeof(*mod_hash),
			 "malloc", "modules hash table");
		return -1;
	}
	return 0;
}

/**
 * Free modules subsystem.
 */
void
module_free(void)
{
	while (mh_size(mod_hash) > 0) {
		mh_int_t i = mh_first(mod_hash);
		struct module *m = mh_strnptr_node(mod_hash, i)->val;
		module_gc(m);
	}
	mh_strnptr_delete(mod_hash);
	mod_hash = NULL;
}