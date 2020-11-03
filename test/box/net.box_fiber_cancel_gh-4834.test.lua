remote = require 'net.box'
fiber = require 'fiber'
test_run = require('test_run').new()

-- #4834: Cancelling fiber doesn't interrupt netbox operations
function infinite_call() fiber.channel(1):get() end
box.schema.func.create('infinite_call')
box.schema.user.grant('guest', 'execute', 'function', 'infinite_call')

error_msg = nil
test_run:cmd("setopt delimiter ';'")
function gh4834()
    local cn = remote.connect(box.cfg.listen)
    local f = fiber.new(function()
        _, error_msg = pcall(cn.call, cn, 'infinite_call')
    end)
    f:set_joinable(true)
    fiber.yield()
    f:cancel()
    f:join()
    cn:close()
end;
test_run:cmd("setopt delimiter ''");
gh4834()
error_msg
box.schema.func.drop('infinite_call')
infinite_call = nil
channel = nil
error_msg = nil
