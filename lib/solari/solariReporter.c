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
#include "solari/solariTlv.h"

#include <mbedtls/entropy.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REP_URL_MAX   256
#define REP_PATH_MAX  256
#define REP_DRAIN_MAX 64

struct solariReporter {
    char         primaryUrl[REP_URL_MAX];
    char         failoverUrl[REP_URL_MAX];
    char         ackUrl[REP_URL_MAX];
    char         ackFailoverUrl[REP_URL_MAX];
    bool         useTls;
    char         caFile[REP_PATH_MAX];
    char         certFile[REP_PATH_MAX];
    char         keyFile[REP_PATH_MAX];
    bool         hasCa, hasCert, hasKey;
    uint64_t     nodeId;
    uint32_t     seqNo;
    uint8_t      session[16];       /* survives spool replay in each report TLV */
    int          activeUrl;          /* 0 primary, 1 failover */
    solariConn  *conn;
    solariConn  *ackConn;
    solariSpool *spool;
};

/* A fresh incarnation keeps sequence reuse after restart/wrap unambiguous. */
static solariStatus reporterNewSession(solariReporter *r)
{
    mbedtls_entropy_context entropy;
    int result;
    mbedtls_entropy_init(&entropy);
    result = mbedtls_entropy_func(&entropy, r->session, sizeof r->session);
    mbedtls_entropy_free(&entropy);
    return result == 0 ? SOLARI_OK : ERR_TLS;
}

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

static bool deriveAckUrl(const char *primary, char *out, size_t cap)
{
    const char *colon;
    char *end;
    unsigned long port;
    size_t prefix;
    if (!primary || !out || cap == 0) return false;
    colon = strrchr(primary, ':');
    if (!colon || !colon[1]) return false;
    port = strtoul(colon + 1, &end, 10);
    if (*end != '\0' || port == 0 || port >= 65535) return false;
    prefix = (size_t)(colon - primary + 1);
    if (prefix + 6 >= cap) return false;
    memcpy(out, primary, prefix);
    snprintf(out + prefix, cap - prefix, "%lu", port + 1);
    return true;
}

solariStatus solariReporterOpen(const solariReporterOpts *opts, solariReporter **out)
{
    solariReporter *r;
    if (!opts || !out || !opts->primaryUrl) return ERR_INVALID_ARG;
    *out = NULL;

    r = calloc(1, sizeof *r);
    if (!r) return ERR_PLATFORM;
    if (reporterNewSession(r) != SOLARI_OK) { free(r); return ERR_TLS; }

    copyField(r->primaryUrl,  sizeof r->primaryUrl,  opts->primaryUrl,  NULL);
    copyField(r->failoverUrl, sizeof r->failoverUrl, opts->failoverUrl, NULL);
    if (opts->ackUrl && opts->ackUrl[0])
        copyField(r->ackUrl, sizeof r->ackUrl, opts->ackUrl, NULL);
    else if (opts->primaryUrl[0] &&
             !deriveAckUrl(opts->primaryUrl, r->ackUrl, sizeof r->ackUrl)) {
        free(r);
        return ERR_INVALID_ARG;
    }
    if (opts->ackFailoverUrl && opts->ackFailoverUrl[0])
        copyField(r->ackFailoverUrl, sizeof r->ackFailoverUrl, opts->ackFailoverUrl, NULL);
    else if (r->failoverUrl[0] &&
             !deriveAckUrl(r->failoverUrl, r->ackFailoverUrl, sizeof r->ackFailoverUrl)) {
        free(r);
        return ERR_INVALID_ARG;
    }
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
    if (r->ackConn) solariConnClose(r->ackConn);
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

static solariStatus reporterDialAck(solariReporter *r, int idx)
{
    solariConnOpts o;
    const char *url = idx == 1 ? r->ackFailoverUrl : r->ackUrl;
    if (!url[0]) return ERR_CONN_RETRY;
    if (r->ackConn) return SOLARI_OK;
    memset(&o, 0, sizeof o);
    o.url = url;
    o.pattern = SOLARI_PATTERN_REQ;
    o.useTls = r->useTls;
    o.caFile = r->hasCa ? r->caFile : NULL;
    o.certFile = r->hasCert ? r->certFile : NULL;
    o.keyFile = r->hasKey ? r->keyFile : NULL;
    o.dialTimeoutMs = 3000;
    o.sendTimeoutMs = 2000;
    o.recvTimeoutMs = 3000;
    return solariConnOpen(&o, &r->ackConn);
}

static bool isDurableReport(uint8_t msgType)
{
    return msgType == SCP_MSG_CLIENT_REPORT ||
           msgType == SCP_MSG_MONITOR_REPORT ||
           msgType == SCP_MSG_SURVEY_RESP;
}

static solariStatus reporterSendConfirmed(solariReporter *r,
                                          const uint8_t *frame, size_t len)
{
    const uint8_t *rx = NULL, *payload = NULL;
    size_t rxLen = 0, payloadLen = 0;
    solariFrameHeader sent, got;
    char peerCn[256];
    solariTlvReader reader;
    const uint8_t *value;
    uint16_t type, valueLen;
    uint8_t session[16];
    bool haveSession = false, haveAckSession = false;
    solariStatus rc;

    rc = solariFrameParse(frame, len, &sent, &payload, &payloadLen, NULL);
    if (rc != SOLARI_OK || !isDurableReport(sent.msgType)) return ERR_INVALID_ARG;
    solariTlvReaderInit(&reader, payload, payloadLen);
    while ((rc = solariTlvNext(&reader, &type, &value, &valueLen)) == SOLARI_OK) {
        if (type == TLV_REPORT_SESSION) {
            if (haveSession || valueLen != sizeof session) return ERR_INVALID_ARG;
            memcpy(session, value, sizeof session);
            haveSession = true;
        }
    }
    if (rc != ERR_TLV_END || !haveSession) return ERR_INVALID_ARG;
    {
        int first = (r->activeUrl == 1 && r->ackFailoverUrl[0]) ? 1 : 0;
        int attempt;
        for (attempt = 0; attempt < 2; attempt++) {
            int idx = attempt == 0 ? first : 1 - first;
            rc = reporterDialAck(r, idx);
            if (rc == SOLARI_OK) rc = solariConnSend(r->ackConn, frame, len);
            if (rc == SOLARI_OK) rc = solariConnRecv(r->ackConn, &rx, &rxLen);
            if (rc == SOLARI_OK) {
                if (r->useTls) {
                    rc = solariConnPeerCn(r->ackConn, peerCn, sizeof peerCn);
                    if (rc == SOLARI_OK &&
                        strcmp(peerCn, "server") != 0 && strncmp(peerCn, "server.", 7) != 0)
                        rc = ERR_AUTH_ROLE;
                }
                if (rc == SOLARI_OK)
                    rc = solariFrameParse(rx, rxLen, &got, &payload, &payloadLen, NULL);
                if (rc == SOLARI_OK && got.msgType == SCP_MSG_ACK &&
                    got.correlationId == sent.seqNo) {
                    solariTlvReaderInit(&reader, payload, payloadLen);
                    while ((rc = solariTlvNext(&reader, &type, &value, &valueLen)) == SOLARI_OK) {
                        if (type != TLV_REPORT_SESSION) continue;
                        if (haveAckSession || valueLen != sizeof session ||
                            memcmp(value, session, sizeof session) != 0) {
                            rc = ERR_CONN_RETRY;
                            break;
                        }
                        haveAckSession = true;
                    }
                    if (rc == ERR_TLV_END && haveAckSession) return SOLARI_OK;
                }
            }
            if (r->ackConn) {
                solariConnClose(r->ackConn);
                r->ackConn = NULL;
            }
        }
    }
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
    solariStatus rc;
    uint8_t *reportPayload = NULL;
    if (!r || !out || !outLen) return ERR_INVALID_ARG;
    if (r->seqNo == UINT32_MAX) {
        rc = reporterNewSession(r);
        if (rc != SOLARI_OK) return rc;
        r->seqNo = 0;
    }
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
    if (msgType == SCP_MSG_CLIENT_REPORT || msgType == SCP_MSG_MONITOR_REPORT ||
        msgType == SCP_MSG_SURVEY_RESP) {
        solariTlvWriter writer;
        if ((!payload && payloadLen) || payloadLen > SOLARI_MAX_FRAME - 20 ||
            tlvCount == UINT16_MAX) return ERR_BUFFER_FULL;
        reportPayload = (uint8_t *)malloc(payloadLen + 20);
        if (!reportPayload) return ERR_PLATFORM;
        if (payloadLen) memcpy(reportPayload, payload, payloadLen);
        solariTlvWriterInit(&writer, reportPayload, payloadLen + 20);
        writer.len = payloadLen;
        writer.count = tlvCount;
        rc = solariTlvAppend(&writer, TLV_REPORT_SESSION, r->session, sizeof r->session);
        if (rc == SOLARI_OK) {
            h.tlvCount = writer.count;
            rc = solariFrameBuild(&h, reportPayload, writer.len, out, cap, outLen);
        }
        free(reportPayload);
        return rc;
    }
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

    {
        solariFrameHeader h;
        rc = solariFrameParse(frame, len, &h, NULL, NULL, NULL);
        if (rc != SOLARI_OK) return rc;
        if (isDurableReport(h.msgType)) {
            rc = reporterSendConfirmed(r, frame, len);
            if (rc == SOLARI_OK) {
                (void)solariReporterDrain(r);
                return SOLARI_OK;
            }
            if (r->spool) {
                solariStatus qrc = solariSpoolPush(r->spool, frame, len);
                if (qrc != SOLARI_OK) return qrc;
                solariLogf(SOLARI_LOG_INFO,
                           "report unacknowledged; retained in spool (depth=%zu)",
                           solariSpoolDepth(r->spool));
                return SOLARI_OK;
            }
            return rc;
        }
    }

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
    if (!r->spool) return SOLARI_OK;

    for (i = 0; i < REP_DRAIN_MAX; i++) {
        uint64_t rowId = 0;
        const uint8_t *f = NULL;
        size_t len = 0;
        solariStatus rc = solariSpoolPeek(r->spool, &rowId, &f, &len);
        if (rc != SOLARI_OK) return rc;
        if (!f) return SOLARI_OK;                    /* queue empty */

        {
            solariFrameHeader h;
            rc = solariFrameParse(f, len, &h, NULL, NULL, NULL);
            if (rc == SOLARI_OK && isDurableReport(h.msgType))
                rc = reporterSendConfirmed(r, f, len);
            else if (rc == SOLARI_OK && r->conn)
                rc = solariConnSend(r->conn, f, len);
            else if (rc == SOLARI_OK)
                rc = ERR_CONN_RETRY;
        }
        if (rc == SOLARI_OK) {
            rc = solariSpoolAck(r->spool, rowId);
            if (rc != SOLARI_OK) return rc;
            continue;
        }
        if (solariSpoolNack(r->spool, rowId) != SOLARI_OK)
            solariLogf(SOLARI_LOG_ERROR, "spool nack failed for row=%llu",
                       (unsigned long long)rowId);
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
