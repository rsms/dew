#include "chan.c"

#define NUM_PRODUCER_THREADS 2
#define NUM_CONSUMER_THREADS 3
#define OPS_PER_PRODUCER 200000

Chan* nullable queue_create() {
    u32 cap = 64;
    u32 entsize = sizeof(int);
    return chan_open(cap, entsize);
}

bool queue_enqueue(Chan* q, int value) {
    return chan_write(q, &value);
}

bool queue_dequeue(Chan* q, int* value_out) {
    return chan_read(q, value_out);
}

#define HAS_QUEUE_CLOSE_ENQUEUE
static void queue_close_enqueue(Chan* q) {
    // called when all messages have been written
    u32 nwritten = atomic_load_explicit(&q->w_tail, memory_order_relaxed);
    u32 nread = atomic_load_explicit(&q->r_tail, memory_order_relaxed);
    // printf("chan_shutdown; wrote %u entries (read %u entries so far)\n", nwritten, nread);
    chan_shutdown(q);
}

void queue_destroy(Chan* q) {
    chan_close(q);
}

#define Queue Chan

#define QUEUE_ENQUEUE_IS_BLOCKING
#define QUEUE_DEQUEUE_IS_BLOCKING

// #define VALTYPE    void*
// #define VALMAKE(i) (void*)(uintptr_t)(i)

//———————————————————————————————————————————————————

// for test program
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>
#include <sched.h>

#if defined(__APPLE__) && !defined(NO_APPLE_INCLUDES)
    #undef panic
    #include <sys/sysctl.h>
    #include <mach/mach.h>
    #include <mach/mach_time.h>
    #include <mach/thread_policy.h>
    #include <mach/thread_act.h>
    #undef MIN
    #undef MAX
#endif

#ifndef NUM_PRODUCER_THREADS
    #define NUM_PRODUCER_THREADS 3
#endif
#ifndef NUM_CONSUMER_THREADS
    #define NUM_CONSUMER_THREADS 1
#endif
#ifndef OPS_PER_PRODUCER
    #define OPS_PER_PRODUCER 1000
#endif
// #define ENABLE_RANDOM_PRODUCER_DELAYS
// #define ENABLE_RANDOM_CONSUMER_DELAYS
// #define QUEUE_ENQUEUE_IS_BLOCKING

#ifndef VALTYPE
#define VALTYPE int
#endif

#ifndef VALMAKE
#define VALMAKE(i) (i)
#endif

typedef uint64_t DTime;         // monotonic high-resolution clock with undefined base
typedef int64_t  DTimeDuration; // duration, like 134ms or -1.2h

#define D_TIME_HOUR        ((DTimeDuration)3600e9)
#define D_TIME_MINUTE      ((DTimeDuration)60e9)
#define D_TIME_SECOND      ((DTimeDuration)1e9)
#define D_TIME_MILLISECOND ((DTimeDuration)1e6)
#define D_TIME_MICROSECOND ((DTimeDuration)1e3)
#define D_TIME_NANOSECOND  ((DTimeDuration)1)

#if defined(__linux__) || defined(__wasm__)
    DTime DTimeNow() {
        struct timespec ts;
        int r = clock_gettime(CLOCK_MONOTONIC, &ts);
        if (r != 0)
            return 0;
        return ((uint64_t)(ts.tv_sec) * 1000000000ull) + ts.tv_nsec;
    }
#elif defined(__APPLE__)
    // fraction to multiply a value in mach tick units with to convert it to nanoseconds
    static mach_timebase_info_data_t g_tbase;
    __attribute__((constructor)) static void time_init() {
        mach_timebase_info(&g_tbase);
    }
    DTime DTimeNow() {
        uint64_t t = mach_absolute_time();
        return (t * g_tbase.numer) / g_tbase.denom;
    }
#endif

DTimeDuration DTimeBetween(DTime a, DTime b) {
    DTime d = a - b;
    return (DTimeDuration)(d < 0 ? -d : d);
}

const char* DTimeDurationFormat(DTimeDuration d, char buf[26]) {
    // max value: "18446744073709551615.9min\0"
    double ns = (double)d;
    if (ns < 1000.0)             { sprintf(buf, "%.0f ns",  ns); }
    else if (ns < 1000000.0)     { sprintf(buf, "%.1f us",  ns / 1000.0); }
    else if (ns < 1000000000.0)  { sprintf(buf, "%.1f ms",  ns / 1000000.0); }
    else if (ns < 60000000000.0) { sprintf(buf, "%.1f s",   ns / 1000000000.0); }
    else                         { sprintf(buf, "%.1f min", ns / 60000000000.0); }
    return buf;
}

// Shared state for coordinating shutdown
typedef struct {
    Queue* queue;
    _Atomic int producers_done;
    _Atomic int consumers_done;
    _Atomic int items_produced;
    _Atomic int items_consumed;
} SharedState;

// Thread arguments
typedef struct {
    SharedState* shared;
    int thread_id;
    int cpu_id;
} ThreadArg;


void random_sleep() {
    struct timespec ts = {0};
    // random duration in range [100-100000000] ns
    unsigned long ns = (rand() % 99999901) + 100;  // 99999901 = 100000000 - 100 + 1
    ts.tv_sec = ns / 1000000000UL;
    ts.tv_nsec = ns % 1000000000UL;
    nanosleep(&ts, NULL);
    // ignore interrupts
}


static void set_max_priority() {
#ifdef __APPLE__
    mach_port_t thread = mach_thread_self();

    // Set real-time constraints for maximum priority
    thread_time_constraint_policy_data_t policy;
    policy.period = 1000000;        // Computation time allotment (1ms)
    policy.computation = 999000;    // Computation time within period
    policy.constraint = 1000000;    // Maximum time between periods
    policy.preemptible = 0;         // Non-preemptible thread

    kern_return_t ret = thread_policy_set(
        thread,
        THREAD_TIME_CONSTRAINT_POLICY,
        (thread_policy_t)&policy,
        THREAD_TIME_CONSTRAINT_POLICY_COUNT
    );

    if (ret != KERN_SUCCESS) {
        fprintf(stderr, "warning: Failed to set thread policy: %d\n", ret);
        return;
    }

    // Set thread to maximum priority using macOS-specific QoS
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}


static void set_thread_affinity(int core_id) {
    #ifdef __linux__
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);
        if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset))
            fprintf(stderr, "pthread_setaffinity_np failed: %s\n", strerror(errno));
    #elif defined(__APPLE__)
        mach_port_t mach_thread = pthread_mach_thread_np(pthread_self());

        // Try extended policy first
        thread_extended_policy_data_t extended_policy;
        extended_policy.timeshare = 0; // Set to non-timeshare to increase likelihood of staying on one core

        kern_return_t ret = thread_policy_set(
            mach_thread,
            THREAD_EXTENDED_POLICY,
            (thread_policy_t)&extended_policy,
            THREAD_EXTENDED_POLICY_COUNT
        );

        if (ret != KERN_SUCCESS) {
            fprintf(stderr, "Warning: Could not set extended thread policy on macOS: %d\n", ret);
        }

        // Then set affinity tag
        thread_affinity_policy_data_t affinity_policy;
        affinity_policy.affinity_tag = core_id + 1; // tag must be non-zero

        ret = thread_policy_set(
            mach_thread,
            THREAD_AFFINITY_POLICY,
            (thread_policy_t)&affinity_policy,
            THREAD_AFFINITY_POLICY_COUNT
        );

        #ifndef NDEBUG
        if (ret != KERN_SUCCESS) {
            //fprintf(stderr, "Warning: Could not set thread affinity on macOS: %d\n", ret);
            // Continue anyway - affinity is just a hint on macOS
        }
        #endif
    #endif
}


// Producer thread
void* producer_thread(void* arg) {
    ThreadArg* targ = (ThreadArg*)arg;
    SharedState* shared = targ->shared;

    set_max_priority();

    #ifdef __APPLE__
        set_thread_affinity(targ->cpu_id);
    #endif

    for (int i = 0; i < OPS_PER_PRODUCER; ) {
        // Create unique values that encode producer ID and sequence
        VALTYPE value = VALMAKE((targ->thread_id * OPS_PER_PRODUCER) + i);
        // printf("produce %d\n", value);
        #ifdef QUEUE_ENQUEUE_IS_BLOCKING
            if (!queue_enqueue(shared->queue, value)) {
                #if !defined(HAS_QUEUE_CLOSE_DEQUEUE)
                    fprintf(stderr, "queue_enqueue failed; exiting\n");
                #endif
                break;
            }
            atomic_fetch_add(&shared->items_produced, 1);
        #else
            u32 attempts = 1;
            const u32 many_attempts = 100000;
            for (;;) {
                if (queue_enqueue(shared->queue, value)) {
                    atomic_fetch_add(&shared->items_produced, 1);
                    break;
                }
                sched_yield();
                attempts++;
                if (attempts % many_attempts == 0)
                    printf("producer: queue_enqueue attempt %u\n", attempts);
            }
            if (attempts >= many_attempts)
                printf("producer: queue_enqueue OK\n");
        #endif
        #ifdef ENABLE_RANDOM_PRODUCER_DELAYS
            random_sleep();
        #endif
        i++;
    }

    printf("producer %d done\n", targ->thread_id);

    // If this was the last producer to finish, mark producers as done
    if (atomic_fetch_add(&shared->producers_done, 1)+1 == NUM_PRODUCER_THREADS) {
        printf("all producers done\n");
        #ifdef HAS_QUEUE_CLOSE_ENQUEUE
            queue_close_enqueue(shared->queue);
        #endif
    }

    return NULL;
}

// Consumer thread
void* consumer_thread(void* arg) {
    ThreadArg* targ = (ThreadArg*)arg;
    SharedState* shared = targ->shared;
    VALTYPE value;

    set_max_priority();

    #ifdef __APPLE__
        set_thread_affinity(targ->cpu_id);
    #endif

    while (1) {
        // printf("consume\n");
        if (atomic_load(&shared->producers_done) == NUM_PRODUCER_THREADS &&
            atomic_load(&shared->items_consumed) == atomic_load(&shared->items_produced))
        {
            // we are done
            break;
        }
        if (queue_dequeue(shared->queue, &value)) {
            uint32_t items_consumed = atomic_fetch_add(&shared->items_consumed, 1) + 1;
            // printf("consumed %d entries\n", items_consumed);
            #ifdef ENABLE_RANDOM_CONSUMER_DELAYS
                random_sleep();
            #endif
        } else {
            #ifdef QUEUE_DEQUEUE_IS_BLOCKING
                #ifdef HAS_QUEUE_CLOSE_ENQUEUE
                    //queue_close_enqueue(shared->queue);
                #else
                    fprintf(stderr, "queue_dequeue failed; exiting\n");
                #endif
                break;
            #else
                // reduce contention
                sched_yield();
            #endif
        }
    }

    printf("consumer %d done\n", targ->thread_id);

    if (atomic_fetch_add(&shared->consumers_done, 1)+1 == NUM_CONSUMER_THREADS) {
        printf("all consumers done\n");
        #ifdef HAS_QUEUE_CLOSE_DEQUEUE
            queue_close_dequeue(shared->queue);
        #endif
    }

    return NULL;
}

int get_physical_core_count() {
#ifdef __linux__
    FILE* fp = fopen("/proc/cpuinfo", "r");
    if (fp == NULL) {
        perror("Error opening /proc/cpuinfo");
        return -1;
    }

    char line[256];
    int physical_id = -1;
    int core_id = -1;
    int physical_cores = 0;
    int seen_cores[256][256] = {0}; // Assumes max 256 physical CPUs and 256 cores per CPU

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "physical id", 11) == 0) {
            sscanf(line, "physical id : %d", &physical_id);
        }
        else if (strncmp(line, "core id", 7) == 0) {
            sscanf(line, "core id : %d", &core_id);

            if (physical_id != -1 && core_id != -1) {
                if (!seen_cores[physical_id][core_id]) {
                    seen_cores[physical_id][core_id] = 1;
                    physical_cores++;
                }
                physical_id = core_id = -1;
            }
        }
    }

    fclose(fp);
    return physical_cores;

#elif defined(__APPLE__)
    int physical_cores;
    size_t len = sizeof(physical_cores);
    if (sysctlbyname("hw.physicalcpu", &physical_cores, &len, NULL, 0) == 0) {
        return physical_cores;
    }
    return -1;
#else
    #error "Unsupported platform"
#endif
}


// Get mapping of physical cores to their CPU IDs
void get_physical_core_map(int* core_map, int max_cores) {
#ifdef __linux__
    FILE* fp = fopen("/proc/cpuinfo", "r");
    if (fp == NULL) {
        perror("Error opening /proc/cpuinfo");
        return;
    }

    char line[256];
    int processor = -1;
    int physical_id = -1;
    int core_id = -1;
    int core_count = 0;
    int seen_cores[256][256] = {0};

    while (fgets(line, sizeof(line), fp) && core_count < max_cores) {
        if (strncmp(line, "processor", 9) == 0) {
            sscanf(line, "processor : %d", &processor);
        }
        else if (strncmp(line, "physical id", 11) == 0) {
            sscanf(line, "physical id : %d", &physical_id);
        }
        else if (strncmp(line, "core id", 7) == 0) {
            sscanf(line, "core id : %d", &core_id);

            if (processor != -1 && physical_id != -1 && core_id != -1) {
                if (!seen_cores[physical_id][core_id]) {
                    seen_cores[physical_id][core_id] = 1;
                    core_map[core_count++] = processor;
                }
                processor = physical_id = core_id = -1;
            }
        }
    }

    fclose(fp);

#elif defined(__APPLE__)
    // On macOS, physical cores are numbered sequentially from 0
    for (int i = 0; i < max_cores; i++) {
        core_map[i] = i;
    }
#endif
}


int main() {
    // Initialize shared state
    SharedState shared = {
        .queue = queue_create(),
        .producers_done = false,
        .consumers_done = 0,
        .items_produced = 0,
        .items_consumed = 0
    };
    assert(shared.queue != NULL);

    set_max_priority();
    srand(time(NULL));

    // Create thread arguments
    ThreadArg producer_args[NUM_PRODUCER_THREADS];
    ThreadArg consumer_args[NUM_CONSUMER_THREADS];
    pthread_t producer_threads[NUM_PRODUCER_THREADS];
    pthread_t consumer_threads[NUM_CONSUMER_THREADS];

    // check cpu availabilty for pinning.
    // Note: sysconf(_SC_NPROCESSORS_ONLN) returns logical cores (incl hyperthreading),
    // which could be an alternative.
    int num_cores = get_physical_core_count();
    int num_threads = NUM_CONSUMER_THREADS + NUM_PRODUCER_THREADS;
    printf("%d CPU cores, %d producer thread(s), %d consumer thread(s), %d msg per producer\n",
           num_cores, NUM_PRODUCER_THREADS, NUM_CONSUMER_THREADS, OPS_PER_PRODUCER);
    if (NUM_CONSUMER_THREADS + NUM_PRODUCER_THREADS > num_cores)
        printf("Warning: More threads than physical cores available\n");

    int* core_map = malloc(num_cores * sizeof(int));
    if (core_map == NULL) {
        perror("malloc");
        return EXIT_FAILURE;
    }
    get_physical_core_map(core_map, num_cores);
    // for (int i = 0; i < num_cores; i++)
    //     printf("  CPU %-3d %d\n", i, core_map[i]);


    #ifdef __linux__
        pthread_attr_t attrs[NUM_CONSUMER_THREADS + NUM_PRODUCER_THREADS];
        for (int i = 0; i < NUM_CONSUMER_THREADS + NUM_PRODUCER_THREADS; i++) {
            pthread_attr_init(&attrs[i]);
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(consumer_args[i].cpu_id, &cpuset);
            int rc = pthread_attr_setaffinity_np(&attrs[i], sizeof(cpu_set_t), &cpuset);
            if (rc != 0) {
                fprintf(stderr, "Error setting thread affinity attribute: %s\n", strerror(rc));
                continue;
            }
        }
    #endif

    // Start threads
    int tidx = 0;
    for (int i = 0; i < NUM_CONSUMER_THREADS; i++, tidx++) {
        consumer_args[i].shared = &shared;
        consumer_args[i].thread_id = i;
        consumer_args[i].cpu_id = core_map[tidx++ % num_cores];
        #ifdef __linux__
            pthread_create(&consumer_threads[i], &attrs[tidx], consumer_thread, &consumer_args[i]);
        #else
            pthread_create(&consumer_threads[i], NULL, consumer_thread, &consumer_args[i]);
        #endif
    }
    for (int i = 0; i < NUM_PRODUCER_THREADS; i++, tidx++) {
        producer_args[i].shared = &shared;
        producer_args[i].thread_id = i;
        producer_args[i].cpu_id = core_map[tidx++ % num_cores];
        #ifdef __linux__
            pthread_create(&producer_threads[i], &attrs[tidx], producer_thread, &producer_args[i]);
        #else
            pthread_create(&producer_threads[i], NULL, producer_thread, &producer_args[i]);
        #endif
    }

    // Start timing
    DTime start_time = DTimeNow();

    // Wait for threads
    for (int i = 0; i < NUM_PRODUCER_THREADS; i++)
        pthread_join(producer_threads[i], NULL);
    for (int i = 0; i < NUM_CONSUMER_THREADS; i++)
        pthread_join(consumer_threads[i], NULL);

    DTime end_time = DTimeNow();

    // Print results
    char buf[64];
    printf("Test completed in %s (%.3f us avg per message)\n",
           DTimeDurationFormat(DTimeBetween(end_time, start_time), buf),
           (double)DTimeBetween(end_time, start_time)/1000.0/(double)shared.items_consumed);
    printf("Total produced, consumed = %d, %d\n", shared.items_produced, shared.items_consumed);
    printf("Test %s\n", (shared.items_produced == shared.items_consumed) ? "PASSED" : "FAILED");

    // Cleanup
    queue_destroy(shared.queue);

    return (shared.items_produced != shared.items_consumed) ||
           (shared.items_produced != (NUM_PRODUCER_THREADS * OPS_PER_PRODUCER));
}
