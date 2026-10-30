/*
 * test_server_ingest.c - the ingest authorization matrix and the replay dedup
 * ring (§9.1). Pure logic: CN->role mapping, the role/msgType send matrix, and
 * the per-source recent-seqNo guard. No DB, no sockets.
 *
 * serverIngest.c is #included to reach its static helpers; the cross-subsystem
 * dispatch targets it references are satisfied by the weak server_stubs.c.
 */
#include "unity.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "serverIngest.c"

#include <string.h>

static uint64_t g_expectedNodeId;
static const char *g_expectedCertCn;
static solariStatus g_identityLookupStatus;
static solariStatus g_crlAckStatus = SOLARI_OK;
static unsigned g_crlAckCalls;

solariStatus serverDbNodeCertMatches(serverDb *db, uint64_t nodeId,
                                     const char *certCn, bool *matches)
{
    if (!db || !matches) return ERR_INVALID_ARG;
    *matches = false;
    if (g_identityLookupStatus != SOLARI_OK) return g_identityLookupStatus;
    *matches = nodeId == g_expectedNodeId && certCn && g_expectedCertCn &&
               strcmp(certCn, g_expectedCertCn) == 0;
    return SOLARI_OK;
}

solariStatus serverDbCertificateCrlAcknowledge(serverDb *db, uint64_t nodeId,
                                                const char *issuerFp,
                                                uint64_t crlNumber,
                                                const char *crlFp)
{
    (void)db; (void)nodeId; (void)issuerFp; (void)crlNumber; (void)crlFp;
    g_crlAckCalls++;
    return g_crlAckStatus;
}

void setUp(void) { g_crlAckStatus = SOLARI_OK; g_crlAckCalls = 0; }
void tearDown(void) {}

/* ---- CN -> role ---- */

static void test_role_from_cn(void)
{
    TEST_ASSERT_EQUAL_INT(INGEST_ROLE_CLIENT,  serverIngestRoleFromCn("client.akoria.net"));
    TEST_ASSERT_EQUAL_INT(INGEST_ROLE_MONITOR, serverIngestRoleFromCn("monitor.akoria.net"));
    TEST_ASSERT_EQUAL_INT(INGEST_ROLE_SERVER,  serverIngestRoleFromCn("server.akoria.net"));
    /* Enrollment uses the same dotted-role form; colon subjects are rejected. */
    TEST_ASSERT_EQUAL_INT(INGEST_ROLE_MONITOR, serverIngestRoleFromCn("monitor.beryl.akoria.net"));
    TEST_ASSERT_EQUAL_INT(INGEST_ROLE_UNKNOWN, serverIngestRoleFromCn("monitor:beryl.akoria.net"));
    /* bare labels (no dot) are honoured */
    TEST_ASSERT_EQUAL_INT(INGEST_ROLE_CLIENT,  serverIngestRoleFromCn("client"));
    TEST_ASSERT_EQUAL_INT(INGEST_ROLE_MONITOR, serverIngestRoleFromCn("monitor"));
    /* unknown / empty / NULL -> UNKNOWN */
    TEST_ASSERT_EQUAL_INT(INGEST_ROLE_UNKNOWN, serverIngestRoleFromCn("router.akoria.net"));
    TEST_ASSERT_EQUAL_INT(INGEST_ROLE_UNKNOWN, serverIngestRoleFromCn(""));
    TEST_ASSERT_EQUAL_INT(INGEST_ROLE_UNKNOWN, serverIngestRoleFromCn(NULL));
    /* a prefix that merely starts with "client" but is a different label */
    TEST_ASSERT_EQUAL_INT(INGEST_ROLE_UNKNOWN, serverIngestRoleFromCn("clienthost.x"));
}

static void test_hello_identity_matches_authenticated_certificate(void)
{
    TEST_ASSERT_TRUE(serverIngestHelloMatchesIdentity(
        ROLE_CLIENT, "node.akoria.net", "client.node.akoria.net"));
    TEST_ASSERT_TRUE(serverIngestHelloMatchesIdentity(
        ROLE_MONITOR, "monitor-1.akoria.net", "monitor.monitor-1.akoria.net"));
    TEST_ASSERT_TRUE(serverIngestHelloMatchesIdentity(
        ROLE_SERVER, "xenon.akoria.net", "server.xenon.akoria.net"));
    TEST_ASSERT_TRUE(serverIngestHelloMatchesIdentity(
        ROLE_CLIENT, "NODE.AKORIA.NET", "client.node.akoria.net"));
    TEST_ASSERT_FALSE(serverIngestHelloMatchesIdentity(
        ROLE_CLIENT, "other.akoria.net", "client.node.akoria.net"));
    TEST_ASSERT_FALSE(serverIngestHelloMatchesIdentity(
        ROLE_MONITOR, "node.akoria.net", "client.node.akoria.net"));
    TEST_ASSERT_FALSE(serverIngestHelloMatchesIdentity(
        (solariRole)99, "node.akoria.net", "client.node.akoria.net"));
    TEST_ASSERT_FALSE(serverIngestHelloMatchesIdentity(
        ROLE_CLIENT, "", "client.node.akoria.net"));
    TEST_ASSERT_FALSE(serverIngestHelloMatchesIdentity(
        ROLE_CLIENT, "node.akoria.net", NULL));
}

/* ---- authorization matrix ---- */

static void test_matrix_client(void)
{
    /* a client may send these */
    TEST_ASSERT_TRUE(serverIngestRoleMaySend(INGEST_ROLE_CLIENT, SCP_MSG_HELLO));
    TEST_ASSERT_TRUE(serverIngestRoleMaySend(INGEST_ROLE_CLIENT, SCP_MSG_CLIENT_REPORT));
    TEST_ASSERT_TRUE(serverIngestRoleMaySend(INGEST_ROLE_CLIENT, SCP_MSG_WATCHDOG));
    TEST_ASSERT_TRUE(serverIngestRoleMaySend(INGEST_ROLE_CLIENT, SCP_MSG_DISCOVERY_ADVERT));
    TEST_ASSERT_TRUE(serverIngestRoleMaySend(INGEST_ROLE_CLIENT, SCP_MSG_TOPOLOGY_REPORT));
    TEST_ASSERT_TRUE(serverIngestRoleMaySend(INGEST_ROLE_CLIENT, SCP_MSG_WHO_IS_PRIMARY));
    /* but NOT a monitor report, nor any server-originated type */
    TEST_ASSERT_FALSE(serverIngestRoleMaySend(INGEST_ROLE_CLIENT, SCP_MSG_MONITOR_REPORT));
    TEST_ASSERT_FALSE(serverIngestRoleMaySend(INGEST_ROLE_CLIENT, SCP_MSG_WELCOME));
    TEST_ASSERT_FALSE(serverIngestRoleMaySend(INGEST_ROLE_CLIENT, SCP_MSG_CONTROL));
    TEST_ASSERT_FALSE(serverIngestRoleMaySend(INGEST_ROLE_CLIENT, SCP_MSG_SURVEY));
}

static void test_matrix_monitor_is_superset(void)
{
    /* a monitor may send MONITOR_REPORT plus the whole client set */
    TEST_ASSERT_TRUE(serverIngestRoleMaySend(INGEST_ROLE_MONITOR, SCP_MSG_MONITOR_REPORT));
    TEST_ASSERT_TRUE(serverIngestRoleMaySend(INGEST_ROLE_MONITOR, SCP_MSG_CLIENT_REPORT));
    TEST_ASSERT_TRUE(serverIngestRoleMaySend(INGEST_ROLE_MONITOR, SCP_MSG_HELLO));
    TEST_ASSERT_TRUE(serverIngestRoleMaySend(INGEST_ROLE_MONITOR, SCP_MSG_TOPOLOGY_REPORT));
    /* still cannot impersonate a server */
    TEST_ASSERT_FALSE(serverIngestRoleMaySend(INGEST_ROLE_MONITOR, SCP_MSG_CONTROL));
}

static void test_matrix_server_and_unknown(void)
{
    /* peer server: only handshake/liveness traffic */
    TEST_ASSERT_TRUE(serverIngestRoleMaySend(INGEST_ROLE_SERVER, SCP_MSG_HELLO));
    TEST_ASSERT_TRUE(serverIngestRoleMaySend(INGEST_ROLE_SERVER, SCP_MSG_WHO_IS_PRIMARY));
    TEST_ASSERT_TRUE(serverIngestRoleMaySend(INGEST_ROLE_SERVER, SCP_MSG_ACK));
    /* a peer server does NOT push host/probe reports into the sink */
    TEST_ASSERT_FALSE(serverIngestRoleMaySend(INGEST_ROLE_SERVER, SCP_MSG_CLIENT_REPORT));
    TEST_ASSERT_FALSE(serverIngestRoleMaySend(INGEST_ROLE_SERVER, SCP_MSG_MONITOR_REPORT));

    /* unknown role may send nothing at all */
    TEST_ASSERT_FALSE(serverIngestRoleMaySend(INGEST_ROLE_UNKNOWN, SCP_MSG_HELLO));
    TEST_ASSERT_FALSE(serverIngestRoleMaySend(INGEST_ROLE_UNKNOWN, SCP_MSG_CLIENT_REPORT));
}

static void test_crl_activation_receipt_parser(void)
{
    static const char receipt[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef:42:"
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    uint8_t payload[256];
    solariTlvWriter writer;
    char issuerFp[65], crlFp[65];
    uint64_t number;
    bool found;

    solariTlvWriterInit(&writer, payload, sizeof payload);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvAppend(&writer, TLV_CRL_ACTIVATION,
                                                     receipt, sizeof receipt - 1));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverIngestParseCrlActivation(payload, writer.len,
        &found, issuerFp, &number, crlFp));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                             issuerFp);
    TEST_ASSERT_EQUAL_UINT64(42, number);
    TEST_ASSERT_EQUAL_STRING("abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
                             crlFp);

    solariTlvWriterInit(&writer, payload, sizeof payload);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvAppend(&writer, TLV_CRL_ACTIVATION,
                                                     "bad", 3));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG, serverIngestParseCrlActivation(payload, writer.len,
        &found, issuerFp, &number, crlFp));

    solariTlvWriterInit(&writer, payload, sizeof payload);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverIngestParseCrlActivation(payload, 0,
        &found, issuerFp, &number, crlFp));
    TEST_ASSERT_FALSE(found);
}

static void test_crl_activation_receipt_parser_rejects_invalid_numbers_and_duplicates(void)
{
    static const char *invalid[] = {
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef:0:"
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef:042:"
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789ABCDEF:42:"
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef:18446744073709551616:"
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
    };
    static const char valid[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef:42:"
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    uint8_t payload[512];
    solariTlvWriter writer;
    char issuerFp[65], crlFp[65];
    uint64_t number;
    bool found;
    size_t i;

    for (i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        solariTlvWriterInit(&writer, payload, sizeof payload);
        TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvAppend(&writer, TLV_CRL_ACTIVATION,
                                                         invalid[i], strlen(invalid[i])));
        TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG, serverIngestParseCrlActivation(payload,
            writer.len, &found, issuerFp, &number, crlFp));
    }

    solariTlvWriterInit(&writer, payload, sizeof payload);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvAppend(&writer, TLV_CRL_ACTIVATION,
                                                     valid, sizeof valid - 1));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvAppend(&writer, TLV_CRL_ACTIVATION,
                                                     valid, sizeof valid - 1));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG, serverIngestParseCrlActivation(payload,
        writer.len, &found, issuerFp, &number, crlFp));
}

static void test_malformed_optional_crl_receipt_does_not_reject_report(void)
{
    uint8_t payload[16];
    solariTlvWriter writer;
    serverContext ctx = {0};
    solariFrameHeader hdr = {0};

    hdr.sourceNodeId = 42;
    solariTlvWriterInit(&writer, payload, sizeof payload);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvAppend(&writer, TLV_CRL_ACTIVATION,
                                                     "bad", 3));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverIngestApplyCrlActivation(
        &ctx, &hdr, payload, writer.len));
    TEST_ASSERT_EQUAL_UINT(0, g_crlAckCalls);
}

static void test_crl_acknowledgement_failure_does_not_reject_committed_report(void)
{
    static const char receipt[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef:42:"
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    uint8_t payload[256];
    solariTlvWriter writer;
    serverContext ctx = {0};
    solariFrameHeader hdr = {0};

    hdr.sourceNodeId = 42;
    solariTlvWriterInit(&writer, payload, sizeof payload);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvAppend(&writer, TLV_CRL_ACTIVATION,
                                                     receipt, sizeof receipt - 1));

    g_crlAckStatus = ERR_REJECTED;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverIngestApplyCrlActivation(
        &ctx, &hdr, payload, writer.len));
    TEST_ASSERT_EQUAL_UINT(1, g_crlAckCalls);

    g_crlAckStatus = ERR_CONN_RETRY;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverIngestApplyCrlActivation(
        &ctx, &hdr, payload, writer.len));
    TEST_ASSERT_EQUAL_UINT(2, g_crlAckCalls);

    g_crlAckStatus = ERR_DB;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverIngestApplyCrlActivation(
        &ctx, &hdr, payload, writer.len));
    TEST_ASSERT_EQUAL_UINT(3, g_crlAckCalls);

    g_crlAckStatus = SOLARI_OK;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverIngestApplyCrlActivation(
        &ctx, &hdr, payload, writer.len));
    TEST_ASSERT_EQUAL_UINT(4, g_crlAckCalls);
}

/* ---- dedup ring ---- */

static void test_dedup_basic(void)
{
    /* fresh process state: distinct (source,seq) are all first-seen misses */
    memset(g_dedup, 0, sizeof g_dedup);

    TEST_ASSERT_FALSE(serverDedupSeen(0xAA, 1, 1000));  /* first */
    TEST_ASSERT_TRUE (serverDedupSeen(0xAA, 1, 1001));  /* replay */
    TEST_ASSERT_FALSE(serverDedupSeen(0xAA, 2, 1002));  /* new seq */
    TEST_ASSERT_FALSE(serverDedupSeen(0xBB, 1, 1003));  /* different source, same seq */
    TEST_ASSERT_TRUE (serverDedupSeen(0xBB, 1, 1004));  /* replay on BB */

    /* node 0 is never a real source: always a miss, never recorded */
    TEST_ASSERT_FALSE(serverDedupSeen(0, 7, 1005));
    TEST_ASSERT_FALSE(serverDedupSeen(0, 7, 1006));
}

static void test_dedup_window_rolls_over(void)
{
    memset(g_dedup, 0, sizeof g_dedup);

    /* fill exactly the window with unique seqs */
    for (uint32_t s = 0; s < SERVER_DEDUP_WINDOW; s++)
        TEST_ASSERT_FALSE(serverDedupSeen(0xC0, s, 2000 + s));

    /* the most recent are still remembered */
    TEST_ASSERT_TRUE(serverDedupSeen(0xC0, SERVER_DEDUP_WINDOW - 1, 9000));

    /* push one past the window; the oldest (seq 0) is now evicted */
    TEST_ASSERT_FALSE(serverDedupSeen(0xC0, SERVER_DEDUP_WINDOW, 9001));
    TEST_ASSERT_FALSE(serverDedupSeen(0xC0, 0, 9002)); /* evicted -> miss again */
}

static void test_report_identity_requires_one_complete_session(void)
{
    solariFrameHeader header;
    serverReportReceipt receipt;
    solariTlvWriter writer;
    uint8_t payload[128], session[16];
    memset(&header, 0, sizeof header);
    memset(session, 0x42, sizeof session);
    header.msgType = SCP_MSG_CLIENT_REPORT;
    header.seqNo = 7;
    solariTlvWriterInit(&writer, payload, sizeof payload);
    TEST_ASSERT_NOT_EQUAL(SOLARI_OK, serverReportIdentity(&header, payload, 0, false, &receipt));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariTlvAppend(&writer, TLV_REPORT_SESSION, session, sizeof session));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverReportIdentity(&header, payload, writer.len, false, &receipt));
    TEST_ASSERT_EQUAL_MEMORY(session, receipt.session, sizeof session);
    TEST_ASSERT_EQUAL_UINT32(7, receipt.seqNo);
    TEST_ASSERT_FALSE(receipt.legacy);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariTlvAppend(&writer, TLV_REPORT_SESSION, session, sizeof session));
    TEST_ASSERT_NOT_EQUAL(SOLARI_OK,
        serverReportIdentity(&header, payload, writer.len, true, &receipt));
}

static void test_legacy_report_admitted_only_one_way_and_opted_in(void)
{
    serverContext ctx;
    serverConfig cfg;
    solariFrameHeader header;
    serverReportReceipt receipt;
    uint8_t payload[1] = {0};
    memset(&ctx, 0, sizeof ctx);
    memset(&header, 0, sizeof header);
    memset(&cfg, 0, sizeof cfg);
    ctx.cfg = &cfg;
    memset(g_dedup, 0, sizeof g_dedup);
    header.msgType = SCP_MSG_CLIENT_REPORT;
    header.sourceNodeId = 0x51;
    header.seqNo = 9;

    /* default: a session-less report is rejected */
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG, serverReportAdmit(&ctx, &header, payload, 0, &receipt));

    /* opted in: admitted as legacy; admission alone does not mark the seqNo,
     * so a write that failed can be retried. Once committed, a replay drops. */
    cfg.allowLegacyReports = true;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverReportAdmit(&ctx, &header, payload, 0, &receipt));
    TEST_ASSERT_TRUE(receipt.legacy);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverReportAdmit(&ctx, &header, payload, 0, &receipt));
    serverDedupMark(header.sourceNodeId, header.seqNo, 1);
    TEST_ASSERT_EQUAL_INT(ERR_CONN_RETRY, serverReportAdmit(&ctx, &header, payload, 0, &receipt));

    /* survey responses are never legacy */
    header.msgType = SCP_MSG_SURVEY_RESP;
    header.seqNo = 11;
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG, serverReportAdmit(&ctx, &header, payload, 0, &receipt));
    header.msgType = SCP_MSG_CLIENT_REPORT;

    /* an ACK-requesting sender must carry a session even when opted in */
    header.seqNo = 10;
    header.flags = SCP_FLAG_ACK_REQ;
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG, serverReportAdmit(&ctx, &header, payload, 0, &receipt));
}

static void test_report_validation_never_marks_before_commit(void)
{
    serverContext ctx;
    solariFrameHeader header;
    memset(&ctx, 0, sizeof ctx);
    memset(&header, 0, sizeof header);
    ctx.db = (serverDb *)1; /* test stub models the approved node lookup */
    header.msgType = SCP_MSG_CLIENT_REPORT;
    header.sourceNodeId = 42;
    header.seqNo = 7;
    g_expectedNodeId = 42;
    g_expectedCertCn = "client.test";
    g_identityLookupStatus = SOLARI_OK;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverIngestValidate(&ctx, &header, "client.test"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverIngestValidate(&ctx, &header, "client.test"));
    header.sourceNodeId = 43;
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverIngestValidate(&ctx, &header, "client.test"));
    header.sourceNodeId = 42;
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverIngestValidate(&ctx, &header, "client.other"));
    g_identityLookupStatus = ERR_DB;
    TEST_ASSERT_EQUAL_INT(ERR_DB,
        serverIngestValidate(&ctx, &header, "client.test"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_role_from_cn);
    RUN_TEST(test_hello_identity_matches_authenticated_certificate);
    RUN_TEST(test_matrix_client);
    RUN_TEST(test_matrix_monitor_is_superset);
    RUN_TEST(test_matrix_server_and_unknown);
    RUN_TEST(test_crl_activation_receipt_parser);
    RUN_TEST(test_crl_activation_receipt_parser_rejects_invalid_numbers_and_duplicates);
    RUN_TEST(test_malformed_optional_crl_receipt_does_not_reject_report);
    RUN_TEST(test_crl_acknowledgement_failure_does_not_reject_committed_report);
    RUN_TEST(test_dedup_basic);
    RUN_TEST(test_dedup_window_rolls_over);
    RUN_TEST(test_report_identity_requires_one_complete_session);
    RUN_TEST(test_legacy_report_admitted_only_one_way_and_opted_in);
    RUN_TEST(test_report_validation_never_marks_before_commit);
    return UNITY_END();
}
