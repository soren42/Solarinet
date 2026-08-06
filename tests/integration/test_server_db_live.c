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
 *   SOLARI_DB_PASS  (default empty)
 * The target schema must already be applied (migrations 001 + 002).
 */
#include "unity.h"
#include "solari/solariCommon.h"
#include "solari/solariError.h"
#include "solari/solariMsg.h"
#include "server.h"

#include <mariadb/mysql.h>

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

/* ---- connection is alive ---- */
static void test_open_ping(void)
{
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbPing(g_db));
}

/* ---- node upsert / touch / state (incl. the 'retired' enum from 002) ---- */
static void test_node_lifecycle(void)
{
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbUpsertNode(g_db, ITEST_NODE, ROLE_CLIENT,
                           "itest.host", "itest-cn", "linux", "x86_64", 7));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbTouchNode(g_db, ITEST_NODE, 1700000000000ULL));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbSetNodeState(g_db, ITEST_NODE, "retired"));
    /* second upsert exercises the ON DUPLICATE KEY path with a bumped epoch */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbUpsertNode(g_db, ITEST_NODE, ROLE_CLIENT,
                           "itest.host", "itest-cn", "linux", "x86_64", 8));
}

/* ---- the heavy writer: hostCurrent upsert + hostHistory append, one txn ---- */
static void test_client_report(void)
{
    serverContext ctx;
    solariClientReport rep;
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

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbWriteClientReport(&ctx, ITEST_NODE, &rep));
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

/* ---- enrollment state machine: create -> get -> decide ---- */
static void test_enrollment(void)
{
    serverEnrollment e, got;
    uint64_t enrId = 0;

    memset(&e, 0, sizeof e);
    snprintf(e.host, sizeof e.host, "enr.itest");
    snprintf(e.ip,   sizeof e.ip,   "203.0.113.8");
    e.role   = ROLE_MONITOR;
    e.status = ENROLL_PENDING;

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbEnrollCreate(g_db, &e, 1700000000000ULL, &enrId));
    TEST_ASSERT_TRUE(enrId > 0);

    memset(&got, 0, sizeof got);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbEnrollGet(g_db, enrId, &got));
    TEST_ASSERT_EQUAL_INT(ROLE_MONITOR, got.role);
    TEST_ASSERT_EQUAL_STRING("203.0.113.8", got.ip);

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbEnrollDecide(g_db, enrId, ENROLL_APPROVED, "itester",
                             1700000002000ULL));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbEnrollGet(g_db, enrId, &got));
    TEST_ASSERT_EQUAL_INT(ENROLL_APPROVED, got.status);
}

/* ---- lease claim/read: the conditional-UPDATE election ---- */
static void test_lease(void)
{
    bool won = false;
    uint64_t epoch = 0, holder = 0, rEpoch = 0;

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        serverDbLeaseClaim(g_db, ITEST_NODE, 15, &won, &epoch));
    TEST_ASSERT_TRUE(won);

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, serverDbLeaseRead(g_db, &holder, &rEpoch));
    TEST_ASSERT_EQUAL_UINT64(ITEST_NODE, holder);
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
    g_cfg.dbSocket[0] = '\0';   /* force TCP host/port */

    if (serverDbOpen(&g_cfg, &g_db) != SOLARI_OK) {
        printf("FATAL test_server_db_live: serverDbOpen failed for %s:%u/%s\n",
               g_cfg.dbHost, (unsigned)g_cfg.dbPort, g_cfg.dbName);
        return 1;
    }

    UNITY_BEGIN();
    RUN_TEST(test_open_ping);
    RUN_TEST(test_node_lifecycle);
    RUN_TEST(test_client_report);
    RUN_TEST(test_alert_event);
    RUN_TEST(test_lifecycle_transition_rolls_back_cascade);
    RUN_TEST(test_discovered);
    RUN_TEST(test_enrollment);
    RUN_TEST(test_lease);
    serverDbClose(g_db);
    return UNITY_END();
}
