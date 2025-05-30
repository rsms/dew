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
		// zero entries data
		memset(pool_entries(p), 0x00, (usize)newcap * elemsize);
	} else {
		// new freebm overlap with existing entries, so we need to move the entries up
		// before we set all bits to 1 in the new freebm chunk
		void* oldfreebm = (u8*)p->freebm + ((usize)oldcap >> 3);
		void* newfreebm = (u8*)p->freebm + ((usize)newcap >> 3);
		memmove(newfreebm, oldfreebm, p->maxidx * elemsize);
		*(u64*)oldfreebm = ~(u64)0; // set all bits to 1 (all slots are free);
		// zero new entries data
		memset(pool_entries(p) + oldcap*elemsize, 0x00, (usize)(newcap - oldcap) * elemsize);
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

	// dlog(">> free bit %u in chunk %u", bit_idx, chunk_idx);
	// dlog("   %016lx | %016lx = %016lx",
	//      p->freebm[chunk_idx], (u64)1 << bit_idx,
	//      p->freebm[chunk_idx] | ((u64)1 << bit_idx));

	p->freebm[chunk_idx] |= (u64)1 << bit_idx;

	if (idx != p->maxidx)
		return;

	// maxidx was freed -- scan for new maxidx.
	// This means looking through every bit of every chunk below current maxidx
	// until we find a 0 bit. This can be quite time intensive but is 100% accurate.
	// Good if we spend most of our time on operations that are bound by maxidx.
	for (;;) {
		u64 chunk = p->freebm[chunk_idx];
		if (chunk == 0) {
			// chunk is full and next chunk is empty
			// dlog(">> chunk %u is full and next chunk is empty", chunk_idx);
			p->maxidx = (chunk_idx + 1) << 6;
			break;
		} else if (chunk != ~(u64)0) {
			// chunk has at least one 0 bit.
			// Find first 0 bit by first flipping all bits with xor,
			// then use ffs to find the first 1 bit.
			// dlog(">> chunk %u has at least one 0 bit: 0x%016lx", chunk_idx, chunk);
			p->maxidx = (sizeof(chunk) * 8) - dew_clz(~chunk);
			break;
		} else {
			// chunk is completely free
			// dlog(">> chunk %u is completely free", chunk_idx);
			if (chunk_idx == 0) {
				p->maxidx = 0;
				break;
			}
			chunk_idx--;
		}
	}
	// logmsg("maxidx was freed; it's now: %u", p->maxidx);
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
