/* Mutual-TLS gossip convergence and authenticated-identity rejection. */
#include "unity.h"
#include "monitor.h"
#include "solari/solariTime.h"
#include "solari/solariError.h"
#include "solari/solariNet.h"
#include "solari/solariFrame.h"
#include <string.h>

#define URL_A "tls+tcp://127.0.0.1:18813"
#define URL_B "tls+tcp://127.0.0.1:18814"
#define URL_C "tls+tcp://127.0.0.1:18817"
#define URL_PLAIN "tcp://127.0.0.1:18815"
static const char *caFile, *certA, *keyA, *certB, *keyB, *certC, *keyC;
static const char *certRevoked, *keyRevoked, *certReplacement, *keyReplacement;
static bool checkRevokedLeaf = true;
void setUp(void) {}
void tearDown(void) {}

static void cfgWith(monitorConfig *c, const char *ownUrl, const char *peerUrl,
                    const char *host, const char *cert, const char *key)
{
    monitorConfigDefaults(c);
    strncpy(c->gossipUrl, ownUrl, sizeof c->gossipUrl - 1);
    strncpy(c->peerUrls[0], peerUrl, sizeof c->peerUrls[0] - 1);
    c->peerCount = 1;
    c->useTls = true;
    strncpy(c->hostFqdn, host, sizeof c->hostFqdn - 1);
    strncpy(c->caFile, caFile, sizeof c->caFile - 1);
    strncpy(c->certFile, cert, sizeof c->certFile - 1);
    strncpy(c->keyFile, key, sizeof c->keyFile - 1);
}

static void cfgAddPeer(monitorConfig *c, const char *peerUrl)
{
    size_t index = c->peerCount;
    if (index >= MONITOR_MAX_FLEET) return;
    strncpy(c->peerUrls[index], peerUrl, sizeof c->peerUrls[index] - 1);
    c->peerCount++;
}

static bool sameFleet(const uint64_t *a, size_t aLen,
                      const uint64_t *b, size_t bLen)
{
    size_t i, j;
    if (aLen != bLen) return false;
    for (i = 0; i < aLen; i++) {
        for (j = 0; j < bLen && b[j] != a[i]; j++) {}
        if (j == bLen) return false;
    }
    return true;
}

static void test_three_monitors_converge_churn_and_reject_forgery(void)
{
    monitorConfig cfgA, cfgB, cfgC;
    monitorGossip *gA = NULL, *gB = NULL, *gC = NULL;
    monitorPeers regA, regB, regC;
    uint64_t nodeA, nodeB, nodeC, now = 1000;
    uint64_t fleetA[8], fleetB[8], fleetC[8];
    size_t nA, nB, nC;
    int tries;
    bool converged = false;
    solariConn *forger = NULL;
    solariConn *revoked = NULL;
    solariConn *replacement = NULL;
    monitorConfig cfgRevoked;
    solariConnOpts opts;
    uint8_t frame[64];
    size_t frameLen = 0;
    solariFrameHeader h;

    cfgWith(&cfgA, URL_A, URL_B, "a.gossip.test", certA, keyA);
    cfgWith(&cfgB, URL_B, URL_A, "b.gossip.test", certB, keyB);
    cfgWith(&cfgC, URL_C, URL_A, "c.gossip.test", certC, keyC);
    cfgA.peerTtlSec = cfgB.peerTtlSec = cfgC.peerTtlSec = 3;
    cfgAddPeer(&cfgA, URL_C);
    cfgAddPeer(&cfgB, URL_C);
    cfgAddPeer(&cfgC, URL_B);
    nodeA = monitorNodeId(&cfgA);
    nodeB = monitorNodeId(&cfgB);
    nodeC = monitorNodeId(&cfgC);
    monitorPeersInit(&regA, nodeA);
    monitorPeersInit(&regB, nodeB);
    monitorPeersInit(&regC, nodeC);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, monitorGossipOpen(&cfgA, nodeA, &gA));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, monitorGossipOpen(&cfgB, nodeB, &gB));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, monitorGossipOpen(&cfgC, nodeC, &gC));
    TEST_ASSERT_NOT_NULL(gA);
    TEST_ASSERT_NOT_NULL(gB);
    TEST_ASSERT_NOT_NULL(gC);
    for (tries = 0; tries < 100 && !converged; tries++) {
        monitorGossipTick(gA, &regA, now);
        monitorGossipTick(gB, &regB, now);
        monitorGossipTick(gC, &regC, now);
        now += 1000;
        converged = regA.count == 2 && regB.count == 2 && regC.count == 2;
        if (!converged) solariSleepMs(20);
    }
    TEST_ASSERT_TRUE(converged);
    nA = monitorPeersFleet(&regA, now, cfgA.peerTtlSec, fleetA, 8);
    nB = monitorPeersFleet(&regB, now, cfgB.peerTtlSec, fleetB, 8);
    nC = monitorPeersFleet(&regC, now, cfgC.peerTtlSec, fleetC, 8);
    TEST_ASSERT_EQUAL_size_t(3, nA);
    TEST_ASSERT_EQUAL_size_t(3, nB);
    TEST_ASSERT_EQUAL_size_t(3, nC);
    TEST_ASSERT_TRUE(sameFleet(fleetA, nA, fleetB, nB));
    TEST_ASSERT_TRUE(sameFleet(fleetA, nA, fleetC, nC));
    {
        int ti;
        for (ti = 0; ti < 120; ti++) {
            char targetId[40];
            int owners = 0;
            snprintf(targetId, sizeof targetId, "tcp:mesh-target-%d:443", ti);
            owners += monitorOwnsTarget(nodeA, fleetA, nA, targetId, 2);
            owners += monitorOwnsTarget(nodeB, fleetB, nB, targetId, 2);
            owners += monitorOwnsTarget(nodeC, fleetC, nC, targetId, 2);
            TEST_ASSERT_EQUAL_INT(2, owners);
        }
    }

    /* A departed monitor disappears from both survivor views after the peer
     * TTL, and the two-node membership produces exactly two owners again. */
    monitorGossipClose(gC);
    gC = NULL;
    converged = false;
    for (tries = 0; tries <= (int)cfgA.peerTtlSec + 2 && !converged; tries++) {
        monitorGossipTick(gA, &regA, now);
        monitorGossipTick(gB, &regB, now);
        monitorPeersPrune(&regA, now, cfgA.peerTtlSec);
        monitorPeersPrune(&regB, now, cfgB.peerTtlSec);
        now += 1000;
        nA = monitorPeersFleet(&regA, now, cfgA.peerTtlSec, fleetA, 8);
        nB = monitorPeersFleet(&regB, now, cfgB.peerTtlSec, fleetB, 8);
        converged = nA == 2 && nB == 2 && sameFleet(fleetA, nA, fleetB, nB);
        if (!converged) solariSleepMs(2);
    }
    TEST_ASSERT_TRUE(converged);
    TEST_ASSERT_TRUE(!monitorOwnsTarget(nodeC, fleetA, nA, "tcp:mesh-target-0:443", 2));
    TEST_ASSERT_TRUE(monitorOwnsTarget(nodeA, fleetA, nA, "tcp:mesh-target-0:443", 2));
    TEST_ASSERT_TRUE(monitorOwnsTarget(nodeB, fleetB, nB, "tcp:mesh-target-0:443", 2));

    /* A valid A certificate cannot claim an unrelated node ID. */
    memset(&opts, 0, sizeof opts);
    opts.url = URL_B;
    opts.pattern = SOLARI_PATTERN_PUSH;
    opts.useTls = true;
    opts.caFile = caFile;
    opts.certFile = certA;
    opts.keyFile = keyA;
    opts.sendTimeoutMs = 500;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariConnOpen(&opts, &forger));
    memset(&h, 0, sizeof h);
    h.magic[0] = SCP_MAGIC_0;
    h.magic[1] = SCP_MAGIC_1;
    h.protoVersion = SCP_PROTO_VERSION;
    h.msgType = SCP_MSG_PEER_ALIVE;
    h.sourceNodeId = UINT64_C(0xdeadbeef);
    h.sendTimeUnixMs = now;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
                          solariFrameBuild(&h, NULL, 0, frame, sizeof frame, &frameLen));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariConnSend(forger, frame, frameLen));
    solariSleepMs(30);
    monitorGossipTick(gB, &regB, now + 1);
    for (tries = 0; tries < (int)regB.count; tries++)
        TEST_ASSERT_NOT_EQUAL_UINT64(UINT64_C(0xdeadbeef), regB.peers[tries].nodeId);
    solariConnClose(forger);

    /* A validly signed but revoked leaf must fail the listener's TLS check. */
    if (checkRevokedLeaf) {
    cfgWith(&cfgRevoked, "", URL_B, "revoked.gossip.test", certRevoked, keyRevoked);
    memset(&opts, 0, sizeof opts);
    opts.url = URL_B;
    opts.pattern = SOLARI_PATTERN_PUSH;
    opts.useTls = true;
    opts.caFile = caFile;
    opts.certFile = certRevoked;
    opts.keyFile = keyRevoked;
    opts.sendTimeoutMs = 500;
    TEST_ASSERT_EQUAL_INT(ERR_TLS, solariConnOpen(&opts, &revoked));
    TEST_ASSERT_NULL(revoked);
    h.sourceNodeId = monitorNodeId(&cfgRevoked);
    for (tries = 0; tries < (int)regB.count; tries++)
        TEST_ASSERT_NOT_EQUAL_UINT64(h.sourceNodeId, regB.peers[tries].nodeId);
    solariConnClose(revoked);

    /* The replacement leaf has the same logical identity but a new key and
     * serial. Revoking its predecessor must not invalidate this credential. */
    opts.certFile = certReplacement;
    opts.keyFile = keyReplacement;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariConnOpen(&opts, &replacement));
    TEST_ASSERT_NOT_NULL(replacement);
    h.magic[0] = SCP_MAGIC_0;
    h.magic[1] = SCP_MAGIC_1;
    h.protoVersion = SCP_PROTO_VERSION;
    h.msgType = SCP_MSG_PEER_ALIVE;
    h.sourceNodeId = monitorNodeId(&cfgRevoked);
    h.sendTimeUnixMs = now + 2;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
                          solariFrameBuild(&h, NULL, 0, frame, sizeof frame, &frameLen));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariConnSend(replacement, frame, frameLen));
    {
        bool replacementSeen = false;
        /* PUSH send completion only queues the frame; allow the TLS listener
         * to receive and verify it instead of assuming a fixed 30 ms delay. */
        for (tries = 0; tries < 50 && !replacementSeen; tries++) {
            size_t peer;
            monitorGossipTick(gB, &regB, now + 3);
            for (peer = 0; peer < (size_t)regB.count; peer++)
                if (regB.peers[peer].nodeId == h.sourceNodeId)
                    replacementSeen = true;
            if (!replacementSeen) solariSleepMs(10);
        }
        TEST_ASSERT_TRUE(replacementSeen);
    }
    solariConnClose(replacement);
    }
    monitorGossipClose(gA);
    monitorGossipClose(gB);
    monitorGossipClose(gC);
}

/* A present but expired or malformed CRL must not be treated as a clean trust
 * bundle. Accept rejection while either endpoint parses the trust file, but do
 * not accept a successful TLS connection with the valid leaf. */
static void test_invalid_crl_fails_closed(void)
{
    solariConn *listener = NULL, *client = NULL;
    solariConnOpts opts;
    int rc;
    const uint8_t payload[] = {0x53, 0x4e};

    memset(&opts, 0, sizeof opts);
    opts.url = "tls+tcp://127.0.0.1:18816";
    opts.pattern = SOLARI_PATTERN_PULL;
    opts.recvTimeoutMs = 50;
    opts.useTls = true;
    opts.caFile = caFile;
    opts.certFile = certB;
    opts.keyFile = keyB;
    rc = solariConnListen(&opts, &listener);
    if (rc != SOLARI_OK) {
        TEST_ASSERT_EQUAL_INT(ERR_TLS, rc);
        return;
    }

    opts.pattern = SOLARI_PATTERN_PUSH;
    opts.certFile = certA;
    opts.keyFile = keyA;
    opts.sendTimeoutMs = 500;
    rc = solariConnOpen(&opts, &client);
    if (rc == SOLARI_OK) {
        rc = solariConnSend(client, payload, sizeof payload);
        TEST_ASSERT_EQUAL_INT(ERR_TLS, rc);
    } else {
        TEST_ASSERT_EQUAL_INT(ERR_TLS, rc);
    }
    solariConnClose(client);
    solariConnClose(listener);
}

static void test_plain_pipe_has_no_verified_peer_identity(void)
{
    solariConnOpts opts;
    solariConn *listener = NULL, *sender = NULL;
    const uint8_t frame[] = {0x53, 0x4e};
    const uint8_t *received = NULL;
    size_t receivedLen = 0;
    char peerCn[128];
    int tries;

    memset(&opts, 0, sizeof opts);
    opts.url = URL_PLAIN;
    opts.pattern = SOLARI_PATTERN_PULL;
    opts.recvTimeoutMs = 20;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariConnListen(&opts, &listener));
    opts.pattern = SOLARI_PATTERN_PUSH;
    opts.sendTimeoutMs = 500;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariConnOpen(&opts, &sender));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
                          solariConnSend(sender, frame, sizeof frame));

    for (tries = 0; tries < 50; tries++) {
        if (solariConnRecv(listener, &received, &receivedLen) == SOLARI_OK) break;
        solariSleepMs(10);
    }
    TEST_ASSERT_TRUE(tries < 50);
    TEST_ASSERT_EQUAL_size_t(sizeof frame, receivedLen);
    TEST_ASSERT_EQUAL_INT(ERR_TLS,
                          solariConnPeerCn(listener, peerCn, sizeof peerCn));
    solariConnClose(sender);
    solariConnClose(listener);
}

int main(int argc, char **argv)
{
    if (argc != 12 && argc != 13) return 2;
    caFile = argv[1]; certA = argv[2]; keyA = argv[3]; certB = argv[4]; keyB = argv[5];
    certC = argv[6]; keyC = argv[7];
    certRevoked = argv[8]; keyRevoked = argv[9];
    certReplacement = argv[10]; keyReplacement = argv[11];
    if (argc == 13 && strcmp(argv[12], "expect-invalid-crl") == 0) {
        UNITY_BEGIN();
        RUN_TEST(test_invalid_crl_fails_closed);
        return UNITY_END();
    }
    if (argc == 13 && strcmp(argv[12], "wrong-issuer") == 0)
        checkRevokedLeaf = false;
    else if (argc == 13)
        return 2;
    UNITY_BEGIN();
    RUN_TEST(test_three_monitors_converge_churn_and_reject_forgery);
    RUN_TEST(test_plain_pipe_has_no_verified_peer_identity);
    return UNITY_END();
}
