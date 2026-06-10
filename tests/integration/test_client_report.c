/*
 * test_client_report.c - the client reporting path (sec 7.3) end to end.
 *
 * Two cases, both against a loopback PULL "server":
 *   1. live send: connect, collect, push a CLIENT_REPORT, verify it arrives and
 *      carries this node's id.
 *   2. fault tolerance: with the server DOWN, a report is durably spooled; when
 *      the server comes up, draining replays it and empties the queue.
 *
 * Gated behind SOLARI_WITH_IO && SOLARI_WITH_SQLITE (it needs nng + the spool),
 * so it builds and runs where the I/O layer exists (xenon).
 */
#include "unity.h"
#include "client.h"

#include "solari/solariNet.h"
#include "solari/solariFrame.h"
#include "solari/solariSpool.h"
#include "solari/solariTime.h"
#include "solari/solariError.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define URL_LIVE  "tcp://127.0.0.1:8810"
#define URL_SPOOL "tcp://127.0.0.1:8811"

void setUp(void) {}
void tearDown(void) {}

static void test_report_live_send(void)
{
    solariConnOpts pullOpts;
    solariConn    *pull = NULL;
    clientConfig   cfg;
    clientContext  ctx;
    clientState    st;
    solariClientReport rep;
    const uint8_t *rx = NULL;
    size_t rlen = 0;
    solariFrameHeader h;
    solariStatus sendRc = ERR_CONN_RETRY;
    int tries;

    clientConfigDefaults(&cfg);
    strncpy(cfg.hostFqdn, "creport-live.akoria.net", sizeof cfg.hostFqdn - 1);
    strncpy(cfg.primaryUrl, URL_LIVE, sizeof cfg.primaryUrl - 1);
    /* no spoolDb: send returns ERR_CONN_RETRY until the pipe is live */

    memset(&pullOpts, 0, sizeof pullOpts);
    pullOpts.url = URL_LIVE;
    pullOpts.pattern = SOLARI_PATTERN_PULL;
    pullOpts.recvTimeoutMs = 4000;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariConnListen(&pullOpts, &pull));

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientContextInit(&ctx, &cfg));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientConnect(&ctx));

    memset(&st, 0, sizeof st);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientCollectReport(&cfg, &st, true, &rep));

    for (tries = 0; tries < 80 && sendRc != SOLARI_OK; tries++) {
        sendRc = clientReportSend(&ctx, &rep);
        if (sendRc != SOLARI_OK) solariSleepMs(50);
    }
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, sendRc);

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariConnRecv(pull, &rx, &rlen));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariFrameParse(rx, rlen, &h, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT8(SCP_MSG_CLIENT_REPORT, h.msgType);
    TEST_ASSERT_EQUAL_UINT64(ctx.nodeId, h.sourceNodeId);

    clientContextClose(&ctx);
    solariConnClose(pull);
}

static void test_spool_then_drain(void)
{
    char dbpath[64];
    clientConfig  cfg;
    clientContext ctx;
    clientState   st;
    solariClientReport rep;
    solariConnOpts pullOpts;
    solariConn    *pull = NULL;
    const uint8_t *rx = NULL;
    size_t rlen = 0;
    solariFrameHeader h;
    int tries;

    snprintf(dbpath, sizeof dbpath, "/tmp/solari_creport_%ld.db", (long)getpid());
    remove(dbpath);

    clientConfigDefaults(&cfg);
    strncpy(cfg.hostFqdn, "creport-spool.akoria.net", sizeof cfg.hostFqdn - 1);
    strncpy(cfg.primaryUrl, URL_SPOOL, sizeof cfg.primaryUrl - 1);
    strncpy(cfg.spoolDb, dbpath, sizeof cfg.spoolDb - 1);

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientContextInit(&ctx, &cfg));
    clientConnect(&ctx);                         /* dialer; no listener yet */

    memset(&st, 0, sizeof st);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientCollectReport(&cfg, &st, true, &rep));

    /* server down -> the report must be spooled, not lost */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientReportSend(&ctx, &rep));
    TEST_ASSERT_TRUE(solariSpoolDepth(ctx.spool) >= 1);

    /* bring the server up; draining replays the queue to empty */
    memset(&pullOpts, 0, sizeof pullOpts);
    pullOpts.url = URL_SPOOL;
    pullOpts.pattern = SOLARI_PATTERN_PULL;
    pullOpts.recvTimeoutMs = 4000;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariConnListen(&pullOpts, &pull));
    clientConnect(&ctx);                         /* ensure a live conn exists */

    for (tries = 0; tries < 80 && solariSpoolDepth(ctx.spool) > 0; tries++) {
        clientDrainSpool(&ctx);
        if (solariSpoolDepth(ctx.spool) > 0) solariSleepMs(50);
    }
    TEST_ASSERT_EQUAL_size_t(0, solariSpoolDepth(ctx.spool));

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariConnRecv(pull, &rx, &rlen));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariFrameParse(rx, rlen, &h, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT8(SCP_MSG_CLIENT_REPORT, h.msgType);

    clientContextClose(&ctx);
    solariConnClose(pull);
    remove(dbpath);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_report_live_send);
    RUN_TEST(test_spool_then_drain);
    return UNITY_END();
}
