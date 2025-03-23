#pragma once
#include "../dew.h"
#include "array.h"
#include "time.h"
API_BEGIN

typedef struct Timer     Timer;
typedef struct TimerInfo TimerInfo; // cache friendly 'timers' array entry
typedef Array(TimerInfo) TimerPQ;

typedef T* nullable (*TimerF)(Timer* timer, void* nullable arg);

struct Timer {
    DTime          when;   // a specific point in time. -1 if dead (not in s.timers)
    DTimeDuration  period; // if >0, repeat every when+period
    DTimeDuration  leeway; // precision request; how much this timer is willing to fluctuate
    u8             nrefs;  // reference count (internal + lua, so really just 2 bits needed)
    void* nullable arg;    // passed to f
    TimerF         f;      // f can return non-NULL to have a T woken up
};

struct TimerInfo {
    Timer* timer;
    DTime  when;
};

bool   timers_add(TimerPQ* timers, Timer* timer);
void   timers_remove(TimerPQ* timers, Timer* timer);
Timer* timers_remove_min(TimerPQ* timers); // removes the timer with the soonest 'when' time
void   timers_remove_at(TimerPQ* timers, u32 i);

inline static void timer_release(Timer* timer) {
    assert(timer->nrefs > 0);
    if (--timer->nrefs == 0) {
        // dlog("*** free timer %p ***", timer);
        free(timer);
    }
}

API_END
