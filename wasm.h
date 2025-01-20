#pragma once
#ifdef __wasm__
#include "dew.h"
API_BEGIN

#define WASM_IMPORT extern
#define WASM_EXPORT __attribute__((visibility("default")))

typedef struct IPCMsg {
	int placeholder;
} IPCMsg;

typedef enum SysOp : u32 {
	SysOp_NANOSLEEP = 1, // u32 sec, u32 nsec, u32* nullable rem_sec, u32* nullable rem_nsec
	SysOp_IPCRECV   = 2, // IPCMsg* msg, u32 flags
} SysOp;

WASM_IMPORT long syscall(SysOp, long, long, long, long, long);
WASM_IMPORT long syscall_I(SysOp, int64_t, long, long, long, long);
WASM_IMPORT long syscall_f(SysOp, double, long, long, long, long);

WASM_IMPORT void* _Nullable wlongjmp_scope(void(^try_block)());
WASM_IMPORT NORET wlongjmp(void* _Nullable retval);

// libc functions missing from wasi libc, implemented by us
int nanosleep(const struct timespec* req, struct timespec* rem);
unsigned int sleep(unsigned int seconds);

API_END
#endif // defined(__wasm__)
