#include "intfmt.h"
#include "runtime.h" // ERR_


// intfmt formats an integer value as a string in a specified base.
// value (integer): The integer value to format. Can be signed or unsigned.
// base (integer): The base for the conversion. Must be in the range [2, 36].
// is_unsigned (boolean, optional): If true, treat the value as unsigned. Defaults to false.
//
// Return value:
//   A string representing the integer in the specified base.
//   If an error occurs, raises a Lua error.
int l_intfmt(lua_State* L) {
    int err = 0;

    // buf fits base-2 representations of all 64-bit numbers + NUL term
    char buf[66];
    char* str = &buf[sizeof(buf) - 1];
    *str = 0;

    // check arguments
    int nargs = lua_gettop(L);
    if (nargs < 2 || nargs > 3) {
        err = ERR_INVALID;
        goto end;
    }
    i64 value = luaL_checkinteger(L, 1);
    lua_Integer base = luaL_checkinteger(L, 2);
    if (base < 2 || base > 36) {
        err = ERR_RANGE;
        goto end;
    }
    int is_unsigned = (nargs > 2) ? lua_toboolean(L, 3) : 0;

    u64 uvalue = is_unsigned ? (u64)value : (u64)((value < 0) ? -(u64)value : value);
    do {
        // 0xdf (0b_1101_1111) normalizes the case of letters, i.e. 'A' => 'a'
        *--str = "0123456789abcdefghijklmnopqrstuvwxyz"[(uvalue % base) & 0xdf];
        uvalue /= base;
    } while (uvalue);

    if (!is_unsigned && value < 0)
        *--str = '-';

end:
    lua_pushstring(L, str);
    lua_pushinteger(L, err);
    return 2;
}
