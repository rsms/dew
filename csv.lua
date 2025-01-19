local csv = {}

-- csv.read example use:
--[[
local iter, header = csv.read("example.csv")
for row in iter do
	for i, col in ipairs(header) do
		print(col, row[i])
	end
end
]]
csv.read = function(filename)
	local file, err = io.open(filename, "r")
	if not file then error("Could not open file " .. filename .. ": " .. tostring(err)) end

	-- Helper function to split a line into a table by commas
	local function split_csv_line(line)
		local row = {}
		local pattern = '"([^"]*)"' -- Pattern for quoted fields
		local pos = 1
		while pos <= #line do
			local c = line:sub(pos, pos)
			if c == '"' then
				local start_pos = pos + 1
				local end_pos = line:find('"', start_pos)
				table.insert(row, line:sub(start_pos, end_pos - 1))
				pos = end_pos + 1
			elseif c == ',' then
				table.insert(row, '')
				pos = pos + 1
			else
				local end_pos = line:find(',', pos) or (#line + 1)
				table.insert(row, line:sub(pos, end_pos - 1))
				pos = end_pos + 1
			end
		end
		return row
	end

	-- Capture header from first line
	local headers = split_csv_line(assert(file:read()))

	local function iterator()
		local line = file:read()
		if not line then
			file:close()
			return
		end
		return split_csv_line(line)
	end

	-- Return iterator and headers
	return iterator, headers
end

return csv
