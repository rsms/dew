-- Supplementary runtime functions implemented in lua.
-- Note that most runtime functions are implemented in C.

-- sintconv converts between integer types
function __rt.sintconv(v, nbits)
    -- mask
    local r = v & ((1 << nbits) - 1)
    -- sign extend if sign bit is set
    return (v & (1 << (nbits - 1)) ~= 0) and r - (1 << nbits) or r
end

-- f_trunc_{s,u} converts a floating-point number to an integer by truncation
function __rt.f_trunc_s(v, nbits)
    return __rt.sintconv(math.modf(v), nbits)
end
function __rt.f_trunc_u(v, nbits)
    return math.modf(v) & ((1 << nbits) - 1)
end
