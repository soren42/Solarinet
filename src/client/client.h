/*
 * client.h - solariClient internal types (sec 7).
 *
 * The client core is platform-agnostic: it speaks only to the PAL (platOS.h)
 * and libsolari. Configuration is parsed from the .conf (sec 13) into a
 * clientConfig; per-cycle mutable state (log tail offsets) lives separately in
 * clientState so the config stays read-only during collection.
 *
 * The reporting layer (clientContext + send/spool, guarded by
 * CLIENT_WITH_REPORTING) is compiled only when libsolari has the nng transport
 * and the SQLite spool (SOLARI_WITH_IO && SOLARI_WITH_SQLITE).
 */
#ifndef SOLARI_CLIENT_H
#define SOLARI_CLIENT_H

#include "solari/solariCommon.h"
#include "solari/solariMsg.h"

#define CLIENT_URL_MAX   256
#define CLIENT_PATH_MAX  256

/* A watched log file: path + optional POSIX ERE (empty = count every line). */
typedef struct {
    char path[SOLARI_LOGPATH_MAX];
    char regex[128];
} clientLogWatch;

/* Static client configuration, derived from the .conf (sec 13). */
typedef struct {
    char     hostFqdn[SOLARI_FQDN_MAX];   /* override; empty = autodetect */
    uint64_t nodeId;                      /* 0 = derive from fqdn+role     */
    uint64_t configEpoch;
    char     agentVersion[SOLARI_AGENTVER_MAX];
    uint32_t sampleIntervalSec;           /* default 15 */
    uint32_t watchdogIntervalSec;         /* default 5  */

    /* server + transport */
    char     primaryUrl[CLIENT_URL_MAX];  /* empty = local-only (no reporting) */
    char     failoverUrl[CLIENT_URL_MAX];
    bool     useTls;
    char     caFile[CLIENT_PATH_MAX];
    char     certFile[CLIENT_PATH_MAX];
    char     keyFile[CLIENT_PATH_MAX];
    char     spoolDb[CLIENT_PATH_MAX];    /* empty = no store-and-forward */

    /* watch lists */
    char     procs[SOLARI_MAX_PROCS][SOLARI_PROCNAME_MAX];
    uint8_t  procCount;
    clientLogWatch logs[SOLARI_MAX_LOGS];
    uint8_t  logCount;
} clientConfig;

/* Mutable per-cycle runtime state: where each watched log was last read. */
typedef struct {
    uint64_t logOffset[SOLARI_MAX_LOGS];
} clientState;

/* Fill a clientConfig from a parsed .conf file. Absent keys take sensible
 * defaults; identity.hostFqdn empty means "autodetect at collection time". */
solariStatus clientConfigFromFile(const char *path, clientConfig *out);

/* Fill a clientConfig with autodetect defaults and no watch targets. */
void clientConfigDefaults(clientConfig *out);

/* Gather one report via the PAL. catalogueOnly=true fills only slow-changing
 * identity (for HELLO); false adds fast samples, watched procs, and log deltas.
 * Updates st->logOffset[]. */
solariStatus clientCollectReport(const clientConfig *cfg, clientState *st,
                                 bool catalogueOnly, solariClientReport *out);

/* ===================== reporting (transport + spool) ===================== */
#ifdef CLIENT_WITH_REPORTING

#include "solari/solariNet.h"
#include "solari/solariSpool.h"

/* Runtime reporting context: the live connection to the active server, the
 * durable spool, and the per-source sequence counter. The push-or-spool path
 * (clientReportSend) is what makes the client fault tolerant: when the server
 * is unreachable the frame is durably queued and drained on reconnect. */
typedef struct {
    const clientConfig *cfg;
    solariConn  *conn;        /* PUSH dialer to the active server; may be NULL */
    solariSpool *spool;       /* store-and-forward; NULL if no spoolDb         */
    uint64_t     nodeId;      /* stable id for this node (sec 5.5)             */
    uint32_t     seqNo;       /* per-source monotonic frame sequence           */
    int          activeUrl;   /* 0 = primary, 1 = failover                     */
} clientContext;

/* Derive nodeId, open the spool (if configured). Does not connect. */
solariStatus clientContextInit(clientContext *ctx, const clientConfig *cfg);
/* Close the connection and spool (NULL-safe). */
void         clientContextClose(clientContext *ctx);

/* Best-effort connect to the active server (primary, then failover). Returns
 * SOLARI_OK if a dialer was created (nng connects asynchronously), or
 * ERR_CONN_RETRY if no server URL is usable. Safe to call repeatedly. */
solariStatus clientConnect(clientContext *ctx);

/* Announce this node to the server (SCP_MSG_HELLO over the push channel).
 * Best-effort; the WELCOME reply is handled once the server exists (Phase 5). */
solariStatus clientSendHello(clientContext *ctx);

/* Build a CLIENT_REPORT frame and send it; on any transient/offline failure,
 * durably spool it instead. On a successful live send, opportunistically drains
 * the spool. Returns SOLARI_OK when the report was either sent or spooled. */
solariStatus clientReportSend(clientContext *ctx, const solariClientReport *rep);

/* Replay queued frames to the server in order: peek -> send -> ack, or nack and
 * stop on failure (retried next cycle). Bounded per call. */
solariStatus clientDrainSpool(clientContext *ctx);

#endif /* CLIENT_WITH_REPORTING */

#endif /* SOLARI_CLIENT_H */
