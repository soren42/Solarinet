/*
 * monitorReport.c - monitor reporting on the shared transport (sec 8.1).
 *
 * Mirrors the client: derive the node id, build the MONITOR_REPORT payload, and
 * hand frames to solariReporter (libsolari), which pushes them live or durably
 * spools and replays them on reconnect. Compiled only when reporting is
 * available (MONITOR_WITH_REPORTING == SOLARI_WITH_IO && SOLARI_WITH_SQLITE).
 */
#include "monitor.h"

#include "solari/solariFrame.h"
#include "solari/solariLog.h"

#include <string.h>

#define MON_PAYLOAD_CAP 32768
#define MON_FRAME_CAP   (MON_PAYLOAD_CAP + 128)

solariStatus monitorContextInit(monitorContext *ctx, const monitorConfig *cfg)
{
    solariReporterOpts o;
    if (!ctx || !cfg) return ERR_INVALID_ARG;
    memset(ctx, 0, sizeof *ctx);
    ctx->cfg    = cfg;
    ctx->nodeId = monitorNodeId(cfg);

    memset(&o, 0, sizeof o);
    o.primaryUrl  = cfg->primaryUrl;
    o.failoverUrl = cfg->failoverUrl[0] ? cfg->failoverUrl : NULL;
    o.useTls      = cfg->useTls;
    o.caFile      = cfg->caFile[0]   ? cfg->caFile   : NULL;
    o.certFile    = cfg->certFile[0] ? cfg->certFile : NULL;
    o.keyFile     = cfg->keyFile[0]  ? cfg->keyFile  : NULL;
    o.spoolDb     = cfg->spoolDb[0]  ? cfg->spoolDb  : NULL;
    o.nodeId      = ctx->nodeId;
    return solariReporterOpen(&o, &ctx->rep);
}

void monitorContextClose(monitorContext *ctx)
{
    if (ctx && ctx->rep) { solariReporterClose(ctx->rep); ctx->rep = NULL; }
}

solariStatus monitorConnect(monitorContext *ctx)
{
    if (!ctx) return ERR_INVALID_ARG;
    return solariReporterConnect(ctx->rep);
}

solariStatus monitorReportSend(monitorContext *ctx, const solariMonitorReport *rep)
{
    uint8_t payload[MON_PAYLOAD_CAP], frame[MON_FRAME_CAP];
    size_t plen = 0, flen = 0;
    uint16_t tc = 0;
    solariStatus rc;

    if (!ctx || !rep) return ERR_INVALID_ARG;
    rc = solariMsgBuildMonitorReport(rep, payload, sizeof payload, &plen, &tc);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR, "monitor report build failed: %s", solariStrError(rc));
        return rc;
    }
    rc = solariReporterFrame(ctx->rep, SCP_MSG_MONITOR_REPORT, SCP_FLAG_NONE,
                             payload, plen, tc, frame, sizeof frame, &flen);
    if (rc != SOLARI_OK) return rc;
    return solariReporterSend(ctx->rep, frame, flen);    /* push or durably spool */
}

solariStatus monitorDrainSpool(monitorContext *ctx)
{
    if (!ctx) return ERR_INVALID_ARG;
    return solariReporterDrain(ctx->rep);
}
