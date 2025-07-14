function resolve_unit(unit)
    unit.ir = __rt.buf_create(#unit.ast)
    unit.ir_srcmap = {} -- set to nil to not save srcpos

    local internmap = {} -- (hash u64) => (idx i32) -- TODO: per package? global?
    local ast = unit.ast
    local ir = unit.ir
    local idstack = __rt.buf_create(256) -- [(id_idx u, idx u32)]
    local idstack_scope_offs = 0 -- bottom of current scope
    local resolver = {
        ctxtype = 0,
        unit = unit,
        internmap = internmap,
        fun_idx = 0, -- current function
        fun_restyp_idx = 0, -- current function's result type
    }

    function resolver.typeof(idx)
        return ast_typeof(ir, idx)
    end

    function resolver.diag_err(srcpos, format, ...)
        return diag(unit, DIAG_ERR, srcpos, format, ...)
    end

    function resolver.diag_warn(srcpos, format, ...)
        return diag(unit, DIAG_WARN, srcpos, format, ...)
    end

    function resolver.srcpos(idx)
        return 0
    end

    function resolver.resolve(idx, flags) --> ir_idx
        assert(idx ~= 0)
        local kind = ast:get_u8(ast_offs_of_idx(idx))
        local info = ast_info[kind]
        assert(info.resolve ~= nil, "TODO: " .. info.name .. ".resolve")
        if flags == nil then
            flags = 0
        end
        return info.resolve(ast, idx, ir, resolver, flags)
    end

    function resolver.id_scope_open() --> scopestate
        idstack_scope_offs = #idstack
        return #idstack
    end

    function report_unused(idx, offs, kind)
        -- TODO: for top-level (scope==0), ignore "pub" storage, exported from package
        local load_and_store_count = ir:get_u32(offs + 12)
        if load_and_store_count ~= 0 then
            return
        end
        if kind == AST_FUN.kind and AST_FUN.body(ir, idx) == 0 then
            return resolver.diag_warn(resolver.srcpos(idx),
                                      "unused function declaration %s", ast_fmt(ir, idx))
        end
        return resolver.diag_warn(resolver.srcpos(idx),
                                  "unused %s %s (use '_' to silence)",
                                  ast_descr(ir, idx), ast_fmt(ir, idx))
    end

    function resolver.id_scope_close(scope, warn_unused)
        assert(scope <= #idstack and scope % 8 == 0, tostring(#idstack) .. ", " .. tostring(scope))
        if #idstack - scope == 0 then
            return
        end

        -- check for unused storage (vars, params, local functions)
        if warn_unused ~= false then
            for idstack_offs = scope, #idstack - 8, 8 do
                local idx = idstack:get_i32(idstack_offs + 4)
                local offs = ast_offs_of_idx(idx)
                local kind = ir:get_u8(offs)
                if kind == AST_VAR.kind or kind == AST_PARAM.kind or kind == AST_FUN.kind then
                    report_unused(idx, offs, kind)
                end
            end
        end

        idstack:resize(scope)
        idstack_scope_offs = scope
    end

    local function idstack_lookup(id_idx, base_offs) --> idx (0 if not found)
        assert(id_idx ~= 0)
        if id_idx == ID__ then
            return 0
        end
        local offs = idstack:find_u32(#idstack, base_offs, 8, id_idx)
        return offs ~= nil and idstack:get_i32(offs + 4) or 0
    end

    function resolver.id_lookup(id_idx) --> idx (0 if not found)
        assert(id_idx ~= 0)
        local idx = idstack_lookup(id_idx, 0)
        if idx ~= 0 then
            return idx
        end
        -- fall back to looking for a built-in thing
        return builtin_idtab_lookup(id_idx)
    end

    function resolver.id_lookup_def(id_idx) --> idx (0 if not found)
        assert(id_idx ~= 0)
        return idstack_lookup(id_idx, 0)
    end

    function resolver.id_lookup_def_in_current_scope(id_idx) --> idx (0 if not found)
        assert(id_idx ~= 0)
        return idstack_lookup(id_idx, idstack_scope_offs)
    end

    function resolver.id_define(id_idx, idx) --> idx
        if id_idx ~= 0 and id_idx ~= ID__ then
            idstack:push_u64((id_idx & 0xffffff) | ((idx & 0xffffffff) << 32))
        end
        return idx
    end

    function resolver.record_srcpos(ir_idx, srcpos)
    end

    function resolver.is_type_assignable(dst_typ_idx, src_typ_idx) --> bool
        if dst_typ_idx == src_typ_idx then
            return true
        elseif (dst_typ_idx == 0 or src_typ_idx == 0) and unit.errcount > 0 then
            return true -- avoid cascading errors
        end
        -- types differ; delegate to AST-kind-specific is_type_assignable function
        local f = ast_info[ast_kind(ir, dst_typ_idx)].is_type_assignable
        if f == nil then
            return false
        end
        return f(ir, dst_typ_idx, src_typ_idx)
    end

    function resolver.check_type_assignable(dst_typ_idx, src_typ_idx, src_idx) --> bool
        if resolver.is_type_assignable(dst_typ_idx, src_typ_idx) then
            return true
        end
        resolver.diag_err(resolver.srcpos(src_idx ~= 0 and src_idx or src_typ_idx),
            "value of type '%s' is not assignable to type '%s'",
            ast_fmt(ir, src_typ_idx), ast_fmt(ir, dst_typ_idx))
        return false
    end

    function resolver.intern_type(idx, kind, offs, hash_offs, hash_size) --> idx
        assert(hash_offs >= offs)
        local hash = ir:hash(kind, hash_offs, hash_offs + hash_size)
        local other_idx = internmap[hash]
        if other_idx ~= nil then
            ir:resize(offs) -- undo additions to IR stack
            return other_idx
        end
        internmap[hash] = idx
        return idx
    end

    -- map ir_idx => srcpos, if requested
    if unit.ir_srcmap ~= nil then
        local resolve_fun = resolver.resolve
        resolver.resolve = function(idx, flags)
            local srcpos = ast:get_u32(ast_offs_of_idx(idx) + 4)
            local ir_idx = resolve_fun(idx, flags)
            -- trace("unit.ir_srcmap[%u] = 0x%08x", ir_idx, srcpos)
            if ir_idx ~= 0 and ir_idx ~= nil and unit.ir_srcmap[ir_idx] == nil then
                unit.ir_srcmap[ir_idx] = srcpos
            end
            return ir_idx
        end
        function resolver.srcpos(idx) --> srcpos
            local srcpos = unit.ir_srcmap[idx]
            return srcpos ~= nil and srcpos or 0
        end
        function resolver.record_srcpos(ir_idx, srcpos)
            resolver.unit.ir_srcmap[ir_idx] = srcpos
        end
        -- function resolver.record_srcpos(idx, ir_idx)
        --     resolver.unit.ir_srcmap[ir_idx] = ast_srcpos(ast, idx)
        -- end
    end

    -- map ir_idx => [comment], if requested
    if unit.commentmap ~= nil then
        local resolve_fun = resolver.resolve
        local commentmap_orig = unit.commentmap
        unit.commentmap = {}
        resolver.resolve = function(idx, flags)
            local ir_idx = resolve_fun(idx, flags)
            local comments = commentmap_orig[idx]
            if comments ~= nil and ir_idx ~= 0 then
                unit.commentmap[ir_idx] = comments
            end
            return ir_idx
        end
    end

    if not DEBUG_RESOLVE then
        unit.ir_idx = resolver.resolve(unit.ast_idx, 0)
        return
    end

    -- DEBUG_RESOLVE (remainder of function)
    -- wrap resolve() in trace statements, if DEBUG_RESOLVE is enabled
    local trace_depth = 0
    local function trace(format, ...)
        dlog("\x1b[1;35mresolve⟩\x1b[0m " .. string.rep("    ", trace_depth) .. format, ...)
    end
    resolver.trace = trace

    local id_scope_open_orig = resolver.id_scope_open
    resolver.id_scope_open = function() --> offs
        local scope = id_scope_open_orig()
        trace("id_scope_open %d", scope)
        return scope
    end

    local id_scope_close_orig = resolver.id_scope_close
    resolver.id_scope_close = function(scope, ...)
        trace("id_scope_close %d", scope)
        local scope = id_scope_close_orig(scope, ...)
        return scope
    end

    local id_define_orig = resolver.id_define
    resolver.id_define = function(id_idx, idx) --> idx
        trace("define ID %s => #%d [offs %u, scope %d]",
              id_str(id_idx), idx, #idstack, idstack_scope_offs//8)
        return id_define_orig(id_idx, idx)
    end

    local id_lookup_orig = resolver.id_lookup
    resolver.id_lookup = function(id_idx)
        trace("lookup ID %s ...", id_str(id_idx))
        local idx = id_lookup_orig(id_idx)
        if idx == 0 then
            trace("lookup ID %s => NOT FOUND [scope %d]", id_str(id_idx), idstack_scope_offs//8)
        else
            trace("lookup ID %s => #%d [scope %d]", id_str(id_idx), idx, idstack_scope_offs//8)
        end
        return idx
    end

    local resolve_orig = resolver.resolve
    resolver.resolve = function(idx, flags)
        assert(idx ~= 0, "resolver.resolve(0)")
        local info = ast_info[ast:get_u8(ast_offs_of_idx(idx))]
        trace("%s#%d ...", info.name, idx)
        trace_depth = trace_depth + 1
        local ir_idx = resolve_orig(idx, flags)
        trace_depth = trace_depth - 1
        if ir_idx == 0 or ir_idx == nil then
            trace("%s#%d => (nothing)", info.name, idx)
        elseif ir_idx < 0 then
            trace("%s#%d => builtin#%d %s", info.name, idx, ir_idx, builtin_name(ir_idx))
        elseif ast_kind(ir, ir_idx) == AST_REF.kind then
            -- ID & REF are special: stores target in the meta slot (unlike other nodes)
            local target_idx = AST_REF.target(ir, ir_idx)
            local typ_idx = resolver.typeof(target_idx)
            trace("%s#%d => ir#%d REF -> ir#%d %s (type #%d %s)",
                  info.name, idx, ir_idx, target_idx, ast_kindname(ir, target_idx),
                  typ_idx, ast_kindname(ir, typ_idx))
        else
            local typ_idx = resolver.typeof(ir_idx)
            trace("%s#%d => ir#%d %s (type #%d %s)",
                  info.name, idx, ir_idx, ast_kindname(ir, ir_idx),
                  typ_idx, ast_kindname(ir, typ_idx))
        end
        assert(ir_idx ~= nil, info.name .. ".resolve() returned nil")
                local offs = ast_offs_of_idx(idx)
        return ir_idx
    end

    trace("%s", unit.srcfile)

    local unit_scope = resolver.id_scope_open()
    if unit.ast_idx ~= 0 then
        unit.ir_idx = resolver.resolve(unit.ast_idx)
    else
        unit.ir_idx = 0
    end
    resolver.id_scope_close(unit_scope, true)

    printf("IR after resolve_unit: (%d B)", #unit.ir)
    print(ir_repr(unit, unit.ir_idx))
end
