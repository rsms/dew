-- Operator precedence
-- Binary operators of the same precedence associate from left to right.
-- E.g. x / y * z is the same as (x / y) * z.
PREC_MAX   = 7  --  . (member)
PREC_UNARY = 6  --  ++  --  +  -  !  ~  *  &  ?
PREC_BIN5  = 5  --  *  /  %  <<  >>  &
PREC_BIN4  = 4  --  +  -  |  ^
PREC_BIN3  = 3  --  ==  !=  <  <=  >  >=
PREC_BIN2  = 2  --  &&
PREC_BIN1  = 1  --  ||
PREC_MIN   = 0  --  ,

-- flags used with resolve and codegen functions
RVALUE = 1<<0 -- as rvalue

ast_nodes = {} -- keyed by ast kind (k)

function ast_make(N, val24, srcpos)
	-- bit           1111111111222222222233 333333334444444444555555 55556666
	--     01234567890123456789012345678901 234567890123456789012345 67890123
	--     srcpos                           val24                    kind
	--     u32                              u24                      u8
	assert(N ~= nil)
	assert(val24 ~= nil)
	assert(srcpos ~= nil)
	return (N.kind & 0xff) | ((val24 & 0xffffff) << 8) | ((srcpos & 0xffffffff) << 32)
end

function defast(tab)
	local kind_idx = #ast_nodes + 1
	assert(kind_idx <= 0xFF)
	ast_nodes[kind_idx] = tab
	tab.kind = kind_idx
	return tab
end




AST_INT = defast{ name = 'INT', -- int literal, e.g. 123
	span = function(n)
		return ast_get_1val_span(n)
	end,
	visit = function(ast_stack, src, idx, visitor, n)
		return visitor(n, ast_get_1val(ast_stack, idx, n))
	end,
	repr = function(write, fmtnode, n, val)
		return write(fmt(" %d", val))
	end,
	resolve = function (r, flags, idx, n, val)
		local typ_idx = r.ctxtype == TYPE_any and TYPE_int or r.ctxtype
		local typ = r.ast_node(typ_idx)
		if ast_kind(typ) == AST_PRIMTYPE.kind then
			local typ_nbits = ast_primtype_nbits(typ)
			local is_signed = ast_primtype_is_signed(typ)
			-- dlog("AST_INT.resolve> type: %s nbits=%d is_signed=%s",
			--      r.ast_str(typ_idx), typ_nbits, tostring(is_signed))
			if is_signed then
				local smin = -(1 << (typ_nbits - 1))
				local smax = (1 << (typ_nbits - 1)) - 1
				-- dlog("AST_INT.resolve> smin %d 0x%x, smax %d 0x%x, val %d 0x%x",
				--      smin, smin, smax, smax, val, val)
				if val < 0 and val < smin then
					r.diag_err(nil, "integer literal %d overflows %s", val, ast_primtype_name(typ))
				elseif val > 0 and val > smax then
					r.diag_err(nil, "integer literal %d overflows %s", val, ast_primtype_name(typ))
				end
			else
				local umax = (1 << typ_nbits) - 1
				-- dlog("AST_INT.resolve> umax %d 0x%x, val %d 0x%x", umax, umax, val, val)
				-- note: we never get this far with an unsigned integer literal since overflow
				-- will be caught much earlier while parsing the literal.
				if typ_nbits < 64 and (val < 0 or val > umax) then
					r.diag_err(nil, "integer literal %d overflows %s", val, ast_primtype_name(typ))
				end
			end
			return typ_idx
		else
			r.diag_err(nil, "cannot use integer literal for type %s", r.ast_str(typ_idx))
		end

		return TYPE_int
	end,
	codegen = function(g, flags, n, val)
		g.write(val)
	end,
	str = function() return "integer" end,
}

AST_FLOAT = defast{ name = 'FLOAT', -- floating-point literal, e.g. 1.23
	span = function() return 2 end,
	visit = function(ast_stack, src, idx, visitor, n) return visitor(n, ast_stack[idx - 1]) end,
	repr = function(write, fmtnode, n, val) return write(fmt(" %g", val)) end,
	resolve = function (r, flags, idx, n, val) return TYPE_float end,
	codegen = function(g, flags, n, val) g.write(val) end,
	str = function() return "floating-point number" end,
}

AST_REST = defast{ name = 'REST', -- ...
	span = function() return 1 end,
	visit = function(ast_stack, src, idx, visitor, n) return visitor(n) end,
	resolve = function (r, flags, idx, n)
		error("TODO resolve REST")
		return TYPE_void
	end,
	codegen = function(g) g.write("...") end,
	str = function() return "..." end
}

-- LIST is used for modeling concrete AST types like TUPLE and MULTIVAR
AST_LIST = {
	create = function(ast_stack, ast_kind, stack_start, count, srcpos)
		-- Span is precomputed and stored in the top 20 bits of val24.
		-- Small tuples have precomputed count stored in the bottom 4 bits of val24.
		-- Large tuples are counted on the fly. For example:
		--   span=5, count=4
		--   00000000000000000101 0100
		--                      5    4 = embedded count
		--   span=5, count=19
		--   00000000000000000101 1111
		--                      5   15 (0xf) = perform counting
		--
		local span = #ast_stack - stack_start -- span of elements, excluding the tuple itself
		assert(span <= 0xfffff, "list too large")
		local val24 = (span & 0xfffff) << 4
		if count > 0xf then count = 0xf end -- so we can tell if there's precomputed count
		val24 = val24 | count&0xf
		return ast_add(ast_stack, ast_kind, val24, srcpos)
	end,
	count1 = function(ast_stack, idx, n, span)
		-- assert(ast_kind(n) == AST_TUPLE.kind)
		local count = ast_val24(n) & 0xf
		if count == 0xf then
			-- count is larger than fits in flags; count "manually"
			count = 0
			local i, endidx = idx - 1, idx - span
			while i > endidx do
				i = i - ast_stack_span(ast_stack, i)
				count = count + 1
			end
		end
		return count
	end,
	count = function(ast_stack, idx)
		local n = ast_node(ast_stack, idx)
		return AST_LIST.count1(ast_stack, idx, n, AST_LIST.span(n))
	end,
	span = function(n)
		return (ast_val24(n) >> 4) + 1
	end,
	children = function(ast_stack, idx)
		local n = ast_node(ast_stack, idx)
		local span = (ast_val24(n) >> 4) + 1
		return ast_make_seq_iterator(ast_stack, idx, span)()
	end,
	visit = function(ast_stack, src, idx, visitor, n)
		local span = (ast_val24(n) >> 4) + 1
		local function countfun() return AST_LIST.count1(ast_stack, idx, n, span) end
		return visitor(n, countfun, ast_make_seq_iterator(ast_stack, idx, span))
	end,
	repr = function(write, fmtnode, n, countfn, childiter)
		write(fmt(" %d", countfn()))
		for idx in childiter() do
			fmtnode(idx)
		end
	end,
}

AST_TUPLE = defast{ name = 'TUPLE', -- tuple, e.g. (x, 3)
	count = AST_LIST.count, -- function(ast_stack, idx) count
	span = AST_LIST.span,
	visit = AST_LIST.visit,
	repr = AST_LIST.repr,
	resolve = function(r, flags, idx, n, countfn, childiter)
		local idxv = {}
		for idx in childiter() do
			idxv[#idxv + 1] = r.resolve(idx)
		end
		if #idxv == 0 then
			-- empty tuple is void, e.g. parameters "()" of a function
			return TYPE_void
		end
		return AST_TUPLEREFTYPE.create(r.unit.ast, idxv, 0)
	end,
	codegen_vals = function(g, flags, idx)
		-- just values, not including "{" and "}"
		local span = AST_LIST.span(g.unit.ast[idx])
		local childiter = ast_make_seq_iterator(g.unit.ast, idx, span)
		local is_first = true
		for idx in childiter() do
			if is_first then is_first = false else g.write(", ") end
			g.codegen(idx)
		end
	end,
	codegen = function(g, flags, n, countfn, childiter)
		local is_first = true
		local count = countfn()
		if count > 1 then g.write("{") end
		for idx in childiter() do
			if is_first then is_first = false else g.write(", ") end
			g.codegen(idx)
		end
		if count > 1 then g.write("}") end
	end,

	str = function(ast_stack, idx, n)
		local buf = { "(" }
		for child_idx in AST_LIST.children(ast_stack, idx) do
			if #buf > 1 then buf[#buf + 1] = ", " end
			child_idx = AST_ID.unwind(ast_stack, child_idx)
			buf[#buf + 1] = ast_str(ast_stack, child_idx)
		end
		buf[#buf + 1] = ")"
		return table.concat(buf, "")
	end,
}


AST_TUPLETYPE = defast{ name = 'TUPLETYPE',
	-- tuple type, e.g. "(int, float)"
	count = AST_LIST.count, -- function(ast_stack, idx) count
	span = AST_LIST.span,
	visit = AST_LIST.visit,
	repr = AST_LIST.repr,
	resolve = function(r, flags, idx, n, countfn, childiter)
		for idx in childiter() do
			r.resolve(idx)
			-- local _, idx2 = r.resolve(idx)
			-- assert(idx == idx2 or ast_kind(r.ast_node(idx)) == AST_ID.kind,
			--        fmt("%d == %d", idx, idx2))
		end
		return TYPE_type
	end,

	children = function(ast_stack, idx)
		if ast_kind(ast_node(ast_stack, idx)) == AST_TUPLEREFTYPE.kind then
			return AST_TUPLEREFTYPE.child_visitor(ast_stack, idx)
		else
			assert_ast_kind(ast_node(ast_stack, idx), AST_TUPLETYPE)
			return AST_LIST.children(ast_stack, idx)
		end
	end,

	is_type_assignable = function(unit, dst_idx, src_idx) -- bad_idx, seq_index
		-- Note: This function is used by both TUPLETYPE and TUPLEREFTYPE since
		-- src and dst can be either TUPLETYPE or TUPLEREFTYPE.
		local dst = ast_node(unit.ast, dst_idx)
		local src = ast_node(unit.ast, src_idx)

		local dst_kind = ast_kind(dst)
		local src_kind = ast_kind(src)

		assert(dst_kind == AST_TUPLEREFTYPE.kind or dst_kind == AST_TUPLETYPE.kind)
		if src_kind ~= AST_TUPLEREFTYPE.kind and src_kind ~= AST_TUPLETYPE.kind then
			return src_idx, 0
		end

		local dst_next = AST_TUPLETYPE.children(unit.ast, dst_idx)
		local src_next = AST_TUPLETYPE.children(unit.ast, src_idx)

		local seq_index = 1
		while true do
			local dst_child_idx = dst_next()
			local src_child_idx = src_next()
			if dst_child_idx == nil then
				-- fail is dst has more elements than src, or true if they match
				-- ie. return 0 if count match, or bad_idx
				return (src_child_idx == nil and 0 or src_child_idx), seq_index
			end
			if src_child_idx == nil then
				-- dst has fewer elements than src
				return dst_child_idx, seq_index
			end
			dst_child_idx = is_type_assignable(unit, dst_child_idx, src_child_idx)
			if dst_child_idx ~= 0 then
				return dst_child_idx, seq_index -- bad_idx
			end
			seq_index = seq_index + 1
		end
		return 0
	end,

	str = AST_TUPLE.str,
}

AST_TUPLEREFTYPE = defast{ name = 'TUPLEREFTYPE',
	-- tuple type which references existing nodes, e.g. "(int, float)"
	create = function(ast_stack, idxv, srcpos)
		for i = 1, #idxv, 2 do
			local idx2 = i <= #idxv and idxv[i+1] or 0
			ast_push_i32x2(ast_stack, idxv[i], idx2)
		end
		return ast_add(ast_stack, AST_TUPLEREFTYPE, #idxv, srcpos)
	end,
	count = function(n) return ast_val24(n) end,
	child_visitor = function(ast_stack, idx)
		local count = ast_val24(ast_stack[idx])
		local end_idx, i, v1, v2 = idx, 0, 0, 0
		idx = idx - ((count + 1) // 2) -- round up
		return function()
			if i == count then return end
			i = i + 1
			if i % 2 == 1 then
				idx = idx + 1
				v1, v2 = ast_get_i32x2(ast_stack, idx - 1)
				return v1
			else
				return v2 == 0 and nil or v2
			end
		end
	end,
	span = function(n)
		local count = ast_val24(n)
		return 1 + ((count + 1) // 2) -- round up, +1
	end,
	visit = function(ast_stack, src, idx, visitor, n)
		local count = ast_val24(n)
		local childiter = function()
			return AST_TUPLEREFTYPE.child_visitor(ast_stack, idx)
		end
		return visitor(n, count, childiter)
	end,
	resolve = function() return TYPE_type end,
	is_type_assignable = AST_TUPLETYPE.is_type_assignable,
	repr = function(write, fmtnode, n, count, childiter)
		write(fmt(" %d", count))
		for idx in childiter() do
			fmtnode(idx)
		end
	end,
	str = function(ast_stack, idx, n)
		local buf = { "(" }
		for idx in AST_TUPLEREFTYPE.child_visitor(ast_stack, idx) do
			if #buf > 1 then buf[#buf + 1] = ", " end
			buf[#buf + 1] = ast_str(ast_stack, idx)
		end
		buf[#buf + 1] = ")"
		return table.concat(buf, "")
	end,
}


AST_ID = defast{ name = 'ID', -- identifier, e.g. x
	span = function() return 2 end,
	visit = function(ast_stack, src, idx, visitor, n)
		local id_idx = ast_val24(n) + 1
		local target_idx = ast_node(ast_stack, idx - 1)
		return visitor(n, id_idx, target_idx)
	end,
	repr = function(write, fmtnode, n, id_idx, target_idx)
		write(fmt(" %s", id_str(id_idx)))
		if target_idx ~= 0 then
			fmtnode(target_idx)
		end
	end,
	resolve = function(r, flags, idx, n, id_idx)
		local target_idx = r.id_lookup(id_idx)
		-- print("resolve_id", id_str(id_idx), "=>", target_idx)
		if target_idx == nil then
			r.diag_err(ast_srcpos(n), "undefined '%s'", id_str(id_idx))
			return 0
		end
		r.unit.ast[idx - 1] = target_idx
		return r.resolve(target_idx), target_idx
	end,
	codegen = function(g, flags, n, id_idx)
		g.write(id_str(id_idx))
	end,
	str = function() return "identifier" end,
	id_idx = function(n) return ast_val24(n) + 1 end,
	target_idx = function(ast_stack, self_idx) -- idx of target
		return ast_stack[self_idx - 1]
	end,
	unwind = function(ast_stack, self_idx)
		assert(self_idx ~= nil)
		while self_idx ~= 0 and ast_kind(ast_node(ast_stack, self_idx)) == AST_ID.kind do
			self_idx = ast_stack[self_idx - 1] -- AST_ID.target_idx(ast_stack, self_idx)
		end
		return self_idx
	end,
}

AST_PARAM = defast{ name = 'PARAM', -- parameter, e.g. x int
	create = function(ast_stack, id_idx, srcpos)
		assert(id_idx > 0)
		return ast_add(ast_stack, AST_PARAM, id_idx - 1, srcpos)
	end,
	id_idx = function(n)
		return ast_val24(n) + 1
	end,
	span = function(n, ast_stack, idx)
		-- two values on stack:
		-- [idx-1] type
		-- [idx]   param (holds identifier)
		return 1 + ast_stack_span(ast_stack, idx - 1)
	end,
	visit = function(ast_stack, src, idx, visitor, n)
		local id_idx = ast_val24(n) + 1
		local type_idx = idx - 1
		return visitor(n, id_idx, type_idx)
	end,
	repr = function(write, fmtnode, n, id_idx, type_idx)
		if id_idx == ID_REST then
			write(" ...")
		else
			write(fmt(" %s", id_str(id_idx)))
		end
		return fmtnode(type_idx)
	end,
	resolve = function(r, flags, idx, n, id_idx, type_idx)
		_, type_idx = r.resolve(type_idx)
		if id_idx == ID_REST then -- ...
			type_idx = AST_RESTTYPE.create(r.unit.ast, ast_srcpos(n), type_idx)
		end
		-- check to avoid defining names in return types and function types
		if r.define_params and id_idx ~= 0 then
			r.id_define(id_idx, idx)
		end
		return type_idx
	end,
	codegen = function(g, flags, n, id_idx, type_idx)
		if id_idx == ID_REST then
			g.write("...")
		else
			g.write(id_str(id_idx))
		end
		-- ignore type
	end,
	str = function(ast_stack, idx, n)
		local id_idx = ast_val24(n) + 1
		return fmt("parameter '%s'", id_str(id_idx))
	end,
}

AST_VAR = defast{ name = 'VAR', -- variable definition or assignment, e.g. id = expr
	SUBTYPE_VARDEF       = 0, -- 0b00 Definition of a variable
	SUBTYPE_VARASSIGN    = 1, -- 0b01 Assignment to existing variable
	SUBTYPE_SPREADDEF    = 2, -- 0b10 Like VARDEF but has no value, used with AST_SPREADVAR
	SUBTYPE_SPREADASSIGN = 3, -- 0b11 Like VARASSIGN but has no value, used with AST_SPREADVAR

	subtype_is_spread = function(subtype)
		return subtype & 2 == 2
	end,
	subtype_is_assign = function(subtype)
		return subtype & 1 == 1
	end,
	subtype_is_def = function(subtype)
		return subtype & 1 == 0
	end,

	create = function(ast_stack, srcpos, id_idx, subtype)
		-- Note: expects the rvalue to be on top of ast_stack already
		assert((id_idx - 1) <= 0x3fffff, fmt("id_idx %d too large for VAR", id_idx))
		assert(subtype <= 3)
		local val24 = subtype | ((id_idx - 1) << 2)
		return ast_add(ast_stack, AST_VAR, val24, srcpos)
	end,

	id_idx = function(n)
		return (ast_val24(n) >> 2) + 1
	end,

	unpack = function(ast_stack, idx) -- subtype, id_idx, val_idx
		local n = ast_stack[idx]
		local val24 = ast_val24(n)
		local subtype = val24 & 3
		local id_idx = (val24 >> 2) + 1
		local val_idx = (subtype & 2 == 2) and 0 or idx - 1
		return subtype, id_idx, val_idx
	end,

	span = function(n, ast_stack, idx)
		local subtype = ast_val24(n) & 3
		if subtype & 2 == 2 then
			return 1
		end
		local val_idx = idx - 1
		return 1 + ast_stack_span(ast_stack, val_idx)
	end,

	visit = function(ast_stack, src, idx, visitor, n)
		local subtype, id_idx, val_idx = AST_VAR.unpack(ast_stack, idx)
		return visitor(n, subtype, id_idx, val_idx)
	end,

	repr = function(write, fmtnode, n, subtype, id_idx, val_idx)
		if     subtype == AST_VAR.SUBTYPE_VARDEF       then write(" VARDEF")
		elseif subtype == AST_VAR.SUBTYPE_VARASSIGN    then write(" VARASSIGN")
		elseif subtype == AST_VAR.SUBTYPE_SPREADDEF    then write(" SPREADDEF")
		elseif subtype == AST_VAR.SUBTYPE_SPREADASSIGN then write(" SPREADASSIGN")
		end
		write(fmt(" %s", id_str(id_idx)))
		if not AST_VAR.subtype_is_spread(subtype) then
			return fmtnode(val_idx)
		end
	end,

	resolve1 = function(r, idx, id_idx, val_typ_idx)
		local existing_idx = r.id_lookup_or_define(id_idx, idx)
		local n = r.ast_node(idx)
		-- dlog("VAR.resolve1> #%d '%s' => #%s %s",
		--      idx, id_str(id_idx), tostring(existing_idx),
		--      existing_idx == nil and "(none)" or r.ast_str(existing_idx))
		if existing_idx == nil then
			-- variable definition
			-- set subtype (Note: 0xfffffc masks out current subtype)
			ast_val24_set(r.unit.ast, idx, (ast_val24(n) & 0xfffffc) | AST_VAR.SUBTYPE_VARDEF)
			return val_typ_idx
		end
		-- set subtype (Note: 0xfffffc masks out current subtype)
		ast_val24_set(r.unit.ast, idx, (ast_val24(n) & 0xfffffc) | AST_VAR.SUBTYPE_VARASSIGN)
		-- assignment; check types
		local var_typ_idx = r.ast_typeof(existing_idx)
		if var_typ_idx ~= 0 and val_typ_idx ~= 0 then
			local bad_idx = r.is_type_assignable(var_typ_idx, val_typ_idx)
			if bad_idx ~= 0 then
				-- Error: type mismatch.
				-- Note: we check if {left,right}_typ_idx is not zero to avoid false errors
				-- when the type could not be resolved because of another error.
				r.diag_err(r.ast_srcpos(bad_idx),
				           "cannot assign value of type %s to %s of type %s",
				           r.ast_str(val_typ_idx), r.ast_str(existing_idx),
				           r.ast_str(var_typ_idx))
				r.diag_info(ast_srcpos(r.ast_node(existing_idx)),
				            "%s defined here", r.ast_str(existing_idx))
			-- elseif
			--   TODO: check if existing_idx is a constant, which cannot be assigned to
			end
		end
		return TYPE_void -- 'void' since assignment is not an expression
	end,

	resolve = function(r, flags, idx, n, subtype, id_idx, val_idx)
		local val_typ_idx = r.resolve(val_idx, RVALUE)
		return AST_VAR.resolve1(r, idx, id_idx, val_typ_idx)
	end,

	codegen = function (g, flags, n, subtype, id_idx, val_idx)
		if subtype ~= AST_VAR.SUBTYPE_VARASSIGN then
			g.write("local ")
		end
		g.write(id_str(id_idx))
		g.write(" = "); g.codegen(val_idx, RVALUE)
	end,

	str = function(ast_stack, idx, n)
		local subtype, id_idx, _ = AST_VAR.unpack(ast_stack, idx)
		local subtype_str = "variable"
		if subtype == AST_VAR.SUBTYPE_VARASSIGN or subtype == AST_VAR.SUBTYPE_SPREADASSIGN then
			subtype_str = "assignment to"
		end
		return fmt("%s '%s'", subtype_str, id_str(id_idx))
	end,
}


AST_SPREADVAR = defast{ name = 'SPREADVAR', -- tuple, e.g. "x, y, z = expr"
	create = function(ast_stack, stack_start, count, srcpos)
		return AST_LIST.create(ast_stack, AST_SPREADVAR, stack_start, count, srcpos)
	end,

	varcount = function(ast_stack, n, idx, span)
		local count = ast_val24(n) & 0xf
		if count == 0xf then
			-- count is larger than fits in flags; count "manually".
			-- Note: we start counting vars at idx-2 since idx-1 is the rvalue.
			count = 0
			local i, endidx = idx - 2, idx - span
			while i > endidx do
				i = i - ast_stack_span(ast_stack, i)
				count = count + 1
			end
		end
		return count
	end,

	span = function(n)
		return (ast_val24(n) >> 4) + 1
	end,

	visit = function(ast_stack, src, idx, visitor, n)
		local span = (ast_val24(n) >> 4) + 1
		local varcount = AST_SPREADVAR.varcount(ast_stack, n, idx, span)
		local val_idx = idx - varcount - 1
		local function variter()
			local var_idx = idx - varcount
			return function()
				if var_idx < idx then
					var_idx = var_idx + 1
					return var_idx - 1
				end
			end
		end
		return visitor(n, val_idx, varcount, variter)
	end,

	repr = function(write, fmtnode, n, val_idx, varcount, variter)
		for idx in variter() do
			fmtnode(idx)
		end
		fmtnode(val_idx)
	end,

	resolve = function(r, flags, idx, n, val_idx, varcount, variter)
		-- resolve rvalue expression (first child of SPREADVAR)
		local tuple_typ_idx = 0
		tuple_typ_idx, val_idx = r.resolve(val_idx, RVALUE)
		local tuple_typ = r.ast_node(tuple_typ_idx)

		-- expect a tuple type
		local next_child = variter()
		if ast_kind(tuple_typ) ~= AST_TUPLEREFTYPE.kind then
			-- Not a tuple type.
			-- Treat this scenario as missing value for 2nd var, e.g. "x, y = 1"
			next_child()
			local id_idx = AST_VAR.id_idx(r.unit.ast[next_child()])
			r.diag_err(srcpos_after(r.ast_srcpos(val_idx)),
			           "missing value for '%s'", id_str(id_idx))
			return TYPE_void
		end

		-- check arity
		local valcount = AST_TUPLEREFTYPE.count(tuple_typ)
		if valcount < varcount then
			r.diag_err(r.ast_srcpos(val_idx),
			           "not enough values from expression of type %s " ..
			           "in assignment to %d variables",
			           r.ast_str(tuple_typ_idx), varcount)
			return TYPE_void
		elseif valcount > varcount then
			r.diag_err(r.ast_srcpos(val_idx),
			           "too many values from expression of type %s " ..
			           "in assignment to %d variables",
			           r.ast_str(tuple_typ_idx), varcount)
			return TYPE_void
		end

		-- define variables
		local tuple_typ_next_child = AST_TUPLEREFTYPE.child_visitor(r.unit.ast, tuple_typ_idx)
		while true do
			local var_idx = next_child()
			if var_idx == nil then break end
			local typ_idx = tuple_typ_next_child()
			local id_idx = AST_VAR.id_idx(r.unit.ast[var_idx])
			local existing_idx = r.id_lookup_or_define(id_idx, var_idx)
			if existing_idx ~= nil then
				-- variable assignment; set subtype (Note: 0xfffffc masks out current subtype)
				local n = r.ast_node(var_idx)
				local new_val24 = (ast_val24(n) & 0xfffffc) | AST_VAR.SUBTYPE_SPREADASSIGN
				ast_val24_set(r.unit.ast, var_idx, new_val24)
			end
			r.mark_resolved(var_idx, typ_idx, var_idx)
		end

		return TYPE_void
	end,

	codegen = function(g, flags, n, val_idx, varcount, variter)
		-- generate left-hand side
		AST_MULTIVAR.codegen_LHS(g, varcount, variter)

		-- generate right-hand side value
		local val = g.ast_node(val_idx)
		local val_kind = ast_kind(val)
		if val_kind == AST_TUPLE.kind then
			-- TUPLE literal: directly use its values
			-- i.e. source      "a, b = (1, 2)"
			--      becomes     "a, b = 1, 2"
			--      rather than "a, b = table.unpack({1, 2})"
			AST_TUPLE.codegen_vals(g, RVALUE, val_idx)
		elseif val_kind == AST_CALL.kind then
			-- function call
			g.codegen(val_idx, RVALUE)
		else
			-- tuples are implemented as Lua arrays, i.e. "(1, 2, 3)" becomes "{1, 2, 3}"
			g.write("table.unpack("); g.codegen(val_idx, RVALUE); g.write(")")
		end
	end,
}


AST_MULTIVAR = defast{ name = 'MULTIVAR', -- tuple, e.g. "x, y, z = 1, 2, 3"
	span = AST_LIST.span,
	visit = AST_LIST.visit,
	repr = AST_LIST.repr,
	resolve = function(r, flags, idx, n, countfn, childiter)
		-- Resolving multivar assignment is done in two passes:
		--   1. resolve all rvalues
		--   2. process VAR nodes and check types
		-- We begin by checking if the right-hand side is a single expression to be "spread."
		assert(countfn() > 1) -- assume that there are always at least two names
		local next_child = childiter()
		local child_idx = next_child(); assert_ast_kind(r.ast_node(child_idx), AST_VAR)
		local _, id_idx, val_idx = AST_VAR.unpack(r.unit.ast, child_idx)
		local val_typ_idx = r.resolve(val_idx, RVALUE)
		child_idx = next_child(); assert_ast_kind(r.ast_node(child_idx), AST_VAR)
		-- multiple individual values, e.g. "a, b, c = 1, 2, 3"
		while child_idx ~= nil do
			assert_ast_kind(r.ast_node(child_idx), AST_VAR)
			_, id_idx, val_idx = AST_VAR.unpack(r.unit.ast, child_idx)
			r.resolve(val_idx)
			child_idx = next_child()
		end
		for child_idx in childiter() do
			_, id_idx, val_idx = AST_VAR.unpack(r.unit.ast, child_idx)
			val_typ_idx = r.ast_typeof(val_idx)
			AST_VAR.resolve1(r, child_idx, id_idx, val_typ_idx)
			r.mark_resolved(child_idx, val_typ_idx, child_idx)
		end
		-- assignment is a statement ('void' type)
		return TYPE_void
	end,
	codegen_LHS = function(g, varcount, variter)
		-- Note: This function is shared by AST_MULTIVAR and AST_SPREADVAR
		local is_first = true
		local defcount = 0

		-- define new variables
		for idx in variter() do
			local subtype, id_idx, _ = AST_VAR.unpack(g.unit.ast, idx)
			if AST_VAR.subtype_is_def(subtype) then
				if is_first then
					is_first = false
					g.write("local ")
				else
					g.write(", ")
				end
				g.write(id_str(id_idx))
				defcount = defcount + 1
			end
		end

		-- assignment statement is used if there's at least one assignment
		if defcount < varcount then
			if defcount > 0 then
				g.write("; ")
			end
			is_first = true
			for idx in variter() do
				local _, id_idx, _ = AST_VAR.unpack(g.unit.ast, idx)
				if is_first then is_first = false else g.write(", ") end
				g.write(id_str(id_idx))
			end
		end

		g.write(" = ")
	end,
	codegen = function(g, flags, n, countfn, childiter)
		-- Note: since we support mixing assignments with variable definitions in one
		-- multi-assigment statement (which lua does not support) we have to take care
		-- here to avoid overwriting values used on both sides. For example:
		--
		--   x, y = 1, 2
		--   x, y, z = y, x, y
		--
		-- The last statement of this example swaps the values of x and y,
		-- and assigns the "old" value of y to z (ie. assigns 2 to z, not 1).
		-- If we are not careful we might generate code that assigns the "new" value
		-- of y (1) to z, which would be incorrect.
		--
		-- Because newly declared variables can't be used on the right-hand side,
		-- we can guarantee the correct semantics by first defining all variables and then,
		-- as a second step, assigning to existing variables.
		AST_MULTIVAR.codegen_LHS(g, countfn(), childiter)
		local is_first = true
		for idx in childiter() do
			local _, _, val_idx = AST_VAR.unpack(g.unit.ast, idx)
			if is_first then is_first = false else g.write(", ") end
			g.codegen(val_idx, RVALUE)
		end
	end,
}

AST_RETURN = defast{ name = 'RETURN', -- return statement
	span = function(n, ast_stack, idx)
		if ast_val24(n) == 0 then return 1 end -- no value, just "return;"
		return 1 + ast_stack_span(ast_stack, idx - 1)
	end,
	visit = function(ast_stack, src, idx, visitor, n)
		local val_idx = ast_val24(n) == 0 and 0 or idx - 1
		return visitor(n, val_idx)
	end,
	repr = function(write, fmtnode, n, val_idx)
		return fmtnode(val_idx)
	end,
	resolve = function(r, flags, idx, n, val_idx)
		-- resolve value
		local val_typ_idx
		val_typ_idx, val_idx = r.resolve(val_idx, RVALUE)

		-- check that we are inside a function
		if r.fun_idx == 0 then
			r.diag_err(nil, "'return' outside of function")
			return val_typ_idx
		end

		-- check function return type
		assert(r.fun_typ_idx ~= 0)
		local _, ret_typ_idx = AST_FUNTYPE.elems(r.unit.ast, r.fun_typ_idx)
		if (ret_typ_idx == 0 and val_typ_idx ~= 0) or (ret_typ_idx ~= 0 and val_typ_idx == 0) then
			AST_RETURN.report_type_error(r, val_idx, val_typ_idx, ret_typ_idx, 0, 0)
		else
			local bad_idx, seq_index = r.is_type_assignable(ret_typ_idx, val_typ_idx)
			if bad_idx ~= 0 then
				AST_RETURN.report_type_error(r, val_idx, val_typ_idx, ret_typ_idx,
				                             bad_idx, seq_index)
			end
		end

		return val_typ_idx
	end,

	report_type_error = function(r, val_idx, val_typ_idx, ret_typ_idx, bad_idx, seq_index)
		local have_count = multitype_count(r.unit.ast, val_typ_idx)
		local want_count = multitype_count(r.unit.ast, ret_typ_idx)
		if have_count ~= want_count then
			local have_str = multitype_str(r.unit.ast, val_typ_idx, have_count)
			local want_str = multitype_str(r.unit.ast, ret_typ_idx, want_count)
			r.diag_err(nil, "%s return values\n  have %s\n  want %s",
			           want_count < have_count and "too many" or "not enough",
			           have_str, want_str)
		elseif seq_index == 0 or have_count == 1 then
			assert(bad_idx ~= 0)
			r.diag_err(r.ast_srcpos(bad_idx),
			           "cannot use value of type %s as %s in return statement",
			           r.ast_str(val_typ_idx), r.ast_str(ret_typ_idx))
		else
			local val_next = AST_TUPLETYPE.children(r.unit.ast, val_idx)
			local want_next = AST_TUPLETYPE.children(r.unit.ast, ret_typ_idx)
			local val_child_idx, want_child_idx
			while seq_index > 0 do
				val_child_idx = val_next()
				want_child_idx = AST_ID.unwind(r.unit.ast, want_next())
				seq_index = seq_index - 1
			end
			r.diag_err(r.ast_srcpos(val_child_idx),
			           "cannot use value of type %s as %s in return statement",
			           r.ast_str(r.ast_typeof(val_child_idx)), r.ast_str(want_child_idx))
		end
	end,

	codegen = function(g, flags, n, val_idx)
		if val_idx == 0 then
			return g.write("return")
		end
		g.write("return ")
		-- special case for tuple literal (TUPLE) which is returned as multiple values.
		-- E.g. "return x, y" is generated as "return x, y" not "return {x, y}"
		if ast_kind(g.ast_node(val_idx)) == AST_TUPLE.kind then
			return AST_TUPLE.codegen_vals(g, RVALUE, val_idx)
		else
			return g.codegen(val_idx, RVALUE)
		end
	end,
}

AST_NREF = defast{ name = 'NREF', -- node reference
	create = function(ast_stack, target_idx, srcpos)
		return ast_make(AST_NREF, target_idx, srcpos)
	end,
	target = function(n)
		return ast_val24signed(n)
	end,
	update = function(ast_stack, idx, new_target_idx)
		return ast_val24_set(ast_stack, idx, new_target_idx)
	end,
	-- this is special as its resolved automatically; visitor callbacks don't ever see it
	span = function() return 1 end,
	visit = function(ast_stack, src, idx, visitor, n)
		idx = AST_NREF.target(n)
		return ast_visit(ast_stack, src, idx, visitor)
	end,
}

AST_PREFIXOP = defast{ name = 'PREFIXOP', -- prefix operation, e.g. -n
	span = function(n, ast_stack, idx)
		return 1 + ast_stack_span(ast_stack, idx - 1)
	end,
	visit = function(ast_stack, src, idx, visitor, n)
		return visitor(n, idx - 1, ast_val24(n))
	end,
	repr = function(write, fmtnode, n, operand_idx, tok)
		write(fmt(" %s", tokname(tok)))
		return fmtnode(operand_idx)
	end,
	resolve = function(r, flags, idx, n, operand_idx, tok)
		local operand_type_idx = r.resolve(operand_idx, RVALUE)
		local operand = r.ast_node(operand_idx)
		if tok == TOK_MINUS and ast_kind(operand) == AST_INT.kind then
			-- check range of integer literal
			-- Note: we can simply check if the value is <0 since lua integers are signed.
			local intval = ast_get_1val(r.unit.ast, operand_idx, operand)
			if intval < 0 then
				r.diag_err(r.ast_srcpos(operand_idx),
				           "integer literal %d overflows int64", intval)
			end
		end
		return operand_type_idx
	end,
	codegen = function(g, flags, n, operand_idx)
		local op = ast_val24(n)
		g.write(tokname(op))
		g.codegen(operand_idx, RVALUE)
	end,
}

AST_BINOP = defast{ name = 'BINOP', -- binary operation
	span = function(n, ast_stack, idx)
		-- note: we can ignore 'right' operand since it's always just above binop
		-- in the stack and 'left' operand is always further up
		local left_idx, _ = ast_get_1op(ast_stack, idx, n)
		assert(left_idx > 0 and left_idx < idx, fmt("left_idx=%d idx=%d", left_idx, idx))
		local left_span = ast_stack_span(ast_stack, left_idx)
		return idx - (left_idx - left_span)
	end,
	visit = function(ast_stack, src, idx, visitor, n)
		local left_idx, self_span = ast_get_1op(ast_stack, idx, n)
		-- 'right' operand is directly above binop in stack,
		-- so subtract span of n from idx to get stack index of 'right' operand
		local right_idx = idx - self_span
		return visitor(n, left_idx, right_idx)
	end,
	repr = function(write, fmtnode, n, left_idx, right_idx)
		write(fmt(" %s", tokname(ast_val8(n))))
		fmtnode(left_idx)
		return fmtnode(right_idx)
	end,
	resolve = function(r, flags, idx, n, left_idx, right_idx)
		local lt_idx = r.resolve(left_idx, RVALUE)
		local rt_idx = r.resolve(right_idx, RVALUE)
		if lt_idx ~= rt_idx then
			if lt_idx ~= 0 and rt_idx ~= 0 then
				r.diag_err(ast_srcpos(n),
				           "invalid operation; mismatched types %s and %s" ..
				           " (use an explicit cast)",
				           ast_str(r.unit.ast, lt_idx), ast_str(r.unit.ast, rt_idx))
			end
		end
		local op = ast_val8(n) -- a TOK_ constant
		-- TODO: check if op is defined for type
		return lt_idx
	end,
	codegen = function(g, flags, n, left_idx, right_idx)
		g.write("("); g.codegen(left_idx, RVALUE); g.write(" ")
		local op = ast_val8(n) -- a TOK_ constant
		if op == TOK_SLASH then
			g.write('//') -- intdiv is different in Lua
		else
			g.write(tokname(op))
		end
		g.write(" "); g.codegen(right_idx, RVALUE); g.write(")")
	end,
}

AST_ASSIGN = defast{ name = 'ASSIGN', -- assignment operation, e.g. "x.y = z"
	-- note: variable assignment is done via AST_VAR, never AST_ASSIGN
	span = AST_BINOP.span,
	visit = AST_BINOP.visit,
	repr = function(write, fmtnode, n, left_idx, right_idx)
		fmtnode(left_idx)
		return fmtnode(right_idx)
	end,
	resolve = function(r, flags, idx, n, left_idx, right_idx)
		local right_typ_idx = r.resolve(right_idx, RVALUE)
		local left_typ_idx = r.resolve(left_idx, RVALUE)
		local bad_idx = r.is_type_assignable(left_typ_idx, right_typ_idx)
		if bad_idx ~= 0 then
			r.diag_err(r.ast_srcpos(bad_idx),
			           "cannot assign value of type %s to %s of type %s",
			           r.ast_str(right_typ_idx),
			           r.ast_str(left_idx),
			           r.ast_str(left_typ_idx) )
		end
		return TYPE_void -- assignment is not an expression
	end,
	codegen = function(g, flags, n, left_idx, right_idx)
		g.codegen(left_idx); g.write(" = "); g.codegen(right_idx, RVALUE)
	end,
}

AST_BLOCK = defast{ name = 'BLOCK', -- block, e.g. { ... }
	span = function(n)
		-- Span is precomputed and stored in val24
		return ast_val24(n)
	end,
	visit = function(ast_stack, src, idx, visitor, n)
		local span = ast_val24(n)
		return visitor(n, ast_make_seq_iterator(ast_stack, idx, span))
	end,
	repr = function(write, fmtnode, n, childiter)
		for idx in childiter() do
			fmtnode(idx)
		end
	end,
	resolve = function(r, flags, idx, n, childiter)
		local t = TYPE_void
		for idx in childiter() do
			t = r.resolve(idx)
		end
		if flags&RVALUE == 0 then
			t = TYPE_void
		end
		return t
	end,
	codegen = function(g, flags, n, childiter)
		local is_first = true
		for idx in childiter() do
			if is_first then is_first = false else g.write("\n") end
			g.codegen(idx)
		end
	end,
}

AST_CALL = defast{ name = 'CALL', -- call, e.g. x(y, z)
	span = function(n, ast_stack, idx)
		local recv_idx, _ = ast_get_1op(ast_stack, idx, n)
		assert(recv_idx > 0 and recv_idx < idx)
		local left_span = ast_stack_span(ast_stack, recv_idx)
		return idx - (recv_idx - left_span)
	end,
	visit = function(ast_stack, src, idx, visitor, n)
		local recv_idx, self_span = ast_get_1op(ast_stack, idx, n)
		local args_idx = idx - self_span -- args is directly above CALL node in stack
		-- recv_idx = AST_ID.unwind(ast_stack, recv_idx)
		return visitor(n, recv_idx, args_idx)
	end,
	repr = function(write, fmtnode, n, recv_idx, args_idx)
		fmtnode(recv_idx)
		return fmtnode(args_idx)
	end,
	resolve = function(r, flags, idx, n, recv_idx, args_idx)
		local recv_t, recv_idx_canonical = r.resolve(recv_idx, RVALUE)
		if recv_t == TYPE_type then
			-- calling a type
			-- use recv_idx_canonical which has had an IDentifiers resolved
			r.ctxtype_push(recv_idx_canonical)
		end
		local args_t = r.resolve(args_idx)
		if recv_t == TYPE_type then
			r.ctxtype_pop()
			return recv_idx_canonical
		end
		dlog("TODO: AST_CALL.resolve: check arguments")
		return TYPE_void -- TODO
	end,
	codegen = function(g, flags, n, recv_idx, args_idx)
		local recv_idx_canonical = AST_ID.unwind(g.unit.ast, recv_idx)
		if ast_is_primtype(recv_idx_canonical) then
			return AST_CALL.codegen_primtype(g, flags, n, recv_idx_canonical, args_idx)
		end
		local recv_typ_idx = g.ast_typeof(recv_idx)
		if recv_typ_idx == TYPE_type then
			return AST_CALL.codegen_type(g, flags, n, recv_idx, args_idx)
		end
		-- regular function call
		-- print("CALL.codegen>", g.ast_str(recv_idx), g.ast_str(recv_typ_idx))
		g.codegen(recv_idx, RVALUE)
		g.write("("); g.codegen(args_idx); g.write(")")
	end,
	codegen_type = function(g, flags, n, recv_idx, args_idx)
		-- TODO: complex types, e.g. struct
		if flags & RVALUE ~= 0 then
			g.write("("); g.codegen(args_idx); g.write(")")
		else
			g.codegen(args_idx)
		end
	end,
	codegen_primtype = function(g, flags, n, dst_typ_idx, src_idx)
		local src_typ_idx = g.ast_typeof(src_idx)

		-- matching types requires no conversion
		if src_typ_idx == dst_typ_idx then
			return g.codegen(src_idx, flags)
		end

		-- convert src_typ -> dst_typ
		local dst_typ = g.ast_node(dst_typ_idx)
		local src_typ = g.ast_node(src_typ_idx)
		if ast_is_primtype_float(dst_typ_idx) then
			if ast_is_primtype_int(src_typ_idx) then
				-- int -> float
				if ast_primtype_is_signed(src_typ) then
					g.write("("); g.codegen(src_idx, RVALUE); g.write("+0.0)")
					return
				end
				print("TODO: handle unsigned int to float conversion")
				-- TODO e.g. uint(0x7fffffffffffffff+1) is treated as -9223372036854775808 by lua
				-- since lua treats integers as signed.
			end
		elseif ast_is_primtype_int(dst_typ_idx) then
			-- int to int conversion has two dimensions:
			-- size -> size, e.g. i32 -> i16, i12 -> i13
			-- sign -> sign, e.g. i32 -> u32, u16 -> i16
			-- combinations, e.g. i32 -> u64, i32 -> u16
			local src_bits = ast_primtype_nbits(src_typ)
			local dst_bits = ast_primtype_nbits(dst_typ)
			local src_issigned = ast_primtype_is_signed(src_typ)
			local dst_issigned = ast_primtype_is_signed(dst_typ)

			-- conversion of source type to integer (Lua integer, i64)
			local gen_intval
			if ast_is_primtype_int(src_typ_idx) then
				-- just int
				return codegen_intconv(g, src_issigned, src_bits, dst_issigned, dst_bits,
				                       function()
				                       	return g.codegen(src_idx, RVALUE)
				                       end)
			elseif ast_is_primtype_float(src_typ_idx) then
				-- float -> int
				src_typ = builtin_ast_stack[-TYPE_int]
				src_bits = ast_primtype_nbits(src_typ)
				-- codegen_intconv(g, src_issigned, src_bits, dst_issigned, dst_bits, src_expr_gen)
				return codegen_intconv(g, src_issigned, src_bits, dst_issigned, dst_bits,
				                       function()
				                       	g.write("select(1,math.modf(")
				                       	g.codegen(src_idx, RVALUE)
				                       	g.write("))")
				                       end)
			elseif src_typ_idx == TYPE_any then
				-- any -> int
				src_typ = builtin_ast_stack[-TYPE_int]
				src_bits = ast_primtype_nbits(src_typ)
				src_issigned = true
				return codegen_intconv(g, src_issigned, src_bits, dst_issigned, dst_bits,
				                       function()
				                       	g.write("select(1,math.modf(")
				                       	g.codegen(src_idx, RVALUE)
				                       	g.write("))")
				                       end)
			end
		end
		-- fail
		src_idx = AST_ID.unwind(g.unit.ast, src_idx)
		g.diag_err(nil, "cannot convert %s of type %s to type %s",
		           g.ast_str(src_idx), g.ast_str(src_typ_idx), g.ast_str(dst_typ_idx))
		g.write("nil")
	end,
}

AST_FUN = defast{ name = 'FUN', -- function declaration or definition, e.g. fun f(x, y t) t
	-- flags (8 bits)
	HAS_NAME    = 1<<0,
	HAS_PARAMS  = 1<<1,
	HAS_RETTYPE = 1<<2,
	HAS_BODY    = 1<<3,
	AS_TYPE     = 1<<4, -- expresses a function type, not a prototype or implementation
	span = function(n, ast_stack, idx)
		local span = 1 -- fun node itself
		local flags = ast_val24(n)
		if flags&AST_FUN.HAS_BODY ~= 0 then
			span = span + ast_stack_span(ast_stack, idx - span)
		end
		if flags&AST_FUN.HAS_RETTYPE ~= 0 then
			span = span + ast_stack_span(ast_stack, idx - span)
		end
		if flags&AST_FUN.HAS_PARAMS ~= 0 then
			span = span + ast_stack_span(ast_stack, idx - span)
		end
		if flags&AST_FUN.HAS_NAME ~= 0 and (flags >> 8) == 0 then
			-- name is at the end of the function's AST stack range (since its parsed first)
			span = span + 1
		end
		return span
	end,
	visit = function(ast_stack, src, idx, visitor, n)
		local flags = ast_val24(n)
		local idx2 = idx - 1
		local params_idx, ret_idx, body_idx = 0, 0, 0
		if flags&AST_FUN.HAS_BODY ~= 0 then
			body_idx = idx2
			idx2 = idx2 - ast_stack_span(ast_stack, idx2)
		end
		if flags&AST_FUN.HAS_RETTYPE ~= 0 then
			ret_idx = idx2
			idx2 = idx2 - ast_stack_span(ast_stack, idx2)
		end
		if flags&AST_FUN.HAS_PARAMS ~= 0 then
			params_idx = idx2
			idx2 = idx2 - ast_stack_span(ast_stack, idx2)
		end
		local name_id_idx = flags >> 8 -- name idx (0 if anonymous)
		if flags&AST_FUN.HAS_NAME ~= 0 and name_id_idx == 0 then
			-- name ID idx in separate stack entry, at the top of the FUN stack
			name_id_idx = ast_val24(ast_stack[idx2]) + 1
		end
		local as_type = flags&AST_FUN.AS_TYPE ~= 0
		return visitor(n, name_id_idx, params_idx, ret_idx, body_idx, as_type)
	end,
	repr = function(write, fmtnode, n, name_id_idx, params_idx, ret_idx, body_idx, as_type)
		if name_id_idx ~= 0 then write(fmt(" %s", id_str(name_id_idx))) end
		if params_idx ~= 0  then fmtnode(params_idx) end
		if ret_idx ~= 0     then fmtnode(ret_idx) end
		if body_idx ~= 0    then fmtnode(body_idx) end
	end,
	resolve = function(r, flags, idx, n, name_id_idx, params_idx, ret_idx, body_idx, as_type)
		if body_idx == 0 and flags&RVALUE ~= 0 then
			-- e.g. "x := fun()"
			r.diag_err(ast_srcpos(n), "function prototype used as value")
		end
		local outer_fun_idx = r.fun_idx
		local outer_fun_typ_idx = r.fun_typ_idx
		r.fun_idx = idx
		r.fun_typ_idx = 0
		local params_typ_idx = 0, 0
		if name_id_idx ~= 0 then
			r.id_define(name_id_idx, idx)
		end
		local scope1 = r.scope_open()
		if params_idx ~= 0 then
			local was_define_params = r.define_params
			r.define_params = body_idx ~= 0 -- define parameters in scope if there's a body
			params_typ_idx, params_idx = r.resolve(params_idx)
			r.define_params = was_define_params
			-- check for special case of type-only parameters,
			-- which can be the case for function types, e.g. "fun f(x fun(int)int)"
			local params = r.ast_node(params_idx)
			local params_kind = ast_kind(r.ast_node(params_idx))
			if params_kind ~= AST_PARAM.kind and
			   params_kind ~= AST_TUPLEREFTYPE and
			   params_kind ~= AST_TUPLETYPE
			then
				params_typ_idx = params_idx
			end
		end
		-- local ret_typ_idx = TYPE_void
		local ret_typ_idx = 0
		if ret_idx ~= 0 then
			-- return type is either expressed as a type, a tuple or param
			ret_typ_idx, ret_idx = r.resolve(ret_idx)
			if ret_typ_idx == TYPE_type then
				-- return type is expressed as a type
				ret_typ_idx = ret_idx
			end
		end
		local typ_idx = AST_FUNTYPE.create(r.unit.ast, params_typ_idx, ret_typ_idx, ast_srcpos(n))
		if body_idx ~= 0 then
			r.fun_typ_idx = typ_idx
			local scope2 = r.scope_open()
			r.resolve(body_idx)
			r.scope_close(scope2)
			r.fun_typ_idx = outer_fun_typ_idx
		end
		r.scope_close(scope1)
		r.fun_idx = outer_fun_idx
		if as_type then
			return typ_idx, typ_idx
		end
		return typ_idx, idx
	end,
	codegen = function(g, flags, n, name_id_idx, params_idx, ret_idx, body_idx)
		if flags&RVALUE ~= 0 then
			g.write("(function ")
			if name_id_idx ~= 0 then
				local name_srcpos = AST_FUN.name_srcpos(g.unit.ast, g.idx)
				g.diag_warn(name_srcpos, "named function used as value")
			end
		elseif body_idx == 0 then
			-- just a prototype; don't generate anything
			return
		-- TODO: top-level function; check if inside another function, like r.fun_idx
		-- 	g.write("function ")
		else
			g.write("local function ")
			if name_id_idx ~= 0 then g.write(id_str(name_id_idx)) end
		end

		-- parameters
		g.write("(")
		if params_idx ~= 0 then
			if ast_kind(g.ast_node(params_idx)) == AST_TUPLE.kind then
				AST_TUPLE.codegen_vals(g, RVALUE, params_idx)
			else
				g.codegen(params_idx)
			end
		end
		g.write(")")

		-- return type as a comment (for debugging)
		if ret_idx ~= 0 then
			g.write(" --> ")
			ret_idx = AST_ID.unwind(g.unit.ast, ret_idx)
			g.write(g.ast_str(ret_idx))
		end

		-- body
		if body_idx ~= 0 then
			g.write("\n")
			g.codegen(body_idx)
			g.write("\nend")
		else
			g.write(" end")
		end

		if flags&RVALUE ~= 0 then
			g.write(")")
		end
	end,

	-- ast_fun.name_srcpos returns the srcpos of a function's name. 0 if no name
	name_srcpos = function(ast_stack, idx)
		assert(idx > 0 and idx <= #ast_stack, fmt("invalid idx %d", idx))
		local n = ast_node(ast_stack, idx)
		local flags = ast_val24(n)
		if flags&AST_FUN.HAS_NAME == 0 then return 0 end
		local name_id_idx = flags >> 8
		if name_id_idx ~= 0 then
			-- embedded name; assume that it starts one space after "fun" keyword
			local srcpos = ast_srcpos(n)
			local span = #(id_str(name_id_idx))
			return srcpos_make(srcpos_off(srcpos) + 4, span)
		end
		-- name is at the end of the function's AST stack range (since its parsed first)
		local span = ast_stack_span(ast_stack, idx)
		return ast_srcpos(ast_stack[idx - (span - 1)])
	end,
}

-- types (AST_PRIMTYPE must be first type, for ast_is_type() to work properly)

AST_PRIMTYPE = defast{ name = 'PRIMTYPE', -- primitive type, e.g. int
	IS_SIGNED = 1<<0, -- flags
	span = function(n) return 1 end,
	visit = function(ast_stack, src, idx, visitor, n) return visitor(n) end,
	is_type_assignable = function(unit, dst_idx, src_idx) -- bad_idx
		return dst_idx == src_idx and 0 or src_idx
	end,
	repr = function(write, fmtnode, n)
		-- embedded id_idx
		return write(fmt(" %s", ast_primtype_name(n)))
	end,
	str = function(ast_stack, idx, n)
		return ast_primtype_name(n)
	end,
}

AST_RESTTYPE = defast{ name = 'RESTTYPE', -- rest type, e.g. ...int
	create = function(ast_stack, srcpos, elem_idx)
		return ast_add_1val(ast_stack, AST_RESTTYPE, srcpos, elem_idx)
	end,
	span = function(n) return ast_get_1val_span(n) end,
	visit = function(ast_stack, src, idx, visitor, n)
		local elem_idx = ast_get_1val(ast_stack, idx, n)
		return visitor(n, elem_idx)
	end,
	repr = function(write, fmtnode, n, elem_idx)
		return fmtnode(elem_idx)
	end,
	resolve = function(r, flags, idx, n, elem_idx)
		local _, elem_idx2 = r.resolve(elem_idx)
		if elem_idx2 ~= elem_idx then
			dlog("TODO: RESTTYPE.resolve: substitution: %s -> %s",
			     r.ast_str(elem_idx), r.ast_str(elem_idx2))
		end
		return TYPE_type
	end,
	is_type_assignable = function(unit, dst_idx, src_idx) -- bad_idx
		error("TODO")
	end,
	str = function(ast_stack, idx, n)
		local elem_idx = ast_get_1op(ast_stack, idx, n)
		return "..." .. ast_str(ast_stack, elem_idx)
	end,
}

AST_FUNTYPE = defast{ name = 'FUNTYPE', -- e.g. (t, t) t
	create = function(ast_stack, params_type_idx, ret_type_idx, srcpos) -- idx
		local flags8 = 0
		return ast_add_2ops(ast_stack, AST_FUNTYPE, flags8, srcpos, params_type_idx, ret_type_idx)
	end,
	elems = function(ast_stack, idx) -- params_idx, ret_idx
		return ast_get_2ops(ast_stack, idx, ast_node(ast_stack, idx))
	end,
	span = function(n) return ast_get_2ops_span(n) end,
	visit = function(ast_stack, src, idx, visitor, n)
		local params_idx, ret_idx = ast_get_2ops(ast_stack, idx, n)
		return visitor(n, params_idx, ret_idx)
	end,
	repr = function(write, fmtnode, n, params_idx, ret_idx)
		if params_idx ~= 0 then fmtnode(params_idx) else write(" ()") end
		if ret_idx ~= 0    then fmtnode(ret_idx) end
	end,
	resolve = function(r, flags, idx, n, name_id_idx, params_idx, ret_idx, body_idx)
		if params_idx ~= 0 then r.resolve(params_idx) end
		if ret_idx ~= 0    then r.resolve(ret_idx) end
		return TYPE_type
	end,
	is_type_assignable = function(unit, dst_idx, src_idx) -- bad_idx
		error("TODO")
	end,
	str = function() return "function type" end,
}




-- built-in AST nodes
builtin_symtab = {} -- built-in definitions, keyed by id_idx
builtin_ast_stack = {} -- keyed by idx (negated in use, i.e. idx -3 is stored as index 3)

function def_builtin(n)
	local idx = #builtin_ast_stack + 1
	builtin_ast_stack[idx] = n
	return -idx
end

function def_builtin_primtype(name, nbits, flag4)
	-- val24:
	-- bit              11  111111112222
	--     0123   45678901  234567890123
	--     flag4  nbits-1   name_id_idx
	assert(nbits >= 1 and nbits <= 0xff)
	assert(flag4 <= 0xf)
	local name_id_idx = id_intern(name); assert(name_id_idx <= 0xfff)
	local val24 = (flag4 & 0xf)<<20 | ((nbits - 1) & 0xff)<<12 | (name_id_idx & 0xfff)
	local idx = def_builtin(ast_make(AST_PRIMTYPE, val24, 0))
	builtin_symtab[name_id_idx] = idx
	return idx
end

TYPE_type   = def_builtin_primtype("type",    1, 0)
TYPE_any    = def_builtin_primtype("any",     1, 0)
TYPE_void   = def_builtin_primtype("void",    1, 0)
TYPE_bool   = def_builtin_primtype("bool",    1, 0)
TYPE_float  = def_builtin_primtype("float",  64, 1)
TYPE_uint   = def_builtin_primtype("uint",   64, 0) ; TYPE_first_int = TYPE_uint
TYPE_int    = def_builtin_primtype("int",    64, 1) -- 1 = is_signed
-- explicit-size integers:
TYPE_int2   = def_builtin_primtype("int2",    2, 1)
TYPE_uint2  = def_builtin_primtype("uint2",   2, 0)
TYPE_int3   = def_builtin_primtype("int3",    3, 1)
TYPE_uint3  = def_builtin_primtype("uint3",   3, 0)
TYPE_int4   = def_builtin_primtype("int4",    4, 1)
TYPE_uint4  = def_builtin_primtype("uint4",   4, 0)
TYPE_int5   = def_builtin_primtype("int5",    5, 1)
TYPE_uint5  = def_builtin_primtype("uint5",   5, 0)
TYPE_int6   = def_builtin_primtype("int6",    6, 1)
TYPE_uint6  = def_builtin_primtype("uint6",   6, 0)
TYPE_int7   = def_builtin_primtype("int7",    7, 1)
TYPE_uint7  = def_builtin_primtype("uint7",   7, 0)
TYPE_int8   = def_builtin_primtype("int8",    8, 1)
TYPE_uint8  = def_builtin_primtype("uint8",   8, 0)
TYPE_int9   = def_builtin_primtype("int9",    9, 1)
TYPE_uint9  = def_builtin_primtype("uint9",   9, 0)
TYPE_int10  = def_builtin_primtype("int10",  10, 1)
TYPE_uint10 = def_builtin_primtype("uint10", 10, 0)
TYPE_int11  = def_builtin_primtype("int11",  11, 1)
TYPE_uint11 = def_builtin_primtype("uint11", 11, 0)
TYPE_int12  = def_builtin_primtype("int12",  12, 1)
TYPE_uint12 = def_builtin_primtype("uint12", 12, 0)
TYPE_int13  = def_builtin_primtype("int13",  13, 1)
TYPE_uint13 = def_builtin_primtype("uint13", 13, 0)
TYPE_int14  = def_builtin_primtype("int14",  14, 1)
TYPE_uint14 = def_builtin_primtype("uint14", 14, 0)
TYPE_int15  = def_builtin_primtype("int15",  15, 1)
TYPE_uint15 = def_builtin_primtype("uint15", 15, 0)
TYPE_int16  = def_builtin_primtype("int16",  16, 1)
TYPE_uint16 = def_builtin_primtype("uint16", 16, 0)
TYPE_int17  = def_builtin_primtype("int17",  17, 1)
TYPE_uint17 = def_builtin_primtype("uint17", 17, 0)
TYPE_int18  = def_builtin_primtype("int18",  18, 1)
TYPE_uint18 = def_builtin_primtype("uint18", 18, 0)
TYPE_int19  = def_builtin_primtype("int19",  19, 1)
TYPE_uint19 = def_builtin_primtype("uint19", 19, 0)
TYPE_int20  = def_builtin_primtype("int20",  20, 1)
TYPE_uint20 = def_builtin_primtype("uint20", 20, 0)
TYPE_int21  = def_builtin_primtype("int21",  21, 1)
TYPE_uint21 = def_builtin_primtype("uint21", 21, 0)
TYPE_int22  = def_builtin_primtype("int22",  22, 1)
TYPE_uint22 = def_builtin_primtype("uint22", 22, 0)
TYPE_int23  = def_builtin_primtype("int23",  23, 1)
TYPE_uint23 = def_builtin_primtype("uint23", 23, 0)
TYPE_int24  = def_builtin_primtype("int24",  24, 1)
TYPE_uint24 = def_builtin_primtype("uint24", 24, 0)
TYPE_int25  = def_builtin_primtype("int25",  25, 1)
TYPE_uint25 = def_builtin_primtype("uint25", 25, 0)
TYPE_int26  = def_builtin_primtype("int26",  26, 1)
TYPE_uint26 = def_builtin_primtype("uint26", 26, 0)
TYPE_int27  = def_builtin_primtype("int27",  27, 1)
TYPE_uint27 = def_builtin_primtype("uint27", 27, 0)
TYPE_int28  = def_builtin_primtype("int28",  28, 1)
TYPE_uint28 = def_builtin_primtype("uint28", 28, 0)
TYPE_int29  = def_builtin_primtype("int29",  29, 1)
TYPE_uint29 = def_builtin_primtype("uint29", 29, 0)
TYPE_int30  = def_builtin_primtype("int30",  30, 1)
TYPE_uint30 = def_builtin_primtype("uint30", 30, 0)
TYPE_int31  = def_builtin_primtype("int31",  31, 1)
TYPE_uint31 = def_builtin_primtype("uint31", 31, 0)
TYPE_int32  = def_builtin_primtype("int32",  32, 1)
TYPE_uint32 = def_builtin_primtype("uint32", 32, 0)
TYPE_int33  = def_builtin_primtype("int33",  33, 1)
TYPE_uint33 = def_builtin_primtype("uint33", 33, 0)
TYPE_int34  = def_builtin_primtype("int34",  34, 1)
TYPE_uint34 = def_builtin_primtype("uint34", 34, 0)
TYPE_int35  = def_builtin_primtype("int35",  35, 1)
TYPE_uint35 = def_builtin_primtype("uint35", 35, 0)
TYPE_int36  = def_builtin_primtype("int36",  36, 1)
TYPE_uint36 = def_builtin_primtype("uint36", 36, 0)
TYPE_int37  = def_builtin_primtype("int37",  37, 1)
TYPE_uint37 = def_builtin_primtype("uint37", 37, 0)
TYPE_int38  = def_builtin_primtype("int38",  38, 1)
TYPE_uint38 = def_builtin_primtype("uint38", 38, 0)
TYPE_int39  = def_builtin_primtype("int39",  39, 1)
TYPE_uint39 = def_builtin_primtype("uint39", 39, 0)
TYPE_int40  = def_builtin_primtype("int40",  40, 1)
TYPE_uint40 = def_builtin_primtype("uint40", 40, 0)
TYPE_int41  = def_builtin_primtype("int41",  41, 1)
TYPE_uint41 = def_builtin_primtype("uint41", 41, 0)
TYPE_int42  = def_builtin_primtype("int42",  42, 1)
TYPE_uint42 = def_builtin_primtype("uint42", 42, 0)
TYPE_int43  = def_builtin_primtype("int43",  43, 1)
TYPE_uint43 = def_builtin_primtype("uint43", 43, 0)
TYPE_int44  = def_builtin_primtype("int44",  44, 1)
TYPE_uint44 = def_builtin_primtype("uint44", 44, 0)
TYPE_int45  = def_builtin_primtype("int45",  45, 1)
TYPE_uint45 = def_builtin_primtype("uint45", 45, 0)
TYPE_int46  = def_builtin_primtype("int46",  46, 1)
TYPE_uint46 = def_builtin_primtype("uint46", 46, 0)
TYPE_int47  = def_builtin_primtype("int47",  47, 1)
TYPE_uint47 = def_builtin_primtype("uint47", 47, 0)
TYPE_int48  = def_builtin_primtype("int48",  48, 1)
TYPE_uint48 = def_builtin_primtype("uint48", 48, 0)
TYPE_int49  = def_builtin_primtype("int49",  49, 1)
TYPE_uint49 = def_builtin_primtype("uint49", 49, 0)
TYPE_int50  = def_builtin_primtype("int50",  50, 1)
TYPE_uint50 = def_builtin_primtype("uint50", 50, 0)
TYPE_int51  = def_builtin_primtype("int51",  51, 1)
TYPE_uint51 = def_builtin_primtype("uint51", 51, 0)
TYPE_int52  = def_builtin_primtype("int52",  52, 1)
TYPE_uint52 = def_builtin_primtype("uint52", 52, 0)
TYPE_int53  = def_builtin_primtype("int53",  53, 1)
TYPE_uint53 = def_builtin_primtype("uint53", 53, 0)
TYPE_int54  = def_builtin_primtype("int54",  54, 1)
TYPE_uint54 = def_builtin_primtype("uint54", 54, 0)
TYPE_int55  = def_builtin_primtype("int55",  55, 1)
TYPE_uint55 = def_builtin_primtype("uint55", 55, 0)
TYPE_int56  = def_builtin_primtype("int56",  56, 1)
TYPE_uint56 = def_builtin_primtype("uint56", 56, 0)
TYPE_int57  = def_builtin_primtype("int57",  57, 1)
TYPE_uint57 = def_builtin_primtype("uint57", 57, 0)
TYPE_int58  = def_builtin_primtype("int58",  58, 1)
TYPE_uint58 = def_builtin_primtype("uint58", 58, 0)
TYPE_int59  = def_builtin_primtype("int59",  59, 1)
TYPE_uint59 = def_builtin_primtype("uint59", 59, 0)
TYPE_int60  = def_builtin_primtype("int60",  60, 1)
TYPE_uint60 = def_builtin_primtype("uint60", 60, 0)
TYPE_int61  = def_builtin_primtype("int61",  61, 1)
TYPE_uint61 = def_builtin_primtype("uint61", 61, 0)
TYPE_int62  = def_builtin_primtype("int62",  62, 1)
TYPE_uint62 = def_builtin_primtype("uint62", 62, 0)
TYPE_int63  = def_builtin_primtype("int63",  63, 1)
TYPE_uint63 = def_builtin_primtype("uint63", 63, 0)
TYPE_int64  = def_builtin_primtype("int64",  64, 1)
TYPE_uint64 = def_builtin_primtype("uint64", 64, 0) ; TYPE_last_int = TYPE_uint64
TYPE_last_primtype = TYPE_uint64

-- TODO: constants: true, false, nil

function ast_is_primtype(idx)       return idx < 0 and idx >= TYPE_last_primtype end
function ast_is_primtype_int(idx)   return idx <= TYPE_first_int and idx >= TYPE_last_int end
function ast_is_primtype_float(idx) return idx == TYPE_float end

function astkind_is_type(k)
	-- if k < 0 then
	-- 	k = -k
	-- 	return k >= AST_PRIMTYPE.kind
	-- else
	return k == AST_TUPLETYPE.kind or
	       k == AST_TUPLEREFTYPE.kind or
	       k == AST_PRIMTYPE.kind or
	       k == AST_RESTTYPE.kind or
	       k == AST_FUNTYPE.kind
end
function astkind_name(k)
	return k == 0 and "<nothing>" or ast_nodes[k].name
end

function ast_kind(n)      return n & 0xff end
function ast_kind_name(n) return astkind_name(ast_kind(n)) end
function ast_is_type(n)   return astkind_is_type((n & 0xff) + 1) end
function ast_val24(n)     return n>>8 & 0xffffff end
function ast_val8(n)      return n>>8 & 0xff end
function ast_val24signed(n)
	local v = n>>8 & 0xffffff
	if v >= 0x800000 then v = v - 0x1000000 end
	return v

	-- 24,16,1,7FFFFF,8388607,S24_MAX
	-- 24,16,1,-800000,-8388608,S24_MIN
	-- 24,16,0,FFFFFF,16777215,U24_MAX

	-- function ast_get_i32x2(ast_stack, idx) -- v1, v2
	-- 	local v64 = ast_stack[idx]
	-- 	local v1 = v64 & 0xffffffff
	-- 	local v2 = (v64 >> 32) & 0xffffffff
	-- 	if v1 >= 0x80000000 then v1 = v1 - 0x100000000 end
	-- 	if v2 >= 0x80000000 then v2 = v2 - 0x100000000 end
	-- 	return v1, v2
	-- end
end
function ast_srcpos(n)    return n>>32 & 0xffffffff end
function ast_srcoff(n)    return n>>32 & 0xffffff end
function ast_srcspan(n)   return n>>56 & 0xff end
-- accessors for AST_PRIMTYPE nodes:
function ast_primtype_flags(n)     return ast_val24(n)>>20 & 0xf end
function ast_primtype_is_signed(n) return ast_primtype_flags(n) & 1 ~= 0 end
function ast_primtype_id_idx(n)    return ast_val24(n) & 0xfff end
function ast_primtype_name(n)      return id_str(ast_primtype_id_idx(n)) end
function ast_primtype_nbits(n)     return (ast_val24(n)>>12 & 0xff) + 1 end
function ast_primtype_size(n)      return ((ast_primtype_nbits(n) + 7) // 8) * 8 end
function ast_primtype_align(n)     return ast_primtype_size(n) end
-- accessors for AST_*TYPE nodes:
function ast_type_flags(n) return (n>>8 & 0xffffff)&0xfffff end
function ast_type_align(n) return ((n>>8 & 0xffffff)>>20) + 1 end
function ast_type_size(n)  return ast_type_align(n) end

function assert_ast_kind(n, expect_N)
	assert(ast_kind(n) == expect_N.kind, fmt("%s != %s", ast_kind_name(n), expect_N.name))
end

-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_type])   == 1, "type")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_any])    == 1, "any")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_void])   == 1, "void")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_bool])   == 1, "bool")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint])   == 64, "uint")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int])    == 64, "int")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int2])   == 2, "int2")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint2])  == 2, "uint2")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int3])   == 3, "int3")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint3])  == 3, "uint3")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int4])   == 4, "int4")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint4])  == 4, "uint4")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int5])   == 5, "int5")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint5])  == 5, "uint5")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int6])   == 6, "int6")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint6])  == 6, "uint6")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int7])   == 7, "int7")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint7])  == 7, "uint7")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int8])   == 8, "int8")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint8])  == 8, "uint8")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int9])   == 9, "int9")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint9])  == 9, "uint9")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int10])  == 10, "int10")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint10]) == 10, "uint10")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int11])  == 11, "int11")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint11]) == 11, "uint11")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int12])  == 12, "int12")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint12]) == 12, "uint12")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int13])  == 13, "int13")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint13]) == 13, "uint13")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int14])  == 14, "int14")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint14]) == 14, "uint14")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int15])  == 15, "int15")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint15]) == 15, "uint15")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int16])  == 16, "int16")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint16]) == 16, "uint16")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int17])  == 17, "int17")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint17]) == 17, "uint17")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int18])  == 18, "int18")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint18]) == 18, "uint18")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int19])  == 19, "int19")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint19]) == 19, "uint19")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int20])  == 20, "int20")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint20]) == 20, "uint20")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int21])  == 21, "int21")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint21]) == 21, "uint21")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int22])  == 22, "int22")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint22]) == 22, "uint22")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int23])  == 23, "int23")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint23]) == 23, "uint23")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int24])  == 24, "int24")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint24]) == 24, "uint24")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int25])  == 25, "int25")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint25]) == 25, "uint25")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int26])  == 26, "int26")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint26]) == 26, "uint26")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int27])  == 27, "int27")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint27]) == 27, "uint27")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int28])  == 28, "int28")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint28]) == 28, "uint28")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int29])  == 29, "int29")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint29]) == 29, "uint29")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int30])  == 30, "int30")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint30]) == 30, "uint30")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int31])  == 31, "int31")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint31]) == 31, "uint31")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int32])  == 32, "int32")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint32]) == 32, "uint32")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int33])  == 33, "int33")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint33]) == 33, "uint33")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int34])  == 34, "int34")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint34]) == 34, "uint34")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int35])  == 35, "int35")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint35]) == 35, "uint35")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int36])  == 36, "int36")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint36]) == 36, "uint36")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int37])  == 37, "int37")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint37]) == 37, "uint37")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int38])  == 38, "int38")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint38]) == 38, "uint38")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int39])  == 39, "int39")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint39]) == 39, "uint39")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int40])  == 40, "int40")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint40]) == 40, "uint40")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int41])  == 41, "int41")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint41]) == 41, "uint41")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int42])  == 42, "int42")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint42]) == 42, "uint42")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int43])  == 43, "int43")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint43]) == 43, "uint43")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int44])  == 44, "int44")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint44]) == 44, "uint44")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int45])  == 45, "int45")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint45]) == 45, "uint45")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int46])  == 46, "int46")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint46]) == 46, "uint46")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int47])  == 47, "int47")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint47]) == 47, "uint47")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int48])  == 48, "int48")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint48]) == 48, "uint48")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int49])  == 49, "int49")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint49]) == 49, "uint49")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int50])  == 50, "int50")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint50]) == 50, "uint50")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int51])  == 51, "int51")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint51]) == 51, "uint51")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int52])  == 52, "int52")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint52]) == 52, "uint52")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int53])  == 53, "int53")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint53]) == 53, "uint53")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int54])  == 54, "int54")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint54]) == 54, "uint54")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int55])  == 55, "int55")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint55]) == 55, "uint55")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int56])  == 56, "int56")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint56]) == 56, "uint56")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int57])  == 57, "int57")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint57]) == 57, "uint57")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int58])  == 58, "int58")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint58]) == 58, "uint58")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int59])  == 59, "int59")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint59]) == 59, "uint59")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int60])  == 60, "int60")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint60]) == 60, "uint60")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int61])  == 61, "int61")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint61]) == 61, "uint61")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int62])  == 62, "int62")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint62]) == 62, "uint62")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int63])  == 63, "int63")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint63]) == 63, "uint63")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_int64])  == 64, "int64")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_uint64]) == 64, "uint64")
-- assert(ast_primtype_nbits(builtin_ast_stack[-TYPE_float])  == 64, "float")

function ast_val24_set(ast_stack, idx, new_val24)
	ast_stack[idx] = (ast_stack[idx] & 0xffffffff000000ff) | ((new_val24 & 0xffffff) << 8)
end

function ast_typeof(unit, idx) -- typ_idx, val_idx
	assert(idx ~= nil)
	if ast_is_primtype(idx) then
		return TYPE_type, idx
	end
	local v = unit.resmap[idx]
	if v == nil then
		return 0, 0
	end
	return unpack_i32x2(v)
end

-- AST node get/push functions
-- ast_get...  retrieves values or operands. idx arguments and return values are 1-based
-- ast_add...  adds nodes with values or operands to the stack. idx arguments are 1-based.
-- ast_push... adds a value to ast_stack (used by ast_add...)

function ast_push64(ast_stack, value)
	local idx = #ast_stack + 1
	ast_stack[idx] = value
	return idx
end

function ast_push_u32x2(ast_stack, v1, v2)
	assert(v1 <= 0xffffffff)
	assert(v2 <= 0xffffffff)
	return ast_push64(ast_stack, (v1 & 0xffffffff) | ((v2 & 0xffffffff) << 32))
end

function ast_get_u32x2(ast_stack, idx) -- v1, v2
	local v64 = ast_stack[idx]
	return (v64 & 0xffffffff), ((v >> 32) & 0xffffffff)
end

function pack_i32x2(v1, v2) -- i64
	assert((v1 < 0 and v1 >= -0x80000000) or (v1 >= 0 and v1 <= 0x7fffffff),
	       "v1 out of range: "..v1)
	assert((v2 < 0 and v2 >= -0x80000000) or (v2 >= 0 and v2 <= 0x7fffffff),
	       "v2 out of range: "..v2)
	return (v1 & 0xffffffff) | ((v2 & 0xffffffff) << 32)
end

function unpack_i32x2(v64) -- i32, i32
	local v1 = v64 & 0xffffffff
	local v2 = (v64 >> 32) & 0xffffffff
	if v1 >= 0x80000000 then v1 = v1 - 0x100000000 end
	if v2 >= 0x80000000 then v2 = v2 - 0x100000000 end
	return v1, v2
end

function ast_push_i32x2(ast_stack, v1, v2)
	return ast_push64(ast_stack, pack_i32x2(v1, v2))
end

function ast_get_i32x2(ast_stack, idx) -- v1, v2
	return unpack_i32x2(ast_stack[idx])
end

function ast_add(ast_stack, N, val24, srcpos) -- idx
	assert(N ~= nil)
	return ast_push64(ast_stack, ast_make(N, val24, srcpos))
end

function ast_add_1val(ast_stack, N, srcpos, v)
	-- bit            111111111122222
	--     12  3456789012345678901234
	--     |   v
	--     +-- encoding: 0=separate stack entry, 1=unsigned, 2=signed, 3=unused
	-- 0x3fffff = 0b001111111111111111111111   value mask
	-- 0x400000 = 0b010000000000000000000000 1 unsigned
	-- 0x800000 = 0b100000000000000000000000 2 signed
	local val24 = 0
	if v >= 0 and v <= 0x3fffff then
		-- unsigned integer that fits in val24
		val24 = val24 | (v & 0x3fffff) | 0x400000
	elseif v < 0 and -v <= 0x200000 then
		-- signed integer that fits in val24
		val24 = val24 | (v & 0x3fffff) | 0x800000
	else
		ast_push64(ast_stack, v)
	end
	return ast_add(ast_stack, N, val24, srcpos)
end

function ast_get_1val_span(n)
	return (ast_val24(n) & (0x400000 | 0x800000) ~= 0) and 1 or 2
end

function ast_get_1val(ast_stack, idx, n) -- value
	local v = ast_val24(n)
	if v & 0x400000 ~= 0 then
		-- embedded unsigned value
		return v & 0x3fffff
	elseif v & 0x800000 ~= 0 then
		-- embedded signed value
		-- if v1 >= 0x80000000 then v1 = v1 - 0x100000000 end
		v = v & 0x3fffff
		if v >= 0x200000 then v = v - 0x400000 end
		return v
	end
	-- separate stack entry
	return ast_stack[idx - 1]
end

function ast_set_1val(ast_stack, idx, new_val)
	local old_val24 = ast_val24(ast_stack[idx])
	if old_val24 & (0x400000 | 0x800000) == 0 then
		-- separate stack entry
		ast_stack[idx - 1] = new_val
	else
		local new_val24 = 0
		if v >= 0 and v <= 0x3fffff then
			-- unsigned integer that fits in val24
			new_val24 = new_val24 | (v & 0x3fffff) | 0x400000
		elseif v < 0 and -v <= 0x200000 then
			-- signed integer that fits in val24
			new_val24 = new_val24 | (v & 0x3fffff) | 0x800000
		else
			-- would need to insert entry into ast_stack which would alter other idx's
			error("cannot grow 1val ast_stack entry")
		end
		ast_val24_set(ast_stack, idx, new_val24)
	end
end

-- -- little test of ast_add_1val
-- do
-- 	local ast_stack = {}
-- 	local idx, v

-- 	ast_stack = {}
-- 	idx = ast_add_1val(ast_stack, AST_ID, 0, 123456)
-- 	-- print(table.unpack(ast_stack))
-- 	v = ast_get_1val(ast_stack, idx, ast_stack[idx])
-- 	assert(v == 123456, fmt("%d", v))
-- 	assert(#ast_stack == 1, "more than one value added")

-- 	ast_stack = {}
-- 	idx = ast_add_1val(ast_stack, AST_ID, 0, -123456)
-- 	v = ast_get_1val(ast_stack, idx, ast_stack[idx])
-- 	assert(v == -123456, fmt("%d", v))
-- 	assert(#ast_stack == 1, "more than one value added")

-- 	-- U22_MAX
-- 	ast_stack = {}
-- 	idx = ast_add_1val(ast_stack, AST_ID, 0, 4194303)
-- 	v = ast_get_1val(ast_stack, idx, ast_stack[idx])
-- 	assert(v == 4194303, fmt("%d", v))
-- 	assert(#ast_stack == 1, "more than one value added")

-- 	-- I22_MAX
-- 	ast_stack = {}
-- 	idx = ast_add_1val(ast_stack, AST_ID, 0, 2097151)
-- 	v = ast_get_1val(ast_stack, idx, ast_stack[idx])
-- 	assert(v == 2097151, fmt("%d", v))
-- 	assert(#ast_stack == 1, "more than one value added")

-- 	-- I22_MIN
-- 	ast_stack = {}
-- 	idx = ast_add_1val(ast_stack, AST_ID, 0, -2097152)
-- 	v = ast_get_1val(ast_stack, idx, ast_stack[idx])
-- 	assert(v == -2097152, fmt("%d", v))
-- 	assert(#ast_stack == 1, "more than one value added")

-- 	-- U22_MAX+1
-- 	ast_stack = {}
-- 	idx = ast_add_1val(ast_stack, AST_ID, 0, 4194303+1)
-- 	v = ast_get_1val(ast_stack, idx, ast_stack[idx])
-- 	assert(v == 4194303+1, fmt("%d", v))
-- 	assert(#ast_stack == 2, "only one value added")

-- 	-- I22_MIN-1
-- 	ast_stack = {}
-- 	idx = ast_add_1val(ast_stack, AST_ID, 0, -2097152-1)
-- 	v = ast_get_1val(ast_stack, idx, ast_stack[idx])
-- 	assert(v == -2097152-1, fmt("%d", v))
-- 	assert(#ast_stack == 2, "only one value added")

-- 	error("OK")
-- end

function ast_add_1op(ast_stack, N, val24, srcpos, op1_idx)
	assert((val24 & 0xff) == val24, fmt("%x overflows uint8", val24))
	-- note: final "ast_add(ast_stack, N, val24, srcpos)" is given idx #ast_stack+1
	local op1_off = #ast_stack+1 - op1_idx
	if op1_idx < 0 or op1_off < 0 or op1_off > 0x7fff then
		ast_push64(ast_stack, op1_idx)
	else
		-- bit            1111111  11122222
		--     1  234567890123456  78901234
		--        op1_off          val24
		assert(op1_idx >= 0 and op1_idx <= #ast_stack, fmt("op1 %d not on ast_stack", op1_idx))
		val24 = val24
		      | (op1_off & 0x7fff) << 8
		      | 0x800000 -- bit 24 indicates operand is relative & embedded
	end
	return ast_add(ast_stack, N, val24, srcpos)
end

function ast_get_1op_span(n)
	return (ast_val24(n) & 0x800000 ~= 0) and 1 or 2
end

function ast_get_1op(ast_stack, idx, n) -- op1_idx, span
	local v = ast_val24(n)
	if v & 0x800000 ~= 0 then
		-- embedded relative stack offset (relative to 'idx')
		return (idx - (v>>8 & 0x7fff)), 1
	end
	return ast_stack[idx - 1], 2
end

function ast_add_2ops(ast_stack, N, val24, srcpos, op1_idx, op2_idx) -- idx
	assert((val24 & 0xff) == val24, fmt("%x overflows uint8", val24))
	op1_idx, op2_idx = op1_idx - 1, op2_idx - 1 -- 1-based to 0-based
	local idx = #ast_stack + 1
	local op1_off, op2_off = idx - op1_idx, idx - op2_idx
	if op1_idx < 0 or op2_idx < 0 then
		op1_off = 0x100
	end
	if op1_off > 0xff or op2_off > 0x7f then
		ast_push_i32x2(ast_stack, op1_idx, op2_idx)
	else
		-- bit             1111111   11122222
		--     1  2345678  90123456  78901234
		--        op2_off  op1_off   val24
		--
		-- e.g. val24=3, op1_off=4, op2_off=5 = 1 0000101 00000100 00000011
		assert(op1_idx > 0 and op1_idx <= #ast_stack, "op1 not on ast_stack")
		assert(op2_idx > 0 and op2_idx <= #ast_stack, "op2 not on ast_stack")
		-- print("EMBED i32x2", idx, "--", op1_off, op2_off)
		val24 = val24
		      | (op1_off & 0xff) << 8
		      | (op2_off & 0x7f) << 16
		      | 0x800000 -- bit 24 indicates operands are relative & embedded
	end
	return ast_add(ast_stack, N, val24, srcpos)
end

function ast_get_2ops_span(n)
	return (ast_val24(n) & 0x800000 ~= 0) and 1 or 2
end

function ast_get_2ops(ast_stack, idx, n) -- op1_idx, op2_idx
	local v = ast_val24(n)
	local op1_idx, op2_idx
	if v & 0x800000 == 0 then
		op1_idx, op2_idx = ast_get_i32x2(ast_stack, idx - 1)
	else
		-- embedded relative stack offsets (relative to 'idx')
		-- print("UN-EMBED i32x2", idx, "--", ((v >> 8) & 0xff), ((v >> 16) & 0x7f))
		op1_idx = idx - ((v >> 8) & 0xff)
		op2_idx = idx - ((v >> 16) & 0x7f)
	end
	return op1_idx+1, op2_idx+1
end

-- local function ast_add_1srcref(ast_stack, N, val1, srcpos, srcstart, srcend)
-- 	assert(srcstart <= srcend)
-- 	assert(val1 <= 1, fmt("%x overflows uint1", val1))
-- 	-- Check if we can pack it into flags.
-- 	-- 18 bits for offset limits us to 262,143
-- 	-- 5 bits for length limits us to 31 (most identifiers are shorter than this)
-- 	--   bit          111111111 12222  2
-- 	--       123456789012345678 90123  4
-- 	--       srcoff             srclen val1
-- 	local srclen = srcend - srcstart -- store start+len instead of start+end
-- 	if srcstart <= 0x3ffff and srclen <= 0x1f then
-- 		val1 = val1
-- 		      | (srclen & 0x1f) << 1
-- 		      | (srcstart & 0x3ffff) << 6
-- 	else
-- 		ast_push_u32x2(ast_stack, srcstart, srclen)
-- 	end
-- 	return ast_add(ast_stack, N, val1, srcpos)
-- end

function ast_node(ast_stack, idx)
	assert((idx < 0 and -idx <= #builtin_ast_stack) or (idx > 0 and idx <= #ast_stack),
	       fmt("invalid idx %d", idx))
	return idx < 0 and builtin_ast_stack[-idx] or ast_stack[idx]
end

-- ast_stack_span returns the number of stack slots that node at i spans,
-- including all its operands
function ast_stack_span(ast_stack, idx)
	local n = ast_node(ast_stack, idx)
	if n == 0 then return 1 end
	local k = ast_kind(n)
	assert(ast_nodes[k] ~= nil, fmt("invalid AST kind: %d", k))
	if k == AST_PRIMTYPE.kind then
		return 0
	elseif ast_nodes[k].span == nil then
		error(fmt("TODO: %s.span", astkind_name(k)))
	end
	return ast_nodes[k].span(n, ast_stack, idx)
end

function ast_visit(ast_stack, src, idx, visitor)
	local n = ast_node(ast_stack, idx)
	if n == 0 then return visitor(n) end
	local k = ast_kind(n)
	assert(k ~= 0 and k <= #ast_nodes, fmt("invalid ast_kind %d", k))
	if ast_nodes[k].visit == nil then
		error(fmt("TODO: %s.visit", astkind_name(k)))
	end
	return ast_nodes[k].visit(ast_stack, src, idx, visitor, n)
end

function ast_str(ast_stack, idx)
	if idx == 0 then return "<unknown>" end
	local n = ast_node(ast_stack, idx)
	if n == nil then return fmt("(ast_str:idx=%d)", idx) end
	local k = ast_kind(n)
	if k == AST_PRIMTYPE.kind then
		return ast_primtype_name(n)
	elseif ast_nodes[k].str == nil then
		dlog("TODO: ast_str: implement %s.str", astkind_name(k))
		return astkind_name(k)
	end
	return ast_nodes[k].str(ast_stack, idx, n)
end

function ast_make_seq_iterator(ast_stack, idx, span)
	return function()
		if span == 1 then return function() end end -- empty
		-- visit in reverse order
		-- TODO: something more efficient than making a temporary array
		local a = {}
		local function vv(idx, parent_span)
			local span = ast_stack_span(ast_stack, idx)
			parent_span = parent_span - span
			if parent_span > 0 then
				vv(idx - span, parent_span)
			end
			a[#a + 1] = idx
		end
		vv(idx - 1, span - 1)
		local i = 1
		return function()
			local v = a[i]
			i = i + 1
			return v
		end
	end
end


function multitype_count(ast_stack, idx)
	if idx == 0 then
		return 0
	end
	local n = ast_node(ast_stack, idx)
	local k = ast_kind(n)
	if k == AST_TUPLETYPE.kind then
		return AST_TUPLETYPE.count(ast_stack, idx)
	elseif k == AST_TUPLEREFTYPE.kind then
		return AST_TUPLEREFTYPE.count(n)
	end
	return 1
end

function multitype_str(ast_stack, idx, count)
	if count == nil then
		count = multitype_count(ast_stack, idx)
	end
	if count == 0 then
		return "()"
	end
	local s = ast_str(ast_stack, idx)
	local val_kind = ast_kind(ast_node(ast_stack, idx))
	if val_kind ~= AST_TUPLETYPE.kind and val_kind ~= AST_TUPLEREFTYPE.kind then
		s = "(" .. s .. ")"
	end
	return s
end


function ast_repr(ast_stack, src, idx, maxdepth, resmap)
	if #ast_stack == 0 then return "()" end
	if idx == nil then idx = #ast_stack end

	-- local function unwrap_nref(idx)
	-- 	local n = ast_node(ast_stack, idx)
	-- 	if ast_kind(n) == AST_NREF.kind then
	-- 		return AST_NREF.target(n)
	-- 	end
	-- 	return idx
	-- end

	-- pass 1/2: find referencing nodes
	local depth = 0
	local seen = {}
	local reftab = {}
	local reftab_next_no = 0
	local function write() end
	local function reprnode(idx)
		if idx == 0 then return end
		-- dlog("reprnode> [%d]", idx)
		-- idx = unwrap_nref(idx)
		return ast_visit(ast_stack, src, idx, function(n, ...)
			if ast_is_primtype(idx) then return end
			local k = ast_kind(n)
			if maxdepth ~= nil and depth >= maxdepth then return end
			depth = depth + 1
			if seen[idx] ~= nil then
				if seen[idx] == 2 and reftab[idx] == nil then
					reftab_next_no = reftab_next_no + 1
					reftab[idx] = reftab_next_no
				end
			else
				seen[idx] = 1
				local N = ast_nodes[k]
				if N ~= nil and N.repr ~= nil then
					-- dlog("reprnode> [%d] 0x%x", idx, n)
					N.repr(write, reprnode, n, ...)
				end
				-- append type
				if resmap ~= nil and not ast_is_type(n) then
					local v64 = resmap[idx]
					if v64 ~= nil then
						local typ_idx, idx2 = unpack_i32x2(v64)
						if typ_idx ~= 0 and typ_idx ~= TYPE_type then
							reprnode(typ_idx)
						end
					end
				end
				seen[idx] = 2
			end
			depth = depth - 1
		end)
	end
	reprnode(idx)

	-- pass 2/2: print
	depth = 0
	seen = {}
	local buf = {}
	local function write(s) table.insert(buf, s) end
	local function reprnode(idx)
		if idx == 0 then
			if depth > 0 then write("\n" .. string.rep("    ", depth)) end
			write("<idx=0>")
			return
		end
		-- idx = unwrap_nref(idx)
		return ast_visit(ast_stack, src, idx, function(n, ...)
			local k = ast_kind(n)
			if maxdepth ~= nil and depth >= maxdepth then write(")"); return end
			if depth > 0 then
				write("\n" .. string.rep("    ", depth))
			end
			depth = depth + 1
			local style = ast_is_type(n) and "1;36" or "1"
			write("(\x1b[" .. style .. "m" .. astkind_name(k) .. "\x1b[0m")
			local refno = reftab[idx]
			if refno ~= nil then
				write(fmt(" \x1b[0;3%dm#%d\x1b[0m", refno % 6, refno))
			end
			if seen[idx] ~= nil and not ast_is_primtype(idx) then
				if seen[idx] == 1 then
					write(fmt(" <recursive#%d>", idx))
				end
			else
				seen[idx] = 1
				write(fmt(" \x1b[2m[%s+%d]\x1b[0m",
				          srcpos_fmt(ast_srcpos(n), src), ast_srcspan(n)))
				local N = ast_nodes[k]
				if N ~= nil and N.repr ~= nil then
					N.repr(write, reprnode, n, ...)
				end
				-- append type
				if resmap ~= nil and not ast_is_type(n) then
					local v64 = resmap[idx]
					if v64 ~= nil then
						local typ_idx, idx2 = unpack_i32x2(v64)
						if typ_idx ~= 0 and typ_idx ~= TYPE_type then
							reprnode(typ_idx)
						end
					end
				end
				seen[idx] = 2
			end
			write(")")
			depth = depth - 1
		end)
	end
	reprnode(idx)
	return table.concat(buf, "")
end


function ast_dump(ast_stack, ast_stack_root)
	dlog("  %4s  %16s  %-10s  %6s  %-7s  %-7s  %s",
	     "#", "NODE", "TYPE", "VAL24", "SRCPOS", "STKSPAN", "IDSTR")
	local v, marker
	if ast_stack_root == nil then ast_stack_root = #ast_stack end
	for idx = 1, #ast_stack do
		v = ast_stack[idx]
		marker = ast_stack_root == idx and "> " or "  "
		if numtype(v) == 'float' then
			dlog("%s%4d  %16f  %-10s  %6s  %7s  %7d", marker, idx, v, "<float>", "", "", 1)
		elseif type(v) == 'string' then
			dlog("%s%4d  %16s  %-10s  %06x  %7s  %7d",
			     marker, idx, limit_string(v, 16), "<str>", 0, "", 1)
		elseif idx < #ast_stack and
		       type(ast_stack[idx + 1]) == 'number' and
		       numtype(ast_stack[idx + 1]) == 'integer' and
		       ast_kind(ast_stack[idx + 1]) == AST_ID.kind
		then
			dlog("%s%4d  %016x  %-10s  %-13d    %7d", marker, idx, v, "<i64>", v, 1)
		else
			-- use pcall to handle errors in ast_stack_span for unimplemented AST types
			local ok, span = pcall(ast_stack_span, ast_stack, idx)
			if not ok then span = 0 end
			local k = ast_kind(v)
			if v == 0 or ast_nodes[k] == nil then
				-- probably some arbitrary value
				dlog("%s%4d  %016x  %-10s  %-13d    %7d", marker, idx, v, "<i64>", v, 1)
			else
				local val24 = ast_val24(v)
				local s = ""
				if k == AST_ID.kind or k == AST_PARAM.kind then
					s = id_str(val24 + 1)
				elseif k == AST_VAR.kind then
					_, id_idx, _ = AST_VAR.unpack(ast_stack, idx)
					s = id_str(id_idx)
				elseif k == AST_FUN.kind then
					local id_idx = ast_val24(v) >> 8
					if id_idx ~= 0 then
						s = id_str(id_idx)
					end
				end
				dlog("%s%4d  %016x  %-10s  %06x  %3d:%-3d  %7d  %s",
				     marker, idx, v, astkind_name(k), val24,
				     ast_srcoff(v), ast_srcspan(v), span, s)
			end
		end
	end
end
