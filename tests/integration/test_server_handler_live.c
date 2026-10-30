/* Live handler receipt faults against the same scratch DB used by test_server_db_live. */
#include "unity.h"
#include "client.h"
#include "server.h"
#include "solari/solariFrame.h"
#include "solari/solariNet.h"
#include "solari/solariReporter.h"
#include "solari/solariTime.h"
#include "../test_temp_path.h"

#include <mariadb/mysql.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define main testServerProgramMain
#include "../../src/server/main.c"
#undef main

#define HANDLER_URL "tcp://127.0.0.1:18841"
#define HANDLER_FQDN "handler-receipt.itest"
#define HANDLER_CN "client.handler-receipt.itest"
#define HANDLER_TRIGGER "trg_test_handler_report_failure"

static serverConfig g_serverCfg;
static serverDb *g_db;
static serverContext g_serverCtx;
static solariConn *g_control;
static clientConfig g_clientCfg;
static clientContext g_clientCtx;
static volatile int g_dropAck;
static unsigned g_ackSendCount;
static char g_spoolPath[512];

void setUp(void) {}
void tearDown(void) {}

MYSQL *serverDbConn(serverDb *db);
solariStatus __real_solariConnSend(solariConn *conn, const void *data, size_t len);

solariStatus __wrap_solariConnPeerCn(solariConn *conn, char *out, size_t cap)
{
    (void)conn;
    if (!out || cap <= strlen(HANDLER_CN)) return ERR_INVALID_ARG;
    strcpy(out, HANDLER_CN);
    return SOLARI_OK;
}

solariStatus __wrap_solariConnSend(solariConn *conn, const void *data, size_t len)
{
    solariFrameHeader hdr;
    if (data && solariFrameParse(data, len, &hdr, NULL, NULL, NULL) == SOLARI_OK &&
        hdr.msgType == SCP_MSG_ACK) {
        g_ackSendCount++;
        if (g_dropAck) return ERR_CONN_RETRY;
    }
    return __real_solariConnSend(conn, data, len);
}

static const char *envOr(const char *key, const char *fallback)
{
    const char *value = getenv(key);
    return value && value[0] ? value : fallback;
}

static uint64_t countForNode(const char *table)
{
    char sql[160];
    MYSQL_RES *res;
    MYSQL_ROW row;
    snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM %s WHERE nodeId=%llu",
             table, (unsigned long long)g_clientCtx.nodeId);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(serverDbConn(g_db), sql));
    res = mysql_store_result(serverDbConn(g_db));
    TEST_ASSERT_NOT_NULL(res);
    row = mysql_fetch_row(res);
    TEST_ASSERT_NOT_NULL(row);
    {
        uint64_t value = strtoull(row[0], NULL, 10);
        mysql_free_result(res);
        return value;
    }
}

static void *serveOne(void *unused)
{
    (void)unused;
    serverServiceControl(&g_serverCtx);
    return NULL;
}

static void drainSpoolThroughServerOnce(void)
{
    pthread_t thread;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, serveOne, NULL));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientDrainSpool(&g_clientCtx));
    TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL));
}

static solariClientReport makeReport(uint32_t ramKb)
{
    solariClientReport report;
    memset(&report, 0, sizeof report);
    snprintf(report.hostFqdn, sizeof report.hostFqdn, "%s", HANDLER_FQDN);
    snprintf(report.osName, sizeof report.osName, "linux");
    snprintf(report.arch, sizeof report.arch, "x86_64");
    report.coreCount = 2;
    report.ramTotalKb = 1024 * 1024;
    report.ramUsedKb = ramKb;
    return report;
}

static void openListener(void)
{
    solariConnOpts options;
    memset(&options, 0, sizeof options);
    options.url = HANDLER_URL;
    options.pattern = SOLARI_PATTERN_REP;
    options.recvTimeoutMs = 8000;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariConnListen(&options, &g_control));
    g_serverCtx.control = g_control;
}

static void test_real_control_handler_receipt_faults(void)
{
    solariClientReport report;
    MYSQL *conn = serverDbConn(g_db);
    uint64_t historyBefore, receiptBefore;
    unsigned ackBefore;

    TEST_ASSERT_NOT_NULL(conn);
    TEST_ASSERT_EQUAL_INT(0, mysql_query(conn, "DROP TRIGGER IF EXISTS " HANDLER_TRIGGER));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbUpsertNode(g_db, g_clientCtx.nodeId, ROLE_CLIENT,
                           HANDLER_FQDN, HANDLER_CN, "linux", "x86_64", 0));
    TEST_ASSERT_EQUAL_INT(0, mysql_query(conn,
        "CREATE TRIGGER " HANDLER_TRIGGER " BEFORE INSERT ON hostHistory "
        "FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='injected commit path failure'"));

    historyBefore = countForNode("hostHistory");
    receiptBefore = countForNode("reportReceipt");
    ackBefore = g_ackSendCount;
    report = makeReport(128);
    {
        pthread_t thread;
        TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, serveOne, NULL));
        TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientReportSend(&g_clientCtx, &report));
        TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL));
    }
    TEST_ASSERT_EQUAL_size_t(1, solariReporterSpoolDepth(g_clientCtx.rep));
    TEST_ASSERT_EQUAL_UINT64(historyBefore, countForNode("hostHistory"));
    TEST_ASSERT_EQUAL_UINT64(receiptBefore, countForNode("reportReceipt"));
    TEST_ASSERT_EQUAL_UINT(ackBefore, g_ackSendCount); /* DB failure emitted no ACK */

    TEST_ASSERT_EQUAL_INT(0, mysql_query(conn, "DROP TRIGGER " HANDLER_TRIGGER));
    drainSpoolThroughServerOnce();
    TEST_ASSERT_EQUAL_size_t(0, solariReporterSpoolDepth(g_clientCtx.rep));
    TEST_ASSERT_EQUAL_UINT64(historyBefore + 1, countForNode("hostHistory"));
    TEST_ASSERT_EQUAL_UINT64(receiptBefore + 1, countForNode("reportReceipt"));
    TEST_ASSERT_EQUAL_UINT(ackBefore + 1, g_ackSendCount);

    /* The next report commits while its ACK is intentionally lost. Its retry
     * reaches the same server handler and must receive an idempotent duplicate ACK. */
    report = makeReport(256);
    ackBefore = g_ackSendCount;
    g_dropAck = 1;
    {
        pthread_t thread;
        TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, serveOne, NULL));
        TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientReportSend(&g_clientCtx, &report));
        TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL));
    }
    g_dropAck = 0;
    TEST_ASSERT_EQUAL_UINT(ackBefore + 1, g_ackSendCount);
    TEST_ASSERT_EQUAL_size_t(1, solariReporterSpoolDepth(g_clientCtx.rep));
    TEST_ASSERT_EQUAL_UINT64(historyBefore + 2, countForNode("hostHistory"));
    TEST_ASSERT_EQUAL_UINT64(receiptBefore + 2, countForNode("reportReceipt"));
    TEST_ASSERT_EQUAL_UINT(ackBefore + 1, g_ackSendCount);

    solariConnClose(g_control);
    g_control = NULL;
    g_serverCtx.control = NULL;
    openListener();
    drainSpoolThroughServerOnce();
    TEST_ASSERT_EQUAL_size_t(0, solariReporterSpoolDepth(g_clientCtx.rep));
    TEST_ASSERT_EQUAL_UINT(ackBefore + 2, g_ackSendCount);
    TEST_ASSERT_EQUAL_UINT64(historyBefore + 2, countForNode("hostHistory"));
    TEST_ASSERT_EQUAL_UINT64(receiptBefore + 2, countForNode("reportReceipt"));

}

int main(void)
{
    const char *dbName = getenv("SOLARI_DB_NAME");
    if (!getenv("SOLARI_TEST_DB")) {
        puts("SKIP test_server_handler_live: SOLARI_TEST_DB unset");
        return 0;
    }
    if (!dbName || strcmp(dbName, "solarinet") == 0) {
        fputs("FATAL test_server_handler_live: refusing production schema solarinet\n", stderr);
        return 1;
    }
    serverConfigDefaults(&g_serverCfg);
    snprintf(g_serverCfg.dbHost, sizeof g_serverCfg.dbHost, "%s", envOr("SOLARI_DB_HOST", "127.0.0.1"));
    g_serverCfg.dbPort = (uint16_t)atoi(envOr("SOLARI_DB_PORT", "13306"));
    snprintf(g_serverCfg.dbName, sizeof g_serverCfg.dbName, "%s", dbName);
    snprintf(g_serverCfg.dbUser, sizeof g_serverCfg.dbUser, "%s", envOr("SOLARI_DB_USER", "solari"));
    snprintf(g_serverCfg.dbPass, sizeof g_serverCfg.dbPass, "%s", envOr("SOLARI_DB_PASS", ""));
    snprintf(g_serverCfg.dbSocket, sizeof g_serverCfg.dbSocket, "%s", envOr("SOLARI_DB_SOCKET", ""));
    if (g_serverCfg.dbSocket[0]) g_serverCfg.dbHost[0] = '\0';
    if (serverDbOpen(&g_serverCfg, &g_db) != SOLARI_OK) return 1;

    memset(&g_serverCtx, 0, sizeof g_serverCtx);
    g_serverCtx.db = g_db;
    g_serverCtx.nodeId = 0x534552564552ULL;
    openListener();

    clientConfigDefaults(&g_clientCfg);
    snprintf(g_clientCfg.hostFqdn, sizeof g_clientCfg.hostFqdn, "%s", HANDLER_FQDN);
    snprintf(g_clientCfg.primaryUrl, sizeof g_clientCfg.primaryUrl, "%s", HANDLER_URL);
    snprintf(g_clientCfg.reportAckUrl, sizeof g_clientCfg.reportAckUrl, "%s", HANDLER_URL);
    if (!solariTestTempPath(g_spoolPath, sizeof g_spoolPath,
                            "server-handler-spool.db")) return 1;
    if (strlen(g_spoolPath) >= sizeof g_clientCfg.spoolDb) return 1;
    strcpy(g_clientCfg.spoolDb, g_spoolPath);
    unlink(g_spoolPath);
    if (clientContextInit(&g_clientCtx, &g_clientCfg) != SOLARI_OK) return 1;
    if (clientConnect(&g_clientCtx) != SOLARI_OK) return 1;

    UNITY_BEGIN();
    RUN_TEST(test_real_control_handler_receipt_faults);
    clientContextClose(&g_clientCtx);
    unlink(g_spoolPath);
    solariConnClose(g_control);
    (void)mysql_query(serverDbConn(g_db), "DROP TRIGGER IF EXISTS " HANDLER_TRIGGER);
    {
        char sql[160];
        snprintf(sql, sizeof sql, "DELETE FROM node WHERE nodeId=%llu",
                 (unsigned long long)g_clientCtx.nodeId);
        (void)mysql_query(serverDbConn(g_db), sql);
    }
    serverDbClose(g_db);
    return UNITY_END();
}
