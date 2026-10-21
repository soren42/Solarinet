/*
 * serverProvision.c - enrollment + lifecycle (Handoff §7.3, §7.4).
 *
 * Owns the enrollment state machine (token->pending->approved/rejected) with
 * CSR signing via the internal CA through solariCtl; emits CTRL_PROVISION
 * (bring-up), CTRL_DECOMMISSION (guarded teardown with one-time confirm token +
 * audit row, carried by SCP_MSG_DECOMMISSION), CTRL_ADOPT_TARGET dispatch, and
 * tracks build convergence for /api/builds.
 *
 * Design note (testability, §17): wire payloads use small builders; the
 * decommission confirmation itself is a durable DB state transition. Its
 * 64-bit wire token comes from the OS CSPRNG, expires after ten minutes, and is
 * stored only as a SHA-256 digest. Confirmation consumes it and retires the
 * node atomically.
 */
#include "server.h"

#include "solari/solariCrypto.h"
#include "solari/solariError.h"
#include "solari/solariLog.h"
#include "solari/solariNet.h"
#include "solari/solariTime.h"
#include "solari/solariTlv.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/* The server's own protocol version stamp for frames it originates. The shared
 * header does not re-export SCP_PROTO_VERSION's numeric value through a server
 * symbol, so we pin it to the wire constant defined in solariCommon.h. */
#ifndef SCP_PROTO_VERSION
#define SCP_PROTO_VERSION 1
#endif

/* Upper bound for a single control/decommission frame we build here. A control
 * payload is a handful of small TLVs plus an opaque blob; cap generously but
 * well under SOLARI_MAX_FRAME. */
#define PROV_FRAME_CAP 4096
/* Largest opaque control payload (config JSON / probe spec) we will carry. */
#define PROV_PAYLOAD_CAP 2048
/* Cap for a signed cert PEM coming back from the CA. */
#define PROV_CERT_PEM_CAP 8192
/* Cap for a CSR PEM held pending signature. */
#define PROV_CSR_PEM_CAP 8192

static uint64_t provNodeIdForIdentity(const char *hostFqdn, solariRole role)
{
    return solariNodeIdFromFqdnRole(hostFqdn, role);
}

/* ===================================================================== */
/* Pure helpers (no I/O) - unit-testable in isolation                     */
/* ===================================================================== */

static solariStatus provRandomConfirmToken(uint64_t *token)
{
    int fd;
    size_t off = 0;
    uint8_t bytes[8];
    int i;
    uint64_t value = 0;

    if (!token) return ERR_INVALID_ARG;
    *token = 0;
    fd = open("/dev/urandom", O_RDONLY
#ifdef O_CLOEXEC
              | O_CLOEXEC
#endif
    );
    if (fd < 0) return ERR_PLATFORM;
    do {
        off = 0;
        value = 0;
        while (off < sizeof bytes) {
            ssize_t n = read(fd, bytes + off, sizeof bytes - off);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { close(fd); return ERR_PLATFORM; }
            off += (size_t)n;
        }
        for (i = 0; i < 8; ++i) value = (value << 8) | bytes[i];
    } while (value == 0);
    close(fd);
    memset(bytes, 0, sizeof bytes);
    *token = value;
    return SOLARI_OK;
}

static void provConfirmTokenHash(uint64_t token, char out[65])
{
    char text[24];
    uint8_t digest[32];
    (void)snprintf(text, sizeof text, "%llu", (unsigned long long)token);
    solariSha256(text, strlen(text), digest);
    solariHexEncode(digest, sizeof digest, out, 65);
    memset(text, 0, sizeof text);
    memset(digest, 0, sizeof digest);
}

/* Build a CTRL_PROVISION (verb 7) control payload: verb + target epoch + the
 * addressee node (the directive rides the broadcast PUB channel, so the target
 * is in-payload) + the opaque config blob. Pure TLV assembly over a caller
 * buffer. */
static solariStatus provBuildProvisionPayload(uint64_t targetNode,
                                              uint64_t targetEpoch,
                                              const uint8_t *cfgBlob,
                                              uint16_t cfgLen,
                                              uint8_t *out, size_t cap,
                                              size_t *outLen, uint16_t *count)
{
    solariTlvWriter w;
    solariStatus st;

    solariTlvWriterInit(&w, out, cap);
    if ((st = solariTlvAppendU8 (&w, TLV_CTRL_VERB, (uint8_t)CTRL_PROVISION)) != SOLARI_OK)
        return st;
    if ((st = solariTlvAppendU64(&w, TLV_CTRL_TARGET_EPOCH, targetEpoch)) != SOLARI_OK)
        return st;
    if (targetNode != 0) {
        if ((st = solariTlvAppendU64(&w, TLV_CTRL_TARGET_NODE, targetNode)) != SOLARI_OK)
            return st;
    }
    if (cfgLen > 0) {
        if ((st = solariTlvAppend(&w, TLV_CTRL_PAYLOAD, cfgBlob, cfgLen)) != SOLARI_OK)
            return st;
    }
    *outLen = w.len;
    *count  = w.count;
    return SOLARI_OK;
}

/* Build the SCP_MSG_DECOMMISSION (0x32) payload: verb + addressed node ID +
 * challenge generation + wipe-scope bitfield. The target TLV is required
 * because this frame is published to the fleet-wide SUB channel. */
static solariStatus provBuildDecommissionPayload(uint64_t targetNode,
                                                 uint64_t generation,
                                                 uint32_t wipeScope,
                                                 uint8_t *out, size_t cap,
                                                 size_t *outLen, uint16_t *count)
{
    solariTlvWriter w;
    solariStatus st;

    solariTlvWriterInit(&w, out, cap);
    if ((st = solariTlvAppendU8 (&w, TLV_CTRL_VERB, (uint8_t)CTRL_DECOMMISSION)) != SOLARI_OK)
        return st;
    if ((st = solariTlvAppendU64(&w, TLV_CTRL_TARGET_NODE, targetNode)) != SOLARI_OK)
        return st;
    if ((st = solariTlvAppendU64(&w, TLV_LIFE_GENERATION, generation)) != SOLARI_OK)
        return st;
    if ((st = solariTlvAppendU8 (&w, TLV_LIFE_WIPE_SCOPE,
                                 (uint8_t)(wipeScope & 0xFFu))) != SOLARI_OK)
        return st;
    *outLen = w.len;
    *count  = w.count;
    return SOLARI_OK;
}

/* Build a CTRL_ADOPT_TARGET (verb 9) control payload: verb + the probe-spec
 * blob (no epoch convergence - the monitor just adds a live target). Pure. */
static solariStatus provBuildAdoptPayload(const char *probeSpec,
                                          uint8_t *out, size_t cap,
                                          size_t *outLen, uint16_t *count)
{
    solariTlvWriter w;
    solariStatus st;

    solariTlvWriterInit(&w, out, cap);
    if ((st = solariTlvAppendU8(&w, TLV_CTRL_VERB, (uint8_t)CTRL_ADOPT_TARGET)) != SOLARI_OK)
        return st;
    /* Preserve the monitor target grammar verbatim, including HTTP
     * path|status and any trailing " : label" suffix. */
    if (probeSpec && probeSpec[0]) {
        if ((st = solariTlvAppendStr(&w, TLV_CTRL_PAYLOAD, probeSpec)) != SOLARI_OK)
            return st;
    }
    *outLen = w.len;
    *count  = w.count;
    return SOLARI_OK;
}

/* FNV-1a-64 HRW weight for a (targetId, nodeId) pair, mirroring the monitor's
 * rendezvous keying (monitorPeer.c) so the server selects the same owners the
 * fleet would self-elect. Pure. */
static uint64_t provHrwWeight(const char *targetId, uint64_t nodeId)
{
    uint8_t buf[SERVER_TARGETID_MAX + 8];
    size_t n = strlen(targetId);
    int i;
    if (n > SERVER_TARGETID_MAX) n = SERVER_TARGETID_MAX;
    memcpy(buf, targetId, n);
    for (i = 0; i < 8; i++) buf[n + i] = (uint8_t)(nodeId >> (i * 8));
    return solariFnv1a64(buf, n + (size_t)8);
}

/* Select the single highest-weight monitor for targetId from a fleet list,
 * ties broken by nodeId (same rule as monitorOwnsTarget). Returns the chosen
 * node id, or 0 if the fleet is empty. Pure. */
static uint64_t provHrwTopOwner(const char *targetId,
                                const uint64_t *fleet, size_t fleetLen)
{
    uint64_t best = 0, bestW = 0;
    size_t i;
    for (i = 0; i < fleetLen; i++) {
        uint64_t w = provHrwWeight(targetId, fleet[i]);
        if (best == 0 || w > bestW || (w == bestW && fleet[i] > best)) {
            best  = fleet[i];
            bestW = w;
        }
    }
    return best;
}

/* ===================================================================== */
/* Frame assembly + transport                                             */
/* ===================================================================== */

/* Stamp a server-originated frame header for `msgType`/`targetCorrelation` and
 * assemble [len|hdr|payload|crc] into out. correlationId is left 0 for an
 * unsolicited directive. The seqNo is the wall clock low 32 bits - monotonic
 * enough for dedup within a source without threading a counter through here. */
static solariStatus provAssembleFrame(serverContext *ctx, uint8_t msgType,
                                      uint16_t tlvCount,
                                      const uint8_t *payload, size_t payloadLen,
                                      uint8_t *out, size_t cap, size_t *outLen)
{
    solariFrameHeader hdr;
    uint64_t now = solariNowUnixMs();

    memset(&hdr, 0, sizeof hdr);
    hdr.magic[0]       = SCP_MAGIC_0;
    hdr.magic[1]       = SCP_MAGIC_1;
    hdr.protoVersion   = SCP_PROTO_VERSION;
    hdr.msgType        = msgType;
    hdr.flags          = SCP_FLAG_ACK_REQ;   /* control wants its result echoed */
    hdr.reserved       = 0;
    hdr.tlvCount       = tlvCount;
    hdr.sourceNodeId   = ctx->nodeId;
    hdr.sendTimeUnixMs = now;
    hdr.seqNo          = (uint32_t)(now & 0xFFFFFFFFu);
    hdr.correlationId  = 0;

    return solariFrameBuild(&hdr, payload, payloadLen, out, cap, outLen);
}

/* Emit a built frame on the control plane. If the control conn is not bound
 * (this server is STANDBY, or sockets are down) the frame is logged and treated
 * as a transient retry rather than a hard failure - the caller has already
 * persisted intent to the DB, which is the source of truth (§7.3). */
static solariStatus provSendControl(serverContext *ctx,
                                    const uint8_t *frame, size_t len)
{
    solariStatus st;
    /* Server->node directives are PUBLISHED on the fleet PUB channel. The REP
     * control socket only answers node-initiated requests (who-is-primary,
     * control acks) and cannot originate an unsolicited send, so it is the wrong
     * channel for a push. Nodes SUB to the PUB endpoint and act on directives
     * addressed to them; their CONTROL_RESULT returns over the ingest path. A
     * PUB send succeeds even with no current subscribers (the desired state is
     * already persisted and will reconverge), so this never fails the push. */
    if (!ctx->pub) {
        solariLogf(SOLARI_LOG_WARN,
                   "provision: no pub conn bound; directive deferred (%zu B)",
                   len);
        return ERR_CONN_RETRY;
    }
    st = solariConnSend(ctx->pub, frame, len);
    if (st != SOLARI_OK) {
        solariLogf(SOLARI_LOG_WARN,
                   "provision: directive publish failed: %s "
                   "(persisted; will reconverge)", solariStrError(st));
    }
    return st;
}

static uint64_t provDecommissionRetryDelayMs(uint32_t previousAttempts)
{
    if (previousAttempts >= 6) return UINT64_C(300000);
    return UINT64_C(5000) << previousAttempts;
}

/* Publish one persisted decommission generation and advance its durable retry
 * schedule before sending. If the process exits during publication, the saved
 * deadline bounds the next attempt; an ack/cancel makes the row ineligible. */
static solariStatus provDispatchDecommission(serverContext *ctx,
                                             const serverDecommissionDelivery *job,
                                             uint64_t now)
{
    uint8_t payload[PROV_PAYLOAD_CAP];
    uint8_t frame[PROV_FRAME_CAP];
    size_t payloadLen = 0, frameLen = 0;
    uint16_t tlvCount = 0;
    uint64_t delayMs;
    uint64_t nextRetry;
    solariStatus st;

    if (!ctx || !ctx->db || !job || !job->nodeId || !job->generation || !now)
        return ERR_INVALID_ARG;
    delayMs = provDecommissionRetryDelayMs(job->deliveryAttempts);
    nextRetry = now + delayMs;
    st = provBuildDecommissionPayload(job->nodeId, job->generation,
                                       job->wipeScope, payload, sizeof payload,
                                       &payloadLen, &tlvCount);
    if (st != SOLARI_OK) return st;
    st = provAssembleFrame(ctx, SCP_MSG_DECOMMISSION, tlvCount,
                           payload, payloadLen, frame, sizeof frame, &frameLen);
    if (st != SOLARI_OK) return st;
    st = serverDbDecommissionNoteAttempt(ctx->db, job->nodeId, job->generation,
                                         now, nextRetry);
    if (st != SOLARI_OK) return st;
    return provSendControl(ctx, frame, frameLen);
}

void serverProvisionRetryDecommissions(serverContext *ctx, uint64_t nowUnixMs)
{
    enum { RETRY_BATCH = 16 };
    serverDecommissionDelivery jobs[RETRY_BATCH];
    size_t count = 0, i;
    solariStatus st;

    if (!ctx || !ctx->db || ctx->role != SRV_ACTIVE) return;
    if (!nowUnixMs) nowUnixMs = solariNowUnixMs();
    st = serverDbDecommissionListDue(ctx->db, nowUnixMs, jobs,
                                     RETRY_BATCH, &count);
    if (st != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/decommission: retry scan failed: %s",
                   solariStrError(st));
        return;
    }
    for (i = 0; i < count; i++) {
        st = provDispatchDecommission(ctx, &jobs[i], nowUnixMs);
        if (st != SOLARI_OK && st != ERR_CONN_RETRY)
            solariLogf(SOLARI_LOG_WARN,
                       "provision/decommission: retry failed node=%llu generation=%llu: %s",
                       (unsigned long long)jobs[i].nodeId,
                       (unsigned long long)jobs[i].generation,
                       solariStrError(st));
    }
}

/* ===================================================================== */
/* Enrollment state machine (§7.3)                                        */
/* ===================================================================== */

solariStatus serverProvisionApprove(serverContext *ctx, uint64_t enrId,
                                    const char *operator_)
{
    serverEnrollment enr;
    char csrPem[PROV_CSR_PEM_CAP];
    char certPem[PROV_CERT_PEM_CAP];
    char leafCertFp[65], leafCertSerial[41], leafIssuerFp[65];
    char expectedCn[SERVER_CN_MAX];
    const char *roleLabel;
    int cnLen;
    uint64_t nodeId;
    uint64_t now;
    solariStatus st;

    if (!ctx || !ctx->db || enrId == 0) return ERR_INVALID_ARG;
    if (!operator_) operator_ = "";

    memset(&enr, 0, sizeof enr);
    if ((st = serverDbEnrollGet(ctx->db, enrId, &enr)) != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/approve: enroll %llu not found: %s",
                   (unsigned long long)enrId, solariStrError(st));
        return st;
    }
    /* State guard: only a PENDING row (CSR attached) may be approved. */
    if (enr.status != ENROLL_PENDING) {
        solariLogf(SOLARI_LOG_WARN,
                   "provision/approve: enroll %llu in status %d, not PENDING",
                   (unsigned long long)enrId, (int)enr.status);
        return ERR_AUTH_ROLE;   /* nearest "illegal in this state" code */
    }

    /* Pull the held CSR and sign it with the internal CA (key stays in ctl). */
    csrPem[0] = '\0';
    if ((st = serverDbEnrollGetCsr(ctx->db, enrId, csrPem, sizeof csrPem)) != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/approve: no CSR for enroll %llu: %s",
                   (unsigned long long)enrId, solariStrError(st));
        return st;
    }
    if (!ctx->ctl) {
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/approve: CA bridge (solariCtl) unavailable");
        return ERR_TLS;
    }
    switch (enr.role) {
    case ROLE_CLIENT:  roleLabel = "client"; break;
    case ROLE_MONITOR: roleLabel = "monitor"; break;
    case ROLE_SERVER:  roleLabel = "server"; break;
    default:
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/approve: enrollment %llu has invalid role",
                   (unsigned long long)enrId);
        return ERR_AUTH_ROLE;
    }
    cnLen = snprintf(expectedCn, sizeof expectedCn, "%s.%s", roleLabel, enr.host);
    if (cnLen < 0 || (size_t)cnLen >= sizeof expectedCn) {
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/approve: enrollment %llu identity is too long",
                   (unsigned long long)enrId);
        return ERR_BUFFER_FULL;
    }
    nodeId = provNodeIdForIdentity(enr.host, enr.role);
    if (!nodeId) {
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/approve: enrollment %llu has invalid stable identity",
                   (unsigned long long)enrId);
        return ERR_INVALID_ARG;
    }
    certPem[0] = '\0';
    leafCertFp[0] = leafCertSerial[0] = leafIssuerFp[0] = '\0';
    if ((st = serverCtlSignCsr(ctx->ctl, csrPem, expectedCn, enr.host,
                               certPem, sizeof certPem,
                               leafCertFp, sizeof leafCertFp,
                               leafCertSerial, sizeof leafCertSerial,
                               leafIssuerFp, sizeof leafIssuerFp)) != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/approve: CA sign or identity validation failed for enroll %llu: %s",
                   (unsigned long long)enrId, solariStrError(st));
        return st;
    }

    now = solariNowUnixMs();

    /* Persist the issued certificate, node identity, and approval together.
     * The database compare-and-set prevents duplicate or stale approvals. */
    if ((st = serverDbEnrollFinalize(ctx->db, enrId, nodeId, enr.role,
                                     enr.host, expectedCn, certPem,
                                     leafCertFp, leafCertSerial, leafIssuerFp,
                                     operator_, now)) != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/approve: atomic finalize enroll %llu: %s",
                   (unsigned long long)enrId, solariStrError(st));
        return st;
    }

    /* Audit the approval alongside the alert/audit trail. */
    (void)serverDbWriteAlertEvent(ctx->db, 0, enrId, NULL, "info",
                                  "enrollment approved", now, "audit", 0, 0, 2, "suppress", NULL);

    solariLogf(SOLARI_LOG_INFO,
               "provision/approve: enroll %llu host=%s node=0x%llx approved by %s, cert issued",
               (unsigned long long)enrId, enr.host,
               (unsigned long long)nodeId, operator_);
    return SOLARI_OK;
}

solariStatus serverProvisionReject(serverContext *ctx, uint64_t enrId,
                                   const char *operator_)
{
    serverEnrollment enr;
    uint64_t now;
    solariStatus st;

    if (!ctx || !ctx->db || enrId == 0) return ERR_INVALID_ARG;
    if (!operator_) operator_ = "";

    memset(&enr, 0, sizeof enr);
    if ((st = serverDbEnrollGet(ctx->db, enrId, &enr)) != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/reject: enroll %llu not found: %s",
                   (unsigned long long)enrId, solariStrError(st));
        return st;
    }
    /* A terminal row may not be re-decided. */
    if (enr.status == ENROLL_APPROVED || enr.status == ENROLL_REJECTED) {
        solariLogf(SOLARI_LOG_WARN,
                   "provision/reject: enroll %llu already decided (status %d)",
                   (unsigned long long)enrId, (int)enr.status);
        return ERR_AUTH_ROLE;
    }

    now = solariNowUnixMs();
    if ((st = serverDbEnrollDecide(ctx->db, enrId, ENROLL_REJECTED,
                                   operator_, now)) != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/reject: decide(REJECTED) enroll %llu: %s",
                   (unsigned long long)enrId, solariStrError(st));
        return st;
    }
    /* Drop any held CSR; a rejected request must not retain key material. */
    (void)serverDbEnrollClearCsr(ctx->db, enrId);
    (void)serverDbWriteAlertEvent(ctx->db, 0, enrId, NULL, "warn",
                                  "enrollment rejected", now, "audit", 0, 0, 2, "suppress", NULL);

    solariLogf(SOLARI_LOG_INFO,
               "provision/reject: enroll %llu host=%s rejected by %s",
               (unsigned long long)enrId, enr.host, operator_);
    return SOLARI_OK;
}

/* ===================================================================== */
/* Bring-up: CTRL_PROVISION (§7.3)                                        */
/* ===================================================================== */

solariStatus serverProvisionNode(serverContext *ctx, uint64_t nodeId,
                                 uint64_t buildId, const char *configJson,
                                 uint64_t targetEpoch)
{
    uint8_t payload[PROV_PAYLOAD_CAP];
    uint8_t frame[PROV_FRAME_CAP];
    size_t  payloadLen = 0, frameLen = 0;
    uint16_t tlvCount = 0;
    uint16_t cfgLen;
    size_t  cfgRaw;
    solariStatus st;

    if (!ctx || !ctx->db || nodeId == 0) return ERR_INVALID_ARG;
    if (!configJson) configJson = "";

    cfgRaw = strlen(configJson);
    if (cfgRaw > PROV_PAYLOAD_CAP - 64) {       /* leave room for the TLV frame */
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/node: config blob too large (%zu B) for node %llu",
                   cfgRaw, (unsigned long long)nodeId);
        return ERR_BUFFER_FULL;
    }
    cfgLen = (uint16_t)cfgRaw;

    /* Persist the target epoch + config first; this is what makes provisioning
     * idempotent and what convergence is measured against (§7.3). */
    if ((st = serverDbSetNodeTargetEpoch(ctx->db, nodeId, targetEpoch,
                                         configJson)) != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/node: set target epoch %llu for node %llu: %s",
                   (unsigned long long)targetEpoch, (unsigned long long)nodeId,
                   solariStrError(st));
        return st;
    }

    /* Build the CTRL_PROVISION directive carrying the config blob. */
    if ((st = provBuildProvisionPayload(nodeId, targetEpoch,
                                        (const uint8_t *)configJson, cfgLen,
                                        payload, sizeof payload,
                                        &payloadLen, &tlvCount)) != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/node: build payload for node %llu: %s",
                   (unsigned long long)nodeId, solariStrError(st));
        return st;
    }
    if ((st = provAssembleFrame(ctx, SCP_MSG_CONTROL, tlvCount,
                                payload, payloadLen,
                                frame, sizeof frame, &frameLen)) != SOLARI_OK) {
        return st;
    }

    st = provSendControl(ctx, frame, frameLen);
    /* A deferred send (STANDBY / link down) is not an error: the directive is
     * persisted and will reconverge. */
    if (st == ERR_CONN_RETRY) st = SOLARI_OK;

    solariLogf(SOLARI_LOG_INFO,
               "provision/node: node=%llu build=%llu epoch=%llu provisioned (%zu B)",
               (unsigned long long)nodeId, (unsigned long long)buildId,
               (unsigned long long)targetEpoch, frameLen);
    return st;
}

/* ===================================================================== */
/* Guarded decommission (§7.3) + retire                                   */
/* ===================================================================== */

solariStatus serverProvisionDecommission(serverContext *ctx, uint64_t nodeId,
                                         uint32_t wipeScope, const char *operator_,
                                         uint64_t *confirmToken)
{
    uint64_t token, now;
    char tokenHash[65];
    char detail[128];
    solariStatus st;

    if (confirmToken) *confirmToken = 0;
    if (!ctx || !ctx->db || nodeId == 0) return ERR_INVALID_ARG;
    if (!operator_) operator_ = "";
    /* A scope of 0 erases nothing; require at least one bit (caller mistake). */
    if ((wipeScope & 0x1Fu) == 0) {
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/decommission: empty wipe scope for node %llu",
                   (unsigned long long)nodeId);
        return ERR_INVALID_ARG;
    }

    if (!confirmToken || !operator_[0]) return ERR_INVALID_ARG;
    now = solariNowUnixMs();
    if ((st = provRandomConfirmToken(&token)) != SOLARI_OK) return st;
    provConfirmTokenHash(token, tokenHash);

    /* Audit BEFORE the destructive directive leaves the server (§7.3): the
     * alertEvent row is the durable record that a teardown was authorized, and
     * it must exist even if the send or the node's wipe later fails. */
    (void)snprintf(detail, sizeof detail,
                   "decommission authorized by %s scope=0x%02x",
                   operator_, (unsigned)(wipeScope & 0xFFu));
    if ((st = serverDbWriteAlertEvent(ctx->db, 0, nodeId, NULL, "crit",
                                      detail, now, "audit", 0, 0, 2, "suppress", NULL)) != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/decommission: audit row write failed for node %llu: %s",
                   (unsigned long long)nodeId, solariStrError(st));
        return st;   /* refuse to proceed without the audit trail */
    }

    /* Persist the challenge before returning it. No destructive directive is
     * sent until a separate call consumes this exact challenge. */
    st = serverDbDecommissionIssue(ctx->db, nodeId, wipeScope, operator_,
                                  tokenHash, now, now + UINT64_C(600000), NULL);
    memset(tokenHash, 0, sizeof tokenHash);
    if (st != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/decommission: challenge persistence failed for node %llu",
                   (unsigned long long)nodeId);
        return st;
    }

    *confirmToken = token;

    solariLogf(SOLARI_LOG_WARN,
               "provision/decommission: node=%llu scope=0x%02x by=%s challenge issued",
               (unsigned long long)nodeId,
               (unsigned)(wipeScope & 0xFFu), operator_);
    return SOLARI_OK;
}

solariStatus serverProvisionConfirmDecommission(serverContext *ctx,
                                                uint64_t nodeId,
                                                uint32_t wipeScope,
                                                const char *operator_,
                                                uint64_t confirmToken)
{
    serverDecommissionDelivery job;
    char tokenHash[65];
    uint64_t now;
    solariStatus st;

    if (!ctx || !ctx->db || nodeId == 0 || !operator_ || !operator_[0] ||
        confirmToken == 0 || (wipeScope & 0x1Fu) == 0)
        return ERR_INVALID_ARG;
    provConfirmTokenHash(confirmToken, tokenHash);
    now = solariNowUnixMs();
    uint64_t generation = 0;
    st = serverDbDecommissionConfirm(ctx->db, nodeId, wipeScope, operator_,
                                     tokenHash, now, &generation);
    memset(tokenHash, 0, sizeof tokenHash);
    if (st != SOLARI_OK) return st;
    /* Confirmation consumes the challenge. The durable retry worker owns
     * delivery from this point, and retirement waits for a matching ack. */
    memset(&job, 0, sizeof job);
    job.nodeId = nodeId;
    job.wipeScope = wipeScope;
    job.generation = generation;
    st = provDispatchDecommission(ctx, &job, now);
    (void)serverDbWriteAlertEvent(ctx->db, 0, nodeId, NULL, "warn",
                                  st == SOLARI_OK
                                      ? "decommission confirmed; directive published; awaiting node ack"
                                      : "decommission confirmed; delivery retry scheduled",
                                  now, "audit", 0, 0, 2,
                                  "suppress", NULL);
    solariLogf(SOLARI_LOG_INFO,
               "provision/decommission: node=%llu generation=%llu confirmation consumed; retry scheduled%s",
               (unsigned long long)nodeId,
               (unsigned long long)generation,
               st == SOLARI_OK ? "" : " after initial delivery failure");
    /* Persistence of the consumed challenge is the acceptance boundary. Even
     * if this immediate attempt failed, the active-server loop will retry it. */
    return SOLARI_OK;
}

static bool provCertificateRevocationReasonValid(const char *reason)
{
    return reason &&
        (strcmp(reason, "unspecified") == 0 ||
         strcmp(reason, "keyCompromise") == 0 ||
         strcmp(reason, "affiliationChanged") == 0 ||
         strcmp(reason, "superseded") == 0 ||
         strcmp(reason, "cessationOfOperation") == 0);
}

solariStatus serverProvisionIssueCertificateRevocation(
    serverContext *ctx, uint64_t nodeId, const char *serialHex,
    const char *leafCertFp, const char *reason, const char *operator_,
    uint64_t *confirmToken)
{
    char issuerFp[65], tokenHash[65];
    uint64_t token = 0, now, resolvedNodeId = 0;
    solariStatus st;

    if (confirmToken) *confirmToken = 0;
    if (!ctx || !ctx->db || !serialHex ||
        !serialHex[0] || strlen(serialHex) > 40 || !leafCertFp ||
        strlen(leafCertFp) != 64 || !provCertificateRevocationReasonValid(reason) ||
        !operator_ || !operator_[0] || !confirmToken)
        return ERR_INVALID_ARG;
    st = serverDbGetLeafCertificateIssuer(ctx->db, nodeId, serialHex,
                                          leafCertFp, issuerFp, &resolvedNodeId);
    if (st != SOLARI_OK) return st;
    if ((st = provRandomConfirmToken(&token)) != SOLARI_OK) return st;
    provConfirmTokenHash(token, tokenHash);
    now = solariNowUnixMs();
    st = serverDbCertificateRevocationIssue(ctx->db, resolvedNodeId, issuerFp,
            serialHex, leafCertFp, reason, operator_, tokenHash,
            now, now + UINT64_C(600000));
    memset(tokenHash, 0, sizeof tokenHash);
    if (st != SOLARI_OK) return st;
    *confirmToken = token;
    solariLogf(SOLARI_LOG_WARN,
               "provision/revoke-cert: confirmation issued node=%llu serial=%s by=%s",
               (unsigned long long)nodeId, serialHex, operator_);
    return SOLARI_OK;
}

solariStatus serverProvisionConfirmCertificateRevocation(
    serverContext *ctx, uint64_t nodeId, const char *serialHex,
    const char *leafCertFp, const char *reason, const char *operator_,
    uint64_t confirmToken)
{
    char issuerFp[65], tokenHash[65];
    solariStatus st;
    uint64_t now, resolvedNodeId = 0;

    if (!ctx || !ctx->db || !serialHex ||
        !serialHex[0] || strlen(serialHex) > 40 || !leafCertFp ||
        strlen(leafCertFp) != 64 || !provCertificateRevocationReasonValid(reason) ||
        !operator_ || !operator_[0] || confirmToken == 0)
        return ERR_INVALID_ARG;
    st = serverDbGetLeafCertificateIssuer(ctx->db, nodeId, serialHex,
                                          leafCertFp, issuerFp, &resolvedNodeId);
    if (st != SOLARI_OK) return st;
    provConfirmTokenHash(confirmToken, tokenHash);
    now = solariNowUnixMs();
    st = serverDbCertificateRevocationConfirm(ctx->db, resolvedNodeId, issuerFp,
            serialHex, leafCertFp, reason, operator_, tokenHash, now);
    memset(tokenHash, 0, sizeof tokenHash);
    if (st != SOLARI_OK) return st;
    solariLogf(SOLARI_LOG_WARN,
               "provision/revoke-cert: durable ledger updated node=%llu serial=%s by=%s; "
               "CRL publication and peer activation are not implemented",
               (unsigned long long)nodeId, serialHex, operator_);
    return SOLARI_OK;
}

solariStatus serverProvisionRetire(serverContext *ctx, uint64_t nodeId)
{
    uint64_t now;
    solariStatus st;

    if (!ctx || !ctx->db || nodeId == 0) return ERR_INVALID_ARG;

    if ((st = serverDbRetireNode(ctx->db, nodeId)) != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/retire: set state 'retired' for node %llu: %s",
                   (unsigned long long)nodeId, solariStrError(st));
        return st;
    }
    now = solariNowUnixMs();
    (void)serverDbWriteAlertEvent(ctx->db, 0, nodeId, NULL, "warn",
                                  "node retired", now, "audit", 0, 0, 2, "suppress", NULL);

    solariLogf(SOLARI_LOG_INFO, "provision/retire: node=%llu marked retired",
               (unsigned long long)nodeId);
    return SOLARI_OK;
}

/* ===================================================================== */
/* Probe-target adoption: CTRL_ADOPT_TARGET (§7.4)                        */
/* ===================================================================== */

/* First TCP port mentioned in a services blob like ["ssh:22","https:443"];
 * returns 0 if none is present (caller then falls back to an ICMP target). */
static int provFirstPort(const char *services)
{
    const char *p;
    if (!services) return 0;
    for (p = services; *p; p++) {
        if (*p == ':' && p[1] >= '0' && p[1] <= '9') {
            int port = 0;
            const char *q = p + 1;
            while (*q >= '0' && *q <= '9') { port = port * 10 + (*q - '0'); q++; }
            if (port > 0 && port <= 65535) return port;
        }
    }
    return 0;
}

solariStatus serverProvisionDispatchTarget(serverContext *ctx,
                                           const char *probeSpec)
{
    uint8_t payload[PROV_PAYLOAD_CAP];
    uint8_t frame[PROV_FRAME_CAP];
    size_t  payloadLen = 0, frameLen = 0;
    uint16_t tlvCount = 0;
    uint64_t owner;
    uint64_t fleet[1];
    solariStatus st;

    if (!ctx || !probeSpec || !probeSpec[0]) return ERR_INVALID_ARG;

    /* HRW-select the owning monitor for this target. The shared serverDb API
     * exposes no "list active monitors" query (CONTRACT GAP, see file footer),
     * so with no fleet view available we fall back to a fleet of one - this
     * server's own node id - which still produces a deterministic, correct
     * single-owner dispatch the monitor accepts; once a fleet enumerator
     * exists, pass the real list to provHrwTopOwner for true k-of-n spread. */
    fleet[0] = ctx->nodeId;
    owner = provHrwTopOwner(probeSpec, fleet, 1);

    if ((st = provBuildAdoptPayload(probeSpec, payload, sizeof payload,
                                    &payloadLen, &tlvCount)) != SOLARI_OK) {
        return st;
    }
    if ((st = provAssembleFrame(ctx, SCP_MSG_CONTROL, tlvCount,
                                payload, payloadLen,
                                frame, sizeof frame, &frameLen)) != SOLARI_OK) {
        return st;
    }

    st = provSendControl(ctx, frame, frameLen);
    if (st == ERR_CONN_RETRY) {
        /* Owner unreachable/link down/no pub bound right now; this is benign -
         * the caller has already persisted intent to the DB (source of truth)
         * and can retry. Propagate ERR_CONN_RETRY as-is (rather than masking
         * it to SOLARI_OK) so callers that need to distinguish "deferred" from
         * "delivered" - e.g. serverProvisionAdoptTarget's discovered-row status
         * transition - can still tell the two apart; callers that don't care
         * (e.g. serverAssets.c) already treat any non-OK return as log+continue. */
        solariLogf(SOLARI_LOG_WARN,
                   "provision/dispatch: monitor %llu unreachable; adoption "
                   "pending for target=%s", (unsigned long long)owner,
                   probeSpec);
        return ERR_CONN_RETRY;
    }
    if (st != SOLARI_OK) return st;

    solariLogf(SOLARI_LOG_INFO,
               "provision/dispatch: target=%s -> monitor %llu (%zu B)",
               probeSpec, (unsigned long long)owner, frameLen);
    return SOLARI_OK;
}

solariStatus serverProvisionAdoptTarget(serverContext *ctx, uint64_t discId,
                                        const char *probeSpec)
{
    serverDiscEntity ent;
    char specBuf[PROV_PAYLOAD_CAP];
    int port;
    const char *proto;
    const char *label;
    solariStatus st;

    if (!ctx || !ctx->db || discId == 0) return ERR_INVALID_ARG;

    /* Resolve the discovered row so we can derive a probe spec / HRW key. */
    memset(&ent, 0, sizeof ent);
    if ((st = serverDbGetDiscovered(ctx->db, discId, &ent)) != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/adopt: discovered %llu not found: %s",
                   (unsigned long long)discId, solariStrError(st));
        return st;
    }

    port = provFirstPort(ent.servicesJson);
    proto = port ? "tcp" : "icmp";
    label = ent.host[0] ? ent.host : ent.ip;

    /* The probe spec is operator-supplied, or synthesized from the discovered
     * entity in the monitor target grammar: proto:host[:port][ : label].
     * An explicit probeSpec overrides it verbatim. */
    if (probeSpec && probeSpec[0]) {
        (void)snprintf(specBuf, sizeof specBuf, "%s", probeSpec);
    } else if (port) {
        (void)snprintf(specBuf, sizeof specBuf, "tcp:%s:%d : %s",
                       ent.ip, port, label);
    } else {
        (void)snprintf(specBuf, sizeof specBuf, "icmp:%s : %s", ent.ip, label);
    }
    if (specBuf[0] == '\0' || ent.ip[0] == '\0') {
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/adopt: discovered %llu has no addressable target",
                   (unsigned long long)discId);
        return ERR_INVALID_ARG;
    }

    /* A discovered address belonging to a retained lifecycle tombstone is
     * evidence worth surfacing, never permission to resurrect monitoring. */
    {
        uint64_t assetId = 0;
        bool active = false;
        if (serverDbGetAssetIdByIp(ctx->db, ent.ip, &assetId) == SOLARI_OK && assetId != 0 &&
            serverDbAssetIsActive(ctx->db, assetId, &active) == SOLARI_OK && !active) {
            (void)serverDbSetDiscoveredStatus(ctx->db, discId, "ignored");
            solariLogf(SOLARI_LOG_INFO,
                       "provision/adopt: discovered %s ignored; asset %llu is lifecycle-tombstoned",
                       ent.ip, (unsigned long long)assetId);
            return SOLARI_OK;
        }
    }

    /* Catalog the adopted entity as a probe target NOW (before dispatch), so it
     * is immediately visible in /api/probes and assignable to monitors even if
     * the owning monitor is currently unreachable or none is enrolled yet. The
     * proto/port are derived from the first discovered service; a host with no
     * known TCP service falls back to an ICMP reachability target. */
    {
        char        targetId[160];
        solariStatus prc;
        if (port) (void)snprintf(targetId, sizeof targetId, "tcp:%s:%d", ent.ip, port);
        else      (void)snprintf(targetId, sizeof targetId, "icmp:%s", ent.ip);
        prc = serverDbUpsertProbeTarget(ctx->db, targetId, ent.ip, port, proto,
                                        2 /* replFactor */, label, ent.segId, 0,
                                        proto, NULL);
        if (prc != SOLARI_OK)
            solariLogf(SOLARI_LOG_WARN,
                       "provision/adopt: probeTarget upsert for disc %llu failed: %s "
                       "(continuing)", (unsigned long long)discId, solariStrError(prc));
    }

    /* Mark the row as adopting before dispatch so a concurrent re-advert does
     * not resurface it as "new". */
    (void)serverDbSetDiscoveredStatus(ctx->db, discId, "adopting");

    st = serverProvisionDispatchTarget(ctx, specBuf);
    if (st == ERR_CONN_RETRY) {
        /* Owner unreachable/no pub bound right now; leave the row "adopting"
         * so it is retried later rather than resurfacing as "new". Already
         * logged by serverProvisionDispatchTarget. */
        return SOLARI_OK;
    }
    if (st != SOLARI_OK) {
        (void)serverDbSetDiscoveredStatus(ctx->db, discId, "new");
        return st;
    }

    (void)serverDbSetDiscoveredStatus(ctx->db, discId, "adopted");
    solariLogf(SOLARI_LOG_INFO,
               "provision/adopt: disc=%llu target=%s adopted",
               (unsigned long long)discId, specBuf);
    return SOLARI_OK;
}

/*
 * CONTRACT GAPS (for the integrator - do not edit server.h here):
 *
 * 1. serverProvisionDispatchTarget needs the live monitor fleet for true HRW
 *    k-of-n owner selection, but serverDb exposes no "list nodes by role/state"
 *    query. The pure provHrwTopOwner()/provHrwWeight() helpers implement the
 *    same rendezvous keying as monitorPeer.c and are ready to consume a real
 *    fleet list; today dispatch feeds them a single-element fleet (this
 *    server's node id) so dispatch stays deterministic and correct for one
 *    owner. Recommend adding e.g. serverDbListNodesByRole(db, ROLE_MONITOR,
 *    uint64_t *out, size_t *count) and a replication factor k.
 *
 * 2. Build-convergence "tracking" for /api/builds is served by the existing
 *    serverDbBuildConvergence() (join over nodeConfig.appliedEpoch); this file
 *    drives the inputs to it via serverDbSetNodeTargetEpoch() on provision and
 *    relies on serverControl.c calling serverDbSetNodeApplied() when a node's
 *    CONTROL_RESULT lands. No extra query was needed here.
 *
 * 3. SCP_PROTO_VERSION's numeric value is pinned locally (default 1) because it
 *    is not re-exported through a server symbol; align with solariCommon.h if it
 *    bumps.
 */
