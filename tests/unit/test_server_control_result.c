/* CONTROL_RESULT must be complete and carry a nonzero correlation before DB I/O. */
#include "unity.h"
#include "server.h"
#include "solari/solariTlv.h"

#include <stdio.h>
#include <string.h>

static serverContext ctx;
static uint8_t payload[128];
static size_t payloadLen;
static uint64_t testEpoch;
static bool dbWasCalled;
static bool dbSawEpoch;
static uint8_t dbSawVerb;
static char dbSawResult[32];

solariStatus serverDbControlResultApply(serverDb *db, uint64_t nodeId,
    uint32_t correlationId, uint8_t verb, uint64_t appliedEpoch,
    bool haveAppliedEpoch, uint16_t errorCode, const char *lastResult,
    uint64_t whenUnixMs)
{
    (void)db; (void)nodeId; (void)correlationId; (void)appliedEpoch;
    (void)errorCode; (void)whenUnixMs;
    dbWasCalled = true;
    dbSawEpoch = haveAppliedEpoch;
    dbSawVerb = verb;
    snprintf(dbSawResult, sizeof dbSawResult, "%s", lastResult ? lastResult : "");
    return SOLARI_OK;
}

solariStatus serverDbDecommissionAck(serverDb *db, uint64_t nodeId,
                                      uint64_t lifeGeneration)
{ (void)db; (void)nodeId; (void)lifeGeneration; return SOLARI_OK; }

void setUp(void)
{
    memset(&ctx, 0, sizeof ctx);
    ctx.db = (serverDb *)1; /* invalid pointer: malformed replies must not use it */
    payloadLen = 0;
    dbWasCalled = false;
    testEpoch = 7;
    dbSawEpoch = false;
    dbSawVerb = 0;
    dbSawResult[0] = '\0';
}

void tearDown(void) {}

static void buildResult(uint8_t verb, int haveVerb, int haveError, int haveEpoch)
{
    solariTlvWriter writer;
    solariTlvWriterInit(&writer, payload, sizeof payload);
    if (haveVerb)
        TEST_ASSERT_EQUAL_INT(SOLARI_OK,
            solariTlvAppendU8(&writer, TLV_CTRL_VERB, verb));
    if (haveError)
        TEST_ASSERT_EQUAL_INT(SOLARI_OK,
            solariTlvAppendU16(&writer, TLV_ERROR_CODE, 0));
    if (haveEpoch)
        TEST_ASSERT_EQUAL_INT(SOLARI_OK,
            solariTlvAppendU64(&writer, TLV_CTRL_TARGET_EPOCH, testEpoch));
    payloadLen = writer.len;
}

static void test_missing_error_is_rejected(void)
{
    buildResult(CTRL_SET_CONFIG, 1, 0, 1);
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG,
        serverControlOnResult(&ctx, 42, 17, payload, payloadLen));
    TEST_ASSERT_FALSE(dbWasCalled);
}

static void test_missing_verb_is_rejected(void)
{
    buildResult(CTRL_SET_CONFIG, 0, 1, 1);
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG,
        serverControlOnResult(&ctx, 42, 17, payload, payloadLen));
    TEST_ASSERT_FALSE(dbWasCalled);
}

static void test_missing_epoch_does_not_advance_config(void)
{
    buildResult(CTRL_SET_CONFIG, 1, 1, 0);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverControlOnResult(&ctx, 42, 17, payload, payloadLen));
    TEST_ASSERT_TRUE(dbWasCalled);
    TEST_ASSERT_FALSE(dbSawEpoch);
    TEST_ASSERT_EQUAL_UINT8(CTRL_SET_CONFIG, dbSawVerb);
    TEST_ASSERT_EQUAL_STRING("epoch-missing", dbSawResult);
}

static void test_nonconvergent_result_accepts_legacy_missing_epoch(void)
{
    buildResult(CTRL_RESTART, 1, 1, 0);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverControlOnResult(&ctx, 42, 17, payload, payloadLen));
    TEST_ASSERT_TRUE(dbWasCalled);
    TEST_ASSERT_FALSE(dbSawEpoch);
    TEST_ASSERT_EQUAL_UINT8(CTRL_RESTART, dbSawVerb);
    TEST_ASSERT_EQUAL_STRING("ok", dbSawResult);
}

static void test_present_zero_epoch_is_not_missing(void)
{
    testEpoch = 0;
    buildResult(CTRL_SET_CONFIG, 1, 1, 1);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverControlOnResult(&ctx, 42, 17, payload, payloadLen));
    TEST_ASSERT_TRUE(dbWasCalled);
    TEST_ASSERT_TRUE(dbSawEpoch);
}

static void test_zero_correlation_is_rejected(void)
{
    buildResult(CTRL_SET_CONFIG, 1, 1, 1);
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG,
        serverControlOnResult(&ctx, 42, 0, payload, payloadLen));
    TEST_ASSERT_FALSE(dbWasCalled);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_missing_error_is_rejected);
    RUN_TEST(test_missing_verb_is_rejected);
    RUN_TEST(test_missing_epoch_does_not_advance_config);
    RUN_TEST(test_nonconvergent_result_accepts_legacy_missing_epoch);
    RUN_TEST(test_present_zero_epoch_is_not_missing);
    RUN_TEST(test_zero_correlation_is_rejected);
    return UNITY_END();
}
