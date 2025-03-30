#include "inbox.h"


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
