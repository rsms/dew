#include "lutil.h"


int l_errno_error(lua_State* L, int err_no) { // note: always returns 0
    lua_pushstring(L, strerror(err_no));
    return lua_error(L);
}


bool x_luaL_optboolean(lua_State* L, int index, bool default_value) {
    if (lua_isnoneornil(L, index))
        return default_value;
    return lua_toboolean(L, index);
}
