#include "dew.h"
#include <sys/time.h>


void _logmsg(int level, const char* fmt, ...) {
	FILE* fp = stderr;
	flockfile(fp);

	#if 1 /* enable ANSI colors */
		char style[] = { "\e[1;30m" };
		if (level >= 3) {
			// debug = only use purple if it contains a keyword like "TODO:"
			if (strstr(fmt, "TODO:") || strstr(fmt, "FIXME:")) {
				style[5] = '5';
			} else {
				// style[5] = '5';
				// style[0] = 0;
				style[3] = 'm'; style[4] = 0; // "\e[1m"
			}
		} else if (level == 2) {
			// info = bold
			style[3] = 'm';
			style[4] = 0;
		} else {
			// err = bold red, warn = bold yellow
			style[5] = level <= 0 ? '1' : '3';
		}
		if (style[0])
			fwrite(style, strlen(style), 1, fp);
	#endif

	#if 1 /* enable timestamp */
	{
		char buf[19]; // "X HH:MM:SS.uuuuuu \0"
		struct timeval tv = {};
		gettimeofday(&tv, NULL);
		if (tv.tv_usec >= 1000000) {
			tv.tv_usec -= 1000000;
			tv.tv_sec++;
		}
		struct tm tm;
		localtime_r(&tv.tv_sec, &tm);
		buf[0] = "EWID"[MIN(MAX(level, 0), 3)];
		isize n = strftime(buf+1, sizeof(buf)-1, " %H:%M:%S", &tm) + 1;
		if (n <= 0)
			n = 0;
		n += snprintf(buf + n, sizeof(buf) - n, ".%06d ", tv.tv_usec);
		if (n < 0)
			n = 0;
		fwrite(buf, n, 1, fp);
		#if 1 /* enable ANSI colors */
			fprintf(fp, "\e[0m");
		#endif
	}
	#endif

	va_list ap;
	va_start(ap, fmt);
	vfprintf(fp, fmt, ap);
	va_end(ap);

	#if 1 /* enable ANSI colors */
		fprintf(fp, "\e[0m\n");
	#else
		fprintf(fp, "\n");
	#endif

	funlockfile(fp);
	if (level < 1 || level > 2) {
		fflush(fp);
		fsync(STDERR_FILENO);
	}
}
