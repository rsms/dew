return function()
	local T2 = __rt.spawn_task(function()
		while true do
			print("T2: recv ...")
			local typ, sender, msg1, msg2 = __rt.recv()
			print("T2: recv =>", typ, sender, msg1, msg2)
		end
	end)
	for i = 1, 4 do
		print("T1: send(T2, " .. (i*10) .. ") ...")
		print("T1: send(T2) =>", __rt.send(T2, i*10, i*100))
	end
end
