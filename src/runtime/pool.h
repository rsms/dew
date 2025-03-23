// Pool maps data to dense indices.
// It's like a slab allocator that uses u32 integers as addresses.
// Think "file descriptor allocator" rather than "virtual-memory allocator."
// Tries to be cache friendly by always using the smallest free index during allocation.
#pragma once
#include "../dew.h"
API_BEGIN

typedef struct Pool {
    u32 cap;      // capacity, a multiple of 64
    u32 maxidx;   // max allocated index
    u64 freebm[]; // bitmap; bit=1 means entries[bit] is free
    // TYPE entries[];
} Pool;

bool pool_init(Pool** pp, u32 cap, usize elemsize);
inline static void pool_free_pool(Pool* nullable p) { free(p); }
void* nullable pool_entry_alloc(Pool** pp, u32* idxp, usize elemsize);
void pool_entry_free(Pool* p, u32 idx);

u32 _pool_find_entry(const Pool* p, const void* entry_ptr, usize elemsize);
u32 _pool_find_entry_ptr(const Pool* p, const void* entry_val);

inline static u32 pool_find_entry(const Pool* p, const void* entry_ptr, usize elemsize) {
    if (elemsize == sizeof(void*))
        return _pool_find_entry_ptr(p, *(const void**)entry_ptr);
    return _pool_find_entry(p, entry_ptr, elemsize);
}

inline static bool pool_entry_isfree(const Pool* p, u32 idx) {
    u32 chunk_idx = (idx - 1) >> 6; // (idx-1)/64
    u32 bit_idx = (idx - 1) & (64 - 1); // (idx-1)%64
    return idx > 0 && idx <= p->cap && (p->freebm[chunk_idx] & ((u64)1 << bit_idx));
}

inline static void* pool_entries(const Pool* p) {
    return (void*)p->freebm + (p->cap >> 3); // + cap/bytes_per_freebm
}

inline static void* pool_entry(const Pool* p, u32 idx, usize elemsize) {
    assert(idx > 0 && idx <= p->maxidx);
    return pool_entries(p) + (idx-1)*elemsize;
}

API_END
