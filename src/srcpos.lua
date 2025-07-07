function srcpos_make(off, span)
    -- bit           1111111111222222222233 33333333 444444444455555555556666
    --     01234567890123456789012345678901 23456789 012345678901234567890123
    --     unused                           span     off
    --     u32                              u8       u24
    -- assert(numtype(off) == 'integer', fmt("numtype(off) = %s", numtype(off)))
    -- assert(numtype(span) == 'integer', fmt("numtype(span) = %s", numtype(span)))
    return ((off > 0xffffff and 0xffffff or off) & 0xffffff) |
           (((span > 0xff and 0xff or span) & 0xff) << 24)
end

function srcpos_off(srcpos) return srcpos & 0xffffff end
function srcpos_span(srcpos) return srcpos>>24 & 0xff end

function srcpos_union(srcpos1, srcpos2) --> srcpos
    if srcpos1 == 0 then
        return srcpos2
    end
    if srcpos2 == 0 then
        return srcpos1
    end
    local start1, start2 = srcpos_off(srcpos1), srcpos_off(srcpos2)
    local end1, end2 = start1 + srcpos_span(srcpos1), start2 + srcpos_span(srcpos2)
    local start3 = start1 < start2 and start1 or start2
    local end3 = end1 > end2 and end1 or end2
    return srcpos_make(start3, end3 - start3)
end

function srcpos_after(srcpos, span) --> srcpos
    local off = srcpos_off(srcpos) + srcpos_span(srcpos)
    if span == nil then span = 0 end
    return srcpos_make(off, span)
end

function srcpos_linecol(srcpos, src) --> line, col
    assert(srcpos ~= nil)
    local off = srcpos_off(srcpos)
    local i, j, iend, line = 1, 0, off, 1
    local col, col_prev = 0, 0
    while i < iend do
        j = string.find(src, '\n', i, true)
        if j == nil or j > iend then break end
        line = line + 1
        i = j + 1
        col_prev = col
        col = (off - i) + 1
    end
    -- synthetic semicolon appears at the newline position, which ends up with the
    -- otherwise impossible column '0', so we check for that and return the end of the
    -- previous line for synthetic semicolons:
    if col == 0 and line > 1 then
        return line - 1, col_prev
    end
    return line, (off - i) + 1
end

function srcpos_fmt(srcpos, unit, omit_srcfile) --> string
    if srcpos == 0 or unit == nil or unit.src == nil or #unit.src == 0 then
        if omit_srcfile or unit == nil then
            return ""
        end
        return unit.srcfile
    end
    local line, col = srcpos_linecol(srcpos, unit.src)
    if omit_srcfile then
        return fmt("%d:%d", line, col)
    end
    return fmt("%s:%d:%d", unit.srcfile, line, col)
end

function srcpos_source_snippet(srcpos, unit, as_error, context_lines) --> string
    local src = unit.src
    local buf = __rt.buf_create(256)

    local function find_line_start(begin_at_offs) --> start_offs
        for i = begin_at_offs, 0, -1 do
            if string.byte(src, i) == 0x0A then
                return i + 1
            end
        end
        return 0
    end

    local function find_line_end(begin_at_offs) --> end_offs
        local end_offs = string.find(src, '\n', begin_at_offs, true)
        return end_offs == nil and #src + 1 or end_offs
    end

    -- find focus line
    local offs = srcpos_off(srcpos)
    local line_start = find_line_start(offs)
    local line_end = find_line_end(offs)
    local ctx_line_start, ctx_line_end
    local line_no, column = srcpos_linecol(srcpos, unit.src)

    -- find & add context line before focus line
    if line_start > 1 and context_lines > 0 then
        ctx_line_end = line_start - 1
        ctx_line_start = find_line_start(ctx_line_end - 1)
        buf:append(fmt("\n\x1b[2m%6d │", line_no - 1))
        buf:append(src:sub(ctx_line_start, ctx_line_end - 1))
        buf:append("\x1b[0m")
    end

    -- add focus line
    if as_error then
        buf:append(fmt("\n\x1b[1;41;37m%6d \x1b[0;2m│\x1b[0m", line_no))
    else
        buf:append(fmt("\n\x1b[47;30m%6d \x1b[0;2m│\x1b[0m", line_no))
    end
    local line = src:sub(line_start, line_end - 1)
    buf:append(line)

    -- find context line after focus line
    ctx_line_start = line_end + 1
    ctx_line_end = find_line_end(ctx_line_start)

    -- highlight relevant range with arrow or underline
    local span = srcpos_span(srcpos)
    local indent = line:gsub("^(%s*).*$", "%1") -- leading whitespace
    if column > #indent then
        indent = indent .. string.rep(" ", (column - 1) - #indent)
    end
    if ctx_line_start < ctx_line_end and context_lines > 0 then
        buf:append("\n       \x1b[2m│\x1b[22m" .. indent)
    else
        buf:append("\n        " .. indent)
    end
    local highlight_style = as_error and "\x1b[1;31m" or "\x1b[1m"
    if span == 0 then
        buf:append(highlight_style .. "▲\x1b[0m")
    else
        buf:append(highlight_style .. string.rep("▔", span) .. "\x1b[0m")
    end

    -- add context line after focus line
    if ctx_line_start < ctx_line_end and context_lines > 0 then
        buf:append(fmt("\n\x1b[2m%6d │", line_no + 1))
        buf:append(src:sub(ctx_line_start, ctx_line_end - 1))
        buf:append("\x1b[0m")
    end

    return buf:str()
end
