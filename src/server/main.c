/*
 * main.c - solariServer entry point (§9).
 *
 * Thin: load config, open serverDb, init the context, then run the loops -
 * lease tick (failover), ingest accept + dispatch, control-plane service, and
 * the operator bridge poll. All real logic lives in libservercore; this file
 * only wires the subsystems together.
 *
 * The loop (§9 data flow):
 *   1. serverLeaseTick  - every cfg->leaseRenewSec, claim/renew the shared lease.
 *      Winning binds the ingest/control/pub listeners (serverApplyRole, driven
 *      from inside the tick); losing/erroring tears them down. Only the ACTIVE
 *      master touches the fleet.
 *   2. ingest (PULL)    - drain legacy/non-durable WATCHDOG, DISCOVERY,
 *      TOPOLOGY, and CONTROL_RESULT frames: parse, authorize by the presenting
 *      cert CN, and dispatch. This one-way link does not confirm persistence.
 *   3. control (REP)    - answer HELLO and WHO_IS_PRIMARY requests, and process
 *      durable CLIENT/MONITOR/SURVEY reports before returning an ACK. REP is
 *      lockstep, so every accepted request is answered (ERROR on failure).
 *   4. operator bridge  - serverCtlPoll services pending dashboard requests on
 *      the Unix socket (non-blocking).
 *
 * Shutdown is signal-driven (SIGINT/SIGTERM): the handler trips a flag the loop
 * checks each iteration, so sockets, the bridge, the context and the db are torn
 * down cleanly on the way out.
 */
#define _POSIX_C_SOURCE 200809L   /* sigaction/sigemptyset under -std=c99 */

#include "server.h"

#include "solari/solariLog.h"
#include "solari/solariTime.h"
#include "solari/solariTlv.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Loop tuning. The active listeners are bound with a ~100ms recv timeout
 * (serverLease), so an idle active loop paces itself on those timeouts; a standby
 * server has no listeners bound and idles on this sleep until the next tick. */
enum {
    SERVER_INGEST_BURST   = 64,   /* max ingest frames drained per iteration   */
    SERVER_STANDBY_IDLE_MS = 100, /* standby pause between lease re-evaluations */
    SERVER_CTL_QUEUE_BATCH = 8    /* max queued privileged requests per tick    */
};

/* Reply scratch sizes. WELCOME / PRIMARY_IS / ERROR payloads are a handful of
 * small TLVs; these bound the stack scratch well under SOLARI_MAX_FRAME. */
enum {
    SERVER_REPLY_TLV_MAX   = 1024,
    SERVER_REPLY_FRAME_MAX = 2048
};

/* ----------------------------------------------------------- signal handling */

static volatile sig_atomic_t g_stop = 0;

static void serverOnSignal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void serverInstallSignals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = serverOnSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);   /* a dropped peer must not kill the server */
}

/* ------------------------------------------------------------ reply framing */

/* Per-process monotonic seqNo for server-originated replies on the control link.
 * Only the single main loop emits these, so a plain static counter suffices for
 * the receiver's (sourceNodeId, seqNo) dedup. Mirrors serverControl's emitter. */
static uint32_t serverReplyNextSeq(void)
{
    static uint32_t seq = 0;
    return ++seq;   /* start at 1; 0 is reserved for "unsolicited" */
}

/* Stamp a reply frame header and assemble [len|header|payload|crc] into out.
 * correlationId echoes the request's seqNo so the node matches reply to request
 * (§5.2). payload may be NULL iff payloadLen == 0. */
static solariStatus serverBuildReplyFrame(const serverContext *ctx, uint8_t msgType,
                                          uint32_t correlationId, uint16_t tlvCount,
                                          const uint8_t *payload, size_t payloadLen,
                                          uint8_t *out, size_t cap, size_t *outLen)
{
    solariFrameHeader hdr;

    memset(&hdr, 0, sizeof hdr);
    hdr.magic[0]       = SCP_MAGIC_0;
    hdr.magic[1]       = SCP_MAGIC_1;
    hdr.protoVersion   = SCP_PROTO_VERSION;
    hdr.msgType        = msgType;
    hdr.flags          = SCP_FLAG_NONE;
    hdr.tlvCount       = tlvCount;
    hdr.sourceNodeId   = ctx ? ctx->nodeId : 0;
    hdr.sendTimeUnixMs = solariNowUnixMs();
    hdr.seqNo          = serverReplyNextSeq();
    hdr.correlationId  = correlationId;
    return solariFrameBuild(&hdr, payload, payloadLen, out, cap, outLen);
}

/* Build a structured ERROR (0x51) reply frame so a REP request that cannot be
 * answered normally still gets a well-formed response (REP is lockstep). */
static solariStatus serverBuildErrorReply(const serverContext *ctx, uint32_t corr,
                                          uint16_t code, const char *detail,
                                          uint8_t *out, size_t cap, size_t *outLen)
{
    solariErrorMsg em;
    uint8_t        tlv[256];
    size_t         tlvLen = 0;
    uint16_t       tlvCount = 0;
    solariStatus   rc;

    memset(&em, 0, sizeof em);
    em.code = code;
    snprintf(em.detail, sizeof em.detail, "%s", detail ? detail : "rejected");

    rc = solariMsgBuildError(&em, tlv, sizeof tlv, &tlvLen, &tlvCount);
    if (rc != SOLARI_OK) return rc;

    return serverBuildReplyFrame(ctx, SCP_MSG_ERROR, corr, tlvCount,
                                 tlv, tlvLen, out, cap, outLen);
}

/* Send a best-effort ERROR reply on the control link (used when a request is
 * malformed or rejected). A send failure is logged at debug; the peer is gone. */
static void serverSendError(serverContext *ctx, uint32_t corr,
                            uint16_t code, const char *detail)
{
    uint8_t frame[SERVER_REPLY_FRAME_MAX];
    size_t  frameLen = 0;

    if (serverBuildErrorReply(ctx, corr, code, detail,
                              frame, sizeof frame, &frameLen) == SOLARI_OK)
        (void)solariConnSend(ctx->control, frame, frameLen);
}

static solariStatus serverSendReportAck(serverContext *ctx,
                                        const solariFrameHeader *request,
                                        const uint8_t *payload, size_t payloadLen)
{
    solariTlvReader reader;
    solariTlvWriter writer;
    const uint8_t *value;
    uint16_t type, valueLen;
    uint8_t session[16], ackTlvs[32], frame[SERVER_REPLY_FRAME_MAX];
    size_t ackLen = 0, frameLen = 0;
    uint16_t ackCount = 0;
    solariStatus rc;
    bool found = false;

    solariTlvReaderInit(&reader, payload, payloadLen);
    while ((rc = solariTlvNext(&reader, &type, &value, &valueLen)) == SOLARI_OK) {
        if (type != TLV_REPORT_SESSION) continue;
        if (found || valueLen != sizeof session) return ERR_INVALID_ARG;
        memcpy(session, value, sizeof session);
        found = true;
    }
    if (rc != ERR_TLV_END || !found) return ERR_INVALID_ARG;

    solariTlvWriterInit(&writer, ackTlvs, sizeof ackTlvs);
    rc = solariTlvAppend(&writer, TLV_REPORT_SESSION, session, sizeof session);
    if (rc != SOLARI_OK) return rc;
    ackLen = writer.len;
    ackCount = writer.count;
    rc = serverBuildReplyFrame(ctx, SCP_MSG_ACK, request->seqNo, ackCount,
                               ackTlvs, ackLen, frame, sizeof frame, &frameLen);
    if (rc == SOLARI_OK) rc = solariConnSend(ctx->control, frame, frameLen);
    return rc;
}

/* ------------------------------------------------------------ ingest (PULL) */

/* Drain ready one-way ingest frames, bounded per iteration so a flood cannot
 * starve the lease tick / control link. This remains for watchdog and legacy or
 * non-durable traffic. Durable reports use REP so persistence can be ACKed. */
static void serverDrainIngest(serverContext *ctx)
{
    int processed;

    if (!ctx->ingest) return;

    for (processed = 0; processed < SERVER_INGEST_BURST; processed++) {
        const uint8_t    *buf = NULL, *payload = NULL;
        size_t            len = 0, plen = 0;
        solariFrameHeader hdr;
        char              cn[SERVER_CN_MAX];
        solariStatus      rc;

        rc = solariConnRecv(ctx->ingest, &buf, &len);
        if (rc != SOLARI_OK) break;     /* ERR_CONN_RETRY when idle, else fatal */

        rc = solariFrameParse(buf, len, &hdr, &payload, &plen, NULL);
        if (rc != SOLARI_OK) {
            solariLogf(SOLARI_LOG_WARN, "ingest: frame parse failed: %s",
                       solariStrError(rc));
            continue;
        }

        if (solariConnPeerCn(ctx->ingest, cn, sizeof cn) != SOLARI_OK)
            cn[0] = '\0';

        rc = serverIngestValidate(ctx, &hdr, cn[0] ? cn : NULL);
        if (rc != SOLARI_OK) continue;  /* ERR_AUTH_ROLE / ERR_CONN_RETRY (dup) */

        rc = serverIngestDispatch(ctx, &hdr, payload, plen);
        if (rc != SOLARI_OK)
            solariLogf(SOLARI_LOG_ERROR, "ingest: dispatch failed src=0x%llx seq=%u: %s",
                       (unsigned long long)hdr.sourceNodeId, (unsigned)hdr.seqNo,
                       solariStrError(rc));
    }
}

/* ----------------------------------------------------------- control (REP) */

/* Service one request on the control REP link: HELLO -> WELCOME,
 * WHO_IS_PRIMARY -> PRIMARY_IS, durable report -> post-persistence ACK. REP is
 * lockstep, so every accepted request gets one response (ERROR on failure). */
static void serverServiceControl(serverContext *ctx)
{
    const uint8_t    *buf = NULL, *payload = NULL;
    size_t            len = 0, plen = 0;
    solariFrameHeader hdr;
    char              cn[SERVER_CN_MAX];
    uint8_t           tlv[SERVER_REPLY_TLV_MAX];
    uint8_t           frame[SERVER_REPLY_FRAME_MAX];
    size_t            tlvLen = 0, frameLen = 0;
    uint16_t          tlvCount = 0;
    solariStatus      rc;

    if (!ctx->control) return;

    rc = solariConnRecv(ctx->control, &buf, &len);
    if (rc != SOLARI_OK) return;        /* nothing ready (RETRY) or transient */

    rc = solariFrameParse(buf, len, &hdr, &payload, &plen, NULL);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_WARN, "control: frame parse failed: %s",
                   solariStrError(rc));
        serverSendError(ctx, 0, 1, "malformed frame");
        return;
    }

    if (solariConnPeerCn(ctx->control, cn, sizeof cn) != SOLARI_OK)
        cn[0] = '\0';

    /* Authorize by cert role. A duplicate (ERR_CONN_RETRY) is benign on a
     * request/response link - the request is idempotent, so we re-answer; only a
     * role violation is refused. */
    rc = serverIngestValidate(ctx, &hdr, cn[0] ? cn : NULL);
    if (rc == ERR_AUTH_ROLE) {
        solariLogf(SOLARI_LOG_WARN,
                   "control: rejecting msgType=0x%02x from src=0x%llx (role)",
                   (unsigned)hdr.msgType, (unsigned long long)hdr.sourceNodeId);
        serverSendError(ctx, hdr.seqNo, 2, "role not permitted");
        return;
    }
    if (rc != SOLARI_OK && rc != ERR_CONN_RETRY) {
        solariLogf(SOLARI_LOG_ERROR,
                   "control: identity validation failed for src=0x%llx: %s",
                   (unsigned long long)hdr.sourceNodeId, solariStrError(rc));
        serverSendError(ctx, hdr.seqNo, 8, "identity validation failed");
        return;
    }

    switch (hdr.msgType) {
    case SCP_MSG_CLIENT_REPORT:
    case SCP_MSG_MONITOR_REPORT:
    case SCP_MSG_SURVEY_RESP:
        if ((hdr.flags & SCP_FLAG_ACK_REQ) == 0) {
            serverSendError(ctx, hdr.seqNo, 7, "report ACK required");
            return;
        }
        rc = serverIngestDispatch(ctx, &hdr, payload, plen);
        if (rc != SOLARI_OK) {
            serverSendError(ctx, hdr.seqNo, 8, "report persistence failed");
            return;
        }
        rc = serverSendReportAck(ctx, &hdr, payload, plen);
        if (rc != SOLARI_OK)
            solariLogf(SOLARI_LOG_WARN, "control: report ACK send failed src=0x%llx seq=%u",
                       (unsigned long long)hdr.sourceNodeId, (unsigned)hdr.seqNo);
        break;

    case SCP_MSG_HELLO: {
        solariHello hello;
        rc = solariMsgParseHello(payload, plen, &hello);
        if (rc == SOLARI_OK)
            rc = serverMasterAcceptHello(ctx, &hdr, &hello, cn[0] ? cn : NULL,
                                         tlv, sizeof tlv, &tlvLen, &tlvCount);
        if (rc != SOLARI_OK) {
            serverSendError(ctx, hdr.seqNo, 3, "hello rejected");
            return;
        }
        rc = serverBuildReplyFrame(ctx, SCP_MSG_WELCOME, hdr.seqNo, tlvCount,
                                   tlv, tlvLen, frame, sizeof frame, &frameLen);
        if (rc == SOLARI_OK) (void)solariConnSend(ctx->control, frame, frameLen);
        else serverSendError(ctx, hdr.seqNo, 4, "welcome build failed");
        break;
    }

    case SCP_MSG_WHO_IS_PRIMARY:
        rc = serverLeaseAnswerWhoIsPrimary(ctx, tlv, sizeof tlv,
                                           &tlvLen, &tlvCount);
        if (rc != SOLARI_OK) {
            serverSendError(ctx, hdr.seqNo, 5, "primary lookup failed");
            return;
        }
        rc = serverBuildReplyFrame(ctx, SCP_MSG_PRIMARY_IS, hdr.seqNo, tlvCount,
                                   tlv, tlvLen, frame, sizeof frame, &frameLen);
        if (rc == SOLARI_OK) (void)solariConnSend(ctx->control, frame, frameLen);
        else serverSendError(ctx, hdr.seqNo, 4, "primary build failed");
        break;

    default:
        solariLogf(SOLARI_LOG_WARN,
                   "control: unhandled msgType=0x%02x from src=0x%llx",
                   (unsigned)hdr.msgType, (unsigned long long)hdr.sourceNodeId);
        serverSendError(ctx, hdr.seqNo, 6, "unsupported on control link");
        break;
    }
}

/* ------------------------------------------------------------------- driver */

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [--config PATH] [--init-db] [-h]\n"
        "  --config PATH   load the server .conf (sec 13)\n"
        "  --init-db       retired; use tools/migrate.py --database monitoring\n",
        argv0);
}

/* The accept/dispatch loop, run once the context + db are up. Owns no resources
 * itself; tears nothing down (main does the cleanup). */
static void serverRunLoop(serverContext *ctx, const serverConfig *cfg)
{
    uint64_t nextLeaseMs = 0;   /* 0 -> claim/renew on the very first iteration */
    uint64_t nextDecommissionRetryMs = 0;

    solariLogf(SOLARI_LOG_INFO,
               "solariServer nodeId=0x%llx entering main loop "
               "(lease renew %us / ttl %us)",
               (unsigned long long)ctx->nodeId,
               (unsigned)cfg->leaseRenewSec, (unsigned)cfg->leaseTtlSec);

    while (!g_stop) {
        uint64_t now = solariNowUnixMs();

        /* 1. Failover lease. serverLeaseTick drives serverApplyRole internally,
         *    so a win binds the listeners and a loss/error tears them down. */
        if (now >= nextLeaseMs) {
            serverRoleState role = ctx->role;
            uint64_t        epoch = ctx->epoch;
            (void)serverLeaseTick(ctx, &role, &epoch);
            nextLeaseMs = now + (uint64_t)cfg->leaseRenewSec * 1000u;
        }

        /* 2/3. Only the ACTIVE master has listeners bound and serves the fleet. */
        if (ctx->role == SRV_ACTIVE) {
            serverDrainIngest(ctx);      /* PULL reports + adverts          */
            serverServiceControl(ctx);   /* REP HELLO/WHO_IS_PRIMARY        */
            if (now >= nextDecommissionRetryMs) {
                serverProvisionRetryDecommissions(ctx, now);
                nextDecommissionRetryMs = now + UINT64_C(5000);
            }
        } else {
            solariSleepMs(SERVER_STANDBY_IDLE_MS);
        }

        /* 4. Operator bridge (non-blocking; NULL when the bridge failed to open). */
        if (ctx->ctl) (void)serverCtlPoll(ctx->ctl);

        /* 5. Privileged request-queue consumer (Task #1). Only the ACTIVE master
         * — the CA-key holder — drains privileged actions the dashboard queued
         * but was refused at the socket. Bounded per tick so a backlog cannot
         * starve the fleet path; serverCtlQueuePoll executes at most one row and
         * returns ERR_CONN_RETRY when the queue is empty. */
        if (ctx->ctl && ctx->role == SRV_ACTIVE) {
            int drained = 0;
            while (drained < SERVER_CTL_QUEUE_BATCH &&
                   serverCtlQueuePoll(ctx->ctl) == SOLARI_OK)
                drained++;
        }
    }

    solariLogf(SOLARI_LOG_INFO, "solariServer: shutdown signal received");
}

int main(int argc, char **argv)
{
    const char    *cfgPath = NULL;
    int            initDb = 0, i;
    serverConfig   cfg;
    serverContext  ctx;
    serverDb      *db = NULL;
    solariStatus   rc;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) cfgPath = argv[++i];
        else if (!strcmp(argv[i], "--init-db")) initDb = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]); return 0;
        } else {
            fprintf(stderr, "unknown arg: %s\n", argv[i]);
            usage(argv[0]); return 2;
        }
    }

    if (initDb) {
        fprintf(stderr, "--init-db is retired: db/schema.sql has drifted past "
                "the numbered migrations. Use tools/migrate.py --database "
                "monitoring --db-name NAME after auditing existing schemas.\n");
        return 2;
    }

    solariLogInit(SOLARI_LOG_SINK_STDERR, NULL, "solariServer");

    if (cfgPath) {
        rc = serverConfigFromFile(cfgPath, &cfg);
        if (rc != SOLARI_OK) {
            /* An operator who named a config expects it honoured; silently
             * falling back to defaults could bind the wrong ports or db. Fail. */
            solariLogf(SOLARI_LOG_FATAL, "config load failed (%s): %s",
                       cfgPath, solariStrError(rc));
            solariLogShutdown();
            return 1;
        }
    } else {
        serverConfigDefaults(&cfg);
        solariLogf(SOLARI_LOG_WARN, "no --config; using defaults");
    }

    if (cfg.allowLegacyReports)
        solariLogf(SOLARI_LOG_WARN,
                   "ingest: allowLegacyReports on; session-less one-way reports "
                   "accepted without durable receipts (upgrade agents, then disable)");

    rc = serverContextInit(&ctx, &cfg);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_FATAL, "context init failed: %s", solariStrError(rc));
        solariLogShutdown();
        return 1;
    }

    /* The db is required for the run loop (lease, persistence,
     * convergence all flow through it). Fail closed rather than spin a server
     * that can never win the lease or persist a report. */
    rc = serverDbOpen(&cfg, &db);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_FATAL, "db open failed: %s", solariStrError(rc));
        serverContextClose(&ctx);
        solariLogShutdown();
        return 1;
    }
    ctx.db = db;

    serverInstallSignals();

    /* Operator bridge: non-fatal if it cannot bind - the fleet-facing loop still
     * runs, only dashboard-driven control is unavailable until it comes up. */
    rc = serverCtlOpen(&cfg, &ctx, &ctx.ctl);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_WARN,
                   "operator bridge unavailable: %s (dashboard control disabled)",
                   solariStrError(rc));
        ctx.ctl = NULL;
    }

    serverRunLoop(&ctx, &cfg);

    /* Best-effort: relinquish the active role so a standby can take over fast. */
    (void)serverApplyRole(&ctx, SRV_STANDBY);
    serverContextClose(&ctx);   /* unbinds any listeners, closes the ctl bridge */
    serverDbClose(db);
    solariLogShutdown();
    return 0;
}
