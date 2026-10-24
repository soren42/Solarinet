/*
 * test_server_provision.c - pure layer of serverProvision.c (§7.3, §7.4):
 *   - cryptographic decommission-token generation and digest formatting;
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

static void test_confirm_token_random_and_hashed(void)
{
    uint64_t a = 0, b = 0;
    char hashA[65], hashB[65];
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, provRandomConfirmToken(&a));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, provRandomConfirmToken(&b));
    TEST_ASSERT_NOT_EQUAL_UINT64(0, a);
    TEST_ASSERT_NOT_EQUAL_UINT64(a, b);
    provConfirmTokenHash(a, hashA);
    provConfirmTokenHash(a, hashB);
    TEST_ASSERT_EQUAL_STRING(hashA, hashB); /* stable digest for the challenge row */
    TEST_ASSERT_EQUAL_size_t(64, strlen(hashA));
}

static void test_provision_identity_matches_shared_derivation(void)
{
    TEST_ASSERT_EQUAL_UINT64(
        solariNodeIdFromFqdnRole("argon.akoria.net", ROLE_CLIENT),
        provNodeIdForIdentity("argon.akoria.net", ROLE_CLIENT));
    TEST_ASSERT_EQUAL_UINT64(0,
        provNodeIdForIdentity("", ROLE_CLIENT));
}

static void test_enrollment_node_id_is_stable_by_role_and_host(void)
{
    uint64_t client = provNodeIdForIdentity("beryl.akoria.net", ROLE_CLIENT);
    uint64_t monitor = provNodeIdForIdentity("beryl.akoria.net", ROLE_MONITOR);
    TEST_ASSERT_EQUAL_HEX64(solariFnv1a64Str("beryl.akoria.net|1"), client);
    TEST_ASSERT_EQUAL_HEX64(solariFnv1a64Str("beryl.akoria.net|2"), monitor);
    TEST_ASSERT_NOT_EQUAL(client, monitor);
    TEST_ASSERT_NOT_EQUAL(0, client);
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
    solariStatus st = provBuildProvisionPayload(0xA0DE0B0BULL, 0xDEAD,
                                                blob, sizeof blob,
                                                buf, sizeof buf, &len, &count);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, st);
    TEST_ASSERT_EQUAL_UINT16(4, count);   /* verb + epoch + addressee + payload */

    uint8_t verb = 0; uint64_t epoch = 0, node = 0;
    TEST_ASSERT_TRUE(findTlvU8 (buf, len, TLV_CTRL_VERB, &verb));
    TEST_ASSERT_TRUE(findTlvU64(buf, len, TLV_CTRL_TARGET_EPOCH, &epoch));
    TEST_ASSERT_TRUE(findTlvU64(buf, len, TLV_CTRL_TARGET_NODE, &node));
    TEST_ASSERT_EQUAL_UINT8(CTRL_PROVISION, verb);
    TEST_ASSERT_EQUAL_HEX64(0xDEAD, epoch);
    TEST_ASSERT_EQUAL_HEX64(0xA0DE0B0BULL, node);

    /* no blob, broadcast (node 0) -> just verb + epoch */
    st = provBuildProvisionPayload(0, 1, NULL, 0, buf, sizeof buf, &len, &count);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, st);
    TEST_ASSERT_EQUAL_UINT16(2, count);
}

static void test_build_decommission_payload(void)
{
    uint8_t buf[256]; size_t len = 0; uint16_t count = 0;
    solariStatus st = provBuildDecommissionPayload(0x1122334455667788ULL,
                          0xCAFEBABEULL,
                          WIPE_CONFIG | WIPE_CERTS | WIPE_SPOOL,
                          buf, sizeof buf, &len, &count);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, st);
    TEST_ASSERT_EQUAL_UINT16(4, count);   /* verb + target + generation + wipescope */

    uint8_t verb = 0, scope = 0; uint64_t target = 0, generation = 0;
    TEST_ASSERT_TRUE(findTlvU8 (buf, len, TLV_CTRL_VERB, &verb));
    TEST_ASSERT_TRUE(findTlvU64(buf, len, TLV_CTRL_TARGET_NODE, &target));
    TEST_ASSERT_TRUE(findTlvU64(buf, len, TLV_LIFE_GENERATION, &generation));
    TEST_ASSERT_TRUE(findTlvU8 (buf, len, TLV_LIFE_WIPE_SCOPE, &scope));
    TEST_ASSERT_EQUAL_UINT8(CTRL_DECOMMISSION, verb);
    TEST_ASSERT_EQUAL_HEX64(0x1122334455667788ULL, target);
    TEST_ASSERT_EQUAL_HEX64(0xCAFEBABEULL, generation);
    TEST_ASSERT_EQUAL_UINT8(WIPE_CONFIG | WIPE_CERTS | WIPE_SPOOL, scope);
}

static void test_decommission_retry_backoff_is_bounded(void)
{
    TEST_ASSERT_EQUAL_UINT64(5000, provDecommissionRetryDelayMs(0));
    TEST_ASSERT_EQUAL_UINT64(10000, provDecommissionRetryDelayMs(1));
    TEST_ASSERT_EQUAL_UINT64(160000, provDecommissionRetryDelayMs(5));
    TEST_ASSERT_EQUAL_UINT64(300000, provDecommissionRetryDelayMs(6));
    TEST_ASSERT_EQUAL_UINT64(300000, provDecommissionRetryDelayMs(UINT32_MAX));
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
    RUN_TEST(test_confirm_token_random_and_hashed);
    RUN_TEST(test_provision_identity_matches_shared_derivation);
    RUN_TEST(test_enrollment_node_id_is_stable_by_role_and_host);
    RUN_TEST(test_hrw_top_owner);
    RUN_TEST(test_hrw_weight_keying);
    RUN_TEST(test_build_provision_payload);
    RUN_TEST(test_build_decommission_payload);
    RUN_TEST(test_decommission_retry_backoff_is_bounded);
    RUN_TEST(test_build_adopt_payload);
    return UNITY_END();
}
