-- resolve (type resolution & analysis)


function is_type_assignable(unit, dst_typ_idx, src_typ_idx) -- bad_idx (0 if ok)
	assert(dst_typ_idx ~= nil)
	assert(src_typ_idx ~= nil)
	if dst_typ_idx == 0 or src_typ_idx == 0 then
		if unit.errcount == 0 then
			if dst_typ_idx ~= 0 then return dst_typ_idx end
			if src_typ_idx ~= 0 then return src_typ_idx end
		end
		return 0
	end

	-- local dst_typ = ast_node(unit.ast, dst_typ_idx)
	-- local src_typ = ast_node(unit.ast, src_typ_idx)
	-- local dst_kind = ast_kind(dst_typ)
	-- if dst_kind ~= ast_kind(src_typ) then
	-- 	dlog("dst=%s src=%s", ast_kind_name(dst_typ), ast_kind_name(src_typ))
	-- 	return false
	-- end

	dst_typ_idx = AST_ID.unwind(unit.ast, dst_typ_idx)
	src_typ_idx = AST_ID.unwind(unit.ast, src_typ_idx)

	local dst_kind = ast_kind(ast_node(unit.ast, dst_typ_idx))

	local N_is_type_assignable = ast_nodes[dst_kind].is_type_assignable
	assert(N_is_type_assignable ~= nil,
	       fmt("TODO: implement %s.is_type_assignable", astkind_name(dst_kind)))
	return N_is_type_assignable(unit, dst_typ_idx, src_typ_idx)
end


function resolve_unit(unit)
	if DEBUG_RESOLVE then dlog("\x1b[1;35mresolve %s\x1b[0m", unit.srcfile) end
	local ast_stack = unit.ast
	if #ast_stack == 0 then return 0 end
	local src = unit.src
	local depth = 0
	local symstack_id, symstack_val, symstack_len, symstack_scope = {}, {}, 0, 0
	local ctxtype_stack, ctxtype_stack_len = { TYPE_any }, 1
	local resmap = unit.resmap -- idx -> (typ_idx, val_idx)

	local function assert_istype(idx)
		assert(ast_is_type(ast_node(ast_stack, idx)),
		       fmt("%s is not a type", ast_kind_name(ast_node(ast_stack, idx))) )
	end

	local r; r = {
		unit = unit,
		idx = idx,
		ctxtype = TYPE_any,
		fun_idx = 0, -- current function, 0 if top-level
		fun_typ_idx = 0, -- type of fun_idx
		define_params = false,
		ast_node = function(idx) assert(idx ~= nil); return ast_node(ast_stack, idx) end,
		ast_repr = function(idx) assert(idx ~= nil); return ast_repr(ast_stack, src, idx) end,
		ast_str = function(idx) assert(idx ~= nil); return ast_str(ast_stack, idx) end,
		ast_typeof = function(idx) return ast_typeof(unit, idx) end, -- typ_idx, val_idx
		ast_srcpos = function(idx) return ast_srcpos(ast_node(ast_stack, idx)) end,
		is_type_assignable = function(dst_typ_idx, src_typ_idx) -- bad_idx
			return is_type_assignable(unit, dst_typ_idx, src_typ_idx)
		end,
		id_lookup = function(id_idx)
			-- print("id_lookup", id_str(id_idx), id_idx)
			assert(id_idx ~= 0)
			for i = symstack_len, 1, -1 do
				if symstack_id[i] == id_idx then
					return symstack_val[i]
				end
			end
			return builtin_symtab[id_idx]
		end,
		id_define = function(id_idx, idx)
			assert(id_idx ~= 0)
			if id_idx == ID__ then
				-- the "hole" identifier '_' is never defined
				return
			end
			local shadows = 0
			for i = symstack_len, symstack_scope, -1 do
				if symstack_id[i] == id_idx then
					shadows = symstack_val[i]
					r.diag_err(nil, "'%s' redeclared", id_str(id_idx))
					r.diag_info(r.ast_srcpos(shadows), "'%s' previously defined here", id_str(id_idx))
					break
				end
			end
			symstack_len = symstack_len + 1
			symstack_id[symstack_len] = id_idx
			symstack_val[symstack_len] = idx
			return shadows
		end,
		id_lookup_or_define = function(id_idx, idx) -- existing_idx or nil
			if id_idx == ID__ then return nil end
			local existing_idx = r.id_lookup(id_idx)
			if existing_idx ~= nil then
				return existing_idx
			end
			symstack_len = symstack_len + 1
			symstack_id[symstack_len] = id_idx
			symstack_val[symstack_len] = idx
			return nil
		end,
		scope_open = function()
			symstack_scope = symstack_len
			return symstack_len
		end,
		scope_close = function(scope)
			symstack_scope = scope
			symstack_len = scope
		end,
		ctxtype_push = function(typ_idx)
			typ_idx = AST_ID.unwind(ast_stack, typ_idx)
			dlog("ctxtype_push> %s", ast_repr(ast_stack, src, typ_idx))
			assert_istype(typ_idx)
			ctxtype_stack_len = ctxtype_stack_len + 1
			ctxtype_stack[ctxtype_stack_len] = typ_idx
			r.ctxtype = typ_idx
		end,
		ctxtype_pop = function()
			assert(ctxtype_stack_len > 0)
			ctxtype_stack_len = ctxtype_stack_len - 1
			r.ctxtype = ctxtype_stack[ctxtype_stack_len]
			dlog("ctxtype_pop>  %s", ast_repr(ast_stack, src, r.ctxtype))
		end,
		diag_err = function(srcpos, format, ...)
			if srcpos == nil then srcpos = ast_srcpos(ast_stack[r.idx]) end
			return diag(DIAG_ERR, unit, srcpos, format, ...)
		end,
		diag_warn = function(srcpos, format, ...)
			if srcpos == nil then srcpos = ast_srcpos(ast_stack[r.idx]) end
			return diag(DIAG_WARN, unit, srcpos, format, ...)
		end,
		diag_info = function(srcpos, format, ...)
			if srcpos == nil then srcpos = ast_srcpos(ast_stack[r.idx]) end
			return diag(DIAG_INFO, unit, srcpos, format, ...)
		end,
		resolve = function(idx, flags) -- typ_idx, idx
			if idx < 0 then
				-- built-in things have predefined type
				return ast_typeof(unit, idx)
			end
			if idx == 0 then
				-- nothing/error
				return 0, idx
			end
			local n = r.ast_node(idx)
			assert(n ~= 0, "encountered <nothing> AST node at idx " .. idx)

			-- check for NREF
			local nref_idx = 0
			if ast_kind(n) == AST_NREF.kind then
				nref_idx = idx
				idx = AST_NREF.target(n)
				n = r.ast_node(idx)
			end

			-- check if already resolved
			local v64 = resmap[idx]
			if v64 ~= nil then
				-- already resolved
				local typ_idx, idx2 = unpack_i32x2(v64)
				--dlog("resolve> already resolved: #%d %s (#%d %s, #%d %s)",
				--     idx, ast_kind_name(n),
				--     typ_idx, ast_kind_name(r.ast_node(typ_idx)),
				--     idx2, ast_kind_name(r.ast_node(idx2)) )
				if nref_idx ~= 0 then
					-- update NREF to point to potentially-new node
					AST_NREF.update(ast_stack, nref_idx, idx2)
				end
				return typ_idx, idx2
			end

			if DEBUG_RESOLVE then
				dlog("\x1b[1;35mR>\x1b[0m%s %s ...", string.rep("  ", depth), ast_kind_name(n))
			end

			if flags == nil then flags = 0 end
			r.idx = idx
			return ast_visit(ast_stack, src, idx, function(n, ...)
				depth = depth + 1
				local k = ast_kind(n)
				if k == 0 then
					return TYPE_void, idx
				end
				local f = ast_nodes[k].resolve
				if f == nil then
					error(fmt("TODO: resolve %s", astkind_name(k)))
				end

				local typ_idx, idx2 = f(r, flags, idx, n, ...)
				if idx2 == nil then
					idx2 = idx
				end

				assert(typ_idx ~= nil, fmt("resolve %s returned nil", astkind_name(k)))
				if typ_idx == 0 and unit.errcount == 0 then
					unit.errcount = unit.errcount + 1
					dlog("resolver error: %s.resolve did not return a type (returned 0)",
					     ast_kind_name(n))
				end

				depth = depth - 1

				-- print("resolve> memoize resolve(#"..idx..") =>", typ_idx, idx2)
				r.mark_resolved(idx, typ_idx, idx2)
				if nref_idx ~= 0 and idx ~= idx2 then
					-- update NREF to point to new node
					AST_NREF.update(ast_stack, nref_idx, idx2)
				end

				if DEBUG_RESOLVE then
					xpcall(function()
						dlog("\x1b[1;35mR>\x1b[0m%s %s => (%s, #%s %s)",
						     string.rep("  ", depth),
						     ast_kind_name(n),
						     ast_repr(ast_stack, src, typ_idx, 1),
						     idx2, ast_repr(ast_stack, src, idx2, 1) )
					end, function(err)
						print("[recovered] " .. debug.traceback(err, 2))
						dlog("\x1b[1;35mR>\x1b[0m%s %s => <ast_repr error>",
						     string.rep("  ", depth), ast_kind_name(n))
						-- ast_dump(ast_stack, unit.ast_root); os.exit(1) -- XXX
					end)
				end

				return typ_idx, idx2
			end)
		end,
		mark_resolved = function(idx, typ_idx, val_idx)
			resmap[idx] = pack_i32x2(typ_idx, val_idx)
		end,
	}
	r.resolve(unit.ast_root)
	if DEBUG_RESOLVE then
		print("ast_stack and AST after resolve_unit:")
		ast_dump(ast_stack, unit.ast_root)
		print(ast_repr(unit.ast, src, unit.ast_root, nil, resmap))
	end
end
