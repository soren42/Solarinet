/*
 * test_msg.c - typed message build/parse round-trips (§6.4).
 */
#include "unity.h"
#include "solari/solariMsg.h"
#include "solari/solariError.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_clientReport_roundTrip(void)
{
    solariClientReport in, out;
    uint8_t payload[2048];
    size_t outLen = 0;
    uint16_t tlvCount = 0;

    memset(&in, 0, sizeof in);
    strcpy(in.hostFqdn, "hydrogen.akoria.net");
    strcpy(in.osName, "Linux 6.1.0");
    strcpy(in.arch, "arm64");
    in.coreCount = 4;
    in.cpuLoadMilli[0] = 320; in.cpuLoadMilli[1] = 410;
    in.cpuLoadMilli[2] = 290; in.cpuLoadMilli[3] = 380;
    in.ramUsedKb = 5242880; in.ramTotalKb = 16777216;
    in.swapUsedKb = 0; in.swapTotalKb = 2097152;

    in.diskCount = 2;
    strcpy(in.disks[0].mount, "/");      in.disks[0].freeKb = 100; in.disks[0].totalKb = 500;
    strcpy(in.disks[1].mount, "/var");   in.disks[1].freeKb = 200; in.disks[1].totalKb = 800;

    in.ifaceCount = 1;
    strcpy(in.ifaces[0].name, "eth0");
    in.ifaces[0].rxKbps = 1000; in.ifaces[0].txKbps = 500; in.ifaces[0].capacityKbps = 1000000;

    in.procCount = 1;
    strcpy(in.procs[0].name, "apache2");
    in.procs[0].pid = 812; in.procs[0].state = 'R';
    in.procs[0].nFiles = 142; in.procs[0].nSockets = 38; in.procs[0].rssKb = 88210;

    in.logCount = 1;
    strcpy(in.logs[0].path, "/var/log/syslog");
    in.logs[0].sizeDelta = 4096; in.logs[0].matchCount = 3; in.logs[0].lastOffset = 99999;

    in.health.fsReadonlyCount = 1;
    in.health.blockDevMissing = 1;
    in.health.smartFailCount = 1;
    in.health.failedUnitCount = 2;
    in.health.dmesgCritCount = 3;
    strcpy(in.health.fsReadonlyList, "/data");
    strcpy(in.health.smartFailList, "sde:FAILING");
    strcpy(in.health.failedUnitList, "nginx.service,forgejo.service");
    strcpy(in.health.dmesgCritSample, "btrfs: error: forced readonly");

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariMsgBuildClientReport(&in, payload, sizeof payload, &outLen, &tlvCount));
    TEST_ASSERT_TRUE(outLen > 0);
    TEST_ASSERT_TRUE(tlvCount >= 12);

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariMsgParseClientReport(payload, outLen, &out));

    TEST_ASSERT_EQUAL_STRING("hydrogen.akoria.net", out.hostFqdn);
    TEST_ASSERT_EQUAL_STRING("Linux 6.1.0", out.osName);
    TEST_ASSERT_EQUAL_STRING("arm64", out.arch);
    TEST_ASSERT_EQUAL_UINT8(4, out.coreCount);
    TEST_ASSERT_EQUAL_UINT32(410, out.cpuLoadMilli[1]);
    TEST_ASSERT_EQUAL_UINT64(16777216, out.ramTotalKb);
    TEST_ASSERT_EQUAL_UINT8(2, out.diskCount);
    TEST_ASSERT_EQUAL_STRING("/var", out.disks[1].mount);
    TEST_ASSERT_EQUAL_UINT64(800, out.disks[1].totalKb);
    TEST_ASSERT_EQUAL_STRING("eth0", out.ifaces[0].name);
    TEST_ASSERT_EQUAL_UINT8('R', out.procs[0].state);
    TEST_ASSERT_EQUAL_INT(812, out.procs[0].pid);
    TEST_ASSERT_EQUAL_UINT32(38, out.procs[0].nSockets);
    TEST_ASSERT_EQUAL_STRING("/var/log/syslog", out.logs[0].path);
    TEST_ASSERT_EQUAL_UINT32(3, out.logs[0].matchCount);
    TEST_ASSERT_EQUAL_UINT8(1, out.health.fsReadonlyCount);
    TEST_ASSERT_EQUAL_UINT8(1, out.health.blockDevMissing);
    TEST_ASSERT_EQUAL_UINT8(1, out.health.smartFailCount);
    TEST_ASSERT_EQUAL_UINT16(2, out.health.failedUnitCount);
    TEST_ASSERT_EQUAL_UINT16(3, out.health.dmesgCritCount);
    TEST_ASSERT_EQUAL_STRING("/data", out.health.fsReadonlyList);
    TEST_ASSERT_EQUAL_STRING("sde:FAILING", out.health.smartFailList);
    TEST_ASSERT_EQUAL_STRING("nginx.service,forgejo.service", out.health.failedUnitList);
    TEST_ASSERT_EQUAL_STRING("btrfs: error: forced readonly", out.health.dmesgCritSample);
}

static void test_clientReport_zeroHealth_roundTrip(void)
{
    solariClientReport in, out;
    uint8_t payload[2048];
    size_t outLen = 0;
    uint16_t tlvCount = 0;

    memset(&in, 0, sizeof in);
    strcpy(in.hostFqdn, "clean.akoria.net");

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariMsgBuildClientReport(&in, payload, sizeof payload, &outLen, &tlvCount));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariMsgParseClientReport(payload, outLen, &out));

    TEST_ASSERT_EQUAL_UINT8(0, out.health.fsReadonlyCount);
    TEST_ASSERT_EQUAL_UINT8(0, out.health.blockDevMissing);
    TEST_ASSERT_EQUAL_UINT8(0, out.health.smartFailCount);
    TEST_ASSERT_EQUAL_UINT16(0, out.health.failedUnitCount);
    TEST_ASSERT_EQUAL_UINT16(0, out.health.dmesgCritCount);
    TEST_ASSERT_EQUAL_STRING("", out.health.fsReadonlyList);
    TEST_ASSERT_EQUAL_STRING("", out.health.smartFailList);
    TEST_ASSERT_EQUAL_STRING("", out.health.failedUnitList);
    TEST_ASSERT_EQUAL_STRING("", out.health.dmesgCritSample);
}

static void test_monitorReport_roundTrip(void)
{
    solariMonitorReport in, out;
    uint8_t payload[4096];
    size_t outLen = 0;
    uint16_t tlvCount = 0;

    memset(&in, 0, sizeof in);
    strcpy(in.hostFqdn, "vantage1.akoria.net");
    in.probeCount = 2;
    in.probes[0].proto = 2; in.probes[0].outcome = 0; in.probes[0].dstPort = 443;
    in.probes[0].rttMicros = 1500; in.probes[0].jitterMicros = 80;
    in.probes[0].lossPermille = 0; in.probes[0].throughputKbps = 90000;
    in.probes[0].probeTimeUnixMs = 1733500000123ull;
    in.probes[0].dstAddr[15] = 1;
    in.probes[1].proto = 1; in.probes[1].outcome = 1; in.probes[1].dstPort = 0;
    in.probes[1].rttMicros = 0; in.probes[1].lossPermille = 1000;

    in.linkCount = 1;
    in.links[0].peerNodeId = 0xCAFEBABEull;
    in.links[0].transitMicros = 700;
    in.links[0].throughputKbps = 50000;
    in.links[0].capacityKbps = 1000000;
    in.links[0].overheadPermille = 25;

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariMsgBuildMonitorReport(&in, payload, sizeof payload, &outLen, &tlvCount));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariMsgParseMonitorReport(payload, outLen, &out));

    TEST_ASSERT_EQUAL_STRING("vantage1.akoria.net", out.hostFqdn);
    TEST_ASSERT_EQUAL_UINT16(2, out.probeCount);
    TEST_ASSERT_EQUAL_UINT16(443, out.probes[0].dstPort);
    TEST_ASSERT_EQUAL_UINT32(1500, out.probes[0].rttMicros);
    TEST_ASSERT_EQUAL_UINT64(1733500000123ull, out.probes[0].probeTimeUnixMs);
    TEST_ASSERT_EQUAL_UINT8(1, out.probes[0].dstAddr[15]);
    TEST_ASSERT_EQUAL_UINT16(1000, out.probes[1].lossPermille);
    TEST_ASSERT_EQUAL_UINT8(1, out.linkCount);
    TEST_ASSERT_EQUAL_UINT64(0xCAFEBABEull, out.links[0].peerNodeId);
    TEST_ASSERT_EQUAL_UINT16(25, out.links[0].overheadPermille);
}

static void test_hello_roundTrip(void)
{
    solariHello in, out;
    uint8_t payload[512];
    size_t outLen = 0;
    uint16_t tlvCount = 0;

    memset(&in, 0, sizeof in);
    in.role = ROLE_MONITOR;
    strcpy(in.agentVersion, "1.0.0");
    strcpy(in.hostFqdn, "vantage1.akoria.net");
    in.configEpoch = 42;

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariMsgBuildHello(&in, payload, sizeof payload, &outLen, &tlvCount));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariMsgParseHello(payload, outLen, &out));
    TEST_ASSERT_EQUAL_INT(ROLE_MONITOR, out.role);
    TEST_ASSERT_EQUAL_STRING("1.0.0", out.agentVersion);
    TEST_ASSERT_EQUAL_STRING("vantage1.akoria.net", out.hostFqdn);
    TEST_ASSERT_EQUAL_UINT64(42, out.configEpoch);
}

static void test_control_roundTrip(void)
{
    solariControl in, out;
    uint8_t payload[256];
    const uint8_t blob[] = { 't','o','k','e','n', 0x00, 0x10 };
    size_t outLen = 0;
    uint16_t tlvCount = 0;

    memset(&in, 0, sizeof in);
    in.verb = CTRL_SET_CONFIG;
    in.targetEpoch = 7;
    in.payload = blob;
    in.payloadLen = sizeof blob;

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariMsgBuildControl(&in, payload, sizeof payload, &outLen, &tlvCount));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariMsgParseControl(payload, outLen, &out));
    TEST_ASSERT_EQUAL_UINT8(CTRL_SET_CONFIG, out.verb);
    TEST_ASSERT_EQUAL_UINT64(7, out.targetEpoch);
    TEST_ASSERT_EQUAL_UINT16(sizeof blob, out.payloadLen);
    TEST_ASSERT_EQUAL_MEMORY(blob, out.payload, sizeof blob);
}

static void test_error_roundTrip(void)
{
    solariErrorMsg in, out;
    uint8_t payload[512];
    size_t outLen = 0;
    uint16_t tlvCount = 0;

    memset(&in, 0, sizeof in);
    in.code = 40;   /* magnitude of ERR_AUTH_ROLE */
    strcpy(in.detail, "client may not send MONITOR_REPORT");

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariMsgBuildError(&in, payload, sizeof payload, &outLen, &tlvCount));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariMsgParseError(payload, outLen, &out));
    TEST_ASSERT_EQUAL_UINT16(40, out.code);
    TEST_ASSERT_EQUAL_STRING("client may not send MONITOR_REPORT", out.detail);
}

/* Forward-compat: an unknown TLV spliced into a Hello payload is skipped. */
static void test_unknownTlv_skipped(void)
{
    solariHello in, out;
    uint8_t payload[512];
    size_t outLen = 0;
    uint16_t tlvCount = 0;

    memset(&in, 0, sizeof in);
    in.role = ROLE_CLIENT;
    strcpy(in.hostFqdn, "h.akoria.net");
    solariMsgBuildHello(&in, payload, sizeof payload, &outLen, &tlvCount);

    /* Append a bogus unknown TLV (type 0x7777, 2 bytes) by hand. */
    payload[outLen++] = 0x77; payload[outLen++] = 0x77;
    payload[outLen++] = 0x00; payload[outLen++] = 0x02;
    payload[outLen++] = 0xDE; payload[outLen++] = 0xAD;

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariMsgParseHello(payload, outLen, &out));
    TEST_ASSERT_EQUAL_STRING("h.akoria.net", out.hostFqdn);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_clientReport_roundTrip);
    RUN_TEST(test_clientReport_zeroHealth_roundTrip);
    RUN_TEST(test_monitorReport_roundTrip);
    RUN_TEST(test_hello_roundTrip);
    RUN_TEST(test_control_roundTrip);
    RUN_TEST(test_error_roundTrip);
    RUN_TEST(test_unknownTlv_skipped);
    return UNITY_END();
}
