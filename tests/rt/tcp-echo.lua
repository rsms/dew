function echo_client(addr_uri)
	local buf = __rt.buf_create(64)
	local fd = __rt.socket(__rt.AF_INET, __rt.SOCK_STREAM)
	print("connecting to " .. addr_uri)
	__rt.connect(fd, addr_uri)
	while true do
		local n = __rt.read(fd, buf)
		print("read =>", n)
		if n == 0 then break end -- EOF
	end
	print("buf_str =>")
	print(__rt.buf_str(buf))
end

__rt.main(function()
	-- TEST DISABLED because it requires a server that's running, which breaks CI testing.
	-- local T2 = __rt.spawn_task(echo_client, "tcp:127.0.0.1:12345")
	-- __rt.await(T2)
end)
