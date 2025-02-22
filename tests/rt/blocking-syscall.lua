return function()
	-- run a second task that prints "tick" just to show that the main task is not
	-- actually blocking execution on an OS-thread level
	__rt.spawn_task(function()
		while true do
			__rt.sleep(400000000)
			print("T2: tick")
		end
	end)

	__rt.xxx_blocking_syscall()
	__rt.sleep(10*1000*1000)
end
