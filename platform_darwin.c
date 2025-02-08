#include "runtime.h"
#include <sys/event.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <mach/mach_time.h> // XXX
#include <fcntl.h>


// #define TRACE_KQUEUE
// #define TRACE_KQUEUE_TIMEOUT


typedef struct kevent64_s KEv;


#ifdef TRACE_KQUEUE
	#define trace(fmt, ...) dlog("\e[1;36m%-15s│\e[0m " fmt, __FUNCTION__, ##__VA_ARGS__)

	const char* kev_filter_str(int filter) {
		switch (filter) {
			case EVFILT_READ:     return "READ";
			case EVFILT_WRITE:    return "WRITE";
			case EVFILT_AIO:      return "AIO";
			case EVFILT_VNODE:    return "VNODE";
			case EVFILT_PROC:     return "PROC";
			case EVFILT_SIGNAL:   return "SIGNAL";
			case EVFILT_TIMER:    return "TIMER";
			case EVFILT_MACHPORT: return "MACHPORT";
			case EVFILT_FS:       return "FS";
			case EVFILT_USER:     return "USER";
			case EVFILT_VM:       return "VM";
			case EVFILT_EXCEPT:   return "EXCEPT";
			default:              return "?";
		}
	}
#else
	#define trace(fmt, ...) ((void)0)
#endif


#ifdef TRACE_KQUEUE_TIMEOUT
	#define trace_timeout(fmt, ...) trace("timeout: " fmt, ##__VA_ARGS__)
#else
	#define trace_timeout(...) ((void)0)
#endif


int iopoll_init(IOPoll* iopoll, S* s) {
	iopoll->s = s;
	iopoll->kq = kqueue();
	if UNLIKELY(iopoll->kq == -1)
		return -errno;

	// setup event used by iopoll_interrupt
	// TODO: consider doing this in the first kevent64 syscall in iopoll_poll instead.
	// On my 2021 M1 MacBook kevent64 takes a total of 750ns, so no biggie.
	KEv req = {
		.ident = (uintptr)iopoll,
  		.filter = EVFILT_USER,
  		.flags = EV_ADD | EV_CLEAR,
	};
	int err = kevent64(iopoll->kq, &req, 1, NULL, 0, 0, NULL);
	if UNLIKELY(err < 0) {
		dlog("kevent64 failed to setup EVFILT_USER");
		close(iopoll->kq);
		return err;
	}

	return 0;
}


void iopoll_dispose(IOPoll* iopoll) {
	close(iopoll->kq);
}


int iopoll_interrupt(IOPoll* iopoll) {
	// Note: S guarantees that this function is only called exactly once
	KEv req = {
		.ident = (uintptr)iopoll,
  		.filter = EVFILT_USER,
  		.fflags = NOTE_TRIGGER,
	};
	int err = kevent64(iopoll->kq, &req, 1, NULL, 0, 0, NULL);
	if UNLIKELY(err < 0) {
		dlog("kevent64 failed to setup EVFILT_USER");
		close(iopoll->kq);
		return err;
	}
	return 0;
}


#ifdef TRACE_KQUEUE
	static void trace_kevent(const KEv* ev) {
		char flagstr[128] = {0};
		if (ev->flags&EV_ADD) strcat(flagstr, "|ADD");
		if (ev->flags&EV_DELETE) strcat(flagstr, "|DELETE");
		if (ev->flags&EV_ENABLE) strcat(flagstr, "|ENABLE");
		if (ev->flags&EV_DISABLE) strcat(flagstr, "|DISABLE");
		if (ev->flags&EV_ONESHOT) strcat(flagstr, "|ONESHOT");
		if (ev->flags&EV_CLEAR) strcat(flagstr, "|CLEAR");
		if (ev->flags&EV_RECEIPT) strcat(flagstr, "|RECEIPT");
		if (ev->flags&EV_DISPATCH) strcat(flagstr, "|DISPATCH");
		if (ev->flags&EV_UDATA_SPECIFIC) strcat(flagstr, "|UDATA_SPECIFIC");
		if (ev->flags&EV_VANISHED) strcat(flagstr, "|VANISHED");
		if (ev->flags&EV_SYSFLAGS) strcat(flagstr, "|SYSFLAGS");
		if (ev->flags&EV_FLAG0) strcat(flagstr, "|FLAG0");
		if (ev->flags&EV_FLAG1) strcat(flagstr, "|FLAG1");
		if (ev->flags&EV_EOF) strcat(flagstr, "|EOF");
		if (ev->flags&EV_ERROR) strcat(flagstr, "|ERROR");
		if (*flagstr) *flagstr = ' ';
		trace("kev> %llu %s (flags 0x%04x%s) (fflags 0x%08x) (data 0x%llx)",
		      ev->ident,
		      kev_filter_str(ev->filter),
		      ev->flags, flagstr,
		      ev->fflags,
		      ev->data);
	}
#else
	#define trace_kevent(ev) ((void)0)
#endif


int iopoll_poll(IOPoll* iopoll, DTime deadline, DTimeDuration deadline_leeway) {
	u32 flags = 0;
	struct timespec ts = {};
	struct timespec* tp;
	u64 event_count = 0;
	KEv events[64];
	KEv req;
	IODesc* wakev[64];
	u32 wakec = 0;

	for (;;) {
		// setup deadline by configuring ts (which is relative to "now")
		#ifdef TRACE_KQUEUE_TIMEOUT
			DTime now = 0;
		#endif
		int reqc = 0;
		if (deadline == (DTime)-1) {
			// no deadline
			trace_timeout("none");
			tp = NULL;
		} else if (deadline == 0) {
			// immediate deadline (zero ts is treated same as flag KEVENT_FLAG_IMMEDIATE)
			trace_timeout("immediate");
			tp = &ts;
			ts.tv_sec = 0;
			ts.tv_nsec = 0;
		} else {
			// specific deadline
			//
			// Note: kevent64 with a timeout is consistently 1ms late (Observed on macOS 12.5).
			// If we use an absolute timer with NOTE_CRITICAL timeout becomes more accurate.
			#ifdef TRACE_KQUEUE_TIMEOUT
				now = DTimeNow();
				trace_timeout("%.3fms (%.3fms leeway)\n",
				              (double)DTimeBetween(deadline,now)/1000000.0,
				              deadline_leeway == 0 ? 0.0 : (double)deadline_leeway/1000000.0);
			#endif
			DTimeDuration duration = DTimeUntil(deadline);
			if (duration < 0)
				duration = 0;
			if (duration < 1*D_TIME_MICROSECOND) {
				// Deadline is sooner than the practical precision (+ overhead of wakeup.)
				// Immediate deadline.
				trace_timeout("  deadline too soon; use to immediate timeout");
				tp = &ts;
				ts.tv_sec = 0;
				ts.tv_nsec = 0;
			} else if (deadline_leeway < 0 || deadline_leeway >= D_TIME_MILLISECOND) {
				// leeway is unspecified (-1) or too large*;
				// no need for a timer, just kevent timeout.
				//
				// * "too large": More work is needed in the runtime 'timers' implementation
				// to support "true" (large) leeway. As it currently stands, the priority queue
				// of timers is ordered by deadline ('when') without regard to leeway.
				// If we have two timers (1000,1000) and (1001,1) the second timer with a 1 leeway
				// may actually be late because of the first timer with leeway 1000.
				// So for now, only communicate leeway when it's really small.
				DTimeDurationTimespec(duration, &ts);
				// Darwin returns EINVAL if the sleep time is too long
				if (ts.tv_sec > 1000000)
					ts.tv_sec = 1000000;
				tp = &ts;
			} else {
				// use a timer to communicate leeway
				tp = NULL;
				struct timeval tv;
				gettimeofday(&tv, NULL);
				u64 deadline2 = (u64)tv.tv_sec*1000000000 + (u64)tv.tv_usec*1000;
				deadline2 += (u64)duration;
				req = (KEv){
					.ident = (uintptr)&req,
					.filter = EVFILT_TIMER,
					.flags = EV_ADD | EV_ONESHOT,
					.fflags = NOTE_ABSOLUTE | NOTE_NSECONDS
					        | NOTE_MACH_CONTINUOUS_TIME | NOTE_LEEWAY,
					.data = (i64)deadline2,
					.ext = { 0, deadline_leeway },
				};
				if (deadline_leeway <= D_TIME_MILLISECOND)
					req.fflags |= NOTE_CRITICAL;
				reqc = 1;
			}
		}

		// poll
		int n = kevent64(iopoll->kq, &req, reqc, events, countof(events), flags, tp);

		#ifdef TRACE_KQUEUE_TIMEOUT
			DTime now2 = DTimeNow();
			trace("kevent64 returned after %.3fms\n", (double)(now2 - now) / 1000000.0);
		#endif

		// check for error
		if UNLIKELY(n < 0) {
			if (n != -EINTR && n != -ETIMEDOUT) {
				logerr("kevent on fd %d failed: %s", iopoll->kq, strerror(-n));
				return -n;
			}
			if (deadline > 0) {
				// caller should recalculate deadline and call us back
				return 0;
			}
			// try again
			continue;
		}

		// process events
		for (int i = 0; i < n; i++) {
			KEv* ev = &events[i];

			// trace event information
			trace_kevent(ev);

			// check if iopoll_interrupt was called
			if (ev->filter == EVFILT_USER) {
				trace("interruped");
				break;
			}

			// nothing to do for TIMER events as they are only used for precise poll timeout
			if (ev->filter == EVFILT_TIMER)
				continue;

			// get IODesc passed along event in udata
			IODesc* d = (IODesc*)(uintptr)ev->udata;

			// check if d as stored in event is outdated
			if (d->seq != (u32)ev->ext[0]) {
				trace("ignoring outdated event (IODesc seq %u != %u)", d->seq, (u32)ev->ext[0]);
				continue;
			}

			// transfer state of kevent to IODesc
			i64 n;
			switch (ev->filter) {
				case EVFILT_READ:
					// Note: ev->data contains total amount to read, not just "for this event"
					d->nread = ev->data;
					if (ev->flags & EV_ERROR)
						d->nread = -d->nread;
					d->events = 'r'; // note: set bits, don't add bits
					// See https://github.com/golang/go/blob/release-branch.go1.24/
					//     src/runtime/netpoll_kqueue.go#L147-L155
					if (ev->flags & EV_EOF)
						d->events += 'w';
					goto wake;
				case EVFILT_WRITE:
					d->nwrite = ev->data;
					if (ev->flags & EV_ERROR)
						d->nwrite = -d->nwrite;
					d->events = 'w';
					goto wake;
			}
			if ((ev->flags & EV_ERROR) == 0) {
				dlog("warning: kevent returned unexpected filter (0x%x); ignoring", ev->filter);
				continue;
			}
		wake:
			if (d->t) {
				// batch wakeups so that if we receive multiple events for the same fd
				// we avoid missing a wakeup.
				assert(wakec < countof(wakev));
				wakev[wakec++] = d;
			}
		}

		return s_iopoll_wake(iopoll->s, wakev, wakec);
	}

	return 0;
}


int iopoll_open(IOPoll* iopoll, IODesc* d) {
	// adds fd in edge-triggered mode (EV_CLEAR) for the whole fd lifetime.
	// The notifications are automatically unregistered when fd is closed.
	KEv reqs[2] = {
		{
			.ident = d->fd,
	  		.filter = EVFILT_READ,
	  		.flags = EV_ADD | EV_CLEAR,
			.udata = (uintptr)d,
			.ext[0] = (uintptr)d->seq,
		},
		{
			.ident = d->fd,
	  		.filter = EVFILT_WRITE,
	  		.flags = EV_ADD | EV_CLEAR,
			.udata = (uintptr)d,
			.ext[0] = (uintptr)d->seq,
		},
	};
	int nev = kevent64(iopoll->kq, reqs, countof(reqs), NULL, 0, 0, NULL);
	return nev < 0 ? -nev : 0;
}


int iopoll_close(IOPoll* iopoll, IODesc* d) {
	// No need to unregister because calling close() on fd will remove any kevents
	// that reference the descriptor
	//trace("fd=%d", d->fd);
	if (d->fd > -1)
		return close(d->fd) ? -errno : 0;
	return 0;
}
