#include "dew.h"


static bool fifo_allocsize(u32 cap, usize elemsize, usize* nbyte_out) {
	return !check_mul_overflow((usize)cap, elemsize, nbyte_out) &&
	       !check_add_overflow(*nbyte_out, sizeof(FIFO), nbyte_out);
}


FIFO* nullable fifo_alloc(u32 cap, usize elemsize) {
	FIFO* q;
	usize newsize;
	if (!fifo_allocsize(cap, elemsize, &newsize))
		return NULL;
	if (( q = malloc(newsize) )) {
		q->cap = cap;
		q->head = 0;
		q->tail = 0;
	}
	return q;
}


static int _fifo_grow(FIFO** qp, usize elemsize, u32 maxcap) {
	FIFO* q = *qp;
	if (q->cap >= maxcap)
		return -1;

	// try to double capacity, fall back to incrementing it by 1
	u32 newcap;
	if (check_mul_overflow(q->cap, 2u, &newcap)) {
		if (q->cap == U32_MAX)
			return -1;
		newcap = q->cap + 1;
	}

	// limit capacity to maxcap
	if (newcap > maxcap)
		newcap = maxcap;

	// calculate new memory allocation size
	usize newsize;
	if (!fifo_allocsize(newcap, elemsize, &newsize))
		return -1;

	// realloc
	FIFO* newq = realloc(q, newsize);
	if (!newq)
		return -1;
	q = newq;

	// check for wrapping head & tail pointers
	if (q->head > q->tail) {
		u32 tailcount = q->cap - q->head;
		u32 new_head = q->head + (newcap - q->cap);
		void* entries = (void*)q + ALIGN2(sizeof(*q), elemsize);
		void* dst = entries + (usize)new_head*elemsize;
		void* src = entries + (usize)q->head*elemsize;
		memmove(dst, src, tailcount*elemsize);
		q->head = new_head;
	}

	q->cap = newcap;
	*qp = q;

	return 0;
}


void* nullable fifo_push(FIFO** qp, usize elemsize, u32 maxcap) {
	FIFO* q = *qp;
	u32 newtail = (q->tail + 1) % q->cap;
	if UNLIKELY(newtail == q->head) {
		if UNLIKELY(_fifo_grow(qp, elemsize, maxcap))
			return NULL;
		q = *qp;
		newtail = (q->tail + 1) % q->cap;
	}
	void* entries = (void*)q + ALIGN2(sizeof(*q), elemsize);
	void* entry = entries + (usize)q->tail*elemsize;
	q->tail = newtail;
	return entry;
}


void* nullable fifo_pop(FIFO* q, usize elemsize) {
	if (q->head == q->tail) // empty
		return NULL;
	void* entries = (void*)q + ALIGN2(sizeof(*q), elemsize);
	void* entry = entries + (usize)q->head*elemsize;
	q->head = (q->head + 1) % q->cap;
	return entry;
}


// little unit test for FIFO
#ifdef DEBUG
__attribute__((constructor)) static void fifo_test() {
	struct { FIFO fifo; u32 entries[]; }* q;
	q = (__typeof__(q))fifo_alloc(4, sizeof(*q->entries));
	assert(q != NULL);
	assert(q->fifo.cap == 4);

	// note: actual capacity is cap-1; set maxcap to 2x to ensure _fifo_grow is tested
	u32 maxcap = q->fifo.cap*2;
	u32* entry;
	for (u32 i = 0; i < maxcap-1; i++) {
		entry = fifo_push((FIFO**)&q, sizeof(*q->entries), maxcap);
		assert(entry != NULL);
		*entry = i;
		// dlog("entry #%u %p", i+1, entry);
	}
	// this should fail as we are at maxcap
	entry = fifo_push((FIFO**)&q, sizeof(*q->entries), maxcap);
	assert(entry == NULL);

	// should dequeue in order
	for (u32 i = 0; i < maxcap-1; i++) {
		entry = fifo_pop(&q->fifo, sizeof(*q->entries));
		assert(entry != NULL);
		assert(*entry == i);
	}
	// should be empty now
	entry = fifo_pop(&q->fifo, sizeof(*q->entries));
	assert(entry == NULL);

	// should have room for maxap-1 more entries now without needing to grow in size
	assert(q->fifo.cap == maxcap);
	for (u32 i = 0; i < maxcap-1; i++)
		fifo_push((FIFO**)&q, sizeof(*q->entries), maxcap);
	assert(q->fifo.cap == maxcap); // ensure it did not grow

	dlog("OK: fifo_test");
}
#endif
