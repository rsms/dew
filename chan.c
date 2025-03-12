/* inter-thread message queue (MP-MC safe)

Design:

          consumers           producers
        r_tail  r_head      w_tail      w_head
  0   1   │   3   │   5   6   │   8   9   │   11  12  13  14  15
┌───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┐
│ A │ B │ C │ D │ E │ F │ G │ . │ . │ . │   │   │   │   │   │   │
└───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘
  ┄┄┄┄┄   ╰───╯   ╰───────╯   ╰───────╯   ┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄
  free    read in   unread     write in    free (unwritten)
          progress             progress

 total occupied range = w_head - r_tail  (i.e. "C D E F G . . ." above)
 can write            = w_head - r_tail <= cap
 available to write   = cap - (w_head - r_tail)  (Note: mind potential overflow)
 available to read    = w_tail - r_head

*/
#include "dew.h"
#include "chan.h"
#include <stdatomic.h>
API_BEGIN

#define SPIN_YIELD() cpu_yield()
// #include <sched.h>
// #define SPIN_YIELD() sched_yield()


struct Chan { // aligned to CPU_CACHE_LINE_SIZE
    // writer/producer fields
    _Atomic(u32)  w_head;
    _Atomic(u32)  w_tail;
    u32           w_mask;
    _Atomic(bool) w_shutdown;
    TSem          w_sem; // available to write
    _Atomic(u32)  w_sem_value;
    _Atomic(u32)  w_sem_waiters;

    // reader/consumer fields
    _Atomic(u32)  r_head __attribute__((aligned(CPU_CACHE_LINE_SIZE)));
    _Atomic(u32)  r_tail;
    u32           r_mask;
    _Atomic(bool) r_shutdown;
    TSem          r_sem; // available to read
    _Atomic(u32)  r_sem_value;
    _Atomic(u32)  r_sem_waiters;

    // writer & reader fields
    u32 entsize __attribute__((aligned(CPU_CACHE_LINE_SIZE)));
    u8  entries[] __attribute__((aligned(8)));
};


Chan* nullable chan_open(u32 cap, u32 entsize) {
    assertf(cap > 1 && IS_POW2(cap), "cap must be pow2");
    if (cap == 0 || !IS_POW2(cap))
        return NULL;

    usize nbyte = ALIGN2(sizeof(Chan) + ((usize)cap * (usize)entsize), CPU_CACHE_LINE_SIZE);
    Chan* q = aligned_alloc(CPU_CACHE_LINE_SIZE, nbyte);
    if (!q)
        return NULL;

    // Actual capacity may be larger because of cache line alignment of allocation.
    // cap must remain pow2 for mask.
    cap = FLOOR_POW2((nbyte - sizeof(Chan)) / entsize);

    q->w_head = 0;
    q->w_tail = 0;
    q->w_mask = cap - 1;
    q->w_shutdown = false;
    q->w_sem_value = cap - 1;
    q->w_sem_waiters = 0;
    int err = tsem_open(&q->w_sem, 0);
    if UNLIKELY(err) {
        free(q);
        return NULL;
    }

    q->r_tail = 0;
    q->r_head = 0;
    q->r_mask = cap - 1;
    q->r_shutdown = false;
    q->r_sem_value = 0;
    q->r_sem_waiters = 0;
    if UNLIKELY(( err = tsem_open(&q->r_sem, 0) )) {
        tsem_close(&q->w_sem);
        free(q);
        return NULL;
    }

    q->entsize = entsize;

    return q;
}


u32 chan_cap(const Chan* ch) {
    return ch->r_mask;
}


void chan_close(Chan* q) {
    tsem_close(&q->r_sem);
    tsem_close(&q->w_sem);
    free(q);
}


void chan_shutdown(Chan* q) {
    atomic_thread_fence(memory_order_acquire);
    atomic_store_explicit(&q->w_shutdown, true, memory_order_release);
    atomic_store_explicit(&q->r_shutdown, true, memory_order_release);
    atomic_fetch_add(&q->r_sem_value, 0xffff);
    atomic_fetch_add(&q->w_sem_value, 0xffff);
    for (u32 n = atomic_load(&q->r_sem_waiters); n--;)
        tsem_signal(&q->r_sem);
    for (u32 n = atomic_load(&q->w_sem_waiters); n--;)
        tsem_signal(&q->w_sem);
}


static void chan_sem_wait2(TSem* sem, _Atomic(u32)* sem_value, _Atomic(u32)* sem_waiters) {
    atomic_fetch_add(sem_waiters, 1);

    // Check if a resource became available after incrementing waiters
    u32 expected = atomic_load(sem_value);
    if (expected > 0 &&
        atomic_compare_exchange_strong(sem_value, &expected, expected - 1)) {
        atomic_fetch_sub(sem_waiters, 1);
        return;
    }

    // No resource available, wait on OS semaphore
    tsem_wait(sem);
    atomic_fetch_sub(sem_waiters, 1);
}


static void chan_sem_wait(TSem* sem, _Atomic(u32)* sem_value, _Atomic(u32)* sem_waiters) {
    // Fast path: try to decrement if value > 0
    u32 expected = atomic_load(sem_value);
    while LIKELY(expected > 0) {
        if LIKELY(atomic_compare_exchange_weak(sem_value, &expected, expected - 1))
            return;
        // If CAS failed, expected has been updated with the current value
    }
    // Slow path: value is 0 or negative, need to wait
    return chan_sem_wait2(sem, sem_value, sem_waiters);
}


static void chan_sem_signal(TSem* sem, _Atomic(u32)* sem_value, _Atomic(u32)* sem_waiters) {
    assert(atomic_load(sem_value) < U32_MAX);
    atomic_fetch_add(sem_value, 1);
    if (atomic_load(sem_waiters) > 0)
        tsem_signal(sem);
}


ChanTx chan_write_begin(Chan* q) {
    chan_sem_wait(&q->w_sem, &q->w_sem_value, &q->w_sem_waiters);

    if UNLIKELY(atomic_load_explicit(&q->w_shutdown, memory_order_acquire))
        return (ChanTx){};

    u32 w_head = atomic_fetch_add_explicit(&q->w_head, 1, memory_order_acq_rel);
    u32 r_tail = atomic_load_explicit(&q->r_tail, memory_order_acquire);

    while UNLIKELY(w_head - r_tail >= q->w_mask) {
        // queue is full; we lost the race to another thread
        SPIN_YIELD();
        if UNLIKELY(atomic_load_explicit(&q->w_shutdown, memory_order_acquire))
            return (ChanTx){};
        r_tail = atomic_load_explicit(&q->r_tail, memory_order_acquire);
    }

    u32 entry_idx = (w_head & q->w_mask) * q->entsize;
    return (ChanTx){ .entry = &q->entries[entry_idx], .tx = w_head };
}

ChanTx chan_read_begin(Chan* q) {
    chan_sem_wait(&q->r_sem, &q->r_sem_value, &q->r_sem_waiters);

    u32 r_head = atomic_fetch_add_explicit(&q->r_head, 1, memory_order_acq_rel);
    u32 w_tail = atomic_load_explicit(&q->w_tail, memory_order_acquire);

    while UNLIKELY (w_tail - (r_head + 1) >= q->w_mask) {
        // Queue is empty; slot r_head is not yet available to read. Wait for w_tail to advance.
        if UNLIKELY(atomic_load_explicit(&q->r_shutdown, memory_order_acquire))
            return (ChanTx){};
        SPIN_YIELD();
        w_tail = atomic_load_explicit(&q->w_tail, memory_order_acquire);
    }

    u32 entry_idx = (r_head & q->w_mask) * q->entsize;
    return (ChanTx){ .entry = &q->entries[entry_idx], .tx = r_head };
}

static void chan_commit(Chan* q, u32 my_head, _Atomic(u32)* tailp, char op) {
    // Make sure we don't increment tail until other writer threads that started before us
    // have finished. Essentially:
    //   w_tail = MIN(w_tail of each writer)
    //   r_tail = MIN(r_tail of each reader)
    //
    // For example, say thread x and y are racing to write.
    // The initial state, before any thread starts writing, is:
    //
    //         w_tail╷w_head
    //       0   1   │   3   4   5   6
    //     ┌───┬───┬─│─┬───┬───┬───┬───
    //     │ A │ B │   │   │   │   │
    //     └───┴───┴───┴───┴───┴───┴───
    //
    // Thread x wins the race to start writing: x's head is 2, y's head is 3:
    //
    //         w_tail╷       ╷w_head
    //       0   1   │   3   │   5   6
    //     ┌───┬───┬─│─┬───┬─│─┬───┬───
    //     │ A │ B │ . │ . │   │   │
    //     └───┴───┴─╷─┴─╷─┴───┴───┴───
    //               x   y
    //
    // Now, imagine that thread y finished before thread x. We can't just increment w_tail
    // since that would prematurely mark thread x's slot as "ready to read":
    //
    //              w_tail   w_head
    //       0   1   2   │   │   5   6
    //     ┌───┬───┬───┬─│─┬─│─┬───┬───
    //     │ A │ B │ . │ D │   │   │       slot 2 not actually ready!
    //     └───┴───┴───┴───┴───┴───┴───
    //
    // What we have to do is increment w_tail in the sequence the threads started.
    // I.e. thread y must wait for thread x to increment w_tail before y can proceed:
    //
    //         w_tail╷       ╷w_head
    //       0   1   │   3   │   5   6
    //     ┌───┬───┬─│─┬───┬─│─┬───┬───
    //     │ A │ B │ . │ D │   │   │       thread y finished, waiting for thread x to commit
    //     └───┴───┴─╷─┴─╷─┴───┴───┴───
    //               x   y
    //
    //             w_tail╷   ╷w_head
    //       0   1   2   │   │   5   6
    //     ┌───┬───┬───┬─│─┬─│─┬───┬───
    //     │ A │ B │ C │ D │   │   │       thread x commits, advancing w_tail
    //     └───┴───┴───┴─╷─┴───┴───┴───
    //               x   y
    //
    //                 w_tail╷w_head
    //       0   1   2   3   │   5   6
    //     ┌───┬───┬───┬───┬─│─┬───┬───
    //     │ A │ B │ C │ D │   │   │       thread y commits, advancing w_tail
    //     └───┴───┴───┴───┴───┴───┴───
    //               x   y
    //

#if 0
    u32 expect_tail = my_head;
    u32 new_tail = my_head + 1;
    while (!atomic_compare_exchange_weak_explicit(
                tailp, &expect_tail, new_tail, memory_order_release, memory_order_relaxed))
    {
        // put this thread at the end of the scheduler run qeueue
        SPIN_YIELD();
        expect_tail = new_tail-1; // CAS operation updates expect_tail
    }
#else
    while (atomic_load_explicit(tailp, memory_order_acquire) != my_head)
        SPIN_YIELD();
    atomic_store_explicit(tailp, my_head+1, memory_order_release);
    // atomic_fetch_add_explicit(tailp, 1, memory_order_release);
#endif
}

void chan_write_commit(Chan* q, ChanTx tx) {
    chan_commit(q, tx.tx, &q->w_tail, 'w');
    chan_sem_signal(&q->r_sem, &q->r_sem_value, &q->r_sem_waiters);
}

void chan_read_commit(Chan* q, ChanTx tx) {
    chan_commit(q, tx.tx, &q->r_tail, 'r');
    chan_sem_signal(&q->w_sem, &q->w_sem_value, &q->w_sem_waiters);
}

bool chan_write(Chan* q, const void* value_src) {
    ChanTx tx = chan_write_begin(q);
    if (tx.entry == NULL)
        return false;
    memcpy(tx.entry, value_src, q->entsize);
    chan_write_commit(q, tx);
    return true;
}

bool chan_read(Chan* q, void* value_dst) {
    ChanTx tx = chan_read_begin(q);
    if (tx.entry == NULL)
        return false;
    memcpy(value_dst, tx.entry, q->entsize);
    chan_read_commit(q, tx);
    return true;
}

API_END
