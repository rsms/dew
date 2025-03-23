// Convert integer value between different bit widths and signedness
#pragma once
#include "../dew.h"
API_BEGIN

// fun intconv(value i64, src_bits, dst_bits uint, src_issigned, dst_issigned bool) int
int l_intconv(lua_State* L);

API_END
