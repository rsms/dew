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

// bool IS_POW2(T x) returns true if x is a power-of-two value
#define IS_POW2(x)    ({ __typeof__(x) xtmp__ = (x); IS_POW2_X(xtmp__); })
#define IS_POW2_X(x)  ( ((x) & ((x) - 1)) == 0 )


#ifdef DEBUG
	#define dlog(fmt, ...) ( \
		fprintf(stderr, "[%s] " fmt " (%s:%d)\n", \
		        __FUNCTION__, ##__VA_ARGS__, __FILE__, __LINE__), \
		fflush(stderr) \
	)
#else
	#define dlog(fmt, ...) ((void)0)
#endif

#define logerr(fmt, ...) \
	(fprintf(stderr, "%s: " fmt "\n", g_prog, ##__VA_ARGS__), fflush(stderr))
