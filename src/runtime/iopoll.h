// platform specific I/O facility
#pragma once
#include "../dew.h"
#include "time.h"
API_BEGIN

typedef struct S      S; // forward declaration since S embeds IOPoll
typedef struct T      T;
typedef struct IOPoll IOPoll;
typedef struct IODesc IODesc; // wraps a file descriptor

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

int  iopoll_init(IOPoll* iopoll, S* s);
void iopoll_dispose(IOPoll* iopoll);
int  iopoll_interrupt(IOPoll* iopoll); // -errno on error
int  iopoll_poll(IOPoll* iopoll, DTime deadline, DTimeDuration deadline_leeway);
int  iopoll_open(IOPoll* iopoll, IODesc* d);
int  iopoll_close(IOPoll* iopoll, IODesc* d);

// l_iodesc_create allocates & pushes an IODesc object onto L's stack.
// Throws LUA_ERRMEM if memory allocation fails.
IODesc* l_iodesc_create(lua_State* L);
IODesc* nullable l_iodesc_check(lua_State* L, int idx);

void luaopen_iopoll(lua_State* L);

API_END
