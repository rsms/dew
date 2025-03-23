DIAG_ERR  = "error"
DIAG_WARN = "warning"
DIAG_INFO = "note"

DIAG_COLORS = {
	[DIAG_ERR] = "31",
	[DIAG_WARN] = "33",
	[DIAG_INFO] = "",
}

function diag(kind, unit, srcpos, format, ...)
	assert(srcpos ~= nil)
	if kind == DIAG_ERR then
		unit.errcount = unit.errcount + 1
	end
	local line, col = srcpos_linecol(srcpos, unit.src)
	local color = DIAG_COLORS[kind]
	io.stderr:write(fmt(
		"%s:%d:%d: \x1b[0;1;%sm%s:\x1b[0m " .. format .. "\n",
		unit.srcfile, line, col, color, kind, ...))
end
