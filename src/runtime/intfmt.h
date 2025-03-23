#pragma once
#include "../dew.h"
API_BEGIN

// intfmt formats an integer value as a string in a specified base.
// value (integer): The integer value to format. Can be signed or unsigned.
// base (integer): The base for the conversion. Must be in the range [2, 36].
// is_unsigned (boolean, optional): If true, treat the value as unsigned. Defaults to false.
//
// Return value:
//   A string representing the integer in the specified base.
//   If an error occurs, raises a Lua error.
int l_intfmt(lua_State* L);

API_END
