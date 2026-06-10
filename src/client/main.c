/*
 * main.c - solariClient entry point (sec 7.3).
 *
 * Two modes:
 *   - local (default / no server): collect a report and print a human summary.
 *   - reporting (a [server] primaryUrl is configured and the build has the
 *     transport+spool): HELLO, then sample -> push-or-spool every interval.
 * The watchdog sibling and the CONTROL back-channel land in the next increment.
 */
#include "client.h"

#include "solari/solariLog.h"
#include "solari/solariTime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [--config PATH] [--interval SEC] [--loop] [--once] [-h]\n"
        "  --config PATH   load a client .conf (sec 13); else autodetect-only\n"
        "  --interval SEC  override the sample interval\n"
        "  --loop          run continuously (Ctrl-C to stop)\n"
        "  --once          a single cycle, then exit (default)\n",
        argv0);
}

static double avgCpuPct(const solariClientReport *r)
{
    uint32_t sum = 0; uint8_t i;
    if (r->coreCount == 0) return 0.0;
    for (i = 0; i < r->coreCount; i++) sum += r->cpuLoadMilli[i];
    return (double)sum / r->coreCount / 10.0;     /* permille -> percent */
}

static void printReport(const solariClientReport *r)
{
    uint8_t i;
    double ramPct = r->ramTotalKb ? (double)r->ramUsedKb * 100.0 / r->ramTotalKb : 0.0;

    printf("solariClient  host=%s  os=%s  arch=%s  cores=%u\n",
           r->hostFqdn, r->osName, r->arch, (unsigned)r->coreCount);
    printf("  cpu avg %.1f%%   ram %llu/%llu MiB (%.0f%%)   swap %llu/%llu MiB\n",
           avgCpuPct(r),
           (unsigned long long)(r->ramUsedKb / 1024),
           (unsigned long long)(r->ramTotalKb / 1024), ramPct,
           (unsigned long long)(r->swapUsedKb / 1024),
           (unsigned long long)(r->swapTotalKb / 1024));

    printf("  disks(%u):", (unsigned)r->diskCount);
    for (i = 0; i < r->diskCount; i++)
        printf(" %s %llu/%lluMiB", r->disks[i].mount,
               (unsigned long long)(r->disks[i].freeKb / 1024),
               (unsigned long long)(r->disks[i].totalKb / 1024));
    printf("\n");

    printf("  ifaces(%u):", (unsigned)r->ifaceCount);
    for (i = 0; i < r->ifaceCount; i++)
        printf(" %s rx%lluk tx%lluk cap%lluk", r->ifaces[i].name,
               (unsigned long long)r->ifaces[i].rxKbps,
               (unsigned long long)r->ifaces[i].txKbps,
               (unsigned long long)r->ifaces[i].capacityKbps);
    printf("\n");

    if (r->procCount) {
        printf("  procs(%u):", (unsigned)r->procCount);
        for (i = 0; i < r->procCount; i++)
            printf(" %s[%d]%s/%uf/%us/%lluMiB", r->procs[i].name, r->procs[i].pid,
                   r->procs[i].pid < 0 ? "(down)" : "",
                   (unsigned)r->procs[i].nFiles, (unsigned)r->procs[i].nSockets,
                   (unsigned long long)(r->procs[i].rssKb / 1024));
        printf("\n");
    }
    if (r->logCount) {
        printf("  logs(%u):", (unsigned)r->logCount);
        for (i = 0; i < r->logCount; i++)
            printf(" %s +%lluB match=%u", r->logs[i].path,
                   (unsigned long long)r->logs[i].sizeDelta,
                   (unsigned)r->logs[i].matchCount);
        printf("\n");
    }
}

/* local mode: collect + print, no server. */
static int runLocal(const clientConfig *cfg, int loop)
{
    clientState st;
    solariStatus rc = SOLARI_OK;
    memset(&st, 0, sizeof st);
    do {
        solariClientReport rep;
        rc = clientCollectReport(cfg, &st, false, &rep);
        if (rc != SOLARI_OK) {
            solariLogf(SOLARI_LOG_ERROR, "collect failed: %s", solariStrError(rc));
            break;
        }
        printReport(&rep);
        fflush(stdout);
        if (loop) solariSleepMs(cfg->sampleIntervalSec * 1000u);
    } while (loop);
    return rc == SOLARI_OK ? 0 : 1;
}

#ifdef CLIENT_WITH_REPORTING
/* reporting mode: announce, then sample -> push-or-spool every interval. */
static int runReporting(const clientConfig *cfg, int loop)
{
    clientContext ctx;
    clientState   st;
    solariStatus  rc;

    memset(&st, 0, sizeof st);
    if (clientContextInit(&ctx, cfg) != SOLARI_OK) {
        solariLogf(SOLARI_LOG_FATAL, "context init failed");
        return 1;
    }
    clientConnect(&ctx);                       /* best-effort */
    clientSendHello(&ctx);                      /* best-effort announce */

    do {
        solariClientReport rep;
        rc = clientCollectReport(cfg, &st, false, &rep);
        if (rc != SOLARI_OK) {
            solariLogf(SOLARI_LOG_ERROR, "collect failed: %s", solariStrError(rc));
        } else {
            if (!ctx.conn) clientConnect(&ctx);  /* reconnect if we went offline */
            clientReportSend(&ctx, &rep);        /* sends or durably spools */
        }
        if (loop) solariSleepMs(cfg->sampleIntervalSec * 1000u);
    } while (loop);

    clientContextClose(&ctx);
    return 0;
}
#endif

int main(int argc, char **argv)
{
    const char *cfgPath = NULL;
    long intervalOverride = -1;
    int loop = 0, i, ret;
    clientConfig cfg;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) cfgPath = argv[++i];
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc) intervalOverride = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--loop")) loop = 1;
        else if (!strcmp(argv[i], "--once")) loop = 0;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); usage(argv[0]); return 2; }
    }

    solariLogInit(SOLARI_LOG_SINK_STDERR, NULL, "solariClient");

    if (cfgPath) {
        solariStatus rc = clientConfigFromFile(cfgPath, &cfg);
        if (rc != SOLARI_OK) {
            solariLogf(SOLARI_LOG_FATAL, "config load failed (%s): %s",
                       cfgPath, solariStrError(rc));
            solariLogShutdown();
            return 1;
        }
        solariLogf(SOLARI_LOG_INFO, "config %s: %u procs, %u logs, interval %us, server '%s'",
                   cfgPath, (unsigned)cfg.procCount, (unsigned)cfg.logCount,
                   (unsigned)cfg.sampleIntervalSec, cfg.primaryUrl);
    } else {
        clientConfigDefaults(&cfg);
        solariLogf(SOLARI_LOG_INFO, "no --config; autodetect-only collection");
    }
    if (intervalOverride > 0) cfg.sampleIntervalSec = (uint32_t)intervalOverride;

#ifdef CLIENT_WITH_REPORTING
    if (cfg.primaryUrl[0]) ret = runReporting(&cfg, loop);
    else                   ret = runLocal(&cfg, loop);
#else
    ret = runLocal(&cfg, loop);
#endif

    solariLogShutdown();
    return ret;
}
