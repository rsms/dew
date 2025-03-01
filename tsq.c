// Thread submission queue, FIFO usable by single-producer-multiple-consumers
#include "dew.h"
#include <stdatomic.h>


TSQ* nullable tsq_open(u32 entsize, u32 cap) {
	usize nbyte = sizeof(TSQ) + cap*entsize;
	nbyte += CPU_CACHE_LINE_SIZE - (nbyte % CPU_CACHE_LINE_SIZE);
	TSQ* q = aligned_alloc(CPU_CACHE_LINE_SIZE, nbyte);

	// Actual capacity may be larger because of cache line alignment of allocation.
	cap = (nbyte - sizeof(TSQ)) / entsize;

	if (!q)
		return NULL;

	atomic_store_explicit(&q->r, 0, memory_order_relaxed);
	q->w = 0;
	q->cap = cap;
	q->mask = cap - 1;
	q->entsize = entsize;

	int err = tsem2_open(&q->rsem, &q->rsem_count, 0);
	if (err) {
		fprintf(stderr, "tsem2_open failed\n");
		free(q);
		return NULL;
	}

	err = tsem2_open(&q->wsem, &q->wsem_count, cap);
	if (err) {
		fprintf(stderr, "tsem2_open failed\n");
		tsem2_close(&q->rsem, &q->rsem_count);
		free(q);
		return NULL;
	}

	atomic_thread_fence(memory_order_release);
	return q;
}


void tsq_close(TSQ* nullable q) {
	if (!q)
		return;
	tsem2_close(&q->rsem, &q->rsem_count);
	tsem2_close(&q->wsem, &q->wsem_count);
	free(q);
}


void tsq_close_reader(TSQ* q) {
	tsem2_softclose(&q->rsem, &q->rsem_count);
}


void tsq_close_writer(TSQ* q) {
	tsem2_softclose(&q->wsem, &q->wsem_count);
}


void* nullable tsq_write(TSQ* q) {
	if (!tsem2_wait(&q->wsem, &q->wsem_count))
		return NULL; // queue closed

	u32 next_w = q->w + 1;

	// ensure that there's room for at least one entry
	assert(next_w - atomic_load_explicit(&q->r, memory_order_acquire) <= q->cap);

	// return a pointer to the entry and advance w
	u32 entry_idx = q->w & q->mask;
	q->w = next_w;
	return (void*)q->entries + q->entsize*entry_idx;
}


void* nullable tsq_read(TSQ* q) {
	if (!tsem2_wait(&q->rsem, &q->rsem_count))
		return NULL; // queue closed
	u32 r = atomic_fetch_add_explicit(&q->r, 1, memory_order_acq_rel);
	u32 entry_idx = r & q->mask;
	return (void*)q->entries + q->entsize*entry_idx;
}


// bool tsq_is_full(TSQ* q) {
// 	u32 r = atomic_load_explicit(&q->r, memory_order_acquire);
// 	return ((q->w + 1) - r > q->cap);
// }
