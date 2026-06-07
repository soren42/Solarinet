/*
 * test_config.c - .conf parser (section 13): sections, comments, repeated keys,
 * typed getters, value preservation, epoch.
 */
#include "unity.h"
#include "solari/solariConfig.h"
#include "solari/solariError.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* A representative client.conf (section 13), including repeated watch keys and a
 * value that itself contains ':' and a regex. */
static const char *CLIENT_CONF =
    "[identity]\n"
    "nodeId      = # filled at enrollment\n"
    "hostFqdn    = hydrogen.akoria.net\n"
    "configEpoch = 42\n"
    "\n"
    "[server]\n"
    "primaryUrl  = tls+tcp://argon.akoria.net:7701\n"
    "failoverUrl = tls+tcp://neon.akoria.net:7701\n"
    "\n"
    "[schedule]\n"
    "sampleIntervalSec   = 15\n"
    "watchdogIntervalSec = 5\n"
    "\n"
    "[watch]\n"
    "process = apache2\n"
    "process = mariadbd\n"
    "logfile = /var/log/syslog : (error|fail)\n"
    "spoolDb = /var/lib/solari/spool.db\n"
    "# a trailing comment line\n";

static solariConfig *parse(void)
{
    solariConfig *c = NULL;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariConfigParse(CLIENT_CONF, &c));
    TEST_ASSERT_NOT_NULL(c);
    return c;
}

static void test_string_lookup(void)
{
    solariConfig *c = parse();
    TEST_ASSERT_EQUAL_STRING("hydrogen.akoria.net",
        solariConfigGetStr(c, "identity", "hostFqdn", "?"));
    TEST_ASSERT_EQUAL_STRING("tls+tcp://argon.akoria.net:7701",
        solariConfigGetStr(c, "server", "primaryUrl", "?"));
    solariConfigFree(c);
}

static void test_value_with_colons_preserved(void)
{
    solariConfig *c = parse();
    /* the whole RHS (including ':' and the regex) must survive verbatim */
    TEST_ASSERT_EQUAL_STRING("/var/log/syslog : (error|fail)",
        solariConfigGetStr(c, "watch", "logfile", "?"));
    solariConfigFree(c);
}

static void test_inline_comment_yields_empty(void)
{
    solariConfig *c = parse();
    /* "nodeId = # filled at enrollment" -> value is empty after comment strip */
    TEST_ASSERT_EQUAL_STRING("", solariConfigGetStr(c, "identity", "nodeId", "MISSING"));
    solariConfigFree(c);
}

static void test_int_and_bool(void)
{
    solariConfig *c = parse();
    TEST_ASSERT_EQUAL_INT(15, solariConfigGetInt(c, "schedule", "sampleIntervalSec", -1));
    TEST_ASSERT_EQUAL_INT(5,  solariConfigGetInt(c, "schedule", "watchdogIntervalSec", -1));
    /* absent int returns default */
    TEST_ASSERT_EQUAL_INT(99, solariConfigGetInt(c, "schedule", "nope", 99));
    /* bool default path (no bool keys here) */
    TEST_ASSERT_TRUE(solariConfigGetBool(c, "schedule", "missing", true));
    solariConfigFree(c);
}

static void test_repeated_keys(void)
{
    solariConfig *c = parse();
    TEST_ASSERT_EQUAL_size_t(2, solariConfigCount(c, "watch", "process"));
    TEST_ASSERT_EQUAL_STRING("apache2",  solariConfigGetStrAt(c, "watch", "process", 0, "?"));
    TEST_ASSERT_EQUAL_STRING("mariadbd", solariConfigGetStrAt(c, "watch", "process", 1, "?"));
    /* out-of-range index returns default */
    TEST_ASSERT_EQUAL_STRING("none", solariConfigGetStrAt(c, "watch", "process", 2, "none"));
    /* getStr returns the first of a repeated key */
    TEST_ASSERT_EQUAL_STRING("apache2", solariConfigGetStr(c, "watch", "process", "?"));
    solariConfigFree(c);
}

static void test_epoch(void)
{
    solariConfig *c = parse();
    TEST_ASSERT_EQUAL_UINT64(42, solariConfigEpoch(c));
    solariConfigFree(c);
}

static void test_missing_section_default(void)
{
    solariConfig *c = parse();
    TEST_ASSERT_EQUAL_STRING("dflt", solariConfigGetStr(c, "nosuch", "key", "dflt"));
    TEST_ASSERT_EQUAL_size_t(0, solariConfigCount(c, "nosuch", "key"));
    solariConfigFree(c);
}

static void test_case_insensitive_keys(void)
{
    solariConfig *c = parse();
    /* lookups are case-insensitive on section and key */
    TEST_ASSERT_EQUAL_STRING("hydrogen.akoria.net",
        solariConfigGetStr(c, "IDENTITY", "HostFQDN", "?"));
    solariConfigFree(c);
}

static void test_overlay_without_json_returns_platform(void)
{
    solariConfig *c = parse();
    /* base build has no cJSON; overlay must report ERR_PLATFORM, not crash */
    TEST_ASSERT_EQUAL_INT(ERR_PLATFORM, solariConfigOverlayJson(c, "{\"a.b\":\"c\"}"));
    solariConfigFree(c);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_string_lookup);
    RUN_TEST(test_value_with_colons_preserved);
    RUN_TEST(test_inline_comment_yields_empty);
    RUN_TEST(test_int_and_bool);
    RUN_TEST(test_repeated_keys);
    RUN_TEST(test_epoch);
    RUN_TEST(test_missing_section_default);
    RUN_TEST(test_case_insensitive_keys);
    RUN_TEST(test_overlay_without_json_returns_platform);
    return UNITY_END();
}
