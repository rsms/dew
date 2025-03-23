#include "pool.h"


static bool pool_grow(Pool** pp, u32 newcap, usize elemsize) {
	Pool* p = *pp;
	u32 oldcap = 0;

	// if p is not empty, newcap is current capacity + one extra chunk
	if (p && p->cap > 0) {
		oldcap = p->cap;
		if (check_add_overflow(oldcap, 64u, &newcap))
			return false;
	}

	// calculate new size, checking for overflow
	usize newentsize;
	usize newsize = sizeof(Pool) + ((usize)newcap >> 3); // newcap>>3 == newcap/sizeof(u64)
	if (check_mul_overflow(elemsize, (usize)newcap, &newentsize) ||
	    check_add_overflow(newentsize, newsize, &newsize) )
	{
		return false;
	}

	// dlog("pool_grow (cap %u -> %u) total %zu B (entries %zu B, freebm %u B)",
	//      oldcap, newcap, newsize, newentsize, newcap/(u32)sizeof(u64));

	// attempt to resize allocation
	Pool* newp = realloc(p, newsize);
	if UNLIKELY(!newp)
		return false;
	p = newp;
	p->cap = newcap;
	*pp = p;

	if (oldcap == 0) {
		// new allocation; set all freebm chunks' bits to 1 (all slots are free)
		p->maxidx = 0;
		memset(p->freebm, 0xff, (usize)newcap >> 3);
	} else {
		// new freebm overlap with existing entries, so we need to move the entries up
		// before we set all bits to 1 in the new freebm chunk
		void* oldfreebm = (void*)p->freebm + ((usize)oldcap >> 3);
		void* newfreebm = (void*)p->freebm + ((usize)newcap >> 3);
		memmove(newfreebm, oldfreebm, p->maxidx * elemsize);
		*(u64*)oldfreebm = ~(u64)0; // set all bits to 1 (all slots are free);
	}

	return true;
}


bool pool_init(Pool** pp, u32 cap, usize elemsize) {
	*pp = NULL;
	u32 aligned_cap = ALIGN2(cap, 64);
	if (aligned_cap < cap) // overflow; cap too large
		return false;
	return pool_grow(pp, aligned_cap, elemsize);
}


void* nullable pool_entry_alloc(Pool** pp, u32* idxp, usize elemsize) {
	Pool* p;
	if UNLIKELY(*pp == NULL)
		goto grow;
again:
	p = *pp;
	for (u32 chunk_idx = 0, nchunks = p->cap>>6; chunk_idx < nchunks; chunk_idx++) {
		u64 bm = p->freebm[chunk_idx];
		u32 bit_idx = dew_ffs(bm); // find first free buffer id (if any) in this chunk
		if (bit_idx) {
			// found a free block; mark it as used by setting bit (bit_idx-1) to 0
			u32 idx = chunk_idx*64 + bit_idx; // note: idx is 1-based, not 0-based
			// dlog(">>> allocate bit %u in chunk %u (idx = %u)", bit_idx-1, chunk_idx, idx);
			p->freebm[chunk_idx] = bm & ~((u64)1 << (bit_idx - 1));
			*idxp = idx;
			if (idx > p->maxidx)
				p->maxidx = idx;
			return pool_entry(p, idx, elemsize);
		}
		// all blocks of this chunk are in use; try next chunk
	}
	// all chunks were occupied; grow pool
grow:
	if (!pool_grow(pp, 64, elemsize))
		return NULL;
	goto again;
}


void pool_entry_free(Pool* p, u32 idx) {
	u32 chunk_idx = (idx - 1) >> 6; // (idx-1)/64
	u32 bit_idx = (idx - 1) & (64 - 1); // (idx-1)%64

	// assert that this index is valid and that the slot is marked as occupied
	assert(idx > 0 && idx <= p->cap);
	assert((p->freebm[chunk_idx] & ((u64)1 << bit_idx)) == 0);

	p->freebm[chunk_idx] |= (u64)1 << bit_idx;

	if (idx != p->maxidx)
		return;

	// maxidx was freed.
	// We have three options in this scenario:

	// 1. Do nothing else and leave maxidx as is.
	//    Unless the free call pattern is "perfect", maxidx will often not be true.
	//    We will waste time if we do a lot of operations that are bound by maxidx.
	#if 0
	p->maxidx--;
	#endif

	// 2. Scan for the actual maxidx.
	//    This means looking through every bit of every chunk below current maxidx
	//    until we find a 0 bit. This can be quite time intensive but is 100% accurate.
	//    Good if we spend most of our time on operations that are bound by maxidx.
	#if 1
	for (;;) {
		u64 chunk = p->freebm[chunk_idx];
		if (chunk == 0) {
			// chunk is full and next chunk is empty
			p->maxidx = (chunk_idx + 1) << 6;
			break;
		} else if (chunk != ~(u64)0) {
			// chunk has at least one 0 bit.
			// Find first 0 bit by first flipping all bits with xor,
			// then use ffs to find the first 1 bit.
			p->maxidx = dew_ffs(chunk ^ ~(u64)0);
			break;
		} else {
			// chunk is completely free
			if (chunk_idx == 0) {
				p->maxidx = 0;
				break;
			}
			chunk_idx--;
		}
	}
	#endif

	// 3. Something in between: scan for entire free bit chunks.
	//    Balanced trade off between accuracy and speed.
	#if 0
	p->maxidx--;
	while (p->freebm[chunk_idx] == ~(u64)0) {
		p->maxidx = chunk_idx << 6;
		dlog("free chunk %u, setting maxidx %u", chunk_idx, p->maxidx);
		if (chunk_idx == 0)
			break;
		chunk_idx--;
	}
	#endif
}


u32 _pool_find_entry(const Pool* p, const void* entry_ptr, usize elemsize) {
	for (u32 idx = p->maxidx; idx > 0; idx--) {
		if (!pool_entry_isfree(p, idx)) {
			void* entp = pool_entry(p, idx, elemsize);
			if (memcmp(entp, entry_ptr, elemsize) == 0)
				return idx;
		}
	}
	return 0;
}

u32 _pool_find_entry_ptr(const Pool* p, const void* entry_val) {
	for (u32 idx = p->maxidx; idx > 0; idx--) {
		if (!pool_entry_isfree(p, idx)) {
			void** entp = (void**)pool_entry(p, idx, sizeof(entry_val));
			if (*entp == entry_val)
				return idx;
		}
	}
	return 0;
}


#ifdef DEBUG
__attribute__((constructor)) static void pool_test() {
	Pool* p;
	bool ok = pool_init(&p, /*cap*/3, 8); assert(ok);

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
	dlog("OK: %s", __FUNCTION__);
}
#endif // DEBUG
