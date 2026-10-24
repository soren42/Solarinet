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

#include <stdio.h>
#include <string.h>

static unsigned gDecommissionIssueCalls;
static unsigned gDecommissionConfirmCalls;
static unsigned gScanSubmitCalls;
static char gScanRequester[129];
static char gScanStatusRequester[129];
static unsigned gAssetAdoptCalls;
static bool gQueuedDecommissionReady;
static bool gQueuedDecommissionCompleted;
static char gQueuedDecommissionResult[256];

solariStatus serverDbCtlRequestClaim(serverDb *db, const char *claimedBy,
    unsigned long long *reqId, char *verb, size_t verbCap, char *args, size_t argsCap,
    char *requester, size_t requesterCap)
{
    (void)db; (void)claimedBy;
    if (!gQueuedDecommissionReady) return ERR_TLV_END;
    gQueuedDecommissionReady = false;
    if (reqId) *reqId = 77;
    snprintf(verb, verbCap, "DECOMMISSION");
    snprintf(args, argsCap, "node=42 scope=0x03 inlineConfirm=1");
    snprintf(requester, requesterCap, "alice");
    return SOLARI_OK;
}

solariStatus serverDbCtlRequestComplete(serverDb *db, unsigned long long reqId,
                                         bool ok, const char *result)
{
    (void)db; (void)reqId;
    gQueuedDecommissionCompleted = ok;
    snprintf(gQueuedDecommissionResult, sizeof gQueuedDecommissionResult,
             "%s", result ? result : "");
    return SOLARI_OK;
}

solariStatus serverAssetsAdopt(serverContext *ctx, const serverAdoptOpts *opts,
                               uint64_t *assetId)
{
    (void)ctx;
    (void)opts;
    gAssetAdoptCalls++;
    if (assetId) *assetId = 424242;
    return SOLARI_OK;
}

solariStatus serverProvisionDecommission(serverContext *ctx, uint64_t nodeId,
                                         uint32_t wipeScope, const char *op,
                                         uint64_t *token)
{
    (void)ctx; (void)nodeId; (void)wipeScope; (void)op;
    gDecommissionIssueCalls++;
    *token = UINT64_C(0x123456789abcdef0);
    return SOLARI_OK;
}

solariStatus serverProvisionConfirmDecommission(serverContext *ctx,
                                                uint64_t nodeId,
                                                uint32_t wipeScope,
                                                const char *op,
                                                uint64_t token)
{
    (void)ctx; (void)nodeId; (void)wipeScope; (void)op;
    gDecommissionConfirmCalls++;
    return token == UINT64_C(0x123456789abcdef0) ? SOLARI_OK : ERR_AUTH_ROLE;
}

solariStatus serverScanPlan(const char *cidr, const char *ports, uint32_t *total)
{
    if (!cidr || strcmp(cidr, "10.0.0.0/24") != 0 ||
        (ports && ports[0] && strcmp(ports, "22") != 0)) return ERR_INVALID_ARG;
    if (total) *total = 254;
    return SOLARI_OK;
}

solariStatus serverDbDiscoveryScanSubmit(serverDb *db, uint64_t owner,
    uint64_t epoch, const char *requester, const char *cidr, const char *ports,
    uint32_t total, uint64_t *scanId)
{
    (void)db; (void)owner; (void)epoch; (void)cidr; (void)ports;
    gScanSubmitCalls++;
    snprintf(gScanRequester, sizeof gScanRequester, "%s", requester ? requester : "");
    TEST_ASSERT_EQUAL_UINT32(254, total);
    if (scanId) *scanId = 42;
    return SOLARI_OK;
}

solariStatus serverDbDiscoveryScanGet(serverDb *db, uint64_t scanId,
    const char *requester, serverDiscoveryScanJob *job)
{
    (void)db;
    snprintf(gScanStatusRequester, sizeof gScanStatusRequester, "%s",
             requester ? requester : "");
    if (scanId != 42) return ERR_TLV_END;
    memset(job, 0, sizeof *job);
    job->scanId = scanId;
    job->hostsTotal = 254;
    job->hostsDone = 254;
    job->hostsFound = 3;
    snprintf(job->status, sizeof job->status, "completed");
    return SOLARI_OK;
}

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
    TEST_ASSERT_TRUE(ctlVerbIsDestructive("REVOKE_CERT"));
    TEST_ASSERT_TRUE(ctlVerbIsDestructive("ASSET_REMOVE"));
    TEST_ASSERT_TRUE(ctlVerbIsDestructive("TARGET_REMOVE"));
    TEST_ASSERT_TRUE(ctlVerbIsDestructive("POOL_DEL"));
    TEST_ASSERT_TRUE(ctlVerbIsDestructive("RULE_DEL"));
    TEST_ASSERT_TRUE(ctlVerbIsDestructive("ENROLL_TOKEN"));
    TEST_ASSERT_FALSE(ctlVerbIsDestructive("PROVISION"));
    TEST_ASSERT_FALSE(ctlVerbIsDestructive("ALERT_ACK"));

    /* destructive without op= is refused */
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE, ctlCheckRbac("DECOMMISSION", "node=1", op, sizeof op));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
                          ctlCheckRbac("REVOKE_CERT", "node=1", op, sizeof op));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE, ctlCheckRbac("ASSET_REMOVE", "asset=1", op, sizeof op));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE, ctlCheckRbac("TARGET_REMOVE", "target=icmp:1.2.3.4", op, sizeof op));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE, ctlCheckRbac("POOL_DEL", "pool=2", op, sizeof op));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE, ctlCheckRbac("RULE_DEL", "rule=1", op, sizeof op));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE, ctlCheckRbac("ALERT_ACK", "event=1", op, sizeof op));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE, ctlCheckRbac("DISCOVER", "cidr=10.0.0.0/24", op, sizeof op));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE, ctlCheckRbac("REQUEST_GET", "request=1", op, sizeof op));
    /* destructive WITH op= passes and reports the operator */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, ctlCheckRbac("RETIRE", "node=1 op=carol", op, sizeof op));
    TEST_ASSERT_EQUAL_STRING("carol", op);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, ctlCheckRbac("ALERT_ACK", "event=1 op=dana", op, sizeof op));
    TEST_ASSERT_EQUAL_STRING("dana", op);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, ctlCheckRbac("REQUEST_GET", "request=1 op=dana", op, sizeof op));
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
    TEST_ASSERT_EQUAL_INT(CTL_VC_ORDINARY, ctlVerbPrivClass("DISCOVER_STATUS"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass("DISCOVER"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_ORDINARY, ctlVerbPrivClass("SURVEY"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_ORDINARY, ctlVerbPrivClass("ADOPT"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_ORDINARY, ctlVerbPrivClass("ASSET_SET"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_ORDINARY, ctlVerbPrivClass("IGNORE"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_ORDINARY, ctlVerbPrivClass("POOL_NEW"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_ORDINARY, ctlVerbPrivClass("POOL_SET"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_ORDINARY, ctlVerbPrivClass("RULE_SET"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_ORDINARY, ctlVerbPrivClass("ALERT_ACK"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_ORDINARY, ctlVerbPrivClass("CRIT_SET"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_ORDINARY, ctlVerbPrivClass("ENROLL_SUBMIT"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_ORDINARY, ctlVerbPrivClass("ENROLL_ACK"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass("ENROLL_TOKEN"));

    /* formerly-destructive verbs remain PRIVILEGED */
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass("DECOMMISSION"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass("REVOKE_CERT"));
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

static void test_decommission_confirmation_reuses_issued_token(void)
{
    serverCtl ctl;
    char reply[256];
    char first[] = "DECOMMISSION node=42 scope=0x03 op=alice";
    char confirm[] = "DECOMMISSION node=42 scope=0x03 op=alice confirm=1311768467463790320";

    memset(&ctl, 0, sizeof ctl);
    gDecommissionIssueCalls = 0;
    gDecommissionConfirmCalls = 0;

    (void)ctlHandleLine(&ctl, first, CTL_PEER_OPERATOR, 0, reply, sizeof reply);
    TEST_ASSERT_TRUE(strstr(reply, "confirm=1311768467463790320") != NULL);
    TEST_ASSERT_EQUAL_UINT(1, gDecommissionIssueCalls);
    TEST_ASSERT_EQUAL_UINT(0, gDecommissionConfirmCalls);

    (void)ctlHandleLine(&ctl, confirm, CTL_PEER_OPERATOR, 0, reply, sizeof reply);
    TEST_ASSERT_TRUE(strstr(reply, "accepted=1") != NULL);
    TEST_ASSERT_EQUAL_UINT(1, gDecommissionIssueCalls);
    TEST_ASSERT_EQUAL_UINT(1, gDecommissionConfirmCalls);
}

static void test_discovery_scan_rejected_on_standby(void)
{
    serverCtl ctl;
    serverContext ctx;
    char reply[256];
    char line[] = "DISCOVER cidr=10.0.0.0/24 op=alice";

    memset(&ctl, 0, sizeof ctl);
    memset(&ctx, 0, sizeof ctx);
    ctx.role = SRV_STANDBY;
    ctl.ctx = &ctx;

    (void)ctlHandleLine(&ctl, line, CTL_PEER_OPERATOR, 0, reply, sizeof reply);
    TEST_ASSERT_TRUE(strstr(reply, "ERR") != NULL);
    TEST_ASSERT_TRUE(strstr(reply, "active server required") != NULL);
}

static void test_discovery_adopt_rejected_on_standby_before_mutation(void)
{
    serverCtl ctl;
    serverContext ctx;
    char standbyReply[256];
    char activeReply[256];
    char standbyLine[] = "ADOPT disc=42 name=test-host op=alice";
    char activeLine[] = "ADOPT disc=42 name=test-host op=alice";

    memset(&ctl, 0, sizeof ctl);
    memset(&ctx, 0, sizeof ctx);
    ctx.role = SRV_STANDBY;
    ctl.ctx = &ctx;
    gAssetAdoptCalls = 0;

    (void)ctlHandleLine(&ctl, standbyLine, CTL_PEER_OPERATOR, 0,
                        standbyReply, sizeof standbyReply);
    TEST_ASSERT_TRUE(strstr(standbyReply, "active server required") != NULL);
    TEST_ASSERT_EQUAL_UINT(0, gAssetAdoptCalls);

    ctx.role = SRV_ACTIVE;
    (void)ctlHandleLine(&ctl, activeLine, CTL_PEER_OPERATOR, 0,
                        activeReply, sizeof activeReply);
    TEST_ASSERT_TRUE(strstr(activeReply, "assetId=424242") != NULL);
    TEST_ASSERT_EQUAL_UINT(1, gAssetAdoptCalls);
}

static void test_operator_bridge_standby_is_read_only(void)
{
    serverCtl ctl;
    serverContext ctx;
    char reply[256];
    char write[] = "ASSET_SET ip=10.0.0.9 name=standby-write";
    char ping[] = "PING";

    memset(&ctl, 0, sizeof ctl);
    memset(&ctx, 0, sizeof ctx);
    ctx.role = SRV_STANDBY;
    ctl.ctx = &ctx;

    (void)ctlHandleLine(&ctl, write, CTL_PEER_OPERATOR, 0, reply, sizeof reply);
    TEST_ASSERT_TRUE(strstr(reply, "active server required") != NULL);

    (void)ctlHandleLine(&ctl, ping, CTL_PEER_OPERATOR, 0, reply, sizeof reply);
    TEST_ASSERT_TRUE(strstr(reply, "OK pong") != NULL);
}

static void test_discovery_submits_durable_job_and_scopes_status(void)
{
    serverCtl ctl;
    serverContext ctx;
    char reply[512];
    char submit[] = "DISCOVER cidr=10.0.0.0/24 ports=22 op=alice";
    char status[] = "DISCOVER_STATUS scan=42 op=alice";

    memset(&ctl, 0, sizeof ctl);
    memset(&ctx, 0, sizeof ctx);
    ctx.role = SRV_ACTIVE;
    ctx.nodeId = 7;
    ctx.epoch = 9;
    ctl.ctx = &ctx;
    gScanSubmitCalls = 0;
    gScanRequester[0] = gScanStatusRequester[0] = '\0';

    (void)ctlHandleLine(&ctl, submit, CTL_PEER_OPERATOR, 0, reply, sizeof reply);
    TEST_ASSERT_TRUE(strstr(reply, "scan=42 status=queued hosts=254") != NULL);
    TEST_ASSERT_EQUAL_UINT(1, gScanSubmitCalls);
    TEST_ASSERT_EQUAL_STRING("alice", gScanRequester);

    (void)ctlHandleLine(&ctl, status, CTL_PEER_OPERATOR, 0, reply, sizeof reply);
    TEST_ASSERT_TRUE(strstr(reply, "status=completed") != NULL);
    TEST_ASSERT_TRUE(strstr(reply, "done=254 total=254 found=3") != NULL);
    TEST_ASSERT_EQUAL_STRING("alice", gScanStatusRequester);
}

static void test_enrollment_token_hash(void)
{
    char token[65], digest[65], fp[96];
    memset(token, 'a', 64);
    token[64] = '\0';
    TEST_ASSERT_TRUE(ctlHashPresentedEnrollmentToken(token, digest));
    TEST_ASSERT_EQUAL_INT(64, (int)strlen(digest));
    TEST_ASSERT_TRUE(strcmp(token, digest) != 0);
    token[10] = 'z';
    TEST_ASSERT_FALSE(ctlHashPresentedEnrollmentToken(token, digest));
    TEST_ASSERT_FALSE(ctlHashPresentedEnrollmentToken("short", digest));
    TEST_ASSERT_TRUE(ctlValidEnrollmentHost("node.akoria.net"));
    TEST_ASSERT_FALSE(ctlValidEnrollmentHost("node..akoria.net"));
    TEST_ASSERT_FALSE(ctlValidEnrollmentHost("node.akoria.net%00.evil"));
    memset(fp, 'a', 95);
    for (int i = 2; i < 95; i += 3) fp[i] = ':';
    fp[95] = '\0';
    TEST_ASSERT_TRUE(ctlValidEnrollmentFingerprint(fp));
    fp[2] = 'a';
    TEST_ASSERT_FALSE(ctlValidEnrollmentFingerprint(fp));
    TEST_ASSERT_TRUE(ctlVerbRequiresOperator("ENROLL_TOKEN"));
}

static void test_enrollment_token_acl(void)
{
    serverCtl ctl;
    char reply[256];
    char direct[] = "ENROLL_TOKEN op=web role=client host=node.akoria.net";
    char queued[] = "REQUEST_SUBMIT verb=ENROLL_TOKEN op=web";
    char scan[] = "DISCOVER cidr=10.0.0.0/24";
    memset(&ctl, 0, sizeof ctl);
    (void)ctlHandleLine(&ctl, scan, CTL_PEER_DASHBOARD, 33,
                        reply, sizeof reply);
    TEST_ASSERT_TRUE(strncmp(reply, "ERR", 3) == 0);
    TEST_ASSERT_NOT_NULL(strstr(reply, "privileged verb"));
    (void)ctlHandleLine(&ctl, direct, CTL_PEER_DASHBOARD, 33,
                        reply, sizeof reply);
    TEST_ASSERT_TRUE(strncmp(reply, "ERR", 3) == 0);
    TEST_ASSERT_NOT_NULL(strstr(reply, "privileged verb"));
    (void)ctlHandleLine(&ctl, queued, CTL_PEER_DASHBOARD, 33,
                        reply, sizeof reply);
    TEST_ASSERT_TRUE(strncmp(reply, "ERR", 3) == 0);
    TEST_ASSERT_NOT_NULL(strstr(reply, "issued directly to the operator"));
}

static void test_decommission_tokens_are_never_queueable(void)
{
    serverCtl ctl;
    char reply[256];
    char queued[] = "REQUEST_SUBMIT verb=DECOMMISSION op=alice args=node%3D42%20scope%3D0x03%20confirm%3D123";
    memset(&ctl, 0, sizeof ctl);
    (void)ctlHandleLine(&ctl, queued, CTL_PEER_DASHBOARD, 33,
                        reply, sizeof reply);
    TEST_ASSERT_TRUE(strncmp(reply, "ERR", 3) == 0);
    TEST_ASSERT_NOT_NULL(strstr(reply, "confirmation tokens may not be queued"));
}

static void test_ping_advertises_inline_decommission(void)
{
    serverCtl ctl;
    char reply[256];
    char ping[] = "PING";
    memset(&ctl, 0, sizeof ctl);
    (void)ctlHandleLine(&ctl, ping, CTL_PEER_DASHBOARD, 33,
                        reply, sizeof reply);
    TEST_ASSERT_NOT_NULL(strstr(reply, "decommissionInline=1"));
}

static void test_queued_decommission_confirms_without_persisting_token(void)
{
    serverCtl ctl;
    serverContext ctx;
    memset(&ctl, 0, sizeof ctl);
    memset(&ctx, 0, sizeof ctx);
    ctx.db = (serverDb *)1;
    ctx.role = SRV_ACTIVE;
    ctl.ctx = &ctx;
    gQueuedDecommissionReady = true;
    gQueuedDecommissionCompleted = false;
    gQueuedDecommissionResult[0] = '\0';
    gDecommissionIssueCalls = 0;
    gDecommissionConfirmCalls = 0;

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverCtlQueuePoll(&ctl));
    TEST_ASSERT_TRUE(gQueuedDecommissionCompleted);
    TEST_ASSERT_EQUAL_UINT(1, gDecommissionIssueCalls);
    TEST_ASSERT_EQUAL_UINT(1, gDecommissionConfirmCalls);
    TEST_ASSERT_NOT_NULL(strstr(gQueuedDecommissionResult, "accepted=1"));
    TEST_ASSERT_NULL(strstr(gQueuedDecommissionResult, "confirm="));
    TEST_ASSERT_NULL(strstr(gQueuedDecommissionResult, "1311768467463790320"));
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

static void test_request_stream_requires_complete_bounded_line(void)
{
    int fds[2];
    char request[CTL_REQ_CAP];
    char long_line[8192];
    memset(long_line, 'x', sizeof long_line);
    long_line[sizeof long_line - 1] = '\n';
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    TEST_ASSERT_EQUAL_INT(4, write(fds[0], "PING", 4));
    TEST_ASSERT_EQUAL_INT(1, write(fds[0], "\n", 1));
    TEST_ASSERT_EQUAL_INT(5, ctlReadLine(fds[1], request, sizeof request));
    TEST_ASSERT_EQUAL_STRING("PING\n", request);
    TEST_ASSERT_EQUAL_INT((int)sizeof long_line,
                          write(fds[0], long_line, sizeof long_line));
    TEST_ASSERT_EQUAL_INT((int)sizeof long_line,
                          ctlReadLine(fds[1], request, sizeof request));
    TEST_ASSERT_EQUAL_INT(0, shutdown(fds[0], SHUT_WR));
    close(fds[0]);
    close(fds[1]);

    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    TEST_ASSERT_EQUAL_INT(4, write(fds[0], "PING", 4));
    TEST_ASSERT_EQUAL_INT(0, shutdown(fds[0], SHUT_WR));
    TEST_ASSERT_EQUAL_INT(-1, ctlReadLine(fds[1], request, sizeof request));
    close(fds[0]);
    close(fds[1]);
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
    RUN_TEST(test_decommission_confirmation_reuses_issued_token);
    RUN_TEST(test_discovery_scan_rejected_on_standby);
    RUN_TEST(test_discovery_adopt_rejected_on_standby_before_mutation);
    RUN_TEST(test_operator_bridge_standby_is_read_only);
    RUN_TEST(test_discovery_submits_durable_job_and_scopes_status);
    RUN_TEST(test_enrollment_token_hash);
    RUN_TEST(test_enrollment_token_acl);
    RUN_TEST(test_decommission_tokens_are_never_queueable);
    RUN_TEST(test_ping_advertises_inline_decommission);
    RUN_TEST(test_queued_decommission_confirms_without_persisting_token);
    RUN_TEST(test_classify_peer);
    RUN_TEST(test_peer_may_invoke);
    RUN_TEST(test_pool_delete_guard);
    RUN_TEST(test_reply_format);
    RUN_TEST(test_looks_like_csr);
    RUN_TEST(test_request_stream_requires_complete_bounded_line);
    return UNITY_END();
}
