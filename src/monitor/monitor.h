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

#endif /* SOLARI_MONITOR_H */
