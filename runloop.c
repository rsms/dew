#include "dew.h"
#include "libev/config.h"
#include "libev/ev.h"

// #define TRACE_RUNLOOP
// #define TRACE_RUNLOOP_REQALLOC

#ifdef TRACE_RUNLOOP
	#define trace(fmt, ...) dlog("[runloop] " fmt, ##__VA_ARGS__)
#else
	#define trace(fmt, ...) ((void)0)
#endif

#ifdef TRACE_RUNLOOP_REQALLOC
	#define trace_reqalloc(fmt, ...) dlog("[runloop] " fmt, ##__VA_ARGS__)
	#define TRACE_RUNLOOP_MEMBERS int nlivew; // number of allocated W
#else
	#define trace_reqalloc(fmt, ...) ((void)0)
	#define TRACE_RUNLOOP_MEMBERS
#endif

#if defined(TRACE_RUNLOOP) || defined(TRACE_RUNLOOP_REQALLOC)
	#define TRACE_LABEL_P ,const char* trace_label
	#define TRACE_LABEL_A(v) ,v
#else
	#define TRACE_LABEL_P
	#define TRACE_LABEL_A(v)
#endif


typedef enum : u8 {
	EVType_TIMER,
} EVType;

enum : u8 { EVType_DEAD = 0xff }; // signals "in callback"

typedef union {
	ev_watcher w;        // base
	ev_io      io;       // invoked when fd is either EV_READable or EV_WRITEable
	ev_timer   timer;    // invoked after a specific time
	ev_signal  signal;   // invoked when the given signal has been received
	ev_child   child;    // invoked when sigchld is received
	ev_idle    idle;     // invoked when the nothing else needs to be done
	ev_prepare prepare;  // invoked for each run of the mainloop, just before the blocking call
	ev_check   check;    // invoked for each run of the mainloop, just after the blocking call
	ev_fork    fork;     // invoked before check in the child process when a fork was detected
	// note: ev_stat is not in the union since it's very large
} W;

typedef struct WBlock {
	struct WBlock* nullable next;
	W                       w[/*W_CAP*/];
} WBlock;

struct Runloop {
	struct ev_loop* loop;
	TRACE_RUNLOOP_MEMBERS
	WBlock  wblock; // first block in list
};

enum { W_CAP = (4096 - sizeof(Runloop)) / sizeof(W) };


int RunloopCreate(Runloop** result) {
	usize allocsize = sizeof(Runloop) + W_CAP*sizeof(W);
	Runloop* rl = calloc(1, allocsize);
	if (!rl)
		return -ENOMEM;
	struct ev_loop* loop = ev_loop_new(EVFLAG_NOENV | EVBACKEND_ALL);
	if (loop == NULL) {
		free(rl);
		return -ENOMEM;
	}
	ev_set_userdata(loop, rl);
	rl->loop = loop;
	*result = rl;

	// dlog("W          %zu", sizeof(W));
	// dlog("ev_watcher %zu", sizeof(ev_watcher));
	// dlog("ev_io      %zu", sizeof(ev_io));
	// dlog("ev_timer   %zu", sizeof(ev_timer));
	// dlog("ev_signal  %zu", sizeof(ev_signal));
	// dlog("ev_child   %zu", sizeof(ev_child));
	// dlog("ev_idle    %zu", sizeof(ev_idle));
	// dlog("ev_prepare %zu", sizeof(ev_prepare));
	// dlog("ev_check   %zu", sizeof(ev_check));
	// dlog("ev_fork    %zu", sizeof(ev_fork));

	return 0;
}


static void wblock_free(WBlock* wb) {
	if (wb->next)
		wblock_free(wb->next);
	free(wb);
}


void RunloopFree(Runloop* rl) {
	ev_loop_destroy(rl->loop);
	if (rl->wblock.next)
		wblock_free(rl->wblock.next);
	free(rl);
}


int RunloopRun(Runloop* rl, u32 flags) {
	// ev_run:
	//
	// If the flags argument is specified as 0, it will keep handling events until
	// either no event watchers are active anymore or ev_break was called.
	//
	// The return value is false if there are no more active watchers
	// (which usually means "all jobs done" or "deadlock"), and true in all
	// other cases (which usually means "you should call ev_run again").
	//
	// A flags value of EVRUN_NOWAIT will look for new events, will handle those
	// events and any already outstanding ones, but will not wait and block
	// your process in case there are no events and will return after one
	// iteration of the loop. This is sometimes useful to poll and handle new
	// events while doing lengthy calculations, to keep the program responsive.
	//
	// A flags value of EVRUN_ONCE will look for new events (waiting if necessary) and
	// will handle those and any already outstanding ones. It will block your process
	// until at least one new event arrives (which could be an event internal to libev
	// itself, so there is no guarantee that a user-registered callback will be
	// called), and will return after one iteration of the loop.
	//
	return ev_run(rl->loop, EVRUN_ONCE); // >0=more, 0=empty
}


static ev_watcher* nullable w_of_reqid(Runloop* rl, int reqid) {
	WBlock* b = &rl->wblock;

	// common case: in main block
	if (reqid < W_CAP)
		return &b->w[reqid].w;

	// it's in some other block
	// dlog("w_of_reqid(reqid=%d) => wblock[%d][%d]", reqid, reqid / W_CAP, reqid % W_CAP);
	for (int bi = reqid / W_CAP; bi > 0; bi--) {
		b = b->next;
		if (b == NULL)
			return NULL;
	}
	return &b->w[reqid % W_CAP].w;
}


static bool w_moreblock(Runloop* rl, WBlock* tail_block) {
	usize allocsize = sizeof(WBlock) + W_CAP*sizeof(W);
	WBlock* wb = calloc(1, allocsize);
	if (!wb) {
		trace_reqalloc("failed to alloc wblock of %zu B", allocsize);
		return false;
	}
	trace_reqalloc("alloc wblock %p (%zu B)", wb, allocsize);
	tail_block->next = wb;
	return true;
}


static void* nullable w_alloc(Runloop* rl, EVType evtype, void* udcb, void* nullable ud) {
	// we're using blocks instead of a heap or slab since an ev_watcher cannot be relocated in
	// memory while active.
	WBlock* wb = &rl->wblock;
	for (int bi = 0;; bi++) {
		for (int i = 0; i < W_CAP; i++) {
			if (wb->w[i].w.udcb == NULL) {
				ev_watcher* w = &wb->w[i].w;
				w->udcb = udcb;
				w->ud = ud;
				w->reqid = i + bi*W_CAP;
				w->evtype = evtype;
				#ifdef TRACE_RUNLOOP_REQALLOC
					rl->nlivew++;
					trace_reqalloc("alloc reqid=%d w=%p wblock=%p (%d live)",
					               w->reqid, w, wb, rl->nlivew);
				#endif
				return w;
			}
		}
		// attempt to allocate another block if all blocks are full
		if (wb->next == NULL && !w_moreblock(rl, wb))
			return NULL;
		wb = wb->next;
	}
}


static int w_free(Runloop* rl, void* wp TRACE_LABEL_P) {
	ev_watcher* w = wp;
	#ifdef TRACE_RUNLOOP_REQALLOC
		rl->nlivew--;
		trace_reqalloc("free reqid=%d w=%p f=%s (%d live)",
		               w->reqid, w, trace_label, rl->nlivew);
	#endif
	assert(w->udcb != NULL);
	w->udcb = NULL;
	return 0;
}


int RunloopRemove(Runloop* rl, int reqid) {
	ev_watcher* w = w_of_reqid(rl, reqid);
	if (!w)
		return -EINVAL;
	if (w->udcb == NULL)
		return -ENOENT;
	switch (w->evtype) {
		case EVType_TIMER:
			ev_timer_stop(rl->loop, (ev_timer*)w);
			return w_free(rl, w TRACE_LABEL_A(__FUNCTION__));

		case EVType_DEAD:
			// We are being called from inside a callback which will free the watcher
			return -EALREADY;
	}
	dlog("invalid evtype 0x%04x in reqid=%d", w->evtype, reqid);
	unreachable();
}


static void timer_cb(struct ev_loop* loop, ev_timer* w, int revents) {
	// note: revents&EV_TIMER for timer
	trace("timer_cb reqid=%d revents=0x%08x active=%d pending=%d",
	      w->reqid, revents, w->active, w->pending);

	// mark as "will be freed" to potential RunloopRemove call from inside the callback
	if (w->active == 0 && w->pending == 0)
		w->evtype = EVType_DEAD;

	Runloop* rl = ev_userdata(loop);
	((TimerCallback)w->udcb)(rl, w->reqid, w->ud);

	// Free if watcher became inactive
	// Note: Check w->udcb in case callback called RunloopRemove
	if (w->evtype == EVType_DEAD)
		w_free(rl, w TRACE_LABEL_A(__FUNCTION__));
}


int RunloopAddTimeout(Runloop* rl, TimerCallback cb, void* nullable ud, DTime deadline) {
	ev_timer* w = w_alloc(rl, EVType_TIMER, cb, ud); if (!w) return -ENOMEM;
	const i64 estimated_minimum_overhead = 1000; //ns
	i64 ns = DTimeUntil(deadline) - estimated_minimum_overhead;
	if (ns < 0) ns = 0;
	ev_timer_init(w, timer_cb, (double)ns / 1000000000.0, 0.0);
	ev_timer_start(rl->loop, w);
	return w->reqid;
}


int RunloopAddInterval(Runloop* rl, TimerCallback cb, void* nullable ud, DTimeDuration interval) {
	if (interval < 0) return -EINVAL;
	ev_timer* w = w_alloc(rl, EVType_TIMER, cb, ud);  if (!w) return -ENOMEM;
	const double interval_sec = (double)interval / 1000000000.0;
	ev_timer_init(w, timer_cb, interval_sec, interval_sec);
	ev_timer_start(rl->loop, w);
	return w->reqid;
}
