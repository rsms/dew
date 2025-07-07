local function test_parse(unit) --> diag_errors, diag_messages
    local diag_errors = {}
    local diag_messages = {}
    unit.errcount = 0
    unit.include_comments = true
    unit.diag_handler = function(unit, kind, srcpos, msg_format, ...)
        diag_messages[#diag_messages + 1] = diag_fmt(unit, kind, srcpos, msg_format, ...)
        if kind == DIAG_ERR then
            diag_errors[#diag_errors + 1] = { srcpos = srcpos, msg = fmt(msg_format, ...) }
        end
    end
    -- print("—————————————————————————————\n" .. unit.src .. "\n—————————————————————————————")

    -- Tokenize (text -> tokens)
    tokens = tokenize_unit(unit)

    -- Note: Do not stop for errcount here since it may include "integer literal overflows"
    -- diagnostics which we want to check for

    -- Parse (tokens -> AST)
    parse_unit(unit, tokens, PARSE_SRCMAP)

    return diag_errors, diag_messages
end


local function test_parse_and_resolve(unit) --> diag_errors, diag_messages
    local diag_errors, diag_messages = test_parse(unit)

    -- Resolve (AST -> IR)
    if unit.errcount == 0 then
        resolve_unit(unit)
    end

    return diag_errors, diag_messages
end


local function print_diag_messages(diag_messages)
    for _, msg in ipairs(diag_messages) do
        io.stderr:write(msg .. "\n")
    end
end

local function collect_comments(unit, prefix_filter) --> [Comment]
    local comments = {}
    for idx in pairs(unit.commentmap) do
        for _, c in ipairs(unit.commentmap[idx]) do
            if prefix_filter == nil or c.value:sub(1, #prefix_filter) == prefix_filter then
                c.idx = idx
                comments[#comments + 1] = c
            end
        end
    end
    -- sort by srcpos
    table.sort(comments, function(a, b)
        -- return true when the first element must come before the second in the final order
        return srcpos_off(a.srcpos) < srcpos_off(b.srcpos)
    end)
    return comments
end


-- Before we run tests based on fixtures, make sure comment parsing works as expected.
-- Otherwise we can't trust other parse tests.
do
    unit = unit_create_buf("<stdin>",
        "// whole line\n" ..
        "a = 1 * 2 // end of line\n" ..
        "t = (1, // one\n" ..
        "     2, // two\n" ..
        "     3) // three\n" ..
        "a = 1 // cannot assign value of type\n")
    local expect_comments = {
        "// whole line",
        "// end of line",
        "// one",
        "// two",
        "// three",
        "// cannot assign value of type",
    }
    local diag_errors, diag_messages = test_parse(unit)
    local i = 1
    if unit.errcount > 0 then
        print_diag_messages(diag_messages)
        return FAIL("parsing of comment test")
    end
    -- verify that we got what we expect
    local actual_comments = collect_comments(unit)
    for i, c in ipairs(actual_comments) do
        if i <= #expect_comments and expect_comments[i] ~= c.value then
            local line, _ = srcpos_linecol(c.srcpos, unit.src)
            FAIL(fmt("unexpected comment [%d] at line %d:\n" ..
                     "  got      \"%s\"\n" ..
                     "  expected \"%s\"",
                     i, line, c.value, expect_comments[i]))
            break
        end
    end
    if #actual_comments > #expect_comments then
        return FAIL(fmt("got more comments (%d) than expected (%d)",
                        #actual_comments, #expect_comments))
    elseif #actual_comments < #expect_comments then
        return FAIL(fmt("got fewer comments (%d) than expected (%d)",
                        #actual_comments, #expect_comments))
    end
    if test_failures > 0 then
        return
    end
    if test_verbose then
        dlog("parse comments test: PASS")
    end
end


local function test_parse_and_resolve_fixture(filename)
    if test_verbose then
        dlog("parse & resolve %s", filename)
    end
    unit = unit_create_file(filename)
    local diag_errors, diag_messages = test_parse_and_resolve(unit)

    -- check if test is expected to succeed or fail by looking for "//!error" comments
    local diag_i = 1
    local err_comment_prefix = "//!error"
    local error_comments = collect_comments(unit, err_comment_prefix)
    for ci, c in ipairs(error_comments) do
        local line, col = srcpos_linecol(c.srcpos, unit.src)
        local findstr = trim_string(c.value:sub(#err_comment_prefix + 1))
        local diag = nil
        while diag_i <= #diag_errors do
            local d = diag_errors[diag_i]
            diag_i = diag_i + 1
            if d.msg:lower():match(findstr) then
                diag = d
                break
            end
            -- diagnostic does not match; log & keep searching
            local line, _ = srcpos_linecol(d.srcpos, unit.src)
            WARN("ignoring unexpected diagnostic error on line %d:\n  \"%s\"", line, d.msg)
        end
        if diag == nil then
            FAIL(fmt("No matching diagnostic for expected error: \"%s\"", findstr))
            printf("Got %d error(s):", #diag_errors)
            for i, d in ipairs(diag_errors) do
                print("  \"" .. d.msg .. "\"")
            end
            printf("Expected to find %d error match(es):", #error_comments)
            for i, c in ipairs(error_comments) do
                local findstr = trim_string(c.value:sub(#err_comment_prefix + 1))
                print("  \"" .. findstr .. "\"")
            end
            print_diag_messages(diag_messages)
            return
        end
    end

    if unit.errcount > 0 and #error_comments == 0 then
        print_diag_messages(diag_messages)
        return FAIL(fmt("parsing failed with %d error(s)", unit.errcount))
    elseif unit.errcount ~= #error_comments then
        print_diag_messages(diag_messages)
        return FAIL(fmt("expected %d error(s), got %d error(s)", #error_comments, unit.errcount))
    end
end


-- Run all tests found in the tests/parse directory
local fixture_dir = "tests/parse"
local fixture_files = __rt.fs_readdir(fixture_dir) ; table.sort(fixture_files)
local testcases_count = 1 -- 1 for the comments test above
for _, filename in ipairs(fixture_files) do
    if filename:sub(-4) == ".dew" then
        test_parse_and_resolve_fixture(fixture_dir .. "/" .. filename)
        testcases_count = testcases_count + 1
    end
end

return fmt("%d test cases", testcases_count)
