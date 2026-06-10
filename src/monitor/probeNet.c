/*
 * probeNet.c - TCP/UDP/ICMP probe engine (sec 8.3, Appendix C).
 *
 * Linux/POSIX sockets. Each probeRun resolves the target once, runs `count`
 * attempts, and reports an aggregate verdict + avg RTT + jitter (mean absolute
 * deviation) + loss permille. ICMP uses the unprivileged SOCK_DGRAM/IPPROTO_ICMP
 * path where the gid permits, falling back to SOCK_RAW, else reporting that it
 * could not probe rather than crashing.
 */
#define _GNU_SOURCE

#include "probeNet.h"
#include "solari/solariTime.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <sys/socket.h>

#define PROBE_RTT_SAMPLES 64

static probeOutcome errnoToOutcome(int e)
{
    switch (e) {
        case ECONNREFUSED:               return PROBE_REFUSED;
        case EHOSTUNREACH: case ENETUNREACH: return PROBE_UNREACHABLE;
        case ETIMEDOUT:                  return PROBE_TIMEOUT;
        default:                         return PROBE_PROTO_ERR;
    }
}

static solariStatus resolveTarget(const char *host, uint16_t port, int socktype,
                                  struct addrinfo **res)
{
    struct addrinfo hints;
    char portstr[8];
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = (socktype == SOCK_RAW) ? 0 : socktype;
    snprintf(portstr, sizeof portstr, "%u", (unsigned)port);
    if (getaddrinfo(host, port ? portstr : NULL, &hints, res) != 0) return ERR_PLATFORM;
    return SOLARI_OK;
}

static void fillDstAddr(const struct sockaddr *sa, uint8_t out[16])
{
    memset(out, 0, 16);
    if (sa->sa_family == AF_INET) {
        out[10] = 0xff; out[11] = 0xff;                     /* IPv4-mapped */
        memcpy(out + 12, &((const struct sockaddr_in *)(const void *)sa)->sin_addr, 4);
    } else if (sa->sa_family == AF_INET6) {
        memcpy(out, &((const struct sockaddr_in6 *)(const void *)sa)->sin6_addr, 16);
    }
}

/* ---- per-attempt probes ---------------------------------------------------- */

static probeOutcome tcpOnce(const struct addrinfo *ai, uint32_t timeoutMs,
                            uint32_t *rtt, const char *expectBanner)
{
    int fd, fl, err = 0;
    socklen_t el = sizeof err;
    uint64_t t0;
    probeOutcome oc;
    int rc;

    fd = socket(ai->ai_family, SOCK_STREAM, 0);
    if (fd < 0) return PROBE_PROTO_ERR;
    fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    t0 = solariMonotonicMicros();
    rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc == 0) {
        oc = PROBE_OK;
    } else if (errno != EINPROGRESS) {
        oc = errnoToOutcome(errno);
        close(fd);
        return oc;
    } else {
        struct pollfd pfd;
        pfd.fd = fd; pfd.events = POLLOUT;
        rc = poll(&pfd, 1, (int)timeoutMs);
        if (rc == 0) { close(fd); return PROBE_TIMEOUT; }
        if (rc < 0)  { close(fd); return PROBE_PROTO_ERR; }
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err != 0) { close(fd); return errnoToOutcome(err); }
        oc = PROBE_OK;
    }
    *rtt = (uint32_t)(solariMonotonicMicros() - t0);

    if (oc == PROBE_OK && expectBanner && *expectBanner) {
        struct pollfd pfd;
        char buf[512];
        pfd.fd = fd; pfd.events = POLLIN;
        if (poll(&pfd, 1, (int)timeoutMs) > 0) {
            ssize_t r = recv(fd, buf, sizeof buf - 1, 0);
            if (r > 0) { buf[r] = '\0'; if (!strstr(buf, expectBanner)) oc = PROBE_PROTO_ERR; }
            else oc = PROBE_PROTO_ERR;
        } else {
            oc = PROBE_PROTO_ERR;
        }
    }
    close(fd);
    return oc;
}

static probeOutcome udpOnce(const struct addrinfo *ai, uint32_t timeoutMs, uint32_t *rtt)
{
    int fd;
    uint64_t t0;
    struct pollfd pfd;
    char buf[64];
    const char one = 0;
    int pr;
    ssize_t r;

    fd = socket(ai->ai_family, SOCK_DGRAM, 0);
    if (fd < 0) return PROBE_PROTO_ERR;
    t0 = solariMonotonicMicros();
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
        probeOutcome oc = errnoToOutcome(errno); close(fd); return oc;
    }
    if (send(fd, &one, 1, 0) != 1) {
        probeOutcome oc = errnoToOutcome(errno); close(fd); return oc;
    }
    pfd.fd = fd; pfd.events = POLLIN;
    pr = poll(&pfd, 1, (int)timeoutMs);
    if (pr == 0) { close(fd); *rtt = 0; return PROBE_TIMEOUT; }   /* no reply: ambiguous */
    if (pr < 0)  { close(fd); return PROBE_PROTO_ERR; }
    r = recv(fd, buf, sizeof buf, 0);
    *rtt = (uint32_t)(solariMonotonicMicros() - t0);
    if (r >= 0) { close(fd); return PROBE_OK; }
    { probeOutcome oc = (errno == ECONNREFUSED) ? PROBE_REFUSED : PROBE_PROTO_ERR;
      close(fd); return oc; }
}

static uint16_t icmpChecksum(const void *data, size_t len)
{
    const uint16_t *p = data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static int icmpSocket(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);   /* unprivileged path */
    if (fd >= 0) return fd;
    return socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);        /* needs CAP_NET_RAW */
}

static probeOutcome icmpOnce(const struct addrinfo *ai, uint32_t timeoutMs,
                             uint32_t *rtt, uint16_t seq)
{
    int fd;
    uint8_t pkt[24];
    struct icmphdr *ic;
    uint64_t t0;
    struct pollfd pfd;
    uint8_t rbuf[256];
    int pr;
    ssize_t r;

    if (ai->ai_family != AF_INET) return PROBE_UNREACHABLE;   /* IPv4 ICMP only */
    fd = icmpSocket();
    if (fd < 0) return PROBE_UNREACHABLE;                      /* cannot probe */

    memset(pkt, 0, sizeof pkt);
    ic = (struct icmphdr *)(void *)pkt;
    ic->type = ICMP_ECHO;
    ic->un.echo.id       = htons((uint16_t)(getpid() & 0xffff));
    ic->un.echo.sequence = htons(seq);
    ic->checksum = icmpChecksum(pkt, sizeof pkt);

    t0 = solariMonotonicMicros();
    if (sendto(fd, pkt, sizeof pkt, 0, ai->ai_addr, ai->ai_addrlen) < 0) {
        probeOutcome oc = errnoToOutcome(errno); close(fd); return oc;
    }
    pfd.fd = fd; pfd.events = POLLIN;
    pr = poll(&pfd, 1, (int)timeoutMs);
    if (pr == 0) { close(fd); return PROBE_TIMEOUT; }
    if (pr < 0)  { close(fd); return PROBE_PROTO_ERR; }
    r = recv(fd, rbuf, sizeof rbuf, 0);
    *rtt = (uint32_t)(solariMonotonicMicros() - t0);
    close(fd);
    if (r < 0) return PROBE_PROTO_ERR;

    {   /* SOCK_RAW prepends the IP header; SOCK_DGRAM does not */
        struct icmphdr *rep;
        if ((rbuf[0] >> 4) == 4 && r >= 20) {
            int ihl = (rbuf[0] & 0x0f) * 4;
            rep = (struct icmphdr *)(void *)(rbuf + ihl);
        } else {
            rep = (struct icmphdr *)(void *)rbuf;
        }
        if (rep->type == ICMP_ECHOREPLY)   return PROBE_OK;
        if (rep->type == ICMP_DEST_UNREACH) return PROBE_UNREACHABLE;
        return PROBE_PROTO_ERR;
    }
}

/* ---- aggregate round ------------------------------------------------------- */

solariStatus probeRun(const probeSpec *spec, solariProbeResult *out)
{
    struct addrinfo *res = NULL, *ai;
    uint16_t count, i, nr = 0, ok = 0;
    uint32_t timeout, rtts[PROBE_RTT_SAMPLES];
    probeOutcome lastFail = PROBE_TIMEOUT;
    int socktype;

    if (!spec || !out) return ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    out->proto = (uint8_t)spec->proto;
    out->dstPort = spec->dstPort;
    out->probeTimeUnixMs = solariNowUnixMs();

    count   = spec->count ? spec->count : 1;
    timeout = spec->timeoutMs ? spec->timeoutMs : 1000;
    socktype = (spec->proto == PROBE_TCP) ? SOCK_STREAM
             : (spec->proto == PROBE_UDP) ? SOCK_DGRAM : SOCK_RAW;

    if (resolveTarget(spec->targetHost, spec->dstPort, socktype, &res) != SOLARI_OK) {
        out->outcome = (uint8_t)PROBE_DNS_FAIL;
        return SOLARI_OK;
    }
    ai = res;
    if (spec->proto == PROBE_ICMP) {                 /* ICMP needs an IPv4 addr */
        struct addrinfo *p;
        for (p = res; p; p = p->ai_next) if (p->ai_family == AF_INET) { ai = p; break; }
    }
    fillDstAddr(ai->ai_addr, out->dstAddr);

    for (i = 0; i < count; i++) {
        uint32_t rtt = 0;
        probeOutcome oc;
        if      (spec->proto == PROBE_TCP) oc = tcpOnce(ai, timeout, &rtt, spec->expectBanner);
        else if (spec->proto == PROBE_UDP) oc = udpOnce(ai, timeout, &rtt);
        else                               oc = icmpOnce(ai, timeout, &rtt, (uint16_t)(i + 1));
        if (oc == PROBE_OK) { if (nr < PROBE_RTT_SAMPLES) rtts[nr++] = rtt; ok++; }
        else lastFail = oc;
    }
    freeaddrinfo(res);

    out->lossPermille = (uint16_t)(((uint32_t)(count - ok) * 1000u) / count);
    if (ok > 0) {
        uint64_t sum = 0, dev = 0;
        uint32_t avg;
        for (i = 0; i < nr; i++) sum += rtts[i];
        avg = (uint32_t)(sum / nr);
        for (i = 0; i < nr; i++) dev += (rtts[i] > avg) ? (rtts[i] - avg) : (avg - rtts[i]);
        out->rttMicros    = avg;
        out->jitterMicros = nr ? (uint32_t)(dev / nr) : 0;
        out->outcome      = (uint8_t)PROBE_OK;
    } else {
        out->outcome = (uint8_t)lastFail;
    }
    return SOLARI_OK;
}

solariStatus probeLinkToPeer(uint64_t peerNodeId, const char *peerUrl, solariLinkStat *out)
{
    if (!out) return ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    out->peerNodeId = peerNodeId;
    SOLARI_UNUSED(peerUrl);
    return SOLARI_OK;          /* transit/throughput populated when the mesh lands */
}
