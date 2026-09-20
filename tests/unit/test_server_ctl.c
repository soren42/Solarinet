/*
 * test_server_ctl.c - pure request-protocol layer of solariCtl.c (§11.1): line
 * trimming, k=v argument extraction (string / u64 / hex), verb splitting, the
 * destructive-verb RBAC gate, reply formatting, and the structural CSR check.
 * No socket / CA / DB.
 */
#include "unity.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "solariCtl.c"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_trim_line(void)
{
    char a[] = "PING\r\n";
    TEST_ASSERT_EQUAL_UINT(4, ctlTrimLine(a));
    TEST_ASSERT_EQUAL_STRING("PING", a);
    char b[] = "  spaced  \t\n";
    ctlTrimLine(b);
    TEST_ASSERT_EQUAL_STRING("  spaced", b); /* only trailing ws trimmed */
}

static void test_arg_str(void)
{
    const char *args = "node=42 op=alice scope=0x07";
    char out[64];
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, ctlArgStr(args, "op", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("alice", out);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, ctlArgStr(args, "node", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("42", out);
    /* a key that is a prefix of another must not false-match */
    TEST_ASSERT_EQUAL_INT(ERR_TLV_END, ctlArgStr(args, "no", out, sizeof out));
    /* absent key */
    TEST_ASSERT_EQUAL_INT(ERR_TLV_END, ctlArgStr(args, "missing", out, sizeof out));
    /* value too large for the buffer */
    char tiny[3];
    TEST_ASSERT_EQUAL_INT(ERR_BUFFER_FULL, ctlArgStr(args, "op", tiny, sizeof tiny));
}

static void test_arg_numeric(void)
{
    const char *args = "node=12345 scope=0x1f bad=xyz";
    uint64_t n = 0;
    uint32_t s = 0;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, ctlArgU64(args, "node", &n));
    TEST_ASSERT_EQUAL_UINT64(12345, n);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, ctlArgU32Hex(args, "scope", &s));
    TEST_ASSERT_EQUAL_UINT32(0x1f, s);
    /* present but non-numeric -> INVALID_ARG */
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG, ctlArgU64(args, "bad", &n));
    /* absent -> TLV_END (out untouched) */
    TEST_ASSERT_EQUAL_INT(ERR_TLV_END, ctlArgU64(args, "nope", &n));
}

static void test_split_verb(void)
{
    char line[] = "PROVISION node=7 cfg=abc";
    char verb[32];
    const char *args = NULL;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, ctlSplitVerb(line, verb, sizeof verb, &args));
    TEST_ASSERT_EQUAL_STRING("PROVISION", verb);
    TEST_ASSERT_EQUAL_STRING("node=7 cfg=abc", args);

    /* verb-only line -> empty args */
    char line2[] = "PING";
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, ctlSplitVerb(line2, verb, sizeof verb, &args));
    TEST_ASSERT_EQUAL_STRING("PING", verb);
    TEST_ASSERT_EQUAL_STRING("", args);

    /* empty line -> INVALID_ARG */
    char line3[] = "   ";
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG, ctlSplitVerb(line3, verb, sizeof verb, &args));
}

static void test_rbac(void)
{
    char op[64];
    /* non-destructive verbs pass without an operator */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, ctlCheckRbac("PING", "", op, sizeof op));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, ctlCheckRbac("PROVISION", "node=1", op, sizeof op));

    TEST_ASSERT_TRUE(ctlVerbIsDestructive("DECOMMISSION"));
    TEST_ASSERT_TRUE(ctlVerbIsDestructive("RETIRE"));
    TEST_ASSERT_TRUE(ctlVerbIsDestructive("ASSET_REMOVE"));
    TEST_ASSERT_TRUE(ctlVerbIsDestructive("TARGET_REMOVE"));
    TEST_ASSERT_TRUE(ctlVerbIsDestructive("POOL_DEL"));
    TEST_ASSERT_TRUE(ctlVerbIsDestructive("RULE_DEL"));
    TEST_ASSERT_FALSE(ctlVerbIsDestructive("PROVISION"));
    TEST_ASSERT_FALSE(ctlVerbIsDestructive("ALERT_ACK"));

    /* destructive without op= is refused */
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE, ctlCheckRbac("DECOMMISSION", "node=1", op, sizeof op));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE, ctlCheckRbac("ASSET_REMOVE", "asset=1", op, sizeof op));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE, ctlCheckRbac("TARGET_REMOVE", "target=icmp:1.2.3.4", op, sizeof op));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE, ctlCheckRbac("POOL_DEL", "pool=2", op, sizeof op));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE, ctlCheckRbac("RULE_DEL", "rule=1", op, sizeof op));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE, ctlCheckRbac("ALERT_ACK", "event=1", op, sizeof op));
    /* destructive WITH op= passes and reports the operator */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, ctlCheckRbac("RETIRE", "node=1 op=carol", op, sizeof op));
    TEST_ASSERT_EQUAL_STRING("carol", op);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, ctlCheckRbac("ALERT_ACK", "event=1 op=dana", op, sizeof op));
    TEST_ASSERT_EQUAL_STRING("dana", op);
}

/* Task #1 — verb-class mapping is an ALLOWLIST (review F1/F4). Only an explicit
 * set of read/ordinary dashboard verbs is ORDINARY; the two queue verbs are
 * ENQUEUE; EVERYTHING ELSE — including any verb not yet invented — defaults to
 * PRIVILEGED. This is the fix for the original denylist, under which a new
 * privileged verb (or one merely absent from the destructive list, like
 * PROVISION/APPROVE/CONFIG_SET) was silently ordinary and reachable by PHP. */
static void test_verb_priv_class(void)
{
    /* the ENQUEUE queue verbs */
    TEST_ASSERT_EQUAL_INT(CTL_VC_ENQUEUE, ctlVerbPrivClass("REQUEST_SUBMIT"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_ENQUEUE, ctlVerbPrivClass("REQUEST_GET"));

    /* the ALLOWLISTED ordinary dashboard-drivable verbs (exhaustive) */
    TEST_ASSERT_EQUAL_INT(CTL_VC_ORDINARY, ctlVerbPrivClass("PING"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_ORDINARY, ctlVerbPrivClass("DISCOVER"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_ORDINARY, ctlVerbPrivClass("SURVEY"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_ORDINARY, ctlVerbPrivClass("ADOPT"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_ORDINARY, ctlVerbPrivClass("ASSET_SET"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_ORDINARY, ctlVerbPrivClass("IGNORE"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_ORDINARY, ctlVerbPrivClass("POOL_NEW"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_ORDINARY, ctlVerbPrivClass("POOL_SET"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_ORDINARY, ctlVerbPrivClass("RULE_SET"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_ORDINARY, ctlVerbPrivClass("ALERT_ACK"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_ORDINARY, ctlVerbPrivClass("CRIT_SET"));

    /* formerly-destructive verbs remain PRIVILEGED */
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass("DECOMMISSION"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass("RETIRE"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass("ASSET_REMOVE"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass("TARGET_REMOVE"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass("POOL_DEL"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass("RULE_DEL"));

    /* the F1/F4 core: non-destructive-but-privileged verbs the OLD denylist let
     * through as ordinary are now PRIVILEGED and thus refused to the dashboard */
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass("PROVISION"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass("APPROVE"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass("REJECT"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass("CONFIG_SET"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass("CONTROL"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass("DEPLOY"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass("FLEET_PROVISION"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass("FLEET_IMAGE"));

    /* default-deny: an unknown / not-yet-invented verb is PRIVILEGED, not ordinary */
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass("SOME_NEW_VERB_2027"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass(""));
}

/* Task #1 — peer classification from SO_PEERCRED uid. root and the operator uid
 * are OPERATOR; the configured dashboard uid is DASHBOARD; anyone else UNKNOWN.
 * A dashboardUid of 0 (unset) must match nobody, not root. */
static void test_classify_peer(void)
{
    const uint32_t opUid = 1000, dashUid = 33;
    /* root is always operator */
    TEST_ASSERT_EQUAL_INT(CTL_PEER_OPERATOR, ctlClassifyPeer(0, opUid, dashUid));
    /* the operator uid */
    TEST_ASSERT_EQUAL_INT(CTL_PEER_OPERATOR, ctlClassifyPeer(opUid, opUid, dashUid));
    /* the dashboard uid */
    TEST_ASSERT_EQUAL_INT(CTL_PEER_DASHBOARD, ctlClassifyPeer(dashUid, opUid, dashUid));
    /* an unrelated uid */
    TEST_ASSERT_EQUAL_INT(CTL_PEER_UNKNOWN, ctlClassifyPeer(4242, opUid, dashUid));
    /* unset dashboardUid (0) must not turn a stray uid into a dashboard, and
     * must not shadow root's operator status */
    TEST_ASSERT_EQUAL_INT(CTL_PEER_UNKNOWN, ctlClassifyPeer(4242, opUid, 0));
    TEST_ASSERT_EQUAL_INT(CTL_PEER_OPERATOR, ctlClassifyPeer(0, opUid, 0));
}

/* Task #1 — the authorization matrix. OPERATOR may invoke anything; DASHBOARD
 * everything except PRIVILEGED; UNKNOWN nothing at all. This is the core of the
 * boundary: PHP (DASHBOARD) is refused every privileged verb even with op=. */
static void test_peer_may_invoke(void)
{
    /* OPERATOR: all three classes */
    TEST_ASSERT_TRUE(ctlPeerMayInvoke(CTL_PEER_OPERATOR, CTL_VC_ORDINARY));
    TEST_ASSERT_TRUE(ctlPeerMayInvoke(CTL_PEER_OPERATOR, CTL_VC_ENQUEUE));
    TEST_ASSERT_TRUE(ctlPeerMayInvoke(CTL_PEER_OPERATOR, CTL_VC_PRIVILEGED));
    /* DASHBOARD: ordinary + enqueue, but NEVER privileged */
    TEST_ASSERT_TRUE(ctlPeerMayInvoke(CTL_PEER_DASHBOARD, CTL_VC_ORDINARY));
    TEST_ASSERT_TRUE(ctlPeerMayInvoke(CTL_PEER_DASHBOARD, CTL_VC_ENQUEUE));
    TEST_ASSERT_FALSE(ctlPeerMayInvoke(CTL_PEER_DASHBOARD, CTL_VC_PRIVILEGED));
    /* UNKNOWN: nothing */
    TEST_ASSERT_FALSE(ctlPeerMayInvoke(CTL_PEER_UNKNOWN, CTL_VC_ORDINARY));
    TEST_ASSERT_FALSE(ctlPeerMayInvoke(CTL_PEER_UNKNOWN, CTL_VC_ENQUEUE));
    TEST_ASSERT_FALSE(ctlPeerMayInvoke(CTL_PEER_UNKNOWN, CTL_VC_PRIVILEGED));
}

static void test_pool_delete_guard(void)
{
    TEST_ASSERT_FALSE(ctlPoolCanDelete(0));
    TEST_ASSERT_FALSE(ctlPoolCanDelete(1));
    TEST_ASSERT_TRUE(ctlPoolCanDelete(2));
}

static void test_reply_format(void)
{
    char out[128];
    size_t n = ctlReplyOk(out, sizeof out, "confirm=123");
    TEST_ASSERT_EQUAL_STRING("OK confirm=123\n", out);
    TEST_ASSERT_EQUAL_UINT(strlen("OK confirm=123\n"), n);

    ctlReplyOk(out, sizeof out, NULL);
    TEST_ASSERT_EQUAL_STRING("OK\n", out);

    ctlReplyErr(out, sizeof out, ERR_AUTH_ROLE, "not authorized");
    /* "ERR <code> <msg>\n" - code is the numeric solariStatus */
    TEST_ASSERT_EQUAL_INT(0, strncmp(out, "ERR ", 4));
    TEST_ASSERT_NOT_NULL(strstr(out, "not authorized"));
    TEST_ASSERT_EQUAL_INT('\n', out[strlen(out) - 1]);
}

static void test_looks_like_csr(void)
{
    const char *good =
        "-----BEGIN CERTIFICATE REQUEST-----\nMIIB...\n-----END CERTIFICATE REQUEST-----\n";
    const char *bad = "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----\n";
    TEST_ASSERT_TRUE(ctlLooksLikeCsr(good));
    TEST_ASSERT_FALSE(ctlLooksLikeCsr(bad));
    TEST_ASSERT_FALSE(ctlLooksLikeCsr(NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_trim_line);
    RUN_TEST(test_arg_str);
    RUN_TEST(test_arg_numeric);
    RUN_TEST(test_split_verb);
    RUN_TEST(test_rbac);
    RUN_TEST(test_verb_priv_class);
    RUN_TEST(test_classify_peer);
    RUN_TEST(test_peer_may_invoke);
    RUN_TEST(test_pool_delete_guard);
    RUN_TEST(test_reply_format);
    RUN_TEST(test_looks_like_csr);
    return UNITY_END();
}
