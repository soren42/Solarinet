/*
 * serverControl.c - control-plane emit + result tracking (§9.1, §11).
 *
 * The active master steers the fleet over the control plane. This file owns the
 * two outbound directives the server originates and the two inbound replies they
 * (and a survey round) provoke:
 *
 *   serverControlBuild       - assemble a complete SCP_MSG_CONTROL (0x30) frame
 *     for one target node carrying a verb (solariCtrlVerb), the config epoch to
 *     converge to, and an optional opaque payload. The frame's correlationId is
 *     set to its own seqNo so the node's CONTROL_RESULT can be matched straight
 *     back to this directive (§5.2: "correlationId echoes a request's seqNo").
 *
 *   serverControlBuildSurvey - assemble an SCP_MSG_SURVEY (0x20) frame, an empty
 *     directive that demands every reachable node answer with an immediate report
 *     round (SURVEY_RESP) instead of waiting for its sample timer.
 *
 *   serverControlOnResult    - consume an inbound CONTROL_RESULT (0x31): parse
 *     the node's outcome TLVs (echoed verb + error magnitude, optional applied
 *     epoch) and fold it into per-node convergence after consuming its durable
 *     pending directive record.
 *
 *   SURVEY_RESP uses the durable monitor-report path in serverIngest.c.
 *
 * The wire contract for CONTROL_RESULT mirrors what the monitor emits
 * (monitorControl.c): TLV_CTRL_VERB (u8) + TLV_ERROR_CODE (u16, 0 == success),
 * with TLV_CTRL_TARGET_EPOCH (u64) required for successful config convergence
 * and optional for other verbs or failed results. Parsing and lastResult
 * formatting are factored into
 * pure static helpers (no I/O) so they are unit-testable without a live MariaDB
 * or any sockets; only the two On* entry points touch serverDb / serverMaster.
 *
 * Frame headers are stamped here: sourceNodeId = this server's id, sendTime =
 * wall clock, and a per-process monotonic seqNo (the contract carries no shared
 * seqNo counter in serverContext - see the integrator note in the handoff).
 */
#include "server.h"

#include "solari/solariLog.h"
#include "solari/solariTlv.h"
#include "solari/solariTime.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------- outbound seqNo */

/* Per-source monotonic frame seqNo (§5.5). The shared serverContext carries no
 * seqNo field and this file MUST NOT edit the header, so the server's control
 * emitter keeps its own counter. It need only be monotonic-per-source for the
 * receiver's (sourceNodeId, seqNo) dedup ring; uniqueness across a process
 * lifetime is sufficient. A single emit path (the active master) advances it, so
 * a plain static is adequate; if the server ever emits control from multiple
 * threads the integrator should promote this to an atomic or a ctx field. */
static uint32_t serverControlNextSeq(void)
{
    static uint32_t seq = 0;
    return ++seq;          /* start at 1; 0 is reserved for "unsolicited" */
}

/* Fill a control-plane frame header for an outbound directive of `msgType`.
 * correlationId is set to this frame's own seqNo for the CONTROL case so the
 * node's reply can be matched back; SURVEY is a broadcast with no per-node
 * correlation, but echoing the seqNo is harmless and keeps the field non-zero.
 * Pure aside from reading the clock; *hdr is fully written. */
static void serverControlInitHeader(const serverContext *ctx, uint8_t msgType,
                                    uint16_t tlvCount, solariFrameHeader *hdr)
{
    uint32_t seq = serverControlNextSeq();

    memset(hdr, 0, sizeof *hdr);
    hdr->magic[0]       = SCP_MAGIC_0;
    hdr->magic[1]       = SCP_MAGIC_1;
    hdr->protoVersion   = SCP_PROTO_VERSION;
    hdr->msgType        = msgType;
    hdr->flags          = SCP_FLAG_NONE;
    hdr->reserved       = 0;
    hdr->tlvCount       = tlvCount;
    hdr->sourceNodeId   = ctx ? ctx->nodeId : 0;
    hdr->sendTimeUnixMs = solariNowUnixMs();
    hdr->seqNo          = seq;
    hdr->correlationId  = seq;   /* node echoes this in its CONTROL_RESULT */
}

/* ------------------------------------------------------------ serverControlBuild */

solariStatus serverControlBuild(serverContext *ctx, uint64_t targetNode,
                                solariCtrlVerb verb, uint64_t targetEpoch,
                                const uint8_t *payload, uint16_t payloadLen,
                                uint8_t *out, size_t cap, size_t *outLen)
{
    /* A single control directive's TLV payload (verb + epoch + one opaque blob)
     * is small; bound the scratch buffer well under the 1 MiB frame cap so it is
     * stack-safe while still admitting a generous config blob. */
    enum { SERVER_CONTROL_TLV_MAX = 64 * 1024 };
    solariControl     ctrl;
    solariFrameHeader hdr;
    uint8_t           tlvBuf[SERVER_CONTROL_TLV_MAX];
    size_t            tlvLen = 0;
    uint16_t          tlvCount = 0;
    solariStatus      rc;

    if (outLen) *outLen = 0;                        /* defensive pre-zero */

    if (!ctx || !out || !outLen) return ERR_INVALID_ARG;
    /* A payload pointer is optional, but a non-zero length with no pointer is a
     * caller bug (solariMsgBuildControl would read past nothing). */
    if (payloadLen > 0 && payload == NULL) return ERR_INVALID_ARG;

    /* 1. Build the control TLV payload (verb + targetEpoch + addressee +
     * optional blob). The directive is PUBLISHED on the fleet PUB channel, so
     * the addressee rides inside the payload (TLV_CTRL_TARGET_NODE) and every
     * other subscriber drops the frame. */
    memset(&ctrl, 0, sizeof ctrl);
    ctrl.verb        = (uint8_t)verb;
    ctrl.targetEpoch = targetEpoch;
    ctrl.targetNode  = targetNode;
    ctrl.payload     = payload;
    ctrl.payloadLen  = payloadLen;

    rc = solariMsgBuildControl(&ctrl, tlvBuf, sizeof tlvBuf, &tlvLen, &tlvCount);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "control: build CONTROL payload for node=0x%llx verb=%u failed: %s",
                   (unsigned long long)targetNode, (unsigned)verb,
                   solariStrError(rc));
        return rc;
    }

    /* 2. Stamp the frame header and assemble the complete frame. The targetNode
     * is not a header field (the frame is delivered over the per-node control
     * link); it is logged for traceability and is the caller's routing key. */
    serverControlInitHeader(ctx, SCP_MSG_CONTROL, tlvCount, &hdr);

    rc = solariFrameBuild(&hdr, tlvBuf, tlvLen, out, cap, outLen);
    if (rc != SOLARI_OK) {
        *outLen = 0;
        solariLogf(SOLARI_LOG_ERROR,
                   "control: frame CONTROL for node=0x%llx verb=%u failed: %s",
                   (unsigned long long)targetNode, (unsigned)verb,
                   solariStrError(rc));
        return rc;
    }

    solariLogf(SOLARI_LOG_DEBUG,
               "control: CONTROL node=0x%llx verb=%u epoch=%llu corr=%u "
               "(%u TLVs, %zu bytes)",
               (unsigned long long)targetNode, (unsigned)verb,
               (unsigned long long)targetEpoch, (unsigned)hdr.correlationId,
               (unsigned)tlvCount, *outLen);
    return SOLARI_OK;
}

/* ------------------------------------------------------- serverControlBuildSurvey */

solariStatus serverControlBuildSurvey(serverContext *ctx,
                                      uint8_t *out, size_t cap, size_t *outLen)
{
    solariFrameHeader hdr;
    solariStatus      rc;

    if (outLen) *outLen = 0;                        /* defensive pre-zero */
    if (!ctx || !out || !outLen) return ERR_INVALID_ARG;

    /* A survey is a content-free demand: an empty-payload frame whose msgType is
     * the whole signal. tlvCount 0, no payload bytes. */
    serverControlInitHeader(ctx, SCP_MSG_SURVEY, 0, &hdr);

    rc = solariFrameBuild(&hdr, NULL, 0, out, cap, outLen);
    if (rc != SOLARI_OK) {
        *outLen = 0;
        solariLogf(SOLARI_LOG_ERROR, "control: frame SURVEY failed: %s",
                   solariStrError(rc));
        return rc;
    }

    solariLogf(SOLARI_LOG_DEBUG, "control: SURVEY seq=%u (%zu bytes)",
               (unsigned)hdr.seqNo, *outLen);
    return SOLARI_OK;
}

/* ------------------------------------------------------ CONTROL_RESULT parsing */

/* Parsed view of a CONTROL_RESULT body. */
typedef struct {
    uint8_t  verb;          /* echoed solariCtrlVerb (0 if absent)           */
    uint16_t errCode;       /* error magnitude, 0 == success                 */
    uint64_t appliedEpoch;  /* epoch the node now reports applied            */
    uint64_t lifeGeneration;/* decommission challenge identity              */
    bool     haveVerb;
    bool     haveErr;
    bool     haveEpoch;
    bool     haveLifeGeneration;
} serverControlResult;

/* Parse a CONTROL_RESULT payload into res. Tolerant of unknown/extra TLVs;
 * required-field validation happens at the state-changing boundary below.
 * Pure: no I/O. Returns ERR_TLV_TRUNCATED on malformed payload, else SOLARI_OK.
 * *res is zeroed first. */
static solariStatus serverControlParseResult(const uint8_t *payload, size_t len,
                                             serverControlResult *res)
{
    solariTlvReader r;
    uint16_t        type, vlen;
    const uint8_t  *val;
    solariStatus    rc;

    memset(res, 0, sizeof *res);
    if (!payload && len > 0) return ERR_INVALID_ARG;

    solariTlvReaderInit(&r, payload, len);
    for (;;) {
        rc = solariTlvNext(&r, &type, &val, &vlen);
        if (rc == ERR_TLV_END) break;
        if (rc != SOLARI_OK) return rc;   /* truncated: surface to caller */

        switch (type) {
        case TLV_CTRL_VERB:
            if (solariTlvReadU8(val, vlen, &res->verb) == SOLARI_OK)
                res->haveVerb = true;
            break;
        case TLV_ERROR_CODE:
            if (solariTlvReadU16(val, vlen, &res->errCode) == SOLARI_OK)
                res->haveErr = true;
            break;
        case TLV_CTRL_TARGET_EPOCH:
            if (solariTlvReadU64(val, vlen, &res->appliedEpoch) == SOLARI_OK)
                res->haveEpoch = true;
            break;
        case TLV_LIFE_GENERATION:
            if (solariTlvReadU64(val, vlen, &res->lifeGeneration) == SOLARI_OK)
                res->haveLifeGeneration = true;
            break;
        default:
            break;                        /* skip unknown TLVs (§5.6) */
        }
    }
    return SOLARI_OK;
}

/* Format a node's lastResult string from the parsed outcome. Success ("ok") when
 * the error magnitude is 0 (or absent); otherwise "err:<code>". Writes a
 * NUL-terminated string into buf (cap > 0). Pure. */
static void serverControlFormatResult(const serverControlResult *res,
                                      char *buf, size_t cap)
{
    if (cap == 0) return;
    if (!res->haveErr || res->errCode == 0)
        snprintf(buf, cap, "ok");
    else
        snprintf(buf, cap, "err:%u", (unsigned)res->errCode);
}

/* ----------------------------------------------------- serverControlOnResult */

solariStatus serverControlOnResult(serverContext *ctx, uint64_t nodeId,
                                   uint32_t correlationId,
                                   const uint8_t *payload, size_t len)
{
    serverControlResult res;
    char                lastResult[32];
    uint64_t            whenMs;
    solariStatus        rc;

    if (!ctx || !ctx->db) return ERR_INVALID_ARG;
    if (!payload && len > 0) return ERR_INVALID_ARG;

    rc = serverControlParseResult(payload, len, &res);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_WARN,
                   "control: malformed CONTROL_RESULT from node=0x%llx (corr=%u): %s",
                   (unsigned long long)nodeId, (unsigned)correlationId,
                   solariStrError(rc));
        return rc;
    }

    /* A bare or uncorrelated ACK must never be interpreted as convergence. */
    if (!correlationId || !res.haveVerb || !res.haveErr || res.verb == 0) {
        solariLogf(SOLARI_LOG_WARN,
                   "control: incomplete or uncorrelated result from node=0x%llx (corr=%u)",
                   (unsigned long long)nodeId, (unsigned)correlationId);
        return ERR_INVALID_ARG;
    }

    serverControlFormatResult(&res, lastResult, sizeof lastResult);
    whenMs = solariNowUnixMs();

    /* Decommission generation is a lifecycle identity, not a config epoch.
     * Do not feed it into applied-epoch convergence. Only a successful,
     * generation-matched result from this TLS-authenticated node retires it. */
    if (res.haveVerb && res.verb == CTRL_DECOMMISSION) {
        if (!res.haveErr || !res.haveLifeGeneration || res.lifeGeneration == 0) {
            solariLogf(SOLARI_LOG_WARN,
                       "control: incomplete decommission result from node=0x%llx (corr=%u)",
                       (unsigned long long)nodeId, (unsigned)correlationId);
            return ERR_INVALID_ARG;
        }
        if (res.errCode != 0) {
            solariLogf(SOLARI_LOG_WARN,
                       "control: node=0x%llx reported decommission failure code=%u generation=%llu",
                       (unsigned long long)nodeId, (unsigned)res.errCode,
                       (unsigned long long)res.lifeGeneration);
            return ERR_PLATFORM;
        }
        rc = serverDbDecommissionAck(ctx->db, nodeId, res.lifeGeneration);
        if (rc != SOLARI_OK) {
            solariLogf(SOLARI_LOG_WARN,
                       "control: ignored stale or unmatched decommission ack from node=0x%llx generation=%llu",
                       (unsigned long long)nodeId,
                       (unsigned long long)res.lifeGeneration);
            return rc;
        }
        solariLogf(SOLARI_LOG_INFO,
                   "control: node=0x%llx decommission generation=%llu acknowledged (corr=%u)",
                   (unsigned long long)nodeId,
                   (unsigned long long)res.lifeGeneration, (unsigned)correlationId);
        return SOLARI_OK;
    }

    /* Only config convergence can change appliedEpoch. Older agents may omit
     * the optional epoch on non-config results. A successful convergence result
     * without an epoch is recorded as unverified and never advances the epoch. */
    if (res.errCode == 0 &&
        (res.verb == CTRL_SET_CONFIG || res.verb == CTRL_PROVISION) &&
        !res.haveEpoch) {
        solariLogf(SOLARI_LOG_WARN,
                   "control: result without applied epoch from node=0x%llx (corr=%u)",
                   (unsigned long long)nodeId, (unsigned)correlationId);
        (void)snprintf(lastResult, sizeof lastResult, "epoch-missing");
    }

    /* Record convergence only after the required result fields were validated. */
    rc = serverDbControlResultApply(ctx->db, nodeId, correlationId, res.verb,
                                    res.appliedEpoch, res.haveEpoch, res.errCode,
                                    lastResult, whenMs);
    if (rc != SOLARI_OK) {
        if (rc == ERR_AUTH_ROLE) {
            solariLogf(SOLARI_LOG_INFO,
                       "control: ignored unmatched or duplicate result from node=0x%llx (corr=%u)",
                       (unsigned long long)nodeId, (unsigned)correlationId);
        } else {
            solariLogf(SOLARI_LOG_ERROR,
                       "control: record applied for node=0x%llx (%s) failed: %s",
                       (unsigned long long)nodeId, lastResult, solariStrError(rc));
        }
        return rc;
    }

    solariLogf(SOLARI_LOG_INFO,
               "control: node=0x%llx verb=%u corr=%u applied epoch=%llu result=%s",
               (unsigned long long)nodeId, (unsigned)res.verb,
               (unsigned)correlationId,
               (unsigned long long)res.appliedEpoch, lastResult);
    return SOLARI_OK;
}
