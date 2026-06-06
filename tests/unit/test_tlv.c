/*
 * test_tlv.c - TLV writer/reader symmetry, truncation, forward-compat skip.
 */
#include "unity.h"
#include "solari/solariTlv.h"
#include "solari/solariError.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_writer_counts_and_len(void)
{
    uint8_t buf[64];
    solariTlvWriter w;
    solariTlvWriterInit(&w, buf, sizeof buf);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvAppendU8(&w, TLV_ROLE, 2));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvAppendU32(&w, TLV_CONFIG_EPOCH, 0x01020304u));
    /* two elements: (4+1) + (4+4) = 13 bytes */
    TEST_ASSERT_EQUAL_UINT16(2, w.count);
    TEST_ASSERT_EQUAL_size_t(13, w.len);
}

static void test_typed_roundTrip(void)
{
    uint8_t buf[128];
    solariTlvWriter w;
    solariTlvReader r;
    uint16_t type, len;
    const uint8_t *val;

    solariTlvWriterInit(&w, buf, sizeof buf);
    solariTlvAppendU8 (&w, TLV_ROLE, 3);
    solariTlvAppendU16(&w, TLV_ERROR_CODE, 0xABCD);
    solariTlvAppendU32(&w, TLV_CTRL_TARGET_EPOCH, 0xDEADBEEFu);
    solariTlvAppendU64(&w, TLV_RAM_USED_KB, 0x1122334455667788ull);
    solariTlvAppendStr(&w, TLV_HOST_FQDN, "hydrogen.akoria.net");

    solariTlvReaderInit(&r, buf, w.len);

    uint8_t u8; uint16_t u16; uint32_t u32; uint64_t u64;

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvNext(&r, &type, &val, &len));
    TEST_ASSERT_EQUAL_UINT16(TLV_ROLE, type);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvReadU8(val, len, &u8));
    TEST_ASSERT_EQUAL_UINT8(3, u8);

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvNext(&r, &type, &val, &len));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvReadU16(val, len, &u16));
    TEST_ASSERT_EQUAL_UINT16(0xABCD, u16);

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvNext(&r, &type, &val, &len));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvReadU32(val, len, &u32));
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, u32);

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvNext(&r, &type, &val, &len));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvReadU64(val, len, &u64));
    TEST_ASSERT_EQUAL_UINT64(0x1122334455667788ull, u64);

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvNext(&r, &type, &val, &len));
    TEST_ASSERT_EQUAL_UINT16(TLV_HOST_FQDN, type);
    TEST_ASSERT_EQUAL_UINT16(19, len);   /* strlen("hydrogen.akoria.net") */
    TEST_ASSERT_EQUAL_MEMORY("hydrogen.akoria.net", val, 19);

    TEST_ASSERT_EQUAL_INT(ERR_TLV_END, solariTlvNext(&r, &type, &val, &len));
}

static void test_typedRead_lengthMismatch(void)
{
    uint8_t buf[16];
    solariTlvWriter w;
    solariTlvReader r;
    uint16_t type, len;
    const uint8_t *val;
    uint32_t u32;

    solariTlvWriterInit(&w, buf, sizeof buf);
    solariTlvAppendU16(&w, TLV_ERROR_CODE, 0x1234);  /* 2-byte value */
    solariTlvReaderInit(&r, buf, w.len);
    solariTlvNext(&r, &type, &val, &len);
    /* reading a u32 from a 2-byte value must be rejected */
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG, solariTlvReadU32(val, len, &u32));
}

static void test_bufferFull(void)
{
    uint8_t buf[6];   /* room for a 1-byte TLV (5) but not two */
    solariTlvWriter w;
    solariTlvWriterInit(&w, buf, sizeof buf);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvAppendU8(&w, TLV_ROLE, 1));
    TEST_ASSERT_EQUAL_INT(ERR_BUFFER_FULL, solariTlvAppendU8(&w, TLV_ROLE, 2));
}

static void test_reader_truncated(void)
{
    /* a TLV header claiming 10 bytes but only 2 present */
    uint8_t raw[6] = { 0x01, 0x01, 0x00, 0x0A, 0xAA, 0xBB };
    solariTlvReader r;
    uint16_t type, len;
    const uint8_t *val;
    solariTlvReaderInit(&r, raw, sizeof raw);
    TEST_ASSERT_EQUAL_INT(ERR_TLV_TRUNCATED, solariTlvNext(&r, &type, &val, &len));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_writer_counts_and_len);
    RUN_TEST(test_typed_roundTrip);
    RUN_TEST(test_typedRead_lengthMismatch);
    RUN_TEST(test_bufferFull);
    RUN_TEST(test_reader_truncated);
    return UNITY_END();
}
