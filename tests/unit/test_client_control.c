/*
 * test_client_control.c - the client's pure control-plane logic (§7.3, §9.1):
 *   - JSON blob -> clientConfig apply (missing keys keep current values; a
 *     malformed blob is a whole-document no-op);
 *   - directive handling: addressing filter over the broadcast PUB channel,
 *     epoch monotonicity (never re-apply the past; idempotent ack), and the
 *     CONTROL_RESULT payload - parsed back with the same TLV walk
 *     serverControlOnResult uses (verb + error magnitude + applied epoch), so
 *     the convergence round-trip is covered without sockets or a DB.
 */
#include "unity.h"

#include "clientControl.h"

#include "solari/solariMsg.h"
#include "solari/solariTlv.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

#define SELF_NODE 0x1111222233334444ULL

/* Mirror of serverControlParseResult's TLV walk (serverControl.c): the wire
 * contract the server folds into serverDbSetNodeApplied. */
typedef struct {
    uint8_t  verb;
    uint16_t errCode;
    uint64_t appliedEpoch;
    bool haveVerb, haveErr, haveEpoch;
} resultView;

static solariStatus parseResult(const uint8_t *payload, size_t len, resultView *res)
{
    solariTlvReader r;
    uint16_t type, vlen;
    const uint8_t *val;
    solariStatus rc;

    memset(res, 0, sizeof *res);
    solariTlvReaderInit(&r, payload, len);
    for (;;) {
        rc = solariTlvNext(&r, &type, &val, &vlen);
        if (rc == ERR_TLV_END) break;
        if (rc != SOLARI_OK) return rc;
        switch (type) {
        case TLV_CTRL_VERB:
            if (solariTlvReadU8(val, vlen, &res->verb) == SOLARI_OK) res->haveVerb = true;
            break;
        case TLV_ERROR_CODE:
            if (solariTlvReadU16(val, vlen, &res->errCode) == SOLARI_OK) res->haveErr = true;
            break;
        case TLV_CTRL_TARGET_EPOCH:
            if (solariTlvReadU64(val, vlen, &res->appliedEpoch) == SOLARI_OK) res->haveEpoch = true;
            break;
        default: break;
        }
    }
    return SOLARI_OK;
}

/* Build a CONTROL directive payload the way the server does (solariMsgBuildControl
 * is the shared codec both serverControlBuild and provBuild* feed). */
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

/* ---- blob apply ---- */

static void test_apply_blob_full(void)
{
    clientConfig cfg;
    const char *blob =
        "{\"schedule\":{\"sampleIntervalSec\":25,\"watchdogIntervalSec\":9},"
        "\"processes\":[\"mariadbd\",\"nginx\"],"
        "\"logfiles\":[\"/var/log/syslog : ERROR|WARN\",\"/var/log/auth.log\"]}";

    clientConfigDefaults(&cfg);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        clientControlApplyBlob(&cfg, blob, strlen(blob)));

    TEST_ASSERT_EQUAL_UINT32(25, cfg.sampleIntervalSec);
    TEST_ASSERT_EQUAL_UINT32(9,  cfg.watchdogIntervalSec);
    TEST_ASSERT_EQUAL_UINT8(2, cfg.procCount);
    TEST_ASSERT_EQUAL_STRING("mariadbd", cfg.procs[0]);
    TEST_ASSERT_EQUAL_STRING("nginx",    cfg.procs[1]);
    TEST_ASSERT_EQUAL_UINT8(2, cfg.logCount);
    TEST_ASSERT_EQUAL_STRING("/var/log/syslog", cfg.logs[0].path);
    TEST_ASSERT_EQUAL_STRING("ERROR|WARN",      cfg.logs[0].regex);
    TEST_ASSERT_EQUAL_STRING("/var/log/auth.log", cfg.logs[1].path);
    TEST_ASSERT_EQUAL_STRING("", cfg.logs[1].regex);
}

static void test_apply_blob_missing_keys_keep_current(void)
{
    clientConfig cfg;
    clientConfigDefaults(&cfg);
    cfg.sampleIntervalSec = 33;
    strncpy(cfg.procs[0], "keepme", sizeof cfg.procs[0] - 1);
    cfg.procCount = 1;

    /* only the watchdog interval present: everything else untouched */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        clientControlApplyBlob(&cfg, "{\"watchdogIntervalSec\":11}",
                               strlen("{\"watchdogIntervalSec\":11}")));
    TEST_ASSERT_EQUAL_UINT32(33, cfg.sampleIntervalSec);
    TEST_ASSERT_EQUAL_UINT32(11, cfg.watchdogIntervalSec);
    TEST_ASSERT_EQUAL_UINT8(1, cfg.procCount);
    TEST_ASSERT_EQUAL_STRING("keepme", cfg.procs[0]);

    /* empty blob = epoch-only directive: full no-op success */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientControlApplyBlob(&cfg, NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(33, cfg.sampleIntervalSec);
}

static void test_apply_blob_bad_json_is_noop(void)
{
    clientConfig cfg, before;
    const char *bad = "{\"sampleIntervalSec\":5, \"processes\":[\"x\"";  /* truncated */

    clientConfigDefaults(&cfg);
    cfg.sampleIntervalSec = 60;
    before = cfg;

    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG,
        clientControlApplyBlob(&cfg, bad, strlen(bad)));
    TEST_ASSERT_EQUAL_MEMORY(&before, &cfg, sizeof cfg);   /* untouched */

    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG,
        clientControlApplyBlob(&cfg, "not json at all", strlen("not json at all")));
    TEST_ASSERT_EQUAL_MEMORY(&before, &cfg, sizeof cfg);
}

static void test_apply_blob_clamps_interval(void)
{
    clientConfig cfg;
    clientConfigDefaults(&cfg);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        clientControlApplyBlob(&cfg, "{\"sampleIntervalSec\":0}",
                               strlen("{\"sampleIntervalSec\":0}")));
    TEST_ASSERT_EQUAL_UINT32(1, cfg.sampleIntervalSec);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        clientControlApplyBlob(&cfg, "{\"sampleIntervalSec\":999999}",
                               strlen("{\"sampleIntervalSec\":999999}")));
    TEST_ASSERT_EQUAL_UINT32(86400, cfg.sampleIntervalSec);
}

/* ---- directive handling: apply + result round-trip ---- */

static void test_handle_set_config_applies_and_acks(void)
{
    clientConfig cfg;
    clientControlState st;
    clientControlOutcome oc;
    uint8_t dir[512], res[CLIENT_CTRL_RESULT_CAP];
    size_t dlen, rlen = 0;
    uint16_t tc = 0, rcount = 0;
    resultView view;

    clientConfigDefaults(&cfg);
    memset(&st, 0, sizeof st);
    st.appliedEpoch = 3;

    dlen = buildDirective(CTRL_SET_CONFIG, 7, SELF_NODE,
                          "{\"sampleIntervalSec\":20}", dir, sizeof dir, &tc);

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        clientControlHandle(&st, &cfg, SELF_NODE, dir, dlen,
                            res, sizeof res, &rlen, &rcount, &oc));
    TEST_ASSERT_TRUE(oc.wantReply);
    TEST_ASSERT_TRUE(oc.applied);
    TEST_ASSERT_EQUAL_UINT8(CTRL_SET_CONFIG, oc.verb);
    TEST_ASSERT_EQUAL_UINT32(20, cfg.sampleIntervalSec);
    TEST_ASSERT_EQUAL_UINT64(7, st.appliedEpoch);

    /* the reply parses exactly as serverControlOnResult expects */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, parseResult(res, rlen, &view));
    TEST_ASSERT_TRUE(view.haveVerb && view.haveErr && view.haveEpoch);
    TEST_ASSERT_EQUAL_UINT8(CTRL_SET_CONFIG, view.verb);
    TEST_ASSERT_EQUAL_UINT16(0, view.errCode);           /* success */
    TEST_ASSERT_EQUAL_UINT64(7, view.appliedEpoch);      /* convergence signal */
}

static void test_handle_epoch_monotonic(void)
{
    clientConfig cfg;
    clientControlState st;
    clientControlOutcome oc;
    uint8_t dir[512], res[CLIENT_CTRL_RESULT_CAP];
    size_t dlen, rlen = 0;
    uint16_t tc = 0, rcount = 0;
    resultView view;

    clientConfigDefaults(&cfg);
    cfg.sampleIntervalSec = 45;
    memset(&st, 0, sizeof st);
    st.appliedEpoch = 10;

    /* an older epoch must NOT re-apply, but must ack as converged */
    dlen = buildDirective(CTRL_PROVISION, 9, SELF_NODE,
                          "{\"sampleIntervalSec\":5}", dir, sizeof dir, &tc);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        clientControlHandle(&st, &cfg, SELF_NODE, dir, dlen,
                            res, sizeof res, &rlen, &rcount, &oc));
    TEST_ASSERT_TRUE(oc.wantReply);
    TEST_ASSERT_FALSE(oc.applied);
    TEST_ASSERT_EQUAL_UINT32(45, cfg.sampleIntervalSec);  /* no thrash */
    TEST_ASSERT_EQUAL_UINT64(10, st.appliedEpoch);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, parseResult(res, rlen, &view));
    TEST_ASSERT_EQUAL_UINT16(0, view.errCode);
    TEST_ASSERT_EQUAL_UINT64(10, view.appliedEpoch);

    /* the same epoch (idempotent re-provision) also just acks */
    dlen = buildDirective(CTRL_PROVISION, 10, SELF_NODE,
                          "{\"sampleIntervalSec\":5}", dir, sizeof dir, &tc);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        clientControlHandle(&st, &cfg, SELF_NODE, dir, dlen,
                            res, sizeof res, &rlen, &rcount, &oc));
    TEST_ASSERT_FALSE(oc.applied);
    TEST_ASSERT_EQUAL_UINT32(45, cfg.sampleIntervalSec);

    /* a NEWER epoch applies */
    dlen = buildDirective(CTRL_PROVISION, 11, SELF_NODE,
                          "{\"sampleIntervalSec\":5}", dir, sizeof dir, &tc);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        clientControlHandle(&st, &cfg, SELF_NODE, dir, dlen,
                            res, sizeof res, &rlen, &rcount, &oc));
    TEST_ASSERT_TRUE(oc.applied);
    TEST_ASSERT_EQUAL_UINT32(5, cfg.sampleIntervalSec);
    TEST_ASSERT_EQUAL_UINT64(11, st.appliedEpoch);
}

static void test_handle_addressing_filter(void)
{
    clientConfig cfg;
    clientControlState st;
    clientControlOutcome oc;
    uint8_t dir[512], res[CLIENT_CTRL_RESULT_CAP];
    size_t dlen, rlen = 0;
    uint16_t tc = 0, rcount = 0;

    clientConfigDefaults(&cfg);
    memset(&st, 0, sizeof st);

    /* addressed to ANOTHER node: silent drop, no reply, no apply */
    dlen = buildDirective(CTRL_SET_CONFIG, 5, SELF_NODE + 1,
                          "{\"sampleIntervalSec\":2}", dir, sizeof dir, &tc);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        clientControlHandle(&st, &cfg, SELF_NODE, dir, dlen,
                            res, sizeof res, &rlen, &rcount, &oc));
    TEST_ASSERT_FALSE(oc.wantReply);
    TEST_ASSERT_FALSE(oc.applied);
    TEST_ASSERT_EQUAL_UINT64(0, st.appliedEpoch);
    TEST_ASSERT_EQUAL_UINT32(15, cfg.sampleIntervalSec);  /* default kept */

    /* legacy broadcast (no addressee TLV) is acted on */
    dlen = buildDirective(CTRL_SET_CONFIG, 5, 0,
                          "{\"sampleIntervalSec\":2}", dir, sizeof dir, &tc);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        clientControlHandle(&st, &cfg, SELF_NODE, dir, dlen,
                            res, sizeof res, &rlen, &rcount, &oc));
    TEST_ASSERT_TRUE(oc.wantReply);
    TEST_ASSERT_TRUE(oc.applied);
    TEST_ASSERT_EQUAL_UINT32(2, cfg.sampleIntervalSec);
}

/* A present-but-malformed TLV_CTRL_TARGET_NODE (wrong length) must be rejected
 * whole — NOT silently defaulted to 0 (which would broadcast a corrupt
 * single-node directive to the entire fleet). Regression for the fail-open
 * addressing bug. */
static void test_handle_malformed_target_node_rejected(void)
{
    clientConfig cfg;
    clientControlState st;
    clientControlOutcome oc;
    solariTlvWriter w;
    uint8_t dir[256], res[CLIENT_CTRL_RESULT_CAP];
    size_t rlen = 0;
    uint16_t rcount = 0;
    uint32_t badNode = 0xDEADBEEF;   /* 4-byte value under a u64 tag */

    clientConfigDefaults(&cfg);
    memset(&st, 0, sizeof st);

    /* Hand-assemble verb + a 4-byte (wrong length) TARGET_NODE + a blob. */
    solariTlvWriterInit(&w, dir, sizeof dir);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvAppendU8(&w, TLV_CTRL_VERB, CTRL_SET_CONFIG));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvAppendU64(&w, TLV_CTRL_TARGET_EPOCH, 9));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariTlvAppend(&w, TLV_CTRL_TARGET_NODE, (const uint8_t *)&badNode, sizeof badNode));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariTlvAppend(&w, TLV_CTRL_PAYLOAD, (const uint8_t *)"{\"sampleIntervalSec\":2}", 23));

    /* Parse must fail closed → handle returns the parse error, applies nothing,
     * sends no reply (no trustworthy verb/epoch), keeps the default interval. */
    TEST_ASSERT_NOT_EQUAL(SOLARI_OK,
        clientControlHandle(&st, &cfg, SELF_NODE, dir, w.len,
                            res, sizeof res, &rlen, &rcount, &oc));
    TEST_ASSERT_FALSE(oc.applied);
    TEST_ASSERT_FALSE(oc.wantReply);
    TEST_ASSERT_EQUAL_UINT64(0, st.appliedEpoch);
    TEST_ASSERT_EQUAL_UINT32(15, cfg.sampleIntervalSec);
}

static void test_handle_bad_blob_reports_error(void)
{
    clientConfig cfg;
    clientControlState st;
    clientControlOutcome oc;
    uint8_t dir[512], res[CLIENT_CTRL_RESULT_CAP];
    size_t dlen, rlen = 0;
    uint16_t tc = 0, rcount = 0;
    resultView view;

    clientConfigDefaults(&cfg);
    memset(&st, 0, sizeof st);
    st.appliedEpoch = 1;

    dlen = buildDirective(CTRL_SET_CONFIG, 2, SELF_NODE,
                          "{broken", dir, sizeof dir, &tc);
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG,
        clientControlHandle(&st, &cfg, SELF_NODE, dir, dlen,
                            res, sizeof res, &rlen, &rcount, &oc));
    TEST_ASSERT_TRUE(oc.wantReply);                       /* server must learn */
    TEST_ASSERT_FALSE(oc.applied);
    TEST_ASSERT_EQUAL_UINT64(1, st.appliedEpoch);         /* epoch NOT advanced */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, parseResult(res, rlen, &view));
    TEST_ASSERT_EQUAL_UINT16((uint16_t)-ERR_INVALID_ARG, view.errCode);
    TEST_ASSERT_EQUAL_UINT64(1, view.appliedEpoch);
}

static void test_handle_unknown_verb_and_garbage(void)
{
    clientConfig cfg;
    clientControlState st;
    clientControlOutcome oc;
    uint8_t dir[512], res[CLIENT_CTRL_RESULT_CAP];
    uint8_t garbage[] = { 0xFF, 0x01, 0xFF, 0xFF, 0x00 };  /* truncated TLV */
    size_t dlen, rlen = 0;
    uint16_t tc = 0, rcount = 0;
    resultView view;

    clientConfigDefaults(&cfg);
    memset(&st, 0, sizeof st);

    /* an unhandled verb answers with an error result (server sees err:) */
    dlen = buildDirective(CTRL_RESTART, 4, SELF_NODE, NULL, dir, sizeof dir, &tc);
    TEST_ASSERT_EQUAL_INT(ERR_UNKNOWN_MSG,
        clientControlHandle(&st, &cfg, SELF_NODE, dir, dlen,
                            res, sizeof res, &rlen, &rcount, &oc));
    TEST_ASSERT_TRUE(oc.wantReply);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, parseResult(res, rlen, &view));
    TEST_ASSERT_EQUAL_UINT8(CTRL_RESTART, view.verb);
    TEST_ASSERT_TRUE(view.errCode != 0);

    /* malformed TLV soup off the wire: rejected without crash or reply */
    TEST_ASSERT_TRUE(
        clientControlHandle(&st, &cfg, SELF_NODE, garbage, sizeof garbage,
                            res, sizeof res, &rlen, &rcount, &oc) != SOLARI_OK);
    TEST_ASSERT_FALSE(oc.wantReply);
    TEST_ASSERT_EQUAL_UINT64(0, st.appliedEpoch);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_apply_blob_full);
    RUN_TEST(test_apply_blob_missing_keys_keep_current);
    RUN_TEST(test_apply_blob_bad_json_is_noop);
    RUN_TEST(test_apply_blob_clamps_interval);
    RUN_TEST(test_handle_set_config_applies_and_acks);
    RUN_TEST(test_handle_epoch_monotonic);
    RUN_TEST(test_handle_addressing_filter);
    RUN_TEST(test_handle_malformed_target_node_rejected);
    RUN_TEST(test_handle_bad_blob_reports_error);
    RUN_TEST(test_handle_unknown_verb_and_garbage);
    return UNITY_END();
}
