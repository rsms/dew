function function_name(f)
    local name = debug.getinfo(f, "n").name
    if name then
        return name
    end
    return "[" .. tostring(f) .. "]"
end

function expect_error(error_substr, f, ...)
    local ok, err = pcall(f, ...)
    -- print("pcall =>", ok, err)
    if ok then
        error("call to " .. function_name(f) .. " succeeded; expected it to fail")
    end
    if string.find(err, error_substr, 1, true) == nil then
        error("unexpected error from call to " .. function_name(f) .. ":" ..
              "\n\tExpected substring \"" .. error_substr .. "\" not found in error:" ..
              "\n\t\"" .. tostring(err) .. "\"")
    end
end
