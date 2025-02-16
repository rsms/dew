return function()
	-- send_later: set to true to make it so that when we call send(),
	-- the receiver has not yet called recv().
	local send_later = true
	local T2 = __rt.spawn_task(function()
		if send_later then
			__rt.yield() -- give control back to main task
		end
		local i = 1
		for i = 1, 8 do
			print("T2: recv ...")
			local typ, sender, msg1, msg2 = __rt.recv()
			print("T2: recv =>", typ, sender, msg1, msg2)
			assert(msg1 == i*10)
			assert(msg2 == i*100)
			i = i + 1
		end
	end)
	for i = 1, 8 do
		print("T1: send(T2, " .. (i*10) .. ") ...")
		print("T1: send(T2) =>", __rt.send(T2, i*10, i*100))
	end
	local ok, err = __rt.await(T2)
	if ok == 0 then error(err) end
end
