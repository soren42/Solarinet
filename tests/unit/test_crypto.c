/*
 * test_crypto.c - FNV-1a-64, CRC-32, SHA-256, hex, cert CN against known
 * published test vectors.
 */
#include "unity.h"
#include "solari/solariCrypto.h"
#include "solari/solariError.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* FNV-1a-64 reference values (fnvhash test suite). */
static void test_fnv1a64_vectors(void)
{
    TEST_ASSERT_EQUAL_UINT64(0xcbf29ce484222325ull, solariFnv1a64Str(""));
    TEST_ASSERT_EQUAL_UINT64(0xaf63dc4c8601ec8cull, solariFnv1a64Str("a"));
    TEST_ASSERT_EQUAL_UINT64(0x85944171f73967e8ull, solariFnv1a64Str("foobar"));
}

/* CRC-32 (IEEE) reference values. */
static void test_crc32_vectors(void)
{
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, solariCrc32(0, "", 0));
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, solariCrc32(0, "123456789", 9));
    TEST_ASSERT_EQUAL_HEX32(0x414FA339u,
        solariCrc32(0, "The quick brown fox jumps over the lazy dog", 43));
}

/* CRC-32 streamed in two chunks must equal the one-shot. */
static void test_crc32_streaming(void)
{
    uint32_t a = solariCrc32(0, "12345", 5);
    uint32_t b = solariCrc32(a, "6789", 4);
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, b);
}

static void hexEq(const uint8_t *d, const char *want)
{
    char hex[65];
    solariHexEncode(d, 32, hex, sizeof hex);
    TEST_ASSERT_EQUAL_STRING(want, hex);
}

/* SHA-256 NIST vectors. */
static void test_sha256_vectors(void)
{
    uint8_t d[32];
    solariSha256("", 0, d);
    hexEq(d, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    solariSha256("abc", 3, d);
    hexEq(d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    solariSha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d);
    hexEq(d, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

/* SHA-256 streamed in pieces must equal the one-shot. */
static void test_sha256_streaming(void)
{
    solariSha256Ctx c;
    uint8_t d[32];
    solariSha256Init(&c);
    solariSha256Update(&c, "ab", 2);
    solariSha256Update(&c, "c", 1);
    solariSha256Final(&c, d);
    hexEq(d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

static void test_certCn_basic(void)
{
    char cn[64];
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariCertCn("CN=monitor.akoria.net,O=Akoria,C=US", cn, sizeof cn));
    TEST_ASSERT_EQUAL_STRING("monitor.akoria.net", cn);
}

static void test_certCn_midDn_andSpaces(void)
{
    char cn[64];
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariCertCn("O=Akoria, CN= argon.akoria.net , C=US", cn, sizeof cn));
    TEST_ASSERT_EQUAL_STRING("argon.akoria.net", cn);
}

static void test_certCn_escapedComma(void)
{
    char cn[64];
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariCertCn("CN=node\\,01.akoria.net,O=Akoria", cn, sizeof cn));
    TEST_ASSERT_EQUAL_STRING("node,01.akoria.net", cn);
}

static void test_certCn_missing(void)
{
    char cn[64];
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG,
        solariCertCn("O=Akoria,C=US", cn, sizeof cn));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fnv1a64_vectors);
    RUN_TEST(test_crc32_vectors);
    RUN_TEST(test_crc32_streaming);
    RUN_TEST(test_sha256_vectors);
    RUN_TEST(test_sha256_streaming);
    RUN_TEST(test_certCn_basic);
    RUN_TEST(test_certCn_midDn_andSpaces);
    RUN_TEST(test_certCn_escapedComma);
    RUN_TEST(test_certCn_missing);
    return UNITY_END();
}
