/*
 * clientControl.c - client control-plane receive + apply (§7.3, §9.1).
 *
 * See clientControl.h for the layering contract. The pure half (apply +
 * handle) mirrors serverControl.c's tolerant parser style: every byte comes
 * off the broadcast PUB channel and is untrusted, so parsing is bounded,
 * unknown TLVs/JSON keys are skipped, and a malformed directive is rejected
 * whole (never partially applied, never fatal).
 *
 * Epoch safety (§7.3): a directive whose targetEpoch is <= the epoch this
 * node has already applied is answered as converged (CONTROL_RESULT carrying
 * the current applied epoch) but NOT re-applied, so an idempotent
 * re-provision converges instead of thrashing the live config.
 */
#include "clientControl.h"

#include "solari/solariJson.h"
#include "solari/solariLog.h"
#include "solari/solariTlv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bounds for hot-applied scheduling values (defensive: the dashboard
 * validates tighter ranges; the agent only refuses the absurd). */
enum {
    CLIENT_CTRL_INTERVAL_MIN = 1,
    CLIENT_CTRL_INTERVAL_MAX = 86400,
    CLIENT_CTRL_BLOB_MAX     = 60 * 1024   /* mirrors PROV_PAYLOAD_CAP order */
};

/* ===================================================================== */
/* Pure: blob -> config                                                  */
/* ===================================================================== */

/* Replace cfg's watched-process list from a JSON string array under `key`.
 * Returns true if the key existed as an array (list replaced). */
static bool applyProcessList(clientConfig *cfg, const char *json, size_t len,
                             const char *key)
{
    char name[SOLARI_PROCNAME_MAX];
    size_t i;
    uint8_t n = 0;
    solariStatus rc;

    if (!solariJsonHasArray(json, len, key)) return false;

    for (i = 0; n < SOLARI_MAX_PROCS; i++) {
        rc = solariJsonGetStrAt(json, len, key, i, name, sizeof name);
        if (rc == ERR_TLV_END) break;
        if (rc != SOLARI_OK) continue;       /* skip non-string entries */
        if (name[0] == '\0') continue;
        strncpy(cfg->procs[n], name, SOLARI_PROCNAME_MAX - 1);
        cfg->procs[n][SOLARI_PROCNAME_MAX - 1] = '\0';
        n++;
    }
    cfg->procCount = n;
    return true;
}

/* Replace cfg's watched-log list from a JSON string array of
 * "path[ : regex]" entries under `key` (the .conf `logfile =` syntax). */
static bool applyLogList(clientConfig *cfg, const char *json, size_t len,
                         const char *key)
{
    char entry[SOLARI_LOGPATH_MAX + 132];
    size_t i;
    uint8_t n = 0;
    solariStatus rc;

    if (!solariJsonHasArray(json, len, key)) return false;

    for (i = 0; n < SOLARI_MAX_LOGS; i++) {
        clientLogWatch *w;
        const char *sep;
        size_t plen;

        rc = solariJsonGetStrAt(json, len, key, i, entry, sizeof entry);
        if (rc == ERR_TLV_END) break;
        if (rc != SOLARI_OK) continue;
        if (entry[0] == '\0') continue;

        w = &cfg->logs[n];
        memset(w, 0, sizeof *w);
        sep = strstr(entry, " : ");                 /* "path : regex" */
        if (sep) {
            plen = (size_t)(sep - entry);
            if (plen >= sizeof w->path) plen = sizeof w->path - 1;
            memcpy(w->path, entry, plen);
            w->path[plen] = '\0';
            strncpy(w->regex, sep + 3, sizeof w->regex - 1);
        } else {
            strncpy(w->path, entry, sizeof w->path - 1);
        }
        if (w->path[0] == '\0') continue;
        n++;
    }
    cfg->logCount = n;
    return true;
}

solariStatus clientControlApplyBlob(clientConfig *cfg, const char *json, size_t len)
{
    uint64_t v;

    if (!cfg) return ERR_INVALID_ARG;
    if (!json || len == 0) return SOLARI_OK;       /* empty blob: epoch-only */
    if (len > CLIENT_CTRL_BLOB_MAX) return ERR_BUFFER_FULL;

    /* Whole-document gate: a malformed blob must be a no-op, never a partial
     * application of whichever keys happened to scan out of the wreckage. */
    if (solariJsonValidate(json, len) != SOLARI_OK) return ERR_INVALID_ARG;

    if (solariJsonGetU64(json, len, "sampleIntervalSec", &v) == SOLARI_OK) {
        if (v < CLIENT_CTRL_INTERVAL_MIN) v = CLIENT_CTRL_INTERVAL_MIN;
        if (v > CLIENT_CTRL_INTERVAL_MAX) v = CLIENT_CTRL_INTERVAL_MAX;
        cfg->sampleIntervalSec = (uint32_t)v;
    }
    if (solariJsonGetU64(json, len, "watchdogIntervalSec", &v) == SOLARI_OK) {
        if (v > CLIENT_CTRL_INTERVAL_MAX) v = CLIENT_CTRL_INTERVAL_MAX;
        cfg->watchdogIntervalSec = (uint32_t)v;
    }

    /* Watch lists: replace-when-present (the blob is the desired state). */
    if (!applyProcessList(cfg, json, len, "processes"))
        (void)applyProcessList(cfg, json, len, "procs");
    if (!applyLogList(cfg, json, len, "logfiles"))
        (void)applyLogList(cfg, json, len, "logs");

    return SOLARI_OK;
}

/* ===================================================================== */
/* Pure: directive -> outcome + CONTROL_RESULT payload                   */
/* ===================================================================== */

/* Build the CONTROL_RESULT payload: echoed verb, error magnitude (0 = ok),
 * and the epoch this node now reports applied (the convergence signal
 * serverControlOnResult folds into serverDbSetNodeApplied). */
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
    if (outLen)   *outLen   = w.len;
    if (tlvCount) *tlvCount = w.count;
    return SOLARI_OK;
}

solariStatus clientControlHandle(clientControlState *st, clientConfig *cfg,
                                 uint64_t selfNodeId,
                                 const uint8_t *payload, size_t payloadLen,
                                 uint8_t *resultBuf, size_t resultCap,
                                 size_t *resultLen, uint16_t *resultTlvCount,
                                 clientControlOutcome *out)
{
    solariControl c;
    solariStatus  rc, outcome;

    if (out) memset(out, 0, sizeof *out);
    if (resultLen)      *resultLen      = 0;
    if (resultTlvCount) *resultTlvCount = 0;
    if (!st || !cfg || !resultBuf || !out) return ERR_INVALID_ARG;
    if (!payload && payloadLen > 0) return ERR_INVALID_ARG;

    rc = solariMsgParseControl(payload ? payload : (const uint8_t *)"",
                               payload ? payloadLen : 0, &c);
    if (rc != SOLARI_OK) {
        /* Malformed wire input off the broadcast channel: drop, no reply
         * (there is no trustworthy verb/epoch to echo). */
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
    case CTRL_SET_CONFIG:
    case CTRL_PROVISION:
        /* Epoch monotonicity: never re-apply the past. Ack as converged so a
         * re-published directive settles the server's drift view. */
        if (c.targetEpoch <= st->appliedEpoch) {
            solariLogf(SOLARI_LOG_INFO,
                       "control: epoch %llu already applied (at %llu); ack only",
                       (unsigned long long)c.targetEpoch,
                       (unsigned long long)st->appliedEpoch);
            outcome = SOLARI_OK;
            break;
        }
        outcome = clientControlApplyBlob(cfg, (const char *)c.payload,
                                         c.payloadLen);
        if (outcome == SOLARI_OK) {
            st->appliedEpoch = c.targetEpoch;
            out->applied = true;
            out->blob    = c.payload;
            out->blobLen = c.payloadLen;
            solariLogf(SOLARI_LOG_INFO,
                       "control: verb %u applied, epoch %llu (interval %us, "
                       "%u procs, %u logs)",
                       (unsigned)c.verb, (unsigned long long)c.targetEpoch,
                       (unsigned)cfg->sampleIntervalSec,
                       (unsigned)cfg->procCount, (unsigned)cfg->logCount);
        } else {
            solariLogf(SOLARI_LOG_ERROR,
                       "control: verb %u epoch %llu apply failed: %s",
                       (unsigned)c.verb, (unsigned long long)c.targetEpoch,
                       solariStrError(outcome));
        }
        break;

    default:
        solariLogf(SOLARI_LOG_WARN, "control: verb %u not handled",
                   (unsigned)c.verb);
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

void clientControlStatePath(const clientConfig *cfg, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!cfg) return;
    if (cfg->ctrlStateFile[0])
        snprintf(out, cap, "%s", cfg->ctrlStateFile);
    else if (cfg->spoolDb[0])
        snprintf(out, cap, "%s.ctrl", cfg->spoolDb);
}

solariStatus clientControlStateLoad(clientControlState *st, clientConfig *cfg)
{
    char   path[CLIENT_PATH_MAX + 8];
    FILE  *f;
    char  *blob = NULL;
    char   line[32];
    uint64_t epoch;
    long   sz;
    size_t blobLen = 0;

    if (!st || !cfg) return ERR_INVALID_ARG;
    st->appliedEpoch = cfg->configEpoch;        /* .conf floor */

    clientControlStatePath(cfg, path, sizeof path);
    if (path[0] == '\0') return SOLARI_OK;      /* persistence off */

    f = fopen(path, "rb");
    if (!f) return SOLARI_OK;                    /* first run: nothing saved */

    if (!fgets(line, sizeof line, f)) { fclose(f); return SOLARI_OK; }
    epoch = (uint64_t)strtoull(line, NULL, 10);

    /* The rest of the file is the applied blob. */
    if (fseek(f, 0, SEEK_END) == 0 && (sz = ftell(f)) > 0) {
        size_t hdr = strlen(line);
        if ((size_t)sz > hdr && (size_t)sz - hdr <= CLIENT_CTRL_BLOB_MAX) {
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
        /* Re-apply the persisted push over the .conf so a restart keeps it. */
        if (!blob || clientControlApplyBlob(cfg, blob, blobLen) == SOLARI_OK) {
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

solariStatus clientControlStateSave(const clientConfig *cfg,
                                    const clientControlState *st,
                                    const uint8_t *blob, uint16_t blobLen)
{
    char  path[CLIENT_PATH_MAX + 8];
    char  tmp[CLIENT_PATH_MAX + 16];
    FILE *f;

    if (!cfg || !st) return ERR_INVALID_ARG;
    clientControlStatePath(cfg, path, sizeof path);
    if (path[0] == '\0') return SOLARI_OK;      /* persistence off */

    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) return ERR_PLATFORM;
    fprintf(f, "%llu\n", (unsigned long long)st->appliedEpoch);
    if (blob && blobLen > 0) fwrite(blob, 1, blobLen, f);
    if (fclose(f) != 0) { remove(tmp); return ERR_PLATFORM; }
    if (rename(tmp, path) != 0) { remove(tmp); return ERR_PLATFORM; }
    return SOLARI_OK;
}

/* ===================================================================== */
/* Transport glue (SUB dial + poll + reply)                              */
/* ===================================================================== */
#ifdef CLIENT_WITH_REPORTING

#include "solari/solariFrame.h"
#include "solari/solariTime.h"

/* How long one idle recv may block: the poll loop's pacing quantum. Short
 * enough that a waitMs deadline is honored within one quantum. */
#define CLIENT_CTRL_RECV_SLICE_MS 250
/* Directive burst bound per poll call - a flood cannot starve sampling. */
#define CLIENT_CTRL_BURST 16

/* Derive the pub-channel URL: configured subUrl wins; otherwise re-point the
 * primary ingest URL at the standard pub port (§2: ingest 7701 / control 7702
 * / pub 7703), preserving scheme and the DIAL-BY-NAME host so the server
 * cert's DNS SAN still verifies under mbedTLS. */
static void ctrlDeriveSubUrl(const clientConfig *cfg, char *out, size_t cap)
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

solariStatus clientControlOpen(const clientConfig *cfg, clientControlIo *io)
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
    o.recvTimeoutMs = CLIENT_CTRL_RECV_SLICE_MS;

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

void clientControlClose(clientControlIo *io)
{
    if (io && io->sub) { solariConnClose(io->sub); io->sub = NULL; }
}

/* Send the CONTROL_RESULT for one handled directive over the reporter (push
 * channel -> server ingest -> serverControlOnResult). Durable: an offline
 * result spools with the reports and replays on reconnect, so convergence
 * survives a flapping link. */
static void ctrlSendResult(clientContext *ctx, uint32_t correlationId,
                           const uint8_t *tlv, size_t tlvLen, uint16_t tlvCount)
{
    uint8_t frame[CLIENT_CTRL_RESULT_CAP + 128];
    size_t  flen = 0;

    if (solariReporterFrameCorr(ctx->rep, SCP_MSG_CONTROL_RESULT,
                                SCP_FLAG_NONE, correlationId,
                                tlv, tlvLen, tlvCount,
                                frame, sizeof frame, &flen) != SOLARI_OK)
        return;
    (void)solariReporterSend(ctx->rep, frame, flen);
}

void clientControlPoll(clientControlIo *io, clientContext *ctx,
                       clientConfig *cfg, clientControlState *st,
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
        uint8_t           tlv[CLIENT_CTRL_RESULT_CAP];
        size_t            tlvLen = 0;
        uint16_t          tlvCount = 0;
        clientControlOutcome oc;
        solariStatus      rc;
        uint64_t          now;

        rc = solariConnRecv(io->sub, &buf, &len);   /* blocks <= one slice */
        if (rc == SOLARI_OK) {
            if (solariFrameParse(buf, len, &hdr, &payload, &plen, NULL) == SOLARI_OK &&
                hdr.msgType == SCP_MSG_CONTROL &&
                handled < CLIENT_CTRL_BURST) {
                handled++;
                (void)clientControlHandle(st, cfg, ctx->nodeId, payload, plen,
                                          tlv, sizeof tlv, &tlvLen, &tlvCount,
                                          &oc);
                if (oc.applied &&
                    clientControlStateSave(cfg, st, oc.blob, oc.blobLen) != SOLARI_OK)
                    /* Hot-apply succeeded (running config is current, so the
                     * convergence ack below is truthful for this process); only
                     * restart-durability failed. Make it loud rather than silent. */
                    solariLogf(SOLARI_LOG_ERROR,
                               "control: applied epoch %llu but state persist "
                               "failed; config will not survive a restart",
                               (unsigned long long)st->appliedEpoch);
                if (oc.wantReply) {
                    if (!solariReporterConnected(ctx->rep))
                        (void)solariReporterConnect(ctx->rep);
                    ctrlSendResult(ctx, hdr.seqNo, tlv, tlvLen, tlvCount);
                }
            }
            /* non-CONTROL publishes (surveys etc.) are other increments */
        } else if (rc == ERR_CONN_FATAL) {
            solariLogf(SOLARI_LOG_WARN, "control: subscribe link lost");
            solariConnClose(io->sub);
            io->sub = NULL;
            break;
        }
        /* ERR_CONN_RETRY = idle slice elapsed; loop until the deadline */
        now = solariNowUnixMs();
        if (now >= deadline) break;
        if (deadline - now < CLIENT_CTRL_RECV_SLICE_MS && rc != SOLARI_OK) {
            solariSleepMs((uint32_t)(deadline - now));
            break;
        }
    } while (waitMs > 0);

    /* Channel dropped mid-wait: honor the remaining sleep so the sample
     * cadence is preserved. */
    if (!io->sub) {
        uint64_t now = solariNowUnixMs();
        if (now < deadline) solariSleepMs((uint32_t)(deadline - now));
    }
}

#endif /* CLIENT_WITH_REPORTING */
