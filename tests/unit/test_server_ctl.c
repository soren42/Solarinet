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
#define CTL_READ_TIMEOUT_MS 150
/* Capture ctlSpawnDetached's argv/log instead of exec'ing (see ctlTestSpawn). */
#define SOLARI_CTL_TEST_SPAWN 1

#include "solariCtl.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static unsigned gDecommissionIssueCalls;
static unsigned gDecommissionConfirmCalls;
static unsigned gScanSubmitCalls;
static char gScanRequester[129];
static char gScanStatusRequester[129];
static unsigned gAssetAdoptCalls;
static bool gQueuedDecommissionReady;
static bool gQueuedDecommissionCompleted;
static char gQueuedDecommissionResult[256];

/* ctlSpawnDetached test seam: record what would have been exec'd. */
static unsigned gSpawnCalls;
static char gSpawnArgv[2048];      /* argv joined with '|' */
static char gSpawnLog[SERVER_PATH_MAX];

/* Hermetic helper tree + log dir (mkdtemp) every spawning test runs against:
 * ctlSpawnHelper's preflight (helper executable, log opens) runs for real. */
static char gFixture[128];
static char gHelperRoot[256];
static char gLogDir[256];
static const char *const kHelpers[] = {
    "deploy/remote-deploy.sh", "deploy/fleet/fleet-provision.sh",
    "deploy/fleet/fleet-image.sh", "deploy/fleet/image-refresh.py",
    "deploy/fleet/prov-stage.py", "deploy/fleet/stack-apply.py",
    "deploy/fleet/fleet-updates.py",
};

static void fixture_path(char *out, size_t cap, const char *rel)
{
    int n = snprintf(out, cap, "%s/%s", gHelperRoot, rel);
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < cap);
}

static void fixture_write_helper(const char *rel, mode_t mode)
{
    char path[512];
    FILE *f;
    fixture_path(path, sizeof path, rel);
    f = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs("#!/bin/sh\nexit 0\n", f);
    fclose(f);
    TEST_ASSERT_EQUAL_INT(0, chmod(path, mode));
}

static void fixture_setup(void)
{
    char dir[512];
    size_t i;
    snprintf(gFixture, sizeof gFixture, "/tmp/solari-ctl-helpers-XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(gFixture));
    snprintf(gHelperRoot, sizeof gHelperRoot, "%s/release", gFixture);
    snprintf(gLogDir, sizeof gLogDir, "%s/logs", gFixture);
    TEST_ASSERT_EQUAL_INT(0, mkdir(gHelperRoot, 0755));
    TEST_ASSERT_EQUAL_INT(0, mkdir(gLogDir, 0755));
    fixture_path(dir, sizeof dir, "deploy");
    TEST_ASSERT_EQUAL_INT(0, mkdir(dir, 0755));
    fixture_path(dir, sizeof dir, "deploy/fleet");
    TEST_ASSERT_EQUAL_INT(0, mkdir(dir, 0755));
    for (i = 0; i < sizeof kHelpers / sizeof kHelpers[0]; i++)
        fixture_write_helper(kHelpers[i], 0755);
}

static void fixture_teardown(void)
{
    char cmd[256];
    if (!gFixture[0]) return;
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", gFixture);
    (void)system(cmd);
}

/* Point a test serverCtl at the fixture (what serverCtlOpen resolves). */
static void ctl_use_fixture(serverCtl *ctl)
{
    snprintf(ctl->helperRoot, sizeof ctl->helperRoot, "%s", gHelperRoot);
    snprintf(ctl->logDir, sizeof ctl->logDir, "%s", gLogDir);
}

static solariStatus ctlTestSpawn(char *const argv[], const char *logPath)
{
    size_t o = 0;
    int i;
    gSpawnCalls++;
    gSpawnArgv[0] = '\0';
    for (i = 0; argv[i]; i++) {
        int n = snprintf(gSpawnArgv + o, sizeof gSpawnArgv - o, "%s%s",
                         i ? "|" : "", argv[i]);
        TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof gSpawnArgv - o);
        o += (size_t)n;
    }
    snprintf(gSpawnLog, sizeof gSpawnLog, "%s", logPath ? logPath : "");
    return SOLARI_OK;
}

/* REQUEST_SUBMIT capture (overrides the weak server_stubs.c stub). */
static unsigned gSubmitCalls;
static char gSubmitVerb[64];
static char gSubmitArgs[1024];
static char gSubmitOp[160];

solariStatus serverDbCtlRequestSubmit(serverDb *db, const char *verb,
    const char *args, const char *requestedBy, uint32_t peerUid,
    const char *idem, unsigned long long *reqOut)
{
    (void)db; (void)peerUid; (void)idem;
    gSubmitCalls++;
    snprintf(gSubmitVerb, sizeof gSubmitVerb, "%s", verb ? verb : "");
    snprintf(gSubmitArgs, sizeof gSubmitArgs, "%s", args ? args : "");
    snprintf(gSubmitOp, sizeof gSubmitOp, "%s", requestedBy ? requestedBy : "");
    if (reqOut) *reqOut = 9001;
    return SOLARI_OK;
}

/* Generic queued row for serverCtlQueuePoll replay tests. */
static bool gQueuedCustomReady;
static char gQueuedVerb[48];
static char gQueuedArgs[1024];
static char gQueuedRequester[128];

solariStatus serverDbCtlRequestClaim(serverDb *db, const char *claimedBy,
    unsigned long long *reqId, char *verb, size_t verbCap, char *args, size_t argsCap,
    char *requester, size_t requesterCap)
{
    (void)db; (void)claimedBy;
    if (gQueuedCustomReady) {
        gQueuedCustomReady = false;
        if (reqId) *reqId = 78;
        snprintf(verb, verbCap, "%s", gQueuedVerb);
        snprintf(args, argsCap, "%s", gQueuedArgs);
        snprintf(requester, requesterCap, "%s", gQueuedRequester);
        return SOLARI_OK;
    }
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

static long elapsed_ms(struct timespec a, struct timespec b)
{
    return (b.tv_sec - a.tv_sec) * 1000L + (b.tv_nsec - a.tv_nsec) / 1000000L;
}

static void test_ctl_read_eof_and_stall_deadline(void)
{
    int fd[2]; char out[32]; struct timespec a, b;
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
    (void)write(fd[0], "PING", 4); close(fd[0]);
    clock_gettime(CLOCK_MONOTONIC, &a);
    TEST_ASSERT_TRUE(ctlReadLine(fd[1], out, sizeof out) < 0);
    clock_gettime(CLOCK_MONOTONIC, &b);
    TEST_ASSERT_TRUE(elapsed_ms(a, b) < 500); close(fd[1]);

    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
    (void)write(fd[0], "PING", 4);
    clock_gettime(CLOCK_MONOTONIC, &a);
    TEST_ASSERT_TRUE(ctlReadLine(fd[1], out, sizeof out) < 0);
    clock_gettime(CLOCK_MONOTONIC, &b);
    TEST_ASSERT_TRUE(elapsed_ms(a, b) >= 100 && elapsed_ms(a, b) < 500);
    close(fd[0]); close(fd[1]);
}

typedef struct { int fd; size_t expected; } drainArgs;
static void *drain_slowly(void *opaque)
{
    drainArgs *args = opaque; size_t total = 0; char buf[37];
    while (total < args->expected) {
        ssize_t n = read(args->fd, buf, sizeof buf);
        if (n <= 0) break;
        total += (size_t)n;
        usleep(500);
    }
    return (void *)(uintptr_t)(total == args->expected);
}

static void test_ctl_write_partial_and_deadline(void)
{
    int fd[2], sndbuf = 1024; char payload[16384]; pthread_t thread;
    drainArgs args; void *result = NULL; struct timespec a, b;
    memset(payload, 'R', sizeof payload);
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
    TEST_ASSERT_EQUAL_INT(0, setsockopt(fd[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf));
    args.fd = fd[1]; args.expected = sizeof payload;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, drain_slowly, &args));
    TEST_ASSERT_EQUAL_INT(0, ctlWriteReply(fd[0], payload, sizeof payload, 1500));
    TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, &result));
    TEST_ASSERT_EQUAL_PTR((void *)(uintptr_t)1, result);
    close(fd[0]); close(fd[1]);

    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
    TEST_ASSERT_EQUAL_INT(0, setsockopt(fd[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf));
    clock_gettime(CLOCK_MONOTONIC, &a);
    TEST_ASSERT_TRUE(ctlWriteReply(fd[0], payload, sizeof payload, 150) < 0);
    clock_gettime(CLOCK_MONOTONIC, &b);
    TEST_ASSERT_TRUE(elapsed_ms(a, b) >= 100 && elapsed_ms(a, b) < 500);
    close(fd[0]); close(fd[1]);
}

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
    TEST_ASSERT_TRUE(ctlVerbIsDestructive("FLEET_UPDATES"));
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

    /* P3 provisioning verbs: destructive, operator-required, never on standby */
    TEST_ASSERT_TRUE(ctlVerbIsDestructive("IMAGE_REFRESH"));
    TEST_ASSERT_TRUE(ctlVerbIsDestructive("PROV_STAGE"));
    TEST_ASSERT_TRUE(ctlVerbIsDestructive("STACK_APPLY"));
    TEST_ASSERT_TRUE(ctlVerbIsDestructive("FLEET_UPDATES"));
    TEST_ASSERT_TRUE(ctlVerbRequiresOperator("IMAGE_REFRESH"));
    TEST_ASSERT_TRUE(ctlVerbRequiresOperator("PROV_STAGE"));
    TEST_ASSERT_TRUE(ctlVerbRequiresOperator("STACK_APPLY"));
    TEST_ASSERT_TRUE(ctlVerbRequiresOperator("FLEET_UPDATES"));
    TEST_ASSERT_FALSE(ctlVerbMayRunOnStandby("IMAGE_REFRESH"));
    TEST_ASSERT_FALSE(ctlVerbMayRunOnStandby("PROV_STAGE"));
    TEST_ASSERT_FALSE(ctlVerbMayRunOnStandby("STACK_APPLY"));
    TEST_ASSERT_FALSE(ctlVerbMayRunOnStandby("FLEET_UPDATES"));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE, ctlCheckRbac("IMAGE_REFRESH", "image=1", op, sizeof op));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE, ctlCheckRbac("PROV_STAGE", "entity=1 image=1 stacks=core op=", op, sizeof op));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE, ctlCheckRbac("STACK_APPLY", "entity=1 stacks=core dryRun=1", op, sizeof op));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE, ctlCheckRbac("FLEET_UPDATES", "run=77 mode=apply", op, sizeof op));
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
    TEST_ASSERT_EQUAL_INT(CTL_VC_ORDINARY, ctlVerbPrivClass("ASSET_CREATE"));
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
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass("IMAGE_REFRESH"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass("PROV_STAGE"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass("STACK_APPLY"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass("FLEET_UPDATES"));

    /* default-deny: an unknown / not-yet-invented verb is PRIVILEGED, not ordinary */
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass("SOME_NEW_VERB_2027"));
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED, ctlVerbPrivClass(""));
}

/* Tiny dependency-free reader for the manifest's flat ctlVerbs objects. */
static bool json_string_value(const char *object, const char *key,
                              char *out, size_t cap)
{
    char needle[64];
    const char *p;
    size_t n = 0;
    if (snprintf(needle, sizeof needle, "\"%s\":", key) >= (int)sizeof needle)
        return false;
    p = strstr(object, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (*p++ != '"') return false;
    while (*p && *p != '"') {
        if (n + 1 >= cap || *p == '\\') return false;
        out[n++] = *p++;
    }
    if (*p != '"') return false;
    out[n] = '\0';
    return true;
}

static bool json_bool_value(const char *object, const char *key, bool *out)
{
    char needle[64];
    const char *p;
    if (snprintf(needle, sizeof needle, "\"%s\":", key) >= (int)sizeof needle)
        return false;
    p = strstr(object, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "true", 4) == 0) { *out = true; return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

static void test_ctl_manifest_matches_classifier(void)
{
#ifndef AUTHZ_MANIFEST_PATH
#define AUTHZ_MANIFEST_PATH "docs/authz/AUTHZ_MANIFEST.json"
#endif
    FILE *fp = fopen(AUTHZ_MANIFEST_PATH, "rb");
    char *data, *section, *p;
    long size;
    unsigned checked = 0, sentinels = 0;
    TEST_ASSERT_NOT_NULL_MESSAGE(fp, "authorization manifest could not be opened");
    TEST_ASSERT_EQUAL_INT(0, fseek(fp, 0, SEEK_END));
    size = ftell(fp);
    TEST_ASSERT_TRUE(size > 0);
    rewind(fp);
    data = malloc((size_t)size + 1);
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_EQUAL_UINT((size_t)size, fread(data, 1, (size_t)size, fp));
    data[size] = '\0';
    fclose(fp);
    section = strstr(data, "\"ctlVerbs\"");
    TEST_ASSERT_NOT_NULL_MESSAGE(section, "manifest ctlVerbs section missing");
    p = strchr(section, '[');
    TEST_ASSERT_NOT_NULL(p);
    p++;
    for (;;) {
        char object[4096], verb[64], cls[32];
        const char *end;
        bool requiresOperator, destructive, mayRunOnStandby, inStr = false;
        size_t len;
        ctlVerbClass expectedClass;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
        if (*p == ']') break;                       /* end of the ctlVerbs array */
        TEST_ASSERT_EQUAL_CHAR_MESSAGE('{', *p, "manifest ctlVerbs: expected an object");
        /* Find the object's closing brace, skipping braces inside strings
         * (route patterns such as /api/x/{id} live in phpCallers). The
         * objects are flat: no nested objects, only string arrays. */
        for (end = p + 1; *end; end++) {
            if (inStr) {
                if (*end == '\\' && end[1]) end++;
                else if (*end == '"') inStr = false;
            } else if (*end == '"') {
                inStr = true;
            } else if (*end == '{') {
                TEST_FAIL_MESSAGE("manifest ctlVerbs: nested object");
            } else if (*end == '}') {
                break;
            }
        }
        TEST_ASSERT_EQUAL_CHAR_MESSAGE('}', *end, "manifest ctlVerbs: unterminated object");
        len = (size_t)(end - p + 1);
        TEST_ASSERT_TRUE_MESSAGE(len < sizeof object, "manifest ctl verb object too large");
        memcpy(object, p, len); object[len] = '\0';
        TEST_ASSERT_TRUE(json_string_value(object, "verb", verb, sizeof verb));
        TEST_ASSERT_TRUE(json_string_value(object, "class", cls, sizeof cls));
        TEST_ASSERT_TRUE(json_bool_value(object, "requiresOperator", &requiresOperator));
        TEST_ASSERT_TRUE(json_bool_value(object, "destructive", &destructive));
        TEST_ASSERT_TRUE(json_bool_value(object, "mayRunOnStandby", &mayRunOnStandby));
        if (strcmp(cls, "ordinary") == 0) expectedClass = CTL_VC_ORDINARY;
        else if (strcmp(cls, "enqueue") == 0) expectedClass = CTL_VC_ENQUEUE;
        else if (strcmp(cls, "privileged") == 0) expectedClass = CTL_VC_PRIVILEGED;
        else { TEST_FAIL_MESSAGE("manifest has unknown ctl class"); expectedClass = CTL_VC_PRIVILEGED; }
        TEST_ASSERT_EQUAL_INT_MESSAGE(expectedClass, ctlVerbPrivClass(verb), verb);
        TEST_ASSERT_EQUAL_INT_MESSAGE(requiresOperator, ctlVerbRequiresOperator(verb), verb);
        TEST_ASSERT_EQUAL_INT_MESSAGE(destructive, ctlVerbIsDestructive(verb), verb);
        TEST_ASSERT_EQUAL_INT_MESSAGE(mayRunOnStandby, ctlVerbMayRunOnStandby(verb), verb);
        if (strcmp(verb, "IMAGE_REFRESH") == 0 || strcmp(verb, "PROV_STAGE") == 0 ||
            strcmp(verb, "STACK_APPLY") == 0 || strcmp(verb, "FLEET_UPDATES") == 0 ||
            strcmp(verb, "REQUEST_GET") == 0 ||
            strcmp(verb, "REQUEST_SUBMIT") == 0)
            sentinels++;
        checked++;
        p = (char *)end + 1;
    }
    /* Every object was walked, including those after REQUEST_GET (whose
     * phpCallers carry {placeholders}) — a first-'}' reader stopped there. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(6, sentinels, "manifest walk missed a verb");
    TEST_ASSERT_TRUE_MESSAGE(checked > 0, "manifest has no ctl verb objects");
    TEST_ASSERT_EQUAL_INT(CTL_VC_PRIVILEGED,
        ctlVerbPrivClass("AUTHZ_MANIFEST_UNLISTED_VERB"));
    free(data);
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

/* enforcePeer=false must still keep PHP off the host: only root and the
 * operator uid are OPERATOR; every other uid is a DASHBOARD peer, which is
 * refused the privileged provisioning verbs but may still REQUEST_SUBMIT. */
static void test_classify_peer_legacy_fails_closed(void)
{
    const uint32_t opUid = 1000;
    const char *const priv[] = { "IMAGE_REFRESH", "PROV_STAGE", "STACK_APPLY" };
    TEST_ASSERT_EQUAL_INT(CTL_PEER_OPERATOR,  ctlClassifyPeerLegacy(0, opUid));
    TEST_ASSERT_EQUAL_INT(CTL_PEER_OPERATOR,  ctlClassifyPeerLegacy(opUid, opUid));
    TEST_ASSERT_EQUAL_INT(CTL_PEER_DASHBOARD, ctlClassifyPeerLegacy(33, opUid));
    for (size_t i = 0; i < sizeof priv / sizeof priv[0]; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(CTL_VC_PRIVILEGED, ctlVerbPrivClass(priv[i]), priv[i]);
        TEST_ASSERT_FALSE_MESSAGE(ctlPeerMayInvoke(ctlClassifyPeerLegacy(33, opUid),
                                                   ctlVerbPrivClass(priv[i])), priv[i]);
        TEST_ASSERT_TRUE_MESSAGE(ctlPeerMayInvoke(ctlClassifyPeerLegacy(opUid, opUid),
                                                  ctlVerbPrivClass(priv[i])), priv[i]);
    }
    TEST_ASSERT_TRUE(ctlPeerMayInvoke(ctlClassifyPeerLegacy(33, opUid),
                                      ctlVerbPrivClass("REQUEST_SUBMIT")));
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

static ssize_t read_fragmented(const char *bytes, size_t len, size_t split,
                               int bytewise, char *out, size_t cap)
{
    int fds[2];
    pid_t child;
    ssize_t got;
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    child = fork();
    TEST_ASSERT_TRUE(child >= 0);
    if (child == 0) {
        size_t off = 0;
        (void)signal(SIGPIPE, SIG_IGN);
        close(fds[1]);
        while (off < len) {
            size_t n = bytewise ? 1 : (off == 0 ? split : len - off);
            if (n > len - off) n = len - off;
            {
                ssize_t wrote = write(fds[0], bytes + off, n);
                if (wrote < 0 && errno == EPIPE) break;
                if (wrote != (ssize_t)n) _exit(2);
            }
            off += n;
            (void)usleep(1000); /* keep each test fragment independently readable */
        }
        shutdown(fds[0], SHUT_WR);
        close(fds[0]);
        _exit(0);
    }
    close(fds[0]);
    got = ctlReadLine(fds[1], out, cap);
    close(fds[1]);
    { int status = 0; waitpid(child, &status, 0); TEST_ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0); }
    return got;
}

static void test_ctl_line_byte_boundaries(void)
{
    static const char valid[] = "PING\r\n";
    char out[CTL_REQ_CAP];
    size_t split;
    for (split = 1; split < sizeof valid - 1; split++) {
        TEST_ASSERT_EQUAL_INT(sizeof valid - 1,
            read_fragmented(valid, sizeof valid - 1, split, 0, out, sizeof out));
        TEST_ASSERT_EQUAL_MEMORY(valid, out, sizeof valid - 1);
    }
    TEST_ASSERT_EQUAL_INT(sizeof valid - 1,
        read_fragmented(valid, sizeof valid - 1, 1, 1, out, sizeof out));

    {
        char oversized[CTL_REQ_CAP + 2];
        memset(oversized, 'x', sizeof oversized);
        oversized[sizeof oversized - 1] = '\n';
        TEST_ASSERT_EQUAL_INT(-1, read_fragmented(oversized, sizeof oversized,
            1, 1, out, sizeof out));
    }
    {
        const char nul_line[] = {'P','I','N','G','\0','x','\n'};
        TEST_ASSERT_EQUAL_INT(-1, read_fragmented(nul_line, sizeof nul_line,
            2, 0, out, sizeof out));
    }
    {
        const char partial[] = "PING";
        TEST_ASSERT_EQUAL_INT(-1, read_fragmented(partial, sizeof partial - 1,
            2, 0, out, sizeof out));
    }
}

/* ===================================================================== */
/* P3 provisioning verbs: IMAGE_REFRESH / PROV_STAGE / STACK_APPLY         */
/* ===================================================================== */

/* Run one line through the dispatcher as `peer`, with no ctx (role gate off). */
static void prov_call(const char *line, ctlPeerClass peer, char *reply, size_t cap)
{
    serverCtl ctl;
    char buf[CTL_REQ_CAP];
    memset(&ctl, 0, sizeof ctl);
    ctl_use_fixture(&ctl);
    TEST_ASSERT_TRUE(strlen(line) < sizeof buf);
    snprintf(buf, sizeof buf, "%s", line);
    gSpawnCalls = 0;
    gSpawnArgv[0] = '\0';
    gSpawnLog[0] = '\0';
    (void)ctlHandleLine(&ctl, buf, peer, peer == CTL_PEER_DASHBOARD ? 33 : 0,
                        reply, cap);
}

/* Assert `line` is refused with ERR <code> and nothing is spawned. */
static void prov_expect_err(const char *line, solariStatus code)
{
    char reply[512], prefix[32];
    prov_call(line, CTL_PEER_OPERATOR, reply, sizeof reply);
    snprintf(prefix, sizeof prefix, "ERR %d ", (int)code);
    if (strncmp(reply, prefix, strlen(prefix)) != 0 || gSpawnCalls != 0) {
        char msg[1200];
        snprintf(msg, sizeof msg, "line <%s> reply <%s> spawns=%u", line, reply, gSpawnCalls);
        TEST_FAIL_MESSAGE(msg);
    }
}

/* Assert `line` is accepted and execs exactly `argv` (|-joined) logging to `log`. */
/* argv[0] and log are given relative; the helper must run as
 * <helperRoot>/<argv0> and log to <logDir>/<log>. */
static void prov_expect_spawn(const char *line, const char *argv, const char *log,
                              const char *status)
{
    char reply[1024], want[1024], wantArgv[2048], wantLog[512];
    prov_call(line, CTL_PEER_OPERATOR, reply, sizeof reply);
    snprintf(wantArgv, sizeof wantArgv, "%s/%s", gHelperRoot, argv);
    snprintf(wantLog, sizeof wantLog, "%s/%s", gLogDir, log);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, gSpawnCalls, line);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(wantArgv, gSpawnArgv, line);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(wantLog, gSpawnLog, line);
    snprintf(want, sizeof want, "OK log=%s status=%s\n", wantLog, status);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, reply, line);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, access(wantLog, W_OK), line);   /* opened */
}

static bool ctlProvValidSlug(const char *s)
{
    return s && ctlProvValidSlugN(s, strlen(s));
}

static void test_prov_validators_pure(void)
{
    uint32_t v = 0;
    bool dry = true;
    char out[CTL_PROV_STACKS_CAP], line[4096];
    size_t o;
    int i;

    /* ids */
    TEST_ASSERT_TRUE(ctlProvValidId("1", &v));          TEST_ASSERT_EQUAL_UINT32(1, v);
    TEST_ASSERT_TRUE(ctlProvValidId("4294967295", &v)); TEST_ASSERT_EQUAL_UINT32(4294967295u, v);
    TEST_ASSERT_TRUE(ctlProvValidId("1000000000", &v));
    TEST_ASSERT_FALSE(ctlProvValidId("0", &v));
    TEST_ASSERT_FALSE(ctlProvValidId("-1", &v));
    TEST_ASSERT_FALSE(ctlProvValidId("+1", &v));
    TEST_ASSERT_FALSE(ctlProvValidId("01", &v));
    TEST_ASSERT_FALSE(ctlProvValidId("1e3", &v));
    TEST_ASSERT_FALSE(ctlProvValidId("4294967296", &v));
    TEST_ASSERT_FALSE(ctlProvValidId("9999999999", &v));
    TEST_ASSERT_FALSE(ctlProvValidId("10000000000", &v));
    TEST_ASSERT_FALSE(ctlProvValidId("12 3", &v));
    TEST_ASSERT_FALSE(ctlProvValidId(" 1", &v));
    TEST_ASSERT_FALSE(ctlProvValidId("1\n", &v));
    TEST_ASSERT_FALSE(ctlProvValidId("0x10", &v));
    TEST_ASSERT_FALSE(ctlProvValidId("", &v));
    TEST_ASSERT_FALSE(ctlProvValidId(NULL, &v));

    /* slugs */
    TEST_ASSERT_TRUE(ctlProvValidSlug("core"));
    TEST_ASSERT_TRUE(ctlProvValidSlug("dev-tools"));
    TEST_ASSERT_TRUE(ctlProvValidSlug("0"));
    TEST_ASSERT_TRUE(ctlProvValidSlug("a-"));
    TEST_ASSERT_TRUE(ctlProvValidSlug("abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuv"));   /* 48 */
    TEST_ASSERT_FALSE(ctlProvValidSlug("abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvw")); /* 49 */
    TEST_ASSERT_FALSE(ctlProvValidSlug(""));
    TEST_ASSERT_FALSE(ctlProvValidSlug("-core"));
    TEST_ASSERT_FALSE(ctlProvValidSlug("Core"));
    TEST_ASSERT_FALSE(ctlProvValidSlug("dev_tools"));
    TEST_ASSERT_FALSE(ctlProvValidSlug(".."));
    TEST_ASSERT_FALSE(ctlProvValidSlug("a/b"));
    TEST_ASSERT_FALSE(ctlProvValidSlug("a;b"));
    TEST_ASSERT_FALSE(ctlProvValidSlug("$(id)"));
    TEST_ASSERT_FALSE(ctlProvValidSlug("`id`"));
    TEST_ASSERT_FALSE(ctlProvValidSlug("a b"));
    TEST_ASSERT_FALSE(ctlProvValidSlug("a\nb"));
    TEST_ASSERT_FALSE(ctlProvValidSlug("a%20b"));

    /* stacks: normalized (dedupe, order kept) */
    TEST_ASSERT_TRUE(ctlProvValidStacks("core", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("core", out);
    TEST_ASSERT_TRUE(ctlProvValidStacks("web,core,web,ai,core", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("web,core,ai", out);
    TEST_ASSERT_TRUE(ctlProvValidStacks("a,ab,a", out, sizeof out));     /* prefix is not a dup */
    TEST_ASSERT_EQUAL_STRING("a,ab", out);
    TEST_ASSERT_FALSE(ctlProvValidStacks("", out, sizeof out));
    TEST_ASSERT_FALSE(ctlProvValidStacks("core,", out, sizeof out));
    TEST_ASSERT_FALSE(ctlProvValidStacks(",core", out, sizeof out));
    TEST_ASSERT_FALSE(ctlProvValidStacks("core,,web", out, sizeof out));
    TEST_ASSERT_FALSE(ctlProvValidStacks(",", out, sizeof out));
    TEST_ASSERT_FALSE(ctlProvValidStacks("core,Web", out, sizeof out));
    TEST_ASSERT_FALSE(ctlProvValidStacks("core;web", out, sizeof out));
    TEST_ASSERT_FALSE(ctlProvValidStacks("core web", out, sizeof out));
    /* 32 distinct accepted; 33 distinct refused; 33 with a duplicate collapses */
    for (o = 0, i = 0; i < 33; i++)
        o += (size_t)snprintf(line + o, sizeof line - o, "%ss%d", i ? "," : "", i);
    TEST_ASSERT_FALSE(ctlProvValidStacks(line, out, sizeof out));
    *strrchr(line, ',') = '\0';
    TEST_ASSERT_TRUE(ctlProvValidStacks(line, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING(line, out);
    strcat(line, ",s0");
    TEST_ASSERT_TRUE(ctlProvValidStacks(line, out, sizeof out));
    /* 32 maximal slugs fill the normalized buffer exactly */
    for (o = 0, i = 0; i < 32; i++)
        o += (size_t)snprintf(line + o, sizeof line - o, "%s%02d%s", i ? "," : "", i,
                              "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrst");
    TEST_ASSERT_EQUAL_UINT(CTL_PROV_STACKS_CAP - 1, strlen(line));
    TEST_ASSERT_TRUE(ctlProvValidStacks(line, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING(line, out);

    /* dryRun */
    TEST_ASSERT_TRUE(ctlProvValidDryRun("0", &dry));  TEST_ASSERT_FALSE(dry);
    TEST_ASSERT_TRUE(ctlProvValidDryRun("1", &dry));  TEST_ASSERT_TRUE(dry);
    TEST_ASSERT_FALSE(ctlProvValidDryRun("2", &dry));
    TEST_ASSERT_FALSE(ctlProvValidDryRun("yes", &dry));
    TEST_ASSERT_FALSE(ctlProvValidDryRun("", &dry));
    TEST_ASSERT_FALSE(ctlProvValidDryRun("01", &dry));
    TEST_ASSERT_FALSE(ctlProvValidDryRun("true", &dry));

    /* principal */
    TEST_ASSERT_TRUE(ctlProvValidPrincipal("alice"));
    TEST_ASSERT_TRUE(ctlProvValidPrincipal("jason@akoria.org"));
    TEST_ASSERT_TRUE(ctlProvValidPrincipal("svc:kc+ops.1_x-y"));
    TEST_ASSERT_FALSE(ctlProvValidPrincipal(""));
    TEST_ASSERT_FALSE(ctlProvValidPrincipal(".alice"));
    TEST_ASSERT_FALSE(ctlProvValidPrincipal("-alice"));
    TEST_ASSERT_FALSE(ctlProvValidPrincipal("al ice"));
    TEST_ASSERT_FALSE(ctlProvValidPrincipal("al;ice"));
    TEST_ASSERT_FALSE(ctlProvValidPrincipal("al/ice"));
    TEST_ASSERT_FALSE(ctlProvValidPrincipal("al\nice"));
    TEST_ASSERT_FALSE(ctlProvValidPrincipal("$(id)"));
    memset(line, 'a', 128); line[128] = '\0';
    TEST_ASSERT_TRUE(ctlProvValidPrincipal(line));
    line[128] = 'a'; line[129] = '\0';
    TEST_ASSERT_FALSE(ctlProvValidPrincipal(line));

    /* mac: exactly provlib.MAC_RE (lowercase, colon) */
    TEST_ASSERT_TRUE(ctlProvValidMac("aa:bb:cc:dd:ee:ff"));
    TEST_ASSERT_TRUE(ctlProvValidMac("00:11:22:33:44:55"));
    TEST_ASSERT_TRUE(ctlProvValidMac("0a:1b:2c:3d:4e:5f"));
    TEST_ASSERT_FALSE(ctlProvValidMac("AA:BB:CC:DD:EE:FF"));     /* uppercase */
    TEST_ASSERT_FALSE(ctlProvValidMac("aa:bb:cc:dd:ee:FF"));
    TEST_ASSERT_FALSE(ctlProvValidMac("aa-bb-cc-dd-ee-ff"));     /* hyphen */
    TEST_ASSERT_FALSE(ctlProvValidMac("aabb.ccdd.eeff"));
    TEST_ASSERT_FALSE(ctlProvValidMac("aabbccddeeff"));
    TEST_ASSERT_FALSE(ctlProvValidMac("aa:bb:cc:dd:ee"));        /* short */
    TEST_ASSERT_FALSE(ctlProvValidMac("aa:bb:cc:dd:ee:f"));
    TEST_ASSERT_FALSE(ctlProvValidMac("aa:bb:cc:dd:ee:ff:00"));  /* extra */
    TEST_ASSERT_FALSE(ctlProvValidMac("aa:bb:cc:dd:ee:ff "));
    TEST_ASSERT_FALSE(ctlProvValidMac("aa:bb:cc:dd:ee:ff\n"));
    TEST_ASSERT_FALSE(ctlProvValidMac(" aa:bb:cc:dd:ee:ff"));
    TEST_ASSERT_FALSE(ctlProvValidMac("gg:bb:cc:dd:ee:ff"));
    TEST_ASSERT_FALSE(ctlProvValidMac("aa::bb:cc:dd:ee:f"));
    TEST_ASSERT_FALSE(ctlProvValidMac("aa:bb:cc:dd:ee;ff"));
    TEST_ASSERT_FALSE(ctlProvValidMac(""));
    TEST_ASSERT_FALSE(ctlProvValidMac(NULL));

    /* arch: exactly provlib.ARCHES / prov_images.arch */
    TEST_ASSERT_TRUE(ctlProvValidArch("x86_64"));
    TEST_ASSERT_TRUE(ctlProvValidArch("arm64"));
    TEST_ASSERT_TRUE(ctlProvValidArch("armhf"));
    TEST_ASSERT_TRUE(ctlProvValidArch("riscv64"));
    TEST_ASSERT_TRUE(ctlProvValidArch("i386"));
    TEST_ASSERT_FALSE(ctlProvValidArch("amd64"));     /* PHP normalises agent spellings */
    TEST_ASSERT_FALSE(ctlProvValidArch("aarch64"));
    TEST_ASSERT_FALSE(ctlProvValidArch("X86_64"));
    TEST_ASSERT_FALSE(ctlProvValidArch("x86_64 "));
    TEST_ASSERT_FALSE(ctlProvValidArch("arm"));
    TEST_ASSERT_FALSE(ctlProvValidArch("unknown"));
    TEST_ASSERT_FALSE(ctlProvValidArch(""));
    TEST_ASSERT_FALSE(ctlProvValidArch(NULL));
}

static void test_prov_strict_decode_refuses_embedded_nul(void)
{
    char out[32];
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, ctlArgStrStrict("a=5 b=x", "a", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("5", out);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, ctlArgStrStrict("op=a%2Bb", "op", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("a+b", out);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, ctlArgStrStrict("op=50%", "op", out, sizeof out));   /* malformed % kept */
    TEST_ASSERT_EQUAL_STRING("50%", out);
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG, ctlArgStrStrict("a=5%00x", "a", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG, ctlArgStrStrict("a=%00", "a", out, sizeof out));
    TEST_ASSERT_EQUAL_INT(ERR_TLV_END, ctlArgStrStrict("a=1", "b", out, sizeof out));
    /* first match wins, like ctlArgStr */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, ctlArgStrStrict("a=1 a=%00", "a", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("1", out);
}

static void test_prov_verbs_exec_exact_argv(void)
{
    prov_expect_spawn("IMAGE_REFRESH image=7 op=alice",
        "deploy/fleet/image-refresh.py|--image|7|--op|alice",
        "image-refresh-7.log", "refreshing");
    prov_expect_spawn("IMAGE_REFRESH op=alice image=4294967295 junk=ignored",
        "deploy/fleet/image-refresh.py|--image|4294967295|--op|alice",
        "image-refresh-4294967295.log", "refreshing");
    prov_expect_spawn("PROV_STAGE entity=42 image=3 stacks=core,dev-tools mac=aa:bb:cc:dd:ee:ff arch=x86_64 op=jason@akoria.org",
        "deploy/fleet/prov-stage.py|--entity|42|--image|3|--stacks|core,dev-tools|--mac|aa:bb:cc:dd:ee:ff|--arch|x86_64|--op|jason@akoria.org",
        "prov-stage-42.log", "staging");
    /* duplicates collapse before reaching the helper */
    prov_expect_spawn("PROV_STAGE entity=42 image=3 stacks=web,core,web mac=aa:bb:cc:dd:ee:ff arch=x86_64 op=alice",
        "deploy/fleet/prov-stage.py|--entity|42|--image|3|--stacks|web,core|--mac|aa:bb:cc:dd:ee:ff|--arch|x86_64|--op|alice",
        "prov-stage-42.log", "staging");
    /* pct-encoded principal is decoded once (queue replay encodes requestedBy) */
    prov_expect_spawn("PROV_STAGE entity=42 image=3 stacks=core mac=aa:bb:cc:dd:ee:ff arch=x86_64 op=a%2Bb",
        "deploy/fleet/prov-stage.py|--entity|42|--image|3|--stacks|core|--mac|aa:bb:cc:dd:ee:ff|--arch|x86_64|--op|a+b",
        "prov-stage-42.log", "staging");
    prov_expect_spawn("STACK_APPLY entity=9 stacks=core dryRun=1 op=alice",
        "deploy/fleet/stack-apply.py|--entity|9|--stacks|core|--dry-run|--op|alice",
        "stack-apply-9.log", "applying");
    prov_expect_spawn("STACK_APPLY entity=9 stacks=core,web dryRun=0 op=alice",
        "deploy/fleet/stack-apply.py|--entity|9|--stacks|core,web|--op|alice",
        "stack-apply-9.log", "applying");
}

static void test_fleet_updates_exec_exact_argv(void)
{
    char reply[1024], buf[CTL_REQ_CAP], wantArgv[512], wantLog[512];
    serverCtl ctl;
    memset(&ctl, 0, sizeof ctl);
    ctl_use_fixture(&ctl);
    snprintf(buf, sizeof buf, "%s", "FLEET_UPDATES run=77 mode=check op=alice");
    gSpawnCalls = 0; gSpawnArgv[0] = '\0'; gSpawnLog[0] = '\0';
    (void)ctlHandleLine(&ctl, buf, CTL_PEER_OPERATOR, 0, reply, sizeof reply);
    snprintf(wantArgv, sizeof wantArgv, "%s/deploy/fleet/fleet-updates.py|--mode|check|--op|alice", gHelperRoot);
    snprintf(wantLog, sizeof wantLog, "%s/fleet-updates-77.log", gLogDir);
    TEST_ASSERT_EQUAL_UINT(1, gSpawnCalls);
    TEST_ASSERT_EQUAL_STRING(wantArgv, gSpawnArgv);
    TEST_ASSERT_EQUAL_STRING(wantLog, gSpawnLog);
    TEST_ASSERT_EQUAL_STRING("OK status=started\n", reply);

    prov_expect_err("FLEET_UPDATES run=77 mode=--help op=alice", ERR_INVALID_ARG);
    prov_expect_err("FLEET_UPDATES run=../x mode=check op=alice", ERR_INVALID_ARG);
    prov_expect_err("FLEET_UPDATES mode=check op=alice", ERR_INVALID_ARG);
    prov_expect_err("FLEET_UPDATES run=77 mode=apply", ERR_AUTH_ROLE);
}

static void test_prov_verbs_refuse_bad_args(void)
{
    static const char *const badIds[] = {
        "0", "-1", "01", "1e3", "4294967296", "12%203", "+5", "0x1", "",
        "1%00", "1%0a", "1%3b", "%31%30%00", "1;id", "1$(id)", "1`id`", "ab"
    };
    static const char *const badSlugs[] = {
        "", "Core", "co_re", "..", "a/b", "a;b", "$(id)", "`id`", "a%20b", "a%0ab",
        "a%3bb", "a%00", "core%2c", "%2ccore", "a%2fb", "-core", "core%0d",
        "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvw", "core,", ",core",
        "core,,web", "a%24%28id%29", "a%60id%60"
    };
    static const char *const badDry[] = { "2", "yes", "", "true", "01", "1%00", "%31%0a", "-1" };
    static const char *const badOps[] = {
        "%20alice", "al%20ice", "alice%0a", "alice%3bid", "alice%00x", ".alice",
        "-alice", "al/ice", "%24%28id%29", "a%60id%60"
    };
    char line[8192];
    size_t o, i;

    /* RBAC: missing / empty op -> ERR_AUTH_ROLE before any validation */
    prov_expect_err("IMAGE_REFRESH image=1", ERR_AUTH_ROLE);
    prov_expect_err("IMAGE_REFRESH image=1 op=", ERR_AUTH_ROLE);
    prov_expect_err("PROV_STAGE entity=1 image=1 stacks=core", ERR_AUTH_ROLE);
    prov_expect_err("PROV_STAGE entity=1 image=1 stacks=core mac=aa:bb:cc:dd:ee:ff arch=x86_64 op=", ERR_AUTH_ROLE);
    prov_expect_err("STACK_APPLY entity=1 stacks=core dryRun=0", ERR_AUTH_ROLE);
    prov_expect_err("STACK_APPLY entity=1 stacks=core dryRun=0 op=", ERR_AUTH_ROLE);

    /* missing args */
    prov_expect_err("IMAGE_REFRESH op=alice", ERR_INVALID_ARG);
    prov_expect_err("PROV_STAGE image=1 stacks=core mac=aa:bb:cc:dd:ee:ff arch=x86_64 op=alice", ERR_INVALID_ARG);
    prov_expect_err("PROV_STAGE entity=1 stacks=core mac=aa:bb:cc:dd:ee:ff arch=x86_64 op=alice", ERR_INVALID_ARG);
    prov_expect_err("PROV_STAGE entity=1 image=1 mac=aa:bb:cc:dd:ee:ff arch=x86_64 op=alice", ERR_INVALID_ARG);
    prov_expect_err("STACK_APPLY stacks=core dryRun=0 op=alice", ERR_INVALID_ARG);
    prov_expect_err("STACK_APPLY entity=1 dryRun=0 op=alice", ERR_INVALID_ARG);
    prov_expect_err("STACK_APPLY entity=1 stacks=core op=alice", ERR_INVALID_ARG);

    for (i = 0; i < sizeof badIds / sizeof badIds[0]; i++) {
        snprintf(line, sizeof line, "IMAGE_REFRESH image=%s op=alice", badIds[i]);
        prov_expect_err(line, ERR_INVALID_ARG);
        snprintf(line, sizeof line, "PROV_STAGE entity=%s image=1 stacks=core mac=aa:bb:cc:dd:ee:ff arch=x86_64 op=alice", badIds[i]);
        prov_expect_err(line, ERR_INVALID_ARG);
        snprintf(line, sizeof line, "PROV_STAGE entity=1 image=%s stacks=core mac=aa:bb:cc:dd:ee:ff arch=x86_64 op=alice", badIds[i]);
        prov_expect_err(line, ERR_INVALID_ARG);
        snprintf(line, sizeof line, "STACK_APPLY entity=%s stacks=core dryRun=0 op=alice", badIds[i]);
        prov_expect_err(line, ERR_INVALID_ARG);
    }
    for (i = 0; i < sizeof badSlugs / sizeof badSlugs[0]; i++) {
        snprintf(line, sizeof line, "PROV_STAGE entity=1 image=1 stacks=%s mac=aa:bb:cc:dd:ee:ff arch=x86_64 op=alice", badSlugs[i]);
        prov_expect_err(line, ERR_INVALID_ARG);
        snprintf(line, sizeof line, "STACK_APPLY entity=1 stacks=core,%s dryRun=1 op=alice", badSlugs[i]);
        prov_expect_err(line, ERR_INVALID_ARG);
    }
    for (i = 0; i < sizeof badDry / sizeof badDry[0]; i++) {
        snprintf(line, sizeof line, "STACK_APPLY entity=1 stacks=core dryRun=%s op=alice", badDry[i]);
        prov_expect_err(line, ERR_INVALID_ARG);
    }
    for (i = 0; i < sizeof badOps / sizeof badOps[0]; i++) {
        snprintf(line, sizeof line, "IMAGE_REFRESH image=1 op=%s", badOps[i]);
        prov_expect_err(line, ERR_INVALID_ARG);
        snprintf(line, sizeof line, "PROV_STAGE entity=1 image=1 stacks=core mac=aa:bb:cc:dd:ee:ff arch=x86_64 op=%s", badOps[i]);
        prov_expect_err(line, ERR_INVALID_ARG);
        snprintf(line, sizeof line, "STACK_APPLY entity=1 stacks=core dryRun=0 op=%s", badOps[i]);
        prov_expect_err(line, ERR_INVALID_ARG);
    }

    /* 33 distinct stacks */
    o = (size_t)snprintf(line, sizeof line, "PROV_STAGE entity=1 image=1 mac=aa:bb:cc:dd:ee:ff arch=x86_64 op=alice stacks=");
    for (i = 0; i < 33; i++)
        o += (size_t)snprintf(line + o, sizeof line - o, "%ss%u", i ? "," : "", (unsigned)i);
    prov_expect_err(line, ERR_INVALID_ARG);
    /* an oversized raw stacks value is refused, not truncated */
    o = (size_t)snprintf(line, sizeof line, "STACK_APPLY entity=1 dryRun=0 op=alice stacks=core");
    while (o + 6 < CTL_PROV_STACKS_RAW + 64)
        o += (size_t)snprintf(line + o, sizeof line - o, ",core");
    prov_expect_err(line, ERR_INVALID_ARG);
}

/* PROV_STAGE carries the operator-picked MAC and the server-resolved arch to
 * the helper verbatim; anything outside MAC_RE / ARCHES is refused unspawned. */
static void test_prov_stage_mac_arch(void)
{
    static const char *const badMacs[] = {
        "AA:BB:CC:DD:EE:FF", "aa:bb:cc:dd:ee:FF", "aa-bb-cc-dd-ee-ff", "aabbccddeeff",
        "aa:bb:cc:dd:ee", "aa:bb:cc:dd:ee:ff:00", "aa:bb:cc:dd:ee:f", "gg:bb:cc:dd:ee:ff",
        "aa:bb:cc:dd:ee:ff%00", "aa:bb:cc:dd:ee:ff%0a", "aa:bb:cc:dd:ee:ff%20x",
        "aa:bb:cc:dd:ee:ff;id", "aa%3Abb", "", "%24%28id%29",
    };
    static const char *const badArches[] = {
        "amd64", "aarch64", "X86_64", "arm", "unknown", "", "x86_64%00", "x86_64%20",
        "x86_64;id", "riscv", "i686", "%24%28id%29",
    };
    char line[512];
    size_t i;

    prov_expect_spawn("PROV_STAGE entity=7 image=2 stacks=core mac=dc:a6:32:01:02:03 arch=arm64 op=alice",
        "deploy/fleet/prov-stage.py|--entity|7|--image|2|--stacks|core|--mac|dc:a6:32:01:02:03|--arch|arm64|--op|alice",
        "prov-stage-7.log", "staging");
    /* a percent-encoded value (any replayed/encoded frame) decodes exactly once */
    prov_expect_spawn("PROV_STAGE entity=7 image=2 stacks=core mac=dc%3Aa6%3A32%3A01%3A02%3A03 arch=riscv64 op=alice",
        "deploy/fleet/prov-stage.py|--entity|7|--image|2|--stacks|core|--mac|dc:a6:32:01:02:03|--arch|riscv64|--op|alice",
        "prov-stage-7.log", "staging");
    /* order of keys is irrelevant; first occurrence wins */
    prov_expect_spawn("PROV_STAGE op=alice arch=i386 mac=00:11:22:33:44:55 stacks=core image=2 entity=7 mac=AA:BB:CC:DD:EE:FF",
        "deploy/fleet/prov-stage.py|--entity|7|--image|2|--stacks|core|--mac|00:11:22:33:44:55|--arch|i386|--op|alice",
        "prov-stage-7.log", "staging");

    /* both are mandatory */
    prov_expect_err("PROV_STAGE entity=1 image=1 stacks=core arch=x86_64 op=alice", ERR_INVALID_ARG);
    prov_expect_err("PROV_STAGE entity=1 image=1 stacks=core mac=aa:bb:cc:dd:ee:ff op=alice", ERR_INVALID_ARG);
    prov_expect_err("PROV_STAGE entity=1 image=1 stacks=core op=alice", ERR_INVALID_ARG);
    for (i = 0; i < sizeof badMacs / sizeof badMacs[0]; i++) {
        char reply[512];
        snprintf(line, sizeof line, "PROV_STAGE entity=1 image=1 stacks=core mac=%s arch=x86_64 op=alice", badMacs[i]);
        prov_expect_err(line, ERR_INVALID_ARG);
        prov_call(line, CTL_PEER_OPERATOR, reply, sizeof reply);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(reply, "invalid mac"), line);
    }
    for (i = 0; i < sizeof badArches / sizeof badArches[0]; i++) {
        char reply[512];
        snprintf(line, sizeof line, "PROV_STAGE entity=1 image=1 stacks=core mac=aa:bb:cc:dd:ee:ff arch=%s op=alice", badArches[i]);
        prov_expect_err(line, ERR_INVALID_ARG);
        prov_call(line, CTL_PEER_OPERATOR, reply, sizeof reply);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(reply, "invalid arch"), line);
    }
}

static void test_prov_verbs_dashboard_must_queue(void)
{
    static const char *const direct[] = {
        "IMAGE_REFRESH image=1 op=alice",
        "PROV_STAGE entity=1 image=1 stacks=core mac=aa:bb:cc:dd:ee:ff arch=x86_64 op=alice",
        "STACK_APPLY entity=1 stacks=core dryRun=1 op=alice",
    };
    static const char *const inner[] = { "IMAGE_REFRESH", "PROV_STAGE", "STACK_APPLY" };
    static const char *const wire[] = {
        "image%3D1",
        "entity%3D1%20image%3D1%20stacks%3Dcore",
        "entity%3D1%20stacks%3Dcore%20dryRun%3D1",
    };
    static const char *const stored[] = {
        "image=1",
        "entity=1 image=1 stacks=core",
        "entity=1 stacks=core dryRun=1",
    };
    char reply[512], line[512];
    size_t i;
    serverContext ctx;
    serverCtl ctl;

    for (i = 0; i < 3; i++) {
        /* direct call from the dashboard peer: refused at the peer ACL */
        prov_call(direct[i], CTL_PEER_DASHBOARD, reply, sizeof reply);
        TEST_ASSERT_EQUAL_INT(0, strncmp(reply, "ERR", 3));
        TEST_ASSERT_NOT_NULL(strstr(reply, "privileged verb must be queued via REQUEST_SUBMIT"));
        TEST_ASSERT_EQUAL_UINT(0, gSpawnCalls);
        /* an unknown peer gets nothing either */
        prov_call(direct[i], CTL_PEER_UNKNOWN, reply, sizeof reply);
        TEST_ASSERT_EQUAL_INT(0, strncmp(reply, "ERR", 3));
        TEST_ASSERT_EQUAL_UINT(0, gSpawnCalls);

        /* REQUEST_SUBMIT of the verb is accepted as PRIVILEGED and only enqueued */
        memset(&ctx, 0, sizeof ctx);
        memset(&ctl, 0, sizeof ctl);
        ctx.role = SRV_ACTIVE;
        ctx.db = (serverDb *)1;
        ctl.ctx = &ctx;
        gSubmitCalls = 0;
        gSpawnCalls = 0;
        snprintf(line, sizeof line, "REQUEST_SUBMIT verb=%s op=alice args=%s idem=k%u",
                 inner[i], wire[i], (unsigned)i);
        (void)ctlHandleLine(&ctl, line, CTL_PEER_DASHBOARD, 33, reply, sizeof reply);
        TEST_ASSERT_EQUAL_STRING("OK request=9001 status=pending\n", reply);
        TEST_ASSERT_EQUAL_UINT(1, gSubmitCalls);
        TEST_ASSERT_EQUAL_UINT(0, gSpawnCalls);
        TEST_ASSERT_EQUAL_STRING(inner[i], gSubmitVerb);
        TEST_ASSERT_EQUAL_STRING(stored[i], gSubmitArgs);
        TEST_ASSERT_EQUAL_STRING("alice", gSubmitOp);
    }

    /* standby: refused before any effect even for the operator peer */
    memset(&ctx, 0, sizeof ctx);
    memset(&ctl, 0, sizeof ctl);
    ctx.role = SRV_STANDBY;
    ctl.ctx = &ctx;
    for (i = 0; i < 3; i++) {
        snprintf(line, sizeof line, "%s", direct[i]);
        gSpawnCalls = 0;
        (void)ctlHandleLine(&ctl, line, CTL_PEER_OPERATOR, 0, reply, sizeof reply);
        TEST_ASSERT_NOT_NULL(strstr(reply, "active server required"));
        TEST_ASSERT_EQUAL_UINT(0, gSpawnCalls);
    }
}

/* The queue consumer binds op from the stored requester, never from an op= the
 * dashboard smuggled into the queued args. */
static void test_prov_queue_replay_binds_requester(void)
{
    serverCtl ctl;
    serverContext ctx;
    memset(&ctl, 0, sizeof ctl);
    memset(&ctx, 0, sizeof ctx);
    ctx.db = (serverDb *)1;
    ctx.role = SRV_ACTIVE;
    ctl.ctx = &ctx;
    ctl_use_fixture(&ctl);

    gQueuedCustomReady = true;
    snprintf(gQueuedVerb, sizeof gQueuedVerb, "STACK_APPLY");
    snprintf(gQueuedArgs, sizeof gQueuedArgs,
             "op=mallory entity=12 stacks=core,web dryRun=0");
    snprintf(gQueuedRequester, sizeof gQueuedRequester, "jason+ops@akoria.org");
    gSpawnCalls = 0;
    gQueuedDecommissionCompleted = false;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverCtlQueuePoll(&ctl));
    TEST_ASSERT_EQUAL_UINT(1, gSpawnCalls);
    {
        char want[1024];
        snprintf(want, sizeof want, "%s/deploy/fleet/stack-apply.py|--entity|12|"
                 "--stacks|core,web|--op|jason+ops@akoria.org", gHelperRoot);
        TEST_ASSERT_EQUAL_STRING(want, gSpawnArgv);
        TEST_ASSERT_TRUE(gQueuedDecommissionCompleted);
        snprintf(want, sizeof want, "OK log=%s/stack-apply-12.log status=applying", gLogDir);
        TEST_ASSERT_EQUAL_STRING(want, gQueuedDecommissionResult);
    }

    /* PROV_STAGE replays from the queue; an encoded mac decodes exactly once */
    gQueuedCustomReady = true;
    snprintf(gQueuedVerb, sizeof gQueuedVerb, "PROV_STAGE");
    snprintf(gQueuedArgs, sizeof gQueuedArgs,
             "entity=12 image=3 stacks=core mac=aa%%3Abb%%3Acc%%3Add%%3Aee%%3Aff arch=arm64");
    snprintf(gQueuedRequester, sizeof gQueuedRequester, "alice");
    gSpawnCalls = 0;
    gQueuedDecommissionCompleted = false;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverCtlQueuePoll(&ctl));
    TEST_ASSERT_EQUAL_UINT(1, gSpawnCalls);
    {
        char want[1024];
        snprintf(want, sizeof want, "%s/deploy/fleet/prov-stage.py|--entity|12|--image|3|"
                 "--stacks|core|--mac|aa:bb:cc:dd:ee:ff|--arch|arm64|--op|alice", gHelperRoot);
        TEST_ASSERT_EQUAL_STRING(want, gSpawnArgv);
        TEST_ASSERT_TRUE(gQueuedDecommissionCompleted);
    }

    /* a bad queued arg fails the row cleanly and spawns nothing */
    gQueuedCustomReady = true;
    snprintf(gQueuedVerb, sizeof gQueuedVerb, "PROV_STAGE");
    snprintf(gQueuedArgs, sizeof gQueuedArgs, "entity=12 image=3 stacks=core%%3Bid");
    snprintf(gQueuedRequester, sizeof gQueuedRequester, "alice");
    gSpawnCalls = 0;
    gQueuedDecommissionCompleted = true;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverCtlQueuePoll(&ctl));
    TEST_ASSERT_EQUAL_UINT(0, gSpawnCalls);
    TEST_ASSERT_FALSE(gQueuedDecommissionCompleted);
    TEST_ASSERT_NOT_NULL(strstr(gQueuedDecommissionResult, "invalid stacks"));
}

/* ===================================================================== */
/* Helper root / log dir resolution and the fail-loud spawn preflight      */
/* ===================================================================== */

static void test_helper_path_primitives(void)
{
    char out[64], small[8];
    TEST_ASSERT_TRUE(ctlJoinPath(out, sizeof out, "/a", "b/c"));
    TEST_ASSERT_EQUAL_STRING("/a/b/c", out);
    TEST_ASSERT_TRUE(ctlJoinPath(out, sizeof out, "/", "x"));
    TEST_ASSERT_EQUAL_STRING("/x", out);
    TEST_ASSERT_FALSE(ctlJoinPath(out, sizeof out, "", "x"));
    TEST_ASSERT_FALSE(ctlJoinPath(small, sizeof small, "/abcd", "efgh"));
    TEST_ASSERT_EQUAL_STRING("", small);

    ctlDirName("/run/solari/solariCtl.sock", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("/run/solari", out);
    ctlDirName("run/solariCtl.sock", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("run", out);
    ctlDirName("solariCtl.sock", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING(".", out);
    ctlDirName("/sock", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("/", out);

    snprintf(out, sizeof out, "run");
    TEST_ASSERT_TRUE(ctlAbsolutize(out, sizeof out, "/repo"));
    TEST_ASSERT_EQUAL_STRING("/repo/run", out);
    snprintf(out, sizeof out, "/abs");
    TEST_ASSERT_TRUE(ctlAbsolutize(out, sizeof out, "/repo"));
    TEST_ASSERT_EQUAL_STRING("/abs", out);
    snprintf(out, sizeof out, ".");
    TEST_ASSERT_TRUE(ctlAbsolutize(out, sizeof out, "/repo"));
    TEST_ASSERT_EQUAL_STRING("/repo", out);
    snprintf(out, sizeof out, "run");
    TEST_ASSERT_FALSE(ctlAbsolutize(out, sizeof out, ""));       /* no cwd */
}

static void test_helper_paths_resolve_defaults_and_config(void)
{
    char root[SERVER_PATH_MAX], logd[SERVER_PATH_MAX], link[512], real[PATH_MAX];

    /* production defaults: root = cwd, logs beside the ctl socket */
    TEST_ASSERT_TRUE(ctlResolveHelperPaths("", "", "/run/solari/solariCtl.sock",
                                           "/nonexistent/rel-a", root, sizeof root,
                                           logd, sizeof logd));
    TEST_ASSERT_EQUAL_STRING("/nonexistent/rel-a", root);
    TEST_ASSERT_EQUAL_STRING("/run/solari", logd);

    /* dev checkout: relative ctlSocket run/solariCtl.sock -> <repo>/run */
    TEST_ASSERT_TRUE(ctlResolveHelperPaths(NULL, NULL, "run/solariCtl.sock",
                                           "/nonexistent/repo", root, sizeof root,
                                           logd, sizeof logd));
    TEST_ASSERT_EQUAL_STRING("/nonexistent/repo", root);
    TEST_ASSERT_EQUAL_STRING("/nonexistent/repo/run", logd);

    /* explicit [ctl] keys win; relative ones anchor at cwd */
    TEST_ASSERT_TRUE(ctlResolveHelperPaths("helpers", "/var/lib/solarinet/run",
                                           "/run/solari/solariCtl.sock", "/nonexistent/c",
                                           root, sizeof root, logd, sizeof logd));
    TEST_ASSERT_EQUAL_STRING("/nonexistent/c/helpers", root);
    TEST_ASSERT_EQUAL_STRING("/var/lib/solarinet/run", logd);

    /* a `current` symlink is pinned to the release it points at */
    snprintf(link, sizeof link, "%s/current", gFixture);
    TEST_ASSERT_EQUAL_INT(0, symlink("release", link));
    TEST_ASSERT_TRUE(ctlResolveHelperPaths(link, "", "/run/solari/solariCtl.sock", "/",
                                           root, sizeof root, logd, sizeof logd));
    TEST_ASSERT_NOT_NULL(realpath(gHelperRoot, real));
    TEST_ASSERT_EQUAL_STRING(real, root);
    TEST_ASSERT_EQUAL_INT(0, unlink(link));

    /* unusable: relative root with no cwd */
    TEST_ASSERT_FALSE(ctlResolveHelperPaths("rel", "", "/run/solari/s.sock", "",
                                            root, sizeof root, logd, sizeof logd));
    TEST_ASSERT_EQUAL_STRING("", root);
}

static void test_helper_env_overrides_socket_and_ca(void)
{
    serverCtl ctl;
    ctlHelperEnv he;
    char *parent[] = {
        (char *)"PATH=/usr/bin", (char *)"SOLARI_CTL_SOCK=/stale.sock",
        (char *)"HOME=/home/x", (char *)"SOLARI_CA_PEM=/stale.pem",
        (char *)"SOLARI_CTL_SOCKET=keep", NULL
    };
    memset(&ctl, 0, sizeof ctl);
    snprintf(ctl.helperSock, sizeof ctl.helperSock, "/run/solari/solariCtl.sock");
    snprintf(ctl.caFile, sizeof ctl.caFile, "/var/lib/solarinet/pki/ca.pem");
    TEST_ASSERT_TRUE(ctlHelperEnvBuild(&ctl, parent, &he));
    TEST_ASSERT_EQUAL_STRING("PATH=/usr/bin", he.envp[0]);
    TEST_ASSERT_EQUAL_STRING("HOME=/home/x", he.envp[1]);
    TEST_ASSERT_EQUAL_STRING("SOLARI_CTL_SOCKET=keep", he.envp[2]);
    TEST_ASSERT_EQUAL_STRING("SOLARI_CTL_SOCK=/run/solari/solariCtl.sock", he.envp[3]);
    TEST_ASSERT_EQUAL_STRING("SOLARI_CA_PEM=/var/lib/solarinet/pki/ca.pem", he.envp[4]);
    TEST_ASSERT_NULL(he.envp[5]);
    ctlHelperEnvFree(&he);

    /* no CA configured: the stale value is still dropped, none is added */
    ctl.caFile[0] = '\0';
    TEST_ASSERT_TRUE(ctlHelperEnvBuild(&ctl, parent, &he));
    TEST_ASSERT_EQUAL_STRING("SOLARI_CTL_SOCK=/run/solari/solariCtl.sock", he.envp[3]);
    TEST_ASSERT_NULL(he.envp[4]);
    ctlHelperEnvFree(&he);

    TEST_ASSERT_TRUE(ctlHelperEnvBuild(&ctl, NULL, &he));
    TEST_ASSERT_EQUAL_STRING("SOLARI_CTL_SOCK=/run/solari/solariCtl.sock", he.envp[0]);
    TEST_ASSERT_NULL(he.envp[1]);
    ctlHelperEnvFree(&he);
}

static void test_fleet_and_deploy_verbs_exec_from_helper_root(void)
{
    prov_expect_spawn("DEPLOY host=nas.akoria.net arch=arm64 op=alice",
        "deploy/remote-deploy.sh|--host|nas.akoria.net|--arch|arm64|--op|alice",
        "deploy-nas.akoria.net.log", "deploying");
    prov_expect_spawn("FLEET_PROVISION target=aa:bb:cc:dd:ee:ff distro=debian arch=x86_64 "
                      "hostname=h1 op=alice",
        "deploy/fleet/fleet-provision.sh|--target|aa:bb:cc:dd:ee:ff|--distro|debian|"
        "--arch|x86_64|--hostname|h1|--op|alice",
        "provision-h1.log", "provisioning");
    prov_expect_spawn("FLEET_IMAGE hostname=pi1 arch=arm64 op=alice",
        "deploy/fleet/fleet-image.sh|--hostname|pi1|--arch|arm64|--distro|raspios|--op|alice",
        "image-pi1.log", "imaging");
}

/* Assert line answers ERR -70 naming `needle` and spawns nothing. */
static void helper_expect_refusal(const char *line, const char *needle)
{
    char reply[1024], msg[1400];
    prov_call(line, CTL_PEER_OPERATOR, reply, sizeof reply);
    snprintf(msg, sizeof msg, "line <%s> reply <%s>", line, reply);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(reply, "ERR -70 ", 8), msg);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(reply, needle), msg);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, gSpawnCalls, msg);
    TEST_ASSERT_NULL_MESSAGE(strstr(reply, gFixture), msg);   /* no abs paths */
}

static void test_missing_helper_fails_loudly(void)
{
    static const struct { const char *line, *helper; } cases[] = {
        { "DEPLOY host=nas op=alice", "deploy/remote-deploy.sh" },
        { "FLEET_PROVISION target=t distro=debian arch=x86_64 hostname=h op=alice",
          "deploy/fleet/fleet-provision.sh" },
        { "FLEET_IMAGE hostname=pi1 arch=arm64 op=alice", "deploy/fleet/fleet-image.sh" },
        { "IMAGE_REFRESH image=7 op=alice", "deploy/fleet/image-refresh.py" },
        { "PROV_STAGE entity=4 image=3 stacks=core mac=aa:bb:cc:dd:ee:ff arch=x86_64 op=alice", "deploy/fleet/prov-stage.py" },
        { "STACK_APPLY entity=4 stacks=core dryRun=1 op=alice", "deploy/fleet/stack-apply.py" },
    };
    char path[512], want[160];
    size_t i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        snprintf(want, sizeof want, "helper not installed or not executable: %s",
                 cases[i].helper);
        fixture_path(path, sizeof path, cases[i].helper);
        /* present but not executable */
        TEST_ASSERT_EQUAL_INT(0, chmod(path, 0644));
        helper_expect_refusal(cases[i].line, want);
        /* absent */
        TEST_ASSERT_EQUAL_INT(0, unlink(path));
        helper_expect_refusal(cases[i].line, want);
        /* a directory in its place */
        TEST_ASSERT_EQUAL_INT(0, mkdir(path, 0755));
        helper_expect_refusal(cases[i].line, want);
        TEST_ASSERT_EQUAL_INT(0, rmdir(path));
        fixture_write_helper(cases[i].helper, 0755);
    }
}

static void test_unwritable_log_dir_fails_loudly(void)
{
    char saved[sizeof gLogDir];
    memcpy(saved, gLogDir, sizeof saved);
    snprintf(gLogDir, sizeof gLogDir, "%s/no-such-dir", gFixture);
    helper_expect_refusal("IMAGE_REFRESH image=7 op=alice",
                          "helper log not writable: image-refresh-7.log");
    helper_expect_refusal("STACK_APPLY entity=4 stacks=core dryRun=0 op=alice",
                          "helper log not writable: stack-apply-4.log");
    memcpy(gLogDir, saved, sizeof saved);

    /* a symlinked log name is refused (O_NOFOLLOW), not followed */
    {
        char link[512];
        snprintf(link, sizeof link, "%s/prov-stage-5.log", gLogDir);
        TEST_ASSERT_EQUAL_INT(0, symlink("/etc/passwd", link));
        helper_expect_refusal("PROV_STAGE entity=5 image=3 stacks=core mac=aa:bb:cc:dd:ee:ff arch=x86_64 op=alice",
                              "helper log not writable: prov-stage-5.log");
        TEST_ASSERT_EQUAL_INT(0, unlink(link));
    }
}

/* Helper logs are handed to the ctl socket group (the dashboard reads them via
 * solari-ctl membership, not the server's primary group) at mode 0640. */
static void test_helper_log_group_is_socket_gid(void)
{
    serverCtl ctl;
    char line[128], reply[512], log[512];
    struct stat sb;
    gid_t groups[64];
    int n = getgroups(64, groups), i;
    gid_t want = getegid();
    for (i = 0; i < n; i++)                       /* prefer a non-primary group */
        if (groups[i] != getegid()) { want = groups[i]; break; }
    memset(&ctl, 0, sizeof ctl);
    ctl_use_fixture(&ctl);
    ctl.socketGid = (uint32_t)want;
    snprintf(line, sizeof line, "IMAGE_REFRESH image=9 op=alice");
    gSpawnCalls = 0;
    (void)ctlHandleLine(&ctl, line, CTL_PEER_OPERATOR, 0, reply, sizeof reply);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(reply, "OK ", 3), reply);
    snprintf(log, sizeof log, "%s/image-refresh-9.log", gLogDir);
    TEST_ASSERT_EQUAL_INT(0, stat(log, &sb));
    TEST_ASSERT_EQUAL_UINT((unsigned)want, (unsigned)sb.st_gid);
    TEST_ASSERT_EQUAL_UINT(0640u, (unsigned)(sb.st_mode & 07777));
}

static void test_unconfigured_helper_root_fails_loudly(void)
{
    serverCtl ctl;
    char line[128], reply[512];
    memset(&ctl, 0, sizeof ctl);                 /* never opened: no root */
    snprintf(line, sizeof line, "PROV_STAGE entity=4 image=3 stacks=core mac=aa:bb:cc:dd:ee:ff arch=x86_64 op=alice");
    gSpawnCalls = 0;
    (void)ctlHandleLine(&ctl, line, CTL_PEER_OPERATOR, 0, reply, sizeof reply);
    TEST_ASSERT_EQUAL_INT(0, strncmp(reply, "ERR -70 ", 8));
    TEST_ASSERT_NOT_NULL(strstr(reply, "helper root/log dir not configured"));
    TEST_ASSERT_EQUAL_UINT(0, gSpawnCalls);
}

/* A queued privileged verb whose helper is missing fails its ctl_requests row
 * with the inner ERR line (the PHP poller surfaces it) and spawns nothing. */
static void test_queued_verb_with_missing_helper_fails_row(void)
{
    serverCtl ctl;
    serverContext ctx;
    char path[512];
    memset(&ctl, 0, sizeof ctl);
    memset(&ctx, 0, sizeof ctx);
    ctx.db = (serverDb *)1;
    ctx.role = SRV_ACTIVE;
    ctl.ctx = &ctx;
    ctl_use_fixture(&ctl);
    fixture_path(path, sizeof path, "deploy/fleet/image-refresh.py");
    TEST_ASSERT_EQUAL_INT(0, unlink(path));

    gQueuedCustomReady = true;
    snprintf(gQueuedVerb, sizeof gQueuedVerb, "IMAGE_REFRESH");
    snprintf(gQueuedArgs, sizeof gQueuedArgs, "image=7");
    snprintf(gQueuedRequester, sizeof gQueuedRequester, "alice");
    gSpawnCalls = 0;
    gQueuedDecommissionCompleted = true;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverCtlQueuePoll(&ctl));
    TEST_ASSERT_EQUAL_UINT(0, gSpawnCalls);
    TEST_ASSERT_FALSE(gQueuedDecommissionCompleted);
    TEST_ASSERT_NOT_NULL(strstr(gQueuedDecommissionResult,
        "ERR -70 helper not installed or not executable: deploy/fleet/image-refresh.py"));
    fixture_write_helper("deploy/fleet/image-refresh.py", 0755);
}

int main(void)
{
    int rc;
    fixture_setup();
    UNITY_BEGIN();
    RUN_TEST(test_trim_line);
    RUN_TEST(test_arg_str);
    RUN_TEST(test_arg_numeric);
    RUN_TEST(test_split_verb);
    RUN_TEST(test_rbac);
    RUN_TEST(test_verb_priv_class);
    RUN_TEST(test_ctl_manifest_matches_classifier);
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
    RUN_TEST(test_classify_peer_legacy_fails_closed);
    RUN_TEST(test_peer_may_invoke);
    RUN_TEST(test_pool_delete_guard);
    RUN_TEST(test_reply_format);
    RUN_TEST(test_looks_like_csr);
    RUN_TEST(test_request_stream_requires_complete_bounded_line);
    RUN_TEST(test_ctl_line_byte_boundaries);
    RUN_TEST(test_ctl_read_eof_and_stall_deadline);
    RUN_TEST(test_ctl_write_partial_and_deadline);
    RUN_TEST(test_prov_validators_pure);
    RUN_TEST(test_prov_strict_decode_refuses_embedded_nul);
    RUN_TEST(test_prov_verbs_exec_exact_argv);
    RUN_TEST(test_fleet_updates_exec_exact_argv);
    RUN_TEST(test_prov_verbs_refuse_bad_args);
    RUN_TEST(test_prov_stage_mac_arch);
    RUN_TEST(test_prov_verbs_dashboard_must_queue);
    RUN_TEST(test_prov_queue_replay_binds_requester);
    RUN_TEST(test_helper_path_primitives);
    RUN_TEST(test_helper_paths_resolve_defaults_and_config);
    RUN_TEST(test_helper_env_overrides_socket_and_ca);
    RUN_TEST(test_fleet_and_deploy_verbs_exec_from_helper_root);
    RUN_TEST(test_missing_helper_fails_loudly);
    RUN_TEST(test_unwritable_log_dir_fails_loudly);
    RUN_TEST(test_unconfigured_helper_root_fails_loudly);
    RUN_TEST(test_helper_log_group_is_socket_gid);
    RUN_TEST(test_queued_verb_with_missing_helper_fails_row);
    rc = UNITY_END();
    fixture_teardown();
    return rc;
}
