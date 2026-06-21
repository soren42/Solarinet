/*
 * serverIngest.c - frame intake -> persistence (§9.1).
 *
 * Binds the PULL (+ RESPONDENT) sink, validates frames (msgType legal for the
 * presenting cert role, dedup by (sourceNodeId, seqNo)), and dispatches each
 * parsed report/advert to the right handler. seq/dedup discipline lives here.
 *
 * Two public entry points, both fed by main's recv loop after solariFrameParse
 * has already verified magic/version/CRC and split off the header + payload:
 *
 *   serverIngestValidate - the authorization + replay gate. It derives the
 *     presenting node's role from its certificate CN (leading dotted label of
 *     "<role>.<fqdn>", per §2 / solariCertCn) and rejects any frame whose
 *     msgType is illegal for that role (ERR_AUTH_ROLE). It then runs the
 *     (sourceNodeId, seqNo) dedup ring and returns ERR_CONN_RETRY (benign) for
 *     a recognised duplicate so the caller drops it without persisting twice.
 *
 *   serverIngestDispatch - the router. Each accepted msgType is parsed with the
 *     matching libsolari typed parser and handed to its owning subsystem:
 *       CLIENT_REPORT   -> serverDb writer + serverAlert (host scope)
 *       MONITOR_REPORT  -> serverDb writer + serverMaster reconcile + alerts
 *       WATCHDOG        -> serverDbTouchNode (liveness heartbeat)
 *       DISCOVERY_ADVERT-> serverDiscovery
 *       TOPOLOGY_REPORT -> serverTopology
 *       SURVEY_RESP     -> serverControl (-> master reconcile)
 *       CONTROL_RESULT  -> serverControl (convergence tracker)
 *
 * The role matrix and the dedup ring are factored into small static helpers
 * with no I/O so they are unit-testable without a live MariaDB or any sockets.
 * The ring is process-local (a fixed-size open-addressed table keyed by
 * sourceNodeId, each slot holding a SERVER_DEDUP_WINDOW-deep recent-seqNo
 * window); it is a best-effort replay guard layered over SCP_FLAG_SPOOL_REPLAY,
 * not a durable exactly-once store.
 */
#include "server.h"

#include "solari/solariCrypto.h"
#include "solari/solariLog.h"
#include "solari/solariTime.h"

#include <string.h>

/* ------------------------------------------------------------- role matrix */

/* Role implied by a presenting certificate, derived from its CN. Distinct from
 * solariRole so an unparseable / unknown CN has its own bucket that no msgType
 * is legal for. */
typedef enum {
    INGEST_ROLE_UNKNOWN = 0,
    INGEST_ROLE_CLIENT,
    INGEST_ROLE_MONITOR,
    INGEST_ROLE_SERVER
} serverIngestRole;

/* Map a certificate CN to its role. The enrollment CA issues CNs of the form
 * "<role>.<fqdn>" (e.g. "monitor.akoria.net"); the leading dotted label is the
 * authoritative role token (§2 security model). A bare "client"/"monitor"/
 * "server" CN (no dot) is also honoured for completeness. Returns
 * INGEST_ROLE_UNKNOWN for NULL / empty / unrecognised CNs. */
static serverIngestRole serverIngestRoleFromCn(const char *cn)
{
    size_t labelLen;
    const char *dot;

    if (!cn || cn[0] == '\0') return INGEST_ROLE_UNKNOWN;

    dot = strchr(cn, '.');
    labelLen = dot ? (size_t)(dot - cn) : strlen(cn);

    if (labelLen == 6 && strncmp(cn, "client", 6) == 0)  return INGEST_ROLE_CLIENT;
    if (labelLen == 7 && strncmp(cn, "monitor", 7) == 0) return INGEST_ROLE_MONITOR;
    if (labelLen == 6 && strncmp(cn, "server", 6) == 0)  return INGEST_ROLE_SERVER;
    return INGEST_ROLE_UNKNOWN;
}

/* The authorization matrix: is msgType legal coming from a node presenting
 * `role`? Only the ingest-facing message types are admitted at all; everything
 * a server *sends* (WELCOME, CONTROL, SURVEY, PRIMARY_IS, ...) is illegal as an
 * inbound frame and falls through to false.
 *
 *   CLIENT  : CLIENT_REPORT, WATCHDOG, HELLO, DISCOVERY_ADVERT, TOPOLOGY_REPORT,
 *             SURVEY_RESP, CONTROL_RESULT, WHO_IS_PRIMARY, ACK, ERROR
 *   MONITOR : the client set plus MONITOR_REPORT (a monitor is a superset host)
 *   SERVER  : peer-server traffic (HELLO, WHO_IS_PRIMARY, ACK, ERROR) - a peer
 *             server does not push host/probe reports through this sink
 *
 * A client may NOT send MONITOR_REPORT; an unknown role may send nothing. */
static bool serverIngestRoleMaySend(serverIngestRole role, uint8_t msgType)
{
    switch (role) {
    case INGEST_ROLE_MONITOR:
        if (msgType == SCP_MSG_MONITOR_REPORT) return true;
        /* fall through: a monitor may also send everything a client may */
        /* FALLTHROUGH */
    case INGEST_ROLE_CLIENT:
        switch (msgType) {
        case SCP_MSG_HELLO:
        case SCP_MSG_CLIENT_REPORT:
        case SCP_MSG_WATCHDOG:
        case SCP_MSG_DISCOVERY_ADVERT:
        case SCP_MSG_TOPOLOGY_REPORT:
        case SCP_MSG_SURVEY_RESP:
        case SCP_MSG_CONTROL_RESULT:
        case SCP_MSG_WHO_IS_PRIMARY:
        case SCP_MSG_ACK:
        case SCP_MSG_ERROR:
            return true;
        default:
            return false;
        }
    case INGEST_ROLE_SERVER:
        switch (msgType) {
        case SCP_MSG_HELLO:
        case SCP_MSG_WHO_IS_PRIMARY:
        case SCP_MSG_ACK:
        case SCP_MSG_ERROR:
            return true;
        default:
            return false;
        }
    case INGEST_ROLE_UNKNOWN:
    default:
        return false;
    }
}

/* Short label for a role, for log lines only. */
static const char *serverIngestRoleName(serverIngestRole role)
{
    switch (role) {
    case INGEST_ROLE_CLIENT:  return "client";
    case INGEST_ROLE_MONITOR: return "monitor";
    case INGEST_ROLE_SERVER:  return "server";
    case INGEST_ROLE_UNKNOWN:
    default:                  return "unknown";
    }
}

/* ------------------------------------------------------------- dedup ring */

/*
 * Per-source recent-seqNo guard. SCP seqNo is per-source monotonic (§5.5), but
 * store-and-forward replay (SCP_FLAG_SPOOL_REPLAY) and at-least-once delivery
 * can re-present a frame we already persisted. We keep, per source node, a ring
 * of the last SERVER_DEDUP_WINDOW seqNos seen; a hit means "already processed".
 *
 * The source table is a fixed open-addressed array keyed by sourceNodeId. It is
 * deliberately bounded and process-local: under churn the least-recently-touched
 * source's window is recycled, which at worst lets a very old replay through
 * once. This is a defensive de-dup layer, not a durable ledger.
 */
#define SERVER_DEDUP_SOURCES 256   /* distinct source nodes tracked at once */

typedef struct {
    uint64_t nodeId;                         /* 0 == empty slot              */
    uint64_t lastTouchUnixMs;                /* for LRU recycling            */
    uint32_t seq[SERVER_DEDUP_WINDOW];       /* recent seqNos (ring)         */
    uint16_t head;                           /* next write index             */
    uint16_t fill;                           /* valid entries (<= WINDOW)    */
} serverDedupSource;

static serverDedupSource g_dedup[SERVER_DEDUP_SOURCES];

/* Find the slot tracking `nodeId`, or claim one for it (recycling the LRU slot
 * when full). Never returns NULL. */
static serverDedupSource *serverDedupSlot(uint64_t nodeId, uint64_t nowMs)
{
    size_t i;
    size_t freeIdx = SERVER_DEDUP_SOURCES;   /* first empty slot, if any   */
    size_t lruIdx  = 0;                        /* oldest-touched fallback    */
    uint64_t lruTime = UINT64_MAX;
    serverDedupSource *s;

    for (i = 0; i < SERVER_DEDUP_SOURCES; ++i) {
        if (g_dedup[i].nodeId == nodeId && nodeId != 0) {
            g_dedup[i].lastTouchUnixMs = nowMs;
            return &g_dedup[i];
        }
        if (g_dedup[i].nodeId == 0 && freeIdx == SERVER_DEDUP_SOURCES)
            freeIdx = i;
        if (g_dedup[i].lastTouchUnixMs < lruTime) {
            lruTime = g_dedup[i].lastTouchUnixMs;
            lruIdx  = i;
        }
    }

    s = &g_dedup[(freeIdx < SERVER_DEDUP_SOURCES) ? freeIdx : lruIdx];
    memset(s, 0, sizeof *s);
    s->nodeId          = nodeId;
    s->lastTouchUnixMs = nowMs;
    return s;
}

/* True if (nodeId, seqNo) was seen recently. On a miss, records it and returns
 * false. Pure aside from the static ring; safe to call with nodeId==0 (always a
 * miss, never recorded, since a 0 source id is not a real enrolled node). */
static bool serverDedupSeen(uint64_t nodeId, uint32_t seqNo, uint64_t nowMs)
{
    serverDedupSource *s;
    uint16_t i;

    if (nodeId == 0) return false;

    s = serverDedupSlot(nodeId, nowMs);
    for (i = 0; i < s->fill; ++i) {
        if (s->seq[i] == seqNo) return true;
    }

    s->seq[s->head] = seqNo;
    s->head = (uint16_t)((s->head + 1u) % SERVER_DEDUP_WINDOW);
    if (s->fill < SERVER_DEDUP_WINDOW) s->fill++;
    return false;
}

/* ------------------------------------------------------------- validation */

solariStatus serverIngestValidate(serverContext *ctx,
                                  const solariFrameHeader *hdr,
                                  const char *peerCertCn)
{
    serverIngestRole role;
    uint64_t nowMs;

    if (!ctx || !hdr) return ERR_INVALID_ARG;

    /* 1. Authorization: the msgType must be legal for the presenting cert role.
     *    A missing / unparseable CN maps to UNKNOWN, for which nothing is legal. */
    role = serverIngestRoleFromCn(peerCertCn);
    if (!serverIngestRoleMaySend(role, hdr->msgType)) {
        solariLogf(SOLARI_LOG_WARN,
                   "ingest reject: role=%s cn=%s msgType=0x%02x src=0x%llx (ERR_AUTH_ROLE)",
                   serverIngestRoleName(role), peerCertCn ? peerCertCn : "(none)",
                   (unsigned)hdr->msgType,
                   (unsigned long long)hdr->sourceNodeId);
        return ERR_AUTH_ROLE;
    }

    /* 2. Replay: drop a frame whose (sourceNodeId, seqNo) we already processed.
     *    Reported as the benign ERR_CONN_RETRY so the caller simply discards. */
    nowMs = solariNowUnixMs();
    if (serverDedupSeen(hdr->sourceNodeId, hdr->seqNo, nowMs)) {
        solariLogf(SOLARI_LOG_DEBUG,
                   "ingest dup: src=0x%llx seq=%u msgType=0x%02x (dropped)",
                   (unsigned long long)hdr->sourceNodeId,
                   (unsigned)hdr->seqNo, (unsigned)hdr->msgType);
        return ERR_CONN_RETRY;
    }

    return SOLARI_OK;
}

/* ------------------------------------------------------------- dispatch */

/* CLIENT_REPORT (0x10): persist the host snapshot, then evaluate host-scoped
 * alert rules. A persistence failure is fatal to the frame; an alert-eval
 * failure is logged but not propagated (the data is already durable). */
static solariStatus serverIngestClientReport(serverContext *ctx,
                                             const solariFrameHeader *hdr,
                                             const uint8_t *payload, size_t len)
{
    solariClientReport rep;
    solariStatus rc;
    bool urgent = (hdr->flags & SCP_FLAG_URGENT) != 0;

    rc = solariMsgParseClientReport(payload, len, &rep);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_WARN,
                   "ingest CLIENT_REPORT parse failed src=0x%llx: %s",
                   (unsigned long long)hdr->sourceNodeId, solariStrError(rc));
        return rc;
    }

    rc = serverDbWriteClientReport(ctx, hdr->sourceNodeId, &rep);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "ingest CLIENT_REPORT persist failed src=0x%llx: %s",
                   (unsigned long long)hdr->sourceNodeId, solariStrError(rc));
        return rc;
    }

    rc = serverAlertEvalClient(ctx, hdr->sourceNodeId, &rep, urgent);
    if (rc != SOLARI_OK)
        solariLogf(SOLARI_LOG_WARN, "ingest CLIENT_REPORT alert eval src=0x%llx: %s",
                   (unsigned long long)hdr->sourceNodeId, solariStrError(rc));

    return SOLARI_OK;
}

/* MONITOR_REPORT (0x11): persist probe results, reconcile overlapping vantages
 * into authoritative state, then evaluate probe-scoped alerts. Persistence is
 * fatal; reconcile + alert failures are logged, non-fatal. */
static solariStatus serverIngestMonitorReport(serverContext *ctx,
                                              const solariFrameHeader *hdr,
                                              const uint8_t *payload, size_t len)
{
    solariMonitorReport rep;
    solariStatus rc;
    bool urgent = (hdr->flags & SCP_FLAG_URGENT) != 0;

    rc = solariMsgParseMonitorReport(payload, len, &rep);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_WARN,
                   "ingest MONITOR_REPORT parse failed src=0x%llx: %s",
                   (unsigned long long)hdr->sourceNodeId, solariStrError(rc));
        return rc;
    }

    rc = serverDbWriteMonitorReport(ctx, hdr->sourceNodeId, &rep);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "ingest MONITOR_REPORT persist failed src=0x%llx: %s",
                   (unsigned long long)hdr->sourceNodeId, solariStrError(rc));
        return rc;
    }

    rc = serverMasterReconcile(ctx, hdr->sourceNodeId, &rep);
    if (rc != SOLARI_OK)
        solariLogf(SOLARI_LOG_WARN, "ingest MONITOR_REPORT reconcile src=0x%llx: %s",
                   (unsigned long long)hdr->sourceNodeId, solariStrError(rc));

    rc = serverAlertEvalMonitor(ctx, hdr->sourceNodeId, &rep, urgent);
    if (rc != SOLARI_OK)
        solariLogf(SOLARI_LOG_WARN, "ingest MONITOR_REPORT alert eval src=0x%llx: %s",
                   (unsigned long long)hdr->sourceNodeId, solariStrError(rc));

    return SOLARI_OK;
}

/* WATCHDOG (0x12): a bare liveness heartbeat for a supervised client. No TLV
 * body to parse - just touch the node's lastSeenAt with the frame's stamp. */
static solariStatus serverIngestWatchdog(serverContext *ctx,
                                         const solariFrameHeader *hdr)
{
    uint64_t whenMs = hdr->sendTimeUnixMs ? hdr->sendTimeUnixMs : solariNowUnixMs();
    solariStatus rc = serverDbTouchNode(ctx->db, hdr->sourceNodeId, whenMs);
    if (rc != SOLARI_OK)
        solariLogf(SOLARI_LOG_WARN, "ingest WATCHDOG touch src=0x%llx: %s",
                   (unsigned long long)hdr->sourceNodeId, solariStrError(rc));
    return rc;
}

solariStatus serverIngestDispatch(serverContext *ctx,
                                  const solariFrameHeader *hdr,
                                  const uint8_t *payload, size_t len)
{
    if (!ctx || !hdr) return ERR_INVALID_ARG;
    if (len > 0 && !payload) return ERR_INVALID_ARG;

    switch (hdr->msgType) {
    case SCP_MSG_CLIENT_REPORT:
        return serverIngestClientReport(ctx, hdr, payload, len);

    case SCP_MSG_MONITOR_REPORT:
        return serverIngestMonitorReport(ctx, hdr, payload, len);

    case SCP_MSG_WATCHDOG:
        return serverIngestWatchdog(ctx, hdr);

    case SCP_MSG_DISCOVERY_ADVERT:
        return serverDiscoveryOnAdvert(ctx, hdr->sourceNodeId, payload, len);

    case SCP_MSG_TOPOLOGY_REPORT:
        return serverTopologyOnReport(ctx, hdr->sourceNodeId, payload, len);

    case SCP_MSG_SURVEY_RESP:
        return serverControlOnSurveyResp(ctx, hdr->sourceNodeId, payload, len);

    case SCP_MSG_CONTROL_RESULT:
        return serverControlOnResult(ctx, hdr->sourceNodeId,
                                     hdr->correlationId, payload, len);

    default:
        solariLogf(SOLARI_LOG_WARN,
                   "ingest dispatch: unhandled msgType=0x%02x src=0x%llx",
                   (unsigned)hdr->msgType,
                   (unsigned long long)hdr->sourceNodeId);
        return ERR_UNKNOWN_MSG;
    }
}
