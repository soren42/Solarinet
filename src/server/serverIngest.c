/*
 * serverIngest.c - frame intake -> persistence (§9.1).
 *
 * Binds the PULL (+ RESPONDENT) sink, validates frames (msgType legal for the
 * presenting cert role, durable report identity), and dispatches each
 * parsed report/advert to the right handler. seq/dedup discipline lives here.
 *
 * Two public entry points, both fed by main's recv loop after solariFrameParse
 * has already verified magic/version/CRC and split off the header + payload:
 *
 *   serverIngestValidate - the authorization + replay gate. It derives the
 *     presenting node's role from its certificate CN (leading dotted label of
 *     "<role>.<fqdn>", per §2 / solariCertCn) and rejects any frame whose
 *     msgType is illegal for that role (ERR_AUTH_ROLE). It then runs the
 *     (sourceNodeId, seqNo) ring for non-report traffic. Reports bypass that
 *     process-local ring and reserve a session/sequence receipt in their DB txn.
 *
 *   serverIngestDispatch - the router. Each accepted msgType is parsed with the
 *     matching libsolari typed parser and handed to its owning subsystem:
 *       CLIENT_REPORT   -> serverDb writer + serverAlert (host scope)
 *       MONITOR_REPORT  -> serverDb writer + serverMaster reconcile + alerts
 *       WATCHDOG        -> serverDbTouchNode (liveness heartbeat)
 *       DISCOVERY_ADVERT-> serverDiscovery
 *       TOPOLOGY_REPORT -> serverTopology
 *       SURVEY_RESP     -> monitor report persistence + master reconcile
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
#include "solari/solariTlv.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

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

/* A HELLO's self-declared identity must match the already-authenticated
 * role.fqdn certificate identity. This prevents a valid node from changing the
 * authoritative role or hostFqdn columns by sending a forged HELLO body. */
bool serverIngestHelloMatchesIdentity(solariRole role, const char *hostFqdn,
                                      const char *peerCertCn)
{
    const char *roleLabel;
    const char *expectedHost;
    const char *certHost;
    const char *dot;
    char expectedCn[SERVER_CN_MAX];
    int n;

    if (!hostFqdn || !hostFqdn[0] || !peerCertCn || !peerCertCn[0]) return false;
    switch (role) {
    case ROLE_CLIENT:  roleLabel = "client"; break;
    case ROLE_MONITOR: roleLabel = "monitor"; break;
    case ROLE_SERVER:  roleLabel = "server"; break;
    default: return false;
    }
    /* During the approved migration window, an existing enrolled leaf may
     * carry only its host FQDN. serverIngestValidate has already resolved its
     * role from that node's exact enrollment row before this HELLO check. */
    if (strcasecmp(hostFqdn, peerCertCn) == 0) return true;
    n = snprintf(expectedCn, sizeof expectedCn, "%s.%s", roleLabel, hostFqdn);
    if (n <= 0 || (size_t)n >= sizeof expectedCn) return false;
    dot = strchr(expectedCn, '.');
    if (!dot) return false;
    expectedHost = dot + 1;
    if (strncmp(expectedCn, peerCertCn, (size_t)(expectedHost - expectedCn)) != 0)
        return false;  /* role label is case-sensitive */
    certHost = peerCertCn + (expectedHost - expectedCn);
    return certHost[0] != '\0' && strcasecmp(expectedHost, hostFqdn) == 0 &&
           strcasecmp(expectedHost, certHost) == 0;
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

/* True if (nodeId, seqNo) is in the source's recent ring. nodeId 0 is not a
 * real enrolled node, so it is always a miss. */
static bool serverDedupHas(uint64_t nodeId, uint32_t seqNo, uint64_t nowMs)
{
    serverDedupSource *s;
    uint16_t i;

    if (nodeId == 0) return false;

    s = serverDedupSlot(nodeId, nowMs);
    for (i = 0; i < s->fill; ++i) {
        if (s->seq[i] == seqNo) return true;
    }
    return false;
}

/* Record (nodeId, seqNo) without a lookup; callers check serverDedupHas first. */
static void serverDedupMark(uint64_t nodeId, uint32_t seqNo, uint64_t nowMs)
{
    serverDedupSource *s;

    if (nodeId == 0) return;

    s = serverDedupSlot(nodeId, nowMs);
    s->seq[s->head] = seqNo;
    s->head = (uint16_t)((s->head + 1u) % SERVER_DEDUP_WINDOW);
    if (s->fill < SERVER_DEDUP_WINDOW) s->fill++;
}

/* True if (nodeId, seqNo) was seen recently. On a miss, records it and returns
 * false. Pure aside from the static ring; safe to call with nodeId==0 (always a
 * miss, never recorded, since a 0 source id is not a real enrolled node). */
static bool serverDedupSeen(uint64_t nodeId, uint32_t seqNo, uint64_t nowMs)
{
    if (serverDedupHas(nodeId, seqNo, nowMs)) return true;
    serverDedupMark(nodeId, seqNo, nowMs);
    return false;
}

/* ------------------------------------------------------------- validation */

solariStatus serverIngestValidate(serverContext *ctx,
                                  const solariFrameHeader *hdr,
                                  const char *peerCertCn)
{
    serverIngestRole role;
    solariRole enrolledRole = (solariRole)0;
    char enrolledHost[SOLARI_FQDN_MAX];
    const char *cnHost = NULL;
    uint64_t nowMs;
    solariStatus identityStatus;

    if (!ctx || !hdr) return ERR_INVALID_ARG;

    /* Resolve identity against the active enrollment before inferring a role.
     * For role-prefixed leaves, the DB role, CN role, and CN host must all
     * agree. Existing host-only leaves inherit only the role enrolled on this
     * exact node ID and are accepted only while the stored CN equals FQDN. */
    if (!peerCertCn || !peerCertCn[0] || !ctx->db) return ERR_AUTH_ROLE;
    identityStatus = serverDbNodeIdentity(ctx->db, hdr->sourceNodeId,
                                           peerCertCn, &enrolledRole,
                                           enrolledHost, sizeof enrolledHost);
    if (identityStatus != SOLARI_OK) {
        solariLogf(SOLARI_LOG_WARN,
                   "ingest reject: cert identity does not match node src=0x%llx (%s)",
                   (unsigned long long)hdr->sourceNodeId,
                   identityStatus == ERR_AUTH_ROLE ? "not enrolled" : "lookup failed");
        return identityStatus;
    }
    role = serverIngestRoleFromCn(peerCertCn);
    if (strcasecmp(peerCertCn, enrolledHost) == 0) {
        switch (enrolledRole) {
        case ROLE_CLIENT: role = INGEST_ROLE_CLIENT; break;
        case ROLE_MONITOR: role = INGEST_ROLE_MONITOR; break;
        case ROLE_SERVER: role = INGEST_ROLE_SERVER; break;
        default: role = INGEST_ROLE_UNKNOWN; break;
        }
    } else {
        const char *dot = strchr(peerCertCn, '.');
        cnHost = dot ? dot + 1 : NULL;
        if (!cnHost || strcasecmp(cnHost, enrolledHost) != 0) {
            role = INGEST_ROLE_UNKNOWN;
        } else if ((role == INGEST_ROLE_CLIENT && enrolledRole != ROLE_CLIENT) ||
                   (role == INGEST_ROLE_MONITOR && enrolledRole != ROLE_MONITOR) ||
                   (role == INGEST_ROLE_SERVER && enrolledRole != ROLE_SERVER)) {
            role = INGEST_ROLE_UNKNOWN;
        }
    }

    /* 1. Authorization: the msgType must be legal for the enrolled role.
     *    Unknown or mismatched identity maps to UNKNOWN, which can send nothing. */
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
    if (hdr->msgType != SCP_MSG_CLIENT_REPORT && hdr->msgType != SCP_MSG_MONITOR_REPORT &&
        hdr->msgType != SCP_MSG_SURVEY_RESP &&
        serverDedupSeen(hdr->sourceNodeId, hdr->seqNo, nowMs)) {
        solariLogf(SOLARI_LOG_DEBUG,
                   "ingest dup: src=0x%llx seq=%u msgType=0x%02x (dropped)",
                   (unsigned long long)hdr->sourceNodeId,
                   (unsigned)hdr->seqNo, (unsigned)hdr->msgType);
        return ERR_CONN_RETRY;
    }

    return SOLARI_OK;
}

/* ------------------------------------------------------------- dispatch */

static bool serverIngestLowerHex(const char *value, size_t length)
{
    size_t i;
    for (i = 0; i < length; ++i)
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'))) return false;
    return true;
}

static solariStatus serverIngestParseCrlActivation(const uint8_t *payload,
                                                    size_t payloadLen,
                                                    bool *found,
                                                    char issuerFp[65],
                                                    uint64_t *crlNumber,
                                                    char crlFp[65])
{
    solariTlvReader reader;
    const uint8_t *value;
    uint16_t type, valueLen;
    solariStatus rc;

    if ((!payload && payloadLen) || !found || !issuerFp || !crlNumber || !crlFp)
        return ERR_INVALID_ARG;
    *found = false;
    issuerFp[0] = '\0'; crlFp[0] = '\0'; *crlNumber = 0;
    solariTlvReaderInit(&reader, payload, payloadLen);
    while ((rc = solariTlvNext(&reader, &type, &value, &valueLen)) == SOLARI_OK) {
        char text[151];
        char *first, *second;
        size_t i, digits;
        uint64_t number = 0;
        if (type != TLV_CRL_ACTIVATION) continue;
        if (*found || valueLen < 131 || valueLen > 150) return ERR_INVALID_ARG;
        memcpy(text, value, valueLen);
        text[valueLen] = '\0';
        first = strchr(text, ':');
        if (!first || first - text != 64) return ERR_INVALID_ARG;
        second = strchr(first + 1, ':');
        if (!second || second - first < 2 || strlen(second + 1) != 64 ||
            strchr(second + 1, ':')) return ERR_INVALID_ARG;
        digits = (size_t)(second - first - 1);
        if (digits > 20 || !serverIngestLowerHex(text, 64) ||
            !serverIngestLowerHex(second + 1, 64) ||
            (digits > 1 && first[1] == '0')) return ERR_INVALID_ARG;
        for (i = 0; i < digits; ++i) {
            unsigned digit;
            char c = first[1 + i];
            if (c < '0' || c > '9') return ERR_INVALID_ARG;
            digit = (unsigned)(c - '0');
            if (number > (UINT64_MAX - digit) / 10u) return ERR_INVALID_ARG;
            number = number * 10u + digit;
        }
        if (!number) return ERR_INVALID_ARG;
        memcpy(issuerFp, text, 64); issuerFp[64] = '\0';
        memcpy(crlFp, second + 1, 64); crlFp[64] = '\0';
        *crlNumber = number;
        *found = true;
    }
    return rc == ERR_TLV_END ? SOLARI_OK : rc;
}

static solariStatus serverIngestApplyCrlActivation(serverContext *ctx,
                                                    const solariFrameHeader *hdr,
                                                    const uint8_t *payload,
                                                    size_t payloadLen)
{
    char issuerFp[65], crlFp[65];
    uint64_t crlNumber = 0;
    bool found = false;
    solariStatus rc = serverIngestParseCrlActivation(payload, payloadLen, &found,
                                                      issuerFp, &crlNumber, crlFp);
    if (rc != SOLARI_OK) {
        /* Receipt metadata is optional. The report was persisted before this
         * helper runs, so malformed metadata must not turn an accepted report
         * into a retry loop. The parser still rejects it for focused tests. */
        solariLogf(SOLARI_LOG_WARN,
                   "ingest ignored malformed CRL receipt src=0x%llx status=%s",
                   (unsigned long long)hdr->sourceNodeId, solariStrError(rc));
        return SOLARI_OK;
    }
    if (!found) return SOLARI_OK;
    rc = serverDbCertificateCrlAcknowledge(ctx->db, hdr->sourceNodeId,
                                            issuerFp, crlNumber, crlFp);
    if (rc == ERR_REJECTED) {
        solariLogf(SOLARI_LOG_INFO,
                   "ingest rejected stale or unpublished CRL receipt src=0x%llx issuer=%s number=%llu",
                   (unsigned long long)hdr->sourceNodeId, issuerFp,
                   (unsigned long long)crlNumber);
        return SOLARI_OK; /* accept the report; only the stale receipt is ignored */
    }
    if (rc != SOLARI_OK) {
        /* Acknowledgement persistence is supplemental to a report already
         * committed. Later reports carry the receipt again, so a DB outage or
         * unapplied migration must not make the durable report retry forever. */
        solariLogf(SOLARI_LOG_WARN,
                   "ingest could not store CRL receipt src=0x%llx issuer=%s number=%llu status=%s",
                   (unsigned long long)hdr->sourceNodeId, issuerFp,
                   (unsigned long long)crlNumber, solariStrError(rc));
    }
    return SOLARI_OK;
}

/* Reports carry a restart-safe identity. Receipt insertion happens only in
 * the same transaction as current/history writes, never during validation.
 * allowLegacy admits a session-less report only when it was sent one-way
 * (no ACK requested), which is how pre-session agents deliver. */
static solariStatus serverReportIdentity(const solariFrameHeader *hdr,
                                         const uint8_t *payload, size_t len,
                                         bool allowLegacy,
                                         serverReportReceipt *receipt)
{
    solariTlvReader reader;
    const uint8_t *value;
    uint16_t type, length;
    bool found = false;
    solariStatus rc;
    memset(receipt, 0, sizeof *receipt);
    receipt->seqNo = hdr->seqNo;
    receipt->msgType = hdr->msgType;
    solariTlvReaderInit(&reader, payload, len);
    while ((rc = solariTlvNext(&reader, &type, &value, &length)) == SOLARI_OK) {
        if (type != TLV_REPORT_SESSION) continue;
        if (found || length != sizeof receipt->session) return ERR_INVALID_ARG;
        memcpy(receipt->session, value, sizeof receipt->session);
        found = true;
    }
    if (rc != ERR_TLV_END) return rc;
    if (!found) {
        if (!allowLegacy || (hdr->flags & SCP_FLAG_ACK_REQ) != 0) return ERR_INVALID_ARG;
        receipt->legacy = true;
    }
    solariSha256(payload, len, receipt->payloadHash);
    return SOLARI_OK;
}

/* Identity for a durable report. A legacy report has no receipt, so it falls
 * back to the in-memory seqNo replay guard; a replay is ERR_CONN_RETRY. The
 * caller marks the seqNo only after commit, so a failed write is not mistaken
 * for a replay. Survey responses never predate sessions and stay strict. */
static solariStatus serverReportAdmit(serverContext *ctx,
                                      const solariFrameHeader *hdr,
                                      const uint8_t *payload, size_t len,
                                      serverReportReceipt *receipt)
{
    bool allowLegacy = ctx->cfg && ctx->cfg->allowLegacyReports &&
                       (hdr->msgType == SCP_MSG_CLIENT_REPORT ||
                        hdr->msgType == SCP_MSG_MONITOR_REPORT);
    solariStatus rc = serverReportIdentity(hdr, payload, len, allowLegacy, receipt);
    if (rc != SOLARI_OK || !receipt->legacy) return rc;
    if (serverDedupHas(hdr->sourceNodeId, hdr->seqNo, solariNowUnixMs()))
        return ERR_CONN_RETRY;
    solariLogf(SOLARI_LOG_DEBUG, "ingest legacy report src=0x%llx seq=%u msgType=0x%02x",
               (unsigned long long)hdr->sourceNodeId, (unsigned)hdr->seqNo,
               (unsigned)hdr->msgType);
    return SOLARI_OK;
}

/* CLIENT_REPORT (0x10): persist the host snapshot, then evaluate host-scoped
 * alert rules. A persistence failure is fatal to the frame; an alert-eval
 * failure is logged but not propagated (the data is already durable). */
static solariStatus serverIngestClientReport(serverContext *ctx,
                                             const solariFrameHeader *hdr,
                                             const uint8_t *payload, size_t len)
{
    solariClientReport rep;
    serverReportReceipt receipt;
    solariStatus rc;
    bool urgent = (hdr->flags & SCP_FLAG_URGENT) != 0;

    rc = solariMsgParseClientReport(payload, len, &rep);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_WARN,
                   "ingest CLIENT_REPORT parse failed src=0x%llx: %s",
                   (unsigned long long)hdr->sourceNodeId, solariStrError(rc));
        return rc;
    }

    rc = serverReportAdmit(ctx, hdr, payload, len, &receipt);
    if (rc == ERR_CONN_RETRY) return SOLARI_OK;    /* legacy replay */
    if (rc != SOLARI_OK) return rc;
    rc = serverDbWriteClientReport(ctx, hdr->sourceNodeId, &rep, &receipt);
    if (rc == ERR_CONN_RETRY) return SOLARI_OK;    /* already committed; no repeat alerts */
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "ingest CLIENT_REPORT persist failed src=0x%llx: %s",
                   (unsigned long long)hdr->sourceNodeId, solariStrError(rc));
        return rc;
    }
    if (receipt.legacy)
        serverDedupMark(hdr->sourceNodeId, hdr->seqNo, solariNowUnixMs());

    /* A client that just reported is, by definition, reachable: mark it up and
     * advance lastSeenAt so the dashboard reflects a live agent (best-effort;
     * derived health for probe targets is reconciled separately). */
    (void)serverDbSetNodeState(ctx->db, hdr->sourceNodeId, "up");
    (void)serverDbTouchNode(ctx->db, hdr->sourceNodeId,
                            hdr->sendTimeUnixMs ? hdr->sendTimeUnixMs : solariNowUnixMs());

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
    serverReportReceipt receipt;
    solariStatus rc;
    bool urgent = (hdr->flags & SCP_FLAG_URGENT) != 0;

    rc = solariMsgParseMonitorReport(payload, len, &rep);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_WARN,
                   "ingest MONITOR_REPORT parse failed src=0x%llx: %s",
                   (unsigned long long)hdr->sourceNodeId, solariStrError(rc));
        return rc;
    }

    rc = serverReportAdmit(ctx, hdr, payload, len, &receipt);
    if (rc == ERR_CONN_RETRY) return SOLARI_OK;    /* legacy replay */
    if (rc != SOLARI_OK) return rc;
    rc = serverDbWriteMonitorReport(ctx, hdr->sourceNodeId, &rep, &receipt);
    if (rc == ERR_CONN_RETRY) return SOLARI_OK;
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "ingest MONITOR_REPORT persist failed src=0x%llx: %s",
                   (unsigned long long)hdr->sourceNodeId, solariStrError(rc));
        return rc;
    }
    if (receipt.legacy)
        serverDedupMark(hdr->sourceNodeId, hdr->seqNo, solariNowUnixMs());

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
    case SCP_MSG_CLIENT_REPORT: {
        solariStatus rc = serverIngestClientReport(ctx, hdr, payload, len);
        return rc == SOLARI_OK ? serverIngestApplyCrlActivation(ctx, hdr, payload, len) : rc;
    }

    case SCP_MSG_MONITOR_REPORT: {
        solariStatus rc = serverIngestMonitorReport(ctx, hdr, payload, len);
        return rc == SOLARI_OK ? serverIngestApplyCrlActivation(ctx, hdr, payload, len) : rc;
    }

    case SCP_MSG_WATCHDOG:
        return serverIngestWatchdog(ctx, hdr);

    case SCP_MSG_DISCOVERY_ADVERT:
        return serverDiscoveryOnAdvert(ctx, hdr->sourceNodeId, payload, len);

    case SCP_MSG_TOPOLOGY_REPORT:
        return serverTopologyOnReport(ctx, hdr->sourceNodeId, payload, len);

    case SCP_MSG_SURVEY_RESP: {
        solariStatus rc = serverIngestMonitorReport(ctx, hdr, payload, len);
        return rc == SOLARI_OK ? serverIngestApplyCrlActivation(ctx, hdr, payload, len) : rc;
    }

    case SCP_MSG_CONTROL_RESULT:
        return serverControlOnResult(ctx, hdr->sourceNodeId,
                                     hdr->correlationId, payload, len);

    case SCP_MSG_HELLO: {
        /* A node that pushes HELLO over the one-way ingest (rather than the
         * control REQ/REP) still registers/refreshes its node row. WELCOME can't
         * be returned here (PULL is one-way); the node converges via control. */
        solariHello hello;
        uint8_t     wbuf[1024];
        size_t      wlen = 0;
        uint16_t    wtc = 0;
        solariStatus rc = solariMsgParseHello(payload, len, &hello);
        if (rc != SOLARI_OK) {
            solariLogf(SOLARI_LOG_WARN, "ingest HELLO parse failed src=0x%llx: %s",
                       (unsigned long long)hdr->sourceNodeId, solariStrError(rc));
            return rc;
        }
        return serverMasterAcceptHello(ctx, hdr, &hello, NULL,
                                       wbuf, sizeof wbuf, &wlen, &wtc);
    }

    default:
        solariLogf(SOLARI_LOG_WARN,
                   "ingest dispatch: unhandled msgType=0x%02x src=0x%llx",
                   (unsigned)hdr->msgType,
                   (unsigned long long)hdr->sourceNodeId);
        return ERR_UNKNOWN_MSG;
    }
}
