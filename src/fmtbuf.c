#include "dew.h"
#ifdef DEBUG

thread_local char        g_fmtbufv[8][512];
static thread_local int  g_fmtbufi = 0;

char* fmtbuf_get() {
    return g_fmtbufv[g_fmtbufi++ % 8];
}

#endif
