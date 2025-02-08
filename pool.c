#include "dew.h"


typedef __typeof__(((Pool*)NULL)->freebm[0]) BitChunk;
enum { CHUNKBITS = sizeof(BitChunk)*8 };


static bool pool_grow(Pool** pp, u32 newcap, usize elemsize) {
	Pool* p = *pp;

	// TODO: This growth heuristic can be improved to align up to cache lines or page sizes.
	// Currently growth often looks like this:
	//   pool_grow (cap 256 -> 512) total 4164 B (entries 4096 B, freebm 64 B)
	//

	if (p && p->cap > 0) {
		if (check_mul_overflow(p->cap, 2u, &newcap)) {
			if (p->cap == U32_MAX)
				return false;
			newcap = p->cap + 1;
		}
	}

	usize newsize;
	usize newsize_1, newsize_2;
	usize newnfreebm = IDIV_CEIL((usize)newcap, CHUNKBITS);
	if (check_mul_overflow(elemsize, (usize)newcap, &newsize_1) ||
	    check_mul_overflow(sizeof(BitChunk), newnfreebm, &newsize_2) ||
	    check_add_overflow(newsize_1, newsize_2, &newsize) ||
	    check_add_overflow(sizeof(Pool), newsize, &newsize) )
	{
		return false;
	}

	// dlog("pool_grow (cap %u -> %u) total %zu B (entries %zu B, freebm %zu B)",
	//      p ? p->cap : 0, newcap, newsize, newsize_1, newsize_2);

	if (p == NULL) {
		p = malloc(newsize);
		if UNLIKELY(!p) {
			dlog("malloc failed");
			return false;
		}
		*pp = p;
		p->maxidx = 0;
		memset(p->freebm, 0xff, newnfreebm*sizeof(BitChunk));
	} else {
		usize oldcap = p->cap;
		usize oldnfreebm = IDIV_CEIL(oldcap, CHUNKBITS);
		Pool* newp = realloc(p, newsize);
		if UNLIKELY(!newp) {
			dlog("realloc failed");
			return false;
		}
		*pp = newp;
		p = newp;
		if (newnfreebm > oldnfreebm) {
			// new freebm will overlap with existing entries
			usize extra_freebm_size = (newnfreebm - oldnfreebm) * sizeof(BitChunk);
			// move up entries to make room for extended freebm
			memmove(p->freebm + newnfreebm, p->freebm + oldnfreebm, p->maxidx * elemsize);
			// set all bits to 1 of new freebm slots (1 bit = "free")
			memset(&newp->freebm[oldnfreebm], 0xff, extra_freebm_size);
		}
	}

	p->cap = newcap;
	return true;
}


bool pool_init(Pool** pp, u32 cap, usize elemsize) {
	*pp = NULL;
	return pool_grow(pp, cap, elemsize);
}


void* nullable pool_entry_alloc(Pool** pp, u32* idxp, usize elemsize) {
	Pool* p;
	u32 nchunks;

	if UNLIKELY(*pp == NULL)
		goto grow;

again:
	p = *pp;
	nchunks = pool_freebm_chunks(p);
	for (u32 chunk_idx = 0; chunk_idx < nchunks; chunk_idx++) {
		BitChunk bm = p->freebm[chunk_idx];

		// ffs effectively returns the lowest free buffer id, if any, in this chunk
		u32 bit_idx = dew_ffs(bm);

		// note: we don't do bit_idx-1 since idx is 1 based, not 0 based
		u32 idx = chunk_idx*CHUNKBITS + bit_idx;

		if (bit_idx && idx <= p->cap) {
			// found a free block; mark it as used by setting bit (bit_idx-1) to 0
			// dlog(">>> allocate bit %u in chunk %u (idx = %u)", bit_idx-1, chunk_idx, idx);
			p->freebm[chunk_idx] = bm & ~((BitChunk)1 << (bit_idx - 1));
			*idxp = idx;
			if (idx > p->maxidx)
				p->maxidx = idx;
			return pool_entry(p, idx, elemsize);
		}
		// all blocks of this chunk are in use; try next chunk
	}
	// all chunks were occupied; grow pool

grow:
	if (!pool_grow(pp, 32, elemsize))
		return NULL;
	goto again;
}


void pool_entry_free(Pool* p, u32 idx) {
	assert(pool_entry_islive(p, idx));

	u32 chunk_idx = (idx - 1) / CHUNKBITS;
	u32 bit_idx = (idx - 1) % CHUNKBITS;
	// dlog(">>> recycle bit %u in chunk %u (idx = %u)", bit_idx, chunk_idx, idx);
	p->freebm[chunk_idx] |= ((BitChunk)1 << bit_idx);

	if (idx == p->maxidx)
		// TODO: if it would benefit the common case, we could scan backwards for the first
		// occupied (0 bit) slot and set maxidx correctly. Can use __builtin_ctz for this.
		p->maxidx--;
}


#ifdef DEBUG
__attribute__((constructor)) static void pool_test() {
	Pool* p;
	bool ok = pool_init(&p, /*cap*/3, sizeof(u64)); assert(ok);

	u32 N = 200;

	// allocate dense range
	for (u32 idx = 1; idx <= N; idx++) {
		u32 idx2;
		u64* vp = pool_entry_alloc(&p, &idx2, sizeof(u64));
		assert(vp);
		assert(idx == idx2); // expect dense sequential index
		*vp = idx;
		// dlog("%u, %u", idx, idx2);
	}

	// verify
	for (u32 idx = 1; idx <= N; idx++) {
		assert(pool_entry_islive(p, idx));
		u64* vp = pool_entry(p, idx, sizeof(u64));
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
			assert(!pool_entry_islive(p, idx));
		} else {
			assert(pool_entry_islive(p, idx));
			u64* vp = pool_entry(p, idx, sizeof(u64));
			assert(*vp == idx);
		}
	}

	// re-allocate free'd slots
	for (u32 idx = 1; idx <= N; idx++) {
		if (idx % 4 == 3) {
			u32 idx2;
			u64* vp = pool_entry_alloc(&p, &idx2, sizeof(u64));
			assert(vp);
			assert(idx == idx2); // expect dense sequential index
			*vp = idx;
		}
	}

	// verify
	for (u32 idx = 1; idx <= N; idx++) {
		assert(pool_entry_islive(p, idx));
		u64* vp = pool_entry(p, idx, sizeof(u64));
		assert(*vp == idx);
	}

	pool_free_pool(p);
	dlog("OK: %s", __FUNCTION__);
}
#endif // DEBUG

