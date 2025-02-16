#pragma once
#include "dew.h"

#if defined(__linux__) || defined(__APPLE__)
	#include <pthread.h>
	typedef pthread_t OSThread;
#endif

API_BEGIN

typedef struct S          S;          // scheduler (M+P in Go lingo)
typedef struct T          T;          // task
typedef struct Worker     Worker;     // CPU-specific instance of S
typedef struct IOPoll     IOPoll;     // platform specific I/O facility
typedef struct IOPollDesc IOPollDesc; // descriptor (state used for I/O requests)
typedef struct IODesc     IODesc;     // wraps a file descriptor, used by iopoll
typedef struct Buf        Buf;        // growable array usable via buf_* Lua functions
typedef struct Timer      Timer;
typedef struct TimerInfo  TimerInfo;  // cache friendly 'timers' array entry
typedef struct FIFO       FIFO;
typedef struct RunQ       RunQ;
typedef struct Inbox      Inbox;
typedef struct InboxMsg   InboxMsg;


struct Buf {
	usize cap, len;
	u8* nullable bytes;
};

struct IOPoll {
	S* s;
	#if defined(__APPLE__)
	int kq;
	#endif
};

struct IODesc {
	T* nullable t;      // task to wake up on events
	i32         fd;     // -1 if unused
	u16         seq;    // sequence for detecting use after close
	u8          events; // 'r', 'w' or 'r'+'w' (114, 119 or 233)
	u8          _unused;
	i64         nread;  // bytes available to read, or -errno on error
	i64         nwrite; // bytes available to write, or -errno on error
};

typedef T* nullable (*TimerF)(Timer* timer, void* nullable arg);

struct Timer {
	DTime          when;   // a specific point in time. -1 if dead (not in s.timers)
	DTimeDuration  period; // if >0, repeat every when+period
	DTimeDuration  leeway; // precision request; how much this timer is willing to fluctuate
	u8             nrefs;  // reference count (internal + lua, so really just 2 bits needed)
	void* nullable arg;    // passed to f
	TimerF         f;      // f can return non-NULL to have a T woken up
};

struct TimerInfo {
	Timer* timer;
	DTime  when;
};

typedef Array(TimerInfo) TimerPQ;

enum InboxMsgType {
	InboxMsgType_TIMER,      // timer rang
	InboxMsgType_MSG,        // message via send(), with payload
	InboxMsgType_MSG_DIRECT, // message via send(), delivered directly
};

struct InboxMsg {
	u8  type; // enum InboxMsgType
	int nres; // number of result values
	union {
		struct {
			int ref; // Lua ref to an array table holding values
		} msg;
	};
};

struct Inbox {
	union {
		// Caution! The following struct makes assumptions about the layout of the FIFO struct
		struct {
			u32 _fifo_header[3];
			u32 waiters; // list of tasks (tid's) waiting to send to this inbox
		};
		FIFO fifo;
	};
	InboxMsg entries[];
};

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
	T_DEAD,        // dead
};

enum TDied {
	TDied_ERR,   // uncaught error
	TDied_CLEAN, // clean exit
	TDied_STOP,  // stopped by parent task
};

struct T {
	S*              s;            // owning S
	Inbox* nullable inbox;        // message queue

	u32 tid;          // task identifier
	u32 prev_sibling; // tid of previous sibling in parent's list of children
	u32 next_sibling; // tid of next sibling in parent's list of children
	u32 first_child;  // list of children (most recently spawned. tid)
	u32 parent;       // tid of task that spawned this task (0 for main task)
	u32 nrefs;        // references to task (when 0, T may be GC'd)
	u8  status;       // T_ constant (enum TStatus)
	u8  resume_nres;  // number of results on stack, to be returned via resume
	u16 ntimers;      // number of live timers
	u32 waiters;      // list of tasks (tid's) waiting for this task (info.wait_task)

	// 'info' holds data specific to 'status', used while task is suspended
	union {
		struct { // T_WAIT_TASK
			u32 wait_tid; // tid of other task this task is waiting for
			u32 next_tid; // link to other waiting task in 'waiters' list
		} wait_task;
		struct { // T_WAIT_WORKER
			u32 next_tid; // link to other waiting task in 'waiters' list
		} wait_worker;
		struct { // T_DEAD
			u8 how; // TDied_ constant
			// note: must not overlay wait_task.wait_tid
		} dead;
		u64 _align;
	} info;

	// rest of struct is a lua_State struct
	// Note: With Lua 5.4, the total size of T + lua_State is 248 B on 64-bit arch
};

enum { // S.notes
	S_NOTE_WEXIT = 1u<<0, // a worker spawned by this S has exited
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

	// workers spawned by this S
	Worker* nullable workers; // list
};

enum WorkerStatus {
	Worker_CLOSED,
	Worker_OPEN,
	Worker_READY,
};

struct Worker {
	// state accessed only by parent thread
	T*               parent;  // T which spawned this worker
	Worker* nullable next;    // list link in S.workers

	// state accessed by both parent thread and worker thread
	OSThread     thread;
	_Atomic(u8)  status;  // Worker_ constant (enum WorkerStatus)
	_Atomic(u8)  nrefs;
	_Atomic(u32) waiters; // list of tasks (tid's) waiting for this task (T.info.wait_task)

	// input & output data (as input for worker_open, as output from worker exit.)
	union {
		struct {
			// mainfun_lcode contains Lua bytecode for the main function, used during setup.
			// Worker owns this memory, and it's free'd during thread setup.
			u32            mainfun_lcode_len; // number of bytes at mainfun_lcode
			void* nullable mainfun_lcode;
		};
		struct {
			// When a worker has exited, this holds the description of an error
			// which caused the worker to exit, or NULL if no error or if there are no waiters.
			// Memory is free'd by worker_free.
			char* nullable errdesc;
		};
	};

	S s;
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


int iopoll_init(IOPoll* iopoll, S* s);
void iopoll_dispose(IOPoll* iopoll);
int iopoll_interrupt(IOPoll* iopoll); // -errno on error
int iopoll_poll(IOPoll* iopoll, DTime deadline, DTimeDuration deadline_leeway);
int iopoll_open(IOPoll* iopoll, IODesc* d);
int iopoll_close(IOPoll* iopoll, IODesc* d);

int s_iopoll_wake(S* s, IODesc** dv, u32 count);


API_END
