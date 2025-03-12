__rt.main(function()
	-- -- run a second task that prints "tick" just to show that the main task is not
	-- -- actually blocking execution on an OS-thread level
	-- __rt.spawn_task(function()
	-- 	while true do
	-- 		__rt.sleep(400000000)
	-- 		print("T2: tick")
	-- 	end
	-- end)

	__rt.spawn_task(function()
		print("T2: __rt.xxx_blocking_syscall() =>", __rt.xxx_blocking_syscall())
	end)
	__rt.spawn_task(function()
		print("T3: __rt.xxx_blocking_syscall() =>", __rt.xxx_blocking_syscall())
	end)
	__rt.spawn_task(function()
		print("T4: __rt.xxx_blocking_syscall() =>", __rt.xxx_blocking_syscall())
	end)
	__rt.spawn_task(function()
		print("T5: __rt.xxx_blocking_syscall() =>", __rt.xxx_blocking_syscall())
	end)
	__rt.spawn_task(function()
		print("T6: __rt.xxx_blocking_syscall() =>", __rt.xxx_blocking_syscall())
	end)

	print("T1: __rt.xxx_blocking_syscall() 1 =>", __rt.xxx_blocking_syscall())
	print("T1: __rt.xxx_blocking_syscall() 2 =>", __rt.xxx_blocking_syscall())
	-- __rt.sleep(10*1000*1000)
	print("DONE")
end)
