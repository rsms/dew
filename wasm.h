#pragma once
#ifdef __wasm__
#include "dew.h"
API_BEGIN

#define WASM_IMPORT extern
#define WASM_EXPORT __attribute__((visibility("default")))

typedef struct IPCMsg {
	u32 type;
	union {
		struct {
			char* srcptr;
			usize srclen;
			// char* srcname;
		} runsource;
	};
} IPCMsg;

typedef struct WIOReq {
	u16 op;
	u16 _unused;
	u32 flags;
} WIOReq;

typedef struct WIOEv {
	u16 op;
	u16 _unused;
	u32 flags;
} WIOEv;

typedef struct WIO {
	// request queue
	u32    req_q_head;
	u32    req_q_tail;
	WIOReq req_q_v[128];

	// event queue
	u32    ev_q_head;
	u32    ev_q_tail;
	WIOEv  ev_q_v[128];
} WIO;

typedef enum SysOp : u32 {
	SysOp_NANOSLEEP = 1, // u32 sec, u32 nsec, u32* nullable rem_sec, u32* nullable rem_nsec
	SysOp_IPCRECV   = 2, // IPCMsg* msg, u32 flags
	SysOp_IOWAIT    = 3, //
} SysOp;

WASM_IMPORT long syscall(SysOp, long, long, long, long, long);
WASM_IMPORT long syscall_I(SysOp, int64_t, long, long, long, long);
WASM_IMPORT long syscall_f(SysOp, double, long, long, long, long);

WASM_IMPORT void* _Nullable wlongjmp_scope(void(^try_block)());
WASM_IMPORT NORET wlongjmp(void* _Nullable retval);

// int wio_wait();

// libc functions missing from wasi libc, implemented by us
int nanosleep(const struct timespec* req, struct timespec* rem);
unsigned int sleep(unsigned int seconds);

API_END
#endif // defined(__wasm__)
