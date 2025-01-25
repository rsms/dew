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


typedef struct S S; // scheduler (M+P in Go lingo)
typedef struct T T; // task

typedef enum TStatus : u8 {
	TStatus_RUN,   // running
	TStatus_DEAD,  // dead
	TStatus_YIELD, // suspended
	TStatus_NORM,  // normal
} TStatus;

struct T {
	S*          s;      // S which owns this task
	T* nullable parent; // task that spawned this task
	lua_State*  L;      // Lua coroutine
};

struct S {
	lua_State* L; // Lua environment
	Runloop*   runloop;
	T          t0; // main task

	u32 nlive; // number of live tasks

	// runq is a queue of tasks ready to run (circular buffer)
	u32 runq_head;
	u32 runq_tail;
	u32 runq_cap;
	T** runq;

	// runnext is non-null if a task is to be run immediately, skipping runq
	T* nullable runnext;
};


#define t_id(t) ((unsigned long)(uintptr)(t)->L)
#define T_ID_F  "T#%lx"


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


// s_tstatus returns the status of a task.
// Based on auxstatus from lcorolib.c
static TStatus s_tstatus(S* s, T* t) {
	if (s->L == t->L)
		return TStatus_RUN;
	switch (lua_status(t->L)) {
		case LUA_YIELD:
			return TStatus_YIELD;
		case LUA_OK: {
			lua_Debug ar;
			if (lua_getstack(t->L, 0, &ar))  /* does it have frames? */
				return TStatus_NORM;  /* it is running */
			else if (lua_gettop(t->L) == 0)
				return TStatus_DEAD;
			else
				return TStatus_YIELD;  /* initial state */
		}
		default:  /* some error occurred */
			return TStatus_DEAD;
	}
}


static void s_runq_put(S* s, T* t) {
	u32 head = s->runq_head;
	u32 tail = s->runq_tail;
	if (tail - head < s->runq_cap) {
		trace("runq put [%u] = " T_ID_F, tail % s->runq_cap, t_id(t));
		s->runq[tail % s->runq_cap] = t;
		s->runq_tail = tail + 1;
		return;
	}
	// Note: Go uses a fixed-size runq and moves half of the locally scheduled runnables
	// to global runq un this scenario, and then proceeds to add t to the local runq.
	assert(!"TODO: grow runq");
}


static T* nullable s_runq_get(S* s) {
	T* t;
	if (s->runnext) {
		t = s->runnext;
		s->runnext = NULL;
	} else {
		if (s->runq_head == s->runq_tail) // empty
			return NULL;
		t = s->runq[s->runq_head % s->runq_cap];
		s->runq_head = s->runq_head + 1;
	}
	return t;
}


static void t_assoc_l(T* t, lua_State* L) {
	// T -> L
	t->L = L;

	// L -> T
	lua_pushlightuserdata(L, (void*)&t_assoc_l);
	lua_pushlightuserdata(L, t);
    lua_settable(L, LUA_REGISTRYINDEX);
}


// l_get_t returns the task which owns Lua coroutine (aka thread) L
static T* l_get_t(lua_State* L) {
	assert(lua_isyieldable(L) || !"not a coroutine");
	lua_pushlightuserdata(L, (void*)&t_assoc_l);
	lua_gettable(L, LUA_REGISTRYINDEX);
	T* t = (T*)lua_touserdata(L, -1);
	assert(t != NULL);
	lua_pop(L, 1);
	return t;
}


static void t_report_error(T* t) {
	lua_State* L = t->L;

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
	const char* msg2 = lua_tostring(t->L, -1);
	if (msg2) msg = msg2;

	logerr("uncaught error in "T_ID_F ":\n%s", t_id(t), msg);
	lua_pop(t->L, 1);
}


static void s_tfinalize(S* s, T* t, bool clean_exit) {
	trace("finalize " T_ID_F, t_id(t));

	if UNLIKELY(!clean_exit)
		t_report_error(t);

	assert(s->nlive > 0);
	s->nlive--;
	if (t != &s->t0)
		free(t);
}


static int t_resume(T* t, lua_State* L) {
	trace("resume " T_ID_F, t_id(t));
	assert(L != t->L);
	int narg = 0, nres;
	int status = lua_resume(t->L, L, narg, &nres);
	// check if task was suspended or exited
	if (status == LUA_YIELD) {
		trace(T_ID_F " suspended (nres=%d)", t_id(t), nres);
		lua_pop(t->L, nres); // discard results
		s_runq_put(t->s, t);
		// Note: we return 1 here simply to allow tail recursion when used by l_spawn
		return 1;
	} else {
		// task is dead, either from clean exit or error (LUA_OK or LUA_ERR* status)
		trace(T_ID_F " exited (%s nres=%d)", t_id(t), l_status_str(status), nres);
		lua_pop(t->L, nres);
		s_tfinalize(t->s, t, status == LUA_OK);
		return 0;
	}
}


// t_create creates a new task, which function should be on stack L (the spawner thread.)
// L should also be the Lua thread that initiated the spawn (S's L or a task's L.)
static int t_create(T* t, lua_State* L) {
	// create Lua coroutine
	// Note: See coroutine.create in luaB_cocreate (lcorolib.c)
	if UNLIKELY(lua_type(L, 1) != LUA_TFUNCTION)
		return luaL_typeerror(L, 1, lua_typename(L, LUA_TFUNCTION));
	lua_State* NL = lua_newthread(L);
	lua_pushvalue(L, 1); // move function to top
	lua_xmove(L, NL, 1); // move function from L to NL

	// assign coroutine handle to T
	t_assoc_l(t, NL);

	return 1;
}


static int l_main(lua_State* L) {
	// create scheduler for this OS thread & Lua context
	T* runq[64]; // TODO: dynamic allocation
	S* s = &(S){
		.L = L,
		.runq_cap = countof(runq),
		.runq = runq,
	};

	// create runloop for scheduler.
	// TODO: integrate runloop into scheduler.
	int err = RunloopCreate(&s->runloop);
	if (err) {
		trace("RunloopCreate failed: %s", strerror(-err));
		return luaL_error(L, strerror(-err));
	}

	// create and immediately switch to the main coroutine
	s->t0.s = s;
	int nres = t_create(&s->t0, L);
	if (nres == 0) // error
		return 0;
	trace("main spawn " T_ID_F, t_id(&s->t0));
	s->nlive++;
	t_resume(&s->t0, L);

	// Scheduler loop: find a runnable task and execute it.
	// Stops when all tasks have finished or an error occurred.
	T* t;
	for (;;) {
		// find a task to run
		if (( t = s_runq_get(s) )) {
			trace("found "T_ID_F" on runq", t_id(t));
		} else if (s->nlive == 0) {
			// all tasks completed
			trace("no tasks");
			break;
		} else {
			// wait for an event to wake a T up
			trace("wait I/O");
			int n = RunloopRun(s->runloop, 0);
			if UNLIKELY(n < 0) {
				logerr("internal I/O error: %s", strerror(-n));
				luaL_error(L, strerror(-n));
				break;
			}
			// check runq & allt again
			// assert(!"XXX");
			continue;
		}
		t_resume(t, L);
	}

	trace("finalize S#%lx", (unsigned long)(uintptr)s);
	RunloopFree(s->runloop);
	return 0;
}


static int l_spawn(lua_State* L) {
	T* t = l_get_t(L); // parent

	// create new task
	T* newt = calloc(1, sizeof(T));
	newt->s = t->s;
	newt->parent = t;

	// spawn task
	int nres = t_create(newt, L);
	if UNLIKELY(nres == 0) {
		// error
		free(newt);
		return 0;
	}
	assert(nres == 1);
	t->s->nlive++;
	trace(T_ID_F " spawn " T_ID_F, t_id(t), t_id(newt));
	return t_resume(newt, L);
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
