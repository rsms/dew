local csv = require("csv")
local verbose = false
fmt = string.format

function printf(...) print(fmt(...)) end

function unpack_csv_row(row)
	local nbits, base, is_signed = row[1], row[2], row[3]
	nbits = tonumber(nbits, 10); assert(nbits >= 2 and nbits <= 64)
	base = tonumber(base, 10); assert(base >= 2 and base <= 36)
	is_signed = is_signed == '1'
	local limit = is_signed and (1 << (nbits - 1)) or ((1 << nbits) - 1)
	return nbits, base, is_signed, limit, row[4], row[5], row[6]
end

function print_intscan_invocation(input, base, limit, isneg, comment, value, err)
	local err_name, err_desc = dew.errstr(err)
	printf("intscan('%s', %d, 0x%x, isneg=%s)%s\n" ..
	       ">> %d %x [%s (%s)]",
	       input, base, limit, isneg and "true" or "false",
	       comment == nil and "" or " -- " .. comment,
	       value, value, err_name, err_desc)
end

function test_one_good_input(nbits, base, is_signed, limit, isneg, input, expected_value, comment)
	local value, err = dew.intscan(input, base, limit, isneg)
	if verbose then print_intscan_invocation(input, base, limit, isneg, comment, value, err) end

	if err ~= 0 then
		error(fmt("unexpected error:\n" ..
		          "  intscan('%s', %d, 0x%x, isneg=%s)%s\n" ..
		          "  >> %s (%s)",
		          input, base, limit, isneg and "true" or "false",
		          comment == nil and "" or " -- " .. comment,
		          dew.errstr(err)))
	end

	local expected_value_int = tonumber(expected_value, 10)
	if value ~= expected_value_int then
		error(fmt("unexpected result:\n" ..
		          "  intscan('%s', %d, 0x%x, isneg=%s)%s\n" ..
		          "  >> expected:  %-20d %016x '%s'\n" ..
		          "  >> actual:    %-20d %016x\n",
		          input, base, limit, isneg and "true" or "false",
		          comment == nil and "" or " -- " .. comment,
		          expected_value_int, expected_value_int, expected_value,
		          value, value))
	end
end

function test_good_input()
	local iter, columns = csv.read("intscan_test_data_ok.csv")
	local HYPHEN_BYTE = string.byte('-')
	local testcount = 0
	for row in iter do
		local nbits, base, is_signed, limit, input, expected_value, comment = unpack_csv_row(row)
		local isneg = false
		test_one_good_input(nbits, base, is_signed, limit, isneg, input, expected_value, comment)
		testcount = testcount + 1
		if is_signed and string.byte(input, 1) == HYPHEN_BYTE then
			-- also test explicit negative number parsing,
			-- e.g. "123" (without leading "-") == "-123"
			isneg = true
			input = string.sub(input, 2) -- remove "-" from input, e.g. "-123" => "123"
			test_one_good_input(nbits, base, is_signed, limit, isneg, input,
			                    expected_value, comment)
			testcount = testcount + 1
		end
	end
	return testcount
end

function test_one_bad_input(nbits, base, is_signed, limit, isneg, input, comment, expect_err)
	local value, err = dew.intscan(input, base, limit, isneg)
	if verbose then print_intscan_invocation(input, base, limit, isneg, comment, value, err) end

	if err ~= expect_err then
		error(fmt("intscan('%s', %d, 0x%x, isneg=%s) unexpected error:\n" ..
		          "  expected: %s (%s)" ..
		          "  actual:   %s (%s)",
		          input, base, limit, isneg and "true" or "false",
		          dew.errstr(expect_err),
		          dew.errstr(err)))
	end
end

function test_bad_input(filename, expect_err)
	local iter, columns = csv.read(filename)
	local testcount = 0
	for row in iter do
		local nbits, base, is_signed, limit, input, comment = unpack_csv_row(row)
		local isneg = false
		test_one_bad_input(nbits, base, is_signed, limit, isneg, input, comment, expect_err)
		testcount = testcount + 1
		if is_signed and string.byte(input, 1) == HYPHEN_BYTE then
			-- also test explicit negative number parsing,
			-- e.g. "123" (without leading "-") == "-123"
			isneg = true
			input = string.sub(input, 2) -- remove "-" from input, e.g. "-123" => "123"
			test_one_bad_input(nbits, base, is_signed, limit, isneg, input, comment, expect_err)
			testcount = testcount + 1
		end
	end
	return testcount
end

return function(options)
	verbose = options.verbose == true
	local testcount = 0
	testcount = testcount + test_good_input()
	testcount = testcount + test_bad_input("intscan_test_data_err_input.csv", dew.ERR_INPUT)
	testcount = testcount + test_bad_input("intscan_test_data_err_range.csv", dew.ERR_RANGE)
	printf("intscan_test: OK (%d test cases)", testcount)
end
