DIAG_ERR  = "error"
DIAG_WARN = "warning"
DIAG_INFO = "note"

DIAG_COLORS = {
    [DIAG_ERR] = "31", -- red
    [DIAG_WARN] = "33", -- yellow
    [DIAG_INFO] = "",
}

function diag(unit, level, srcpos, format, ...) --> void
    assert(srcpos ~= nil)
    if level == DIAG_ERR then
        unit.errcount = unit.errcount + 1
    end
    if unit.diag_handler ~= nil then
        return unit:diag_handler(level, srcpos, format, ...)
    else
        return diag_print(unit, level, srcpos, format, ...)
    end
end

function diag_fmt(unit, level, srcpos, format, ...) --> string
    local s = fmt("%s: \x1b[0;1;%sm%s:\x1b[0m " .. format,
                  srcpos_fmt(srcpos, unit), DIAG_COLORS[level], level, ...)

    -- if level == DIAG_ERR then
    local as_error = level == DIAG_ERR
    local context_lines = as_error and 1 or 0
    local source_snippet = srcpos_source_snippet(srcpos, unit, as_error, context_lines)
    if #source_snippet > 0 then
        s = s .. source_snippet
    end
    -- end

    return s
end

function diag_print(unit, level, srcpos, format, ...)
    return io.stderr:write(diag_fmt(unit, level, srcpos, format, ...) .. "\n")
end
