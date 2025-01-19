if #arg ~= 3 then
	io.stderr:write("Usage: " .. arg[0] .. " <input> <output> <identifier>\n")
	os.exit(1)
end

local input_file = arg[1]
local output_file = arg[2]
local name = arg[3]

local file, err = io.open(input_file, "rb")
if not file then
	io.stderr:write("Error opening input file: " .. err .. "\n")
	os.exit(1)
end

local content = file:read("*a")
file:close()

local c_code = { "static const char ", name, "[] = {\n    " }
local line_length = 0
local is_first = true
for i = 1, #content do
	local byte = string.byte(content, i)
	local byte_str = tostring(byte)
	if line_length + #byte_str + 1 > 90 then -- +1 accounts for ","
		c_code[#c_code + 1] = "\n    "
		line_length = 0
	end
	c_code[#c_code + 1] = byte_str
	line_length = line_length + #byte_str + 1
	c_code[#c_code + 1] = ","
end
c_code[#c_code] = "};\n" -- replace last ","
c_code = table.concat(c_code, "")

file, err = io.open(output_file, "w")
if not file then
	io.stderr:write(output_file .. ": " .. err .. "\n")
	os.exit(1)
end
file:write(c_code)
file:close()
