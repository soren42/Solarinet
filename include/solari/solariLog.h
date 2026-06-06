/*
 * solariLog.h - leveled, thread-safe logging with syslog and/or file sinks.
 *
 * Timestamps are ISO-8601 UTC (e.g. 2026-06-06T22:54:01.123Z). The logger is a
 * process-global singleton initialized once at startup; all calls are safe to
 * make concurrently. Format strings follow printf semantics.
 */
#ifndef SOLARI_LOG_H
#define SOLARI_LOG_H

#include "solari/solariCommon.h"

typedef enum {
    SOLARI_LOG_TRACE = 0,
    SOLARI_LOG_DEBUG = 1,
    SOLARI_LOG_INFO  = 2,
    SOLARI_LOG_WARN  = 3,
    SOLARI_LOG_ERROR = 4,
    SOLARI_LOG_FATAL = 5
} solariLogLevel;

typedef enum {
    SOLARI_LOG_SINK_STDERR = 0x01,  /* write to stderr                       */
    SOLARI_LOG_SINK_FILE   = 0x02,  /* append to the file given to Init      */
    SOLARI_LOG_SINK_SYSLOG = 0x04   /* POSIX syslog(); ignored on Windows    */
} solariLogSink;

/* Initialize the global logger. sinks is a bitwise-OR of solariLogSink.
 * filePath may be NULL unless SOLARI_LOG_SINK_FILE is requested. ident labels
 * syslog lines (and is harmless otherwise). Returns SOLARI_OK or an error.
 * Calling Init again reconfigures the singleton (old file sink is closed). */
solariStatus solariLogInit(int sinks, const char *filePath, const char *ident);

/* Set the minimum level that will be emitted; lower-severity calls are dropped
 * cheaply. Default after Init is SOLARI_LOG_INFO. */
void solariLogSetLevel(solariLogLevel level);

/* Emit a formatted line at the given level. Thread-safe. A trailing newline is
 * added automatically; do not include one in fmt. */
void solariLogf(solariLogLevel level, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* Flush and release sinks. Safe to call without a prior Init. After Shutdown
 * the logger reverts to an uninitialized stderr-only state. */
void solariLogShutdown(void);

#endif /* SOLARI_LOG_H */
