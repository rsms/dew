#include "buf.h"
#include "lutil.h"
#include "string_repr.h"


static u8 g_buf_luatabkey; // Buf object prototype


int buf_free(Buf* buf) {
    // if 'bytes' does not point into embedded memory, free it
    if (buf->bytes != (u8*)buf + sizeof(*buf))
        free(buf->bytes);
    return 0;
}


bool buf_resize(Buf* buf, usize newcap) {
    if (newcap > 0 && newcap < USIZE_MAX - sizeof(void*))
        newcap = ALIGN2(newcap, sizeof(void*));

    if (newcap == buf->cap)
        return true;

    // try to double current capacity
    usize cap2x;
    if (newcap > 0 && !check_mul_overflow(buf->cap, 2lu, &cap2x) && newcap <= cap2x)
        newcap = cap2x;
    // dlog("buf_resize %zu -> %zu", buf->cap, newcap);

    void* newbytes;
    if (buf->bytes == (u8*)buf + sizeof(*buf)) {
        // current buffer data is embedded
        if (newcap < buf->cap) {
            // can't shrink embedded buffer, however we update cap & len to uphold the promise
            // that after "buf_resize(b,N)", "buf_cap(b)==N && buf_len(b)<=N"
            buf->cap = newcap;
            if (newcap < buf->len)
                buf->len = newcap;
            return true;
        }
        // note: We can't check for shrunk embedded buffer since we don't know the initial cap
        if (( newbytes = malloc(newcap) ))
            memcpy(newbytes, buf->bytes, buf->len);
    } else if (newcap == 0) {
        // allow 'buf_resize(buf, 0)' to be used as an explicit way to release a buffer,
        // when relying on GC is not adequate
        free(buf->bytes);
        buf->bytes = NULL;
        buf->cap = 0;
        buf->len = 0;
        return true;
    } else {
        newbytes = realloc(buf->bytes, newcap);
    }
    if (!newbytes)
        return false;
    buf->bytes = newbytes;
    buf->cap = newcap;
    if (newcap < buf->len)
        buf->len = newcap;
    return true;
}


void* nullable buf_reserve(Buf* buf, usize minavail) {
    usize avail = buf->cap - buf->len;
    if UNLIKELY(avail < minavail) {
        usize newcap = buf->cap + (minavail - avail);
        if (!buf_resize(buf, newcap))
            return NULL;
    }
    return buf->bytes + buf->len;
}


bool buf_append(Buf* buf, const void* data, usize len) {
    usize avail = buf->cap - buf->len;
    if UNLIKELY(avail < len) {
        usize newcap = buf->cap + (len - avail);
        if (!buf_resize(buf, newcap))
            return false;
    }
    memcpy(buf->bytes + buf->len, data, len);
    buf->len += len;
    return true;
}


static int buf_append_luafun_writer(lua_State* L, const void* data, usize len, void* ud) {
    Buf* buf = ud;
    bool ok = buf_append(buf, data, len);
    if UNLIKELY(!ok)
        dlog("buf_append(len=%zu) failed (OOM)", len);
    return !ok;
}


int buf_append_luafun(Buf* buf, lua_State* L, bool strip_debuginfo) {
    if (!buf_reserve(buf, 512))
        return -ENOMEM;
    if (lua_dump(L, buf_append_luafun_writer, buf, strip_debuginfo))
        return -EINVAL;
    return 0;
}


Buf* nullable l_buf_check(lua_State* L, int idx) {
    return uval_check(L, idx, UValType_Buf, "Buf");
}


int l_buf_gc(lua_State* L) {
    Buf* buf = lua_touserdata(L, 1);
    return buf_free(buf);
}


Buf* nullable l_buf_createx(lua_State* L, u64 cap) {
    int nuvalue = 0;
    // See comment in l_iodesc_create.
    // dlog("allocating buffer with cap %lu (total %zu B)", cap, sizeof(Buf) + (usize)cap);
    Buf* buf = uval_new(L, UValType_Buf, sizeof(Buf) + (usize)cap, nuvalue);
    if (!buf)
        return NULL;
    buf->cap = (usize)cap;
    buf->len = 0;
    buf->bytes = (u8*)buf + sizeof(Buf);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &g_buf_luatabkey);
    lua_setmetatable(L, -2);
    return buf;
}


// fun buf_create(cap u64)
int l_buf_create(lua_State* L) {
    u64 cap = lua_tointegerx(L, 1, NULL);
    if UNLIKELY(cap > (u64)USIZE_MAX - sizeof(Buf))
        luaL_error(L, "capacity too large");
    if (cap == 0) {
        cap = 64 - sizeof(Buf);
    } else {
        cap = ALIGN2(cap, sizeof(void*));
    }
    if (l_buf_createx(L, cap))
        return 1;
    return l_errno_error(L, ENOMEM);
}


// fun buf_resize(buf Buf, newcap uint)
int l_buf_resize(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    i64 newcap = luaL_checkinteger(L, 2);
    if UNLIKELY((USIZE_MAX < U64_MAX && (u64)newcap > (u64)USIZE_MAX) ||
                newcap < 0 || (u64)newcap > 0x10000000000) // llvm asan has 1TiB limit
    {
        if (newcap < 0)
            return luaL_error(L, "negative capacity");
        return luaL_error(L, "capacity too large");
    }
    if (!buf_resize(buf, newcap))
        return l_errno_error(L, ENOMEM);
    return 0;
}


#define ELLIPSIS     "…"
#define ELLIPSIS_LEN strlen(ELLIPSIS)


int l_buf_len(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    if (!buf)
        return 0;
    lua_pushinteger(L, buf->len);
    return 1;
}


int l_buf_eq(lua_State* L) {
    Buf* a = l_buf_check(L, 1);
    Buf* b = l_buf_check(L, 2);
    if (!a || !b)
        return 0;
    lua_pushboolean(L, a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0);
    return 1;
}


int l_buf_compare(lua_State* L) {
    Buf* a = l_buf_check(L, 1);
    Buf* b = l_buf_check(L, 2);
    if (!a || !b)
        return 0;
    usize min_len = (a->len < b->len) ? a->len : b->len;
    i64 result = memcmp(a->bytes, b->bytes, min_len);
    if (result == 0)
        result = ((i64)a->len - (i64)b->len);
    lua_pushinteger(L, result);
    return 1;
}


int l_buf_str(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    if (!buf)
        return 0;
    // TODO: optional second argument for how to encode the data, e.g. verbatim, hex, base64

    const usize maxlen = 1024;
    usize len = MIN(buf->len, maxlen);
    // lua_pushlstring(L, (char*)buf->bytes, len); return 1; // raw

    // allocate destination buffer that's 2x the size of the input, +1 byte for NUL
    usize dstcap = len*2 + 1;
    usize dstcap_extra = (usize)(len < buf->len) * ELLIPSIS_LEN;
    char* dst = malloc(dstcap + dstcap_extra);
    if (dst == NULL)
        return l_errno_error(L, ENOMEM);

    for (;;) {
        // usize len2 = string_hex(dst, dstcap, buf->bytes, len); // hex
        usize len2 = string_repr(dst, dstcap, buf->bytes, len); // repr

        // check for error
        if (len2 == 0) {
            free(dst);
            return 0;
        }

        // check if output fit in dst
        if (len2 < dstcap) {
            if (buf->len > maxlen) {
                memcpy(&dst[len2], ELLIPSIS, ELLIPSIS_LEN);
                len2 += ELLIPSIS_LEN;
            }
            lua_pushlstring(L, dst, len2);
            free(dst);
            return 1;
        }

        // grow dst
        dstcap = len2 + 1;
        char* dst2 = realloc(dst, dstcap + dstcap_extra);
        if (dst2 == NULL) {
            free(dst);
            return l_errno_error(L, ENOMEM);
        }
        dst = dst2;
    }
}


void luaopen_buf(lua_State* L) {
    luaL_newmetatable(L, "Buf");

    lua_pushcfunction(L, l_buf_gc);
    lua_setfield(L, -2, "__gc");

    lua_pushcfunction(L, l_buf_str);
    lua_setfield(L, -2, "__tostring");

    lua_pushcfunction(L, l_buf_len);
    lua_setfield(L, -2, "__len");

    lua_pushcfunction(L, l_buf_eq);
    lua_setfield(L, -2, "__eq");

    // Setup __index table to allow accessing "methods"
    lua_pushvalue(L, -1);  // Duplicate metatable
    lua_setfield(L, -2, "__index");  // metatable.__index = metatable


    lua_pushcfunction(L, l_buf_compare);
    lua_setfield(L, -2, "compare");

    lua_rawsetp(L, LUA_REGISTRYINDEX, &g_buf_luatabkey);
}
