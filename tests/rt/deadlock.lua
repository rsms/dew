return function()
	-- TODO: detect deadlock, e.g. main task recv() with no timers or io work pending

	__rt.timer_start(1*1000*1000, 0, 0)
	-- this recv should not deadlock since there's a timer
	__rt.recv()

	-- this recv should deadlock
	__rt.recv()
end
