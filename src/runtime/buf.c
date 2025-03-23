#include "buf.h"
#include "lutil.h"


static u8 g_buf_luatabkey; // Buf object prototype


int buf_free(Buf* buf) {
    // if 'bytes' does not point into embedded memory, free it
    if (buf->bytes != (void*)buf + sizeof(*buf))
        free(buf->bytes);
    return 0;
}


static bool buf_resize(Buf* buf, usize newcap) {
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
    if (buf->bytes == (void*)buf + sizeof(*buf)) {
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
    return l_uobj_check(L, idx, &g_buf_luatabkey, "Buf");
}


int l_buf_gc(lua_State* L) {
    Buf* buf = lua_touserdata(L, 1);
    return buf_free(buf);
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
    // dlog("allocating buffer with cap %lu (total %zu B)", cap, sizeof(Buf) + (usize)cap);

    // see comment in l_iodesc_create
    int nuvalue = 0;
    Buf* b = lua_newuserdatauv(L, sizeof(Buf) + (usize)cap, nuvalue);
    b->cap = (usize)cap;
    b->len = 0;
    b->bytes = (void*)b + sizeof(Buf);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &g_buf_luatabkey);
    lua_setmetatable(L, -2);

    return 1;
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


int l_buf_str(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    // TODO: optional second argument for how to encode the data, e.g. hex, base64
    lua_pushlstring(L, (char*)buf->bytes, buf->len);
    return 1;
}


void luaopen_buf(lua_State* L) {
    luaL_newmetatable(L, "Buf");
    lua_pushcfunction(L, l_buf_gc);
    lua_setfield(L, -2, "__gc");
    lua_rawsetp(L, LUA_REGISTRYINDEX, &g_buf_luatabkey);
}
