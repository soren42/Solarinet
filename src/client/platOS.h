/*
 * platOS.h - platform abstraction layer (PAL) for solariClient (sec 7.2).
 *
 * Exactly one plat/plat<OS>.c is compiled, chosen by CMake from the target.
 * The client core includes ONLY this header and nothing platform-specific;
 * this is the seam that keeps the core ANSI-clean. Adding an OS means adding
 * one plat/<os>.c that satisfies these signatures - nothing else changes.
 *
 * Convention (sec 17): every function returns solariStatus and writes its
 * out-params only on success unless documented otherwise. Buffers are
 * caller-owned; the PAL never allocates on the caller's behalf.
 */
#ifndef PLAT_OS_H
#define PLAT_OS_H

#include "solari/solariCommon.h"
#include "solari/solariMsg.h"   /* solariDiskEntry / solariIfaceEntry / ... */

/* ---- identity & catalogue (slow-changing) ---- */

/* Fully-qualified hostname (falls back to the short name if no domain). */
solariStatus platHostFqdn (char *out, size_t cap);
/* OS name + release, e.g. "Linux 6.8.0". */
solariStatus platOsName   (char *out, size_t cap);
/* Normalized arch token, e.g. "x86_64", "aarch64", "armv7l". */
solariStatus platArch     (char *out, size_t cap);
/* Count of online logical CPUs. */
solariStatus platCpuCount (uint8_t *out);

/* ---- fast samples ---- */

/* Per-core busy load as permille (0..1000). Writes up to `cap` entries and
 * sets *coreCount to how many were written. May block briefly to measure a
 * delta (Linux: two /proc/stat reads ~100ms apart). */
solariStatus platCpuLoad  (uint32_t *loadMilli, uint8_t cap, uint8_t *coreCount);
/* RAM/swap in KiB. used = total - available. */
solariStatus platMemInfo  (uint64_t *ramUsedKb, uint64_t *ramTotalKb,
                           uint64_t *swapUsedKb, uint64_t *swapTotalKb);
/* Free/total per real mounted filesystem (pseudo-fs skipped). */
solariStatus platDiskFree (solariDiskEntry *disks, uint8_t cap, uint8_t *count);
/* Per-interface instantaneous throughput vs link capacity (loopback skipped). */
solariStatus platNetIfaces(solariIfaceEntry *ifaces, uint8_t cap, uint8_t *count);
/* Per-USB-bus throughput vs capacity. The Linux reference reports none yet
 * (sets *count=0); kept in the contract for parity with the report struct. */
solariStatus platUsbThroughput(solariUsbEntry *bus, uint8_t cap, uint8_t *count);

/* ---- process & log watch ---- */

/* Inspect a watched process by name -> pid/state/open-files/sockets/RSS.
 * On success with the process RUNNING: out is fully populated.
 * On success with the process NOT running: out->name is set, out->pid = -1,
 * out->state = 0, counts = 0 (a "watched but down" record, not an error). */
solariStatus platProcInspect(const char *procName, solariProcEntry *out);
/* Stat a log file: current size, and the count of newly-appended lines (since
 * *lastOffsetInOut) matching `regex` (POSIX ERE; NULL counts every new line).
 * Updates *lastOffsetInOut to the new size; resets to 0 if the file shrank
 * (rotation). out->* written on success. */
solariStatus platLogStat   (const char *path, const char *regex,
                            uint64_t *sizeNow, uint32_t *matchCount,
                            uint64_t *lastOffsetInOut);

/* ---- discovery & topology (Phase 3 Handoff sec 4) ---- */

/* A neighbor the node can cheaply see (ARP / neighbor table). */
typedef struct {
    char ip[46];                     /* dotted IPv4 (or IPv6) */
    char mac[18];                    /* "aa:bb:cc:dd:ee:ff"   */
    char iface[SOLARI_IFNAME_MAX];
} platNeighbor;

/* An interface's network binding (for segment inference). */
typedef struct {
    char     ifName[SOLARI_IFNAME_MAX];
    char     cidr[64];               /* "10.42.0.0/24" (network address) */
    uint64_t speedMbps;
} platIfaceCidr;

/* The node's uplink: the default-route interface, its gateway, link speed.
 * LLDP chassis/port are populated by a fuller path later; this is the cheap
 * always-available hint (default route) the topology view starts from. */
typedef struct {
    char     localIf[SOLARI_IFNAME_MAX];
    char     gatewayIp[46];
    uint64_t speedMbps;
} platUplink;

/* Neighbors from the ARP/neighbor table (complete entries only). */
solariStatus platArpNeighbors(platNeighbor *out, uint8_t cap, uint8_t *count);
/* Per-interface IPv4 network CIDRs (loopback skipped). */
solariStatus platIfaceCidrs(platIfaceCidr *out, uint8_t cap, uint8_t *count);
/* The default-route uplink. ERR_PLATFORM if there is no default route. */
solariStatus platDefaultUplink(platUplink *out);

/* ---- watchdog support ---- */

/* Fork+exec a fresh copy of the agent (argv must be NULL-terminated). Returns
 * in the parent; the child replaces itself with the new image. */
solariStatus platSpawnSelf  (const char *argv0, int argc, char *const argv[]);
/* Liveness check for a pid (kill(pid,0) semantics). */
solariStatus platProcessAlive(int64_t pid, bool *alive);

#endif /* PLAT_OS_H */
