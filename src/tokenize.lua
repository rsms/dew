token_names = {}
keywords = {}
keywordmin, keywordmax = 1000, 0

function tokname(tok) return token_names[tok] end

do
local function deftok(name)
	local tok = #token_names + 1; assert(tok <= 0xff)
	token_names[tok] = name
	return tok
end
local function defkeyword(name)
	local tok = deftok(name)
	keywords[name] = tok
	if #name < keywordmin then keywordmin = #name end
	if #name > keywordmax then keywordmax = #name end
	return tok
end

-- tokens
TOK_SEMI                                 = deftok(';')
TOK_COMMA                                = deftok(',')
TOK_COLON                                = deftok(':')
TOK_DOT,     TOK_DOTDOTDOT               = deftok('.'),  deftok('...')
TOK_LPAREN,  TOK_RPAREN                  = deftok('('),  deftok(')')
TOK_LBRACK,  TOK_RBRACK                  = deftok('['),  deftok(']')
TOK_LBRACE,  TOK_RBRACE                  = deftok('{'),  deftok('}')
TOK_LT,      TOK_LTEQ                    = deftok('<'),  deftok('<=')
TOK_GT,      TOK_GTEQ                    = deftok('>'),  deftok('>=')
TOK_LTLT,    TOK_LTLTEQ                  = deftok('<<'), deftok('<<=')
TOK_GTGT,    TOK_GTGTEQ                  = deftok('>>'), deftok('>>=')
TOK_EQ,      TOK_EQEQ                    = deftok('='),  deftok('==')
TOK_NOT,     TOK_NOTEQ                   = deftok('!'),  deftok('!=')
TOK_PLUS,    TOK_PLUSPLUS,   TOK_PLUSEQ  = deftok('+'),  deftok('++'), deftok('+=')
TOK_MINUS,   TOK_MINUSMINUS, TOK_MINUSEQ = deftok('-'),  deftok('--'), deftok('-=')
TOK_STAR,    TOK_STAREQ                  = deftok('*'),  deftok('*=')
TOK_SLASH,   TOK_SLASHEQ                 = deftok('/'),  deftok('/=')
TOK_PERCENT, TOK_PERCENTEQ               = deftok('%'),  deftok('%=')
TOK_HAT,     TOK_HATEQ                   = deftok('^'),  deftok('^=')
TOK_AND,     TOK_ANDAND,     TOK_ANDEQ   = deftok('&'),  deftok('&&'), deftok('&=')
TOK_OR,      TOK_OROR,       TOK_OREQ    = deftok('|'),  deftok('||'), deftok('|=')
TOK_TILDE                                = deftok('~')

TOK_COMMENT                              = deftok('COMMENT')
TOK_ID                                   = deftok('ID')
TOK_INT                                  = deftok('INT')
TOK_FLOAT                                = deftok('FLOAT')

-- keywords
TOK_FUN    = defkeyword('fun')
TOK_RETURN = defkeyword('return')

end

function tokenize_unit(unit)
	local include_comments = unit.include_comments
	local src = unit.src
	local srcidx, srcend = 1, #src + 1
	local tok, tokstart, tokend = TOK_SEMI, 1, 0
	local lineidx, lineno = 1, 1, 0
	local insertsemi = false
	local column = function() return (tokstart + 1) - lineidx end
	local curr_byte = function() return string.byte(src, srcidx) end
	local value = nil
	local value_is_int = false

	local function diag_err(format, ...)
		local srcpos = srcpos_make(tokstart, tokend - tokstart)
		srcidx = srcend -- stop scanning
		diag(unit, DIAG_ERR, srcpos, format, ...)
		if DEBUG_TOKENS then error("syntax error") end
	end

	local function next_byte(byte1, byte2)
		if string.byte(src, srcidx) == byte1 then
			if byte2 == nil then
				tokend = tokend + 1
				srcidx = srcidx + 1
				return true
			elseif string.byte(src, srcidx + 1) == byte2 then
				tokend = tokend + 2
				srcidx = srcidx + 2
				return true
			end
		end
		return false
	end

	local function scan_comment()
		-- enter with srcidx just after second "/" in "// comment"
		while srcidx < srcend do
			byte = string.byte(src, srcidx)
			if byte == 0x0A then break end
			srcidx = srcidx + 1
		end
		tokend = srcidx
		value = string.sub(src, tokstart, tokend - 1)
	end

	local function scan_block_comment()
		diag_err("block comments not implemented")
	end

	local function byte_is_sep(byte)
		return (byte < B('0') and byte ~= B('$')) or -- 0x00..SP !"#%&'()*+,-./
		       (byte > B('9') and byte < B('A')) or -- :;<=>?@
		       (byte > B('Z') and byte < B('a') and byte ~= B('_')) or -- [\]^`
		       (byte > B('z') and byte < 0x7F) -- {|}~
	end

	local function scan_ident()
		while srcidx < srcend do
			byte = string.byte(src, srcidx)
			if byte_is_sep(byte) then
				break
			end
			srcidx = srcidx + 1
		end
		tokend = srcidx
		insertsemi = true

		local str = string.sub(src, tokstart, tokend - 1)

		-- lookup keyword
		local len = tokend - tokstart
		if len >= keywordmin and len <= keywordmax then
			local kw_tok = keywords[str]
			if kw_tok ~= nil then
				return kw_tok
			end
		end

		value = id_intern(str) -- id_idx
		value_is_int = true -- id_idx is an integer
		return TOK_ID
	end

	local function scan_num(base)
		local allowsign = false
		local allowdot = base == 10
		local need_tr = false
		value_is_int = true
		local function err_invalid(format, ...)
			diag_err("invalid %s literal" .. format,
			         value_is_int and "integer" or "floating-point", ...)
		end
		while srcidx < srcend do
			byte = string.byte(src, srcidx)
			if byte == B('E') or byte == B('e') then
				if base == 10 then
					allowsign = true
					allowdot = false
					value_is_int = false
				elseif base ~= 16 then
					break
				end
			elseif byte == B('+') or byte == B('-') then
				if base ~= 10 or not allowsign then break end
				value_is_int = false
			elseif byte == B('.') then
				if not allowdot then
					tokstart = srcidx
					tokend = srcidx + 1
					err_invalid(": unexpected '.'")
					break
				end
				allowsign = true
				value_is_int = false
			elseif byte == B('_') then
				need_tr = true
			elseif byte_is_sep(byte) then
				break
			end
			srcidx = srcidx + 1
		end
		tokend = srcidx
		insertsemi = true

		-- interpret number literal
		local tokstart_off = base ~= 10 and 2 or 0 -- skip leading "0x"
		value = string.sub(src, tokstart + tokstart_off, tokend - 1)
		if value_is_int then
			-- since we don't know the type, interpret number as u64; limit=0xffffffffffffffff
			value, err = __rt.intscan(value, base, 0xffffffffffffffff)
			if err ~= 0 then
				if err == __rt.ERR_RANGE then
					err_invalid("; too large, overflows i64")
				else
					local errname, errdesc = __rt.errstr(err)
					err_invalid("; %s", errdesc)
				end
			end
			return TOK_INT
		else
			if need_tr then
				if string.byte(value, #value - 1) == B('_') then
					err_invalid("; ends with '_'")
				end
				value = string.gsub(value, "_", "")
			end
			value = tonumber(value, nil)
			if value == nil or numtype(value) ~= 'float' then
				err_invalid("")
			end
			return TOK_FLOAT
		end
	end

	local function scan_num0()
		-- e.g. 0xBEEF, 0o0644, 0b01011, 0.12, 0123
		local base = 10
		local byte = curr_byte()
		if     byte == B('x') or byte == B('X') then base = 16; srcidx = srcidx + 1
		elseif byte == B('o') or byte == B('O') then base = 8; srcidx = srcidx + 1
		elseif byte == B('b') or byte == B('B') then base = 2; srcidx = srcidx + 1
		end
		return scan_num(base)
	end

	local function scan_next()
		value = nil
		value_is_int = false
		local byte = 0
		local insertsemi_do = false
		local insertsemi_prev = insertsemi

		-- whitespace
		while true do
			-- check for end of input
			if srcidx == srcend then
				tokend = srcidx
				tokstart = srcidx
				if insertsemi then insertsemi = false; return TOK_SEMI end
				return nil
			end
			byte = curr_byte()
			-- dlog("%3d\t%2d:%-2d\t%02X\t%c",
			--      srcidx, lineno, column(), byte, (byte > 20) and byte or 0x20)
			srcidx = srcidx + 1
			if byte == B('\n') then
				lineidx = srcidx
				lineno = lineno + 1
				insertsemi_do = insertsemi
			elseif byte ~= B('\t') and byte ~= B(' ') then
				break
			end
		end

		insertsemi = false -- reset state
		if insertsemi_do then
			srcidx = srcidx - 1
			tokstart = tokstart + (tokend - tokstart)
			return TOK_SEMI
		end
		tokstart = srcidx - 1
		tokend = srcidx

		-- token
		if     byte == B(';') then return TOK_SEMI
		elseif byte == B(',') then return TOK_COMMA
		elseif byte == B(':') then return TOK_COLON
		elseif byte == B('.') then
			if next_byte(B('.'),B('.')) then insertsemi = true; return TOK_DOTDOTDOT end
			return TOK_DOT
		elseif byte == B('(') then return TOK_LPAREN
		elseif byte == B(')') then insertsemi = true; return TOK_RPAREN
		elseif byte == B('[') then return TOK_LBRACK
		elseif byte == B(']') then insertsemi = true; return TOK_RBRACK
		elseif byte == B('{') then return TOK_LBRACE
		elseif byte == B('}') then insertsemi = true; return TOK_RBRACE
		elseif byte == B('<') then
			if next_byte(B('<'),B('=')) then return TOK_LTLTEQ end
			if next_byte(B('<')) then return TOK_LTLT end
			if next_byte(B('=')) then return TOK_LTEQ end
			return TOK_LT
		elseif byte == B('>') then
			if next_byte(B('>'),B('=')) then return TOK_GTGTEQ end
			if next_byte(B('>')) then return TOK_GTGT end
			if next_byte(B('=')) then return TOK_GTEQ end
			return TOK_GT
		elseif byte == B('=') then return next_byte(B('=')) and TOK_EQEQ or TOK_EQ
		elseif byte == B('!') then return next_byte(B('=')) and TOK_NOTEQ or TOK_NOT
		elseif byte == B('+') then
			if next_byte(B('+')) then insertsemi = true; return TOK_PLUSPLUS end
			if next_byte(B('=')) then return TOK_PLUSEQ end
			return TOK_PLUS
		elseif byte == B('-') then
			if next_byte(B('-')) then insertsemi = true; return TOK_MINUSMINUS end
			if next_byte(B('=')) then return TOK_MINUSEQ end
			return TOK_MINUS
		elseif byte == B('*') then return next_byte(B('=')) and TOK_STAREQ or TOK_STAR
		elseif byte == B('/') then
			if next_byte(B('/')) then
				insertsemi = insertsemi_prev
				scan_comment()
				if include_comments then return TOK_COMMENT end
				return scan_next()
			end
			if next_byte(B('*')) then
				insertsemi = insertsemi_prev
				scan_block_comment()
				if include_comments then return TOK_COMMENT end
				return scan_next()
			end
			if next_byte(B('=')) then return TOK_SLASHEQ end
			return TOK_SLASH
		elseif byte == B('%') then return next_byte(B('%')) and TOK_PERCENTEQ or TOK_PERCENT
		elseif byte == B('^') then return next_byte(B('^')) and TOK_HATEQ or TOK_HAT
		elseif byte == B('&') then
			if next_byte(B('&')) then return TOK_ANDAND end
			if next_byte(B('=')) then return TOK_ANDEQ end
			return TOK_AND
		elseif byte == B('|') then
			if next_byte(B('|')) then return TOK_OROR end
			if next_byte(B('=')) then return TOK_OREQ end
			return TOK_OR
		elseif byte == B('~') then return TOK_TILDE
		elseif byte == B("'") then return scan_charlit()
		elseif byte == B('"') then return scan_strlit()
		elseif byte == B('0') then return scan_num0()
		elseif byte >= B('1') and byte <= B('9') then return scan_num(10)
		else return scan_ident()
		end
	end

	-- scan & encode tokens
	local tokens = {}
	while true do
		tok = scan_next()
		if tok == nil then
			break
		end

		-- if DEBUG_TOKENS then
		-- 	dlog("token: %s:%d:%d\t%s\t[%d:%d] '%s'",
		-- 	     unit.srcfile, lineno, column(), tokname(tok), tokstart, tokend,
		-- 	     string.sub(src, tokstart, tokend - 1))
		-- end

		-- Encode token into u64.
		-- Most tokens need just one u64, where there's no additional value (e.g. punctuation)
		-- or the value is a small integer (e.g. operator or identifier.)
		-- Tokens with large or complex values (e.g. floating-point or string literal) have their
		-- value stored in a second array slot.
		-- The primary encoding looks like this:
		--
		-- bit           1111111111222222222233 333333334444444444555555 55556666
		--     01234567890123456789012345678901 234567890123456789012345 67890123
		--     srcpos                           value                    tok
		--     u32                              u24                      u8
		--
		local srcpos = srcpos_make(tokstart, tokend - tokstart)
		local p1 = tok | (srcpos & 0xffffffff)<<32
		if value_is_int then
			if value >= 0 and value < 0xfffffff then
				-- squeeze value into 24 bits of token entry
				p1 = p1 | value<<8
				value = nil
			else
				p1 = p1 | 0xffffff00
			end
		end
		tokens[#tokens + 1] = p1
		tokens[#tokens + 1] = value -- no-op if value is nil
	end

	if DEBUG_TOKENS then
		dlog("tokens of unit %s:", unit.srcfile); dlog_tokens(tokens, unit)
	end

	return tokens
end


function token_iterator(tokens)
	local i, tok, srcpos, value = 1, 0, 0, 0, 0
	local function next()
		if i > #tokens then
			return nil, 0, nil
		end
		local v = tokens[i]
		tok = v & 0xff
		srcpos = v>>32
		value = v>>8 & 0xffffff
		assert(value ~= 0 or tok ~= TOK_ID, "ID with separate value (id_idx > 0xffffff)")
		if tok == TOK_COMMENT or tok == TOK_FLOAT or (tok == TOK_INT and value == 0xffffff) then
			i = i + 1 -- value in second array slot
			value = tokens[i]
		end
		i = i + 1
		return tok, srcpos, value
	end
	local function save_state()
		return { i, tok, srcpos, value }
	end
	local function restore_state(snapshot)
		i, tok, srcpos, value = table.unpack(snapshot)
		return tok, srcpos, value
	end
	return next, save_state, restore_state
end


function dlog_tokens(tokens, unit, line_prefix)
	local i = 1
	line_prefix = line_prefix and line_prefix or ""
	for tok, srcpos, value, meta in token_iterator(tokens) do
		local srcpos_str = (unit == nil) and "" or srcpos_fmt(srcpos, unit.src)
		if tok == TOK_INT then
			-- value = fmt("%8s 0x%s", __rt.intfmt(value, 10), __rt.intfmt(value, 16, true))
			value = fmt("%8d 0x%x", value, value)
		elseif tok == TOK_FLOAT then
			value = fmt("%8g", value)
		elseif tok == TOK_ID then
			value = id_str(value)
		else
			value = tostring(value)
		end
		dlog("%s%3d │ %-8s #%-3d %5d+%-3d  %-8s  %8s",
		     line_prefix, i,
		     token_names[tok], tok,
		     srcpos_off(srcpos), srcpos_span(srcpos), srcpos_str,
		     value)
		i = i + 1
	end
end
