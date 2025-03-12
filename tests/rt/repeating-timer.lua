__rt.main(function()
	local total_delay = 0
	local N = 3
	local start = __rt.monotime()
	local timer = __rt.timer_start(start + 50000000, 50000000, 0) -- every 50ms
	for i = N, 1, -1 do
		__rt.sleep(10000000, 1) -- simulate some slow work that takes a long time (100ms)
		__rt.recv()
		local delay = __rt.monotime() - start
		total_delay = total_delay + delay
		print("recv =>", (delay / 1000000).."ms")
		if i == 1 then
			__rt.timer_stop(timer)
			break
		end
		start = __rt.monotime()
	end
	print("avg interval:", ((total_delay / N) / 1000000).."ms")
end)
