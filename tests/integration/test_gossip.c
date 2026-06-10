/*
 * test_gossip.c - two monitors gossiping over loopback (sec 8.2): each announces
 * itself to the other, both registries converge on a two-node fleet, and the
 * live fleet each derives includes the peer. Gated on MONITOR_WITH_REPORTING.
 */
#include "unity.h"
#include "monitor.h"

#include "solari/solariTime.h"
#include "solari/solariError.h"

#include <string.h>

#define URL_A "tcp://127.0.0.1:8813"
#define URL_B "tcp://127.0.0.1:8814"

void setUp(void) {}
void tearDown(void) {}

static void cfgWith(monitorConfig *c, const char *ownUrl, const char *peerUrl)
{
    monitorConfigDefaults(c);
    strncpy(c->gossipUrl, ownUrl, sizeof c->gossipUrl - 1);
    strncpy(c->peerUrls[0], peerUrl, sizeof c->peerUrls[0] - 1);
    c->peerCount = 1;
}

static void test_two_monitors_converge(void)
{
    monitorConfig  cfgA, cfgB;
    monitorGossip *gA = NULL, *gB = NULL;
    monitorPeers   regA, regB;
    uint64_t nodeA = 0xAAAA, nodeB = 0xBBBB, now = 1000;
    uint64_t fleet[8];
    size_t n;
    int tries;
    bool converged = false;

    cfgWith(&cfgA, URL_A, URL_B);
    cfgWith(&cfgB, URL_B, URL_A);
    monitorPeersInit(&regA, nodeA);
    monitorPeersInit(&regB, nodeB);

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, monitorGossipOpen(&cfgA, nodeA, &gA));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, monitorGossipOpen(&cfgB, nodeB, &gB));
    TEST_ASSERT_NOT_NULL(gA);
    TEST_ASSERT_NOT_NULL(gB);

    for (tries = 0; tries < 50 && !converged; tries++) {
        monitorGossipTick(gA, &regA, now);
        monitorGossipTick(gB, &regB, now);
        now += 1000;
        converged = (regA.count >= 1 && regB.count >= 1);
        if (!converged) solariSleepMs(20);
    }
    TEST_ASSERT_TRUE(regA.count >= 1);          /* A learned B */
    TEST_ASSERT_TRUE(regB.count >= 1);          /* B learned A */

    /* A's live fleet is { self, B } */
    n = monitorPeersFleet(&regA, now, 90, fleet, 8);
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_UINT64(nodeA, fleet[0]);
    TEST_ASSERT_EQUAL_UINT64(nodeB, fleet[1]);

    monitorGossipClose(gA);
    monitorGossipClose(gB);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_two_monitors_converge);
    return UNITY_END();
}
