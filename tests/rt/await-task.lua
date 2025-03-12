__rt.main(function()
	do	-- waiting for a task which exits immediately
		local T2 = __rt.spawn_task(function()
			print("T2: exiting")
			return 10, 20, 30
		end)
		print("T1: waiting for T2 to exit")
		local ok, res1, res2, res3 = __rt.await(T2)
		print("T1: await(T2) =>", __rt.await(T2))
		assert(ok)
		assert(res1 == 10)
		assert(res2 == 20)
		assert(res3 == 30)
	end


	do	-- waiting for a task that gets suspended
		local T2 = __rt.spawn_task(function()
			print("T2: sleeping")
			__rt.sleep(10*1000*1000)
			return 10, 20, 30
		end)
		print("T1: waiting for T2 to exit")
		local ok, res1, res2, res3 = __rt.await(T2)
		print("T1: await(T2) =>", __rt.await(T2))
		assert(ok)
		assert(res1 == 10)
		assert(res2 == 20)
		assert(res3 == 30)
	end


	do	-- many tasks waiting for a task to exit
		local T2 = __rt.spawn_task(function()
			print("T2: sleeping")
			__rt.sleep(10*1000*1000)
			-- error("meow meow")
			return 1, 22, 333, 4444, 55555
		end)
		__rt.spawn_task(function()
			print("T3: waiting for T2 to exit")
			print("T3: await(T2) =>", __rt.await(T2))
		end)
		__rt.spawn_task(function()
			print("T4: waiting for T2 to exit")
			print("T4: await(T2) =>", __rt.await(T2))
		end)
		print("T1: waiting for T2 to exit")
		print("T1: await(T2) =>", __rt.await(T2))
		__rt.sleep(10*1000*1000)
	end


	do	-- many tasks waiting for a task to exit
		-- this scenario tests what happens when waiting tasks are stopped
		-- before the waitee task exits
		local T4
		__rt.spawn_task(function()
			print("T2: sleeping")
			__rt.sleep(5*1000*1000)
			print("T2: waiting for T4")
			__rt.await(T4)
		end)
		__rt.spawn_task(function()
			print("T3: sleeping")
			__rt.sleep(5*1000*1000)
			print("T3: waiting for T4")
			__rt.await(T4)
		end)
		T4 = __rt.spawn_task(function()
			print("T4: sleeping")
			__rt.sleep(100*1000*1000)
		end)
		__rt.sleep(10*1000*1000)
		print("T1: exiting")
	end
end)
