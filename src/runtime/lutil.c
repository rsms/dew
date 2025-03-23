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


void* nullable l_uobj_check(lua_State* L, int idx, const void* key, const char* tname) {
    void* p = lua_touserdata(L, idx);
    // get the metatable of the userdata
    if UNLIKELY(!p || !lua_getmetatable(L, idx))
        goto error;
    // push the cached metatable from the registry
    lua_rawgetp(L, LUA_REGISTRYINDEX, key);
    // compare the two metatables using pointer equality
    int match = lua_rawequal(L, -1, -2);
    lua_pop(L, 2); // remove metatables from the stack
    if (match)
        return p;
error:
    luaL_typeerror(L, idx, tname);
    return NULL;
}
