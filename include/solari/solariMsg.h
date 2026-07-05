/*
 * solariMsg.h - typed message structs and build/parse pairs (§6.4).
 *
 * This layer sits above the raw TLV codec so components never touch TLV codes
 * directly: each message type has an in-memory struct plus a builder (struct ->
 * TLV payload) and a parser (TLV payload -> struct). Builders write into a
 * caller-owned payload buffer and report how many TLV elements they wrote (for
 * the frame header's tlvCount); parsers tolerate unknown/extra TLVs.
 *
 * Repeated structured fields (disks, ifaces, procs, probe results) are encoded
 * as one TLV per element of the relevant type, each with a fixed internal byte
 * layout documented beside its struct. All integers big-endian.
 */
#ifndef SOLARI_MSG_H
#define SOLARI_MSG_H

#include "solari/solariCommon.h"

/* ---- field size bounds ---- */
#define SOLARI_FQDN_MAX        256
#define SOLARI_OSNAME_MAX      64
#define SOLARI_ARCH_MAX        16
#define SOLARI_MOUNT_MAX       64
#define SOLARI_IFNAME_MAX      32
#define SOLARI_USBNAME_MAX     32
#define SOLARI_PROCNAME_MAX    128
#define SOLARI_LOGPATH_MAX     256
#define SOLARI_TARGETHOST_MAX  256
#define SOLARI_AGENTVER_MAX    32
#define SOLARI_ERRDETAIL_MAX   256

/* ===================== shared sub-records ===================== */

typedef struct {                 /* TLV_DISK_FREE_KB: [u64 free][u64 total][mount] */
    char     mount[SOLARI_MOUNT_MAX];
    uint64_t freeKb;
    uint64_t totalKb;
} solariDiskEntry;

typedef struct {                 /* TLV_NET_IFACE: [u64 rx][u64 tx][u64 cap][name] */
    char     name[SOLARI_IFNAME_MAX];
    uint64_t rxKbps;
    uint64_t txKbps;
    uint64_t capacityKbps;
} solariIfaceEntry;

typedef struct {                 /* TLV_USB_THROUGHPUT: [u64 thru][u64 cap][bus] */
    char     bus[SOLARI_USBNAME_MAX];
    uint64_t throughputKbps;
    uint64_t capacityKbps;
} solariUsbEntry;

typedef struct {                 /* TLV_PROC_WATCH: [i32 pid][u8 state][u32 nFiles][u32 nSockets][u64 rss][name] */
    char     name[SOLARI_PROCNAME_MAX];
    int32_t  pid;
    uint8_t  state;              /* single-char run state ('R','S','Z',...)    */
    uint32_t nFiles;
    uint32_t nSockets;
    uint64_t rssKb;
} solariProcEntry;

typedef struct {                 /* TLV_LOGFILE_STAT: [u64 sizeDelta][u32 matchCt][u64 lastOffset][path] */
    char     path[SOLARI_LOGPATH_MAX];
    uint64_t sizeDelta;
    uint32_t matchCount;
    uint64_t lastOffset;
} solariLogEntry;

/* ---- probe result (§5.7), fixed 42-byte TLV value ---- */
typedef struct {
    uint8_t  proto;          /* 1=ICMP 2=TCP 3=UDP                      */
    uint8_t  outcome;        /* probeOutcome: ok/timeout/refused/...    */
    uint16_t dstPort;        /* 0 for ICMP                              */
    uint8_t  dstAddr[16];    /* IPv4-mapped or IPv6                     */
    uint32_t rttMicros;
    uint32_t jitterMicros;
    uint16_t lossPermille;
    uint32_t throughputKbps;
    uint64_t probeTimeUnixMs;
} solariProbeResult;

#define SOLARI_PROBE_RESULT_WIRE_LEN 42

/* ---- link stat to a peer monitor, fixed 22-byte TLV value ---- */
typedef struct {                 /* TLV_LINK_STAT */
    uint64_t peerNodeId;
    uint32_t transitMicros;
    uint32_t throughputKbps;
    uint32_t capacityKbps;
    uint16_t overheadPermille;
} solariLinkStat;

#define SOLARI_LINK_STAT_WIRE_LEN 22

/* ===================== CLIENT_REPORT (§6.4) ===================== */

typedef struct {
    char     hostFqdn[SOLARI_FQDN_MAX];
    char     osName[SOLARI_OSNAME_MAX];
    char     arch[SOLARI_ARCH_MAX];
    uint32_t cpuLoadMilli[SOLARI_MAX_CORES];
    uint8_t  coreCount;
    uint64_t ramUsedKb, ramTotalKb, swapUsedKb, swapTotalKb;
    solariDiskEntry  disks[SOLARI_MAX_DISKS];   uint8_t diskCount;
    solariIfaceEntry ifaces[SOLARI_MAX_IFACES]; uint8_t ifaceCount;
    solariUsbEntry   usb[SOLARI_MAX_USB];       uint8_t usbCount;
    solariProcEntry  procs[SOLARI_MAX_PROCS];   uint8_t procCount;
    solariLogEntry   logs[SOLARI_MAX_LOGS];     uint8_t logCount;
} solariClientReport;

/* Serialize report into payload (TLV). *outLen = bytes written, *tlvCount =
 * number of TLV elements (for the frame header). ERR_BUFFER_FULL if too small. */
solariStatus solariMsgBuildClientReport(const solariClientReport *rep,
                                        uint8_t *payload, size_t cap,
                                        size_t *outLen, uint16_t *tlvCount);
/* Parse a payload back into a report (server side). Zeroes rep first; unknown
 * TLVs are skipped; entries beyond the struct caps are dropped (logged). */
solariStatus solariMsgParseClientReport(const uint8_t *payload, size_t len,
                                        solariClientReport *rep);

/* ===================== MONITOR_REPORT ===================== */

typedef struct {
    char     hostFqdn[SOLARI_FQDN_MAX];
    solariProbeResult probes[SOLARI_MAX_PROBES]; uint16_t probeCount;
    solariLinkStat    links[SOLARI_MAX_IFACES];  uint8_t  linkCount;
} solariMonitorReport;

solariStatus solariMsgBuildMonitorReport(const solariMonitorReport *rep,
                                         uint8_t *payload, size_t cap,
                                         size_t *outLen, uint16_t *tlvCount);
solariStatus solariMsgParseMonitorReport(const uint8_t *payload, size_t len,
                                         solariMonitorReport *rep);

/* ===================== HELLO ===================== */

typedef struct {
    solariRole role;
    char       agentVersion[SOLARI_AGENTVER_MAX];
    char       hostFqdn[SOLARI_FQDN_MAX];
    uint64_t   configEpoch;
} solariHello;

solariStatus solariMsgBuildHello(const solariHello *h,
                                 uint8_t *payload, size_t cap,
                                 size_t *outLen, uint16_t *tlvCount);
solariStatus solariMsgParseHello(const uint8_t *payload, size_t len,
                                 solariHello *h);

/* ===================== CONTROL ===================== */

typedef enum {
    CTRL_SET_CONFIG   = 1,
    CTRL_DEPLOY       = 2,
    CTRL_RESTART      = 3,
    CTRL_PROBE_NOW    = 4,
    CTRL_SET_SCHEDULE = 5,
    CTRL_DRAIN_SPOOL  = 6,
    CTRL_PROVISION    = 7,    /* first-time bring-up; re-provision converges to epoch */
    CTRL_DECOMMISSION = 8,    /* carried by SCP_MSG_DECOMMISSION: teardown + secure erase */
    CTRL_ADOPT_TARGET = 9     /* monitor: promote a discovered entity to a live target */
} solariCtrlVerb;

typedef struct {
    uint8_t        verb;          /* solariCtrlVerb                      */
    uint64_t       targetEpoch;   /* config epoch to converge to         */
    uint64_t       targetNode;    /* addressee nodeId; 0 = broadcast (the
                                   * PUB channel reaches every subscriber,
                                   * so directives carry their addressee) */
    const uint8_t *payload;       /* opaque blob (aliases caller / source)*/
    uint16_t       payloadLen;
} solariControl;

solariStatus solariMsgBuildControl(const solariControl *c,
                                   uint8_t *payload, size_t cap,
                                   size_t *outLen, uint16_t *tlvCount);
/* Parser points c->payload into the source buffer (zero-copy). */
solariStatus solariMsgParseControl(const uint8_t *payload, size_t len,
                                   solariControl *c);

/* ===================== ERROR ===================== */

typedef struct {
    uint16_t code;                       /* maps to Appendix A magnitude */
    char     detail[SOLARI_ERRDETAIL_MAX];
} solariErrorMsg;

solariStatus solariMsgBuildError(const solariErrorMsg *e,
                                 uint8_t *payload, size_t cap,
                                 size_t *outLen, uint16_t *tlvCount);
solariStatus solariMsgParseError(const uint8_t *payload, size_t len,
                                 solariErrorMsg *e);

#endif /* SOLARI_MSG_H */
