-- tests that send() to a dead worker errors and not deadlocks
require("tests/rt/_testutil")
__rt.main(function()
    local W1 = __rt.spawn_worker(function()
        -- print("worker: exiting")
    end)

    -- wait for the task to exit
    __rt.await(W1)

    -- send should fail
    expect_error("send to dead task", __rt.send, W1)
end)
