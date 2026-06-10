/*
 * probeNet.h - the monitor's measurement surface (sec 8.3).
 *
 * Hand-rolled BSD sockets (the monitor is Linux-only, sec 12.2) for the direct
 * socket control nng can't give: TCP connect timing, UDP reachability, and ICMP
 * echo with RTT/jitter/loss. Each probeRun does `count` attempts so loss and
 * jitter are meaningful. RTT uses the monotonic clock, never the wall clock.
 */
#ifndef PROBE_NET_H
#define PROBE_NET_H

#include "solari/solariCommon.h"
#include "solari/solariMsg.h"   /* solariProbeResult, solariLinkStat */

typedef enum { PROBE_ICMP = 1, PROBE_TCP = 2, PROBE_UDP = 3 } probeProto;

typedef enum {
    PROBE_OK = 0,
    PROBE_TIMEOUT,
    PROBE_REFUSED,
    PROBE_UNREACHABLE,
    PROBE_DNS_FAIL,
    PROBE_TLS_FAIL,
    PROBE_PROTO_ERR
} probeOutcome;

typedef struct {
    char        targetHost[SOLARI_TARGETHOST_MAX];
    uint16_t    dstPort;             /* 0 for ICMP                          */
    probeProto  proto;
    uint16_t    count;               /* probes per round (>=1); for loss/jitter */
    uint32_t    timeoutMs;           /* per-attempt timeout                 */
    const char *expectBanner;        /* optional TCP banner substring match */
} probeSpec;

/* Run one probe round per spec, filling *out (sec 5.7): outcome, avg RTT,
 * jitter, loss permille, and the resolved dstAddr. Returns SOLARI_OK whenever a
 * result was produced (the verdict is in out->outcome); ERR_INVALID_ARG on bad
 * args. DNS failure is reported as out->outcome = PROBE_DNS_FAIL, not an error. */
solariStatus probeRun(const probeSpec *spec, solariProbeResult *out);

/* Link characteristics to a peer monitor: transit time, throughput vs capacity.
 * (Stub baseline in this increment; populated as the peer mesh lands.) */
solariStatus probeLinkToPeer(uint64_t peerNodeId, const char *peerUrl,
                             solariLinkStat *out);

#endif /* PROBE_NET_H */
