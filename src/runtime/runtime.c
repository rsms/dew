#include "runtime.h"
#include "lutil.h"
#include "array.h"
#include "buf.h"
#include "time.h"
#include "intconv.h"
#include "intscan.h"
#include "intfmt.h"
#include "structclone.h"
#include "worker.h"

#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <pthread.h> // TODO: move into platform_SYS.c
#include <stdatomic.h> // TODO: move into platform_SYS.c


#define TRACE_SCHED
// #define TRACE_SCHED_RUNQ
#define TRACE_SCHED_WORKER

#ifdef TRACE_SCHED
	#define trace_sched(fmt, ...) \
		dlog("\e[1;34m%-15.15s S%-2u│\e[0m " fmt, __FUNCTION__, tls_s_id, ##__VA_ARGS__)
#else
	#define trace_sched(fmt, ...) ((void)0)
#endif

#ifdef TRACE_SCHED_RUNQ
	#define trace_runq(fmt, ...) \
		dlog("\e[1;35m%-15.15s S%-2u│\e[0m " fmt, __FUNCTION__, tls_s_id, ##__VA_ARGS__)
#else
	#define trace_runq(fmt, ...) ((void)0)
#endif

#ifdef TRACE_SCHED_WORKER
	#define trace_worker(fmt, ...) ( \
		tls_s_id && tls_w_id ? dlog("\e[1;32m%-15.15s W%-2u│\e[0m S%-2u " fmt, \
									__FUNCTION__, tls_w_id, tls_s_id, ##__VA_ARGS__) : \
		tls_w_id ?             dlog("\e[1;32m%-15.15s W%-2u│\e[0m " fmt, \
									__FUNCTION__, tls_w_id, ##__VA_ARGS__) : \
							   dlog("\e[1;32m%-15.15s S%-2u│\e[0m " fmt, \
									__FUNCTION__, tls_s_id, ##__VA_ARGS__) \
	)
#else
	#define trace_worker(fmt, ...) ((void)0)
#endif


// LUA_EXTRASPACE is defined in lua/src/luaconf.h
static_assert(sizeof(T) == LUA_EXTRASPACE, "");


static u8 g_reftabkey;              // table of objects with compex lifetime, to avoid GC
static u8 g_timerobj_luatabkey;     // Timer object prototype
static u8 g_uworker_uval_luatabkey; // Worker object prototype

// tls_s holds S for the current thread
static _Thread_local S* tls_s = NULL;

#if defined(TRACE_SCHED) || defined(TRACE_SCHED_RUNQ) || defined(TRACE_SCHED_WORKER)
	static _Atomic(u32)      tls_s_idgen = 1;
	static _Thread_local u32 tls_s_id = 0;
#endif

#if defined(TRACE_SCHED_WORKER)
	static _Atomic(u32)      tls_w_idgen = 1;
	static _Thread_local u32 tls_w_id = 0;
#endif


static int err_from_errno(int errno_val) {
	static const int tab[] = {
		#define _(NAME, ERRNO, ...) [ERRNO == -1 ? NAME+0xff : ERRNO] = NAME,
		FOREACH_ERR(_)
		#undef _
	};
	if (errno_val == 0) return 0;
	if (errno_val < (int)countof(tab)) {
		int err = tab[errno_val];
		if (err < 0xff)
			return err;
	}
	return ERR_ERROR;
}


// errstr takes an integer argument that is one of the ERR_ constants and returns a string.
// e.g. errstr(ERR_INVALID) == "INVALID"
static int l_errstr(lua_State* L) {
	const int err = luaL_checkinteger(L, 1);
	const char* name = "ERROR";
	const char* description = "unspecified error";
	switch (err) {
		#define _(NAME, ERRNO, DESC) \
			case NAME: \
				name = &#NAME[4]; \
				description = DESC; \
				break;
		FOREACH_ERR(_)
		#undef _
	}
	lua_pushstring(L, name);
	lua_pushstring(L, description);
	return 2;
}


// l_copy_values copies (by reference) n values from stack src to stack dst.
// This is similar to lua_xmove but copies instead of moves.
// Note: src & dst must belong to the same OS thread (not different Workers.)
static void l_copy_values(lua_State* src, lua_State* dst, int n) {
	assertf(lua_gettop(src) >= n, "src has at least n values at indices 1 .. n");
	// make copies of the arguments onto the top of src stack
	for (int i = 1; i <= n; i++)
		lua_pushvalue(src, i);
	// move them to dst
	lua_xmove(src, dst, n);
}


// ———————————————————————————————————————————————————————————————————————————————————————————
// BEGIN wasm ipc experiment

#ifdef __wasm__

extern lua_State* g_L; // dew.c

static int l_ipcrecv(lua_State* L) {
	// TODO: this could be generalized into a io_uring like API.
	// Could then use async reading of stdin instead of a dedicated "ipc" syscall.

	// TODO: arguments
	IPCMsg msg = {};
	u32 flags = 0;
	long result = syscall(SysOp_IPCRECV, (uintptr)&msg, (long)flags, 0, 0, 0);
	// TODO: return message
	lua_pushinteger(L, (lua_Integer)result);
	return 1;
}

static lua_State* g_ipcrecv_co = NULL;

extern int ipcrecv(int arg);

__attribute__((visibility("default"))) int ipcsend(long value) {
	// printf("ipcsend %ld\n", value);
	if (g_ipcrecv_co == NULL)
		return -EINVAL;
	lua_State* L_from = g_L;
	lua_State* L = g_ipcrecv_co;
	g_ipcrecv_co = NULL;
	lua_pushinteger(L, value);
	int status = lua_resume(L, L_from, 1, NULL);
	if (status == LUA_OK)
		return 0;
	fprintf(stderr, "Error resuming coroutine: %s\n", lua_tostring(L, -1));
	return -ECHILD;
}

static int l_ipcrecv_co(lua_State* L) {
	if (!lua_isyieldable(L))
		return luaL_error(L, "not called from a coroutine");
	int r = ipcrecv(123);
	// TODO: can return result immediately here is there is some
	g_ipcrecv_co = L;
	return lua_yield(L, 0);
}

static int l_iowait(lua_State* L) {
	long result = syscall(SysOp_IOWAIT, 0, 0, 0, 0, 0);
	lua_pushinteger(L, (lua_Integer)result);
	return 1;
}

#endif // __wasm__

// END wasm ipc experiment
// ———————————————————————————————————————————————————————————————————————————————————————————


static const char* l_status_str(int status) {
	switch (status) {
		case LUA_OK:        return "OK";
		case LUA_YIELD:     return "YIELD";
		case LUA_ERRRUN:    return "ERRRUN";
		case LUA_ERRSYNTAX: return "ERRSYNTAX";
		case LUA_ERRMEM:    return "ERRMEM";
		case LUA_ERRERR:    return "ERRERR";
	}
	return "?";
}


static int l_error_not_a_task(lua_State* L, T* t) {
	// note: this can happen in two scenarios:
	// 1. calling a task-specific API function from a non-task, or
	// 2. calling a task-specific API function from a to-be-closed variable (__close)
	//    during task shutdown.
	const char* msg = t->s ? "task is shutting down" : "not called from a task";
	dlog("%s: invalid call by user: %s", __FUNCTION__, msg);
	return luaL_error(L, msg);
}


static T* nullable l_check_task(lua_State* L, int idx) {
	lua_State* other_L = lua_tothread(L, idx);
	if LIKELY(other_L) {
		T* other_t = L_t(other_L);
		if LIKELY(other_t->s)
			return other_t;
	}
	luaL_typeerror(L, idx, "Task");
	return NULL;
}


#define REQUIRE_TASK(L) ({ \
	T* __t = L_t(L); \
	if UNLIKELY(__t->s == NULL) \
		return l_error_not_a_task(L, __t); \
	__t; \
})


typedef int(*TaskContinuation)(lua_State* L, int l_thrd_status, void* arg);


static const char* t_status_str(u8 status) {
	switch ((enum TStatus)status) {
		case T_READY:       return "T_READY";
		case T_RUNNING:     return "T_RUNNING";
		case T_WAIT_IO:     return "T_WAIT_IO";
		case T_WAIT_SEND:   return "T_WAIT_SEND";
		case T_WAIT_RECV:   return "T_WAIT_RECV";
		case T_WAIT_TASK:   return "T_WAIT_TASK";
		case T_WAIT_WORKER: return "T_WAIT_WORKER";
		case T_WAIT_ASYNC:  return "T_WAIT_ASYNC";
		case T_DEAD:        return "T_DEAD";
	}
	return "?";
}


S* s_get_thread_local() {
	return tls_s;
}


inline static int t_suspend(T* t, u8 tstatus, void* nullable arg, TaskContinuation nullable cont) {
	assertf(tstatus != T_READY || tstatus != T_RUNNING || tstatus != T_DEAD,
			"%s", t_status_str(tstatus));
	trace_sched(T_ID_F " %s", t_id(t), t_status_str(tstatus));
	t->status = tstatus;
	return lua_yieldk(t_L(t), 0, (intptr_t)arg, (int(*)(lua_State*,int,u64))cont);
}


// t_iopoll_wait suspends a task that is waiting for file descriptor events.
// Once 'd' is told that there are changes, t is woken up which causes the continuation 'cont'
// to be invoked on the unchanged stack in t_resume before execution is handed back to the task.
inline static int t_iopoll_wait(T* t, IODesc* d, int(*cont)(lua_State*,int,IODesc*)) {
	d->t = t;
	return t_suspend(t, T_WAIT_IO, d, (TaskContinuation)cont);
	// return lua_yieldk(L, 0, (intptr_t)d, (int(*)(lua_State*,int,lua_KContext))cont);
}


// t_cancel_timers finds and cancels all pending timers started by t.
static void t_cancel_timers(T* t) {
	trace_sched("canceling %u timers started by " T_ID_F, t->ntimers, t_id(t));
	// Linear search to find matching timers.
	//
	// Note: Tasks are not expected to exit with timers still running,
	// so it's okay that this is slow.
	//
	// TODO: this could be made much more efficient.
	// Currently we are removing a timer one by one, sifting through the heap
	// and resetting scanning everytime we find one.
	//
	TimerPQ* timers = &t->s->timers;
	u32 i;
	for (i = timers->len; i-- && t->ntimers > 0;) {
		if (timers->v[i].timer->arg == t) {
			Timer* timer = timers->v[i].timer;
			timers_remove_at(timers, i);
			timer->when = -1; // signals that the timer is dead
			timer_release(timer); // release internal reference
			t->ntimers--;
			i = timers->len; // reset loop index
		}
	}
	assert(t->ntimers == 0 || !"ntimers larger than matching timers");
}


static T* nullable s_timers_check(S* s) {
	if (s->timers.len == 0)
		return NULL;
	// const DTimeDuration leeway = 10 * D_TIME_MICROSECOND;
	// DTime now = DTimeNow() - leeway;
	DTime now = DTimeNow();
	while (s->timers.len > 0 && s->timers.v[0].when <= now) {
		Timer* timer = timers_remove_min(&s->timers);
		T* t = timer->f(timer, timer->arg);
		if (t == NULL) {
			now = DTimeNow();
		} else if (timer->period > 0) {
			// repeating timer
			//
			// Two different approaches to updating 'when':
			// 1. steady rythm, variable delay between wakeups,
			//    i.e. timer rings every when+period time.
			// 2. variable rythm, steady delay between wakeups,
			//    i.e. timer rings with period delay.
			// It's not clear to me which is better.
			// In tests, the two approaches yield identical results.
			// I also benchmarked with a go program (identical results across the board.)
			// Approach 1 seems the correct one from first principles and does not require a new
			// timestamp, so let's go with that one for now.
			timer->when += timer->period; // 1
			// timer->when = now + timer->period; // 2

			bool ok = timers_add(&s->timers, timer);
			assert(ok); // never need to grow memory
			return t;
		} else {
			// one-shot timer
			// Since timers are accessible in userland, they are reference counted.
			// We dereference the timer here rather than in timer->f since we need to access
			// the timer after f returns.
			timer->when = -1; // signals that timer is dead
			timer_release(timer);
			assert(t->ntimers > 0);
			t->ntimers--;
			return t;
		}
	}
	return NULL;
}


static int l_timerobj_gc(lua_State* L) {
	TimerUVal* obj = lua_touserdata(L, 1);
	// dlog("*** l_timerobj_gc %p ***", obj->timer);
	timer_release(obj->timer);
	return 0;
}


static T* nullable l_timer_done(Timer* timer, void* arg) {
	// dlog("*** l_timer_done %p ***", timer);
	T* t = arg;
	// deliver timer message to task's inbox
	const u32 maxcap = 0xffff;
	InboxMsg* msg = inbox_add(&t->inbox, maxcap);
	if LIKELY(msg != NULL) {
		memset(msg, 0, sizeof(*msg));
		msg->type = InboxMsgType_TIMER;
	} else {
		// Inbox is full (very full!) Drop timer with a warning.
		logwarn("T%u inbox is full; dropping timer message", t->tid);
	}
	return t; // wake t
}


static Timer* nullable s_timer_start(
	S*             s,
	DTime          when,
	DTimeDuration  period,
	DTimeDuration  leeway,
	void* nullable f_arg,
	TimerF         f)
{
	// create timer
	Timer* timer = calloc(1, sizeof(Timer));
	if UNLIKELY(!timer)
		return NULL;
	timer->when = when;
	timer->period = period;
	timer->leeway = leeway;
	timer->nrefs = 1;
	timer->arg = f_arg;
	timer->f = f;

	// schedule timer
	if UNLIKELY(!timers_add(&s->timers, timer)) {
		free(timer);
		return NULL;
	}

	return timer;
}


// fun timer_start(when Time, period, leeway TimeDuration) Timer
static int l_timer_start(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	DTime when = luaL_checkinteger(L, 1);
	DTimeDuration period = luaL_checkinteger(L, 2);
	DTimeDuration leeway = luaL_checkinteger(L, 3);

	// increment task's "live timers" counter
	if UNLIKELY(++t->ntimers == 0) {
		t->ntimers--;
		return luaL_error(L, "too many concurrent timers (%d)", t->ntimers);
	}

	// create & schedule a timer
	Timer* timer = s_timer_start(t->s, when, period, leeway, t, l_timer_done);
	if UNLIKELY(!timer) {
		t->ntimers--;
		return l_errno_error(L, ENOMEM);
	}

	// create ref
	TimerUVal* obj = uval_new(L, UValType_Timer, sizeof(TimerUVal), 0);
	if UNLIKELY(!obj) {
		timers_remove(&t->s->timers, timer);
		free(timer);
		t->ntimers--;
		return 0;
	}
	timer->nrefs++;
	obj->uval.type = UValType_Timer;
	obj->timer = timer;
	lua_rawgetp(L, LUA_REGISTRYINDEX, &g_timerobj_luatabkey);
	lua_setmetatable(L, -2);

	// return timer object
	return 1;
}


// fun timer_update(timer Timer, when Time, period, leeway TimeDuration)
static int l_timer_update(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	TimerUVal* obj = uval_check(L, 1, UValType_Timer, "Timer");
	DTime when = luaL_checkinteger(L, 2);
	DTimeDuration period = luaL_checkinteger(L, 3);
	DTimeDuration leeway = luaL_checkinteger(L, 4);

	Timer* timer = obj->timer;

	if UNLIKELY(timer->when != (DTime)-1) {
		// Timer is still active.
		// Remove it and re-add it to uphold correct position in timers priority queue.
		timers_remove(&t->s->timers, timer);
		timer->when = when;
		timer->period = period;
		timer->leeway = leeway;
		if UNLIKELY(!timers_add(&t->s->timers, timer)) {
			timer_release(timer); // release our internal reference
			assert(t->ntimers > 0);
			t->ntimers--;
			return l_errno_error(L, ENOMEM);
		}
	} else {
		// Timer already expired.
		// This is basically the same as starting a new timer with timer_start.
		timer->when = when;
		timer->period = period;
		timer->leeway = leeway;
		// First, increment task's "live timers" counter
		if UNLIKELY(++t->ntimers == 0) {
			t->ntimers--;
			return luaL_error(L, "too many concurrent timers (%d)", t->ntimers);
		}
		if UNLIKELY(!timers_add(&t->s->timers, timer))
			return l_errno_error(L, ENOMEM);
		assert(timer->nrefs == 1); // should have exactly on ref (Lua GC)
		timer->nrefs++;
	}
	return 0;
}


// fun timer_stop(timer Timer)
static int l_timer_stop(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	TimerUVal* obj = uval_check(L, 1, UValType_Timer, "Timer");

	if (!obj || obj->timer->when == (DTime)-1)
		return 0; // not a timer or already expired

	timers_remove(&t->s->timers, obj->timer);
	timer_release(obj->timer); // release our internal reference
	assert(t->ntimers > 0);
	t->ntimers--;
	return 0;
}


static T* l_sleep_done(Timer* timer, void* arg) {
	T* t = arg;
	return t; // wake t
}


// fun sleep(delay TimeDuration, leeway TimeDuration = -1)
// Sleep is a simplified case of timer_start.
// "sleep(123, 456)" is semantically equivalent to "timer_start(time()+123, 0, 456); recv()"
static int l_sleep(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	DTimeDuration delay = luaL_checkinteger(L, 1);
	if (delay < 0)
		return luaL_error(L, "negative timeout");
	DTime when = (DTimeNow() + delay) - D_TIME_MICROSECOND;
	int has_leeway;
	DTimeDuration leeway = lua_tointegerx(L, 2, &has_leeway);
	if (!has_leeway)
		leeway = -1;

	// trace
	if (leeway < 0) {
		trace_sched(T_ID_F " sleep %.3f ms", t_id(t), (double)delay / D_TIME_MILLISECOND);
	} else {
		trace_sched(T_ID_F " sleep %.3f ms (%.3f ms leeway)", t_id(t),
					(double)delay / D_TIME_MILLISECOND,
					(double)leeway / D_TIME_MILLISECOND);
	}

	// increment task's "live timers" counter
	if UNLIKELY(++t->ntimers == 0) {
		t->ntimers--;
		return luaL_error(L, "too many concurrent timers (%d)", t->ntimers);
	}

	// create & schedule a timer
	DTimeDuration period = 0;
	Timer* timer = s_timer_start(t->s, when, period, leeway, t, l_sleep_done);
	if UNLIKELY(!timer) {
		t->ntimers--;
		return l_errno_error(L, ENOMEM);
	}

	// return one result
	t->resume_nres = 1;

	return t_suspend(t, T_WAIT_IO, NULL, NULL);
	// return lua_yieldk(L, 0, 0, NULL);
}


static bool s_task_register(S* s, T* t) {
	T** tp = (T**)pool_entry_alloc(&s->tasks, &t->tid, sizeof(T*));
	if (tp != NULL)
		*tp = t;
	return tp != NULL;
}


static void s_task_unregister(S* s, T* t) {
	// optimization for main task (tid 1): skip work incurred by pool_entry_free
	if (t->tid != 1)
		pool_entry_free(s->tasks, t->tid);
}


inline static T* s_task(S* s, u32 tid) {
	T** tp = (T**)pool_entry(s->tasks, tid, sizeof(T*));
	return *tp;
}


static i32 s_runq_find(const S* s, const T* t);


static bool s_runq_put(S* s, T* t) {
	assertf(t->status != T_DEAD, T_ID_F, t_id(t));
	assertf(s_runq_find(s, t) == -1, T_ID_F " already in runq", t_id(t));
	t->status = T_READY;
	const u32 runq_cap_limit = U32_MAX;
	T** tp = (T**)fifo_push((FIFO**)&s->runq, sizeof(*s->runq->entries), runq_cap_limit);
	if (tp == NULL)
		return false;
	*tp = t;
	trace_runq("runq put " T_ID_F, t_id(t));
	return true;
}


static bool s_runq_put_runnext(S* s, T* t) {
	trace_runq("runq put runnext = " T_ID_F, t_id(t));
	assertf(t->status != T_DEAD, T_ID_F, t_id(t));
	assertf(s_runq_find(s, t) == -1, T_ID_F " already in runq", t_id(t));
	if UNLIKELY(s->runnext) {
		// kick out previous runnext to runq
		assertf(s->runnext != t, T_ID_F " already runnext", t_id(t));
		trace_runq("runq kick out runnext " T_ID_F " to runq", t_id(s->runnext));
		if UNLIKELY(!s_runq_put(s, s->runnext))
			return false;
	}
	t->status = T_READY;
	s->runnext = t;
	return true;
}


static T* nullable s_runq_get(S* s) {
	T* t;
	if (s->runnext) {
		t = s->runnext;
		s->runnext = NULL;
		trace_runq("runq get runnext = " T_ID_F, t_id(t));
	} else {
		T** tp = (T**)fifo_pop(&s->runq->fifo, sizeof(*s->runq->entries));
		if (tp == NULL) // empty
			return NULL;
		t = *tp;
		trace_runq("runq get " T_ID_F, t_id(t));
	}
	return t;
}


static void s_runq_remove(S* s, T* t) {
	if (s->runnext == t) {
		trace_runq("runq remove runnext " T_ID_F, t_id(t));
		s->runnext = NULL;
	} else if (s->runq->fifo.head != s->runq->fifo.tail) { // not empty
		RunQ* runq = s->runq;
		FIFO* fifo = &runq->fifo;
		u32 i = fifo->head;
		u32 count = (fifo->tail >= fifo->head) ?
					(fifo->tail - fifo->head) :
					(fifo->cap - fifo->head + fifo->tail);
		for (u32 j = 0; j < count; j++) {
			if (runq->entries[i] == t) {
				trace_runq("runq remove [%u] " T_ID_F, i, t_id(t));
				u32 next = (i + 1 == fifo->cap) ? 0 : i + 1;
				usize nbyte = (fifo->tail - next) * sizeof(*runq->entries);
				memmove(&runq->entries[i], &runq->entries[next], nbyte);
				fifo->tail = (fifo->tail == 0) ? fifo->cap - 1 : fifo->tail - 1;
				return;
			}
			i = (i + 1 == fifo->cap) ? 0 : i + 1;
		}
		trace_sched("warning: s_runq_remove(" T_ID_F ") called but task not found", t_id(t));
	}
}


static i32 s_runq_find(const S* s, const T* t) {
	if (s->runnext == t)
		return I32_MAX;

	if (s->runq->fifo.head != s->runq->fifo.tail) { // not empty
		RunQ* runq = s->runq;
		FIFO* fifo = &runq->fifo;
		u32 i = fifo->head;
		u32 count = (fifo->tail >= fifo->head) ?
					(fifo->tail - fifo->head) :
					(fifo->cap - fifo->head + fifo->tail);
		for (u32 j = 0; j < count; j++) {
			if (runq->entries[i] == t)
				return (i32)i;
			i = (i + 1 == fifo->cap) ? 0 : i + 1;
		}
	}

	return -1;
}


static void t_add_child(T* parent, T* child) {
	assertf(child->prev_sibling == 0, "should not be in a list");
	assertf(child->next_sibling == 0, "should not be in a list");
	// T uses a doubly-linked list of tids.
	// As an example, consider the following task tree:
	//    T1
	//      T2
	//      T3
	//      T4
	// where T1 is the parent, the list looks like this:
	//   T1 —> T4 <—> T3 <—> T2
	// i.e.
	//   T1  .first_child = 4
	//   T4  .prev_sibling = 0  .next_sibling = 3
	//   T3  .prev_sibling = 4  .next_sibling = 2
	//   T2  .prev_sibling = 3  .next_sibling = 0
	// so if we were to t_add_child(T1, T5) we'd get:
	//   T1 —> T5 <—> T4 <—> T3 <—> T2
	// i.e.
	//   T1  .first_child = 5
	//   T5  .prev_sibling = 0  .next_sibling = 4
	//   T4  .prev_sibling = 5  .next_sibling = 3
	//   T3  .prev_sibling = 4  .next_sibling = 2
	//   T2  .prev_sibling = 3  .next_sibling = 0
	//
	if (parent->first_child) {
		T* first_child = s_task(parent->s, parent->first_child);
		first_child->prev_sibling = child->tid;
	}
	child->next_sibling = parent->first_child;
	parent->first_child = child->tid;
}


static void t_remove_child(T* parent, T* t) {
	assertf(parent->tid == t->parent, "%u == %u", parent->tid, t->parent);
	t->parent = 0;
	trace_sched("remove child " T_ID_F " from " T_ID_F, t_id(t), t_id(parent));
	if (parent->first_child == t->tid) {
		// removing the most recently spawned task
		// dlog("HEAD " T_ID_F, t_id(t));
		parent->first_child = t->next_sibling;
		if (parent->first_child) {
			T* first_child = s_task(parent->s, parent->first_child);
			first_child->prev_sibling = 0;
			t->next_sibling = 0;
		}
	} else {
		assert(t->prev_sibling > 0);
		T* prev_sibling = s_task(parent->s, t->prev_sibling);
		prev_sibling->next_sibling = t->next_sibling;
		if (t->next_sibling) {
			// removing a task in the middle of the list
			// dlog("MIDDLE " T_ID_F, t_id(t));
			T* next_sibling = s_task(parent->s, t->next_sibling);
			next_sibling->prev_sibling = t->prev_sibling;
		} else {
			// removing a task at the end of the list
			// dlog("TAIL " T_ID_F, t_id(t));
		}
	}

	// verify that t is actually removed
	#if 0 && defined(DEBUG)
	if (parent->first_child != 0) {
		T* child = s_task(parent->s, parent->first_child);
		T* prev_child = NULL;
		for (;;) {
			assert(child != t);
			assert(prev_child != t);
			prev_child = child;
			if (child->next_sibling == 0)
				break;
			child = s_task(parent->s, child->next_sibling);
		}
	}
	#endif
}


static const char* l_fmt_error(lua_State* L) {
	const char* msg = lua_tostring(L, -1);
	if (msg == NULL) { /* is error object not a string? */
		if (luaL_callmeta(L, 1, "__tostring") &&  /* does it have a metamethod */
			lua_type(L, -1) == LUA_TSTRING)       /* that produces a string? */
		{
			/* that is the message */
		} else {
			msg = lua_pushfstring(L, "[%s]", luaL_typename(L, 1));
		}
	}
	luaL_traceback(L, L, msg, 1);  /* append a standard traceback */
	const char* msg2 = lua_tostring(L, -1);
	lua_pop(L, 1);
	return msg2 ? msg2 : msg;
}


static void t_report_error(T* t, const char* context_msg) {
	lua_State* L = t_L(t);
	const char* msg = l_fmt_error(L);
	fprintf(stderr, "%s: ["T_ID_F "]\n%s\n", context_msg, t_id(t), msg);
}


static char* t_report_error_buf(T* t) {
	lua_State* L = t_L(t);
	const char* msg = l_fmt_error(L);
	return strdup(msg);
}


static void dlog_task_tree(const T* t, int level) {
	for (u32 child_tid = t->first_child; child_tid;) {
		T* child = s_task(t->s, child_tid);
		dlog("%*s" T_ID_F, level*4, "", t_id(child));
		dlog_task_tree(child, level + 1);
		child_tid = child->next_sibling;
	}
}


static void t_finalize(T* t, enum TDied died_how, u8 prev_tstatus);


static void t_stop(T* nullable parent, T* child) {
	if (parent) {
		trace_sched("stop " T_ID_F " by parent " T_ID_F, t_id(child), t_id(parent));
	} else {
		trace_sched("stop " T_ID_F, t_id(child));
	}

	// set status to DEAD _before_ calling lua_closethread to ensure that any to-be-closed
	// variables with __close metamethods that call into our API are properly handled.
	bool is_on_runq = child->status == T_READY;
	assert(child->status != T_DEAD);
	u8 prev_status = child->status;
	child->status = T_DEAD;

	// Shut down Lua "thread"
	//
	// Note that a __close metatable entry can be used to clean up things like open files.
	// For example:
	//     local _ <close> = setmetatable({}, { __close = function()
	//         print("cleanup here")
	//     end })
	//
	// Note: status will be OK in the common case and ERRRUN if an error occurred inside a
	// to-be-closed variable with __close metamethods.
	//
	lua_State* parent_L;
	if (parent) {
		parent_L = t_L(parent);
	} else {
		parent_L = child->s->L;
	}
	int status = lua_closethread(t_L(child), parent_L);
	if (status != LUA_OK) {
		dlog("warning: lua_closethread => %s", l_status_str(status));
		t_report_error(child, "Error in defer handler");
	}

	// remove from runq
	if (is_on_runq && !child->s->isclosed)
		s_runq_remove(child->s, child);

	// // finalize as "runtime error"
	// lua_pushstring(t_L(child), "parent task exited");
	// return t_finalize(child, LUA_ERRRUN);

	// finalize as "clean exit"
	child->resume_nres = 0;
	return t_finalize(child, TDied_STOP, prev_status);
}


static void t_stop_r(T* parent, T* child) {
	if (child->next_sibling)
		t_stop_r(parent, s_task(parent->s, child->next_sibling));
	if (child->first_child)
		t_stop_r(child, s_task(parent->s, child->first_child));
	return t_stop(parent, child);
}


// t_gc is called by Lua's luaE_freethread when a task is GC'd, thus it must not be 'static'
__attribute__((visibility("hidden")))
void t_gc(lua_State* L, T* t) {
	trace_sched(T_ID_F " GC", t_id(t));
	// remove from S's 'tasks' registry, effectively invalidating tid
	s_task_unregister(t->s, t);
}


static void t_release_to_gc(T* t) {
	trace_sched(T_ID_F " release to GC", t_id(t));
	assert(t->status == T_DEAD);
	// remove GC ref to allow task (Lua thread) to be garbage collected
	lua_State* TL = t_L(t);
	lua_State* SL = t->s->L;
	lua_rawgetp(SL, LUA_REGISTRYINDEX, &g_reftabkey); // put ref table on stack
	lua_pushthread(TL);   // Push the thread onto its own stack
	lua_xmove(TL, SL, 1); // Move the thread object to the `SL` state
	lua_pushnil(SL);      // Use `nil` to remove the key
	lua_rawset(SL, -3);   // reftab[thread] = nil in `SL`
	lua_pop(SL, 1);       // Remove the thread table from the stack
}


inline static void t_release(T* t) {
	if (--t->nrefs == 0)
		t_release_to_gc(t);
}


inline static void t_retain(T* t) {
	t->nrefs++;
}


static void t_remove_from_waiters(T* t) {
	S* s = t->s;

	u32 next_waiter_tid = t->info.wait_task.next_tid;
	u32 wait_tid = t->info.wait_task.wait_tid;
	assertf(!pool_entry_isfree(s->tasks, wait_tid), "%u", wait_tid);
	T* other_t = s_task(s, wait_tid);

	// common case is head of list
	if (other_t->waiters == t->tid) {
		other_t->waiters = next_waiter_tid;
		return;
	}

	// find in middle or end of list
	u32 next_tid = other_t->waiters;
	while (next_tid > 0) {
		T* waiter_t = s_task(s, next_tid);
		assert(waiter_t->status == T_WAIT_TASK);
		assert(waiter_t->info.wait_task.wait_tid == wait_tid);
		next_tid = waiter_t->info.wait_task.next_tid;
		if (next_tid == t->tid) {
			waiter_t->info.wait_task.next_tid = next_waiter_tid;
			return;
		}
	}

	dlog("warning: %s " T_ID_F ": not found", __FUNCTION__, t_id(t));
	dlog("TODO: check in other_t->inbox->waiters");
}


static void worker_wake_waiters(Worker* w) {
	S* s = w->s;
	for (u32 tid = w->waiters; tid > 0;) {
		T* waiting_t = s_task(s, tid);
		trace_sched("wake " T_ID_F " waiting on Worker %p", t_id(waiting_t), w);

		// we expect task to be waiting for a user worker or work performed by an async worker
		assert(waiting_t->status == T_WAIT_WORKER || waiting_t->status == T_WAIT_ASYNC);

		// put waiting task on (priority) runq
		bool ok;
		if (tid == w->waiters) {
			ok = s_runq_put_runnext(s, waiting_t);
		} else {
			ok = s_runq_put(s, waiting_t);
		}
		if UNLIKELY(!ok)
			panic_oom();

		tid = waiting_t->info.wait_worker.next_tid;
	}
	w->waiters = 0;
}


static void t_wake_waiters(T* t, u32 waiters) {
	S* s = t->s;
	for (u32 tid = waiters; tid > 0;) {
		T* waiting_t = s_task(s, tid);

		assert(waiting_t->status == T_WAIT_TASK ||
			   waiting_t->status == T_WAIT_SEND);

		trace_sched("wake " T_ID_F " waiting on " T_ID_F, t_id(waiting_t), t_id(t));

		// put waiting task on (priority) runq
		bool ok;
		if (tid == waiters) {
			ok = s_runq_put_runnext(s, waiting_t);
		} else {
			ok = s_runq_put(s, waiting_t);
		}
		if UNLIKELY(!ok) {
			dlog("failed to allocate memory");
			t_stop(t, waiting_t);
		}

		tid = waiting_t->info.wait_task.next_tid;
	}
}


static UWorker* s_worker(S* s) {
	assert(s->isworker);
	return (UWorker*)((u8*)s - offsetof(UWorker, s));
}


UNUSED static const char* TDied_str(enum TDied died_how) {
	switch (died_how) {
	case TDied_ERR:   return "ERR";
	case TDied_CLEAN: return "CLEAN";
	case TDied_STOP:  return "STOP";
	}
	return "?";
};


static void t_finalize(T* t, enum TDied died_how, u8 prev_tstatus) {
	S* s = t->s;

	// Looking for a bug that trips assert(s->nlive > 0) further down.
	// Seems to happens when a child task is begin stopped
	assert(prev_tstatus != T_DEAD);

	// Note: t_stop updates t->status before calling t_finalize. For that reason we can't trust
	// the current value of t->status in here but must use prev_tstatus.
	trace_sched(T_ID_F " exited (died_how=%s)", t_id(t), TDied_str(died_how));
	if UNLIKELY(died_how == TDied_ERR) {
		// if this is the main task, set s->exiterr
		if (t->tid == 1)
			s->exiterr = true;

		// one error as the final result value
		t->resume_nres = 1;

		// unless there are tasks waiting for this task, report the error as unhandled
		if (t->waiters == 0) {
			if (s->isworker && t->tid == 1) {
				// main task of a worker; only report error if no task is waiting for worker
				UWorker* w = s_worker(s);
				if (atomic_load_explicit(&w->w.waiters, memory_order_acquire)) {
					if (!w->errdesc_invalid)
						free(w->errdesc); // just in case...
					w->errdesc_invalid = 0;
					w->errdesc = t_report_error_buf(t);
					#if DEBUG
					t_report_error(t, "[DEBUG] Uncaught error in worker");
					#endif
				} else {
					t_report_error(t, "Uncaught error in worker");
				}
			} else {
				t_report_error(t, "Uncaught error");
			}
		} else {
			#ifdef DEBUG
			char* s = t_report_error_buf(t);
			dlog("warning: %s\n(Error not reported because a task is awaiting this task)", s);
			free(s);
			#endif
		}
	}
	t->status = T_DEAD;

	// decrement "live tasks" counter
	if (s->nlive == 0) {
		dlog("error: s->nlive==0 while task " T_ID_F " is still alive in " S_ID_F,
			 t_id(t), s_id(t->s));
		assert(s->nlive > 0);
	}
	s->nlive--;

	// dlog("task tree for " T_ID_F ":", t_id(t)); dlog_task_tree(t, 1);

	// cancel associated waiting state
	if (prev_tstatus == T_WAIT_TASK) {
		// Task was stopped while waiting for another task.
		// Remove task from list of waiters of target task.
		t_remove_from_waiters(t);
	}

	// stop child tasks
	if (t->first_child)
		t_stop_r(t, s_task(s, t->first_child));

	// wake any tasks waiting for this task to exit
	if (t->waiters) {
		t_wake_waiters(t, t->waiters);
		t->waiters = 0;
	}

	// if S is supposed to exit() when done, do that now if this is the last task
	if (s->doexit && s->nlive == 0 && s->workers == NULL) {
		trace_sched("exit(%d)", (int)s->exiterr);
		return exit(s->exiterr);
	}

	// stop any still-running timers (optimization: skip when S is shutting down)
	if UNLIKELY(t->ntimers && !s->isclosed)
		t_cancel_timers(t);

	if (t->parent) {
		// remove task from parent's list of children
		T* parent = s_task(s, t->parent);
		t_remove_child(parent, t);
	} else {
		assertf(t->waiters == 0, "cannot wait for the main task");
	}

	// set info.dead.how
	t->info.dead.how = died_how;

	// release S's "live" reference to the task
	t_release(t);
}


static void t_resume(T* t) {
	// get Lua "thread" for task
	lua_State* L = t_L(t);
	assert(t->s != NULL);
	assert(t->s->L != L);

	// switch from l_main to task
	// nargs: number of values on T's stack to be returned from 'yield' inside task.
	// nres:  number of values passed to 'yield' by task.
	int nargs = t->resume_nres, nres;
	t->resume_nres = 0;
	t->status = T_RUNNING;

	trace_sched("resume " T_ID_F " nargs=%d", t_id(t), nargs);
	// dlog_lua_stackf(L, "stack: (nargs=%d)", nargs);
	int status = lua_resume(L, t->s->L, nargs, &nres);

	// check if task exited
	if UNLIKELY(status != LUA_YIELD) {
		t->resume_nres = nres > 0xff ? 0xff : nres;
		u8 died_how = (status == LUA_OK) ? TDied_CLEAN : TDied_ERR;
		return t_finalize(t, died_how, t->status);
	}

	// discard results
	return lua_pop(L, nres);
}


// t_yield suspends t and puts it on the run queue.
// Switches control back to scheduler loop.
// Note: Must only be called from a task, never l_main.
static int t_yield(T* t, int nresults) {
	trace_sched(T_ID_F " yield", t_id(t));
	if UNLIKELY(!s_runq_put(t->s, t))
		return luaL_error(t_L(t), "out of memory");
	return lua_yieldk(t_L(t), nresults, 0, NULL);
}


// s_spawn_task creates a new task, which function should be on stack L (the spawner thread),
// and adds it to the runq as runnext.
// L should be the Lua thread that initiated the spawn (S's L or a task's L.)
// Returns 1 on success with Lua "thread" on L stack, or 0 on failure with Lua error set in L.
static int s_spawn_task(S* s, lua_State* L, T* nullable parent) {
	// create Lua "thread".
	// Note: See coroutine.create in luaB_cocreate (lcorolib.c).

	// make sure first argument is a function
	if UNLIKELY(lua_type(L, 1) != LUA_TFUNCTION)
		return luaL_typeerror(L, 1, lua_typename(L, LUA_TFUNCTION));

	// rest of arguments will be passed along to the task's main function when started
	int nargs = lua_gettop(L) - 1;

	// create "thread" & push it on stack
	lua_State* NL = lua_newthread(L);

	// Move value on top of stack (the "thread" object) to the bottom of the stack.
	// I.e. [function, arg1, arg2, argN, thread] -> [thread, function, arg1, arg2, argN]
	lua_rotate(L, 1, 1);

	// Move new task's function and arguments from caller task's stack to new task's stack.
	// What remains on L's stack is the "thread".
	lua_xmove(L, NL, 1 + nargs);

	// Hold on to a GC reference to thread, e.g. "S.L.reftab[thread] = true"
	// Note: this is quite complex of a Lua stack operation. In particular lua_pushthread is
	// gnarly as it pushes the thread onto its own stack, which we then move over to S's stack.
	// If we get things wrong we (at best) get a memory bus error in asan with no stack trace,
	// making this tricky to debug.
	lua_rawgetp(s->L, LUA_REGISTRYINDEX, &g_reftabkey); // push table on S's stack
	lua_pushvalue(L, 1);      // Copy thread
	lua_xmove(L, s->L, 1);    // Move thread to S's stack
	lua_pushboolean(s->L, 1); // "true"
	lua_rawset(s->L, -3);     // reftab[thread] = true
	lua_pop(s->L, 1);         // Remove table from stack

	// initialize T struct (which lives in the LUA_EXTRASPACE header of lua_State)
	T* t = L_t(NL);
	memset(t, 0, sizeof(*t));
	t->s = s;
	if (parent)
		t->parent = parent->tid;
	t->nrefs = 1; // S's "live" reference
	t->resume_nres = nargs;

	// allocate id and store in 'tasks' set
	if UNLIKELY(!s_task_register(s, t)) {
		lua_closethread(NL, L);
		return l_errno_error(L, ENOMEM);
	}
	trace_sched("register task " T_ID_F, t_id(t));

	// assert that main task is assigned tid 1
	if (!parent)
		assertf(t->tid == 1, "main task was not assigned tid 1");

	// setup t to be run next by schedule
	if UNLIKELY(!s_runq_put_runnext(s, t)) {
		s_task_unregister(s, t);
		lua_closethread(NL, L);
		return l_errno_error(L, ENOMEM);
	}

	s->nlive++;

	// add t as a child of parent
	if (parent)
		t_add_child(parent, t);

	if (parent) {
		trace_sched(T_ID_F " spawns " T_ID_F, t_id(parent), t_id(t));
	} else {
		trace_sched("spawn main task " T_ID_F, t_id(t));
	}

	return 1;
}


// s_iopoll_wake is called by iopoll_poll when waiting tasks should be woken up
int s_iopoll_wake(S* s, IODesc** dv, u32 count) {
	int err = 0;
	for (u32 i = 0; i < count; i++) {
		IODesc* d = dv[i];

		T* t = d->t;

		// skip duplicates
		if (t == NULL)
			continue;

		// "take" t from d, to make sure that we don't attempt to wake a task from an event
		// that occurs when the task is running.
		d->t = NULL;

		trace_sched(T_ID_F " woken by iopoll" , t_id(t));

		// TODO: check if t is already on runq and don't add it if so.
		// For now, use an assertion (note: this is likely to happen)
		assert(t->status == T_WAIT_IO);
		assert(s_runq_find(s, t) == -1);

		if (!s_runq_put(s, t))
			err = -ENOMEM;
	}
	return err;
}


static void worker_retain(Worker* w);
static void worker_release(Worker* w);
static bool worker_close(Worker* w);


static void s_workers_add(S* s, Worker* w) {
	assert(w->next == NULL);
	w->next = s->workers;
	s->workers = w;
	worker_retain(w);
}


static void s_reap_workers(S* s) {
	Worker* w = s->workers;
	Worker* prev_w = NULL;
	while (w) {
		if (atomic_load_explicit(&w->status, memory_order_acquire) == Worker_CLOSED) {
			trace_worker("worker %p exited", w);

			// wake any tasks waiting for this worker
			if (w->waiters)
				worker_wake_waiters(w);

			// remove from list of live workers
			Worker* next = w->next;
			if (prev_w) {
				prev_w->next = next;
			} else {
				s->workers = next;
			}
			w->next = NULL;
			worker_release(w);
			w = next;
		} else {
			prev_w = w;
			w = w->next;
		}
	}
}


static void t_worker_msg_stow(T* t, InboxMsg* msg) {
	lua_State* dst_L = t_L(t);

	// Store message payload in a table.
	// This way the message payload gets GC'd when the destination task is GC'd,
	// regardless of message delivery.
	lua_createtable(dst_L, msg->nres, 0);

	// first value is the sender task (Lua "thread")
	lua_pushnil(dst_L);         // TODO: push sender worker onto its own stack
	lua_rawseti(dst_L, -2, 1);  // Store in table at index 1

	for (int i = 2; i <= msg->nres; i++) {
		lua_pushinteger(dst_L, 123); // TODO
		lua_rawseti(dst_L, -2, i);   // Store in table at index (i-1)
	}

	msg->msg.ref = luaL_ref(dst_L, LUA_REGISTRYINDEX);
}


static void s_recv_worker_msg(S* s, AsyncWorkRes* res) {
	// Deliver message from worker to this S's main task.
	// Inbox is practically unbounded in this case. The only other option is to drop the message.
	const u32 maxcap = U32_MAX;

	T* t = (s->nlive > 0) ? s_task(s, 1) : NULL; // main task
	if (!t || t->status == T_DEAD) {
		logwarn("ignoring message from worker received during shutdown");
		goto end;
	}

	InboxMsg* msg = inbox_add(&t->inbox, maxcap);
	if UNLIKELY(msg == NULL) {
		logwarn("T%u inbox is full; dropping message", t->tid);
		goto end;
	}

	memset(msg, 0, sizeof(*msg));
	msg->type = InboxMsgType_MSG;
	if (t->status == T_WAIT_SEND)
		trace_sched("wake " T_ID_F " waiting on send", t_id(t));

	// TODO: see l_msg_stow, how it stores message values for the receiver

	// if the destination task is not currently waiting in a call to recv(),
	// the message will be delivered later.
	if (t->status != T_WAIT_RECV) {
		trace_sched("deliver buffered msg to " T_ID_F, t_id(t));
		dlog("TODO: stow message");
		t_worker_msg_stow(t, msg);
	} else {
		trace_sched("wake " T_ID_F " waiting on recv", t_id(t));
		s_runq_put_runnext(s, t);
	}

end: {}
	//dlog("TODO: dispose of structured-clone msg");
}


static void s_asyncwork_read_cq(S* s) {
	// There's a natural race condition with asyncwork completions where delivering
	// S_NOTE_ASYNCWORK to wake up a runloop races with delivery of AsyncWorkRes on the
	// completion queue.
	//
	// N completions may lead <=N S_NOTE_ASYNCWORK deliveries because notes are delivered not in
	// a queue but as a signal to wake up S's runloop. I.e. many logical notes may result in just
	// one runloop wakeup; one call to s_check_notes.
	//
	// Because of this, we sometimes process many completions in this loop and sometimes no
	// completions at all (when we processed them in a previous loop.) Since NOTEs are always
	// causally delivered after writing to the CQ, completions can never "appear" _after_ a
	// NOTE, only _before_ a NOTE (i.e. when we process more than one completion in one note.)
	AsyncWorkRes res;
	for (;;) {
		if (!chan_read(s->asyncwork_cq, CHAN_TRY, &res))
			break;

		if (res.op == AsyncWorkOp_WORKER_MSG) {
			s_recv_worker_msg(s, &res);
			continue;
		}

		T* t = s_task(s, res.tid);
		trace_sched("wake " T_ID_F " waiting on asyncwork", t_id(t));
		if (res.flags & AsyncWorkFlag_HAS_CONT) {
			t->info.wait_async.result = res.result;
		} else {
			lua_pushinteger(t_L(t), res.result);
			t->resume_nres = 1;
		}
		if UNLIKELY(!s_runq_put(s, t))
			panic_oom();
		t_release(t);
	}
}


static void s_check_notes(S* s, u8 notes) {
	if (notes & S_NOTE_WEXIT)
		s_reap_workers(s);

	if (notes & S_NOTE_ASYNCWORK)
		s_asyncwork_read_cq(s);

	// Clear notes bits.
	// We are racing with worker threads here, so use a CAS but don't loop to retry since
	// s_check_notes is called by s_find_runnable, which will call s_check_notes again if a
	// worker raced us and added more events.
	atomic_compare_exchange_strong_explicit(
		&s->notes, &notes, 0, memory_order_acq_rel, memory_order_relaxed);
}


static void s_shutdown(S* s) {
	// note: this function is only called by other threads, never from "inside" S
	if (atomic_exchange_explicit(&s->isclosed, true, memory_order_acq_rel) == false) {
		trace_sched("interrupting iopoll");
		iopoll_interrupt(&s->iopoll);
	}
}


static void s_free(S* s) {
	iopoll_dispose(&s->iopoll);
	pool_free_pool(s->tasks);
	free(s->runq);
	array_free((struct Array*)&s->timers);
}


static int s_finalize(S* s) {
	trace_sched("finalize " S_ID_F, s_id(s));
	atomic_store_explicit(&s->isclosed, true, memory_order_release);

	// stop main task
	if (s->nlive > 0) {
		assertf(!pool_entry_isfree(s->tasks, 1),
				"main task is DEAD but other tasks are still alive");
		t_stop(NULL, s_task(s, 1));
	}

	// clear timers before GC to avoid costly (and useless) timers_remove
	s->timers.len = 0;

	// run GC to ensure pending IODesc close before S goes away
	lua_gc(s->L, LUA_GCCOLLECT);

	// stop & wait for workers
	if UNLIKELY(s->workers) {
		trace_worker("shutting down workers");

		// shutdown asyncwork channels as worker may be blocked on sq
		if (s->asyncwork_sq != NULL)
			chan_shutdown(s->asyncwork_sq);
		if (s->asyncwork_cq != NULL)
			chan_shutdown(s->asyncwork_cq);

		// close all workers
		for (Worker* w = s->workers; w; w = w->next)
			worker_close(w);

		for (Worker* w = s->workers; w;) {
			int err = pthread_join(w->thread, NULL);
			if (err)
				logwarn("failed to wait for worker thread=%p: %s", w->thread, strerror(err));
			Worker* next_w = w->next;
			worker_release(w);
			w = next_w;
		}

		// if S is supposed to exit() when done, do that now
		if (s->doexit) {
			trace_sched("exit(%d)", (int)s->exiterr);
			exit(s->exiterr);
		}

		if (s->asyncwork_sq)
			chan_close(s->asyncwork_sq);
		if (s->asyncwork_cq)
			chan_close(s->asyncwork_cq);
	}

	trace_sched("finalized " S_ID_F, s_id(s));

	// clear TLS entry to catch bugs
	tls_s = NULL;
	#if defined(TRACE_SCHED) || defined(TRACE_SCHED_RUNQ) || defined(TRACE_SCHED_WORKER)
		tls_s_id = 0;
	#endif

	s_free(s);
	return 0;
}


static int s_find_runnable(S* s, T** tp) {
	// check for expired timers
	if (( *tp = s_timers_check(s) )) {
		trace_sched(T_ID_F " taken from timers", t_id(*tp));
		return 1;
	}

	// check for worker events
	u8 notes = atomic_load_explicit(&s->notes, memory_order_acquire);
	if (notes != 0)
		s_check_notes(s, notes);

	// try to grab a task from the run queue
	if (( *tp = s_runq_get(s) )) {
		trace_sched(T_ID_F " taken from runq", t_id(*tp));
		return 1;
	}

	// check if we ran out tasks (all have exited) or if S is closing down
	if (s->nlive == 0 || atomic_load_explicit(&s->isclosed, memory_order_acquire)) {
		if (atomic_load_explicit(&s->isclosed, memory_order_acquire)) {
			trace_sched("scheduler shutting down; exiting scheduler loop");
		} else {
			trace_sched("no more tasks; exiting scheduler loop");
		}
		return 0;
	}

	// There are no tasks which are ready to run.
	// Poll for I/O events (with timeout if there are any active timers.)

	// determine iopoll deadline
	DTime deadline = (DTime)-1;
	DTimeDuration deadline_leeway = 0;
	if (s->timers.len > 0) {
		deadline = s->timers.v[0].timer->when;
		deadline_leeway = s->timers.v[0].timer->leeway;
	}

	// trace
	#ifdef TRACE_SCHED
		if (deadline == (DTime)-1) {
			trace_sched("iopoll (no timeout)");
		} else {
			char buf[30];
			trace_sched("iopoll with %s timeout", DTimeDurationFormat(DTimeUntil(deadline), buf));
		}
	#endif

	// wait for events
	int n = iopoll_poll(&s->iopoll, deadline, deadline_leeway);
	if UNLIKELY(n < 0) {
		if (s->isclosed) // ignore i/o errors that occur during shutdown
			return 0;
		logerr("internal I/O error: %s", strerror(-n));
		return l_errno_error(s->L, -n);
	}

	// check timers & runq again
	return_tail s_find_runnable(s, tp);
}


bool runtime_handle_signal(int signo) {
	S* s = tls_s;
	if (s && s->doexit) {
		trace_sched("got signal %d; shutting down " S_ID_F, signo, s_id(s));
		atomic_store_explicit(&s->isclosed, true, memory_order_release);
		return true;
	}
	return false;
}


__attribute__((always_inline))
static int s_main(S* s) {
	s->timers.v = s->timers_storage;
	s->timers.cap = countof(s->timers_storage);

	lua_State* L = s->L;
	if UNLIKELY(tls_s != NULL)
		return luaL_error(L, "S already active");
	tls_s = s;
	#if defined(TRACE_SCHED) || defined(TRACE_SCHED_RUNQ) || defined(TRACE_SCHED_WORKER)
		tls_s_id = atomic_fetch_add(&tls_s_idgen, 1);
	#endif

	// allocate runq with inital space for (8 - 1) entries
	if (!( s->runq = (RunQ*)fifo_alloc(8, sizeof(*s->runq)) ))
		return l_errno_error(L, ENOMEM);

	// allocate task pool with inital space for 8 entries
	if UNLIKELY(!pool_init(&s->tasks, 8, sizeof(T*)))
		return l_errno_error(L, ENOMEM);

	// initialize platform I/O facility
	int err = iopoll_init(&s->iopoll, s);
	if (err) {
		dlog("error: iopoll_init: %s", strerror(-err));
		s_free(s);
		return l_errno_error(L, -err);
	}

	// create refs table (for GC management)
	lua_createtable(L, 0, /*estimated common-case lowball count*/8);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &g_reftabkey);

	// create main task
	int nres = s_spawn_task(s, L, NULL);
	if UNLIKELY(nres == 0) { // error
		s_free(s);
		return 0;
	}

	// discard thread from L's stack
	lua_pop(L, 1);

	// scheduler loop: finds a runnable task and executes it.
	// Stops when all tasks have finished (or an error occurred.)
	T* t;
	while (!s->isclosed && s_find_runnable(s, &t))
		t_resume(t);
	return s_finalize(s);
}


// fun main(main fun(), exit_when_done bool = true)
static int l_main(lua_State* L) {
	// create scheduler for this OS thread & Lua context
	S* s = &(S){
		.L = L,
		.doexit = lua_isboolean(L, 2) ? lua_toboolean(L, 2) : true,
	};
	return s_main(s);
}


int luaopen_runtime(lua_State* L);


static void worker_free(Worker* w) {
	trace_worker("free Worker %p", w);
	if (w->wkind == WorkerKind_USER && !((UWorker*)w)->errdesc_invalid) {
		trace_worker("free UWorker.errdesc %p", ((UWorker*)w)->errdesc);
		free(((UWorker*)w)->errdesc);
		((UWorker*)w)->errdesc = NULL;
	}
	free(w);
}


static void worker_retain(Worker* w) {
	UNUSED u8 nrefs = atomic_fetch_add_explicit(&w->nrefs, 1, memory_order_acq_rel);
	assert(nrefs < 0xff || !"overflow");
}


static void worker_release(Worker* w) {
	if (atomic_fetch_sub_explicit(&w->nrefs, 1, memory_order_acq_rel) == 1)
		worker_free(w);
}


static bool worker_add_waiter(Worker* w, T* waiter_task) {
	// Add waiter_task to list of tasks waiting for w.
	// Note that this list is only ever modified by this current thread,
	// but is read by the worker's thread.
	waiter_task->info.wait_worker.next_tid = w->waiters;
	atomic_store_explicit(&w->waiters, waiter_task->tid, memory_order_release);

	// check if worker exited already
	return atomic_load_explicit(&w->status, memory_order_acquire) != Worker_CLOSED;
}


static int l_uworker_uval_gc(lua_State* L) {
	UWorkerUVal* uv = lua_touserdata(L, 1);
	trace_worker("GC");
	worker_close((Worker*)uv->uw);
	worker_release((Worker*)uv->uw);
	return 0;
}


// worker_cas_status attempts to set w->status to next_status.
// Returns false if another thread set CLOSED "already" (it's a race.)
static bool worker_cas_status(Worker* w, u8 next_status) {
	u8 status = atomic_load_explicit(&w->status, memory_order_acquire);
	for (;;) {
		if (status == Worker_CLOSED)
			return false;
		if (atomic_compare_exchange_weak_explicit(
				&w->status, &status, next_status, memory_order_acq_rel, memory_order_relaxed))
		{
			return true;
		}
	}
}


static void s_notify(S* s, u8 addl_notes) {
	// Called from another OS thread than S is running on
	u8 notes = atomic_load_explicit(&s->notes, memory_order_acquire);
	for (;;) {
		u8 newnotes = notes | addl_notes;
		if (atomic_compare_exchange_weak_explicit(
				&s->notes, &notes, newnotes, memory_order_acq_rel, memory_order_relaxed))
		{
			break;
		}
	}
	iopoll_interrupt(&s->iopoll);
}


static void asyncworker_complete(AWorker* aw, i64 result) {
	S* s = aw->w.s;

	ChanTx tx = chan_write_begin(s->asyncwork_cq, 0);
	assertf(tx.entry != NULL, "chan_write(cq) failed");
	AsyncWorkRes* res = tx.entry;
	res->op = aw->req.op;
	res->flags = aw->req.flags;
	res->tid = aw->req.tid;
	res->result = result;
	chan_write_commit(s->asyncwork_cq, tx);

	trace_sched("T%u TCQ write_commit result=%ld", res->tid, result);
	aw->req_is_active = 0;
}


static void worker_thread_exit(Worker** wp) {
	Worker* w = *wp;
	trace_worker("exit");

	// set status to CLOSED (likely already set via worker_cas_status)
	atomic_store_explicit(&w->status, Worker_CLOSED, memory_order_release);

	// close Lua environment of USER worker
	if (w->wkind == WorkerKind_USER) {
		// tell parent S that we exited
		s_notify(w->s, S_NOTE_WEXIT);

		UWorker* uw = (UWorker*)w;
		if (uw->s.L)
			lua_close(uw->s.L);
	} else {
		assert(w->wkind == WorkerKind_ASYNC);
		AWorker* aw = (AWorker*)w;
		u8 notes = S_NOTE_WEXIT;

		// handle ongoing work that was cancelled by pthread_cancel
		if (aw->req_is_active) {
			notes |= S_NOTE_ASYNCWORK;
			asyncworker_complete(aw, -EINTR);
		}

		// tell parent S that we exited
		s_notify(w->s, notes);
	}

	worker_release(w);
}


static const char* uworker_load_reader(lua_State* L, void* ud, usize* lenp) {
	UWorker* w = ud;
	*lenp = w->mainfun_lcode_len;
	return w->mainfun_lcode;
}


static void uworker_main(UWorker* w) {
	// create a Lua environment for this thread
	lua_State* L = luaL_newstate();
	w->s.L = L;

	// open libraries
	luaL_openlibs(L);
	luaL_requiref(L, "__rt", luaopen_runtime, 1);
	lua_setglobal(L, "__rt");

	// switch status to READY while checking if worker has been closed
	if UNLIKELY(!worker_cas_status((Worker*)w, Worker_READY))
		return trace_worker("CLOSED before getting READY");

	// disable thread cancelation to ensure graceful shutdown
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	// load Lua function from mainfun_lcode
	int status = lua_load(L, uworker_load_reader, (void*)w, "<worker>", "bt");

	// free memory back to malloc so we don't hold on to it "forever"
	free(w->mainfun_lcode);
	w->mainfun_lcode_len = 0; // sets errdesc_invalid=0
	w->mainfun_lcode = NULL;

	// check if load succeeded
	if UNLIKELY(status != LUA_OK) {
		trace_worker("failed to load worker function: %s", l_status_str(status));
		dlog("failed to load worker function: %s", l_status_str(status));
		return;
	}
	// Enter scheduler which will use the function on top of the stack as the main coroutine.
	// We ignore the return value from s_main since we exit on return regardless.
	s_main(&w->s);
}


#define ASYNCWORK_SQ_CAP 64
#define ASYNCWORK_CQ_CAP ASYNCWORK_SQ_CAP


static int s_asyncwork_cq_setup(S* s) {
	trace_sched("creating asyncwork CQ with cap=%d", ASYNCWORK_CQ_CAP);
	Chan* cq = chan_open(ASYNCWORK_CQ_CAP, sizeof(AsyncWorkRes));
	if (!cq)
		return -ENOMEM;
	s->asyncwork_cq = cq;
	return 0;
}


static int s_asyncwork_sq_setup(S* s) {
	trace_sched("creating asyncwork SQ with cap=%d", ASYNCWORK_SQ_CAP);
	Chan* sq = chan_open(ASYNCWORK_SQ_CAP, sizeof(AsyncWorkReq));
	if (!sq)
		return -ENOMEM;
	s->asyncwork_sq = sq;
	return 0;
}


static i64 asyncwork_do(const AsyncWorkReq* req);


static void asyncworker_main(AWorker* aw) {
	// switch status to READY while checking if worker has been closed
	if UNLIKELY(!worker_cas_status((Worker*)aw, Worker_READY)) {
		trace_worker("CLOSED before getting READY");
		return;
	}

	for (;;) {
		// pick up a work request
		// Note: This can't be interrupted by pthread_cancel; work_is_active checks are safe
		trace_worker("asyncwork SQ read...");
		if UNLIKELY(!chan_read(aw->w.s->asyncwork_sq, 0, &aw->req)) {
			// asyncwork_sq closed
			break;
		}

		// perform the work
		i64 result = asyncwork_do(&aw->req);

		// notify S that the work has completed
		asyncworker_complete(aw, result);
		s_notify(aw->w.s, S_NOTE_ASYNCWORK);
	}
}


static void worker_thread(Worker* w) {
	// setup thread cancelation handler and enable thread cancelation
	static int cleanup_pop_arg = 0;
	pthread_cleanup_push((void(*)(void*))worker_thread_exit, &w);

	#if defined(TRACE_SCHED_WORKER)
		tls_w_id = atomic_fetch_add(&tls_w_idgen, 1);
		trace_worker("start worker thread=%p", w->thread);
	#endif

	if (w->wkind == WorkerKind_USER) {
		uworker_main((UWorker*)w);
	} else {
		assert(w->wkind == WorkerKind_ASYNC);
		asyncworker_main((AWorker*)w);
	}

	pthread_cleanup_pop(&cleanup_pop_arg);
}


static int worker_start(Worker* w) {
	w->status = Worker_OPEN;
	worker_retain(w); // thread's reference
	pthread_attr_t* thr_attr = NULL;
	int err = pthread_create(&w->thread, thr_attr, (void*(*)(void*))worker_thread, w);
	trace_sched("spawn worker thread=%p", w->thread);
	if UNLIKELY(err) {
		free(w);
		return -err;
	}
	s_workers_add(w->s, w); // add worker to S's list of live workers
	return 0;
}


static int worker_open(Worker* w, S* s, u8 wkind) {
	w->s = s;
	w->wkind = wkind;
	w->nrefs = 1; // caller's reference

	// Chan* nullable chan_open(u32 cap, u32 entsize);

	return 0;
}


static int uworker_open(UWorker** uwp, S* s, void* mainfun_lcode, u32 mainfun_lcode_len) {
	UWorker* uw = calloc(1, sizeof(UWorker));
	if (!uw)
		return -ENOMEM;

	int err = worker_open(&uw->w, s, WorkerKind_USER);
	if (err) {
		free(uw);
		return err;
	}

	uw->mainfun_lcode_len = mainfun_lcode_len;
	uw->mainfun_lcode = mainfun_lcode;
	uw->s.isworker = true; // note: uw->s is the worker's S, not the spawner S 's'

	if (( err = s_asyncwork_cq_setup(&uw->s) )) {
		free(uw);
		return err;
	}

	*uwp = uw;
	return worker_start((Worker*)uw);
}


static bool worker_close(Worker* w) {
	assert(pthread_self() != w->thread); // must not call from worker's own thread

	if (!worker_cas_status(w, Worker_CLOSED)) {
		// already closed
		return false;
	}

	trace_worker("closing worker thread=%p", w->thread);

	// signal to worker's scheduler that it's time to shut down
	if (w->wkind == WorkerKind_USER) {
		s_shutdown(&((UWorker*)w)->s);
	} else {
		assert(w->wkind == WorkerKind_ASYNC);
		AWorker* aw = (AWorker*)w;
		// w->s->asyncwork_nworkers--;
	}

	pthread_cancel(w->thread);
	return true;
}


static int l_spawn_worker(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	luaL_checktype(L, 1, LUA_TFUNCTION);

	// serialize thread main function as Lua code, to be transferred to the thread
	lua_settop(L, 1); // ensure function is on the top of the stack
	Buf buf = {};
	int err = buf_append_luafun(&buf, L, /*strip_debuginfo*/false);
	if UNLIKELY(err) {
		buf_free(&buf);
		if (err == -EINVAL)
			return luaL_error(L, "unable to serialize worker function");
		return l_errno_error(L, -err);
	}

	// start a worker
	UWorker* uw;
	if UNLIKELY(( err = uworker_open(&uw, t->s, buf.bytes, buf.len) )) {
		buf_free(&buf);
		return l_errno_error(L, -err);
	}

	// allocate & return Worker object so that we know when the user is done with it (GC)
	UWorkerUVal* uval = uval_new(L, UValType_UWorker, sizeof(UWorkerUVal), 0);
	if UNLIKELY(!uval) {
		worker_close((Worker*)uw);
		worker_release((Worker*)uw);
		return 0;
	}
	lua_rawgetp(L, LUA_REGISTRYINDEX, &g_uworker_uval_luatabkey);
	lua_setmetatable(L, -2);
	uval->uw = uw;

	return 1;
}


static int s_asyncwork_spawn_worker(S* s) {
	trace_sched("spawn asyncwork worker");
	AWorker* aw = calloc(1, sizeof(AWorker));
	if (!aw)
		return -ENOMEM;
	int err = worker_open(&aw->w, s, WorkerKind_ASYNC);
	if (err) {
		free(aw);
		return err;
	}
	aw->req_is_active = 0;
	err = worker_start((Worker*)aw);
	if (err == 0)
		s->asyncwork_nworkers++;
	return err;
}


static int s_asyncwork_prepare(S* s) {
	u32 nreqs = atomic_fetch_add_explicit(&s->asyncwork_nreqs, 1, memory_order_acq_rel);
	trace_sched("[asyncwork] nreqs, nworkers = %u, %u", nreqs+1, s->asyncwork_nworkers);
	if LIKELY(s->asyncwork_sq != NULL && nreqs < s->asyncwork_nworkers)
		return 0;
	int err;
	if (s->asyncwork_sq == NULL && (err = s_asyncwork_sq_setup(s)))
		return err;
	if (s->asyncwork_cq == NULL && (err = s_asyncwork_cq_setup(s)))
		return err;
	return s_asyncwork_spawn_worker(s);
}


static int t_asyncwork_req(
	T* t, AsyncWorkReq* req, void* nullable cont_arg, TaskContinuation nullable cont)
{
	// if t is the only task running on S's thread we can just block, bypassing worker threads
	if (t->s->nlive == 1 && t->ntimers == 0) {
		trace_sched(T_ID_F " asyncwork op=%u immediate", t_id(t), req->op);
		i64 result = asyncwork_do(req);
		if (cont) {
			t->info.wait_async.result = result;
			return cont(t_L(t), T_RUNNING, cont_arg);
		} else {
			lua_pushinteger(t_L(t), result);
			return 1;
		}
	}

	// setup submission queue and/or spawn a worker thread, if needed
	int err = s_asyncwork_prepare(t->s);
	if (err)
		return err;

	// enqueue work request
	req->flags |= (u16)(cont != NULL) * AsyncWorkFlag_HAS_CONT;
	bool ok = chan_write(t->s->asyncwork_sq, 0, req);
	if UNLIKELY (!ok)
		return -ENOMEM;
	t_retain(t); // work's ref, released by s_asyncwork_read_cq
	trace_sched(T_ID_F " asyncwork op=%u SQ append", t_id(t), req->op);

	// suspend task
	t->resume_nres = 0;
	return t_suspend(t, T_WAIT_ASYNC, cont_arg, (TaskContinuation)cont);
}


static i64 asyncwork_do_nanosleep(const AsyncWorkReq* req) {
	u64 nsec = req->arg;
	struct timespec ts = {
		.tv_sec = nsec / 1000000000,
		.tv_nsec = nsec % 1000000000,
	};
	struct timespec ts_rem = {};
	while (nanosleep(&ts, &ts) == -1) {
		if (errno != EINTR)
			return -errno;
		if (ts_rem.tv_sec == 0 && ts_rem.tv_nsec < 100)
			break;
		dlog("nanosleep interrupted; continuing");
	}
	return 0;
}

static int l_syscall_nanosleep(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	u64 nsec = luaL_checkinteger(L, 1);
	AsyncWorkReq req = {
		.op = AsyncWorkOp_NANOSLEEP,
		.tid = t->tid,
		.arg = nsec,
	};
	return t_asyncwork_req(t, &req, NULL, NULL);
}


typedef struct AddrInfoReq {
	u16                       port;
	const char*               hostname;
	struct addrinfo           hints;
	struct addrinfo* nullable res;
} AddrInfoReq;


static i64 asyncwork_do_addrinfo(const AsyncWorkReq* req) {
	AddrInfoReq* aireq = (AddrInfoReq*)(uintptr)req->arg;

	char service[8];
	sprintf(service, "%u", aireq->port);
	trace_sched("getaddrinfo BEGIN hostname=\"%s\" port=%s", aireq->hostname, service);

	return getaddrinfo(aireq->hostname, service, &aireq->hints, &aireq->res);
}

static void l_syscall_addrinfo_cont_mkres(lua_State* L, AddrInfoReq* aireq) {
	int index = 1;
	char addr_str[INET6_ADDRSTRLEN];

	#define KV_I(k, v) ( lua_pushstring(L, k), lua_pushinteger(L, v), lua_settable(L, -3) )
	#define KV_S(k, v) ( lua_pushstring(L, k), lua_pushstring(L, v), lua_settable(L, -3) )

	lua_createtable(L, 0, 0);

	for (struct addrinfo *rp = aireq->res; rp != NULL; rp = rp->ai_next) {
		lua_createtable(L, 0, 5); // Table with 5 fields
		KV_I("family", rp->ai_family);
		KV_I("socktype", rp->ai_socktype);
		KV_I("protocol", rp->ai_protocol);
		if (rp->ai_family == AF_INET) {
			struct sockaddr_in* sa = (struct sockaddr_in*)rp->ai_addr;
			KV_I("port", ntohs(sa->sin_port));
			if (inet_ntop(AF_INET, &(sa->sin_addr), addr_str, INET_ADDRSTRLEN) != NULL)
				KV_S("addr", addr_str);
		} else if (rp->ai_family == AF_INET6) {
			struct sockaddr_in6* sa = (struct sockaddr_in6*)rp->ai_addr;
			KV_I("port", ntohs(sa->sin6_port));
			if (inet_ntop(AF_INET6, &(sa->sin6_addr), addr_str, INET6_ADDRSTRLEN) != NULL)
				KV_S("addr", addr_str);
		}

		// Add the address entry table to the main array at the current index
		lua_rawseti(L, -2, index++);
	}

	#undef KV_I
	#undef KV_S
}

static int l_syscall_addrinfo_cont(lua_State* L, int ltstatus, void* nullable arg) {
	T* t = L_t(L);
	AddrInfoReq* aireq = arg;

	int res = (int)t->info.wait_async.result;
	trace_sched("getaddrinfo FINISH => %d (%s)", res, res == 0 ? "" : gai_strerror(res));
	int nret = 1;
	if (res != 0) {
		lua_pushnil(L);
		lua_pushstring(L, gai_strerror(res));
		nret++;
	} else {
		l_syscall_addrinfo_cont_mkres(L, aireq);
		// family, socktype, protocol, port, addr
	}

	if (aireq->res)
		freeaddrinfo(aireq->res);
	free(aireq);

	return nret;
}

// syscall_addrinfo(hostname str, port=0, protocol=0, family=0, socktype=0, flags=0 uint)
// -> (addresses [{family=int, socktype=int, protocol=int, port=uint, addr=str}])
// -> (addresses nil, errmsg str)
static int l_syscall_addrinfo(lua_State* L) {
	T* t = REQUIRE_TASK(L);

	const char* hostname = luaL_checkstring(L, 1);
	if (!hostname)
		return 0;

	int ai_protocol = 0; // IPPROTO_ (e.g. IPPROTO_TCP, IPPROTO_UDP, etc)
	int ai_family = AF_UNSPEC; // IPv4 or IPv6
	int ai_socktype = 0; // SOCK_STREAM, SOCK_DGRAM, or 0 for any
	int ai_flags = AI_NUMERICSERV;
	u16 port = 0;
	int isnum, v;
	u64 v64;

	if (( v64 = lua_tointegerx(L, 2, &isnum), isnum )) {
		if (v64 > U16_MAX)
			return luaL_error(L, "port number too large: %d", v64);
		port = (u16)v64;
	}
	if ((v = lua_tointegerx(L, 3, &isnum)), isnum) ai_protocol = v;
	if ((v = lua_tointegerx(L, 4, &isnum)), isnum) ai_family = (v == 0) ? AF_UNSPEC : v;
	if ((v = lua_tointegerx(L, 5, &isnum)), isnum) ai_socktype = v;
	if ((v = lua_tointegerx(L, 6, &isnum)), isnum) ai_flags |= v;

	AddrInfoReq* aireq = calloc(1, sizeof(AddrInfoReq));
	if (!aireq)
		return l_errno_error(L, ENOMEM);
	aireq->port = (u16)port;
	aireq->hostname = hostname;
	aireq->hints.ai_family = ai_family;
	aireq->hints.ai_socktype = ai_socktype; // Datagram socket
	aireq->hints.ai_flags = ai_flags;
	aireq->hints.ai_protocol = ai_protocol;

	AsyncWorkReq req = {
		.op = AsyncWorkOp_ADDRINFO,
		.tid = t->tid,
		.arg = (uintptr)aireq,
	};
	return t_asyncwork_req(t, &req, aireq, l_syscall_addrinfo_cont);
}


static i64 asyncwork_do(const AsyncWorkReq* req) {
	switch ((enum AsyncWorkOp)req->op) {
	case AsyncWorkOp_NOP:        return 0; break;
	case AsyncWorkOp_NANOSLEEP:  return asyncwork_do_nanosleep(req);
	case AsyncWorkOp_ADDRINFO:   return asyncwork_do_addrinfo(req);
	case AsyncWorkOp_WORKER_MSG: assertf(0, "invalid WORKER_MSG"); break;
	}
	assertf(0, "op=%u", req->op);
	unreachable();
}


static int l_yield(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	int nargs = lua_gettop(L);
	return t_yield(t, nargs);
}


// fun spawn_task(f fun(... any), ... any)
static int l_spawn_task(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	t->resume_nres = s_spawn_task(t->s, L, t);
	if UNLIKELY(t->resume_nres == 0)
		return 0;

	// suspend the calling task, switching control back to scheduler loop
	return t_yield(t, 0);
}


// fun socket(domain, socktype int) FD
static int l_socket(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	int domain = luaL_checkinteger(L, 1); // PF_ constant
	int type = luaL_checkinteger(L, 2);   // SOCK_ constants
	int protocol = 0;

	#if defined(__linux__)
		type |= SOCK_NONBLOCK | SOCK_CLOEXEC;
	#endif

	int fd = socket(domain, type, protocol);
	if UNLIKELY(fd < 0)
		return l_errno_error(L, errno);

	// Ask the OS to check the connection once in a while (stream sockets only)
	if (type == SOCK_STREAM) {
		int optval = 1;
		if UNLIKELY(setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) < 0)
			dlog("warning: failed to set SO_KEEPALIVE on socket: %s", strerror(errno));
	}

	// Set file descriptor to non-blocking mode..
	// Not needed on linux where we pass SOCK_NONBLOCK when creating the socket.
	#if !defined(__linux__)
		int flags = fcntl(fd, F_GETFL, 0);
		if UNLIKELY(fcntl(fd, F_SETFL, flags | O_NONBLOCK)) {
			int err = errno;
			close(fd);
			return luaL_error(L, "socket: failed to set O_NONBLOCK: %s", strerror(err));
		}
	#endif

	IODesc* d = l_iodesc_create(L);
	d->fd = fd;

	int err = iopoll_open(&t->s->iopoll, d);
	if UNLIKELY(err) {
		close(fd);
		d->fd = -1;
		return l_errno_error(L, -err);
	}

	return 1;
}


static int l_connect_cont(lua_State* L, __attribute__((unused)) int ltstatus, IODesc* d) {
	T* t = L_t(L);
	trace_sched(T_ID_F, t_id(t));

	dlog("d events=%s nread=%ld nwrite=%ld",
		 d->events == 'r'+'w' ? "rw" : d->events == 'r' ? "r" : d->events == 'w' ? "w" : "0",
		 d->nread, d->nwrite);

	// check for connection failure on darwin, which sets EOF, which we propagate as r+w
	#if defined(__APPLE__)
	if UNLIKELY(d->events == 'r'+'w' && d->nread >= 0 && d->nwrite >= 0) {
		int error = 0;
		socklen_t len = sizeof(error);
		if (getsockopt(d->fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0) {
			if (error != 0)
				return l_errno_error(L, error);
		}
	}
	#endif

	// check for error
	if UNLIKELY(d->nread < 0 || d->nwrite < 0) {
		int err;
		if ((d->events & 'r') == 'r' && d->nread < 0) {
			err = (int)-d->nread;
		} else {
			err = (int)-d->nwrite;
		}
		if (err > 0)
			return l_errno_error(L, err);
	}

	return 0;
}


// connect(fd FD, addr string)
static int l_connect(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	IODesc* d = l_iodesc_check(L, 1);

	// Second argument is the address to connect to.
	// Interpretation depends on d's socket type.
	size_t addrstr_len;
	const char* addrstr = luaL_checklstring(L, 2, &addrstr_len);
	if (!addrstr)
		return 0;

	dlog("addrstr: %s", addrstr);

	// construct network address
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(12345);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (connect(d->fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
		// connect succeeded immediately, without blocking
		return 0;
	}

	if (errno == EINPROGRESS) {
		trace_sched("wait for connect");
		return t_iopoll_wait(t, d, l_connect_cont);
	}

	// error
	int err = errno;
	close(d->fd);
	d->fd = -1;
	return l_errno_error(L, err);
}


static int l_read_cont(lua_State* L, __attribute__((unused)) int ltstatus, IODesc* d) {
	// check for error
	if UNLIKELY(d->nread < 0)
		return l_errno_error(L, (int)-d->nread);

	// check for EOF
	if UNLIKELY(d->nread == 0) {
		lua_pushinteger(L, 0);
		return 1;
	}

	Buf* buf = l_buf_check(L, 2);
	if (!buf)
		return 0;

	#if 1 // version of the code that resizes buffer if needed
		// TODO: check to see if there's a 3rd argument with explicit read limit
		usize readlim = d->nread;
		if (buf_reserve(buf, readlim) == NULL)
			return l_errno_error(L, ENOMEM);
	#else // version of the code that only reads what can fit in buf
		// If there's no available space in buf (i.e. buf->cap - buf->len == 0) then read() will
		// return 0 which the caller should interpret as EOF. However, a caller that does not stop
		// calling read when it returns 0 may end up in an infinite loop.
		// For that reason we treat "read nothing" as an error.
		if UNLIKELY(buf->cap - buf->len == 0)
			return luaL_error(L, "no space in buffer to read into");
		// usize readlim = MIN((usize)d->nread, buf->cap - buf->len);
	#endif

	// read
	//dlog("read bytes[%zu] (<= %zu B)", buf->len, readlim);
	ssize_t len = read(d->fd, &buf->bytes[buf->len], readlim);

	// check for error
	if UNLIKELY(len < 0) {
		if (errno == EAGAIN)
			return t_iopoll_wait(L_t(L), d, l_read_cont);
		return l_errno_error(L, errno);
	}

	// update d & buf
	if (len == 0) { // EOF
		d->nread = 0;
	} else {
		d->nread -= MIN(d->nread, (i64)len);
		buf->len += (usize)len;
	}

	// TODO: if there's a 3rd argument 'nread' that is >0, t_iopoll_wait again if
	// d->nread < nread so that "read" only returns after reading 'nread' bytes.

	lua_pushinteger(L, len);
	return 1;
}


// fun read(fd FD, buf Buf, nbytes uint = 0)
static int l_read(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	IODesc* d = l_iodesc_check(L, 1);
	// TODO: if there's a 3rd argument 'nread' that is >0, wait unless d->nread >= nread.
	// if there's nothing available to read, we will have to wait for it
	if (d->nread == 0)
		return t_iopoll_wait(t, d, l_read_cont);
	return l_read_cont(L, 0, d);
}


static int l_msg_stow(lua_State* src_L, lua_State* dst_L, InboxMsg* msg) {
	// Store message payload in a table.
	// This way the message payload gets GC'd when the destination task is GC'd,
	// regardless of message delivery.
	lua_createtable(dst_L, msg->nres, 0);

	// first value is the sender task (Lua "thread")
	lua_pushthread(src_L);      // push sender "thread" onto its own stack
	lua_xmove(src_L, dst_L, 1); // move sender "thread" to receiver's stack
	lua_rawseti(dst_L, -2, 1);  // Store in table at index 1

	for (int i = 2; i <= msg->nres; i++) {
		lua_pushvalue(src_L, i);    // Get argument from its position in src_L ...
		lua_xmove(src_L, dst_L, 1); // ... and move it to dst_L
		lua_rawseti(dst_L, -2, i);  // Store in table at index (i-1)
	}

	#ifdef DEBUG
	for (int i = 1; i <= msg->nres; i++) {
		lua_rawgeti(dst_L, -1, i);
		if (lua_isnil(dst_L, -1)) {
			lua_pop(dst_L, 2);  // pop nil and table
			panic("failed to store argument %d", i);
		} else {
			// dlog("msg payload[%d] = %s", i, lua_typename(dst_L, lua_type(dst_L, -1)));
		}
		lua_pop(dst_L, 1);  // pop checked value
	}
	#endif

	msg->msg.ref = luaL_ref(dst_L, LUA_REGISTRYINDEX);

	return 0;
}


static int l_msg_unload(lua_State* dst_L, InboxMsg* msg) {
	// Note: Payload table will be placed at index 2 in the stack.
	// Stack index 1 is occupied by msg->type, pushed by l_recv_deliver.
	lua_rawgeti(dst_L, LUA_REGISTRYINDEX, msg->msg.ref); // Get the payload table
	assertf(lua_type(dst_L, 2) == LUA_TTABLE,
			"%s", lua_typename(dst_L, lua_type(dst_L, 2)));
	for (int i = 1; i <= msg->nres; i++) {
		lua_rawgeti(dst_L, 2, i); // get value i from table
		assert(!lua_isnoneornil(dst_L, -1));
	}
	lua_remove(dst_L, 2); // remove payload table from stack
	luaL_unref(dst_L, LUA_REGISTRYINDEX, msg->msg.ref); // free reference
	return 1 + msg->nres; // msg.type + payload
}


static int l_recv_deliver(lua_State* dst_L, T* t, InboxMsg* msg) {
	trace_sched(T_ID_F " deliver msg type=%u", t_id(t), msg->type);
	assert(msg->type != InboxMsgType_MSG_DIRECT); // should never happen

	// wake tasks waiting to send
	if UNLIKELY(t->inbox->waiters) {
		t_wake_waiters(t, t->inbox->waiters);
		t->inbox->waiters = 0;
	}

	lua_pushinteger(dst_L, msg->type);
	if (msg->type == InboxMsgType_MSG)
		return l_msg_unload(dst_L, msg);
	return 1 + msg->nres;
}


static int l_recv_cont(lua_State* L, int ltstatus, void* arg) {
	T* t = arg;
	assert(t->inbox != NULL);
	InboxMsg* msg = inbox_pop(t->inbox);
	assert(msg != NULL);
	if (msg->type == InboxMsgType_MSG_DIRECT)
		return 1 + msg->nres;
	return l_recv_deliver(L, t, msg);
}


// fun recv(from... Task|Worker) (type int, sender any, ... any)
// If no 'from' is specified, receive from anyone
static int l_recv(lua_State* L) {
	T* t = REQUIRE_TASK(L);

	// TODO: check "from" arguments.

	// Remove all arguments. This is required for l_recv_deliver & l_msg_unload to
	// work properly as they place return values onto the stack.
	lua_pop(L, lua_gettop(L));

	// check for a ready message in the inbox
	if (t->inbox) {
		InboxMsg* msg = inbox_pop(t->inbox);
		if (msg)
			return l_recv_deliver(L, t, msg);
	}

	// check for deadlock
	S* s = t->s;
	bool empty_runq = s->runnext == NULL && s->runq->fifo.head == s->runq->fifo.tail;
	if UNLIKELY(
		s->runnext == NULL && s->runq->fifo.head == s->runq->fifo.tail && // empty runq
		empty_runq && s->nlive == 1 && // only task running
		t->ntimers == 0 && // no timers
		s->workers == NULL && // no workers
		(!s->isworker || t->tid != 1) // t is not the main task of a worker
	) {
		if (s->nlive == 1 && t->ntimers == 0) {
			// T is the only live task and no timers are active
			return luaL_error(t_L(t), "deadlock detected: recv would never return");
		}
	}

	// wait for a message
	return t_suspend(t, T_WAIT_RECV, t, l_recv_cont);
}


static int l_send_task1(lua_State* L, T* t, T* dst_t);


static int l_send_task_cont(lua_State* L, int ltstatus, void* arg) {
	T* dst_t = arg;
	return l_send_task1(L, L_t(L), dst_t);
}


static int l_send_task1(lua_State* L, T* t, T* dst_t) {
	// place message in receiver's inbox
	const u32 maxcap = 64;
	InboxMsg* msg = inbox_add(&dst_t->inbox, maxcap);
	if UNLIKELY(msg == NULL) {
		// receiver inbox is full; wait until there's space
		trace_sched("send later to " T_ID_F " (inbox full)", t_id(dst_t));
		if UNLIKELY(dst_t->inbox == NULL) // failed to create inbox
			return l_errno_error(L, ENOMEM);

		// add t to list of tasks waiting to send to this inbox
		t->info.wait_task.next_tid = dst_t->inbox->waiters;
		t->info.wait_task.wait_tid = dst_t->tid;
		dst_t->inbox->waiters = t->tid;
		t->resume_nres = 0;

		// wait until there's space in the inbox and try again
		return t_suspend(t, T_WAIT_SEND, dst_t, l_send_task_cont);
	}
	memset(msg, 0, sizeof(*msg));
	msg->type = InboxMsgType_MSG;

	// Set message argument count, which includes msg type and sender.
	// Note: The logical arithmetic is non-trivial here: It's actually "(lua_gettop(L)-1) + 1"
	// -1 the destination argument to 'send()' +1 sender.
	msg->nres = lua_gettop(L);

	lua_State* src_L = L;
	lua_State* dst_L = t_L(dst_t);

	// if the destination task is not currently waiting in a call to recv(),
	// the message will be delivered later.
	if (dst_t->status != T_WAIT_RECV) {
		trace_sched("send buffered to " T_ID_F, t_id(dst_t));
		return l_msg_stow(src_L, dst_L, msg);
	}

	trace_sched("send directly to " T_ID_F, t_id(dst_t));

	// Destination task is already waiting for a message, deliver it directly.
	// Move message values from send() function's arguments to destination task's L stack,
	// which is essentially the stack (activation record) of the recv() function call of the
	// destination task.
	lua_pushinteger(dst_L, msg->type); // push message type
	lua_pushthread(src_L);             // push sender "thread" onto its own stack
	lua_xmove(src_L, dst_L, 1);        // move sender "thread" to receiver's stack
	lua_xmove(src_L, dst_L, msg->nres - 1); // arguments passed after 'dst' to send()
	msg->type = InboxMsgType_MSG_DIRECT;

	// Now, there are two options for how to resume the waiting receiver task.
	// Either we can switch to it right here, bypassing the scheduler, with
	//    t_resume(dst_t); return 0;
	// or we can put the receiver task on the priority run queue and switch to the
	// scheduler with
	//    s_runq_put_runnext(t->s, dst_t); return t_yield(t, 0);
	// On my MacBook, a direct switch takes on average 140ns while going through the
	// scheduler takes on average 170ns. Very small difference in practice.
	// While switching directly is more efficient, it has the disadvantage of allowing two
	// tasks to hog the scheduler, by ping ponging send, recv send, recv send, ...
	// So we are going through the scheduler. A wee bit less efficient but more correct.
	//
	// put destination task in the priority runq so that it runs asap
	s_runq_put_runnext(t->s, dst_t);
	//
	// put the sender task at the end of the runq and return to scheduler loop (s_main)
	return t_yield(t, 0);
}


static int l_send_task(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	T* dst_t = l_check_task(L, 1);
	if (!dst_t)
		return 0;
	return l_send_task1(L, t, dst_t);
}


// fun structclone_encode(flags uint, transfer_list [any], value ...any) Buf
static int l_structclone_encode(lua_State* L) {
	u64 flags = luaL_checkinteger(L, 1);
	// ignore arg 2 "transfer_list" for now, until it's supported

	// create buffer (lifetime managed by GC)
	Buf* buf = l_buf_createx(L, 512);
	if UNLIKELY(!buf)
		return l_errno_error(L, ENOMEM);

	// replace the flags argument on stack with the buffer
	lua_replace(L, 1);

	// L's stack should now look like this:
	//   buffer (userdata)
	//   transfer_list
	//   arg1
	//   arg2
	//   argN

	// tell structclone_encode that there's a transfer_list (even if nil) on the stack
	flags |= StructCloneEnc_TRANSFER_LIST;

	int nargs = lua_gettop(L) - 2; // -2: not including buf or transfer_list
	int err = structclone_encode(L, buf, flags, nargs);
	if UNLIKELY(err)
		return 0;

	// pop transfer_list
	lua_pop(L, 1);

	// return buffer
	dlog_lua_stackf(L, "ret");
	return 1;
}


static int l_structclone_decode(lua_State* L) {
	Buf* buf = l_buf_check(L, 1);
	if (!buf)
		return 0;
	lua_pop(L, 1); // remove buffer from stack
	return structclone_decode(L, buf->bytes, buf->len);
}


static int l_send_worker(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	UWorkerUVal* uval = uval_check(L, 1, UValType_UWorker, "Task or Worker");
	if (!uval)
		return 0;
	UWorker* uw = uval->uw;

	// structurally clone the arguments
	int nargs = lua_gettop(L) - 1; // -1: not including worker
	Buf buf = {};
	int err = structclone_encode(L, &buf, 0, nargs);
	if (err)
		return l_errno_error(L, -err);

	// put message on target worker's CQ
	ChanTx tx = chan_write_begin(uw->s.asyncwork_cq, 0);
	assertf(tx.entry != NULL, "chan_write(cq) failed");
	AsyncWorkRes* res = tx.entry;
	res->op = AsyncWorkOp_WORKER_MSG;
	res->msg.a = 1;
	res->msg.b = 2;
	res->msg.c = 3;
	chan_write_commit(uw->s.asyncwork_cq, tx);

	// notify the worker's scheduler that there's stuff in asyncwork_cq
	s_notify(&uw->s, S_NOTE_ASYNCWORK);

	return 0;
}


// fun send(Task|Worker destination, msg... any)
static int l_send(lua_State* L) {
	if (lua_isthread(L, 1))
		return l_send_task(L);
	return l_send_worker(L);
}


static int l_await_task_cont1(lua_State* L, T* t, T* other_t) {
	// first return value is status 0=error, 1=clean exit, 2=stopped
	lua_pushinteger(L, other_t->info.dead.how);

	// copy return values from other task
	int nres = other_t->resume_nres;
	lua_State* other_L = t_L(other_t);
	l_copy_values(other_L, L, nres);

	return nres + 1;
}


static int l_await_task_cont(lua_State* L, int ltstatus, void* arg) {
	T* t = arg;

	// note: even though other_t is T_DEAD, its memory is still valid
	// since await holds a GC reference via arguments.
	T* other_t = s_task(t->s, t->info.wait_task.wait_tid);

	return l_await_task_cont1(L, t, other_t);
}


// fun await(t Task) (ok int, ... any)
// Returns ok=0 on error with the error as the 2nd result.
// Returns ok=1 on clean exit with return values from t as rest results.
// Returns ok=2 if t was stopped.
static int l_await_task(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	T* other_t = l_check_task(L, 1);
	if (!other_t)
		return 0;

	// a task can't await itself
	if UNLIKELY(t == other_t)
		return luaL_error(L, "attempt to 'await' itself");

	// if the other task already existed, we can return immediately
	if (other_t->status == T_DEAD)
		return l_await_task_cont1(L, t, other_t);

	// we don't support waiting for a task running on a different OS thread (Worker)
	if UNLIKELY(other_t->s != t->s)
		return luaL_error(L, "attempt to await task of a different Worker");

	// other task cannot possibly be running, since the calling task is running
	assert(other_t->status != T_RUNNING);

	// add t to list of tasks waiting for other_t
	t->info.wait_task.next_tid = other_t->waiters;
	t->info.wait_task.wait_tid = other_t->tid;
	other_t->waiters = t->tid;

	t->resume_nres = 0;

	// wait
	return t_suspend(t, T_WAIT_TASK, t, l_await_task_cont);
}


static int l_await_worker_cont1(lua_State* L, UWorker* uw) {
	if LIKELY(uw->s.exiterr == 0) {
		lua_pushboolean(L, true);
		return 1;
	}
	// worker exited because of an error
	lua_pushboolean(L, false);
	const char* errdesc = "unknown error";
	if (!uw->errdesc_invalid && uw->errdesc && *uw->errdesc)
		errdesc = uw->errdesc;
	lua_pushstring(L, errdesc);
	return 2;
}


static int l_await_worker_cont(lua_State* L, int ltstatus, void* arg) {
	return l_await_worker_cont1(L, arg);
}


// fun await(w Worker) (ok bool, err string)
// Returns true if w exited cleanly or false if w exited because of an error.
static int l_await_worker(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	UWorkerUVal* uval = uval_check(L, 1, UValType_UWorker, "Task or Worker");
	if (!uval)
		return 0;
	UWorker* uw = uval->uw;

	// can't await the worker the calling task is running on
	if UNLIKELY(t->s == &uw->s)
		return luaL_error(L, "attempt to 'await' itself");

	if (worker_add_waiter((Worker*)uw, t)) {
		t->resume_nres = 0;
		return t_suspend(t, T_WAIT_WORKER, uw, l_await_worker_cont);
	}

	// worker closed before we could add t to list of tasks waiting for w
	return l_await_worker_cont1(L, uw);
}


static int l_await(lua_State* L) {
	if (lua_isthread(L, 1))
		return l_await_task(L);
	return l_await_worker(L);
}


static int l_taskblock_begin(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	trace_sched("taskblock_begin");
	// TODO
	return t_yield(t, 0);
}


static int l_taskblock_end(lua_State* L) {
	T* t = REQUIRE_TASK(L);
	trace_sched("taskblock_end");
	return t_yield(t, 0);
}


static int l_monotime(lua_State* L) {
	lua_pushinteger(L, DTimeNow());
	return 1;
}


// fun typename(v any) str
static int l_typename(lua_State* L) {
	int t = lua_type(L, 1);
	if (t == LUA_TNUMBER) {
		int isint;
		i64 val = lua_tointegerx(L, 1, &isint);
		lua_pushstring(L, isint ? "int" : "float");
		return 1;
	} else if (t == LUA_TTHREAD) {
		// check if it's a task
		lua_State* TL = assertnotnull(lua_tothread(L, 1));
		T* t = L_t(TL);
		if (t->s) {
			lua_pushstring(L, "Task");
			return 1;
		}
	} else if (t == LUA_TUSERDATA && lua_getmetatable(L, 1)) {
		// use __name from metatable if available
		lua_getfield(L, -1, "__name");
		if (!lua_isnil(L, -1))
			return 1;
		lua_pop(L, 2); // pop nil and metatable
	}
	// fall back to Lua's builtin type(v) function
	lua_getglobal(L, "type");
	lua_pushvalue(L, 1);
	lua_call(L, 1, 1);
	return 1;
}


static const luaL_Reg dew_lib[] = {
	{"intscan", l_intscan},
	{"intfmt", l_intfmt},
	{"intconv", l_intconv},
	{"errstr", l_errstr},
	{"monotime", l_monotime},
	{"typename", l_typename},

	// "new" runtime API
	{"main", l_main},
	{"spawn_task", l_spawn_task},
	{"spawn_worker", l_spawn_worker},
	{"yield", l_yield},
	{"sleep", l_sleep},
	{"socket", l_socket},
	{"connect", l_connect},
	{"read", l_read},
	{"buf_create", l_buf_create},
	{"buf_resize", l_buf_resize},
	{"buf_str", l_buf_str},
	{"timer_start", l_timer_start},
	{"timer_update", l_timer_update},
	{"timer_stop", l_timer_stop},
	{"await", l_await},
	{"recv", l_recv},
	{"send", l_send},
	{"structclone_encode", l_structclone_encode},
	{"structclone_decode", l_structclone_decode},

	// potentially long-time blocking syscalls, performed in worker thread pool
	{"syscall_nanosleep", l_syscall_nanosleep},
	{"syscall_addrinfo", l_syscall_addrinfo},

	// WIP
	{"taskblock_begin", l_taskblock_begin},
	{"taskblock_end", l_taskblock_end},

	// wasm experiment
	#ifdef __wasm__
	{"ipcrecv", l_ipcrecv},
	{"ipcrecv_co", l_ipcrecv_co},
	{"iowait", l_iowait},
	#endif

	{NULL, NULL} // Sentinel
};

int luaopen_runtime(lua_State* L) {
	#ifdef __wasm__
	g_L = L; // wasm experiment
	#endif

	// // Override the built-in type function
	// lua_getglobal(L, "type");
	// lua_pushvalue(L, -1);
	// lua_setglobal(L, "_lua_type"); // save original function
	// lua_pushcfunction(L, l_type);
	// lua_setglobal(L, "type");

	luaL_newlib(L, dew_lib);

	luaopen_iopoll(L);
	luaopen_buf(L);

	// Timer (ref to an internally managed Timer struct)
	luaL_newmetatable(L, "Timer");
	lua_pushcfunction(L, l_timerobj_gc);
	lua_setfield(L, -2, "__gc");
	lua_rawsetp(L, LUA_REGISTRYINDEX, &g_timerobj_luatabkey);

	// Worker
	luaL_newmetatable(L, "Worker");
	lua_pushcfunction(L, l_uworker_uval_gc);
	lua_setfield(L, -2, "__gc");
	lua_rawsetp(L, LUA_REGISTRYINDEX, &g_uworker_uval_luatabkey);

	// export libc & syscall constants
	#define _(NAME) \
		lua_pushinteger(L, NAME); \
		lua_setfield(L, -2, #NAME);
	// address families
	_(AF_LOCAL)  // Host-internal protocols, formerly called PF_UNIX
	_(AF_INET)   // Internet version 4 protocols
	_(AF_INET6)  // Internet version 6 protocols
	_(AF_ROUTE)  // Internal Routing protocol
	_(AF_VSOCK)  // VM Sockets protocols
	// protocols
	_(IPPROTO_IP)
	_(IPPROTO_ICMP)
	_(IPPROTO_TCP)
	_(IPPROTO_UDP)
	_(IPPROTO_IPV6)
	_(IPPROTO_ROUTING)
	_(IPPROTO_RAW)
	// socket types
	_(SOCK_STREAM)
	_(SOCK_DGRAM)
	_(SOCK_RAW)
	#undef _

	// export ERR_ constants
	#define _(NAME, ERRNO, ...) \
		lua_pushinteger(L, NAME); \
		lua_setfield(L, -2, #NAME);
	FOREACH_ERR(_)
	// note: not including ERR_ERROR on purpose as it's "unknown error"
	#undef _

	return 1;
}


// #include "lua/src/lstate.h"
// __attribute__((constructor)) static void init() {
// 	printf("sizeof(lua_State): %zu B\n", sizeof(T) + sizeof(lua_State));
// }
