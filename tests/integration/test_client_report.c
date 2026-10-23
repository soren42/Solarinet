/*
 * test_client_report.c - the client reporting path (sec 7.3) end to end.
 *
 * Two cases against a loopback REQ/REP acknowledgment endpoint:
 *   1. live send: verify a matching session/sequence ACK completes delivery.
 *   2. fault tolerance: with the endpoint DOWN, a report is durably spooled;
 *      when it comes up, draining keeps the report until ACK.
 *
 * Gated behind SOLARI_WITH_IO && SOLARI_WITH_SQLITE (it needs nng + the spool),
 * so it builds and runs where the I/O layer exists (xenon).
 */
#define _GNU_SOURCE
#include "unity.h"
#include "../test_temp_path.h"
#include "client.h"

#include "solari/solariNet.h"
#include "solari/solariCrypto.h"
#include "solari/solariFrame.h"
#include "solari/solariTlv.h"
#include "solari/solariReporter.h"
#include "solari/solariTime.h"
#include "solari/solariError.h"
#include "solari/solariMsg.h"
#include "report_ack_stub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define URL_LIVE  "tcp://127.0.0.1:8810"
#define URL_SPOOL "tcp://127.0.0.1:8811"
#define URL_LIVE_ACK "tcp://127.0.0.1:8811"
#define URL_SPOOL_ACK "tcp://127.0.0.1:8812"
#define URL_BAD_ACK "tcp://127.0.0.1:8814"

void setUp(void) {}
void tearDown(void) {}

static void test_report_live_send(void)
{
    solariConnOpts repOpts;
    solariConn    *ackRep = NULL;
    clientConfig   cfg;
    clientContext  ctx;
    clientState    st;
    solariClientReport report;
    solariStatus sendRc = ERR_CONN_RETRY;
    pthread_t ackThread;
    reportAckStub ackStub;
    int tries;

    clientConfigDefaults(&cfg);
    strncpy(cfg.hostFqdn, "creport-live.akoria.net", sizeof cfg.hostFqdn - 1);
    strncpy(cfg.primaryUrl, URL_LIVE, sizeof cfg.primaryUrl - 1);
    /* no spoolDb: the report requires a server ACK to return SOLARI_OK */

    memset(&repOpts, 0, sizeof repOpts);
    repOpts.url = URL_LIVE_ACK;
    repOpts.pattern = SOLARI_PATTERN_REP;
    repOpts.recvTimeoutMs = 4000;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariConnListen(&repOpts, &ackRep));

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientContextInit(&ctx, &cfg));
    TEST_ASSERT_EQUAL_UINT64(
        solariNodeIdFromFqdnRole("creport-live.akoria.net", ROLE_CLIENT),
        ctx.nodeId);
    (void)clientConnect(&ctx); /* report delivery uses the confirmed ACK endpoint */

    memset(&st, 0, sizeof st);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientCollectReport(&cfg, &st, true, &report));

    ackStub.conn = ackRep; ackStub.replies = 1; ackStub.corruptFirst = false;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&ackThread, NULL, reportAckStubRun, &ackStub));
    for (tries = 0; tries < 80 && sendRc != SOLARI_OK; tries++) {
        sendRc = clientReportSend(&ctx, &report);
        if (sendRc != SOLARI_OK) solariSleepMs(50);
    }
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, sendRc);
    pthread_join(ackThread, NULL);

    clientContextClose(&ctx);
    solariConnClose(ackRep);
}

static void test_spool_then_drain(void)
{
    char dbpath[512], leaf[128];
    clientConfig  cfg;
    clientContext ctx;
    clientState   st;
    solariClientReport report;
    solariConnOpts repOpts;
    solariConn    *ackRep = NULL;
    int tries;
    pthread_t ackThread;
    reportAckStub ackStub;

    snprintf(leaf, sizeof leaf, "solari_creport_%ld.db", (long)getpid());
    TEST_ASSERT_TRUE(solariTestTempPath(dbpath, sizeof dbpath, leaf));
    remove(dbpath);

    clientConfigDefaults(&cfg);
    strncpy(cfg.hostFqdn, "creport-spool.akoria.net", sizeof cfg.hostFqdn - 1);
    strncpy(cfg.primaryUrl, URL_SPOOL, sizeof cfg.primaryUrl - 1);
    strncpy(cfg.spoolDb, dbpath, sizeof cfg.spoolDb - 1);

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientContextInit(&ctx, &cfg));
    clientConnect(&ctx);                         /* dialer; no listener yet */

    memset(&st, 0, sizeof st);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientCollectReport(&cfg, &st, true, &report));

    /* server down -> the report must be spooled, not lost */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientReportSend(&ctx, &report));
    TEST_ASSERT_TRUE(solariReporterSpoolDepth(ctx.rep) >= 1);

    /* bring the server up; draining replays the queue to empty */
    memset(&repOpts, 0, sizeof repOpts);
    repOpts.url = URL_SPOOL_ACK;
    repOpts.pattern = SOLARI_PATTERN_REP;
    repOpts.recvTimeoutMs = 4000;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariConnListen(&repOpts, &ackRep));
    ackStub.conn = ackRep; ackStub.replies = 1; ackStub.corruptFirst = false;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&ackThread, NULL, reportAckStubRun, &ackStub));

    for (tries = 0; tries < 80 && solariReporterSpoolDepth(ctx.rep) > 0; tries++) {
        clientDrainSpool(&ctx);
        if (solariReporterSpoolDepth(ctx.rep) > 0) solariSleepMs(50);
    }
    pthread_join(ackThread, NULL);
    TEST_ASSERT_EQUAL_size_t(0, solariReporterSpoolDepth(ctx.rep));

    clientContextClose(&ctx);
    solariConnClose(ackRep);
    remove(dbpath);
}

static void test_mismatched_ack_keeps_report_spooled(void)
{
    char dbpath[512], leaf[128];
    clientConfig cfg;
    clientContext ctx;
    clientState st;
    solariClientReport report;
    solariConnOpts repOpts;
    solariConn *ackRep = NULL;
    reportAckStub ackStub;
    pthread_t ackThread;

    snprintf(leaf, sizeof leaf, "solari_creport_bad_ack_%ld.db", (long)getpid());
    TEST_ASSERT_TRUE(solariTestTempPath(dbpath, sizeof dbpath, leaf));
    remove(dbpath);
    clientConfigDefaults(&cfg);
    strncpy(cfg.hostFqdn, "creport-bad-ack.akoria.net", sizeof cfg.hostFqdn - 1);
    strncpy(cfg.primaryUrl, URL_BAD_ACK, sizeof cfg.primaryUrl - 1);
    strncpy(cfg.reportAckUrl, URL_BAD_ACK, sizeof cfg.reportAckUrl - 1);
    strncpy(cfg.spoolDb, dbpath, sizeof cfg.spoolDb - 1);
    memset(&repOpts, 0, sizeof repOpts);
    repOpts.url = URL_BAD_ACK;
    repOpts.pattern = SOLARI_PATTERN_REP;
    repOpts.recvTimeoutMs = 4000;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariConnListen(&repOpts, &ackRep));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientContextInit(&ctx, &cfg));
    memset(&st, 0, sizeof st);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientCollectReport(&cfg, &st, true, &report));

    ackStub.conn = ackRep; ackStub.replies = 2; ackStub.corruptFirst = true;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&ackThread, NULL, reportAckStubRun, &ackStub));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientReportSend(&ctx, &report));
    TEST_ASSERT_EQUAL_size_t(1, solariReporterSpoolDepth(ctx.rep));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientDrainSpool(&ctx));
    pthread_join(ackThread, NULL);
    TEST_ASSERT_EQUAL_size_t(0, solariReporterSpoolDepth(ctx.rep));

    clientContextClose(&ctx);
    solariConnClose(ackRep);
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

static void reportSession(solariReporter *reporter, uint8_t session[16], uint32_t *seq)
{
    uint8_t frame[256];
    const uint8_t *payload, *value;
    size_t length, payloadLength;
    uint16_t type, valueLength;
    solariFrameHeader header;
    solariTlvReader reader;
    bool found = false;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariReporterFrame(reporter,
        SCP_MSG_CLIENT_REPORT, 0, NULL, 0, 0, frame, sizeof frame, &length));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariFrameParse(frame, length, &header, &payload, &payloadLength, NULL));
    *seq = header.seqNo;
    solariTlvReaderInit(&reader, payload, payloadLength);
    while (solariTlvNext(&reader, &type, &value, &valueLength) == SOLARI_OK) {
        if (type != TLV_REPORT_SESSION) continue;
        TEST_ASSERT_EQUAL_UINT16(16, valueLength);
        TEST_ASSERT_FALSE(found);
        memcpy(session, value, 16);
        found = true;
    }
    TEST_ASSERT_TRUE(found);
}

static void test_reporter_restart_has_distinct_identity(void)
{
    solariReporterOpts options;
    solariReporter *reporter = NULL;
    uint8_t first[16], second[16], restarted[16];
    uint32_t seq;
    memset(&options, 0, sizeof options);
    options.primaryUrl = URL_LIVE;
    options.nodeId = 42;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariReporterOpen(&options, &reporter));
    reportSession(reporter, first, &seq);
    TEST_ASSERT_EQUAL_UINT32(1, seq);
    reportSession(reporter, second, &seq);
    TEST_ASSERT_EQUAL_UINT32(2, seq);
    TEST_ASSERT_EQUAL_MEMORY(first, second, 16);
    solariReporterClose(reporter);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariReporterOpen(&options, &reporter));
    reportSession(reporter, restarted, &seq);
    TEST_ASSERT_EQUAL_UINT32(1, seq);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(first, restarted, 16));
    solariReporterClose(reporter);
}

static void test_reporter_attaches_only_a_valid_activation_receipt(void)
{
    solariReporterOpts options;
    solariReporter *reporter = NULL;
    char caPath[256], receiptPath[280];
    const char *receipt = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef:42:"
                          "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789\n";
    uint8_t frame[512];
    const uint8_t *payload = NULL, *value = NULL;
    size_t frameLen = 0, payloadLen = 0;
    uint16_t type, valueLen;
    solariFrameHeader header;
    solariTlvReader reader;
    bool found;
    FILE *f;

    TEST_ASSERT_TRUE(solariTestTempPath(caPath, sizeof caPath, "solari-receipt-test.pem"));
    TEST_ASSERT_TRUE(snprintf(receiptPath, sizeof receiptPath, "%s.activated", caPath) > 0);
    memset(&options, 0, sizeof options);
    options.primaryUrl = URL_LIVE;
    options.caFile = caPath;
    options.nodeId = 42;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariReporterOpen(&options, &reporter));

    f = fopen(receiptPath, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_UINT(strlen(receipt), fwrite(receipt, 1, strlen(receipt), f));
    fclose(f);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariReporterFrame(reporter,
        SCP_MSG_CLIENT_REPORT, 0, NULL, 0, 0, frame, sizeof frame, &frameLen));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariFrameParse(frame, frameLen, &header, &payload, &payloadLen, NULL));
    solariTlvReaderInit(&reader, payload, payloadLen);
    found = false;
    while (solariTlvNext(&reader, &type, &value, &valueLen) == SOLARI_OK) {
        if (type != TLV_CRL_ACTIVATION) continue;
        TEST_ASSERT_FALSE(found);
        TEST_ASSERT_EQUAL_UINT(strlen(receipt) - 1, valueLen);
        TEST_ASSERT_EQUAL_MEMORY(receipt, value, valueLen);
        found = true;
    }
    TEST_ASSERT_TRUE(found);

    f = fopen(receiptPath, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_UINT(4, fwrite("bad\n", 1, 4, f));
    fclose(f);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariReporterFrame(reporter,
        SCP_MSG_CLIENT_REPORT, 0, NULL, 0, 0, frame, sizeof frame, &frameLen));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariFrameParse(frame, frameLen, &header, &payload, &payloadLen, NULL));
    solariTlvReaderInit(&reader, payload, payloadLen);
    found = false;
    while (solariTlvNext(&reader, &type, &value, &valueLen) == SOLARI_OK)
        if (type == TLV_CRL_ACTIVATION) found = true;
    TEST_ASSERT_FALSE(found);
    unlink(receiptPath);
    solariReporterClose(reporter);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_report_live_send);
    RUN_TEST(test_spool_then_drain);
    RUN_TEST(test_mismatched_ack_keeps_report_spooled);
    RUN_TEST(test_discovery_frame);
    RUN_TEST(test_topology_frame);
    RUN_TEST(test_reporter_restart_has_distinct_identity);
    RUN_TEST(test_reporter_attaches_only_a_valid_activation_receipt);
    return UNITY_END();
}
