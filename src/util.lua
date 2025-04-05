fmt = string.format
numtype = math.type -- "integer" or "float"

local function _print_table(tbl, indent, seen)
	local indent_chunk = "  "
	local indent_str = string.rep(indent_chunk, indent)
	if type(tbl) ~= "table" then
		print(indent_str .. tostring(tbl))
		return
	end
	if seen[tbl] then
		print("<cycle " .. tostring(tbl) .. ">")
		return
	end
	seen[tbl] = true
	print(indent_str .. "{ " .. tostring(tbl))
	local keys = {}
	for k in pairs(tbl) do table.insert(keys, k) end
	table.sort(keys)
	for _, k in ipairs(keys) do
		local key = tostring(k)
		local v = tbl[k]
		io.write(indent_str .. indent_chunk .. "[" .. key .. "] = ")
		if type(v) == "table" then
			_print_table(v, indent + 1, seen)
		else
			print(tostring(v))
		end
	end
	print(indent_str .. "}")
end

function print_table(tbl, indent)
	_print_table(tbl, indent or 0, {})
end

function limit_string(s, maxlen)
	if #s <= maxlen then return s end
	return s:sub(1, maxlen - 1) .. "…"
end

function noop() end

function dlog(...)
	print(fmt(...))
end

function memory_usage()
    return collectgarbage("count") -- Returns memory usage in kilobytes
end

B = string.byte
