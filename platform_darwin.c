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

int iopoll_open(IOPoll* iopoll, S* s) {
	iopoll->kq = kqueue();
	if UNLIKELY(iopoll->kq == -1)
		return -errno;
	iopoll->s = s;
	return 0;
}


void iopoll_close(IOPoll* iopoll) {
	close(iopoll->kq);
}


int iopoll_poll(IOPoll* iopoll, DTime deadline) {
	u32 flags = 0;
	struct timespec ts = {};
	struct timespec* tp;
	u64 event_count = 0;
	KEv events[64];

	for (;;) {
		// configure syscall ts, which is relative to "now"
		if (deadline == (DTime)-1) {
			tp = NULL;
		} else if (deadline == 0) {
			tp = &ts;
			ts.tv_sec = 0;
			ts.tv_nsec = 0;
		} else {
			DTimeDuration duration = DTimeUntil(deadline);
			if (duration < 0)
				duration = 0;
			DTimeDurationTimespec(duration, &ts);
			// Darwin returns EINVAL if the sleep time is too long
			if (ts.tv_sec > 1000000)
				ts.tv_sec = 1000000;
			tp = &ts;
		}

		int n = kevent64(iopoll->kq, NULL, 0, events, countof(events), flags, tp);

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

		for (int i = 0; i < n; i++) {
			KEv* ev = &events[i];

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

			IOPollDesc* pd = (IOPollDesc*)(uintptr)ev->udata;
			if (pd->seq != ev->ext[0]) {
				trace("ignoring outdated event (pd->seq)");
				continue;
			}
			pd->active = (ev->flags & EV_ONESHOT) == 0;
			pd->result = ev->data;
			if (ev->flags & EV_ERROR)
				pd->result = -ev->data;
			switch (ev->filter) {
				case EVFILT_READ:
					pd->events = IOPollDesc_EV_READ;
					// See https://github.com/golang/go/blob/release-branch.go1.24/
					//     src/runtime/netpoll_kqueue.go#L147-L155
					// if (ev->flags&EV_EOF)
					// 	pd->events |= IOPollDesc_EV_WRITE;
					goto wake;
				case EVFILT_WRITE:
					pd->events = IOPollDesc_EV_WRITE;
					goto wake;
				case EVFILT_TIMER:
					pd->events = IOPollDesc_EV_TIMER;
					goto wake;
			}
			if ((ev->flags & EV_ERROR) == 0) {
				dlog("warning: kevent returned unexpected filter (0x%x); ignoring", ev->filter);
				continue;
			}
			wake:
			// pd->result = -ENOENT; // XXX simulate error
			if (pd->use == IOPollDesc_USE_CONNECT) {
				// 'connect' that fails does not set EV_ERROR, but rather EV_EOF and we have to
				// retrieve the actual error via getsockopt(SO_ERROR).
				if (ev->flags&EV_EOF && (ev->flags & EV_ERROR) == 0) {
					int error = 0;
					socklen_t len = sizeof(error);
					if (getsockopt((int)ev->ident, SOL_SOCKET, SO_ERROR, &error, &len) == 0) {
						if (error != 0)
							pd->result = -error;
					}
				}
			} else if (ev->flags&EV_EOF) {
				pd->events |= IOPollDesc_EV_EOF;
			}
			if (ev->flags & EV_ERROR) {
				trace("ready " T_ID_F " error=%s", t_id(pd->t), strerror((int)-pd->result));
			} else {
				trace("ready " T_ID_F, t_id(pd->t));
			}
			s_iopoll_wake(iopoll->s, pd);
		}
		break;
	}

	return 0;
}


int iopoll_add_fd(IOPoll* iopoll, IOPollDesc* pd, int fd, u8 events) {
	// adds fd in edge-triggered mode (EV_CLEAR) for the whole fd lifetime.
	// The notifications are automatically unregistered when fd is closed.
	KEv reqs[2];
	int nreqs = 0;
	u16 flags = 0;
	// if (pd->use == IOPollDesc_USE_CONNECT)
	// 	flags |= EV_ONESHOT;
	if (events & IOPollDesc_EV_READ) {
		reqs[nreqs++] = (KEv){
			.ident = (uintptr)fd,
	  		.filter = EVFILT_READ,
	  		.flags = EV_ADD | EV_CLEAR,
			.udata = (uintptr)pd,
			.ext[0] = (uintptr)pd->seq,
		};
	}
	if (events & IOPollDesc_EV_WRITE) {
		reqs[nreqs++] = (KEv){
			.ident = (uintptr)fd,
	  		.filter = EVFILT_WRITE,
	  		.flags = EV_ADD | EV_CLEAR,
			.udata = (uintptr)pd,
			.ext[0] = (uintptr)pd->seq,
		};
	}
	if (nreqs == 0) {
		dlog("error: no events requested");
		return -EINVAL;
	}
	int nev = kevent64(iopoll->kq, reqs, nreqs, NULL, 0, 0, NULL);
	return nev < 0 ? -nev : 0;
}


int iopoll_remove_fd(IOPoll* iopoll, int fd) {
	// No need to unregister because calling close() on fd will remove any kevents
	// that reference the descriptor
	return 0;
}


int iopoll_add_timer(IOPoll* iopoll, IOPollDesc* pd, DTime deadline) {
	#if 1 /* implementation using absolute deadline with gettimeofday */
		struct timeval tv;
		gettimeofday(&tv, NULL);
		u64 deadline2 = (u64)tv.tv_sec*1000000000 + (u64)tv.tv_usec*1000;
		deadline2 += (u64)DTimeUntil(deadline);
		const i64 estimated_minimum_overhead = 1000; //ns
		KEv req = {
			.ident = (uintptr)pd->t,
	  		.filter = EVFILT_TIMER,
	  		.flags = EV_ADD | EV_ONESHOT,
	  		.fflags = NOTE_ABSOLUTE | NOTE_NSECONDS | NOTE_MACH_CONTINUOUS_TIME | NOTE_CRITICAL,
			.data = (i64)(deadline2 - estimated_minimum_overhead),
			.udata = (uintptr)pd,
			.ext[0] = (uintptr)pd->seq,
		};
		int nev = kevent64(iopoll->kq, &req, 1, NULL, 0, 0, NULL);
		return nev < 0 ? -nev : 0;
	#else /* implementation using relative deadline */
		const i64 estimated_minimum_overhead = 1000; //ns
		i64 ns = DTimeUntil(deadline) - estimated_minimum_overhead;
		if (ns < 0) ns = 0;
		KEv req = {
			.ident = (uintptr)d->t,
	  		.filter = EVFILT_TIMER,
	  		.flags = EV_ADD | EV_ONESHOT,
	  		.fflags = NOTE_NSECONDS,
			.data = ns,
			.udata = (uintptr)d,
			// .ext[0] = (uintptr)userdata,
		};
		int nev = kevent64(iopoll->kq, &req, 1, NULL, 0, 0, NULL);
		return nev < 0 ? -nev : 0;
	#endif
}

