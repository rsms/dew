return function()
	local worker = __rt.spawn_worker(function()
		print("in worker")
		local tasks = {}
		for i = 1, 4 do
			tasks[#tasks + 1] = __rt.spawn_task(function()
				__rt.sleep(10*1000*1000)
			end)
		end
		for _, t in ipairs(tasks) do
			__rt.await(t)
		end
	end)
	local T2 = __rt.spawn_task(function()
		print("T2: waiting for worker")
		local ok = __rt.await(worker)
		assert(ok)
	end)
	print("worker =>", worker)
	local ok = __rt.await(worker)
	assert(ok)
	__rt.await(T2)
end
