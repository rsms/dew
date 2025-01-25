// common fields of all watcher structs
#define EV_COMMON \
	uint64_t ud; \
	void*    udcb; \
	int      reqid; \
	uint8_t  evtype;

// // callback function
// #define EV_CB_DECLARE(type) \
// 	void (*cb)(EV_P_ struct type *w, int revents);
// #define EV_CB_INVOKE(watcher,revents) \
// 	(watcher)->cb (EV_A_ (watcher), (revents))

// disable priority feature
#define EV_MINPRI 0
#define EV_MAXPRI 0

// disable features we don't use
#define EV_PERIODIC_ENABLE 0
#define EV_EMBED_ENABLE 0
#define EV_ASYNC_ENABLE 0

// don't need pre libev 4.0 compatibility
#define EV_COMPAT3 0

#if defined(__APPLE__)
	#include "config_darwin.h"
#elif defined(__linux__)
	#include "config_linux.h"
#endif
