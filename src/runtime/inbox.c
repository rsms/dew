#include "inbox.h"

static_assert(sizeof(InboxMsg) % sizeof(void*) == 0, "");


InboxMsg* nullable inbox_add(Inbox** inboxp, u32 maxcap) {
    // setup inbox if needed
    if UNLIKELY(*inboxp == NULL) {
        assert(maxcap > 0);
        const u32 initcap = 8;
        *inboxp = (Inbox*)fifo_alloc(initcap, sizeof(*(*inboxp)->entries));
        if UNLIKELY(!*inboxp)
            return NULL;
        (*inboxp)->waiters = 0;
    }

    return fifo_push((FIFO**)inboxp, sizeof(*(*inboxp)->entries), maxcap);
}


const char* inbox_msg_type_str(enum InboxMsgType t) {
    switch (t) {
    case InboxMsgType_TIMER:         return "TIMER";
    case InboxMsgType_MSG:           return "MSG";
    case InboxMsgType_MSG_DIRECT:    return "MSG_DIRECT";
    case InboxMsgType_MSG_WORKER:    return "MSG_WORKER";
    case InboxMsgType_WORKER_CLOSED: return "WORKER_CLOSED";
    }
    return "?";
}
