#pragma once
#include "dew.h"
API_BEGIN

typedef struct S S; // scheduler (M+P in Go lingo)
typedef struct T T; // task

// platform specific I/O facility
typedef struct IOPoll     IOPoll;     // facility
typedef struct IOPollDesc IOPollDesc; // descriptor (state used for I/O requests)

struct IOPoll {
	S* s;
	#if defined(__APPLE__)
	int kq;
	#endif
};

enum : u8 {
	IOPollDesc_EV_READ  = 1<<0,
	IOPollDesc_EV_WRITE = 1<<1,
	IOPollDesc_EV_TIMER = 1<<2,
	IOPollDesc_EV_EOF   = 1<<3,
};

enum : u8 {
	IOPollDesc_USE_GENERIC = 0,
	IOPollDesc_USE_CONNECT,
};

struct IOPollDesc {
	IOPollDesc* nullable link;

	T*   t;
	u32  seq;
	u8   use;    // IOPollDesc_USE_* constant
	u8   events; // IOPollDesc_EV_* bits
	bool active;
	i64  result; // -errno or positive value (meaning depends on use)
};

typedef enum TStatus : u8 {
	TStatus_RUN,   // running
	TStatus_DEAD,  // dead
	TStatus_YIELD, // suspended
	TStatus_NORM,  // normal
} TStatus;

struct T {
	S*                   s;            // owning S
	T* nullable          parent;       // task that spawned this task (NULL = S's main task)
	T* nullable          next_sibling; // next sibling in parent's list of children
	T* nullable          first_child;  // list of children
	IOPollDesc* nullable polldesc;     // non-NULL when task has pending I/O request
	int                  resume_nres;  // number of results on stack, to be returned via resume
	u8                   is_live;      // 1 when running or waiting, 0 when exited
	u8                   _unused[3];
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


int  iopoll_open(IOPoll* iopoll, S* s);
void iopoll_close(IOPoll* iopoll);
int  iopoll_poll(IOPoll* iopoll, DTime deadline);
int  iopoll_add_timer(IOPoll* iopoll, IOPollDesc* d, DTime deadline);
int  iopoll_add_fd(IOPoll* iopoll, IOPollDesc* d, int fd, u8 events /*IOPollDesc_EV_* bits*/);

int s_iopoll_wake(S* s, IOPollDesc* d);


API_END
