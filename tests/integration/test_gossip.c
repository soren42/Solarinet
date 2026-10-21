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
#define URL_PLAIN "tcp://127.0.0.1:18815"
static const char *caFile, *certA, *keyA, *certB, *keyB;
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

static void test_two_monitors_converge_and_reject_forgery(void)
{
    monitorConfig cfgA, cfgB;
    monitorGossip *gA = NULL, *gB = NULL;
    monitorPeers regA, regB;
    uint64_t nodeA, nodeB, now = 1000, fleet[8];
    size_t n;
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
    nodeA = monitorNodeId(&cfgA);
    nodeB = monitorNodeId(&cfgB);
    monitorPeersInit(&regA, nodeA);
    monitorPeersInit(&regB, nodeB);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, monitorGossipOpen(&cfgA, nodeA, &gA));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, monitorGossipOpen(&cfgB, nodeB, &gB));
    TEST_ASSERT_NOT_NULL(gA);
    TEST_ASSERT_NOT_NULL(gB);
    for (tries = 0; tries < 60 && !converged; tries++) {
        monitorGossipTick(gA, &regA, now);
        monitorGossipTick(gB, &regB, now);
        now += 1000;
        converged = regA.count >= 1 && regB.count >= 1;
        if (!converged) solariSleepMs(20);
    }
    TEST_ASSERT_TRUE(regA.count >= 1);
    TEST_ASSERT_TRUE(regB.count >= 1);
    n = monitorPeersFleet(&regA, now, 90, fleet, 8);
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_UINT64(nodeA, fleet[0]);
    TEST_ASSERT_EQUAL_UINT64(nodeB, fleet[1]);

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
    if (argc != 10 && argc != 11) return 2;
    caFile = argv[1]; certA = argv[2]; keyA = argv[3]; certB = argv[4]; keyB = argv[5];
    certRevoked = argv[6]; keyRevoked = argv[7];
    certReplacement = argv[8]; keyReplacement = argv[9];
    if (argc == 11 && strcmp(argv[10], "expect-invalid-crl") == 0) {
        UNITY_BEGIN();
        RUN_TEST(test_invalid_crl_fails_closed);
        return UNITY_END();
    }
    if (argc == 11 && strcmp(argv[10], "wrong-issuer") == 0)
        checkRevokedLeaf = false;
    else if (argc == 11)
        return 2;
    UNITY_BEGIN();
    RUN_TEST(test_two_monitors_converge_and_reject_forgery);
    RUN_TEST(test_plain_pipe_has_no_verified_peer_identity);
    return UNITY_END();
}
