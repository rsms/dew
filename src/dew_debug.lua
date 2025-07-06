do
local dew_debug_callstack = {}

function dew_debug_calltrace_enable()
    debug.sethook(function(what)
        if what == "return" then
            dew_debug_callstack[#dew_debug_callstack] = nil
        else
            -- see https://www.lua.org/manual/5.4/manual.html#lua_getinfo
            dew_debug_callstack[#dew_debug_callstack + 1] = debug.getinfo(3, "lnSt")
        end
    end, "cr")
end

function dew_debug_print_calltrace(limit)
    debug.sethook()
    if limit == nil then
        limit = 0
    end
    local start = #dew_debug_callstack - 2
    for i = start, 1, -1 do
        local info = dew_debug_callstack[i]
        local line = info.currentline ~= nil and info.currentline or info.linedefined
        if i == start then
            info.istailcall = false
        elseif info.istailcall and info.currentline == info.linedefined + 1 then
            line = info.linedefined
        end
        local srcloc = (info.short_src and info.short_src or "?") .. ":" .. line
        local fun = info.name ~= nil and info.name or "_"
        local prefix = i == start and "┌╴" or "├╴"
        local suffix = ""
        if info.istailcall then
            suffix = " \x1b[36m(tail call at " .. info.currentline .. ")\x1b[0m"
        end
        local srclines_ctx = 1
        local srclines_ctx_before = line == info.linedefined and 0 or srclines_ctx
        local srclines = dew_debug_srclines(info.short_src, line, srclines_ctx_before, srclines_ctx)
        -- local what = info.namewhat ~= "" and info.namewhat or "function"
        printf("%s\x1b[47;30m %-20s \x1b[0m \x1b[34;1m%s\x1b[0m%s",
               prefix, fun, srcloc, suffix)
        if #srclines then
            for i = 1, #srclines do
                print("│" .. srclines[i])
            end
            print("│")
        end
        limit = limit - 1
        if limit == 0 then
            print("╵  ... " .. i .. " more")
            break
        end
    end
    dew_debug_callstack = {}
end

function dew_debug_srclines(srcfile, line_number, nbefore, nafter)
    local srclines = {}
    local file, _ = io.open(srcfile)
    if not file then
        return srclines
    end
    local current_line = 1
    for line in file:lines() do
        if current_line == line_number then
            srclines[#srclines + 1] = line
        elseif current_line >= line_number - nbefore and current_line <= line_number + nafter then
            srclines[#srclines + 1] = "\x1b[2m" .. line .. "\x1b[0m"
        end
        if current_line == line_number + nafter then
            break
        end
        current_line = current_line + 1
    end
    file:close()
    return srclines
end

end
