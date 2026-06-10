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
    uint32_t roundIntervalSec;            /* default 30 */
    uint16_t probesPerRound;              /* default 5  */
    uint32_t probeTimeoutMs;              /* default 1000 */
    uint8_t  replFactor;                  /* default 2  */
    monitorTarget targets[MONITOR_MAX_TARGETS];
    uint8_t  targetCount;

    /* server + transport (used by the reporting increment) */
    char     primaryUrl[256], failoverUrl[256];
    bool     useTls;
    char     caFile[256], certFile[256], keyFile[256];
    char     spoolDb[256];

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
