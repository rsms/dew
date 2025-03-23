#pragma once
#include "../dew.h"
#include "timer.h"
#include "pool.h"
#include "chan.h"
#include "fifo.h"
#include "iopoll.h"
#include "inbox.h"

API_BEGIN

typedef struct S    S; // scheduler (M+P in Go lingo)
typedef struct T    T; // task
typedef struct RunQ RunQ;

// forward declaration since S uses this type
#if DEW_ENABLE_WORKERS
    typedef struct Worker Worker;
#endif

struct RunQ {
	FIFO fifo;
	T*   entries[];
};

enum TStatus {
	T_READY,       // on runq
	T_RUNNING,     // currently running
	T_WAIT_IO,     // suspended, waiting for I/O or sleep timer
	T_WAIT_SEND,   // suspended, waiting to send to inbox
	T_WAIT_RECV,   // suspended, waiting for inbox message
	T_WAIT_TASK,   // suspended, waiting for a task to exit
	T_WAIT_WORKER, // suspended, waiting for a worker to exit
	T_WAIT_ASYNC,  // suspended, waiting for an async operation (e.g. syscall) to finish
	T_DEAD,        // dead
};

enum TDied {
	TDied_ERR,   // uncaught error
	TDied_CLEAN, // clean exit
	TDied_STOP,  // stopped by parent task
};

struct T {
	S*              s;     // owning S
	Inbox* nullable inbox; // message queue

	u32 tid;   // task identifier
	u32 nrefs; // references to task (when 0, T may be GC'd)

	u8  status;      // T_ constant (enum TStatus)
	u8  resume_nres; // number of results on stack, to be returned via resume
	u16 ntimers;     // number of live timers
	u32 waiters;     // list of tasks (tid's) waiting for this task (info.wait_task)

	u32 parent;       // tid of task that spawned this task (0 for main task)
	u32 prev_sibling; // tid of previous sibling in parent's list of children
	u32 next_sibling; // tid of next sibling in parent's list of children
	u32 first_child;  // list of children (most recently spawned. tid)

	// 'info' holds data specific to 'status', used while task is suspended
	union {
		struct { // T_WAIT_TASK
			u32 wait_tid; // tid of other task this task is waiting for
			u32 next_tid; // link to other waiting task in 'waiters' list
		} wait_task;
		struct { // T_WAIT_WORKER
			u32 next_tid; // link to other waiting task in 'waiters' list
		} wait_worker;
		struct { // T_WAIT_ASYNC
			i64 result;
		} wait_async;
		struct { // T_DEAD
			u8 how; // TDied_ constant
			// note: must not overlay wait_task.wait_tid
		} dead;
		u64 _align;
	} info;

	// rest of struct is a lua_State struct
};

enum { // S.notes
	S_NOTE_WEXIT     = 1u<<0, // a worker spawned by this S has exited
	S_NOTE_ASYNCWORK = 1u<<1, // a worker completed AsyncWorkReq
};

struct S {
	lua_State*    L;         // base Lua environment
	IOPoll        iopoll;    // platform-specific I/O facility
	Pool*         tasks;     // all live tasks
	u32           nlive;     // number of live (not T_DEAD) tasks
	_Atomic(bool) isclosed;  // true when the parent worker is shutting down
	_Atomic(u8)   notes;     // events from a worker (e.g. worker exited; S_NOTE_ bits)
	bool          exiterr;   // true if S ended because of a runtime error
	bool          doexit;    // call exit(exiterr) when S ends
	bool          isworker;  // true if S is part of a Worker

	RunQ*       runq;    // queue of tasks ready to run; a circular buffer
	T* nullable runnext; // task to be run immediately, skipping runq

	TimerPQ   timers;            // priority queue (heap) of TimerInfo entries
	TimerInfo timers_storage[8]; // initial storage for 'timers' array

	#if DEW_ENABLE_WORKERS

	// workers spawned by this S
	Worker* nullable workers; // list

	// asyncwork threads
	u32            asyncwork_nworkers; // number of live workers (monotonic)
	_Atomic(u32)   asyncwork_nreqs;    // number of pending work requests
	Chan* nullable asyncwork_sq;       // submission queue
	Chan* nullable asyncwork_cq;       // completion queue

	#endif // DEW_ENABLE_WORKERS
};


// s_id formats an identifier of a S for logging
#define s_id(s) ((unsigned long)(uintptr)(s))
#define S_ID_F  "S#%lx"

// t_id formats an identifier of a T for logging
#define t_id(t) (t)->tid, ((unsigned long)(uintptr)t_L(t))
#define T_ID_F  "T%u#%lx"


// L_t returns Lua state of task, while L_t returns task of Lua state.
// T is stored in the LUA_EXTRASPACE in the head of Lua managed lua_State
inline static lua_State* t_L(const T* t) { return (void*)t + sizeof(*t); }
inline static T* L_t(lua_State* L) { return (void*)L - sizeof(T); }


S* nullable s_get_thread_local();
int s_iopoll_wake(S* s, IODesc** dv, u32 count);


// TODO: get rid of these
#define FOREACH_ERR(_) \
	/* NAME, errno equivalent, description */ \
	_( ERR_OK,           0,       "no error") \
	_( ERR_INVALID,      EINVAL,  "invalid data or argument") \
	_( ERR_RANGE,        ERANGE,  "result out of range") \
	_( ERR_INPUT,        -1,      "invalid input") \
	_( ERR_SYSOP,        -1,      "invalid syscall op or syscall op data") \
	_( ERR_BADFD,        EBADF,   "invalid file descriptor") \
	_( ERR_BADNAME,      -1,      "invalid or misformed name") \
	_( ERR_NOTFOUND,     ENOENT,  "resource not found") \
	_( ERR_NAMETOOLONG,  -1,      "name too long") \
	_( ERR_CANCELED,     -1,      "operation canceled") \
	_( ERR_NOTSUPPORTED, ENOTSUP, "not supported") \
	_( ERR_EXISTS,       EEXIST,  "already exists") \
	_( ERR_END,          -1,      "end of resource") \
	_( ERR_ACCESS,       EACCES,  "permission denied") \
	_( ERR_NOMEM,        -1,      "cannot allocate memory") \
	_( ERR_MFAULT,       -1,      "bad memory address") \
	_( ERR_OVERFLOW,     -1,      "value too large") \
	_( ERR_READONLY,     EROFS,   "read-only") \
	_( ERR_IO,           EIO,     "I/O error") \
	_( ERR_NOTDIR,       ENOTDIR, "not a directory") \
	_( ERR_ISDIR,        EISDIR,  "is a directory") \
// end FOREACH_ERR
enum {
	#define _(NAME, ERRNO, ...) NAME,
	FOREACH_ERR(_)
	#undef _
	ERR_ERROR = 0xff,
};


API_END
