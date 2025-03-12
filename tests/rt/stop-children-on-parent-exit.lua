-- Topic: Structured Concurrency.
-- When a parent task exits, child tasks are stopped
__rt.main(function()
	__rt.spawn_task(function()
		print("task A started; spawning task B & C")
		__rt.spawn_task(function()
			print("task B started")
			__rt.sleep(10*1000*1000)
			print("task B exiting") -- will not get here
		end)
		__rt.spawn_task(function()
			print("task C started")
			__rt.sleep(10*1000*1000)
			print("task C exiting") -- will not get here
		end)
		print("task A exiting")
		-- task B & C will be stopped here
	end)
	-- Wait here for longer than task B or C to showcase that we never see
	-- "task B exiting" or "task C exiting" since they got canceled by task A exiting.
	print("main sleeping")
	__rt.sleep(20*1000*1000)
	print("main exiting")
end)
--[[ output:
task A started; spawning task B & C
task B started
main sleeping
task C started
task A exiting
main exiting
]]
