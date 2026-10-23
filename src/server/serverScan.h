/*
 * serverScan.h - active network discovery scan (Handoff §7.1).
 *
 * A server-initiated sweep of an intranet range that populates the `discovered`
 * table the dashboard surfaces for adoption. The core mechanism is a native
 * standard-C TCP connect-scan (no external dependencies). When built with
 * SOLARI_WITH_DISCOVERY_TOOLS, results are additionally enriched, best-effort,
 * from the host's zeroconf (avahi) and SNMP utilities; LLDP enrichment is a
 * documented follow-up (needs lldpd). This is the operator-driven counterpart to
 * the passive DISCOVERY_ADVERT path (serverDiscovery.c).
 */
#ifndef SOLARI_SERVER_SCAN_H
#define SOLARI_SERVER_SCAN_H

#include "server.h"

typedef solariStatus (*serverScanProgressFn)(void *userData,
                                            uint32_t hostsDone,
                                            uint32_t hostsFound);

/* Validate scan bounds without opening sockets. */
solariStatus serverScanPlan(const char *cidr, const char *portsCsv,
                            uint32_t *hostsTotalOut);

/* Run a discovery scan over `cidr` (e.g. "192.168.1.0/24") probing the TCP
 * ports in `portsCsv` (e.g. "22,80,443"; NULL/"" uses a built-in common set).
 * Each host with at least one open port (or a reachable enrichment hit) is
 * upserted into `discovered` via serverDb. *foundOut (optional) receives the
 * number of hosts recorded. Bounds the host count and per-connect timeout so a
 * scan cannot run unbounded. Returns ERR_INVALID_ARG on a bad CIDR. */
solariStatus serverScanRun(serverContext *ctx, const char *cidr,
                           const char *portsCsv, size_t *foundOut);

/* Variant used by the durable scan worker. Calls progress between bounded
 * batches and result commits; a non-OK callback cancels the scan. */
solariStatus serverScanRunProgress(serverContext *ctx, const char *cidr,
                                   const char *portsCsv, size_t *foundOut,
                                   serverScanProgressFn progress,
                                   void *userData);

#endif /* SOLARI_SERVER_SCAN_H */
