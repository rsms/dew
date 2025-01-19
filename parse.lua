function parse_unit(unit, tokens)
	local ast_stack = unit.ast
	local exprtab_prefix = {}
	local exprtab_infix = {}
	local typetab_prefix = {}
	local typetab_infix = {}
	local tok_iter, save_tokenizer_state, tok_iter_restore = token_iterator(tokens)
	local tok, srcpos_curr, tok_value = 0, 0, nil

	local function next_token()
		if tok == nil then error("next_token called after already encountering END") end
		tok, srcpos_curr, tok_value = tok_iter()
	end
	local function restore_tokenizer_state(snapshot)
		tok, srcpos_curr, tok_value = tok_iter_restore(snapshot)
	end

	local function diag_err(srcpos, format, ...)
		unit.errcount = unit.errcount + 1
		if srcpos == nil then srcpos = srcpos_curr end
		return diag(DIAG_ERR, unit, srcpos, format, ...)
	end

	local function mkast(t)
		t._srcbegin = tokstart
		t._srcend = tokend
		return t
	end

	local function syntax_error(format, ...)
		if unit.errcount == 0 or tok ~= nil then -- avoid cascading errors after EOF
			diag_err(srcpos_curr, format, ...)
		end
		-- error("syntax_error")
		return false
	end

	local function tok_fastforward(stoptok)
		-- advance to next semicolon or stoptok, whichever comes first
		if tok == TOK_SEMI or tok == stoptok then
			-- make sure we make _some_ progress
			return tok ~= nil and next_token() or nil
		end
		while tok ~= nil and not (tok == TOK_SEMI or tok == stoptok) do
			next_token()
		end
	end

	local function syntax_error_unexpected(expected_tok)
		syntax_error("unexpected '%s', expected '%s'", tokname(tok), tokname(expected_tok))
	end

	local function expect(expected_tok)
		if tok == expected_tok then
			return next_token()
		end
		syntax_error_unexpected(expected_tok)
		tok_fastforward(expected_tok)
	end

	local function parse_parselet(prec, tab_prefix, tab_infix)
		-- -- consume comments
		-- while tok == TOK_COMMENT do
		-- 	-- dlog("comment>%s", tok_value)
		-- 	next_token()
		-- end

		-- check for end of input
		if tok == nil then
			return nil
		end

		-- prefix
		local parselet = tab_prefix[tok]
		if parselet == nil then
			if unit.errcount == 0 then
				syntax_error("unexpected '%s'", tokname(tok))
				tok_fastforward()
			end
			return nil
		end
		assert(type(parselet) == 'function', fmt("tok='%s'", tokname(tok)))
		local idx = parselet()
		-- dlog("parse_parselet> tab_prefix(%s) => %s", tokname(tok), tostring(idx))

		-- infix
		assert(prec ~= nil)
		while true do
			local t = tab_infix[tok] -- { parselet, parselet_prec }
			if t == nil or t[2] < prec then
				return idx
			end
			idx = t[1](idx, t[2], tok)
		end
	end

	local function parse_expr(prec) return parse_parselet(prec, exprtab_prefix, exprtab_infix) end
	local function parse_type(prec) return parse_parselet(prec, typetab_prefix, typetab_infix) end

	local function parse_id_type()
		local id_idx = tok_value
		local srcpos = srcpos_curr
		expect(TOK_ID)
		ast_push64(ast_stack, 0) -- placeholder for target
		return ast_add(ast_stack, AST_ID, id_idx - 1, srcpos)
	end
	typetab_prefix[TOK_ID] = parse_id_type

	local function parse_id_assign(id_idx, srcpos, rest) -- e.g. "x = 1" or "x, y, z = 1, 2, 3"
		next_token() -- consume "="
		local stack_start = #ast_stack

		-- parse first value (e.g. "1" in "x, y = 1, 2")
		parse_expr(PREC_MIN)

		-- Parse remaining vars.
		-- 'rest' is an array of interleaved id_idx and srcpos of additional names,
		-- e.g. {id_idx,srcpos,id_idx,srcpos ...}

		-- Check for spread assignment, e.g. "x, y, z = expr"
		if tok ~= TOK_COMMA and rest ~= nil then
			AST_VAR.create(ast_stack, srcpos, id_idx, AST_VAR.SUBTYPE_SPREADDEF)
			for i = 1, #rest, 2 do
				local id_idx2, srcpos2 = rest[i], rest[i + 1]
				AST_VAR.create(ast_stack, srcpos2, id_idx2, AST_VAR.SUBTYPE_SPREADDEF)
			end
			local count = 1 + (#rest // 2)
			return AST_SPREADVAR.create(ast_stack, stack_start, count, srcpos)
		end

		-- One or more variable assignments.
		-- Return a single VAR early if it's just one name on the left (rest == nil)
		local idx = AST_VAR.create(ast_stack, srcpos, id_idx, AST_VAR.SUBTYPE_VARDEF)
		if rest == nil then
			return idx
		end

		for i = 1, #rest, 2 do
			local id_idx2, srcpos2 = rest[i], rest[i + 1]
			-- expecting another value, preceeded by a comma
			if tok ~= TOK_COMMA then
				-- -- There are fewer expressions on the right-hand side than on the left-hand side.
				-- -- This either means that the user forgot a value or that one of the
				-- -- RHS expressions is a tuple. So we need to wait until typecheck pass to check.
				-- ast_push64(ast_stack, 0) -- placeholder for "no value"
				diag_err(srcpos_curr, "missing value for '%s'", id_str(id_idx2))
				return idx
			end
			next_token()
			parse_expr(PREC_MIN)
			AST_VAR.create(ast_stack, srcpos2, id_idx2, AST_VAR.SUBTYPE_VARDEF)
		end
		-- customized error message for when too many values are provided
		if tok == TOK_COMMA then
			next_token()
			if tok ~= TOK_SEMI and tok ~= nil then
				diag_err(srcpos_curr, "too many values in variable assignment")
				tok_fastforward()
			end
		end

		local count = 1 + (#rest // 2)
		return AST_LIST.create(ast_stack, AST_MULTIVAR, stack_start, count, srcpos)
	end

	local function parse_id_expr()
		local id_idx = tok_value
		local srcpos = srcpos_curr
		expect(TOK_ID)

		-- check for assignment, e.g. "x = 1"
		if tok == TOK_COMMA or tok == TOK_EQ then
			if tok == TOK_EQ then
				-- single-value assignment or variable definition, e.g. "x = 1"
				return parse_id_assign(id_idx, srcpos, nil)
			end
			-- maybe multi-value assignment or variable definition, e.g. "x, y = 1, 2"
			local tokstate = save_tokenizer_state()
			local rest = {}
			next_token()
			while tok == TOK_ID do
				rest[#rest + 1] = tok_value
				rest[#rest + 1] = srcpos_curr
				next_token()
				if tok == TOK_EQ then
					return parse_id_assign(id_idx, srcpos, rest)
				elseif tok ~= TOK_COMMA then
					break
				end
				next_token()
			end
			-- nope, this is something else; undo tokenizer changes
			restore_tokenizer_state(tokstate)
		end

		ast_push64(ast_stack, 0) -- placeholder for target
		return ast_add(ast_stack, AST_ID, id_idx - 1, srcpos)
	end
	exprtab_prefix[TOK_ID] = parse_id_expr

	local function parse_rest_expr()
		local srcpos = srcpos_curr
		expect(TOK_DOTDOTDOT)
		return ast_add(ast_stack, AST_REST, 0, srcpos)
	end
	exprtab_prefix[TOK_DOTDOTDOT] = parse_rest_expr

	local function parse_rest_type() -- "... type"
		local srcpos = srcpos_curr
		local type_idx
		expect(TOK_DOTDOTDOT)
		if tok == TOK_DOTDOTDOT then
			-- must handle special case of "... ..." here
			syntax_error("unexpected '...', expected a type")
			tok_fastforward()
			type_idx = TYPE_void
		else
			type_idx = parse_type(PREC_MIN)
		end
		return AST_RESTTYPE.create(ast_stack, srcpos, type_idx)
	end
	typetab_prefix[TOK_DOTDOTDOT] = parse_rest_type

	-- parse_many is a helper function that parses one or more nodes separated by sep_tok.
	-- Supports trailing sep_tok if stop_tok is provided.
	local function parse_many(sep_tok, stop_tok, parse_elem_fn) -- count
		local count = 0
		while tok ~= stop_tok and tok ~= nil do
			if parse_elem_fn(PREC_MIN) == nil then
				if stop_tok ~= nil and unit.errcount == 0 then
					if tok == nil then
						syntax_error("unexpected end of input")
					else
						syntax_error("unexpected end of sequence")
					end
				end
				break
			end
			count = count + 1
			if tok ~= sep_tok then
				if stop_tok == nil then
					syntax_error("unexpected '%s', expected '%s'", tokname(tok), tokname(sep_tok))
					tok_fastforward()
				end
				break
			end
			next_token() -- consume sep_tok
		end
		return count
	end

	local function parse_tuple_body(N, parse_elem_fn, srcpos)
		-- tuple_body = [ elem ("," elem)* ","? ]
		-- e.g. "1" "1,2" "1,2,"
		assert(N == AST_TUPLE or N == AST_TUPLETYPE, fmt("N=%d", N.kind))
		local stack_start = #ast_stack
		local count = parse_many(TOK_COMMA, TOK_RPAREN, parse_elem_fn)
		if count == 1 then
			return #ast_stack
		end
		return AST_LIST.create(ast_stack, N, stack_start, count, srcpos)
	end

	local function parse_tuple(N, parse_elem_fn)
		-- tuple = "(" [ elem ("," elem)* ","? ] ")"
		-- e.g. "()" "(1)" "(1,2)" "(1,2,)"
		local srcpos = srcpos_curr
		expect(TOK_LPAREN)
		local idx = parse_tuple_body(N, parse_elem_fn, srcpos)
		expect(TOK_RPAREN)
		return idx
	end
	local function parse_expr_tuple()
		return parse_tuple(AST_TUPLE, parse_expr)
	end
	local function parse_type_tuple()
		return parse_tuple(AST_TUPLETYPE, parse_type)
	end
	exprtab_prefix[TOK_LPAREN] = parse_expr_tuple
	typetab_prefix[TOK_LPAREN] = parse_type_tuple

	local function parse_block_body(srcpos, stoptok)
		-- block_body = [ expr (";" expr)* ";"? ]
		-- e.g. "1;" "1;2" "1;2;"
		local stack_start = #ast_stack
		local count = parse_many(TOK_SEMI, stoptok, parse_expr)
		if count <= 1 then
			return #ast_stack
		end
		-- store precomputed stack span in val24 field
		local span = (#ast_stack - stack_start) + 1
		if span > 0xffffff then syntax_error("block too large") end
		return ast_add(ast_stack, AST_BLOCK, span, srcpos)
	end

	local function parse_block()
		-- block = "{" [ expr (";" expr)* ";"? ] "}"
		local srcpos = srcpos_curr
		expect(TOK_LBRACE)
		local idx = parse_block_body(srcpos, TOK_RBRACE)
		expect(TOK_RBRACE)
		return idx
	end

	local function parse_params(allow_only_types) -- count
		local srcpos = srcpos_curr
		local tokstate, ast_stack_start = nil, #ast_stack
		if allow_only_types then
			tokstate = save_tokenizer_state()
		end
		expect(TOK_LPAREN)
		local stack_start = #ast_stack
		local count = 0
		local type_pending = {}
		local type_idx
		local param_srcpos = srcpos_curr

		local function flush_type_pending()
			if #type_pending > 0 then
				-- apply type to preceeding parameters which are missing type
				-- e.g. "(x, y, z int)" => "(x int, y int, z int)"
				local type_n = ast_stack[type_idx]
				if ast_stack_span(ast_stack, type_idx) > 1 then
					-- use a node reference instead of copying type
					type_n = AST_NREF.create(ast_stack, type_idx, 0)
				end
				for _, idx in ipairs(type_pending) do
					ast_stack[idx] = type_n
				end
				type_pending = {}
			end
		end

		while tok ~= TOK_RPAREN do
			-- accept "..." as last argument
			if tok == TOK_DOTDOTDOT then
				next_token()
				type_idx = parse_type(PREC_MIN)
				flush_type_pending()
				AST_PARAM.create(ast_stack, ID_REST, param_srcpos)
				count = count + 1
				break
			end

			-- Parse name, but don't add it to ast_stack until we have parsed type.
			-- This is essentially a specialized infix parselet.
			local param_id_idx = tok_value
			expect(TOK_ID)
			if tok == TOK_COMMA then
				-- add type placeholder to stack
				local idx = ast_push64(ast_stack, 0)
				type_pending[#type_pending + 1] = idx
			elseif tok ~= TOK_RPAREN then
				type_idx = parse_type(PREC_MIN)
				allow_only_types = false -- "t,t" no longer allowed when we've seen "x t"
				flush_type_pending()
			elseif allow_only_types then
				-- oh, actually it's just a list of types, e.g. "(int, float)"
				for _ = 1, (#ast_stack - ast_stack_start) do table.remove(ast_stack) end
				restore_tokenizer_state(tokstate)
				return parse_type_tuple()
			else
				-- e.g. "(x, y)"
				if builtin_symtab[param_id_idx] ~= nil then
					if #type_pending > 0 then
						-- e.g. "(int, float)"
						syntax_error("missing name of parameters (use '_' for no name)")
					else
						-- e.g. "(x int, float)"
						syntax_error("missing name of parameter (use '_' for no name)")
					end
				else
					if #type_pending > 0 then
						-- e.g. "(x, y)"
						syntax_error("missing type of parameters")
					else
						-- e.g. "(x int, y)"
						syntax_error("missing type of parameter '%s'", id_str(param_id_idx))
					end
				end
			end
			AST_PARAM.create(ast_stack, param_id_idx, param_srcpos)
			count = count + 1
			if tok ~= TOK_COMMA then break end
			next_token() -- consume ","
			param_srcpos = srcpos_curr
			-- note: this supports trailing comma by virtue of the loop condition
		end
		expect(TOK_RPAREN)
		if count > 1 then
			AST_LIST.create(ast_stack, AST_TUPLE, stack_start, count, srcpos)
		end
		return count
	end

	local function parse_fun(as_type)
		local srcpos = srcpos_curr -- position of "fun" keyword
		local val24 = 0 -- bottom 8 bits for flags, top 16 for name id
		local id_idx, id_srcpos = 0, 0
		if as_type then val24 = AST_FUN.AS_TYPE end
		next_token() -- consume "fun" keyword

		-- Name
		if tok == TOK_ID then
			id_srcpos = srcpos_curr
			id_idx = tok_value
			val24 = val24 | AST_FUN.HAS_NAME
			if id_idx > 0xffff then
				-- Store name (ID index) in separate node.
				-- (Top 16 bits of val24 being zero communicates this)
				ast_add(ast_stack, AST_ID, id_idx - 1, id_srcpos)
			else
				val24 = val24 | ((id_idx & 0xffff) << 8)
			end
			srcpos = srcpos_union(srcpos, id_srcpos)
			next_token()
		end

		-- Parameters, e.g. "(x, y int)" or "()"
		if tok == TOK_LPAREN and parse_params(as_type) > 0 then
			val24 = val24 | AST_FUN.HAS_PARAMS
		end

		-- Return type, e.g. "int", or "(x, y int, z str)".
		-- Thus, return type can be TUPLE or PARAM, in addition to some TYPE. I.e.
		--   fun f() int                     -- PRIMTYPE
		--   fun f() (x int)                 -- PARAM
		--   fun f() (x int, y float, z int) -- TUPLE of PARAMs
		--   fun f() (int, float, int)       -- TUPLETYPE
		if tok ~= TOK_LBRACE and tok ~= TOK_SEMI then
			if tok == TOK_LPAREN then
				parse_params(--[[allow_only_types]]true)
			else
				parse_type(PREC_MIN)
			end
			val24 = val24 | AST_FUN.HAS_RETTYPE
		end

		-- Body, e.g. "{ x; y }", "x;" (equivalent to "{x}")
		if as_type ~= true and tok ~= TOK_SEMI then
			parse_block()
			val24 = val24 | AST_FUN.HAS_BODY
		end

		return ast_add(ast_stack, AST_FUN, val24, srcpos)
	end
	exprtab_prefix[TOK_FUN] = parse_fun

	local function parse_fun_type() return parse_fun(true) end
	typetab_prefix[TOK_FUN] = parse_fun_type

	local function parse_int_literal(neg_srcpos)
		local srcpos = srcpos_curr
		local value = tok_value
		if neg_srcpos ~= nil then
			-- has leading '-'; negate (must check for overflow first)
			srcpos = srcpos_union(neg_srcpos, srcpos)
			if value < 0 and value ~= -9223372036854775808 then
				diag_err(srcpos, "integer literal overflows i64")
			end
			value = -value
		end
		next_token()
		return ast_add_1val(ast_stack, AST_INT, srcpos, value)
	end
	exprtab_prefix[TOK_INT] = parse_int_literal

	local function parse_float_literal(neg_srcpos)
		local srcpos = srcpos_curr
		local value = tok_value
		if neg_srcpos ~= nil then
			-- has leading '-'; negate (must check for overflow first)
			srcpos = srcpos_union(neg_srcpos, srcpos)
			value = -value
		end
		next_token()
		ast_push64(ast_stack, value)
		return ast_add(ast_stack, AST_FLOAT, 0, srcpos)
	end
	exprtab_prefix[TOK_FLOAT] = parse_float_literal

	local function parse_return()
		-- return_stmt = "return" [expr ("," expr)*]
		local srcpos = srcpos_curr
		local val24 = 0
		next_token()
		if tok ~= TOK_SEMI and tok ~= TOK_RBRACE then
			parse_tuple_body(AST_TUPLE, parse_expr, srcpos)
			val24 = 1 -- has value
		end
		return ast_add(ast_stack, AST_RETURN, val24, srcpos)
	end
	exprtab_prefix[TOK_RETURN] = parse_return

	local function parse_assign(left_idx, prec, tok)
		local left_kind = ast_kind(ast_stack[left_idx])
		if left_kind ~= AST_ID.kind then
			syntax_error("cannot assign to %s", ast_nodes[left_kind].name)
		end
		local srcpos = srcpos_curr
		next_token()
		parse_expr(prec)
		return ast_add_1op(ast_stack, AST_ASSIGN, 0, srcpos, left_idx)
	end
	exprtab_infix[TOK_EQ] = { parse_assign, PREC_MIN }

	local function parse_call(recv_idx)
		local srcpos = srcpos_curr
		next_token()
		parse_tuple_body(AST_TUPLE, parse_expr, srcpos)
		expect(TOK_RPAREN)
		return ast_add_1op(ast_stack, AST_CALL, 0, srcpos, recv_idx)
	end
	exprtab_infix[TOK_LPAREN] = { parse_call, PREC_MIN }

	local function parse_binop(left_idx, prec, tok)
		local srcpos = srcpos_curr
		next_token()
		parse_expr(prec)
		return ast_add_1op(ast_stack, AST_BINOP, tok, srcpos, left_idx)
	end
	-- precedence level 5
	exprtab_infix[TOK_STAR] = { parse_binop, PREC_BIN5 }
	exprtab_infix[TOK_SLASH] = exprtab_infix[TOK_STAR]
	exprtab_infix[TOK_PERCENT] = exprtab_infix[TOK_STAR]
	exprtab_infix[TOK_LTLT] = exprtab_infix[TOK_STAR]
	exprtab_infix[TOK_GTGT] = exprtab_infix[TOK_STAR]
	exprtab_infix[TOK_AND] = exprtab_infix[TOK_STAR]
	-- precedence level 4
	exprtab_infix[TOK_PLUS] = { parse_binop, PREC_BIN4 }
	exprtab_infix[TOK_MINUS] = exprtab_infix[TOK_PLUS]
	exprtab_infix[TOK_OR] = exprtab_infix[TOK_PLUS]
	exprtab_infix[TOK_HAT] = exprtab_infix[TOK_PLUS]
	-- precedence level 3
	exprtab_infix[TOK_EQEQ] = { parse_binop, PREC_BIN3 }
	exprtab_infix[TOK_NOTEQ] = exprtab_infix[TOK_EQEQ]
	exprtab_infix[TOK_LT] = exprtab_infix[TOK_EQEQ]
	exprtab_infix[TOK_LTEQ] = exprtab_infix[TOK_EQEQ]
	exprtab_infix[TOK_GT] = exprtab_infix[TOK_EQEQ]
	exprtab_infix[TOK_GTEQ] = exprtab_infix[TOK_EQEQ]
	-- precedence level 2
	exprtab_infix[TOK_ANDAND] = { parse_binop, PREC_BIN2 }
	-- precedence level 1
	exprtab_infix[TOK_OROR] = { parse_binop, PREC_BIN1 }

	local function parse_prefixop()
		local op = tok
		local srcpos = srcpos_curr
		next_token()
		-- special case for negative numeric literal
		if tok == TOK_INT then
			return parse_int_literal(srcpos)
		elseif tok == TOK_FLOAT then
			return parse_float_literal(srcpos)
		end
		parse_expr(PREC_MIN)
		return ast_add(ast_stack, AST_PREFIXOP, op, srcpos)
	end
	exprtab_prefix[TOK_MINUS] = parse_prefixop

	-- parse unit
	next_token()
	if tok ~= nil then
		parse_block_body(srcpos_curr)
		-- assert(tok == nil, "stopped before EOF, at " .. tokname(tok))
	end
	unit.ast_root = #ast_stack

	if DEBUG_PARSER then
		dlog("AST of unit %s:", unit.srcfile)
		ast_dump(ast_stack, unit.ast_root)
		print(ast_repr(unit.ast, unit.src))
	end
end
