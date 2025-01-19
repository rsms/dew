function codegen_unit(unit) -- code
	if DEBUG_CODEGEN then dlog("\x1b[1;34mcodegen %s\x1b[0m", unit.srcfile) end
	local ast_stack = unit.ast
	if #ast_stack == 0 then return "" end
	local src = unit.src
	local depth = 0
	local buf = {}
	local function write(s) table.insert(buf, s) end
	local g = {}
	local function codegen(idx, flags)
		if idx == 0 then return end
		if flags == nil then flags = 0 end
		g.idx = idx
		return ast_visit(ast_stack, src, idx, function(n, a, b, c, d)
			depth = depth + 1
			local k = ast_kind(n)
			assert(k ~= 0, "encountered <nothing> AST node")
			local f = ast_nodes[k].codegen
			if f == nil then error(fmt("TODO: %s", astkind_name(k))) end
			f(g, flags, n, a, b, c, d)
			depth = depth - 1
		end)
	end
	g.unit = unit
	g.idx = idx
	g.write = write
	g.ast_node = function(idx) assert(idx ~= nil); return ast_node(ast_stack, idx) end
	g.ast_typeof = function(idx) return ast_typeof(unit, idx) end -- typ_idx, val_idx
	g.ast_str = function(idx) assert(idx ~= nil); return ast_str(ast_stack, idx) end
	g.codegen = codegen
	g.diag_err = function(srcpos, format, ...)
		if srcpos == nil then srcpos = ast_srcpos(ast_stack[g.idx]) end
		return diag(DIAG_ERR, unit, srcpos, format, ...)
	end
	g.diag_warn = function(srcpos, format, ...)
		if srcpos == nil then srcpos = ast_srcpos(ast_stack[g.idx]) end
		return diag(DIAG_WARN, unit, srcpos, format, ...)
	end
	g.diag_info = function(srcpos, format, ...)
		if srcpos == nil then srcpos = ast_srcpos(ast_stack[g.idx]) end
		return diag(DIAG_INFO, unit, srcpos, format, ...)
	end
	codegen(unit.ast_root)
	local code = table.concat(buf, "")
	if DEBUG_CODEGEN then
		print("\x1b[2m——————————————————————————————————————————————————\x1b[0;1m")
		print(code)
		print("\x1b[0;2m——————————————————————————————————————————————————\x1b[0m")
	end
	return code
end


function codegen_intconv(g, src_issigned, src_bits, dst_issigned, dst_bits, src_expr_gen)
	if src_bits == dst_bits --[[and src_issigned == dst_issigned]] then
		return src_expr_gen()
	end
	if not src_issigned and not dst_issigned then
		g.write("(")
		src_expr_gen()
		local mask = (1 << math.min(src_bits, dst_bits)) - 1
		g.write(fmt(" & 0x%x)", mask))
	else
		-- dew.intconv(srcval, src_bits, dst_bits, src_issigned, dst_issigned)
		g.write("dew.intconv(")
		src_expr_gen()
		g.write(fmt(",%d,%d,%s,%s)",
		            src_bits,
		            dst_bits,
		            src_issigned and "true" or "false",
		            dst_issigned and "true" or "false"))
	end
end
