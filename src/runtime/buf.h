// growable array usable via buf_* Lua functions
#pragma once
#include "../dew.h"
#include "uval.h"
API_BEGIN

typedef struct Buf {
    UVal         uval;
    usize        cap, len;
    u8* nullable bytes;
} Buf;

// buf_free frees buf->bytes, unless 'bytes' points to (void*)buf+sizeof(Buf),
// making it safe to call this function on plain buffers "in C" and those created by l_buf_createx.
int buf_free(Buf* buf);

// buf_reserve makes sure that there is at least minavail bytes available at bytes+len
void* nullable buf_reserve(Buf* buf, usize minavail);
bool buf_append(Buf* buf, const void* data, usize len);
int buf_append_luafun(Buf* buf, lua_State* L, bool strip_debuginfo);
bool buf_resize(Buf* buf, usize newcap);
inline static bool buf_append_byte(Buf* buf, u8 byte) {
    if (UNLIKELY(buf->cap == buf->len) && !buf_resize(buf, buf->cap + 1))
        return false;
    buf->bytes[buf->len++] = byte;
    return true;
}

// l_buf_createx allocates a buffer as a lua object (used by l_buf_create.)
// It puts a Lua buffer object on L's stack.
Buf* nullable l_buf_createx(lua_State* L, u64 cap);

int l_buf_gc(lua_State* L);
int l_buf_create(lua_State* L);
int l_buf_resize(lua_State* L);
int l_buf_str(lua_State* L);
Buf* nullable l_buf_check(lua_State* L, int idx);

void luaopen_buf(lua_State* L);

API_END
