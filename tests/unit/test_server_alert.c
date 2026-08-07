/*
 * test_server_alert.c - pure rule-evaluation logic of serverAlert.c (§9.1/§10):
 * threshold ops (gt/lt/eq), severity-code mapping, host + probe metric name
 * resolution, the per-probe targetId, and the breach-state table find/create +
 * recycle behaviour. No DB / PUB socket.
 */
#include "unity.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "serverAlert.c"

#include <string.h>

void setUp(void) { memset(gAlertState, 0, sizeof gAlertState); }
void tearDown(void) {}

/* ---- threshold ops ---- */

static void test_threshold_ops(void)
{
    TEST_ASSERT_TRUE (alertThresholdBreached("gt", 10.0, 5.0));
    TEST_ASSERT_FALSE(alertThresholdBreached("gt", 5.0, 5.0));   /* strict */
    TEST_ASSERT_TRUE (alertThresholdBreached("lt", 1.0, 5.0));
    TEST_ASSERT_FALSE(alertThresholdBreached("lt", 5.0, 5.0));
    TEST_ASSERT_TRUE (alertThresholdBreached("eq", 5.0, 5.0));
    TEST_ASSERT_TRUE (alertThresholdBreached("eq", 5.2, 5.0));   /* within epsilon */
    TEST_ASSERT_FALSE(alertThresholdBreached("eq", 6.0, 5.0));
    /* transition / unknown ops are never a scalar breach */
    TEST_ASSERT_FALSE(alertThresholdBreached("transition", 100.0, 0.0));
    TEST_ASSERT_FALSE(alertThresholdBreached("bogus", 100.0, 0.0));
}

static void test_severity_code(void)
{
    TEST_ASSERT_EQUAL_UINT16(ALERT_CODE_CRIT, alertSeverityCode("crit"));
    TEST_ASSERT_EQUAL_UINT16(ALERT_CODE_WARN, alertSeverityCode("warn"));
    TEST_ASSERT_EQUAL_UINT16(ALERT_CODE_INFO, alertSeverityCode("info"));
    TEST_ASSERT_EQUAL_UINT16(ALERT_CODE_INFO, alertSeverityCode("unknown")); /* default */
}

static void test_tier_composition(void)
{
    const char *s = NULL;
    TEST_ASSERT_FALSE(alertComposeSeverity("crit", 0, &s));
    TEST_ASSERT_TRUE(alertComposeSeverity("crit", 1, &s)); TEST_ASSERT_EQUAL_STRING("warn", s);
    TEST_ASSERT_TRUE(alertComposeSeverity("warn", 2, &s)); TEST_ASSERT_EQUAL_STRING("warn", s);
    TEST_ASSERT_TRUE(alertComposeSeverity("crit", 3, &s)); TEST_ASSERT_EQUAL_STRING("crit", s);
    TEST_ASSERT_TRUE(alertComposeSeverity("info", 4, &s)); TEST_ASSERT_EQUAL_STRING("crit", s);
}

/* ---- host metric resolution ---- */

static void test_client_metrics(void)
{
    solariClientReport rep;
    double v = -1;
    memset(&rep, 0, sizeof rep);
    rep.coreCount = 2; rep.cpuLoadMilli[0] = 400; rep.cpuLoadMilli[1] = 600;
    rep.ramUsedKb = 750; rep.ramTotalKb = 1000;
    rep.swapUsedKb = 0;  rep.swapTotalKb = 0;
    rep.diskCount = 2;
    rep.disks[0].freeKb = 900; rep.disks[1].freeKb = 50;
    rep.procCount = 7; rep.ifaceCount = 3;

    /* metrics are integral counters widened to double; compare exactly as ints
     * (Unity is built without double-assertion support). */
    TEST_ASSERT_TRUE(alertClientMetric(&rep, "cpuAvgMilli", &v));
    TEST_ASSERT_EQUAL_INT(500, (int)v);
    TEST_ASSERT_TRUE(alertClientMetric(&rep, "ramUsedPct", &v));
    TEST_ASSERT_EQUAL_INT(75, (int)v);
    /* swapUsedPct with zero total must not divide by zero */
    TEST_ASSERT_TRUE(alertClientMetric(&rep, "swapUsedPct", &v));
    TEST_ASSERT_EQUAL_INT(0, (int)v);
    /* diskFreeMinKb = worst mount */
    TEST_ASSERT_TRUE(alertClientMetric(&rep, "diskFreeMinKb", &v));
    TEST_ASSERT_EQUAL_INT(50, (int)v);
    TEST_ASSERT_TRUE(alertClientMetric(&rep, "procCount", &v));
    TEST_ASSERT_EQUAL_INT(7, (int)v);
    /* an unknown metric is rejected so the rule is skipped */
    TEST_ASSERT_FALSE(alertClientMetric(&rep, "bogusMetric", &v));
}

/* ---- probe metric resolution ---- */

static void test_probe_metrics(void)
{
    solariProbeResult p;
    double v = -1;
    memset(&p, 0, sizeof p);
    p.lossPermille = 120; p.rttMicros = 2500; p.jitterMicros = 30;
    p.throughputKbps = 9000; p.outcome = 0;

    TEST_ASSERT_TRUE(alertProbeMetric(&p, "lossPermille", &v));
    TEST_ASSERT_EQUAL_INT(120, (int)v);
    TEST_ASSERT_TRUE(alertProbeMetric(&p, "rttMs", &v));
    TEST_ASSERT_EQUAL_INT(25, (int)(v * 10.0 + 0.5));   /* 2.5 ms -> 25 tenths */
    TEST_ASSERT_TRUE(alertProbeMetric(&p, "reachable", &v));
    TEST_ASSERT_EQUAL_INT(1, (int)v);   /* outcome 0 == ok == reachable */
    p.outcome = 1;
    TEST_ASSERT_TRUE(alertProbeMetric(&p, "reachable", &v));
    TEST_ASSERT_EQUAL_INT(0, (int)v);
    TEST_ASSERT_FALSE(alertProbeMetric(&p, "nope", &v));
}

static void test_probe_target_id(void)
{
    char buf[SERVER_TARGETID_MAX];
    solariProbeResult p;
    memset(&p, 0, sizeof p);
    p.proto = 2; p.dstPort = 8080;
    p.dstAddr[10] = 0xff; p.dstAddr[11] = 0xff;
    p.dstAddr[12] = 172; p.dstAddr[13] = 16; p.dstAddr[14] = 0; p.dstAddr[15] = 1;
    alertProbeTargetId(&p, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("2:172.16.0.1:8080", buf);
}

/* ---- breach-state table ---- */

static void test_state_find_create(void)
{
    /* not present, no-create -> NULL */
    TEST_ASSERT_NULL(alertStateFind(1, 0xAA, "t1", false));

    /* create allocates a slot */
    alertBreachState *s = alertStateFind(1, 0xAA, "t1", true);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(s->inUse);
    TEST_ASSERT_EQUAL_INT(1, s->ruleId);

    /* a second lookup with the same key returns the SAME slot */
    s->fired = true; s->breachSinceMs = 12345;
    alertBreachState *again = alertStateFind(1, 0xAA, "t1", false);
    TEST_ASSERT_EQUAL_PTR(s, again);
    TEST_ASSERT_TRUE(again->fired);
    TEST_ASSERT_EQUAL_UINT64(12345, again->breachSinceMs);

    /* a different key is a different slot */
    alertBreachState *other = alertStateFind(2, 0xAA, "t1", true);
    TEST_ASSERT_NOT_NULL(other);
    TEST_ASSERT_NOT_EQUAL(s, other);

    /* host-scope (NULL targetId) is keyed as "" and is stable */
    alertBreachState *h1 = alertStateFind(9, 0xBB, NULL, true);
    alertBreachState *h2 = alertStateFind(9, 0xBB, "", false);
    TEST_ASSERT_EQUAL_PTR(h1, h2);
}

static void test_drop_target_state_is_scoped(void)
{
    alertBreachState *a = alertStateFind(1, 0xAA, "target-a", true);
    alertBreachState *b = alertStateFind(2, 0xBB, "target-b", true);
    TEST_ASSERT_NOT_NULL(a); TEST_ASSERT_NOT_NULL(b);
    a->fired = true; a->breachSinceMs = 111; a->haveLast = true;
    b->fired = true; b->breachSinceMs = 222; b->haveLast = true;
    serverAlertDropTargetState("target-a");
    TEST_ASSERT_FALSE(a->inUse);
    TEST_ASSERT_TRUE(b->inUse); TEST_ASSERT_TRUE(b->fired);
    TEST_ASSERT_EQUAL_UINT64(222, b->breachSinceMs); TEST_ASSERT_TRUE(b->haveLast);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_threshold_ops);
    RUN_TEST(test_severity_code);
    RUN_TEST(test_tier_composition);
    RUN_TEST(test_client_metrics);
    RUN_TEST(test_probe_metrics);
    RUN_TEST(test_probe_target_id);
    RUN_TEST(test_state_find_create);
    RUN_TEST(test_drop_target_state_is_scoped);
    return UNITY_END();
}
