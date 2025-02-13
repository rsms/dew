-- When an error occurs in the main task of a worker,
-- the worker is stopped and await(worker) returns false.
return function()
	local worker = __rt.spawn_worker(function()
		error("error in worker's main task")
	end)
	local T2 = __rt.spawn_task(function()
		print("T2: waiting for worker")
		local ok, err = __rt.await(worker)
		print("T2: await(worker) =>", ok, err)
		assert(ok == false)
	end)
	print("T1: worker =>", worker)
	local ok, err = __rt.await(worker)
	print("T1: await(worker) =>", ok, err)
	assert(ok == false)
	__rt.await(T2)
end
