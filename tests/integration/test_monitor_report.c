/*
 * test_monitor_report.c - the monitor reporting path (sec 8.1) on the shared
 * transport: build a MONITOR_REPORT and complete its matching session/sequence
 * ACK over a loopback REQ/REP endpoint. This confirms the monitor uses the same
 * confirmed-delivery contract as clients.
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
#include "report_ack_stub.h"

#include <string.h>
#include <pthread.h>

#define URL "tcp://127.0.0.1:8812"
#define ACK_URL "tcp://127.0.0.1:8813"

void setUp(void) {}
void tearDown(void) {}

static void test_monitor_report_live(void)
{
    solariConnOpts repOpts;
    solariConn    *ackRep = NULL;
    monitorConfig  cfg;
    monitorContext ctx;
    solariMonitorReport mrep;
    solariStatus rc = ERR_CONN_RETRY;
    int tries;
    pthread_t ackThread;
    reportAckStub ackStub;

    monitorConfigDefaults(&cfg);
    strncpy(cfg.hostFqdn, "mon.akoria.net", sizeof cfg.hostFqdn - 1);
    strncpy(cfg.primaryUrl, URL, sizeof cfg.primaryUrl - 1);

    memset(&repOpts, 0, sizeof repOpts);
    repOpts.url = ACK_URL;
    repOpts.pattern = SOLARI_PATTERN_REP;
    repOpts.recvTimeoutMs = 4000;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariConnListen(&repOpts, &ackRep));

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, monitorContextInit(&ctx, &cfg));
    (void)monitorConnect(&ctx);

    memset(&mrep, 0, sizeof mrep);
    strncpy(mrep.hostFqdn, "mon.akoria.net", sizeof mrep.hostFqdn - 1);
    mrep.probes[0].proto = 2;            /* tcp */
    mrep.probes[0].outcome = 0;          /* ok  */
    mrep.probes[0].dstPort = 443;
    mrep.probes[0].rttMicros = 1234;
    mrep.probeCount = 1;

    ackStub.conn = ackRep; ackStub.replies = 1; ackStub.corruptFirst = false;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&ackThread, NULL, reportAckStubRun, &ackStub));
    for (tries = 0; tries < 80 && rc != SOLARI_OK; tries++) {
        rc = monitorReportSend(&ctx, &mrep);
        if (rc != SOLARI_OK) solariSleepMs(50);
    }
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, rc);

    pthread_join(ackThread, NULL);

    monitorContextClose(&ctx);
    solariConnClose(ackRep);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_monitor_report_live);
    return UNITY_END();
}
