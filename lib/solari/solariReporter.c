/*
 * solariReporter.c - shared push-or-spool report transport (sec 6.5, 6.6).
 *
 * One implementation of the dial / frame / send-or-spool / drain cycle, used by
 * both solariClient and solariMonitor. Built into libsolari only when the nng
 * transport and the SQLite spool are compiled in (SOLARI_WITH_IO &&
 * SOLARI_WITH_SQLITE).
 */
#include "solari/solariReporter.h"
#include "solari/solariNet.h"
#include "solari/solariSpool.h"
#include "solari/solariFrame.h"
#include "solari/solariLog.h"
#include "solari/solariTime.h"

#include <stdlib.h>
#include <string.h>

#define REP_URL_MAX   256
#define REP_PATH_MAX  256
#define REP_DRAIN_MAX 64

struct solariReporter {
    char         primaryUrl[REP_URL_MAX];
    char         failoverUrl[REP_URL_MAX];
    bool         useTls;
    char         caFile[REP_PATH_MAX];
    char         certFile[REP_PATH_MAX];
    char         keyFile[REP_PATH_MAX];
    bool         hasCa, hasCert, hasKey;
    uint64_t     nodeId;
    uint32_t     seqNo;
    int          activeUrl;          /* 0 primary, 1 failover */
    solariConn  *conn;
    solariSpool *spool;
};

static void copyField(char *dst, size_t cap, const char *src, bool *present)
{
    dst[0] = '\0';
    if (present) *present = false;
    if (src && src[0]) {
        strncpy(dst, src, cap - 1);
        dst[cap - 1] = '\0';
        if (present) *present = true;
    }
}

solariStatus solariReporterOpen(const solariReporterOpts *opts, solariReporter **out)
{
    solariReporter *r;
    if (!opts || !out || !opts->primaryUrl) return ERR_INVALID_ARG;

    r = calloc(1, sizeof *r);
    if (!r) return ERR_PLATFORM;

    copyField(r->primaryUrl,  sizeof r->primaryUrl,  opts->primaryUrl,  NULL);
    copyField(r->failoverUrl, sizeof r->failoverUrl, opts->failoverUrl, NULL);
    copyField(r->caFile,   sizeof r->caFile,   opts->caFile,   &r->hasCa);
    copyField(r->certFile, sizeof r->certFile, opts->certFile, &r->hasCert);
    copyField(r->keyFile,  sizeof r->keyFile,  opts->keyFile,  &r->hasKey);
    r->useTls = opts->useTls;
    r->nodeId = opts->nodeId;

    if (opts->spoolDb && opts->spoolDb[0]) {
        if (solariSpoolOpen(opts->spoolDb, &r->spool) != SOLARI_OK) {
            solariLogf(SOLARI_LOG_WARN, "spool open failed (%s); reports drop when offline",
                       opts->spoolDb);
            r->spool = NULL;
        } else {
            solariLogf(SOLARI_LOG_INFO, "spool ready (%s, depth=%zu)",
                       opts->spoolDb, solariSpoolDepth(r->spool));
        }
    }
    *out = r;
    return SOLARI_OK;
}

void solariReporterClose(solariReporter *r)
{
    if (!r) return;
    if (r->conn)  solariConnClose(r->conn);
    if (r->spool) solariSpoolClose(r->spool);
    free(r);
}

static solariStatus dialUrl(solariReporter *r, const char *url, int idx)
{
    solariConnOpts o;
    memset(&o, 0, sizeof o);
    o.url      = url;
    o.pattern  = SOLARI_PATTERN_PUSH;
    o.useTls   = r->useTls;
    o.caFile   = r->hasCa   ? r->caFile   : NULL;
    o.certFile = r->hasCert ? r->certFile : NULL;
    o.keyFile  = r->hasKey  ? r->keyFile  : NULL;
    o.dialTimeoutMs = 3000;
    o.sendTimeoutMs = 2000;
    o.recvTimeoutMs = 2000;
    if (solariConnOpen(&o, &r->conn) == SOLARI_OK) { r->activeUrl = idx; return SOLARI_OK; }
    return ERR_CONN_RETRY;
}

solariStatus solariReporterConnect(solariReporter *r)
{
    if (!r) return ERR_INVALID_ARG;
    if (r->conn) return SOLARI_OK;
    if (r->primaryUrl[0] && dialUrl(r, r->primaryUrl, 0) == SOLARI_OK) {
        solariLogf(SOLARI_LOG_INFO, "connected (primary %s)", r->primaryUrl);
        return SOLARI_OK;
    }
    if (r->failoverUrl[0] && dialUrl(r, r->failoverUrl, 1) == SOLARI_OK) {
        solariLogf(SOLARI_LOG_WARN, "connected (failover %s)", r->failoverUrl);
        return SOLARI_OK;
    }
    solariLogf(SOLARI_LOG_WARN, "no server reachable; operating from spool");
    return ERR_CONN_RETRY;
}

bool solariReporterConnected(const solariReporter *r)
{
    return r && r->conn != NULL;
}

solariStatus solariReporterFrameCorr(solariReporter *r, uint8_t msgType, uint8_t flags,
                                     uint32_t correlationId,
                                     const uint8_t *payload, size_t payloadLen,
                                     uint16_t tlvCount,
                                     uint8_t *out, size_t cap, size_t *outLen)
{
    solariFrameHeader h;
    if (!r || !out || !outLen) return ERR_INVALID_ARG;
    memset(&h, 0, sizeof h);
    h.magic[0] = SCP_MAGIC_0;
    h.magic[1] = SCP_MAGIC_1;
    h.protoVersion   = SCP_PROTO_VERSION;
    h.msgType        = msgType;
    h.flags          = flags;
    h.tlvCount       = tlvCount;
    h.sourceNodeId   = r->nodeId;
    h.sendTimeUnixMs = solariNowUnixMs();
    h.seqNo          = ++r->seqNo;
    h.correlationId  = correlationId;
    return solariFrameBuild(&h, payload, payloadLen, out, cap, outLen);
}

solariStatus solariReporterFrame(solariReporter *r, uint8_t msgType, uint8_t flags,
                                 const uint8_t *payload, size_t payloadLen,
                                 uint16_t tlvCount,
                                 uint8_t *out, size_t cap, size_t *outLen)
{
    return solariReporterFrameCorr(r, msgType, flags, 0,
                                   payload, payloadLen, tlvCount, out, cap, outLen);
}

solariStatus solariReporterSend(solariReporter *r, const uint8_t *frame, size_t len)
{
    solariStatus rc;
    if (!r || !frame) return ERR_INVALID_ARG;

    if (r->conn) {
        rc = solariConnSend(r->conn, frame, len);
        if (rc == SOLARI_OK) {
            solariReporterDrain(r);                 /* opportunistic catch-up */
            return SOLARI_OK;
        }
        if (rc == ERR_CONN_FATAL) { solariConnClose(r->conn); r->conn = NULL; }
        /* transient/offline: fall through to spool */
    }
    if (r->spool) {
        rc = solariSpoolPush(r->spool, frame, len);
        if (rc == SOLARI_OK)
            solariLogf(SOLARI_LOG_INFO, "server unreachable; spooled frame (depth=%zu)",
                       solariSpoolDepth(r->spool));
        return rc;
    }
    return ERR_CONN_RETRY;                           /* offline and no spool */
}

solariStatus solariReporterSendEphemeral(solariReporter *r, const uint8_t *frame, size_t len)
{
    solariStatus rc;
    if (!r || !frame) return ERR_INVALID_ARG;
    if (!r->conn) return ERR_CONN_RETRY;
    rc = solariConnSend(r->conn, frame, len);
    if (rc == ERR_CONN_FATAL) { solariConnClose(r->conn); r->conn = NULL; }
    return rc;
}

solariStatus solariReporterDrain(solariReporter *r)
{
    int i;
    if (!r) return ERR_INVALID_ARG;
    if (!r->spool || !r->conn) return SOLARI_OK;

    for (i = 0; i < REP_DRAIN_MAX; i++) {
        uint64_t rowId = 0;
        const uint8_t *f = NULL;
        size_t len = 0;
        solariStatus rc = solariSpoolPeek(r->spool, &rowId, &f, &len);
        if (rc != SOLARI_OK) return rc;
        if (!f) return SOLARI_OK;                    /* queue empty */

        rc = solariConnSend(r->conn, f, len);
        if (rc == SOLARI_OK) { solariSpoolAck(r->spool, rowId); continue; }
        solariSpoolNack(r->spool, rowId);
        if (rc == ERR_CONN_FATAL) { solariConnClose(r->conn); r->conn = NULL; }
        return SOLARI_OK;                            /* stop; retry next cycle */
    }
    return SOLARI_OK;
}

size_t solariReporterSpoolDepth(const solariReporter *r)
{
    if (!r || !r->spool) return 0;
    /* solariSpoolDepth takes a non-const handle; the depth query is read-only. */
    return solariSpoolDepth(((solariReporter *)r)->spool);
}
