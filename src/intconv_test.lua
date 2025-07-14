local samples = { -- { src_type, src_val } -> { dst_typ, expected_val }
    -- reinterpret
    { { "uint8", 123 }, { "int8",  123 } }, -- u -> s, no sign bit
    { { "int8",  123 }, { "uint8", 123 } }, -- s -> u, no sign bit
    { { "int8",  -33 }, { "uint8", 223 } }, -- s -> u, with sign bit
    { { "uint8", 223 }, { "int8",  -33 } }, -- u -> s, with sign bit

    -- truncate
    { { "uint16", 10987 }, { "int8",  -21 } }, -- u -> s
    { { "uint16", 12345 }, { "int8",   57 } }, -- u -> s
    { { "int16",  -1234 }, { "uint8",  46 } }, -- s -> u
    { { "int16",   1234 }, { "uint8", 210 } }, -- s -> u
    { { "int16",   1234 }, { "int8",  -46 } }, -- s -> s
    { { "uint16", 39030 }, { "uint8", 118 } }, -- u -> u

    -- extend
    { { "uint8", 210 }, { "int16",    210 } }, -- u -> s (trivial)
    { { "uint8", 210 }, { "uint16",   210 } }, -- u -> u (trivial)
    { { "int8",  -46 }, { "int16",    -46 } }, -- s -> s (trivial)
    { { "int8",  -46 }, { "uint16", 65490 } }, -- s -> u

    -- truncate float -> int
    { { "float", 1234.9 }, { "int8",  -46 } },
    { { "float", 1234.9 }, { "uint8", 210 } },
    { { "float",  123.9 }, { "int8",  123 } },
    { { "float",  234.9 }, { "uint8", 234 } },
    { { "float", -123.9 }, { "int8", -123 } },
    { { "float", -234.9 }, { "uint8",  22 } },
}
for _, sample in ipairs(samples) do
    local src_typ, src_val = sample[1][1], sample[1][2]
    local dst_typ, dst_val = sample[2][1], sample[2][2]

    local src_is_signed = src_typ:sub(1,1) ~= "u"
    local dst_is_signed = dst_typ:sub(1,1) ~= "u"

    local src_nbits =
        (src_typ == "float") and 64 or tonumber(src_typ:sub(src_is_signed and 4 or 5))>>0
    local dst_nbits =
        (dst_typ == "float") and 64 or tonumber(dst_typ:sub(dst_is_signed and 4 or 5))>>0

    -- expect all test cases to be conversions
    -- dlog("test_intcast> %s %s -> %s %s",
    --      src_typ, tostring(src_val), dst_typ, tostring(dst_val))
    assert(src_typ ~= dst_typ, "unexpected test case: same type: " .. dst_typ)
    local res_val = 0

    --
    -- BEGIN main logic --
    --
    -- This should match codegen.intcast
    --
    if src_typ ~= "float" and dst_typ ~= "float" then
        -- int -> int conversion
        if src_nbits < dst_nbits and (not src_is_signed or dst_is_signed) then
            -- trivial extension, e.g. u16(u8(3))=>3 or i16(u8(3))=>3 or i16(i8(-3))=>-3
            res_val = src_val
        elseif dst_is_signed then
            -- mask & sign extend if needed (depends on value, so done at runtime)
            res_val = __rt.sintconv(src_val, dst_nbits)
        else
            -- simple truncation
            res_val = src_val & ((1 << dst_nbits) - 1)
        end
    elseif src_typ ~= "float" then
        -- int -> float conversion
        error("TODO int -> float conversion")
    else
        -- float -> int conversion
        if dst_is_signed then
            res_val = __rt.f_trunc_s(src_val, dst_nbits)
        else
            res_val = __rt.f_trunc_u(src_val, dst_nbits)
        end
    end
    --
    -- END main logic --
    --

    if res_val == dst_val then
        if test_verbose then
            dlog("OK %s(%s(%s)) => %s", src_typ, dst_typ, tostring(src_val), tostring(res_val))
        end
    else
        FAIL("FAIL %s(%s(%s)) =>\n  Got:  %10s\n  Want: %10s",
             src_typ, dst_typ, tostring(src_val), tostring(res_val), tostring(dst_val) )
    end
end
