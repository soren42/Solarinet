/* Scan parsing, subprocess cancellation, and worker progress tests. */
#include "unity.h"

#include "serverScan.c"

/* Exercise the actual worker callback with a deterministic clock and DB spy.
 * The renamed service entry point is linked but never invoked by this test. */
static uint64_t g_workerNowMs;
static uint64_t scanTestNowMs(void) { return g_workerNowMs; }
#define solariNowUnixMs scanTestNowMs
#define main scanWorkerUnusedMain
#include "scanWorkerMain.c"
#undef main
#undef solariNowUnixMs

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

void setUp(void) {}
void tearDown(void) {}

static uint64_t g_leaseHolder = 42;
static uint64_t g_leaseEpoch = 1;
static unsigned g_fencedWrites;
static unsigned g_progressCalls;
static const char *g_toolMarker;
static unsigned g_progressWrites;

void serverConfigDefaults(serverConfig *out) { (void)out; }
solariStatus serverConfigFromFile(const char *path, serverConfig *out)
{ (void)path; (void)out; return ERR_PLATFORM; }
solariStatus serverContextInit(serverContext *ctx, const serverConfig *cfg)
{ (void)ctx; (void)cfg; return ERR_PLATFORM; }
void serverContextClose(serverContext *ctx) { (void)ctx; }
solariStatus serverDbDiscoveryScanClaim(serverDb *db, uint64_t owner,
                                        uint64_t epoch, serverDiscoveryScanJob *out)
{ (void)db; (void)owner; (void)epoch; (void)out; return ERR_CONN_RETRY; }
solariStatus serverDbDiscoveryScanUpdate(serverDb *db, uint64_t scanId,
    uint64_t owner, uint64_t epoch, const char *status, uint32_t done,
    uint32_t found, int32_t errorCode, const char *errorText)
{
    (void)db; (void)scanId; (void)owner; (void)epoch; (void)status;
    (void)done; (void)found; (void)errorCode; (void)errorText;
    g_progressWrites++;
    return SOLARI_OK;
}

solariStatus serverDbLeaseRead(serverDb *db, uint64_t *holder, uint64_t *epoch)
{
    (void)db;
    if (holder) *holder = g_leaseHolder;
    if (epoch) *epoch = g_leaseEpoch;
    return SOLARI_OK;
}

solariStatus serverDbUpsertDiscoveredFenced(serverDb *db,
    const serverDiscEntity *entity, uint64_t when, uint64_t owner,
    uint64_t epoch, uint64_t *discId)
{
    (void)db; (void)entity; (void)when; (void)owner; (void)epoch;
    g_fencedWrites++;
    if (discId) *discId = g_fencedWrites;
    return SOLARI_OK;
}

static solariStatus loseLeaseAfterFirstBatch(void *userData, uint32_t done,
                                             uint32_t found)
{
    (void)userData; (void)done; (void)found;
    g_progressCalls++;
    g_leaseHolder = 43;
    return SOLARI_OK;
}

static solariStatus cancelAfterFirstBatch(void *userData, uint32_t done,
                                          uint32_t found)
{
    (void)userData; (void)done; (void)found;
    g_progressCalls++;
    return ERR_PLATFORM;
}

static solariStatus cancelWhenFakeToolStarts(void *userData, uint32_t done,
                                             uint32_t found)
{
    FILE *marker;
    long pid = -1;
    char extra;
    int parsed;
    (void)userData; (void)done; (void)found;
    g_progressCalls++;
    marker = fopen(g_toolMarker, "r");
    if (!marker) return SOLARI_OK;
    parsed = fscanf(marker, "%ld %c", &pid, &extra);
    fclose(marker);
    if (parsed == 1 && pid > 1 && (long)(pid_t)pid == pid)
        return ERR_PLATFORM;
    return SOLARI_OK;
}

static void test_worker_throttles_unchanged_enrichment_progress(void)
{
    serverContext ctx = {0};
    serverDiscoveryScanJob job = {0};
    workerProgress progress = {0};
    ctx.nodeId = g_leaseHolder = 42;
    ctx.epoch = g_leaseEpoch = 1;
    job.hostsTotal = 10;
    progress.ctx = &ctx;
    progress.job = &job;
    progress.lastDone = 9;
    progress.lastWriteMs = progress.lastLeaseCheckMs = g_workerNowMs = 10000;
    g_stop = 0;
    g_progressWrites = 0;

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, updateProgress(&progress, 10, 0));
    TEST_ASSERT_EQUAL_UINT32(1, g_progressWrites);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, updateProgress(&progress, 10, 0));
    g_workerNowMs += 999;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, updateProgress(&progress, 10, 0));
    TEST_ASSERT_EQUAL_UINT32(1, g_progressWrites);
    g_workerNowMs++;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, updateProgress(&progress, 10, 0));
    TEST_ASSERT_EQUAL_UINT32(2, g_progressWrites);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, updateProgress(&progress, 10, 1));
    TEST_ASSERT_EQUAL_UINT32(3, g_progressWrites);
}

static void test_cancel_requires_complete_positive_pid_marker(void)
{
    char markerPath[] = "/tmp/solari-scan-marker.XXXXXX";
    const char *values[] = { "", "not-a-pid", "0\n", "-1\n", "123oops\n", "123\n" };
    solariStatus observed[6];
    int fd = mkstemp(markerPath);
    size_t i;
    int ioOk = 1;
    TEST_ASSERT_TRUE(fd >= 0);
    g_toolMarker = markerPath;
    for (i = 0; i < sizeof values / sizeof values[0]; i++) {
        size_t len = strlen(values[i]);
        if (ftruncate(fd, 0) || lseek(fd, 0, SEEK_SET) < 0 ||
            write(fd, values[i], len) != (ssize_t)len) ioOk = 0;
        observed[i] = cancelWhenFakeToolStarts(NULL, 0, 0);
    }
    close(fd);
    unlink(markerPath);
    g_toolMarker = NULL;
    TEST_ASSERT_TRUE(ioOk);
    for (i = 0; i < 5; i++) TEST_ASSERT_EQUAL_INT(SOLARI_OK, observed[i]);
    TEST_ASSERT_EQUAL_INT(ERR_PLATFORM, observed[5]);
}

static int processStopped(pid_t pid)
{
    char path[64], statLine[512], *state;
    unsigned attempt;
    snprintf(path, sizeof path, "/proc/%ld/stat", (long)pid);
    for (attempt = 0; attempt < 100; attempt++) {
        FILE *proc = fopen(path, "r");
        if (!proc) return 1;
        if (!fgets(statLine, sizeof statLine, proc)) { fclose(proc); return 0; }
        fclose(proc);
        state = strrchr(statLine, ')');
        if (state && state[1] == ' ' && state[2] == 'Z') return 1;
        usleep(10000);
    }
    return 0;
}

static void test_cidr_excludes_network_and_broadcast(void)
{
    uint32_t first = 0, count = 0;
    uint8_t prefix = 0;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        scanParseCidr("10.12.0.12/24", &first, &count, &prefix));
    TEST_ASSERT_EQUAL_UINT32(0x0A0C0001u, first);
    TEST_ASSERT_EQUAL_UINT32(254u, count);
    TEST_ASSERT_EQUAL_UINT8(24u, prefix);
}

static void test_small_cidr_ranges(void)
{
    uint32_t first = 0, count = 0;
    uint8_t prefix = 0;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        scanParseCidr("172.16.0.6/31", &first, &count, &prefix));
    TEST_ASSERT_EQUAL_UINT32(0xAC100006u, first);
    TEST_ASSERT_EQUAL_UINT32(2u, count);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        scanParseCidr("172.31.255.254/31", &first, &count, &prefix));
    TEST_ASSERT_EQUAL_UINT32(0xAC1FFFFEu, first);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        scanParseCidr("192.168.2.9/32", &first, &count, &prefix));
    TEST_ASSERT_EQUAL_UINT32(0xC0A80209u, first);
    TEST_ASSERT_EQUAL_UINT32(1u, count);
}

static void test_cidr_rejects_malformed_or_unbounded_ranges(void)
{
    uint32_t first = 0, count = 0;
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG,
        scanParseCidr("192.0.2.0/24tail", &first, &count, NULL));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG,
        scanParseCidr("192.0.2.0/", &first, &count, NULL));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG,
        scanParseCidr("192.0.2.0/24/1", &first, &count, NULL));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG,
        scanParseCidr("0.0.0.0/0", &first, &count, NULL));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG,
        scanParseCidr("192.0.2.0/24", &first, &count, NULL));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG,
        scanParseCidr("10.0.0.0/7", &first, &count, NULL));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG,
        scanParseCidr("172.32.0.0/24", &first, &count, NULL));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG,
        scanParseCidr("192.168.0.0/15", &first, &count, NULL));
}

static void test_ports_are_strict_and_bounded(void)
{
    int ports[SCAN_MAX_PORTS];
    TEST_ASSERT_EQUAL_INT(3, scanParsePorts("22,80,443", ports));
    TEST_ASSERT_EQUAL_INT(22, ports[0]);
    TEST_ASSERT_EQUAL_INT(80, ports[1]);
    TEST_ASSERT_EQUAL_INT(443, ports[2]);
    TEST_ASSERT_EQUAL_INT(0, scanParsePorts("22,,80", ports));
    TEST_ASSERT_EQUAL_INT(0, scanParsePorts("22x,80", ports));
    TEST_ASSERT_EQUAL_INT(0, scanParsePorts("0,80", ports));
    TEST_ASSERT_EQUAL_INT(0, scanParsePorts("65536", ports));
    TEST_ASSERT_EQUAL_INT(0, scanParsePorts("22,", ports));
    TEST_ASSERT_EQUAL_INT(0, scanParsePorts("22,22", ports));
}

static void test_scan_plan_validates_before_admission(void)
{
    uint32_t total = 0;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverScanPlan("10.0.0.0/24", "22,443", &total));
    TEST_ASSERT_EQUAL_UINT32(254, total);
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG,
        serverScanPlan("10.0.0.0/24", "22,,443", &total));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG,
        serverScanPlan("10.0.0.0/19", "22", &total));
}

static void test_lease_loss_between_batches_stops_before_result_writes(void)
{
    serverContext ctx;
    char ports[256] = {0};
    size_t off = 0, found = 99;
    int port;
    solariStatus rc;

    memset(&ctx, 0, sizeof ctx);
    ctx.db = (serverDb *)1;
    ctx.role = SRV_ACTIVE;
    ctx.nodeId = 42;
    ctx.epoch = 1;
    g_leaseHolder = ctx.nodeId;
    g_leaseEpoch = ctx.epoch;
    g_fencedWrites = 0;
    g_progressCalls = 0;

    for (port = 1; port <= 64; port++) {
        int n = snprintf(ports + off, sizeof ports - off, "%s%d",
                         port == 1 ? "" : ",", port);
        TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof ports - off);
        off += (size_t)n;
    }

    rc = serverScanRunProgress(&ctx, "127.0.0.0/24", ports, &found,
                               loseLeaseAfterFirstBatch, NULL);
    TEST_ASSERT_EQUAL_INT(ERR_LEASE_LOST, rc);
    TEST_ASSERT_EQUAL_UINT32(1, g_progressCalls);
    TEST_ASSERT_EQUAL_UINT32(0, g_fencedWrites);
    TEST_ASSERT_EQUAL_UINT32(0, found);
}

static void test_cooperative_cancel_after_batch_skips_result_writes(void)
{
    serverContext ctx;
    size_t found = 99;

    memset(&ctx, 0, sizeof ctx);
    ctx.db = (serverDb *)1;
    ctx.role = SRV_ACTIVE;
    ctx.nodeId = 42;
    ctx.epoch = 1;
    g_leaseHolder = ctx.nodeId;
    g_leaseEpoch = ctx.epoch;
    g_fencedWrites = 0;
    g_progressCalls = 0;

    TEST_ASSERT_EQUAL_INT(ERR_PLATFORM,
        serverScanRunProgress(&ctx, "127.0.0.1/32", "1", &found,
                              cancelAfterFirstBatch, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, g_progressCalls);
    TEST_ASSERT_EQUAL_UINT32(0, g_fencedWrites);
    TEST_ASSERT_EQUAL_UINT32(0, found);
}

static void test_cancel_terminates_enrichment_process_group(void)
{
    serverContext ctx;
    struct sockaddr_in addr;
    socklen_t addrLen = sizeof addr;
    char dirTemplate[] = "/tmp/solari-scan-tool.XXXXXX";
    char *dir = mkdtemp(dirTemplate);
    char toolPath[256], markerPath[256], argsPath[256], script[768], portText[16];
    char pathValue[512];
    char argsText[1024] = {0}, expectedPorts[32];
    const char *pathEnv = getenv("PATH");
    char *oldPath = pathEnv ? strdup(pathEnv) : NULL;
    size_t found = 99;
    uint64_t started, elapsed;
    int listener = -1, toolFd = -1, n, parsed = 0, stopped = 0;
    FILE *marker;
    long childPid = -1;
    solariStatus rc;

    TEST_ASSERT_NOT_NULL(dir);
    listener = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT_TRUE(listener >= 0);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_ASSERT_EQUAL_INT(0, bind(listener, (struct sockaddr *)&addr, sizeof addr));
    TEST_ASSERT_EQUAL_INT(0, listen(listener, 1));
    TEST_ASSERT_EQUAL_INT(0, getsockname(listener, (struct sockaddr *)&addr, &addrLen));

    n = snprintf(toolPath, sizeof toolPath, "%s/nmap", dir);
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof toolPath);
    n = snprintf(markerPath, sizeof markerPath, "%s/started", dir);
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof markerPath);
    n = snprintf(argsPath, sizeof argsPath, "%s/args", dir);
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof argsPath);
    n = snprintf(script, sizeof script,
                 "#!/bin/sh\nprintf '%%s\\n' \"$@\" > \"$SCAN_TEST_ARGS\"\n"
                 "tmp=\"$SCAN_TEST_MARKER.tmp.$$\"\n"
                 "/bin/sleep 30 & child=$!\n"
                 "echo $child > \"$tmp\" && mv \"$tmp\" \"$SCAN_TEST_MARKER\"\n"
                 "exit 0\n");
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof script);
    toolFd = open(toolPath, O_WRONLY | O_CREAT | O_EXCL, 0700);
    TEST_ASSERT_TRUE(toolFd >= 0);
    TEST_ASSERT_EQUAL_INT(n, (int)write(toolFd, script, (size_t)n));
    TEST_ASSERT_EQUAL_INT(0, close(toolFd));

    n = snprintf(pathValue, sizeof pathValue, "%s:/usr/bin:/bin", dir);
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof pathValue);
    TEST_ASSERT_EQUAL_INT(0, setenv("PATH", pathValue, 1));
    TEST_ASSERT_EQUAL_INT(0, setenv("SCAN_TEST_MARKER", markerPath, 1));
    TEST_ASSERT_EQUAL_INT(0, setenv("SCAN_TEST_ARGS", argsPath, 1));
    g_toolMarker = markerPath;

    memset(&ctx, 0, sizeof ctx);
    ctx.db = (serverDb *)1;
    ctx.role = SRV_ACTIVE;
    ctx.nodeId = 42;
    ctx.epoch = 1;
    g_leaseHolder = ctx.nodeId;
    g_leaseEpoch = ctx.epoch;
    g_fencedWrites = 0;
    g_progressCalls = 0;
    snprintf(portText, sizeof portText, "%u", (unsigned)ntohs(addr.sin_port));

    started = solariNowUnixMs();
    rc = serverScanRunProgress(&ctx, "127.0.0.1/32", portText, &found,
                               cancelWhenFakeToolStarts, NULL);
    elapsed = solariNowUnixMs() - started;
    marker = fopen(markerPath, "r");
    if (marker) {
        parsed = fscanf(marker, "%ld", &childPid);
        fclose(marker);
    }
    marker = fopen(argsPath, "r");
    if (marker) {
        (void)fread(argsText, 1, sizeof argsText - 1, marker);
        fclose(marker);
    }
    if (parsed == 1 && childPid > 1) {
        stopped = processStopped((pid_t)childPid);
        if (!stopped) (void)kill((pid_t)childPid, SIGKILL);
    }

    close(listener);
    if (oldPath) (void)setenv("PATH", oldPath, 1);
    else (void)unsetenv("PATH");
    (void)unsetenv("SCAN_TEST_MARKER");
    (void)unsetenv("SCAN_TEST_ARGS");
    unlink(markerPath);
    unlink(argsPath);
    unlink(toolPath);
    rmdir(dir);
    free(oldPath);
    g_toolMarker = NULL;

    /* Restore process/environment state before Unity can abort an assertion. */
    TEST_ASSERT_EQUAL_INT(ERR_PLATFORM, rc);
    TEST_ASSERT_TRUE(elapsed < 2000);
    TEST_ASSERT_TRUE(g_progressCalls >= 2);
    TEST_ASSERT_EQUAL_UINT32(0, g_fencedWrites);
    TEST_ASSERT_EQUAL_UINT32(0, found);
    TEST_ASSERT_EQUAL_INT(1, parsed);
    TEST_ASSERT_TRUE(childPid > 1);
    TEST_ASSERT_TRUE(stopped);
    snprintf(expectedPorts, sizeof expectedPorts, "-p\n%s\n", portText);
    TEST_ASSERT_NOT_NULL(strstr(argsText, expectedPorts));
    TEST_ASSERT_NOT_NULL(strstr(argsText, "--version-light\n"));
    TEST_ASSERT_NOT_NULL(strstr(argsText, "--osscan-limit\n"));
}

static void test_enrichment_timeout_terminates_tool(void)
{
    char output[16];
    uint64_t started = scanMonotonicMs();
    int rc = scanToolRun("exec /usr/bin/yes X", output, sizeof output,
                         false, NULL);
    uint64_t elapsed = scanMonotonicMs() - started;
    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_TRUE(elapsed >= SCAN_TOOL_TIMEOUT_MS);
    TEST_ASSERT_TRUE(elapsed < SCAN_TOOL_TIMEOUT_MS + 1500);
    TEST_ASSERT_EQUAL_CHAR('X', output[0]);
}

static void test_enrichment_output_overflow_is_drained(void)
{
    char output[32];
    uint64_t started = solariNowUnixMs();
    int rc = scanToolRun("yes X | head -c 100000", output, sizeof output,
                         false, NULL);
    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_TRUE(solariNowUnixMs() - started < 2000);
    TEST_ASSERT_EQUAL_UINT8('\0', output[sizeof output - 1]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cidr_excludes_network_and_broadcast);
    RUN_TEST(test_small_cidr_ranges);
    RUN_TEST(test_cidr_rejects_malformed_or_unbounded_ranges);
    RUN_TEST(test_ports_are_strict_and_bounded);
    RUN_TEST(test_scan_plan_validates_before_admission);
    RUN_TEST(test_lease_loss_between_batches_stops_before_result_writes);
    RUN_TEST(test_cooperative_cancel_after_batch_skips_result_writes);
    RUN_TEST(test_worker_throttles_unchanged_enrichment_progress);
    RUN_TEST(test_cancel_requires_complete_positive_pid_marker);
    RUN_TEST(test_cancel_terminates_enrichment_process_group);
    RUN_TEST(test_enrichment_timeout_terminates_tool);
    RUN_TEST(test_enrichment_output_overflow_is_drained);
    return UNITY_END();
}
