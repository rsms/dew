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

__rt.main(function()
    -- local buf = __rt.xxx_structclone_encode(nil, true, 1, 2.3, "four", "long string")
    -- local buf = __rt.xxx_structclone_encode(nil, true, 1, 2.3, "four", "long string", {5, "six"})

    -- local a1 = {"a1", 2, 3, 4}
    -- local buf = __rt.xxx_structclone_encode(a1)

    -- local a1 = {"A", "B", {22, 222}, 3}
    local a1 = {"A", "B", 3}
    local buf = __rt.xxx_structclone_encode(a1)

    -- local a1 = {"a1"}
    -- local a2 = {"a2"}
    -- local buf = __rt.xxx_structclone_encode(a1, a2, a1)

    -- local a1 = {"a1"}
    -- local a2 = {"a2", a1, 3, 4}
    -- a1[#a1 + 1] = a2
    -- local buf = __rt.xxx_structclone_encode(a1)

    -- local buf = __rt.xxx_structclone_encode({5, "six", {7}})
    -- local buf = __rt.xxx_structclone_encode({x = 5, y = "six"})
    -- local buf = __rt.xxx_structclone_encode(1, 2.3, "four", {5, "six"})
    print("xxx_structclone_encode =>", buf)
    -- print("xxx_structclone_decode =>", __rt.xxx_structclone_decode(buf))
    print_table(__rt.xxx_structclone_decode(buf))

    -- print("xxx_structclone_encode =>",
    --       __rt.xxx_structclone_encode(function() return 1, 2.3, "four", {5, "six"} end))
    --       -- __rt.xxx_structclone_encode())
    --       -- __rt.xxx_structclone_encode(1, 2, 3))
    --       -- __rt.xxx_structclone_encode(1, 2.3, "four", {5, "six"}))
    print("DONE")
    -- local f = __rt.xxx_structclone_encode(1, 2.3, "four", {5, "six"})
    -- print("xxx_structclone_encode =>", f)
    -- print("() =>", f())

    -- local W1 = __rt.spawn_worker(function()
    --  -- yield: enable to make it so that when we call send(),
    --  -- the receiver has not yet called recv().
    --  --__rt.yield() -- give control back to main task

    --  local i = 1
    --  for i = 1, 8 do
    --      print("W1: recv ...")
    --      local typ, sender, msg1, msg2 = __rt.recv()
    --      print("W1: recv =>", typ, sender, msg1, msg2)
    --      assert(msg1 == i*10)
    --      assert(msg2 == i*100)
    --      i = i + 1
    --  end
    --  print("worker exiting")
    -- end)
    -- for i = 1, 8 do
    --  print("T1: send(W1, " .. (i*10) .. ") ...")
    --  print("T1: send(W1) =>", __rt.send(W1, i*10, i*100))
    -- end
    -- print("waiting for worker to exit")
    -- local ok, err = __rt.await(W1)
    -- print("worker exited:", ok, err)
    -- if ok == 0 then error(err) end
end)
