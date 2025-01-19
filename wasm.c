#include "wasm.h"
API_BEGIN


WASM_EXPORT void exec_block(void(^block)()) {
	block();
}


int nanosleep(const struct timespec* req, struct timespec* rem) {
	u32 rem_sec = 0;
	u32 rem_nsec = 0;
	u32 sec, nsec;
	if ((u64)req->tv_sec > (u64)U32_MAX) {
		sec = U32_MAX;
	} else {
		sec = (u32)req->tv_sec;
	}
	if ((u64)req->tv_nsec > (u64)U32_MAX) {
		nsec = U32_MAX;
	} else {
		nsec = (u32)req->tv_nsec;
	}
	int r = syscall(SysOp_NANOSLEEP, sec, nsec, (uintptr)&rem_sec, (uintptr)&rem_nsec, 0);
	if (rem) {
		rem->tv_sec = rem_sec;
		rem->tv_nsec = rem_nsec;
	}
 	return r;
}


unsigned int sleep(unsigned int seconds) {
	struct timespec rem;
	int r = nanosleep(&(struct timespec){ .tv_sec = seconds }, &rem);
	if (r == 0)
		return 0;
	u32 rem_sec;
	if (rem.tv_sec >= U32_MAX) {
		rem_sec = U32_MAX;
	} else {
		rem_sec = rem.tv_sec + (u32)(rem.tv_nsec >= 500000000);
	}
	return rem_sec;
}


API_END
