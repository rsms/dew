-- flags for parse_unit
PARSE_SRCMAP = 1 -- enable populating unit.ast_srcmap

function parse_unit(unit, tokens, flags) --> void
	unit.ast = __rt.buf_create(2048)
	unit.ast_srcmap = {}

	local ast = unit.ast
	local exprtab_prefix = {}
	local exprtab_infix = {}
	local typetab_prefix = {}
	local typetab_infix = {}
	local tok_iter, save_tokenizer_state, tok_iter_restore = token_iterator(tokens)
	local tok, srcpos_curr, tok_value = 0, 0, nil

	local function ast_make(N, srcpos, ...)
		return N.cons(ast, N.alloc(ast), srcpos, ...)
	end


	-- tokenizer functions
	local function next_token()
		assert(tok ~= nil, "next_token called after already encountering END")
		tok, srcpos_curr, tok_value = tok_iter()
	end
	local function restore_tokenizer_state(snapshot)
		tok, srcpos_curr, tok_value = tok_iter_restore(snapshot)
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


	-- comment-handling shims
	local commentq = {}
	local commentq_flush
	if unit.include_comments then
		-- enable handling of comments
		unit.commentmap = {} -- idx => [comment]
		next_token = function()
			while true do
				assert(tok ~= nil, "next_token called after already encountering END")
				tok, srcpos_curr, tok_value = tok_iter()
				if tok ~= TOK_COMMENT then
					return
				end
				local comment = { tok = tok, value = tok_value, srcpos = srcpos_curr}
				commentq[#commentq + 1] = comment
			end
		end
		commentq_flush = function(idx)
			-- if DEBUG_PARSER then
			-- 	dlog("parse> associating %d comment%s with %s#%d:",
			-- 	     #commentq, #commentq == 1 and "" or "s", ast_kindname(ast, idx), idx)
			-- end
			for i = 1, #commentq do
				local comments = unit.commentmap[idx]
				if comments == nil then
					comments = {}
					unit.commentmap[idx] = comments
				end
				comments[#comments + 1] = commentq[i]
				commentq[i] = nil
				-- if DEBUG_PARSER then
				-- 	local c = comments[#comments]
				-- 	local line, col = srcpos_linecol(c.srcpos, unit.src)
				-- 	dlog("  %s\t%s:%d\t%s", tokname(c.tok), line, col, c.value)
				-- end
			end
		end
	end


	-- diagnostics functions
	local function diag_err(srcpos, format, ...)
		if (flags & PARSE_SRCMAP) == 0 then
			dlog("[diag] re-parsing with srcmap enabled")
			return parse_unit(unit, tokens, flags | PARSE_SRCMAP)
		end
		if srcpos == nil then srcpos = srcpos_curr end
		return diag(unit, DIAG_ERR, srcpos, format, ...)
	end
	local function diag_info(srcpos, format, ...)
		if (flags & PARSE_SRCMAP) == 0 then
			dlog("[diag] re-parsing with srcmap enabled")
			return parse_unit(unit, tokens, flags | PARSE_SRCMAP)
		end
		if srcpos == nil then srcpos = srcpos_curr end
		return diag(unit, DIAG_INFO, srcpos, format, ...)
	end

	local function syntax_error(format, ...)
		if unit.errcount == 0 or tok ~= nil then -- avoid cascading errors after EOF
			diag_err(srcpos_curr, format, ...)
		end
		return false
	end

	local function syntax_error_at(srcpos, format, ...)
		if unit.errcount == 0 or tok ~= nil then -- avoid cascading errors after EOF
			diag_err(srcpos, format, ...)
		end
		return false
	end

	local function syntax_error_unexpected(expected_tok)
		if type(expected_tok) == "string" then
			return syntax_error("unexpected '%s' (expected %s)", tokname(tok), expected_tok)
		elseif expected_tok == TOK_RBRACE then
			return syntax_error("unexpected '%s' (expected '%s' or ';')",
			                    tokname(tok), tokname(expected_tok))
		else
			return syntax_error("unexpected '%s' (expected '%s')",
			                    tokname(tok), tokname(expected_tok))
		end
	end

	local function expect(expected_tok)
		if tok == expected_tok then
			return next_token()
		end
		syntax_error_unexpected(expected_tok)
		tok_fastforward(expected_tok)
	end


	-- parselets
	local function parselet(prec, tab_prefix, tab_infix)
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
		-- dlog("parselet> tab_prefix(%s) => %s", tokname(tok), tostring(idx))

		-- infix
		assert(prec ~= nil)
		while tok ~= TOK_SEMI do
			local t = tab_infix[tok] -- { parselet, parselet_prec }
			if t == nil or t[2] < prec then
				break
			end
			idx = t[1](idx, t[2], tok)
		end

		return idx
	end

	local function parse_expr(prec)
		return parselet(prec, exprtab_prefix, exprtab_infix)
	end

	local function parse_type(prec)
		return parselet(prec, typetab_prefix, typetab_infix)
	end

	local function parse_id_type()
		local id_idx = tok_value
		local srcpos = srcpos_curr
		expect(TOK_ID)
		return AST_ID.make(ast, srcpos, id_idx)
	end
	typetab_prefix[TOK_ID] = parse_id_type

	local function parse_multivar(id_idx, srcpos, count)
		-- e.g. "x, y, z = 1, 2, 3" or "x, y, z = source"

		-- make list up front, create VARs by reading all IDs
		local list_offs = AST_MULTIVAR.alloc(ast, srcpos, count)
		local var_offs = list_offs + 8 -- VARs follow immediately
		AST_VAR.cons(ast, var_offs, srcpos, id_idx, 0)
		for i = 1, count - 1 do
			assert(tok == TOK_COMMA)
			next_token() -- consume ","
			AST_VAR.cons(ast, var_offs + AST_VAR.size*i, srcpos_curr, tok_value, 0)
			next_token() -- consume ID
		end

		assert(tok == TOK_EQ)
		next_token() -- consume '='

		-- read values
		for i = 0, count - 1 do
			local value_idx = parse_expr(PREC_MIN)
			AST_VAR.set_value(ast, var_offs + AST_VAR.size*i, value_idx)
			if i == count - 1 then
				if tok == TOK_COMMA then -- e.g. "a, b = 1, 2, 3"
					syntax_error("too many values in multi assignment")
					tok_fastforward()
				end
				break
			end

			-- check next token
			if tok == TOK_COMMA then
				next_token() -- consume ","
			elseif i == 0 then -- e.g. "x, y, z = tuple"
				-- Convert AST_MULTIVAR to AST_SPREADVAR and process remaining identifiers
				ast:set_u8(list_offs, AST_SPREADVAR.kind)
				while i < count - 1 do
					i = i + 1
					AST_VAR.set_value(ast, var_offs + AST_VAR.size*i, 0)
				end
				break
			elseif i < count - 1 then
				local id_idx = ast:get_u32(var_offs + AST_VAR.size*(i + 1)) >> 8
				if tok == TOK_SEMI then
					syntax_error("missing value for '%s'", id_str(id_idx))
				else
					syntax_error_unexpected(TOK_COMMA)
					tok_fastforward(TOK_COMMA)
				end
				break
			end
		end

		return ast_idx_of_offs(list_offs)
	end

	local function parse_id_expr()
		local srcpos = srcpos_curr
		local id_idx = tok_value
		next_token() -- consume ID

		-- check for variable assignment, e.g. "x = 1"
		if tok == TOK_EQ then -- single assignment, e.g. "x = 1"
			next_token() -- consume "="
			local value_idx = parse_expr(PREC_MIN)
			return AST_VAR.make(ast, srcpos, id_idx, value_idx)
		end

		-- if we see a comma after id, look ahead for multi-var assignment, e.g. "x, y = 1, 2"
		if tok == TOK_COMMA then
			local tokstate = save_tokenizer_state()
			local count = 1
			next_token() -- consume ","
			while tok == TOK_ID do
				count = count + 1
				next_token() -- consume ID
				if tok == TOK_EQ then
					restore_tokenizer_state(tokstate)
					return parse_multivar(id_idx, srcpos, count)
				elseif tok ~= TOK_COMMA then
					break
				end
				next_token()
			end
			-- nope, this is something else; undo tokenizer changes
			restore_tokenizer_state(tokstate)
		end

		return AST_ID.make(ast, srcpos, id_idx)
	end
	exprtab_prefix[TOK_ID] = parse_id_expr

	local function parse_rest_expr()
		error("TODO: convert to new AST")
		local srcpos = srcpos_curr
		expect(TOK_DOTDOTDOT)
		return ast_add(ast_stack, AST_REST, 0, srcpos)
	end
	exprtab_prefix[TOK_DOTDOTDOT] = parse_rest_expr

	local function parse_rest_type() -- "... type"
		local srcpos = srcpos_curr
		local type_idx = 0
		expect(TOK_DOTDOTDOT)
		if tok == TOK_DOTDOTDOT then
			-- must handle special case of "... ..." here
			syntax_error("unexpected '...', expected a type")
			tok_fastforward()
		else
			type_idx = parse_type(PREC_MIN)
		end
		return AST_RESTTYPE.make(ast, srcpos, type_idx)
	end
	typetab_prefix[TOK_DOTDOTDOT] = parse_rest_type

	local function parse_array_type() -- "[type]"
		error("TODO: convert to new AST")
		local srcpos = srcpos_curr
		next_token() -- consume '['
		parse_type(PREC_MIN)
		expect(TOK_RBRACK)
		return AST_ARRAYTYPE.create(ast_stack, srcpos)
	end
	typetab_prefix[TOK_LBRACK] = parse_array_type

	-- parse_list and parse_list_elems parses one or more nodes separated by sep_tok.
	-- Supports trailing sep_tok if stop_tok is provided.
	local function parse_list_elems(sep_tok, stop_tok, parse_elem_fn, first_idx) --> idxv_i32
		local idxv_i32 = __rt.buf_create(64)
		if first_idx ~= nil then
			idxv_i32:push_u32(first_idx)
		end
		while tok ~= stop_tok and tok ~= nil do
			local idx = parse_elem_fn(PREC_MIN) -- PREC_MIN for sep_tok (',' or ';')

			-- check unexpected EOF, for a partial list, e.g. "{ x; y"
			if idx == nil then
				if stop_tok ~= nil and unit.errcount == 0 then
					syntax_error("unexpected end of input"..
					             (tok == nil and "input" or "sequence"))
				end
				break
			end

			-- check for sublist, e.g. "x, y" in "{ x, y; z }"
			if tok ~= sep_tok and tok == TOK_COMMA then
				local srcpos = srcpos_curr
				next_token() -- consume ','
				local idxv2 = parse_list_elems(TOK_COMMA, sep_tok, parse_elem_fn, idx)
				idx = ast_list_add(ast, AST_TUPLE.kind, srcpos, idxv2)
			end

			idxv_i32:push_u32(idx)


			if tok ~= sep_tok then
				if stop_tok == nil then
					syntax_error_unexpected(sep_tok)
					tok_fastforward()
				end
				break
			end

			next_token() -- consume sep_tok

			if #commentq > 0 then
				commentq_flush(idx)
			end
		end
		return idxv_i32
	end
	local function parse_list(N, srcpos, sep_tok, stop_tok, parse_elem_fn, elide_if_empty) --> idx
		local idxv_i32 = parse_list_elems(sep_tok, stop_tok, parse_elem_fn)
		if #idxv_i32 == 0 and elide_if_empty then
			return 0
		end
		-- if there's just one item in the list, return that item instead of creating a list
		if #idxv_i32 == 4 then
			return idxv_i32:get_u32(0)
		end
		return ast_list_add(ast, N.kind, srcpos, idxv_i32)
	end

	-- tuples
	local function parse_tuple_body(N, srcpos, parse_elem_fn, elide_if_empty)
		-- tuple_body = [ elem ("," elem)* ","? ]
		-- e.g. "1" "1,2" "1,2,"
		return parse_list(N, srcpos, TOK_COMMA, TOK_RPAREN, parse_elem_fn, elide_if_empty)
	end
	local function parse_tuple(N, parse_elem_fn)
		-- tuple = "(" [ elem ("," elem)* ","? ] ")"
		-- e.g. "()" "(1)" "(1,2)" "(1,2,)"
		local srcpos = srcpos_curr
		expect(TOK_LPAREN)
		local elide_if_empty = false
		local idx = parse_tuple_body(N, srcpos, parse_elem_fn, elide_if_empty, nil)
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

	-- blocks
	local function parse_block_body(srcpos, stoptok, elide_if_empty)
		-- block_body = [ expr (";" expr)* ";"? ]
		-- e.g. "1;" "1;2" "1;2;"
		local idxv_i32 = parse_list_elems(TOK_SEMI, stoptok, parse_expr)
		if #idxv_i32 == 0 and elide_if_empty then
			return 0
		end
		return ast_list_add(ast, AST_BLOCK.kind, srcpos, idxv_i32)
		-- return parse_list(AST_BLOCK, srcpos, TOK_SEMI, stoptok, parse_expr, elide_if_empty)
	end
	local function parse_block(elide_if_empty)
		-- block = "{" [ expr (";" expr)* ";"? ] "}"
		local srcpos = srcpos_curr
		expect(TOK_LBRACE)
		local idx = parse_block_body(srcpos, TOK_RBRACE, elide_if_empty)
		expect(TOK_RBRACE)
		return idx
	end
	exprtab_prefix[TOK_LBRACE] = function() return parse_block(true) end

	local function parse_params(allow_only_types) --> idx
		-- Save token & ast state so that we can support only types.
		-- e.g. undo when we discover "(T, T)" rather than "(name, name T)"
		local tokstate = allow_only_types and save_tokenizer_state() or nil
		local aststate = #ast

		local list_srcpos = srcpos_curr
		local idxv_i32 = __rt.buf_create(64)
		local type_pending_start = -1 -- offset in idxv_i32

		expect(TOK_LPAREN)
		local srcpos = 0

		local function report_err(name_id_idx)
			if builtin_idtab_lookup(name_id_idx) ~= 0 then
				-- handle common case of built-in named types
				local f = function(...) return syntax_error_at(srcpos, ...) end
				if type_pending_start > -1 then -- "(int, float)"
					return f("missing name of parameters (use '_' for no name)")
				else -- "(x int, float)"
					return f("missing name of parameter (use '_' for no name)")
				end
			else
				if type_pending_start > -1 then -- "(x, y)"
					return f("missing type of parameters")
				else -- "(x int, y)"
					return f("missing type of parameter '%s'", id_str(name_id_idx))
				end
			end
		end

		while tok ~= TOK_RPAREN do
			srcpos = srcpos_curr
			local name_id_idx = tok_value
			local typ_idx = 0
			local value_idx = 0

			-- either a name (param name or type name), or ...type
			-- expect(TOK_ID)
			if tok == TOK_ID then
				next_token()
			elseif allow_only_types and tok == TOK_DOTDOTDOT then -- ...
				name_id_idx = ID__
				typ_idx = parse_type(PREC_MIN)
				-- ...T must be last parameter
				if tok ~= TOK_RPAREN then
					syntax_error_unexpected(TOK_RPAREN)
					tok_fastforward(TOK_RPAREN)
					break
				end
			elseif allow_only_types then -- T
				name_id_idx = ID__
				typ_idx = parse_type(PREC_MIN)
			else
				syntax_error_unexpected("parameter name or type")
				tok_fastforward(TOK_RPAREN)
				break
			end

			if tok == TOK_COMMA then
				-- Type not yet known, e.g. parsing "x" in "(x, y T)"
				if type_pending_start == -1 then
					type_pending_start = #idxv_i32
				end
			elseif typ_idx == 0 and tok ~= TOK_RPAREN then
				typ_idx = parse_type(PREC_MIN)
				allow_only_types = false -- "t,t" no longer allowed when we've seen "x t"
				-- fixup pending types
				if type_pending_start > -1 then
					if tok == TOK_EQ then
						-- disallow ambiguous situation, e.g.
						-- "f(x, y int = 2)"; does x have a default value?
						syntax_error("ambiguous default value only applies to last parameter." ..
						             " Use explicit type for other parameters")
					end
					for offs = type_pending_start, #idxv_i32 - 4, 4 do
						local param_offs = ast_offs_of_idx(idxv_i32:get_u32(offs))
						AST_PARAM.set_type(ast, param_offs, typ_idx)
					end
					type_pending_start = -1
				end
			else
				if allow_only_types then
					-- Oh, actually it's just a list of types, e.g. "(int, float)"
					ast:resize(aststate) -- remove anything we added to the AST
					restore_tokenizer_state(tokstate)
					return parse_type_tuple() -- AST_TUPLETYPE
				else
					-- e.g. "(x, y)" missing type at end (while requiring types)
					report_err(name_id_idx)
				end
			end

			-- optional initial value, e.g. "x int = 3"
			if tok == TOK_EQ then
				next_token() -- consume "="
				value_idx = parse_expr(PREC_MIN)
			end

			idxv_i32:push_u32(AST_PARAM.make(ast, srcpos, name_id_idx, value_idx, typ_idx))

			-- If we get something else than a comma here, we are done
			if tok ~= TOK_COMMA then break end
			next_token() -- consume ","
			srcpos = srcpos_curr
			-- note: this supports trailing comma by virtue of the loop condition
		end

		expect(TOK_RPAREN)

		-- return nothing, just one param or a tuple of params
		if #idxv_i32 == 0 then
			return 0
		end
		return ast_list_add(ast, AST_TUPLE.kind, list_srcpos, idxv_i32)
	end

	local function parse_fun(as_type)
		local srcpos = srcpos_curr -- position of "fun" keyword
		next_token() -- consume "fun" keyword

		local name_id_idx = 0
		local params_idx = 0
		local result_idx = 0
		local body_idx = 0

		-- Name
		if tok == TOK_ID then
			name_id_idx = tok_value
			srcpos = srcpos_union(srcpos, srcpos_curr)
			next_token()
		end

		-- Parameters, e.g. "(x, y int)" or "()"
		if tok == TOK_LPAREN then
			params_idx = parse_params(as_type)
		end

		-- Return type, e.g. "int", or "(x, y int, z str)".
		-- Thus, return type can be TUPLE or PARAM, in addition to some TYPE. I.e.
		--   fun f() int                     -- (PRIMTYPE)
		--   fun f() (x int)                 -- (PARAM)
		--   fun f() (x int, y float, z int) -- (TUPLE (PARAM ...))
		--   fun f() (int, float, int)       -- (TUPLETYPE (TYPE ...))
		if tok == TOK_LPAREN then -- "fun() (int, int)"
			result_idx = parse_params(true)
		elseif tok ~= TOK_LBRACE and tok ~= TOK_SEMI then -- "fun() int"
			result_idx = parse_type(PREC_MIN)
			if tok == TOK_COMMA and not as_type then
				-- handle common error "f () T, Y { ... }"
				syntax_error("unexpected '%s'; expected '{' or ';'", tokname(tok))
				diag_info(srcpos_curr, "Wrap multiple results in '(…)'")
				tok_fastforward()
			end
		end

		-- Body, e.g. "{ x; y }", "x;" (equivalent to "{x}")
		if as_type ~= true and tok ~= TOK_SEMI then
			body_idx = parse_block(false)
		end

		return AST_FUN.make(ast, srcpos, name_id_idx, body_idx, params_idx, result_idx)
	end
	exprtab_prefix[TOK_FUN] = parse_fun

	local function parse_fun_type() return parse_fun(true) end
	typetab_prefix[TOK_FUN] = parse_fun_type

	local function parse_int_literal(neg_srcpos)
		local srcpos = srcpos_curr
		local value = tok_value
		local base = tok == TOK_INT   and 10 or
		             tok == TOK_INT2  and  2 or
		             tok == TOK_INT8  and  8 or
		             tok == TOK_INT16 and 16
		local is_neg = neg_srcpos ~= nil
		if is_neg then
			-- has leading '-'; negate (must check for overflow first)
			srcpos = srcpos_union(neg_srcpos, srcpos)
			if value < 0 and value > -9223372036854775808 then
				-- diag_err(srcpos, "integer literal overflows int64")
				-- communicate to resolver that this value is out of range
				base = 0
			end
			value = -value
		end
		next_token()
		return AST_INT.make(ast, srcpos, base, is_neg, value)
	end
	exprtab_prefix[TOK_INT] = parse_int_literal
	exprtab_prefix[TOK_INT2] = parse_int_literal
	exprtab_prefix[TOK_INT8] = parse_int_literal
	exprtab_prefix[TOK_INT16] = parse_int_literal
	exprtab_prefix[TOK_INTBIG] = function()
		local srcpos = srcpos_curr
		next_token()
		return AST_INT.make(ast, srcpos, 0, false, 0)
	end

	local function parse_float_literal(neg_srcpos)
		local srcpos = srcpos_curr
		local value = tok_value
		if neg_srcpos ~= nil then
			-- has leading '-'; negate
			srcpos = srcpos_union(neg_srcpos, srcpos)
			value = -value
		end
		next_token()
		return AST_FLOAT.make(ast, srcpos, value)
	end
	exprtab_prefix[TOK_FLOAT] = parse_float_literal

	local function parse_return()
		-- return_stmt = "return" [expr ("," expr)*]
		local srcpos = srcpos_curr
		next_token()
		local value_idx = 0
		if tok ~= TOK_SEMI and tok ~= TOK_RBRACE then
			local val_srcpos = srcpos_curr
			local elide_if_empty = true
			value_idx = parse_tuple_body(AST_TUPLE, val_srcpos, parse_expr, elide_if_empty, nil)
		end
		return AST_RETURN.make(ast, srcpos, value_idx)
	end
	exprtab_prefix[TOK_RETURN] = parse_return

	local function parse_assign(left_idx, prec, tok)
		error("TODO: convert to new AST")
		local left_kind = ast_kind(ast_stack[left_idx])
		if left_kind ~= AST_ID.kind then
			syntax_error("cannot assign to %s", ast_info[left_kind].name)
		end
		local srcpos = srcpos_curr
		next_token()
		parse_expr(prec)
		return ast_add_1op(ast_stack, AST_ASSIGN, 0, srcpos, left_idx)
	end
	exprtab_infix[TOK_EQ] = { parse_assign, PREC_MIN }

	local function parse_call(recv_idx)
		local srcpos = srcpos_curr
		next_token() -- consume "("
		local idxv_i32 = parse_list_elems(TOK_COMMA, TOK_RPAREN, parse_expr, recv_idx)
		local idx = ast_list_add(ast, AST_CALL.kind, srcpos, idxv_i32)
        -- local args_idx = ast_list_add(ast, AST_TUPLE.kind, srcpos, idxv_i32)
        -- local idx = AST_CALL.make(ast, srcpos, recv_idx, idxv_i32)
		expect(TOK_RPAREN) -- expect ")"
		-- return AST_CALL.make(ast, srcpos, recv_idx, args_idx)
		return idx
	end
	exprtab_infix[TOK_LPAREN] = { parse_call, PREC_UNPOST }

	local function parse_binop(left_idx, prec, op)
		local srcpos = srcpos_curr
		next_token()
		local right_idx = parse_expr(prec)
		-- return ast_make(AST_BINOP, srcpos, op, left_idx, right_idx)
		return AST_BINOP.cons(ast, AST_BINOP.alloc(ast), srcpos, op, left_idx, right_idx)
	end
	-- precedence level 5
	exprtab_infix[TOK_STAR] = { parse_binop, PREC_BIN5 }
	exprtab_infix[TOK_STARSTAR] = exprtab_infix[TOK_STAR]
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
		local op, srcpos = tok, srcpos_curr
		local offs = AST_PREFIXOP.alloc(ast)
		next_token()
		if op == TOK_MINUS then
			if tok >= TOK_INT and tok <= TOK_INT16 then
				return parse_int_literal(srcpos)
			elseif tok == TOK_FLOAT then
				return parse_float_literal(srcpos)
			end
		end
		local operand_idx = parse_expr(PREC_UNPRE)
		return AST_PREFIXOP.cons(ast, offs, srcpos, op, operand_idx)
	end
	exprtab_prefix[TOK_MINUS] = parse_prefixop
	exprtab_prefix[TOK_NOT] = parse_prefixop
	exprtab_prefix[TOK_TILDE] = parse_prefixop

	-- parse unit
	next_token()
	if tok ~= nil then
		unit.ast_idx = parse_block_body(srcpos_curr, nil, false)
		-- assert(tok == nil, "stopped before EOF, at " .. tokname(tok))
	else
		unit.ast_idx = 0
	end

	if DEBUG_PARSER then
		dlog("AST of unit %s: (%u B)", unit.srcfile, #unit.ast)
		print(ast_repr(unit, unit.ast_idx))
	end
end
