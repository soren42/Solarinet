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
#include "solari/solariTlv.h"
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

static void test_discovery_frame(void)
{
    clientConfig cfg;
    clientContext ctx;
    uint8_t frame[8192];
    size_t flen = 0, plen = 0;
    uint8_t n = 0;
    solariFrameHeader h;
    const uint8_t *payload = NULL, *val = NULL;
    solariTlvReader r;
    uint16_t type = 0, len = 0;
    int sawSource = 0;

    clientConfigDefaults(&cfg);
    strncpy(cfg.hostFqdn, "disc.akoria.net", sizeof cfg.hostFqdn - 1);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientContextInit(&ctx, &cfg));

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        clientBuildDiscoveryFrame(&ctx, frame, sizeof frame, &flen, &n));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariFrameParse(frame, flen, &h, &payload, &plen, NULL));
    TEST_ASSERT_EQUAL_UINT8(SCP_MSG_DISCOVERY_ADVERT, h.msgType);
    TEST_ASSERT_EQUAL_UINT64(ctx.nodeId, h.sourceNodeId);

    solariTlvReaderInit(&r, payload, plen);
    while (solariTlvNext(&r, &type, &val, &len) == SOLARI_OK)
        if (type == TLV_DISC_SOURCE) sawSource = 1;
    TEST_ASSERT_TRUE(sawSource);            /* the ARP source tag is always present */

    clientContextClose(&ctx);
}

static void test_topology_frame(void)
{
    clientConfig cfg;
    clientContext ctx;
    uint8_t frame[8192];
    size_t flen = 0, plen = 0;
    uint8_t n = 0;
    solariFrameHeader h;
    const uint8_t *payload = NULL, *val = NULL;
    solariTlvReader r;
    uint16_t type = 0, len = 0;
    int sawUplink = 0, sawSegment = 0;

    clientConfigDefaults(&cfg);
    strncpy(cfg.hostFqdn, "topo.akoria.net", sizeof cfg.hostFqdn - 1);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientContextInit(&ctx, &cfg));

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        clientBuildTopologyFrame(&ctx, frame, sizeof frame, &flen, &n));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariFrameParse(frame, flen, &h, &payload, &plen, NULL));
    TEST_ASSERT_EQUAL_UINT8(SCP_MSG_TOPOLOGY_REPORT, h.msgType);

    solariTlvReaderInit(&r, payload, plen);
    while (solariTlvNext(&r, &type, &val, &len) == SOLARI_OK) {
        if (type == TLV_TOPO_UPLINK)  sawUplink = 1;
        if (type == TLV_TOPO_SEGMENT) sawSegment = 1;
    }
    TEST_ASSERT_TRUE(sawSegment);          /* >=1 IPv4 interface on this host */
    TEST_ASSERT_TRUE(sawUplink);           /* this host has a default route   */

    clientContextClose(&ctx);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_report_live_send);
    RUN_TEST(test_spool_then_drain);
    RUN_TEST(test_discovery_frame);
    RUN_TEST(test_topology_frame);
    return UNITY_END();
}
