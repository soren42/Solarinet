/*
 * clientReport.c - report transport with store-and-forward (sec 7.3, 6.5, 6.6).
 *
 * Compiled only when libsolari carries the nng transport and the SQLite spool
 * (CLIENT_WITH_REPORTING). The push-or-spool path is the client's fault
 * tolerance: a report is framed and pushed to the active server, and on any
 * transient/offline failure it is durably spooled and replayed in order on
 * reconnect. The frame's (sourceNodeId, seqNo) lets the server dedup replays.
 */
#include "client.h"
#include "platOS.h"

#include "solari/solariCrypto.h"
#include "solari/solariFrame.h"
#include "solari/solariLog.h"
#include "solari/solariTime.h"

#include <stdio.h>
#include <string.h>

#define CLIENT_PAYLOAD_CAP 32768
#define CLIENT_FRAME_CAP   (CLIENT_PAYLOAD_CAP + 128)
#define CLIENT_DRAIN_MAX   64           /* frames flushed per drain call */

/* ---- helpers -------------------------------------------------------------- */

static uint64_t deriveNodeId(const clientConfig *cfg)
{
    char fqdn[SOLARI_FQDN_MAX];
    char buf[SOLARI_FQDN_MAX + 8];
    int n;
    if (cfg->nodeId) return cfg->nodeId;          /* enrolled id wins */
    if (cfg->hostFqdn[0]) {
        strncpy(fqdn, cfg->hostFqdn, sizeof fqdn - 1);
        fqdn[sizeof fqdn - 1] = '\0';
    } else if (platHostFqdn(fqdn, sizeof fqdn) != SOLARI_OK) {
        strncpy(fqdn, "unknown", sizeof fqdn - 1);
        fqdn[sizeof fqdn - 1] = '\0';
    }
    n = snprintf(buf, sizeof buf, "%s|%d", fqdn, (int)ROLE_CLIENT);
    return solariFnv1a64(buf, (size_t)(n > 0 ? n : 0));
}

static void dialOpts(const clientConfig *cfg, const char *url, solariConnOpts *o)
{
    memset(o, 0, sizeof *o);
    o->url = url;
    o->pattern = SOLARI_PATTERN_PUSH;
    o->useTls  = cfg->useTls;
    o->caFile   = cfg->caFile[0]   ? cfg->caFile   : NULL;
    o->certFile = cfg->certFile[0] ? cfg->certFile : NULL;
    o->keyFile  = cfg->keyFile[0]  ? cfg->keyFile  : NULL;
    o->dialTimeoutMs = 3000;
    o->sendTimeoutMs = 2000;
    o->recvTimeoutMs = 2000;
}

/* Build a complete CLIENT_REPORT frame into out; advances ctx->seqNo. */
static solariStatus buildReportFrame(clientContext *ctx,
                                     const solariClientReport *rep, uint8_t flags,
                                     uint8_t *out, size_t cap, size_t *outLen)
{
    uint8_t payload[CLIENT_PAYLOAD_CAP];
    size_t  plen = 0;
    uint16_t tlvCount = 0;
    solariFrameHeader h;
    solariStatus rc;

    rc = solariMsgBuildClientReport(rep, payload, sizeof payload, &plen, &tlvCount);
    if (rc != SOLARI_OK) return rc;

    memset(&h, 0, sizeof h);
    h.magic[0] = SCP_MAGIC_0;
    h.magic[1] = SCP_MAGIC_1;
    h.protoVersion   = SCP_PROTO_VERSION;
    h.msgType        = SCP_MSG_CLIENT_REPORT;
    h.flags          = flags;
    h.tlvCount       = tlvCount;
    h.sourceNodeId   = ctx->nodeId;
    h.sendTimeUnixMs = solariNowUnixMs();
    h.seqNo          = ++ctx->seqNo;
    return solariFrameBuild(&h, payload, plen, out, cap, outLen);
}

/* ---- lifecycle ------------------------------------------------------------ */

solariStatus clientContextInit(clientContext *ctx, const clientConfig *cfg)
{
    if (!ctx || !cfg) return ERR_INVALID_ARG;
    memset(ctx, 0, sizeof *ctx);
    ctx->cfg    = cfg;
    ctx->nodeId = deriveNodeId(cfg);

    if (cfg->spoolDb[0]) {
        if (solariSpoolOpen(cfg->spoolDb, &ctx->spool) != SOLARI_OK) {
            solariLogf(SOLARI_LOG_WARN, "spool open failed (%s); reports drop when offline",
                       cfg->spoolDb);
            ctx->spool = NULL;
        } else {
            solariLogf(SOLARI_LOG_INFO, "spool ready (%s, depth=%zu)",
                       cfg->spoolDb, solariSpoolDepth(ctx->spool));
        }
    }
    return SOLARI_OK;
}

void clientContextClose(clientContext *ctx)
{
    if (!ctx) return;
    if (ctx->conn)  { solariConnClose(ctx->conn);   ctx->conn = NULL; }
    if (ctx->spool) { solariSpoolClose(ctx->spool); ctx->spool = NULL; }
}

solariStatus clientConnect(clientContext *ctx)
{
    solariConnOpts o;
    if (!ctx) return ERR_INVALID_ARG;
    if (ctx->conn) return SOLARI_OK;

    if (ctx->cfg->primaryUrl[0]) {
        dialOpts(ctx->cfg, ctx->cfg->primaryUrl, &o);
        if (solariConnOpen(&o, &ctx->conn) == SOLARI_OK) {
            ctx->activeUrl = 0;
            solariLogf(SOLARI_LOG_INFO, "connected (primary %s)", ctx->cfg->primaryUrl);
            return SOLARI_OK;
        }
    }
    if (ctx->cfg->failoverUrl[0]) {
        dialOpts(ctx->cfg, ctx->cfg->failoverUrl, &o);
        if (solariConnOpen(&o, &ctx->conn) == SOLARI_OK) {
            ctx->activeUrl = 1;
            solariLogf(SOLARI_LOG_WARN, "connected (failover %s)", ctx->cfg->failoverUrl);
            return SOLARI_OK;
        }
    }
    solariLogf(SOLARI_LOG_WARN, "no server reachable; operating from spool");
    return ERR_CONN_RETRY;
}

/* ---- hello ---------------------------------------------------------------- */

solariStatus clientSendHello(clientContext *ctx)
{
    solariHello hi;
    uint8_t payload[1024], frame[1152];
    size_t plen = 0, flen = 0;
    uint16_t tc = 0;
    solariFrameHeader h;
    solariStatus rc;

    if (!ctx) return ERR_INVALID_ARG;
    if (!ctx->conn) return ERR_CONN_RETRY;

    memset(&hi, 0, sizeof hi);
    hi.role = ROLE_CLIENT;
    strncpy(hi.agentVersion, ctx->cfg->agentVersion, sizeof hi.agentVersion - 1);
    if (ctx->cfg->hostFqdn[0])
        strncpy(hi.hostFqdn, ctx->cfg->hostFqdn, sizeof hi.hostFqdn - 1);
    else
        platHostFqdn(hi.hostFqdn, sizeof hi.hostFqdn);
    hi.configEpoch = ctx->cfg->configEpoch;

    rc = solariMsgBuildHello(&hi, payload, sizeof payload, &plen, &tc);
    if (rc != SOLARI_OK) return rc;

    memset(&h, 0, sizeof h);
    h.magic[0] = SCP_MAGIC_0; h.magic[1] = SCP_MAGIC_1;
    h.protoVersion = SCP_PROTO_VERSION;
    h.msgType = SCP_MSG_HELLO;
    h.flags = SCP_FLAG_ACK_REQ;            /* server WELCOMEs in reply (Phase 5) */
    h.tlvCount = tc;
    h.sourceNodeId = ctx->nodeId;
    h.sendTimeUnixMs = solariNowUnixMs();
    h.seqNo = ++ctx->seqNo;
    rc = solariFrameBuild(&h, payload, plen, frame, sizeof frame, &flen);
    if (rc != SOLARI_OK) return rc;
    return solariConnSend(ctx->conn, frame, flen);
}

/* ---- report send / spool drain ------------------------------------------- */

solariStatus clientReportSend(clientContext *ctx, const solariClientReport *rep)
{
    uint8_t frame[CLIENT_FRAME_CAP];
    size_t  flen = 0;
    solariStatus rc;

    if (!ctx || !rep) return ERR_INVALID_ARG;

    rc = buildReportFrame(ctx, rep, SCP_FLAG_NONE, frame, sizeof frame, &flen);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR, "report frame build failed: %s", solariStrError(rc));
        return rc;                                 /* never spool a bad frame */
    }

    if (ctx->conn) {
        rc = solariConnSend(ctx->conn, frame, flen);
        if (rc == SOLARI_OK) {
            clientDrainSpool(ctx);                 /* opportunistic catch-up */
            return SOLARI_OK;
        }
        if (rc == ERR_CONN_FATAL) {
            solariConnClose(ctx->conn);
            ctx->conn = NULL;
        }
        /* ERR_CONN_RETRY / fatal: fall through to spool */
    }

    if (ctx->spool) {
        rc = solariSpoolPush(ctx->spool, frame, flen);
        if (rc == SOLARI_OK)
            solariLogf(SOLARI_LOG_INFO, "server unreachable; spooled report (depth=%zu)",
                       solariSpoolDepth(ctx->spool));
        return rc;
    }

    solariLogf(SOLARI_LOG_WARN, "server unreachable and no spool; report dropped");
    return ERR_CONN_RETRY;
}

solariStatus clientDrainSpool(clientContext *ctx)
{
    int i;
    if (!ctx) return ERR_INVALID_ARG;
    if (!ctx->spool || !ctx->conn) return SOLARI_OK;

    for (i = 0; i < CLIENT_DRAIN_MAX; i++) {
        uint64_t rowId = 0;
        const uint8_t *f = NULL;
        size_t len = 0;
        solariStatus rc;

        rc = solariSpoolPeek(ctx->spool, &rowId, &f, &len);
        if (rc != SOLARI_OK) return rc;
        if (!f) return SOLARI_OK;                  /* queue empty */

        rc = solariConnSend(ctx->conn, f, len);
        if (rc == SOLARI_OK) {
            solariSpoolAck(ctx->spool, rowId);
            continue;
        }
        /* send failed: leave it queued (backoff) and stop for this cycle */
        solariSpoolNack(ctx->spool, rowId);
        if (rc == ERR_CONN_FATAL) { solariConnClose(ctx->conn); ctx->conn = NULL; }
        return SOLARI_OK;
    }
    return SOLARI_OK;
}
