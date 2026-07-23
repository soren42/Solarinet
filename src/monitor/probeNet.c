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

static probeOutcome tcpConnectFd(const struct addrinfo *ai, uint32_t timeoutMs, int *outFd)
{
    int fd, fl, err = 0;
    socklen_t el = sizeof err;
    int rc;

    fd = socket(ai->ai_family, SOCK_STREAM, 0);
    if (fd < 0) return PROBE_PROTO_ERR;
    fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc != 0) {
        if (errno != EINPROGRESS) {
            probeOutcome oc = errnoToOutcome(errno);
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
        }
    }

    *outFd = fd;
    return PROBE_OK;
}

static probeOutcome sendAllPoll(int fd, const void *buf, size_t len, uint32_t timeoutMs)
{
    const uint8_t *p = buf;
    size_t off = 0;

    while (off < len) {
        ssize_t n = send(fd, p + off, len - off, 0);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd;
            int pr;
            pfd.fd = fd; pfd.events = POLLOUT;
            pr = poll(&pfd, 1, (int)timeoutMs);
            if (pr == 0) return PROBE_TIMEOUT;
            if (pr < 0)  return PROBE_PROTO_ERR;
            continue;
        }
        return errnoToOutcome(errno);
    }
    return PROBE_OK;
}

static ssize_t recvPoll(int fd, void *buf, size_t cap, uint32_t timeoutMs,
                        probeOutcome *oc)
{
    struct pollfd pfd;
    int pr;

    pfd.fd = fd; pfd.events = POLLIN;
    pr = poll(&pfd, 1, (int)timeoutMs);
    if (pr == 0) { *oc = PROBE_TIMEOUT; return -1; }
    if (pr < 0)  { *oc = PROBE_PROTO_ERR; return -1; }
    {
        ssize_t r = recv(fd, buf, cap, 0);
        if (r <= 0) { *oc = (r == 0) ? PROBE_PROTO_ERR : errnoToOutcome(errno); return -1; }
        return r;
    }
}

static probeOutcome appCheckDns(const struct addrinfo *ai, uint32_t timeoutMs,
                                uint32_t *rtt)
{
    uint8_t q[] = {
        0x53, 0x4e, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01
    };
    uint8_t buf[512];
    struct pollfd pfd;
    int fd, pr;
    ssize_t r;
    uint64_t t0;

    fd = socket(ai->ai_family, SOCK_DGRAM, 0);
    if (fd < 0) return PROBE_PROTO_ERR;
    t0 = solariMonotonicMicros();
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
        probeOutcome oc = errnoToOutcome(errno); close(fd); return oc;
    }
    if (send(fd, q, sizeof q, 0) != (ssize_t)sizeof q) {
        probeOutcome oc = errnoToOutcome(errno); close(fd); return oc;
    }
    pfd.fd = fd; pfd.events = POLLIN;
    pr = poll(&pfd, 1, (int)timeoutMs);
    if (pr == 0) { close(fd); return PROBE_TIMEOUT; }
    if (pr < 0)  { close(fd); return PROBE_PROTO_ERR; }
    r = recv(fd, buf, sizeof buf, 0);
    *rtt = (uint32_t)(solariMonotonicMicros() - t0);
    close(fd);
    if (r < 12) return PROBE_PROTO_ERR;
    if (buf[0] != q[0] || buf[1] != q[1]) return PROBE_PROTO_ERR;
    if ((buf[2] & 0x80) == 0) return PROBE_PROTO_ERR;
    return PROBE_OK;
}

static int berLen(const uint8_t *buf, size_t cap, size_t *hdrLen, size_t *valLen)
{
    size_t n, i;
    if (cap < 2) return 0;
    if ((buf[1] & 0x80) == 0) {
        *hdrLen = 2;
        *valLen = buf[1];
        return *hdrLen + *valLen <= cap;
    }
    n = buf[1] & 0x7f;
    if (n == 0 || n > 2 || cap < 2 + n) return 0;
    *valLen = 0;
    for (i = 0; i < n; i++) *valLen = (*valLen << 8) | buf[2 + i];
    *hdrLen = 2 + n;
    return *hdrLen + *valLen <= cap;
}

static probeOutcome appCheckLdap(int fd, uint32_t timeoutMs)
{
    static const uint8_t bindReq[] = {
        0x30, 0x0c, 0x02, 0x01, 0x01, 0x60, 0x07,
        0x02, 0x01, 0x03, 0x04, 0x00, 0x80, 0x00
    };
    uint8_t buf[512];
    size_t hdrLen, valLen, pos, end;
    ssize_t r;
    probeOutcome oc;

    oc = sendAllPoll(fd, bindReq, sizeof bindReq, timeoutMs);
    if (oc != PROBE_OK) return oc;
    r = recvPoll(fd, buf, sizeof buf, timeoutMs, &oc);
    if (r < 0) return oc;
    if (buf[0] != 0x30 || !berLen(buf, (size_t)r, &hdrLen, &valLen)) return PROBE_PROTO_ERR;
    pos = hdrLen;
    end = hdrLen + valLen;
    if (end < pos + 5 || buf[pos] != 0x02 || buf[pos + 1] != 0x01) return PROBE_PROTO_ERR;
    pos += 2 + buf[pos + 1];
    if (pos >= end || buf[pos] != 0x61) return PROBE_PROTO_ERR;
    return PROBE_OK;
}

static probeOutcome appCheckHttp(int fd, uint32_t timeoutMs, const char *host,
                                 const char *pathSpec)
{
    char pathBuf[256];
    char req[512];
    char buf[512];
    const char *path = pathSpec;
    const char *want = NULL;
    const char *bar;
    size_t pathLen;
    size_t len, pos, versionLen;
    ssize_t r;
    int n, code, wantCode = -1, wantClass = -1;
    probeOutcome oc;

    if (!path || !*path) path = "/";
    bar = strchr(path, '|');
    if (bar) {
        pathLen = (size_t)(bar - path);
        want = bar + 1;
        if (pathLen == 0) {
            path = "/";
        } else {
            if (pathLen >= sizeof pathBuf) return PROBE_PROTO_ERR;
            memcpy(pathBuf, path, pathLen);
            pathBuf[pathLen] = '\0';
            path = pathBuf;
        }
        if (want[0] >= '0' && want[0] <= '9' &&
            want[1] >= '0' && want[1] <= '9' &&
            want[2] >= '0' && want[2] <= '9' &&
            want[3] == '\0') {
            wantCode = (want[0] - '0') * 100 + (want[1] - '0') * 10 + (want[2] - '0');
        } else if (want[0] >= '0' && want[0] <= '9' &&
                   want[1] == 'x' && want[2] == 'x' && want[3] == '\0') {
            wantClass = want[0] - '0';
        } else {
            return PROBE_PROTO_ERR;
        }
    }
    n = snprintf(req, sizeof req,
                 "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                 path, host ? host : "");
    if (n <= 0 || (size_t)n >= sizeof req) return PROBE_PROTO_ERR;
    oc = sendAllPoll(fd, req, (size_t)n, timeoutMs);
    if (oc != PROBE_OK) return oc;
    r = recvPoll(fd, buf, sizeof buf - 1, timeoutMs, &oc);
    if (r < 0) return oc;
    buf[r] = '\0';
    len = (size_t)r;
    if (len < 5 || memcmp(buf, "HTTP/", 5) != 0) return PROBE_PROTO_ERR;
    pos = 5;
    versionLen = 0;
    while (pos < len && buf[pos] != ' ' && versionLen < 8) {
        if (!((buf[pos] >= '0' && buf[pos] <= '9') || buf[pos] == '.')) return PROBE_PROTO_ERR;
        pos++;
        versionLen++;
    }
    if (pos >= len || buf[pos] != ' ') return PROBE_PROTO_ERR;
    pos++;
    if (pos + 3 > len ||
        buf[pos] < '0' || buf[pos] > '9' ||
        buf[pos + 1] < '0' || buf[pos + 1] > '9' ||
        buf[pos + 2] < '0' || buf[pos + 2] > '9' ||
        (pos + 3 < len && buf[pos + 3] >= '0' && buf[pos + 3] <= '9')) {
        return PROBE_PROTO_ERR;
    }
    code = (buf[pos] - '0') * 100 + (buf[pos + 1] - '0') * 10 + (buf[pos + 2] - '0');
    if (wantCode >= 0 && code == wantCode) return PROBE_OK;
    if (wantClass >= 0 && code / 100 == wantClass) return PROBE_OK;
    if (!want && code >= 200 && code < 300) return PROBE_OK;
    return PROBE_PROTO_ERR;
}

static probeOutcome appCheckMysql(int fd, uint32_t timeoutMs)
{
    uint8_t buf[512];
    ssize_t r;
    probeOutcome oc;

    r = recvPoll(fd, buf, sizeof buf, timeoutMs, &oc);
    if (r < 0) return oc;
    if (r < 5) return PROBE_PROTO_ERR;
    if (buf[3] != 0x00 || buf[4] != 0x0a) return PROBE_PROTO_ERR;
    if ((buf[0] | (buf[1] << 8) | (buf[2] << 16)) == 0) return PROBE_PROTO_ERR;
    return PROBE_OK;
}

static probeOutcome appCheckAmqp(int fd, uint32_t timeoutMs)
{
    static const uint8_t hdr[] = { 'A', 'M', 'Q', 'P', 0x00, 0x00, 0x09, 0x01 };
    uint8_t buf[512];
    ssize_t r;
    probeOutcome oc;

    oc = sendAllPoll(fd, hdr, sizeof hdr, timeoutMs);
    if (oc != PROBE_OK) return oc;
    r = recvPoll(fd, buf, sizeof buf, timeoutMs, &oc);
    if (r < 0) return oc;
    if (r < 11) return PROBE_PROTO_ERR;
    if (buf[0] != 0x01) return PROBE_PROTO_ERR;       /* METHOD frame */
    if (((uint16_t)buf[7] << 8 | buf[8]) != 10) return PROBE_PROTO_ERR;
    return PROBE_OK;
}

static probeOutcome tcpAppOnce(const struct addrinfo *ai, uint32_t timeoutMs,
                               uint32_t *rtt, const probeSpec *spec)
{
    int fd = -1;
    uint64_t t0;
    probeOutcome oc;

    t0 = solariMonotonicMicros();
    oc = tcpConnectFd(ai, timeoutMs, &fd);
    if (oc == PROBE_OK) {
        switch (spec->appCheck) {
            case APP_CHECK_LDAP:  oc = appCheckLdap(fd, timeoutMs); break;
            case APP_CHECK_HTTP:  oc = appCheckHttp(fd, timeoutMs, spec->targetHost, spec->appArg); break;
            case APP_CHECK_MYSQL: oc = appCheckMysql(fd, timeoutMs); break;
            case APP_CHECK_AMQP:  oc = appCheckAmqp(fd, timeoutMs); break;
            default:              oc = PROBE_PROTO_ERR; break;
        }
    }
    *rtt = (uint32_t)(solariMonotonicMicros() - t0);
    if (fd >= 0) close(fd);
    return oc;
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
        if (spec->appCheck == APP_CHECK_DNS) oc = appCheckDns(ai, timeout, &rtt);
        else if (spec->appCheck != APP_CHECK_NONE && spec->proto == PROBE_TCP)
            oc = tcpAppOnce(ai, timeout, &rtt, spec);
        else if (spec->appCheck != APP_CHECK_NONE) oc = PROBE_PROTO_ERR;
        else if (spec->proto == PROBE_TCP) oc = tcpOnce(ai, timeout, &rtt, spec->expectBanner);
        else if (spec->proto == PROBE_UDP) oc = udpOnce(ai, timeout, &rtt);
        else oc = icmpOnce(ai, timeout, &rtt, (uint16_t)(i + 1));
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
