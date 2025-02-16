#include "runtime.h"

#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <sys/socket.h>
#include <netinet/in.h>

#include <pthread.h> // TODO: move into platform_SYS.c
#include <stdatomic.h> // TODO: move into platform_SYS.c


#define TRACE_SCHED
// #define TRACE_SCHED_RUNQ
#define TRACE_SCHED_WORKER

#ifdef TRACE_SCHED
	#define trace_sched(fmt, ...) \
		dlog("\e[1;34m%-15.15s S%-2u│\e[0m " fmt, __FUNCTION__, tls_s_id, ##__VA_ARGS__)
#else
	#define trace_sched(fmt, ...) ((void)0)
#endif

#ifdef TRACE_SCHED_RUNQ
	#define trace_runq(fmt, ...) \
		dlog("\e[1;35m%-15.15s S%-2u│\e[0m " fmt, __FUNCTION__, tls_s_id, ##__VA_ARGS__)
#else
	#define trace_runq(fmt, ...) ((void)0)
#endif

#ifdef TRACE_SCHED_WORKER
	#define trace_worker(fmt, ...) ( \
		tls_s_id && tls_w_id ? dlog("\e[1;32m%-15.15s W%-2u│\e[0m S%-2u " fmt, \
		                            __FUNCTION__, tls_w_id, tls_s_id, ##__VA_ARGS__) : \
		tls_w_id ?             dlog("\e[1;32m%-15.15s W%-2u│\e[0m " fmt, \
		                            __FUNCTION__, tls_w_id, ##__VA_ARGS__) : \
		                       dlog("\e[1;32m%-15.15s S%-2u│\e[0m " fmt, \
		                            __FUNCTION__, tls_s_id, ##__VA_ARGS__) \
	)
#else
	#define trace_worker(fmt, ...) ((void)0)
#endif


// LUA_EXTRASPACE is defined in lua/src/luaconf.h
static_assert(sizeof(T) == LUA_EXTRASPACE, "");


static u8 g_reftabkey;           // table where objects with compex lifetime are stored, to avoid GC
static u8 g_iodesc_luatabkey;    // IODesc object prototype
static u8 g_buf_luatabkey;       // Buf object prototype
static u8 g_timerobj_luatabkey;  // Timer object prototype
static u8 g_workerobj_luatabkey; // Worker object prototype

// tls_s holds S for the current thread
static _Thread_local S* tls_s = NULL;

#if defined(TRACE_SCHED) || defined(TRACE_SCHED_RUNQ) || defined(TRACE_SCHED_WORKER)
	static _Atomic(u32)      tls_s_idgen = 1;
	static _Thread_local u32 tls_s_id = 0;
#endif

#if defined(TRACE_SCHED_WORKER)
	static _Atomic(u32)      tls_w_idgen = 1;
	static _Thread_local u32 tls_w_id = 0;
#endif


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


static int l_errno_error(lua_State* L, int err_no) { // note: always returns 0
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


// l_copy_values copies (by reference) n values from stack src to stack dst.
// This is similar to lua_xmove but copies instead of moves.
// Note: src & dst must belong to the same OS thread (not different Workers.)
static void l_copy_values(lua_State* src, lua_State* dst, int n) {
    assertf(lua_gettop(src) >= n, "src has at least n values at indices 1 .. n");
    // make copies of the arguments onto the top of src stack
    for (int i = 1; i <= n; i++)
        lua_pushvalue(src, i);
    // move them to dst
    lua_xmove(src, dst, n);
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



// ———————————————————————————————————————————————————————————————————————————————————————————
// BEGIN wasm ipc experiment

#ifdef __wasm__

extern lua_State* g_L; // dew.c

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

// END wasm ipc experiment
// ———————————————————————————————————————————————————————————————————————————————————————————


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
	dlog("%s: invalid call by user: %s", __FUNCTION__, msg);
	return luaL_error(L, msg);
}


static T* nullable l_check_task(lua_State* L, int idx) {
	lua_State* other_L = lua_tothread(L, idx);
	if LIKELY(other_L) {
		T* other_t = L_t(other_L);
		if LIKELY(other_t->s)
			return other_t;
	}
	luaL_typeerror(L, idx, "Task");
	return NULL;
}


#define REQUIRE_TASK(L) ({ \
	T* __t = L_t(L); \
	if UNLIKELY(__t->s == NULL) \
		return l_error_not_a_task(L, __t); \
	__t; \
})


static IODesc* nullable l_iodesc_check(lua_State* L, int idx) {
	return l_uobj_check(L, idx, &g_iodesc_luatabkey, "FD");
}

static Buf* nullable l_buf_check(lua_State* L, int idx) {
	return l_uobj_check(L, idx, &g_buf_luatabkey, "Buf");
}



// typedef enum LuaThreadStatus : u8 {
// 	LuaThreadStatus_RUN,   // running
// 	LuaThreadStatus_DEAD,  // dead
// 	LuaThreadStatus_YIELD, // suspended
// 	LuaThreadStatus_NORM,  // normal
// } LuaThreadStatus;
// // s_lua_thread_status returns the status of a task.
// // Based on auxstatus from lcorolib.c
// static LuaThreadStatus s_lua_thread_status(S* s, T* t) {
// 	if (s->L == t_L(t))
// 		return LuaThreadStatus_RUN;
// 	switch (lua_status(t_L(t))) {
// 		case LUA_YIELD:
// 			return LuaThreadStatus_YIELD;
// 		case LUA_OK: {
// 			lua_Debug ar;
// 			if (lua_getstack(t_L(t), 0, &ar))  /* does it have frames? */
// 				return LuaThreadStatus_NORM;  /* it is running */
// 			else if (lua_gettop(t_L(t)) == 0)
// 				return LuaThreadStatus_DEAD;
// 			else
// 				return LuaThreadStatus_YIELD;  /* initial state */
// 		}
// 		default:  /* some error occurred */
// 			return LuaThreadStatus_DEAD;
// 	}
// }


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


static bool buf_append(Buf* buf, const void* data, usize len) {
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


static int buf_append_luafun(Buf* buf, lua_State* L, bool strip_debuginfo) {
	if (!buf_reserve(buf, 512))
		return -ENOMEM;
	if (lua_dump(L, buf_append_luafun_writer, buf, strip_debuginfo))
		return -EINVAL;
	return 0;
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


static int l_buf_str(lua_State* L) {
	Buf* buf = l_buf_check(L, 1);
	// TODO: optional second argument for how to encode the data, e.g. hex, base64
	lua_pushlstring(L, (char*)buf->bytes, buf->len);
	return 1;
}


static InboxMsg* nullable inbox_pop(Inbox* inbox) {
	return fifo_pop(&inbox->fifo, sizeof(*inbox->entries));
}


static InboxMsg* nullable inbox_add(Inbox** inboxp) {
	const u32 initcap = 8;  // messages to allocate space for up front
	const u32 maxcap  = 64; // messages we can send() without blocking

	// setup inbox if needed
	if UNLIKELY(*inboxp == NULL) {
		*inboxp = (Inbox*)fifo_alloc(initcap, sizeof(*(*inboxp)->entries));
		if UNLIKELY(!*inboxp)
			return NULL;
	}

	return fifo_push((FIFO**)inboxp, sizeof(*(*inboxp)->entries), maxcap);
}


typedef int(*TaskContinuation)(lua_State* L, int l_thrd_status, void* arg);


static const char* t_status_str(u8 status) {
	switch ((enum TStatus)status) {
		case T_READY:       return "T_READY";
		case T_RUNNING:     return "T_RUNNING";
		case T_WAIT_IO:     return "T_WAIT_IO";
		case T_WAIT_RECV:   return "T_WAIT_RECV";
		case T_WAIT_TASK:   return "T_WAIT_TASK";
		case T_WAIT_WORKER: return "T_WAIT_WORKER";
		case T_DEAD:        return "T_DEAD";
	}
	return "?";
}


inline static int t_suspend(T* t, u8 tstatus, void* nullable arg, TaskContinuation nullable cont) {
	assertf(tstatus != T_READY || tstatus != T_RUNNING || tstatus != T_DEAD,
	        "%s", t_status_str(tstatus));
	trace_sched(T_ID_F " %s", t_id(t), t_status_str(tstatus));
	t->status = tstatus;
	return lua_yieldk(t_L(t), 0, (intptr_t)arg, (int(*)(lua_State*,int,lua_KContext))cont);
}


// t_iopoll_wait suspends a task that is waiting for file descriptor events.
// Once 'd' is told that there are changes, t is woken up which causes the continuation 'cont'
// to be invoked on the unchanged stack in t_resume before execution is handed back to the task.
inline static int t_iopoll_wait(T* t, IODesc* d, int(*cont)(lua_State*,int,IODesc*)) {
	d->t = t;
	return t_suspend(t, T_WAIT_IO, d, (TaskContinuation)cont);
	// return lua_yieldk(L, 0, (intptr_t)d, (int(*)(lua_State*,int,lua_KContext))cont);
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


static void timers_remove_at(TimerPQ* timers, u32 i) {
	// dlog("remove timers[%u] %p (timers.len=%u)", i, timers->v[i].timer, timers->len);
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


static void timers_remove(TimerPQ* timers, Timer* timer) {
	if (timers->len == 0 || timer->when == (DTime)-1)
		return;
	// linear search to find index of timer
	u32 i;
	if (timer->when > timers->v[timers->len / 2].when) {
		for (i = timers->len; i--;) {
			if (timers->v[i].timer == timer)
				return timers_remove_at(timers, i);
		}
	} else {
		for (i = 0; i < timers->len; i++) {
			if (timers->v[i].timer == timer)
				return timers_remove_at(timers, i);
		}
	}
	dlog("warning: timer %p not found!", timer);
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
		// Note: 'min when' changed to 'when'
	}
	// timers_dlog(timers);
	return true;
}


inline static void timer_release(Timer* timer) {
	assert(timer->nrefs > 0);
	if (--timer->nrefs == 0) {
		// dlog("*** free timer %p ***", timer);
		free(timer);
	}
}


// t_cancel_timers finds and cancels all pending timers started by t.
static void t_cancel_timers(T* t) {
	trace_sched("canceling %u timers started by " T_ID_F, t->ntimers, t_id(t));
	// Linear search to find matching timers.
	//
	// Note: Tasks are not expected to exit with timers still running,
	// so it's okay that this is slow.
	//
	// TODO: this could be made much more efficient.
	// Currently we are removing a timer one by one, sifting through the heap
	// and resetting scanning everytime we find one.
	//
	TimerPQ* timers = &t->s->timers;
	u32 i;
	for (i = timers->len; i-- && t->ntimers > 0;) {
		if (timers->v[i].timer->arg == t) {
			Timer* timer = timers->v[i].timer;
			timers_remove_at(timers, i);
			timer->when = -1; // signals that the timer is dead
			timer_release(timer); // release internal reference
			t->ntimers--;
			i = timers->len; // reset loop index
		}
	}
	assert(t->ntimers == 0 || !"ntimers larger than matching timers");
}


static T* nullable s_timers_check(S* s) {
	if (s->timers.len == 0)
		return NULL;
	// const DTimeDuration leeway = 10 * D_TIME_MICROSECOND;
	// DTime now = DTimeNow() - leeway;
	DTime now = DTimeNow();
	while (s->timers.len > 0 && s->timers.v[0].when <= now) {
		Timer* timer = timers_remove_min(&s->timers);
		T* t = timer->f(timer, timer->arg);
		if (t == NULL) {
			now = DTimeNow();
		} else if (timer->period > 0) {
			// repeating timer
			//
			// Two different approaches to updating 'when':
			// 1. steady rythm, variable delay between wakeups,
			//    i.e. timer rings every when+period time.
			// 2. variable rythm, steady delay between wakeups,
			//    i.e. timer rings with period delay.
			// It's not clear to me which is better.
			// In tests, the two approaches yield identical results.
			// I also benchmarked with a go program (identical results across the board.)
			// Approach 1 seems the correct one from first principles and does not require a new
			// timestamp, so let's go with that one for now.
			timer->when += timer->period; // 1
			// timer->when = now + timer->period; // 2

			bool ok = timers_add(&s->timers, timer);
			assert(ok); // never need to grow memory
			return t;
		} else {
			// one-shot timer
			// Since timers are accessible in userland, they are reference counted.
			// We dereference the timer here rather than in timer->f since we need to access
			// the timer after f returns.
			timer->when = -1; // signals that timer is dead
			timer_release(timer);
			assert(t->ntimers > 0);
			t->ntimers--;
			return t;
		}
	}
	return NULL;
}


// TimerObj is a Lua-managed (GC'd) wrapper around a internally managed Timer.
// This allows a timer to be referenced by userland independently of its state.
typedef struct TimerObj {
	Timer* timer;
} TimerObj;


static TimerObj* nullable l_timerobj_check(lua_State* L, int idx) {
	return l_uobj_check(L, idx, &g_timerobj_luatabkey, "Timer");
}


static int l_timerobj_gc(lua_State* L) {
	TimerObj* obj = lua_touserdata(L, 1);
	// dlog("*** l_timerobj_gc %p ***", obj->timer);
	timer_release(obj->timer);
	return 0;
}


static T* nullable l_timer_done(Timer* timer, void* arg) {
	// dlog("*** l_timer_done %p ***", timer);
	T* t = arg;
	// deliver timer message to task's inbox
	InboxMsg* msg = inbox_add(&t->inbox);
	if LIKELY(msg != NULL) {
		memset(msg, 0, sizeof(*msg));
		msg->type = InboxMsgType_TIMER;
		return t; // wake t
	}

	// Inbox is full.
	//
	// In the case of a task calling send(), it should block until there's space in the
	// receiver's inbox.
	//
	// But in the case of a timer delivering a message ... how do we handle this safely?
	// We can't really block since that would block the entire scheduler...
	// Maybe when the timer starts we can somehow pre-allocate an inbox message.
	// For example, the Inbox could have a field 'npending' which is checked by
	// inbox_add when considering if the inbox is full or not.
	dlog("TODO: task inbox is full when timer rang");
	return NULL;
}


static Timer* nullable s_timer_start(
	S*             s,
	DTime          when,
	DTimeDuration  period,
	DTimeDuration  leeway,
	void* nullable f_arg,
	TimerF         f)
{
	// create timer
	Timer* timer = calloc(1, sizeof(Timer));
	if UNLIKELY(!timer)
		return NULL;
	timer->when = when;
	timer->period = period;
	timer->leeway = leeway;
	timer->nrefs = 1;
	timer->arg = f_arg;
	timer->f = f;

	// schedule timer
	if UNLIKELY(!timers_add(&s->timers, timer)) {
		free(timer);
		return NULL;
	}

	return timer;
}


// fun timer_start(when Time, period, leeway TimeDuration) Timer
static int l_timer_start(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	DTime when = luaL_checkinteger(L, 1);
	DTimeDuration period = luaL_checkinteger(L, 2);
	DTimeDuration leeway = luaL_checkinteger(L, 3);

	// increment task's "live timers" counter
	if UNLIKELY(++t->ntimers == 0) {
		t->ntimers--;
		return luaL_error(L, "too many concurrent timers (%u)", t->ntimers);
	}

	// create & schedule a timer
	Timer* timer = s_timer_start(t->s, when, period, leeway, t, l_timer_done);
	if UNLIKELY(!timer) {
		t->ntimers--;
		return l_errno_error(L, ENOMEM);
	}

	// create ref
	TimerObj* obj = lua_newuserdatauv(L, sizeof(TimerObj), 0);
	if UNLIKELY(!obj) {
		timers_remove(&t->s->timers, timer);
		free(timer);
		t->ntimers--;
		return 0;
	}
	timer->nrefs++;
	obj->timer = timer;
	lua_rawgetp(L, LUA_REGISTRYINDEX, &g_timerobj_luatabkey);
	lua_setmetatable(L, -2);

	// return timer object
	return 1;
}


// fun timer_update(timer Timer, when Time, period, leeway TimeDuration)
static int l_timer_update(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	TimerObj* obj = l_timerobj_check(L, 1);
	DTime when = luaL_checkinteger(L, 2);
	DTimeDuration period = luaL_checkinteger(L, 3);
	DTimeDuration leeway = luaL_checkinteger(L, 4);

	Timer* timer = obj->timer;

	if UNLIKELY(timer->when != (DTime)-1) {
		// Timer is still active.
		// Remove it and re-add it to uphold correct position in timers priority queue.
		timers_remove(&t->s->timers, timer);
		timer->when = when;
		timer->period = period;
		timer->leeway = leeway;
		if UNLIKELY(!timers_add(&t->s->timers, timer)) {
			timer_release(timer); // release our internal reference
			assert(t->ntimers > 0);
			t->ntimers--;
			return l_errno_error(L, ENOMEM);
		}
	} else {
		// Timer already expired.
		// This is basically the same as starting a new timer with timer_start.
		timer->when = when;
		timer->period = period;
		timer->leeway = leeway;
		// First, increment task's "live timers" counter
		if UNLIKELY(++t->ntimers == 0) {
			t->ntimers--;
			return luaL_error(L, "too many concurrent timers (%u)", t->ntimers);
		}
		if UNLIKELY(!timers_add(&t->s->timers, timer))
			return l_errno_error(L, ENOMEM);
		assert(timer->nrefs == 1); // should have exactly on ref (Lua GC)
		timer->nrefs++;
	}
	return 0;
}


// fun timer_stop(timer Timer)
static int l_timer_stop(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	TimerObj* obj = l_timerobj_check(L, 1);

	if (obj->timer->when == (DTime)-1)
		return 0; // already expired

	timers_remove(&t->s->timers, obj->timer);
	timer_release(obj->timer); // release our internal reference
	assert(t->ntimers > 0);
	t->ntimers--;
	return 0;
}


static T* l_sleep_done(Timer* timer, void* arg) {
	T* t = arg;
	return t; // wake t
}


// fun sleep(delay TimeDuration, leeway TimeDuration = -1)
// Sleep is a simplified case of timer_start.
// "sleep(123, 456)" is semantically equivalent to "timer_start(time()+123, 0, 456); recv()"
static int l_sleep(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	DTimeDuration delay = luaL_checkinteger(L, 1);
	if (delay < 0)
		return luaL_error(L, "negative timeout");
	DTime when = (DTimeNow() + delay) - D_TIME_MICROSECOND;
	int has_leeway;
	DTimeDuration leeway = lua_tointegerx(L, 2, &has_leeway);
	if (!has_leeway)
		leeway = -1;

	// trace
	if (leeway < 0) {
		trace_sched(T_ID_F " sleep %.3f ms", t_id(t), (double)delay / D_TIME_MILLISECOND);
	} else {
		trace_sched(T_ID_F " sleep %.3f ms (%.3f ms leeway)", t_id(t),
		            (double)delay / D_TIME_MILLISECOND,
		            (double)leeway / D_TIME_MILLISECOND);
	}

	// increment task's "live timers" counter
	if UNLIKELY(++t->ntimers == 0) {
		t->ntimers--;
		return luaL_error(L, "too many concurrent timers (%u)", t->ntimers);
	}

	// create & schedule a timer
	DTimeDuration period = 0;
	Timer* timer = s_timer_start(t->s, when, period, leeway, t, l_sleep_done);
	if UNLIKELY(!timer) {
		t->ntimers--;
		return l_errno_error(L, ENOMEM);
	}

	// return one result
	t->resume_nres = 1;

	return t_suspend(t, T_WAIT_IO, NULL, NULL);
	// return lua_yieldk(L, 0, 0, NULL);
}


static bool s_task_register(S* s, T* t) {
	T** tp = (T**)pool_entry_alloc(&s->tasks, &t->tid, sizeof(T*));
	if (tp != NULL)
		*tp = t;
	return tp != NULL;
}


static void s_task_unregister(S* s, T* t) {
	// optimization for main task (tid 1): skip work incurred by pool_entry_free
	if (t->tid != 1)
		pool_entry_free(s->tasks, t->tid);
}


inline static T* s_task(S* s, u32 tid) {
	T** tp = (T**)pool_entry(s->tasks, tid, sizeof(T*));
	return *tp;
}


static bool s_runq_put(S* s, T* t) {
	t->status = T_READY;
	const u32 runq_cap_limit = U32_MAX;
	T** tp = (T**)fifo_push((FIFO**)&s->runq, sizeof(*s->runq->entries), runq_cap_limit);
	if (tp == NULL)
		return false;
	*tp = t;
	trace_runq("runq put " T_ID_F, t_id(t));
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
	t->status = T_READY;
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
		T** tp = (T**)fifo_pop(&s->runq->fifo, sizeof(*s->runq->entries));
		if (tp == NULL) // empty
			return NULL;
		t = *tp;
		trace_runq("runq get " T_ID_F, t_id(t));
	}
	return t;
}


static void s_runq_remove(S* s, T* t) {
	if (s->runnext == t) {
		trace_runq("runq remove runnext " T_ID_F, t_id(t));
		s->runnext = NULL;
	} else if (s->runq->fifo.head != s->runq->fifo.tail) { // not empty
		RunQ* runq = s->runq;
		FIFO* fifo = &runq->fifo;
		u32 i = fifo->head;
		u32 count = (fifo->tail >= fifo->head) ?
					(fifo->tail - fifo->head) :
					(fifo->cap - fifo->head + fifo->tail);
		for (u32 j = 0; j < count; j++) {
			if (runq->entries[i] == t) {
				trace_runq("runq remove [%u] " T_ID_F, i, t_id(t));
				u32 next = (i + 1 == fifo->cap) ? 0 : i + 1;
				usize nbyte = (fifo->tail - next) * sizeof(*runq->entries);
				memmove(&runq->entries[i], &runq->entries[next], nbyte);
				fifo->tail = (fifo->tail == 0) ? fifo->cap - 1 : fifo->tail - 1;
				return;
			}
			i = (i + 1 == fifo->cap) ? 0 : i + 1;
		}
		trace_sched("warning: s_runq_remove(" T_ID_F ") called but task not found", t_id(t));
	}
}


static i32 s_runq_find(const S* s, const T* t) {
	if (s->runnext == t)
		return I32_MAX;

	if (s->runq->fifo.head != s->runq->fifo.tail) { // not empty
		RunQ* runq = s->runq;
		FIFO* fifo = &runq->fifo;
		u32 i = fifo->head;
		u32 count = (fifo->tail >= fifo->head) ?
					(fifo->tail - fifo->head) :
					(fifo->cap - fifo->head + fifo->tail);
		for (u32 j = 0; j < count; j++) {
			if (runq->entries[i] == t)
				return (i32)i;
			i = (i + 1 == fifo->cap) ? 0 : i + 1;
		}
	}

	return -1;
}


static void t_add_child(T* parent, T* child) {
	assertf(child->prev_sibling == 0, "should not be in a list");
	assertf(child->next_sibling == 0, "should not be in a list");
	// T uses a doubly-linked list of tids.
	// As an example, consider the following task tree:
	//    T1
	//      T2
	//      T3
	//      T4
	// where T1 is the parent, the list looks like this:
	//   T1 —> T4 <—> T3 <—> T2
	// i.e.
	//   T1  .first_child = 4
	//   T4  .prev_sibling = 0  .next_sibling = 3
	//   T3  .prev_sibling = 4  .next_sibling = 2
	//   T2  .prev_sibling = 3  .next_sibling = 0
	// so if we were to t_add_child(T1, T5) we'd get:
	//   T1 —> T5 <—> T4 <—> T3 <—> T2
	// i.e.
	//   T1  .first_child = 5
	//   T5  .prev_sibling = 0  .next_sibling = 4
	//   T4  .prev_sibling = 5  .next_sibling = 3
	//   T3  .prev_sibling = 4  .next_sibling = 2
	//   T2  .prev_sibling = 3  .next_sibling = 0
	//
	if (parent->first_child) {
		T* first_child = s_task(parent->s, parent->first_child);
		first_child->prev_sibling = child->tid;
	}
	child->next_sibling = parent->first_child;
	parent->first_child = child->tid;
}


static void t_remove_child(T* parent, T* t) {
	assertf(parent->tid == t->parent, "%u == %u", parent->tid, t->parent);
	t->parent = 0;
	trace_sched("remove child " T_ID_F " from " T_ID_F, t_id(t), t_id(parent));
	if (parent->first_child == t->tid) {
		// removing the most recently spawned task
		parent->first_child = t->next_sibling;
		if (parent->first_child) {
			T* first_child = s_task(parent->s, parent->first_child);
			first_child->prev_sibling = 0;
			t->next_sibling = 0;
		}
	} else {
		// removing a task in the middle or end of the list
		assert(t->prev_sibling > 0);
		T* prev_sibling = s_task(parent->s, t->prev_sibling);
		prev_sibling->next_sibling = t->next_sibling;
	}
}


static const char* l_fmt_error(lua_State* L) {
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
	lua_pop(L, 1);
	return msg2 ? msg2 : msg;
}


static void t_report_error(T* t, const char* context_msg) {
	lua_State* L = t_L(t);
	const char* msg = l_fmt_error(L);
	fprintf(stderr, "%s: ["T_ID_F "]\n%s\n", context_msg, t_id(t), msg);
}


static char* t_report_error_buf(T* t) {
	lua_State* L = t_L(t);
	const char* msg = l_fmt_error(L);
	return strdup(msg);
}


static void dlog_task_tree(const T* t, int level) {
	for (u32 child_tid = t->first_child; child_tid;) {
		T* child = s_task(t->s, child_tid);
		dlog("%*s" T_ID_F, level*4, "", t_id(child));
		dlog_task_tree(child, level + 1);
		child_tid = child->next_sibling;
	}
}


static void t_finalize(T* t, enum TDied died_how, u8 prev_tstatus);


static void t_stop(T* nullable parent, T* child) {
	if (parent) {
		trace_sched("stop " T_ID_F " by parent " T_ID_F, t_id(child), t_id(parent));
	} else {
		trace_sched("stop " T_ID_F, t_id(child));
	}

	// set status to DEAD _before_ calling lua_closethread to ensure that any to-be-closed
	// variables with __close metamethods that call into our API are properly handled.
	bool is_on_runq = child->status == T_READY;
	assert(child->status != T_DEAD);
	u8 prev_status = child->status;
	child->status = T_DEAD;

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
	lua_State* parent_L;
	if (parent) {
		parent_L = t_L(parent);
	} else {
		parent_L = child->s->L;
	}
	int status = lua_closethread(t_L(child), parent_L);
	if (status != LUA_OK) {
		dlog("warning: lua_closethread => %s", l_status_str(status));
		t_report_error(child, "Error in defer handler");
	}

	// remove from runq
	if (is_on_runq && !child->s->isclosed)
		s_runq_remove(child->s, child);

	// // finalize as "runtime error"
	// lua_pushstring(t_L(child), "parent task exited");
	// return t_finalize(child, LUA_ERRRUN);

	// finalize as "clean exit"
	child->resume_nres = 0;
	return t_finalize(child, TDied_STOP, prev_status);
}


static void t_stop_r(T* parent, T* child) {
	if (child->next_sibling)
		t_stop_r(parent, s_task(parent->s, child->next_sibling));
	if (child->first_child)
		t_stop_r(child, s_task(parent->s, child->first_child));
	return t_stop(parent, child);
}


// t_gc is called by Lua's luaE_freethread when a task is GC'd, thus it must not be 'static'
__attribute__((visibility("hidden")))
void t_gc(lua_State* L, T* t) {
	trace_sched(T_ID_F " GC", t_id(t));
	// remove from S's 'tasks' registry, effectively invalidating tid
	s_task_unregister(t->s, t);
}


inline static void t_retain(T* t) {
	t->nrefs++;
}


static void t_release(T* t) {
	if (--t->nrefs != 0)
		return;
	trace_sched(T_ID_F " release", t_id(t));
	// remove GC ref to allow task (Lua thread) to be garbage collected
	lua_State* TL = t_L(t);
	lua_State* SL = t->s->L;
	lua_rawgetp(SL, LUA_REGISTRYINDEX, &g_reftabkey); // put ref table on stack
	lua_pushthread(TL);   // Push the thread onto its own stack
	lua_xmove(TL, SL, 1); // Move the thread object to the `SL` state
	lua_pushnil(SL);      // Use `nil` to remove the key
	lua_rawset(SL, -3);   // reftab[thread] = nil in `SL`
	lua_pop(SL, 1);       // Remove the thread table from the stack
}


static void t_remove_from_waiters(T* t) {
	S* s = t->s;

	u32 next_waiter_tid = t->info.wait_task.next_tid;
	u32 wait_tid = t->info.wait_task.wait_tid;
	assertf(!pool_entry_isfree(s->tasks, wait_tid), "%u", wait_tid);
	T* other_t = s_task(s, wait_tid);

	// common case is head of list
	if (other_t->waiters == t->tid) {
		other_t->waiters = next_waiter_tid;
		return;
	}

	// find in middle or end of list
	u32 next_tid = other_t->waiters;
	while (next_tid > 0) {
		T* waiter_t = s_task(s, next_tid);
		assert(waiter_t->status == T_WAIT_TASK);
		assert(waiter_t->info.wait_task.wait_tid == wait_tid);
		next_tid = waiter_t->info.wait_task.next_tid;
		if (next_tid == t->tid) {
			waiter_t->info.wait_task.next_tid = next_waiter_tid;
			return;
		}
	}

	dlog("warning: %s " T_ID_F ": not found", __FUNCTION__, t_id(t));
}


static void worker_wake_waiters(Worker* w) {
	S* s = w->parent->s;
	for (u32 tid = w->waiters; tid > 0;) {
		T* waiting_t = s_task(s, tid);
		assert(waiting_t->status == T_WAIT_WORKER);
		trace_sched("wake " T_ID_F " waiting on Worker %p", t_id(waiting_t), w);

		// put waiting task on (priority) runq
		bool ok;
		if (tid == w->waiters) {
			ok = s_runq_put_runnext(s, waiting_t);
		} else {
			ok = s_runq_put(s, waiting_t);
		}
		if UNLIKELY(!ok)
			dlog("failed to allocate memory");

		tid = waiting_t->info.wait_worker.next_tid;
	}
	w->waiters = 0;
}


static void t_wake_waiters(T* t) {
	S* s = t->s;
	for (u32 tid = t->waiters; tid > 0;) {
		T* waiting_t = s_task(s, tid);
		assert(waiting_t->status == T_WAIT_TASK);
		trace_sched("wake " T_ID_F " waiting on " T_ID_F, t_id(waiting_t), t_id(t));

		// put waiting task on (priority) runq
		bool ok;
		if (tid == t->waiters) {
			ok = s_runq_put_runnext(s, waiting_t);
		} else {
			ok = s_runq_put(s, waiting_t);
		}
		if UNLIKELY(!ok) {
			dlog("failed to allocate memory");
			t_stop(t, waiting_t);
		}

		tid = waiting_t->info.wait_task.next_tid;
	}
	t->waiters = 0;
}


static Worker* s_worker(S* s) {
	assert(s->isworker);
	return (void*)s - offsetof(Worker, s);
}


static void t_finalize(T* t, enum TDied died_how, u8 prev_tstatus) {
	S* s = t->s;

	// Note: t_stop updates t->status before calling t_finalize. For that reason we can't trust
	// the current value of t->status in here but must use prev_tstatus.
	trace_sched(T_ID_F " exited (died_how=%d)", t_id(t), died_how);
	if UNLIKELY(died_how == TDied_ERR) {
		// if this is the main task, set s->exiterr
		if (t->tid == 1)
			s->exiterr = true;

		// one error as the final result value
		t->resume_nres = 1;

		// unless there are tasks waiting for this task, report the error as unhandled
		if (t->waiters == 0) {
			if (s->isworker && t->tid == 1) {
				// main task of a worker; only report error if no task is waiting for worker
				Worker* w = s_worker(s);
				if (atomic_load_explicit(&w->waiters, memory_order_acquire)) {
					free(w->errdesc); // just in case...
					w->errdesc = t_report_error_buf(t);
				} else {
					t_report_error(t, "Uncaught error in worker");
				}
			} else {
				t_report_error(t, "Uncaught error");
			}
		} else {
			#ifdef DEBUG
			char* s = t_report_error_buf(t);
			dlog("warning: %s\n(Error not reported because a task is awaiting this task)", s);
			free(s);
			#endif
		}
	}
	t->status = T_DEAD;

	// decrement "live tasks" counter
	if (s->nlive == 0) {
		dlog("error: s->nlive==0 while task " T_ID_F " is still alive in " S_ID_F,
		     t_id(t), s_id(t->s));
		assert(s->nlive > 0);
	}
	assert(s->nlive > 0);
	s->nlive--;

	// dlog("task tree for " T_ID_F ":", t_id(t)); dlog_task_tree(t, 1);

	// cancel associated waiting state
	if (prev_tstatus == T_WAIT_TASK) {
		// Task was stopped while waiting for another task.
		// Remove task from list of waiters of target task.
		t_remove_from_waiters(t);
	}

	// stop child tasks
	if (t->first_child)
		t_stop_r(t, s_task(s, t->first_child));

	// wake any tasks waiting for this task to exit
	if (t->waiters)
		t_wake_waiters(t);

	// if S is supposed to exit() when done, do that now if this is the last task
	if (s->doexit && s->nlive == 0 && s->workers == NULL) {
		trace_sched("exit(%d)", (int)s->exiterr);
		return exit(s->exiterr);
	}

	// stop any still-running timers (optimization: skip when S is shutting down)
	if UNLIKELY(t->ntimers && !s->isclosed)
		t_cancel_timers(t);

	if (t->parent) {
		// remove task from parent's list of children
		T* parent = s_task(s, t->parent);
		t_remove_child(parent, t);
	} else {
		assertf(t->waiters == 0, "cannot wait for the main task");
	}

	// set info.dead.how
	t->info.dead.how = died_how;

	// release S's "live" reference to the task
	t_release(t);
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
	t->resume_nres = 0;
	t->status = T_RUNNING;

	trace_sched("resume " T_ID_F " nargs=%d", t_id(t), nargs);
	int status = lua_resume(L, t->s->L, nargs, &nres);

	// check if task exited
	if UNLIKELY(status != LUA_YIELD) {
		t->resume_nres = nres > 0xff ? 0xff : nres;
		u8 died_how = (status == LUA_OK) ? TDied_CLEAN : TDied_ERR;
		return t_finalize(t, died_how, t->status);
	}

	// discard results
	return lua_pop(L, nres);
}


// t_yield suspends t and puts it on the run queue.
// Switches control back to scheduler loop.
// Note: Must only be called from a task, never l_main.
static int t_yield(T* t, int nresults) {
	trace_sched(T_ID_F " yield", t_id(t));
	if UNLIKELY(!s_runq_put(t->s, t))
		return luaL_error(t_L(t), "out of memory");
	return lua_yieldk(t_L(t), nresults, 0, NULL);
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

	// Hold on to a GC reference to thread, e.g. "S.L.reftab[thread] = true"
	// Note: this is quite complex of a Lua stack operation. In particular lua_pushthread is
	// gnarly as it pushes the thread onto its own stack, which we then move over to S's stack.
	// If we get things wrong we (at best) get a memory bus error in asan with no stack trace,
	// making this tricky to debug.
	lua_rawgetp(s->L, LUA_REGISTRYINDEX, &g_reftabkey);
	lua_pushthread(NL);     // Push thread as key onto its own stack
	lua_xmove(NL, s->L, 1); // Move the thread object to the `L` state
	lua_pushboolean(s->L, 1);  // "true"
	lua_rawset(s->L, -3);  // reftab[thread] = true
	lua_pop(s->L, 1);  // Remove table from stack

	// initialize T struct (which lives in the LUA_EXTRASPACE header of lua_State)
	T* t = L_t(NL);
	memset(t, 0, sizeof(*t));
	t->s = s;
	if (parent) t->parent = parent->tid;
	t->nrefs = 1; // S's "live" reference

	// allocate id and store in 'tasks' set
	if UNLIKELY(!s_task_register(s, t)) {
		lua_closethread(NL, L);
		return l_errno_error(L, ENOMEM);
	}
	trace_sched("register task " T_ID_F, t_id(t));

	// assert that main task is assigned tid 1
	if (!parent)
		assertf(t->tid == 1, "main task was not assigned tid 1");

	// setup t to be run next by schedule
	if UNLIKELY(!s_runq_put_runnext(s, t)) {
		s_task_unregister(s, t);
		lua_closethread(NL, L);
		return l_errno_error(L, ENOMEM);
	}

	s->nlive++;

	// add t as a child of parent
	if (parent)
		t_add_child(parent, t);

	if (parent) {
		trace_sched(T_ID_F " spawns " T_ID_F, t_id(parent), t_id(t));
	} else {
		trace_sched("spawn main task " T_ID_F, t_id(t));
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

		trace_sched(T_ID_F " woken by iopoll" , t_id(t));

		// TODO: check if t is already on runq and don't add it if so.
		// For now, use an assertion (note: this is likely to happen)
		assert(t->status == T_WAIT_IO);
		assert(s_runq_find(s, t) == -1);

		if (!s_runq_put(s, t))
			err = -ENOMEM;
	}
	return err;
}


static void worker_retain(Worker* w);
static void worker_release(Worker* w);
static bool worker_close(Worker* w);


static void s_workers_add(S* s, Worker* w) {
	assert(w->next == NULL);
	w->next = s->workers;
	s->workers = w;
	worker_retain(w);
}


static void s_reap_workers(S* s) {
	Worker* w = s->workers;
	Worker* prev_w = NULL;
	while (w) {
		if (atomic_load_explicit(&w->status, memory_order_acquire) == Worker_CLOSED) {
			trace_worker("worker %p exited", w);

			// wake any tasks waiting for this worker
			if (w->waiters)
				worker_wake_waiters(w);

			// remove from list of live workers
			Worker* next = w->next;
			if (prev_w) {
				prev_w->next = next;
			} else {
				s->workers = next;
			}
			w->next = NULL;
			worker_release(w);
			w = next;
		} else {
			prev_w = w;
			w = w->next;
		}
	}
}


static void s_check_notes(S* s, u8 notes) {
	if (notes & S_NOTE_WEXIT)
		s_reap_workers(s);

	// Clear notes bits.
	// We are racing with worker threads here, so use a CAS but don't loop to retry since
	// s_check_notes is called by s_find_runnable, which will call s_check_notes again if a
	// worker raced us and added more events.
	atomic_compare_exchange_strong_explicit(
		&s->notes, &notes, 0, memory_order_acq_rel, memory_order_relaxed);
}


static void s_shutdown(S* s) {
	// note: this function is only called by other threads, never from "inside" S
	if (atomic_exchange_explicit(&s->isclosed, true, memory_order_acq_rel) == false) {
		trace_sched("interrupting iopoll");
		iopoll_interrupt(&s->iopoll);
	}
}


static void s_free(S* s) {
	iopoll_dispose(&s->iopoll);
	pool_free_pool(s->tasks);
	free(s->runq);
	array_free((struct Array*)&s->timers);
}


static int s_finalize(S* s) {
	trace_sched("finalize " S_ID_F, s_id(s));
	atomic_store_explicit(&s->isclosed, true, memory_order_release);

	// stop main task
	if (s->nlive > 0) {
		assertf(!pool_entry_isfree(s->tasks, 1),
		        "main task is DEAD but other tasks are still alive");
		t_stop(NULL, s_task(s, 1));
	}

	// clear timers before GC to avoid costly (and useless) timers_remove
	s->timers.len = 0;

	// run GC to ensure pending IODesc close before S goes away
	lua_gc(s->L, LUA_GCCOLLECT);

	// stop & wait for workers
	if UNLIKELY(s->workers) {
		trace_worker("shutting down workers");
		for (Worker* w = s->workers; w;) {
			Worker* next_w = w->next;
			worker_close(w);
			int err = pthread_join(w->thread, NULL);
			if (err)
				logwarn("failed to wait for worker thread=%p: %s", w->thread, strerror(err));
			worker_release(w);
			w = next_w;
		}

		// if S is supposed to exit() when done, do that now
		if (s->doexit) {
			trace_sched("exit(%d)", (int)s->exiterr);
			exit(s->exiterr);
		}
	}

	trace_sched("finalized " S_ID_F, s_id(s));

	// clear TLS entry to catch bugs
	tls_s = NULL;
	#if defined(TRACE_SCHED) || defined(TRACE_SCHED_RUNQ) || defined(TRACE_SCHED_WORKER)
		tls_s_id = 0;
	#endif

	s_free(s);
	return 0;
}


static int s_find_runnable(S* s, T** tp) {
	// check for expired timers
	if (( *tp = s_timers_check(s) )) {
		trace_sched(T_ID_F " taken from timers", t_id(*tp));
		return 1;
	}

	// check for worker events
	u8 notes = atomic_load_explicit(&s->notes, memory_order_acquire);
	if (notes != 0)
		s_check_notes(s, notes);

	// try to grab a task from the run queue
	if (( *tp = s_runq_get(s) )) {
		trace_sched(T_ID_F " taken from runq", t_id(*tp));
		return 1;
	}

	// check if we ran out tasks (all have exited) or if S is closing down
	if (s->nlive == 0 || atomic_load_explicit(&s->isclosed, memory_order_acquire)) {
		if (atomic_load_explicit(&s->isclosed, memory_order_acquire)) {
			trace_sched("scheduler shutting down; exiting scheduler loop");
		} else {
			trace_sched("no more tasks; exiting scheduler loop");
		}
		return 0;
	}

	// There are no tasks which are ready to run.
	// Poll for I/O events (with timeout if there are any active timers.)

	// determine iopoll deadline
	DTime deadline = (DTime)-1;
	DTimeDuration deadline_leeway = 0;
	if (s->timers.len > 0) {
		deadline = s->timers.v[0].timer->when;
		deadline_leeway = s->timers.v[0].timer->leeway;
	}

	// trace
	#ifdef TRACE_SCHED
		if (deadline == (DTime)-1) {
			trace_sched("iopoll (no timeout)");
		} else {
			char buf[30];
			trace_sched("iopoll with %s timeout", DTimeDurationFormat(DTimeUntil(deadline), buf));
		}
	#endif

	// wait for events
	int n = iopoll_poll(&s->iopoll, deadline, deadline_leeway);
	if UNLIKELY(n < 0) {
		if (s->isclosed) // ignore i/o errors that occur during shutdown
			return 0;
		logerr("internal I/O error: %s", strerror(-n));
		return l_errno_error(s->L, -n);
	}

	// check timers & runq again
	TAIL_CALL s_find_runnable(s, tp);
}


bool runtime_handle_signal(int signo) {
	S* s = tls_s;
	if (s && s->doexit) {
		trace_sched("got signal %d; shutting down " S_ID_F, signo, s_id(s));
		atomic_store_explicit(&s->isclosed, true, memory_order_release);
		return true;
	}
	return false;
}


__attribute__((always_inline))
static int s_main(S* s) {
	s->timers.v = s->timers_storage;
	s->timers.cap = countof(s->timers_storage);

	lua_State* L = s->L;
	if UNLIKELY(tls_s != NULL)
		return luaL_error(L, "S already active");
	tls_s = s;
	#if defined(TRACE_SCHED) || defined(TRACE_SCHED_RUNQ) || defined(TRACE_SCHED_WORKER)
		tls_s_id = atomic_fetch_add(&tls_s_idgen, 1);
	#endif

	// allocate runq with inital space for (8 - 1) entries
	if (!( s->runq = (RunQ*)fifo_alloc(8, sizeof(*s->runq)) ))
		return l_errno_error(L, ENOMEM);

	// allocate task pool with inital space for 8 entries
	if UNLIKELY(!pool_init(&s->tasks, 8, sizeof(T*)))
		return l_errno_error(L, ENOMEM);

	// initialize platform I/O facility
	int err = iopoll_init(&s->iopoll, s);
	if (err) {
		dlog("error: iopoll_init: %s", strerror(-err));
		s_free(s);
		return l_errno_error(L, -err);
	}

	// create refs table (for GC management)
	lua_createtable(L, 0, /*estimated common-case lowball count*/8);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &g_reftabkey);

	// create main task
	int nres = s_spawntask(s, L, NULL);
	if UNLIKELY(nres == 0) { // error
		s_free(s);
		return 0;
	}

	// discard thread from L's stack
	lua_pop(L, 1);

	// scheduler loop: finds a runnable task and executes it.
	// Stops when all tasks have finished (or an error occurred.)
	T* t;
	while (!s->isclosed && s_find_runnable(s, &t))
		t_resume(t);
	return s_finalize(s);
}


// fun main(main fun(), exit_when_done bool = true)
static int l_main(lua_State* L) {
	// create scheduler for this OS thread & Lua context
	S* s = &(S){
		.L = L,
		.doexit = lua_isboolean(L, 2) ? lua_toboolean(L, 2) : true,
	};
	return s_main(s);
}


// WorkerObj is a Lua-managed (GC'd) wrapper around a internally managed Worker.
// This allows a timer to be referenced by userland independently of its state.
typedef struct WorkerObj { Worker* w; } WorkerObj;

static WorkerObj* nullable l_workerobj_check(lua_State* L, int idx) {
	return l_uobj_check(L, idx, &g_workerobj_luatabkey, "Worker");
}


int luaopen_runtime(lua_State* L);


static void worker_free(Worker* w) {
	trace_worker("free");
	free(w->errdesc);
	free(w);
}


static void worker_retain(Worker* w) {
	UNUSED u8 nrefs = atomic_fetch_add_explicit(&w->nrefs, 1, memory_order_acq_rel);
	assert(nrefs < 0xff || !"overflow");
}


static void worker_release(Worker* w) {
	if (atomic_fetch_sub_explicit(&w->nrefs, 1, memory_order_acq_rel) == 1)
		worker_free(w);
}


static int l_workerobj_gc(lua_State* L) {
	WorkerObj* obj = lua_touserdata(L, 1);
	trace_worker("GC");
	worker_close(obj->w);
	worker_release(obj->w);
	return 0;
}


static const char* worker_load_reader(lua_State* L, void* ud, usize* lenp) {
	Worker* w = ud;
	*lenp = w->mainfun_lcode_len;
	return w->mainfun_lcode;
}


// worker_cas_status attempts to set w->status to next_status.
// Returns false if another thread set CLOSED "already" (it's a race.)
static bool worker_cas_status(Worker* w, u8 next_status) {
	u8 status = atomic_load_explicit(&w->status, memory_order_acquire);
	for (;;) {
		if (status == Worker_CLOSED)
			return false;
		if (atomic_compare_exchange_strong_explicit(
				&w->status, &status, next_status, memory_order_acq_rel, memory_order_relaxed))
		{
			return true;
		}
	}
}


static void s_notify(S* s, u8 addl_notes) {
	// Called from another OS thread than S is running on
	u8 notes = atomic_load_explicit(&s->notes, memory_order_acquire);
	for (;;) {
		u8 newnotes = notes | addl_notes;
		if (atomic_compare_exchange_strong_explicit(
				&s->notes, &notes, newnotes, memory_order_acq_rel, memory_order_relaxed))
		{
			break;
		}
	}
	iopoll_interrupt(&s->iopoll);
}


static void worker_thread_exit(Worker** wp) {
	Worker* w = *wp;
	trace_worker("exit");

	// set status to CLOSED (likely already set via worker_cas_status)
	atomic_store_explicit(&w->status, Worker_CLOSED, memory_order_release);

	// tell parent S that we exited
	s_notify(w->parent->s, S_NOTE_WEXIT);

	if (w->s.L)
		lua_close(w->s.L);
	worker_release(w);
}


static void worker_thread(Worker* w) {
	// setup thread cancelation handler and enable thread cancelation
	static int cleanup_pop_arg = 0;
	pthread_cleanup_push((void(*)(void*))worker_thread_exit, &w);

	#if defined(TRACE_SCHED_WORKER)
		tls_w_id = atomic_fetch_add(&tls_w_idgen, 1);
		trace_worker("start worker thread=%p", w->thread);
	#endif

	// create a Lua environment for this thread
	lua_State* L = luaL_newstate();
	w->s.L = L;

	// open libraries
	luaL_openlibs(L);
	luaL_requiref(L, "__rt", luaopen_runtime, 1);
	lua_setglobal(L, "__rt");

	// switch status to READY while checking if worker has been closed
	if UNLIKELY(!worker_cas_status(w, Worker_READY))
		return trace_worker("CLOSED before getting READY");

	// actual work happens now
	if (w->mainfun_lcode_len > 0) {
		// disable thread cancelation to ensure graceful shutdown
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		// load Lua function from mainfun_lcode
		int status = lua_load(L, worker_load_reader, (void*)w, "<worker>", "bt");

		// free memory back to malloc so we don't hold on to it "forever"
		free(w->mainfun_lcode);
		w->mainfun_lcode = NULL;
		w->mainfun_lcode_len = 0;

		// check if load succeeded
		if UNLIKELY(status != LUA_OK) {
			trace_worker("failed to load worker function: %s", l_status_str(status));
			dlog("failed to load worker function: %s", l_status_str(status));
			return;
		}
		// Enter scheduler which will use the function on top of the stack as the main coroutine.
		// We ignore the return value from s_main since we exit on return regardless.
		s_main(&w->s);
	} else {
		// allow passing a w->f to be run here
		assert(!"TODO: worker function");
	}

	pthread_cleanup_pop(&cleanup_pop_arg);
}


static int worker_open(
	Worker** wp, T* parent, void* nullable mainfun_lcode, u32 mainfun_lcode_len)
{
	Worker* w = calloc(1, sizeof(Worker));
	if (!w)
		return -ENOMEM;
	w->parent = parent;
	w->status = Worker_OPEN;
	w->nrefs = 2; // thread + caller
	w->mainfun_lcode_len = mainfun_lcode_len;
	w->mainfun_lcode = mainfun_lcode;
	w->s.isworker = true;
	pthread_attr_t* thr_attr = NULL;
	int err = pthread_create(&w->thread, thr_attr, (void*(*)(void*))worker_thread, w);
	trace_sched("spawn worker thread=%p", w->thread);
	if UNLIKELY(err) {
		free(w);
		return -err;
	}
	*wp = w;
	return 0;
}


static bool worker_close(Worker* w) {
	assert(pthread_self() != w->thread); // must not call from worker's own thread

	if (!worker_cas_status(w, Worker_CLOSED)) {
		// already closed
		return false;
	}

	trace_worker("closing worker thread=%p", w->thread);

	// signal to worker's scheduler that it's time to shut down
	s_shutdown(&w->s);
	pthread_cancel(w->thread);
	return true;
}


static int l_spawn_worker(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	luaL_checktype(L, 1, LUA_TFUNCTION);

	// serialize thread main function as Lua code, to be transferred to the thread
	lua_settop(L, 1); // ensure function is on the top of the stack
	Buf buf = {};
	int err = buf_append_luafun(&buf, L, /*strip_debuginfo*/false);
	if UNLIKELY(err) {
		buf_free(&buf);
		if (err == -EINVAL)
			return luaL_error(L, "unable to serialize worker function");
		return l_errno_error(L, -err);
	}

	// start a worker
	Worker* w;
	if UNLIKELY(( err = worker_open(&w, t, buf.bytes, buf.len) )) {
		buf_free(&buf);
		return l_errno_error(L, -err);
	}

	// add worker to S's list of live workers
	s_workers_add(t->s, w);

	// allocate & return Worker object so that we know when the user is done with it (GC)
	WorkerObj* obj = lua_newuserdatauv(L, sizeof(Worker), 0);
	if UNLIKELY(!obj) {
		worker_close(w);
		worker_release(w);
		return 0;
	}
	lua_rawgetp(L, LUA_REGISTRYINDEX, &g_workerobj_luatabkey);
	lua_setmetatable(L, -2);
	obj->w = w;

	return 1;
}


static int l_yield(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	int nargs = lua_gettop(L);
	return t_yield(t, nargs);
}


static int l_spawn_task(lua_State* L) {
	T* t = REQUIRE_TASK(L);

	t->resume_nres = s_spawntask(t->s, L, t);
	if UNLIKELY(t->resume_nres == 0)
		return 0;

	// suspend the calling task, switching control back to scheduler loop
	return t_yield(t, 0);
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
	trace_sched(T_ID_F, t_id(t));

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
		return t_iopoll_wait(t, d, l_connect_cont);
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
			return t_iopoll_wait(L_t(L), d, l_read_cont);
		return l_errno_error(L, errno);
	}

	// update d & buf
	if (len == 0) { // EOF
		d->nread = 0;
	} else {
		d->nread -= MIN(d->nread, (i64)len);
		buf->len += (usize)len;
	}

	// TODO: if there's a 3rd argument 'nread' that is >0, t_iopoll_wait again if
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
		return t_iopoll_wait(t, d, l_read_cont);
	return l_read_cont(L, 0, d);
}


static int l_msg_stow(lua_State* src_L, lua_State* dst_L, InboxMsg* msg) {
	// Store message payload in a table.
	// This way the message payload gets GC'd when the destination task is GC'd,
	// regardless of message delivery.
	lua_createtable(dst_L, msg->nres, 0);

	// first value is the sender task (Lua "thread")
	lua_pushthread(src_L);      // push sender "thread" onto its own stack
	lua_xmove(src_L, dst_L, 1); // move sender "thread" to receiver's stack
	lua_rawseti(dst_L, -2, 1);  // Store in table at index 1

	for (int i = 2; i <= msg->nres; i++) {
        lua_pushvalue(src_L, i);    // Get argument from its position in src_L ...
        lua_xmove(src_L, dst_L, 1); // ... and move it to dst_L
        lua_rawseti(dst_L, -2, i);  // Store in table at index (i-1)
	}

	#ifdef DEBUG
	for (int i = 1; i <= msg->nres; i++) {
		lua_rawgeti(dst_L, -1, i);
		if (lua_isnil(dst_L, -1)) {
			lua_pop(dst_L, 2);  // pop nil and table
			panic("failed to store argument %d", i);
		} else {
			// dlog("msg payload[%d] = %s", i, lua_typename(dst_L, lua_type(dst_L, -1)));
		}
		lua_pop(dst_L, 1);  // pop checked value
	}
	#endif

	msg->msg.ref = luaL_ref(dst_L, LUA_REGISTRYINDEX);

	return 0;
}


static int l_msg_unload(lua_State* dst_L, InboxMsg* msg) {
	// Note: Payload table will be placed at index 2 in the stack.
	// Stack index 1 is occupied by msg->type, pushed by l_recv_deliver.
	lua_rawgeti(dst_L, LUA_REGISTRYINDEX, msg->msg.ref); // Get the payload table
	assertf(lua_type(dst_L, 2) == LUA_TTABLE,
	        "%s", lua_typename(dst_L, lua_type(dst_L, 2)));
	for (int i = 1; i <= msg->nres; i++) {
		lua_rawgeti(dst_L, 2, i); // get value i from table
		assert(!lua_isnoneornil(dst_L, -1));
	}
	lua_remove(dst_L, 2); // remove payload table from stack
	luaL_unref(dst_L, LUA_REGISTRYINDEX, msg->msg.ref); // free reference
	return 1 + msg->nres; // msg.type + payload
}


static int l_recv_deliver(lua_State* dst_L, T* t, InboxMsg* msg) {
	trace_sched(T_ID_F " deliver msg type=%u", t_id(t), msg->type);
	assert(msg->type != InboxMsgType_MSG_DIRECT); // should never happen
	lua_pushinteger(dst_L, msg->type);
	if (msg->type == InboxMsgType_MSG)
		return l_msg_unload(dst_L, msg);
	return 1 + msg->nres;
}


static int l_recv_cont(lua_State* L, int ltstatus, void* arg) {
	T* t = arg;
	assert(t->inbox != NULL);
	InboxMsg* msg = inbox_pop(t->inbox);
	assert(msg != NULL);
	if (msg->type == InboxMsgType_MSG_DIRECT)
		return 1 + msg->nres;
	return l_recv_deliver(L, t, msg);
}


// fun recv(from... Task|Worker) (type int, sender any, ... any)
// If no 'from' is specified, receive from anyone
static int l_recv(lua_State* L) {
	T* t = REQUIRE_TASK(L);

	// TODO: check "from" arguments.

	// Remove all arguments. This is required for l_recv_deliver & l_msg_unload to
	// work properly as they place return values onto the stack.
	lua_pop(L, lua_gettop(L));

	// check for a ready message in the inbox
	if (t->inbox) {
		InboxMsg* msg = inbox_pop(t->inbox);
		if (msg)
			return l_recv_deliver(L, t, msg);
	}

	// check for deadlock
	S* s = t->s;
	if UNLIKELY(s->runnext == NULL && s->runq->fifo.head == s->runq->fifo.tail) {
		// empty runq
		if (s->nlive == 1 && t->ntimers == 0) {
			// T is the only live task and no timers are active
			return luaL_error(t_L(t), "deadlock detected: recv would never return");
		}
	}

	// wait for a message
	return t_suspend(t, T_WAIT_RECV, t, l_recv_cont);
}


// fun send(Task destination, msg... any)
static int l_send(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	T* dst_t = l_check_task(L, 1);
	if (!dst_t)
		return 0;

	// place message in receiver's inbox
	InboxMsg* msg = inbox_add(&dst_t->inbox);
	if UNLIKELY(msg == NULL) {
		// receiver inbox is full; wait until there's space
		dlog("TODO: wait until there's space in destination task's inbox");
		return luaL_error(L, "receiver inbox full");
	}
	memset(msg, 0, sizeof(*msg));
	msg->type = InboxMsgType_MSG;

	// Set message argument count, which includes msg type and sender.
	// Note: The logical arithmetic is non-trivial here: It's actually "(lua_gettop(L)-1) + 1"
	// -1 the destination argument to 'send()' +1 sender.
	msg->nres = lua_gettop(L);

	lua_State* src_L = L;
	lua_State* dst_L = t_L(dst_t);

	// if the destination task is not currently waiting in a call to recv(),
	// the message will be delivered later.
	if (dst_t->status != T_WAIT_RECV)
		return l_msg_stow(src_L, dst_L, msg);

	// Destination task is already waiting for a message, deliver it directly.
	// Move message values from send() function's arguments to destination task's L stack,
	// which is essentially the stack (activation record) of the recv() function call of the
	// destination task.
	lua_pushinteger(dst_L, msg->type); // push message type
	lua_pushthread(src_L);             // push sender "thread" onto its own stack
	lua_xmove(src_L, dst_L, 1);        // move sender "thread" to receiver's stack
	lua_xmove(src_L, dst_L, msg->nres - 1); // arguments passed after 'dst' to send()
	msg->type = InboxMsgType_MSG_DIRECT;

	// Now, there are two options for how to resume the waiting receiver task.
	// Either we can switch to it right here, bypassing the scheduler, with
	//    t_resume(dst_t); return 0;
	// or we can put the receiver task on the priority run queue and switch to the
	// scheduler with
	//    s_runq_put_runnext(t->s, dst_t); return t_yield(t, 0);
	// On my MacBook, a direct switch takes on average 140ns while going through the
	// scheduler takes on average 170ns. Very small difference in practice.
	// While switching directly is more efficient, it has the disadvantage of allowing two
	// tasks to hog the scheduler, by ping ponging send, recv send, recv send, ...
	// So we are going through the scheduler. A wee bit less efficient but more correct.
	//
	// put destination task in the priority runq so that it runs asap
	s_runq_put_runnext(t->s, dst_t);
	//
	// put the sender task at the end of the runq and return to scheduler loop (s_main)
	return t_yield(t, 0);
}


static int l_await_task_cont1(lua_State* L, T* t, T* other_t) {
	// first return value is status 0=error, 1=clean exit, 2=stopped
	lua_pushinteger(L, other_t->info.dead.how);

	// copy return values from other task
	int nres = other_t->resume_nres;
	lua_State* other_L = t_L(other_t);
	l_copy_values(other_L, L, nres);

	return nres + 1;
}


static int l_await_task_cont(lua_State* L, int ltstatus, void* arg) {
	T* t = arg;

	// note: even though other_t is T_DEAD, its memory is still valid
	// since await holds a GC reference via arguments.
	T* other_t = s_task(t->s, t->info.wait_task.wait_tid);

	return l_await_task_cont1(L, t, other_t);
}


// fun await(t Task) (ok int, ... any)
// Returns ok=0 on error with the error as the 2nd result.
// Returns ok=1 on clean exit with return values from t as rest results.
// Returns ok=2 if t was stopped.
static int l_await_task(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	T* other_t = l_check_task(L, 1);
	if (!other_t)
		return 0;

	// a task can't await itself
	if UNLIKELY(t == other_t)
		return luaL_error(L, "attempt to 'await' itself");

	// if the other task already existed, we can return immediately
	if (other_t->status == T_DEAD)
		return l_await_task_cont1(L, t, other_t);

	// we don't support waiting for a task running on a different OS thread (Worker)
	if UNLIKELY(other_t->s != t->s)
		return luaL_error(L, "attempt to await task of a different Worker");

	// other task cannot possibly be running, since the calling task is running
	assert(other_t->status != T_RUNNING);

	// add t to list of tasks waiting for other_t
	t->info.wait_task.next_tid = other_t->waiters;
	t->info.wait_task.wait_tid = other_t->tid;
	other_t->waiters = t->tid;

	t->resume_nres = 0;

	// wait
	return t_suspend(t, T_WAIT_TASK, t, l_await_task_cont);
}


static int l_await_worker_cont1(lua_State* L, Worker* w) {
	if LIKELY(w->s.exiterr == 0) {
		lua_pushboolean(L, true);
		return 1;
	}
	// worker exited because of an error
	lua_pushboolean(L, false);
	lua_pushstring(L, (w->errdesc && *w->errdesc) ? w->errdesc : "unknown error");
	return 2;
}


static int l_await_worker_cont(lua_State* L, int ltstatus, void* arg) {
	return l_await_worker_cont1(L, arg);
}


// fun await(w Worker) (ok bool, err string)
// Returns true if w exited cleanly or false if w exited because of an error.
static int l_await_worker(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	WorkerObj* obj = l_uobj_check(L, 1, &g_workerobj_luatabkey, "Task or Worker");
	if (!obj)
		return 0;
	Worker* w = obj->w;

	// can't await the worker the calling task is running on
	if UNLIKELY(t->s == &w->s)
		return luaL_error(L, "attempt to 'await' itself");

	// Add t to list of tasks waiting for w.
	// Note that this list is only ever modified by this current thread,
	// but is read by the worker's thread.
	t->info.wait_worker.next_tid = w->waiters;
	atomic_store_explicit(&w->waiters, t->tid, memory_order_release);

	// check if worker exited already
	if (atomic_load_explicit(&w->status, memory_order_acquire) == Worker_CLOSED)
		return l_await_worker_cont1(L, w);

	t->resume_nres = 0;
	return t_suspend(t, T_WAIT_WORKER, w, l_await_worker_cont);
}


static int l_await(lua_State* L) {
	if (lua_isthread(L, 1))
		return l_await_task(L);
	return l_await_worker(L);
}


static int l_taskblock_begin(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	trace_sched("taskblock_begin");
	// TODO
	return t_yield(t, 0);
}


static int l_taskblock_end(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	trace_sched("taskblock_end");
	return t_yield(t, 0);
}


static int l_time(lua_State* L) {
	lua_pushinteger(L, DTimeNow());
	return 1;
}


static const luaL_Reg dew_lib[] = {
	{"intscan", l_intscan},
	{"intfmt", l_intfmt},
	{"intconv", l_intconv},
	{"errstr", l_errstr},
	{"time", l_time},

	// "new" runtime API
	{"main", l_main},
	{"spawn_task", l_spawn_task},
	{"spawn_worker", l_spawn_worker},
	{"yield", l_yield},
	{"sleep", l_sleep},
	{"socket", l_socket},
	{"connect", l_connect},
	{"read", l_read},
	{"buf_alloc", l_buf_alloc},
	{"buf_resize", l_buf_resize},
	{"buf_str", l_buf_str},
	{"timer_start", l_timer_start},
	{"timer_update", l_timer_update},
	{"timer_stop", l_timer_stop},
	{"await", l_await},
	{"recv", l_recv},
	{"send", l_send},
	{"taskblock_begin", l_taskblock_begin},
	{"taskblock_end", l_taskblock_end},

	// wasm experiment
	#ifdef __wasm__
	{"ipcrecv", l_ipcrecv},
	{"ipcrecv_co", l_ipcrecv_co},
	{"iowait", l_iowait},
	#endif

	{NULL, NULL} // Sentinel
};

int luaopen_runtime(lua_State* L) {
	#ifdef __wasm__
	g_L = L; // wasm experiment
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

	// Timer (ref to an internally managed Timer struct)
	luaL_newmetatable(L, "Timer");
	lua_pushcfunction(L, l_timerobj_gc);
	lua_setfield(L, -2, "__gc");
	lua_rawsetp(L, LUA_REGISTRYINDEX, &g_timerobj_luatabkey);

	// Worker
	luaL_newmetatable(L, "Worker");
	lua_pushcfunction(L, l_workerobj_gc);
	lua_setfield(L, -2, "__gc");
	lua_rawsetp(L, LUA_REGISTRYINDEX, &g_workerobj_luatabkey);

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

	return 1;
}


// #include "lua/src/lstate.h"
// __attribute__((constructor)) static void init() {
// 	printf("sizeof(lua_State): %zu B\n", sizeof(T) + sizeof(lua_State));
// }
