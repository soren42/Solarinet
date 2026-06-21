/*
 * serverMaster.c - master coordination / session acceptance + reconciliation
 * (§9.1).
 *
 * Two responsibilities, both authoritative-tier:
 *
 *   serverMasterAcceptHello - the session handshake. A node opens with an
 *     SCP_MSG_HELLO (0x01) declaring its role, agent version, fqdn and the
 *     config epoch it currently runs. The master upserts the node row (creating
 *     it on first contact, bumping lastSeenAt thereafter) and answers with an
 *     SCP_MSG_WELCOME (0x02) payload that carries the server-assigned config
 *     epoch (the authoritative lease/config epoch from ctx) plus the sampling
 *     schedule the node should run. WELCOME is how the server "corrects drift"
 *     of a node onto the fleet-wide schedule and epoch (§8.2 note: the server is
 *     the source of truth and corrects via WELCOME/CONTROL). Like the lease's
 *     PRIMARY_IS builder, this writes the TLV *payload* + element count into the
 *     caller buffer; the ingest layer wraps it in a frame.
 *
 *   serverMasterReconcile - the fusion step. Monitors run overlapping
 *     assignments (HRW redundancy, §8.2), so the same target is probed from
 *     several vantage points and the server receives multiple monitor reports
 *     for it. probeCurrent is keyed (targetId, monitorNode), so persisting the
 *     report already records the per-vantage state; reconcile then fuses those
 *     vantages into one derived node-level health verdict and writes it to
 *     node.state. Divergence between vantages is treated as signal (a target
 *     reachable from A but not B localises a partition), so the fusion is a
 *     worst-observed-then-degrade policy rather than a simple AND/OR.
 *
 * The wire-building (WELCOME TLVs) and the health-fusion kernel are factored
 * into small static pure helpers with no I/O, so the schedule contract and the
 * derived-health policy are unit-testable without a live MariaDB or any socket.
 */
#include "server.h"

#include "solari/solariTlv.h"
#include "solari/solariLog.h"
#include "solari/solariTime.h"

#include <string.h>

/* ===================================================================== */
/* WELCOME schedule contract                                              */
/* ===================================================================== */
/*
 * The node-side WELCOME parser is still Phase 5 work, so there is not yet a
 * frozen schedule-TLV contract in solariTlv.h. We mint the schedule TLVs in the
 * 0x01xx identity/session category - the same category the lease answer reused
 * for its holder-id TLV - because readers skip unknown types (solariTlv.h), so
 * emitting them now is forward-compatible and a node that does not yet read them
 * simply keeps its configured defaults. If the integrator wants these promoted
 * to named constants in the shared header, see the contract note in the handoff.
 *
 *   TLV_SCHED_SAMPLE_SEC   uint32 - client metric sampling interval (seconds)
 *   TLV_SCHED_PROBE_SEC    uint32 - monitor probe round interval   (seconds)
 *   TLV_ASSIGNED_ROLE      uint8  - role the server recognises this node as
 *                                   (echo of HELLO role; lets a node detect a
 *                                    role it was not enrolled for)
 */
#define TLV_SCHED_SAMPLE_SEC 0x0110u
#define TLV_SCHED_PROBE_SEC  0x0111u
#define TLV_ASSIGNED_ROLE    0x0112u

/* Fleet-wide default schedule (§13 [schedule]; mirrors the client/monitor
 * sample defaults). serverConfig does not yet carry per-fleet schedule knobs, so
 * these are the assigned defaults the master hands out until config drives them.
 * Centralised here so the assignment policy lives in one place. */
#define SERVER_DEFAULT_SAMPLE_SEC 15u   /* client sampleIntervalSec default */
#define SERVER_DEFAULT_PROBE_SEC  30u   /* monitor roundIntervalSec default */

/* ===================================================================== */
/* Derived health (pure policy kernel)                                    */
/* ===================================================================== */

/* node.state ENUM string values (§10 node.state). Kept as named constants so
 * the fusion policy and the DB call agree on the exact spelling. */
#define SERVER_STATE_UP      "up"
#define SERVER_STATE_DEGRADED "degraded"
#define SERVER_STATE_DOWN    "down"
#define SERVER_STATE_UNKNOWN "unknown"

/* A target is "reachable" from a vantage iff its probe outcome is ok (0).
 * Everything else (timeout/refused/unreachable/dns_fail/tls_fail/proto_err) is a
 * reachability failure for fusion purposes. */
static int serverMasterProbeReachable(const solariProbeResult *p)
{
    return p && p->outcome == 0 /* PROBE_OK */;
}

/* A loss rate at/above this permille is treated as a degraded link even when the
 * probe technically returned ok. 100 permille == 10% loss. */
#define SERVER_DEGRADE_LOSS_PERMILLE 100u

/* Fuse the probe results carried in one monitor report into a single derived
 * node-level health verdict, returning the node.state ENUM string. This is the
 * worst-observed-then-degrade policy:
 *
 *   - no probes at all                 -> "unknown" (nothing to judge)
 *   - every probe failed reachability  -> "down"
 *   - some failed, some ok             -> "degraded" (partial / divergent view)
 *   - all ok but any with high loss    -> "degraded"
 *   - all ok, all healthy              -> "up"
 *
 * Pure: depends only on the report, so the health policy is unit-testable with
 * no DB. *anyProbes is set to whether the report carried any probe at all (lets
 * the caller skip a redundant state write for an empty/link-only report). */
static const char *serverMasterDeriveHealth(const solariMonitorReport *rep,
                                            bool *anyProbes)
{
    uint16_t i, n;
    unsigned okCount = 0, failCount = 0, lossyCount = 0;

    if (anyProbes) *anyProbes = false;
    if (!rep || rep->probeCount == 0) return SERVER_STATE_UNKNOWN;

    n = rep->probeCount;
    if (n > SOLARI_MAX_PROBES) n = SOLARI_MAX_PROBES; /* defensive clamp */
    if (anyProbes) *anyProbes = (n > 0);

    for (i = 0; i < n; i++) {
        const solariProbeResult *p = &rep->probes[i];
        if (serverMasterProbeReachable(p)) {
            okCount++;
            if (p->lossPermille >= SERVER_DEGRADE_LOSS_PERMILLE) lossyCount++;
        } else {
            failCount++;
        }
    }

    if (okCount == 0)               return SERVER_STATE_DOWN;     /* all failed */
    if (failCount > 0)              return SERVER_STATE_DEGRADED; /* partial    */
    if (lossyCount > 0)             return SERVER_STATE_DEGRADED; /* lossy ok   */
    return SERVER_STATE_UP;                                       /* all clean  */
}

/* ===================================================================== */
/* WELCOME payload builder (pure)                                         */
/* ===================================================================== */

/* Build the WELCOME (0x02) TLV payload into [out, out+cap):
 *   TLV_CONFIG_EPOCH      (uint64) = the server-assigned authoritative epoch
 *   TLV_ASSIGNED_ROLE     (uint8)  = the role the server recognises (echo)
 *   TLV_SCHED_SAMPLE_SEC  (uint32) = assigned client sampling interval
 *   TLV_SCHED_PROBE_SEC   (uint32) = assigned monitor probe interval
 *
 * The node learns both the epoch it must converge to and the schedule it should
 * run, independent of the carrying frame. Pure: no I/O, so the schedule contract
 * is testable. *outLen and *tlvCount are written on success; on any append
 * failure (buffer too small) the partial write is reported via the rc. */
static solariStatus serverMasterBuildWelcome(uint64_t assignedEpoch,
                                             solariRole assignedRole,
                                             uint32_t sampleSec, uint32_t probeSec,
                                             uint8_t *out, size_t cap,
                                             size_t *outLen, uint16_t *tlvCount)
{
    solariTlvWriter w;
    solariStatus    rc;

    solariTlvWriterInit(&w, out, cap);

    rc = solariTlvAppendU64(&w, TLV_CONFIG_EPOCH, assignedEpoch);
    if (rc != SOLARI_OK) return rc;

    rc = solariTlvAppendU8(&w, TLV_ASSIGNED_ROLE, (uint8_t)assignedRole);
    if (rc != SOLARI_OK) return rc;

    rc = solariTlvAppendU32(&w, TLV_SCHED_SAMPLE_SEC, sampleSec);
    if (rc != SOLARI_OK) return rc;

    rc = solariTlvAppendU32(&w, TLV_SCHED_PROBE_SEC, probeSec);
    if (rc != SOLARI_OK) return rc;

    *outLen   = w.len;
    *tlvCount = w.count;
    return SOLARI_OK;
}

/* ===================================================================== */
/* serverMasterAcceptHello                                                */
/* ===================================================================== */

solariStatus serverMasterAcceptHello(serverContext *ctx,
                                     const solariFrameHeader *hdr,
                                     const solariHello *hello, const char *peerCertCn,
                                     uint8_t *out, size_t cap,
                                     size_t *outLen, uint16_t *tlvCount)
{
    uint64_t     nodeId;
    uint64_t     assignedEpoch;
    const char  *fqdn;
    const char  *certCn;
    solariStatus rc;

    if (outLen)   *outLen = 0;          /* defensive pre-zero */
    if (tlvCount) *tlvCount = 0;

    if (!ctx || !ctx->db || !hdr || !hello || !out || !outLen || !tlvCount)
        return ERR_INVALID_ARG;

    /* The node's stable id rides in the frame header (§5.5); the HELLO body
     * carries its declared identity. Both are needed: the header id keys the
     * node row, the body supplies fqdn/role/epoch. */
    nodeId = hdr->sourceNodeId;

    /* serverDbUpsertNode requires a non-NULL fqdn and certCn. Prefer the HELLO
     * fqdn; the cert CN is the authenticated identity (may be NULL if the caller
     * could not resolve it - fall back to the declared fqdn so the upsert still
     * records something stable rather than failing the whole handshake). */
    fqdn   = hello->hostFqdn[0] ? hello->hostFqdn : "";
    certCn = (peerCertCn && peerCertCn[0]) ? peerCertCn
           : (fqdn[0] ? fqdn : "");

    /* Upsert the node from the HELLO. HELLO does not carry os/arch (only client
     * reports do), so we pass NULL for them - the upsert leaves the existing
     * columns untouched on a duplicate and stores NULL on first insert until a
     * later CLIENT_REPORT fills them in. The node is recorded at the epoch it
     * declared so convergence tracking can see drift against the assigned epoch. */
    rc = serverDbUpsertNode(ctx->db, nodeId, hello->role,
                            fqdn, certCn, NULL, NULL, hello->configEpoch);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "master: HELLO upsert node=0x%llx (%s) failed: %s",
                   (unsigned long long)nodeId, fqdn, solariStrError(rc));
        return rc;
    }

    /* Stamp the heartbeat explicitly off the frame's send time so lastSeenAt
     * reflects the node's clock view of the handshake. Best-effort: a touch
     * failure does not sink an otherwise-good handshake (the upsert above
     * already bumped lastSeenAt to now). */
    {
        uint64_t whenMs = hdr->sendTimeUnixMs ? hdr->sendTimeUnixMs
                                              : solariNowUnixMs();
        solariStatus trc = serverDbTouchNode(ctx->db, nodeId, whenMs);
        if (trc != SOLARI_OK)
            solariLogf(SOLARI_LOG_DEBUG,
                       "master: HELLO touch node=0x%llx failed: %s (non-fatal)",
                       (unsigned long long)nodeId, solariStrError(trc));
    }

    /* The assigned epoch is the server's authoritative epoch (the lease epoch
     * the active master advances on a fresh claim). A node lagging behind this
     * learns the gap from WELCOME and will converge via a follow-on CONTROL. */
    assignedEpoch = ctx->epoch;

    rc = serverMasterBuildWelcome(assignedEpoch, hello->role,
                                  SERVER_DEFAULT_SAMPLE_SEC,
                                  SERVER_DEFAULT_PROBE_SEC,
                                  out, cap, outLen, tlvCount);
    if (rc != SOLARI_OK) {
        *outLen = 0;
        *tlvCount = 0;
        solariLogf(SOLARI_LOG_WARN,
                   "master: build WELCOME for node=0x%llx failed: %s",
                   (unsigned long long)nodeId, solariStrError(rc));
        return rc;
    }

    solariLogf(SOLARI_LOG_INFO,
               "master: HELLO node=0x%llx role=%d epoch=%llu -> WELCOME epoch=%llu "
               "(sample=%us probe=%us, %u TLVs, %zu bytes)",
               (unsigned long long)nodeId, (int)hello->role,
               (unsigned long long)hello->configEpoch,
               (unsigned long long)assignedEpoch,
               (unsigned)SERVER_DEFAULT_SAMPLE_SEC,
               (unsigned)SERVER_DEFAULT_PROBE_SEC,
               (unsigned)*tlvCount, *outLen);
    return SOLARI_OK;
}

/* ===================================================================== */
/* serverMasterReconcile                                                  */
/* ===================================================================== */

solariStatus serverMasterReconcile(serverContext *ctx, uint64_t nodeId,
                                   const solariMonitorReport *rep)
{
    const char  *state;
    bool         anyProbes = false;
    solariStatus rc;

    if (!ctx || !ctx->db || !rep) return ERR_INVALID_ARG;

    /* Persist this vantage's view first. probeCurrent is keyed
     * (targetId, monitorNode), so writing the report records THIS monitor's
     * per-target state without clobbering the other vantages' rows - that is
     * the "merge overlapping reports" step: the merge is the keyed upsert, and
     * the union across monitorNode rows is the multi-vantage matrix the
     * dashboard reads. */
    rc = serverDbWriteMonitorReport(ctx, nodeId, rep);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "master: reconcile write monitor=0x%llx (%u probes) failed: %s",
                   (unsigned long long)nodeId, (unsigned)rep->probeCount,
                   solariStrError(rc));
        return rc;
    }

    /* Compute the fused derived health from the probes this vantage reported and
     * record it on the monitor node's row. (A link-only / empty report yields
     * "unknown" and is left alone so a vantage that happened to send no probes
     * this round does not knock the node to unknown.) */
    state = serverMasterDeriveHealth(rep, &anyProbes);
    if (!anyProbes) {
        solariLogf(SOLARI_LOG_DEBUG,
                   "master: reconcile monitor=0x%llx carried no probes; "
                   "state unchanged", (unsigned long long)nodeId);
        return SOLARI_OK;
    }

    rc = serverDbSetNodeState(ctx->db, nodeId, state);
    if (rc != SOLARI_OK) {
        /* The authoritative measurements are already durable; a failed state
         * write is a derived-view miss, not a data loss. Surface it so the
         * caller/monitoring notices, but the report itself was accepted. */
        solariLogf(SOLARI_LOG_WARN,
                   "master: reconcile setState node=0x%llx '%s' failed: %s",
                   (unsigned long long)nodeId, state, solariStrError(rc));
        return rc;
    }

    solariLogf(SOLARI_LOG_DEBUG,
               "master: reconciled monitor=0x%llx %u probes -> state '%s'",
               (unsigned long long)nodeId, (unsigned)rep->probeCount, state);
    return SOLARI_OK;
}
