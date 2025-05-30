--[[
This checks that task identifiers that are recycled gets unique generation numbers.

Note: This test relies on the implementation of GTID;
GTID encoding:

bit 00000000 011111111112222222222333 33333334 444444444555555555566666
    12345678 901234567890123456789012 34567890 123456789012345678901234
   ┌───────────────────────────────────────────────────────────────────┐
   │                              gtid (64)                            │
   ├─────────────────────────────────┬─────────────────────────────────┤
   │             tid (32)            │             sid (32)            │
   ├────────────────────────┬────────┼────────────────────────┬────────┤
   │         idx (24)       │ gen (8)│         idx (24)       │ gen (8)│
   └────────────────────────┴────────┴────────────────────────┴────────┘

E.g. {s_gen=0, s_idx=1, t_gen=1, t_idx=3} 0x0000000101000003 in little endian:
    00000000 000000000000000000000001 00000001 000000000000000000000011
    └─s_gen┘ └─────────s_idx────────┘ └─t_gen┘ └─────────t_idx────────┘

fun tid(task T = nil) uint
]]
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

    -- expect SID==1 in this test; check and strip
    assert((tid2a >> 32) & 0xffffff == 1); tid2a = tid2a & 0xffffffff
    assert((tid3a >> 32) & 0xffffff == 1); tid3a = tid3a & 0xffffffff
    assert((tid2b >> 32) & 0xffffff == 1); tid2b = tid2b & 0xffffffff
    assert((tid3b >> 32) & 0xffffff == 1); tid3b = tid3b & 0xffffffff

    -- extract t_idx & t_gen from tid
    local gen2a, gen3a = tid2a >> 24, tid3a >> 24
    local gen2b, gen3b = tid2b >> 24, tid3b >> 24
    tid2a, tid3a = tid2a & 0xffffff, tid3a & 0xffffff
    tid2b, tid3b = tid2b & 0xffffff, tid3b & 0xffffff

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
