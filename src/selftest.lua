test_failures = 0
test_verbose = false
all_tests = {
    "tokenize_test",
    "parse_test",
    "intconv_test",
    "runtime/intscan_test",
    "runtime/intconv_test",
}

local function run_test(file)
    test_failures = 0

    function PASS(msg_format, ...)
        printf("\x1b[1;42m PASS \x1b[0m " .. msg_format, ...)
    end

    function FAIL(msg, level)
        if level == nil then level = 1 end
        local info = debug.getinfo(level + 1, "Snl")
        io.stderr:write(fmt("\x1b[1;41m %s FAIL \x1b[0m %s:%s\n" ..
                            "\x1b[1m%s\n" ..
                            "\x1b[0;2m",
                            info.short_src, file, info.currentline, msg))
        io.stderr:write(debug.traceback(nil, level + 1))
        io.stderr:write("\x1b[0m\n")
        test_failures = test_failures + 1
    end

    function WARN(msg_format, ...)
        io.stderr:write(fmt("\x1b[1;33m%s: warning:\x1b[0m ", file))
        io.stderr:write(fmt(msg_format, ...))
        io.stderr:write("\n")
    end

    printf("\x1b[2m%s START\x1b[0m", file)

    local timespent = __rt.monotime()
    local result = require(file)
    timespent = __rt.monotime() - timespent

    if test_failures > 0 then
        io.stdout:write("\n")
        os.exit(2)
    end

    result = (type(result) == "string") and " " .. result or ""
    printf("\x1b[1;32m%s PASS\x1b[0m%s (%s)", file, result, format_monotime_duration(timespent))
end

return function(filter, verbose)
    test_verbose = verbose
    local test_count = 0
    for _, file in ipairs(all_tests) do
        if #filter == 0 or string.match(file, filter) then
            test_count = test_count + 1
            run_test(file)
        end
    end
    if test_count > 0 then
        return
    end
    printf("[selftest] No tests matching filter \"%s\"", filter)
    print("[selftest] See lua.org/manual/5.4/manual.html#6.4.1 for pattern documentation.")
    print("[selftest] Available tests:")
    for _, file in ipairs(all_tests) do
        printf("[selftest]    %s", file)
    end
    os.exit(1)
end
