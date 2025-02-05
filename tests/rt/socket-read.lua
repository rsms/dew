--[[
This test requires a local TCP server to be running on port 12345
and write some short ASCII text every few milliseconds.
You can use etc/hello-server.c, just start it like a shell script:
  ./etc/hello-server.c
]]
return function()
	local buf = __rt.buf_alloc(64)
	local fd = __rt.socket(__rt.PF_INET, __rt.SOCK_STREAM)
	__rt.connect(fd, "127.0.0.1:12345")
	while true do
		local n = __rt.read(fd, buf)
		print("read =>", n)
		if n == 0 then break end -- EOF
	end
	print("buf_str =>")
	print(__rt.buf_str(buf))
end
