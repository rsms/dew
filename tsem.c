#include "dew.h"
#include <stdatomic.h>


#if defined(_WIN32)
	#include <windows.h>
	#undef min
	#undef max
#elif defined(__MACH__)
	#undef panic // mach/mach.h defines a function called panic()
	#include <mach/mach.h>
	#undef panic
#else
	#include <semaphore.h>
#endif


#define USECS_IN_1_SEC 1000000
#define NSECS_IN_1_SEC 1000000000


//—————————————————————————————————————————————————————————————————————————————————————————————————
// Windows
#if defined(_WIN32)
#warning "UNTESTED implementation of TSem"

int tsem_open(TSem* sp, u32 initcount) {
	assert(initcount <= 0x7fffffff);
	*sp = (sema_t)CreateSemaphoreW(NULL, (int)initcount, 0x7fffffff, NULL);
	return *sp == NULL ? -1 : 0;
}

void tsem_close(TSem* sp) {
	CloseHandle(*sp);
}

void tsem_wait(TSem* sp) {
	const unsigned long infinite = 0xffffffff;
	WaitForSingleObject(*sp, infinite);
}

bool tsem_trywait(TSem* sp) {
	return WaitForSingleObject(*sp, 0) == 0;
}

bool tsem_timedwait(TSem* sp, u64 timeout_usecs) {
	return WaitForSingleObject(*sp, (unsigned long)(timeout_usecs / 1000)) == 0;
}

void tsem_signal(TSem* sp) {
	return ReleaseSemaphore(*sp, 1, NULL);
}

//—————————————————————————————————————————————————————————————————————————————————————————————————
// Darwin: use mach semaphores as POSIX semaphores are broken on macOS
#elif defined(__MACH__)

int tsem_open(TSem* sp, u32 initcount) {
	assert(initcount <= 0x7fffffff);
	kern_return_t rc = semaphore_create(
		mach_task_self(), (semaphore_t*)sp, SYNC_POLICY_FIFO, (int)initcount);
	return rc == KERN_SUCCESS ? 0 : -ENOMEM;
}

void tsem_close(TSem* sp) {
	semaphore_destroy(mach_task_self(), *(semaphore_t*)sp);
}

void tsem_wait(TSem* sp) {
	semaphore_t s = *(semaphore_t*)sp;
	for (;;) {
		// KERN_INVALID_ARGUMENT sp is invalid
		// KERN_TERMINATED       semaphore_destroy was called
		// KERN_ABORTED          like EINTR
		// KERN_SUCCESS
		kern_return_t rc = semaphore_wait(s);
		if (rc != KERN_ABORTED) {
			assert(rc == KERN_SUCCESS);
			break;
		}
	}
}

bool tsem_trywait(TSem* sp) {
	return tsem_timedwait(sp, 0);
}

bool tsem_timedwait(TSem* sp, u64 timeout_usecs) {
	mach_timespec_t ts;
	ts.tv_sec = (u32)(timeout_usecs / USECS_IN_1_SEC);
	ts.tv_nsec = (int)((timeout_usecs % USECS_IN_1_SEC) * 1000);
	semaphore_t s = *(semaphore_t*)sp;
	return semaphore_timedwait(s, ts) == KERN_SUCCESS;
}

void tsem_signal(TSem* sp) {
	semaphore_t s = *(semaphore_t*)sp;
	semaphore_signal(s);
}

//—————————————————————————————————————————————————————————————————————————————————————————————————
// POSIX
#else

int tsem_open(TSem* sp, u32 initcount) {
	return sem_init((sem_t*)sp, 0, initcount) ? -errno : 0;
}

void tsem_close(TSem* sp) {
	sem_destroy((sem_t*)sp);
}

void tsem_wait(TSem* sp) {
	while (sem_wait((sem_t*)sp))
		assert(errno == EINTR);
}

bool tsem_trywait(TSem* sp) {
	return sem_trywait((sem_t*)sp) == 0;
}

bool tsem_timedwait(TSem* sp, u64 timeout_usecs) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += (time_t)(timeout_usecs / USECS_IN_1_SEC);
	ts.tv_nsec += (long)(timeout_usecs % USECS_IN_1_SEC) * 1000;
	// sem_timedwait borks if you have more than 1e9 in tv_nsec
	// so we have to clean things up before passing it in
	if (ts.tv_nsec >= NSECS_IN_1_SEC) {
		ts.tv_nsec -= NSECS_IN_1_SEC;
		++ts.tv_sec;
	}
	for (;;) {
		if LIKELY(sem_timedwait((sem_t*)sp, &ts) == 0)
			return true;
		if (errno != EINTR)
			return false;
	}
}

void tsem_signal(TSem* sp) {
	sem_post((sem_t*)sp);
}

#endif
