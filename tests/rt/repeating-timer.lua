return function()
	local total_delay = 0
	local N = 3
	local start = __rt.time()
	local timer = __rt.timer_start(start + 500000000, 500000000, 0) -- every 500ms
	for i = N, 1, -1 do
		__rt.sleep(100000000, 1) -- simulate some slow work that takes a long time (100ms)
		__rt.recv()
		local delay = __rt.time() - start
		total_delay = total_delay + delay
		print("recv =>", (delay / 1000000).."ms")
		if i == 1 then
			__rt.timer_stop(timer)
			break
		end
		start = __rt.time()
	end
	print("avg delay:", ((total_delay / N) / 1000000).."ms")
end
