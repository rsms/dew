do

function ast_offs_of_idx(idx) return (idx - 1) * 8 end
function ast_idx_of_offs(offs) return offs//8 + 1 end

-- child visitor functions
local function visit_i32imm(ast, idx, visit)
    -- Node { kind u8; _ u24; imm_idx u32; }
    return visit(ast:get_i32(ast_offs_of_idx(idx) + 4))
end
local function visit_i32(ast, idx, visit)
    -- Node { kind u8; _ u24; meta u32; child_idx i32; }
    return visit(ast:get_i32(ast_offs_of_idx(idx) + 8))
end
local function visit_i32x2(ast, idx, visit)
    -- Node { kind u8; _ u24; meta u32; child1_idx, child2_idx i32; }
    local offs = ast_offs_of_idx(idx)
    visit(ast:get_i32(offs + 8))
    return visit(ast:get_i32(offs + 12))
end
local function visit_i32x3(ast, idx, visit)
    -- Node { kind u8; _ u24; meta u32; child1_idx, child2_idx, child3_idx i32; }
    local offs = ast_offs_of_idx(idx)
    visit(ast:get_i32(offs + 8))
    visit(ast:get_i32(offs + 12))
    return visit(ast:get_i32(offs + 16))
end
local function visit_list(ast, idx, visit)
    -- List { kind u8; count u24; meta u32; idxv i32[.count] }
    local offs = ast_offs_of_idx(idx)
    local count = ast:get_u32(offs) >> 8
    for offs = offs + 8, offs + 4 + (count * 4), 4 do
        visit(ast:get_i32(offs))
    end
end
local function visit_list_2d(ast, idx, visit)
    -- List2D { kind u8; count1 u24; count2 u32; idx1v i32[.count1]; idx2v i32[.count2] }
    local offs = ast_offs_of_idx(idx)
    local count = (ast:get_u32(offs) >> 8) + ast:get_u32(offs + 4)
    for offs = offs + 8, offs + 4 + (count * 4), 4 do
        visit(ast:get_i32(offs))
    end
end
local function visit_varlist(ast, idx, visit)
    -- List { kind u8; count u24; meta u32; vars Var[.count] }
    local offs = ast_offs_of_idx(idx)
    local count = ast:get_u32(offs) >> 8
    offs = offs + 8
    local end_offs = offs + count*AST_VAR.size
    while offs < end_offs do
        visit(ast_idx_of_offs(offs))
        offs = offs + AST_VAR.size
    end
end
local function visit_TODO(ast, idx) --> fun()i32?
    local kind = ast:get_u8(ast_offs_of_idx(idx))
    error("TODO: AST visitor for " .. ast_info[kind].name)
    return function() end
end

-- AST nodes
ast_info = {} -- kind u8 => info table
local function _(name, descr, visit)
    local info = {
        name = name,
        descr = descr,
        visit = visit,
        kind = #ast_info + 1,
        is_type = name:sub(-4) == "TYPE",
    }
    ast_info[info.kind] = info
    return info
end
ast_info[0] = {
    name = "<?>",
    descr = "<?>",
    kind = 0,
    is_type = false,
}

-- Pure rvalues (can only be used as rvalues, not as alone as "statements" in a block)
AST_NIL      = _('NIL',      'nil',                   nil) -- nil
AST_BOOL     = _('BOOL',     'boolean',               nil) -- true
AST_INT      = _('INT',      'integer',               nil) -- 123
AST_FLOAT    = _('FLOAT',    'floating-point number', nil) -- 1.23
AST_BINOP    = _('BINOP',    'infix operation',       visit_i32x2) -- 2 + 3
AST_PREFIXOP = _('PREFIXOP', 'prexif operation',      visit_i32) -- -n
AST_ID       = _('ID',       'identifier',            nil) -- x
AST_REF      = _('REF',      'REF',                   visit_i32imm) -- <reference>
AST_TUPLE    = _('TUPLE',    'tuple',                 visit_list) -- (x, 3)
AST_RVALUE_MAX_KIND = AST_TUPLE.kind

-- other expressions & statements
AST_NOOP      = _('NOOP',      'NOOP',                nil)
AST_PARAM     = _('PARAM',     'parameter',           visit_i32x2)   -- x int
AST_VAR       = _('VAR',       'variable definition', visit_i32)     -- id = expr
AST_MULTIVAR  = _('MULTIVAR',  'variables',           visit_varlist) -- x, y, z = 1, 2, 3
AST_SPREADVAR = _('SPREADVAR', 'variables',           visit_varlist) -- x, y, z = expr
AST_ASSIGN    = _('ASSIGN',    'assignment',          visit_i32x2)   -- x.y = z
AST_BLOCK     = _('BLOCK',     'block',               visit_list)    -- { ... }
AST_FUN       = _('FUN',       'function',            visit_i32x3)   -- fun f(x, y T) T
AST_RETURN    = _('RETURN',    'return statement',    visit_i32)     -- return 3
AST_CALL      = _('CALL',      'function call',       visit_list)    -- x(y, z)
AST_CONS      = _('CONS',      'constructor',         visit_list)    -- T(y, z)
AST_CAST      = _('CAST',      'type cast',           visit_i32)     -- T(x)

-- types
AST_PRIMTYPE  = _('PRIMTYPE',  'PRIMTYPE',      nil)         -- int
AST_TUPLETYPE = _('TUPLETYPE', 'tuple type',    visit_list)  -- (T, T)
AST_FUNTYPE   = _('FUNTYPE',   'function type', visit_i32x2) -- T -> T
AST_RESTTYPE  = _('RESTTYPE',  'rest type',     visit_i32)   -- ...T

-- TODO
AST_REST      = _('REST', 'rest', visit_TODO) -- ...
AST_ARRAYTYPE = _('ARRAYTYPE', 'array type', visit_TODO) -- [T]

--[[
Node {
    kind u8
    _    u24
    meta union {
        srcpos  u32
        typ_idx i32
        builtin u32
    }
}
]]
-------------------------------------------------------
-- Bool { kind u8; value u8; _ u16; meta u32 }
function AST_BOOL.repr(ast, idx, write)
    write(ast:get_u8(ast_offs_of_idx(idx) + 1) == 1 and " true" or " false")
end
-------------------------------------------------------
-- Int { kind u8; base u8; is_neg u8; _ u8; meta u32; value u64 }
function AST_INT.make(ast, meta, base, is_neg, value)
    local v24 = base | (is_neg and 0x100 or 0x000)
    return ast_add_u24_u64(ast, AST_INT.kind, meta, v24, value)
end
function AST_INT.load(ast, idx) --> base, is_neg, value
    local offs = ast_offs_of_idx(idx)
    local h = ast:get_u32(offs)
    return ((h>>8) & 0xff), (h>>16 & 0xff) == 1, ast:get_i64(offs + 8)
end
function AST_INT.str(ast, idx) --> string
    local base, is_neg, value = AST_INT.load(ast, idx)
    local prefix = base == 10 and "" or base == 16 and "0x" or base == 8 and "0o" or "0b"
    return prefix .. __rt.intfmt(value, base, not is_neg)
end
function AST_INT.repr(ast, idx, write, repr)
    return write(" " .. AST_INT.str(ast, idx))
end
function AST_INT.fmt(ast, idx, write, fmtchild)
    return write(AST_INT.str(ast, idx))
end
function AST_INT.resolve(ast, idx, ir, resolver, flags)
    -- if flags & RVALUE == 0 then
    --     resolver.diag_warn(ast_srcpos(ast, idx), "unused %s", ast_descr(ast, idx))
    -- end
    local base, is_neg, value = AST_INT.load(ast, idx)

    -- select type
    local typ_idx = resolver.ctxtype
    if not ast_is_inttype(typ_idx) then
        typ_idx = TYPE_int
    end

    -- check for overflow
    local nbits, is_signed = inttype_info(typ_idx)
    local function err_overflow()
        resolver.diag_err(ast_srcpos(ast, idx), "integer literal overflows %s",
                          ast_descr(ast, typ_idx))
    end
    if base == 0 then
        err_overflow()
    elseif is_signed then
        local smin = -(1 << (nbits - 1))
        local smax = (1 << (nbits - 1)) - 1
        -- dlog("AST_INT.resolve> %s nbits=%d is_signed=%s\n" ..
        --      "  smin  %20d 0x%016x\n" ..
        --      "  smax  %20d 0x%016x\n" ..
        --      "  value " .. (is_neg and "%20d" or "%20u") .. " 0x%016x",
        --      ast_descr(ast, typ_idx), nbits, tostring(is_signed),
        --      smin, smin, smax, smax, value, value)
        if (value < 0 and value < smin) or
           (value > 0 and value > smax) or
           (nbits == 64 and not is_neg and value < 0)
        then
            err_overflow()
        end
    else
        local umax = (1 << nbits) - 1
        -- dlog("AST_INT.resolve> %s nbits=%d is_signed=%s\n" ..
        --      "  umax  %20u 0x%016x\n" ..
        --      "  value " .. (is_neg and "%20d" or "%20u") .. " 0x%016x",
        --      ast_descr(ast, typ_idx), nbits, tostring(is_signed),
        --      umax, umax, value, value)
        if is_neg then
            -- e.g. "uint(-3)" or "uint(2 * -3)" or "fun f(x uint); f(-3)"
            -- Semantics: "uint16(-3)" => "uint16(UINT16_MAX - 3)" => "uint16(0xfffd)"
            local smin = -(1 << (nbits - 1))
            if (nbits < 64 and value+1 >= -umax) or value >= smin then
                value = __rt.intconv(value, 64, nbits, true, false)
            end
        end
        if nbits < 64 and (value < 0 or value > umax) then
            err_overflow()
        end
    end

    return AST_INT.make(ir, typ_idx, base, is_neg, value)
end
-------------------------------------------------------
-- Float { kind u8; _ u24; meta u32;
--         value f64 }
function AST_FLOAT.make(ast, meta, value)
    return ast_add_u24_f64(ast, AST_FLOAT.kind, meta, 0, value)
end
function AST_FLOAT.load(ast, idx) --> float
    return ast:get_f64(ast_offs_of_idx(idx) + 8)
end
function AST_FLOAT.repr(ast, idx, write, repr)
    return write(fmt(" %g", AST_FLOAT.load(ast, idx)))
end
function AST_FLOAT.resolve(ast, idx, ir, resolver)
    -- select type
    local typ_idx = resolver.ctxtype
    if not ast_is_floattype(typ_idx) then
        typ_idx = TYPE_float
    end
    return AST_FLOAT.make(ir, typ_idx, ast:get_f64(ast_offs_of_idx(idx) + 8))
end
-------------------------------------------------------
-- Id { kind u8; id_idx u24; meta u32; }
function AST_ID.make(ast, meta, id_idx)
    return ast_add_u24(ast, AST_ID.kind, meta, id_idx)
end
function AST_ID.repr(ast, idx, write, repr)
    local id_idx = ast:get_u32(ast_offs_of_idx(idx)) >> 8
    return write(" " .. id_str(id_idx))
end
function AST_ID.resolve(ast, idx, ir, resolver, flags) --> REF
    local id_idx = ast:get_u32(ast_offs_of_idx(idx)) >> 8
    local target_idx = resolver.id_lookup(id_idx)
    if target_idx == 0 then
        resolver.diag_err(ast_srcpos(ast, idx), "undefined '%s'", id_str(id_idx))
    elseif target_idx > 0 then
        -- increment load_count
        local target_kind = ast_kind(ir, target_idx)
        if target_kind == AST_VAR.kind or
           target_kind == AST_PARAM.kind or
           target_kind == AST_FUN.kind
        then
            AST_VAR.load_inc(ir, target_idx, 1)
        elseif not ast_info[target_kind].is_type then
            printf("TODO: #%d %s.load_inc (in AST_ID.resolve)",
                   target_idx, ast_info[target_kind].name)
        end
    end
    if ast_istype(ir, target_idx) then
        return target_idx
    end
    return AST_REF.make(ir, id_idx, target_idx)
end
-------------------------------------------------------
-- Ref { kind u8; id_idx u24; target_idx i32; }
function AST_REF.make(ast, id_idx, target_idx)
    return ast_add_u24(ast, AST_REF.kind, target_idx, id_idx)
end
function AST_REF.target(ast, idx) --> target_idx, id_idx
    local offs = ast_offs_of_idx(idx)
    return ast:get_i32(offs + 4), ast:get_u32(offs) >> 8
end
function AST_REF.repr(ast, idx, write, repr)
    local target_idx, id_idx = AST_REF.target(ast, idx)
    if id_idx ~= 0 then
        write(" " .. id_str(id_idx))
    end
    return repr(target_idx)
end
-------------------------------------------------------
-- Var { kind u8; id_idx u24; meta u32;
--       value_idx i32; load_count u16; store_count u16 }
AST_VAR.size = 16
function AST_VAR.make(ast, meta, id_idx, value_idx)
    return ast_add_u24_i32(ast, AST_VAR.kind, meta, id_idx, value_idx)
end
function AST_VAR.cons(ast, offs, meta, id_idx, value_idx) --> offs+size
    return ast_set_u24_i32x2(ast, offs, AST_VAR.kind, meta, id_idx, value_idx, 0)
end
function AST_VAR.set_value(ast, offs, value_idx) --> offs+size
    ast:set_i32(offs + 8, value_idx)
end
function AST_VAR.load_inc(ast, idx, additional_count)
    return ast:inc_u16(ast_offs_of_idx(idx) + 12, additional_count)
end
function AST_VAR.store_inc(ast, idx, additional_count)
    return ast:inc_u16(ast_offs_of_idx(idx) + 14, additional_count)
end
function AST_VAR.repr(ast, idx, write, repr)
    local offs = ast_offs_of_idx(idx)
    local id_idx = ast:get_u32(offs) >> 8
    write(" " .. id_str(id_idx))
    if id_idx ~= ID__ then
        local load_count = ast:get_u16(offs + 12)
        local store_count = ast:get_u16(offs + 14)
        write(fmt(" \x1b[1;34m{stores=%d loads=%d}\x1b[0m", store_count, load_count))
    end
    local value_idx = ast:get_i32(offs + 8)
    return repr(value_idx)
end
function AST_VAR.id(ast, idx) --> string
    local id_idx = ast:get_u32(ast_offs_of_idx(idx)) >> 8
    return id_str(id_idx)
end
function AST_VAR.fmt(ast, idx, write, fmt)
    return write("'" .. AST_VAR.id(ast, idx) .. "'")
end
function AST_VAR.resolve_cons(ast, ir, ast_offs, ir_offs, resolver, flags) --> idx
    local id_idx = ast:get_u32(ast_offs) >> 8
    local value_idx = ast:get_i32(ast_offs + 8)

    -- lookup existing storage
    local def_idx = resolver.id_lookup_def(id_idx)

    -- resolve value, in the context of the storage's type if existing storage was found
    local def_typ_idx = def_idx == 0 and 0 or resolver.typeof(def_idx)
    local outer_ctxtype = resolver.ctxtype
    resolver.ctxtype = def_typ_idx
    local ir_value_idx = resolver.resolve(value_idx, flags | RVALUE)
    local value_typ_idx = resolver.typeof(ir_value_idx)
    resolver.ctxtype = outer_ctxtype

    -- if no existing storage was found, construct a VAR definition
    if def_idx == 0 then
        AST_VAR.cons(ir, ir_offs, value_typ_idx, id_idx, ir_value_idx)
        if value_typ_idx == TYPE_nil and id_idx ~= ID__ then
            -- TODO: when/if we support optional values, allow use of 'nil' to signify "empty"
            resolver.diag_err(ast_srcpos(ast, value_idx), "cannot define a variable of type nil")
        elseif flags & RVALUE ~= 0 then
            local idx = ast_idx_of_offs(ast_offs)
            resolver.diag_err(ast_srcpos(ast, idx), "cannot use %s as a result value",
                  ast_descr(ast, idx))
            -- avoid "warning: unused variable"
            AST_VAR.load_inc(ir, ast_idx_of_offs(ir_offs), 1)
        end
        return resolver.id_define(id_idx, ast_idx_of_offs(ir_offs))
    end
    -- else, construct ASSIGN

    -- check if destination can be stored to
    local def_kind = ast_kind(ir, def_idx)
    if def_idx == ir_value_idx then
        -- e.g. "x = x"
        resolver.diag_err(
            srcpos_union(ast_srcpos(ast, ast_idx_of_offs(ast_offs)),
                         ast_srcpos(ast, value_idx)),
            "cannot assign %s to itself", ast_descr(ir, def_idx))
    else
        resolver.check_type_assignable(def_typ_idx, value_typ_idx, ir_value_idx)
    end

    -- increment store_count
    local target_kind = ast_kind(ir, def_idx)
    if target_kind == AST_VAR.kind or target_kind == AST_PARAM.kind then
        AST_VAR.store_inc(ir, def_idx, 1)
    elseif not ast_info[target_kind].is_type then
        printf("TODO: #%d %s.store_inc", def_idx, ast_kindname(ast, def_idx))
    end

    AST_ASSIGN.cons(ir, ir_offs, value_typ_idx, def_idx, ir_value_idx)
    return ast_idx_of_offs(ir_offs)
end
function AST_VAR.resolve(ast, idx, ir, resolver, flags)
    local ast_offs = ast_offs_of_idx(idx)
    local ir_offs = ir:alloc(AST_VAR.size)
    return AST_VAR.resolve_cons(ast, ir, ast_offs, ir_offs, resolver, flags)
end
-------------------------------------------------------
-- Assign { kind u8; _ u24; meta u32;
--          dst_idx i32; value_idx i32; }
function AST_ASSIGN.cons(ast, offs, meta, dst_idx, value_idx) --> offs+size
    return ast_set_u24_i32x2(ast, offs, AST_ASSIGN.kind, meta, 0, dst_idx, value_idx)
end
-------------------------------------------------------
-- MultiVar = { kind u8; count u24; meta u32;
--              vars Var[.count] }
function AST_MULTIVAR.alloc(ast, meta, count) --> offs
    return varlist_alloc(ast, AST_MULTIVAR.kind, meta, count)
end
function AST_MULTIVAR.resolve(ast, idx, ir, resolver, flags)
    local ast_offs = ast_offs_of_idx(idx)
    local count = ast:get_u32(ast_offs) >> 8
    local ir_offs = varlist_alloc(ir, AST_MULTIVAR.kind, TYPE_nil, count)
    local ir_idx = ast_idx_of_offs(ir_offs)
    ast_offs = ast_offs + 8
    ir_offs = ir_offs + 8
    for i = 0, count - 1 do
        AST_VAR.resolve_cons(ast, ir, ast_offs + i*AST_VAR.size, ir_offs + i*AST_VAR.size,
                             resolver, flags)
    end
    return ir_idx
end
-------------------------------------------------------
-- SpreadVar = { kind u8; count u24; meta u32;
--               vars Var[.count] }
function AST_SPREADVAR.resolve(ast, idx, ir, resolver, flags)
    local ast_offs = ast_offs_of_idx(idx)
    local count = ast:get_u32(ast_offs) >> 8
    local ir_offs = varlist_alloc(ir, AST_SPREADVAR.kind, TYPE_nil, count)
    local ir_idx = ast_idx_of_offs(ir_offs)
    ast_offs = ast_offs + 8
    ir_offs = ir_offs + 8

    -- temporary storage of effective TUPLETYPE of destination vars
    typ_idxv = __rt.buf_create(count * 4)
    typ_idxv:alloc(count * 4)

    -- resolve value (its idx is stored in the first VAR's value_idx slot)
    local value_idx = ast:get_i32(ast_offs + 8)
    local ir_value_idx = resolver.resolve(value_idx, flags | RVALUE)
    local value_typ_idx = resolver.typeof(ir_value_idx)

    -- check if RHS is of tuple type, with matching count
    local value_count = 0
    if ast_kind(ir, value_typ_idx) ~= AST_TUPLETYPE.kind then
        resolver.diag_err(ast_srcpos(ast, value_idx),
            "cannot spread value of non-tuple type %s", ast_fmt(ir, value_typ_idx))
    else
        value_count = ast_list_count(ir, value_typ_idx)
        if value_count ~= count then
            if value_count > count then
                resolver.diag_err(ast_srcpos(ast, value_idx),
                                  "too many values in spread expression: want %d, got %d",
                                  count, value_count)
            else
                resolver.diag_err(ast_srcpos(ast, value_idx),
                                  "not enough values in spread expression: want %d, got %d." ..
                                  " Use '_' to ignore value",
                                  count, value_count)
            end
        end
    end

    -- resolve VARs as either VAR (definition) or ASSIGN (assignment)
    for i = 0, count - 1 do
        local id_idx = ast:get_u32(ast_offs + i*AST_VAR.size) >> 8
        local def_idx = resolver.id_lookup_def(id_idx)
        local val_type = TYPE_any
        if i < value_count then
            val_type = ast_list_elem(ir, value_typ_idx, i)
        end
        if i > 0 then
            ir_value_idx = 0
        end
        if def_idx == 0 then
            -- variable definition
            local var_offs = ir_offs + i*AST_VAR.size
            AST_VAR.cons(ir, var_offs, val_type, id_idx, ir_value_idx)
            local var_idx = ast_idx_of_offs(var_offs)
            resolver.id_define(id_idx, var_idx)
        else
            -- assignment
            local def_typ_idx = resolver.typeof(def_idx)
            if def_typ_idx ~= val_type and val_type ~= TYPE_any then
                local var_idx = ast_idx_of_offs(ast_offs + i*AST_VAR.size)
                resolver.diag_err(ast_srcpos(ast, var_idx),
                    "cannot assign spread value of type %s to %s of type %s",
                    ast_fmt(ir, val_type), ast_descr(ir, def_idx), ast_fmt(ir, def_typ_idx))
            end
            AST_ASSIGN.cons(ir, ir_offs + i*AST_VAR.size, val_type, def_idx, ir_value_idx)
        end
    end

    return ir_idx
end
-------------------------------------------------------
-- AstParam { kind u8; id_idx u24; meta u32;
--            initval_idx i32; typ_idx i32; }
-- IRParam = Var
function AST_PARAM.make(ast, meta, id_idx, initval_idx, typ_idx)
    return ast_add_u24_i32x2(ast, AST_PARAM.kind, meta, id_idx, initval_idx, typ_idx)
end
function AST_PARAM.set_type(ast, offs, typ_idx)
    return ast:set_i32(offs + 12, typ_idx)
end
function AST_PARAM.load(ast, idx) --> id_idx, initval_idx, typ_idx
    local offs = ast_offs_of_idx(idx)
    return (ast:get_u32(offs) >> 8), ast:get_i32(offs + 8), ast:get_i32(offs + 12)
end
function AST_PARAM.repr(ast, idx, write, repr, as_ir)
    local id_idx, initval_idx, typ_idx = AST_PARAM.load(ast, idx)
    write(" " .. id_str(id_idx))
    if as_ir then
        if id_idx ~= ID__ then
            local offs = ast_offs_of_idx(idx)
            local load_count = ast:get_u16(offs + 12)
            local store_count = ast:get_u16(offs + 14)
            write(fmt(" \x1b[1;34m{stores=%d loads=%d}\x1b[0m", store_count, load_count))
        end
    else
        repr(typ_idx)
    end
    if initval_idx ~= 0 then
        return repr(initval_idx)
    end
end
function AST_PARAM.fmt(ast, idx, write, fmt)
    return AST_VAR.fmt(ast, idx, write, fmt)
end
function AST_PARAM.resolve(ast, idx, ir, resolver, flags)
    local offs = ast_offs_of_idx(idx)
    local id_idx = ast:get_u32(offs) >> 8
    local initval_idx = ast:get_i32(offs + 8)
    local typ_idx = ast:get_i32(offs + 12)
    typ_idx = resolver.resolve(typ_idx, flags | RVALUE)
    if initval_idx ~= 0 then
        initval_idx = resolver.resolve(initval_idx, flags | RVALUE)
        dlog("TODO: PARAM.resovle: check type match of initval_idx vs typ_idx")
    end
    local collision = resolver.id_lookup_def_in_current_scope(id_idx)
    if collision ~= 0 then
        resolver.diag_err(ast_srcpos(ast, idx), "duplicate parameter '%s'", id_str(id_idx))
    end
    idx = AST_PARAM.make(ir, typ_idx, id_idx, initval_idx, 0)
    return resolver.id_define(id_idx, idx)
end
-------------------------------------------------------
-- List { kind u8; count u24; meta u32;
--        idxv i32[.count] }
-- Block = List of Stmt
function AST_BLOCK.make(ast, meta, idxv_i32)
    return ast_list_add(ast, AST_BLOCK.kind, meta, idxv_i32)
end
function AST_BLOCK.resolve(ast, idx, ir, resolver, flags)
    local offs = ast_offs_of_idx(idx)
    local kind, count = 0, ast:get_u32(offs)
    kind, count = count & 0xff, count >> 8
    local id_scope = resolver.id_scope_open()

    -- if the block contains just a single thing, just yield that
    -- e.g. "{3}" => "3"
    if count == 1 then
        local elem_idx = ast:get_i32(offs + 8)
        -- Note: we must keep BLOCK around VAR definition because of lexical scoping of variables
        if ast_kind(ast, elem_idx) ~= AST_VAR.kind then
            idx = resolver.resolve(elem_idx, flags)
            resolver.id_scope_close(id_scope, true)
            return idx
        end
    end

    local ir_offs = ast_list_add_upfront(ir, kind, TYPE_nil, count)

    if count > 0 then
        -- Open ctxtype scope: saves current resolver.ctxtype and resets it to 0.
        -- Later, for the last expression of the block, we restore ctxtype.
        --   e.g. "4" in "uint8({ 4; 5 })" should be interpreted as default "int", not "uint8".
        local ctxtype = resolver.ctxtype
        resolver.ctxtype = 0
        local end_offs = offs + 4 + (count * 4)
        local elem_offs = ir_offs + 8
        local elem_idx = 0
        local child_flags = flags & ~RVALUE
        for offs = offs + 8, end_offs, 4 do
            if offs == end_offs then -- last child
                child_flags = flags
                resolver.ctxtype = ctxtype
            end

            elem_idx = ast:get_i32(offs)
            elem_idx = resolver.resolve(elem_idx, child_flags)

            ir:set_i32(elem_offs, elem_idx)
            elem_offs = elem_offs + 4
        end
        -- When a block is used as an rvalue, its type is the type of the last expression
        if flags & RVALUE ~= 0 then
            ir:set_i32(ir_offs + 4, resolver.typeof(elem_idx))
        end
    end

    resolver.id_scope_close(id_scope, true)

    return ast_idx_of_offs(ir_offs)
end
-------------------------------------------------------
-- Tuple = List of Expr
function AST_TUPLE.make(ast, meta, idxv_i32)
    return ast_list_add(ast, AST_TUPLE.kind, meta, idxv_i32)
end
function AST_TUPLE.fmt(ast, idx, write, fmt)
    return ast_list_fmt(ast, idx, write, fmt)
end
function AST_TUPLE.resolve_elems(ast, ast_offs, ir, ir_offs, count, resolver, flags) --> typ_idx
    -- temporary storage for element types
    local typ_idxv_offs = 0
    local typ_idxv = __rt.buf_create(count * 4, count * 4)

    -- ctxtype spread, i.e.
    --   ctxtype "(TUPLETYPE (int16, uint8))"
    --   value   "(1, 2)"
    --   means ctxtype is "int16" for "1" and "uint8" for "2"
    local ctxtype = resolver.ctxtype
    local ctxtype_offs = 0
    if ast_kind(ir, ctxtype) == AST_TUPLETYPE.kind and ast_list_count(ir, ctxtype) >= count then
        ctxtype_offs = ast_offs_of_idx(ctxtype) + 8 -- first List element
    end

    -- resolve each element
    for elem_offs = ast_offs, (ast_offs + ((count - 1) * 4)), 4 do
        if ctxtype_offs ~= 0 then
            resolver.ctxtype = ir:get_i32(ctxtype_offs)
            ctxtype_offs = ctxtype_offs + 4
        end

        local elem_idx = ast:get_i32(elem_offs)
        elem_idx = resolver.resolve(elem_idx, flags)

        ir:set_i32(ir_offs, elem_idx)
        ir_offs = ir_offs + 4

        local elem_typ_idx = resolver.typeof(elem_idx)

        -- If any type is invalid, make the list's type invalid.
        -- For example in "(x, y, ze)" say "ze" is undefined which means its type is 0,
        -- then we want the type of the tuple to also be 0. Otherwise we would produce
        -- cascading (confusing) error messages.
        if elem_typ_idx == 0 then
            resolver.ctxtype = ctxtype
            return 0
        end

        typ_idxv:set_i32(typ_idxv_offs, elem_typ_idx)
        typ_idxv_offs = typ_idxv_offs + 4
    end

    resolver.ctxtype = ctxtype

    -- Intern TUPLETYPE (should match AST_TUPLETYPE.resolve & resolver.intern_type).
    -- TODO: consider if/when we can use ctxtype (when ctxtype_offs~=0).
    -- We can simply hash the array of idx values, with the type's kind as the hash seed.
    local typ_hash = typ_idxv:hash(AST_TUPLETYPE.kind)
    local typ_idx = resolver.internmap[typ_hash]
    if typ_idx == nil then
        typ_idx = ast_list_add(ir, AST_TUPLETYPE.kind, TYPE_type, typ_idxv)
        resolver.internmap[typ_hash] = typ_idx
    end

    return typ_idx
end
function AST_TUPLE.resolve(ast, idx, ir, resolver, flags)
    local ast_offs = ast_offs_of_idx(idx)
    local kind = ast:get_u8(ast_offs)
    local count = ast:get_u32(ast_offs) >> 8
    local ir_offs = ast_list_add_upfront(ir, kind, 0, count)
    local typ_idx = AST_TUPLE.resolve_elems(
        ast, ast_offs + 8, ir, ir_offs + 8, count, resolver, flags|RVALUE)
    ir:set_i32(ir_offs + 4, typ_idx)
    return ast_idx_of_offs(ir_offs)
end
-------------------------------------------------------

-- TupleType = List of Type
function AST_TUPLETYPE.make(ast, meta, idxv_i32)
    return ast_list_add(ast, AST_TUPLETYPE.kind, meta, idxv_i32)
end
function AST_TUPLETYPE.resolve(ast, idx, ir, resolver, flags)
    local offs = ast_offs_of_idx(idx)
    local count = ast:get_u32(offs) >> 8
    local ir_offs = ast_list_add_upfront(ir, AST_TUPLETYPE.kind, TYPE_type, count)
    local ir_child_offs = ir_offs + 8
    flags = flags | RVALUE -- make sure IDs resolve to targets rather than REFs
    for offs = offs + 8, offs + 4 + (count * 4), 4 do
        local child_idx = resolver.resolve(ast:get_i32(offs), flags)
        ir:set_i32(ir_child_offs, child_idx)
        ir_child_offs = ir_child_offs + 4
    end
    -- intern type (should match AST_TUPLE.resolve)
    local hash_offs, hash_size = ir_offs+8, count*4
    idx = ast_idx_of_offs(ir_offs)
    return resolver.intern_type(idx, AST_TUPLETYPE.kind, ir_offs, hash_offs, hash_size)
end
function AST_TUPLETYPE.fmt(ast, idx, write, fmt)
    return ast_list_fmt(ast, idx, write, fmt)
end
-------------------------------------------------------
-- Return { kind u8; _ u24; meta u32;
--          value_idx i32; _ u32; }
function AST_RETURN.alloc(ast) --> offs
    return ast:alloc(16)
end
function AST_RETURN.cons(ast, offs, meta, value_idx) --> idx
    ast_set_node(ast, offs, AST_RETURN.kind, meta, 0)
    ast:set_i32(offs + 8, value_idx)
    return offs // 8 + 1
end
function AST_RETURN.make(ast, meta, value_idx)
    return AST_RETURN.cons(ast, AST_RETURN.alloc(ast), meta, value_idx)
end
function AST_RETURN.value(ast, idx) --> idx
    return ast:get_i32(ast_offs_of_idx(idx) + 8)
end
function AST_RETURN.resolve(ast, idx, ir, resolver, flags)
    local ir_offs = AST_RETURN.alloc(ir)
    local typ_idx = TYPE_nil
    local value_idx = ast:get_i32(ast_offs_of_idx(idx) + 8)

    if resolver.fun_idx == 0 then
        resolver.diag_err(ast_srcpos(ast, idx), "return statement outside function body")
        value_idx = 0
    elseif value_idx ~= 0 then
        -- set ctxtype to function's result type
        local outer_ctxtype = resolver.ctxtype
        resolver.ctxtype = resolver.fun_restyp_idx
        value_idx = resolver.resolve(value_idx, flags | RVALUE)
        resolver.ctxtype = outer_ctxtype
        typ_idx = resolver.typeof(value_idx)
    end

    local ir_idx = AST_RETURN.cons(ir, ir_offs, typ_idx, value_idx)

    resolver.record_srcpos(ir_idx, ast_srcpos(ast, idx))
    if resolver.fun_idx ~= 0 then
        AST_FUN.check_result_type(ir, resolver.fun_idx, ir_idx, resolver)
    end
    return ir_idx
end
-------------------------------------------------------
-- Cast { kind u8; _ u24; dst_typ_idx i32;
--        src_val_idx i32; _ i32 }
function AST_CAST.make(ast, dst_typ_idx, src_val_idx) --> idx
    return ast_add_u24_i32(ast, AST_CAST.kind, dst_typ_idx, 0, src_val_idx)
end
function AST_CAST.load(ast, idx) --> dst_typ_idx, src_val_idx
    local offs = ast_offs_of_idx(idx)
    return ast:get_i32(offs + 4), ast:get_i32(offs + 8)
end
-------------------------------------------------------
-- Cons { kind u8; _ u24; meta u32;
--        recv_idx i32; args_idxv i32[.count-1]; }
-------------------------------------------------------
-- Call { kind u8; count u24; meta u32;            -- List with recv_idx as first element
--        recv_idx i32; args_idxv i32[.count-1]; }
function AST_CALL.resolve_cast(ast, idx, ir, resolver, flags, dst_typ, count)
    local src_idx = (count > 1) and ast:get_i32(ast_offs_of_idx(idx) + 12) or 0
    resolver.ctxtype = dst_typ
    src_idx = resolver.resolve(src_idx, flags)
    local src_typ = resolver.typeof(src_idx)

    -- special handling of "type(expr)"
    if dst_typ == TYPE_type then
        -- return (flags & RVALUE ~= 0) and src_typ or AST_REF.make(ir, 0, src_typ)
        return src_typ
    end

    -- if argument is already of same type, there's no need for conversion
    if src_typ == dst_typ or dst_typ == 0 then
        return src_idx
    end

    -- if dst_typ > TYPE_any then
    --     resolver.diag_err(ast_srcpos(ast, idx), "cannot convert %s to %s",
    --                       ast_descr(ir, src_idx), ast_fmt(ir, dst_typ))
    --     return 0
    -- end
    -- if dst_typ ~= TYPE_any then ... end

    -- Check if types are convertible.
    -- Note: Currentl resolve_cast is only called when we know that the destination type
    -- is a primitive. Assert for now, as that may change (i.e. for tuples or arrays)
    assert(dst_typ < 0 and dst_typ >= TYPE_last_int_type)
    if src_typ > 0 or src_typ < TYPE_last_int_type then
        -- source is not a primitive type
        dlog("TODO: check is_convertible %s -> %s (AST_CALL.resolve_cast)",
             ast_fmt(ir, src_typ), ast_fmt(ir, dst_typ))
        resolver.diag_err(ast_srcpos(ast, idx), "cannot convert %s to %s",
                          ast_fmt(ir, src_typ), ast_fmt(ir, dst_typ))
        return 0
    elseif src_typ == TYPE_nil or src_typ == TYPE_type then
        resolver.diag_err(ast_srcpos(ast, idx), "cannot convert %s to %s",
                          ast_fmt(ir, src_typ), ast_fmt(ir, dst_typ))
        return 0
    elseif src_typ == TYPE_any then
        error("TODO: convert from 'any' type")
    end

    return AST_CAST.make(ir, dst_typ, src_idx)
end
function AST_CALL.resolve_cons(ast, idx, ir, resolver, flags, recv_idx, count)
    -- result type of construction is the type being constructed
    resolver.ctxtype = recv_idx

    -- lookup constructor function for recv_idx
    local cons_fn_idx = 0 -- TODO FIXME
    if cons_fn_idx ~= 0 then
        dlog("TODO: custom constructor call (AST_CALL.resolve_cons)")
        return AST_CALL.resolve_call(ast, idx, ir, resolver, flags, cons_fn_idx, count)
    end

    local ir_idx, args_typ = AST_CALL.resolve_call1(
        ast, idx, ir, resolver, flags, AST_CONS.kind, recv_idx, recv_idx, count)
    local resolve_default_cons = ast_info[ast_kind(ir, recv_idx)].resolve_default_cons
    if resolve_default_cons ~= nil then
        if resolve_default_cons(ir, ir_idx, resolver, recv_idx) then
            return ir_idx
        end
    end

    local args_fmt = "()"
    if args_typ ~= 0 then
        args_fmt = ast_fmt(ir, args_typ)
        if ast_kind(ir, args_typ) ~= AST_TUPLETYPE.kind then
            args_fmt = "(" .. args_fmt .. ")"
        end
    end
    return resolver.diag_err(ast_srcpos(ast, idx), "no constructor available for %s%s",
                             ast_fmt(ir, recv_idx), args_fmt)
end
function AST_CALL.resolve_call(ast, idx, ir, resolver, flags, recv_idx, count)
    -- result type of a call is the result type of the function being called
    local typ_idx = AST_FUN.result_type(ir, recv_idx)
    resolver.ctxtype = 0
    dlog("TODO: set ctxtype for %s (AST_CALL.resolve_call)", ast_kindname(ir, recv_idx))
    return AST_CALL.resolve_call1(
        ast, idx, ir, resolver, flags, AST_CALL.kind, typ_idx, recv_idx, count)
end
function AST_CALL.resolve_call1(ast, idx, ir, resolver, flags, kind, typ_idx, recv_idx, count)
    --> idx, typ_idx
    local ast_offs = ast_offs_of_idx(idx)
    local ir_offs = ast_list_add_upfront(ir, kind, typ_idx, count + 1)
    ir:set_i32(ir_offs + 8, recv_idx)
    local args_typ_idx = AST_TUPLE.resolve_elems(
        ast, ast_offs + 12, ir, ir_offs + 12, count - 1, resolver, flags | RVALUE)
    -- last list element is TUPLETYPE of arguments
    ir:set_i32(ir_offs + 8 + count*4, args_typ_idx)
    return ast_idx_of_offs(ir_offs), args_typ_idx
end
function AST_CALL.resolve(ast, idx, ir, resolver, flags) --> idx
    local ast_offs = ast_offs_of_idx(idx)
    local count = ast:get_u32(ast_offs) >> 8
    local recv_idx = ast:get_i32(ast_offs + 8)
    local ir_recv_idx = ast_deref(ir, resolver.resolve(recv_idx, flags | RVALUE))
    local outer_ctxtype = resolver.ctxtype
    if ast_istype(ir, ir_recv_idx) then
        if ir_recv_idx < 0 and ir_recv_idx >= TYPE_last_int_type then
            -- there are no constructors for PRIMTYPEs, only conversion: e.g. "int8(int32(1000))"
            idx = AST_CALL.resolve_cast(ast, idx, ir, resolver, flags, ir_recv_idx, count)
        else
            idx = AST_CALL.resolve_cons(ast, idx, ir, resolver, flags, ir_recv_idx, count)
        end
    elseif ast_kind(ir, resolver.typeof(ir_recv_idx)) == AST_FUNTYPE.kind then
        idx = AST_CALL.resolve_call(ast, idx, ir, resolver, flags, ir_recv_idx, count)
    else
        idx = 0
        resolver.diag_err(ast_srcpos(ast, recv_idx), "cannot call %s of type %s",
                          ast_fmt(ir, ir_recv_idx),
                          ast_fmt(ir, resolver.typeof(ir_recv_idx)))
    end
    resolver.ctxtype = outer_ctxtype
    return idx
end
-------------------------------------------------------
-- RestType { kind u8; _ u24; meta u32;
--            elem_typ_idx i32; _ u32; }
function AST_RESTTYPE.alloc(ast) --> offs
    return ast:alloc(16)
end
function AST_RESTTYPE.cons(ast, offs, meta, elem_typ_idx) --> idx
    ast_set_node(ast, offs, AST_RESTTYPE.kind, meta, 0)
    ast:set_i32(offs + 8, elem_typ_idx)
    return offs // 8 + 1
end
function AST_RESTTYPE.make(ast, meta, elem_typ_idx)
    return AST_RESTTYPE.cons(ast, AST_RESTTYPE.alloc(ast), meta, elem_typ_idx)
end
function AST_RESTTYPE.resolve(ast, idx, ir, resolver, flags)
    local ir_offs = AST_RESTTYPE.alloc(ir)

    local elem_typ_idx = ast:get_i32(ast_offs_of_idx(idx) + 8)
    elem_typ_idx = resolver.resolve(elem_typ_idx)
    local typ_idx = resolver.typeof(elem_typ_idx)

    idx = AST_RESTTYPE.cons(ir, ir_offs, typ_idx, elem_typ_idx)

    -- intern type
    local typ_hash = ir:hash(AST_RESTTYPE.kind, ir_offs, ir_offs+4)
    local other_idx = resolver.internmap[typ_hash]
    if other_idx == nil then
        resolver.internmap[typ_hash] = idx
        return idx
    else
        ir:resize(ir_offs) -- undo additions to IR stack
        return other_idx
    end
end
-------------------------------------------------------
-- Fun { kind u8; id_idx u24; meta u32;       ╮_ binary compat. with Var
--       body_idx i32; load_count u16; _ u16; ╯
--       params_idx i32; result_idx i32; }
function AST_FUN.make(ast, meta, id_idx, body_idx, params_idx, result_idx)
    local header = AST_FUN.kind | (id_idx & 0xffffff)<<8 | ((meta & 0xffffffff) << 32)
    local offs = ast:push_u64(header)
    local a = body_idx & 0xffffffff
    local b = (params_idx & 0xffffffff) | (result_idx & 0xffffffff)<<32
    ast:push_u64x2(a, b)
    return ast_idx_of_offs(offs)
end
function AST_FUN.alloc(ast, id_idx) --> offs
    local offs = ast:alloc(24)
    ast:set_u32(offs, AST_FUN.kind | (id_idx & 0xffffff)<<8)
    return offs
end
function AST_FUN.load(ast, idx) --> id_idx, body_idx, params_idx, result_idx
    local offs = ast_offs_of_idx(idx)
    local id_idx = ast:get_u32(offs) >> 8
    local body_idx = ast:get_i32(offs + 8)
    local params_idx = ast:get_i32(offs + 16)
    local result_idx = ast:get_i32(offs + 20)
    return id_idx, body_idx, params_idx, result_idx
end
function AST_FUN.result(ast, idx) --> result_idx
    return ast:get_i32(ast_offs_of_idx(idx) + 20)
end
function AST_FUN.result_type(ast, idx) --> typ_idx
    local result_idx = ast:get_i32(ast_offs_of_idx(idx) + 20)
    if ast_kind(ast, result_idx) == AST_TUPLE.kind then
        -- result is named parameters; use type of that tuple, e.g. "(a, b int)" => "(int,int)"
        return ast_typeof(ast, result_idx)
    end
    return result_idx
end
function AST_FUN.body(ast, idx) --> body_idx
    return ast:get_i32(ast_offs_of_idx(idx) + 8)
end
function AST_FUN.repr(ast, idx, write, repr)
    local id_idx, body_idx, params_idx, result_idx = AST_FUN.load(ast, idx)
    if id_idx ~= 0 and id_idx ~= ID__ then
        write(" " .. id_str(id_idx))
        local load_count = ast:get_u16(ast_offs_of_idx(idx) + 12)
        write(fmt(" \x1b[1;34m{uses=%d}\x1b[0m", load_count))
    else
        write(" _")
    end
    if params_idx == 0 and result_idx == 0 and body_idx == 0 then return end
    repr(params_idx)
    if result_idx == 0 and body_idx == 0 then return end
    repr(result_idx)
    if body_idx == 0 then return end
    return repr(body_idx)
end
function AST_FUN.fmt(ast, idx, write, fmt)
    -- TODO: custom
    return AST_VAR.fmt(ast, idx, write, fmt)
end
function AST_FUN.resolve(ast, idx, ir, resolver, flags)
    local id_idx, body_idx, params_idx, result_idx = AST_FUN.load(ast, idx)

    -- make & define function up front, since its params and body may reference it
    local offs = AST_FUN.alloc(ir, id_idx)
    resolver.id_define(id_idx, ast_idx_of_offs(offs))

    local child_flags = flags & ~RVALUE
    local param_scope = resolver.id_scope_open()
    params_idx = params_idx == 0 and 0 or resolver.resolve(params_idx, child_flags)
    result_idx = result_idx == 0 and TYPE_nil or resolver.resolve(result_idx, child_flags)

    -- result is named parameters; use type of that tuple, e.g. "(a, b int)" => "(int,int)"
    local result_typ_idx = result_idx
    if ast_kind(ir, result_idx) == AST_TUPLE.kind then
        result_typ_idx = ast_typeof(ir, result_idx)
    end

    local typ_idx = AST_FUNTYPE.make(ir, resolver, params_idx, result_typ_idx)
    ir:set_i32(offs + 4, typ_idx)
    ir:set_i64(offs + 8, 0) -- body_idx i32, load_count u16; _ u16;
    ir:set_i32(offs + 16, params_idx)
    ir:set_i32(offs + 20, result_idx) -- body_idx

    local ir_idx = ast_idx_of_offs(offs)
    local warn_unused = false -- don't warn about unused params unless there's a body

    if body_idx ~= 0 then
        warn_unused = true
        resolver.record_srcpos(ir_idx, ast_srcpos(ast, idx))

        local body_scope = resolver.id_scope_open()
        local outer_fun_idx = resolver.fun_idx
        local outer_fun_restyp_idx = resolver.fun_restyp_idx
        resolver.fun_idx = ir_idx
        resolver.fun_restyp_idx = result_typ_idx

        local outer_ctxtype = resolver.ctxtype
        if result_idx ~= TYPE_nil then
            child_flags = child_flags | RVALUE
            resolver.ctxtype = result_typ_idx
        else
            resolver.ctxtype = 0
        end

        local body_ir_idx = resolver.resolve(body_idx, child_flags)

        resolver.ctxtype = outer_ctxtype

        resolver.fun_restyp_idx = outer_fun_restyp_idx
        resolver.fun_idx = outer_fun_idx
        resolver.id_scope_close(body_scope, warn_unused)

        if result_idx ~= TYPE_nil then
            AST_FUN.check_result_type(ir, ir_idx, body_ir_idx, resolver)
        end

        ir:set_i32(offs + 8, body_ir_idx)
    end

    resolver.id_scope_close(param_scope, warn_unused)

    return ir_idx
end
function AST_FUN.check_result_type(ir, fun_idx, src_idx, resolver)
    local dst_idx = AST_FUN.result(ir, fun_idx)
    local dst_typ_idx = ast_istype(ir, dst_idx) and dst_idx or resolver.typeof(dst_idx)
    local src_typ_idx = resolver.typeof(src_idx)
    if resolver.is_type_assignable(dst_typ_idx, src_typ_idx) then
        return
    end

    -- select srcpos
    local focus_idx = src_idx
    if ast_kind(ir, src_idx) == AST_BLOCK.kind and ast_list_count(ir, src_idx) > 0 then
        -- use srcpos of last expression of block
        focus_idx = ast_list_elem(ir, src_idx, ast_list_count(ir, src_idx) - 1)
    end
    local srcpos = resolver.srcpos(focus_idx)

    -- produce diagnostic
    if src_typ_idx == TYPE_nil then
        srcpos = srcpos_after(srcpos, 0) -- point to just after "return" or after "{" in "{}"
        resolver.diag_err(srcpos, "missing return value (of type %s)", ast_fmt(ir, dst_typ_idx))
    else
        if ast_kind(ir, src_idx) == AST_RETURN.kind then
            srcpos = resolver.srcpos(AST_RETURN.value(ir, src_idx)) -- point to return value
        end
        resolver.diag_err(srcpos,
                          "returning %s from function with result type %s",
                          ast_fmt(ir, src_typ_idx), ast_fmt(ir, dst_typ_idx))
    end
    -- clear type of value, to avoid reporting more errors about its type
    ast_set_meta(ir, ast_offs_of_idx(src_idx), 0)
end
-------------------------------------------------------
-- FunType { kind u8; _ u24; meta u32;
--           param_typ_idx i32; result_typ_idx i32 }
function AST_FUNTYPE.make(ir, resolver, params_idx, result_typ_idx) --> idx
    params_idx = ast_typeof(ir, params_idx)
    local idx = ast_add_u24_i32x2(ir, AST_FUNTYPE.kind, TYPE_type, 0, params_idx, result_typ_idx)
    local offs = ast_offs_of_idx(idx)
    return resolver.intern_type(idx, AST_FUNTYPE.kind, offs, offs + 8, 8)
end
-------------------------------------------------------
-- BinOp { kind u8; op u8; _ u16; meta u32; -- op is token for AST, opcode for IR
--         left_idx i32; right_idx i32 }
function AST_BINOP.alloc(ast) --> offs
    return ast:alloc(16)
end
function AST_BINOP.cons(ast, offs, meta, op, left_idx, right_idx) --> idx
    ast_set_node(ast, offs, AST_BINOP.kind, meta, op)
    ast:set_i64(offs + 8, (left_idx & 0xffffffff) | (right_idx & 0xffffffff)<<32)
    return offs // 8 + 1
end
function AST_BINOP.load(ast, idx) --> op, left_idx, right_idx
    local offs = ast_offs_of_idx(idx)
    return ast:get_u8(offs + 1), ast:get_i32(offs + 8), ast:get_i32(offs + 12)
end
function AST_BINOP.repr(ast, idx, write, repr, as_ir)
    local offs = ast_offs_of_idx(idx)
    local op = ast:get_u8(offs + 1)
    local left_idx, right_idx = ast:get_i32(offs + 8), ast:get_i32(offs + 12)
    write(" " .. tokname(op))
    repr(left_idx)
    return repr(right_idx)
end
function AST_BINOP.resolve(ast, idx, ir, resolver)
    local offs = ast_offs_of_idx(idx)
    local op = ast:get_u8(offs + 1)
    local ir_offs = AST_BINOP.alloc(ir)

    local lval = resolver.resolve(ast:get_i32(offs + 8), RVALUE)
    local ltyp = resolver.typeof(lval)

    local outer_ctxtype = resolver.ctxtype
    resolver.ctxtype = ltyp

    local rval = resolver.resolve(ast:get_i32(offs + 12), RVALUE)
    local rtyp = resolver.typeof(rval)

    resolver.ctxtype = outer_ctxtype

    if ltyp ~= rtyp then
        if ltyp ~= 0 and rtyp ~= 0 then
            resolver.diag_err(ast_srcpos(ast, idx),
                "invalid operation; mismatched types %s and %s",
                ast_fmt(ir, ltyp), ast_fmt(ir, rtyp))
        end
    else -- check if operation is available for type
        if not op_is_defined(op, ltyp, OP_POS_BIN) then
            resolver.diag_err(ast_srcpos(ast, idx), "operator %s not defined for type %s",
                tokname(op), ast_fmt(ir, ltyp))
        end
    end
    return AST_BINOP.cons(ir, ir_offs, ltyp, op, lval, rval)
end
-------------------------------------------------------
-- PrefixOp { kind u8; op u8; _ u16; meta u32;   -- op is token for AST, opcode for IR
--            operand_idx i32; _ i32 }
function AST_PREFIXOP.alloc(ast) --> offs
    return ast:alloc(16)
end
function AST_PREFIXOP.cons(ast, offs, meta, op, operand_idx) --> idx
    ast_set_node(ast, offs, AST_PREFIXOP.kind, meta, op)
    ast:set_i64(offs + 8, operand_idx & 0xffffffff)
    return offs // 8 + 1
end
function AST_PREFIXOP.load(ast, idx) --> op, operand_idx
    local offs = ast_offs_of_idx(idx)
    return ast:get_u8(offs + 1), ast:get_i32(offs + 8)
end
function AST_PREFIXOP.repr(ast, idx, write, repr, as_ir)
    local offs = ast_offs_of_idx(idx)
    local op = ast:get_u8(offs + 1)
    write(" " .. tokname(op))
    local operand_idx = ast:get_i32(offs + 8)
    return repr(operand_idx)
end
function AST_PREFIXOP.resolve(ast, idx, ir, resolver)
    local offs = ast_offs_of_idx(idx)
    local op = ast:get_u8(offs + 1)
    local ir_offs = AST_BINOP.alloc(ir)
    local ir_operand_idx = resolver.resolve(ast:get_i32(offs + 8), RVALUE)
    local typ_idx = resolver.typeof(ir_operand_idx)
    if not op_is_defined(op, typ_idx, OP_POS_PRE) then
        resolver.diag_err(ast_srcpos(ast, idx), "prefix operator %s not defined for type %s",
            tokname(op), ast_fmt(ir, typ_idx))
    end
    return AST_PREFIXOP.cons(ir, ir_offs, typ_idx, op, ir_operand_idx)
end
-------------------------------------------------------
-- PrimType { u8 kind; u8 flags; id_idx u16; typ_idx i32; }
function AST_PRIMTYPE.repr(ast, idx, write)
    write(" " .. id_str(builtin_id_idx(idx)))
end


-- Built-in IR things. They use negative idx in AST & IR, e.g. -7 for "int"
BUILTIN_TYPE_FLAG_INT    = 1<<0 -- integer
BUILTIN_TYPE_FLAG_SIGNED = 1<<1 -- signed
BUILTIN_TYPE_FLAG_BOOL   = 1<<2 -- boolean
builtin_ir = __rt.buf_create(512)
builtin_idtab = __rt.buf_create(32) -- [ [id_idx-builtin_idtab_offs] => idx u8 ]
builtin_idtab_offs = 0xffff
local function builtin_idtab_set(id_idx, idx) --> -idx
    assert(id_idx > 0)
    -- Set builtin_idtab_offs.
    -- Note: we assume id_idx's are allocated during call to builtin_add
    if builtin_idtab_offs == 0xffff then
        builtin_idtab_offs = id_idx
    else
        assert(id_idx > builtin_idtab_offs)
    end
    local offs = id_idx - builtin_idtab_offs
    -- dlog("builtin_idtab[%d (id_idx=%d \"%s\")] => #%d", offs, id_idx, id_str(id_idx), -idx)
    if #builtin_idtab <= offs then builtin_idtab:resize(offs+1, 0) end
    builtin_idtab:set_u8(offs, idx)
    return -idx
end
function builtin_idtab_lookup(id_idx) --> idx
    id_idx = id_idx - builtin_idtab_offs
    if id_idx < 0 or id_idx >= #builtin_idtab then
        return 0
    end
    return -builtin_idtab:get_u8(id_idx)
end
function builtin_id_idx(idx) --> id_idx
    if idx < 0 then idx = -idx end
    return builtin_ir:get_u16(ast_offs_of_idx(idx) + 2)
end
function builtin_name(idx)
    return id_str(builtin_id_idx(idx))
end
function builtin_flags(idx) --> u8
    if idx < 0 then idx = -idx end
    return builtin_ir:get_u8(ast_offs_of_idx(idx) + 1)
end
local function builtin_add(name, kind, a_u8, typ_idx) --> -idx
    local id_idx = id_intern(name)
    assert(id_idx <= 0xffff)
    kind = kind | (a_u8 & 0xff)<<8 | (id_idx & 0xffff)<<16 | ((typ_idx & 0xffffffff) << 32)
    local idx = builtin_ir:push_u64(kind) // 8 + 1
    return builtin_idtab_set(id_idx, idx)
end
local function builtin_primtype_add(name, bitsize, flags) --> idx
    -- bitsize is currently unused
    return builtin_add(name, AST_PRIMTYPE.kind, flags, TYPE_type)
end

-- Built-in primitive types
-- PrimType { u8 kind; u8 flags; id_idx u16; typ_idx i32; }
TYPE_type  = builtin_add("type", AST_PRIMTYPE.kind, 0, 0)
TYPE_nil   = builtin_primtype_add("nil",    0, 0)
TYPE_any   = builtin_primtype_add("any",    0, 0)
TYPE_bool  = builtin_primtype_add("bool",   1, 0)
TYPE_float = builtin_primtype_add("float", 64, 0)
TYPE_int   = builtin_primtype_add("int",   64, BUILTIN_TYPE_FLAG_INT|BUILTIN_TYPE_FLAG_SIGNED)
TYPE_uint  = builtin_primtype_add("uint",  64, BUILTIN_TYPE_FLAG_INT)
-- Built-in primitive types, explicit-size integers
-- Note: If order or stride changes, remember to update inttype_info
TYPE_last_int_type = TYPE_uint
for bitsize = 64, 2, -1 do
    builtin_primtype_add(
        "int"..tostring(bitsize), bitsize, BUILTIN_TYPE_FLAG_INT|BUILTIN_TYPE_FLAG_SIGNED)
    TYPE_last_int_type = builtin_primtype_add(
        "uint"..tostring(bitsize), bitsize, BUILTIN_TYPE_FLAG_INT)
end

-- Built-in constants
CONST_nil   = builtin_add("nil", AST_NIL.kind, 0, TYPE_nil) -- note: shadows 'nil' type
CONST_true  = builtin_add("true", AST_BOOL.kind, 1, TYPE_bool)
CONST_false = builtin_add("false", AST_BOOL.kind, 0, TYPE_bool)


-- Built-in operators
OP_POS_BIN  = 0 -- requires two operands
OP_POS_PRE  = 1 -- before one operand
OP_POS_POST = 2 -- after one operand
OP_FLAG_TYPE_INT   = 1<<0 -- available for all integer types
OP_FLAG_TYPE_FLOAT = 1<<1 -- available for all floating-point types
OP_FLAG_TYPE_BOOL  = 1<<2 -- available for booleans
local opmap = __rt.buf_create(136)
-- opmap maps token to operation flags: [ tok = [ binflags u8, preflags u8, postflags u8 ] ]
_ = function(tok, op_pos, flags) --> nil
    if #opmap < tok*3 then opmap:resize(tok*3, 0) end
    opmap:set_u8((tok - 1)*3 + op_pos, flags)
end
local Int, Float, Bool = OP_FLAG_TYPE_INT, OP_FLAG_TYPE_FLOAT, OP_FLAG_TYPE_BOOL
local Bin, Pre, Post = OP_POS_BIN, OP_POS_PRE, OP_POS_POST
-- comparison
_(TOK_EQEQ,  Bin, Int|Float) -- (EQ)  L == R
_(TOK_NOTEQ, Bin, Int|Float) -- (NEQ) L != R
_(TOK_LT,    Bin, Int|Float) -- (LT)  L <  R
_(TOK_LTEQ,  Bin, Int|Float) -- (LTE) L <= R
_(TOK_GT,    Bin, Int|Float) -- (GT)  L >  R
_(TOK_GTEQ,  Bin, Int|Float) -- (GTE) L >= R
-- arithmetic
_(TOK_PLUS,     Bin, Int|Float) -- (ADD) L +  R
_(TOK_MINUS,    Bin, Int|Float) -- (SUB) L -  R
_(TOK_STAR,     Bin, Int|Float) -- (MUL) L *  R
_(TOK_STARSTAR, Bin, Int|Float) -- (EXP) L ** R -- exponentiation
_(TOK_SLASH,    Bin, Int|Float) -- (DIV) L /  R
_(TOK_PERCENT,  Bin, Int|Float) -- (MOD) L %  R -- remainder of division
_(TOK_MINUS,    Pre, Int|Float) -- (NEG)   -  R
-- bitwise
_(TOK_AND,   Bin, Int) -- (AND) L &  R
_(TOK_OR,    Bin, Int) -- (OR)  L |  R
_(TOK_HAT,   Bin, Int) -- (XOR) L ^  R -- xor
_(TOK_LTLT,  Bin, Int) -- (LSH) L << R
_(TOK_GTGT,  Bin, Int) -- (RSH) L >> R
_(TOK_TILDE, Pre, Int) -- (INV)    ~ R -- invert bits (aka binary NOT)
-- logical
_(TOK_ANDAND, Bin, Bool) -- (LAND) L && R
_(TOK_OROR,   Bin, Bool) -- (LOR)  L || R
_(TOK_NOT,    Pre, Bool) -- (NOT)     ! R
-- dlog("opmap: %d B", #opmap)
----
function op_is_defined(tok, typ_idx, op_pos) --> bool
    if typ_idx >= 0 or #opmap < tok*3 then
        -- (typ_idx >= 0): only built-in types have operations defined (for now)
        return false
    end
    local opflags = opmap:get_u8((tok - 1)*3 + op_pos)
    if typ_idx == TYPE_bool then
        return opflags & OP_FLAG_TYPE_BOOL ~= 0
    elseif typ_idx == TYPE_float then
        return opflags & OP_FLAG_TYPE_FLOAT ~= 0
    elseif builtin_flags(typ_idx) & BUILTIN_TYPE_FLAG_INT ~= 0 then
        return opflags & OP_FLAG_TYPE_INT ~= 0
    end
    return false
end


-- Operator precedence
-- Binary operators of the same precedence associate from left to right.
-- E.g. x / y * z is the same as (x / y) * z.
PREC_MAX    = 8  --│ . (member)
PREC_UNPRE  = 7  --│ +  -  !  ~  *  &  ?
PREC_UNPOST = 6  --│ ++  --  ()  []
PREC_BIN5   = 5  --│ *  /  %  <<  >>  &
PREC_BIN4   = 4  --│ +  -  |  ^
PREC_BIN3   = 3  --│ ==  !=  <  <=  >  >=
PREC_BIN2   = 2  --│ &&
PREC_BIN1   = 1  --│ ||
PREC_MIN    = 0  --│ ,

-- flags used with resolve and codegen functions
AST_FLAG_RVALUE = 1<<0 -- as rvalue
RVALUE = AST_FLAG_RVALUE

end

--------------------------------------------------------------------------------------------------

function ast_srcpos(ast, idx) --> srcpos
    return ast:get_u32(ast_offs_of_idx(idx) + 4)
end

function ast_kind(ast, idx) -- u32
    if idx == 0 then
        return 0
    end
    if idx < 0 then
        ast = builtin_ir
        idx = -idx
    end
    return ast:get_u8(ast_offs_of_idx(idx))
end

function ast_kindname(ast, idx) -- string
    if idx == 0 then
        return "?"
    end
    return ast_info[ast_kind(ast, idx)].name
end

function ast_istype(ast, idx) -- bool
    if idx == 0 then
        return false
    end
    return ast_info[ast_kind(ast, idx)].is_type
end

function ast_is_floattype(idx) --> bool
    return idx == TYPE_float
end

function ast_is_inttype(idx) --> bool
    return idx < 0 and (idx <= TYPE_int and idx >= TYPE_last_int_type)
end

function inttype_info(idx) --> nbits, is_signed
    assert(ast_is_inttype(idx))
    local nbits = idx >= TYPE_uint and 64 or (64 - ((TYPE_uint-1 - idx) // 2))
    local is_signed = (idx - TYPE_int) % 2 == 0
    return nbits, is_signed
end

function ast_deref(ast, idx)
    while ast_kind(ast, idx) == AST_REF.kind do
        idx = AST_REF.target(ast, idx)
    end
    return idx
end

function ast_typeof(ast, idx)
    if idx < 0 then
        ast = builtin_ir
        idx = -idx
    elseif idx == 0 then
        return 0
    elseif ast_kind(ast, idx) == AST_REF.kind then
        return ast_typeof(ast, AST_REF.target(ast, idx))
    end
    return ast_deref(ast, ast:get_i32(ast_offs_of_idx(idx) + 4))
end

--------------------------------------------------------------------------------------------------

function ast_fmt(ast, idx)
    -- TODO: expand this function into a full-featured code formatter
    local buf = __rt.buf_create(32)
    local function write(s)
        -- if #buf > 0 and buf:get_u8(#buf - 1) ~= " " then
        --     s = " " .. s
        -- end
        return buf:append(s)
    end
    local function visit(idx)
        if idx == 0 or idx == nil then
            return write("<?>")
        end
        if idx < 0 then
            return write(builtin_name(idx))
        end
        local info = ast_info[ast:get_u8(ast_offs_of_idx(idx))]
        if info.fmt == nil then
            dlog("TODO: " .. info.name .. ".fmt()")
            return write(ast_kindname(ast, idx))
        end
        return info.fmt(ast, idx, write, visit)
    end
    visit(idx)
    return buf:str()
end

function ast_descr(ast, idx)
    if idx < 0 then
        return builtin_name(idx)
    end
    local info = ast_info[ast:get_u8(ast_offs_of_idx(idx))]
    if info.descr == nil then
        dlog("TODO: " .. info.name .. ".descr")
        return ast_kindname(ast, idx)
    end
    return info.descr
end

function ast_repr(unit, idx, as_ir)
    local ast = as_ir and unit.ir or unit.ast
    local depth = 0
    local builtins_use_specialized_repr = true
    local seen = {}
    local buf = __rt.buf_create(64)
    local function write(s) buf:append(s) end
    local function writef(s, ...) buf:append(fmt(s, ...)) end

    local function visit(idx)
        local ast1 = ast
        local idx_orig = idx
        if idx == 0 then
            return write(" \x1b[31;1m#0\x1b[0m")
        elseif idx < 0 then -- built-in thing
            ast1 = builtin_ir
            idx = -idx
        end

        -- special representation of built-ins
        if builtins_use_specialized_repr and idx_orig < 0 then
            local prefix = ""
            if #buf > 0 and buf:get_u8(#buf - 1) ~= 0x28 then -- 0x28 = "("
                prefix = " "
            end
            return writef("%s\x1b[1;32m%s\x1b[0m", prefix, id_str(builtin_id_idx(idx)))
        end

        -- indentation
        if depth > 0 then write("\n" .. string.rep("    ", depth)) end

        -- get info
        -- dlog("VISIT #%d offs=%u", idx_orig, ast_offs_of_idx(idx))
        local offs = ast_offs_of_idx(idx)
        local kind = ast1:get_u8(offs)
        local info = ast_info[kind]

        -- dlog("%srepr> #%d [%u] %s",
        --      string.rep("    ", depth), idx_orig, offs,
        --      info == nil and fmt("<kind?%d>", kind) or info.name)

        -- node kind name
        local style = info.is_type and "1;36" or "1"
        writef("(\x1b[%sm%s\x1b[0m", style, info.name)
        writef("\x1b[2m#%d\x1b[0m", idx_orig) -- idx

        -- check if we have seen this already
        if seen[idx_orig] ~= nil then
            if seen[idx_orig] == 1 then
                return write(" <recursive#"..idx_orig..">)")
            else
                return write(")")
            end
        end
        seen[idx_orig] = 1

        depth = depth + 1

        -- visit children
        if info.repr ~= nil then
            info.repr(ast1, idx, write, visit, as_ir)
        elseif info.visit ~= nil then
            info.visit(ast1, idx, visit)
        end

        -- visit type
        if as_ir and not ast_istype(ast1, idx) then
            visit(ast_typeof(ast, idx))
        end

        -- comments
        if unit.commentmap ~= nil then
            local comments = unit.commentmap[idx_orig]
            if comments ~= nil then
                write("\n" .. string.rep("    ", depth) .. "\x1b[2;3m(COMMENT")
                for i, c in ipairs(comments) do
                    write(" \"")
                    write(c.value)
                    write("\"")
                end
                write(")\x1b[0m")
            end
        end

        depth = depth - 1
        seen[idx_orig] = 2 -- done

        write(")")
    end

    visit(idx)
    return buf:str()
end

function ir_repr(unit, idx)
    local as_ir = true
    return ast_repr(unit, idx, as_ir)
end

--------------------------------------------------------------------------------------------------
-- ast encode functions

function ast_set_node(ast, offs, kind, meta, a_u24)
    return ast:set_i64(offs, kind | (a_u24 & 0xffffff)<<8 | ((meta & 0xffffffff) << 32))
end
function ast_set_meta(ast, offs, meta)
    return ast:set_i32(offs + 4, meta)
end
function ast_add_u24(ast, kind, meta, a_u24)
    kind = kind | (a_u24 & 0xffffff)<<8 | ((meta & 0xffffffff) << 32)
    return ast:push_u64(kind) // 8 + 1
end
function ast_set_u24(ast, offs, kind, meta, a_u24)
    ast:set_i64(offs, kind | (a_u24 & 0xffffff)<<8 | ((meta & 0xffffffff) << 32))
    return offs + 16
end
function ast_add_u24_u64(ast, kind, meta, a_u24, b_u64)
    kind = kind | (a_u24 & 0xffffff)<<8 | ((meta & 0xffffffff) << 32)
    return ast:push_u64x2(kind, b_u64) // 8 + 1
end
function ast_add_u24_f64(ast, kind, meta, a_u24, b_f64)
    kind = kind | (a_u24 & 0xffffff)<<8 | ((meta & 0xffffffff) << 32)
    local offs = ast:push_u64(kind)
    ast:push_f64(b_f64)
    return offs // 8 + 1
end
function ast_set_u24_i32x2(ast, offs, kind, meta, a_u24, b_i32, c_i32)
    ast:set_i64(offs,     kind | (a_u24 & 0xffffff)<<8 | ((meta & 0xffffffff) << 32))
    ast:set_i64(offs + 8, (b_i32 & 0xffffffff) | (c_i32 & 0xffffffff)<<32)
    return offs + 16
end
function ast_add_u24_i32x2(ast, kind, meta, a_u24, b_i32, c_i32)
    local a = kind | (a_u24 & 0xffffff)<<8 | ((meta & 0xffffffff) << 32)
    local b = (b_i32 & 0xffffffff) | (c_i32 & 0xffffffff)<<32
    return ast:push_u64x2(a, b) // 8 + 1
end
function ast_add_u24_i32(ast, kind, meta, a_u24, b_i32)
    return ast_add_u24_i32x2(ast, kind, meta, a_u24, b_i32, 0)
end

-- List { kind u8; count u24; meta u32; idxv? i32[.count] }
function ast_list_add(ast, kind, meta, idxv_i32) --> idx
    local count = idxv_i32 == nil and 0 or #idxv_i32 // 4
    local idx = ast_add_u24(ast, kind, meta, count)
    if count > 0 then
        ast:append(idxv_i32)
        if count % 2 == 1 then -- uphold 8-byte alignment of ast
            ast:push_u32(0)
        end
    end
    return idx
end
function ast_list_add_upfront(ast, kind, meta, count) --> offs
    local offs = ast:alloc(8 + ((count + (count % 2)) * 4))
    ast:set_i64(offs, kind | (count & 0xffffff)<<8 | ((meta & 0xffffffff) << 32))
    return offs
end
function ast_list_count(ast, idx) --> u32
    local offs = ast_offs_of_idx(idx)
    return ast:get_u32(offs) >> 8
end
function ast_list_elem(ast, idx, elem_no) --> idx
    local offs = ast_offs_of_idx(idx)
    return ast:get_i32(offs + 8 + elem_no*4)
end
function ast_list_fmt(ast, idx, write, fmt)
    local start_offs = ast_offs_of_idx(idx)
    local count = ast:get_u32(start_offs) >> 8
    start_offs = start_offs + 8
    write("(")
    for offs = start_offs, start_offs + (count-1)*4, 4 do
        if offs > start_offs then
            write(",")
        end
        fmt(ast:get_i32(offs))
    end
    write(")")
end

-- VarList = { kind u8; count u24; meta u32;
--             vars Var[.count] }
function varlist_alloc(ast, kind, meta, count) --> offs
    local size = 8 + AST_VAR.size*count ; assert(AST_VAR.size % 8 == 0)
    local offs = ast:alloc(size)
    ast:set_i64(offs, kind | (count & 0xffffff)<<8 | ((meta & 0xffffffff) << 32))
    return offs
end


-- -- extend Buf prototype
-- local Buf = __rt.buf_create(0).__index

-- function Buf.add_u8_u24_u24(buf, kind, a_u8, b_u24, c_u24)
--     -- { kind u8; a u8; b u24; c u24; }
--     kind = kind | (a_u8 << 8) | ((b_u24 & 0xffffff) << 16) | ((c_u24 & 0xffffff) << 40)
--     return buf:push_u64(kind) // 8 + 1
-- end
-- function Buf.add_u32(buf, kind, a_u32) -- { kind u8; _ u8; _ u16; a u32; }
--     return buf:push_u64(kind | (a_u32 & 0xffffffff)<<32) // 8 + 1
-- end
-- function Buf.add_u24_u32(buf, kind, a_u24, b_u32) -- { kind u8; a u24; b u32; }
--     return buf:push_u64(kind | (a_u24 & 0xffffff)<<8 | ((b_u32 & 0xffffffff) << 32)) // 8 + 1
-- end
-- function Buf.add_u24_u32_u64(buf, kind, a_u24, b_u32, c_u64)
--     -- { kind u8; a u24; b u32; c u64; }
--     kind = kind | (a_u24 & 0xffffff)<<8 | ((b_u32 & 0xffffffff) << 32)
--     return buf:push_u64x2(kind, c_u64) // 8 + 1
-- end
