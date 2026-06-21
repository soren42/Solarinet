/*
 * solariFrame.h - the SolariNet Control Protocol (SCP) wire frame (§5).
 *
 * Every tier-to-tier message is one length-prefixed frame:
 *
 *   +----------------+-----------------------+--------------------------+--------+
 *   | uint32 frameLen| FrameHeader (32 bytes)| TLV payload (frameLen-32)| uint32 |
 *   |                |                       |                          |  crc32 |
 *   +----------------+-----------------------+--------------------------+--------+
 *
 *   frameLen = byte count of [FrameHeader + TLV payload] (excludes itself & crc32)
 *   crc32    = CRC-32 (IEEE) over [FrameHeader + TLV payload]
 *
 * All multi-byte integers are big-endian (network byte order). The struct below
 * is the in-memory form; the wire form is defined by the pack/unpack functions,
 * NOT by the struct's memory image. Never memcpy the struct onto the wire.
 */
#ifndef SOLARI_FRAME_H
#define SOLARI_FRAME_H

#include "solari/solariCommon.h"

/* ---- message types (§5.4) ---- */
typedef enum {
    SCP_MSG_HELLO          = 0x01,  /* node   -> server : open session, declare role */
    SCP_MSG_WELCOME        = 0x02,  /* server -> node   : accept, return schedule     */
    SCP_MSG_CLIENT_REPORT  = 0x10,  /* client -> server : host metrics snapshot       */
    SCP_MSG_MONITOR_REPORT = 0x11,  /* monitor-> server : probe results batch         */
    SCP_MSG_WATCHDOG       = 0x12,  /* w-dog  -> server : liveness heartbeat          */
    SCP_MSG_DISCOVERY_ADVERT = 0x13,/* node   -> server : observed neighbors (Handoff sec 4) */
    SCP_MSG_TOPOLOGY_REPORT  = 0x14,/* node   -> server : uplink / segment adjacency  */
    SCP_MSG_PEER_ALIVE     = 0x15,  /* monitor<->monitor: gossip liveness (sec 8.2)   */
    SCP_MSG_SURVEY         = 0x20,  /* server -> fleet  : demand immediate round      */
    SCP_MSG_SURVEY_RESP    = 0x21,  /* node   -> server : answer to a survey          */
    SCP_MSG_CONTROL        = 0x30,  /* server -> node   : config/deploy directive     */
    SCP_MSG_CONTROL_RESULT = 0x31,  /* node   -> server : outcome of a directive       */
    SCP_MSG_DECOMMISSION   = 0x32,  /* server -> node   : authoritative teardown; always audited */
    SCP_MSG_WHO_IS_PRIMARY = 0x40,  /* node   -> server : failover discovery query     */
    SCP_MSG_PRIMARY_IS     = 0x41,  /* server -> node   : names current lease holder   */
    SCP_MSG_ACK            = 0x50,  /* any    -> any    : acknowledgement              */
    SCP_MSG_ERROR          = 0x51   /* any    -> any    : structured error             */
} solariMsgType;

/* ---- frame flags (§5.3), a bitfield in the header's flags byte ---- */
enum {
    SCP_FLAG_NONE         = 0x00,
    SCP_FLAG_ACK_REQ      = 0x01,   /* sender wants an ACK with matching correlationId */
    SCP_FLAG_COMPRESSED   = 0x02,   /* TLV payload is DEFLATE-compressed               */
    SCP_FLAG_SPOOL_REPLAY = 0x04,   /* store-and-forward replay; dedup by (node,seq)    */
    SCP_FLAG_URGENT       = 0x08    /* bypass batching; evaluate alerts immediately     */
};

/* ---- fixed 32-byte frame header (§5.2) ---- */
typedef struct {
    uint8_t  magic[2];       /* {'S','N'} - SolariNet sentinel                 */
    uint8_t  protoVersion;   /* SCP_PROTO_VERSION; bump only on breaking change*/
    uint8_t  msgType;        /* solariMsgType                                  */
    uint8_t  flags;          /* SCP_FLAG_* bitfield                            */
    uint8_t  reserved;       /* 0; keeps following fields aligned             */
    uint16_t tlvCount;       /* number of TLV elements in payload             */
    uint64_t sourceNodeId;   /* stable 64-bit node id (§5.5)                  */
    uint64_t sendTimeUnixMs; /* sender wall clock, ms since epoch             */
    uint32_t seqNo;          /* per-source monotonic; dedup & ordering         */
    uint32_t correlationId;  /* echoes a request's seqNo in its reply; 0 if unsolicited */
} solariFrameHeader;

/* The struct may carry padding in memory, but the wire image is exactly 32
 * bytes laid out by solariFramePackHeader. This guards the field set. */
SOLARI_STATIC_ASSERT(sizeof(uint64_t) == 8, u64_is_8);

/* Sentinel bytes. */
#define SCP_MAGIC_0 'S'
#define SCP_MAGIC_1 'N'

/* Pack a header into buf (>= SOLARI_FRAME_HEADER_LEN bytes), big-endian, field
 * by field. Returns SOLARI_OK, or ERR_BUFFER_FULL if bufLen is too small, or
 * ERR_INVALID_ARG on NULL. Does not depend on host endianness. */
solariStatus solariFramePackHeader(const solariFrameHeader *hdr,
                                   uint8_t *buf, size_t bufLen);

/* Inverse of the above. Validates magic and protoVersion; on version mismatch
 * returns ERR_PROTO_VERSION but still fills hdr for diagnostics. Returns
 * ERR_FRAME_BAD_MAGIC on sentinel mismatch, ERR_BUFFER_FULL if bufLen < 32. */
solariStatus solariFrameUnpackHeader(const uint8_t *buf, size_t bufLen,
                                     solariFrameHeader *hdr);

/* Assemble a complete frame (len prefix + header + payload + crc32) into out.
 * *outLen is set to total bytes written. Computes crc32 internally. payload may
 * be NULL iff payloadLen == 0. Returns ERR_BUFFER_FULL if outCap is too small,
 * ERR_INVALID_ARG if the total would exceed SOLARI_MAX_FRAME. */
solariStatus solariFrameBuild(const solariFrameHeader *hdr,
                              const uint8_t *payload, size_t payloadLen,
                              uint8_t *out, size_t outCap, size_t *outLen);

/* Parse one frame from a byte stream. Verifies crc32 and magic. On success sets
 * hdr, points *payload into buf (zero-copy) with *payloadLen, and sets
 * *frameTotalLen to the number of bytes this frame occupied (so a caller can
 * advance a stream cursor). Returns ERR_FRAME_INCOMPLETE if buf holds less than
 * one full frame, ERR_FRAME_BAD_CRC / ERR_FRAME_BAD_MAGIC on corruption. Any of
 * payload/payloadLen/frameTotalLen may be NULL if not wanted. */
solariStatus solariFrameParse(const uint8_t *buf, size_t bufLen,
                              solariFrameHeader *hdr,
                              const uint8_t **payload, size_t *payloadLen,
                              size_t *frameTotalLen);

#endif /* SOLARI_FRAME_H */
