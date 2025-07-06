local fmt = string.format

-- Local Lua implementation of __rt.intconv
function intconv_lua(srcval, src_bits, dst_bits, src_issigned, dst_issigned)
    -- Ensure value fits in the source bit size
    if src_bits < 64 then
        local src_mask = (1 << src_bits) - 1
        srcval = srcval & src_mask
    end

    -- If source is signed and the value has the sign bit set, sign extend it
    if src_issigned and (srcval & (1 << (src_bits - 1))) ~= 0 then
        srcval = srcval - (1 << src_bits)
    end

    -- Handle destination conversions
    local dst_mask = dst_bits < 64 and ((1 << dst_bits) - 1) or 0xffffffffffffffff

    if not dst_issigned then
        -- For unsigned destination, truncate to fit the destination size
        return srcval & dst_mask
    end
    -- For signed destination, truncate and sign extend if necessary
    srcval = srcval & dst_mask
    if (srcval & (1 << (dst_bits - 1))) ~= 0 then
        srcval = srcval - (1 << dst_bits)
    end
    return srcval
end

local expecttab = {
    ['s64']     =           -9876543290, --   0xfffffffdb34fe8c6
    ['s64_u64'] =    0xfffffffdb34fe8c6, -- 18446744063833008326
    ['s64_u32'] =            3008358598, --           0xb34fe8c6
    ['s64_u16'] =                 59590, --               0xe8c6
    ['s64_u8']  =                   198, --                 0xc6
    ['s64_s32'] =           -1286608698, --           0xb34fe8c6
    ['s64_s16'] =                 -5946, --               0xe8c6
    ['s64_s8']  =                   -58, --                 0xc6
    ['u64_u32'] =            3008358598, --           0xb34fe8c6
    ['u64_u16'] =                 59590, --               0xe8c6
    ['u64_u8']  =                   198, --                 0xc6
    ['u64_s64'] =           -9876543290, --   0xfffffffdb34fe8c6
    ['u64_s32'] =           -1286608698, --           0xb34fe8c6
    ['u64_s16'] =                 -5946, --               0xe8c6
    ['u64_s8']  =                   -58, --                 0xc6
    ['s32_u64'] =    0xffffffffb34fe8c6, -- 18446744072422942918
    ['s32_u32'] =            3008358598, --           0xb34fe8c6
    ['s32_u16'] =                 59590, --               0xe8c6
    ['s32_u8']  =                   198, --                 0xc6
    ['s32_s64'] =           -1286608698, --   0xffffffffb34fe8c6
    ['s32_s16'] =                 -5946, --               0xe8c6
    ['s32_s8']  =                   -58, --                 0xc6
    ['u32_u64'] =            3008358598, --           0xb34fe8c6
    ['u32_u16'] =                 59590, --               0xe8c6
    ['u32_u8']  =                   198, --                 0xc6
    ['u32_s64'] =            3008358598, --           0xb34fe8c6
    ['u32_s32'] =           -1286608698, --           0xb34fe8c6
    ['u32_s16'] =                 -5946, --               0xe8c6
    ['u32_s8']  =                   -58, --                 0xc6
    ['s16_u64'] =    0xffffffffffffe8c6, -- 18446744073709545670
    ['s16_u32'] =            4294961350, --           0xffffe8c6
    ['s16_u16'] =                 59590, --               0xe8c6
    ['s16_u8']  =                   198, --                 0xc6
    ['s16_s64'] =                 -5946, --   0xffffffffffffe8c6
    ['s16_s32'] =                 -5946, --           0xffffe8c6
    ['s16_s8']  =                   -58, --                 0xc6
    ['u16_u64'] =                 59590, --               0xe8c6
    ['u16_u32'] =                 59590, --               0xe8c6
    ['u16_u8']  =                   198, --                 0xc6
    ['u16_s64'] =                 59590, --               0xe8c6
    ['u16_s32'] =                 59590, --               0xe8c6
    ['u16_s16'] =                 -5946, --               0xe8c6
    ['u16_s8']  =                   -58, --               0xffc6
    ['s8_u64']  =    0xffffffffffffffc6, -- 18446744073709551558
    ['s8_u32']  =            4294967238, --           0xffffffc6
    ['s8_u16']  =                 65478, --               0xffc6
    ['s8_u8']   =                   198, --                 0xc6
    ['s8_s64']  =                   -58, --   0xffffffffffffffc6
    ['s8_s32']  =                   -58, --           0xffffffc6
    ['s8_s16']  =                   -58, --               0xffc6
    ['u8_u64']  =                   198, --                 0xc6
    ['u8_u32']  =                   198, --                 0xc6
    ['u8_u16']  =                   198, --                 0xc6
    ['u8_s64']  =                   198, --                 0xc6
    ['u8_s32']  =                   198, --                 0xc6
    ['u8_s16']  =                   198, --                 0xc6
    ['u8_s8']   =                   -58, --                 0xc6

    -- non-pow2 cases
    ['s64_s63'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u63'] =   9223372026978232518, --      7ffffffdb34fe8c6
    ['s64_s62'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u62'] =   4611686008550844614, --      3ffffffdb34fe8c6
    ['s64_s61'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u61'] =   2305842999337150662, --      1ffffffdb34fe8c6
    ['s64_s60'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u60'] =   1152921494730303686, --       ffffffdb34fe8c6
    ['s64_s59'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u59'] =    576460742426880198, --       7fffffdb34fe8c6
    ['s64_s58'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u58'] =    288230366275168454, --       3fffffdb34fe8c6
    ['s64_s57'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u57'] =    144115178199312582, --       1fffffdb34fe8c6
    ['s64_s56'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u56'] =     72057584161384646, --        fffffdb34fe8c6
    ['s64_s55'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u55'] =     36028787142420678, --        7ffffdb34fe8c6
    ['s64_s54'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u54'] =     18014388632938694, --        3ffffdb34fe8c6
    ['s64_s53'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u53'] =      9007189378197702, --        1ffffdb34fe8c6
    ['s64_s52'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u52'] =      4503589750827206, --         ffffdb34fe8c6
    ['s64_s51'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u51'] =      2251789937141958, --         7fffdb34fe8c6
    ['s64_s50'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u50'] =      1125890030299334, --         3fffdb34fe8c6
    ['s64_s49'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u49'] =       562940076878022, --         1fffdb34fe8c6
    ['s64_s48'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u48'] =       281465100167366, --          fffdb34fe8c6
    ['s64_s47'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u47'] =       140727611812038, --          7ffdb34fe8c6
    ['s64_s46'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u46'] =        70358867634374, --          3ffdb34fe8c6
    ['s64_s45'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u45'] =        35174495545542, --          1ffdb34fe8c6
    ['s64_s44'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u44'] =        17582309501126, --           ffdb34fe8c6
    ['s64_s43'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u43'] =         8786216478918, --           7fdb34fe8c6
    ['s64_s42'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u42'] =         4388169967814, --           3fdb34fe8c6
    ['s64_s41'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u41'] =         2189146712262, --           1fdb34fe8c6
    ['s64_s40'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u40'] =         1089635084486, --            fdb34fe8c6
    ['s64_s39'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u39'] =          539879270598, --            7db34fe8c6
    ['s64_s38'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u38'] =          265001363654, --            3db34fe8c6
    ['s64_s37'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u37'] =          127562410182, --            1db34fe8c6
    ['s64_s36'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u36'] =           58842933446, --             db34fe8c6
    ['s64_s35'] =           -9876543290, --      fffffffdb34fe8c6
    ['s64_u35'] =           24483195078, --             5b34fe8c6
    ['s64_s34'] =            7303325894, --             1b34fe8c6
    ['s64_u34'] =            7303325894, --             1b34fe8c6
    ['s64_s33'] =           -1286608698, --      ffffffffb34fe8c6
    ['s64_u33'] =            7303325894, --             1b34fe8c6
    ['s64_s31'] =             860874950, --              334fe8c6
    ['s64_u31'] =             860874950, --              334fe8c6
    ['s64_s30'] =            -212866874, --      fffffffff34fe8c6
    ['s64_u30'] =             860874950, --              334fe8c6
    ['s64_s29'] =            -212866874, --      fffffffff34fe8c6
    ['s64_u29'] =             324004038, --              134fe8c6
    ['s64_s28'] =              55568582, --               34fe8c6
    ['s64_u28'] =              55568582, --               34fe8c6
    ['s64_s27'] =              55568582, --               34fe8c6
    ['s64_u27'] =              55568582, --               34fe8c6
    ['s64_s26'] =             -11540282, --      ffffffffff4fe8c6
    ['s64_u26'] =              55568582, --               34fe8c6
    ['s64_s25'] =             -11540282, --      ffffffffff4fe8c6
    ['s64_u25'] =              22014150, --               14fe8c6
    ['s64_s24'] =               5236934, --                4fe8c6
    ['s64_u24'] =               5236934, --                4fe8c6
    ['s64_s23'] =              -3151674, --      ffffffffffcfe8c6
    ['s64_u23'] =               5236934, --                4fe8c6
    ['s64_s22'] =               1042630, --                 fe8c6
    ['s64_u22'] =               1042630, --                 fe8c6
    ['s64_s21'] =               1042630, --                 fe8c6
    ['s64_u21'] =               1042630, --                 fe8c6
    ['s64_s20'] =                 -5946, --      ffffffffffffe8c6
    ['s64_u20'] =               1042630, --                 fe8c6
    ['s64_s19'] =                 -5946, --      ffffffffffffe8c6
    ['s64_u19'] =                518342, --                 7e8c6
    ['s64_s18'] =                 -5946, --      ffffffffffffe8c6
    ['s64_u18'] =                256198, --                 3e8c6
    ['s64_s17'] =                 -5946, --      ffffffffffffe8c6
    ['s64_u17'] =                125126, --                 1e8c6
    ['s64_s15'] =                 -5946, --      ffffffffffffe8c6
    ['s64_u15'] =                 26822, --                  68c6
    ['s64_s14'] =                 -5946, --      ffffffffffffe8c6
    ['s64_u14'] =                 10438, --                  28c6
    ['s64_s13'] =                  2246, --                   8c6
    ['s64_u13'] =                  2246, --                   8c6
    ['s64_s12'] =                 -1850, --      fffffffffffff8c6
    ['s64_u12'] =                  2246, --                   8c6
    ['s64_s11'] =                   198, --                    c6
    ['s64_u11'] =                   198, --                    c6
    ['s64_s10'] =                   198, --                    c6
    ['s64_u10'] =                   198, --                    c6
    ['s64_s9'] =                   198, --                    c6
    ['s64_u9'] =                   198, --                    c6
    ['s64_s7'] =                   -58, --      ffffffffffffffc6
    ['s64_u7'] =                    70, --                    46
    ['s64_s6'] =                     6, --                     6
    ['s64_u6'] =                     6, --                     6
    ['s64_s5'] =                     6, --                     6
    ['s64_u5'] =                     6, --                     6
    ['s64_s4'] =                     6, --                     6
    ['s64_u4'] =                     6, --                     6
    ['s64_s3'] =                    -2, --      fffffffffffffffe
    ['s64_u3'] =                     6, --                     6
    ['s64_s2'] =                    -2, --      fffffffffffffffe
    ['s64_u2'] =                     2, --                     2
}

function t(srcval, src_sign, src_bits, dst_sign, dst_bits)
    local src_issigned = src_sign == 's'
    local dst_issigned = dst_sign == 's'
    assert(math.type(srcval) == 'integer')

    -- local dstval = intconv_lua(srcval, src_bits, dst_bits, src_issigned, dst_issigned)
    local dstval = __rt.intconv(srcval, src_bits, dst_bits, src_issigned, dst_issigned)

    assert(math.type(dstval) == 'integer',
           fmt("(%s, %s, %s, %s, %s) => %s",
               tostring(srcval), tostring(src_bits), tostring(dst_bits),
               tostring(src_issigned), tostring(dst_issigned),
               tostring(dstval)))

    local key = fmt("%s%d_%s%d", src_sign, src_bits, dst_sign, dst_bits)
    local expected = expecttab[key]
    if expected == nil then
        -- missing!
        -- printf("MISSING expecttab[%s]", key)
        assert(dstval <= 9223372036854775807)
        printf("\t['%s'] = %21d, -- %21x", key, dstval, dstval)
        expected = dstval
    else
        -- assert(expected ~= nil, key)
        assert(math.type(expected) == 'integer',
               "math.type(expecttab[" .. key .. "] = " .. expected ..
               ") => " .. math.type(expected))
    end
    -- print(src_sign, src_bits, srcval, srcval,
    --       dst_sign, dst_bits, dstval, dstval,
    --       dst_sign, dst_bits, expected, expected)
    srcval = srcval & ((1 << src_bits) - 1)
    if test_verbose or dstval ~= expected then
        printf(
            "   %s%-2d %16x %21"..src_sign.."\n" ..
            "-> %s%-2d %16x %21"..dst_sign.."\n" ..
            "ex %s%-2d %16x %21"..dst_sign.."\n" ..
            "————————————————————————————————————————————————————————",
            src_sign, src_bits, srcval, srcval,
            dst_sign, dst_bits, dstval, dstval,
            dst_sign, dst_bits, expected, expected)
    end
    if dstval ~= expected then
        error("unexpected: ")
    end
    return dstval
end


local initval = expecttab["s64"]
local S, D = 0, 0

-- fun intconv(value i64, src_bits, dst_bits uint, src_issigned, dst_issigned bool) int
-- t(srcval, src_sign, src_bits, dst_sign, dst_bits)

local valtab = {
    ["s64"] = initval,
    ["u64"] = t(initval, "s",64, "u",64),
}
local testcount = 2

for S = 63, 2, -1 do
    valtab["s" .. tostring(S)] = t(initval, "s",64, "s",S)
    valtab["u" .. tostring(S)] = t(initval, "s",64, "u",S)
    testcount = testcount + 2
end

if test_verbose then print('————————————————————————————————————————————————————————') end

S = 64
while S >= 8 do
    local srcval_s = valtab["s" .. S]
    local srcval_u = valtab["u" .. S]
    D = 64
    while D >= 8 do
        if S ~= D then
            t(srcval_s, "s",S, "s",D)
            t(srcval_u, "u",S, "u",D)
            testcount = testcount + 2
        end
        t(srcval_s, "s",S, "u",D)
        t(srcval_u, "u",S, "s",D)
        testcount = testcount + 2
        D = D // 2
    end
    S = S // 2
end

return fmt("%d test cases", testcount)
