#pragma once
#include "../dew.h"
API_BEGIN

void* nullable l_uobj_check(lua_State* L, int idx, const void* key, const char* tname);
int l_errno_error(lua_State* L, int err_no); // note: always returns 0
bool x_luaL_optboolean(lua_State* L, int index, bool default_value);

API_END
