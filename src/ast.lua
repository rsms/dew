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

AST_NIL       = _('NIL',       'nil',                   nil) -- nil
AST_BOOL      = _('BOOL',      'boolean',               nil) -- true
AST_INT       = _('INT',       'integer',               nil) -- 123
AST_FLOAT     = _('FLOAT',     'floating-point number', nil) -- 1.23
AST_BINOP     = _('BINOP',     'infix operation',       visit_i32x2) -- 2 + 3
AST_PREFIXOP  = _('PREFIXOP',  'prexif operation',      visit_i32) -- -n
AST_ID        = _('ID',        'identifier',            nil) -- x
AST_PARAM     = _('PARAM',     'parameter',             visit_i32x2) -- x int
AST_VAR       = _('VAR',       'variable',              visit_i32) -- id = expr
AST_MULTIVAR  = _('MULTIVAR',  'variables',             visit_varlist) -- x, y, z = 1, 2, 3
AST_SPREADVAR = _('SPREADVAR', 'variables',             visit_varlist) -- x, y, z = expr
AST_ASSIGN    = _('ASSIGN',    'assignment',            visit_i32x2) -- x.y = z
AST_REF       = _('REF',       'REF',                   visit_i32imm) -- <reference>
AST_TUPLE     = _('TUPLE',     'tuple',                 visit_list) -- (x, 3)
AST_BLOCK     = _('BLOCK',     'block',                 visit_list) -- { ... }
AST_FUN       = _('FUN',       'function',              visit_i32x3) -- fun f(x, y T) T
AST_RETURN    = _('RETURN',    'return statement',      visit_i32) -- return 3

AST_PRIMTYPE  = _('PRIMTYPE',     'PRIMTYPE',      nil) -- int
AST_TUPLETYPE = _('TUPLETYPE',    'tuple type',    visit_list) -- (T, T)
AST_FUNTYPE   = _('FUNTYPE',      'function type', visit_list_2d) -- (T, T) (T, T)

-- TODO:
AST_CALL      = _('CALL', 'function call', visit_TODO) -- x(y, z)
AST_REST      = _('REST', 'rest') -- ...

AST_TUPLEREFTYPE = _('TUPLEREFTYPE', 'TUPLEREFTYPE',  nil)
AST_ARRAYTYPE    = _('ARRAYTYPE',    'array type',    visit_TODO) -- [T]
AST_RESTTYPE     = _('RESTTYPE',     'rest type',     visit_TODO) -- ...T

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
-- Int24 { kind u8; value u24; meta u32 } -- when value < 0xffffff
-- Int64 { kind u8; MAX u24;   meta u32;
--         value u64 }
function AST_INT.make(ast, meta, value)
    if value < 0xffffff then
        return ast_add_u24(ast, AST_INT.kind, meta, value)
    end
    return ast_add_u24_u64(ast, AST_INT.kind, meta, 0xffffff, value)
end
function AST_INT.value(ast, idx)
    idx = ast_offs_of_idx(idx)
    local v = ast:get_u32(idx) >> 8
    return v < 0xffffff and v or ast:get_i64(idx + 8)
end
function AST_INT.repr(ast, idx, write, repr)
    return write(fmt(" %u", AST_INT.value(ast, idx)))
end
function AST_INT.resolve(ast, idx, ir, resolver)
    local typ_idx = resolver.ctxtype ~= nil and resolver.ctxtype or TYPE_int
    return AST_INT.make(ir, typ_idx, AST_INT.value(ast, idx))
end
-------------------------------------------------------
-- Float { kind u8; _ u24; meta u32;
--         value f64 }
function AST_FLOAT.make(ast, meta, value)
    return ast_add_u24_f64(ast, AST_FLOAT.kind, meta, 0, value)
end
function AST_FLOAT.repr(ast, idx, write, repr)
    return write(fmt(" %g", ast:get_f64(ast_offs_of_idx(idx) + 8)))
end
function AST_FLOAT.resolve(ast, idx, ir, resolver)
    local typ_idx = resolver.ctxtype ~= nil and resolver.ctxtype or TYPE_float
    return AST_FLOAT.make(ir, typ_idx, ast:get_f64(ast_offs_of_idx(idx) + 8))
end
-------------------------------------------------------
-- Ref { kind u8; _ u24; target_idx u32; }
function AST_REF.make(ast, target_idx)
    return ast:push_u64(AST_REF.kind | ((target_idx & 0xffffffff) << 32)) // 8 + 1
end
function AST_REF.target(ast, idx) --> target_idx
    return ast:get_i32(ast_offs_of_idx(idx) + 4)
end
-------------------------------------------------------
-- Id { kind u8; id_idx u24; meta u32; }
function AST_ID.make(ast, meta, id_idx)
    return ast_add_u24(ast, AST_ID.kind, meta, id_idx)
end
function AST_ID.repr(ast, idx, write, repr)
    local id_idx = ast:get_i32(ast_offs_of_idx(idx)) >> 8
    return write(" " .. id_str(id_idx))
end
function AST_ID.resolve(ast, idx, ir, resolver, flags)
    local id_idx = ast:get_i32(ast_offs_of_idx(idx)) >> 8
    local target_idx = resolver.id_lookup(id_idx)
    if target_idx == 0 then
        resolver.diag_err(ast_srcpos(ast, idx), "undefined '%s'", id_str(id_idx))
        return 0
    end
    -- use a REF node if this is the top-level node
    if #ir == 0 then
        return AST_REF.make(ir, target_idx)
    end
    return target_idx
    -- -- return the target when resolving an rvalue
    -- if (flags & RVALUE) ~= 0 then
    --     return target_idx
    -- end
    -- return AST_REF.make(ir, target_idx)
end
-------------------------------------------------------
-- Var { kind u8; id_idx u24; meta u32;
--       value_idx i32; _ i32; }
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
function AST_VAR.repr(ast, idx, write, repr)
    local offs = ast_offs_of_idx(idx)
    local id_idx = ast:get_u32(offs) >> 8
    write(" " .. id_str(id_idx))
    local value_idx = ast:get_i32(offs + 8)
    return repr(value_idx)
end
function AST_VAR.resolve_cons(ast, ir, ast_offs, ir_offs, resolver, flags) --> idx
    local id_idx = ast:get_u32(ast_offs) >> 8
    local value_idx = ast:get_i32(ast_offs + 8)
    local ir_value_idx = resolver.resolve(value_idx, flags | RVALUE)
    local typ_idx = resolver.typeof(ir_value_idx)
    local def_idx = resolver.id_lookup_def(id_idx)
    if def_idx == 0 then
        -- variable definition
        AST_VAR.cons(ir, ir_offs, typ_idx, id_idx, ir_value_idx)
        return resolver.id_define(id_idx, ast_idx_of_offs(ir_offs))
    end
    -- assignment
    local def_typ_idx = resolver.typeof(def_idx)
    if typ_idx ~= def_typ_idx then
        resolver.diag_err(ast_srcpos(ast, value_idx),
            "cannot assign value of type %s to %s of type %s",
            ast_fmt(ir, typ_idx), ast_descr(ir, def_idx), ast_fmt(ir, def_typ_idx))
    end
    AST_ASSIGN.cons(ir, ir_offs, typ_idx, def_idx, ir_value_idx)
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
-- Param { kind u8; id_idx u24; meta u32;
--         typ_idx i32; value_idx i32 }
function AST_PARAM.make(ast, meta, id_idx, typ_idx, value_idx)
    return ast_add_u24_i32x2(ast, AST_PARAM.kind, meta, id_idx, typ_idx, value_idx)
end
function AST_PARAM.repr(ast, idx, write, repr)
    local offs = ast_offs_of_idx(idx)
    write(" " .. id_str(ast:get_u32(offs) >> 8))
    repr(ast:get_i32(offs + 8))
    local value_idx = ast:get_i32(offs + 12)
    if value_idx ~= 0 then
        return repr(value_idx)
    end
end
function AST_PARAM.resolve(ast, idx, ir, resolver, flags)
    local offs = ast_offs_of_idx(idx)
    local id_idx = ast:get_u32(offs) >> 8
    local typ_idx = ast:get_i32(offs + 8)
    local value_idx = ast:get_i32(offs + 12)
    typ_idx = resolver.resolve(typ_idx, flags | RVALUE)
    if value_idx ~= 0 then
        value_idx = resolver.resolve(value_idx, flags | RVALUE)
        dlog("TODO: PARAM.resovle: check type match of value_idx vs typ_idx")
    end
    local collision = resolver.id_lookup_def_in_current_scope(id_idx)
    if collision ~= 0 then
        resolver.diag_err(ast_srcpos(ast, idx), "duplicate parameter '%s'", id_str(id_idx))
    end
    idx = AST_PARAM.make(ir, typ_idx, id_idx, typ_idx, value_idx)
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
    local param_scope = resolver.id_scope_open()
    idx = ast_list_resolve(ast, idx, ir, resolver, flags)
    resolver.id_scope_close(param_scope)
    return idx
end
-------------------------------------------------------
-- Tuple = List of Expr
function AST_TUPLE.make(ast, meta, idxv_i32)
    return ast_list_add(ast, AST_TUPLE.kind, meta, idxv_i32)
end
function AST_TUPLE.resolve(ast, idx, ir, resolver, flags)
    return ast_list_resolve(ast, idx, ir, resolver, flags)
end
-------------------------------------------------------
-- TupleType = List of Type
function AST_TUPLETYPE.make(ast, meta, idxv_i32)
    return ast_list_add(ast, AST_TUPLETYPE.kind, meta, idxv_i32)
end
function AST_TUPLETYPE.resolve(ast, idx, ir, resolver, flags)
    local offs = ast_offs_of_idx(idx)
    local count = ast:get_u32(offs) >> 8
    local ir_list_offs = ast_list_add_upfront(ir, AST_TUPLETYPE.kind, TYPE_type, count)
    local ir_child_offs = ir_list_offs + 8
    flags = flags | RVALUE -- make sure IDs resolve to targets rather than REFs
    for offs = offs + 8, offs + 4 + (count * 4), 4 do
        local child_idx = resolver.resolve(ast:get_i32(offs), flags)
        ir:set_i32(ir_child_offs, child_idx)
        ir_child_offs = ir_child_offs + 4
    end
    -- intern type (should match ast_list_resolve)
    ir_child_offs = ir_list_offs + 8
    local typ_hash = ir:hash(AST_TUPLETYPE.kind, ir_child_offs, ir_child_offs + (count * 4))
    local typ_idx = resolver.internmap[typ_hash]
    if typ_idx == nil then
        typ_idx = ast_idx_of_offs(ir_list_offs)
        resolver.internmap[typ_hash] = typ_idx
        return typ_idx
    end
    ir:resize(ir_list_offs) -- undo additions to IR stack
    return typ_idx
end
function AST_TUPLETYPE.fmt(ast, idx, write, fmt)
    return ast_list_fmt(ast, idx, write, fmt)
end
-------------------------------------------------------
-- Return { kind u8; _ u24; meta u32; value_idx i32; _ u32; }
function AST_RETURN.make(ast, meta, value_idx)
    return ast_add_u24_i32(ast, AST_RETURN.kind, meta, 0, value_idx)
end
function AST_RETURN.resolve(ast, idx, ir, resolver, flags)
    local offs = ast_offs_of_idx(idx)
    local value_idx = ast:get_i32(offs + 8)
    value_idx = resolver.resolve(value_idx)
    local typ_idx = resolver.typeof(value_idx)
    local ir_idx = AST_RETURN.make(ir, typ_idx, value_idx)

    resolver.record_srcpos(idx, ir_idx)
    if resolver.fun_idx == 0 then
        resolver.diag_err(ast_srcpos(ast, idx),
            "return statement outside function body")
        return ir_idx
    end
    AST_FUN.check_result_type(ir, resolver.fun_idx, ir_idx, resolver)
    return ir_idx
end
-------------------------------------------------------
-- FunType { kind u8; intype_count u24; outtype_count u32;
--           intype_idx [i32 intype_count]; outtype_idx [i32 outtype_count]; }
function AST_FUNTYPE.make(ir, resolver, params_idx, result_idx) --> idx
    local function count_operands(operands_idx)
        if operands_idx == 0 then
            return 0
        end
        local kind = ast_kind(ir, operands_idx)
        if kind == AST_TUPLE.kind or kind == AST_TUPLETYPE.kind then
            return ast_list_count(ir, operands_idx)
        end
        return 1
    end
    local function process_operands(dst_offs, operands_idx) --> dst_offs
        if operands_idx == 0 then
            return
        end
        local kind = ast_kind(ir, operands_idx)
        if not ast_info[kind].is_type then
            operands_idx = resolver.typeof(operands_idx)
        end
        if kind == AST_TUPLE.kind or kind == AST_TUPLETYPE.kind then
            local src_offs = ast_offs_of_idx(operands_idx)
            local count = ir:get_u32(src_offs) >> 8
            for src_offs = src_offs + 8, src_offs + 4 + (count * 4), 4 do
                ir:set_i32(dst_offs, ir:get_i32(src_offs))
                dst_offs = dst_offs + 4
            end
            return dst_offs
        end
        ir:set_i32(dst_offs, operands_idx)
        return dst_offs + 4
    end

    -- allocate FUNTYPE up front
    local intype_count = count_operands(params_idx)
    local outtype_count = count_operands(result_idx)
    local size = 8 + intype_count*4 + outtype_count*4
    local offs = ir:alloc(size + (8 - (size % 8)))
    ast_set_u24(ir, offs, AST_FUNTYPE.kind, outtype_count, intype_count)

    -- create intype list elements
    local dst_offs = process_operands(offs + 8, params_idx)
    process_operands(dst_offs, result_idx)

    -- intern type
    local typ_hash = ir:hash(AST_FUNTYPE.kind, offs, offs+size)
    local idx = resolver.internmap[typ_hash]
    if idx == nil then
        idx = ast_idx_of_offs(offs)
        resolver.internmap[typ_hash] = idx
    else
        ir:resize(offs) -- undo additions to IR stack
    end
    return idx
end
function AST_FUNTYPE.repr(ast, idx, write, repr)
    -- List2D { kind u8; count1 u24; count2 u32; idx1v i32[.count1]; idx2v i32[.count2] }
    local offs = ast_offs_of_idx(idx)
    local count1 = ast:get_u32(offs) >> 8
    local count2 = ast:get_u32(offs + 4)
    offs = offs + 8
    write(" (")
    for i = 0, count1 - 1 do
        repr(ast:get_i32(offs + i*4))
    end
    write(") (")
    offs = offs + count1*4
    for i = 0, count2 - 1 do
        repr(ast:get_i32(offs + i*4))
    end
    write(")")
end
-------------------------------------------------------
-- Fun { kind u8; id_idx u24; meta u32;
--       params_idx i32; result_idx i32;
--       body_idx i32; _ u32; }
function AST_FUN.make(ast, meta, id_idx, params_idx, result_idx, body_idx)
    local header = AST_FUN.kind | (id_idx & 0xffffff)<<8 | ((meta & 0xffffffff) << 32)
    local offs = ast:push_u64(header)
    local a = (params_idx & 0xffffffff) | (result_idx & 0xffffffff)<<32
    local b = body_idx & 0xffffffff
    ast:push_u64x2(a, b)
    return ast_idx_of_offs(offs)
end
function AST_FUN.alloc(ast, id_idx) --> offs
    local offs = ast:alloc(24)
    ast:set_u32(offs, AST_FUN.kind | (id_idx & 0xffffff)<<8)
    return offs
end
function AST_FUN.load(ast, idx) --> id_idx, params_idx, result_idx, body_idx
    local offs = ast_offs_of_idx(idx)
    local id_idx = ast:get_u32(offs) >> 8
    local params_idx = ast:get_i32(offs + 8)
    local result_idx = ast:get_i32(offs + 12)
    local body_idx = ast:get_i32(offs + 16)
    return id_idx, params_idx, result_idx, body_idx
end
function AST_FUN.result(ast, idx) --> result_idx
    local offs = ast_offs_of_idx(idx)
    return ast:get_i32(offs + 12)
end
function AST_FUN.repr(ast, idx, write, repr)
    local id_idx, params_idx, result_idx, body_idx = AST_FUN.load(ast, idx)
    if id_idx ~= 0 then
        write(" " .. id_str(id_idx))
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
function AST_FUN.resolve(ast, idx, ir, resolver, flags)
    local id_idx, params_idx, result_idx, body_idx = AST_FUN.load(ast, idx)

    -- make & define function up front, since its params and body may reference it
    local offs = AST_FUN.alloc(ir, id_idx)
    resolver.id_define(id_idx, ast_idx_of_offs(offs))

    local child_flags = flags & ~RVALUE
    local param_scope = resolver.id_scope_open()
    params_idx = params_idx == 0 and 0 or resolver.resolve(params_idx, child_flags)
    result_idx = result_idx == 0 and 0 or resolver.resolve(result_idx, child_flags)

    local typ_idx = AST_FUNTYPE.make(ir, resolver, params_idx, result_idx)
    ir:set_i32(offs + 4, typ_idx)
    ir:set_i32(offs + 8, params_idx)
    ir:set_i32(offs + 12, result_idx)
    ir:set_i32(offs + 16, 0) -- body_idx

    local ir_idx = ast_idx_of_offs(offs)

    if body_idx ~= 0 then
        resolver.record_srcpos(idx, ir_idx)

        local body_scope = resolver.id_scope_open()
        local outer_fun_idx = resolver.fun_idx
        resolver.fun_idx = ir_idx

        local body_ir_idx = resolver.resolve(body_idx, child_flags | RVALUE)

        resolver.fun_idx = outer_fun_idx
        resolver.id_scope_close(body_scope)

        local res_typ_idx = AST_FUN.check_result_type(ir, ir_idx, body_ir_idx, resolver)
        -- convert implicit return to actual return
        local body_kind = ast_kind(ir, body_ir_idx)
        if res_typ_idx ~= 0 and body_kind ~= AST_RETURN.kind then
            if body_kind ~= AST_BLOCK.kind then
                -- single expression
                print("LOLCAT!!")
                -- body_ir_idx = AST_RETURN.make(ir, res_typ_idx, body_ir_idx)
            end
        end

        ir:set_i32(offs + 16, body_ir_idx)
    end

    resolver.id_scope_close(param_scope)

    return ir_idx
end
function AST_FUN.check_result_type(ir, fun_idx, val_idx, resolver) --> res_typ_idx_if_match
    local val_typ_idx = resolver.typeof(val_idx)
    local res_idx = AST_FUN.result(ir, fun_idx)
    local res_typ_idx = ast_istype(ir, res_idx) and res_idx or resolver.typeof(res_idx)
    if val_typ_idx == res_typ_idx then
        return res_typ_idx
    end
    if val_typ_idx ~= 0 then
        resolver.diag_err(resolver.srcpos(val_idx),
            "cannot use value of type %s as result type %s",
            ast_fmt(ir, val_typ_idx), ast_fmt(ir, res_typ_idx))
        -- clear type of value, to avoid reporting more errors about its type
        ast_set_meta(ir, ast_offs_of_idx(val_idx), 0)
    end
    return 0
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
    local rval = resolver.resolve(ast:get_i32(offs + 12), RVALUE)
    local ltyp = ir_typeof(ir, lval)
    local rtyp = ir_typeof(ir, rval)
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
-- PrefixOp { kind u8; op u8; _ u16; meta u32; -- op is token for AST, opcode for IR
--            operand_idx i32; _ i32 }
function AST_PREFIXOP.alloc(ast) --> offs
    return ast:alloc(16)
end
function AST_PREFIXOP.cons(ast, offs, meta, op, operand_idx) --> idx
    ast_set_node(ast, offs, AST_PREFIXOP.kind, meta, op)
    ast:set_i64(offs + 8, operand_idx & 0xffffffff)
    return offs // 8 + 1
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
    local typ_idx = ir_typeof(ir, ir_operand_idx)
    if not op_is_defined(op, typ_idx, OP_POS_PRE) then
        resolver.diag_err(ast_srcpos(ast, idx), "prefix operator %s not defined for type %s",
            tokname(op), ast_fmt(ir, typ_idx))
    end
    return AST_PREFIXOP.cons(ir, ir_offs, typ_idx, op, ir_operand_idx)
end
-------------------------------------------------------
-- function AST_MULTIVAR.repr(ast, idx, write, repr)
--     -- id { kind u8; id_idx u24; target_idx u32; }+
--     -- mvar { kind u8; childcount u24; children [u32]; }
--     local offs = ast_offs_of_idx(idx)
--     local count = ast:get_u32(offs) >> 8
--     for offs = offs - count*8, offs - 8, 8 do
--         write(" " .. id_str(ast:get_u32(offs) >> 8))
--     end
--     for offs = offs + 4, offs + (count * 4), 4 do
--         repr(ast:get_u32(offs))
--     end
-- end
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
for bitsize = 2, 64 do
    builtin_primtype_add("int"..tostring(bitsize), bitsize,
                         BUILTIN_TYPE_FLAG_INT|BUILTIN_TYPE_FLAG_SIGNED)
    builtin_primtype_add("uint"..tostring(bitsize), bitsize, BUILTIN_TYPE_FLAG_INT)
end

-- Built-in constants
CONST_nil   = builtin_add("nil", AST_NIL.kind, 0, TYPE_nil)
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
_(TOK_PLUS,    Bin, Int|Float) -- (ADD) L + R
_(TOK_MINUS,   Bin, Int|Float) -- (SUB) L - R
_(TOK_STAR,    Bin, Int|Float) -- (MUL) L * R
_(TOK_SLASH,   Bin, Int|Float) -- (DIV) L / R
_(TOK_PERCENT, Bin, Int|Float) -- (MOD) L % R -- remainder of division
_(TOK_MINUS,   Pre, Int|Float) -- (NEG)   - R
-- bitwise
_(TOK_AND,   Bin, Int) -- (AND) L &  R
_(TOK_OR,    Bin, Int) -- (OR)  L |  R
_(TOK_HAT,   Bin, Int) -- (XOR) L ^  R
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
RVALUE = 1<<0 -- as rvalue

end

--------------------------------------------------------------------------------------------------

function ast_srcpos(ast, idx) -- u32
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
        local offs = ast_offs_of_idx(idx)
        -- dlog("VISIT #%d offs=%u", idx_orig, offs)
        local kind = ast1:get_u8(offs)
        local info = ast_info[kind]
        -- dlog("%srepr> #%d [%u] %s", string.rep("    ", depth), idx_orig, offs, info.name)

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

        -- visit type
        if as_ir and not ast_istype(ast1, idx) then
            local typ_idx = ast1:get_i32(offs + 4)
            visit(typ_idx)
        end

        -- visit children
        if info.repr ~= nil then
            info.repr(ast1, idx, write, visit, as_ir)
        elseif info.visit ~= nil then
            info.visit(ast1, idx, visit)
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
function ast_list_resolve(ast, idx, ir, resolver, flags)
    local offs = ast_offs_of_idx(idx)
    local kind = ast:get_u8(offs)
    local count = ast:get_u32(offs) >> 8

    -- allocate temporary storage for element types
    local typ_idxv
    local typ_idxv_offs = 0
    if kind == AST_TUPLE.kind or (kind == AST_RETURN.kind and count > 1) then
        typ_idxv = __rt.buf_create(count * 4)
        typ_idxv:alloc(count * 4)
    end

    -- create list up front
    local list_offs = ast_list_add_upfront(ir, kind, TYPE_nil, count)

    -- resolve children
    local end_offs = offs + 4 + (count * 4)
    local elem_offs = list_offs + 8
    local elem_idx
    local child_flags = kind == AST_BLOCK.kind and (flags & ~RVALUE) or flags
    for offs = offs + 8, end_offs, 4 do
        if offs == end_offs then
            child_flags = flags
        end

        elem_idx = ast:get_i32(offs)
        elem_idx = resolver.resolve(elem_idx, child_flags)

        -- set list element value
        ir:set_i32(elem_offs, elem_idx)
        elem_offs = elem_offs + 4

        if typ_idxv ~= nil then
            -- set list element type
            typ_idxv:set_i32(typ_idxv_offs, resolver.typeof(elem_idx))
            typ_idxv_offs = typ_idxv_offs + 4
        end
    end

    -- create type (with interning; should match AST_TUPLETYPE.resolve)
    if typ_idxv ~= nil then
        -- We can simply hash the array of idx values, with the type's kind as the hash seed
        local typ_hash = typ_idxv:hash(AST_TUPLETYPE.kind)
        local typ_idx = resolver.internmap[typ_hash]
        if typ_idx == nil then
            typ_idx = ast_list_add(ir, AST_TUPLETYPE.kind, TYPE_type, typ_idxv)
            resolver.internmap[typ_hash] = typ_idx
        end
        ir:set_i32(list_offs + 4, typ_idx)
    elseif child_flags & RVALUE ~= 0 then
        -- When a block is used as an rvalue, its type is the type of the last expression
        ir:set_i32(list_offs + 4, resolver.typeof(elem_idx))
    end

    return ast_idx_of_offs(list_offs)
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
