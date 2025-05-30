#pragma once
#include "../dew.h"
#include "fifo.h"
#include "buf.h"
API_BEGIN

typedef struct Inbox    Inbox;
typedef struct InboxMsg InboxMsg;
typedef struct UWorker  UWorker;
typedef struct S        S;

enum InboxMsgType {
    InboxMsgType_TIMER,         // timer rang
    InboxMsgType_MSG,           // message via send(task ...)
    InboxMsgType_MSG_DIRECT,    // message via send(task ...) delivered directly
    InboxMsgType_MSG_REMOTE,    // message via send(remotetask ...)
    InboxMsgType_WORKER_CLOSED, // message sent by runtime when a worker closed
};

struct InboxMsg {
    u8 type; // enum InboxMsgType
    u8 _unused;
    union {
        struct { // InboxMsgType_TIMER
            u16 nres; // number of result values
        } __attribute__((packed)) timer;
        struct { // InboxMsgType_MSG, InboxMsgType_MSG_DIRECT
            u16 nres; // number of result values
            int ref;  // Lua ref to an array table holding values
        } __attribute__((packed)) msg;
        struct { // InboxMsgType_MSG_REMOTE
            u16      _unused1;
            u32      sender_sid;
            u32      sender_tid;
            u32      _unused2;
            MiniBuf* buf; // owned by this InboxMsg
        } __attribute__((packed)) msg_remote;
        struct { // InboxMsgType_WORKER_CLOSED
            u16      _unused1;
            u32      worker_sid; // stored since worker->s.sid may be reset async
            UWorker* worker;     // reference owned by this InboxMsg
        } __attribute__((packed)) worker_closed;
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
inline static void inbox_free(Inbox* inbox) { fifo_free(&inbox->fifo); }

inline static InboxMsg* nullable inbox_pop(Inbox* inbox) {
    return fifo_pop(&inbox->fifo, sizeof(*inbox->entries));
}

const char* inbox_msg_type_str(enum InboxMsgType t);

API_END
