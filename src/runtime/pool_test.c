#ifdef DEBUG
#include "pool.h"

static void pool_test_bug1() {
    // regression test for a bug in pool_entry_free where the new maxid would be calculated
    // incorrectly during bitmaps scanning.

    Pool* p;
    bool ok = pool_init(&p, 8, 8);
    assert(ok);
    // pool_init(&s->taskreg, 8, sizeof(T*));

    u32 idx;
    u64* vp;
    #define ADD(v) { \
        vp = assertnotnull(pool_entry_alloc(&p, &idx, 8)); \
        assertf((u32)v == idx, "%u == %u", (u32)v, idx); \
        *vp = (v); \
    }
    #define DEL(v) { \
        assertf(!pool_idx_isdead(p, v), "%u", (u32)v); \
        vp = pool_entry(p, v, 8); \
        assertf((u64)v == *vp, "%lu == %lu", (u64)v, *vp); \
        pool_entry_free(p, v); \
    }

    ADD(1); assert(idx == 1);
    ADD(2); assert(idx == 2);
    ADD(3); assert(idx == 3);
    ADD(4); assert(idx == 4);
    ADD(5); assert(idx == 5);
    DEL(3);
    DEL(2);
    ADD(2); assert(idx == 2);
    ADD(3); assert(idx == 3);
    ADD(6); assert(idx == 6);
    ADD(7); assert(idx == 7);
    DEL(7); assertf(p->maxidx == 6, "%u", p->maxidx);
    DEL(6); assertf(p->maxidx == 5, "%u", p->maxidx);
    DEL(3); assertf(p->maxidx == 5, "%u", p->maxidx);
    DEL(2); assertf(p->maxidx == 5, "%u", p->maxidx);
    DEL(5); assertf(p->maxidx == 4, "%u", p->maxidx); // bug was here, in pool_entry_free
    DEL(4); assertf(p->maxidx == 1, "%u", p->maxidx);
    DEL(1); assertf(p->maxidx == 0, "%u", p->maxidx);

    pool_free_pool(p);
    dlog("OK: %s", __FUNCTION__);
}

__attribute__((constructor)) static void pool_test() {
    Pool* p;
    bool ok = pool_init(&p, /*cap*/3, 8);
    assert(ok);

    u32 N = 200;

    // verify that all slots are free
    for (u32 idx = 1; idx <= p->cap; idx++)
        assert(pool_entry_isfree(p, idx));

    // allocate dense range
    for (u32 idx = 1; idx <= N; idx++) {
        u32 idx2;
        u64* vp = pool_entry_alloc(&p, &idx2, 8);
        assert(vp);
        assert(idx == idx2); // expect dense sequential index
        *vp = idx;
        // dlog("%u, %u", idx, idx2);
    }

    // verify
    for (u32 idx = 1; idx <= N; idx++) {
        assert(!pool_entry_isfree(p, idx));
        u64* vp = pool_entry(p, idx, 8);
        assert(*vp == idx);
    }

    // free every 3rd entry
    for (u32 idx = 1; idx <= N; idx++) {
        if (idx % 4 == 3)
            pool_entry_free(p, idx);
    }

    // verify
    for (u32 idx = 1; idx <= N; idx++) {
        if (idx % 4 == 3) {
            assert(pool_entry_isfree(p, idx));
        } else {
            assert(!pool_entry_isfree(p, idx));
            u64* vp = pool_entry(p, idx, 8);
            assert(*vp == idx);
        }
    }

    // re-allocate free'd slots
    for (u32 idx = 1; idx <= N; idx++) {
        if (idx % 4 == 3) {
            u32 idx2;
            u64* vp = pool_entry_alloc(&p, &idx2, 8);
            assert(vp);
            assert(idx == idx2); // expect dense sequential index
            *vp = idx;
        }
    }

    // verify
    for (u32 idx = 1; idx <= N; idx++) {
        assert(!pool_entry_isfree(p, idx));
        u64* vp = pool_entry(p, idx, 8);
        assert(*vp == idx);
    }

    // free all entries with a pattern that causes idx==maxidx in a few cases
    for (u32 idx = 10; idx <= N; idx++)
        if ((idx % 4) != 3) pool_entry_free(p, idx);
    for (u32 idx = 1; idx <= N; idx++)
        if ((idx % 4) == 3 || idx < 10) pool_entry_free(p, idx);

    // verify that all slots are free
    for (u32 idx = 1; idx <= p->cap; idx++)
        assert(pool_entry_isfree(p, idx));

    pool_free_pool(p);

    pool_test_bug1();

    dlog("OK: %s", __FUNCTION__);
}

#endif // DEBUG
