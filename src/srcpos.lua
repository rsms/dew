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

function srcpos_off(p) return p & 0xffffff end
function srcpos_span(p) return p>>24 & 0xff end

function srcpos_union(srcpos1, srcpos2)
	local start1, start2 = srcpos_off(srcpos1), srcpos_off(srcpos2)
	local end1, end2 = start1 + srcpos_span(srcpos1), start2 + srcpos_span(srcpos2)
	local start3 = start1 < start2 and start1 or start2
	local end3 = end1 > end2 and end1 or end2
	return srcpos_make(start3, end3 - start3)
end

function srcpos_after(srcpos, span)
	local off = srcpos_off(srcpos) + srcpos_span(srcpos)
	if span == nil then span = 0 end
	return srcpos_make(off, span)
end

function srcpos_linecol(srcpos, src) -- line, col
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

function srcpos_fmt(srcpos, src)
	if srcpos == 0 or src == nil or #src == 0 then return "" end
	local line, col = srcpos_linecol(srcpos, src)
	return fmt("%d:%d", line, col)
end
