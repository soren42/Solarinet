/*
 * test_server_topology.c - pure parsing/derivation helpers of serverTopology.c
 * (§7.2): the '|'-delimited field splitter, decimal-int parse with clamping, and
 * the schema-legal gearId/segId derivation ([a-z0-9-], collapsed runs, trimmed).
 */
#include "unity.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "serverTopology.c"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_split_field(void)
{
    const char *rec = "chassisA|Gi0/1|eth0";
    size_t rl = strlen(rec);
    char out[64];

    TEST_ASSERT_EQUAL_UINT(8, topoSplitField(rec, rl, 0, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("chassisA", out);
    topoSplitField(rec, rl, 1, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("Gi0/1", out);
    topoSplitField(rec, rl, 2, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("eth0", out);

    /* field beyond the record -> "" */
    topoSplitField(rec, rl, 5, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("", out);

    /* an empty middle field */
    const char *rec2 = "a||c";
    topoSplitField(rec2, strlen(rec2), 1, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_parse_int(void)
{
    int v = -1;
    TEST_ASSERT_TRUE(topoParseInt("1000", &v));
    TEST_ASSERT_EQUAL_INT(1000, v);
    TEST_ASSERT_TRUE(topoParseInt("  42abc", &v)); /* leading space, trailing junk */
    TEST_ASSERT_EQUAL_INT(42, v);
    TEST_ASSERT_FALSE(topoParseInt("notanumber", &v));
    TEST_ASSERT_EQUAL_INT(0, v);                   /* false -> 0 */
    /* clamp guards against absurd values */
    TEST_ASSERT_TRUE(topoParseInt("99999999999", &v));
    TEST_ASSERT_EQUAL_INT(1000000000, v);
}

static void test_derive_gear_id(void)
{
    char out[SERVER_GEARID_MAX];

    topoDeriveGearId("gw-", "192.168.1.1", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("gw-192-168-1-1", out);

    topoDeriveGearId("sw-", "aa:BB:cc:DD:ee:FF", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("sw-aa-bb-cc-dd-ee-ff", out);   /* lowercased */

    /* trailing punctuation trimmed, runs collapsed */
    topoDeriveGearId("gw-", "10.0.0.0/", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("gw-10-0-0-0", out);

    /* empty seed -> just the trimmed prefix (prefix has no trailing dash kept) */
    topoDeriveGearId("gw-", "", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("gw", out);
}

static void test_derive_seg_id(void)
{
    char out[SERVER_SEGID_MAX];
    topoDeriveSegId("10.0.0.0/24", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("seg-10-0-0-0-24", out);
    /* empty cidr -> "" */
    topoDeriveSegId("", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("", out);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_split_field);
    RUN_TEST(test_parse_int);
    RUN_TEST(test_derive_gear_id);
    RUN_TEST(test_derive_seg_id);
    return UNITY_END();
}
