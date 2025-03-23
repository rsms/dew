// thread semaphore
#pragma once
#include "../dew.h"

#if defined(WIN32) || defined(__MACH__)
    typedef uintptr TSem;
#else
    #include <semaphore.h>
    typedef sem_t TSem;
#endif

API_BEGIN

int  tsem_open(TSem*, u32 initcount);          // -errno on failure
void tsem_close(TSem*);                        // no thread must be tsem_wait-ing
void tsem_signal(TSem*);                       // increment
void tsem_wait(TSem*);                         // decrement
bool tsem_trywait(TSem*);                      // true if decremented, false if would block
bool tsem_timedwait(TSem*, u64 timeout_usecs); // true if decremented, false on timeout

API_END
