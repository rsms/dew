#include "dew.h"
#include <sys/event.h>
#include <sys/time.h>
#include <mach/mach_time.h> // XXX
#include <fcntl.h>


#define trace(fmt, ...) dlog("[runloop] " fmt, ##__VA_ARGS__)


// DTimeMachAbsoluteTime returns a mach_absolute_time value for a DTime value.
// Implemented in time.c
u64 DTimeMachAbsoluteTime(DTime t);


typedef struct kevent64_s KEv;
// struct kevent64_s {
//     u64 ident;  // identifier for this event
//     i16 filter; // filter for event
//     u16 flags;  // general flags
//     u32 fflags; // filter-specific flags
//     i64 data;   // filter-specific data
//     u64 udata;  // opaque user data identifier
//     u64 ext[2]; // filter-specific extensions
// };

typedef struct Handler {
	RunloopCallback nullable cb;
	void* nullable           userdata;
} Handler;


struct Runloop {
	int kq;      // kqueue fd
	int idgen;   // generator for kevent64_s.ident
	u32 nactive; // number of active, "live" events

	// 'requests' is a queue of requests to be submitted with next runloop frame.
	// Appended to during a runloop frame and completely consumed by RunloopSubmit.
	// The 'ident' is assigned the index of the corresponding handler.
	KEv* requests;
	u32  requests_cap; // capacity of requests array
	u32  requests_len; // entries in requests array

	// 'handlers' is a set of pending callbacks for events.
	// These may be long lived.
	// A handler is referenced by index (not pointer) since handlers array may be realloc'd.
	// A handler is allocated by linear scan for the first entry with .cb==NULL.
	// Note that a "request" may be fulfilled many times, for example a repeating timer,
	// in which case the "request" entry remains (until no more events are expected.)
	Handler* handlers;
	u32      handlers_cap;   // capacity of handlers array
	u32      handlers_nlive; // number of in-use handlers
};


int RunloopCreate(Runloop** result) {
	Runloop* rl = calloc(1, sizeof(Runloop));
	if (!rl)
		return -ENOMEM;
	rl->requests_cap = 8;
	KEv* requests = malloc(sizeof(KEv)*rl->requests_cap);
	if (!requests) {
		free(rl);
		return -ENOMEM;
	}
	rl->requests = requests;
	rl->kq = kqueue();
	if UNLIKELY(rl->kq == -1) {
		free(requests);
		free(rl);
		return -errno;
	}
	*result = rl;
	return 0;
}


void RunloopFree(Runloop* rl) {
	close(rl->kq);
	free(rl->handlers);
	free(rl->requests);
	free(rl);
}


int RunloopSubmit(Runloop* rl) {
	if (rl->requests_len == 0)
		return 0;
	int nev = kevent64(rl->kq, rl->requests, rl->requests_len, NULL, 0, 0, NULL);
	rl->requests_len = 0;
	return nev < 0 ? -errno : nev;
}


static bool use_ext0(i16 ev_filter) {
	// According to the kqueue manual, EVFILT_MACHPORT is the only event type which uses ext[0]
	return ev_filter != EVFILT_MACHPORT;
	// return false;
}


static int RunloopProcessCompletion(Runloop* rl, KEv* ev) {
	if (ev->flags & EV_ERROR) {
		dlog("#%d (error: %s)", (int)ev->ident, strerror((int)ev->data));
	} else {
		dlog("#%d", (int)ev->ident);
	}
	if (use_ext0(ev->filter)) {
		RunloopCallback cb = (RunloopCallback)(uintptr)ev->udata;
		if (cb)
			cb(rl, (int)ev->ident, ev->data, (void*)(uintptr)ev->ext[0]);
		if (ev->flags & EV_ONESHOT)
			rl->nactive--;
	} else {
		// uses handler slot
		u32 idx = ev->udata;
		rl->handlers[idx].cb(rl, (int)ev->ident, ev->data, rl->handlers[idx].userdata);
		if (ev->flags & EV_ONESHOT) {
			trace("free handler #%d", idx);
			assert(rl->handlers_nlive > 0);
			rl->handlers[idx].cb = NULL;
			rl->handlers_nlive--;
			rl->nactive--;
		}
	}
	return 0;
}


int RunloopProcess(Runloop* rl, u32 min_completions, DTime deadline) {
	u32 flags = 0;
	struct timespec timeout = {};
	u64 completion_count = 0;
	KEv completions[8];

	for (;;) {
		dlog(">>> requests_len %u, nactive %u", rl->requests_len, rl->nactive);
		if (rl->requests_len == 0 && rl->nactive == 0)
			return 0;

		// configure syscall timeout, which is relative to "now"
		struct timespec* timeoutptr;
		if (deadline > 0) {
			DTimeDuration duration = DTimeUntil(deadline);
			if (duration <= 0) // already past deadline
				return -ETIMEDOUT;
			DTimeDurationTimespec(duration, &timeout);
			timeoutptr = &timeout;
		} else {
			timeoutptr = NULL;
		}

		// Note: can pass NULL for 'timeout' argument for "no timeout".
		// Note: kevent64 limits req_len to I32_MAX (0x7fffffff).
		// int kevent64(int kq,
		//              const struct kevent64_s* changelist,
		//              int                      nchanges,
		//              struct kevent64_s*       eventlist,
		//              int                      nevents,
		//              unsigned int             flags,
		//              const struct timespec*   timeout);
		int nev = kevent64(rl->kq,
		                   rl->requests, rl->requests_len,
		                   completions, countof(completions),
		                   flags,
		                   timeoutptr);
		trace("kevent64(nchanges=%d) => %d", (int)rl->requests_len, nev);
		rl->requests_len = 0;
		if (nev == -1) {
			int err = -errno; // should we close(kq) here?
			dlog("kevent64() error: %s", strerror(-err));
			return err;
		}

		// process completions
		for (int i = 0; i < nev; i++)
			RunloopProcessCompletion(rl, &completions[i]);

		// check if we are done
		if (nev == 0)
			return -ETIMEDOUT;
		completion_count += nev;
		if (completion_count >= (u64)min_completions)
			break;
	}

	return (completion_count > 0 || rl->nactive > 0) ? 1 : 0;
}


static bool grow_array(usize elemsize, void** arrayp, u32* capp, bool zero) {
	// try to double the capacity (or start with 8 if cap is 0)
	u32 cap = *capp;
	u32 newcap;
	if (check_mul_overflow(cap > 0 ? cap : 4u, 2u, &newcap)) {
		newcap = cap + 1;
		if (newcap == 0)
			return false;
	}

	// Note: checking for I32_MAX since we are limited by int,
	// since we return errors with <0 from RunloopAdd functions.
	// After all, 2 billion in-flight requests is more than we ever need.
	if (newcap > (u32)I32_MAX)
		return false;

	// calculate new allocation size in bytes
	usize newsize;
	if (check_mul_overflow(elemsize, (usize)newcap, &newsize))
		return false;

	// grow allocation
	void* newarray = realloc(*arrayp, newsize);
	if (!newarray)
		return false;
	if (zero) {
		usize oldsize = elemsize * (usize)*capp;
		memset(newarray + oldsize, 0, newsize - oldsize);
	}
	*arrayp = newarray;
	*capp = newcap;
	return true;
}


static KEv* nullable RunloopAllocReq(
	Runloop*        rl,
	i16             ev_filter,
	RunloopCallback cb,
	void* nullable  userdata)
{
	if UNLIKELY(rl->requests_cap == rl->requests_len) {
		if (!grow_array(sizeof(rl->requests[0]), (void**)&rl->requests, &rl->requests_cap, false))
			return NULL;
	}

	KEv* ev = &rl->requests[rl->requests_len];
	memset(ev, 0, sizeof(*ev));
	if (rl->idgen == I32_MAX)
		rl->idgen = 0;
	ev->ident = rl->idgen++;
	ev->filter = ev_filter;

	if (use_ext0(ev_filter) || cb == NULL) {
		// We can use udata for cb and ext[0] for userdata; no need for a handler slot
		ev->udata = (uintptr)cb;
		ev->ext[0] = (uintptr)userdata;
		rl->requests_len++;
		rl->nactive++;
		trace("enqueue #%d filter=%d", (int)ev->ident, ev_filter);
		return ev;
	}

	// need 'handler' sidecar data for this event
	if UNLIKELY(rl->handlers_cap == rl->handlers_nlive) {
		if (!grow_array(sizeof(rl->handlers[0]), (void**)&rl->handlers, &rl->handlers_cap, true))
			return NULL;
	}

	rl->requests_len++;
	rl->nactive++;

	// select the first free handler slot
	for (u32 idx = 0; idx < rl->handlers_cap; idx++) {
		if (rl->handlers[idx].cb == NULL) {
			trace("alloc handler #%d", idx);
			rl->handlers[idx].cb = cb;
			rl->handlers[idx].userdata = userdata;
			rl->handlers_nlive++;
			ev->udata = idx;
			return ev;
		}
	}
	unreachable();
}


int RunloopTimerCancel(Runloop* rl, int reqid) {
	// look for a not-yet-submitted request
	for (u32 idx = 0; idx < rl->requests_len; idx++) {
		if (rl->requests[idx].ident == (u64)reqid) {
			memmove(&rl->requests[idx], &rl->requests[idx + 1], (rl->requests_len - idx) - 1);
			rl->requests_len--;
			rl->nactive--;
			return 0;
		}
	}

	KEv* ev = RunloopAllocReq(rl, EVFILT_TIMER, NULL, NULL);
	if (!ev)
		return -ENOMEM;
	ev->ident = reqid;
	ev->flags = EV_DELETE;
	// ev->fflags = NOTE_NSECONDS;

	rl->nactive--;

	return ev->ident;
}


int RunloopTimerStart(
	Runloop*        rl,
	RunloopCallback cb,
	void* nullable  userdata,
	DTime           deadline,
	DTimeDuration   leeway)
{
	KEv* ev = RunloopAllocReq(rl, EVFILT_TIMER, cb, userdata);
	if (!ev)
		return -ENOMEM;
	// NOTE_MACHTIME is needed since DTime on darwin is sourced from mach_absolute_time.
	ev->flags = EV_ADD | EV_ONESHOT;

	// Note: as the timer is configured above, it does not tick while the computer is asleep.
	// For a timer that triggers at a "human" time & date, we can use gettimeofday instead.
	#if 1
		struct timeval tv;
		gettimeofday(&tv, NULL);
		u64 deadline2 = (uint64_t)tv.tv_sec*1000000000 + (uint64_t)tv.tv_usec*1000;
		deadline2 += (u64)DTimeUntil(deadline);

		// {
		// 	char buf[9]; // "HH:MM:SS\0"
		// 	tv.tv_sec = deadline2 / 1000000000;
		// 	tv.tv_usec = (deadline2 % 1000000000) / 1000;
		// 	struct tm tm;
		// 	localtime_r(&tv.tv_sec, &tm);
		// 	strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
		// 	if (leeway > 0) {
		// 		dlog("timer is expected to expire at %s.%06d ±%lld%s", buf, tv.tv_usec,
		// 		     leeway < 1000 ? leeway : leeway < 1000000 ? leeway/1000 : leeway/1000000,
		// 		     leeway < 1000 ? "ns"   : leeway < 1000000 ? "us"        : "ms");
		// 	} else {
		// 		dlog("timer is expected to expire at %s.%06d", buf, tv.tv_usec);
		// 	}
		// }

		ev->fflags = NOTE_ABSOLUTE | NOTE_NSECONDS | NOTE_MACH_CONTINUOUS_TIME;
		const i64 estimated_minimum_overhead = 1000;//ns
		ev->data = (i64)deadline2 + estimated_minimum_overhead;
		// if (leeway > 0) {
		// 	ev->fflags |= NOTE_LEEWAY;
		// 	ev->ext[1] = leeway;
		// } else if (leeway == 0) {
			ev->fflags |= NOTE_CRITICAL;
		// }
	#else
		// Use high-precision monotonic mach_absolute_time.
		// Unlike the gettimeofday approach, this does not tick while the computer is asleep.
		// I.e. we cannot use NOTE_MACH_CONTINUOUS_TIME with NOTE_MACHTIME.
		ev->fflags = NOTE_ABSOLUTE | NOTE_MACHTIME;
		ev->data = DTimeMachAbsoluteTime(deadline);
		if (leeway > 0) {
			ev->fflags |= NOTE_LEEWAY;
			ev->ext[1] = DTimeMachAbsoluteTime(leeway);
		} else if (leeway == 0) {
			ev->fflags |= NOTE_CRITICAL;
		}
	#endif

	return ev->ident;
}


int RunloopAddRepeatingTimer(
	Runloop*        rl,
	RunloopCallback cb,
	void* nullable  userdata,
	DTimeDuration   interval,
	DTimeDuration   leeway)
{
	if (interval < 0)
		return -EINVAL;
	KEv* ev = RunloopAllocReq(rl, EVFILT_TIMER, cb, userdata);
	if (!ev)
		return -ENOMEM;
	ev->flags  = EV_ADD | EV_ENABLE;
	ev->fflags = NOTE_NSECONDS;
	ev->data = interval;
	if (leeway > 0) {
		ev->fflags |= NOTE_LEEWAY;
		ev->ext[1] = leeway;
	} else if (leeway == 0) {
		ev->fflags |= NOTE_CRITICAL;
	}
	return ev->ident;
}
