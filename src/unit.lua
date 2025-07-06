function unit_create_buf(srcfile, src)
	return {
		srcfile = srcfile,
		src = src,
		-- ast = __rt.buf_create(128), -- created by parse_unit
		-- ast_srcmap = {}, -- created by parse_unit
		-- ir = __rt.buf_create(128), -- created by resolve_unit
		-- ir_srcmap = {}, -- created by resolve_unit
		ast_idx = -1, -- AST node representing the unit (unit.ast[unit.ast_idx])
		errcount = 0, -- number of diagnostic errors reported
		include_comments = false, -- used by tokenizer
		-- commentmap = {}, -- idx:[comment] created by parser if include_comments
		-- diag_handler = function(unit, kind, srcpos, msg_format, ...) -- custom callback
	}
end

function unit_create_file(srcfile)
	local f, err = io.open(srcfile)
	if err ~= nil then error(err) end
	local src = f:read("a")
	f:close()
	return unit_create_buf(srcfile, src)
end
