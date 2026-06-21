/*
 * test_probe.c - the TCP probe engine (sec 8.3) against loopback: an open port
 * reads OK with zero loss; a closed port is refused; an unrouted host is not
 * reachable; a bogus name is a DNS failure. (ICMP/UDP need privilege/network
 * and are exercised manually, not asserted here.)
 */
#include "unity.h"
#include "monitor.h"
#include "probeNet.h"
#include "solari/solariError.h"
#include "solari/solariMsg.h"
#include "solari/solariTlv.h"

#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

void setUp(void) {}
void tearDown(void) {}

static int startTcpListener(uint16_t port)
{
    struct sockaddr_in a;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    if (listen(fd, 16) != 0) { close(fd); return -1; }
    return fd;
}

static void spec(probeSpec *s, const char *host, uint16_t port, uint16_t count, uint32_t to)
{
    memset(s, 0, sizeof *s);
    strncpy(s->targetHost, host, sizeof s->targetHost - 1);
    s->dstPort = port;
    s->proto = PROBE_TCP;
    s->count = count;
    s->timeoutMs = to;
}

static void test_tcp_open(void)
{
    int lfd = startTcpListener(18900);
    probeSpec s;
    solariProbeResult r;
    TEST_ASSERT_TRUE(lfd >= 0);

    spec(&s, "127.0.0.1", 18900, 5, 1000);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, probeRun(&s, &r));
    TEST_ASSERT_EQUAL_UINT8(PROBE_OK, r.outcome);
    TEST_ASSERT_EQUAL_UINT16(0, r.lossPermille);
    close(lfd);
}

static void test_tcp_refused(void)
{
    probeSpec s;
    solariProbeResult r;
    spec(&s, "127.0.0.1", 1, 2, 1000);            /* nothing listens -> RST */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, probeRun(&s, &r));
    TEST_ASSERT_EQUAL_UINT8(PROBE_REFUSED, r.outcome);
}

static void test_tcp_unreachable(void)
{
    probeSpec s;
    solariProbeResult r;
    spec(&s, "10.255.255.1", 80, 1, 300);         /* unrouted, short timeout */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, probeRun(&s, &r));
    TEST_ASSERT_TRUE(r.outcome != PROBE_OK);
    TEST_ASSERT_TRUE(r.outcome != PROBE_REFUSED);
    TEST_ASSERT_EQUAL_UINT16(1000, r.lossPermille);   /* 100% loss */
}

static void test_dns_fail(void)
{
    probeSpec s;
    solariProbeResult r;
    spec(&s, "no.such.host.invalid", 80, 1, 500);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, probeRun(&s, &r));
    TEST_ASSERT_EQUAL_UINT8(PROBE_DNS_FAIL, r.outcome);
}

/* Build an SCP_MSG_CONTROL payload carrying CTRL_ADOPT_TARGET with `spec` as the
 * directive payload (the same spec form the .conf uses). */
static size_t buildAdopt(const char *spec, uint8_t *payload, size_t cap)
{
    solariControl c;
    size_t outLen = 0;
    uint16_t tc = 0;
    memset(&c, 0, sizeof c);
    c.verb       = CTRL_ADOPT_TARGET;
    c.payload    = (const uint8_t *)spec;
    c.payloadLen = (uint16_t)strlen(spec);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariMsgBuildControl(&c, payload, cap, &outLen, &tc));
    return outLen;
}

/* Returns true if cfg holds a target with the given HRW targetId. */
static int hasTarget(const monitorConfig *cfg, const char *targetId)
{
    uint8_t i;
    for (i = 0; i < cfg->targetCount; i++)
        if (!strcmp(cfg->targets[i].targetId, targetId)) return 1;
    return 0;
}

/* CTRL_ADOPT_TARGET adds the discovered entity to the live schedule, replies ok,
 * and is idempotent (re-adopting the same spec does not double-add). */
static void test_adopt_target(void)
{
    monitorConfig cfg;
    uint8_t payload[256], result[64];
    size_t plen, rlen = 0;
    uint16_t rtc = 0, code = 0xffff;
    uint8_t verb = 0;
    solariTlvReader r;
    uint16_t type; const uint8_t *val; uint16_t len;

    monitorConfigDefaults(&cfg);
    TEST_ASSERT_EQUAL_UINT8(0, cfg.targetCount);

    plen = buildAdopt("tcp:198.51.100.7:443 : edge", payload, sizeof payload);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        monitorHandleControl(&cfg, payload, plen, result, sizeof result, &rlen, &rtc));
    TEST_ASSERT_EQUAL_UINT8(1, cfg.targetCount);
    TEST_ASSERT_TRUE(hasTarget(&cfg, "tcp:198.51.100.7:443"));

    /* result echoes the verb and a zero (ok) error magnitude */
    solariTlvReaderInit(&r, result, rlen);
    while (solariTlvNext(&r, &type, &val, &len) == SOLARI_OK) {
        if (type == TLV_CTRL_VERB)  solariTlvReadU8 (val, len, &verb);
        if (type == TLV_ERROR_CODE) solariTlvReadU16(val, len, &code);
    }
    TEST_ASSERT_EQUAL_UINT8(CTRL_ADOPT_TARGET, verb);
    TEST_ASSERT_EQUAL_UINT16(0, code);

    /* idempotent: adopting the same spec again leaves the count unchanged */
    plen = buildAdopt("tcp:198.51.100.7:443 : edge", payload, sizeof payload);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        monitorHandleControl(&cfg, payload, plen, result, sizeof result, &rlen, &rtc));
    TEST_ASSERT_EQUAL_UINT8(1, cfg.targetCount);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tcp_open);
    RUN_TEST(test_tcp_refused);
    RUN_TEST(test_tcp_unreachable);
    RUN_TEST(test_dns_fail);
    RUN_TEST(test_adopt_target);
    return UNITY_END();
}
