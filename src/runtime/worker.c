#include "worker.h"

const char* _fmtworker(const Worker* w) {
    if (w->wkind == WorkerKind_USER)
        return fmtuworker((UWorker*)w);
    return fmtaworker((AWorker*)w);
}

const char* fmtuworker(const UWorker* uw) {
    char* buf = fmtbuf_get();
    snprintf(buf, fmtbuf_cap(), UWORKER_ID_F, uworker_id(uw));
    return buf;
}

const char* fmtaworker(const AWorker* aw) {
    char* buf = fmtbuf_get();
    snprintf(buf, fmtbuf_cap(), AWORKER_ID_F, aworker_id(aw));
    return buf;
}
