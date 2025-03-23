#include "inbox.h"


InboxMsg* nullable inbox_add(Inbox** inboxp) {
    #if !defined(DEBUG)
        const u32 initcap = 8;  // messages to allocate space for up front
        const u32 maxcap  = 64; // messages we can send() without blocking
    #else
        // DEBUG values
        const u32 initcap = 4;
        const u32 maxcap  = 4;
    #endif

    // setup inbox if needed
    if UNLIKELY(*inboxp == NULL) {
        *inboxp = (Inbox*)fifo_alloc(initcap, sizeof(*(*inboxp)->entries));
        if UNLIKELY(!*inboxp)
            return NULL;
        (*inboxp)->waiters = 0;
    }

    return fifo_push((FIFO**)inboxp, sizeof(*(*inboxp)->entries), maxcap);
}
