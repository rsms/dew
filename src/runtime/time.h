#pragma once
#include "../dew.h"
API_BEGIN

typedef u64 DTime;         // monotonic high-resolution clock with undefined base
typedef i64 DTimeDuration; // duration, like 134ms or -1.2h

#define D_TIME_HOUR        ((DTimeDuration)3600e9)
#define D_TIME_MINUTE      ((DTimeDuration)60e9)
#define D_TIME_SECOND      ((DTimeDuration)1e9)
#define D_TIME_MILLISECOND ((DTimeDuration)1e6)
#define D_TIME_MICROSECOND ((DTimeDuration)1e3)
#define D_TIME_NANOSECOND  ((DTimeDuration)1)

// DTimeNow returns the current monotonic clock value (not "wall clock" time.)
// DTime is compatible with DTimeDuration so to make a time in the future, simply
// add to DTime, i.e. "10 seconds from now" = DTimeNow() + 10*D_TIME_SECOND
DTime DTimeNow();

// DTimeSince returns the time delta between now and a point in time in the past
DTimeDuration DTimeSince(DTime past);

// DTimeUntil returns the time delta between now and a point in time in the future
DTimeDuration DTimeUntil(DTime future);

// DTimeBetween returns the time delta between a and b.
// i.e. DTimeBetween(1, 3) == -2, DTimeBetween(3, 1) == 2.
DTimeDuration DTimeBetween(DTime a, DTime b);

// DTimeTimespec populates a timespec struct with the value of t
void DTimeTimespec(DTime t, struct timespec* ts);

// DTimeDurationTimespec populates a timespec struct with the value of d
void DTimeDurationTimespec(DTimeDuration d, struct timespec* ts);

// DTimeDurationFormat writes a human-readable string like "1.6s" to buf.
// Returns a pointer to beginning of buf.
const char* DTimeDurationFormat(DTimeDuration d, char buf[26]);

float64 DTimeDurationHours(DTimeDuration d);
float64 DTimeDurationMinutes(DTimeDuration d);
float64 DTimeDurationSeconds(DTimeDuration d);
i64 DTimeDurationMilliseconds(DTimeDuration d);
i64 DTimeDurationMicroseconds(DTimeDuration d);
i64 DTimeDurationNanoseconds(DTimeDuration d);

API_END
