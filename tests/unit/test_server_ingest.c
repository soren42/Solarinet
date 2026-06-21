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

void setUp(void) {}
void tearDown(void) {}

/* ---- CN -> role ---- */

static void test_role_from_cn(void)
{
    TEST_ASSERT_EQUAL_INT(INGEST_ROLE_CLIENT,  serverIngestRoleFromCn("client.akoria.net"));
    TEST_ASSERT_EQUAL_INT(INGEST_ROLE_MONITOR, serverIngestRoleFromCn("monitor.akoria.net"));
    TEST_ASSERT_EQUAL_INT(INGEST_ROLE_SERVER,  serverIngestRoleFromCn("server.akoria.net"));
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_role_from_cn);
    RUN_TEST(test_matrix_client);
    RUN_TEST(test_matrix_monitor_is_superset);
    RUN_TEST(test_matrix_server_and_unknown);
    RUN_TEST(test_dedup_basic);
    RUN_TEST(test_dedup_window_rolls_over);
    return UNITY_END();
}
