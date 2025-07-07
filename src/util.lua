fmt = string.format
numtype = math.type -- "integer" or "float"

function noop() end
function printf(...) print(fmt(...)) end
dlog = printf

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
    -- print(indent_str .. "{")
    local keys = {}
    for k in pairs(tbl) do table.insert(keys, tostring(k)) end
    table.sort(keys)
    for _, k in ipairs(keys) do
        local key = tostring(k)
        local v = tbl[k]
        if v == nil then
            io.write(indent_str .. indent_chunk)
            v = k
        else
            io.write(indent_str .. indent_chunk .. "[" .. key .. "] = ")
        end
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

function trim_string(s)
    return s:gsub("^%s*(.-)%s*$", "%1")
end

function string_starts_with(s, prefix)
    return s:sub(1, #prefix) == prefix
end

function format_monotime_duration(monotime_duration) --> string
    -- note: assumes monotime_duration is in same units as __rt.monotime(), which is nanoseconds
    if monotime_duration < 1000 then return fmt("%d ns", monotime_duration) end
    if monotime_duration < 1000000 then return fmt("%d us", monotime_duration // 1000) end
    if monotime_duration < 1000000000 then return fmt("%.2f ms", monotime_duration / 1000000.0) end
    return fmt("%.2f s", monotime_duration / 1000000000.0)
end

function memory_usage()
    return collectgarbage("count") -- Returns memory usage in kilobytes
end

B = string.byte
