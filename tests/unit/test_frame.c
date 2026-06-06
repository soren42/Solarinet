/*
 * test_frame.c - SCP framing conformance & round-trip (§5, §14).
 */
#include "unity.h"
#include "solari/solariFrame.h"
#include "solari/solariError.h"
#include "../fixtures/golden_frames.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static solariFrameHeader goldenHeaderStruct(void)
{
    solariFrameHeader h;
    memset(&h, 0, sizeof h);
    h.magic[0]       = SCP_MAGIC_0;
    h.magic[1]       = SCP_MAGIC_1;
    h.protoVersion   = GF_PROTO_VERSION;
    h.msgType        = GF_MSG_TYPE;
    h.flags          = GF_FLAGS;
    h.reserved       = GF_RESERVED;
    h.tlvCount       = GF_TLV_COUNT;
    h.sourceNodeId   = GF_SOURCE_NODE_ID;
    h.sendTimeUnixMs = GF_SEND_TIME_MS;
    h.seqNo          = GF_SEQ_NO;
    h.correlationId  = GF_CORRELATION_ID;
    return h;
}

/* The packed header must match the hand-computed golden bytes exactly. */
static void test_packHeader_matchesGolden(void)
{
    solariFrameHeader h = goldenHeaderStruct();
    uint8_t buf[32];
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariFramePackHeader(&h, buf, sizeof buf));
    TEST_ASSERT_EQUAL_MEMORY(GOLDEN_HEADER, buf, 32);
}

/* Unpacking the golden bytes must reproduce every field. */
static void test_unpackHeader_fromGolden(void)
{
    solariFrameHeader h;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariFrameUnpackHeader(GOLDEN_HEADER, 32, &h));
    TEST_ASSERT_EQUAL_UINT8(SCP_MAGIC_0, h.magic[0]);
    TEST_ASSERT_EQUAL_UINT8(SCP_MAGIC_1, h.magic[1]);
    TEST_ASSERT_EQUAL_UINT8(GF_PROTO_VERSION, h.protoVersion);
    TEST_ASSERT_EQUAL_UINT8(GF_MSG_TYPE, h.msgType);
    TEST_ASSERT_EQUAL_UINT8(GF_FLAGS, h.flags);
    TEST_ASSERT_EQUAL_UINT16(GF_TLV_COUNT, h.tlvCount);
    TEST_ASSERT_EQUAL_UINT64(GF_SOURCE_NODE_ID, h.sourceNodeId);
    TEST_ASSERT_EQUAL_UINT64(GF_SEND_TIME_MS, h.sendTimeUnixMs);
    TEST_ASSERT_EQUAL_UINT32(GF_SEQ_NO, h.seqNo);
    TEST_ASSERT_EQUAL_UINT32(GF_CORRELATION_ID, h.correlationId);
}

/* pack -> unpack is the identity on all fields. */
static void test_header_roundTrip(void)
{
    solariFrameHeader in = goldenHeaderStruct();
    solariFrameHeader out;
    uint8_t buf[32];
    solariFramePackHeader(&in, buf, sizeof buf);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariFrameUnpackHeader(buf, 32, &out));
    TEST_ASSERT_EQUAL_MEMORY(&in.magic, &out.magic, 2);
    TEST_ASSERT_EQUAL_UINT64(in.sourceNodeId, out.sourceNodeId);
    TEST_ASSERT_EQUAL_UINT32(in.correlationId, out.correlationId);
}

static void test_packHeader_bufferTooSmall(void)
{
    solariFrameHeader h = goldenHeaderStruct();
    uint8_t buf[31];
    TEST_ASSERT_EQUAL_INT(ERR_BUFFER_FULL, solariFramePackHeader(&h, buf, sizeof buf));
}

static void test_unpackHeader_badMagic(void)
{
    uint8_t bad[32];
    solariFrameHeader h;
    memcpy(bad, GOLDEN_HEADER, 32);
    bad[0] = 'X';
    TEST_ASSERT_EQUAL_INT(ERR_FRAME_BAD_MAGIC, solariFrameUnpackHeader(bad, 32, &h));
}

static void test_unpackHeader_badVersion(void)
{
    uint8_t bad[32];
    solariFrameHeader h;
    memcpy(bad, GOLDEN_HEADER, 32);
    bad[2] = 0x7F;
    TEST_ASSERT_EQUAL_INT(ERR_PROTO_VERSION, solariFrameUnpackHeader(bad, 32, &h));
    /* still filled for diagnostics */
    TEST_ASSERT_EQUAL_UINT8(0x7F, h.protoVersion);
}

/* Build a full frame with a payload, parse it back, verify header+payload. */
static void test_frame_buildParse_roundTrip(void)
{
    solariFrameHeader in = goldenHeaderStruct();
    const uint8_t payload[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02 };
    uint8_t frame[128];
    size_t frameLen = 0, total = 0, plen = 0;
    const uint8_t *pp = NULL;
    solariFrameHeader out;

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariFrameBuild(&in, payload, sizeof payload, frame, sizeof frame, &frameLen));
    /* total = 4 (prefix) + 32 (hdr) + 6 (payload) + 4 (crc) = 46 */
    TEST_ASSERT_EQUAL_size_t(46, frameLen);

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariFrameParse(frame, frameLen, &out, &pp, &plen, &total));
    TEST_ASSERT_EQUAL_size_t(sizeof payload, plen);
    TEST_ASSERT_EQUAL_size_t(frameLen, total);
    TEST_ASSERT_NOT_NULL(pp);
    TEST_ASSERT_EQUAL_MEMORY(payload, pp, sizeof payload);
    TEST_ASSERT_EQUAL_UINT64(in.sourceNodeId, out.sourceNodeId);
}

static void test_frame_emptyPayload(void)
{
    solariFrameHeader in = goldenHeaderStruct();
    uint8_t frame[64];
    size_t frameLen = 0, plen = 99;
    const uint8_t *pp = NULL;
    solariFrameHeader out;

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariFrameBuild(&in, NULL, 0, frame, sizeof frame, &frameLen));
    TEST_ASSERT_EQUAL_size_t(40, frameLen);   /* 4 + 32 + 0 + 4 */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariFrameParse(frame, frameLen, &out, &pp, &plen, NULL));
    TEST_ASSERT_EQUAL_size_t(0, plen);
}

static void test_frame_corruptCrc(void)
{
    solariFrameHeader in = goldenHeaderStruct();
    uint8_t frame[64];
    size_t frameLen = 0;
    solariFrameHeader out;

    solariFrameBuild(&in, NULL, 0, frame, sizeof frame, &frameLen);
    frame[10] ^= 0xFF;   /* flip a header byte; crc must now mismatch */
    TEST_ASSERT_EQUAL_INT(ERR_FRAME_BAD_CRC,
        solariFrameParse(frame, frameLen, &out, NULL, NULL, NULL));
}

static void test_frame_incomplete(void)
{
    solariFrameHeader in = goldenHeaderStruct();
    uint8_t frame[64];
    size_t frameLen = 0;
    solariFrameHeader out;

    solariFrameBuild(&in, NULL, 0, frame, sizeof frame, &frameLen);
    /* hand the parser one byte short of a whole frame */
    TEST_ASSERT_EQUAL_INT(ERR_FRAME_INCOMPLETE,
        solariFrameParse(frame, frameLen - 1, &out, NULL, NULL, NULL));
}

static void test_frame_buildBufferTooSmall(void)
{
    solariFrameHeader in = goldenHeaderStruct();
    uint8_t frame[39];   /* one short of the 40-byte empty frame */
    size_t frameLen = 0;
    TEST_ASSERT_EQUAL_INT(ERR_BUFFER_FULL,
        solariFrameBuild(&in, NULL, 0, frame, sizeof frame, &frameLen));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_packHeader_matchesGolden);
    RUN_TEST(test_unpackHeader_fromGolden);
    RUN_TEST(test_header_roundTrip);
    RUN_TEST(test_packHeader_bufferTooSmall);
    RUN_TEST(test_unpackHeader_badMagic);
    RUN_TEST(test_unpackHeader_badVersion);
    RUN_TEST(test_frame_buildParse_roundTrip);
    RUN_TEST(test_frame_emptyPayload);
    RUN_TEST(test_frame_corruptCrc);
    RUN_TEST(test_frame_incomplete);
    RUN_TEST(test_frame_buildBufferTooSmall);
    return UNITY_END();
}
