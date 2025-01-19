#include "dew.h"

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

#define FOREACH_ERR(_) \
	/* NAME, errno equivalent, description */ \
	_( ERR_OK,           0,       "no error") \
	_( ERR_INVALID,      EINVAL,  "invalid data or argument") \
	_( ERR_RANGE,        ERANGE,  "result out of range") \
	_( ERR_INPUT,        -1,      "invalid input") \
	_( ERR_SYSOP,        -1,      "invalid syscall op or syscall op data") \
	_( ERR_BADFD,        EBADF,   "invalid file descriptor") \
	_( ERR_BADNAME,      -1,      "invalid or misformed name") \
	_( ERR_NOTFOUND,     ENOENT,  "resource not found") \
	_( ERR_NAMETOOLONG,  -1,      "name too long") \
	_( ERR_CANCELED,     -1,      "operation canceled") \
	_( ERR_NOTSUPPORTED, ENOTSUP, "not supported") \
	_( ERR_EXISTS,       EEXIST,  "already exists") \
	_( ERR_END,          -1,      "end of resource") \
	_( ERR_ACCESS,       EACCES,  "permission denied") \
	_( ERR_NOMEM,        -1,      "cannot allocate memory") \
	_( ERR_MFAULT,       -1,      "bad memory address") \
	_( ERR_OVERFLOW,     -1,      "value too large") \
	_( ERR_READONLY,     EROFS,   "read-only") \
	_( ERR_IO,           EIO,     "I/O error") \
	_( ERR_NOTDIR,       ENOTDIR, "not a directory") \
	_( ERR_ISDIR,        EISDIR,  "is a directory") \
// end FOREACH_ERR

enum {
	#define _(NAME, ERRNO, ...) NAME,
	FOREACH_ERR(_)
	#undef _
	ERR_ERROR = 0xff,
};


static bool x_luaL_optboolean(lua_State *L, int index, bool default_value) {
	if (lua_isnoneornil(L, index))
		return default_value;
	return lua_toboolean(L, index);
}


static int err_from_errno(int errno_val) {
	static const int tab[] = {
		#define _(NAME, ERRNO, ...) [ERRNO == -1 ? NAME+0xff : ERRNO] = NAME,
		FOREACH_ERR(_)
		#undef _
	};
	if (errno_val == 0) return 0;
	if (errno_val < (int)countof(tab)) {
		int err = tab[errno_val];
		if (err < 0xff)
			return err;
	}
	return ERR_ERROR;
}


// errstr takes an integer argument that is one of the ERR_ constants and returns a string.
// e.g. errstr(ERR_INVALID) == "INVALID"
static int l_errstr(lua_State *L) {
	const int err = luaL_checkinteger(L, 1);
	const char* name = "ERROR";
	const char* description = "unspecified error";
	switch (err) {
		#define _(NAME, ERRNO, DESC) \
			case NAME: \
				name = &#NAME[4]; \
				description = DESC; \
				break;
		FOREACH_ERR(_)
		#undef _
	}
	lua_pushstring(L, name);
	lua_pushstring(L, description);
	return 2;
}


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


// intscan
// fun intscan(s str, base int = 10, limit u64 = U64_MAX) u64, err
//
// Parses a Lua string into an integer using a specific numeric base and limit.
// Arguments:
// - `str` (string): The input string to parse.
// - `base` (integer, optional): The numeric base to use for parsing. Default is 10.
// - `limit` (integer, optional): The maximum value to parse. Default is 0xFFFFFFFFFFFFFFFF.
// - `isneg` (boolean, optional): Set to true to interpret str as a negative number,
//   i.e. intscan( "123", 10, 0x80, true)
//     == intscan("-123", 10, 0x80, false)
//
static int l_intscan(lua_State *L) {
	size_t len;
	const char *str = luaL_checklstring(L, 1, &len);
	lua_Integer base = luaL_optinteger(L, 2, 10);
	u64 limit = luaL_optinteger(L, 3, 0xFFFFFFFFFFFFFFFF);
	bool isneg = x_luaL_optboolean(L, 4, 0);

	const u8 *src = (const u8 *)str;
	u64 result = 0;
	int neg = -(int)isneg; // -1 or 0
	int err = dew_intscan(&src, len, base, limit, neg, &result);

	lua_pushinteger(L, (lua_Integer)result);
	lua_pushinteger(L, err);
	return 2;
}


// intfmt formats an integer value as a string in a specified base.
// value (integer): The integer value to format. Can be signed or unsigned.
// base (integer): The base for the conversion. Must be in the range [2, 36].
// is_unsigned (boolean, optional): If true, treat the value as unsigned. Defaults to false.
//
// Return value:
//   A string representing the integer in the specified base.
//   If an error occurs, raises a Lua error.
static int l_intfmt(lua_State *L) {
	int err = 0;

	// buf fits base-2 representations of all 64-bit numbers + NUL term
	char buf[66];
	char* str = &buf[sizeof(buf) - 1];
	*str = 0;

	// check arguments
	int nargs = lua_gettop(L);
	if (nargs < 2 || nargs > 3) {
		err = ERR_INVALID;
		goto end;
	}
	i64 value = luaL_checkinteger(L, 1);
	lua_Integer base = luaL_checkinteger(L, 2);
	if (base < 2 || base > 36) {
		err = ERR_RANGE;
		goto end;
	}
	int is_unsigned = (nargs > 2) ? lua_toboolean(L, 3) : 0;

	u64 uvalue = is_unsigned ? (u64)value : (u64)((value < 0) ? -value : value);
	do {
		// 0xdf (0b_1101_1111) normalizes the case of letters, i.e. 'a' => 'A'
		*--str = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"[(uvalue % base) & 0xdf];
		uvalue /= base;
	} while (uvalue);

	if (!is_unsigned && value < 0)
		*--str = '-';

end:
	lua_pushstring(L, str);
	lua_pushinteger(L, err);
	return 2;
}


// Convert integer value between different bit widths and signedness
static i64 intconv(
	i64 value, int src_bits, int dst_bits, bool src_issigned, bool dst_issigned)
{
	// For 64-bit signed source values, preserve the original value before masking
	i64 original_value = value;

	// Ensure value fits in the source bit size
	if (src_bits < 64) {
		u64 src_mask = (1ULL << src_bits) - 1;
		value &= src_mask;
	}

	// If source is signed and the value has the sign bit set, sign extend it
	if (src_issigned && (value & (1ULL << (src_bits - 1)))) {
		// For 64-bit signed source, use the original value
		if (src_bits == 64) {
			value = original_value;
		} else {
			value -= (1ULL << src_bits);
		}
	}

	// Handle destination conversions
	if (dst_bits < 64) {
		u64 dst_mask = (1ULL << dst_bits) - 1;
		if (!dst_issigned) {
			// For unsigned destination, truncate to fit the destination size
			return value & dst_mask;
		}
		// For signed destination, truncate and sign extend if necessary
		value = value & dst_mask;
		if (value & (1ULL << (dst_bits - 1)))
			value -= (1ULL << dst_bits);
	}
	return value;
}


// fun intconv(value i64, src_bits, dst_bits uint, src_issigned, dst_issigned bool) int
static int l_intconv(lua_State *L) {
	i64 value = luaL_checkinteger(L, 1);
	i64 src_bits = luaL_checkinteger(L, 2);
	i64 dst_bits = luaL_checkinteger(L, 3);
	bool src_issigned = lua_toboolean(L, 4);
	bool dst_issigned = lua_toboolean(L, 5);

	luaL_argcheck(L, (src_bits > 0 && src_bits <= 64), 3,
	              "source bits must be between 1 and 64");
	luaL_argcheck(L, (dst_bits > 0 && dst_bits <= 64), 5,
	              "destination bits must be between 1 and 64");

	i64 result = intconv(value, src_bits, dst_bits, src_issigned, dst_issigned);

	lua_pushinteger(L, (lua_Integer)result);
	return 1;
}


static const luaL_Reg dew_lib[] = {
	{"intscan", l_intscan},
	{"intfmt", l_intfmt},
	{"intconv", l_intconv},
	{"errstr", l_errstr},
	{NULL, NULL} // Sentinel
};

int luaopen_dew(lua_State *L) {
	luaL_newlib(L, dew_lib);

	#define _(NAME, ERRNO, ...) \
		lua_pushinteger(L, NAME); \
		lua_setfield(L, -2, #NAME);
	FOREACH_ERR(_)
	// note: not including ERR_ERROR on purpose as it's "unknown error"
	#undef _

	return 1;
}