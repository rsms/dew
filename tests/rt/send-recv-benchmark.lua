return function()
	-- send_later: set to true to make it so that when we call send(),
	-- the receiver has not yet called recv().
	local send_later = false
	local T2 = __rt.spawn_task(function()
		if send_later then
			__rt.yield() -- give control back to main task
		end
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
