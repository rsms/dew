__rt.main(function()
    local W1 = __rt.spawn_worker(function()
        local i = 1
        for i = 1, 3 do
            print("W1: recv ...")
            local typ, sender, val1, val2 = __rt.recv()
            print("W1: recv =>", typ, sender, val1, val2)
            assert(val1 == i*10)
            assert(val2 == i*100)

            -- TODO: add support for RemoteTask to send()

            -- echo back
            print("W1: send(T1, " .. (i*10) .. ") ...")
            print("W1: send(T1) =>", __rt.send(sender, i*10, i*100))

            i = i + 1
        end
        -- error("lolcat")
        print("worker exiting")
    end)
    print("W1:", W1)

    -- send messages to worker
    for i = 1, 3 do
        print("T1: send(W1, " .. (i*10) .. ") ...")
        print("T1: send(W1) =>", __rt.send(W1, i*10, i*100))
        print("T1: recv ...")
        local typ, sender, val1, val2 = __rt.recv()
        print("T1: recv =>", typ, sender, val1, val2)
        if typ == 4 then -- 4 == REMOTE_TASK_CLOSED
            print("W1 closed prematurely")
            break
        end
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
