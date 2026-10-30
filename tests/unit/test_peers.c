/*
 * test_peers.c - the peer registry and HRW schedule (sec 8.2): liveness with
 * TTL pruning, the live-fleet view, and target ownership splitting across a
 * multi-node fleet.
 */
#include "unity.h"
#include "monitor.h"
#include "solari/solariCrypto.h"

#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_registry_heard_prune(void)
{
    monitorPeers p;
    uint64_t fleet[8];
    size_t n;

    monitorPeersInit(&p, 1000);
    monitorPeersHeard(&p, 1000, 0);          /* self is never tracked */
    TEST_ASSERT_EQUAL_size_t(0, p.count);

    monitorPeersHeard(&p, 2000, 0);
    monitorPeersHeard(&p, 3000, 0);
    monitorPeersHeard(&p, 2000, 0);          /* dup refresh, not a new entry */
    TEST_ASSERT_EQUAL_size_t(2, p.count);

    n = monitorPeersFleet(&p, 0, 90, fleet, 8);
    TEST_ASSERT_EQUAL_size_t(3, n);          /* self + 2 peers */
    TEST_ASSERT_EQUAL_UINT64(1000, fleet[0]);/* self first */

    /* at t=100s, refresh only 2000; prune (ttl 90s) drops the stale 3000 */
    monitorPeersHeard(&p, 2000, 100000);
    monitorPeersPrune(&p, 100000, 90);
    TEST_ASSERT_EQUAL_size_t(1, p.count);
    n = monitorPeersFleet(&p, 100000, 90, fleet, 8);
    TEST_ASSERT_EQUAL_size_t(2, n);          /* self + 2000 */
}

static void test_peer_certificate_binds_monitor_node_id(void)
{
    monitorConfig cfg;
    uint64_t id;
    monitorConfigDefaults(&cfg);
    snprintf(cfg.hostFqdn, sizeof cfg.hostFqdn, "outpost.akoria.net");
    id = monitorNodeId(&cfg);
    TEST_ASSERT_EQUAL_UINT64(
        solariNodeIdFromFqdnRole("outpost.akoria.net", ROLE_MONITOR), id);
    TEST_ASSERT_TRUE(monitorPeerIdentityMatchesCn(id,
        "monitor.outpost.akoria.net"));
    TEST_ASSERT_FALSE(monitorPeerIdentityMatchesCn(id,
        "outpost.akoria.net")); /* gossip has no enrollment-role lookup */
    TEST_ASSERT_FALSE(monitorPeerIdentityMatchesCn(id + 1,
        "monitor.outpost.akoria.net"));
    TEST_ASSERT_FALSE(monitorPeerIdentityMatchesCn(id,
        "client.outpost.akoria.net"));
    TEST_ASSERT_FALSE(monitorPeerIdentityMatchesCn(id, "monitor."));
    TEST_ASSERT_FALSE(monitorPeerIdentityMatchesCn(id, "other.akoria.net"));
    TEST_ASSERT_FALSE(monitorPeerIdentityMatchesCn(0,
        "monitor.outpost.akoria.net"));
    TEST_ASSERT_FALSE(monitorPeerIdentityMatchesCn(id, NULL));
}

static void test_schedule_partition(void)
{
    monitorConfig cfg;
    uint8_t owned[64];
    uint64_t self = 111, fleet1[1] = { 111 }, fleet3[3] = { 111, 222, 333 };
    size_t no, a, b, cc;
    int i;

    monitorConfigDefaults(&cfg);
    cfg.replFactor = 1;                      /* each target owned by exactly 1 */
    cfg.targetCount = 10;
    for (i = 0; i < 10; i++)
        snprintf(cfg.targets[i].targetId, sizeof cfg.targets[i].targetId, "tcp:h%d:443", i);

    /* standalone: owns all */
    no = monitorOwnedTargets(&cfg, self, fleet1, 1, owned, 64);
    TEST_ASSERT_EQUAL_size_t(10, no);

    /* in a 3-node fleet at k=1, owns a strict subset... */
    no = monitorOwnedTargets(&cfg, self, fleet3, 3, owned, 64);
    TEST_ASSERT_TRUE(no < 10);

    /* ...and the three owned sets exactly partition the 10 targets */
    a  = monitorOwnedTargets(&cfg, 111, fleet3, 3, owned, 64);
    b  = monitorOwnedTargets(&cfg, 222, fleet3, 3, owned, 64);
    cc = monitorOwnedTargets(&cfg, 333, fleet3, 3, owned, 64);
    TEST_ASSERT_EQUAL_size_t(10, a + b + cc);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_registry_heard_prune);
    RUN_TEST(test_peer_certificate_binds_monitor_node_id);
    RUN_TEST(test_schedule_partition);
    return UNITY_END();
}
