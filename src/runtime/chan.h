// inter-thread message queue (MP-MC safe)
#include "../dew.h"
API_BEGIN

typedef struct Chan Chan;
typedef struct ChanTx {
    void* nullable entry; // NULL = queue closed
    u32            tx;
} ChanTx;

#define CHAN_TRY (1u<<0) // flag for making an attempt at an operation, non-blocking

Chan* nullable chan_open(u32 cap, u32 entsize);
void chan_close(Chan* ch);
void chan_shutdown(Chan* ch);
bool chan_is_shutdown(const Chan* ch);
u32 chan_cap(const Chan* ch);

// 'begin' functions blocks until successful or channel closed, indicated by tx.entry=NULL.
// If flags&CHAN_TRY is set, tx.entry=NULL is returned if reading or writing is not immediately
// possible. Ask chan_is_shutdown to tell "full" or "empty" apart from "shut down".

ChanTx chan_write_begin(Chan* ch, u32 flags);
void   chan_write_commit(Chan* ch, ChanTx tx);

ChanTx chan_read_begin(Chan* ch, u32 flags);
void   chan_read_commit(Chan* ch, ChanTx tx);

// convenience functions which copies the value
bool chan_write(Chan* ch, u32 flags, const void* value_src);
bool chan_read(Chan* ch, u32 flags, void* value_dst);

API_END
