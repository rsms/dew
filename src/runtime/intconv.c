#include "intconv.h"


static i64 intconv(
    i64 value, int src_bits, int dst_bits, bool src_issigned, bool dst_issigned)
{
    // For 64-bit signed source values, preserve the original value before masking
    i64 original_value = value;

    // Ensure value fits in the source bit size
    if (src_bits < 64) {
        u64 src_mask = (1ULL << src_bits) - 1;
        value &= src_mask;
    }

    // If source is signed and the value has the sign bit set, sign extend it
    if (src_issigned && (value & (1ULL << (src_bits - 1)))) {
        // For 64-bit signed source, use the original value
        if (src_bits == 64) {
            value = original_value;
        } else {
            value -= (1ULL << src_bits);
        }
    }

    // Handle destination conversions
    if (dst_bits < 64) {
        u64 dst_mask = (1ULL << dst_bits) - 1;
        if (!dst_issigned) {
            // For unsigned destination, truncate to fit the destination size
            return value & dst_mask;
        }
        // For signed destination, truncate and sign extend if necessary
        value = value & dst_mask;
        if (value & (1ULL << (dst_bits - 1)))
            value -= (1ULL << dst_bits);
    }
    return value;
}


int l_intconv(lua_State* L) {
    i64 value = luaL_checkinteger(L, 1);
    i64 src_bits = luaL_checkinteger(L, 2);
    i64 dst_bits = luaL_checkinteger(L, 3);
    bool src_issigned = lua_toboolean(L, 4);
    bool dst_issigned = lua_toboolean(L, 5);

    luaL_argcheck(L, (src_bits > 0 && src_bits <= 64), 3,
                  "source bits must be between 1 and 64");
    luaL_argcheck(L, (dst_bits > 0 && dst_bits <= 64), 5,
                  "destination bits must be between 1 and 64");

    i64 result = intconv(value, src_bits, dst_bits, src_issigned, dst_issigned);

    lua_pushinteger(L, (lua_Integer)result);
    return 1;
}

