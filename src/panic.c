#include "dew.h"

// backtrace function
#if defined(__MACH__) && defined(__APPLE__)
	#define HAS_BACKTRACE
	#include <execinfo.h>
#endif


void _panic(const char* file, int line, const char* fun, const char* fmt, ...) {
	FILE* fp = stderr;

	#ifndef __wasm__
	flockfile(fp);
	#endif

	fprintf(fp, "\npanic: ");

	va_list ap;
	va_start(ap, fmt);
	vfprintf(fp, fmt, ap);
	va_end(ap);

	fprintf(fp, " in %s at %s:%d\n", fun, file, line);

	#ifdef HAS_BACKTRACE
		void* buf[128];
		int framecount = backtrace(buf, countof(buf));
		if (framecount > 1) {
			char** strs = backtrace_symbols(buf, framecount);
			if (strs != NULL) {
				for (int i = 1; i < framecount; ++i) {
					fwrite(strs[i], strlen(strs[i]), 1, fp);
					fputc('\n', fp);
				}
				free(strs);
			} else {
				fflush(fp);
				backtrace_symbols_fd(buf, framecount, fileno(fp));
			}
		}
	#endif

	#ifndef __wasm__
	funlockfile(fp);
	#endif

	fflush(fp);
	fsync(STDERR_FILENO);

	abort();
}
