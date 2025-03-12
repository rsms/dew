__rt.main(function()
	-- start_timer() schedules a "timer" message to be delivered to the calling task
	-- at a certain point in time
	local wait_until = __rt.monotime() + 50000000
	local timer1 = __rt.timer_start(wait_until, 0, 0)
	__rt.spawn_task(function()
		do
			local start = __rt.monotime()
			local timer = __rt.timer_start(start + 1000000, 0, 0)
			local timer2 = __rt.timer_start(start + 1000000, 0, 0)
			local timer3 = __rt.timer_start(start + 1000000, 0, 0)
			local timer4 = __rt.timer_start(start + 1000000, 0, 0)
			local timer5 = __rt.timer_start(start + 1000000, 0, 0)
			local timer6 = __rt.timer_start(start + 1000000, 0, 0)
			print("timer_start =>", timer)
			print("timer_stop =>", __rt.timer_stop(timer))
			-- collectgarbage("collect")
			-- local what, sender, data = __rt.recv()
			-- print("recv =>", what, sender, data, ((__rt.monotime() - start) / 1000000).."ms")
		end
		-- collectgarbage("collect")
	end)
	-- update the timer to expire 500ms later than we initially asked for
	__rt.timer_update(timer1, wait_until + 50000000, 0, 0)
	print("recv =>", __rt.recv())
end)
