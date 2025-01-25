#include "dew.h"
#warning "runloop not yet implemented for WASM"


int RunloopCreate(Runloop** result) {
	return -ENOSYS;
}


void RunloopFree(Runloop* rl) {
}


int RunloopRun(Runloop* rl, u32 flags) {
	return -ENOSYS;
}


int RunloopRemove(Runloop* rl, int reqid) {
	return -ENOSYS;
}


int RunloopAddTimeout(Runloop* rl, TimerCallback cb, u64 ud, DTime deadline) {
	return -ENOSYS;
}


int RunloopAddInterval(Runloop* rl, TimerCallback cb, u64 ud, DTimeDuration interval) {
	return -ENOSYS;
}