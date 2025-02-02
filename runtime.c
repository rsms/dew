#include "runtime.h"

#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <sys/socket.h>
#include <netinet/in.h>

static_assert(sizeof(T) == LUA_EXTRASPACE, "");




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


static bool x_luaL_optboolean(lua_State* L, int index, bool default_value) {
	if (lua_isnoneornil(L, index))
		return default_value;
	return lua_toboolean(L, index);
}


static int l_errno_error(lua_State* L, int err_no) {
	lua_pushstring(L, strerror(err_no));
	return lua_error(L);
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
static int l_errstr(lua_State* L) {
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
static int l_intscan(lua_State* L) {
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


// intfmt formats an integer value as a string in a specified base.
// value (integer): The integer value to format. Can be signed or unsigned.
// base (integer): The base for the conversion. Must be in the range [2, 36].
// is_unsigned (boolean, optional): If true, treat the value as unsigned. Defaults to false.
//
// Return value:
//   A string representing the integer in the specified base.
//   If an error occurs, raises a Lua error.
static int l_intfmt(lua_State* L) {
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
static int l_intconv(lua_State* L) {
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


#ifdef __wasm__
#include "dew.h"
static int l_ipcrecv(lua_State* L) {
	// TODO: this could be generalized into a io_uring like API.
	// Could then use async reading of stdin instead of a dedicated "ipc" syscall.

	// TODO: arguments
	IPCMsg msg = {};
	u32 flags = 0;
	long result = syscall(SysOp_IPCRECV, (uintptr)&msg, (long)flags, 0, 0, 0);
	// TODO: return message
	lua_pushinteger(L, (lua_Integer)result);
	return 1;
}

static lua_State* g_L = NULL;
static lua_State* g_ipcrecv_co = NULL;

extern int ipcrecv(int arg);

__attribute__((visibility("default"))) int ipcsend(long value) {
	// printf("ipcsend %ld\n", value);
	if (g_ipcrecv_co == NULL)
		return -EINVAL;
	lua_State* L_from = g_L;
	lua_State* L = g_ipcrecv_co;
	g_ipcrecv_co = NULL;
	lua_pushinteger(L, value);
	int status = lua_resume(L, L_from, 1, NULL);
	if (status == LUA_OK)
		return 0;
	fprintf(stderr, "Error resuming coroutine: %s\n", lua_tostring(L, -1));
	return -ECHILD;
}

static int l_ipcrecv_co(lua_State* L) {
	if (!lua_isyieldable(L))
		return luaL_error(L, "not called from a coroutine");
	int r = ipcrecv(123);
	// TODO: can return result immediately here is there is some
	g_ipcrecv_co = L;
	return lua_yield(L, 0);
}

static int l_iowait(lua_State* L) {
	long result = syscall(SysOp_IOWAIT, 0, 0, 0, 0, 0);
	lua_pushinteger(L, (lua_Integer)result);
	return 1;
}

#endif // __wasm__



// Note: can check if called on a coroutine with this:
// if (!lua_isyieldable(L))
// 	return luaL_error(L, "not a coroutine");
// lua_pushthread(L);


extern lua_State* g_L; // dew.c
static Runloop* g_main_runloop = NULL;


static int l_time(lua_State* L) {
	lua_pushinteger(L, DTimeNow());
	return 1;
}


static void timer_handler(Runloop* rl, int w, u64 ud) {
	dlog("timer_handler w=%d", w);
	if (ud) {
		lua_State* L = g_L;
		int ref = (int)ud;
		lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
		// Call the function (assume no arguments and 1 return value for simplicity)
		if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
			// Handle Lua error
			lua_error(L);
		}
	}
}


static int l_runloop_add_timeout(lua_State* L) {
	if (!lua_isyieldable(L))
		return luaL_error(L, "not a coroutine");
	DTime deadline = luaL_checkinteger(L, 1);
	u64 userdata = (uintptr)L;
	int w = RunloopAddTimeout(g_main_runloop, timer_handler, userdata, deadline);
	if (w < 0)
		return l_errno_error(L, -w);

	lua_pushinteger(L, 1); // yield 1 to let runtime know we're still going
	return lua_yield(L, 1);
	// lua_pushinteger(L, w);
	// return 1;
}


static int l_runloop_add_interval(lua_State* L) {
	u64 interval_nsec = luaL_checkinteger(L, 1);
	int w = RunloopAddInterval(g_main_runloop, timer_handler, 0, interval_nsec);
	if (w < 0)
		return l_errno_error(L, -w);
	lua_pushinteger(L, w);
	return 1;
}


static int l_runloop_run(lua_State* L) {
	int n = RunloopRun(g_main_runloop, 0);
	if (n < 0)
		return l_errno_error(L, -n);
	lua_pushboolean(L, n);
	return 1;
}


// old runtime stuff above
//——————————————————————————————————————————————————————————————————————————————————————————————
// new runtime below


#define trace(fmt, ...) dlog("\e[1;34m%-15s│\e[0m " fmt, __FUNCTION__, ##__VA_ARGS__)

// #define trace_runq(fmt, ...) dlog("\e[1;35m%-15s│\e[0m " fmt, __FUNCTION__, ##__VA_ARGS__)
#define trace_runq(fmt, ...) ((void)0)

static u8 g_thread_table_key; // table where all live tasks are stored to avoid GC
static u8 g_iodesc_luatabkey; // IODesc object prototype
static u8 g_buf_luatabkey;    // Buf object prototype
static u8 g_timer_luatabkey;  // Timer object prototype

// tls_s holds S for the current thread
static _Thread_local S* tls_s = NULL;


static const char* l_status_str(int status) {
	switch (status) {
		case LUA_OK:        return "OK";
		case LUA_YIELD:     return "YIELD";
		case LUA_ERRRUN:    return "ERRRUN";
		case LUA_ERRSYNTAX: return "ERRSYNTAX";
		case LUA_ERRMEM:    return "ERRMEM";
		case LUA_ERRERR:    return "ERRERR";
	}
	return "?";
}


static void* nullable l_uobj_check(lua_State* L, int idx, const void* key, const char* tname) {
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


static int l_error_not_a_task(lua_State* L, T* t) {
	// note: this can happen in two scenarios:
	// 1. calling a task-specific API function from a non-task, or
	// 2. calling a task-specific API function from a to-be-closed variable (__close)
	//    during task shutdown.
	const char* msg = t->s ? "task is shutting down" : "not called from a task";
	trace("%s: invalid call by user: %s", __FUNCTION__, msg);
	return luaL_error(L, msg);
}


#define REQUIRE_TASK(L) ({ \
	T* __t = L_t(L); \
	if UNLIKELY(!__t->is_live) \
		return l_error_not_a_task(L, __t); \
	__t; \
})


static IODesc* nullable l_iodesc_check(lua_State* L, int idx) {
	return l_uobj_check(L, idx, &g_iodesc_luatabkey, "FD");
}

static Buf* nullable l_buf_check(lua_State* L, int idx) {
	return l_uobj_check(L, idx, &g_buf_luatabkey, "Buf");
}

static Timer* nullable l_timer_check(lua_State* L, int idx) {
	return l_uobj_check(L, idx, &g_timer_luatabkey, "Timer");
}


// s_tstatus returns the status of a task.
// Based on auxstatus from lcorolib.c
static TStatus s_tstatus(S* s, T* t) {
	if (s->L == t_L(t))
		return TStatus_RUN;
	switch (lua_status(t_L(t))) {
		case LUA_YIELD:
			return TStatus_YIELD;
		case LUA_OK: {
			lua_Debug ar;
			if (lua_getstack(t_L(t), 0, &ar))  /* does it have frames? */
				return TStatus_NORM;  /* it is running */
			else if (lua_gettop(t_L(t)) == 0)
				return TStatus_DEAD;
			else
				return TStatus_YIELD;  /* initial state */
		}
		default:  /* some error occurred */
			return TStatus_DEAD;
	}
}


// l_iodesc_gc is called when an IODesc object is about to be garbage collected by Lua
static int l_iodesc_gc(lua_State* L) {
	IODesc* d = lua_touserdata(L, 1);
	S* s = tls_s;
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


// l_iodesc_create allocates & pushes an IODesc object onto L's stack.
// Throws LUA_ERRMEM if memory allocation fails.
static IODesc* l_iodesc_create(lua_State* L) {
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


static int buf_free(Buf* buf) {
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


// buf_reserve makes sure that there is at least minavail bytes available at bytes+len
static void* nullable buf_reserve(Buf* buf, usize minavail) {
	usize avail = buf->cap - buf->len;
	if UNLIKELY(avail < minavail) {
		usize newcap = buf->cap + (minavail - avail);
		if (!buf_resize(buf, newcap))
			return NULL;
	}
	return buf->bytes + buf->len;
}


static int l_buf_gc(lua_State* L) {
	Buf* buf = lua_touserdata(L, 1);
	return buf_free(buf);
}


static int l_buf_alloc(lua_State* L) {
	u64 cap = luaL_checkinteger(L, 1);
	if UNLIKELY(cap > (u64)USIZE_MAX - sizeof(Buf))
		luaL_error(L, "capacity too large");
	if (cap == 0) {
		cap = 64 - sizeof(Buf);
	} else {
		cap = ALIGN2(cap, sizeof(void*));
	}
	// dlog("allocating buffer with cap %llu (total %zu B)", cap, sizeof(Buf) + (usize)cap);

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
static int l_buf_resize(lua_State* L) {
	Buf* buf = l_buf_check(L, 1);
	i64 newcap = luaL_checkinteger(L, 2);
	if UNLIKELY((USIZE_MAX < U64_MAX && newcap > (u64)USIZE_MAX) ||
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


static int l_buf_str(lua_State* L) {
	Buf* buf = l_buf_check(L, 1);
	// TODO: optional second argument for how to encode the data, e.g. hex, base64
	lua_pushlstring(L, (char*)buf->bytes, buf->len);
	return 1;
}


static void timers_remove(TimerPQ* timers, Timer* timer);


static int l_timer_gc(lua_State* L) {
	Timer* timer = lua_touserdata(L, 1);
	if (timer->when != (DTime)-1) {
		S* s = tls_s;
		if (s) {
			timers_remove(&s->timers, timer);
		} else {
			dlog("warning: Timer %p GC'd outside of S lifetime", timer);
		}
	}
	return 0;
}


static Timer* nullable timer_create(lua_State* L, DTime when, DTimeDuration period) {
	// see comment in l_iodesc_create
	int nuvalue = 0;
	Timer* timer = lua_newuserdatauv(L, sizeof(Timer), nuvalue);
	if (!timer)
		return NULL;
	memset(timer, 0, sizeof(*timer));
	timer->when = when;
	timer->period = period;
	lua_rawgetp(L, LUA_REGISTRYINDEX, &g_timer_luatabkey);
	lua_setmetatable(L, -2);
	return timer;
}


static u32 timers_sift_up(TimerPQ* timers, u32 i) {
	TimerInfo last = timers->v[i];
	while (i > 0) {
		u32 parent = (i - 1) / 2;
		if (last.when >= timers->v[parent].when)
			break;
		timers->v[i] = timers->v[parent];
		i = parent;
	}
	timers->v[i] = last;
	return i;
}


static void timers_sift_down(TimerPQ* timers, u32 i) {
	u32 len = timers->len;
	TimerInfo last = timers->v[len];
	for (;;) {
		u32 left = i*2 + 1;
		if (left >= len) // no left child; this is a leaf
			break;

		u32 child = left;
		u32 right = left + 1;

		// if right hand-side timer has smaller 'when', use that instead of left child
		if (right < len && timers->v[right].when < timers->v[left].when)
			child = right;

		if (timers->v[child].when >= last.when)
			break;
		// move the child up
		timers->v[i] = timers->v[child];
		i = child;
	}
	timers->v[i] = last;
}


// timers_remove_min removes the timer with the soonest 'when' time
static Timer* timers_remove_min(TimerPQ* timers) {
	assert(timers->len > 0);
	Timer* timer = timers->v[0].timer;
	u32 len = --timers->len;
	if (len == 0) {
		// Note: min 'when' changed to nothing (no more timers)
	} else {
		timers_sift_down(timers, 0);
		// Note: min 'when' changed to timers->v[0].when
	}
	return timer;
}


// timers_dlog prints the state of a 'timers' priority queue via dlog
#if !defined(DEBUG)
	#define timers_dlog(timers) ((void)0)
#else
static void timers_dlog(const TimerPQ* timers_readonly) {
	u32 n = timers_readonly->len;
	dlog("s.timers: %u", n);
	if (n == 0)
		return;

	// copy timers
	usize nbyte = sizeof(*timers_readonly->v) * n;
	void* v_copy = malloc(nbyte);
	if (!v_copy) {
		dlog("  (malloc failed)");
		return;
	}
	memcpy(v_copy, timers_readonly->v, nbyte);
	TimerPQ timers = { .cap = n, .len = n, .v = v_copy };

	for (u32 i = 0; timers.len > 0; i++) {
		Timer* timer = timers_remove_min(&timers);
		dlog("  [%u] %10llu %p", i, timer->when, timer);
	}

	free(v_copy);
}
#endif


static void timers_remove(TimerPQ* timers, Timer* timer) {
	if (timers->len == 0 || timer->when == (DTime)-1)
		return;
	// linear search to find index of timer
	u32 i;
	if (timer->when > timers->v[timers->len / 2].when) {
		for (i = timers->len; i--;) {
			if (timers->v[i].timer == timer)
				goto found;
		}
	} else {
		for (i = 0; i < timers->len; i++) {
			if (timers->v[i].timer == timer)
				goto found;
		}
	}
	dlog("warning: timer %p not found!", timer);
	return;
found:
	dlog("remove timers[%u] %p (timers.len=%u)", i, timer, timers->len);
	// timers_dlog(timers); // state before
	timers->len--;
	// if i is the last entry (i.e. latest 'when'), we don't need to do anything else
	if (i == timers->len)
		return;
	if (i > 0 && timers->v[timers->len].when < timers->v[/*parent*/(i-1)/2].when) {
		timers_sift_up(timers, i);
	} else {
		timers_sift_down(timers, i);
	}
	// timers_dlog(timers); // state after
}


static bool timers_add(TimerPQ* timers, Timer* timer) {
	// append the timer to the priority queue (heap)
	TimerInfo* ti = array_reserve((struct Array*)timers, sizeof(*timers->v), 1);
	if (!ti) // could not grow array; out of memory
		return false;
	timers->len++;
	if (timer->when == (DTime)-1) timer->when--; // uphold special meaning of -1
	DTime when = timer->when;
	ti->timer = timer;
	ti->when = when;

	// "sift up" heap sort
	u32 i = timers->len - 1;
	i = timers_sift_up(timers, i);
	if (i == 0) {
		// Note: min 'when' changed to 'when'
	}
	// timers_dlog(timers);
	return true;
}


static T* nullable s_timers_check(S* s) {
	if (s->timers.len == 0)
		return NULL;
	// const DTimeDuration leeway = 10 * D_TIME_MICROSECOND;
	// DTime now = DTimeNow() - leeway;
	DTime now = DTimeNow();
	while (s->timers.len > 0 && s->timers.v[0].when <= now) {
		Timer* timer = timers_remove_min(&s->timers);
		T* t = timer->f(timer->arg, timer->seq);
		now = DTimeNow();
		if (timer->period > 0) {
			// repeating timer
			timer->when = now + timer->period;
			bool ok = timers_add(&s->timers, timer);
			assert(ok); // never need to grow memory
		} else {
			timer->when = -1; // signal to l_timer_gc that timer is dead
		}
		// note: timers are GC'd, so no need to free a timer here in an 'else' branch
		if (t)
			return t;
	}
	return NULL;
}


static T* nullable l_timer_ring(uintptr arg, uintptr seq) {
	return (T*)arg;
}


// fun timer_start(when Time, period TimeDuration) Timer
static int l_timer_start(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	DTime when = luaL_checkinteger(L, 1);
	DTimeDuration period = luaL_checkinteger(L, 2);
	Timer* timer = timer_create(L, when, period);
	if (!timer)
		return 0;

	timer->f = l_timer_ring;
	timer->arg = (uintptr)t;
	timer->seq = 0; // unused

	if UNLIKELY(!timers_add(&t->s->timers, timer))
		return l_errno_error(L, ENOMEM);

	return lua_yieldk(L, 0, 0, NULL);
	// return 0; // XXX
}


static bool s_runq_put(S* s, T* t) {
	u32 next_tail = s->runq_tail + 1;
	if (next_tail == s->runq_cap)
		next_tail = 0;
	if UNLIKELY(next_tail == s->runq_head) {
		dlog("TODO: grow runq");
		return false;
	}
	trace_runq("runq put [%u] = " T_ID_F, s->runq_tail, t_id(t));
	s->runq[s->runq_tail] = t;
	s->runq_tail = next_tail;
	return true;
}


static bool s_runq_put_runnext(S* s, T* t) {
	trace_runq("runq put runnext = " T_ID_F, t_id(t));
	if UNLIKELY(s->runnext) {
		// kick out previous runnext to runq
		trace_runq("runq kick out runnext " T_ID_F " to runq", t_id(s->runnext));
		if UNLIKELY(!s_runq_put(s, s->runnext))
			return false;
	}
	s->runnext = t;
	return true;
}


static T* nullable s_runq_get(S* s) {
	T* t;
	if (s->runnext) {
		t = s->runnext;
		s->runnext = NULL;
		trace_runq("runq get runnext = " T_ID_F, t_id(t));
	} else {
		if (s->runq_head == s->runq_tail) // empty
			return NULL;
		t = s->runq[s->runq_head];
		trace_runq("runq get [%u] = " T_ID_F, s->runq_head, t_id(t));
		s->runq_head++;
		if (s->runq_head == s->runq_cap)
			s->runq_head = 0;
	}
	return t;
}


static void s_runq_remove(S* s, T* t) {
	if (s->runnext == t) {
		trace_runq("runq remove runnext " T_ID_F, t_id(t));
		s->runnext = NULL;
	} else if (s->runq_head != s->runq_tail) { // not empty
		u32 i = s->runq_head;
		u32 count = (s->runq_tail >= s->runq_head) ?
					(s->runq_tail - s->runq_head) :
					(s->runq_cap - s->runq_head + s->runq_tail);
		for (u32 j = 0; j < count; j++) {
			if (s->runq[i] == t) {
				trace_runq("runq remove [%u] " T_ID_F, i, t_id(t));
				u32 next = (i + 1 == s->runq_cap) ? 0 : i + 1;
				memmove(&s->runq[i], &s->runq[next], (s->runq_tail - next) * sizeof(*s->runq));
				s->runq_tail = (s->runq_tail == 0) ? s->runq_cap - 1 : s->runq_tail - 1;
				return;
			}
			i = (i + 1 == s->runq_cap) ? 0 : i + 1;
		}
		trace("warning: s_runq_remove(" T_ID_F ") called but task not found", t_id(t));
	}
}


static i32 s_runq_find(const S* s, const T* t) {
	if (s->runnext == t)
		return I32_MAX;
	if (s->runq_head != s->runq_tail) { // not empty
		u32 i = s->runq_head;
		u32 count = (s->runq_tail >= s->runq_head) ?
					(s->runq_tail - s->runq_head) :
					(s->runq_cap - s->runq_head + s->runq_tail);
		for (u32 j = 0; j < count; j++) {
			if (s->runq[i] == t)
				return (i32)i;
			i = (i + 1 == s->runq_cap) ? 0 : i + 1;
		}
	}
	return -1;
}


static void t_add_child(T* parent, T* child) {
	assert(child->next_sibling == NULL /* not in a list */);
	child->next_sibling = parent->first_child;
	parent->first_child = child;
}


static void t_remove_child_r(T* prev_sibling, T* child) {
	if (prev_sibling->next_sibling == child) {
		prev_sibling->next_sibling = child->next_sibling;
		child->next_sibling = NULL;
	} else if LIKELY(prev_sibling->next_sibling != NULL) {
		t_remove_child_r(prev_sibling->next_sibling, child);
	} else {
		dlog("  child not found: " T_ID_F, t_id(child));
		assert(!"child not found");
	}
}


static void t_remove_child(T* parent, T* child) {
	trace("remove child " T_ID_F " from " T_ID_F, t_id(child), t_id(parent));
	assert(child->parent == parent);
	child->parent = NULL;
	if (parent->first_child == child) {
		parent->first_child = child->next_sibling;
		child->next_sibling = NULL;
	} else {
		return t_remove_child_r(parent->first_child, child);
	}
}


static void t_report_error(T* t, const char* context_msg) {
	lua_State* L = t_L(t);

	const char* msg = lua_tostring(L, -1);
	if (msg == NULL) { /* is error object not a string? */
		if (luaL_callmeta(L, 1, "__tostring") &&  /* does it have a metamethod */
		    lua_type(L, -1) == LUA_TSTRING)       /* that produces a string? */
		{
			/* that is the message */
		} else {
			msg = lua_pushfstring(L, "[%s]", luaL_typename(L, 1));
		}
	}
	luaL_traceback(L, L, msg, 1);  /* append a standard traceback */
	const char* msg2 = lua_tostring(L, -1);
	if (msg2) msg = msg2;

	fprintf(stderr, "%s: ["T_ID_F "]\n%s\n", context_msg, t_id(t), msg);
	lua_pop(L, 1);
}


static void dlog_task_tree(const T* t, int level) {
	for (T* child = t->first_child; child; child = child->next_sibling) {
		dlog("%*s" T_ID_F, level*4, "", t_id(child));
		dlog_task_tree(child, level + 1);
	}
}


static void t_finalize(T* t, int status);


static void t_stop(T* parent, T* child) {
	trace("stop " T_ID_F " by parent " T_ID_F, t_id(child), t_id(parent));

	// set is_live to 0 _before_ calling lua_closethread to ensure that any to-be-closed variables
	// with __close metamethods that call into our API are properly handled.
	child->is_live = 0;

	// Shut down Lua "thread"
	//
	// Note that a __close metatable entry can be used to clean up things like open files.
	// For example:
	//     local _ <close> = setmetatable({}, { __close = function()
	//         print("cleanup here")
	//     end })
	//
	// Note: status will be OK in the common case and ERRRUN if an error occurred inside a
	// to-be-closed variable with __close metamethods.
	//
	int status = lua_closethread(t_L(child), t_L(parent));
	if (status != LUA_OK) {
		trace("warning: lua_closethread => %s", l_status_str(status));
		t_report_error(child, "Error in defer handler");
	}

	// remove from runq
	s_runq_remove(child->s, child);

	// // finalize as "runtime error"
	// lua_pushstring(t_L(child), "parent task exited");
	// return t_finalize(child, LUA_ERRRUN);

	// finalize as "clean exit"
	return t_finalize(child, LUA_OK);
}


static void t_stop_r(T* parent, T* child) {
	if (child->next_sibling)
		t_stop_r(parent, child->next_sibling);
	if (child->first_child)
		t_stop_r(child, child->first_child);
	return t_stop(parent, child);
}


static void t_finalize(T* t, int status) {
	// task is dead, either from clean exit or error (LUA_OK or LUA_ERR* status)
	trace(T_ID_F " exited (%s)", t_id(t), l_status_str(status));
	if UNLIKELY(status != LUA_OK)
		t_report_error(t, "Uncaught error");
	t->s->nlive--;
	t->is_live = 0;

	// dlog("task tree for " T_ID_F ":", t_id(t)); dlog_task_tree(t, 1);

	// stop child tasks
	if (t->first_child)
		t_stop_r(t, t->first_child);

	// remove task from parent's list of children
	if (t->parent)
		t_remove_child(t->parent, t);

	// remove GC ref to allow task (Lua thread) to be garbage collected
	lua_State* TL = t_L(t);
	lua_State* SL = t->s->L;
	lua_rawgetp(SL, LUA_REGISTRYINDEX, &g_thread_table_key);  // Get the thread table
	lua_pushthread(TL);   // Push the thread onto its own stack
	lua_xmove(TL, SL, 1); // Move the thread object to the `SL` state
	lua_pushnil(SL);      // Use `nil` to remove the key
	lua_rawset(SL, -3);   // thread_table[thread] = nil in `SL`
	lua_pop(SL, 1);       // Remove the thread table from the stack
}


static void t_resume(T* t) {
	// get Lua "thread" for task
	lua_State* L = t_L(t);
	assert(t->s != NULL);
	assert(t->s->L != L);

	// switch from l_main to task
	// nargs: number of values on T's stack to be returned from 'yield' inside task.
	// nres:  number of values passed to 'yield' by task.
	int nargs = t->resume_nres, nres;

	trace("resume " T_ID_F " nargs=%d", t_id(t), nargs);
	int status = lua_resume(L, t->s->L, nargs, &nres);

	// check if task exited
	if UNLIKELY(status != LUA_YIELD)
		return t_finalize(t, status);

	// discard results
	return lua_pop(L, nres);
}


// t_yield schedules t for immediate resumption,
// suspends t and switches control back to s_schedule loop.
// (Must only be called from a task, never l_main.)
static int t_yield(T* t, int nresults) {
	trace(T_ID_F " yield", t_id(t));
	if UNLIKELY(!s_runq_put(t->s, t))
		return luaL_error(t_L(t), "out of memory");
	return lua_yield(t_L(t), nresults);
}


// s_spawntask creates a new task, which function should be on stack L (the spawner thread),
// and adds it to the runq as runnext.
// L should be the Lua thread that initiated the spawn (S's L or a task's L.)
// Returns 1 on success with Lua "thread" on L stack, or 0 on failure with Lua error set in L.
static int s_spawntask(S* s, lua_State* L, T* nullable parent) {
	// create Lua "thread".
	// Note: See coroutine.create in luaB_cocreate (lcorolib.c).
	if UNLIKELY(lua_type(L, 1) != LUA_TFUNCTION)
		return luaL_typeerror(L, 1, lua_typename(L, LUA_TFUNCTION));
	lua_State* NL = lua_newthread(L);
	lua_pushvalue(L, 1); // move function to top of caller stack
	lua_xmove(L, NL, 1); // move function from L to NL

	// Hold on to a GC reference to thread, e.g. "S.L.thread_table[thread] = true"
	// Note: this is quite complex of a Lua stack operation. In particular lua_pushthread is
	// gnarly as it pushes the thread onto its own stack, which we then move over to S's stack.
	// If we get things wrong we (at best) get a memory bus error in asan with no stack trace,
	// making this tricky to debug.
	lua_rawgetp(s->L, LUA_REGISTRYINDEX, &g_thread_table_key);
	lua_pushthread(NL);     // Push thread as key onto its own stack
	lua_xmove(NL, s->L, 1); // Move the thread object to the `L` state
	lua_pushboolean(s->L, 1);  // "true"
	lua_rawset(s->L, -3);  // thread_table[thread] = true
	lua_pop(s->L, 1);  // Remove table from stack

	// initialize T struct (which lives in the LUA_EXTRASPACE header of lua_State)
	T* t = L_t(NL);
	t->s = s;
	t->parent = parent;
	t->next_sibling = NULL;
	t->first_child = NULL;
	t->resume_nres = 0;
	t->is_live = 1;

	// setup t to be run next by schedule
	if UNLIKELY(!s_runq_put_runnext(s, t)) {
		lua_closethread(NL, L);
		return luaL_error(L, "out of runq space (TODO: grow)");
	}

	// add t as a child of parent
	if (parent)
		t_add_child(parent, t);

	s->nlive++;

	if (parent) {
		trace(T_ID_F " spawns " T_ID_F, t_id(parent), t_id(t));
	} else {
		trace("spawn main task " T_ID_F, t_id(t));
	}

	return 1;
}


// s_iopoll_wake is called by iopoll_poll when waiting tasks should be woken up
int s_iopoll_wake(S* s, IODesc** dv, u32 count) {
	int err = 0;
	for (u32 i = 0; i < count; i++) {
		IODesc* d = dv[i];

		T* t = d->t;

		// skip duplicates
		if (t == NULL)
			continue;

		// "take" t from d, to make sure that we don't attempt to wake a task from an event
		// that occurs when the task is running.
		d->t = NULL;

		trace(T_ID_F " woken by iopoll" , t_id(t));

		// TODO: check if t is already on runq and don't add it if so.
		// For now, use an assertion (note: this is likely to happen)
		assert(t->is_live);
		assert(s_runq_find(s, t) == -1);

		if (!s_runq_put(s, t))
			err = -ENOMEM;
	}
	return err;
}


// l_iopoll_wait suspends a task that is waiting for file descriptor events.
// Once 'd' is told that there are changes, t is woken up which causes the continuation 'cont'
// to be invoked on the unchanged stack in t_resume before execution is handed back to the task.
inline static int l_iopoll_wait(
	lua_State* L, T* t, IODesc* d, int(*cont)(lua_State*,int,IODesc*))
{
	d->t = t;
	return lua_yieldk(L, 0, (intptr_t)d, (int(*)(lua_State*,int,lua_KContext))cont);
}


static int s_shutdown(S* s) {
	trace("shutdown " S_ID_F, s_id(s));

	// clear timers before GC to avoid costly (and useless) timers_remove
	s->timers.len = 0;

	// run GC to ensure pending IODesc close before S goes away
	lua_gc(s->L, LUA_GCCOLLECT);

	iopoll_shutdown(&s->iopoll);
	array_free((struct Array*)&s->timers);
	tls_s = NULL;
	return 0;
}


static int s_schedule(S* s) {
	// Scheduler loop: find a runnable task and execute it.
	// Stops when all tasks have finished or an error occurred.

	// TODO: track min timer deadline in this loop and make sure to iopoll when reaching that
	// deadline, else we might never get to timers if tasks are spawned at the same rate as we
	// are running them.

	static int debug_nwait = 0;
	while (s->nlive > 0) {
		// Look for a ready task in timers and runq.
		// s_timers_check may run non-task timers, like IODesc deadlines.
		T* t = s_timers_check(s);
		if (!t)
			t = s_runq_get(s);
		if (t) {
			trace(T_ID_F " taken from runq", t_id(t));
			debug_nwait = 0; // XXX
			t_resume(t);
			continue;
		}

		// wait for events
		DTime deadline = s->timers.len > 0 ? s->timers.v[0].when : (DTime)-1;
		trace("iopoll_poll until deadline %llu", deadline);
		int n = iopoll_poll(&s->iopoll, deadline);
		if UNLIKELY(n < 0) {
			logerr("internal I/O error: %s", strerror(-n));
			l_errno_error(s->L, -n);
			return 0;
		}

		// check runq & allt again
		// debug check for logic errors (TODO: remove this when I know scheduler works)
		if (n == 0 && ++debug_nwait == 4) {
			dlog("s->nlive %u", s->nlive);
			assert(!"XXX");
		}
	}
	return s_shutdown(s);
}


static int l_main(lua_State* L) {
	if UNLIKELY(tls_s != NULL)
		return luaL_error(L, "S already active");

	// create scheduler for this OS thread & Lua context
	S* s = &(S){
		.L = L,
		.runq_cap = 8,
	};
	s->timers.v = s->timers_storage;
	s->timers.cap = countof(s->timers_storage);
	tls_s = s;

	// allocate initial runq array
	s->runq = malloc(sizeof(*s->runq) * s->runq_cap);
	if (!s->runq)
		return l_errno_error(L, ENOMEM);

	// open platform I/O facility
	int err = iopoll_init(&s->iopoll, s);
	if (err) {
		dlog("error: iopoll_init: %s", strerror(-err));
		return l_errno_error(L, -err);
	}

	// create threads table (for GC refs)
	lua_createtable(L, 0, /*estimated common-case lowball Lua-thread count*/8);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &g_thread_table_key);

	// create main task
	int nres = s_spawntask(s, L, NULL);
	if UNLIKELY(nres == 0) // error
		return 0;

	// discard thread on result stack
	lua_pop(L, 1);

	// enter scheduling loop
	return s_schedule(s);
}


static int l_yield(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	int nargs = lua_gettop(L);
	return t_yield(t, nargs);
}


static int l_spawn(lua_State* L) {
	T* t = REQUIRE_TASK(L);

	t->resume_nres = s_spawntask(t->s, L, t);
	if UNLIKELY(t->resume_nres == 0)
		return 0;

	// suspend the calling task, switching control back to s_schedule loop
	return t_yield(t, 0);
}


static int l_sleep(lua_State* L) {
	T* t = REQUIRE_TASK(L);

	DTimeDuration delay = luaL_checkinteger(L, 1);
	if (delay < 0)
		return luaL_error(L, "negative timeout");
	DTime deadline = DTimeNow() + delay;

	trace(T_ID_F " sleep (%llu ns)", t_id(t), delay);

	return l_errno_error(L, ENOSYS);
}


static int l_socket(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	int domain = luaL_checkinteger(L, 1); // PF_ constant
	int type = luaL_checkinteger(L, 2);   // SOCK_ constants
	int protocol = 0;

	#if defined(__linux__)
		type |= SOCK_NONBLOCK | SOCK_CLOEXEC;
	#endif

	int fd = socket(domain, type, protocol);
	if UNLIKELY(fd < 0)
		return l_errno_error(L, errno);

	// Ask the OS to check the connection once in a while (stream sockets only)
	if (type == SOCK_STREAM) {
		int optval = 1;
		if UNLIKELY(setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) < 0)
			dlog("warning: failed to set SO_KEEPALIVE on socket: %s", strerror(errno));
	}

	// Set file descriptor to non-blocking mode..
	// Not needed on linux where we pass SOCK_NONBLOCK when creating the socket.
	#if !defined(__linux__)
		int flags = fcntl(fd, F_GETFL, 0);
		if UNLIKELY(fcntl(fd, F_SETFL, flags | O_NONBLOCK)) {
			int err = errno;
			close(fd);
			return luaL_error(L, "socket: failed to set O_NONBLOCK: %s", strerror(err));
		}
	#endif

	IODesc* d = l_iodesc_create(L);
	d->fd = fd;

	int err = iopoll_open(&t->s->iopoll, d);
	if UNLIKELY(err) {
		close(fd);
		d->fd = -1;
		return l_errno_error(L, -err);
	}

	return 1;
}


static int l_connect_cont(lua_State* L, __attribute__((unused)) int ltstatus, IODesc* d) {
	T* t = L_t(L);
	trace(T_ID_F, t_id(t));

	dlog("d events=%s nread=%lld nwrite=%lld",
	     d->events == 'r'+'w' ? "rw" : d->events == 'r' ? "r" : d->events == 'w' ? "w" : "0",
	     d->nread, d->nwrite);

	// check for connection failure on darwin, which sets EOF, which we propagate as r+w
	#if defined(__APPLE__)
	if UNLIKELY(d->events == 'r'+'w' && d->nread >= 0 && d->nwrite >= 0) {
		int error = 0;
		socklen_t len = sizeof(error);
		if (getsockopt(d->fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0) {
			if (error != 0)
				return l_errno_error(L, error);
		}
	}
	#endif

	// check for error
	if UNLIKELY(d->nread < 0 || d->nwrite < 0) {
		int err;
		if ((d->events & 'r') == 'r' && d->nread < 0) {
			err = (int)-d->nread;
		} else {
			err = (int)-d->nwrite;
		}
		if (err > 0)
			return l_errno_error(L, err);
	}

	return 0;
}


// connect(fd)
static int l_connect(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	IODesc* d = l_iodesc_check(L, 1);

	// construct network address
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(12345);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (connect(d->fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
		// connect succeeded immediately, without blocking
		return 0;
	}

	if (errno == EINPROGRESS) {
		dlog("wait for connect");
		return l_iopoll_wait(L, t, d, l_connect_cont);
	}

	// error
	int err = errno;
	close(d->fd);
	d->fd = -1;
	return l_errno_error(L, err);
}


static int l_read_cont(lua_State* L, __attribute__((unused)) int ltstatus, IODesc* d) {
	// check for error
	if UNLIKELY(d->nread < 0)
		return l_errno_error(L, (int)-d->nread);

	// check for EOF
	if UNLIKELY(d->nread == 0) {
		lua_pushinteger(L, 0);
		return 1;
	}

	Buf* buf = l_buf_check(L, 2);

	#if 1 // version of the code that resizes buffer if needed
		// TODO: check to see if there's a 3rd argument with explicit read limit
		usize readlim = d->nread;
		if (buf_reserve(buf, readlim) == NULL)
			return l_errno_error(L, ENOMEM);
	#else // version of the code that only reads what can fit in buf
		// If there's no available space in buf (i.e. buf->cap - buf->len == 0) then read() will
		// return 0 which the caller should interpret as EOF. However, a caller that does not stop
		// calling read when it returns 0 may end up in an infinite loop.
		// For that reason we treat "read nothing" as an error.
		if UNLIKELY(buf->cap - buf->len == 0)
			return luaL_error(L, "no space in buffer to read into");
		// usize readlim = MIN((usize)d->nread, buf->cap - buf->len);
	#endif

	// read
	//dlog("read bytes[%zu] (<= %zu B)", buf->len, readlim);
	ssize_t len = read(d->fd, &buf->bytes[buf->len], readlim);

	// check for error
	if UNLIKELY(len < 0) {
		if (errno == EAGAIN)
			return l_iopoll_wait(L, L_t(L), d, l_read_cont);
		return l_errno_error(L, errno);
	}

	// update d & buf
	if (len == 0) { // EOF
		d->nread = 0;
	} else {
		d->nread -= MIN(d->nread, (i64)len);
		buf->len += (usize)len;
	}

	// TODO: if there's a 3rd argument 'nread' that is >0, l_iopoll_wait again if
	// d->nread < nread so that "read" only returns after reading 'nread' bytes.

	lua_pushinteger(L, len);
	return 1;
}


// fun read(fd FD, buf Buf, nbytes uint = 0)
static int l_read(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	IODesc* d = l_iodesc_check(L, 1);
	// TODO: if there's a 3rd argument 'nread' that is >0, wait unless d->nread >= nread.
	// if there's nothing available to read, we will have to wait for it
	if (d->nread == 0)
		return l_iopoll_wait(L, t, d, l_read_cont);
	return l_read_cont(L, 0, d);
}


static int l_taskblock_begin(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	trace("taskblock_begin");
	// TODO
	return t_yield(t, 0);
}


static int l_taskblock_end(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	trace("taskblock_end");
	return t_yield(t, 0);
}


static const luaL_Reg dew_lib[] = {
	{"intscan", l_intscan},
	{"intfmt", l_intfmt},
	{"intconv", l_intconv},
	{"errstr", l_errstr},
	{"time", l_time},

	// "old" runtime API
	{"runloop_run", l_runloop_run},
	{"runloop_add_timeout", l_runloop_add_timeout},
	{"runloop_add_interval", l_runloop_add_interval},

	// "new" runtime API
	{"main", l_main},
	{"spawn", l_spawn},
	{"yield", l_yield},
	{"sleep", l_sleep},
	{"socket", l_socket},
	{"connect", l_connect},
	{"read", l_read},
	{"buf_alloc", l_buf_alloc},
	{"buf_resize", l_buf_resize},
	{"buf_str", l_buf_str},
	{"timer_start", l_timer_start},
	{"taskblock_begin", l_taskblock_begin},
	{"taskblock_end", l_taskblock_end},

	// wasm experiments
	#ifdef __wasm__
	{"ipcrecv", l_ipcrecv},
	{"ipcrecv_co", l_ipcrecv_co},
	{"iowait", l_iowait},
	#endif

	{NULL, NULL} // Sentinel
};

int luaopen_runtime(lua_State* L) {
	#ifdef __wasm__
	g_L = L; // FIXME!
	#endif

	luaL_newlib(L, dew_lib);

	// IODesc
	luaL_newmetatable(L, "FD");
	lua_pushcfunction(L, l_iodesc_gc);
	lua_setfield(L, -2, "__gc");
	lua_rawsetp(L, LUA_REGISTRYINDEX, &g_iodesc_luatabkey);

	// Buf
	luaL_newmetatable(L, "Buf");
	lua_pushcfunction(L, l_buf_gc);
	lua_setfield(L, -2, "__gc");
	lua_rawsetp(L, LUA_REGISTRYINDEX, &g_buf_luatabkey);

	// Timer
	luaL_newmetatable(L, "Timer");
	lua_pushcfunction(L, l_timer_gc);
	lua_setfield(L, -2, "__gc");
	lua_rawsetp(L, LUA_REGISTRYINDEX, &g_timer_luatabkey);

	// export libc & syscall constants
	#define _(NAME) \
		lua_pushinteger(L, NAME); \
		lua_setfield(L, -2, #NAME);
	// protocol families
	_(PF_LOCAL)  // Host-internal protocols, formerly called PF_UNIX
	_(PF_INET)   // Internet version 4 protocols
	_(PF_INET6)  // Internet version 6 protocols
	_(PF_ROUTE)  // Internal Routing protocol
	_(PF_VSOCK)  // VM Sockets protocols
	// socket types
	_(SOCK_STREAM)
	_(SOCK_DGRAM)
	_(SOCK_RAW)
	// address families
	#undef _

	// export ERR_ constants
	#define _(NAME, ERRNO, ...) \
		lua_pushinteger(L, NAME); \
		lua_setfield(L, -2, #NAME);
	FOREACH_ERR(_)
	// note: not including ERR_ERROR on purpose as it's "unknown error"
	#undef _

	int err = RunloopCreate(&g_main_runloop);
	if (err)
		logerr("RunloopCreate failed: %s", strerror(-err));

	return 1;
}


// #include "lua/src/lstate.h"
// __attribute__((constructor)) static void init() {
// 	printf("sizeof(lua_State): %zu B\n", sizeof(T) + sizeof(lua_State));
// }
