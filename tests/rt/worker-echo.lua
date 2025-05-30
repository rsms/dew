__rt.main(function()
    local W1 = __rt.spawn_worker(function()
        local i = 1
        for i = 1, 3 do
            print("W1: recv ...")
            local typ, sender, val1, val2 = __rt.recv()
            print("W1: recv =>", typ, sender, val1, val2)
            assert(val1 == i*10)
            assert(val2 == i*100)

            -- echo back to sender
            print("W1: send(T1)")
            __rt.send(sender, val1, val2)

            -- i = i + 1
        end
        print("worker exiting")
    end)
    print("W1:", W1)

    for i = 1, 3 do
        -- send message to worker
        print("T1: send(W1)")
        __rt.send(W1, i*10, i*100)

        -- receive reply from worker
        print("T1: recv ...")
        local typ, sender, val1, val2 = __rt.recv()
        print("T1: recv =>", typ, sender, val1, val2)

        -- check results
        if typ == 4 then -- 4 == REMOTE_TASK_CLOSED
            error("W1 closed prematurely")
        end
        assert(val1 == i*10)
        assert(val2 == i*100)
    end

    print("waiting for worker to exit")
    -- we can either recv and check for type=REMOTE_TASK_CLOSED or call await
    -- local typ, _, err = __rt.recv(); assert(typ == 4) -- 4 == REMOTE_TASK_CLOSED
    local _, err = __rt.await(W1)
    if err then
        print("worker exited because of an error:")
        error(err)
    else
        print("worker exited cleanly")
    end
end)
