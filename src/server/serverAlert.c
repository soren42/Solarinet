/*
 * serverAlert.c - alert-rule evaluation -> alertEvent (§9.1, §10).
 *
 * Evaluates the enabled alertRule rows of a given scope ('host' for client
 * reports, 'probe' for monitor reports) against the metrics carried by an
 * incoming report. Threshold ops (gt/lt/eq) compare a named scalar metric to a
 * rule threshold; the transition op fires on any change of the metric's
 * coarse state since the last observation. A rule that breaches for at least
 * forSeconds fires an alertEvent row (persisted via serverDb) and a
 * notification is published on the PUB socket for the ntfy bridge; when the
 * breach ends the matching event is cleared. SCP_FLAG_URGENT bypasses the
 * forSeconds sustain window and fires on the first breach.
 *
 * Design: the wire/DB-touching surface (rule loading, event writes, publish)
 * is kept thin; the per-metric extraction, the op comparison, the sustain
 * state machine, and the notification-frame construction are pure static
 * helpers so they can be unit-tested without a live MariaDB or PUB peer.
 *
 * The forSeconds sustain window and re-fire suppression require remembering a
 * little state across reports. The ingest path is single-threaded (§9, §6.5:
 * one PULL sink, no globals in the transport handles), so that state lives in a
 * bounded, file-local table keyed by (ruleId,nodeId,targetId) - it is private
 * to this translation unit, not exported through the shared header.
 */
#include "server.h"

#include "solari/solariLog.h"
#include "solari/solariTime.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

/* Upper bound on rules loaded per scope per report (sized for an intranet
 * ruleset; rules beyond this are dropped with a log, matching the report
 * collection-cap convention in §6.4). */
#define ALERT_MAX_RULES 256

/* Severity -> a small magnitude carried in the notification's error code so the
 * ntfy bridge can map it to a priority without re-reading the rule. */
#define ALERT_CODE_INFO 1
#define ALERT_CODE_WARN 2
#define ALERT_CODE_CRIT 3

/* Float equality tolerance for the 'eq' op (metrics are integral milli/permille
 * counters widened to double, so a tight epsilon is correct). */
#define ALERT_EQ_EPSILON 0.5

/* ---- in-flight breach tracking (file-local, ingest-thread only) ---- */
typedef struct {
    bool     inUse;
    int      ruleId;
    uint64_t nodeId;
    char     targetId[SERVER_TARGETID_MAX]; /* "" for host scope            */
    uint64_t breachSinceMs;                 /* first observed breach (wall)  */
    bool     fired;                         /* an open alertEvent exists     */
    double   lastValue;                     /* for transition-op edge detect */
    bool     haveLast;
} alertBreachState;

/* Sized to comfortably hold the active breaches for an intranet fleet; a full
 * table degrades gracefully (oldest non-firing slot is reused). */
#define ALERT_STATE_SLOTS 1024
static alertBreachState gAlertState[ALERT_STATE_SLOTS];

/* ===================================================================== */
/* Pure helpers                                                           */
/* ===================================================================== */

/* Evaluate a threshold op. Returns true when `value` breaches `threshold`
 * under `op`. The transition op is handled separately (it needs prior state)
 * and is treated here as a no-breach. */
static bool alertThresholdBreached(const char *op, double value, double threshold)
{
    if (strcmp(op, "gt") == 0) return value >  threshold;
    if (strcmp(op, "lt") == 0) return value <  threshold;
    if (strcmp(op, "eq") == 0) return fabs(value - threshold) <= ALERT_EQ_EPSILON;
    return false; /* "transition" or unknown: not a scalar threshold */
}

/* Map a rule severity string to the notification magnitude code. */
static uint16_t alertSeverityCode(const char *severity)
{
    if (strcmp(severity, "crit") == 0) return ALERT_CODE_CRIT;
    if (strcmp(severity, "warn") == 0) return ALERT_CODE_WARN;
    return ALERT_CODE_INFO;
}

/* Mean CPU load across reported cores, in milli-units (0..1000 per core scale).
 * Returns 0 when no cores were reported. */
static double alertClientCpuAvgMilli(const solariClientReport *rep)
{
    uint64_t sum = 0;
    unsigned n = rep->coreCount;
    if (n == 0 || n > SOLARI_MAX_CORES) n = (rep->coreCount > SOLARI_MAX_CORES)
                                            ? SOLARI_MAX_CORES : rep->coreCount;
    if (n == 0) return 0.0;
    for (unsigned i = 0; i < n; i++) sum += rep->cpuLoadMilli[i];
    return (double)sum / (double)n;
}

/* Resolve a host-scope metric name to a scalar value from a client report.
 * Returns true and sets *out when the metric is recognised; false otherwise so
 * the caller can skip a rule that names an unknown metric (logged once). */
static bool alertClientMetric(const solariClientReport *rep, const char *metric,
                              double *out)
{
    if (strcmp(metric, "cpuAvgMilli") == 0) {
        *out = alertClientCpuAvgMilli(rep);
        return true;
    }
    if (strcmp(metric, "ramUsedKb") == 0)   { *out = (double)rep->ramUsedKb;   return true; }
    if (strcmp(metric, "ramTotalKb") == 0)  { *out = (double)rep->ramTotalKb;  return true; }
    if (strcmp(metric, "swapUsedKb") == 0)  { *out = (double)rep->swapUsedKb;  return true; }
    if (strcmp(metric, "swapTotalKb") == 0) { *out = (double)rep->swapTotalKb; return true; }
    if (strcmp(metric, "ramUsedPct") == 0) {
        *out = (rep->ramTotalKb == 0) ? 0.0
             : (100.0 * (double)rep->ramUsedKb / (double)rep->ramTotalKb);
        return true;
    }
    if (strcmp(metric, "swapUsedPct") == 0) {
        *out = (rep->swapTotalKb == 0) ? 0.0
             : (100.0 * (double)rep->swapUsedKb / (double)rep->swapTotalKb);
        return true;
    }
    if (strcmp(metric, "diskFreeMinKb") == 0) {
        /* worst-case (smallest) free space across mounts; 0 if no disks */
        uint64_t worst = (rep->diskCount > 0) ? rep->disks[0].freeKb : 0;
        for (uint8_t i = 1; i < rep->diskCount && i < SOLARI_MAX_DISKS; i++)
            if (rep->disks[i].freeKb < worst) worst = rep->disks[i].freeKb;
        *out = (double)worst;
        return true;
    }
    if (strcmp(metric, "procCount") == 0)  { *out = (double)rep->procCount;  return true; }
    if (strcmp(metric, "ifaceCount") == 0) { *out = (double)rep->ifaceCount; return true; }
    if (strcmp(metric, "diskCount") == 0)  { *out = (double)rep->diskCount;  return true; }
    if (strcmp(metric, "health.fsReadonly") == 0) {
        *out = (double)rep->health.fsReadonlyCount;
        return true;
    }
    if (strcmp(metric, "health.blockDevMissing") == 0) {
        *out = (double)rep->health.blockDevMissing;
        return true;
    }
    if (strcmp(metric, "health.smartFail") == 0) {
        *out = (double)rep->health.smartFailCount;
        return true;
    }
    if (strcmp(metric, "health.failedUnits") == 0) {
        *out = (double)rep->health.failedUnitCount;
        return true;
    }
    if (strcmp(metric, "health.dmesgCrit") == 0) {
        *out = (double)rep->health.dmesgCritCount;
        return true;
    }
    return false;
}

/* Optional detail text for health metrics whose scalar alone is not actionable.
 * Empty/NULL means the alert detail line stays in the generic form. */
static const char *alertClientMetricDetail(const solariClientReport *rep,
                                           const char *metric)
{
    if (strcmp(metric, "health.fsReadonly") == 0)
        return rep->health.fsReadonlyList;
    if (strcmp(metric, "health.blockDevMissing") == 0)
        return "";
    if (strcmp(metric, "health.smartFail") == 0)
        return rep->health.smartFailList;
    if (strcmp(metric, "health.failedUnits") == 0)
        return rep->health.failedUnitList;
    if (strcmp(metric, "health.dmesgCrit") == 0)
        return rep->health.dmesgCritSample;
    return NULL;
}

/* Resolve a probe-scope metric name to a scalar value from a single probe
 * result. */
static bool alertProbeMetric(const solariProbeResult *p, const char *metric,
                             double *out)
{
    if (strcmp(metric, "lossPermille") == 0)   { *out = (double)p->lossPermille;   return true; }
    if (strcmp(metric, "rttMicros") == 0)      { *out = (double)p->rttMicros;      return true; }
    if (strcmp(metric, "rttMs") == 0)          { *out = (double)p->rttMicros / 1000.0; return true; }
    if (strcmp(metric, "jitterMicros") == 0)   { *out = (double)p->jitterMicros;   return true; }
    if (strcmp(metric, "throughputKbps") == 0) { *out = (double)p->throughputKbps; return true; }
    if (strcmp(metric, "outcome") == 0)        { *out = (double)p->outcome;        return true; }
    if (strcmp(metric, "reachable") == 0)      { *out = (p->outcome == 0) ? 1.0 : 0.0; return true; }
    return false;
}

/* Stable per-probe target id "proto:addr:port" used both as the alertEvent
 * targetId and the breach-state key. dstAddr is the 16-byte IPv4-mapped/IPv6
 * field; we render the embedded IPv4 when mapped, else a compact hex form. */
static void alertProbeTargetId(const solariProbeResult *p, char *buf, size_t cap)
{
    const uint8_t *a = p->dstAddr;
    bool v4mapped = true;
    for (int i = 0; i < 10; i++) if (a[i] != 0) { v4mapped = false; break; }
    if (v4mapped && a[10] == 0xff && a[11] == 0xff) {
        snprintf(buf, cap, "%u:%u.%u.%u.%u:%u",
                 (unsigned)p->proto, a[12], a[13], a[14], a[15],
                 (unsigned)p->dstPort);
    } else {
        snprintf(buf, cap,
                 "%u:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%u",
                 (unsigned)p->proto, a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],
                 a[8],a[9],a[10],a[11],a[12],a[13],a[14],a[15],
                 (unsigned)p->dstPort);
    }
}

/* Compose the human-readable alertEvent / notification detail line. */
static void alertBuildDetail(char *buf, size_t cap, const serverAlertRule *r,
                             const char *targetId, double value, bool firing,
                             const char *extraDetail)
{
    if (extraDetail && extraDetail[0]) {
        snprintf(buf, cap, "%s %s %s(%s) %s %g [observed=%g sev=%s] detail=%s",
                 firing ? "FIRED" : "CLEARED",
                 r->scope, r->metric,
                 (targetId && targetId[0]) ? targetId : "-",
                 r->op, r->threshold, value, r->severity, extraDetail);
    } else {
        snprintf(buf, cap, "%s %s %s(%s) %s %g [observed=%g sev=%s]",
                 firing ? "FIRED" : "CLEARED",
                 r->scope, r->metric,
                 (targetId && targetId[0]) ? targetId : "-",
                 r->op, r->threshold, value, r->severity);
    }
}

/* Find (or, if create, allocate) the breach-state slot for a rule instance.
 * Returns NULL when create is requested but the table is exhausted. */
static alertBreachState *alertStateFind(int ruleId, uint64_t nodeId,
                                        const char *targetId, bool create)
{
    const char *tid = targetId ? targetId : "";
    alertBreachState *freeSlot = NULL;
    for (int i = 0; i < ALERT_STATE_SLOTS; i++) {
        alertBreachState *s = &gAlertState[i];
        if (s->inUse) {
            if (s->ruleId == ruleId && s->nodeId == nodeId &&
                strncmp(s->targetId, tid, sizeof(s->targetId)) == 0)
                return s;
        } else if (!freeSlot) {
            freeSlot = s;
        }
    }
    if (!create) return NULL;
    if (!freeSlot) {
        /* Table full: reclaim the first non-firing slot (a firing slot must be
         * kept so its event can later be cleared). */
        for (int i = 0; i < ALERT_STATE_SLOTS; i++) {
            if (!gAlertState[i].fired) { freeSlot = &gAlertState[i]; break; }
        }
        if (!freeSlot) return NULL;
    }
    memset(freeSlot, 0, sizeof(*freeSlot));
    freeSlot->inUse  = true;
    freeSlot->ruleId = ruleId;
    freeSlot->nodeId = nodeId;
    snprintf(freeSlot->targetId, sizeof(freeSlot->targetId), "%s", tid);
    return freeSlot;
}

/* Build a notification frame for an alert transition onto the PUB socket. The
 * SCP has no dedicated alert message type (alerts are a server->subscriber PUB
 * concern); SCP_MSG_ERROR is the structured carrier whose {code,detail} shape
 * fits a notification, so the ntfy bridge subscribes for it. Pure: writes a
 * complete frame into out and reports its length. */
static solariStatus alertBuildNotifyFrame(const serverContext *ctx,
                                          const char *severity,
                                          const char *detail,
                                          uint8_t *out, size_t cap,
                                          size_t *outLen)
{
    solariErrorMsg em;
    uint8_t        payload[SOLARI_ERRDETAIL_MAX + 32];
    size_t         payLen = 0;
    uint16_t       tlvCount = 0;
    solariStatus   st;

    memset(&em, 0, sizeof(em));
    em.code = alertSeverityCode(severity);
    snprintf(em.detail, sizeof(em.detail), "%s", detail);

    st = solariMsgBuildError(&em, payload, sizeof(payload), &payLen, &tlvCount);
    if (st != SOLARI_OK) return st;

    solariFrameHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic[0]        = SCP_MAGIC_0;
    hdr.magic[1]        = SCP_MAGIC_1;
    hdr.protoVersion    = SCP_PROTO_VERSION;
    hdr.msgType         = SCP_MSG_ERROR;
    hdr.flags           = SCP_FLAG_NONE;
    hdr.tlvCount        = tlvCount;
    hdr.sourceNodeId    = ctx->nodeId;
    hdr.sendTimeUnixMs  = solariNowUnixMs();
    hdr.seqNo           = 0; /* PUB notifications are fire-and-forget, not dedup'd */
    hdr.correlationId   = 0;

    return solariFrameBuild(&hdr, payload, payLen, out, cap, outLen);
}

/* ===================================================================== */
/* DB + transport surface                                                 */
/* ===================================================================== */

/* Publish a built notification frame on the PUB socket. Best-effort: a missing
 * socket (standby role, not yet bound) or a transient transport failure is not
 * fatal to alert persistence. */
static void alertPublish(serverContext *ctx, const char *severity,
                         const char *detail)
{
    uint8_t      frame[SOLARI_FRAME_HEADER_LEN + SOLARI_ERRDETAIL_MAX + 64];
    size_t       frameLen = 0;
    solariStatus st;

    if (!ctx->pub) {
        solariLogf(SOLARI_LOG_DEBUG,
                   "alert: PUB socket unbound, notification not published: %s",
                   detail);
        return;
    }
    st = alertBuildNotifyFrame(ctx, severity, detail, frame, sizeof(frame),
                               &frameLen);
    if (st != SOLARI_OK) {
        solariLogf(SOLARI_LOG_WARN, "alert: notify frame build failed: %s",
                   solariStrError(st));
        return;
    }
    st = solariConnSend(ctx->pub, frame, frameLen);
    if (st != SOLARI_OK) {
        solariLogf(SOLARI_LOG_WARN, "alert: PUB publish failed: %s",
                   solariStrError(st));
    }
}

/* Apply one evaluated rule/value to the breach state machine, persisting and
 * publishing fire/clear transitions. `breached` is the scalar-threshold result;
 * for the transition op, `value` is compared against the slot's last value
 * instead. */
static solariStatus alertApply(serverContext *ctx, uint64_t nodeId,
                               const serverAlertRule *r, const char *targetId,
                               double value, bool urgent,
                               const char *extraDetail)
{
    uint64_t          nowMs = solariNowUnixMs();
    bool              isTransition = (strcmp(r->op, "transition") == 0);
    bool              breached;
    alertBreachState *s = alertStateFind(r->ruleId, nodeId, targetId, true);
    char              detail[512];
    solariStatus      st = SOLARI_OK;

    if (!s) {
        solariLogf(SOLARI_LOG_WARN,
                   "alert: breach-state table full, rule %d node %llu dropped",
                   r->ruleId, (unsigned long long)nodeId);
        return SOLARI_OK; /* non-fatal; report write already succeeded */
    }

    if (isTransition) {
        breached = s->haveLast && (fabs(value - s->lastValue) > ALERT_EQ_EPSILON);
        s->lastValue = value;
        s->haveLast  = true;
    } else {
        breached = alertThresholdBreached(r->op, value, r->threshold);
    }

    if (breached) {
        if (s->breachSinceMs == 0) s->breachSinceMs = nowMs;
        if (!s->fired) {
            uint64_t sustainedMs = nowMs - s->breachSinceMs;
            uint64_t needMs = (r->forSeconds > 0)
                            ? (uint64_t)r->forSeconds * 1000u : 0u;
            bool fireNow = urgent || isTransition || sustainedMs >= needMs;
            if (fireNow) {
                alertBuildDetail(detail, sizeof(detail), r, targetId, value, true,
                                 extraDetail);
                st = serverDbWriteAlertEvent(ctx->db, r->ruleId, nodeId,
                                             (targetId && targetId[0]) ? targetId : NULL,
                                             r->severity, detail, nowMs);
                if (st != SOLARI_OK) {
                    solariLogf(SOLARI_LOG_ERROR,
                               "alert: write fired event rule %d failed: %s",
                               r->ruleId, solariStrError(st));
                    return st;
                }
                s->fired = true;
                solariLogf(SOLARI_LOG_INFO, "alert: %s", detail);
                alertPublish(ctx, r->severity, detail);
            }
        }
        /* transition fires are instantaneous edges, not sustained states */
        if (isTransition) { s->fired = false; s->breachSinceMs = 0; }
    } else {
        s->breachSinceMs = 0;
        if (s->fired) {
            alertBuildDetail(detail, sizeof(detail), r, targetId, value, false,
                             extraDetail);
            st = serverDbWriteAlertEvent(ctx->db, r->ruleId, nodeId,
                                         (targetId && targetId[0]) ? targetId : NULL,
                                         r->severity, detail, nowMs);
            if (st != SOLARI_OK) {
                solariLogf(SOLARI_LOG_ERROR,
                           "alert: write cleared event rule %d failed: %s",
                           r->ruleId, solariStrError(st));
                return st;
            }
            s->fired = false;
            solariLogf(SOLARI_LOG_INFO, "alert: %s", detail);
            alertPublish(ctx, r->severity, detail);
        }
        /* a non-breaching, non-firing transition slot can be released */
        if (isTransition && !s->haveLast) s->inUse = false;
    }
    return st;
}

/* ===================================================================== */
/* Public entry points                                                    */
/* ===================================================================== */

solariStatus serverAlertEvalClient(serverContext *ctx, uint64_t nodeId,
                                   const solariClientReport *rep, bool urgent)
{
    serverAlertRule rules[ALERT_MAX_RULES];
    size_t          count = ALERT_MAX_RULES;
    solariStatus    st;

    if (!ctx || !ctx->db || !rep) return ERR_INVALID_ARG;

    st = serverDbLoadAlertRules(ctx->db, "host", rules, &count);
    if (st != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR, "alert: load host rules failed: %s",
                   solariStrError(st));
        return st;
    }

    for (size_t i = 0; i < count && i < ALERT_MAX_RULES; i++) {
        double value;
        if (!alertClientMetric(rep, rules[i].metric, &value)) {
            solariLogf(SOLARI_LOG_DEBUG,
                       "alert: rule %d names unknown host metric '%s', skipped",
                       rules[i].ruleId, rules[i].metric);
            continue;
        }
        /* host scope has a single subject (the node); no per-target id */
        st = alertApply(ctx, nodeId, &rules[i], "", value, urgent,
                        alertClientMetricDetail(rep, rules[i].metric));
        if (st != SOLARI_OK) return st;
    }
    return SOLARI_OK;
}

solariStatus serverAlertEvalMonitor(serverContext *ctx, uint64_t nodeId,
                                    const solariMonitorReport *rep, bool urgent)
{
    serverAlertRule rules[ALERT_MAX_RULES];
    size_t          count = ALERT_MAX_RULES;
    solariStatus    st;
    uint16_t        nProbes;

    if (!ctx || !ctx->db || !rep) return ERR_INVALID_ARG;

    st = serverDbLoadAlertRules(ctx->db, "probe", rules, &count);
    if (st != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR, "alert: load probe rules failed: %s",
                   solariStrError(st));
        return st;
    }

    nProbes = (rep->probeCount > SOLARI_MAX_PROBES) ? SOLARI_MAX_PROBES
                                                    : rep->probeCount;

    /* Each probe-scope rule is evaluated against every probe result in the
     * batch; the probe's "proto:addr:port" is the per-target alert id. */
    for (size_t i = 0; i < count && i < ALERT_MAX_RULES; i++) {
        bool known = false;
        for (uint16_t p = 0; p < nProbes; p++) {
            double value;
            char   targetId[SERVER_TARGETID_MAX];
            if (!alertProbeMetric(&rep->probes[p], rules[i].metric, &value))
                continue;
            known = true;
            alertProbeTargetId(&rep->probes[p], targetId, sizeof(targetId));
            st = alertApply(ctx, nodeId, &rules[i], targetId, value, urgent, NULL);
            if (st != SOLARI_OK) return st;
        }
        if (!known && nProbes > 0) {
            solariLogf(SOLARI_LOG_DEBUG,
                       "alert: rule %d names unknown probe metric '%s', skipped",
                       rules[i].ruleId, rules[i].metric);
        }
    }
    return SOLARI_OK;
}
