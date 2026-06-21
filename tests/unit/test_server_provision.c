/*
 * test_server_provision.c - pure layer of serverProvision.c (§7.3, §7.4):
 *   - the one-time decommission confirm-token derivation (deterministic from its
 *     inputs, never 0, sensitive to every input);
 *   - HRW owner selection over a fleet (determinism + tie-break by nodeId);
 *   - the control-frame TLV payload builders (PROVISION / DECOMMISSION / ADOPT),
 *     verified by parsing the bytes back with the real TLV reader.
 *
 * No DB / CA / sockets are touched.
 */
#include "unity.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "serverProvision.c"

#include "solari/solariTlv.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- confirm token ---- */

static void test_confirm_token_deterministic(void)
{
    uint64_t a = provDeriveConfirmToken(0x1234, WIPE_CONFIG | WIPE_CERTS,
                                        "alice", 1700000000000ULL, 42);
    uint64_t b = provDeriveConfirmToken(0x1234, WIPE_CONFIG | WIPE_CERTS,
                                        "alice", 1700000000000ULL, 42);
    TEST_ASSERT_EQUAL_HEX64(a, b);          /* reproducible for fixed inputs */
    TEST_ASSERT_NOT_EQUAL(0, a);            /* 0 is reserved for "none" */
}

static void test_confirm_token_input_sensitive(void)
{
    uint64_t base = provDeriveConfirmToken(0x1234, WIPE_CONFIG, "alice",
                                           1700000000000ULL, 42);
    /* every input must move the token */
    TEST_ASSERT_NOT_EQUAL(base, provDeriveConfirmToken(0x9999, WIPE_CONFIG, "alice", 1700000000000ULL, 42));
    TEST_ASSERT_NOT_EQUAL(base, provDeriveConfirmToken(0x1234, WIPE_CERTS,  "alice", 1700000000000ULL, 42));
    TEST_ASSERT_NOT_EQUAL(base, provDeriveConfirmToken(0x1234, WIPE_CONFIG, "bob",   1700000000000ULL, 42));
    TEST_ASSERT_NOT_EQUAL(base, provDeriveConfirmToken(0x1234, WIPE_CONFIG, "alice", 1700000000001ULL, 42));
    TEST_ASSERT_NOT_EQUAL(base, provDeriveConfirmToken(0x1234, WIPE_CONFIG, "alice", 1700000000000ULL, 43));
}

/* ---- HRW owner selection ---- */

static void test_hrw_top_owner(void)
{
    uint64_t fleet[3] = { 0x11, 0x22, 0x33 };
    uint64_t o1 = provHrwTopOwner("tcp:10.0.0.1:443", fleet, 3);
    uint64_t o2 = provHrwTopOwner("tcp:10.0.0.1:443", fleet, 3);
    TEST_ASSERT_EQUAL_HEX64(o1, o2);                 /* deterministic */
    /* the chosen owner is a member of the fleet */
    TEST_ASSERT_TRUE(o1 == 0x11 || o1 == 0x22 || o1 == 0x33);

    /* single-element fleet always yields that element */
    uint64_t solo[1] = { 0xABCD };
    TEST_ASSERT_EQUAL_HEX64(0xABCD, provHrwTopOwner("anything", solo, 1));

    /* empty fleet -> 0 */
    TEST_ASSERT_EQUAL_HEX64(0, provHrwTopOwner("x", fleet, 0));
}

static void test_hrw_weight_keying(void)
{
    /* different nodes give different weights for the same target (overwhelmingly
     * likely with a 64-bit hash) so the top-owner is well defined */
    uint64_t wa = provHrwWeight("tcp:1.2.3.4:80", 0x1);
    uint64_t wb = provHrwWeight("tcp:1.2.3.4:80", 0x2);
    TEST_ASSERT_NOT_EQUAL(wa, wb);
    /* same inputs -> same weight */
    TEST_ASSERT_EQUAL_HEX64(wa, provHrwWeight("tcp:1.2.3.4:80", 0x1));
}

/* ---- control payload builders, round-tripped through the TLV reader ---- */

static bool findTlvU8(const uint8_t *buf, size_t len, uint16_t type, uint8_t *out)
{
    solariTlvReader r; uint16_t t, l; const uint8_t *v;
    solariTlvReaderInit(&r, buf, len);
    while (solariTlvNext(&r, &t, &v, &l) == SOLARI_OK)
        if (t == type) return solariTlvReadU8(v, l, out) == SOLARI_OK;
    return false;
}
static bool findTlvU64(const uint8_t *buf, size_t len, uint16_t type, uint64_t *out)
{
    solariTlvReader r; uint16_t t, l; const uint8_t *v;
    solariTlvReaderInit(&r, buf, len);
    while (solariTlvNext(&r, &t, &v, &l) == SOLARI_OK)
        if (t == type) return solariTlvReadU64(v, l, out) == SOLARI_OK;
    return false;
}

static void test_build_provision_payload(void)
{
    uint8_t buf[256]; size_t len = 0; uint16_t count = 0;
    const uint8_t blob[] = { 'c','f','g' };
    solariStatus st = provBuildProvisionPayload(0xDEAD, blob, sizeof blob,
                                                buf, sizeof buf, &len, &count);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, st);
    TEST_ASSERT_EQUAL_UINT16(3, count);   /* verb + epoch + payload */

    uint8_t verb = 0; uint64_t epoch = 0;
    TEST_ASSERT_TRUE(findTlvU8 (buf, len, TLV_CTRL_VERB, &verb));
    TEST_ASSERT_TRUE(findTlvU64(buf, len, TLV_CTRL_TARGET_EPOCH, &epoch));
    TEST_ASSERT_EQUAL_UINT8(CTRL_PROVISION, verb);
    TEST_ASSERT_EQUAL_HEX64(0xDEAD, epoch);

    /* no blob -> just verb + epoch */
    st = provBuildProvisionPayload(1, NULL, 0, buf, sizeof buf, &len, &count);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, st);
    TEST_ASSERT_EQUAL_UINT16(2, count);
}

static void test_build_decommission_payload(void)
{
    uint8_t buf[256]; size_t len = 0; uint16_t count = 0;
    solariStatus st = provBuildDecommissionPayload(0xCAFEBABEULL,
                          WIPE_CONFIG | WIPE_CERTS | WIPE_SPOOL,
                          buf, sizeof buf, &len, &count);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, st);
    TEST_ASSERT_EQUAL_UINT16(3, count);   /* verb + confirm + wipescope */

    uint8_t verb = 0, scope = 0; uint64_t tok = 0;
    TEST_ASSERT_TRUE(findTlvU8 (buf, len, TLV_CTRL_VERB, &verb));
    TEST_ASSERT_TRUE(findTlvU64(buf, len, TLV_LIFE_CONFIRM, &tok));
    TEST_ASSERT_TRUE(findTlvU8 (buf, len, TLV_LIFE_WIPE_SCOPE, &scope));
    TEST_ASSERT_EQUAL_UINT8(CTRL_DECOMMISSION, verb);
    TEST_ASSERT_EQUAL_HEX64(0xCAFEBABEULL, tok);
    TEST_ASSERT_EQUAL_UINT8(WIPE_CONFIG | WIPE_CERTS | WIPE_SPOOL, scope);
}

static void test_build_adopt_payload(void)
{
    uint8_t buf[256]; size_t len = 0; uint16_t count = 0;
    solariStatus st = provBuildAdoptPayload("tcp:10.0.0.9:22",
                                            buf, sizeof buf, &len, &count);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, st);
    TEST_ASSERT_EQUAL_UINT16(2, count);   /* verb + spec */

    uint8_t verb = 0;
    TEST_ASSERT_TRUE(findTlvU8(buf, len, TLV_CTRL_VERB, &verb));
    TEST_ASSERT_EQUAL_UINT8(CTRL_ADOPT_TARGET, verb);

    /* empty spec -> just the verb */
    st = provBuildAdoptPayload("", buf, sizeof buf, &len, &count);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, st);
    TEST_ASSERT_EQUAL_UINT16(1, count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_confirm_token_deterministic);
    RUN_TEST(test_confirm_token_input_sensitive);
    RUN_TEST(test_hrw_top_owner);
    RUN_TEST(test_hrw_weight_keying);
    RUN_TEST(test_build_provision_payload);
    RUN_TEST(test_build_decommission_payload);
    RUN_TEST(test_build_adopt_payload);
    return UNITY_END();
}
