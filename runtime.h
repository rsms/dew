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

struct FIFO {
	u32 cap;
	u32 head;
	u32 tail;
	//TYPE entries[];
};

struct InboxMsg {
	InboxMsg* nullable next;
	u8                 what;
	T*    nullable     sender;
	void* nullable     data;
};

struct Inbox {
	FIFO     fifo;
	InboxMsg entries[];
};

struct RunQ {
	FIFO fifo;
	T*   entries[];
};

typedef enum TStatus : u8 {
	TStatus_READY,     // on runq
	TStatus_RUNNING,   // currently running
	TStatus_WAIT_IO,   // suspended, waiting for I/O or sleep timer
	TStatus_WAIT_RECV, // suspended, waiting for inbox message
	TStatus_DEAD,      // dead
} TStatus;

struct T {
	S*              s;            // owning S
	T* nullable     parent;       // task that spawned this task (NULL = S's main task)
	T* nullable     next_sibling; // next sibling in parent's list of children
	T* nullable     first_child;  // list of children
	Inbox* nullable inbox;        // list of inbox messages
	int             resume_nres;  // number of results on stack, to be returned via resume
	TStatus         status;       // TStatus_ constant
	u8              tid_small;    // low-numbered idx in s->tasks (if tracked) 0=scan
	u16             ntimers;      // number of live timers
	// rest of struct is a lua_State struct
	// Note: With Lua 5.4, the total size of T + lua_State is 248 B on 64-bit arch
};

struct S {
	lua_State*  L;      // base Lua environment
	IOPoll      iopoll; // platform-specific I/O facility

	_Atomic(bool) isclosed; // true when the parent worker is shutting down

	Pool* nullable tasks; // all currently live tasks (tracked if non-NULL)
	u32            nlive; // number of live tasks (always tracked)

	RunQ*       runq;    // queue of tasks ready to run; a circular buffer
	T* nullable runnext; // task to be run immediately, skipping runq

	TimerPQ   timers;            // priority queue (heap) of TimerInfo entries
	TimerInfo timers_storage[8]; // initial storage for 'timers' array

	// workers spawned by this S
	Worker* nullable workers; // list
};

typedef enum WorkerStatus : u8 {
	WorkerStatus_CLOSED,
	WorkerStatus_OPEN,
	WorkerStatus_READY,
} WorkerStatus;

struct Worker {
	// state accessed only by parent thread
	Worker* nullable next; // list link in S.workers

	// state accessed by both parent thread and worker thread
	OSThread    thread;
	_Atomic(u8) status;
	_Atomic(u8) nrefs;

	// mainfun_lcode contains Lua bytecode for the main function
	u32            mainfun_lcode_len; // number of bytes at mainfun_lcode
	void* nullable mainfun_lcode;     // Worker owns this memory, free'd by worker_free

	S s;
};


// s_id formats an identifier of a S for logging
#define s_id(s) ((unsigned long)(uintptr)(s))
#define S_ID_F  "S#%lx"

// t_id formats an identifier of a T for logging
#define t_id(t) ((unsigned long)(uintptr)t_L(t))
#define T_ID_F  "T#%lx"


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
