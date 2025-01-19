fmt = string.format
numtype = math.type -- "integer" or "float"

function print_table(tbl, indent)
	indent = indent or 0
	local indent_str = string.rep("  ", indent)
	if type(tbl) ~= "table" then
		print(indent_str .. tostring(tbl))
		return
	end
	-- print(indent_str .. "{")
	print("{")
	local keys = {}
	for k in pairs(tbl) do table.insert(keys, k) end
	table.sort(keys)
	for _, k in ipairs(keys) do
		local key = tostring(k)
		local v = tbl[k]
		io.write(indent_str .. "  [" .. key .. "] = ")
		if type(v) == "table" then
			print_table(v, indent + 1)
		else
			print(tostring(v))
		end
	end
	print(indent_str .. "}")
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
