#include "dew.h"
#if defined(__APPLE__)
	#include <mach/mach_time.h>
#endif


#if defined(__linux__)

	DTime DTimeNow() {
		struct timespec ts;
		int r = clock_gettime(CLOCK_MONOTONIC, &ts);
		if (r != 0)
			return 0;
		return ((u64)(ts.tv_sec) * 1000000000ull) + ts.tv_nsec;
	}

#elif defined(__APPLE__)

	// fraction to multiply a value in mach tick units with to convert it to nanoseconds
	static mach_timebase_info_data_t g_tbase;

	__attribute__((constructor)) static void time_init() {
		if (mach_timebase_info(&g_tbase) != KERN_SUCCESS)
			logerr("mach_timebase_info failed");
	}

	u64 DTimeMachAbsoluteTime(DTime t) {
		return (t * g_tbase.denom) / g_tbase.numer;
	}

	DTime DTimeNow() {
		u64 t = mach_absolute_time();
		return (t * g_tbase.numer) / g_tbase.denom;
	}

#endif


DTimeDuration DTimeBetween(DTime a, DTime b) {
	DTime d = a - b;
	return (DTimeDuration)(d < 0 ? -d : d);
}

DTimeDuration DTimeSince(DTime past) {
	return DTimeBetween(DTimeNow(), past);
}

DTimeDuration DTimeUntil(DTime future) {
	return DTimeBetween(future, DTimeNow());
}


// These functions return float64 because a commond use case is to print a floating point
// number like 1.5s, and a truncation to integer would make these not so useful.
// Splitting the integer and fraction ourselves guarantees that converting the returned
// float64 to an integer rounds the same way that a pure integer conversion would have,
// even in cases where, say, float64(DTimeDurationNanoseconds(d))/1e9 would have rounded
// differently.

float64 DTimeDurationHours(DTimeDuration d) {
	DTimeDuration hour = d / D_TIME_HOUR;
	DTimeDuration nsec = d % D_TIME_HOUR;
	return (float64)hour + (float64)(nsec)/(60 * 60 * 1e9);
}

float64 DTimeDurationMinutes(DTimeDuration d) {
	DTimeDuration hour = d / D_TIME_MINUTE;
	DTimeDuration nsec = d % D_TIME_MINUTE;
	return (float64)hour + (float64)(nsec)/(60 * 1e9);
}

float64 DTimeDurationSeconds(DTimeDuration d) {
	DTimeDuration hour = d / D_TIME_SECOND;
	DTimeDuration nsec = d % D_TIME_SECOND;
	return (float64)hour + (float64)(nsec)/1e9;
}

i64 DTimeDurationMilliseconds(DTimeDuration d) { return d / 1000000; }
i64 DTimeDurationMicroseconds(DTimeDuration d) { return d / 1000; }
i64 DTimeDurationNanoseconds(DTimeDuration d) { return d; }


void DTimeTimespec(DTime t, struct timespec* ts) {
	ts->tv_sec = t / D_TIME_SECOND;
	ts->tv_nsec = t % D_TIME_SECOND;
}


void DTimeDurationTimespec(DTimeDuration d, struct timespec* ts) {
	ts->tv_sec = d / D_TIME_SECOND;
	ts->tv_nsec = d % D_TIME_SECOND;
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
