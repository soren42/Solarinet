/* test_server_snmp.c - pure IF-MIB walk parsing/merge (serverSnmp.c). No DB,
 * no SNMP backend needed; exercises the normalization the poller relies on. */
#include "unity.h"
#include "serverSnmp.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* A numeric-OID counter line parses to (ifIndex, value). */
static void test_parse_counter_line(void)
{
    int idx = 0;
    char val[64];
    TEST_ASSERT_EQUAL_INT(1, serverSnmpParseWalkLine(
        ".1.3.6.1.2.1.31.1.1.1.6.2 30347517242", &idx, val, sizeof val));
    TEST_ASSERT_EQUAL_INT(2, idx);
    TEST_ASSERT_EQUAL_STRING("30347517242", val);
}

/* A quoted string value (ifName) is de-quoted; ifIndex is the trailing label. */
static void test_parse_string_line(void)
{
    int idx = 0;
    char val[64];
    TEST_ASSERT_EQUAL_INT(1, serverSnmpParseWalkLine(
        ".1.3.6.1.2.1.31.1.1.1.1.3 \"enp1s0\"", &idx, val, sizeof val));
    TEST_ASSERT_EQUAL_INT(3, idx);
    TEST_ASSERT_EQUAL_STRING("enp1s0", val);
}

/* Malformed lines (no space, no OID) are rejected. */
static void test_parse_rejects_garbage(void)
{
    int idx = 0;
    char val[64];
    TEST_ASSERT_EQUAL_INT(0, serverSnmpParseWalkLine("nonsense", &idx, val, sizeof val));
    TEST_ASSERT_EQUAL_INT(0, serverSnmpParseWalkLine("", &idx, val, sizeof val));
}

/* Walk parsing fails closed rather than accepting malformed indices or
 * silently truncating a value that would corrupt an interface sample. */
static void test_parse_rejects_invalid_index_and_truncation(void)
{
    int idx = 99;
    char val[5];
    TEST_ASSERT_EQUAL_INT(0, serverSnmpParseWalkLine("prefix.2 1", &idx, val, sizeof val));
    TEST_ASSERT_EQUAL_INT(0, idx);
    TEST_ASSERT_EQUAL_INT(0, serverSnmpParseWalkLine(".1.3.6.1.2.1.2.2.1.8.999999999999 1", &idx, val, sizeof val));
    TEST_ASSERT_EQUAL_INT(0, serverSnmpParseWalkLine(".1.3.6.1.2.1.2.2.1.8.2 \"longer\"", &idx, val, sizeof val));
    TEST_ASSERT_EQUAL_INT(0, serverSnmpParseWalkLine(".1.3.6.1.2.1.2.2.1.8.2 \"unclosed", &idx, val, sizeof val));
}

/* Merging the four OID kinds by ifIndex builds one iface record per index. */
static void test_merge_by_ifindex(void)
{
    serverSnmpIface ifaces[8];
    size_t count = 0;

    serverSnmpMergeIface(ifaces, &count, 8, SNMP_OID_NAME,   2, "enp1s0");
    serverSnmpMergeIface(ifaces, &count, 8, SNMP_OID_IN,     2, "1000");
    serverSnmpMergeIface(ifaces, &count, 8, SNMP_OID_OUT,    2, "2000");
    serverSnmpMergeIface(ifaces, &count, 8, SNMP_OID_STATUS, 2, "1");
    serverSnmpMergeIface(ifaces, &count, 8, SNMP_OID_NAME,   3, "docker0");
    serverSnmpMergeIface(ifaces, &count, 8, SNMP_OID_STATUS, 3, "2");

    TEST_ASSERT_EQUAL_UINT(2, count);                 /* two distinct ifIndexes */
    TEST_ASSERT_EQUAL_STRING("enp1s0", ifaces[0].name);
    TEST_ASSERT_EQUAL_UINT64(1000, ifaces[0].inOctets);
    TEST_ASSERT_EQUAL_UINT64(2000, ifaces[0].outOctets);
    TEST_ASSERT_EQUAL_INT(1, ifaces[0].operStatus);
    TEST_ASSERT_EQUAL_STRING("docker0", ifaces[1].name);
    TEST_ASSERT_EQUAL_INT(2, ifaces[1].operStatus);
}

/* Invalid numeric values and out-of-range indices never enter the merged set. */
static void test_merge_rejects_bad_values_and_bounds(void)
{
    serverSnmpIface ifaces[1];
    char longName[SERVER_SNMP_MAX_IFACES];
    size_t count = 0;
    memset(longName, 'x', sizeof longName - 1);
    longName[sizeof longName - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(0, serverSnmpMergeIface(ifaces, &count, 1, SNMP_OID_IN, 1,
                                                  "18446744073709551616"));
    TEST_ASSERT_EQUAL_INT(0, serverSnmpMergeIface(ifaces, &count, 1, SNMP_OID_OUT, 1, "-1"));
    TEST_ASSERT_EQUAL_INT(0, serverSnmpMergeIface(ifaces, &count, 1, SNMP_OID_STATUS, 1, "2x"));
    TEST_ASSERT_EQUAL_INT(0, serverSnmpMergeIface(ifaces, &count, 1, SNMP_OID_STATUS, 1, "0"));
    TEST_ASSERT_EQUAL_INT(0, serverSnmpMergeIface(ifaces, &count, 1, SNMP_OID_STATUS, 1, "8"));
    TEST_ASSERT_EQUAL_INT(0, serverSnmpMergeIface(ifaces, &count, 1, SNMP_OID_NAME, 1, longName));
    TEST_ASSERT_EQUAL_INT(0, serverSnmpMergeIface(ifaces, &count, 1, SNMP_OID_IN, 0, "1"));
    TEST_ASSERT_EQUAL_INT(0, count);
    TEST_ASSERT_EQUAL_INT(0, serverSnmpMergeIface(ifaces, &count, 0, SNMP_OID_IN, 1, "1"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_counter_line);
    RUN_TEST(test_parse_string_line);
    RUN_TEST(test_parse_rejects_garbage);
    RUN_TEST(test_parse_rejects_invalid_index_and_truncation);
    RUN_TEST(test_merge_by_ifindex);
    RUN_TEST(test_merge_rejects_bad_values_and_bounds);
    return UNITY_END();
}
