/*
 * test_server_db.c - pure helpers of serverDb.c (no live MariaDB needed):
 * probe targetId synthesis, IPv4-mapped/IPv6 address formatting, enum<->string
 * round trips (role/outcome/proto/enrollStatus), pool-size clamping, and the
 * derived host-health helpers (avg CPU milli, min disk free %).
 *
 * The whole serverDb.c TU is #included so its static helpers are reachable; the
 * mariadb client headers it pulls in compile fine and no DB connection is made.
 */
#include "unity.h"

/* serverDb.c needs gethostname/strtoull-style POSIX surface under -std=c99. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "serverDb.c"   /* brings the static helpers under test */

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- enum <-> string round trips ---- */

static void test_role_str_parse_roundtrip(void)
{
    TEST_ASSERT_EQUAL_STRING("client",  dbRoleStr(ROLE_CLIENT));
    TEST_ASSERT_EQUAL_STRING("monitor", dbRoleStr(ROLE_MONITOR));
    TEST_ASSERT_EQUAL_STRING("server",  dbRoleStr(ROLE_SERVER));

    TEST_ASSERT_EQUAL_INT(ROLE_CLIENT,  dbRoleParse("client"));
    TEST_ASSERT_EQUAL_INT(ROLE_MONITOR, dbRoleParse("monitor"));
    TEST_ASSERT_EQUAL_INT(ROLE_SERVER,  dbRoleParse("server"));
    /* unknown / NULL default to client */
    TEST_ASSERT_EQUAL_INT(ROLE_CLIENT,  dbRoleParse("bogus"));
    TEST_ASSERT_EQUAL_INT(ROLE_CLIENT,  dbRoleParse(NULL));
}

static void test_enroll_status_roundtrip(void)
{
    serverEnrollStatus all[] = { ENROLL_TOKEN, ENROLL_PENDING, ENROLL_APPROVED,
                                 ENROLL_REJECTED, ENROLL_EXPIRED };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        const char *s = dbEnrollStatusStr(all[i]);
        TEST_ASSERT_EQUAL_INT(all[i], dbEnrollStatusParse(s));
    }
    TEST_ASSERT_EQUAL_STRING("pending", dbEnrollStatusStr(ENROLL_PENDING));
    TEST_ASSERT_EQUAL_STRING("approved", dbEnrollStatusStr(ENROLL_APPROVED));
    /* unknown string -> token (safe default) */
    TEST_ASSERT_EQUAL_INT(ENROLL_TOKEN, dbEnrollStatusParse("nonsense"));
    TEST_ASSERT_EQUAL_INT(ENROLL_TOKEN, dbEnrollStatusParse(NULL));
}

static void test_outcome_and_proto_str(void)
{
    TEST_ASSERT_EQUAL_STRING("ok",          dbOutcomeStr(0));
    TEST_ASSERT_EQUAL_STRING("timeout",     dbOutcomeStr(1));
    TEST_ASSERT_EQUAL_STRING("unreachable", dbOutcomeStr(3));
    TEST_ASSERT_EQUAL_STRING("tls_fail",    dbOutcomeStr(5));
    TEST_ASSERT_EQUAL_STRING("proto_err",   dbOutcomeStr(99)); /* out of range */

    TEST_ASSERT_EQUAL_STRING("icmp", dbProtoStr(1));
    TEST_ASSERT_EQUAL_STRING("tcp",  dbProtoStr(2));
    TEST_ASSERT_EQUAL_STRING("udp",  dbProtoStr(3));
    TEST_ASSERT_EQUAL_STRING("tcp",  dbProtoStr(7)); /* unknown -> tcp */
}

/* ---- address formatting ---- */

static void test_format_probe_addr(void)
{
    char buf[SERVER_IP_MAX];
    uint8_t v4[16] = {0,0,0,0, 0,0,0,0, 0,0,0xff,0xff, 192,168,1,42};
    uint8_t zero[16] = {0};
    uint8_t v6[16] = {0x20,0x01,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,1};

    dbFormatProbeAddr(v4, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("192.168.1.42", buf);

    dbFormatProbeAddr(zero, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("0.0.0.0", buf);

    dbFormatProbeAddr(v6, buf, sizeof buf);
    /* leading group 2001:0db8: ... ::1 */
    TEST_ASSERT_EQUAL_STRING("2001:0db8:0000:0000:0000:0000:0000:0001", buf);
}

/* ---- probe targetId synthesis "proto:host:port" / "icmp:host" ---- */

static void test_probe_target_id(void)
{
    char buf[SERVER_TARGETID_MAX];
    solariProbeResult p;

    /* TCP to an IPv4-mapped address with a port */
    memset(&p, 0, sizeof p);
    p.proto = 2; p.dstPort = 443;
    p.dstAddr[10] = 0xff; p.dstAddr[11] = 0xff;
    p.dstAddr[12] = 10; p.dstAddr[13] = 0; p.dstAddr[14] = 0; p.dstAddr[15] = 5;
    dbProbeTargetId(&p, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("tcp:10.0.0.5:443", buf);

    /* ICMP omits the port */
    memset(&p, 0, sizeof p);
    p.proto = 1; p.dstPort = 0;
    p.dstAddr[10] = 0xff; p.dstAddr[11] = 0xff;
    p.dstAddr[12] = 1; p.dstAddr[13] = 1; p.dstAddr[14] = 1; p.dstAddr[15] = 1;
    dbProbeTargetId(&p, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("icmp:1.1.1.1", buf);

    /* UDP */
    memset(&p, 0, sizeof p);
    p.proto = 3; p.dstPort = 53;
    p.dstAddr[10] = 0xff; p.dstAddr[11] = 0xff;
    p.dstAddr[12] = 8; p.dstAddr[13] = 8; p.dstAddr[14] = 8; p.dstAddr[15] = 8;
    dbProbeTargetId(&p, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("udp:8.8.8.8:53", buf);
}

static void test_alert_id_conversion(void)
{
    char out[SERVER_TARGETID_MAX], round[SERVER_TARGETID_MAX];
    TEST_ASSERT_TRUE(dbNumericAlertIdToProbeId("1:2001:db8::1", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("icmp:2001:db8::1", out);
    TEST_ASSERT_TRUE(dbProbeIdToNumericAlertId(out, round, sizeof round));
    TEST_ASSERT_EQUAL_STRING("1:2001:db8::1", round);
    TEST_ASSERT_TRUE(dbNumericAlertIdToProbeId("2:10.0.0.1:443", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("tcp:10.0.0.1:443", out);
    TEST_ASSERT_TRUE(dbNumericAlertIdToProbeId("3:2001:db8::2:53", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("udp:2001:db8::2:53", out);
    TEST_ASSERT_FALSE(dbNumericAlertIdToProbeId("x:bad", out, sizeof out));
    TEST_ASSERT_FALSE(dbProbeIdToNumericAlertId("bogus:host", out, sizeof out));
}

/* ---- pool-size clamp ---- */

static void test_pool_clamp(void)
{
    TEST_ASSERT_EQUAL_UINT8(1,  dbClampPoolSize(0));   /* floor */
    TEST_ASSERT_EQUAL_UINT8(4,  dbClampPoolSize(4));
    TEST_ASSERT_EQUAL_UINT8(16, dbClampPoolSize(16));
    TEST_ASSERT_EQUAL_UINT8(16, dbClampPoolSize(255)); /* ceiling */
}

/* ---- derived host health ---- */

static void test_avg_cpu_and_min_disk(void)
{
    solariClientReport rep;
    memset(&rep, 0, sizeof rep);

    /* no cores -> 0 */
    TEST_ASSERT_EQUAL_UINT(0, dbAvgCpuMilli(&rep));

    rep.coreCount = 4;
    rep.cpuLoadMilli[0] = 100; rep.cpuLoadMilli[1] = 200;
    rep.cpuLoadMilli[2] = 300; rep.cpuLoadMilli[3] = 400;
    TEST_ASSERT_EQUAL_UINT(250, dbAvgCpuMilli(&rep)); /* mean of 100..400 */

    /* no disks -> 100% free */
    memset(&rep, 0, sizeof rep);
    TEST_ASSERT_EQUAL_INT(100, dbMinDiskFreePct(&rep));

    rep.diskCount = 2;
    rep.disks[0].totalKb = 1000; rep.disks[0].freeKb = 500; /* 50% */
    rep.disks[1].totalKb = 1000; rep.disks[1].freeKb = 100; /* 10% (min) */
    TEST_ASSERT_EQUAL_INT(10, dbMinDiskFreePct(&rep));

    /* a zero-total mount is skipped, not divided-by-zero */
    rep.diskCount = 3;
    rep.disks[2].totalKb = 0; rep.disks[2].freeKb = 0;
    TEST_ASSERT_EQUAL_INT(10, dbMinDiskFreePct(&rep));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_role_str_parse_roundtrip);
    RUN_TEST(test_enroll_status_roundtrip);
    RUN_TEST(test_outcome_and_proto_str);
    RUN_TEST(test_format_probe_addr);
    RUN_TEST(test_probe_target_id);
    RUN_TEST(test_alert_id_conversion);
    RUN_TEST(test_pool_clamp);
    RUN_TEST(test_avg_cpu_and_min_disk);
    return UNITY_END();
}
