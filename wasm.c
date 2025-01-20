#include "wasm.h"
API_BEGIN

enum { ASYNCIFY_STACK_SIZE = 64*1024 };
WASM_EXPORT struct {
	// See struct at https://github.com/WebAssembly/binaryen/blob/
	// fd8b2bd43d73cf1976426e60c22c5261fa343510/src/passes/Asyncify.cpp#L106-L120
	void* stack_start;
	void* stack_end;
	u8    data[ASYNCIFY_STACK_SIZE - 8];
} asyncify_data = {
	.stack_start = asyncify_data.data,
	.stack_end = asyncify_data.data + ASYNCIFY_STACK_SIZE,
};

// extern char __heap_base;
// extern char __heap_end;

__attribute__((constructor)) static void wasm_init() {
	// printf("__heap_base:              %p\n", &__heap_base);
	// printf("__heap_end:               %p\n", &__heap_end);
	// printf("asyncify_data             %p\n", &asyncify_data);
	// printf("asyncify_data.stack_start %p\n", asyncify_data.stack_start);
	// printf("asyncify_data.stack_end   %p\n", asyncify_data.stack_end);
}


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
