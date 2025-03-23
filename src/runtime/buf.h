// growable array usable via buf_* Lua functions
#pragma once
#include "../dew.h"
API_BEGIN

typedef struct Buf {
    usize        cap, len;
    u8* nullable bytes;
} Buf;

int buf_free(Buf* buf);

// buf_reserve makes sure that there is at least minavail bytes available at bytes+len
void* nullable buf_reserve(Buf* buf, usize minavail);

bool buf_append(Buf* buf, const void* data, usize len);
int buf_append_luafun(Buf* buf, lua_State* L, bool strip_debuginfo);

int l_buf_gc(lua_State* L);
int l_buf_create(lua_State* L);
int l_buf_resize(lua_State* L);
int l_buf_str(lua_State* L);
Buf* nullable l_buf_check(lua_State* L, int idx);

void luaopen_buf(lua_State* L);

API_END
