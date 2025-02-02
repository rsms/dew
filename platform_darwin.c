#include "runtime.h"
#include <sys/event.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <mach/mach_time.h> // XXX
#include <fcntl.h>


#define TRACE_KQUEUE


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


int iopoll_init(IOPoll* iopoll, S* s) {
	iopoll->kq = kqueue();
	if UNLIKELY(iopoll->kq == -1)
		return -errno;
	iopoll->s = s;
	return 0;
}


void iopoll_shutdown(IOPoll* iopoll) {
	close(iopoll->kq);
}


int iopoll_poll(IOPoll* iopoll, DTime deadline) {
	u32 flags = 0;
	struct timespec ts = {};
	struct timespec* tp;
	u64 event_count = 0;
	KEv events[64];
	IODesc* wakev[64];
	u32 wakec = 0;

	for (;;) {
		// configure ts, which is relative to "now"
		if (deadline == (DTime)-1) {
			// no deadline
			tp = NULL;
		} else if (deadline == 0) {
			// immediate deadline
			tp = &ts;
			ts.tv_sec = 0;
			ts.tv_nsec = 0;
		} else {
			// specific deadline
			DTimeDuration duration = DTimeUntil(deadline);
			if (duration < 0)
				duration = 0;
			DTimeDurationTimespec(duration, &ts);
			// Darwin returns EINVAL if the sleep time is too long
			if (ts.tv_sec > 1000000)
				ts.tv_sec = 1000000;
			tp = &ts;
		}

		// poll
		int n = kevent64(iopoll->kq, NULL, 0, events, countof(events), flags, tp);

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
			#ifdef TRACE_KQUEUE
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
			#endif

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
