#include "timer.h"


// timers_dlog prints the state of a 'timers' priority queue via dlog
#if !defined(DEBUG)
    #define timers_dlog(timers) ((void)0)
#else
static void timers_dlog(const TimerPQ* timers_readonly) {
    u32 n = timers_readonly->len;
    dlog("s.timers: %u", n);
    if (n == 0)
        return;

    // copy timers
    usize nbyte = sizeof(*timers_readonly->v) * n;
    void* v_copy = malloc(nbyte);
    if (!v_copy) {
        dlog("  (malloc failed)");
        return;
    }
    memcpy(v_copy, timers_readonly->v, nbyte);
    TimerPQ timers = { .cap = n, .len = n, .v = v_copy };

    for (u32 i = 0; timers.len > 0; i++) {
        Timer* timer = timers_remove_min(&timers);
        dlog("  [%u] %10lu %p", i, timer->when, timer);
    }

    free(v_copy);
}
#endif


static u32 timers_sift_up(TimerPQ* timers, u32 i) {
    TimerInfo last = timers->v[i];
    while (i > 0) {
        u32 parent = (i - 1) / 2;
        if (last.when >= timers->v[parent].when)
            break;
        timers->v[i] = timers->v[parent];
        i = parent;
    }
    timers->v[i] = last;
    return i;
}


static void timers_sift_down(TimerPQ* timers, u32 i) {
    u32 len = timers->len;
    TimerInfo last = timers->v[len];
    for (;;) {
        u32 left = i*2 + 1;
        if (left >= len) // no left child; this is a leaf
            break;

        u32 child = left;
        u32 right = left + 1;

        // if right hand-side timer has smaller 'when', use that instead of left child
        if (right < len && timers->v[right].when < timers->v[left].when)
            child = right;

        if (timers->v[child].when >= last.when)
            break;
        // move the child up
        timers->v[i] = timers->v[child];
        i = child;
    }
    timers->v[i] = last;
}


Timer* timers_remove_min(TimerPQ* timers) {
    assert(timers->len > 0);
    Timer* timer = timers->v[0].timer;
    u32 len = --timers->len;
    if (len == 0) {
        // Note: min 'when' changed to nothing (no more timers)
    } else {
        timers_sift_down(timers, 0);
        // Note: min 'when' changed to timers->v[0].when
    }
    return timer;
}


void timers_remove_at(TimerPQ* timers, u32 i) {
    // dlog("remove timers[%u] %p (timers.len=%u)", i, timers->v[i].timer, timers->len);
    // timers_dlog(timers); // state before
    timers->len--;
    // if i is the last entry (i.e. latest 'when'), we don't need to do anything else
    if (i == timers->len)
        return;
    if (i > 0 && timers->v[timers->len].when < timers->v[/*parent*/(i-1)/2].when) {
        timers_sift_up(timers, i);
    } else {
        timers_sift_down(timers, i);
    }
    // timers_dlog(timers); // state after
}


void timers_remove(TimerPQ* timers, Timer* timer) {
    if (timers->len == 0 || timer->when == (DTime)-1)
        return;
    // linear search to find index of timer
    u32 i;
    if (timer->when > timers->v[timers->len / 2].when) {
        for (i = timers->len; i--;) {
            if (timers->v[i].timer == timer)
                return timers_remove_at(timers, i);
        }
    } else {
        for (i = 0; i < timers->len; i++) {
            if (timers->v[i].timer == timer)
                return timers_remove_at(timers, i);
        }
    }
    dlog("warning: timer %p not found!", timer);
}


bool timers_add(TimerPQ* timers, Timer* timer) {
    // append the timer to the priority queue (heap)
    TimerInfo* ti = array_reserve((struct Array*)timers, sizeof(*timers->v), 1);
    if (!ti) // could not grow array; out of memory
        return false;
    timers->len++;
    if (timer->when == (DTime)-1) timer->when--; // uphold special meaning of -1
    DTime when = timer->when;
    ti->timer = timer;
    ti->when = when;

    // "sift up" heap sort
    u32 i = timers->len - 1;
    i = timers_sift_up(timers, i);
    if (i == 0) {
        // Note: 'min when' changed to 'when'
    }
    // timers_dlog(timers);
    return true;
}
