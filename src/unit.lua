function unit_create_buf(srcfile, src)
	return {
		srcfile = srcfile,
		src = src,
		ast = {},
		ast_root = 0,
		errcount = 0, -- number of diagnostic errors reported
		resmap = {},
	}
end

function unit_create_file(srcfile)
	local f, err = io.open(srcfile)
	if err ~= nil then error(err) end
	local src = f:read("a")
	f:close()
	return unit_create_buf(srcfile, src)
end
