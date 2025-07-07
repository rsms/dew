local gen = {}

function AST_INT.codegen_lua(ir, idx, g, flags)
    local _, is_neg, value = AST_INT.load(ir, idx)
    if (not is_neg and value < 0) or (is_neg and value > -9223372036854775807) then
        -- must encode as hex or lua will convert to float
        return g.write("0x" .. __rt.intfmt(value, 16, true))
    end
    return g.write(__rt.intfmt(value, 10, not is_neg))
end

function AST_PREFIXOP.codegen_lua(ir, idx, g, flags)
    local op, operand_idx = AST_PREFIXOP.load(ir, idx)
    g.write(tokname(op))
    return g.codegen(operand_idx, RVALUE)
end

function AST_BINOP.codegen_lua(ir, idx, g, flags)
    local op, left_idx, right_idx = AST_BINOP.load(ir, idx)
    g.write("("); g.codegen(left_idx, RVALUE); g.write(" ")
    if op == TOK_SLASH then
        g.write('//') -- intdiv is different in Lua
    else
        g.write(tokname(op))
    end
    g.write(" "); g.codegen(right_idx, RVALUE); g.write(")")
end

function codegen_lua(unit) -- code
    if DEBUG_CODEGEN then dlog("\x1b[1;34mcodegen %s\x1b[0m", unit.srcfile) end

    local ir = unit.ir
    local depth = 0
    local buf = __rt.buf_create(512)
    local g = {
        unit = unit,
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

    function g.codegen(idx, flags)
        assert(idx ~= 0)
        local kind = ir:get_u8(ast_offs_of_idx(idx))
        local info = ast_info[kind]
        assert(info.codegen_lua ~= nil, "TODO: " .. info.name .. ".codegen_lua")
        if flags == nil then
            flags = 0
        end
        return info.codegen_lua(ir, idx, g, flags == nil and 0 or flags)
    end

    g.codegen(unit.ir_idx, 0)

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
