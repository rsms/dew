// Dynamic array
#pragma once
#include "../dew.h"
API_BEGIN

struct Array {
    u32            cap, len;
    void* nullable v;
};

#define Array(T) struct { u32 cap, len; T* nullable v; }

void array_free(struct Array* a); // respects embedded a->v
void* nullable _array_reserve(struct Array* a, u32 elemsize, u32 minavail);
inline static void* nullable array_reserve(struct Array* a, u32 elemsize, u32 minavail) {
    return LIKELY(minavail <= a->cap - a->len) ? a->v + (usize)a->len*(usize)elemsize :
           _array_reserve(a, elemsize, minavail);
}
bool array_append(struct Array* a, u32 elemsize, const void* elemv, u32 elemc);

API_END
