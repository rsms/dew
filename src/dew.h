#pragma once
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
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

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

typedef int8_t        i8;
typedef uint8_t       u8;
typedef int16_t       i16;
typedef uint16_t      u16;
typedef int32_t       i32;
typedef uint32_t      u32;
typedef long          i64;
typedef unsigned long u64;
typedef size_t        usize;
typedef ssize_t       isize;
typedef uintptr_t     uintptr;
typedef float         float32;
typedef double        float64;

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

#if defined(__aarch64__) && defined(__APPLE__)
    // Apple Silicon (M1, M2 etc) has 128B cache lines (`sysctl hw.cachelinesize`)
    #define CPU_CACHE_LINE_SIZE 128
#else
    // Most other x86_64/amd64 and aarch64 CPUs have 64B cache lines
    #define CPU_CACHE_LINE_SIZE 64
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

#if __has_attribute(unused)
    #define UNUSED __attribute__((unused))
#else
    #define UNUSED
#endif

// __attribute__((noreturn)) void unreachable()
#if __has_builtin(__builtin_unreachable)
    #define unreachable() (panic("unreachable"),__builtin_unreachable())
#elif __has_builtin(__builtin_trap)
    #define unreachable() (panic("unreachable"),__builtin_trap)
#else
    #define unreachable() (panic("unreachable"),abort())
#endif

#if __has_attribute(musttail) && !defined(__wasm__)
    // Note on "!defined(__wasm__)": clang 13 claims to have this attribute for wasm
    // targets but it's actually not implemented and causes an error.
    // Tail calls in wasm is a post-v1 feature that can must be explicitly enabled.
    #define MUSTTAIL __attribute__((musttail))
#else
    #define MUSTTAIL
#endif
#define return_tail MUSTTAIL return

#if __has_attribute(format)
    #define ATTR_FORMAT(archetype, fmtarg, checkarg) \
        __attribute__((format(archetype, fmtarg, checkarg)))
#else
    #define ATTR_FORMAT(archetype, fmtarg, checkarg)
#endif

// C23 introduced some aliases for "uglified" standard C functions.
// Define them here for older C standards.
// Testing for <202000L instead of <202300L because --std=c2x.
#if __STDC_VERSION__ < 202000L
    #define thread_local _Thread_local
    #define static_assert _Static_assert
#endif

#define API_BEGIN \
    _Pragma("clang diagnostic push") \
    _Pragma("clang diagnostic ignored \"-Wnullability-completeness\"") \
    _Pragma("clang diagnostic ignored \"-Wnullability-inferred-on-nested-type\"") \
    _Pragma("clang assume_nonnull begin")
#define API_END \
    _Pragma("clang diagnostic pop") \
    _Pragma("clang assume_nonnull end")

// DEW_PLUS_ONE can be used to define count of enums, e.g.
// enum { FOO_COUNT = (0lu FOR_EACH_FOO(DEW_PLUS_ONE)) };
#define DEW_PLUS_ONE(...) + 1lu

#ifndef countof
    #define countof(x) \
        ( (sizeof(x)/sizeof(0[x])) / ((usize)(!(sizeof(x) % sizeof(0[x])))) )
#endif

#if __has_builtin(__builtin_offsetof)
    #undef offsetof
    #define offsetof __builtin_offsetof
#elif !defined(offsetof)
    #define offsetof(st, m) ((usize)&(((st*)0)->m))
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

// bool IS_ALIGN2(T x, anyuint a) returns true if x is aligned to a
#define IS_ALIGN2(x, a)  ( !((x) & ((__typeof__(x))(a) - 1)) )

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

// ANYINT FLOOR_POW2(ANYINT x) rounds down x to nearest power of two.
// Returns 1 if x is 0.
#define FLOOR_POW2(x) ({ \
  __typeof__(x) xtmp__ = (x); \
  FLOOR_POW2_X(xtmp__); \
})
// FLOOR_POW2_X is a constant-expression implementation of FLOOR_POW2.
// When used as a constant expression, compilation fails if x is 0.
#define FLOOR_POW2_X(x) ( \
  ((x) <= 1) ? (__typeof__(x))1 : \
  ((__typeof__(x))1 << dew_ilog2(x)) \
)


// cpu_yield() yields the CPU core
#if defined(__i386) || defined(__i386__) || defined(__x86_64__)
  #define cpu_yield() __asm__ __volatile__("pause")
#elif defined(__arm__) || defined(__arm64__) || defined(__aarch64__)
  #define cpu_yield() __asm__ __volatile__("yield")
#elif defined(WIN32)
    #include <immintrin.h>
    #define cpu_yield() _mm_pause()
#elif defined(__wasm__)
    #define cpu_yield() ((void)0)
#else
    // default to scheduler yield
    #define cpu_yield() sched_yield()
#endif


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


// assert
#undef assert
#if defined(DEBUG) || !defined(NDEBUG)
    #define _assertfail(fmt, args...) \
        _panic(__FILE__, __LINE__, __FUNCTION__, "Assertion failed: " fmt, args)
    #define assert(cond) \
        (UNLIKELY(!(cond)) ? _assertfail("%s", #cond) : ((void)0))
    #define assertf(cond, fmt, args...) \
        (UNLIKELY(!(cond)) ? _assertfail(fmt " (%s)", ##args, #cond) : ((void)0))
    #define assertnull(a) \
        assert((a) == NULL)
    #define assertnotnull(a) ({ \
        __typeof__(*(a))* nullable val__ = (a); \
        UNUSED const void* valp__ = val__; /* build bug on non-pointer */ \
        if (UNLIKELY(val__ == NULL)) \
            _assertfail("%s != NULL", #a); \
        val__; \
    })
#else
    #define assert(cond) ((void)0)
    #define assertf(cond, fmt, args...) ((void)0)
    #define assertnull(cond) ((void)0)
    #define assertnotnull(expr) ({ expr; })
#endif

API_BEGIN

extern const char* g_prog; // dew.c

void _logmsg(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void _vlogmsg(int level, const char* fmt, va_list args);

// panic prints a message with stack trace and then calls abort()
_Noreturn void _panic(
    const char* file, int line, const char* fun, const char* fmt, ...) ATTR_FORMAT(printf, 4, 5);
#define panic(fmt, args...) _panic(__FILE__, __LINE__, __FUNCTION__, fmt, ##args)
#define panic_oom() panic("failed to allocate memory")


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

#ifdef DEBUG
    extern thread_local char g_fmtbufv[8][512];
    inline static usize fmtbuf_cap() { return sizeof(g_fmtbufv[0]); }
    char* fmtbuf_get();
#else
    #define fmtbuf_cap() ((usize)0)
    #define fmtbuf_get() ((char*)NULL)
#endif

isize snprintf_lval(char* buf, usize bufcap, lua_State* L, int i);

#ifdef DEBUG
    void dlog_lua_val(lua_State* L, int val_idx);
    void dlog_lua_stackf(lua_State* L, const char* fmt, ...);
    const char* fmtlval(lua_State* L, int i);
#else
    #define dlog_lua_val(...) ((void)0)
    #define dlog_lua_stackf(...) ((void)0)
    #define fmtlval(...) "value"
#endif

void dew_runtime_init();


API_END
