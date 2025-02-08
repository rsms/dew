#pragma once
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <lprefix.h>

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <assert.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

typedef int8_t    i8;
typedef uint8_t   u8;
typedef int16_t   i16;
typedef uint16_t  u16;
typedef int32_t   i32;
typedef uint32_t  u32;
typedef int64_t   i64;
typedef uint64_t  u64;
typedef size_t    usize;
typedef ssize_t   isize;
typedef uintptr_t uintptr;
typedef float     float32;
typedef double    float64;

#define I8_MAX    0x7f
#define I16_MAX   0x7fff
#define I32_MAX   0x7fffffff
#define I64_MAX   0x7fffffffffffffffll
#define ISIZE_MAX __LONG_MAX__

#define I8_MIN    (-0x80)
#define I16_MIN   (-0x8000)
#define I32_MIN   (-0x80000000)
#define I64_MIN   (-0x8000000000000000ll)
#define ISIZE_MIN (-__LONG_MAX__ -1L)

#define U8_MAX    0xff
#define U16_MAX   0xffff
#define U32_MAX   0xffffffff
#define U64_MAX   0xffffffffffffffffllu
#ifdef __SIZE_MAX__
	#define USIZE_MAX __SIZE_MAX__
#else
	#define USIZE_MAX (__LONG_MAX__ *2UL+1UL)
#endif
#ifndef UINTPTR_MAX
	#ifdef __UINTPTR_MAX__
		#define UINTPTR_MAX __UINTPTR_MAX__
	#else
		#define UINTPTR_MAX USIZE_MAX
	#endif
#endif

// UNLIKELY(integralexpr)->bool
#if __has_builtin(__builtin_expect)
	#define LIKELY(x)   (__builtin_expect((bool)(x), true))
	#define UNLIKELY(x) (__builtin_expect((bool)(x), false))
#else
	#define LIKELY(x)   (x)
	#define UNLIKELY(x) (x)
#endif

#define nullable _Nullable
#define NORET void __attribute__((noreturn))

#if __has_attribute(warn_unused_result)
	#define WARN_UNUSED_RESULT __attribute__((warn_unused_result))
#else
	#define WARN_UNUSED_RESULT
#endif

// __attribute__((noreturn)) void unreachable()
#if __has_builtin(__builtin_unreachable)
	#define unreachable() (assert(!"unreachable"),__builtin_unreachable())
#elif __has_builtin(__builtin_trap)
	#define unreachable() (assert(!"unreachable"),__builtin_trap)
#else
	#define unreachable() (assert(!"unreachable"),abort())
#endif

#if __has_attribute(musttail) && !defined(__wasm__)
	// Note on "!defined(__wasm__)": clang 13 claims to have this attribute for wasm
	// targets but it's actually not implemented and causes an error.
	// Tail calls in wasm is a post-v1 feature that can must be explicitly enabled.
	#define MUSTTAIL __attribute__((musttail))
#else
	#define MUSTTAIL
#endif
#define TAIL_CALL MUSTTAIL return

#define API_BEGIN \
	_Pragma("clang diagnostic push") \
	_Pragma("clang diagnostic ignored \"-Wnullability-completeness\"") \
	_Pragma("clang diagnostic ignored \"-Wnullability-inferred-on-nested-type\"") \
	_Pragma("clang assume_nonnull begin")
#define API_END \
	_Pragma("clang diagnostic pop") \
	_Pragma("clang assume_nonnull end")

#ifndef countof
	#define countof(x) \
		( (sizeof(x)/sizeof(0[x])) / ((usize)(!(sizeof(x) % sizeof(0[x])))) )
#endif

#define MAX(a,b) ( \
	__builtin_constant_p(a) && __builtin_constant_p(b) ? ((a) > (b) ? (a) : (b)) : \
	({__typeof__ (a) _a = (a); \
	  __typeof__ (b) _b = (b); \
	  _a > _b ? _a : _b; }) \
)
#define MIN(a,b) ( \
	__builtin_constant_p(a) && __builtin_constant_p(b) ? ((a) < (b) ? (a) : (b)) : \
	({__typeof__ (a) _a = (a); \
	  __typeof__ (b) _b = (b); \
	  _a < _b ? _a : _b; }) \
)

#define MAX_X(a,b)  ( (a) > (b) ? (a) : (b) )
#define MIN_X(a,b)  ( (a) < (b) ? (a) : (b) )

// bool IS_POW2(T x) returns true if x is a power-of-two value
#define IS_POW2(x)    ({ __typeof__(x) xtmp__ = (x); IS_POW2_X(xtmp__); })
#define IS_POW2_X(x)  ( ((x) & ((x) - 1)) == 0 )

// T ALIGN2<T>(T x, anyuint a) rounds up x to nearest a (a must be a power of two)
#define ALIGN2(x,a) ({ \
	__typeof__(x) atmp__ = (__typeof__(x))(a) - 1; \
	( (x) + atmp__ ) & ~atmp__; \
})
#define ALIGN2_X(x,a) ( \
	( (x) + ((__typeof__(x))(a) - 1) ) & ~((__typeof__(x))(a) - 1) \
)

// T IDIV_CEIL(T x, ANY divisor) divides x by divisor, rounding up.
// If x is zero, returns max value of x (wraps.)
#define IDIV_CEIL(x, divisor) ({ \
	__typeof__(x) div__ = (__typeof__(x))(divisor); \
	( (x) + div__ - 1 ) / div__; \
})
#define IDIV_CEIL_X(x, divisor) \
	( ( (x) + (__typeof__(x))(divisor) - 1 ) / (__typeof__(x))(divisor) )

// int dew_clz(ANYINT x) counts leading zeroes in x,
// starting at the most significant bit position.
// If x is 0, the result is undefined.
#define dew_clz(x) ( \
	_Generic((x), \
		i8:   __builtin_clz,   u8:    __builtin_clz, \
		i16:  __builtin_clz,   u16:   __builtin_clz, \
		i32:  __builtin_clz,   u32:   __builtin_clz, \
		long: __builtin_clzl,  unsigned long: __builtin_clzl, \
		long long:  __builtin_clzll, unsigned long long:   __builtin_clzll \
	)(x) - ( 32 - MIN_X(4, (int)sizeof(__typeof__(x)))*8 ) \
)

// int dew_ctz(ANYINT x) counts the number of trailing 0-bits in x,
// starting at the least significant bit position.
// If x is 0, the result is undefined.
#define dew_ctz(x) _Generic((x), \
	i8:   __builtin_ctz,   u8:    __builtin_ctz, \
	i16:  __builtin_ctz,   u16:   __builtin_ctz, \
	i32:  __builtin_ctz,   u32:   __builtin_ctz, \
	long: __builtin_ctzl,  unsigned long: __builtin_ctzl, \
	long long:  __builtin_ctzll, unsigned long long:   __builtin_ctzll \
)(x)

// int dew_ffs(ANYINT x) returns one plus the index of the least significant 1-bit of x,
// or if x is zero, returns zero. ("Find First Set bit")
#define dew_ffs(x) _Generic((x), \
	i8:   __builtin_ffs,   u8:    __builtin_ffs, \
	i16:  __builtin_ffs,   u16:   __builtin_ffs, \
	i32:  __builtin_ffs,   u32:   __builtin_ffs, \
	long: __builtin_ffsl,  unsigned long: __builtin_ffsl, \
	long long:  __builtin_ffsll, unsigned long long:   __builtin_ffsll \
)(x)

// int dew_fls(ANYINT n) finds the Find Last Set bit
// (last = most-significant)
// e.g. dew_fls(0b1111111111111111) = 15
// e.g. dew_fls(0b1000000000000000) = 15
// e.g. dew_fls(0b1000000000000000) = 15
// e.g. dew_fls(0b1000) = 3
#define dew_fls(x) \
	( (x) ? (int)(sizeof(__typeof__(x)) * 8) - dew_clz(x) : 0 )

// int dew_ilog2(ANYINT n) calculates the log of base 2, rounding down.
// e.g. dew_ilog2(15) = 3, dew_ilog2(16) = 4.
// Result is undefined if n is 0.
#define dew_ilog2(n) (dew_fls(n) - 1)

// ANYINT CEIL_POW2(ANYINT x) rounds up x to nearest power of two.
// Returns 1 when x is 0.
// Returns 0 when x is larger than the max pow2 for x's type
// (e.g. >0x80000000 for u32)
#define CEIL_POW2(x) ({ \
	__typeof__(x) xtmp__ = (x); \
	CEIL_POW2_X(xtmp__); \
})
// CEIL_POW2_X is a constant-expression implementation of CEIL_POW2
#define CEIL_POW2_X(x) ( \
	((x) <= (__typeof__(x))1) ? (__typeof__(x))1 : \
	( ( ((__typeof__(x))1 << \
					dew_ilog2( ((x) - ((x) == (__typeof__(x))1) ) - (__typeof__(x))1) \
			) - (__typeof__(x))1 ) << 1 ) \
	+ (__typeof__(x))2 \
)


// logging (0=error, 1=warning, 2=info, 3=debug)
#define logerr(fmt, args...)  \
	_logmsg(0, "error: " fmt " (%s %d)\n", ##args, __FUNCTION__, __LINE__)
#define logwarn(fmt, args...) _logmsg(1, "warning: " fmt "\n", ##args)
#define logmsg(fmt, args...)  _logmsg(2, fmt "\n", ##args)
#ifdef DEBUG
	#define dlog(format, args...) \
		_logmsg(3, format " \e[2m(%s:%d)\e[0m\n", ##args, __FILE__, __LINE__)
#else
	#define dlog(...) ((void)0)
#endif

API_BEGIN

extern const char* g_prog; // dew.c

void _logmsg(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));


static inline WARN_UNUSED_RESULT bool __must_check_unlikely(bool unlikely) {
	return UNLIKELY(unlikely);
}

// check_add_overflow(T a, T b, T* dst) -> bool did_overflow
#define check_add_overflow(a, b, dst) __must_check_unlikely(({  \
	__typeof__(a) a__ = (a);                 \
	__typeof__(b) b__ = (b);                 \
	__typeof__(dst) dst__ = (dst);           \
	(void) (&a__ == &b__);                   \
	(void) (&a__ == dst__);                  \
	__builtin_add_overflow(a__, b__, dst__); \
}))

#define check_sub_overflow(a, b, dst) __must_check_unlikely(({  \
	__typeof__(a) a__ = (a);                 \
	__typeof__(b) b__ = (b);                 \
	__typeof__(dst) dst__ = (dst);           \
	(void) (&a__ == &b__);                   \
	(void) (&a__ == dst__);                  \
	__builtin_sub_overflow(a__, b__, dst__); \
}))

#define check_mul_overflow(a, b, dst) __must_check_unlikely(({  \
	__typeof__(a) a__ = (a);                 \
	__typeof__(b) b__ = (b);                 \
	__typeof__(dst) dst__ = (dst);           \
	(void) (&a__ == &b__);                   \
	(void) (&a__ == dst__);                  \
	__builtin_mul_overflow(a__, b__, dst__); \
}))


typedef u64 DTime;         // monotonic high-resolution clock with undefined base
typedef i64 DTimeDuration; // duration, like 134ms or -1.2h

#define D_TIME_HOUR        ((DTimeDuration)3600e9)
#define D_TIME_MINUTE      ((DTimeDuration)60e9)
#define D_TIME_SECOND      ((DTimeDuration)1e9)
#define D_TIME_MILLISECOND ((DTimeDuration)1e6)
#define D_TIME_MICROSECOND ((DTimeDuration)1e3)
#define D_TIME_NANOSECOND  ((DTimeDuration)1)

// DTimeNow returns the current monotonic clock value (not "wall clock" time.)
// DTime is compatible with DTimeDuration so to make a time in the future, simply
// add to DTime, i.e. "10 seconds from now" = DTimeNow() + 10*D_TIME_SECOND
DTime DTimeNow();

// DTimeSince returns the time delta between now and a point in time in the past
DTimeDuration DTimeSince(DTime past);

// DTimeUntil returns the time delta between now and a point in time in the future
DTimeDuration DTimeUntil(DTime future);

// DTimeBetween returns the time delta between a and b.
// i.e. DTimeBetween(1, 3) == -2, DTimeBetween(3, 1) == 2.
DTimeDuration DTimeBetween(DTime a, DTime b);

// DTimeTimespec populates a timespec struct with the value of t
void DTimeTimespec(DTime t, struct timespec* ts);

// DTimeDurationTimespec populates a timespec struct with the value of d
void DTimeDurationTimespec(DTimeDuration d, struct timespec* ts);

// DTimeDurationFormat writes a human-readable string like "1.6s" to buf.
// Returns a pointer to beginning of buf.
const char* DTimeDurationFormat(DTimeDuration d, char buf[26]);

float64 DTimeDurationHours(DTimeDuration d);
float64 DTimeDurationMinutes(DTimeDuration d);
float64 DTimeDurationSeconds(DTimeDuration d);
i64 DTimeDurationMilliseconds(DTimeDuration d);
i64 DTimeDurationMicroseconds(DTimeDuration d);
i64 DTimeDurationNanoseconds(DTimeDuration d);



struct Array { u32 cap, len; void* nullable v; };
#define Array(T) struct { u32 cap, len; T* nullable v; }
void array_free(struct Array* a); // respects embedded a->v
void* nullable _array_reserve(struct Array* a, u32 elemsize, u32 minavail);
inline static void* nullable array_reserve(struct Array* a, u32 elemsize, u32 minavail) {
	return LIKELY(minavail <= a->cap - a->len) ? a->v + (usize)a->len*(usize)elemsize :
	       _array_reserve(a, elemsize, minavail);
}
bool array_append(struct Array* a, u32 elemsize, const void* elemv, u32 elemc);


// Pool manages homogenous chunks of memory identified by indices rather than pointers.
// Think "file descriptor" rather than virtual-memory address.
// pool_alloc will allocate the first (smallest index) entry.
typedef struct Pool {
	u32 cap;      // capacity of the entries array (also max possible tid)
	u32 maxidx;   // max allocated index
	u64 freebm[]; // bitmap; bit=1 means entries[bit] is free
	// TYPE entries[];
} Pool;
bool pool_init(Pool** pp, u32 cap, usize elemsize);
inline static void pool_free_pool(Pool* nullable p) { free(p); }
void* nullable pool_entry_alloc(Pool** pp, u32* idxp, usize elemsize);
void pool_entry_free(Pool* p, u32 idx);
inline static bool pool_entry_islive(const Pool* p, u32 idx) {
	u32 chunk_idx = (idx - 1) / (sizeof(*p->freebm)*8);
	u32 bit_idx = (idx - 1) % (sizeof(*p->freebm)*8);
	return idx > 0 && idx <= p->maxidx &&
	       (p->freebm[chunk_idx] & ((__typeof__(*p->freebm))1 << bit_idx)) == 0;
}
inline static usize pool_freebm_chunks(const Pool* p) {
	return IDIV_CEIL(p->cap, sizeof(*p->freebm)*8);
}
inline static void* pool_entries(const Pool* p) {
	return (void*)p->freebm + sizeof(*p->freebm)*pool_freebm_chunks(p);
}
inline static void* pool_entry(const Pool* p, u32 idx, usize elemsize) {
	assert(idx > 0 && idx <= p->maxidx);
	return pool_entries(p) + (idx-1)*elemsize;
}


API_END
