/*
 * solariLog.c - global leveled logger.
 *
 * State lives in one file-static struct guarded by a mutex (CRITICAL_SECTION on
 * Windows, pthread_mutex elsewhere). This is the one sanctioned global in
 * libsolari: a logger is inherently process-wide, and the handle pattern would
 * force every call site to thread a pointer through purely for diagnostics.
 *
 * Needs POSIX gmtime_r / syslog under strict -std=c99; request them up front.
 */
#define _POSIX_C_SOURCE 200809L

#include "solari/solariLog.h"
#include "solari/solariTime.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  include <windows.h>
   typedef CRITICAL_SECTION solariMutex;
#  define MUTEX_INIT(m)   InitializeCriticalSection(m)
#  define MUTEX_LOCK(m)   EnterCriticalSection(m)
#  define MUTEX_UNLOCK(m) LeaveCriticalSection(m)
#  define MUTEX_FREE(m)   DeleteCriticalSection(m)
#else
#  include <pthread.h>
#  include <syslog.h>
   typedef pthread_mutex_t solariMutex;
#  define MUTEX_INIT(m)   pthread_mutex_init((m), NULL)
#  define MUTEX_LOCK(m)   pthread_mutex_lock(m)
#  define MUTEX_UNLOCK(m) pthread_mutex_unlock(m)
#  define MUTEX_FREE(m)   pthread_mutex_destroy(m)
#endif

static struct {
    int            sinks;
    solariLogLevel minLevel;
    FILE          *file;
    char           ident[64];
    solariMutex    mtx;
    bool           ready;
#if !defined(_WIN32)
    bool           syslogOpen;
#endif
} gLog = {
    .sinks = SOLARI_LOG_SINK_STDERR,
    .minLevel = SOLARI_LOG_INFO
    /* remaining members (file, ident, mtx, ready, syslogOpen) zero-init */
};

static const char *levelName(solariLogLevel l)
{
    switch (l) {
    case SOLARI_LOG_TRACE: return "TRACE";
    case SOLARI_LOG_DEBUG: return "DEBUG";
    case SOLARI_LOG_INFO:  return "INFO";
    case SOLARI_LOG_WARN:  return "WARN";
    case SOLARI_LOG_ERROR: return "ERROR";
    case SOLARI_LOG_FATAL: return "FATAL";
    default:               return "?";
    }
}

/* Render ISO-8601 UTC with millisecond precision into buf (>= 25 bytes). */
static void isoTimestamp(char *buf, size_t cap)
{
    uint64_t ms = solariNowUnixMs();
    time_t secs = (time_t)(ms / 1000ULL);
    unsigned millis = (unsigned)(ms % 1000ULL);
    struct tm tmv;
#if defined(_WIN32)
    gmtime_s(&tmv, &secs);
#else
    gmtime_r(&secs, &tmv);
#endif
    char base[20];
    strftime(base, sizeof base, "%Y-%m-%dT%H:%M:%S", &tmv);
    snprintf(buf, cap, "%s.%03uZ", base, millis);
}

solariStatus solariLogInit(int sinks, const char *filePath, const char *ident)
{
    if ((sinks & SOLARI_LOG_SINK_FILE) && filePath == NULL) {
        return ERR_INVALID_ARG;
    }
    if (!gLog.ready) {
        MUTEX_INIT(&gLog.mtx);
        gLog.ready = true;
    }
    MUTEX_LOCK(&gLog.mtx);

    if (gLog.file) { fclose(gLog.file); gLog.file = NULL; }

    gLog.sinks = sinks;
    if (sinks & SOLARI_LOG_SINK_FILE) {
        gLog.file = fopen(filePath, "a");
        if (!gLog.file) {
            MUTEX_UNLOCK(&gLog.mtx);
            return ERR_PLATFORM;
        }
    }
    if (ident) {
        strncpy(gLog.ident, ident, sizeof gLog.ident - 1);
        gLog.ident[sizeof gLog.ident - 1] = '\0';
    } else {
        gLog.ident[0] = '\0';
    }
#if !defined(_WIN32)
    if (sinks & SOLARI_LOG_SINK_SYSLOG) {
        openlog(gLog.ident[0] ? gLog.ident : "solari", LOG_PID, LOG_DAEMON);
        gLog.syslogOpen = true;
    }
#endif
    MUTEX_UNLOCK(&gLog.mtx);
    return SOLARI_OK;
}

void solariLogSetLevel(solariLogLevel level)
{
    gLog.minLevel = level;
}

#if !defined(_WIN32)
static int syslogPriority(solariLogLevel l)
{
    switch (l) {
    case SOLARI_LOG_TRACE:
    case SOLARI_LOG_DEBUG: return LOG_DEBUG;
    case SOLARI_LOG_INFO:  return LOG_INFO;
    case SOLARI_LOG_WARN:  return LOG_WARNING;
    case SOLARI_LOG_ERROR: return LOG_ERR;
    case SOLARI_LOG_FATAL: return LOG_CRIT;
    default:               return LOG_INFO;
    }
}
#endif

void solariLogf(solariLogLevel level, const char *fmt, ...)
{
    char ts[28];
    char msg[1024];
    va_list ap;

    if (level < gLog.minLevel) {
        return;
    }
    if (!gLog.ready) {
        /* Pre-Init: emit to stderr without locking so early errors are seen. */
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
        return;
    }

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    isoTimestamp(ts, sizeof ts);

    MUTEX_LOCK(&gLog.mtx);
    if (gLog.sinks & SOLARI_LOG_SINK_STDERR) {
        fprintf(stderr, "%s %-5s %s\n", ts, levelName(level), msg);
    }
    if ((gLog.sinks & SOLARI_LOG_SINK_FILE) && gLog.file) {
        fprintf(gLog.file, "%s %-5s %s\n", ts, levelName(level), msg);
        fflush(gLog.file);
    }
#if !defined(_WIN32)
    if ((gLog.sinks & SOLARI_LOG_SINK_SYSLOG) && gLog.syslogOpen) {
        syslog(syslogPriority(level), "%s", msg);
    }
#endif
    MUTEX_UNLOCK(&gLog.mtx);
}

void solariLogShutdown(void)
{
    if (!gLog.ready) {
        return;
    }
    MUTEX_LOCK(&gLog.mtx);
    if (gLog.file) { fclose(gLog.file); gLog.file = NULL; }
#if !defined(_WIN32)
    if (gLog.syslogOpen) { closelog(); gLog.syslogOpen = false; }
#endif
    gLog.sinks = SOLARI_LOG_SINK_STDERR;
    gLog.minLevel = SOLARI_LOG_INFO;
    MUTEX_UNLOCK(&gLog.mtx);
    MUTEX_FREE(&gLog.mtx);
    gLog.ready = false;
}
