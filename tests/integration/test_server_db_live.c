/*
 * test_server_db_live.c - integration test for serverDb against a LIVE MariaDB.
 *
 * Unlike the pure-helper suite (test_server_db.c), this opens a real connection
 * and drives the actual prepared statements, validating column/placeholder
 * alignment that cannot be checked offline. It SELF-SKIPS (passes, runs nothing)
 * unless SOLARI_TEST_DB is set, so CI without a database stays green.
 *
 * Connection comes from the environment (same vars the server/dashboard use):
 *   SOLARI_TEST_DB  - set to anything to enable this suite
 *   SOLARI_DB_HOST  (default 127.0.0.1)   SOLARI_DB_PORT (default 13306)
 *   SOLARI_DB_NAME  (default solarinet)   SOLARI_DB_USER (default solari)
 *   SOLARI_DB_PASS  (default empty)   SOLARI_DB_SOCKET (optional)
 * The target schema must include the complete monitoring stream through 036.
 */
#include "unity.h"
#include "solari/solariCommon.h"
#include "solari/solariError.h"
#include "solari/solariMsg.h"
#include "solari/solariFrame.h"
#include "solari/solariTlv.h"
#include "solari/solariTime.h"
#include "server.h"
#include "../test_temp_path.h"

#include <mariadb/mysql.h>
#include <pthread.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A fixed, recognizable test node id so reruns converge (writers are upserts). */
#define ITEST_NODE 0x7e57c0de0001ULL

static serverConfig g_cfg;
static serverDb    *g_db;

/* serverDb.c's existing server-tier accessor; kept private to this live test. */
MYSQL *serverDbConn(serverDb *db);

void setUp(void) {}
void tearDown(void) {}

static const char *envOr(const char *k, const char *d)
{
    const char *v = getenv(k);
    return (v && *v) ? v : d;
}

static uint64_t liveCount(const char *sql)
{
    MYSQL_RES *res;
    MYSQL_ROW row;
    MYSQL *conn = serverDbConn(g_db);
    TEST_ASSERT_NOT_NULL(conn);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(conn, sql));
    res = mysql_store_result(conn);
    TEST_ASSERT_NOT_NULL(res);
    row = mysql_fetch_row(res);
    TEST_ASSERT_NOT_NULL(row);
    {
        uint64_t count = strtoull(row[0], NULL, 10);
        mysql_free_result(res);
        return count;
    }
}

static void liveNodeState(uint64_t nodeId, char *out, size_t cap)
{
    char sql[128];
    MYSQL_RES *res;
    MYSQL_ROW row;
    MYSQL *conn = serverDbConn(g_db);
    TEST_ASSERT_NOT_NULL(conn);
    snprintf(sql, sizeof sql, "SELECT state FROM node WHERE nodeId=%llu",
             (unsigned long long)nodeId);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(conn, sql));
    res = mysql_store_result(conn);
    TEST_ASSERT_NOT_NULL(res);
    row = mysql_fetch_row(res);
    TEST_ASSERT_NOT_NULL(row);
    snprintf(out, cap, "%s", row[0] ? row[0] : "");
    mysql_free_result(res);
}

static uint64_t liveNodeAppliedEpoch(uint64_t nodeId)
{
    char sql[160];
    MYSQL_RES *res;
    MYSQL_ROW row;
    MYSQL *conn = serverDbConn(g_db);
    uint64_t epoch = 0;
    TEST_ASSERT_NOT_NULL(conn);
    snprintf(sql, sizeof sql,
             "SELECT COALESCE((SELECT appliedEpoch FROM nodeConfig WHERE nodeId=node.nodeId),0) FROM node WHERE nodeId=%llu",
             (unsigned long long)nodeId);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(conn, sql));
    res = mysql_store_result(conn);
    TEST_ASSERT_NOT_NULL(res);
    row = mysql_fetch_row(res);
    TEST_ASSERT_NOT_NULL(row);
    epoch = strtoull(row[0], NULL, 10);
    mysql_free_result(res);
    return epoch;
}

/* ---- connection is alive ---- */
static void test_open_ping(void)
{
    MYSQL *conn = serverDbConn(g_db);
    MYSQL *killer;
    MYSQL_RES *res;
    MYSQL_ROW row;
    char killSql[80];
    unsigned long threadId;
    const char *sock = g_cfg.dbSocket[0] != '\0' ? g_cfg.dbSocket : NULL;
    TEST_ASSERT_NOT_NULL(conn);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbPing(g_db));
    TEST_ASSERT_EQUAL_INT(0, mysql_query(conn, "SELECT @@session.time_zone"));
    res = mysql_store_result(conn);
    TEST_ASSERT_NOT_NULL(res);
    row = mysql_fetch_row(res);
    TEST_ASSERT_NOT_NULL(row);
    TEST_ASSERT_EQUAL_STRING("+00:00", row[0]);
    mysql_free_result(res);

    threadId = mysql_thread_id(conn);
    killer = mysql_init(NULL);
    TEST_ASSERT_NOT_NULL(killer);
    TEST_ASSERT_NOT_NULL(mysql_real_connect(killer, g_cfg.dbHost, g_cfg.dbUser,
        g_cfg.dbPass, g_cfg.dbName, g_cfg.dbPort, sock, CLIENT_MULTI_STATEMENTS));
    (void)snprintf(killSql, sizeof killSql, "KILL CONNECTION %lu", threadId);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(killer, killSql));
    mysql_close(killer);

    /* mysql_ping triggers the configured reconnect; the init command must
     * restore UTC before the connection is reused. */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbPing(g_db));
    TEST_ASSERT_TRUE(threadId != mysql_thread_id(conn));
    TEST_ASSERT_EQUAL_INT(0, mysql_query(conn, "SELECT @@session.time_zone"));
    res = mysql_store_result(conn);
    TEST_ASSERT_NOT_NULL(res);
    row = mysql_fetch_row(res);
    TEST_ASSERT_NOT_NULL(row);
    TEST_ASSERT_EQUAL_STRING("+00:00", row[0]);
    mysql_free_result(res);
}

/* ---- node upsert / touch / state (incl. the 'retired' enum from 002) ---- */
static void test_node_lifecycle(void)
{
    bool matches = false;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbUpsertNode(g_db, ITEST_NODE, ROLE_CLIENT,
                           "itest.host", "itest-cn", "linux", "x86_64", 7));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbNodeCertMatches(g_db, ITEST_NODE, "itest-cn", &matches));
    TEST_ASSERT_TRUE(matches);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbNodeCertMatches(g_db, ITEST_NODE + 1, "itest-cn", &matches));
    TEST_ASSERT_FALSE(matches);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbNodeCertMatches(g_db, ITEST_NODE, "ITEST-CN", &matches));
    TEST_ASSERT_FALSE(matches); /* certificate identity is byte-exact */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbUpsertNode(g_db, ITEST_NODE, ROLE_CLIENT,
                           "itest.host", "itest-cn-reissued", "linux", "x86_64", 8));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbNodeCertMatches(g_db, ITEST_NODE, "itest-cn", &matches));
    TEST_ASSERT_FALSE(matches); /* reissue replaces the active certificate identity */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbNodeCertMatches(g_db, ITEST_NODE, "itest-cn-reissued", &matches));
    TEST_ASSERT_TRUE(matches);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbTouchNode(g_db, ITEST_NODE, 1700000000000ULL));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbSetNodeState(g_db, ITEST_NODE, "retired"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbNodeCertMatches(g_db, ITEST_NODE, "itest-cn-reissued", &matches));
    TEST_ASSERT_FALSE(matches); /* retirement closes the ingest identity gate */
    /* second upsert exercises the ON DUPLICATE KEY path with a bumped epoch */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbUpsertNode(g_db, ITEST_NODE, ROLE_CLIENT,
                           "itest.host", "itest-cn-reissued", "linux", "x86_64", 9));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbNodeCertMatches(g_db, ITEST_NODE, "itest-cn-reissued", &matches));
    TEST_ASSERT_TRUE(matches); /* fresh approved HELLO reactivates retired ID */
}

/* Verify pooled transaction cleanup on commit and rollback: autocommit is
 * restored and the driver's reconnect option remains effective afterward. */
static void liveKillPoolConnAndVerifyReconnect(void)
{
    MYSQL *conn = serverDbConn(g_db);
    MYSQL *killer;
    MYSQL_RES *res;
    MYSQL_ROW row;
    char killSql[80];
    unsigned long threadId;
    const char *sock = g_cfg.dbSocket[0] != '\0' ? g_cfg.dbSocket : NULL;
    TEST_ASSERT_NOT_NULL(conn);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(conn, "SELECT @@autocommit"));
    res = mysql_store_result(conn);
    TEST_ASSERT_NOT_NULL(res);
    row = mysql_fetch_row(res);
    TEST_ASSERT_NOT_NULL(row);
    TEST_ASSERT_EQUAL_STRING("1", row[0]);
    mysql_free_result(res);

    threadId = mysql_thread_id(conn);
    killer = mysql_init(NULL);
    TEST_ASSERT_NOT_NULL(killer);
    TEST_ASSERT_NOT_NULL(mysql_real_connect(killer, g_cfg.dbHost, g_cfg.dbUser,
        g_cfg.dbPass, g_cfg.dbName, g_cfg.dbPort, sock, CLIENT_MULTI_STATEMENTS));
    (void)snprintf(killSql, sizeof killSql, "KILL CONNECTION %lu", threadId);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(killer, killSql));
    mysql_close(killer);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbPing(g_db));
    TEST_ASSERT_TRUE(threadId != mysql_thread_id(conn));
}

static void test_transaction_session_reset(void)
{
    const uint64_t commitNode = ITEST_NODE + 0x100;
    const char tokenHash[65] = "0000000000000000000000000000000000000000000000000000000000000000";
    uint64_t generation = 0;
    char sql[128];

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbUpsertNode(g_db, commitNode, ROLE_CLIENT, "txn.itest",
                           "txn-itest-cn", "linux", "x86_64", 1));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbRetireNode(g_db, commitNode));
    liveKillPoolConnAndVerifyReconnect();
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbRetireNode(g_db, commitNode + 2));

    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbDecommissionConfirm(g_db, commitNode + 1, 1, "txn-test",
                                    tokenHash, solariNowUnixMs(), &generation));
    liveKillPoolConnAndVerifyReconnect();

    (void)snprintf(sql, sizeof sql, "DELETE FROM node WHERE nodeId=%llu",
                   (unsigned long long)commitNode);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), sql));
}

static void test_pending_control_directive_match(void)
{
    const uint32_t correlation = 0x7e570001U;
    const uint32_t missingEpochCorrelation = 0x7e570002U;
    const uint32_t failedCorrelation = 0x7e570003U;
    const uint32_t nonconvergentCorrelation = 0x7e570006U;
    const uint32_t zeroEpochCorrelation = 0x7e570007U;
    const uint64_t targetEpoch = 91;
    const uint32_t staleCorrelation = 0x7e570004U;
    char staleSql[256];
    (void)snprintf(staleSql, sizeof staleSql,
        "DELETE FROM pendingControlDirective WHERE nodeId=%llu",
        (unsigned long long)ITEST_NODE);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), staleSql));
    (void)snprintf(staleSql, sizeof staleSql,
        "INSERT INTO pendingControlDirective(nodeId,correlationId,verb,targetEpoch,issuedAt) "
        "VALUES(%llu,%u,%u,0,UTC_TIMESTAMP(6)-INTERVAL 8 DAY)",
        (unsigned long long)ITEST_NODE, staleCorrelation, CTRL_RESTART);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), staleSql));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbControlDirectiveAdd(g_db, ITEST_NODE, 0x7e570005U,
                                    CTRL_RESTART, 0));
    (void)snprintf(staleSql, sizeof staleSql,
        "SELECT COUNT(*) FROM pendingControlDirective WHERE nodeId=%llu AND correlationId=%u",
        (unsigned long long)ITEST_NODE, staleCorrelation);
    /* Expired-record maintenance is scheduled separately; an aged orphan must
     * not make operator sends depend on the cleanup query succeeding. */
    TEST_ASSERT_EQUAL_UINT64(1, liveCount(staleSql));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbControlDirectiveCancel(g_db, ITEST_NODE, 0x7e570005U));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbSetNodeTargetEpoch(g_db, ITEST_NODE, targetEpoch, "{}"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbControlDirectiveAdd(g_db, ITEST_NODE, correlation,
                                    CTRL_SET_CONFIG, targetEpoch));
    /* Wrong verb and wrong successful epoch must leave the record pending. */
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbControlResultApply(g_db, ITEST_NODE, correlation,
                                   CTRL_RESTART, targetEpoch, true, 0, "ok", 1700000000000ULL));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbControlResultApply(g_db, ITEST_NODE, correlation,
                                   CTRL_SET_CONFIG, targetEpoch + 1, true, 0, "ok", 1700000000000ULL));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbControlResultApply(g_db, ITEST_NODE, correlation,
                                   CTRL_SET_CONFIG, 0, true, 0, "ok", 1700000000000ULL));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbControlResultApply(g_db, ITEST_NODE, correlation,
                                   CTRL_SET_CONFIG, targetEpoch, true, 0, "ok", 1700000000000ULL));
    TEST_ASSERT_EQUAL_UINT64(targetEpoch, liveNodeAppliedEpoch(ITEST_NODE));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbControlResultApply(g_db, ITEST_NODE, correlation,
                                   CTRL_SET_CONFIG, targetEpoch, true, 0, "ok", 1700000000000ULL));

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbControlDirectiveAdd(g_db, ITEST_NODE, nonconvergentCorrelation,
                                    CTRL_RESTART, 0));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbControlResultApply(g_db, ITEST_NODE, nonconvergentCorrelation,
                                   CTRL_RESTART, 0, false, 0, "ok",
                                   1700000000000ULL));
    TEST_ASSERT_EQUAL_UINT64(targetEpoch, liveNodeAppliedEpoch(ITEST_NODE));

    /* A legacy successful config result without an epoch is consumed but must
     * not claim convergence; later reconciliation can issue a fresh directive. */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbSetNodeTargetEpoch(g_db, ITEST_NODE, targetEpoch + 1, "{}"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbControlDirectiveAdd(g_db, ITEST_NODE, missingEpochCorrelation,
                                    CTRL_SET_CONFIG, targetEpoch + 1));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbControlResultApply(g_db, ITEST_NODE, missingEpochCorrelation,
                                   CTRL_SET_CONFIG, 0, false, 0, "epoch-missing",
                                   1700000000000ULL));
    TEST_ASSERT_EQUAL_UINT64(targetEpoch, liveNodeAppliedEpoch(ITEST_NODE));

    /* A failed config result likewise records its outcome without replacing
     * the last successfully applied epoch. */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbSetNodeTargetEpoch(g_db, ITEST_NODE, targetEpoch + 2, "{}"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbControlDirectiveAdd(g_db, ITEST_NODE, failedCorrelation,
                                    CTRL_SET_CONFIG, targetEpoch + 2));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbControlResultApply(g_db, ITEST_NODE, failedCorrelation,
                                   CTRL_SET_CONFIG, 0, false, 9, "err:9",
                                   1700000000000ULL));
    TEST_ASSERT_EQUAL_UINT64(targetEpoch, liveNodeAppliedEpoch(ITEST_NODE));

    /* Epoch zero is a valid baseline when it matches the directive, distinct
     * from a present but mismatching zero against a nonzero target. */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbSetNodeTargetEpoch(g_db, ITEST_NODE, 0, "{}"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbControlDirectiveAdd(g_db, ITEST_NODE, zeroEpochCorrelation,
                                    CTRL_SET_CONFIG, 0));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbControlResultApply(g_db, ITEST_NODE, zeroEpochCorrelation,
                                   CTRL_SET_CONFIG, 0, true, 0, "ok",
                                   1700000000001ULL));
    TEST_ASSERT_EQUAL_UINT64(0, liveNodeAppliedEpoch(ITEST_NODE));
}

static void test_decommission_challenge(void)
{
    static const char validHash[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    static const char wrongHash[] =
        "1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    bool matches = false;
    uint64_t now = solariNowUnixMs();
    uint64_t generation = 0, issuedGeneration = 0;
    uint64_t appliedBefore;
    serverDecommissionDelivery due[4];
    size_t dueCount = 0, dueIndex;
    bool foundDue = false;
    uint64_t activeNode = ITEST_NODE + 2;
    uint64_t expiredNode = ITEST_NODE + 3;
    uint64_t forcedNode = ITEST_NODE + 4;

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbUpsertNode(g_db, activeNode, ROLE_CLIENT, "decom.host",
                           "client.decom.host", "linux", "x86_64", 1));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbSetNodeState(g_db, activeNode, "up"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbDecommissionIssue(g_db, activeNode, 7, "test-operator",
                                  validHash, now, now + 60000, &issuedGeneration));
    TEST_ASSERT_TRUE(issuedGeneration > 0);
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbDecommissionConfirm(g_db, activeNode, 7, "test-operator",
                                    wrongHash, now + 1, &generation));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbDecommissionConfirm(g_db, activeNode, 3, "test-operator",
                                    validHash, now + 1, &generation));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbDecommissionConfirm(g_db, activeNode, 7, "other-operator",
                                    validHash, now + 1, &generation));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbDecommissionConfirm(g_db, activeNode, 7, "test-operator",
                                    validHash, now + 1, &generation));
    TEST_ASSERT_EQUAL_UINT64(issuedGeneration, generation);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbDecommissionListDue(g_db, now + 5000, due, 4, &dueCount));
    for (dueIndex = 0; dueIndex < dueCount; dueIndex++) {
        if (due[dueIndex].nodeId == activeNode) {
            foundDue = true;
            TEST_ASSERT_EQUAL_UINT64(generation, due[dueIndex].generation);
            TEST_ASSERT_EQUAL_UINT32(0, due[dueIndex].deliveryAttempts);
        }
    }
    TEST_ASSERT_TRUE(foundDue);
    {
        serverContext retryCtx;
        memset(&retryCtx, 0, sizeof retryCtx);
        retryCtx.db = g_db;
        retryCtx.role = SRV_ACTIVE;
        /* No PUB socket: the worker must persist an attempt and leave a future
         * retry due without performing any live network send. */
        serverProvisionRetryDecommissions(&retryCtx, now + 5000);
    }
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbDecommissionListDue(g_db, now + 9000, due, 4, &dueCount));
    for (dueIndex = 0; dueIndex < dueCount; dueIndex++)
        TEST_ASSERT_NOT_EQUAL(activeNode, due[dueIndex].nodeId);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbDecommissionListDue(g_db, now + 11000, due, 4, &dueCount));
    foundDue = false;
    for (dueIndex = 0; dueIndex < dueCount; dueIndex++) {
        if (due[dueIndex].nodeId == activeNode) {
            foundDue = true;
            TEST_ASSERT_EQUAL_UINT32(1, due[dueIndex].deliveryAttempts);
        }
    }
    TEST_ASSERT_TRUE(foundDue);
    {
        serverContext ctx;
        solariTlvWriter writer;
        uint8_t result[64];
        char state[16];
        memset(&ctx, 0, sizeof ctx);
        ctx.db = g_db;
        TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbSetNodeApplied(g_db, activeNode, 42, "before", now));
        appliedBefore = liveNodeAppliedEpoch(activeNode);
        solariTlvWriterInit(&writer, result, sizeof result);
        TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvAppendU8(&writer, TLV_CTRL_VERB, CTRL_DECOMMISSION));
        TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvAppendU16(&writer, TLV_ERROR_CODE, 0));
        TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvAppendU64(&writer, TLV_LIFE_GENERATION, generation + 1));
        TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
            serverControlOnResult(&ctx, activeNode, 0x101, result, writer.len));
        liveNodeState(activeNode, state, sizeof state);
        TEST_ASSERT_EQUAL_STRING("up", state);
        TEST_ASSERT_EQUAL_UINT64(appliedBefore, liveNodeAppliedEpoch(activeNode));

        solariTlvWriterInit(&writer, result, sizeof result);
        TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvAppendU8(&writer, TLV_CTRL_VERB, CTRL_DECOMMISSION));
        TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvAppendU16(&writer, TLV_ERROR_CODE, 0));
        TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariTlvAppendU64(&writer, TLV_LIFE_GENERATION, generation));
        TEST_ASSERT_EQUAL_INT(SOLARI_OK,
            serverControlOnResult(&ctx, activeNode, 0x102, result, writer.len));
        liveNodeState(activeNode, state, sizeof state);
        TEST_ASSERT_EQUAL_STRING("retired", state);
        TEST_ASSERT_EQUAL_UINT64(appliedBefore, liveNodeAppliedEpoch(activeNode));
    }
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbDecommissionListDue(g_db, now + 12000, due, 4, &dueCount));
    for (dueIndex = 0; dueIndex < dueCount; dueIndex++)
        TEST_ASSERT_NOT_EQUAL(activeNode, due[dueIndex].nodeId);
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbDecommissionAck(g_db, activeNode, generation));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbDecommissionConfirm(g_db, activeNode, 7, "test-operator",
                                    validHash, now + 2, &generation));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbNodeCertMatches(g_db, activeNode, "client.decom.host", &matches));
    TEST_ASSERT_FALSE(matches);

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbUpsertNode(g_db, expiredNode, ROLE_CLIENT, "expired.host",
                           "client.expired.host", "linux", "x86_64", 1));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbDecommissionIssue(g_db, expiredNode, 7, "test-operator",
                                  validHash, now - 60000, now - 1, NULL));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbDecommissionConfirm(g_db, expiredNode, 7, "test-operator",
                                    validHash, now, NULL));

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbUpsertNode(g_db, forcedNode, ROLE_CLIENT, "forced.host",
                           "client.forced.host", "linux", "x86_64", 1));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbDecommissionIssue(g_db, forcedNode, 7, "test-operator",
                                  validHash, now, now + 60000, NULL));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbRetireNode(g_db, forcedNode));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbDecommissionConfirm(g_db, forcedNode, 7, "test-operator",
                                    validHash, now + 1, NULL));
}

static void test_certificate_revocation_ledger(void)
{
    static const char issuerFp[] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    static const char leafFp[] =
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    static const char changedFp[] =
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    static const char tokenHash[] =
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
    static const char wrongHash[] =
        "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
    const char *serial = "ABCDEF9876543210";
    const char *operatorSerial = "112233445566";
    const char *operatorFp =
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    const uint64_t nodeId = ITEST_NODE + 20;
    uint64_t now = solariNowUnixMs();
    uint64_t resolvedNodeId = 0;
    uint64_t enrId = 0;
    serverEnrollment enrollment;
    char sql[512], state[16], resolvedIssuer[65];

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbUpsertNode(g_db, nodeId, ROLE_CLIENT, "revoke.itest",
                           "client.revoke.itest", "linux", "x86_64", 1));
    (void)snprintf(sql, sizeof sql,
        "UPDATE node SET leafCertFp='%s',leafCertSerial='%s',leafIssuerFp='%s' WHERE nodeId=%llu",
        leafFp, serial, issuerFp, (unsigned long long)nodeId);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), sql));
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db),
        "UPDATE certificateInventoryState SET complete=0,completedAt=NULL,"
        "completedBy=NULL WHERE singletonId=1"));
    (void)snprintf(sql, sizeof sql,
        "INSERT INTO certificateIssuance "
        "(issuerFp,serialHex,leafCertFp,nodeId,origin,issuedBy,issuedAt) "
        "VALUES ('%s','%s','%s',%llu,'backfill','itest',FROM_UNIXTIME(%llu / 1000))",
        issuerFp, serial, leafFp, (unsigned long long)nodeId,
        (unsigned long long)now);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), sql));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbGetLeafCertificateIssuer(g_db, nodeId, serial, leafFp,
                                         resolvedIssuer, &resolvedNodeId));
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db),
        "UPDATE certificateInventoryState SET complete=1,completedAt=NOW(6),"
        "completedBy='itest' WHERE singletonId=1"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbGetLeafCertificateIssuer(g_db, nodeId, serial, leafFp,
                                         resolvedIssuer, &resolvedNodeId));
    TEST_ASSERT_EQUAL_STRING(issuerFp, resolvedIssuer);
    TEST_ASSERT_EQUAL_UINT64(nodeId, resolvedNodeId);
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbGetLeafCertificateIssuer(g_db, nodeId, serial, changedFp,
                                         resolvedIssuer, &resolvedNodeId));

    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbCertificateRevocationIssue(g_db, nodeId, issuerFp, serial,
            changedFp, "keyCompromise", "revoker", tokenHash, now, now + 60000));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbCertificateRevocationIssue(g_db, nodeId,
            "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
            serial, leafFp, "keyCompromise", "revoker", tokenHash,
            now, now + 60000));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbCertificateRevocationIssue(g_db, nodeId, issuerFp, serial,
            leafFp, "keyCompromise", "revoker", tokenHash,
            now - 120000, now - 60000));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbCertificateRevocationConfirm(g_db, nodeId, issuerFp, serial,
            leafFp, "keyCompromise", "revoker", tokenHash, now));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbCertificateRevocationIssue(g_db, nodeId, issuerFp, serial,
            leafFp, "keyCompromise", "revoker", tokenHash, now, now + 60000));
    (void)snprintf(sql, sizeof sql,
        "UPDATE node SET leafCertFp='%s' WHERE nodeId=%llu",
        changedFp, (unsigned long long)nodeId);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), sql));
    /* Historical leaf revocation remains valid after a replacement is active;
     * it must not retire that replacement node. */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbCertificateRevocationConfirm(g_db, nodeId, issuerFp, serial,
            leafFp, "keyCompromise", "revoker", tokenHash, now + 1));
    (void)snprintf(sql, sizeof sql,
        "UPDATE node SET leafCertFp='%s' WHERE nodeId=%llu",
        leafFp, (unsigned long long)nodeId);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), sql));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbCertificateRevocationConfirm(g_db, nodeId, issuerFp, serial,
            leafFp, "keyCompromise", "revoker", wrongHash, now + 2));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbCertificateRevocationConfirm(g_db, nodeId, issuerFp, serial,
            leafFp, "keyCompromise", "other-operator", tokenHash, now + 3));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbCertificateRevocationConfirm(g_db, nodeId, issuerFp, serial,
            leafFp, "keyCompromise", "revoker", tokenHash, now + 4));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbCertificateRevocationConfirm(g_db, nodeId, issuerFp, serial,
            leafFp, "keyCompromise", "revoker", tokenHash, now + 5));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbCertificateRevocationIssue(g_db, nodeId, issuerFp, serial,
            leafFp, "keyCompromise", "revoker", tokenHash, now + 6, now + 60006));

    (void)snprintf(sql, sizeof sql,
        "SELECT COUNT(*) FROM certificateRevocation WHERE issuerFp='%s' "
        "AND serialHex='%s' AND leafCertFp='%s' AND nodeId=%llu "
        "AND reason='keyCompromise' AND revokedBy='revoker'",
        issuerFp, serial, leafFp, (unsigned long long)nodeId);
    TEST_ASSERT_EQUAL_UINT64(1, liveCount(sql));
    liveNodeState(nodeId, state, sizeof state);
    TEST_ASSERT_EQUAL_STRING("unknown", state); /* replacement remains unretired */

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbRecordIssuedCertificate(g_db, issuerFp, operatorSerial,
            operatorFp, "operator-itest", now + 6));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbGetLeafCertificateIssuer(g_db, 0, operatorSerial, operatorFp,
                                         resolvedIssuer, &resolvedNodeId));
    TEST_ASSERT_EQUAL_STRING(issuerFp, resolvedIssuer);
    TEST_ASSERT_EQUAL_UINT64(0, resolvedNodeId);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbCertificateRevocationIssue(g_db, 0, issuerFp, operatorSerial,
            operatorFp, "superseded", "revoker", tokenHash,
            now + 7, now + 60007));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbCertificateRevocationConfirm(g_db, 0, issuerFp, operatorSerial,
            operatorFp, "superseded", "revoker", tokenHash, now + 8));
    TEST_ASSERT_EQUAL_UINT64(1, liveCount(
        "SELECT COUNT(*) FROM certificateRevocation WHERE "
        "serialHex='112233445566' AND nodeId IS NULL AND enrollmentId IS NULL"));

    memset(&enrollment, 0, sizeof enrollment);
    snprintf(enrollment.host, sizeof enrollment.host, "revoke.itest");
    snprintf(enrollment.ip, sizeof enrollment.ip, "203.0.113.20");
    enrollment.role = ROLE_CLIENT;
    enrollment.status = ENROLL_TOKEN;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbEnrollCreate(g_db, &enrollment, now, &enrId));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbEnrollSetCsr(g_db, enrId, "replacement-csr", "aa:bb:cc"));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbEnrollFinalize(g_db, enrId, nodeId, ROLE_CLIENT,
            "revoke.itest", "client.revoke.itest", "replacement-cert",
            "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
            "112233445566",
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            "itester", now + 7));
    TEST_ASSERT_EQUAL_UINT64(1, liveCount(
        "SELECT COUNT(*) FROM enrollment WHERE host='revoke.itest' "
        "AND status='pending' AND issuedCertPem IS NULL"));
    liveNodeState(nodeId, state, sizeof state);
    TEST_ASSERT_EQUAL_STRING("unknown", state);
    (void)snprintf(sql, sizeof sql,
        "DELETE FROM enrollment WHERE enrId=%llu", (unsigned long long)enrId);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), sql));

    (void)snprintf(sql, sizeof sql,
        "DELETE FROM certificateRevocationChallenge WHERE nodeId=%llu",
        (unsigned long long)nodeId);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), sql));
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db),
        "DELETE FROM certificateRevocationChallenge WHERE serialHex='112233445566'"));
    (void)snprintf(sql, sizeof sql,
        "DELETE FROM certificateRevocation WHERE nodeId=%llu",
        (unsigned long long)nodeId);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), sql));
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db),
        "DELETE FROM certificateRevocation WHERE serialHex='112233445566'"));
    (void)snprintf(sql, sizeof sql,
        "DELETE FROM certificateIssuance WHERE nodeId=%llu",
        (unsigned long long)nodeId);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), sql));
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db),
        "DELETE FROM certificateIssuance WHERE serialHex='112233445566'"));
    (void)snprintf(sql, sizeof sql, "DELETE FROM node WHERE nodeId=%llu",
                   (unsigned long long)nodeId);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), sql));
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db),
        "UPDATE certificateInventoryState SET complete=0,completedAt=NULL,"
        "completedBy=NULL WHERE singletonId=1"));
}

static solariStatus buildCrlReceiptReportFrame(uint64_t nodeId, uint32_t seqNo,
                                               const uint8_t session[16],
                                               const char *receipt,
                                               uint8_t *frame, size_t frameCap,
                                               size_t *frameLen)
{
    solariFrameHeader header;
    solariTlvWriter writer;
    uint8_t payload[256];
    solariStatus rc;

    solariTlvWriterInit(&writer, payload, sizeof payload);
    rc = solariTlvAppend(&writer, TLV_REPORT_SESSION, session, 16);
    if (rc != SOLARI_OK) return rc;
    rc = solariTlvAppend(&writer, TLV_CRL_ACTIVATION, receipt,
                         (uint16_t)strlen(receipt));
    if (rc != SOLARI_OK) return rc;
    memset(&header, 0, sizeof header);
    header.magic[0] = SCP_MAGIC_0;
    header.magic[1] = SCP_MAGIC_1;
    header.protoVersion = SCP_PROTO_VERSION;
    header.msgType = SCP_MSG_CLIENT_REPORT;
    header.flags = SCP_FLAG_ACK_REQ;
    header.tlvCount = writer.count;
    header.sourceNodeId = nodeId;
    header.sendTimeUnixMs = solariNowUnixMs();
    header.seqNo = seqNo;
    return solariFrameBuild(&header, payload, writer.len, frame, frameCap, frameLen);
}

static void test_certificate_crl_acknowledgement(void)
{
    const uint64_t nodeId = 0x7e57c0de0099ULL;
    const char *issuer = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const char *fp1 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    const char *fp2 = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    const char *receiptText =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa:20:"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    const char *receiptText21 =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa:21:"
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    char sql[512];
    serverContext ctx;
    solariFrameHeader header;
    const uint8_t *payload = NULL;
    uint8_t frame[512];
    uint8_t reportSession[16] = {
        0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef
    };
    size_t frameLen = 0, payloadLen = 0;

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbUpsertNode(g_db, nodeId, ROLE_CLIENT,
        "crl-ack.itest", "client.crl-ack.itest", "linux", "x86_64", 1));
    (void)snprintf(sql, sizeof sql,
        "DELETE FROM certificateCrlAcknowledgement WHERE nodeId=%llu",
        (unsigned long long)nodeId);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), sql));
    (void)snprintf(sql, sizeof sql,
        "DELETE FROM certificateCrlState WHERE issuerFp='%s'", issuer);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), sql));
    (void)snprintf(sql, sizeof sql,
        "INSERT INTO certificateCrlState(issuerFp,nextNumber,publishedNumber,crlFp,nextUpdate) "
        "VALUES('%s',21,20,'%s',UTC_TIMESTAMP(6)+INTERVAL 1 DAY)", issuer, fp1);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), sql));

    /* Construct the reporter's two report TLVs directly, keeping this live DB
     * target independent of the optional transport + SQLite reporter library. */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        buildCrlReceiptReportFrame(nodeId, 1, reportSession, receiptText,
                                   frame, sizeof frame, &frameLen));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariFrameParse(frame, frameLen, &header, &payload, &payloadLen, NULL));
    memset(&ctx, 0, sizeof ctx);
    ctx.db = g_db;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverIngestValidate(&ctx, &header, "client.crl-ack.itest"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverIngestDispatch(&ctx, &header, payload, payloadLen));
    TEST_ASSERT_EQUAL_UINT64(1, liveCount(
        "SELECT COUNT(*) FROM certificateCrlAcknowledgement WHERE nodeId=0x7e57c0de0099 "
        "AND crlNumber=20 AND crlFp='bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbCertificateCrlAcknowledge(g_db, nodeId, issuer, 20, fp1));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbCertificateCrlAcknowledge(g_db, nodeId, issuer, 20, fp1));
    TEST_ASSERT_EQUAL_INT(ERR_REJECTED,
        serverDbCertificateCrlAcknowledge(g_db, nodeId, issuer, 19, fp1));
    TEST_ASSERT_EQUAL_INT(ERR_REJECTED,
        serverDbCertificateCrlAcknowledge(g_db, nodeId, issuer, 20, fp2));
    TEST_ASSERT_EQUAL_UINT64(1, liveCount(
        "SELECT COUNT(*) FROM certificateCrlAcknowledgement WHERE nodeId=0x7e57c0de0099 "
        "AND crlNumber=20 AND crlFp='bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'"));

    (void)snprintf(sql, sizeof sql,
        "UPDATE certificateCrlState SET publishedNumber=21,crlFp='%s',"
        "nextUpdate=UTC_TIMESTAMP(6)+INTERVAL 1 DAY WHERE issuerFp='%s'", fp2, issuer);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), sql));
    reportSession[0]++;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        buildCrlReceiptReportFrame(nodeId, 2, reportSession, receiptText,
                                   frame, sizeof frame, &frameLen));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariFrameParse(frame, frameLen, &header, &payload, &payloadLen, NULL));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverIngestValidate(&ctx, &header, "client.crl-ack.itest"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverIngestDispatch(&ctx, &header, payload, payloadLen));
    TEST_ASSERT_EQUAL_UINT64(1, liveCount(
        "SELECT COUNT(*) FROM certificateCrlAcknowledgement WHERE nodeId=0x7e57c0de0099 "
        "AND crlNumber=20 AND crlFp='bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'"));
    TEST_ASSERT_EQUAL_INT(ERR_REJECTED,
        serverDbCertificateCrlAcknowledge(g_db, nodeId, issuer, 20, fp1));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbCertificateCrlAcknowledge(g_db, nodeId, issuer, 21, fp2));
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db),
        "UPDATE certificateCrlAcknowledgement SET acknowledgedAt='2000-01-01 00:00:00.000000' "
        "WHERE nodeId=0x7e57c0de0099 AND crlNumber=21"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbCertificateCrlAcknowledge(g_db, nodeId, issuer, 21, fp2));
    TEST_ASSERT_EQUAL_UINT64(1, liveCount(
        "SELECT COUNT(*) FROM certificateCrlAcknowledgement WHERE nodeId=0x7e57c0de0099 "
        "AND crlNumber=21 AND acknowledgedAt>'2000-01-01 00:00:00.000000'"));
    TEST_ASSERT_EQUAL_UINT64(1, liveCount(
        "SELECT COUNT(*) FROM certificateCrlAcknowledgement WHERE nodeId=0x7e57c0de0099 "
        "AND crlNumber=21 AND crlFp='cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc'"));

    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db),
        "UPDATE certificateCrlAcknowledgement SET acknowledgedAt='2000-01-01 00:00:00.000000' "
        "WHERE nodeId=0x7e57c0de0099 AND crlNumber=21"));

    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db),
        "UPDATE certificateCrlState SET nextUpdate=UTC_TIMESTAMP(6)-INTERVAL 1 SECOND "
        "WHERE issuerFp='aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'"));
    reportSession[0]++;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        buildCrlReceiptReportFrame(nodeId, 3, reportSession, receiptText21,
                                   frame, sizeof frame, &frameLen));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariFrameParse(frame, frameLen, &header, &payload, &payloadLen, NULL));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverIngestValidate(&ctx, &header, "client.crl-ack.itest"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverIngestDispatch(&ctx, &header, payload, payloadLen));
    TEST_ASSERT_EQUAL_INT(ERR_REJECTED,
        serverDbCertificateCrlAcknowledge(g_db, nodeId, issuer, 21, fp2));
    TEST_ASSERT_EQUAL_UINT64(1, liveCount(
        "SELECT COUNT(*) FROM certificateCrlAcknowledgement WHERE nodeId=0x7e57c0de0099 "
        "AND crlNumber=21 AND acknowledgedAt='2000-01-01 00:00:00.000000'"));

    (void)snprintf(sql, sizeof sql, "DELETE FROM node WHERE nodeId=%llu",
                   (unsigned long long)nodeId);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), sql));
    TEST_ASSERT_EQUAL_UINT64(0, liveCount(
        "SELECT COUNT(*) FROM certificateCrlAcknowledgement WHERE nodeId=0x7e57c0de0099"));
    (void)snprintf(sql, sizeof sql,
        "DELETE FROM certificateCrlState WHERE issuerFp='%s'", issuer);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), sql));
}

/* ---- the heavy writer: hostCurrent upsert + hostHistory append, one txn ---- */
static void test_client_report(void)
{
    serverContext ctx;
    solariClientReport rep;
    serverReportReceipt receipt;
    uint64_t before;
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db),
        "DELETE FROM reportReceipt WHERE nodeId=0x7e57c0de0001"));
    before = liveCount("SELECT COUNT(*) FROM hostHistory WHERE nodeId=0x7e57c0de0001");
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = &g_cfg;
    ctx.db  = g_db;

    memset(&rep, 0, sizeof rep);
    snprintf(rep.hostFqdn, sizeof rep.hostFqdn, "itest.host");
    snprintf(rep.osName,   sizeof rep.osName,   "linux");
    snprintf(rep.arch,     sizeof rep.arch,     "x86_64");
    rep.coreCount      = 2;
    rep.cpuLoadMilli[0] = 250;   /* 25.0% */
    rep.cpuLoadMilli[1] = 400;   /* 40.0% */
    rep.ramUsedKb  = 4ULL * 1024 * 1024;
    rep.ramTotalKb = 16ULL * 1024 * 1024;
    memset(&receipt, 0, sizeof receipt);
    memset(receipt.session, 0x42, sizeof receipt.session);
    receipt.seqNo = 1;
    receipt.msgType = SCP_MSG_CLIENT_REPORT;

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbWriteClientReport(&ctx, ITEST_NODE, &rep, &receipt));
    TEST_ASSERT_EQUAL_UINT64(before + 1,
        liveCount("SELECT COUNT(*) FROM hostHistory WHERE nodeId=0x7e57c0de0001"));

    /* Reopen the DB handle: no process-local dedup state can protect this replay. */
    serverDbClose(g_db);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbOpen(&g_cfg, &g_db));
    ctx.db = g_db;
    TEST_ASSERT_EQUAL_INT(ERR_CONN_RETRY,
        serverDbWriteClientReport(&ctx, ITEST_NODE, &rep, &receipt));
    TEST_ASSERT_EQUAL_UINT64(before + 1,
        liveCount("SELECT COUNT(*) FROM hostHistory WHERE nodeId=0x7e57c0de0001"));
    receipt.session[0]++;                        /* sender restart, same seqNo */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbWriteClientReport(&ctx, ITEST_NODE, &rep, &receipt));
    TEST_ASSERT_EQUAL_UINT64(before + 2,
        liveCount("SELECT COUNT(*) FROM hostHistory WHERE nodeId=0x7e57c0de0001"));
    receipt.payloadHash[0]++;                    /* same identity, changed content */
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG,
        serverDbWriteClientReport(&ctx, ITEST_NODE, &rep, &receipt));
    TEST_ASSERT_EQUAL_UINT64(before + 2,
        liveCount("SELECT COUNT(*) FROM hostHistory WHERE nodeId=0x7e57c0de0001"));
    receipt.session[0] = 0x42;
    receipt.payloadHash[0] = 0;
    for (receipt.seqNo = 2; receipt.seqNo < SERVER_DEDUP_WINDOW + 2; receipt.seqNo++) {
        TEST_ASSERT_EQUAL_INT(SOLARI_OK,
            serverDbWriteClientReport(&ctx, ITEST_NODE, &rep, &receipt));
    }
    receipt.seqNo = 1;
    TEST_ASSERT_EQUAL_INT(ERR_CONN_RETRY,
        serverDbWriteClientReport(&ctx, ITEST_NODE, &rep, &receipt));
    TEST_ASSERT_EQUAL_UINT64(before + 2 + SERVER_DEDUP_WINDOW,
        liveCount("SELECT COUNT(*) FROM hostHistory WHERE nodeId=0x7e57c0de0001"));
}

static void test_failed_report_does_not_poison_receipt(void)
{
    serverContext ctx;
    solariClientReport report;
    serverReportReceipt receipt;
    memset(&ctx, 0, sizeof ctx);
    memset(&report, 0, sizeof report);
    memset(&receipt, 0x55, sizeof receipt);
    receipt.legacy = false;
    receipt.msgType = SCP_MSG_CLIENT_REPORT;
    ctx.cfg = &g_cfg;
    ctx.db = g_db;
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db),
        "DELETE FROM reportReceipt WHERE nodeId=0x7e57c0de0002"));
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db),
        "DELETE FROM node WHERE nodeId=0x7e57c0de0002"));
    TEST_ASSERT_EQUAL_INT(ERR_DB,
        serverDbWriteClientReport(&ctx, ITEST_NODE + 1, &report, &receipt));
    TEST_ASSERT_EQUAL_UINT64(0,
        liveCount("SELECT COUNT(*) FROM reportReceipt WHERE nodeId=0x7e57c0de0002"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbUpsertNode(g_db, ITEST_NODE + 1, ROLE_CLIENT,
                           "retry.itest", "retry-cn", "linux", "x86_64", 1));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbWriteClientReport(&ctx, ITEST_NODE + 1, &report, &receipt));
    TEST_ASSERT_EQUAL_UINT64(1,
        liveCount("SELECT COUNT(*) FROM reportReceipt WHERE nodeId=0x7e57c0de0002"));
}

static void test_monitor_report_is_persisted_once(void)
{
    serverContext ctx;
    solariMonitorReport report;
    serverReportReceipt receipt;
    uint64_t before;
    memset(&ctx, 0, sizeof ctx);
    memset(&report, 0, sizeof report);
    memset(&receipt, 0x66, sizeof receipt);
    receipt.legacy = false;
    ctx.cfg = &g_cfg;
    ctx.db = g_db;
    receipt.msgType = SCP_MSG_MONITOR_REPORT;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbUpsertNode(g_db, ITEST_NODE + 2, ROLE_MONITOR,
                           "monitor.itest", "monitor-cn", "linux", "x86_64", 1));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbUpsertProbeTarget(g_db,
        "tcp:203.0.113.19:443", "203.0.113.19", 443, "tcp", 1,
        "receipt test", NULL, 0, "tcp", NULL));
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db),
        "DELETE FROM reportReceipt WHERE nodeId=0x7e57c0de0003"));
    before = liveCount("SELECT COUNT(*) FROM probeHistory WHERE monitorNode=0x7e57c0de0003");
    report.probeCount = 1;
    report.probes[0].proto = 2;
    report.probes[0].dstPort = 443;
    report.probes[0].dstAddr[10] = report.probes[0].dstAddr[11] = 0xff;
    report.probes[0].dstAddr[12] = 203;
    report.probes[0].dstAddr[13] = 0;
    report.probes[0].dstAddr[14] = 113;
    report.probes[0].dstAddr[15] = 19;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbWriteMonitorReport(&ctx, ITEST_NODE + 2, &report, &receipt));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverMasterReconcile(&ctx, ITEST_NODE + 2, &report));
    TEST_ASSERT_EQUAL_INT(ERR_CONN_RETRY,
        serverDbWriteMonitorReport(&ctx, ITEST_NODE + 2, &report, &receipt));
    TEST_ASSERT_EQUAL_UINT64(before + 1,
        liveCount("SELECT COUNT(*) FROM probeHistory WHERE monitorNode=0x7e57c0de0003"));
}

/* ---- audit/alert row (ruleId 0, NULL targetId = host-scoped) ---- */
static void test_alert_event(void)
{
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbWriteAlertEvent(g_db, 0, ITEST_NODE, NULL,
                                "crit", "integration audit row",
                                1700000000000ULL, "audit", 0, 0, 2,
                                "suppress", NULL));
}

/* A deliberately-invalid ENUM value fails only after the transition has
 * inserted its recovery row and deleted both targets.  Replacing rollback with
 * commit therefore leaves observable partial state and fails these assertions. */
static void test_lifecycle_transition_rolls_back_cascade(void)
{
    const char *ip = "203.0.113.250";
    const char *targetA = "tcp:203.0.113.250:18081";
    const char *targetB = "tcp:203.0.113.250:18082";

    /* CONTRACT-LC §9 J8 hardening: this case purges probe targets and writes
     * alert rows — it REFUSES the production schema outright. SOLARI_DB_NAME
     * defaults to "solarinet", so an operator who only sets the enable flag
     * (SOLARI_TEST_DB=1) must fail HERE, loudly, not run against live data. */
    if (strcmp(g_cfg.dbName, "solarinet") == 0) {
        TEST_FAIL_MESSAGE("destructive lifecycle case refuses the production "
                          "schema; set SOLARI_DB_NAME to a staging clone "
                          "(e.g. solarinet_stage)");
        return;
    }
    char targets[2][SERVER_TARGETID_MAX];
    char sql[256];
    uint64_t assetId = 0, firedEvent = 0;
    size_t count = 2;
    bool active = false;

    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbLifecycleTransition(g_db, 0x7e57c0defffeULL,
                                    "decommissioned", targets, 0));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbUpsertAsset(g_db, ip, "lc-rollback.itest", "lc rollback",
                            "host", 0, NULL, NULL, true, &assetId));
    TEST_ASSERT_TRUE(assetId > 0);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbUpsertProbeTarget(g_db, targetA, ip, 18081, "tcp", 1,
                                  "lc rollback A", NULL, assetId, "tcp", NULL));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbUpsertProbeTarget(g_db, targetB, ip, 18082, "tcp", 1,
                                  "lc rollback B", NULL, assetId, "tcp", NULL));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbWriteAlertEvent(g_db, 0, ITEST_NODE, targetA, "crit",
                                "lc rollback fired", 1700000000000ULL,
                                "fired", 0, assetId, 2, "publish", &firedEvent));
    TEST_ASSERT_TRUE(firedEvent > 0);
    snprintf(targets[0], sizeof targets[0], "%s", targetA);
    snprintf(targets[1], sizeof targets[1], "%s", targetB);

    TEST_ASSERT_NOT_EQUAL(SOLARI_OK,
        serverDbLifecycleTransition(g_db, assetId, "not-a-lifecycle", targets, count));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbAssetIsActive(g_db, assetId, &active));
    TEST_ASSERT_TRUE(active);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbListAssetTargets(g_db, assetId, targets, 2, &count));
    TEST_ASSERT_EQUAL_UINT64(2, count);
    snprintf(sql, sizeof sql,
             "SELECT COUNT(*) FROM alertEvent WHERE openedEventId=%llu AND eventKind='cleared'",
             (unsigned long long)firedEvent);
    TEST_ASSERT_EQUAL_UINT64(0, liveCount(sql));

    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbPurgeAsset(g_db, 0x7e57c0deffffULL, NULL, 0));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbPurgeAsset(g_db, assetId, targets, count));
    snprintf(sql, sizeof sql,
             "DELETE FROM alertEvent WHERE eventId=%llu OR openedEventId=%llu",
             (unsigned long long)firedEvent, (unsigned long long)firedEvent);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), sql));
}

/* ---- discovered upsert keyed (ip,kind): seenCount bump + read-back ---- */
static void test_discovered(void)
{
    serverDiscEntity e, got;
    uint64_t discId = 0, discId2 = 0;

    memset(&e, 0, sizeof e);
    snprintf(e.ip,           sizeof e.ip,           "203.0.113.7");
    snprintf(e.kind,         sizeof e.kind,         "host");
    snprintf(e.via,          sizeof e.via,          "arp");
    snprintf(e.host,         sizeof e.host,         "disc.itest");
    snprintf(e.servicesJson, sizeof e.servicesJson, "[\"ssh:22\"]");
    snprintf(e.arch,         sizeof e.arch,         "x86_64");

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbUpsertDiscovered(g_db, &e, 1700000000000ULL, &discId));
    TEST_ASSERT_TRUE(discId > 0);
    /* re-upsert the same (ip,kind) must converge to the same row */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbUpsertDiscovered(g_db, &e, 1700000001000ULL, &discId2));
    TEST_ASSERT_EQUAL_UINT64(discId, discId2);

    memset(&got, 0, sizeof got);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbGetDiscovered(g_db, discId, &got));
    TEST_ASSERT_EQUAL_STRING("203.0.113.7", got.ip);
    TEST_ASSERT_EQUAL_STRING("host", got.kind);

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbSetDiscoveredStatus(g_db, discId, "adopted"));
}

/* ---- enrollment state machine: create -> CSR -> atomic approval ---- */
static void test_enrollment(void)
{
    serverEnrollment e, got;
    uint64_t enrId = 0, nodeId = 0x7e57c0de0017ULL;
    const uint64_t rollbackNodeId = 0x7e57c0de0018ULL;
    char sql[512];

    /* The live-DB suite may be rerun against the same disposable schema. Clear
     * only these deterministic fixtures so prior approved enrollment rows and
     * node IDs cannot turn the next run into a false failure. */
    (void)snprintf(sql, sizeof sql,
        "DELETE FROM certificateIssuance WHERE nodeId IN (%llu,%llu) "
        "OR enrollmentId IN (SELECT enrId FROM enrollment WHERE host IN "
        "('enr.itest','enr.rollback.itest'))",
        (unsigned long long)nodeId, (unsigned long long)rollbackNodeId);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), sql));
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db),
        "DELETE FROM enrollment WHERE host IN ('enr.itest','enr.rollback.itest')"));
    (void)snprintf(sql, sizeof sql,
        "DELETE FROM node WHERE nodeId IN (%llu,%llu)",
        (unsigned long long)nodeId, (unsigned long long)rollbackNodeId);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), sql));

    memset(&e, 0, sizeof e);
    snprintf(e.host, sizeof e.host, "enr.itest");
    snprintf(e.ip,   sizeof e.ip,   "203.0.113.8");
    e.role   = ROLE_MONITOR;
    e.status = ENROLL_TOKEN;

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbEnrollCreate(g_db, &e, 1700000000000ULL, &enrId));
    TEST_ASSERT_TRUE(enrId > 0);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbEnrollSetCsr(g_db, enrId, "csr-test-pem", "aa:bb:cc"));

    memset(&got, 0, sizeof got);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbEnrollGet(g_db, enrId, &got));
    TEST_ASSERT_EQUAL_INT(ROLE_MONITOR, got.role);
    TEST_ASSERT_EQUAL_STRING("203.0.113.8", got.ip);

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbEnrollFinalize(g_db, enrId, nodeId, ROLE_MONITOR,
                               "enr.itest", "monitor.enr.itest",
                               "issued-test-pem",
                               "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                               "ABCDEF123456",
                               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                               "itester", 1700000002000ULL));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbEnrollFinalize(g_db, enrId, nodeId, ROLE_MONITOR,
                               "enr.itest", "monitor.enr.itest",
                               "duplicate-cert",
                               "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                               "ABCDEF123456",
                               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                               "itester", 1700000003000ULL));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbEnrollGet(g_db, enrId, &got));
    TEST_ASSERT_EQUAL_INT(ENROLL_APPROVED, got.status);

    (void)snprintf(sql, sizeof sql,
        "SELECT COUNT(*) FROM enrollment WHERE enrId=%llu AND status='approved' "
        "AND csrPem IS NULL AND nodeId=%llu AND issuedCertPem='issued-test-pem'",
        (unsigned long long)enrId, (unsigned long long)nodeId);
    TEST_ASSERT_EQUAL_UINT64(1, liveCount(sql));
    (void)snprintf(sql, sizeof sql,
        "SELECT COUNT(*) FROM node WHERE nodeId=%llu AND "
        "leafCertFp='0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef' "
        "AND leafCertSerial='ABCDEF123456' "
        "AND leafIssuerFp='aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'",
        (unsigned long long)nodeId);
    TEST_ASSERT_EQUAL_UINT64(1, liveCount(sql));
    (void)snprintf(sql, sizeof sql,
        "SELECT COUNT(*) FROM certificateIssuance WHERE enrollmentId=%llu "
        "AND issuerFp='aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' "
        "AND serialHex='ABCDEF123456' "
        "AND leafCertFp='0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef'",
        (unsigned long long)enrId);
    TEST_ASSERT_EQUAL_UINT64(1, liveCount(sql));
    (void)snprintf(sql, sizeof sql,
        "SELECT COUNT(*) FROM node WHERE nodeId=%llu AND role='monitor' "
        "AND hostFqdn='enr.itest' AND certCn='monitor.enr.itest'",
        (unsigned long long)nodeId);
    TEST_ASSERT_EQUAL_UINT64(1, liveCount(sql));

    /* Force the node upsert to fail and prove the enrollment update rolls back
     * with it instead of leaving an approved row with no registered node. */
    memset(&e, 0, sizeof e);
    snprintf(e.host, sizeof e.host, "enr.rollback.itest");
    snprintf(e.ip, sizeof e.ip, "203.0.113.9");
    e.role = ROLE_CLIENT;
    e.status = ENROLL_TOKEN;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbEnrollCreate(g_db, &e, 1700000004000ULL, &enrId));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbEnrollSetCsr(g_db, enrId, "rollback-csr", "dd:ee:ff"));
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db),
        "DROP TRIGGER IF EXISTS it_enroll_finalize_fail"));
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db),
        "CREATE TRIGGER it_enroll_finalize_fail BEFORE INSERT ON node "
        "FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='injected enrollment finalize failure'"));
    TEST_ASSERT_EQUAL_INT(ERR_DB,
        serverDbEnrollFinalize(g_db, enrId, rollbackNodeId, ROLE_CLIENT,
                               "enr.rollback.itest", "client.enr.rollback.itest",
                               "rollback-cert",
                               "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                               "ABCDEF123456",
                               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                               "itester", 1700000005000ULL));
    (void)snprintf(sql, sizeof sql,
        "SELECT COUNT(*) FROM enrollment WHERE enrId=%llu AND status='pending' "
        "AND csrPem='rollback-csr' AND issuedCertPem IS NULL AND nodeId IS NULL",
        (unsigned long long)enrId);
    TEST_ASSERT_EQUAL_UINT64(1, liveCount(sql));
    (void)snprintf(sql, sizeof sql,
        "SELECT COUNT(*) FROM node WHERE nodeId=%llu",
        (unsigned long long)rollbackNodeId);
    TEST_ASSERT_EQUAL_UINT64(0, liveCount(sql));
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db),
        "DROP TRIGGER IF EXISTS it_enroll_finalize_fail"));
}

static void test_enrollment_token(void)
{
    serverEnrollment e, got;
    const char *tokenHash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const char *expiredHash = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    char cert[128], sql[512];
    uint64_t enrId = 0, expiredId = 0, nodeId = 0x7e57c0de0019ULL;

    /* Keep token enrollment fixtures isolated across suite reruns. */
    (void)snprintf(sql, sizeof sql,
        "DELETE FROM certificateIssuance WHERE nodeId=%llu OR enrollmentId IN "
        "(SELECT enrId FROM enrollment WHERE host IN ('token.itest','expired.itest') "
        "OR enrollTokenHash IN "
        "('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',"
        "'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'))",
        (unsigned long long)nodeId);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), sql));
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db),
        "DELETE FROM enrollment WHERE host IN ('token.itest','expired.itest') "
        "OR enrollTokenHash IN "
        "('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',"
        "'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb')"));
    (void)snprintf(sql, sizeof sql, "DELETE FROM node WHERE nodeId=%llu",
                   (unsigned long long)nodeId);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), sql));

    memset(&e, 0, sizeof e);
    snprintf(e.host, sizeof e.host, "token.itest");
    snprintf(e.ip, sizeof e.ip, "203.0.113.10");
    e.role = ROLE_CLIENT;
    e.status = ENROLL_TOKEN;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbEnrollCreateToken(g_db, &e, tokenHash,
                                  1700000100000ULL, 1700000000000ULL, &enrId));
    TEST_ASSERT_TRUE(enrId > 0);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbEnrollSubmitToken(g_db, tokenHash, "token.itest", ROLE_CLIENT,
                                  "csr-token-pem", "11:22:33",
                                  1700000001000ULL));
    /* A retry with the same CSR fingerprint is idempotent after a lost reply. */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbEnrollSubmitToken(g_db, tokenHash, "token.itest", ROLE_CLIENT,
                                  "csr-token-pem", "11:22:33",
                                  1700000002000ULL));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbEnrollSubmitToken(g_db, tokenHash, "other.itest", ROLE_CLIENT,
                                  "csr-token-pem", "11:22:33",
                                  1700000002000ULL));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbEnrollSubmitToken(g_db, tokenHash, "token.itest", ROLE_MONITOR,
                                  "csr-token-pem", "11:22:33",
                                  1700000002000ULL));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbEnrollSubmitToken(g_db, tokenHash, "token.itest", ROLE_CLIENT,
                                  "different-csr", "44:55:66",
                                  1700000002000ULL));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbEnrollFinalize(g_db, enrId, nodeId, ROLE_CLIENT,
                               "token.itest", "client.token.itest",
                               "issued-token-cert",
                               "2222222222222222222222222222222222222222222222222222222222222222",
                               "ABCDEF654321",
                               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                               "itester", 1700000003000ULL));
    memset(&got, 0, sizeof got);
    cert[0] = '\0';
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbEnrollGetByToken(g_db, tokenHash, &got, cert, sizeof cert));
    TEST_ASSERT_EQUAL_INT(ENROLL_APPROVED, got.status);
    TEST_ASSERT_EQUAL_UINT64(nodeId, got.nodeId);
    TEST_ASSERT_EQUAL_STRING("issued-token-cert", cert);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbEnrollGetByToken(g_db, tokenHash, &got, cert, sizeof cert));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbEnrollAckToken(g_db, tokenHash, 1700000004000ULL));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbEnrollGetByToken(g_db, tokenHash, &got, cert, sizeof cert));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbEnrollAckToken(g_db, tokenHash, 1700000005000ULL));
    (void)snprintf(sql, sizeof sql,
        "SELECT COUNT(*) FROM enrollment WHERE enrId=%llu AND status='approved' "
        "AND enrollTokenHash IS NULL AND certAckAt IS NOT NULL",
        (unsigned long long)enrId);
    TEST_ASSERT_EQUAL_UINT64(1, liveCount(sql));

    e.host[0] = '\0';
    snprintf(e.host, sizeof e.host, "expired.itest");
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbEnrollCreateToken(g_db, &e, expiredHash,
                                  1700000000500ULL, 1700000000000ULL, &expiredId));
    TEST_ASSERT_TRUE(expiredId > 0);
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbEnrollSubmitToken(g_db, expiredHash, "expired.itest", ROLE_CLIENT,
                                  "csr-expired", "77:88:99",
                                  1700000001000ULL));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbEnrollDecide(g_db, expiredId, ENROLL_REJECTED, "itester",
                             1700000002000ULL));
    TEST_ASSERT_EQUAL_INT(ERR_AUTH_ROLE,
        serverDbEnrollDecide(g_db, expiredId, ENROLL_APPROVED, "itester",
                             1700000003000ULL));
    (void)snprintf(sql, sizeof sql,
        "SELECT COUNT(*) FROM enrollment WHERE enrId=%llu AND status='rejected' "
        "AND enrollTokenHash IS NULL AND csrPem IS NULL",
        (unsigned long long)expiredId);
    TEST_ASSERT_EQUAL_UINT64(1, liveCount(sql));
}

typedef struct leaseRaceGate {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    unsigned ready;
    bool start;
} leaseRaceGate;

typedef struct leaseRaceClaim {
    const serverConfig *config;
    leaseRaceGate *gate;
    uint64_t nodeId;
    solariStatus status;
    bool won;
    uint64_t epoch;
} leaseRaceClaim;

static void *leaseRaceClaimThread(void *opaque)
{
    leaseRaceClaim *claim = (leaseRaceClaim *)opaque;
    serverDb *db = NULL;
    bool mysqlReady = mysql_thread_init() == 0;

    claim->status = mysqlReady ? serverDbOpen(claim->config, &db) : ERR_DB;
    pthread_mutex_lock(&claim->gate->mutex);
    claim->gate->ready++;
    pthread_cond_broadcast(&claim->gate->condition);
    while (!claim->gate->start)
        pthread_cond_wait(&claim->gate->condition, &claim->gate->mutex);
    pthread_mutex_unlock(&claim->gate->mutex);

    if (claim->status == SOLARI_OK)
        claim->status = serverDbLeaseClaim(db, claim->nodeId, 15,
                                           &claim->won, &claim->epoch);
    serverDbClose(db);
    if (mysqlReady) mysql_thread_end();
    return NULL;
}

static void leaseRaceStart(leaseRaceGate *gate)
{
    pthread_mutex_lock(&gate->mutex);
    while (gate->ready < 2)
        pthread_cond_wait(&gate->condition, &gate->mutex);
    gate->start = true;
    pthread_cond_broadcast(&gate->condition);
    pthread_mutex_unlock(&gate->mutex);
}

static void test_simultaneous_lease_claims_advance_once(void)
{
    unsigned iteration;

    for (iteration = 0; iteration < 8; ++iteration) {
        leaseRaceGate gate = { PTHREAD_MUTEX_INITIALIZER,
                               PTHREAD_COND_INITIALIZER, 0, false };
        leaseRaceClaim claims[2] = {
            { &g_cfg, &gate, ITEST_NODE + 10, ERR_DB, false, 0 },
            { &g_cfg, &gate, ITEST_NODE + 11, ERR_DB, false, 0 }
        };
        pthread_t threads[2];
        uint64_t beforeHolder = 0, beforeEpoch = 0, afterHolder = 0, afterEpoch = 0;
        unsigned created = 0, winners;
        int createResult[2] = { 0, 0 };

        TEST_ASSERT_EQUAL_INT(SOLARI_OK,
            serverDbLeaseRead(g_db, &beforeHolder, &beforeEpoch));
        TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db),
            "UPDATE serverLease SET expiresAt=UTC_TIMESTAMP(3)-INTERVAL 1 SECOND "
            "WHERE id=1"));

        createResult[0] = pthread_create(&threads[0], NULL, leaseRaceClaimThread, &claims[0]);
        if (createResult[0] == 0) created++;
        createResult[1] = pthread_create(&threads[1], NULL, leaseRaceClaimThread, &claims[1]);
        if (createResult[1] == 0) created++;
        if (created == 2) {
            leaseRaceStart(&gate);
            pthread_join(threads[0], NULL);
            pthread_join(threads[1], NULL);
        } else {
            pthread_mutex_lock(&gate.mutex);
            gate.start = true;
            pthread_cond_broadcast(&gate.condition);
            pthread_mutex_unlock(&gate.mutex);
            if (createResult[0] == 0) pthread_join(threads[0], NULL);
            if (createResult[1] == 0) pthread_join(threads[1], NULL);
            TEST_FAIL_MESSAGE("could not start both simultaneous lease claimants");
        }

        TEST_ASSERT_EQUAL_INT(SOLARI_OK, claims[0].status);
        TEST_ASSERT_EQUAL_INT(SOLARI_OK, claims[1].status);
        winners = (claims[0].won ? 1U : 0U) + (claims[1].won ? 1U : 0U);
        TEST_ASSERT_EQUAL_UINT32(1, winners);
        TEST_ASSERT_EQUAL_INT(SOLARI_OK,
            serverDbLeaseRead(g_db, &afterHolder, &afterEpoch));
        TEST_ASSERT_EQUAL_UINT64(beforeEpoch + 1, afterEpoch);
        TEST_ASSERT_TRUE(afterHolder == claims[0].nodeId || afterHolder == claims[1].nodeId);
        TEST_ASSERT_EQUAL_UINT64(afterHolder == claims[0].nodeId ? claims[0].epoch : claims[1].epoch,
                                 afterEpoch);

        pthread_cond_destroy(&gate.condition);
        pthread_mutex_destroy(&gate.mutex);
    }
}

/* ---- lease claim/read: the conditional-UPDATE election ---- */
static void test_lease(void)
{
    serverDb *contenderDb = NULL;
    const uint64_t contender = ITEST_NODE + 1;
    bool won = false;
    uint64_t epoch = 0, holder = 0, rEpoch = 0, formerEpoch = 0;

    /* Isolate this election case from a previous interrupted test run. */
    TEST_ASSERT_EQUAL_INT(0,
        mysql_query(serverDbConn(g_db), "DELETE FROM serverLease WHERE id=1"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbOpen(&g_cfg, &contenderDb));

    /* First claim creates generation 1; renewal by the same live holder keeps it. */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbLeaseClaim(g_db, ITEST_NODE, 15, &won, &epoch));
    TEST_ASSERT_TRUE(won);
    TEST_ASSERT_EQUAL_UINT64(1, epoch);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbLeaseClaim(g_db, ITEST_NODE, 15, &won, &rEpoch));
    TEST_ASSERT_TRUE(won);
    TEST_ASSERT_EQUAL_UINT64(epoch, rEpoch);

    /* A second database connection models a standby polling an unexpired lease.
     * It must lose without changing the active holder's epoch. */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbLeaseClaim(contenderDb, contender, 15, &won, &rEpoch));
    TEST_ASSERT_FALSE(won);
    TEST_ASSERT_EQUAL_UINT64(epoch, rEpoch);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbLeaseRead(g_db, &holder, &rEpoch));
    TEST_ASSERT_EQUAL_UINT64(ITEST_NODE, holder);
    TEST_ASSERT_EQUAL_UINT64(epoch, rEpoch);

    /* Reacquisition by the same node after expiry is still a new fencing
     * generation, even though the holder ID does not change. */
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db),
        "UPDATE serverLease SET expiresAt=UTC_TIMESTAMP(3)-INTERVAL 1 SECOND "
        "WHERE id=1"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbLeaseClaim(g_db, ITEST_NODE, 15, &won, &rEpoch));
    TEST_ASSERT_TRUE(won);
    TEST_ASSERT_EQUAL_UINT64(epoch + 1, rEpoch);

    /* A different node now takes the expired lease and advances exactly once. */
    epoch = rEpoch;
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db),
        "UPDATE serverLease SET expiresAt=UTC_TIMESTAMP(3)-INTERVAL 1 SECOND "
        "WHERE id=1"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbLeaseClaim(contenderDb, contender, 15, &won, &rEpoch));
    TEST_ASSERT_TRUE(won);
    TEST_ASSERT_EQUAL_UINT64(epoch + 1, rEpoch);
    formerEpoch = epoch;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbLeaseClaim(contenderDb, contender, 15, &won, &epoch));
    TEST_ASSERT_TRUE(won);
    TEST_ASSERT_EQUAL_UINT64(rEpoch, epoch);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbLeaseRead(g_db, &holder, &epoch));
    TEST_ASSERT_EQUAL_UINT64(contender, holder);
    TEST_ASSERT_EQUAL_UINT64(rEpoch, epoch);

    /* A stale former holder cannot release the successor's live fencing token. */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbLeaseRelease(g_db, ITEST_NODE, formerEpoch));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbLeaseRead(g_db, &holder, &epoch));
    TEST_ASSERT_EQUAL_UINT64(contender, holder);
    TEST_ASSERT_EQUAL_UINT64(rEpoch, epoch);
    /* The current holder can relinquish only its own token. */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbLeaseRelease(contenderDb, contender, rEpoch));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbLeaseRead(g_db, &holder, &epoch));
    TEST_ASSERT_EQUAL_UINT64(0, holder);
    TEST_ASSERT_EQUAL_UINT64(rEpoch, epoch);

    /* Repeated barrier-released races use two independent DB handles. Exactly
     * one claimant must win each expiry, with one fencing-token increment. */
    test_simultaneous_lease_claims_advance_once();

    serverDbClose(contenderDb);
    TEST_ASSERT_EQUAL_INT(0,
        mysql_query(serverDbConn(g_db), "DELETE FROM serverLease WHERE id=1"));
}

/* ---- durable scan jobs + lease fencing ---- */
static void test_discovery_scan_jobs(void)
{
    serverDb *successorDb = NULL;
    const uint64_t successor = ITEST_NODE + 2;
    uint64_t epoch = 0, nextEpoch = 0, scanId = 0, restartId = 0,
             secondId = 0, thirdId = 0;
    bool won = false;
    serverDiscoveryScanJob job;
    serverDiscEntity entity;
    uint64_t discId = 0;
    memset(&entity, 0, sizeof entity);

    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), "DELETE FROM discoveryScanJob"));
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), "DELETE FROM serverLease WHERE id=1"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbLeaseClaim(g_db, ITEST_NODE, 15, &won, &epoch));
    TEST_ASSERT_TRUE(won);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbDiscoveryScanSubmit(g_db, ITEST_NODE, epoch, "scan-operator",
                                    "10.24.0.0/24", "22,443", 254, &scanId));
    TEST_ASSERT_TRUE(scanId > 0);
    TEST_ASSERT_EQUAL_INT(ERR_CONN_RETRY,
        serverDbDiscoveryScanSubmit(g_db, ITEST_NODE, epoch, "scan-operator",
                                    "10.24.1.0/24", "22", 254, &thirdId));
    TEST_ASSERT_EQUAL_UINT64(0, thirdId);
    TEST_ASSERT_EQUAL_INT(ERR_LEASE_LOST,
        serverDbDiscoveryScanSubmit(g_db, successor, epoch, "scan-operator",
                                    "10.24.1.0/24", "22", 254, &thirdId));

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbDiscoveryScanClaim(g_db, ITEST_NODE, epoch, &job));
    TEST_ASSERT_EQUAL_UINT64(scanId, job.scanId);
    TEST_ASSERT_EQUAL_STRING("scan-operator", job.requestedBy);
    TEST_ASSERT_EQUAL_STRING("10.24.0.0/24", job.cidr);
    TEST_ASSERT_EQUAL_STRING("22,443", job.ports);
    TEST_ASSERT_EQUAL_STRING("running", job.status);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbDiscoveryScanUpdate(g_db, scanId, ITEST_NODE, epoch,
                                    "running", 12, 2, 0, NULL));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbDiscoveryScanUpdate(g_db, scanId, ITEST_NODE, epoch,
                                    "completed", 254, 3, 0, NULL));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbDiscoveryScanGet(g_db, scanId, "scan-operator", &job));
    TEST_ASSERT_EQUAL_STRING("completed", job.status);
    TEST_ASSERT_EQUAL_UINT32(254, job.hostsDone);
    TEST_ASSERT_EQUAL_UINT32(3, job.hostsFound);
    TEST_ASSERT_EQUAL_INT(ERR_TLV_END,
        serverDbDiscoveryScanGet(g_db, scanId, "different-operator", &job));

    /* Simulate an abrupt worker exit on the same host while retaining the
     * current lease epoch. The next singleton worker claim settles that orphan. */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbDiscoveryScanSubmit(g_db, ITEST_NODE, epoch, "scan-operator",
                                    "10.24.0.0/25", "22", 126, &restartId));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbDiscoveryScanClaim(g_db, ITEST_NODE, epoch, &job));
    TEST_ASSERT_EQUAL_UINT64(restartId, job.scanId);
    TEST_ASSERT_EQUAL_INT(ERR_CONN_RETRY,
        serverDbDiscoveryScanClaim(g_db, ITEST_NODE, epoch, &job));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbDiscoveryScanGet(g_db, restartId, "scan-operator", &job));
    TEST_ASSERT_EQUAL_STRING("failed", job.status);

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbDiscoveryScanSubmit(g_db, ITEST_NODE, epoch, "scan-operator",
                                    "10.24.1.0/24", "80", 254, &secondId));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbOpen(&g_cfg, &successorDb));
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db),
        "UPDATE serverLease SET expiresAt=UTC_TIMESTAMP(3)-INTERVAL 1 SECOND WHERE id=1"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbLeaseClaim(successorDb, successor, 15, &won, &nextEpoch));
    TEST_ASSERT_TRUE(won);
    TEST_ASSERT_TRUE(nextEpoch > epoch);
    TEST_ASSERT_EQUAL_INT(ERR_LEASE_LOST,
        serverDbDiscoveryScanUpdate(g_db, secondId, ITEST_NODE, epoch,
                                    "failed", 0, 0, ERR_LEASE_LOST, "lease lost"));

    /* A new owner's admission marks the prior queued job failed before adding
     * its own. This also proves the lease-row transaction serializes admission. */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbDiscoveryScanSubmit(successorDb, successor, nextEpoch, "next-operator",
                                    "10.24.2.0/24", "80", 254, &thirdId));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbDiscoveryScanGet(g_db, secondId, "scan-operator", &job));
    TEST_ASSERT_EQUAL_STRING("failed", job.status);
    snprintf(entity.ip, sizeof entity.ip, "10.24.2.99");
    snprintf(entity.kind, sizeof entity.kind, "host");
    snprintf(entity.via, sizeof entity.via, "portscan");
    TEST_ASSERT_EQUAL_INT(ERR_LEASE_LOST,
        serverDbUpsertDiscoveredFenced(g_db, &entity, 1700000000000ULL,
                                       ITEST_NODE, epoch, &discId));
    TEST_ASSERT_EQUAL_UINT64(0, liveCount(
        "SELECT COUNT(*) FROM discovered WHERE ip='10.24.2.99' AND kind='host'"));

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbDiscoveryScanClaim(successorDb, successor, nextEpoch, &job));
    TEST_ASSERT_EQUAL_UINT64(thirdId, job.scanId);
    TEST_ASSERT_EQUAL_STRING("next-operator", job.requestedBy);
    memset(&entity, 0, sizeof entity);
    snprintf(entity.ip, sizeof entity.ip, "10.24.2.1");
    snprintf(entity.kind, sizeof entity.kind, "host");
    snprintf(entity.via, sizeof entity.via, "portscan");
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbUpsertDiscoveredFenced(successorDb, &entity, 1700000000000ULL,
                                       successor, nextEpoch, &discId));
    TEST_ASSERT_TRUE(discId > 0);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbDiscoveryScanUpdate(successorDb, thirdId, successor, nextEpoch,
                                    "completed", 254, 1, 0, NULL));

    /* Exercise both terminal-history bounds in the actual MariaDB statement.
     * Keep 1,000 recent rows, then prove the age cutoff removes older rows. */
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(successorDb),
        "DELETE FROM discoveryScanJob"));
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(successorDb),
        "INSERT INTO discoveryScanJob "
        "(requestedBy,ownerNodeId,leaseEpoch,cidr,ports,status,hostsTotal,hostsDone,hostsFound,createdAt,updatedAt,completedAt) "
        "SELECT 'retention-operator',1,1,'10.24.3.0/24','80','completed',254,254,0,"
        "UTC_TIMESTAMP(3),UTC_TIMESTAMP(3),UTC_TIMESTAMP(3) "
        "FROM information_schema.COLUMNS a CROSS JOIN information_schema.COLUMNS b LIMIT 1005"));
    TEST_ASSERT_EQUAL_UINT64(1005, liveCount(
        "SELECT COUNT(*) FROM discoveryScanJob WHERE status='completed'"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbDiscoveryScanSubmit(successorDb, successor, nextEpoch,
                                    "retention-operator", "10.24.4.0/24", "80",
                                    254, &scanId));
    TEST_ASSERT_EQUAL_UINT64(1000, liveCount(
        "SELECT COUNT(*) FROM discoveryScanJob WHERE status='completed'"));
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(successorDb),
        "DELETE FROM discoveryScanJob"));
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(successorDb),
        "INSERT INTO discoveryScanJob "
        "(requestedBy,ownerNodeId,leaseEpoch,cidr,ports,status,hostsTotal,hostsDone,hostsFound,createdAt,updatedAt,completedAt) "
        "VALUES ('retention-operator',1,1,'10.24.3.0/24','80','completed',254,254,0,"
        "UTC_TIMESTAMP(3)-INTERVAL 31 DAY,UTC_TIMESTAMP(3)-INTERVAL 31 DAY,"
        "UTC_TIMESTAMP(3)-INTERVAL 31 DAY)"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbDiscoveryScanSubmit(successorDb, successor, nextEpoch,
                                    "retention-operator", "10.24.4.0/24", "80",
                                    254, &scanId));
    TEST_ASSERT_EQUAL_UINT64(0, liveCount(
        "SELECT COUNT(*) FROM discoveryScanJob WHERE status='completed'"));

    TEST_ASSERT_EQUAL_INT(0,
        mysql_query(serverDbConn(g_db), "DELETE FROM discoveryScanJob"));
    TEST_ASSERT_EQUAL_INT(0,
        mysql_query(serverDbConn(successorDb), "DELETE FROM serverLease WHERE id=1"));
    serverDbClose(successorDb);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db),
        "DELETE FROM discovered WHERE ip='10.24.2.1'"));
}

/* ---- privileged control-verb request queue (Task #1) ----
 * Exercises the real prepared statements behind the PHP→host boundary: submit,
 * status read, the claim CAS (exactly one of two claimers wins a single pending
 * row), completion, idempotency-key collapse, and empty-queue signalling. */
static void test_ctl_request_queue(void)
{
    unsigned long long id1 = 0, id2 = 0, idIdem = 0, idIdem2 = 0, idIdemDave = 0;
    char verb[48], args[512], state[16], detail[256], reqBy[128];
    unsigned long long claimedId = 0;

    /* Clean slate for a deterministic FIFO/CAS assertion on rerun. */
    TEST_ASSERT_EQUAL_INT(0,
        mysql_query(serverDbConn(g_db), "DELETE FROM ctlRequest"));

    /* Submit two requests; ids are assigned in order. */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbCtlRequestSubmit(g_db,
        "RETIRE", "node=1 op=carol", "carol", 33, NULL, &id1));
    TEST_ASSERT_TRUE(id1 > 0);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbCtlRequestSubmit(g_db,
        "DECOMMISSION", "node=2 op=carol", "carol", 33, NULL, &id2));
    TEST_ASSERT_TRUE(id2 > id1);

    /* A fresh submit is visible as pending. */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbCtlRequestGet(g_db, id1, "carol", state, sizeof state, detail, sizeof detail));
    TEST_ASSERT_EQUAL_STRING("pending", state);
    TEST_ASSERT_EQUAL_INT(ERR_TLV_END,
        serverDbCtlRequestGet(g_db, id1, "mallory", state, sizeof state, detail, sizeof detail));

    /* Idempotency: the same key from the SAME requester collapses to one row and
     * returns its id twice. */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbCtlRequestSubmit(g_db,
        "SIGN", "cn=host op=carol", "carol", 33, "idem-key-abc", &idIdem));
    TEST_ASSERT_TRUE(idIdem > 0);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbCtlRequestSubmit(g_db,
        "SIGN", "cn=host op=carol", "carol", 33, "idem-key-abc", &idIdem2));
    TEST_ASSERT_EQUAL_UINT64(idIdem, idIdem2);
    TEST_ASSERT_EQUAL_UINT64(3, liveCount("SELECT COUNT(*) FROM ctlRequest"));

    /* Reusing a key for a different payload is a conflict, not a replay of the
     * original request id. This prevents callers from pairing one scan/job id
     * with a different CIDR or other queued operation arguments. */
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG, serverDbCtlRequestSubmit(g_db,
        "SIGN", "cn=other op=carol", "carol", 33, "idem-key-abc", &idIdem2));
    TEST_ASSERT_EQUAL_UINT64(0, idIdem2);
    TEST_ASSERT_EQUAL_UINT64(3, liveCount("SELECT COUNT(*) FROM ctlRequest"));

    /* F10: the SAME key from a DIFFERENT requester must NOT collapse. Idempotency
     * is scoped to (requestedBy, idempotencyKey), so one caller cannot pre-seed a
     * predictable key to make another's submit silently return the first's row.
     * dave's submit creates a distinct row despite reusing carol's key. */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbCtlRequestSubmit(g_db,
        "SIGN", "cn=host op=dave", "dave", 34, "idem-key-abc", &idIdemDave));
    TEST_ASSERT_TRUE(idIdemDave > 0);
    TEST_ASSERT_TRUE(idIdemDave != idIdem);   /* not hijacked into carol's row */
    TEST_ASSERT_EQUAL_UINT64(4, liveCount("SELECT COUNT(*) FROM ctlRequest"));

    /* Claim CAS: the first claim wins the OLDEST pending row (id1). F9: the claim
     * also returns the stored requestedBy so the consumer can bind the operator
     * server-side rather than trusting a PHP-supplied op=. */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbCtlRequestClaim(g_db, "claimerA",
        &claimedId, verb, sizeof verb, args, sizeof args, reqBy, sizeof reqBy));
    TEST_ASSERT_EQUAL_UINT64(id1, claimedId);
    TEST_ASSERT_EQUAL_STRING("RETIRE", verb);
    TEST_ASSERT_EQUAL_STRING("node=1 op=carol", args);
    TEST_ASSERT_EQUAL_STRING("carol", reqBy);

    /* The claimed row is no longer pending. */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbCtlRequestGet(g_db, id1, NULL, state, sizeof state, detail, sizeof detail));
    TEST_ASSERT_EQUAL_STRING("claimed", state);

    /* Complete it; the result text and terminal state land. */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbCtlRequestComplete(g_db, id1, true, "OK retired=1"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbCtlRequestGet(g_db, id1, NULL, state, sizeof state, detail, sizeof detail));
    TEST_ASSERT_EQUAL_STRING("done", state);
    TEST_ASSERT_EQUAL_STRING("OK retired=1", detail);

    /* F8: a second Complete on a settled row affects zero rows (WHERE claimed) and
     * must SIGNAL that with ERR_TLV_END, not a false SOLARI_OK — the caller needs
     * to know the row was already terminal. The row's result is untouched. */
    TEST_ASSERT_EQUAL_INT(ERR_TLV_END,
        serverDbCtlRequestComplete(g_db, id1, false, "should-not-apply"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbCtlRequestGet(g_db, id1, NULL, state, sizeof state, detail, sizeof detail));
    TEST_ASSERT_EQUAL_STRING("done", state);
    TEST_ASSERT_EQUAL_STRING("OK retired=1", detail);

    /* Drain the remaining pending rows FIFO, then the queue reports empty. */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbCtlRequestClaim(g_db, "claimerA",
        &claimedId, verb, sizeof verb, args, sizeof args, reqBy, sizeof reqBy));
    TEST_ASSERT_EQUAL_UINT64(id2, claimedId);   /* FIFO: id2 before the idem rows */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbCtlRequestClaim(g_db, "claimerA",
        &claimedId, verb, sizeof verb, args, sizeof args, reqBy, sizeof reqBy));
    TEST_ASSERT_EQUAL_UINT64(idIdem, claimedId);
    TEST_ASSERT_EQUAL_STRING("carol", reqBy);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbCtlRequestClaim(g_db, "claimerA",
        &claimedId, verb, sizeof verb, args, sizeof args, reqBy, sizeof reqBy));
    TEST_ASSERT_EQUAL_UINT64(idIdemDave, claimedId);
    TEST_ASSERT_EQUAL_STRING("dave", reqBy);    /* F9: per-row operator, not a global */
    TEST_ASSERT_EQUAL_INT(ERR_TLV_END, serverDbCtlRequestClaim(g_db, "claimerA",
        &claimedId, verb, sizeof verb, args, sizeof args, reqBy, sizeof reqBy));

    /* A failed completion routes text to error, not result. */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbCtlRequestComplete(g_db, id2, false, "boom"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbCtlRequestGet(g_db, id2, NULL, state, sizeof state, detail, sizeof detail));
    TEST_ASSERT_EQUAL_STRING("failed", state);
    TEST_ASSERT_EQUAL_STRING("boom", detail);

    /* An unknown request id is distinguishable from a real one. */
    TEST_ASSERT_EQUAL_INT(ERR_TLV_END,
        serverDbCtlRequestGet(g_db, 0xffffffffULL, NULL, state, sizeof state, detail, sizeof detail));

    /* tidy up so a rerun starts clean */
    TEST_ASSERT_EQUAL_INT(0,
        mysql_query(serverDbConn(g_db), "DELETE FROM ctlRequest"));
}

int main(void)
{
    if (!getenv("SOLARI_TEST_DB")) {
        printf("SKIP test_server_db_live: SOLARI_TEST_DB unset "
               "(set it + SOLARI_DB_* to run against a live MariaDB)\n");
        return 0;
    }

    /* SUITE-LEVEL production refusal (CONTRACT-LC §9 J8, hardened after a
     * live near-miss on 2026-08-06: this suite upserts nodes and CLAIMS THE
     * SERVER LEASE — running it against production stole leadership from
     * the live solariServer). SOLARI_DB_NAME defaults to "solarinet", so
     * setting only the enable flag must die here, before any connection. */
    {
        const char *dbName = getenv("SOLARI_DB_NAME");
        if (dbName == NULL || strcmp(dbName, "solarinet") == 0) {
            fprintf(stderr,
                    "FATAL test_server_db_live: refusing the production "
                    "schema \"solarinet\" — every case in this suite writes "
                    "(nodes, leases, alerts). Set SOLARI_DB_NAME to a "
                    "staging clone, e.g. solarinet_stage.\n");
            return 1;
        }
    }

    serverConfigDefaults(&g_cfg);
    snprintf(g_cfg.dbHost, sizeof g_cfg.dbHost, "%s", envOr("SOLARI_DB_HOST", "127.0.0.1"));
    g_cfg.dbPort = (uint16_t)atoi(envOr("SOLARI_DB_PORT", "13306"));
    snprintf(g_cfg.dbName, sizeof g_cfg.dbName, "%s", envOr("SOLARI_DB_NAME", "solarinet"));
    snprintf(g_cfg.dbUser, sizeof g_cfg.dbUser, "%s", envOr("SOLARI_DB_USER", "solari"));
    snprintf(g_cfg.dbPass, sizeof g_cfg.dbPass, "%s", envOr("SOLARI_DB_PASS", ""));
    snprintf(g_cfg.dbSocket, sizeof g_cfg.dbSocket, "%s",
             envOr("SOLARI_DB_SOCKET", ""));
    if (g_cfg.dbSocket[0] != '\0') g_cfg.dbHost[0] = '\0';

    if (serverDbOpen(&g_cfg, &g_db) != SOLARI_OK) {
        printf("FATAL test_server_db_live: serverDbOpen failed for %s:%u/%s\n",
               g_cfg.dbHost, (unsigned)g_cfg.dbPort, g_cfg.dbName);
        return 1;
    }

    UNITY_BEGIN();
    RUN_TEST(test_open_ping);
    RUN_TEST(test_node_lifecycle);
    RUN_TEST(test_transaction_session_reset);
    RUN_TEST(test_pending_control_directive_match);
    RUN_TEST(test_decommission_challenge);
    RUN_TEST(test_certificate_revocation_ledger);
    RUN_TEST(test_certificate_crl_acknowledgement);
    RUN_TEST(test_client_report);
    RUN_TEST(test_failed_report_does_not_poison_receipt);
    RUN_TEST(test_monitor_report_is_persisted_once);
    RUN_TEST(test_alert_event);
    RUN_TEST(test_lifecycle_transition_rolls_back_cascade);
    RUN_TEST(test_discovered);
    RUN_TEST(test_enrollment);
    RUN_TEST(test_enrollment_token);
    RUN_TEST(test_lease);
    RUN_TEST(test_discovery_scan_jobs);
    RUN_TEST(test_ctl_request_queue);
    serverDbClose(g_db);
    return UNITY_END();
}
