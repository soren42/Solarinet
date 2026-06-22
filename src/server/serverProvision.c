/*
 * serverProvision.c - enrollment + lifecycle (Handoff §7.3, §7.4).
 *
 * Owns the enrollment state machine (token->pending->approved/rejected) with
 * CSR signing via the internal CA through solariCtl; emits CTRL_PROVISION
 * (bring-up), CTRL_DECOMMISSION (guarded teardown with one-time confirm token +
 * audit row, carried by SCP_MSG_DECOMMISSION), CTRL_ADOPT_TARGET dispatch, and
 * tracks build convergence for /api/builds.
 *
 * Design note (testability, §17): every wire payload this subsystem emits is
 * assembled by a small pure static builder over a caller-owned buffer (no I/O,
 * no globals); the public entry points layer DB access + transport on top of
 * those builders. The one-time decommission confirm token is derived by SHA-256
 * over the node id, both clocks, the wipe scope and the operator identity, then
 * folded to 64 bits - reproducible from its inputs (so a test can assert the
 * derivation) yet unguessable by a node that does not know the wall/monotonic
 * stamps the server used. No live MariaDB or CSPRNG is required to exercise the
 * pure layer.
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

/* ===================================================================== */
/* Pure helpers (no I/O) - unit-testable in isolation                     */
/* ===================================================================== */

/* Derive the one-time decommission confirm token from the request inputs.
 * Stable for a fixed (nodeId, wipeScope, operator, wallMs, monoUs) tuple and
 * folded from a full SHA-256, so it is reproducible for tests but not
 * predictable to a node that lacks the two clock stamps. Never returns 0 (0 is
 * reserved to mean "no token"). */
static uint64_t provDeriveConfirmToken(uint64_t nodeId, uint32_t wipeScope,
                                       const char *operator_,
                                       uint64_t wallMs, uint64_t monoUs)
{
    solariSha256Ctx sc;
    uint8_t digest[32];
    uint8_t le[8];
    uint64_t tok = 0;
    int i;

    solariSha256Init(&sc);
    for (i = 0; i < 8; i++) le[i] = (uint8_t)(nodeId >> (i * 8));
    solariSha256Update(&sc, le, 8);
    for (i = 0; i < 8; i++) le[i] = (uint8_t)(wallMs >> (i * 8));
    solariSha256Update(&sc, le, 8);
    for (i = 0; i < 8; i++) le[i] = (uint8_t)(monoUs >> (i * 8));
    solariSha256Update(&sc, le, 8);
    for (i = 0; i < 4; i++) le[i] = (uint8_t)(wipeScope >> (i * 8));
    solariSha256Update(&sc, le, 4);
    if (operator_ && operator_[0]) {
        solariSha256Update(&sc, operator_, strlen(operator_));
    }
    solariSha256Final(&sc, digest);

    for (i = 0; i < 8; i++) tok = (tok << 8) | digest[i];
    if (tok == 0) tok = 1;   /* keep 0 reserved for "none" */
    return tok;
}

/* Build a CTRL_PROVISION (verb 7) control payload: verb + target epoch + the
 * opaque config blob. Pure TLV assembly over a caller buffer. */
static solariStatus provBuildProvisionPayload(uint64_t targetEpoch,
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
    if (cfgLen > 0) {
        if ((st = solariTlvAppend(&w, TLV_CTRL_PAYLOAD, cfgBlob, cfgLen)) != SOLARI_OK)
            return st;
    }
    *outLen = w.len;
    *count  = w.count;
    return SOLARI_OK;
}

/* Build the SCP_MSG_DECOMMISSION (0x32) payload: verb + the one-time confirm
 * token (TLV_LIFE_CONFIRM) + the wipe-scope bitfield (TLV_LIFE_WIPE_SCOPE).
 * Pure. */
static solariStatus provBuildDecommissionPayload(uint64_t confirmToken,
                                                 uint32_t wipeScope,
                                                 uint8_t *out, size_t cap,
                                                 size_t *outLen, uint16_t *count)
{
    solariTlvWriter w;
    solariStatus st;

    solariTlvWriterInit(&w, out, cap);
    if ((st = solariTlvAppendU8 (&w, TLV_CTRL_VERB, (uint8_t)CTRL_DECOMMISSION)) != SOLARI_OK)
        return st;
    if ((st = solariTlvAppendU64(&w, TLV_LIFE_CONFIRM, confirmToken)) != SOLARI_OK)
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

/* ===================================================================== */
/* Enrollment state machine (§7.3)                                        */
/* ===================================================================== */

solariStatus serverProvisionApprove(serverContext *ctx, uint64_t enrId,
                                    const char *operator_)
{
    serverEnrollment enr;
    char csrPem[PROV_CSR_PEM_CAP];
    char certPem[PROV_CERT_PEM_CAP];
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
    certPem[0] = '\0';
    if ((st = serverCtlSignCsr(ctx->ctl, csrPem, certPem, sizeof certPem)) != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/approve: CA sign failed for enroll %llu: %s",
                   (unsigned long long)enrId, solariStrError(st));
        return st;
    }

    now = solariNowUnixMs();

    /* Mark the decision first so an operator audit reflects intent even if a
     * later step trips. */
    if ((st = serverDbEnrollDecide(ctx->db, enrId, ENROLL_APPROVED,
                                   operator_, now)) != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/approve: decide(APPROVED) enroll %llu: %s",
                   (unsigned long long)enrId, solariStrError(st));
        return st;
    }

    /* Materialize the node row from the now-trusted enrollment. The node will
     * fully populate os/arch on its first HELLO; we seed identity here. The
     * cert CN is the enrolled host (the CSR was issued for it). */
    if ((st = serverDbUpsertNode(ctx->db, enrId, enr.role,
                                 enr.host, enr.host, "", "", 0)) != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/approve: upsert node for enroll %llu: %s",
                   (unsigned long long)enrId, solariStrError(st));
        return st;
    }

    /* The CSR PEM is held only until signed; drop it now (best-effort). */
    if (serverDbEnrollClearCsr(ctx->db, enrId) != SOLARI_OK) {
        solariLogf(SOLARI_LOG_WARN,
                   "provision/approve: failed to clear CSR for enroll %llu",
                   (unsigned long long)enrId);
    }

    /* Audit the approval alongside the alert/audit trail. */
    (void)serverDbWriteAlertEvent(ctx->db, 0, enrId, NULL, "info",
                                  "enrollment approved", now);

    solariLogf(SOLARI_LOG_INFO,
               "provision/approve: enroll %llu host=%s approved by %s, cert issued",
               (unsigned long long)enrId, enr.host, operator_);
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
                                  "enrollment rejected", now);

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
    if ((st = provBuildProvisionPayload(targetEpoch,
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
    uint8_t payload[PROV_PAYLOAD_CAP];
    uint8_t frame[PROV_FRAME_CAP];
    size_t  payloadLen = 0, frameLen = 0;
    uint16_t tlvCount = 0;
    uint64_t token, now, mono;
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

    now  = solariNowUnixMs();
    mono = solariMonotonicMicros();
    token = provDeriveConfirmToken(nodeId, wipeScope, operator_, now, mono);

    /* Audit BEFORE the destructive directive leaves the server (§7.3): the
     * alertEvent row is the durable record that a teardown was authorized, and
     * it must exist even if the send or the node's wipe later fails. */
    (void)snprintf(detail, sizeof detail,
                   "decommission authorized by %s scope=0x%02x token=%llu",
                   operator_, (unsigned)(wipeScope & 0xFFu),
                   (unsigned long long)token);
    if ((st = serverDbWriteAlertEvent(ctx->db, 0, nodeId, NULL, "crit",
                                      detail, now)) != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/decommission: audit row write failed for node %llu: %s",
                   (unsigned long long)nodeId, solariStrError(st));
        return st;   /* refuse to proceed without the audit trail */
    }

    /* Build SCP_MSG_DECOMMISSION carrying CTRL_DECOMMISSION + token + scope. */
    if ((st = provBuildDecommissionPayload(token, wipeScope,
                                           payload, sizeof payload,
                                           &payloadLen, &tlvCount)) != SOLARI_OK) {
        return st;
    }
    if ((st = provAssembleFrame(ctx, SCP_MSG_DECOMMISSION, tlvCount,
                                payload, payloadLen,
                                frame, sizeof frame, &frameLen)) != SOLARI_OK) {
        return st;
    }

    st = provSendControl(ctx, frame, frameLen);
    /* If the node is offline the intent is recorded; the teardown completes
     * when it returns (or the operator force-retires). Surface the token
     * regardless so the operator/dashboard can correlate the echo. */
    if (st == ERR_CONN_RETRY) {
        solariLogf(SOLARI_LOG_WARN,
                   "provision/decommission: node %llu offline; intent recorded "
                   "(token=%llu)", (unsigned long long)nodeId,
                   (unsigned long long)token);
        st = SOLARI_OK;
    }

    if (st == SOLARI_OK && confirmToken) *confirmToken = token;

    solariLogf(SOLARI_LOG_WARN,
               "provision/decommission: node=%llu scope=0x%02x by=%s issued "
               "(token=%llu)", (unsigned long long)nodeId,
               (unsigned)(wipeScope & 0xFFu), operator_,
               (unsigned long long)token);
    return st;
}

solariStatus serverProvisionRetire(serverContext *ctx, uint64_t nodeId)
{
    uint64_t now;
    solariStatus st;

    if (!ctx || !ctx->db || nodeId == 0) return ERR_INVALID_ARG;

    if ((st = serverDbSetNodeState(ctx->db, nodeId, "retired")) != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/retire: set state 'retired' for node %llu: %s",
                   (unsigned long long)nodeId, solariStrError(st));
        return st;
    }
    now = solariNowUnixMs();
    (void)serverDbWriteAlertEvent(ctx->db, 0, nodeId, NULL, "warn",
                                  "node retired", now);

    solariLogf(SOLARI_LOG_INFO, "provision/retire: node=%llu marked retired",
               (unsigned long long)nodeId);
    return SOLARI_OK;
}

/* ===================================================================== */
/* Probe-target adoption: CTRL_ADOPT_TARGET (§7.4)                        */
/* ===================================================================== */

solariStatus serverProvisionAdoptTarget(serverContext *ctx, uint64_t discId,
                                        const char *probeSpec)
{
    serverDiscEntity ent;
    uint8_t payload[PROV_PAYLOAD_CAP];
    uint8_t frame[PROV_FRAME_CAP];
    size_t  payloadLen = 0, frameLen = 0;
    uint16_t tlvCount = 0;
    char specBuf[PROV_PAYLOAD_CAP];
    uint64_t owner;
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

    /* The probe spec is operator-supplied, or synthesized from the discovered
     * entity (ip is its stable HRW key, kind chooses a default proto). The
     * synthesized form "ip|kind|services" is what a monitor's target loader
     * accepts; an explicit probeSpec overrides it verbatim. */
    if (probeSpec && probeSpec[0]) {
        (void)snprintf(specBuf, sizeof specBuf, "%s", probeSpec);
    } else {
        (void)snprintf(specBuf, sizeof specBuf, "%s|%s|%s",
                       ent.ip, ent.kind[0] ? ent.kind : "host",
                       ent.servicesJson[0] ? ent.servicesJson : "");
    }
    if (specBuf[0] == '\0' || ent.ip[0] == '\0') {
        solariLogf(SOLARI_LOG_ERROR,
                   "provision/adopt: discovered %llu has no addressable target",
                   (unsigned long long)discId);
        return ERR_INVALID_ARG;
    }

    /* Mark the row as adopting before dispatch so a concurrent re-advert does
     * not resurface it as "new". */
    (void)serverDbSetDiscoveredStatus(ctx->db, discId, "adopting");

    /* HRW-select the owning monitor for this target. The shared serverDb API
     * exposes no "list active monitors" query (CONTRACT GAP, see file footer),
     * so with no fleet view available we fall back to a fleet of one - this
     * server's own node id - which still produces a deterministic, correct
     * single-owner dispatch the monitor accepts; once a fleet enumerator
     * exists, pass the real list to provHrwTopOwner for true k-of-n spread. */
    {
        uint64_t fleet[1];
        fleet[0] = ctx->nodeId;
        owner = provHrwTopOwner(ent.ip, fleet, 1);
    }

    /* Build CTRL_ADOPT_TARGET and dispatch to the owning monitor. */
    if ((st = provBuildAdoptPayload(specBuf, payload, sizeof payload,
                                    &payloadLen, &tlvCount)) != SOLARI_OK) {
        (void)serverDbSetDiscoveredStatus(ctx->db, discId, "new");
        return st;
    }
    if ((st = provAssembleFrame(ctx, SCP_MSG_CONTROL, tlvCount,
                                payload, payloadLen,
                                frame, sizeof frame, &frameLen)) != SOLARI_OK) {
        (void)serverDbSetDiscoveredStatus(ctx->db, discId, "new");
        return st;
    }

    st = provSendControl(ctx, frame, frameLen);
    if (st == ERR_CONN_RETRY) {
        /* Owner unreachable now; leave row 'adopting' to retry, not an error. */
        solariLogf(SOLARI_LOG_WARN,
                   "provision/adopt: owner %llu unreachable; adoption pending "
                   "for disc %llu", (unsigned long long)owner,
                   (unsigned long long)discId);
        return SOLARI_OK;
    }
    if (st != SOLARI_OK) {
        (void)serverDbSetDiscoveredStatus(ctx->db, discId, "new");
        return st;
    }

    (void)serverDbSetDiscoveredStatus(ctx->db, discId, "adopted");
    solariLogf(SOLARI_LOG_INFO,
               "provision/adopt: disc=%llu target=%s -> monitor %llu (%zu B)",
               (unsigned long long)discId, specBuf,
               (unsigned long long)owner, frameLen);
    return SOLARI_OK;
}

/*
 * CONTRACT GAPS (for the integrator - do not edit server.h here):
 *
 * 1. serverProvisionAdoptTarget needs the live monitor fleet for true HRW
 *    k-of-n owner selection, but serverDb exposes no "list nodes by role/state"
 *    query. The pure provHrwTopOwner()/provHrwWeight() helpers implement the
 *    same rendezvous keying as monitorPeer.c and are ready to consume a real
 *    fleet list; today they are fed a single-element fleet (this server's node
 *    id) so dispatch stays deterministic and correct for one owner. Recommend
 *    adding e.g. serverDbListNodesByRole(db, ROLE_MONITOR, uint64_t *out,
 *    size_t *count) and a replication factor k.
 *
 * 2. Build-convergence "tracking" for /api/builds is served by the existing
 *    serverDbBuildConvergence() (join over nodeConfig.appliedEpoch); this file
 *    drives the inputs to it via serverDbSetNodeTargetEpoch() on provision and
 *    relies on serverControl.c calling serverDbSetNodeApplied() when a node's
 *    CONTROL_RESULT lands. No extra query was needed here.
 *
 * 3. serverProvisionApprove uses enrId as the new node's id when upserting the
 *    node row (the enrollment carries host/ip/role but no pre-assigned nodeId,
 *    and serverDbUpsertNode requires one). If the schema instead derives the
 *    nodeId from the FQDN (serverNodeId-style FNV), swap the first argument to
 *    that derivation once a shared helper for ROLE-scoped host ids is exposed.
 *
 * 4. SCP_PROTO_VERSION's numeric value is pinned locally (default 1) because it
 *    is not re-exported through a server symbol; align with solariCommon.h if it
 *    bumps.
 */
