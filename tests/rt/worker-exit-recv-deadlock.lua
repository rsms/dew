-- This tests a situation where a task:
-- 1. spawns a worker
-- 2. expects it to produce messages
-- 3. enters recv, waiting for a message
-- 4. worker exits without sending a message
--
-- If a bug is crawling around in the runtime, the task could deadlock.
-- The correct behavior is that a "worker exited" message is sent to the task when
-- the worker exited. The message is sent by the worker (sender=worker).
--
__rt.main(function()
    -- we use task:send -> worker:recv here below to guarantee that the worker is still
    -- alive when the task calls recv()
    local W1 = __rt.spawn_worker(function()
        -- use recv to sync with task
        -- print("W1: main task TID:", __rt.tid())
        local typ, sender_tid = __rt.recv()
        -- print("W1: recv =>", typ, sender_tid)
        print("worker: exiting without send()ing")
    end)
    print("task: worker =", W1)

    -- synchronize with worker task
    __rt.send(W1)

    -- receive "remote task exited" message
    local typ, sender_tid, status = __rt.recv()
    print("task: recv =>", typ, sender_tid, status)
    assert(typ == 4) -- TODO FIXME constant
    if sender_tid ~= __rt.tid(W1) then
        print("sender_tid, __rt.tid(W1) = ", sender_tid, __rt.tid(W1))
    end
    assert(sender_tid == __rt.tid(W1))
end)
