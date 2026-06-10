/*
 * clientCollect.c - assemble a solariClientReport via the PAL (sec 7.3).
 *
 * The core never touches a syscall: every measurement comes through platOS.h,
 * so this file is identical on every target. Config mapping (sec 13) lives here
 * too, turning the parsed .conf into the watch lists collection iterates.
 */
#include "client.h"
#include "platOS.h"

#include "solari/solariConfig.h"
#include "solari/solariLog.h"

#include <string.h>

void clientConfigDefaults(clientConfig *out)
{
    memset(out, 0, sizeof *out);
    out->sampleIntervalSec   = 15;
    out->watchdogIntervalSec = 5;
}

solariStatus clientConfigFromFile(const char *path, clientConfig *out)
{
    solariConfig *c = NULL;
    solariStatus rc;
    size_t n, i;

    if (!path || !out) return ERR_INVALID_ARG;
    clientConfigDefaults(out);

    rc = solariConfigLoad(path, &c);
    if (rc != SOLARI_OK) return rc;

    strncpy(out->hostFqdn, solariConfigGetStr(c, "identity", "hostFqdn", ""),
            sizeof out->hostFqdn - 1);
    out->configEpoch = solariConfigEpoch(c);
    out->sampleIntervalSec   =
        (uint32_t)solariConfigGetInt(c, "schedule", "sampleIntervalSec", 15);
    out->watchdogIntervalSec =
        (uint32_t)solariConfigGetInt(c, "schedule", "watchdogIntervalSec", 5);

    /* [watch] process = NAME   (repeated) */
    n = solariConfigCount(c, "watch", "process");
    for (i = 0; i < n && out->procCount < SOLARI_MAX_PROCS; i++) {
        const char *p = solariConfigGetStrAt(c, "watch", "process", i, "");
        if (p[0] == '\0') continue;
        strncpy(out->procs[out->procCount], p, SOLARI_PROCNAME_MAX - 1);
        out->procCount++;
    }

    /* [watch] logfile = PATH [ : REGEX ]   (repeated) */
    n = solariConfigCount(c, "watch", "logfile");
    for (i = 0; i < n && out->logCount < SOLARI_MAX_LOGS; i++) {
        const char *v = solariConfigGetStrAt(c, "watch", "logfile", i, "");
        const char *sep;
        clientLogWatch *w;
        size_t plen;
        if (v[0] == '\0') continue;
        w = &out->logs[out->logCount];
        memset(w, 0, sizeof *w);
        sep = strstr(v, " : ");                  /* "path : regex" */
        if (sep) {
            plen = (size_t)(sep - v);
            if (plen >= sizeof w->path) plen = sizeof w->path - 1;
            memcpy(w->path, v, plen);
            w->path[plen] = '\0';
            strncpy(w->regex, sep + 3, sizeof w->regex - 1);
        } else {
            strncpy(w->path, v, sizeof w->path - 1);
        }
        out->logCount++;
    }

    solariConfigFree(c);
    return SOLARI_OK;
}

solariStatus clientCollectReport(const clientConfig *cfg, clientState *st,
                                 bool catalogueOnly, solariClientReport *out)
{
    uint8_t i;

    if (!cfg || !st || !out) return ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);

    /* ---- identity (always) ---- */
    if (cfg->hostFqdn[0])
        strncpy(out->hostFqdn, cfg->hostFqdn, sizeof out->hostFqdn - 1);
    else
        platHostFqdn(out->hostFqdn, sizeof out->hostFqdn);
    platOsName(out->osName, sizeof out->osName);
    platArch(out->arch, sizeof out->arch);
    platCpuCount(&out->coreCount);

    if (catalogueOnly) return SOLARI_OK;

    /* ---- fast samples ---- */
    platCpuLoad(out->cpuLoadMilli, SOLARI_MAX_CORES, &out->coreCount);
    platMemInfo(&out->ramUsedKb, &out->ramTotalKb, &out->swapUsedKb, &out->swapTotalKb);
    platDiskFree(out->disks,   SOLARI_MAX_DISKS,  &out->diskCount);
    platNetIfaces(out->ifaces, SOLARI_MAX_IFACES, &out->ifaceCount);
    platUsbThroughput(out->usb, SOLARI_MAX_USB,   &out->usbCount);

    /* ---- watched processes ---- */
    for (i = 0; i < cfg->procCount && out->procCount < SOLARI_MAX_PROCS; i++) {
        if (platProcInspect(cfg->procs[i], &out->procs[out->procCount]) == SOLARI_OK)
            out->procCount++;
    }

    /* ---- watched logs (stateful tail offsets) ---- */
    for (i = 0; i < cfg->logCount && out->logCount < SOLARI_MAX_LOGS; i++) {
        solariLogEntry *e = &out->logs[out->logCount];
        const char *re = cfg->logs[i].regex[0] ? cfg->logs[i].regex : NULL;
        uint64_t prevOff = st->logOffset[i];
        uint64_t sizeNow = 0;
        uint32_t matches = 0;

        memset(e, 0, sizeof *e);
        strncpy(e->path, cfg->logs[i].path, sizeof e->path - 1);
        if (platLogStat(cfg->logs[i].path, re, &sizeNow, &matches,
                        &st->logOffset[i]) != SOLARI_OK) {
            solariLogf(SOLARI_LOG_DEBUG, "logStat failed for %s", cfg->logs[i].path);
            continue;
        }
        e->sizeDelta  = sizeNow >= prevOff ? sizeNow - prevOff : sizeNow;
        e->matchCount = matches;
        e->lastOffset = st->logOffset[i];
        out->logCount++;
    }
    return SOLARI_OK;
}
