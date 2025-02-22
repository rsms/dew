#include "runtime.h"


int iopoll_init(IOPoll* iopoll, S* s) {
	return -ENOSYS;
}


void iopoll_dispose(IOPoll* iopoll) {
}


int iopoll_interrupt(IOPoll* iopoll) {
	return -ENOSYS;
}


int iopoll_poll(IOPoll* iopoll, DTime deadline, DTimeDuration deadline_leeway) {
	return -ENOSYS;
}


int iopoll_open(IOPoll* iopoll, IODesc* d) {
	return -ENOSYS;
}


int iopoll_close(IOPoll* iopoll, IODesc* d) {
	return -ENOSYS;
}
