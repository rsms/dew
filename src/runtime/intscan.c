#include "intscan.h"
#include "runtime.h" // ERR_
#include "lutil.h"


static const u8 g_intdectab[256] = { // decoding table, base 2-36
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
     0, 1, 2, 3, 4, 5, 6, 7, 8, 9,-1,-1,-1,-1,-1,-1, // 0-9
    -1,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24, // A-Z
    25,26,27,28,29,30,31,32,33,34,35,-1,-1,-1,-1,-1,
    -1,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24, // a-z
    25,26,27,28,29,30,31,32,33,34,35,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};


// dew_intscan parses a byte string as an unsigned integer, up to 0xffffffffffffffff
// srcp:   pointer to an array of bytes to parse
// srclen: number of bytes at *srcp
// base:   numeric base; valid range [2-32]
// limit:  max integer value, used as a mask. u64=0xffffffffffffffff, i32=0x7fffffff, ...
// result: pointer to where to store the result
// neg:    0 for normal operation, -1 to interpret input "123" as "-123"
// returns 0 on success, -errno on error
static int dew_intscan(const u8** srcp, usize srclen, u32 base, u64 limit, int neg, u64* result) {
    /*
    intscan from musl adapted to dew.
    musl is licensed under the MIT license:

    Copyright © 2005-2020 Rich Felker, et al.

    Permission is hereby granted, free of charge, to any person obtaining
    a copy of this software and associated documentation files (the
    "Software"), to deal in the Software without restriction, including
    without limitation the rights to use, copy, modify, merge, publish,
    distribute, sublicense, and/or sell copies of the Software, and to
    permit persons to whom the Software is furnished to do so, subject to
    the following conditions:

    The above copyright notice and this permission notice shall be
    included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
    EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
    IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
    CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
    TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
    SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
    */

    const u8* src = *srcp;
    const u8* srcend = src + srclen;
    int err = 0;
    u8 c;
    u32 x;
    u64 y;

    if UNLIKELY(srclen == 0 || base > 36 || base == 1)
        return srclen == 0 ? ERR_INPUT : ERR_RANGE;

    #define NEXTCH() (LIKELY(src < srcend) ? ({ \
        int x__ = src+1 < srcend && *src == '_'; \
        u8 c__ = src[x__]; \
        src += 1 + x__; \
        c__; \
    }) : 0xff)

    c = *src++;

    if (c == '-') {
        neg = -1;
        c = NEXTCH();
    }

    if UNLIKELY(g_intdectab[c] >= base) {
        *srcp = src;
        return ERR_INPUT;
    }

    if (base == 10) {
        for (x=0; c-'0' < 10U && x<=UINT_MAX/10-1; c=NEXTCH())
            x = x*10 + (c-'0');
        for (y=x; c-'0'<10U && y<=ULLONG_MAX/10 && 10*y<=ULLONG_MAX-(c-'0'); c=NEXTCH())
            y = y*10 + (c-'0');
        if (c-'0'>=10U)
            goto done;
    } else if (!(base & base-1)) {
        int bs = "\0\1\2\4\7\3\6\5"[(0x17*base)>>5&7];
        for (x=0; g_intdectab[c]<base && x<=UINT_MAX/32; c=NEXTCH())
            x = x<<bs | g_intdectab[c];
        for (y=x; g_intdectab[c]<base && y<=ULLONG_MAX>>bs; c=NEXTCH())
            y = y<<bs | g_intdectab[c];
    } else {
        for (x=0; g_intdectab[c]<base && x<=UINT_MAX/36-1; c=NEXTCH())
            x = x*base + g_intdectab[c];
        for (
            y = x;
            g_intdectab[c]<base && y <= ULLONG_MAX/base && base*y <= ULLONG_MAX-g_intdectab[c];
            c = NEXTCH() )
        {
            y = y*base + g_intdectab[c];
        }
    }

    if (g_intdectab[c] < base) {
        for (; g_intdectab[c] < base; c = NEXTCH());
        err = ERR_RANGE;
        y = limit;
        if (limit&1)
            neg = 0;
    }

done:
    src--;

    if UNLIKELY(c != 0xff) {
        err = ERR_INPUT;
    } else if UNLIKELY(y >= limit) {
        if (!(limit & 1) && !neg) {
            *result = limit - 1;
            err = ERR_RANGE;
        } else if (y > limit) {
            *result = limit;
            err = ERR_RANGE;
        }
    }

    *result = (y ^ neg) - neg;
    *srcp = src;
    return err;
}


int l_intscan(lua_State* L) {
    size_t len;
    const char* str = luaL_checklstring(L, 1, &len);
    lua_Integer base = luaL_optinteger(L, 2, 10);
    u64 limit = luaL_optinteger(L, 3, 0xFFFFFFFFFFFFFFFF);
    bool isneg = x_luaL_optboolean(L, 4, 0);

    const u8* src = (const u8*)str;
    u64 result = 0;
    int neg = -(int)isneg; // -1 or 0
    int err = dew_intscan(&src, len, base, limit, neg, &result);

    lua_pushinteger(L, (lua_Integer)result);
    lua_pushinteger(L, err);
    return 2;
}
