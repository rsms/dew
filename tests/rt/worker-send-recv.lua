__rt.main(function()
    local W1 = __rt.spawn_worker(function()
        local i = 1
        for i = 1, 3 do
            print("W1: recv ...")
            local typ, sender, val1, val2 = __rt.recv()
            print("W1: recv =>", typ, sender, val1, val2)
            assert(val1 == i*10)
            assert(val2 == i*100)
            i = i + 1
        end
        print("worker exiting")
    end)
    print("W1:", W1)

    -- -- another worker capturing W1 (via closure upvalue) and sending to it
    -- __rt.spawn_worker(function()
    --     __rt.send(W1, 10, 100)
    --     print("W2 exiting")
    -- end)

    -- W2 = worker_open("tcp:127.0.0.1:4242")

    -- Make sure to test runtime logic specific to delivery for a task with RECV status
    -- by sleeping 50ms to make sure worker main task is waiting on recv().
    __rt.sleep(50000000, 0)

    -- send messages to worker
    for i = 1, 3 do
        print("T1: send(W1, " .. (i*10) .. ") ...")
        print("T1: send(W1) =>", __rt.send(W1, i*10, i*100))
    end

    print("waiting for worker to exit")
    local ok, err = __rt.await(W1)
    print("worker exited:", ok, err)
    if not ok then
        error(err)
    end
end)
