__rt.main(function()
	__rt.spawn_task(function()
		print("task A yield")
		__rt.yield()
		print("task A resumed")
	end)
	__rt.spawn_task(function()
		print("task B yield")
		__rt.yield()
		print("task B resumed")
	end)
	print("main yield")
	__rt.yield()
	print("main resumed")
end)
--[[ output:
task A yield
task B yield
task A resumed
main yield
task B resumed
main resumed
]]
