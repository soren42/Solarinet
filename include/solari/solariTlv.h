/*
 * solariTlv.h - Type-Length-Value payload codec (§5.6, §6.3).
 *
 * SCP payloads are sequences of TLV triples:
 *
 *   +----------+----------+------------------+
 *   | uint16   | uint16   | value            |
 *   | tlvType  | tlvLen   | (tlvLen bytes)   |
 *   +----------+----------+------------------+
 *
 * All integers big-endian. Unknown TLV types are skipped by readers, never
 * fatal - this is what lets the protocol gain fields without a version bump.
 * Type ranges (high byte = category):
 *   0x01xx identity/session   0x10xx host metrics   0x11xx probe results
 *   0x30xx control directives 0xFFxx error/diagnostic
 */
#ifndef SOLARI_TLV_H
#define SOLARI_TLV_H

#include "solari/solariCommon.h"

enum solariTlvType {
    /* --- identity / session (0x01xx) --- */
    TLV_ROLE              = 0x0101,  /* uint8: 1=client 2=monitor 3=server */
    TLV_AGENT_VERSION     = 0x0102,  /* utf8 string                        */
    TLV_HOST_FQDN         = 0x0103,  /* utf8 string                        */
    TLV_CONFIG_EPOCH      = 0x0104,  /* uint64                             */
    /* --- host metrics (0x10xx) --- */
    TLV_OS_NAME           = 0x1001,  /* utf8                               */
    TLV_ARCH              = 0x1002,  /* utf8                               */
    TLV_CPU_LOAD_MILLI    = 0x1003,  /* repeated uint32: load%*1000 per core */
    TLV_RAM_USED_KB       = 0x1004,  /* uint64                             */
    TLV_RAM_TOTAL_KB      = 0x1005,  /* uint64                             */
    TLV_SWAP_USED_KB      = 0x1006,  /* uint64                             */
    TLV_SWAP_TOTAL_KB     = 0x1007,  /* uint64                             */
    TLV_DISK_FREE_KB      = 0x1008,  /* repeated: {mountStr, uint64 free}  */
    TLV_NET_IFACE         = 0x1009,  /* repeated: iface throughput vs cap  */
    TLV_USB_THROUGHPUT    = 0x100A,  /* repeated: bus throughput vs cap    */
    TLV_PROC_WATCH        = 0x100B,  /* repeated proc entry                */
    TLV_LOGFILE_STAT      = 0x100C,  /* repeated: {path, sizeDelta, matchCt} */
    TLV_NET_ADDR          = 0x100D,  /* repeated: interface IP/MAC         */
    /* --- probe results (0x11xx) --- */
    TLV_PROBE_RESULT      = 0x1101,  /* repeated solariProbeResult (§5.7)  */
    TLV_LINK_STAT         = 0x1102,  /* repeated link stat per peer        */
    TLV_SERVICE_META      = 0x1103,  /* repeated: version/vendor/config kv */
    /* --- discovery (0x12xx, Phase 3 Handoff sec 4.3) --- */
    TLV_DISC_SOURCE       = 0x1203,  /* uint8: 1=mDNS 2=ARP 3=advert 4=portscan 5=LLDP */
    TLV_DISC_ENTITY       = 0x1201,  /* repeated utf8 "ip|mac|iface"        */
    TLV_DISC_SERVICE      = 0x1202,  /* repeated utf8 "proto|port|banner"   */
    /* --- topology / LLDP (0x13xx) --- */
    TLV_TOPO_UPLINK       = 0x1301,  /* utf8 "localIf|gatewayIp|speedMbps"   */
    TLV_TOPO_LLDP_NEIGH   = 0x1302,  /* repeated utf8 "chassis|port|localIf" */
    TLV_TOPO_SEGMENT      = 0x1303,  /* repeated utf8 "ifName|cidr"          */
    /* --- control (0x30xx) --- */
    TLV_CTRL_VERB         = 0x3001,  /* uint8: setConfig|deploy|restart|.. */
    TLV_CTRL_PAYLOAD      = 0x3002,  /* opaque blob                        */
    TLV_CTRL_TARGET_EPOCH = 0x3003,  /* uint64 config epoch to converge to */
    TLV_CTRL_TARGET_NODE  = 0x3004,  /* uint64 addressee nodeId; absent/0 = broadcast */
    /* --- lifecycle (0x32xx) --- */
    TLV_LIFE_CONFIRM      = 0x3201,  /* uint64: echo token the server must match to */
                                     /* authorize a destructive decommission        */
    TLV_LIFE_WIPE_SCOPE   = 0x3202,  /* uint8 bitfield: config|certs|spool|logs|unit */
    /* --- error (0xFFxx) --- */
    TLV_ERROR_CODE        = 0xFF01,  /* uint16 (Appendix A)                */
    TLV_ERROR_DETAIL      = 0xFF02   /* utf8                               */
};

/* ---- TLV_LIFE_WIPE_SCOPE bits (§4) - which local state a decommission erases ---- */
enum {
    SOLARI_WIPE_CONFIG = 0x01,   /* local config files                 */
    SOLARI_WIPE_CERTS  = 0x02,   /* certs & private keys               */
    SOLARI_WIPE_SPOOL  = 0x04,   /* store-and-forward spool DB         */
    SOLARI_WIPE_LOGS   = 0x08,   /* local logs (omit to keep forensics)*/
    SOLARI_WIPE_UNIT   = 0x10    /* disable & remove the service unit  */
};

/* ---- writer: a cursor over a caller-owned buffer ---- */
typedef struct {
    uint8_t  *buf;
    size_t    cap;
    size_t    len;
    uint16_t  count;
} solariTlvWriter;

/* Initialize a writer over [buf, buf+cap). Resets len and count to zero. */
void solariTlvWriterInit(solariTlvWriter *w, uint8_t *buf, size_t cap);

/* Append a raw TLV (type, len bytes from val). val may be NULL iff len == 0.
 * Returns ERR_BUFFER_FULL if it would overflow cap. Increments count. */
solariStatus solariTlvAppend(solariTlvWriter *w, uint16_t type,
                             const void *val, uint16_t len);

/* Typed appends - value stored big-endian. */
solariStatus solariTlvAppendU8 (solariTlvWriter *w, uint16_t type, uint8_t  v);
solariStatus solariTlvAppendU16(solariTlvWriter *w, uint16_t type, uint16_t v);
solariStatus solariTlvAppendU32(solariTlvWriter *w, uint16_t type, uint32_t v);
solariStatus solariTlvAppendU64(solariTlvWriter *w, uint16_t type, uint64_t v);
/* Appends the string WITHOUT its terminating NUL; tlvLen is strlen(s). */
solariStatus solariTlvAppendStr(solariTlvWriter *w, uint16_t type, const char *s);

/* ---- reader: a cursor over an existing payload (zero-copy) ---- */
typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         pos;
} solariTlvReader;

void solariTlvReaderInit(solariTlvReader *r, const uint8_t *buf, size_t len);

/* Read the next element. Sets *type, *len, and points *val into the source
 * buffer (do not free). Returns SOLARI_OK, ERR_TLV_END when exhausted, or
 * ERR_TLV_TRUNCATED if a declared length runs past the buffer. */
solariStatus solariTlvNext(solariTlvReader *r, uint16_t *type,
                           const uint8_t **val, uint16_t *len);

/* Convenience typed reads that also validate the length is exactly right. */
solariStatus solariTlvReadU8 (const uint8_t *val, uint16_t len, uint8_t  *out);
solariStatus solariTlvReadU16(const uint8_t *val, uint16_t len, uint16_t *out);
solariStatus solariTlvReadU32(const uint8_t *val, uint16_t len, uint32_t *out);
solariStatus solariTlvReadU64(const uint8_t *val, uint16_t len, uint64_t *out);

#endif /* SOLARI_TLV_H */
