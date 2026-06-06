/*
 * solariTime.h - clocks. Two distinct notions of time, never mixed:
 *
 *   - Wall clock (solariNowUnixMs): milliseconds since the Unix epoch, used
 *     ONLY for timestamps stamped into frames and DB rows. Subject to NTP
 *     steps; never use it to measure a duration.
 *   - Monotonic clock (solariMonotonicMicros): microseconds from an arbitrary
 *     epoch that never goes backwards, used for RTT and any interval math.
 *
 * Mixing the two is the classic source of negative latencies and is forbidden
 * by the coding standards (§17).
 */
#ifndef SOLARI_TIME_H
#define SOLARI_TIME_H

#include "solari/solariCommon.h"

/* Wall-clock milliseconds since the Unix epoch (UTC). */
uint64_t solariNowUnixMs(void);

/* Monotonic microseconds. Strictly non-decreasing across calls in a process;
 * the zero point is unspecified, so only differences are meaningful. */
uint64_t solariMonotonicMicros(void);

/* Sleep for at least ms milliseconds. Best-effort; may return early only on
 * signal interruption, in which case it resumes for the remainder. */
void solariSleepMs(uint32_t ms);

#endif /* SOLARI_TIME_H */
