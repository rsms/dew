#include "iopoll.h"
#include "lutil.h"
#include "runtime.h" // s_get_thread_local


static u8 g_iodesc_luatabkey; // IODesc object prototype


IODesc* nullable l_iodesc_check(lua_State* L, int idx) {
    return l_uobj_check(L, idx, &g_iodesc_luatabkey, "FD");
}


// l_iodesc_create allocates & pushes an IODesc object onto L's stack.
// Throws LUA_ERRMEM if memory allocation fails.
IODesc* l_iodesc_create(lua_State* L) {
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
    int nuvalue = 0;
    IODesc* d = lua_newuserdatauv(L, sizeof(IODesc), nuvalue);
    memset(d, 0, sizeof(*d));
    lua_rawgetp(L, LUA_REGISTRYINDEX, &g_iodesc_luatabkey);
    lua_setmetatable(L, -2);
    return d;
}


// l_iodesc_gc is called when an IODesc object is about to be garbage collected by Lua
static int l_iodesc_gc(lua_State* L) {
    IODesc* d = lua_touserdata(L, 1);
    S* s = s_get_thread_local();
    if (s) {
        int err = iopoll_close(&s->iopoll, d);
        if UNLIKELY(err)
            logerr("iopoll_close: %s", strerror(-err));
    } else {
        logerr("IODesc GC'd after main exited");
        close(d->fd);
    }
    return 0;
}


void luaopen_iopoll(lua_State* L) {
    luaL_newmetatable(L, "FD");
    lua_pushcfunction(L, l_iodesc_gc);
    lua_setfield(L, -2, "__gc");
    lua_rawsetp(L, LUA_REGISTRYINDEX, &g_iodesc_luatabkey);
}

