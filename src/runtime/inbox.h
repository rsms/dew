#pragma once
#include "../dew.h"
#include "fifo.h"
API_BEGIN

typedef struct Inbox    Inbox;
typedef struct InboxMsg InboxMsg;

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

// maxcap: messages we can send() without blocking. Returns NULL if inbox is full.
InboxMsg* nullable inbox_add(Inbox** inboxp, u32 maxcap);

inline static InboxMsg* nullable inbox_pop(Inbox* inbox) {
    return fifo_pop(&inbox->fifo, sizeof(*inbox->entries));
}

API_END
