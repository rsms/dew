#include "dew.h"
#include <stdatomic.h>


#if defined(_WIN32)
	#include <windows.h>
	#undef min
	#undef max
#elif defined(__MACH__)
	#undef panic // mach/mach.h defines a function called panic()
	#include <mach/mach.h>
	// redefine panic
	#define panic(fmt, ...) _panic(__FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)
#else
	#include <semaphore.h>
#endif


#define USECS_IN_1_SEC 1000000
#define NSECS_IN_1_SEC 1000000000


//—————————————————————————————————————————————————————————————————————————————————————————————————
// Windows
#if defined(_WIN32)
#warning "UNTESTED implementation on TSem"

int tsem_open(TSem* sp, u32 initcount) {
	assert(initcount <= 0x7fffffff);
	*sp = (sema_t)CreateSemaphoreW(NULL, (int)initcount, 0x7fffffff, NULL);
	return *sp == NULL ? -1 : 0;
}

void tsem_close(TSem* sp) {
	CloseHandle(*sp);
}

bool tsem_wait(TSem* sp) {
	const unsigned long infinite = 0xffffffff;
	return WaitForSingleObject(*sp, infinite) == 0;
}

bool tsem_trywait(TSem* sp) {
	return WaitForSingleObject(*sp, 0) == 0;
}

bool tsem_timedwait(TSem* sp, u64 timeout_usecs) {
	return WaitForSingleObject(*sp, (unsigned long)(timeout_usecs / 1000)) == 0;
}

bool tsem_signal(TSem* sp, u32 count) {
	assert(count > 0);
	// while (!ReleaseSemaphore(*sp, count, NULL)) {}
	return ReleaseSemaphore(*sp, count, NULL);
}

//—————————————————————————————————————————————————————————————————————————————————————————————————
// Darwin macOS/iOS
#elif defined(__MACH__)
// Can't use POSIX semaphores due to
// https://web.archive.org/web/20140109214515/
// http://lists.apple.com/archives/darwin-kernel/2009/Apr/msg00010.html

int tsem_open(TSem* sp, u32 initcount) {
	assert(initcount <= 0x7fffffff);
	kern_return_t rc = semaphore_create(
		mach_task_self(), (semaphore_t*)sp, SYNC_POLICY_FIFO, (int)initcount);
	return rc == KERN_SUCCESS ? 0 : -ENOMEM;
}

void tsem_close(TSem* sp) {
	semaphore_destroy(mach_task_self(), *(semaphore_t*)sp);
}

bool tsem_wait(TSem* sp) {
	semaphore_t s = *(semaphore_t*)sp;
	while (1) {
		kern_return_t rc = semaphore_wait(s);
		// Return values:
		//   KERN_INVALID_ARGUMENT
		//     The specified semaphore is invalid.
		//   KERN_TERMINATED
		//     The specified semaphore has been destroyed.
		//   KERN_ABORTED
		//     The caller was blocked due to a negative count on the semaphore,
		//     and was awoken for a reason not related to the semaphore subsystem
		//     (e.g. thread_terminate).
		//   KERN_SUCCESS
		//     The semaphore wait operation was successful.
		if (rc != KERN_ABORTED)
			return rc == KERN_SUCCESS;
	}
}

bool tsem_trywait(TSem* sp) {
	return tsem_timedwait(sp, 0);
}

int tsem_timedwait(TSem* sp, u64 timeout_usecs) {
	mach_timespec_t ts;
	ts.tv_sec = (u32)(timeout_usecs / USECS_IN_1_SEC);
	ts.tv_nsec = (int)((timeout_usecs % USECS_IN_1_SEC) * 1000);
	// Note:
	// semaphore_wait_deadline was introduced in macOS 10.6
	// semaphore_timedwait was introduced in macOS 10.10
	// https://developer.apple.com/library/prerelease/mac/documentation/General/Reference/
	//   APIDiffsMacOSX10_10SeedDiff/modules/Darwin.html
	semaphore_t s = *(semaphore_t*)sp;
	kern_return_t rc = semaphore_timedwait(s, ts);
	if (rc == KERN_SUCCESS)
		return 0;
	if (rc == KERN_OPERATION_TIMED_OUT)
		return -ETIMEDOUT;
	return -EINTR;
	// TODO: retry on interrupt, update ts; subtract time already waited and retry (loop)
}

bool tsem_signal(TSem* sp, u32 count) {
	assert(count > 0);
	semaphore_t s = *(semaphore_t*)sp;
	kern_return_t rc = 0; // KERN_SUCCESS
	while (count-- > 0)
		rc += semaphore_signal(s);
	return rc == KERN_SUCCESS;
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

bool tsem_wait(TSem* sp) {
	// http://stackoverflow.com/questions/2013181/
	// gdb-causes-sem-wait-to-fail-with-eintr-error
	int rc;
	do {
		rc = sem_wait((sem_t*)sp);
	} while (rc == -1 && errno == EINTR);
	return rc == 0;
}

bool tsem_trywait(TSem* sp) {
	int rc;
	do {
		rc = sem_trywait((sem_t*)sp);
	} while (rc == -1 && errno == EINTR);
	return rc == 0;
}

int tsem_timedwait(TSem* sp, u64 timeout_usecs) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += (time_t)(timeout_usecs / USECS_IN_1_SEC);
	ts.tv_nsec += (long)(timeout_usecs % USECS_IN_1_SEC) * 1000;
	// sem_timedwait bombs if you have more than 1e9 in tv_nsec
	// so we have to clean things up before passing it in
	if (ts.tv_nsec >= NSECS_IN_1_SEC) {
		ts.tv_nsec -= NSECS_IN_1_SEC;
		++ts.tv_sec;
	}
	return sem_timedwait((sem_t*)sp, &ts) ? -errno : 0;
	// while (sem_timedwait((sem_t*)sp, &ts) && errno == EINTR) {}
	// return -errno;
}

bool tsem_signal(TSem* sp, u32 count) {
	assert(count > 0);
	while (count-- > 0) {
		while (sem_post((sem_t*)sp) == -1)
			return false;
	}
	return true;
}

#endif

// TODO: implementation based on futex for Linux and OpenBSD


//—————————————————————————————————————————————————————————————————————————————————————————————————
// tsem2

#define TSEM_CLOSED 0x7fffffff

int tsem2_open(TSem* s, _Atomic(i32)* countp, u32 initcount) {
	assert(initcount != TSEM_CLOSED);
    atomic_store_explicit(countp, initcount, memory_order_relaxed);
    return tsem_open(s, 0);
}

void tsem2_softclose(TSem* s, _Atomic(int32_t)* countp) {
	i32 count = atomic_load_explicit(countp, memory_order_acquire);
	for (;;) {
        if (count == TSEM_CLOSED)
        	return; // closed by another thread
    	if (atomic_compare_exchange_weak_explicit(
                countp, &count, TSEM_CLOSED, memory_order_acquire, memory_order_release))
    	{
    		break;
    	}
	}
	if UNLIKELY(count > 0)
        tsem_signal(s, (u32)count);
}

void tsem2_close(TSem* s, _Atomic(i32)* countp) {
	atomic_store_explicit(countp, TSEM_CLOSED, memory_order_relaxed);
	tsem_close(s);
}

int tsem2_wait1(TSem* s, _Atomic(i32)* countp, u64 timeout_usecs) {
    i32 old_count;
    u32 spin = 3;
    while (--spin > 0) {
        i32 old_count = atomic_load_explicit(countp, memory_order_acquire);
        if (old_count == TSEM_CLOSED)
        	return -EINTR; // tsem has closed
        if (old_count > 0 &&
            atomic_compare_exchange_weak_explicit(
                countp, &old_count, old_count - 1, memory_order_acquire, memory_order_release))
        {
            return 0;
        }
    }
    old_count = atomic_fetch_sub_explicit(countp, 1, memory_order_acquire);
    if (old_count > 0)
        return 0;
    if (timeout_usecs == 0)
        return tsem_wait(s);
    int r = tsem_timedwait(s, timeout_usecs);
    if (r == 0)
        return 0;
    if (r != -ETIMEDOUT)
    	return r;
    // At this point, we've timed out waiting for the semaphore, but the count is still
    // decremented indicating we may still be waiting on it. So we have to re-adjust the count,
    // but only if the semaphore wasn't signaled enough times for us too since then.
    // If it was, we need to release the semaphore too.
    for (;;) {
        old_count = atomic_load_explicit(countp, memory_order_acquire);
        if (old_count >= 0 && tsem_trywait(s))
            return 0;
        if (old_count < 0 &&
            atomic_compare_exchange_weak_explicit(
                countp, &old_count, old_count + 1, memory_order_acquire, memory_order_release))
        {
            return -EINTR;
        }
    }
}

bool tsem2_trywait(TSem* s, _Atomic(i32)* countp) {
    i32 old_count = atomic_load_explicit(countp, memory_order_acquire);
    while (old_count > 0 && old_count != TSEM_CLOSED) {
        if (atomic_compare_exchange_weak_explicit(
                countp, &old_count, old_count - 1, memory_order_acquire, memory_order_relaxed))
        {
            return true;
        }
    }
    return false;
}


void tsem2_signal(TSem* s, _Atomic(i32)* countp, u32 count) {
    assert(count > 0 && count <= (u32)I32_MAX);
    i32 old_count = atomic_fetch_add_explicit(countp, count, memory_order_release);
    assertf(old_count != TSEM_CLOSED, "closed");
    i32 release_count = -old_count < (i32)count ? -old_count : (i32)count;
    if UNLIKELY(release_count > 0)
        tsem_signal(s, (u32)release_count);
}
