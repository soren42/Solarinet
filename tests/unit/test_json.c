/*
 * test_json.c - the bounded JSON field extractor (solariJson) that the agents
 * use to apply control-plane config blobs. Wire input is untrusted, so the
 * tests lean on the rejection paths: malformed documents, truncated strings,
 * absent keys, wrong-typed values, and overflow-y numbers.
 */
#include "unity.h"
#include "solari/solariJson.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

#define S(lit) lit, strlen(lit)

/* ---- validation ---- */

static void test_validate_ok(void)
{
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariJsonValidate(S("{}")));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariJsonValidate(S("{\"a\":1}")));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariJsonValidate(S("{\"a\":{\"b\":[1,\"x\",{}]},\"c\":\"}]\"}")));
}

static void test_validate_rejects_malformed(void)
{
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG, solariJsonValidate(NULL, 0));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG, solariJsonValidate(S("")));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG, solariJsonValidate(S("{\"a\":1")));   /* unbalanced */
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG, solariJsonValidate(S("{\"a\":1}}"))); /* extra close */
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG, solariJsonValidate(S("{\"a\":\"x}"))); /* open string */
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG, solariJsonValidate(S("plain text")));  /* no structure */
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG, solariJsonValidate(S("{\"a\":\"x\\"))); /* dangling esc */
}

/* ---- numbers ---- */

static void test_get_u64(void)
{
    uint64_t v = 0;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariJsonGetU64(S("{\"sampleIntervalSec\":42}"), "sampleIntervalSec", &v));
    TEST_ASSERT_EQUAL_UINT64(42, v);

    /* nested (dashboard section layout) resolves the same */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariJsonGetU64(S("{\"schedule\":{\"sampleIntervalSec\": 15}}"),
                         "sampleIntervalSec", &v));
    TEST_ASSERT_EQUAL_UINT64(15, v);

    /* quoted number tolerated */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariJsonGetU64(S("{\"epoch\":\"7\"}"), "epoch", &v));
    TEST_ASSERT_EQUAL_UINT64(7, v);

    /* absent key / non-number value */
    TEST_ASSERT_EQUAL_INT(ERR_UNKNOWN_MSG,
        solariJsonGetU64(S("{\"a\":1}"), "b", &v));
    TEST_ASSERT_EQUAL_INT(ERR_UNKNOWN_MSG,
        solariJsonGetU64(S("{\"a\":\"xyz\"}"), "a", &v));

    /* a key inside a string VALUE must not match */
    TEST_ASSERT_EQUAL_INT(ERR_UNKNOWN_MSG,
        solariJsonGetU64(S("{\"note\":\"the \\\"k\\\": 1 trick\"}"), "k", &v));
}

/* ---- strings ---- */

static void test_get_str(void)
{
    char buf[32];
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariJsonGetStr(S("{\"regex\":\"ERROR|WARN\"}"), "regex", buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("ERROR|WARN", buf);

    /* escapes decode; truncation is bounded + NUL-terminated */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariJsonGetStr(S("{\"s\":\"a\\\"b\\nc\"}"), "s", buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("a\"b\nc", buf);

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariJsonGetStr(S("{\"s\":\"0123456789\"}"), "s", buf, 4));
    TEST_ASSERT_EQUAL_STRING("012", buf);

    TEST_ASSERT_EQUAL_INT(ERR_UNKNOWN_MSG,
        solariJsonGetStr(S("{\"s\":123}"), "s", buf, sizeof buf));
}

/* ---- string arrays ---- */

static void test_get_str_array(void)
{
    const char *doc = "{\"processes\":[\"mariadbd\",\"apache2\", \"sshd\"],\"n\":1}";
    char buf[32];

    TEST_ASSERT_TRUE(solariJsonHasArray(doc, strlen(doc), "processes"));
    TEST_ASSERT_FALSE(solariJsonHasArray(doc, strlen(doc), "n"));
    TEST_ASSERT_FALSE(solariJsonHasArray(doc, strlen(doc), "absent"));

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariJsonGetStrAt(doc, strlen(doc), "processes", 0, buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("mariadbd", buf);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariJsonGetStrAt(doc, strlen(doc), "processes", 2, buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("sshd", buf);
    TEST_ASSERT_EQUAL_INT(ERR_TLV_END,
        solariJsonGetStrAt(doc, strlen(doc), "processes", 3, buf, sizeof buf));

    /* empty array terminates immediately */
    TEST_ASSERT_EQUAL_INT(ERR_TLV_END,
        solariJsonGetStrAt(S("{\"a\":[]}"), "a", 0, buf, sizeof buf));

    /* mixed types: non-string element at the index is an error, but elements
     * beyond it stay reachable (skip logic) */
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG,
        solariJsonGetStrAt(S("{\"a\":[1,\"two\"]}"), "a", 0, buf, sizeof buf));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariJsonGetStrAt(S("{\"a\":[1,\"two\"]}"), "a", 1, buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("two", buf);

    /* nested containers are skipped whole */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariJsonGetStrAt(S("{\"a\":[{\"x\":[1,2]},\"end\"]}"), "a", 1,
                           buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("end", buf);

    /* unterminated array is an error, not a runaway scan */
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG,
        solariJsonGetStrAt(S("{\"a\":[\"x\""), "a", 1, buf, sizeof buf));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_validate_ok);
    RUN_TEST(test_validate_rejects_malformed);
    RUN_TEST(test_get_u64);
    RUN_TEST(test_get_str);
    RUN_TEST(test_get_str_array);
    return UNITY_END();
}
