return function()
	local T2 = __rt.spawn_task(function()
		while true do
			__rt.recv()
		end
	end)
	local N = 400000
	local time = __rt.time()
	for i = 1, N do
		__rt.send(T2, i)
	end
	time = __rt.time() - time
	print(string.format("Sent %d messages between two tasks: total %.2fms, avg %dns",
	                    N, time / 1000000.0, time // N))
end
