/*
 * monitor.h - solariMonitor internal types (sec 8).
 *
 * A monitor probes configured targets and reports reachability. Targets are
 * assigned across the fleet by rendezvous (HRW) hashing at a replication factor
 * (sec 8.2), so each target is independently probed by its top-k monitors with
 * no coordinator and minimal churn when membership changes.
 */
#ifndef SOLARI_MONITOR_H
#define SOLARI_MONITOR_H

#include "solari/solariCommon.h"
#include "solari/solariMsg.h"
#include "probeNet.h"

#define MONITOR_MAX_TARGETS 64
#define MONITOR_MAX_FLEET   64
#define MONITOR_TARGETID_MAX 320

/* A configured probe target. targetId is the stable HRW key. */
typedef struct {
    char       targetId[MONITOR_TARGETID_MAX];   /* "tcp:host:443"     */
    char       label[64];
    char       host[SOLARI_TARGETHOST_MAX];
    uint16_t   port;
    probeProto proto;
} monitorTarget;

typedef struct {
    char     hostFqdn[SOLARI_FQDN_MAX];   /* override; empty = autodetect */
    uint64_t nodeId;                      /* 0 = derive from fqdn+role     */
    uint64_t configEpoch;                 /* [identity] configEpoch (sec 13) */
    uint32_t roundIntervalSec;            /* default 30 */
    uint16_t probesPerRound;              /* default 5  */
    uint32_t probeTimeoutMs;              /* default 1000 */
    uint8_t  replFactor;                  /* default 2  */
    monitorTarget targets[MONITOR_MAX_TARGETS];
    uint8_t  targetCount;

    /* server + transport (used by the reporting increment) */
    char     primaryUrl[256], failoverUrl[256];
    char     subUrl[256];                 /* server PUB channel; empty = derive
                                           * from primaryUrl at pub port 7703 */
    bool     useTls;
    char     caFile[256], certFile[256], keyFile[256];
    char     spoolDb[256];
    char     ctrlStateFile[256];          /* applied-config state; empty =
                                           * derive "<spoolDb>.ctrl" (or none) */

    /* peer mesh (sec 8.2) */
    char     gossipUrl[256];              /* this monitor's gossip listener (empty = off) */
    char     peerUrls[MONITOR_MAX_FLEET][256];
    uint8_t  peerCount;
    uint32_t gossipIntervalSec;           /* default 20 */
    uint32_t peerTtlSec;                  /* default 90 */
} monitorConfig;

/* Defaults: 30s rounds, 5 probes, 1s timeout, replFactor 2, no targets. */
void monitorConfigDefaults(monitorConfig *out);
/* Load a monitor .conf (sec 13). Absent keys take defaults. */
solariStatus monitorConfigFromFile(const char *path, monitorConfig *out);
/* Parse "proto:host[:port][ : label]" into a target (proto = tcp|udp|icmp). */
solariStatus monitorParseTarget(const char *spec, monitorTarget *out);

/* Live scheduler add path (sec 8, Phase 3 Handoff sec 4.2/7.4): parse `spec`
 * and append it to cfg->targets so the next probe round picks it up. Idempotent
 * (adopting an already-present targetId is a no-op success). ERR_BUFFER_FULL at
 * MONITOR_MAX_TARGETS; ERR_INVALID_ARG on a malformed spec. Both the .conf
 * loader and CTRL_ADOPT_TARGET converge through here. */
solariStatus monitorAddTarget(monitorConfig *cfg, const char *spec);

/* ---- control plane (sec 8, sec 9.1) ---- */

/* Result-payload scratch: verb + errCode + appliedEpoch TLVs. */
#define MONITOR_CTRL_RESULT_CAP 64

/* Control-plane runtime state: the config epoch this monitor has applied.
 * Directives at or below it are acknowledged as converged, never re-applied. */
typedef struct {
    uint64_t appliedEpoch;
} monitorControlState;

/* Outcome of one handled directive, for the I/O glue. */
typedef struct {
    bool           wantReply;   /* a CONTROL_RESULT payload was built      */
    bool           applied;     /* cfg changed: persist blob + epoch       */
    uint8_t        verb;        /* directive verb (0 if unparseable)       */
    const uint8_t *blob;        /* applied blob (aliases inbound payload)  */
    uint16_t       blobLen;
} monitorControlOutcome;

/* Apply a JSON config blob onto cfg (whole-document validated first, so a
 * malformed blob is a no-op error). Keys applied (all optional):
 *   roundIntervalSec, probesPerRound, probeTimeoutMs, replFactor - numbers
 *   targets - array of "proto:host[:port][ : label]" specs (replaces the
 *             probe target set; each entry converges through monitorAddTarget)
 * Pure; no I/O. */
solariStatus monitorControlApplyBlob(monitorConfig *cfg, const char *json, size_t len);

/* Handle an inbound SCP_MSG_CONTROL payload off the broadcast PUB channel:
 * parse tolerantly, drop directives addressed to another node
 * (TLV_CTRL_TARGET_NODE), enforce epoch monotonicity for CTRL_SET_CONFIG /
 * CTRL_PROVISION (apply the JSON blob; targetEpoch <= appliedEpoch is
 * acknowledged as converged without re-applying), and adopt CTRL_ADOPT_TARGET
 * specs via monitorAddTarget. Builds the SCP_MSG_CONTROL_RESULT payload
 * (TLV_CTRL_VERB echo + TLV_ERROR_CODE magnitude + TLV_CTRL_TARGET_EPOCH =
 * applied epoch) into resultBuf. Pure; the caller sends the reply / persists.
 * Returns the directive's own outcome (SOLARI_OK also for a clean drop). */
solariStatus monitorHandleControl(monitorControlState *st, monitorConfig *cfg,
                                  uint64_t selfNodeId,
                                  const uint8_t *payload, size_t payloadLen,
                                  uint8_t *resultBuf, size_t resultCap,
                                  size_t *resultLen, uint16_t *resultTlvCount,
                                  monitorControlOutcome *out);

/* Applied-state persistence ("<epoch>\n<blob>"; file I/O only, no sockets).
 * Load floors appliedEpoch at cfg->configEpoch and re-applies a newer stored
 * blob so a restart keeps pushed config; save is write+rename. Both are
 * no-ops (OK) when no state path resolves (no ctrlStateFile and no spoolDb). */
void         monitorControlStatePath(const monitorConfig *cfg, char *out, size_t cap);
solariStatus monitorControlStateLoad(monitorControlState *st, monitorConfig *cfg);
solariStatus monitorControlStateSave(const monitorConfig *cfg,
                                     const monitorControlState *st,
                                     const uint8_t *blob, uint16_t blobLen);

/* HRW ownership (sec 8.2): true if `self` is among the top-k monitors for
 * `targetId` over `fleet`. Deterministic; removing a non-owner never changes a
 * target's owner set (low churn). */
bool monitorOwnsTarget(uint64_t self, const uint64_t *fleet, size_t fleetLen,
                       const char *targetId, uint8_t k);

/* Stable node id: configured id, or FNV-1a-64(fqdn|role) (sec 5.5). */
uint64_t monitorNodeId(const monitorConfig *cfg);

/* Fully-qualified hostname (Linux; gethostname + canonical lookup). */
solariStatus monitorHostFqdn(char *out, size_t cap);

/* ---- peer registry & schedule (sec 8.2) ---- */

typedef struct { uint64_t nodeId; uint64_t lastHeardMs; } monitorPeerEntry;

/* Live-membership view this node maintains from gossip. The fleet for HRW is
 * self plus every peer heard within the TTL. */
typedef struct {
    uint64_t         selfNodeId;
    monitorPeerEntry peers[MONITOR_MAX_FLEET];
    size_t           count;
} monitorPeers;

void monitorPeersInit(monitorPeers *p, uint64_t selfNodeId);
/* Record/refresh a peer's last-heard time (ignores self). */
void monitorPeersHeard(monitorPeers *p, uint64_t nodeId, uint64_t nowMs);
/* Drop peers not heard within ttlSec. */
void monitorPeersPrune(monitorPeers *p, uint64_t nowMs, uint32_t ttlSec);
/* Build the live fleet (self + peers heard within ttlSec) into fleetOut (sorted
 * is not required). Returns the number written (<= cap). */
size_t monitorPeersFleet(const monitorPeers *p, uint64_t nowMs, uint32_t ttlSec,
                         uint64_t *fleetOut, size_t cap);

/* Indices into cfg->targets that `self` owns over `fleet` at cfg->replFactor.
 * Returns the count written into ownedIdx (<= cap). */
size_t monitorOwnedTargets(const monitorConfig *cfg, uint64_t self,
                           const uint64_t *fleet, size_t fleetLen,
                           uint8_t *ownedIdx, size_t cap);

/* ---- gossip transport (sec 8.2) ---- */
#ifdef MONITOR_WITH_REPORTING
typedef struct monitorGossip monitorGossip;

/* Open the gossip endpoint: a PULL listener on cfg->gossipUrl and PUSH dialers
 * to each cfg->peerUrls[]. NULL handle (SOLARI_OK) if no gossipUrl configured. */
solariStatus monitorGossipOpen(const monitorConfig *cfg, uint64_t selfNodeId,
                               monitorGossip **out);
void         monitorGossipClose(monitorGossip *g);
/* One gossip cycle: drain inbound PEER_ALIVE frames (refreshing `reg`), then
 * announce self to every peer. Best-effort; never blocks long. */
solariStatus monitorGossipTick(monitorGossip *g, monitorPeers *reg, uint64_t nowMs);
#endif

/* ===================== reporting (transport + spool) ===================== */
#ifdef MONITOR_WITH_REPORTING

#include "solari/solariReporter.h"

/* Reporting context: the shared push-or-spool transport (solariReporter) plus
 * this monitor's node id. Same fault tolerance as the client - MONITOR_REPORTs
 * are pushed live or durably spooled and replayed on reconnect. */
typedef struct {
    const monitorConfig *cfg;
    solariReporter      *rep;
    uint64_t             nodeId;
} monitorContext;

/* ---- control transport glue (SUB dial + poll + reply) ---- */

#include "solari/solariNet.h"

typedef struct {
    solariConn *sub;                  /* SUB dialer to the server PUB channel */
    char        subUrl[256];
} monitorControlIo;

/* Dial the server PUB channel (cfg->subUrl, or primaryUrl re-pointed at the
 * standard pub port 7703) as a subscriber. Best-effort: on failure io->sub is
 * NULL and polling degrades to plain sleeping. */
solariStatus monitorControlOpen(const monitorConfig *cfg, monitorControlIo *io);
void         monitorControlClose(monitorControlIo *io);

/* Service the control channel for ~waitMs (doubles as the round-loop sleep):
 * receive published frames, dispatch SCP_MSG_CONTROL to monitorHandleControl,
 * persist applied state, and push the CONTROL_RESULT (correlationId = the
 * directive frame's seqNo) via ctx's reporter to the server ingest channel.
 * Never blocks materially past waitMs (worst case one ~250ms recv slice). */
void monitorControlPoll(monitorControlIo *io, monitorContext *ctx,
                        monitorConfig *cfg, monitorControlState *st,
                        uint32_t waitMs);

/* Derive nodeId, open the reporter (and spool, if configured). Does not connect. */
solariStatus monitorContextInit(monitorContext *ctx, const monitorConfig *cfg);
void         monitorContextClose(monitorContext *ctx);
/* Best-effort connect (primary, then failover). */
solariStatus monitorConnect(monitorContext *ctx);
/* Build a MONITOR_REPORT from probe results and push-or-spool it. */
solariStatus monitorReportSend(monitorContext *ctx, const solariMonitorReport *rep);
/* Replay any spooled reports. */
solariStatus monitorDrainSpool(monitorContext *ctx);

#endif /* MONITOR_WITH_REPORTING */

#endif /* SOLARI_MONITOR_H */
