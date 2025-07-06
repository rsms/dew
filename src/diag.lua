DIAG_ERR  = "error"
DIAG_WARN = "warning"
DIAG_INFO = "note"

DIAG_COLORS = {
    [DIAG_ERR] = "31",
    [DIAG_WARN] = "33",
    [DIAG_INFO] = "",
}

function diag(unit, kind, srcpos, format, ...) --> void
    assert(srcpos ~= nil)
    if kind == DIAG_ERR then
        unit.errcount = unit.errcount + 1
    end
    if unit.diag_handler ~= nil then
        return unit:diag_handler(kind, srcpos, format, ...)
    else
        return diag_print(unit, kind, srcpos, format, ...)
    end
end

function diag_fmt(unit, kind, srcpos, format, ...) --> string
    local line, col = srcpos_linecol(srcpos, unit.src)
    local color = DIAG_COLORS[kind]
    return fmt("%s:%d:%d: \x1b[0;1;%sm%s:\x1b[0m " .. format,
               unit.srcfile, line, col, color, kind, ...)
end

function diag_print(unit, kind, srcpos, format, ...)
    return io.stderr:write(diag_fmt(unit, kind, srcpos, format, ...) .. "\n")
end
