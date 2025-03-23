// FIFO is a "first in, last out" queue (not thread safe, obviously)
#pragma once
#include "../dew.h"
API_BEGIN

typedef struct FIFO {
    u32 cap;
    u32 head;
    u32 tail;
    u32 _alignment_padding;
    //TYPE entries[];
} FIFO;

FIFO* nullable fifo_alloc(u32 cap, usize elemsize);
void* nullable fifo_push(FIFO** qp, usize elemsize, u32 maxcap);
void* nullable fifo_pop(FIFO* q, usize elemsize);

API_END
