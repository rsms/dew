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


// fun buf_create(cap uint, len uint = 0) Buf
int l_buf_create(lua_State* L) {
    u64 cap = lua_tointegerx(L, 1, NULL);
    u64 len = lua_tointegerx(L, 2, NULL);
    if UNLIKELY(cap > (u64)USIZE_MAX - sizeof(Buf))
        luaL_error(L, "capacity too large");
    if (cap == 0) {
        cap = 64 - sizeof(Buf);
    } else {
        cap = ALIGN2(cap, sizeof(void*));
    }
    Buf* buf = l_buf_createx(L, cap);
    if (buf) {
        if (len > cap)
            len = cap;
        buf->len = (usize)len;
        return 1;
    }
    return l_errno_error(L, ENOMEM);
}


// // fun Buf.shrinkwrap(buf Buf, newcap uint)
// int l_buf_shrinkwrap(lua_State* L) {
//     Buf* buf = l_buf_check(L, 1);
//     i64 newcap = luaL_checkinteger(L, 2);
//     if UNLIKELY((USIZE_MAX < U64_MAX && (u64)newcap > (u64)USIZE_MAX) ||
//                 newcap < 0 || (u64)newcap > 0x10000000000) // llvm asan has 1TiB limit
//     {
//         if (newcap < 0)
//             return luaL_error(L, "negative capacity");
//         return luaL_error(L, "capacity too large");
//     }
//     if (!buf_resize(buf, newcap))
//         return l_errno_error(L, ENOMEM);
//     return 0;
// }


// fun Buf.resize(buf Buf, newlen uint, fill? u8)
int l_buf_resize(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    u64 newlen = lua_tointegerx(L, 2, NULL);
    if (newlen > buf->cap) {
        if UNLIKELY(!buf_resize(buf, newlen))
            return l_errno_error(L, ENOMEM);
        int fill_ok;
        u8 fill = lua_tointegerx(L, 3, &fill_ok);
        if (fill_ok)
            memset(&buf->bytes[buf->len], fill, newlen - buf->len);
    }
    buf->len = newlen;
    return 1;
}


int l_buf_len(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    if (!buf)
        return 0;
    lua_pushinteger(L, buf->len);
    return 1;
}


int l_buf_equal(lua_State* L) {
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


// fun Buf.append(self Buf, other Buf|string)
int l_buf_append(lua_State* L) {
    const void* src;
    usize srclen = 0;
    Buf* buf = l_buf_check(L, 1);
    switch (lua_type(L, 2)) {
        case LUA_TSTRING:
            src = lua_tolstring(L, 2, &srclen);
            break;
        case LUA_TUSERDATA: {
            Buf* other = l_buf_check(L, 2);
            src = other->bytes;
            srclen = other->len;
            break;
        }
        default:
            luaL_typeerror(L, 2, "Buf|string");
    }
    if UNLIKELY(!buf_append(buf, src, srclen))
        return l_errno_error(L, ENOMEM);
    return 0;
}


int l_buf_str(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    if (!buf)
        return 0;
    lua_pushlstring(L, (char*)buf->bytes, buf->len);
    return 1;
}


static int l_buf_tostring(lua_State* L) {
    #define ELLIPSIS     "…"
    #define ELLIPSIS_LEN strlen(ELLIPSIS)

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
            lua_pushlstring(L, "", 0);
            return 1;
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

    #undef ELLIPSIS
    #undef ELLIPSIS_LEN
}


#define assert_buf_offs(L, buf, offs, size) \
    assertlf(L, \
             offs % size == 0 && \
             offs < 0xffffffffffffffff - size && \
             offs + size <= buf->len, \
             "offs: %I %p (size: %d, buf.len: %I)", offs, offs, size, buf->len)


int l_buf_get_i64(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    u64 offs = lua_tointegerx(L, 2, NULL);
    assert_buf_offs(L, buf, offs, 8);
    lua_pushinteger(L, *(i64*)&buf->bytes[offs]);
    return 1;
}
int l_buf_set_i64(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    u64 offs = lua_tointegerx(L, 2, NULL);
    i64 val = lua_tointegerx(L, 3, NULL);
    assert_buf_offs(L, buf, offs, 8);
    *(i64*)&buf->bytes[offs] = val;
    return 0;
}


int l_buf_get_f64(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    u64 offs = lua_tointegerx(L, 2, NULL);
    assert_buf_offs(L, buf, offs, 8);
    lua_pushnumber(L, *(float64*)&buf->bytes[offs]);
    return 1;
}
int l_buf_set_f64(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    u64 offs = lua_tointegerx(L, 2, NULL);
    float64 val = lua_tonumberx(L, 3, NULL);
    assert_buf_offs(L, buf, offs, 8);
    *(float64*)&buf->bytes[offs] = val;
    return 0;
}


int l_buf_get_u32(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    u64 offs = lua_tointegerx(L, 2, NULL);
    assert_buf_offs(L, buf, offs, 4);
    lua_pushinteger(L, *(u32*)&buf->bytes[offs]);
    return 1;
}
int l_buf_set_u32(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    u64 offs = lua_tointegerx(L, 2, NULL);
    u32 val = lua_tointegerx(L, 3, NULL);
    assert_buf_offs(L, buf, offs, 4);
    *(u32*)&buf->bytes[offs] = val;
    return 0;
}


int l_buf_get_i32(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    u64 offs = lua_tointegerx(L, 2, NULL);
    assert_buf_offs(L, buf, offs, 4);
    lua_pushinteger(L, *(i32*)&buf->bytes[offs]);
    return 1;
}
int l_buf_set_i32(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    u64 offs = lua_tointegerx(L, 2, NULL);
    i32 val = lua_tointegerx(L, 3, NULL);
    assert_buf_offs(L, buf, offs, 4);
    *(i32*)&buf->bytes[offs] = val;
    return 0;
}


int l_buf_get_u16(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    u64 offs = lua_tointegerx(L, 2, NULL);
    assert_buf_offs(L, buf, offs, 2);
    lua_pushinteger(L, *(u16*)&buf->bytes[offs]);
    return 1;
}
int l_buf_set_u16(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    u64 offs = lua_tointegerx(L, 2, NULL);
    u16 val = lua_tointegerx(L, 3, NULL);
    assert_buf_offs(L, buf, offs, 2);
    *(u16*)&buf->bytes[offs] = val;
    return 0;
}
int l_buf_inc_u16(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    u64 offs = lua_tointegerx(L, 2, NULL);
    i32 incr = lua_tointegerx(L, 3, NULL);
    assert_buf_offs(L, buf, offs, 2);
    i32 value = ((i32)*(u16*)&buf->bytes[offs]) + incr;
    *(u16*)&buf->bytes[offs] = (u16)value;
    return 0;
}


int l_buf_set_u8(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    u64 offs = lua_tointegerx(L, 2, NULL);
    u8 val = lua_tointegerx(L, 3, NULL);
    assert_buf_offs(L, buf, offs, 1);
    buf->bytes[offs] = val;
    return 0;
}
int l_buf_get_u8(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    u64 offs = lua_tointegerx(L, 2, NULL);
    assert_buf_offs(L, buf, offs, 1);
    lua_pushinteger(L, buf->bytes[offs]);
    return 1;
}


// fun push_u32(buf Buf, value u32) (offs uint)
int l_buf_push_u32(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    u32 val = lua_tointegerx(L, 2, NULL);
    usize avail = buf->cap - buf->len;
    if UNLIKELY(avail < 4) {
        usize newcap = buf->cap + (4 - avail);
        if (!buf_resize(buf, newcap))
            return l_errno_error(L, ENOMEM);
    }
    u64 offs = buf->len;
    buf->len += 4;
    *(u32*)&buf->bytes[offs] = val;
    lua_pushinteger(L, offs);
    return 1;
}


// fun push_u64(buf Buf, value u64)  (offs uint)
int l_buf_push_u64(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    u64 val = lua_tointegerx(L, 2, NULL);
    usize avail = buf->cap - buf->len;
    if UNLIKELY(avail < 8) {
        usize newcap = buf->cap + (8 - avail);
        if (!buf_resize(buf, newcap))
            return l_errno_error(L, ENOMEM);
    }
    u64 offs = buf->len;
    buf->len += 8;
    memcpy(&buf->bytes[offs], &val, 8);
    lua_pushinteger(L, offs);
    return 1;
}


// fun push_f64(buf Buf, value f64)  (offs uint)
int l_buf_push_f64(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    float64 val = lua_tonumberx(L, 2, NULL);
    usize avail = buf->cap - buf->len;
    if UNLIKELY(avail < 8) {
        usize newcap = buf->cap + (8 - avail);
        if (!buf_resize(buf, newcap))
            return l_errno_error(L, ENOMEM);
    }
    u64 offs = buf->len;
    buf->len += 8;
    memcpy(&buf->bytes[offs], &val, 8);
    lua_pushinteger(L, offs);
    return 1;
}


// fun push_u64(buf Buf, value1 u64, value2 u64)  (offs uint)
int l_buf_push_u64x2(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    u64 val1 = lua_tointegerx(L, 2, NULL);
    u64 val2 = lua_tointegerx(L, 3, NULL);
    usize avail = buf->cap - buf->len;
    if UNLIKELY(avail < 16) {
        usize newcap = buf->cap + (16 - avail);
        if (!buf_resize(buf, newcap))
            return l_errno_error(L, ENOMEM);
    }
    u64 offs = buf->len;
    buf->len += 16;
    memcpy(&buf->bytes[offs], &val1, 8);
    memcpy(&buf->bytes[offs+8], &val2, 8);
    lua_pushinteger(L, offs);
    return 1;
}


// fun find_u32(buf Buf, start_offs, end_offs, stride uint, key u32) uint?
int l_buf_find_u32(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    i64 start_offs = lua_tointegerx(L, 2, NULL);
    i64 end_offs = lua_tointegerx(L, 3, NULL);
    i64 stride   = lua_tointegerx(L, 4, NULL);
    u32 key      = lua_tointegerx(L, 5, NULL);

    // [a b c d e]
    //  0 1 2 3 4
    //
    // (0, 3) => a b c
    // (3, 5) => d e
    // (3, 0) => c b a
    // (3, 2) => c

    if UNLIKELY(stride <= 0 || stride % 4 || start_offs < 0 || end_offs < 0)
        return luaL_error(L, "invalid argument");

    i64 start_check, end_check;
    if (start_offs > end_offs) {
        start_check = end_offs;
        end_check = start_offs;
        start_offs -= stride;
        end_offs -= stride;
        stride = -stride;
    } else {
        start_check = start_offs;
        end_check = end_offs;
    }

    if UNLIKELY(start_check >= end_check ||
                end_check > (i64)buf->len ||
                (end_check - start_check) % stride ||
                start_check % 4 || end_check % 4)
    {
        if (start_check >= end_check)
            return 0;
        if (end_check > (i64)buf->len)
            return luaL_error(L, "out of bounds");
        return luaL_error(L, "invalid argument");
    }

    for (i64 offs = start_offs; offs != end_offs; offs += stride) {
        if (*(u32*)&buf->bytes[offs] == key) {
            lua_pushinteger(L, offs);
            return 1;
        }
    }
    return 0;
}


// fun Buf.alloc(nbyte uint) (offs uint)
int l_buf_alloc(lua_State* L) {
    Buf* buf = l_buf_check(L, 1);
    usize nbyte = lua_tointegerx(L, 2, NULL);
    usize avail = buf->cap - buf->len;
    if UNLIKELY(avail < nbyte) {
        usize newcap = buf->cap + (nbyte - avail);
        if (!buf_resize(buf, newcap))
            return l_errno_error(L, ENOMEM);
    }
    u64 offs = buf->len;
    buf->len += nbyte;
    lua_pushinteger(L, offs);
    return 1;
}


static u64 wyhash(const void* data, usize len, u64 seed, const u64 secret[4]);

// fun Buf.hash(seed=0, start=0, end=0 int) uint
int l_buf_hash(lua_State* L) {
    Buf* buf  = l_buf_check(L, 1);
    u64 seed  = lua_tointegerx(L, 2, NULL);
    i64 start = lua_tointegerx(L, 3, NULL);
    i64 end   = lua_tointegerx(L, 4, NULL);

    if (end == 0)
        end = buf->len;

    if UNLIKELY(start < 0 || end < 0 ||
                end < start ||
                (u64)start > (u64)buf->len ||
                (u64)end > (u64)buf->len)
    {
        return luaL_error(L, "out of bounds: %d..%d", start, end);
    }

    static const u64 kSecret[4] = {
        0xe0e179a6e58651db,
        0x3ca9ef1e1d4dc588,
        0x9fc0c1d1b5bd0cb3,
        0x7e37360d82c1ff71 };
    lua_pushinteger(L, wyhash(&buf->bytes[start], (usize)(end - start), seed, kSecret));
    return 1;
}

// ———————— begin wyhash —————————
// wyhash https://github.com/wangyi-fudan/wyhash (public domain, "unlicense")
#if __LONG_MAX__ <= 0x7fffffff
    #define WYHASH_32BIT_MUM 1
#else
    #define WYHASH_32BIT_MUM 0
#endif
UNUSED static inline u64 _wyrot(u64 x) {
    return (x >> 32) | (x << 32);
}
static inline void _wymum(u64* A, u64* B) {
    #if (WYHASH_32BIT_MUM)
        u64 hh = (*A >> 32) * (*B >> 32), hl = (*A >> 32) * (u32)*B, lh = (u32)*A * (*B >> 32),
            ll = (u64)(u32)*A * (u32)*B;
        *A = _wyrot(hl) ^ hh;
        *B = _wyrot(lh) ^ ll;
    #elif defined(__SIZEOF_INT128__)
        __uint128_t r = *A;
        r *= *B;
        *A = (u64)r;
        *B = (u64)(r >> 64);
    #else
        u64 ha = *A >> 32, hb = *B >> 32, la = (u32)*A, lb = (u32)*B, hi, lo;
        u64 rh = ha * hb, rm0 = ha * lb, rm1 = hb * la, rl = la * lb, t = rl + (rm0 << 32), c = t < rl;
        lo = t + (rm1 << 32);
        c += lo < t;
        hi = rh + (rm0 >> 32) + (rm1 >> 32) + c;
        *A = lo;
        *B = hi;
    #endif
}
static inline u64 _wymix(u64 A, u64 B) {
    _wymum(&A, &B);
    return A ^ B;
}
static inline u64 _wyr8(const u8* p) {
    u64 v;
    memcpy(&v, p, 8);
    return v;
}
static inline u64 _wyr4(const u8* p) {
    u32 v;
    memcpy(&v, p, 4);
    return v;
}
static inline u64 _wyr3(const u8* p, usize k) {
    return (((u64)p[0]) << 16) | (((u64)p[k >> 1]) << 8) | p[k - 1];
}
static u64 wyhash(const void* data, usize len, u64 seed, const u64* secret) {
    const u8* p = (const u8*)data;
    seed ^= _wymix(seed ^ secret[0], secret[1]);
    u64 a, b;
    if (LIKELY(len <= 16)) {
        if (LIKELY(len >= 4)) {
            a = (_wyr4(p) << 32) | _wyr4(p + ((len >> 3) << 2));
            b = (_wyr4(p + len - 4) << 32) | _wyr4(p + len - 4 - ((len >> 3) << 2));
        } else if (LIKELY(len > 0)) {
            a = _wyr3(p, len);
            b = 0;
        } else {
            a = b = 0;
        }
    } else {
        usize i = len;
        if (UNLIKELY(i > 48)) {
            u64 see1 = seed, see2 = seed;
            do {
                seed = _wymix(_wyr8(p) ^ secret[1], _wyr8(p + 8) ^ seed);
                see1 = _wymix(_wyr8(p + 16) ^ secret[2], _wyr8(p + 24) ^ see1);
                see2 = _wymix(_wyr8(p + 32) ^ secret[3], _wyr8(p + 40) ^ see2);
                p += 48;
                i -= 48;
            } while (LIKELY(i > 48));
            seed ^= see1 ^ see2;
        }
        while (UNLIKELY(i > 16)) {
            seed = _wymix(_wyr8(p) ^ secret[1], _wyr8(p + 8) ^ seed);
            i -= 16;
            p += 16;
        }
        a = _wyr8(p + i - 16);
        b = _wyr8(p + i - 8);
    }
    a ^= secret[1];
    b ^= seed;
    _wymum(&a, &b);
    return _wymix(a ^ secret[0] ^ len, b ^ secret[1]);
}
// ———————— end wyhash —————————


void luaopen_buf(lua_State* L) {
    // Note: several buf API functions are defined by luaopen_runtime in addition to these

    luaL_newmetatable(L, "Buf");

    lua_pushcfunction(L, l_buf_gc); lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, l_buf_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, l_buf_len); lua_setfield(L, -2, "__len");

    // Setup __index table to allow accessing "methods"
    lua_pushvalue(L, -1);  // Duplicate metatable
    lua_setfield(L, -2, "__index");  // metatable.__index = metatable

    lua_pushcfunction(L, l_buf_compare); lua_setfield(L, -2, "compare");
    lua_pushcfunction(L, l_buf_equal); lua_setfield(L, -2, "equal");
    lua_pushcfunction(L, l_buf_append); lua_setfield(L, -2, "append");
    lua_pushcfunction(L, l_buf_str); lua_setfield(L, -2, "str");
    lua_pushcfunction(L, l_buf_resize); lua_setfield(L, -2, "resize");
    lua_pushcfunction(L, l_buf_hash); lua_setfield(L, -2, "hash");
    lua_pushcfunction(L, l_buf_find_u32); lua_setfield(L, -2, "find_u32");

    lua_pushcfunction(L, l_buf_get_u8); lua_setfield(L, -2, "get_u8");
    lua_pushcfunction(L, l_buf_set_u8); lua_setfield(L, -2, "set_u8");

    lua_pushcfunction(L, l_buf_get_u16); lua_setfield(L, -2, "get_u16");
    lua_pushcfunction(L, l_buf_set_u16); lua_setfield(L, -2, "set_u16");
    lua_pushcfunction(L, l_buf_inc_u16); lua_setfield(L, -2, "inc_u16");

    lua_pushcfunction(L, l_buf_get_u32); lua_setfield(L, -2, "get_u32");
    lua_pushcfunction(L, l_buf_set_u32); lua_setfield(L, -2, "set_u32");

    lua_pushcfunction(L, l_buf_get_i32); lua_setfield(L, -2, "get_i32");
    lua_pushcfunction(L, l_buf_set_i32); lua_setfield(L, -2, "set_i32");

    lua_pushcfunction(L, l_buf_get_i64); lua_setfield(L, -2, "get_i64");
    lua_pushcfunction(L, l_buf_set_i64); lua_setfield(L, -2, "set_i64");

    lua_pushcfunction(L, l_buf_get_f64); lua_setfield(L, -2, "get_f64");
    lua_pushcfunction(L, l_buf_set_f64); lua_setfield(L, -2, "set_f64");

    lua_pushcfunction(L, l_buf_alloc); lua_setfield(L, -2, "alloc");
    lua_pushcfunction(L, l_buf_push_u32); lua_setfield(L, -2, "push_u32");
    lua_pushcfunction(L, l_buf_push_u64); lua_setfield(L, -2, "push_u64");
    lua_pushcfunction(L, l_buf_push_u64x2); lua_setfield(L, -2, "push_u64x2");
    lua_pushcfunction(L, l_buf_push_f64); lua_setfield(L, -2, "push_f64");

    lua_rawsetp(L, LUA_REGISTRYINDEX, &g_buf_luatabkey);
}
