/*
 * test_control_e2e.c - config-push convergence over real sockets (section 14).
 *
 * Stands up the server side of the control plane on TCP loopback (a PUB
 * listener for directives + a PULL listener for the ingest channel), runs the
 * REAL client control path against it (clientControlOpen SUB dial +
 * clientControlPoll dispatch + reporter push), and asserts the full loop:
 *
 *   publish CTRL_SET_CONFIG{epoch, blob, targetNode}  (as serverProvision does)
 *     -> client receives on SUB, applies the blob, persists applied state
 *     -> client pushes SCP_MSG_CONTROL_RESULT with the directive's seqNo as
 *        correlationId and the applied epoch
 *     -> "server" ingest parses the result exactly as serverControlOnResult
 *        would and sees convergence (applied epoch == target epoch)
 *
 * plus restart persistence: a fresh config + state reload keeps the pushed
 * values. No TLS (the wrapper's TLS path is covered elsewhere) and no DB.
 */
#include "unity.h"

#include "clientControl.h"

#include "solari/solariFrame.h"
#include "solari/solariMsg.h"
#include "solari/solariNet.h"
#include "solari/solariTime.h"
#include "solari/solariTlv.h"

#include <stdio.h>
#include <string.h>

#define PUB_URL  "tcp://127.0.0.1:8811"
#define PULL_URL "tcp://127.0.0.1:8812"
#define DIRECTIVE_SEQ 7701u
#define TARGET_EPOCH  5ull
#define STATE_FILE "/tmp/solari_test_ctrl_state"

void setUp(void) {}
void tearDown(void) {}

/* Build a complete CONTROL frame the way the active master does: shared
 * solariMsgBuildControl payload + a header whose correlationId echoes its own
 * seqNo (serverControlInitHeader's contract). */
static size_t buildControlFrame(uint64_t targetNode, uint64_t epoch,
                                const char *blob, uint8_t *out, size_t cap)
{
    solariControl     c;
    solariFrameHeader h;
    uint8_t  tlv[1024];
    size_t   tlvLen = 0, frameLen = 0;
    uint16_t tlvCount = 0;

    memset(&c, 0, sizeof c);
    c.verb        = CTRL_SET_CONFIG;
    c.targetEpoch = epoch;
    c.targetNode  = targetNode;
    c.payload     = (const uint8_t *)blob;
    c.payloadLen  = (uint16_t)strlen(blob);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariMsgBuildControl(&c, tlv, sizeof tlv, &tlvLen, &tlvCount));

    memset(&h, 0, sizeof h);
    h.magic[0] = SCP_MAGIC_0;
    h.magic[1] = SCP_MAGIC_1;
    h.protoVersion   = SCP_PROTO_VERSION;
    h.msgType        = SCP_MSG_CONTROL;
    h.flags          = SCP_FLAG_ACK_REQ;
    h.tlvCount       = tlvCount;
    h.sourceNodeId   = 0x5E12345678ULL;
    h.sendTimeUnixMs = solariNowUnixMs();
    h.seqNo          = DIRECTIVE_SEQ;
    h.correlationId  = DIRECTIVE_SEQ;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariFrameBuild(&h, tlv, tlvLen, out, cap, &frameLen));
    return frameLen;
}

static void test_config_push_end_to_end(void)
{
    solariConnOpts pubOpts, pullOpts;
    solariConn *pub = NULL, *pull = NULL;
    clientConfig cfg;
    clientContext ctx;
    clientControlState cst;
    clientControlIo cio;
    uint8_t dirFrame[2048];
    size_t  dirLen;
    uint64_t self;
    int tries;
    bool converged = false;
    const char *blob = "{\"sampleIntervalSec\":9,\"processes\":[\"sshd\"]}";

    remove(STATE_FILE);

    /* --- "server": PUB (directives out) + PULL (ingest in) --- */
    memset(&pubOpts, 0, sizeof pubOpts);
    pubOpts.url = PUB_URL;
    pubOpts.pattern = SOLARI_PATTERN_PUB;
    pubOpts.sendTimeoutMs = 1000;
    memset(&pullOpts, 0, sizeof pullOpts);
    pullOpts.url = PULL_URL;
    pullOpts.pattern = SOLARI_PATTERN_PULL;
    pullOpts.recvTimeoutMs = 300;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariConnListen(&pubOpts, &pub));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariConnListen(&pullOpts, &pull));

    /* --- agent: reporter to the ingest sink + SUB to the pub channel --- */
    clientConfigDefaults(&cfg);
    snprintf(cfg.hostFqdn, sizeof cfg.hostFqdn, "e2e.test");
    snprintf(cfg.primaryUrl, sizeof cfg.primaryUrl, "%s", PULL_URL);
    snprintf(cfg.subUrl, sizeof cfg.subUrl, "%s", PUB_URL);
    snprintf(cfg.ctrlStateFile, sizeof cfg.ctrlStateFile, "%s", STATE_FILE);

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientContextInit(&ctx, &cfg));
    (void)clientConnect(&ctx);
    (void)clientControlStateLoad(&cst, &cfg);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientControlOpen(&cfg, &cio));
    self = ctx.nodeId;

    dirLen = buildControlFrame(self, TARGET_EPOCH, blob,
                               dirFrame, sizeof dirFrame);

    /* --- push the directive until the client reports it applied. The SUB
     * join is asynchronous, so early publishes can be lost; the re-publish
     * mirrors the server's reconverge behavior. --- */
    for (tries = 0; tries < 40 && cst.appliedEpoch < TARGET_EPOCH; tries++) {
        (void)solariConnSend(pub, dirFrame, dirLen);
        clientControlPoll(&cio, &ctx, &cfg, &cst, 150);
    }
    TEST_ASSERT_EQUAL_UINT64(TARGET_EPOCH, cst.appliedEpoch);
    TEST_ASSERT_EQUAL_UINT32(9, cfg.sampleIntervalSec);   /* hot-applied */
    TEST_ASSERT_EQUAL_UINT8(1, cfg.procCount);
    TEST_ASSERT_EQUAL_STRING("sshd", cfg.procs[0]);

    /* --- ingest side: the CONTROL_RESULT arrives with the correlation echo
     * and the applied epoch (what serverControlOnResult folds into
     * serverDbSetNodeApplied -> convergence clears). --- */
    for (tries = 0; tries < 40 && !converged; tries++) {
        const uint8_t *rx = NULL, *payload = NULL;
        size_t rlen = 0, plen = 0;
        solariFrameHeader hdr;

        if (solariConnRecv(pull, &rx, &rlen) != SOLARI_OK) {
            /* client may still be dialing/replying; nudge it */
            clientControlPoll(&cio, &ctx, &cfg, &cst, 50);
            continue;
        }
        if (solariFrameParse(rx, rlen, &hdr, &payload, &plen, NULL) != SOLARI_OK)
            continue;
        if (hdr.msgType != SCP_MSG_CONTROL_RESULT) continue;

        TEST_ASSERT_EQUAL_UINT32(DIRECTIVE_SEQ, hdr.correlationId);
        TEST_ASSERT_EQUAL_UINT64(self, hdr.sourceNodeId);
        {
            solariTlvReader r;
            uint16_t type, vlen, err = 0xFFFF;
            const uint8_t *val;
            uint64_t applied = 0;
            uint8_t verb = 0;
            solariTlvReaderInit(&r, payload, plen);
            while (solariTlvNext(&r, &type, &val, &vlen) == SOLARI_OK) {
                if (type == TLV_CTRL_VERB)         solariTlvReadU8(val, vlen, &verb);
                if (type == TLV_ERROR_CODE)        solariTlvReadU16(val, vlen, &err);
                if (type == TLV_CTRL_TARGET_EPOCH) solariTlvReadU64(val, vlen, &applied);
            }
            TEST_ASSERT_EQUAL_UINT8(CTRL_SET_CONFIG, verb);
            TEST_ASSERT_EQUAL_UINT16(0, err);
            TEST_ASSERT_EQUAL_UINT64(TARGET_EPOCH, applied);
        }
        converged = true;
    }
    TEST_ASSERT_TRUE_MESSAGE(converged, "no CONTROL_RESULT reached ingest");

    /* --- restart persistence: a fresh process (new cfg + state) keeps the
     * pushed config and epoch. --- */
    {
        clientConfig cfg2;
        clientControlState cst2;
        clientConfigDefaults(&cfg2);
        snprintf(cfg2.ctrlStateFile, sizeof cfg2.ctrlStateFile, "%s", STATE_FILE);
        TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientControlStateLoad(&cst2, &cfg2));
        TEST_ASSERT_EQUAL_UINT64(TARGET_EPOCH, cst2.appliedEpoch);
        TEST_ASSERT_EQUAL_UINT32(9, cfg2.sampleIntervalSec);
        TEST_ASSERT_EQUAL_UINT8(1, cfg2.procCount);
        TEST_ASSERT_EQUAL_STRING("sshd", cfg2.procs[0]);
    }

    clientControlClose(&cio);
    clientContextClose(&ctx);
    solariConnClose(pub);
    solariConnClose(pull);
    remove(STATE_FILE);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_config_push_end_to_end);
    return UNITY_END();
}
