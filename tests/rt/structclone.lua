require("../../src/util") -- print_table

-- fun structclone_encode(flags uint, transfer_list [any], value ...any) Buf

__rt.main(function()
    local buf

    buf = __rt.structclone_encode(0, nil, 1, 2, 3)
    print("structclone_encode =>\n  (" .. #buf .. " B) \"" .. tostring(buf) .. '"')
    local buf2 = __rt.structclone_encode(0, nil, 1, 2, 3)
    assert(buf == buf2)

    -- -- local buf = __rt.structclone_encode(nil, true, 1, 2.3, "four", "long string")
    -- -- local buf = __rt.structclone_encode(nil, true, 1, 2.3, "four", "long string", {5, "six"})

    -- -- local a1 = {"a1", 2, 3, 4}
    -- -- local buf = __rt.structclone_encode(a1)

    -- local buf1 = __rt.structclone_encode(0, nil, "hello!")

    -- -- local a1 = {"A", "B", {22, 222}, 3}
    -- local long_string1 = "really long string that will be referenced 1"
    -- local long_string2 = "really long string that will be referenced 2"
    -- local a1 = {"A", "B", long_string1, long_string1, long_string2, long_string2}
    -- a1[#a1 + 1] = a1
    -- a1[#a1 + 1] = {x=1, y=3, z=4, [long_string1]=5}
    -- -- local function mul(x, y)
    -- --     local outer = a1 -- will become nil
    -- --     return x * y
    -- -- end
    -- -- a1[#a1 + 1] = mul
    -- -- a1[#a1 + 1] = buf1
    -- -- a1[#a1 + 1] = "really long string that will not be referenced"
    -- local buf = __rt.structclone_encode(0, nil, buf1, 31)
    -- -- local buf = __rt.structclone_encode(0, a1, long_string1, buf1)
    -- -- local buf = __rt.structclone_encode(a1, 9, a1)

    -- -- local a1 = {"a1"}
    -- -- local a2 = {"a2"}
    -- -- local buf = __rt.structclone_encode(a1, a2, a1)

    -- -- local a1 = {"a1"}
    -- -- local a2 = {"a2", a1, 3, 4}
    -- -- a1[#a1 + 1] = a2
    -- -- local buf = __rt.structclone_encode(a1)

    -- -- local buf = __rt.structclone_encode({5, "six", {7}})
    -- -- local buf = __rt.structclone_encode({x = 5, y = "six"})
    -- -- local buf = __rt.structclone_encode(1, 2.3, "four", {5, "six"})
    -- print("structclone_encode =>\n  (" .. #buf .. " B) \"" .. tostring(buf) .. '"')
    -- -- print("structclone_decode =>", __rt.structclone_decode(buf))
    -- local res = table.pack(__rt.structclone_decode(buf))
    -- print("structclone_decode =>", res)

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
    -- -- print("worker:", __rt.typename(__rt.spawn_worker(function() end)))
    -- -- print("task:", __rt.typename(__rt.spawn_task(function() end)))
    -- -- t[#t + 1] = 4
    -- -- print_table(t)

    -- -- print("structclone_encode =>",
    -- --       __rt.structclone_encode(function() return 1, 2.3, "four", {5, "six"} end))
    -- --       -- __rt.structclone_encode())
    -- --       -- __rt.structclone_encode(1, 2, 3))
    -- --       -- __rt.structclone_encode(1, 2.3, "four", {5, "six"}))
    -- print("DONE")
    -- -- local f = __rt.structclone_encode(1, 2.3, "four", {5, "six"})
    -- -- print("structclone_encode =>", f)
    -- -- print("() =>", f())
end)
