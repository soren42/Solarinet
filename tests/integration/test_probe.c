/*
 * test_probe.c - the TCP probe engine (sec 8.3) against loopback: an open port
 * reads OK with zero loss; a closed port is refused; an unrouted host is not
 * reachable; a bogus name is a DNS failure. (ICMP/UDP need privilege/network
 * and are exercised manually, not asserted here.)
 */
#include "unity.h"
#include "probeNet.h"
#include "solari/solariError.h"

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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tcp_open);
    RUN_TEST(test_tcp_refused);
    RUN_TEST(test_tcp_unreachable);
    RUN_TEST(test_dns_fail);
    return UNITY_END();
}
