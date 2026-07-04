/* test_server_topo_snmp.c - pure LLDP-over-SNMP walk parsing/decoding of
 * serverTopology.c: the lldpRem/lldpLoc -Onq line parsers, the MAC/id decoder,
 * the lldpRemSysCapEnabled BITS -> TOPO_CAP_* converter, the (localPortNum,
 * remIndex) merge/group, and end-to-end finalize -> topoNormalizeMibRecord. No
 * DB, no SNMP backend (SOLARI_WITH_SNMP need not be defined - only the always-
 * compiled pure helpers are exercised). */
#include "unity.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "serverTopology.c"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- line parsing ---- */

/* A lldpRemTable line: the OID tail is timeMark.localPortNum.remIndex. */
static void test_parse_rem_line(void)
{
    long lpn = 0, ri = 0;
    char val[128];

    TEST_ASSERT_EQUAL_INT(1, topoLldpParseRemLine(
        ".1.0.8802.1.1.2.1.4.1.1.9.12345.5.1 \"sw-core-01\"",
        &lpn, &ri, val, sizeof val));
    TEST_ASSERT_EQUAL_INT(5, (int)lpn);
    TEST_ASSERT_EQUAL_INT(1, (int)ri);
    TEST_ASSERT_EQUAL_STRING("sw-core-01", val);   /* dequoted */

    /* Hex-STRING (unquoted, space-separated) value survives intact. */
    TEST_ASSERT_EQUAL_INT(1, topoLldpParseRemLine(
        ".1.0.8802.1.1.2.1.4.1.1.5.99.7.3 aa bb cc dd ee ff",
        &lpn, &ri, val, sizeof val));
    TEST_ASSERT_EQUAL_INT(7, (int)lpn);
    TEST_ASSERT_EQUAL_INT(3, (int)ri);
    TEST_ASSERT_EQUAL_STRING("aa bb cc dd ee ff", val);

    /* Malformed (no space / too few labels) is rejected. */
    TEST_ASSERT_EQUAL_INT(0, topoLldpParseRemLine("junk", &lpn, &ri, val, sizeof val));
    TEST_ASSERT_EQUAL_INT(0, topoLldpParseRemLine(".1.2 x", &lpn, &ri, val, sizeof val));
}

/* A lldpLocPortId line: one trailing label (lldpLocPortNum). */
static void test_parse_loc_line(void)
{
    long pn = 0;
    char val[64];
    TEST_ASSERT_EQUAL_INT(1, topoLldpParseLocLine(
        ".1.0.8802.1.1.2.1.3.7.1.3.5 \"eth3\"", &pn, val, sizeof val));
    TEST_ASSERT_EQUAL_INT(5, (int)pn);
    TEST_ASSERT_EQUAL_STRING("eth3", val);
}

/* ---- id decoding ---- */

static void test_format_mac(void)
{
    char out[32];
    topoLldpFormatMac("aa bb cc dd ee ff", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("aa:bb:cc:dd:ee:ff", out);

    topoLldpFormatMac("AABBCCDDEEFF", out, sizeof out);   /* packed, uppercase */
    TEST_ASSERT_EQUAL_STRING("aa:bb:cc:dd:ee:ff", out);

    topoLldpFormatMac("0x0011223344", out, sizeof out);   /* 0x prefix, 5 bytes */
    TEST_ASSERT_EQUAL_STRING("00:11:22:33:44", out);
}

static void test_decode_id(void)
{
    char out[64];
    /* MAC subtype -> colonated hex. */
    topoLldpDecodeId("aa bb cc dd ee ff", true, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("aa:bb:cc:dd:ee:ff", out);
    /* non-MAC subtype -> printable verbatim. */
    topoLldpDecodeId("Gi1/0/24", false, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("Gi1/0/24", out);
}

/* ---- capability BITS -> internal bitmap ---- */

static void test_caps_to_bits(void)
{
    /* bridge = LLDP cap #2 = MSB-2 of octet0 = 0x20 -> internal 0x04. */
    TEST_ASSERT_EQUAL_INT(TOPO_CAP_BRIDGE, topoLldpCapsToBits("20 00"));
    /* bridge + wlanAP (cap #3 = 0x10) = 0x30 -> 0x04|0x08. */
    TEST_ASSERT_EQUAL_INT(TOPO_CAP_BRIDGE | TOPO_CAP_WLAN_AP,
                          topoLldpCapsToBits("30 00"));
    /* router = cap #4 = 0x08 in octet0 -> internal 0x10. */
    TEST_ASSERT_EQUAL_INT(TOPO_CAP_ROUTER, topoLldpCapsToBits("08 00"));
    /* packed / 0x-prefixed forms parse identically. */
    TEST_ASSERT_EQUAL_INT(TOPO_CAP_BRIDGE, topoLldpCapsToBits("0x2000"));
    TEST_ASSERT_EQUAL_INT(0, topoLldpCapsToBits(""));
}

/* A bridge+wlan bitstring resolves to an "ap" via topoInferGearKind. */
static void test_caps_bits_to_kind(void)
{
    topoLldpMibRecord r;
    memset(&r, 0, sizeof r);
    r.capabilities = topoLldpCapsToBits("30 00");   /* bridge + wlanAP */
    TEST_ASSERT_EQUAL_STRING("ap", topoInferGearKind(&r));
}

/* ---- merge grouping ---- */

static void test_merge_group(void)
{
    topoLldpRawNeigh rows[8];
    size_t count = 0;

    /* Two columns of the SAME (localPortNum=5, remIndex=1) collapse to one row. */
    topoLldpMergeRecord(rows, &count, 8, TOPO_LLDP_COL_CHASSIS_ID, 5, 1, "aa bb cc");
    topoLldpMergeRecord(rows, &count, 8, TOPO_LLDP_COL_SYS_NAME,   5, 1, "sw-core-01");
    /* A different remIndex opens a second row. */
    topoLldpMergeRecord(rows, &count, 8, TOPO_LLDP_COL_SYS_NAME,   5, 2, "sw-edge-02");

    TEST_ASSERT_EQUAL_UINT(2, count);
    TEST_ASSERT_EQUAL_STRING("aa bb cc", rows[0].rawChassisId);
    TEST_ASSERT_EQUAL_STRING("sw-core-01", rows[0].sysName);
    TEST_ASSERT_EQUAL_STRING("sw-edge-02", rows[1].sysName);
}

/* ---- finalize + normalize end-to-end ---- */

static void test_finalize_and_normalize(void)
{
    topoLldpRawNeigh rows[4];
    topoLldpLocPort  ports[4];
    topoLldpMibRecord recs[4];
    size_t rc = 0, pc = 0, nrec;
    serverNetGear g;
    serverLldpEdge e;

    /* Assemble one neighbour's columns at (localPortNum=5, remIndex=1). */
    topoLldpMergeRecord(rows, &rc, 4, TOPO_LLDP_COL_CHASSIS_ID,  5, 1, "aa bb cc dd ee ff");
    topoLldpMergeRecord(rows, &rc, 4, TOPO_LLDP_COL_CHASSIS_SUB, 5, 1, "4");   /* MAC */
    topoLldpMergeRecord(rows, &rc, 4, TOPO_LLDP_COL_PORT_ID,     5, 1, "Gi1/0/24");
    topoLldpMergeRecord(rows, &rc, 4, TOPO_LLDP_COL_PORT_SUB,    5, 1, "5");   /* ifName */
    topoLldpMergeRecord(rows, &rc, 4, TOPO_LLDP_COL_SYS_NAME,    5, 1, "sw-core-01");
    topoLldpMergeRecord(rows, &rc, 4, TOPO_LLDP_COL_SYS_DESC,    5, 1, "Cisco IOS Switch");
    topoLldpMergeRecord(rows, &rc, 4, TOPO_LLDP_COL_CAP_ENABLED, 5, 1, "20 00"); /* bridge */

    /* local port 5 -> "eth3" resolves record.localIf. */
    topoLldpMergeLocPort(ports, &pc, 4, 5, "eth3");

    nrec = topoLldpFinalize(rows, rc, ports, pc, recs, 4);
    TEST_ASSERT_EQUAL_UINT(1, nrec);
    TEST_ASSERT_EQUAL_STRING("aa:bb:cc:dd:ee:ff", recs[0].chassisId);
    TEST_ASSERT_EQUAL_STRING("Gi1/0/24", recs[0].portId);
    TEST_ASSERT_EQUAL_STRING("sw-core-01", recs[0].sysName);
    TEST_ASSERT_EQUAL_STRING("eth3", recs[0].localIf);
    TEST_ASSERT_EQUAL_INT(TOPO_CAP_BRIDGE, recs[0].capabilities);
    TEST_ASSERT_FALSE(recs[0].wireless);
    TEST_ASSERT_EQUAL_INT(0, recs[0].speedMbps);

    /* The finalized record feeds the tested normalizer to schema rows. */
    TEST_ASSERT_TRUE(topoNormalizeMibRecord(&recs[0], 0x42ULL, "seg-x", &g, &e));
    TEST_ASSERT_EQUAL_STRING("sw-sw-core-01", g.gearId);   /* sysName-seeded */
    TEST_ASSERT_EQUAL_STRING("switch", g.kind);
    TEST_ASSERT_EQUAL_STRING("Cisco IOS Switch", g.model);
    TEST_ASSERT_EQUAL_STRING("eth3", e.localIf);
    TEST_ASSERT_EQUAL_STRING("Gi1/0/24", e.peerPort);
    TEST_ASSERT_TRUE(e.viaLldp);
}

/* A wireless AP: caps set wireless + the edge is a wireless link. */
static void test_finalize_wireless_ap(void)
{
    topoLldpRawNeigh rows[2];
    topoLldpMibRecord recs[2];
    size_t rc = 0, nrec;
    serverNetGear g;
    serverLldpEdge e;

    topoLldpMergeRecord(rows, &rc, 2, TOPO_LLDP_COL_SYS_NAME,    3, 1, "unifi-ap");
    topoLldpMergeRecord(rows, &rc, 2, TOPO_LLDP_COL_CAP_ENABLED, 3, 1, "30 00"); /* bridge+wlan */

    nrec = topoLldpFinalize(rows, rc, NULL, 0, recs, 2);
    TEST_ASSERT_EQUAL_UINT(1, nrec);
    TEST_ASSERT_TRUE(recs[0].wireless);
    TEST_ASSERT_EQUAL_STRING("", recs[0].localIf);   /* no loc-port map */

    TEST_ASSERT_TRUE(topoNormalizeMibRecord(&recs[0], 1, "", &g, &e));
    TEST_ASSERT_EQUAL_STRING("ap", g.kind);
    TEST_ASSERT_TRUE(g.wireless);
    TEST_ASSERT_EQUAL_STRING("wireless", e.linkType);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_rem_line);
    RUN_TEST(test_parse_loc_line);
    RUN_TEST(test_format_mac);
    RUN_TEST(test_decode_id);
    RUN_TEST(test_caps_to_bits);
    RUN_TEST(test_caps_bits_to_kind);
    RUN_TEST(test_merge_group);
    RUN_TEST(test_finalize_and_normalize);
    RUN_TEST(test_finalize_wireless_ap);
    return UNITY_END();
}
