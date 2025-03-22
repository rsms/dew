__rt.main(function()
	-- run a second task that prints "tick" just to show that the main task is not
	-- actually blocking execution on an OS-thread level
	local did_tick = false
	__rt.spawn_task(function()
		__rt.sleep(10*1000*1000)
		did_tick = true
		print("T2: tick")
		__rt.sleep(60*1000*1000)
		print("T2: tick")
	end)

	local ns = 100*1000*1000

	__rt.spawn_task(function()
		print("T2: __rt.syscall_nanosleep() =>", __rt.syscall_nanosleep(ns))
	end)
	__rt.spawn_task(function()
		print("T3: __rt.syscall_nanosleep() =>", __rt.syscall_nanosleep(ns))
	end)
	__rt.spawn_task(function()
		print("T4: __rt.syscall_nanosleep() =>", __rt.syscall_nanosleep(ns))
	end)
	__rt.spawn_task(function()
		print("T5: __rt.syscall_nanosleep() =>", __rt.syscall_nanosleep(ns))
	end)
	__rt.spawn_task(function()
		print("T6: __rt.syscall_nanosleep() =>", __rt.syscall_nanosleep(ns))
	end)

	print("T1: __rt.syscall_nanosleep() ...")
	print("T1: __rt.syscall_nanosleep() 1/2 =>", __rt.syscall_nanosleep(ns))
	assert(did_tick)
	print("T1: __rt.syscall_nanosleep() 2/2 =>", __rt.syscall_nanosleep(ns))
	-- __rt.sleep(10*1000*1000)
	-- print("DONE")

	-- __rt.timer_start(__rt.monotime() + 50000000, 0, 0)
end)
