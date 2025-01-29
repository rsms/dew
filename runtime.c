#include "dew.h"

#ifdef __wasm__
#include "wasm.h"
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
		return luaL_error(L, strerror(-w));

	lua_pushinteger(L, 1); // yield 1 to let runtime know we're still going
	return lua_yield(L, 1);
	// lua_pushinteger(L, w);
	// return 1;
}


static int l_runloop_add_interval(lua_State* L) {
	u64 interval_nsec = luaL_checkinteger(L, 1);
	int w = RunloopAddInterval(g_main_runloop, timer_handler, 0, interval_nsec);
	if (w < 0)
		return luaL_error(L, strerror(-w));
	lua_pushinteger(L, w);
	return 1;
}


static int l_runloop_run(lua_State* L) {
	int n = RunloopRun(g_main_runloop, 0);
	if (n < 0)
		return luaL_error(L, strerror(-n));
	lua_pushboolean(L, n);
	return 1;
}




#define trace(fmt, ...) dlog("\e[1;34m%-15s│\e[0m " fmt, __FUNCTION__, ##__VA_ARGS__)

// #define trace_runq(fmt, ...) dlog("\e[1;35m%-15s│\e[0m " fmt, __FUNCTION__, ##__VA_ARGS__)
#define trace_runq(fmt, ...) ((void)0)


typedef struct S S; // scheduler (M+P in Go lingo)
typedef struct T T; // task

typedef enum TStatus : u8 {
	TStatus_RUN,   // running
	TStatus_DEAD,  // dead
	TStatus_YIELD, // suspended
	TStatus_NORM,  // normal
} TStatus;

struct T {
	S*          s;            // owning S
	T* nullable parent;       // task that spawned this task (0 means this is S's main task)
	T* nullable next_sibling;
	T* nullable first_child;
	int         nres;         // number of results on stack, to be returned via resume
	u8          is_live;      // 1 when running or waiting, 0 when not yet started or exited
	u8          _unused[3];
	// rest of struct is a lua_State struct
	// Note: With Lua 5.4, the total size of T + lua_State is 240 B
};

static_assert(sizeof(T) == LUA_EXTRASPACE, "");

struct S {
	lua_State* L;       // base Lua environment
	Runloop*   runloop; //
	u32        nlive;   // number of live tasks

	// runq is a queue of tasks (TIDs) ready to run; a circular buffer.
	// (We could get fancy with a red-black tree a la Linux kernel. Let's keep it simple for now.)
	u32 runq_head;
	u32 runq_tail;
	u32 runq_cap;
	T** runq;

	// runnext is a TID (>0) if a task is to be run immediately, skipping runq
	T* nullable runnext;
};


static u8 g_thread_table_key;


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


// L_t returns Lua state of task
inline static lua_State* t_L(const T* t) {
	// T is stored in the LUA_EXTRASPACE in the head of Lua managed lua_State
	return (void*)t + sizeof(*t);
}


// L_t returns task of Lua state
inline static T* L_t(lua_State* L) {
	return (void*)L - sizeof(T);
}


// s_id formats an identifier of a S for logging
#define s_id(s) ((unsigned long)(uintptr)(s))
#define S_ID_F  "S#%lx"

// t_id formats an identifier of a T for logging
#define t_id(t) ((unsigned long)(uintptr)t_L(t))
#define T_ID_F  "T#%lx"


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
	int nargs = t->nres, nres;
	t->nres = 0;
	trace("resume " T_ID_F " nargs=%d", t_id(t), nargs);
	int status = lua_resume(L, t->s->L, nargs, &nres);

	// check if task exited
	if UNLIKELY(status != LUA_YIELD)
		return t_finalize(t, status);

	// discard results
	return lua_pop(L, nres);
}


// t_yield suspends t, switching control back to s_schedule loop
// Must only be called from a task, never l_main.
static int t_yield(T* t, int nresults) {
	trace("suspend " T_ID_F, t_id(t));
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
	t->nres = 0;
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


static int s_schedule(S* s) {
	// Scheduler loop: find a runnable task and execute it.
	// Stops when all tasks have finished or an error occurred.
	static int debug_nwait = 0;
	while (s->nlive > 0) {
		// find a task to run
		T* t = s_runq_get(s);
		if (t) {
			trace(T_ID_F " taken from runq", t_id(t));
			debug_nwait = 0;
			t_resume(t);
		} else {
			// wait for an event to wake a T up
			trace("wait I/O");
			int n = RunloopRun(s->runloop, 0);
			if UNLIKELY(n < 0) {
				logerr("internal I/O error: %s", strerror(-n));
				luaL_error(s->L, strerror(-n));
				return 0;
			}
			// check runq & allt again
			// debug check for logic errors (TODO: remove this when I know scheduler works)
			if (++debug_nwait == 4) {
				dlog("s->nlive %u", s->nlive);
				assert(!"XXX");
			}
		}
	}
	trace("shutdown " S_ID_F, s_id(s));
	RunloopFree(s->runloop);
	return 0;
}


static int l_main(lua_State* L) {
	// create scheduler for this OS thread & Lua context
	S* s = &(S){
		.L = L,
		.runq_cap = 8,
	};

	// allocate initial runq array
	s->runq = malloc(sizeof(*s->runq) * s->runq_cap);
	if (!s->runq)
		return luaL_error(L, strerror(ENOMEM));

	// create runloop for scheduler.
	// TODO: integrate runloop into scheduler.
	int err = RunloopCreate(&s->runloop);
	if UNLIKELY(err) {
		trace("RunloopCreate failed: %s", strerror(-err));
		free(s->runq);
		return luaL_error(L, strerror(-err));
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


// Lua API functions exported as __rt.NAME


#define REQUIRE_TASK(L) ({ \
	T* __t = L_t(L); \
	if UNLIKELY(!__t->is_live) { \
		/* note: this can happen in two scenarios: \
		 * 1. calling a task-specific API function from a non-task, or \
		 * 2. calling a task-specific API function from a to-be-closed variable (__close) \
		 *    during task shutdown. */ \
		const char* msg = __t->s ? "task is shutting down" : "not called from a task"; \
		trace("%s: invalid call by user: %s", __FUNCTION__, msg); \
		return luaL_error(L, msg); \
	} \
	__t; \
})


static int l_yield(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	int nargs = lua_gettop(L);
	return t_yield(t, nargs);
}


static int l_spawn(lua_State* L) {
	T* t = REQUIRE_TASK(L);

	assert(t->nres == 0);
	t->nres = s_spawntask(t->s, L, t);
	if UNLIKELY(t->nres == 0)
		return 0;

	// suspend the calling task, switching control back to s_schedule loop
	return t_yield(t, 0);
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
	{"runloop_run", l_runloop_run},
	{"runloop_add_timeout", l_runloop_add_timeout},
	{"runloop_add_interval", l_runloop_add_interval},

	{"main", l_main},
	{"spawn", l_spawn},
	{"yield", l_yield},
	{"taskblock_begin", l_taskblock_begin},
	{"taskblock_end", l_taskblock_end},

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
