// inter-thread message queue (MP-MC safe)
#include "dew.h"
API_BEGIN

typedef struct Chan Chan;
typedef struct ChanTx {
    void* nullable entry; // NULL = queue closed
    u32            tx;
} ChanTx;

Chan* nullable chan_open(u32 cap, u32 entsize);
void chan_close(Chan* ch);
void chan_shutdown(Chan* ch);

u32 chan_cap(const Chan* ch);

ChanTx chan_write_begin(Chan* ch);
void   chan_write_commit(Chan* ch, ChanTx tx);

ChanTx chan_read_begin(Chan* ch);
void   chan_read_commit(Chan* ch, ChanTx tx);

// convenience functions which copies the value
bool chan_write(Chan* ch, const void* value_src);
bool chan_read(Chan* ch, void* value_dst);

API_END
