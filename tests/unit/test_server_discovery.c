/*
 * test_server_discovery.c - pure helpers of serverDiscovery.c (§7.1): source
 * code -> via/kind enum mapping, the '|'-delimited field splitter, and the
 * services JSON array builder (append + finalize).
 */
#include "unity.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "serverDiscovery.c"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_via_from_source(void)
{
    TEST_ASSERT_EQUAL_STRING("mdns",       discViaFromSource(1));
    TEST_ASSERT_EQUAL_STRING("arp",        discViaFromSource(2));
    TEST_ASSERT_EQUAL_STRING("scp_advert", discViaFromSource(3));
    TEST_ASSERT_EQUAL_STRING("portscan",   discViaFromSource(4));
    TEST_ASSERT_EQUAL_STRING("lldp",       discViaFromSource(5));
    TEST_ASSERT_EQUAL_STRING("scp_advert", discViaFromSource(99)); /* honest fallback */
}

static void test_kind_from_source(void)
{
    TEST_ASSERT_EQUAL_STRING("host",    discKindFromSource(1)); /* mDNS */
    TEST_ASSERT_EQUAL_STRING("host",    discKindFromSource(2)); /* ARP  */
    TEST_ASSERT_EQUAL_STRING("service", discKindFromSource(4)); /* portscan */
    TEST_ASSERT_EQUAL_STRING("host",    discKindFromSource(5)); /* LLDP */
}

static void test_split_field(void)
{
    const char *rec = "10.0.0.5|aa:bb:cc:dd:ee:ff|eth0";
    size_t rl = strlen(rec);
    char out[64];
    discSplitField(rec, rl, 0, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("10.0.0.5", out);
    discSplitField(rec, rl, 1, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("aa:bb:cc:dd:ee:ff", out);
    discSplitField(rec, rl, 2, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("eth0", out);
    discSplitField(rec, rl, 3, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_services_json(void)
{
    char body[256] = {0};
    size_t blen = 0;
    char out[300];

    /* empty body finalizes to "" */
    discServicesFinalize(body, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("", out);

    TEST_ASSERT_TRUE(discServiceAppend(body, sizeof body, &blen, "ssh", "22"));
    TEST_ASSERT_TRUE(discServiceAppend(body, sizeof body, &blen, "http", "80"));
    discServicesFinalize(body, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("[\"ssh:22\",\"http:80\"]", out);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_via_from_source);
    RUN_TEST(test_kind_from_source);
    RUN_TEST(test_split_field);
    RUN_TEST(test_services_json);
    return UNITY_END();
}
