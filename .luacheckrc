std = "luajit"
globals = {"box", "_TARANTOOL", "tonumber64"}
ignore = {
    -- Accessing an undefined field of a global variable <debug>.
    "143/debug",
    -- Accessing an undefined field of a global variable <string>.
    "143/string",
    -- Accessing an undefined field of a global variable <table>.
    "143/table",
    -- Unused argument <self>.
    "212/self",
    -- Redefining a local variable.
    "411",
    -- Redefining an argument.
    "412",
    -- Shadowing a local variable.
    "421",
    -- Shadowing an upvalue.
    "431",
    -- Shadowing an upvalue argument.
    "432",
}

include_files = {
    "extra/**/*.lua",
    "extra/dist/tarantoolctl.in",
    "src/**/*.lua",
    "static-build/**/*.lua",
    "test/sql-tap/**/*.lua"
}

exclude_files = {
    "src/box/lua/serpent.lua",
}

files["test/sql-tap/**/*.lua"] = {
    only = {
        -- Setting an undefined global variable.
        "111"
    }
}

files["src/lua/help.lua"] = {
    -- Globals defined for interactive mode.
    globals = {"help", "tutorial"},
}

files["src/lua/init.lua"] = {
    -- Miscellaneous global function definition.
    globals = {"dostring"},
}
files["src/box/lua/console.lua"] = {
    ignore = {
        -- https://github.com/tarantool/tarantool/issues/5032
        "212",
    }
}
