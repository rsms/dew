-- This checks that task identifiers that are recycled gets unique generation numbers.
-- Note: This test relies on the implementation of l_tid, specifically it expects the return
-- value to be encoded as ((u64)t.tid | ((u64)t.tid_gen << 32))
-- fun tid(task T = nil) uint
__rt.main(function()
    local tid2a, tid3a
    local tid2b, tid3b

    -- spawn two tasks, record their TIDs and wait for them to finish
	do
		local T2 = __rt.spawn_task(function() end)
		local T3 = __rt.spawn_task(function() end)
		tid2a = __rt.tid(T2)
		tid3a = __rt.tid(T3)
		__rt.await(T2)
		__rt.await(T3)
	end

	-- GC to ensure that TIDs are recycled
	collectgarbage("collect")

	-- spawn two tasks & record their TIDs.
	-- They should be assigned the same (recycled) TIDs as the previous two tasks.
	do
		local T2 = __rt.spawn_task(function() end)
		local T3 = __rt.spawn_task(function() end)
		tid2b = __rt.tid(T2)
		tid3b = __rt.tid(T3)
	end

    -- print(string.format("tid2a, tid3a = 0x%016x, 0x%016x", tid2a, tid3a))
    -- print(string.format("tid2b, tid3b = 0x%016x, 0x%016x", tid2b, tid3b))

    -- extract t.tid & t.tid_gen
    local gen2a, gen3a = tid2a >> 32, tid3a >> 32
    local gen2b, gen3b = tid2b >> 32, tid3b >> 32
    tid2a, tid3a = tid2a & 0xffffffff, tid3a & 0xffffffff
    tid2b, tid3b = tid2b & 0xffffffff, tid3b & 0xffffffff

    -- print(string.format("tid2a, tid3a = %d, %d", tid2a, tid3a))
    -- print(string.format("tid2b, tid3b = %d, %d", tid2b, tid3b))
    -- print(string.format("gen2a, gen3a = %d, %d", gen2a, gen3a))
    -- print(string.format("gen2b, gen3b = %d, %d", gen2b, gen3b))

    -- t.tid's should match (recycled)
	assert(tid2a == tid2b)
	assert(tid3a == tid3b)

	-- t.tid_gen should differ
	assert(gen2a ~= gen2b)
	assert(gen3a ~= gen3b)
end)
