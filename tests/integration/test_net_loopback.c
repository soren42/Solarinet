/*
 * test_net_loopback.c - transport smoke test (section 14 integration).
 *
 * Stands up a PULL listener and a PUSH dialer on TCP loopback, sends one real
 * SCP frame, and verifies it arrives byte-identical. This is the first test that
 * actually exercises solariNet, so it is gated behind SOLARI_WITH_IO and only
 * built/run where nng is available (e.g. on xenon after bootstrap).
 *
 * No TLS here: this validates the nng wrapper, framing-over-the-wire, and the
 * recv buffer contract. A TLS round-trip belongs in a fuller integration suite
 * once node certificates exist (section 15.1 enrollment).
 */
#include "unity.h"
#include "solari/solariNet.h"
#include "solari/solariFrame.h"
#include "solari/solariTime.h"
#include "solari/solariError.h"

#include <string.h>

#define LOOPBACK_URL "tcp://127.0.0.1:8799"

void setUp(void) {}
void tearDown(void) {}

static size_t buildHello(uint8_t *out, size_t cap)
{
    solariFrameHeader h;
    size_t len = 0;
    memset(&h, 0, sizeof h);
    h.magic[0] = SCP_MAGIC_0;
    h.magic[1] = SCP_MAGIC_1;
    h.protoVersion = SCP_PROTO_VERSION;
    h.msgType = SCP_MSG_HELLO;
    h.sourceNodeId = 0xABCDEF0123456789ull;
    h.seqNo = 1;
    solariFrameBuild(&h, NULL, 0, out, cap, &len);
    return len;
}

static void test_push_pull_loopback(void)
{
    solariConnOpts pullOpts, pushOpts;
    solariConn *pull = NULL, *push = NULL;
    uint8_t frame[128];
    size_t flen;
    const uint8_t *rx = NULL;
    size_t rlen = 0;
    solariStatus st = ERR_CONN_RETRY;
    int tries;

    memset(&pullOpts, 0, sizeof pullOpts);
    pullOpts.url = LOOPBACK_URL;
    pullOpts.pattern = SOLARI_PATTERN_PULL;
    pullOpts.recvTimeoutMs = 3000;

    memset(&pushOpts, 0, sizeof pushOpts);
    pushOpts.url = LOOPBACK_URL;
    pushOpts.pattern = SOLARI_PATTERN_PUSH;
    pushOpts.sendTimeoutMs = 3000;

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariConnListen(&pullOpts, &pull));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariConnOpen(&pushOpts, &push));

    flen = buildHello(frame, sizeof frame);
    TEST_ASSERT_TRUE(flen > 0);

    /* The PUSH pipe may take a moment to establish; retry briefly. */
    for (tries = 0; tries < 40 && st != SOLARI_OK; tries++) {
        st = solariConnSend(push, frame, flen);
        if (st == ERR_CONN_RETRY) solariSleepMs(50);
    }
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, st);

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariConnRecv(pull, &rx, &rlen));
    TEST_ASSERT_EQUAL_size_t(flen, rlen);
    TEST_ASSERT_EQUAL_MEMORY(frame, rx, flen);

    solariConnClose(push);
    solariConnClose(pull);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_push_pull_loopback);
    return UNITY_END();
}
