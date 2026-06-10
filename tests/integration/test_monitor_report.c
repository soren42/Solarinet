/*
 * test_monitor_report.c - the monitor reporting path (sec 8.1) on the shared
 * transport: connect, build a MONITOR_REPORT, push it to a loopback PULL server,
 * and verify it arrives carrying this node's id. The push-or-spool fault
 * tolerance itself lives in solariReporter (covered by the client suite); this
 * confirms the monitor is wired onto it correctly.
 *
 * Gated behind SOLARI_WITH_IO && SOLARI_WITH_SQLITE.
 */
#include "unity.h"
#include "monitor.h"

#include "solari/solariNet.h"
#include "solari/solariFrame.h"
#include "solari/solariReporter.h"
#include "solari/solariTime.h"
#include "solari/solariError.h"

#include <string.h>

#define URL "tcp://127.0.0.1:8812"

void setUp(void) {}
void tearDown(void) {}

static void test_monitor_report_live(void)
{
    solariConnOpts pullOpts;
    solariConn    *pull = NULL;
    monitorConfig  cfg;
    monitorContext ctx;
    solariMonitorReport mrep;
    const uint8_t *rx = NULL;
    size_t rlen = 0;
    solariFrameHeader h;
    solariStatus rc = ERR_CONN_RETRY;
    int tries;

    monitorConfigDefaults(&cfg);
    strncpy(cfg.hostFqdn, "mon.akoria.net", sizeof cfg.hostFqdn - 1);
    strncpy(cfg.primaryUrl, URL, sizeof cfg.primaryUrl - 1);

    memset(&pullOpts, 0, sizeof pullOpts);
    pullOpts.url = URL;
    pullOpts.pattern = SOLARI_PATTERN_PULL;
    pullOpts.recvTimeoutMs = 4000;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariConnListen(&pullOpts, &pull));

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, monitorContextInit(&ctx, &cfg));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, monitorConnect(&ctx));

    memset(&mrep, 0, sizeof mrep);
    strncpy(mrep.hostFqdn, "mon.akoria.net", sizeof mrep.hostFqdn - 1);
    mrep.probes[0].proto = 2;            /* tcp */
    mrep.probes[0].outcome = 0;          /* ok  */
    mrep.probes[0].dstPort = 443;
    mrep.probes[0].rttMicros = 1234;
    mrep.probeCount = 1;

    for (tries = 0; tries < 80 && rc != SOLARI_OK; tries++) {
        rc = monitorReportSend(&ctx, &mrep);
        if (rc != SOLARI_OK) solariSleepMs(50);
    }
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, rc);

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariConnRecv(pull, &rx, &rlen));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariFrameParse(rx, rlen, &h, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT8(SCP_MSG_MONITOR_REPORT, h.msgType);
    TEST_ASSERT_EQUAL_UINT64(ctx.nodeId, h.sourceNodeId);

    monitorContextClose(&ctx);
    solariConnClose(pull);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_monitor_report_live);
    return UNITY_END();
}
