#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

/*
 * monitorControl.c - SCP_MSG_CONTROL handling on the monitor (sec 8, sec 9.1,
 * Phase 3 Handoff sec 4.2/7.4).
 *
 * The server steers a live monitor without a redeploy by PUBLISHING
 * SCP_MSG_CONTROL directives on its fleet PUB channel:
 *
 *   CTRL_ADOPT_TARGET       - "one-click monitor this": promotes a discovered
 *     entity to a live probe target. TLV_CTRL_PAYLOAD carries the same
 *     "proto:host[:port][ : label]" spec the .conf uses, so adoption converges
 *     through the one scheduler add path (monitorAddTarget) - idempotent.
 *
 *   CTRL_SET_CONFIG / CTRL_PROVISION - converge this node to a config epoch:
 *     TLV_CTRL_PAYLOAD carries the per-node JSON config blob
 *     (nodeConfig.configBlob) and TLV_CTRL_TARGET_EPOCH the epoch to reach.
 *     Applied hot (round interval, probe tuning, target set), persisted to the
 *     local applied-state file, and acknowledged with the applied epoch.
 *
 * The monitor answers each addressed directive with SCP_MSG_CONTROL_RESULT
 * (verb echo + error magnitude + applied epoch) whose correlationId echoes the
 * directive frame's seqNo; the server folds it into per-node convergence
 * (serverControlOnResult consumes the matching durable pending directive).
 *
 * Layering mirrors serverControl.c and clientControl.c: parse/apply/decide is
 * pure over caller memory (unit-testable, no I/O); the state file is small
 * separate file I/O; the SUB-transport glue compiles only with
 * MONITOR_WITH_REPORTING.
 */
#include "monitor.h"

#include "solari/solariJson.h"
#include "solari/solariLog.h"
#include "solari/solariTlv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

/* Bounds for hot-applied probe tuning (defensive; the dashboard validates
 * tighter ranges - the agent only refuses the absurd). */
enum {
    MONITOR_CTRL_INTERVAL_MIN = 1,
    MONITOR_CTRL_INTERVAL_MAX = 86400,
    MONITOR_CTRL_TIMEOUT_MIN  = 50,
    MONITOR_CTRL_TIMEOUT_MAX  = 60000,
    MONITOR_CTRL_BLOB_MAX     = 60 * 1024
};

/* ===================================================================== */
/* Pure: blob -> config                                                  */
/* ===================================================================== */

solariStatus monitorControlApplyBlob(monitorConfig *cfg, const char *json, size_t len)
{
    uint64_t v;

    if (!cfg) return ERR_INVALID_ARG;
    if (!json || len == 0) return SOLARI_OK;       /* empty blob: epoch-only */
    if (len > MONITOR_CTRL_BLOB_MAX) return ERR_BUFFER_FULL;

    /* Whole-document gate: malformed blobs are a no-op, never partial. */
    if (solariJsonValidate(json, len) != SOLARI_OK) return ERR_INVALID_ARG;

    if (solariJsonGetU64(json, len, "roundIntervalSec", &v) == SOLARI_OK) {
        if (v < MONITOR_CTRL_INTERVAL_MIN) v = MONITOR_CTRL_INTERVAL_MIN;
        if (v > MONITOR_CTRL_INTERVAL_MAX) v = MONITOR_CTRL_INTERVAL_MAX;
        cfg->roundIntervalSec = (uint32_t)v;
    }
    if (solariJsonGetU64(json, len, "probesPerRound", &v) == SOLARI_OK) {
        if (v < 1)   v = 1;
        if (v > 100) v = 100;
        cfg->probesPerRound = (uint16_t)v;
    }
    if (solariJsonGetU64(json, len, "probeTimeoutMs", &v) == SOLARI_OK) {
        if (v < MONITOR_CTRL_TIMEOUT_MIN) v = MONITOR_CTRL_TIMEOUT_MIN;
        if (v > MONITOR_CTRL_TIMEOUT_MAX) v = MONITOR_CTRL_TIMEOUT_MAX;
        cfg->probeTimeoutMs = (uint32_t)v;
    }
    if (solariJsonGetU64(json, len, "replFactor", &v) == SOLARI_OK) {
        if (v < 1) v = 1;
        if (v > MONITOR_MAX_FLEET) v = MONITOR_MAX_FLEET;
        cfg->replFactor = (uint8_t)v;
    }

    /* Probe target SET: when present, the array is the desired state - the
     * schedule is rebuilt through the one converging add path. Invalid specs
     * are skipped with a log (tolerant), valid ones land. */
    if (solariJsonHasArray(json, len, "targets")) {
        char spec[MONITOR_TARGETID_MAX];
        size_t i;
        solariStatus rc;

        cfg->targetCount = 0;
        for (i = 0; cfg->targetCount < MONITOR_MAX_TARGETS; i++) {
            rc = solariJsonGetStrAt(json, len, "targets", i, spec, sizeof spec);
            if (rc == ERR_TLV_END) break;
            if (rc != SOLARI_OK) continue;
            if (spec[0] == '\0') continue;
            if (monitorAddTarget(cfg, spec) != SOLARI_OK)
                solariLogf(SOLARI_LOG_WARN,
                           "control: skipped bad target spec '%s'", spec);
        }
    }
    return SOLARI_OK;
}

/* ===================================================================== */
/* Pure: directive -> outcome + CONTROL_RESULT payload                   */
/* ===================================================================== */

/* Build the CONTROL_RESULT payload: echo the verb, report the magnitude
 * (0 = success), and the epoch this node now reports applied. The wire shape
 * matches serverControlOnResult's parser byte-for-byte. */
static solariStatus buildResult(uint8_t verb, solariStatus outcome,
                                uint64_t appliedEpoch,
                                uint8_t *buf, size_t cap,
                                size_t *outLen, uint16_t *tlvCount)
{
    solariTlvWriter w;
    uint16_t mag = (uint16_t)(outcome >= 0 ? 0 : -outcome);
    solariStatus rc;

    solariTlvWriterInit(&w, buf, cap);
    rc = solariTlvAppendU8(&w, TLV_CTRL_VERB, verb);
    if (rc == SOLARI_OK) rc = solariTlvAppendU16(&w, TLV_ERROR_CODE, mag);
    if (rc == SOLARI_OK) rc = solariTlvAppendU64(&w, TLV_CTRL_TARGET_EPOCH, appliedEpoch);
    if (rc != SOLARI_OK) return rc;
    if (outLen)    *outLen    = w.len;
    if (tlvCount)  *tlvCount  = w.count;
    return SOLARI_OK;
}

solariStatus monitorHandleControl(monitorControlState *st, monitorConfig *cfg,
                                  uint64_t selfNodeId,
                                  const uint8_t *payload, size_t payloadLen,
                                  uint8_t *resultBuf, size_t resultCap,
                                  size_t *resultLen, uint16_t *resultTlvCount,
                                  monitorControlOutcome *out)
{
    solariControl c;
    solariStatus  rc, outcome;
    char spec[MONITOR_TARGETID_MAX];
    uint16_t n;

    if (out) memset(out, 0, sizeof *out);
    if (resultLen)      *resultLen      = 0;
    if (resultTlvCount) *resultTlvCount = 0;
    if (!st || !cfg || !resultBuf || !out) return ERR_INVALID_ARG;
    if (!payload && payloadLen > 0) return ERR_INVALID_ARG;

    rc = solariMsgParseControl(payload ? payload : (const uint8_t *)"",
                               payload ? payloadLen : 0, &c);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_WARN, "control: malformed directive dropped: %s",
                   solariStrError(rc));
        return rc;
    }

    /* Addressing: the PUB channel reaches the whole fleet; act only on
     * directives for this node (or legacy broadcasts with no addressee). */
    if (c.targetNode != 0 && c.targetNode != selfNodeId)
        return SOLARI_OK;                       /* silently not for us */

    out->verb = c.verb;

    switch (c.verb) {
    case CTRL_ADOPT_TARGET:
        /* The adopt payload is the target spec; copy it NUL-terminated (TLV
         * strings carry no terminator), bound to a targetId's worth of bytes.
         * No epoch semantics: adoption is a live-schedule nudge. */
        if (!c.payload || c.payloadLen == 0) outcome = ERR_INVALID_ARG;
        else {
            n = c.payloadLen < sizeof spec - 1 ? c.payloadLen : (uint16_t)(sizeof spec - 1);
            memcpy(spec, c.payload, n);
            spec[n] = '\0';
            outcome = monitorAddTarget(cfg, spec);
            if (outcome == SOLARI_OK)
                solariLogf(SOLARI_LOG_INFO, "adopted target '%s' (%u live)",
                           spec, (unsigned)cfg->targetCount);
            else
                solariLogf(SOLARI_LOG_ERROR, "adopt target '%s' failed: %s",
                           spec, solariStrError(outcome));
        }
        break;

    case CTRL_SET_CONFIG:
    case CTRL_PROVISION:
        /* Epoch monotonicity: never re-apply the past; ack as converged so a
         * re-published directive settles the server's drift view. */
        if (c.targetEpoch <= st->appliedEpoch) {
            solariLogf(SOLARI_LOG_INFO,
                       "control: epoch %llu already applied (at %llu); ack only",
                       (unsigned long long)c.targetEpoch,
                       (unsigned long long)st->appliedEpoch);
            outcome = SOLARI_OK;
            break;
        }
        outcome = monitorControlApplyBlob(cfg, (const char *)c.payload,
                                          c.payloadLen);
        if (outcome == SOLARI_OK) {
            st->appliedEpoch = c.targetEpoch;
            out->applied = true;
            out->blob    = c.payload;
            out->blobLen = c.payloadLen;
            solariLogf(SOLARI_LOG_INFO,
                       "control: verb %u applied, epoch %llu (round %us, "
                       "%u targets)",
                       (unsigned)c.verb, (unsigned long long)c.targetEpoch,
                       (unsigned)cfg->roundIntervalSec,
                       (unsigned)cfg->targetCount);
        } else {
            solariLogf(SOLARI_LOG_ERROR,
                       "control: verb %u epoch %llu apply failed: %s",
                       (unsigned)c.verb, (unsigned long long)c.targetEpoch,
                       solariStrError(outcome));
        }
        break;

    default:
        solariLogf(SOLARI_LOG_WARN, "control verb %u not handled", (unsigned)c.verb);
        outcome = ERR_UNKNOWN_MSG;
        break;
    }

    rc = buildResult(c.verb, outcome, st->appliedEpoch,
                     resultBuf, resultCap, resultLen, resultTlvCount);
    if (rc != SOLARI_OK) return rc;            /* result buffer too small */
    out->wantReply = true;
    return outcome;
}

/* ===================================================================== */
/* Applied-state persistence ("<epoch>\n<blob>")                         */
/* ===================================================================== */

void monitorControlStatePath(const monitorConfig *cfg, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!cfg) return;
    if (cfg->ctrlStateFile[0])
        snprintf(out, cap, "%s", cfg->ctrlStateFile);
    else if (cfg->spoolDb[0])
        snprintf(out, cap, "%s.ctrl", cfg->spoolDb);
}

solariStatus monitorControlStateLoad(monitorControlState *st, monitorConfig *cfg)
{
    char   path[sizeof cfg->ctrlStateFile + 8];
    FILE  *f;
    char  *blob = NULL;
    char   line[32];
    uint64_t epoch;
    long   sz;
    size_t blobLen = 0;

    if (!st || !cfg) return ERR_INVALID_ARG;
    st->appliedEpoch = cfg->configEpoch;        /* .conf floor */

    monitorControlStatePath(cfg, path, sizeof path);
    if (path[0] == '\0') return SOLARI_OK;      /* persistence off */

    f = fopen(path, "rb");
    if (!f) return SOLARI_OK;                    /* first run: nothing saved */

    if (!fgets(line, sizeof line, f)) { fclose(f); return SOLARI_OK; }
    epoch = (uint64_t)strtoull(line, NULL, 10);

    if (fseek(f, 0, SEEK_END) == 0 && (sz = ftell(f)) > 0) {
        size_t hdr = strlen(line);
        if ((size_t)sz > hdr && (size_t)sz - hdr <= MONITOR_CTRL_BLOB_MAX) {
            blobLen = (size_t)sz - hdr;
            blob = (char *)malloc(blobLen + 1);
            if (blob && fseek(f, (long)hdr, SEEK_SET) == 0 &&
                fread(blob, 1, blobLen, f) == blobLen)
                blob[blobLen] = '\0';
            else { free(blob); blob = NULL; }
        }
    }
    fclose(f);

    if (epoch > st->appliedEpoch) {
        if (blob && monitorControlApplyBlob(cfg, blob, blobLen) == SOLARI_OK) {
            cfg->configEpoch = epoch;
            st->appliedEpoch = epoch;
            solariLogf(SOLARI_LOG_INFO,
                       "control: restored applied epoch %llu from %s",
                       (unsigned long long)epoch, path);
        } else {
            solariLogf(SOLARI_LOG_WARN,
                       "control: stale state in %s ignored (bad blob)", path);
        }
    }
    free(blob);
    return SOLARI_OK;
}

solariStatus monitorControlStateSave(const monitorConfig *cfg,
                                     const monitorControlState *st,
                                     const uint8_t *blob, uint16_t blobLen)
{
    char path[sizeof cfg->ctrlStateFile + 8];
    char tmp[sizeof cfg->ctrlStateFile + 32];
    FILE *f = NULL;
    int fd = -1, failed = 0;

    if (!cfg || !st || !blob || blobLen == 0 ||
        blobLen > MONITOR_CTRL_BLOB_MAX)
        return ERR_INVALID_ARG;
    monitorControlStatePath(cfg, path, sizeof path);
    if (path[0] == '\0') return ERR_INVALID_ARG;

    if (snprintf(tmp, sizeof tmp, "%s.tmp.XXXXXX", path) >= (int)sizeof tmp)
        return ERR_BUFFER_FULL;
#ifdef _WIN32
    if (_mktemp_s(tmp, strlen(tmp) + 1) != 0) return ERR_PLATFORM;
    fd = _open(tmp, _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
               _S_IREAD | _S_IWRITE);
    f = fd < 0 ? NULL : _fdopen(fd, "wb");
#else
    fd = mkstemp(tmp);
    f = fd < 0 ? NULL : fdopen(fd, "wb");
#endif
    if (!f) {
        if (fd >= 0) {
#ifdef _WIN32
            _close(fd);
#else
            close(fd);
#endif
            remove(tmp);
        }
        return ERR_PLATFORM;
    }
    if (fprintf(f, "%llu\n", (unsigned long long)st->appliedEpoch) < 0 ||
        (blobLen && fwrite(blob, 1, blobLen, f) != blobLen) || fflush(f) != 0)
        failed = 1;
#ifdef _WIN32
    if (!failed && _commit(fd) != 0) failed = 1;
#else
    if (!failed && fsync(fd) != 0) failed = 1;
#endif
    if (fclose(f) != 0) failed = 1;
    if (failed) { remove(tmp); return ERR_PLATFORM; }
    if (rename(tmp, path) != 0) { remove(tmp); return ERR_PLATFORM; }
#ifndef _WIN32
    {
        char directory[sizeof path];
        char *slash;
        int dirfd;
        (void)snprintf(directory, sizeof directory, "%s", path);
        slash = strrchr(directory, '/');
        if (!slash) (void)snprintf(directory, sizeof directory, ".");
        else if (slash == directory) slash[1] = '\0';
        else *slash = '\0';
        dirfd = open(directory, O_RDONLY);
        if (dirfd < 0) return ERR_PLATFORM;
        failed = fsync(dirfd) != 0;
        if (close(dirfd) != 0) failed = 1;
        if (failed) return ERR_PLATFORM;
    }
#endif
    return SOLARI_OK;
}

solariStatus monitorHandleControlDurable(monitorControlState *st,
                                         monitorConfig *cfg,
                                         uint64_t selfNodeId,
                                         const uint8_t *payload,
                                         size_t payloadLen,
                                         uint8_t *resultBuf,
                                         size_t resultCap,
                                         size_t *resultLen,
                                         uint16_t *resultTlvCount,
                                         monitorControlOutcome *out)
{
    monitorConfig candidate;
    monitorControlState proposed;
    solariStatus rc;

    if (!st || !cfg || !out) return ERR_INVALID_ARG;
    candidate = *cfg;
    proposed = *st;
    rc = monitorHandleControl(&proposed, &candidate, selfNodeId,
                              payload, payloadLen, resultBuf, resultCap,
                              resultLen, resultTlvCount, out);
    if (rc != SOLARI_OK || !out->applied) return rc;
    rc = monitorControlStateSave(&candidate, &proposed, out->blob, out->blobLen);
    if (rc != SOLARI_OK) {
        out->applied = false;
        if (buildResult(out->verb, rc, st->appliedEpoch, resultBuf, resultCap,
                        resultLen, resultTlvCount) != SOLARI_OK)
            out->wantReply = false;
        solariLogf(SOLARI_LOG_ERROR,
                   "control: durable save failed; epoch remains %llu",
                   (unsigned long long)st->appliedEpoch);
        return rc;
    }
    candidate.configEpoch = proposed.appliedEpoch;
    *cfg = candidate;
    *st = proposed;
    return SOLARI_OK;
}

/* ===================================================================== */
/* Transport glue (SUB dial + poll + reply)                              */
/* ===================================================================== */
#ifdef MONITOR_WITH_REPORTING

#include "solari/solariFrame.h"
#include "solari/solariTime.h"

#define MONITOR_CTRL_RECV_SLICE_MS 250   /* idle recv quantum in the poll loop */
#define MONITOR_CTRL_BURST 16            /* directive bound per poll call      */

/* Derive the pub-channel URL: configured subUrl wins; otherwise re-point the
 * primary ingest URL at the standard pub port 7703, preserving scheme and the
 * dial-by-name host so the server cert's DNS SAN verifies under mbedTLS. */
static void ctrlDeriveSubUrl(const monitorConfig *cfg, char *out, size_t cap)
{
    const char *p, *hostEnd;
    size_t n;

    if (cfg->subUrl[0]) { snprintf(out, cap, "%s", cfg->subUrl); return; }
    out[0] = '\0';
    if (!cfg->primaryUrl[0]) return;

    p = strstr(cfg->primaryUrl, "://");
    p = p ? p + 3 : cfg->primaryUrl;
    for (hostEnd = p; *hostEnd && *hostEnd != ':' && *hostEnd != '/'; hostEnd++) { }

    n = (size_t)(hostEnd - cfg->primaryUrl);
    if (n >= cap) return;                       /* absurd URL; leave empty */
    memcpy(out, cfg->primaryUrl, n);
    out[n] = '\0';
    {
        size_t used = strlen(out);
        snprintf(out + used, cap - used, ":7703");
    }
}

solariStatus monitorControlOpen(const monitorConfig *cfg, monitorControlIo *io)
{
    solariConnOpts o;
    solariStatus   rc;

    if (!cfg || !io) return ERR_INVALID_ARG;
    memset(io, 0, sizeof *io);
    ctrlDeriveSubUrl(cfg, io->subUrl, sizeof io->subUrl);
    if (io->subUrl[0] == '\0') return ERR_CONN_RETRY;

    memset(&o, 0, sizeof o);
    o.url           = io->subUrl;
    o.pattern       = SOLARI_PATTERN_SUB;
    o.useTls        = strncmp(io->subUrl, "tls+", 4) == 0;
    o.caFile        = cfg->caFile[0]   ? cfg->caFile   : NULL;
    o.certFile      = cfg->certFile[0] ? cfg->certFile : NULL;
    o.keyFile       = cfg->keyFile[0]  ? cfg->keyFile  : NULL;
    o.dialTimeoutMs = 3000;
    o.sendTimeoutMs = 1000;
    o.recvTimeoutMs = MONITOR_CTRL_RECV_SLICE_MS;

    rc = solariConnOpen(&o, &io->sub);
    if (rc != SOLARI_OK) {
        io->sub = NULL;
        solariLogf(SOLARI_LOG_WARN,
                   "control: subscribe to %s failed (%s); directives deferred",
                   io->subUrl, solariStrError(rc));
        return rc;
    }
    solariLogf(SOLARI_LOG_INFO, "control: subscribed to %s", io->subUrl);
    return SOLARI_OK;
}

void monitorControlClose(monitorControlIo *io)
{
    if (io && io->sub) { solariConnClose(io->sub); io->sub = NULL; }
}

/* Push the CONTROL_RESULT over the reporter (ingest channel). Durable: an
 * offline result spools with the reports and replays on reconnect. */
static void ctrlSendResult(monitorContext *ctx, uint32_t correlationId,
                           const uint8_t *tlv, size_t tlvLen, uint16_t tlvCount)
{
    uint8_t frame[MONITOR_CTRL_RESULT_CAP + 128];
    size_t  flen = 0;

    if (solariReporterFrameCorr(ctx->rep, SCP_MSG_CONTROL_RESULT,
                                SCP_FLAG_NONE, correlationId,
                                tlv, tlvLen, tlvCount,
                                frame, sizeof frame, &flen) != SOLARI_OK)
        return;
    (void)solariReporterSend(ctx->rep, frame, flen);
}

void monitorControlPoll(monitorControlIo *io, monitorContext *ctx,
                        monitorConfig *cfg, monitorControlState *st,
                        uint32_t waitMs)
{
    uint64_t deadline = solariNowUnixMs() + waitMs;
    int      handled  = 0;

    if (!io || !io->sub) {                      /* no channel: plain sleep */
        if (waitMs) solariSleepMs(waitMs);
        return;
    }

    do {
        const uint8_t    *buf = NULL, *payload = NULL;
        size_t            len = 0, plen = 0;
        solariFrameHeader hdr;
        uint8_t           tlv[MONITOR_CTRL_RESULT_CAP];
        size_t            tlvLen = 0;
        uint16_t          tlvCount = 0;
        monitorControlOutcome oc;
        solariStatus      rc;
        uint64_t          now;

        rc = solariConnRecv(io->sub, &buf, &len);   /* blocks <= one slice */
        if (rc == SOLARI_OK) {
            if (solariFrameParse(buf, len, &hdr, &payload, &plen, NULL) == SOLARI_OK &&
                hdr.msgType == SCP_MSG_CONTROL &&
                handled < MONITOR_CTRL_BURST) {
                handled++;
                (void)monitorHandleControlDurable(st, cfg, ctx->nodeId,
                                           payload, plen, tlv, sizeof tlv,
                                           &tlvLen, &tlvCount, &oc);
                if (oc.wantReply) {
                    if (!solariReporterConnected(ctx->rep))
                        (void)solariReporterConnect(ctx->rep);
                    ctrlSendResult(ctx, hdr.seqNo, tlv, tlvLen, tlvCount);
                }
            }
        } else if (rc == ERR_CONN_FATAL) {
            solariLogf(SOLARI_LOG_WARN, "control: subscribe link lost");
            solariConnClose(io->sub);
            io->sub = NULL;
            break;
        }
        now = solariNowUnixMs();
        if (now >= deadline) break;
        if (deadline - now < MONITOR_CTRL_RECV_SLICE_MS && rc != SOLARI_OK) {
            solariSleepMs((uint32_t)(deadline - now));
            break;
        }
    } while (waitMs > 0);

    if (!io->sub) {                             /* dropped mid-wait: keep cadence */
        uint64_t now = solariNowUnixMs();
        if (now < deadline) solariSleepMs((uint32_t)(deadline - now));
    }
}

#endif /* MONITOR_WITH_REPORTING */
