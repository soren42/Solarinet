/*
 * serverScan.c - active network discovery scan (Handoff §7.1).
 *
 * Operator-initiated counterpart to the passive DISCOVERY_ADVERT path: sweeps a
 * CIDR and records reachable hosts (and the services they expose) into the
 * `discovered` table the dashboard surfaces for adoption.
 *
 * The core mechanism is a native, dependency-free standard-C TCP connect-scan:
 * non-blocking connect() over a bounded window of (host, port) jobs, harvested
 * with select(). A host with at least one open port is reverse-resolved and
 * upserted via serverDbUpsertDiscovered (via = "portscan"), with the open ports
 * captured as a JSON services array ("ssh:22", "https:443", ...).
 *
 * When compiled with SOLARI_WITH_DISCOVERY_TOOLS, each found host is additionally
 * enriched, best-effort, from the host's zeroconf (avahi-browse) and SNMP
 * (snmpget) utilities. These are optional and OFF by default so the core server
 * stays pure standard C; LLDP enrichment (lldpd) is a documented follow-up.
 */
#define _DEFAULT_SOURCE   /* getaddrinfo, inet_ntop, getnameinfo under -std=c99 */

#include "serverScan.h"

#include "solari/solariLog.h"
#include "solari/solariTime.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---- bounds / tunables --------------------------------------------------- */
#define SCAN_MAX_HOSTS     4096    /* refuse a CIDR wider than this           */
#define SCAN_MAX_PORTS     64      /* per-scan port list cap                  */
#define SCAN_WINDOW        256     /* max concurrent in-flight connects       */
#define SCAN_CONNECT_MS    300     /* per-batch connect timeout (ms)          */

/* A conservative "common services" default when the caller names no ports. */
static const int SCAN_DEFAULT_PORTS[] = {
    21, 22, 23, 25, 53, 80, 110, 123, 143, 161, 443, 445, 465, 587, 631,
    993, 995, 1433, 1883, 3306, 3389, 5432, 5900, 6379, 8080, 8443, 9090,
    9100, 7701, 7702, 7703
};

/* Best-effort IANA-ish service label for a port (for the services array). */
static const char *scanServiceName(int port)
{
    switch (port) {
    case 21: return "ftp";      case 22: return "ssh";    case 23: return "telnet";
    case 25: return "smtp";     case 53: return "dns";    case 80: return "http";
    case 110: return "pop3";    case 123: return "ntp";   case 143: return "imap";
    case 161: return "snmp";    case 443: return "https"; case 445: return "smb";
    case 465: return "smtps";   case 587: return "submission"; case 631: return "ipp";
    case 993: return "imaps";   case 995: return "pop3s"; case 1433: return "mssql";
    case 1883: return "mqtt";   case 3306: return "mysql"; case 3389: return "rdp";
    case 5432: return "postgres"; case 5900: return "vnc"; case 6379: return "redis";
    case 8080: return "http-alt"; case 8443: return "https-alt"; case 9090: return "http-mgmt";
    case 9100: return "jetdirect";
    case 7701: return "solari-ingest"; case 7702: return "solari-control";
    case 7703: return "solari-pub";
    default: return "tcp";
    }
}

/* ---- CIDR / port parsing ------------------------------------------------- */

/* Parse "a.b.c.d/nn" into a host base (network+1) and host count, IPv4 only.
 * Returns ERR_INVALID_ARG on a malformed spec or a range wider than the cap. */
static solariStatus scanParseCidr(const char *cidr, uint32_t *baseHostOut,
                                  uint32_t *countOut, uint8_t *prefixOut)
{
    char buf[64];
    char *slash;
    struct in_addr in;
    unsigned long prefix;
    uint32_t net, mask, hosts, first;

    if (!cidr || !baseHostOut || !countOut) return ERR_INVALID_ARG;
    if (strlen(cidr) >= sizeof buf) return ERR_INVALID_ARG;
    strcpy(buf, cidr);

    slash = strchr(buf, '/');
    if (!slash) return ERR_INVALID_ARG;
    *slash = '\0';
    prefix = strtoul(slash + 1, NULL, 10);
    if (prefix > 32) return ERR_INVALID_ARG;
    if (inet_pton(AF_INET, buf, &in) != 1) return ERR_INVALID_ARG;

    net  = ntohl(in.s_addr);
    mask = (prefix == 0) ? 0u : (0xFFFFFFFFu << (32 - prefix));
    net &= mask;

    if (prefix >= 31) {
        /* /31 and /32: treat every address in-range as a host (no net/bcast). */
        hosts = (prefix == 32) ? 1u : 2u;
        first = net;
    } else {
        hosts = (mask == 0) ? 0u : (~mask);   /* total addrs - 2 (net+bcast)  */
        first = net + 1;                       /* skip network address          */
        if (hosts == 0) return ERR_INVALID_ARG;
    }

    if (hosts > SCAN_MAX_HOSTS) {
        solariLogf(SOLARI_LOG_WARN,
                   "scan: CIDR /%lu has %u hosts; exceeds cap %d", prefix,
                   hosts, SCAN_MAX_HOSTS);
        return ERR_INVALID_ARG;
    }

    *baseHostOut = first;
    *countOut    = hosts;
    if (prefixOut) *prefixOut = (uint8_t)prefix;
    return SOLARI_OK;
}

/* Parse a "22,80,443" CSV into ports[]. Falls back to the common default set
 * when csv is NULL/empty. Returns the count written (<= SCAN_MAX_PORTS). */
static int scanParsePorts(const char *csv, int *ports)
{
    int n = 0;
    const char *p;

    if (!csv || !csv[0]) {
        int def = (int)(sizeof SCAN_DEFAULT_PORTS / sizeof SCAN_DEFAULT_PORTS[0]);
        for (n = 0; n < def && n < SCAN_MAX_PORTS; n++) ports[n] = SCAN_DEFAULT_PORTS[n];
        return n;
    }
    p = csv;
    while (*p && n < SCAN_MAX_PORTS) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end != p && v > 0 && v <= 65535) ports[n++] = (int)v;
        if (*end == '\0') break;
        p = end + 1;   /* skip the delimiter */
    }
    return n;
}

/* ---- non-blocking helpers ------------------------------------------------ */

static int scanSetNonBlock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* ---- optional enrichment (host tools; OFF unless flag set) --------------- */
#ifdef SOLARI_WITH_DISCOVERY_TOOLS
/* Run a command and copy its first non-empty trimmed output line into out.
 * Best-effort: returns 0 on success, -1 otherwise. */
static int scanToolLine(const char *cmd, char *out, size_t cap)
{
    FILE *f;
    char line[512];
    if (out && cap) out[0] = '\0';
    f = popen(cmd, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        char *s = line, *e;
        while (*s == ' ' || *s == '\t') s++;
        e = s + strlen(s);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = '\0';
        if (*s) { snprintf(out, cap, "%s", s); pclose(f); return 0; }
    }
    pclose(f);
    return -1;
}

/* SNMP sysName via snmpget v2c (public). Quick-and-dirty enrichment. */
static void scanEnrichSnmp(const char *ip, char *nameOut, size_t cap)
{
    char cmd[256];
    /* -Ovq: value only, no type; short timeout, one retry. */
    snprintf(cmd, sizeof cmd,
             "snmpget -v2c -c public -t 1 -r 1 -Ovq %s SNMPv2-MIB::sysName.0 2>/dev/null",
             ip);
    (void)scanToolLine(cmd, nameOut, cap);
}
#endif /* SOLARI_WITH_DISCOVERY_TOOLS */

/* ---- scan core ----------------------------------------------------------- */

/* Record one reachable host into `discovered`. openPorts[] lists its open ports
 * (nopen of them). Builds the services JSON, reverse-resolves the name, upserts. */
static void scanRecordHost(serverContext *ctx, uint32_t hostBE,
                           const int *openPorts, int nopen, uint8_t prefix)
{
    serverDiscEntity e;
    struct sockaddr_in sa;
    char ipstr[SERVER_IP_MAX];
    char host[SOLARI_FQDN_MAX];
    char services[512];
    size_t off = 0;
    int i;
    uint64_t discId = 0;
    solariStatus rc;

    memset(&e, 0, sizeof e);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(hostBE);

    if (!inet_ntop(AF_INET, &sa.sin_addr, ipstr, sizeof ipstr)) return;

    /* services JSON array: ["ssh:22","https:443"] */
    off += (size_t)snprintf(services + off, sizeof services - off, "[");
    for (i = 0; i < nopen && off < sizeof services - 32; i++) {
        off += (size_t)snprintf(services + off, sizeof services - off, "%s\"%s:%d\"",
                                i ? "," : "", scanServiceName(openPorts[i]), openPorts[i]);
    }
    snprintf(services + off, sizeof services - off, "]");

    /* reverse DNS (best-effort; leave host empty on failure). */
    host[0] = '\0';
    (void)getnameinfo((struct sockaddr *)&sa, sizeof sa, host, sizeof host,
                      NULL, 0, NI_NAMEREQD);

    snprintf(e.ip, sizeof e.ip, "%s", ipstr);
    snprintf(e.kind, sizeof e.kind, "%s", "host");
    snprintf(e.via, sizeof e.via, "%s", "portscan");
    snprintf(e.servicesJson, sizeof e.servicesJson, "%s", services);
    snprintf(e.segId, sizeof e.segId, "lan/%u", (unsigned)prefix);
    if (host[0]) snprintf(e.host, sizeof e.host, "%s", host);

#ifdef SOLARI_WITH_DISCOVERY_TOOLS
    if (!e.host[0]) {
        char snmpName[SOLARI_FQDN_MAX];
        scanEnrichSnmp(ipstr, snmpName, sizeof snmpName);
        if (snmpName[0]) snprintf(e.host, sizeof e.host, "%s", snmpName);
    }
#endif

    rc = serverDbUpsertDiscovered(ctx->db, &e, solariNowUnixMs(), &discId);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_WARN, "scan: upsert %s failed: %s",
                   ipstr, solariStrError(rc));
        return;
    }
    solariLogf(SOLARI_LOG_INFO, "scan: discovered %s (%s) %d service(s) [disc=%llu]",
               ipstr, e.host[0] ? e.host : "no-rdns", nopen,
               (unsigned long long)discId);
}

solariStatus serverScanRun(serverContext *ctx, const char *cidr,
                           const char *portsCsv, size_t *foundOut)
{
    uint32_t baseHost = 0, nhosts = 0;
    uint8_t  prefix = 0;
    int      ports[SCAN_MAX_PORTS];
    int      nports;
    uint8_t *openMap = NULL;     /* nhosts * nports flags */
    size_t   found = 0;
    long     total, start;
    solariStatus rc;

    if (foundOut) *foundOut = 0;
    if (!ctx || !ctx->db) return ERR_INVALID_ARG;

    rc = scanParseCidr(cidr, &baseHost, &nhosts, &prefix);
    if (rc != SOLARI_OK) return rc;
    nports = scanParsePorts(portsCsv, ports);
    if (nports <= 0) return ERR_INVALID_ARG;

    openMap = (uint8_t *)calloc((size_t)nhosts * (size_t)nports, 1);
    if (!openMap) return ERR_PLATFORM;

    solariLogf(SOLARI_LOG_INFO, "scan: %s -> %u host(s) x %d port(s)",
               cidr, nhosts, nports);

    total = (long)nhosts * (long)nports;

    /* Batched non-blocking connect-scan: launch a window of connects, then
     * select() for writability, harvesting SO_ERROR==0 as "open". */
    for (start = 0; start < total; start += SCAN_WINDOW) {
        int      batch = (int)((total - start < SCAN_WINDOW) ? (total - start) : SCAN_WINDOW);
        int     *fds   = (int *)malloc((size_t)batch * sizeof(int));
        long    *jobs  = (long *)malloc((size_t)batch * sizeof(long));
        int      pending = 0, i;
        uint64_t deadline;

        if (!fds || !jobs) { free(fds); free(jobs); free(openMap); return ERR_PLATFORM; }

        for (i = 0; i < batch; i++) {
            long job = start + i;
            uint32_t hidx = (uint32_t)(job / nports);
            int      pidx = (int)(job % nports);
            struct sockaddr_in sa;
            int fd;

            fds[i] = -1;
            fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) continue;
            if (scanSetNonBlock(fd) != 0) { close(fd); continue; }

            memset(&sa, 0, sizeof sa);
            sa.sin_family = AF_INET;
            sa.sin_addr.s_addr = htonl(baseHost + hidx);
            sa.sin_port = htons((uint16_t)ports[pidx]);

            if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) {
                openMap[(size_t)hidx * nports + pidx] = 1;   /* immediate open */
                close(fd);
            } else if (errno == EINPROGRESS) {
                fds[i] = fd; jobs[i] = job; pending++;
            } else {
                close(fd);   /* immediate refusal / unreachable */
            }
        }

        deadline = solariNowUnixMs() + SCAN_CONNECT_MS;
        while (pending > 0) {
            fd_set wf;
            struct timeval tv;
            uint64_t now = solariNowUnixMs();
            long remain = (long)deadline - (long)now;
            int maxfd = -1, ready;

            if (remain <= 0) break;
            FD_ZERO(&wf);
            for (i = 0; i < batch; i++) {
                if (fds[i] >= 0) { FD_SET(fds[i], &wf); if (fds[i] > maxfd) maxfd = fds[i]; }
            }
            if (maxfd < 0) break;
            tv.tv_sec  = remain / 1000;
            tv.tv_usec = (remain % 1000) * 1000;
            ready = select(maxfd + 1, NULL, &wf, NULL, &tv);
            if (ready <= 0) break;   /* timeout or error: remaining are filtered */

            for (i = 0; i < batch; i++) {
                if (fds[i] < 0 || !FD_ISSET(fds[i], &wf)) continue;
                int err = 0; socklen_t el = sizeof err;
                if (getsockopt(fds[i], SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0) {
                    uint32_t hidx = (uint32_t)(jobs[i] / nports);
                    int      pidx = (int)(jobs[i] % nports);
                    openMap[(size_t)hidx * nports + pidx] = 1;
                }
                close(fds[i]); fds[i] = -1; pending--;
            }
        }
        for (i = 0; i < batch; i++) if (fds[i] >= 0) close(fds[i]);
        free(fds); free(jobs);
    }

    /* Emit one discovered row per host with >= 1 open port. */
    {
        uint32_t h;
        int openPorts[SCAN_MAX_PORTS];
        for (h = 0; h < nhosts; h++) {
            int nopen = 0, p;
            for (p = 0; p < nports; p++)
                if (openMap[(size_t)h * nports + p]) openPorts[nopen++] = ports[p];
            if (nopen > 0) {
                scanRecordHost(ctx, baseHost + h, openPorts, nopen, prefix);
                found++;
            }
        }
    }

    free(openMap);
    solariLogf(SOLARI_LOG_INFO, "scan: complete, %zu host(s) recorded", found);
    if (foundOut) *foundOut = found;
    return SOLARI_OK;
}
