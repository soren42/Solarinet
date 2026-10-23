/*
 * scanWorkerMain.c - independent durable discovery scan worker.
 *
 * This process owns its MariaDB connection and never shares the server event
 * loop's single-threaded database handle. Both active and standby hosts may run
 * it; the shared lease row fences claiming and every result/progress write.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "server.h"
#include "serverScan.h"
#include "solari/solariLog.h"
#include "solari/solariTime.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;

static void onSignal(int sig)
{
    (void)sig;
    g_stop = 1;
}

typedef struct {
    serverContext *ctx;
    const serverDiscoveryScanJob *job;
    uint32_t lastDone;
    uint32_t lastFound;
    uint64_t lastWriteMs;
    uint64_t lastLeaseCheckMs;
} workerProgress;

static solariStatus updateProgress(void *userData, uint32_t done, uint32_t found)
{
    workerProgress *p = (workerProgress *)userData;
    uint64_t now = solariNowUnixMs();
    uint64_t holder = 0, epoch = 0;
    solariStatus rc;
    int terminalTransition;

    if (g_stop) return ERR_PLATFORM;
    if (now - p->lastLeaseCheckMs >= 500) {
        rc = serverDbLeaseRead(p->ctx->db, &holder, &epoch);
        if (rc != SOLARI_OK) return rc;
        if (holder != p->ctx->nodeId || epoch != p->ctx->epoch)
            return ERR_LEASE_LOST;
        p->lastLeaseCheckMs = now;
    }
    /* Publish the first completed-probe transition immediately. Enrichment
     * repeatedly reports that same total, so those callbacks still need the
     * normal write throttle until a result count changes or one second passes. */
    terminalTransition = done >= p->job->hostsTotal &&
                         p->lastDone < p->job->hostsTotal;
    if (!terminalTransition && found == p->lastFound &&
        done >= p->lastDone && done - p->lastDone < 16 &&
        now - p->lastWriteMs < 1000)
        return SOLARI_OK;
    rc = serverDbDiscoveryScanUpdate(p->ctx->db, p->job->scanId,
                                     p->ctx->nodeId, p->ctx->epoch,
                                     "running", done, found, 0, NULL);
    if (rc == SOLARI_OK) {
        p->lastDone = done;
        p->lastFound = found;
        p->lastWriteMs = now;
    }
    return rc;
}

static void failJob(serverContext *ctx, const serverDiscoveryScanJob *job,
                    solariStatus cause, const char *message,
                    uint32_t hostsDone, uint32_t hostsFound)
{
    uint64_t holder = 0, epoch = 0;
    const char *state = g_stop ? "cancelled" : "failed";
    solariStatus leaseRc = serverDbLeaseRead(ctx->db, &holder, &epoch);
    if (leaseRc != SOLARI_OK || holder != ctx->nodeId || epoch != ctx->epoch) {
        solariLogf(SOLARI_LOG_WARN,
                   "scan-worker: scan=%llu stopped after lease loss; successor will settle it",
                   (unsigned long long)job->scanId);
        return;
    }
    leaseRc = serverDbDiscoveryScanUpdate(ctx->db, job->scanId, ctx->nodeId,
                                          ctx->epoch, state, hostsDone,
                                          hostsFound, cause, message);
    if (leaseRc != SOLARI_OK)
        solariLogf(SOLARI_LOG_ERROR,
                   "scan-worker: scan=%llu terminal update failed: %s",
                   (unsigned long long)job->scanId, solariStrError(leaseRc));
}

static solariStatus runJob(serverContext *ctx, const serverDiscoveryScanJob *job)
{
    workerProgress progress;
    size_t found = 0;
    solariStatus rc;
    uint64_t holder = 0, epoch = 0;

    memset(&progress, 0, sizeof progress);
    progress.ctx = ctx;
    progress.job = job;
    progress.lastWriteMs = solariNowUnixMs();
    progress.lastLeaseCheckMs = progress.lastWriteMs;

    solariLogf(SOLARI_LOG_INFO,
               "scan-worker: starting scan=%llu cidr=%s hosts=%u",
               (unsigned long long)job->scanId, job->cidr,
               (unsigned)job->hostsTotal);
    rc = serverScanRunProgress(ctx, job->cidr, job->ports, &found,
                               updateProgress, &progress);
    if (rc != SOLARI_OK) {
        failJob(ctx, job, rc, g_stop ? "worker stopping" : solariStrError(rc),
                progress.lastDone, progress.lastFound);
        return rc;
    }

    rc = serverDbLeaseRead(ctx->db, &holder, &epoch);
    if (rc != SOLARI_OK || holder != ctx->nodeId || epoch != ctx->epoch) {
        solariLogf(SOLARI_LOG_WARN,
                   "scan-worker: scan=%llu completed probing after lease loss; not publishing completion",
                   (unsigned long long)job->scanId);
        return ERR_LEASE_LOST;
    }
    rc = serverDbDiscoveryScanUpdate(ctx->db, job->scanId, ctx->nodeId,
                                     ctx->epoch, "completed", job->hostsTotal,
                                     (uint32_t)found, 0, NULL);
    if (rc == SOLARI_OK)
        solariLogf(SOLARI_LOG_INFO,
                   "scan-worker: completed scan=%llu found=%zu",
                   (unsigned long long)job->scanId, found);
    else
        solariLogf(SOLARI_LOG_ERROR,
                   "scan-worker: scan=%llu completion write failed: %s",
                   (unsigned long long)job->scanId, solariStrError(rc));
    return rc;
}

int main(int argc, char **argv)
{
    serverConfig cfg;
    serverContext ctx;
    serverDb *db = NULL;
    const char *configPath = NULL;
    const char *lockPath = "/run/solari/solariScanWorker.lock";
    int lockFd = -1;
    int i;
    solariStatus rc;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) configPath = argv[++i];
        else if (!strcmp(argv[i], "--lock-file") && i + 1 < argc) lockPath = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            fprintf(stdout, "usage: %s [--config PATH] [--lock-file PATH]\n", argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    solariLogInit(SOLARI_LOG_SINK_STDERR, NULL, "solariScanWorker");
    lockFd = open(lockPath, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lockFd < 0 || flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
        solariLogf(SOLARI_LOG_WARN,
                   "another local scan worker owns the lock (or lock path unavailable): %s",
                   strerror(errno));
        if (lockFd >= 0) close(lockFd);
        solariLogShutdown();
        return lockFd < 0 ? 1 : 0;
    }
    if (configPath) rc = serverConfigFromFile(configPath, &cfg);
    else { serverConfigDefaults(&cfg); rc = SOLARI_OK; }
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_FATAL, "config load failed: %s", solariStrError(rc));
        solariLogShutdown();
        return 1;
    }
    rc = serverContextInit(&ctx, &cfg);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_FATAL, "context init failed: %s", solariStrError(rc));
        solariLogShutdown();
        return 1;
    }
    rc = serverDbOpen(&cfg, &db);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_FATAL, "database open failed: %s", solariStrError(rc));
        serverContextClose(&ctx);
        solariLogShutdown();
        return 1;
    }
    ctx.db = db;
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    solariLogf(SOLARI_LOG_INFO, "scan worker nodeId=0x%llx ready",
               (unsigned long long)ctx.nodeId);
    while (!g_stop) {
        uint64_t holder = 0, epoch = 0;
        rc = serverDbLeaseRead(db, &holder, &epoch);
        if (rc == SOLARI_OK && holder == ctx.nodeId && epoch != 0) {
            serverDiscoveryScanJob job;
            ctx.role = SRV_ACTIVE;
            ctx.epoch = epoch;
            rc = serverDbDiscoveryScanClaim(db, ctx.nodeId, epoch, &job);
            if (rc == SOLARI_OK) {
                (void)runJob(&ctx, &job);
                continue;
            }
            if (rc != ERR_CONN_RETRY)
                solariLogf(SOLARI_LOG_WARN, "scan-worker: claim failed: %s",
                           solariStrError(rc));
        } else {
            ctx.role = SRV_STANDBY;
            ctx.epoch = 0;
            if (rc != SOLARI_OK)
                solariLogf(SOLARI_LOG_WARN, "scan-worker: lease read failed: %s",
                           solariStrError(rc));
        }
        solariSleepMs(500);
    }

    serverDbClose(db);
    serverContextClose(&ctx);
    close(lockFd);
    solariLogf(SOLARI_LOG_INFO, "scan worker stopped");
    solariLogShutdown();
    return 0;
}
