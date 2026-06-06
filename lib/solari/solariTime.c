/*
 * solariTime.c - portable clock implementation.
 *
 * POSIX path uses clock_gettime(CLOCK_REALTIME / CLOCK_MONOTONIC); Windows
 * uses GetSystemTimePreciseAsFileTime + QueryPerformanceCounter. Both are
 * isolated here so the rest of libsolari is clock-portable for free.
 *
 * Under strict -std=c99 the POSIX clock/sleep APIs are hidden unless a feature
 * test macro is requested; do so before any header is pulled in.
 */
#define _POSIX_C_SOURCE 200809L

#include "solari/solariTime.h"

#if defined(_WIN32)

#include <windows.h>

uint64_t solariNowUnixMs(void)
{
    FILETIME ft;
    ULARGE_INTEGER u;
    /* 100ns ticks since 1601-01-01; offset to the Unix epoch. */
    GetSystemTimePreciseAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    /* 11644473600 seconds between 1601 and 1970. */
    return (uint64_t)((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

uint64_t solariMonotonicMicros(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart * 1000000ULL) / (uint64_t)freq.QuadPart);
}

void solariSleepMs(uint32_t ms)
{
    Sleep(ms);
}

#else /* POSIX */

#include <time.h>
#include <errno.h>

uint64_t solariNowUnixMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

uint64_t solariMonotonicMicros(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

void solariSleepMs(uint32_t ms)
{
    struct timespec req, rem;
    req.tv_sec = (time_t)(ms / 1000u);
    req.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&req, &rem) == -1 && errno == EINTR) {
        req = rem;
    }
}

#endif
