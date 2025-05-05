-- fun structclone_encode(flags uint, transfer_list [any], value ...any) Buf
-- fun structclone_decode(Buf encoded) ...any

__rt.main(function()
    local buf, buf2, res

    -- basic types
    buf = __rt.structclone_encode(0, nil, nil, true, 1, 2.3, "five")
    res = table.pack(__rt.structclone_decode(buf))
    assert(#res == 5)
    assert(res[1] == nil)
    assert(res[2] == true)
    assert(res[3] == 1)
    assert(res[4] == 2.3)
    assert(res[5] == "five")

    -- encoding the same values yields the same bytes
    buf2 = __rt.structclone_encode(0, nil, nil, true, 1, 2.3, "five")
    -- print("structclone_encode =>\n  (" .. #buf .. " B) \"" .. tostring(buf) .. '"')
    assert(buf ~= buf2)
    assert(buf:equal(buf2))

    -- long strings are interned & referenced
    buf = __rt.structclone_encode(0, nil, "really long string that will be referenced")
    buf2 = __rt.structclone_encode(0, nil,
                                  "really long string that will be referenced",
                                  "really long string that will be referenced")
    -- there's no better way to test that it works than this:
    assert(#buf2 - #buf < #"really long string that will be referenced")
    res = table.pack(__rt.structclone_decode(buf2))
    assert(#res == 2)
    assert(rawequal(res[1], res[2]))

    -- tables (specialized encoding for array vs dicts)
    -- array
    buf = __rt.structclone_encode(0, nil, {1, "B", "C"})
    res = __rt.structclone_decode(buf)
    assert(__rt.typename(res) == "array")
    assert(#res == 3)
    assert(res[1] == 1)
    assert(res[2] == "B")
    assert(res[3] == "C")
    -- dict
    buf = __rt.structclone_encode(0, nil, {One=1, Two="B", Three="C"})
    res = __rt.structclone_decode(buf)
    assert(__rt.typename(res) == "dict")
    assert(#res == 0)
    assert(res["One"] == 1)
    assert(res["Two"] == "B")
    assert(res["Three"] == "C")

    -- tables are interned
    local a = {1, "B", "C"}
    buf = __rt.structclone_encode(0, nil, a)
    buf2 = __rt.structclone_encode(0, nil, a, a, a, a, a)
    assert(#buf2 < #buf*3) -- compact encoding with internal references
    res = table.pack(__rt.structclone_decode(buf2))
    -- decoding yields references to the "same" (as in data in memory) values
    assert(#res == 5)
    assert(res[2] == res[1])
    assert(res[3] == res[1])
    assert(res[4] == res[1])
    assert(res[5] == res[1])

    -- tables may be cyclical/recursive, referencing themselves
    local a = {}
    a[#a + 1] = a -- add itself as one of its elements
    buf = __rt.structclone_encode(0, nil, a)
    res = __rt.structclone_decode(buf)
    assert(#res == 1)
    assert(res[1] == res)

    -- Dew Buf objects can be cloned. Repeated occurrances are interned, like tables & strings.
    -- Decoded as a buffer with cap==len
    buf2 = buf
    buf = __rt.structclone_encode(0, nil, buf2)
    buf = __rt.structclone_decode(buf)
    assert(buf ~= buf2)
    assert(buf:equal(buf2))

    -- We can even encode functions
    buf = __rt.structclone_encode(0, nil, function (a, b, c)
        return a * b * c
    end)
    local f = __rt.structclone_decode(buf)
    assert(type(f) == "function")
    res = table.pack(f(2, 3, 4))
    assert(#res == 1)
    assert(res[1] == 24)

    -- upvalues (closure vars) of functions are copied
    local upvalue1 = 123
    buf = __rt.structclone_encode(0, nil, function (a, b, c)
        return a * b * c, upvalue1
    end)
    upvalue1 = 456
    f = __rt.structclone_decode(buf)
    assert(type(f) == "function")
    res = table.pack(f(2, 3, 4))
    assert(#res == 2)
    assert(res[1] == 24)
    assert(res[2] == 123) -- upvalue was copied (not referencing the outer local)

    -- functions may be cyclical/recursive, referencing themselves
    local function f1()
        return f1
    end
    buf = __rt.structclone_encode(0, nil, f1)
    f = __rt.structclone_decode(buf)
    assert(type(f) == "function")
    res = table.pack(f())
    assert(#res == 1)
    assert(res[1] == f)

    -- -- Snippet for printing results. Needs require("../../src/util") for print_table
    -- local res = table.pack(__rt.structclone_decode(buf))
    -- local seen = {}
    -- for i, item in ipairs(res) do
    --     if type(item) == "table" then
    --         local eq = seen[item]
    --         if eq ~= nil then
    --             print("#" .. i .. " =", "#" .. eq)
    --         else
    --             seen[item] = i
    --             print("#" .. i .. " =")
    --             print_table(item, 1)
    --         end
    --     else
    --         print("#" .. i .. " =", __rt.typename(item), item)
    --     end
    -- end
end)
