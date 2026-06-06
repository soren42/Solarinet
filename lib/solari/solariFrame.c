/*
 * solariFrame.c - SCP frame pack/unpack/build/parse (§5, §6.2).
 *
 * Wire layout of the 32-byte header (all big-endian):
 *   off  size  field
 *    0    2    magic[2]          {'S','N'}
 *    2    1    protoVersion
 *    3    1    msgType
 *    4    1    flags
 *    5    1    reserved
 *    6    2    tlvCount
 *    8    8    sourceNodeId
 *   16    8    sendTimeUnixMs
 *   24    4    seqNo
 *   28    4    correlationId
 */
#include "solari/solariFrame.h"
#include "solari/solariCrypto.h"
#include "solariEndian.h"

#include <string.h>

#define FRAME_LEN_PREFIX 4u   /* uint32 frameLen at the very front */
#define FRAME_CRC_LEN    4u   /* uint32 crc32 trailer             */

solariStatus solariFramePackHeader(const solariFrameHeader *hdr,
                                   uint8_t *buf, size_t bufLen)
{
    if (!hdr || !buf) {
        return ERR_INVALID_ARG;
    }
    if (bufLen < SOLARI_FRAME_HEADER_LEN) {
        return ERR_BUFFER_FULL;
    }
    buf[0] = hdr->magic[0];
    buf[1] = hdr->magic[1];
    buf[2] = hdr->protoVersion;
    buf[3] = hdr->msgType;
    buf[4] = hdr->flags;
    buf[5] = hdr->reserved;
    solariPutU16(buf + 6,  hdr->tlvCount);
    solariPutU64(buf + 8,  hdr->sourceNodeId);
    solariPutU64(buf + 16, hdr->sendTimeUnixMs);
    solariPutU32(buf + 24, hdr->seqNo);
    solariPutU32(buf + 28, hdr->correlationId);
    return SOLARI_OK;
}

solariStatus solariFrameUnpackHeader(const uint8_t *buf, size_t bufLen,
                                     solariFrameHeader *hdr)
{
    if (!buf || !hdr) {
        return ERR_INVALID_ARG;
    }
    if (bufLen < SOLARI_FRAME_HEADER_LEN) {
        return ERR_BUFFER_FULL;
    }
    hdr->magic[0]       = buf[0];
    hdr->magic[1]       = buf[1];
    hdr->protoVersion   = buf[2];
    hdr->msgType        = buf[3];
    hdr->flags          = buf[4];
    hdr->reserved       = buf[5];
    hdr->tlvCount       = solariGetU16(buf + 6);
    hdr->sourceNodeId   = solariGetU64(buf + 8);
    hdr->sendTimeUnixMs = solariGetU64(buf + 16);
    hdr->seqNo          = solariGetU32(buf + 24);
    hdr->correlationId  = solariGetU32(buf + 28);

    if (hdr->magic[0] != SCP_MAGIC_0 || hdr->magic[1] != SCP_MAGIC_1) {
        return ERR_FRAME_BAD_MAGIC;
    }
    if (hdr->protoVersion != SCP_PROTO_VERSION) {
        return ERR_PROTO_VERSION;   /* hdr still filled for diagnostics */
    }
    return SOLARI_OK;
}

solariStatus solariFrameBuild(const solariFrameHeader *hdr,
                              const uint8_t *payload, size_t payloadLen,
                              uint8_t *out, size_t outCap, size_t *outLen)
{
    size_t frameBody;   /* header + payload */
    size_t total;       /* prefix + body + crc */
    uint32_t crc;
    solariStatus st;

    if (!hdr || !out || (payloadLen > 0 && !payload)) {
        return ERR_INVALID_ARG;
    }
    frameBody = (size_t)SOLARI_FRAME_HEADER_LEN + payloadLen;
    total = FRAME_LEN_PREFIX + frameBody + FRAME_CRC_LEN;

    if (frameBody > SOLARI_MAX_FRAME) {
        return ERR_INVALID_ARG;
    }
    if (outCap < total) {
        return ERR_BUFFER_FULL;
    }

    /* frameLen prefix: byte count of [header + payload], excludes itself & crc */
    solariPutU32(out, (uint32_t)frameBody);

    st = solariFramePackHeader(hdr, out + FRAME_LEN_PREFIX, SOLARI_FRAME_HEADER_LEN);
    if (st != SOLARI_OK) {
        return st;
    }
    if (payloadLen > 0) {
        memcpy(out + FRAME_LEN_PREFIX + SOLARI_FRAME_HEADER_LEN, payload, payloadLen);
    }

    /* crc32 over [header + payload] */
    crc = solariCrc32(0, out + FRAME_LEN_PREFIX, frameBody);
    solariPutU32(out + FRAME_LEN_PREFIX + frameBody, crc);

    if (outLen) {
        *outLen = total;
    }
    return SOLARI_OK;
}

solariStatus solariFrameParse(const uint8_t *buf, size_t bufLen,
                              solariFrameHeader *hdr,
                              const uint8_t **payload, size_t *payloadLen,
                              size_t *frameTotalLen)
{
    uint32_t frameBody;
    size_t total;
    uint32_t crcStored, crcCalc;
    solariStatus st;

    if (!buf || !hdr) {
        return ERR_INVALID_ARG;
    }
    if (bufLen < FRAME_LEN_PREFIX) {
        return ERR_FRAME_INCOMPLETE;
    }
    frameBody = solariGetU32(buf);
    if (frameBody < SOLARI_FRAME_HEADER_LEN || frameBody > SOLARI_MAX_FRAME) {
        return ERR_FRAME_INCOMPLETE;   /* implausible length: treat as not-yet-whole/corrupt */
    }
    total = FRAME_LEN_PREFIX + (size_t)frameBody + FRAME_CRC_LEN;
    if (bufLen < total) {
        return ERR_FRAME_INCOMPLETE;
    }

    /* Verify CRC before trusting any field. */
    crcStored = solariGetU32(buf + FRAME_LEN_PREFIX + frameBody);
    crcCalc   = solariCrc32(0, buf + FRAME_LEN_PREFIX, frameBody);
    if (crcStored != crcCalc) {
        return ERR_FRAME_BAD_CRC;
    }

    st = solariFrameUnpackHeader(buf + FRAME_LEN_PREFIX, SOLARI_FRAME_HEADER_LEN, hdr);
    if (st != SOLARI_OK && st != ERR_PROTO_VERSION) {
        return st;   /* bad magic etc. ERR_PROTO_VERSION still returns payload below */
    }

    if (payload) {
        *payload = buf + FRAME_LEN_PREFIX + SOLARI_FRAME_HEADER_LEN;
    }
    if (payloadLen) {
        *payloadLen = (size_t)frameBody - SOLARI_FRAME_HEADER_LEN;
    }
    if (frameTotalLen) {
        *frameTotalLen = total;
    }
    return st;   /* SOLARI_OK or ERR_PROTO_VERSION (caller decides) */
}
