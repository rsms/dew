#pragma once
#include "dew.h"
API_BEGIN

typedef struct S S; // scheduler (M+P in Go lingo)
typedef struct T T; // task

// growable array usable via buf_* Lua functions
typedef struct Buf {
	usize cap, len;
	u8* nullable bytes;
} Buf;

// platform specific I/O facility
typedef struct IOPoll     IOPoll;     // facility
typedef struct IOPollDesc IOPollDesc; // descriptor (state used for I/O requests)
typedef struct IODesc     IODesc; // wraps a file descriptor, used by iopoll
typedef struct Timer      Timer;
typedef struct TimerInfo  TimerInfo; // cache friendly 'timers' array entry

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

struct Timer {
	DTime         when;     // a specific point in time. -1 if dead (not in s.timers)
	DTimeDuration period;   // if >0, repeat every when+period
	uintptr       arg, seq; // passed along to f
	T* nullable (*f)(uintptr arg, uintptr seq);
};

struct TimerInfo {
	Timer* timer;
	DTime  when;
};

typedef Array(TimerInfo) TimerPQ;

typedef enum TStatus : u8 {
	TStatus_RUN,   // running
	TStatus_DEAD,  // dead
	TStatus_YIELD, // suspended
	TStatus_NORM,  // normal
} TStatus;

struct T {
	S*             s;            // owning S
	T* nullable    parent;       // task that spawned this task (NULL = S's main task)
	T* nullable    next_sibling; // next sibling in parent's list of children
	T* nullable    first_child;  // list of children
	void* nullable _unused1;
	int            resume_nres;  // number of results on stack, to be returned via resume
	u8             is_live;      // 1 when running or waiting, 0 when exited
	u8             _unused[3];
	// rest of struct is a lua_State struct
	// Note: With Lua 5.4, the total size of T + lua_State is 248 B
};

struct S {
	lua_State* L;      // base Lua environment
	IOPoll     iopoll; // platform-specific I/O facility

	u32 nlive; // number of live tasks

	// runq is a queue of tasks (TIDs) ready to run; a circular buffer.
	// (We could get fancy with a red-black tree a la Linux kernel. Let's keep it simple for now.)
	u32 runq_head;
	u32 runq_tail;
	u32 runq_cap;
	T** runq;

	// runnext is a TID (>0) if a task is to be run immediately, skipping runq
	T* nullable runnext;

	// timers
	TimerPQ   timers; // priority queue (heap) of TimerInfo entries
	TimerInfo timers_storage[8];
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
void iopoll_shutdown(IOPoll* iopoll);
int iopoll_poll(IOPoll* iopoll, DTime deadline);
int iopoll_open(IOPoll* iopoll, IODesc* d);
int iopoll_close(IOPoll* iopoll, IODesc* d);

int s_iopoll_wake(S* s, IODesc** dv, u32 count);


API_END
