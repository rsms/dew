#pragma once
#include "../dew.h"
#include "runtime.h"

#if defined(__linux__) || defined(__APPLE__)
    #include <pthread.h>
    typedef pthread_t OSThread;
#else
    #error "TODO: OSThread for target platform"
#endif

API_BEGIN

typedef struct Worker       Worker;       // worker base
typedef struct UWorker      UWorker;      // user worker, with Lua environment
typedef struct AWorker      AWorker;      // custom (internal) worker
typedef struct AsyncWorkReq AsyncWorkReq; // unit of work performed by an AWorker

typedef u16 AsyncWorkOp;
enum AsyncWorkOp {
    AsyncWorkOp_NOP = 0,
    AsyncWorkOp_NANOSLEEP = 1,
    AsyncWorkOp_ADDRINFO = 2,
    AsyncWorkOp_WORKER_MSG = 0x1000, // message sent to the worker's main task from another worker
};

#define AsyncWorkFlag_HAS_CONT ((u16)1 << 0) // has continuation

struct AsyncWorkReq {
    u16 op;    // operation to perform
    u16 flags; //
    u32 tid;   // task waiting for this work
    u64 arg;   // input argument
};

typedef struct AsyncWorkRes {
    u16 op;     // operation performed
    union {
        struct { // all ops but AsyncWorkOp_WORKER_MSG
            u16 flags;  //
            u32 tid;    // task waiting for this work
            i64 result; // output result
        } __attribute__((packed));
        struct { // op == AsyncWorkOp_WORKER_MSG
            u16      _unused1;
            u32      sender_sid;
            u32      sender_tid; // tid in namespace of receiving worker's parent S
            u32      _unsued2;
            MiniBuf* buf; // owned by this AsyncWorkRes
        } __attribute__((packed)) msg;
    };
} AsyncWorkRes;

enum WorkerStatus {
    Worker_CLOSED,
    Worker_OPEN,
    Worker_READY,
};

enum WorkerKind {
    WorkerKind_USER,  // UWorker: userspace worker, with Lua env
    WorkerKind_ASYNC, // AWorker: for blocking async work like syscalls
};

struct Worker {
    // state accessed by both parent thread and worker thread
    u8           wkind;   // enum WorkerKind
    _Atomic(u8)  status;  // enum WorkerStatus
    _Atomic(u8)  nrefs;
    _Atomic(u32) waiters; // list of tasks (tid's) waiting for this worker (T.info.wait_task)
    OSThread     thread;

    // state accessed only by parent thread
    S*               s;    // S which spawned this worker
    Worker* nullable next; // list link in S.workers
};

struct UWorker { // wkind == WorkerKind_USER
    Worker w;
    u32    spawned_by_tid;
    u32    _unused;
    union { // input & output data (as input for workeru_open, as output from worker exit.)
        struct { // input
            // mainfun_lcode contains Lua bytecode for the main function, used during setup.
            // Worker owns this memory, and it's free'd during thread setup.
            u32            mainfun_lcode_len; // number of bytes at mainfun_lcode
            void* nullable mainfun_lcode;
        };
        struct { // output
            // When a worker has exited, this holds the description of an error
            // which caused the worker to exit, or NULL if no error or if there are no waiters.
            // Memory is free'd by worker_free.
            u32            errdesc_invalid; // 0 if errdesc is valid
            char* nullable errdesc;
        };
    };
    S s;
};

struct AWorker { // wkind == WorkerKind_ASYNC
    Worker w;
    //
    // ———— HERE ————
    // change to...
    // - using one SPSC queue per worker (can use AsyncWorkCQ)
    // - upon work to be done, selecting a free worker (1:1 S:AWorker so no race)
    // - get rid of TSQ
    //
    // AsyncWorkCQ q;
    union {
        uintptr      req_is_active; // 0 when work is unused
        AsyncWorkReq req;           // work currently being processed
    };
};

// UWorkerUVal is a Lua-managed (GC'd) wrapper around a internally managed Worker.
// This allows a worker to be referenced by userland independently of its state.
typedef struct UWorkerUVal {
    UVal     uval;
    UWorker* uw;
} UWorkerUVal;

// uworker_id formats a UWorker for logging
#define uworker_id(uw) (unsigned long)(uintptr)(uw), (uw)->s.sid
#define UWORKER_ID_F   "UWorker#%lx(S%u)"

// aworker_id formats a AWorker for logging
#define aworker_id(aw) (unsigned long)(uintptr)(aw)
#define AWORKER_ID_F   "AWorker#%lx"

// const char* fmtworker(T worker)
#define fmtworker(w) _Generic((w), \
    const Worker*: _fmtworker,  Worker*: _fmtworker, \
    const UWorker*: fmtuworker, UWorker*: fmtuworker, \
    const AWorker*: fmtaworker, AWorker*: fmtaworker)(w)
const char* _fmtworker(const Worker*);
const char* fmtuworker(const UWorker*);
const char* fmtaworker(const AWorker*);

API_END
