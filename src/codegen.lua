local gen = {}

function ast_is_nonnative_inttype(ast, typ_idx) --> bool
    if ast_is_inttype(typ_idx) then
        local nbits, _ = inttype_info(typ_idx)
        return nbits ~= 64
    end
    return false
end

function AST_INT.codegen_lua(ir, idx, g, flags)
    local _, is_neg, value = AST_INT.load(ir, idx)
    local typ_idx = ast_typeof(ir, idx)
    assert(ast_is_inttype(typ_idx))
    local nbits, is_signed = inttype_info(typ_idx)
    if (not is_signed and nbits > 63) or (is_neg and value < -9223372036854775807) then
        -- must encode as hex or lua will convert to float, integers...
        --   larger than 9223372036854775807 (INT64_MAX), or
        --   smaller than -9223372036854775807 (INT64_MIN+1)
        return g.write("0x" .. __rt.intfmt(value, 16, true))
    end
    return g.write(__rt.intfmt(value, 10, not is_neg))
end

function AST_FLOAT.codegen_lua(ir, idx, g, flags)
    return g.write(tostring(AST_FLOAT.load(ir, idx)))
end

function AST_PREFIXOP.codegen_lua(ir, idx, g, flags)
    local op, operand_idx = AST_PREFIXOP.load(ir, idx)
    local typ_idx = g.typeof(idx)
    local function gen_expr()
        g.write(tokname(op))
        g.codegen(operand_idx, RVALUE)
    end
    if ast_is_inttype(typ_idx) then
        local dst_nbits, dst_is_signed = inttype_info(typ_idx)
        error("FIXME")
        return g.intcast(nbits, is_signed, gen_expr)
    end
    return gen_expr()
end

function AST_BINOP.codegen_lua(ir, idx, g, flags)
    local op, left_idx, right_idx = AST_BINOP.load(ir, idx)
    local typ_idx = g.typeof(idx)

    local lua_op
    if op == TOK_SLASH and ast_is_inttype(typ_idx) then -- intdiv
        lua_op = '//'
    elseif op == TOK_HAT then -- xor
        lua_op = '~'
    elseif op == TOK_STARSTAR then -- exponentiation
        lua_op = '^'
    elseif op == TOK_NOTEQ then -- neq
        lua_op = '~='
    elseif op == TOK_ANDAND then -- logical and
        lua_op = 'and'
    elseif op == TOK_OROR then -- logical or
        lua_op = 'or'
    elseif op == TOK_NOT then -- logical not
        lua_op = 'not'
    else -- same as dew
        lua_op = tokname(op)
    end

    local function gen_expr()
        g.write("(")
        g.codegen(left_idx, RVALUE)
        g.write(lua_op)
        g.codegen(right_idx, RVALUE)
        g.write(")")
    end
    if ast_is_inttype(typ_idx) then
        local nbits, is_signed = inttype_info(typ_idx)
        error("FIXME")
        return g.intcast(nbits, is_signed, gen_expr)
    end
    return gen_expr()
end

function AST_REF.codegen_lua(ir, idx, g, flags)
    local target_idx, id_idx = AST_REF.target(ir, idx)
    if id_idx ~= 0 then
        return g.write(id_str(id_idx))
    end
    return g.codegen(target_idx)
end

function AST_VAR.codegen_lua(ir, idx, g, flags)
    local offs = ast_offs_of_idx(idx)
    local id_idx = ir:get_u32(offs) >> 8
    local value_idx = ir:get_i32(offs + 8)
    -- Note: "ast_kind(ir, value_idx) <= AST_RVALUE_MAX_KIND" is needed here since Lua does not
    -- allow rvalues to be used as statements. For example: (lua)
    --     do
    --         1 + 2
    --     end
    -- causes "syntax error near 'end'", so we check if the value is an rvalue and actually
    -- store it to hole '_' if so. Otherwise, for non rvalues and non-pure rvalues,
    -- like function calls, we elide the hole variable store, e.g. "_ = x()" becomes just "x()"
    if id_idx ~= ID__ or ast_kind(ir, value_idx) <= AST_RVALUE_MAX_KIND then
        if id_idx ~= ID__ then
            g.write("local ")
        end
        g.write(id_str(id_idx) .. " = ")
    end
    return g.codegen(value_idx, RVALUE)
end

function AST_ASSIGN.codegen_lua(ir, idx, g, flags)
    local offs = ast_offs_of_idx(idx)
    local dst_idx = ir:get_i32(offs + 8)
    local value_idx = ir:get_i32(offs + 12)
    local dst_kind = ast_kind(ir, dst_idx)

    if flags & RVALUE ~= 0 then
        -- e.g. "x = y = 3" => "x = (function() y = 3; return y; end)()"
        g.write("(function()")
    end

    local storage_lua = AST_VAR.id(ir, dst_idx)
    if dst_kind == AST_VAR.kind then
        g.write(storage_lua)
    else
        error("TODO: AST_ASSIGN.codegen_lua: storage %s", ast_descr(ir, dst_idx))
    end

    g.write(" = ")
    g.codegen(value_idx, flags | RVALUE)

    if flags & RVALUE ~= 0 then
        g.write("; return ")
        g.write(storage_lua)
        g.write("; end)()")
    end
end

function ast_list_elems_codegen_lua(ir, idx, g, flags, sepstr)
    local offs = ast_offs_of_idx(idx)
    local count = ir:get_u32(offs) >> 8
    local end_offs = offs + 4 + (count * 4)
    for offs = offs + 8, end_offs, 4 do
        g.codegen(ir:get_i32(offs), flags)
        if offs ~= end_offs then
            g.write(sepstr)
        end
    end
end

function AST_TUPLE.codegen_lua(ir, idx, g, flags, is_retval)
    local offs = ast_offs_of_idx(idx)
    local count = ir:get_u32(offs) >> 8
    local end_offs = offs + 4 + (count * 4)
    flags = flags | RVALUE
    if not is_retval then
        g.write("{")
    end
    for offs = offs + 8, end_offs, 4 do
        g.codegen(ir:get_i32(offs), flags)
        if offs ~= end_offs then
            g.write(",")
        end
    end
    if not is_retval then
        g.write("}")
    end
end

function AST_BLOCK.codegen_lua(ir, idx, g, flags)
    local offs = ast_offs_of_idx(idx)
    local count = ir:get_u32(offs) >> 8
    local end_offs = offs + 4 + (count * 4)

    local function codegen_elem(idx, flags)
        if flags & RVALUE == 0 and ast_kind(ir, idx) <= AST_RVALUE_MAX_KIND then
            -- See "Note" in AST_VAR.codegen_lua
            g.write("_ = ")
        end
        return g.codegen(idx, flags)
    end

    g.blocknest = g.blocknest + 1

    if g.blocknest == 1 then
        -- function body or unit
        -- Make sure to handle implicit return
        local return_last_elem = flags&RVALUE ~= 0 and g.fun_idx ~= 0
        for offs = offs + 8, end_offs, 4 do
            local elem_idx = ir:get_i32(offs)
            g.write("\n")
            if offs == end_offs and return_last_elem then
                if ast_kind(ir, elem_idx) ~= AST_RETURN.kind then
                    g.write("return ")
                end
                g.codegen(elem_idx, flags)
            else
                codegen_elem(elem_idx, flags)
            end
        end
    elseif flags & RVALUE == 0 then
        -- block as statement
        g.write("do")
        for offs = offs + 8, end_offs, 4 do
            g.write("\n")
            codegen_elem(ir:get_i32(offs), flags)
        end
        g.write("\nend")
    else
        -- block as expression (value)
        g.write("(function()")
        local child_flags = flags & ~RVALUE
        for offs = offs + 8, end_offs, 4 do
            if offs == end_offs then
                child_flags = flags
                g.write("\nreturn ")
            else
                g.write("\n")
            end
            codegen_elem(ir:get_i32(offs), child_flags)
        end
        g.write("\nend)()")
    end

    g.blocknest = g.blocknest - 1
end

function AST_PARAM.codegen_lua(ir, idx, g, flags)
    local id_idx, initval_idx, typ_idx = AST_PARAM.load(ir, idx)
    g.write(id_str(id_idx))
end

function AST_FUN.codegen_lua(ir, idx, g, flags)
    local id_idx, body_idx, params_idx, result_idx = AST_FUN.load(ir, idx)
    if body_idx == 0 then
        -- pure declaration
        return
    end

    local is_toplevel = flags&RVALUE == 0 and g.blocknest == 0
    flags = flags & ~RVALUE

    g.write("function")
    if id_idx ~= 0 and id_idx ~= ID__ then
        g.write(" " .. id_str(id_idx))
    elseif is_toplevel then
        g.write(" _")
    end

    -- parameters
    g.write("(")
    if ast_kind(ir, params_idx) == AST_TUPLE.kind then
        -- two or more parameters
        ast_list_elems_codegen_lua(ir, params_idx, g, flags, ", ")
    else
        if params_idx ~= 0 then
            -- one parameter
            g.codegen(params_idx, flags)
        end
    end
    g.write(")")

    -- body
    local body_kind = ast_kind(ir, body_idx)
    if result_idx ~= TYPE_nil then
        -- function produces results
        flags = flags | RVALUE
    end

    local blocknest = g.blocknest
    local fun_idx = g.fun_idx
    g.blocknest = 0
    g.fun_idx = idx

    if result_idx ~= TYPE_nil and body_kind ~= AST_BLOCK.kind and body_kind ~= AST_RETURN.kind then
        g.write("return ")
        g.codegen(body_idx, flags | RVALUE, true)
    else
        g.codegen(body_idx, flags)
    end

    g.fun_idx = fun_idx
    g.blocknest = blocknest

    g.write(" end")
end

function AST_RETURN.codegen_lua(ir, idx, g, flags)
    local value_idx = AST_RETURN.value(ir, idx)
    if value_idx == 0 then
        return g.write("return")
    end
    g.write("return ")
    return g.codegen(value_idx, flags | RVALUE, true)
end

function AST_CAST.codegen_lua(ir, idx, g, flags)
    local dst_typ, src_val = AST_CAST.load(ir, idx)
    local src_typ = g.typeof(src_val)
    local function gen_expr()
        g.codegen(src_val, RVALUE)
    end

    if ast_is_inttype(dst_typ) then
        local dst_nbits, dst_is_signed = inttype_info(dst_typ)
        if ast_is_inttype(src_typ) then
            -- int -> int
            local src_nbits, src_is_signed = inttype_info(src_typ)
            return g.intcast(src_nbits, src_is_signed, dst_nbits, dst_is_signed, gen_expr)
        elseif ast_is_floattype(src_typ) then
            -- float -> int
            g.write(dst_is_signed and "__rt.f_trunc_s(" or "__rt.f_trunc_u(")
            gen_expr(); g.write(fmt(",%u)", dst_nbits))
            return
        else
            error(fmt("TODO: %s -> %s", ast_fmt(ir, src_typ), ast_fmt(ir, dst_typ)))
        end
    elseif ast_is_floattype(dst_typ) then
        -- if ast_is_inttype(src_typ) then
        --     -- int -> float
        -- end
        error(fmt("TODO: %s -> %s", ast_fmt(ir, src_typ), ast_fmt(ir, dst_typ)))
    else
        error(fmt("TODO: %s -> %s", ast_fmt(ir, src_typ), ast_fmt(ir, dst_typ)))
    end

    return gen_expr()
end


---------------------------------------------------------------------------------------------------

function codegen_lua(unit) -- code
    if DEBUG_CODEGEN then dlog("\x1b[1;34mcodegen %s -> lua\x1b[0m", unit.srcfile) end

    local ir = unit.ir
    local depth = 0
    local buf = __rt.buf_create(512)
    local g = {
        unit = unit,
        blocknest = 0, -- current block depht, 0 = function or unit body
        fun_idx = 0, -- current function
    }

    function g.write(s) buf:append(s) end

    function g.typeof(idx)
        local ast = ir
        if idx < 0 then
            ast = builtin_ir
            idx = -idx
        end
        return ast:get_i32(ast_offs_of_idx(idx) + 4)
    end

    function g.intcast(src_nbits, src_is_signed, dst_nbits, dst_is_signed, inner_fn)
        if src_nbits < dst_nbits and (not src_is_signed or dst_is_signed) then
            -- trivial extension, e.g. u16(u8(3))=>3 or i16(u8(3))=>3 or i16(i8(-3))=>-3
            return inner_fn()
        elseif dst_is_signed then
            -- mask & sign extend if needed (depends on value, so done at runtime)
            g.write("__rt.sintconv("); inner_fn(); return g.write(fmt(",%u)", dst_nbits))
        else
            -- simple truncation
            g.write("("); inner_fn(); return g.write(fmt("&0x%x)", (1 << dst_nbits) - 1))
        end
    end

    function g.codegen(idx, flags, is_retval)
        assert(idx ~= 0, "g.codegen got idx #0")
        local kind
        if idx > 0 then
            kind = ir:get_u8(ast_offs_of_idx(idx))
        else
            kind = builtin_ir:get_u8(ast_offs_of_idx(-idx))
        end
        local info = ast_info[kind]
        assert(info.codegen_lua ~= nil, "TODO: " .. info.name .. ".codegen_lua")
        if flags == nil then
            flags = 0
        end
        return info.codegen_lua(ir, idx, g, flags == nil and 0 or flags, is_retval)
    end

    if unit.ir_idx ~= 0 then
        g.codegen(unit.ir_idx, 0)
    end

    local code = buf:str()

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
        -- __rt.intconv(srcval, src_bits, dst_bits, src_issigned, dst_issigned)
        g.write("__rt.intconv(")
        src_expr_gen()
        g.write(fmt(",%d,%d,%s,%s)",
                    src_bits,
                    dst_bits,
                    src_issigned and "true" or "false",
                    dst_issigned and "true" or "false"))
    end
end
