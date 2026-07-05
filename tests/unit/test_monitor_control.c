/*
 * test_monitor_control.c - the monitor's pure control-plane logic (sec 8, 9.1):
 *   - CTRL_ADOPT_TARGET still converges through monitorAddTarget (idempotent);
 *   - CTRL_SET_CONFIG / CTRL_PROVISION apply the JSON blob (round interval,
 *     probe tuning, target set) under epoch monotonicity;
 *   - the CONTROL_RESULT payload carries verb + error magnitude + applied
 *     epoch in the exact TLV shape serverControlOnResult parses;
 *   - addressing over the broadcast PUB channel and malformed-input rejection.
 */
#include "unity.h"

#include "monitor.h"

#include "solari/solariMsg.h"
#include "solari/solariTlv.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

#define SELF_NODE 0x5555666677778888ULL

static size_t buildDirective(uint8_t verb, uint64_t epoch, uint64_t targetNode,
                             const char *blob, uint8_t *out, size_t cap,
                             uint16_t *tlvCount)
{
    solariControl c;
    size_t len = 0;
    memset(&c, 0, sizeof c);
    c.verb        = verb;
    c.targetEpoch = epoch;
    c.targetNode  = targetNode;
    c.payload     = (const uint8_t *)blob;
    c.payloadLen  = blob ? (uint16_t)strlen(blob) : 0;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariMsgBuildControl(&c, out, cap, &len, tlvCount));
    return len;
}

static bool findU64(const uint8_t *buf, size_t len, uint16_t type, uint64_t *out)
{
    solariTlvReader r; uint16_t t, l; const uint8_t *v;
    solariTlvReaderInit(&r, buf, len);
    while (solariTlvNext(&r, &t, &v, &l) == SOLARI_OK)
        if (t == type) return solariTlvReadU64(v, l, out) == SOLARI_OK;
    return false;
}
static bool findU16(const uint8_t *buf, size_t len, uint16_t type, uint16_t *out)
{
    solariTlvReader r; uint16_t t, l; const uint8_t *v;
    solariTlvReaderInit(&r, buf, len);
    while (solariTlvNext(&r, &t, &v, &l) == SOLARI_OK)
        if (t == type) return solariTlvReadU16(v, l, out) == SOLARI_OK;
    return false;
}

/* ---- blob apply ---- */

static void test_apply_blob_tuning_and_targets(void)
{
    monitorConfig cfg;
    const char *blob =
        "{\"probe\":{\"roundIntervalSec\":12,\"probesPerRound\":3,"
        "\"probeTimeoutMs\":700,\"replFactor\":1},"
        "\"targets\":[\"tcp:db.example.net:3306 : db\",\"icmp:10.0.0.1\","
        "\"bogus-spec\"]}";

    monitorConfigDefaults(&cfg);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        monitorControlApplyBlob(&cfg, blob, strlen(blob)));

    TEST_ASSERT_EQUAL_UINT32(12, cfg.roundIntervalSec);
    TEST_ASSERT_EQUAL_UINT16(3,  cfg.probesPerRound);
    TEST_ASSERT_EQUAL_UINT32(700, cfg.probeTimeoutMs);
    TEST_ASSERT_EQUAL_UINT8(1, cfg.replFactor);
    /* the bad spec is skipped; the two valid ones land as the SET */
    TEST_ASSERT_EQUAL_UINT8(2, cfg.targetCount);
    TEST_ASSERT_EQUAL_STRING("tcp:db.example.net:3306", cfg.targets[0].targetId);
    TEST_ASSERT_EQUAL_STRING("db", cfg.targets[0].label);
    TEST_ASSERT_EQUAL_STRING("icmp:10.0.0.1", cfg.targets[1].targetId);
}

static void test_apply_blob_target_set_replaces(void)
{
    monitorConfig cfg;
    monitorConfigDefaults(&cfg);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, monitorAddTarget(&cfg, "tcp:old.host:1"));
    TEST_ASSERT_EQUAL_UINT8(1, cfg.targetCount);

    /* targets present -> the array IS the desired set (old entries drop) */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        monitorControlApplyBlob(&cfg, "{\"targets\":[\"tcp:new.host:2\"]}",
                                strlen("{\"targets\":[\"tcp:new.host:2\"]}")));
    TEST_ASSERT_EQUAL_UINT8(1, cfg.targetCount);
    TEST_ASSERT_EQUAL_STRING("tcp:new.host:2", cfg.targets[0].targetId);

    /* targets absent -> the set is untouched */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        monitorControlApplyBlob(&cfg, "{\"roundIntervalSec\":20}",
                                strlen("{\"roundIntervalSec\":20}")));
    TEST_ASSERT_EQUAL_UINT8(1, cfg.targetCount);
    TEST_ASSERT_EQUAL_UINT32(20, cfg.roundIntervalSec);
}

static void test_apply_blob_bad_json_is_noop(void)
{
    monitorConfig cfg, before;
    monitorConfigDefaults(&cfg);
    before = cfg;
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG,
        monitorControlApplyBlob(&cfg, "{\"roundIntervalSec\":", 20));
    TEST_ASSERT_EQUAL_MEMORY(&before, &cfg, sizeof cfg);
}

/* ---- directive handling ---- */

static void test_handle_adopt_target(void)
{
    monitorConfig cfg;
    monitorControlState st;
    monitorControlOutcome oc;
    uint8_t dir[512], res[MONITOR_CTRL_RESULT_CAP];
    size_t dlen, rlen = 0;
    uint16_t tc = 0, rcount = 0, err = 0xFFFF;

    monitorConfigDefaults(&cfg);
    memset(&st, 0, sizeof st);

    dlen = buildDirective(CTRL_ADOPT_TARGET, 0, SELF_NODE,
                          "tcp:10.9.8.7:443 : web", dir, sizeof dir, &tc);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        monitorHandleControl(&st, &cfg, SELF_NODE, dir, dlen,
                             res, sizeof res, &rlen, &rcount, &oc));
    TEST_ASSERT_TRUE(oc.wantReply);
    TEST_ASSERT_EQUAL_UINT8(1, cfg.targetCount);
    TEST_ASSERT_EQUAL_STRING("tcp:10.9.8.7:443", cfg.targets[0].targetId);
    TEST_ASSERT_TRUE(findU16(res, rlen, TLV_ERROR_CODE, &err));
    TEST_ASSERT_EQUAL_UINT16(0, err);

    /* re-adopting the same target is an idempotent success */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        monitorHandleControl(&st, &cfg, SELF_NODE, dir, dlen,
                             res, sizeof res, &rlen, &rcount, &oc));
    TEST_ASSERT_EQUAL_UINT8(1, cfg.targetCount);
}

static void test_handle_set_config_epoch_flow(void)
{
    monitorConfig cfg;
    monitorControlState st;
    monitorControlOutcome oc;
    uint8_t dir[512], res[MONITOR_CTRL_RESULT_CAP];
    size_t dlen, rlen = 0;
    uint16_t tc = 0, rcount = 0, err = 0xFFFF;
    uint64_t epoch = 0;

    monitorConfigDefaults(&cfg);
    memset(&st, 0, sizeof st);
    st.appliedEpoch = 4;

    /* newer epoch applies + acks the new applied epoch */
    dlen = buildDirective(CTRL_SET_CONFIG, 6, SELF_NODE,
                          "{\"roundIntervalSec\":8}", dir, sizeof dir, &tc);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        monitorHandleControl(&st, &cfg, SELF_NODE, dir, dlen,
                             res, sizeof res, &rlen, &rcount, &oc));
    TEST_ASSERT_TRUE(oc.applied);
    TEST_ASSERT_EQUAL_UINT32(8, cfg.roundIntervalSec);
    TEST_ASSERT_EQUAL_UINT64(6, st.appliedEpoch);
    TEST_ASSERT_TRUE(findU16(res, rlen, TLV_ERROR_CODE, &err));
    TEST_ASSERT_EQUAL_UINT16(0, err);
    TEST_ASSERT_TRUE(findU64(res, rlen, TLV_CTRL_TARGET_EPOCH, &epoch));
    TEST_ASSERT_EQUAL_UINT64(6, epoch);

    /* stale epoch: ack only, never re-apply (no thrash) */
    cfg.roundIntervalSec = 30;
    dlen = buildDirective(CTRL_PROVISION, 6, SELF_NODE,
                          "{\"roundIntervalSec\":8}", dir, sizeof dir, &tc);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        monitorHandleControl(&st, &cfg, SELF_NODE, dir, dlen,
                             res, sizeof res, &rlen, &rcount, &oc));
    TEST_ASSERT_FALSE(oc.applied);
    TEST_ASSERT_EQUAL_UINT32(30, cfg.roundIntervalSec);
    TEST_ASSERT_TRUE(findU64(res, rlen, TLV_CTRL_TARGET_EPOCH, &epoch));
    TEST_ASSERT_EQUAL_UINT64(6, epoch);
}

static void test_handle_addressing_and_garbage(void)
{
    monitorConfig cfg;
    monitorControlState st;
    monitorControlOutcome oc;
    uint8_t dir[512], res[MONITOR_CTRL_RESULT_CAP];
    uint8_t garbage[] = { 0x30, 0x01, 0x40, 0x00 };   /* truncated TLV */
    size_t dlen, rlen = 0;
    uint16_t tc = 0, rcount = 0;

    monitorConfigDefaults(&cfg);
    memset(&st, 0, sizeof st);

    /* addressed elsewhere: silent drop */
    dlen = buildDirective(CTRL_SET_CONFIG, 9, SELF_NODE + 7,
                          "{\"roundIntervalSec\":2}", dir, sizeof dir, &tc);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        monitorHandleControl(&st, &cfg, SELF_NODE, dir, dlen,
                             res, sizeof res, &rlen, &rcount, &oc));
    TEST_ASSERT_FALSE(oc.wantReply);
    TEST_ASSERT_EQUAL_UINT32(30, cfg.roundIntervalSec);
    TEST_ASSERT_EQUAL_UINT64(0, st.appliedEpoch);

    /* malformed TLV soup: rejected without crash or reply */
    TEST_ASSERT_TRUE(
        monitorHandleControl(&st, &cfg, SELF_NODE, garbage, sizeof garbage,
                             res, sizeof res, &rlen, &rcount, &oc) != SOLARI_OK);
    TEST_ASSERT_FALSE(oc.wantReply);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_apply_blob_tuning_and_targets);
    RUN_TEST(test_apply_blob_target_set_replaces);
    RUN_TEST(test_apply_blob_bad_json_is_noop);
    RUN_TEST(test_handle_adopt_target);
    RUN_TEST(test_handle_set_config_epoch_flow);
    RUN_TEST(test_handle_addressing_and_garbage);
    return UNITY_END();
}
