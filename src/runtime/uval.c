#include "uval.h"

void* nullable uval_new(lua_State* L, enum UValType type, usize size, int nuvalue) {
    // lua_newuserdatauv creates and pushes on the stack a new full userdata,
    // with nuvalue associated Lua values, called user values, plus an associated block of
    // raw memory with size bytes. (The user values can be set and read with the functions
    // lua_setiuservalue and lua_getiuservalue.)
    //
    // Lua ensures that the returned address is valid as long as the corresponding userdata
    // is alive. Moreover, if the userdata is marked for finalization, its address is valid
    // at least until the call to its finalizer.
    //
    // Throws LUA_ERRMEM if memory allocation fails
    UVal* uval = lua_newuserdatauv(L, size, nuvalue);
    if (uval)
        uval->type = type;
    return uval;
}

void* nullable uval_check(lua_State* L, int idx, enum UValType type, const char* expectname) {
    UVal* uval = lua_touserdata(L, idx);
    if LIKELY (uval && uval->type == type)
        return uval;
    luaL_typeerror(L, idx, expectname);
    return NULL;
}

const char* uval_typename(lua_State* L, int idx) {
    if (!lua_getmetatable(L, idx))
        return "[object]";
    lua_getfield(L, -1, "__name");
    const char* name = lua_tostring(L, -1);
    lua_pop(L, 2); // pop name and metatable
    return name;
}
