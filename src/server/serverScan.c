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
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE   /* getaddrinfo, inet_ntop, getnameinfo under -std=c99 */
#endif

#include "serverScan.h"

#include "serverDiscoveryEnrich.h"
#include "serverOui.h"
#include "solari/solariLog.h"
#include "solari/solariTime.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---- bounds / tunables --------------------------------------------------- */
#define SCAN_MAX_HOSTS     4096    /* refuse a CIDR wider than this           */
#define SCAN_MAX_PORTS     64      /* per-scan port list cap                  */
#define SCAN_WINDOW        256     /* max concurrent in-flight connects       */
#define SCAN_CONNECT_MS    300     /* per-batch connect timeout (ms)          */
#ifndef SCAN_TOOL_TIMEOUT_MS
#define SCAN_TOOL_TIMEOUT_MS 35000 /* optional enrichment subprocess ceiling  */
#endif

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
    const char *digit;
    struct in_addr in;
    unsigned long prefix;
    uint32_t net, mask, hosts, first;

    if (!cidr || !baseHostOut || !countOut) return ERR_INVALID_ARG;
    if (strlen(cidr) >= sizeof buf) return ERR_INVALID_ARG;
    strcpy(buf, cidr);

    slash = strchr(buf, '/');
    if (!slash) return ERR_INVALID_ARG;
    if (strchr(slash + 1, '/') || slash == buf || slash[1] == '\0')
        return ERR_INVALID_ARG;
    for (digit = slash + 1; *digit; digit++)
        if (*digit < '0' || *digit > '9') return ERR_INVALID_ARG;
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
        hosts = (mask == 0) ? 0u : ((~mask) - 1u); /* exclude network+broadcast */
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
    while (*p) {
        char *end;
        long v;
        const char *digit;

        if (n >= SCAN_MAX_PORTS || *p < '0' || *p > '9') return 0;
        for (digit = p; *digit && *digit != ','; digit++)
            if (*digit < '0' || *digit > '9') return 0;
        v = strtol(p, &end, 10);
        if (end == p || v <= 0 || v > 65535 || (*end != ',' && *end != '\0'))
            return 0;
        {
            int i;
            for (i = 0; i < n; i++) if (ports[i] == (int)v) return 0;
        }
        ports[n++] = (int)v;
        if (*end == '\0') break;
        p = end + 1;
        if (*p == '\0') return 0;
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
static void scanSanitize(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; in && *in && o + 1 < cap; in++) {
        char c = *in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' ||
            c == ':' || c == '-')
            out[o++] = c;
    }
    out[o] = '\0';
}

static void scanTrim(char *s)
{
    char *e;
    if (!s) return;
    while (*s == ' ' || *s == '\t') memmove(s, s + 1, strlen(s));
    e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' ||
                     e[-1] == ' ' || e[-1] == '\t'))
        *--e = '\0';
    if (s[0] == '"' && e > s + 1 && e[-1] == '"') {
        memmove(s, s + 1, (size_t)(e - s - 2));
        s[e - s - 2] = '\0';
    }
}

typedef struct {
    serverScanProgressFn progress;
    void *userData;
    uint32_t hostsDone;
    uint32_t hostsFound;
    solariStatus cancelStatus;
} scanToolControl;

static uint64_t scanMonotonicMs(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void scanStopToolGroup(pid_t pid)
{
    int status, reaped = 0;
    uint64_t until;
    pid_t rc;
    if (pid <= 0) return;

    if (kill(-pid, SIGTERM) != 0) (void)kill(pid, SIGTERM);
    until = scanMonotonicMs() + 200;
    do {
        rc = waitpid(pid, &status, WNOHANG);
        if (rc == pid || (rc < 0 && errno == ECHILD)) { reaped = 1; break; }
        (void)poll(NULL, 0, 10);
    } while (scanMonotonicMs() < until);

    /* The shell may have exited while a grandchild still owns stdout. Kill the
     * process group even after reaping the group leader. */
    (void)kill(-pid, SIGKILL);
    if (!reaped) {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
    }
}

/* Execute one fixed, sanitized host-tool command in its own process group.
 * Output is always drained, including after the caller's bounded buffer fills,
 * so an unexpectedly verbose tool cannot deadlock on a full pipe. */
static int scanToolRun(const char *cmd, char *out, size_t cap, bool firstLine,
                       scanToolControl *control)
{
    int fds[2], flags, eof = 0, childDone = 0, overflow = 0;
    int status = 0, result = -1;
    size_t off = 0, lineLen = 0;
    char line[512], chunk[1024];
    uint64_t started;
    pid_t pid, waited;

    if (!cmd || !out || cap == 0 || pipe(fds) != 0) return -1;
    out[0] = '\0';
    pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        int nullFd;
        (void)setpgid(0, 0);
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(126);
        close(fds[1]);
        nullFd = open("/dev/null", O_WRONLY);
        if (nullFd >= 0) { (void)dup2(nullFd, STDERR_FILENO); close(nullFd); }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    (void)setpgid(pid, pid);
    close(fds[1]);
    flags = fcntl(fds[0], F_GETFL, 0);
    if (flags < 0 || fcntl(fds[0], F_SETFL, flags | O_NONBLOCK) < 0) {
        scanStopToolGroup(pid);
        close(fds[0]);
        return -1;
    }
    started = scanMonotonicMs();

    while (!eof || !childDone) {
        struct pollfd pfd;
        int pollRc;

        if (control && control->progress) {
            solariStatus cancel = control->progress(control->userData,
                                                     control->hostsDone,
                                                     control->hostsFound);
            if (cancel != SOLARI_OK) {
                control->cancelStatus = cancel;
                scanStopToolGroup(pid);
                close(fds[0]);
                return -2;
            }
        }
        if (scanMonotonicMs() - started >= SCAN_TOOL_TIMEOUT_MS) {
            scanStopToolGroup(pid);
            close(fds[0]);
            return -1;
        }

        pfd.fd = fds[0];
        pfd.events = POLLIN | POLLHUP;
        pfd.revents = 0;
        pollRc = poll(&pfd, 1, 100);
        if (pollRc < 0 && errno != EINTR) break;
        if (pollRc > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
            size_t drained = 0;
            while (drained < 16u * 1024u) {
                ssize_t n = read(fds[0], chunk, sizeof chunk);
                size_t i;
                if (n == 0) { eof = 1; break; }
                if (n < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    eof = 1;
                    break;
                }
                drained += (size_t)n;
                for (i = 0; i < (size_t)n; i++) {
                    char c = chunk[i];
                    if (firstLine) {
                        if (c != '\n') {
                            if (lineLen + 1 < sizeof line) line[lineLen++] = c;
                            continue;
                        }
                        line[lineLen] = '\0';
                        {
                            char *begin = line, *end;
                            while (*begin == ' ' || *begin == '\t' || *begin == '\r') begin++;
                            end = begin + strlen(begin);
                            while (end > begin && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) *--end = '\0';
                            if (*begin) {
                                snprintf(out, cap, "%s", begin);
                                result = 0;
                                break;
                            }
                        }
                        lineLen = 0;
                    } else if (off + 1 < cap) {
                        out[off++] = c;
                        out[off] = '\0';
                    } else {
                        overflow = 1;
                    }
                }
                if (result == 0) break;
            }
            if (result == 0) {
                scanStopToolGroup(pid);
                close(fds[0]);
                return 0;
            }
        }
        waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid || (waited < 0 && errno == ECHILD)) childDone = 1;
    }

    if (!childDone) {
        scanStopToolGroup(pid);
    } else if (firstLine && lineLen > 0) {
        line[lineLen] = '\0';
        {
            char *begin = line, *end;
            while (*begin == ' ' || *begin == '\t' || *begin == '\r') begin++;
            end = begin + strlen(begin);
            while (end > begin && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) *--end = '\0';
            if (*begin) { snprintf(out, cap, "%s", begin); result = 0; }
        }
    } else if (!firstLine && off > 0 && !overflow) {
        result = 0;
    }
    close(fds[0]);
    return result;
}

static int scanToolLine(const char *cmd, char *out, size_t cap,
                        scanToolControl *control)
{
    return scanToolRun(cmd, out, cap, true, control);
}

static int scanToolAll(const char *cmd, char *out, size_t cap,
                       scanToolControl *control)
{
    return scanToolRun(cmd, out, cap, false, control);
}

static solariStatus scanToolCancellation(int toolRc, scanToolControl *control)
{
    if (toolRc == -2 && control) return control->cancelStatus;
    return SOLARI_OK;
}

static solariStatus scanEnrichSnmp(const char *ipSafe, char *nameOut, size_t nameCap,
                           char *descrOut, size_t descrCap,
                           scanToolControl *control)
{
    char cmd[256];
    int toolRc;
    if (nameOut && nameCap) nameOut[0] = '\0';
    if (descrOut && descrCap) descrOut[0] = '\0';
    snprintf(cmd, sizeof cmd,
             "snmpget -v2c -c public -t 1 -r 1 -Ovq %s SNMPv2-MIB::sysName.0 2>/dev/null",
             ipSafe);
    toolRc = scanToolLine(cmd, nameOut, nameCap, control);
    if (toolRc == -2) return scanToolCancellation(toolRc, control);
    if (nameOut && nameOut[0]) scanTrim(nameOut);

    snprintf(cmd, sizeof cmd,
             "snmpget -v2c -c public -t 1 -r 1 -Ovq %s .1.3.6.1.2.1.1.1.0 2>/dev/null",
             ipSafe);
    toolRc = scanToolLine(cmd, descrOut, descrCap, control);
    if (toolRc == -2) return scanToolCancellation(toolRc, control);
    if (descrOut && descrOut[0]) scanTrim(descrOut);
    return SOLARI_OK;
}

static solariStatus scanEnrichNmap(const char *ipSafe, const int *openPorts,
                                   int nopen, serverDiscEntity *e,
                                   scanToolControl *control)
{
    char cmd[512], ports[256], xml[32768], svc[256];
    size_t off = 0;
    int i;
    int toolRc;
    if (!openPorts || nopen <= 0) return SOLARI_OK;
    for (i = 0; i < nopen && off < sizeof ports - 8; i++) {
        int n = snprintf(ports + off, sizeof ports - off, "%s%d",
                         i ? "," : "", openPorts[i]);
        if (n <= 0 || (size_t)n >= sizeof ports - off) return SOLARI_OK;
        off += (size_t)n;
    }
    snprintf(cmd, sizeof cmd,
             "nmap -O -sV --version-light --osscan-limit --host-timeout 30s "
             "-Pn -p %s -oX - %s 2>/dev/null", ports, ipSafe);
    toolRc = scanToolAll(cmd, xml, sizeof xml, control);
    if (toolRc == -2) return scanToolCancellation(toolRc, control);
    if (toolRc != 0) return SOLARI_OK;
    discoverParseNmapXml(xml, e->mac, sizeof e->mac, e->vendor, sizeof e->vendor,
                         e->osName, sizeof e->osName, svc, sizeof svc);
    if (!e->sysDescr[0] && svc[0]) snprintf(e->sysDescr, sizeof e->sysDescr, "%s", svc);
    return SOLARI_OK;
}

static solariStatus scanEnrichArp(const char *ipSafe, serverDiscEntity *e,
                                  scanToolControl *control)
{
    char arp[32768];
    int toolRc;
    if (e->mac[0]) return SOLARI_OK;
    toolRc = scanToolAll("cat /proc/net/arp 2>/dev/null", arp, sizeof arp, control);
    if (toolRc == -2) return scanToolCancellation(toolRc, control);
    if (toolRc != 0) return SOLARI_OK;
    (void)discoverParseArpMac(arp, ipSafe, e->mac, sizeof e->mac);
    return SOLARI_OK;
}

static solariStatus scanEnrichMdns(const char *ipSafe, serverDiscEntity *e,
                           char *mdnsTypes, size_t mdnsCap,
                           scanToolControl *control)
{
    char cmd[256], line[512], browse[32768];
    const char *p;
    int toolRc;
    if (mdnsTypes && mdnsCap) mdnsTypes[0] = '\0';

    snprintf(cmd, sizeof cmd, "avahi-resolve -a %s 2>/dev/null", ipSafe);
    toolRc = scanToolLine(cmd, line, sizeof line, control);
    if (toolRc == -2) return scanToolCancellation(toolRc, control);
    if (toolRc == 0) {
        char addr[128], name[SOLARI_FQDN_MAX];
        addr[0] = name[0] = '\0';
        if (sscanf(line, "%127s %255s", addr, name) == 2 && strcmp(addr, ipSafe) == 0 && name[0])
            snprintf(e->host, sizeof e->host, "%s", name);
    }

    if (!mdnsTypes || mdnsCap == 0) return SOLARI_OK;
    toolRc = scanToolAll("avahi-browse -aprt 2>/dev/null", browse, sizeof browse, control);
    if (toolRc == -2) return scanToolCancellation(toolRc, control);
    if (toolRc != 0) return SOLARI_OK;
    p = browse;
    while ((p = strstr(p, ipSafe)) != NULL) {
        const char *lineStart = p;
        const char *lineEnd;
        const char *semi;
        while (lineStart > browse && lineStart[-1] != '\n') lineStart--;
        lineEnd = strchr(p, '\n');
        if (!lineEnd) lineEnd = p + strlen(p);
        semi = lineStart;
        while ((semi = memchr(semi, ';', (size_t)(lineEnd - semi))) != NULL) {
            const char *next = memchr(semi + 1, ';', (size_t)(lineEnd - semi - 1));
            size_t n;
            if (!next) break;
            if (memchr(semi + 1, '_', (size_t)(next - semi - 1))) {
                n = (size_t)(next - semi - 1);
                if (strlen(mdnsTypes) + n + 2 < mdnsCap) {
                    if (mdnsTypes[0]) strcat(mdnsTypes, ",");
                    strncat(mdnsTypes, semi + 1, n);
                }
                break;
            }
            semi = next;
        }
        p = lineEnd;
    }
    return SOLARI_OK;
}

static solariStatus scanEnrichEntity(serverDiscEntity *e,
                                     const int *openPorts, int nopen,
                                     scanToolControl *control)
{
    char ipSafe[128], snmpName[SOLARI_FQDN_MAX], snmpDescr[256], mdnsTypes[512];
    const char *oui;
    solariStatus rc;

    scanSanitize(e->ip, ipSafe, sizeof ipSafe);
    if (!ipSafe[0]) return SOLARI_OK;

    rc = scanEnrichNmap(ipSafe, openPorts, nopen, e, control);
    if (rc != SOLARI_OK) return rc;
    rc = scanEnrichArp(ipSafe, e, control);
    if (rc != SOLARI_OK) return rc;

    rc = scanEnrichSnmp(ipSafe, snmpName, sizeof snmpName,
                        snmpDescr, sizeof snmpDescr, control);
    if (rc != SOLARI_OK) return rc;
    if (!e->host[0] && snmpName[0]) snprintf(e->host, sizeof e->host, "%s", snmpName);
    if (snmpDescr[0]) snprintf(e->sysDescr, sizeof e->sysDescr, "%s", snmpDescr);

    rc = scanEnrichMdns(ipSafe, e, mdnsTypes, sizeof mdnsTypes, control);
    if (rc != SOLARI_OK) return rc;

    if (!e->vendor[0] && e->mac[0]) {
        oui = lookupOui(e->mac);
        if (oui && oui[0]) snprintf(e->vendor, sizeof e->vendor, "%s", oui);
    }
    snprintf(e->deviceRole, sizeof e->deviceRole, "%s",
             discoverInferRole(e->servicesJson, e->sysDescr, e->vendor, mdnsTypes));
    return SOLARI_OK;
}
#endif /* SOLARI_WITH_DISCOVERY_TOOLS */

/* ---- scan core ----------------------------------------------------------- */

/* Record one reachable host into `discovered`. openPorts[] lists its open ports
 * (nopen of them). Builds the services JSON, reverse-resolves the name, upserts. */
static solariStatus scanRecordHost(serverContext *ctx, uint32_t hostBE,
                                   const int *openPorts, int nopen, uint8_t prefix,
                                   serverScanProgressFn progress, void *userData,
                                   uint32_t hostsDone, uint32_t hostsFound)
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
#ifdef SOLARI_WITH_DISCOVERY_TOOLS
    scanToolControl toolControl;
#endif

    memset(&e, 0, sizeof e);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(hostBE);

    if (!inet_ntop(AF_INET, &sa.sin_addr, ipstr, sizeof ipstr)) return ERR_PLATFORM;

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
    toolControl.progress = progress;
    toolControl.userData = userData;
    toolControl.hostsDone = hostsDone;
    toolControl.hostsFound = hostsFound;
    toolControl.cancelStatus = SOLARI_OK;
    rc = scanEnrichEntity(&e, openPorts, nopen, &toolControl);
    if (rc != SOLARI_OK) return rc;
#else
    (void)progress;
    (void)userData;
    (void)hostsDone;
    (void)hostsFound;
#endif

    /* A scan may outlive the control-loop lease. Fence every write so a
     * successor's epoch prevents a former leader from recording stale data. */
    rc = serverDbUpsertDiscoveredFenced(ctx->db, &e, solariNowUnixMs(),
                                        ctx->nodeId, ctx->epoch, &discId);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_WARN, "scan: upsert %s failed: %s",
                   ipstr, solariStrError(rc));
        return rc;
    }
    solariLogf(SOLARI_LOG_INFO, "scan: discovered %s (%s) %d service(s) [disc=%llu]",
               ipstr, e.host[0] ? e.host : "no-rdns", nopen,
               (unsigned long long)discId);
    return SOLARI_OK;
}

solariStatus serverScanPlan(const char *cidr, const char *portsCsv,
                            uint32_t *hostsTotalOut)
{
    uint32_t baseHost = 0, nhosts = 0;
    int ports[SCAN_MAX_PORTS];
    solariStatus rc;
    if (!hostsTotalOut) return ERR_INVALID_ARG;
    *hostsTotalOut = 0;
    rc = scanParseCidr(cidr, &baseHost, &nhosts, NULL);
    if (rc != SOLARI_OK) return rc;
    if (scanParsePorts(portsCsv, ports) <= 0) return ERR_INVALID_ARG;
    *hostsTotalOut = nhosts;
    return SOLARI_OK;
}

static solariStatus scanCheckLease(serverContext *ctx)
{
    uint64_t holder = 0, epoch = 0;
    solariStatus rc;
    if (!ctx || !ctx->db || ctx->role != SRV_ACTIVE || !ctx->nodeId || !ctx->epoch)
        return ERR_LEASE_LOST;
    rc = serverDbLeaseRead(ctx->db, &holder, &epoch);
    if (rc != SOLARI_OK) return rc;
    return holder == ctx->nodeId && epoch == ctx->epoch ? SOLARI_OK : ERR_LEASE_LOST;
}

solariStatus serverScanRun(serverContext *ctx, const char *cidr,
                           const char *portsCsv, size_t *foundOut)
{
    return serverScanRunProgress(ctx, cidr, portsCsv, foundOut, NULL, NULL);
}

solariStatus serverScanRunProgress(serverContext *ctx, const char *cidr,
                                   const char *portsCsv, size_t *foundOut,
                                   serverScanProgressFn progress,
                                   void *userData)
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
    rc = scanCheckLease(ctx);
    if (rc != SOLARI_OK) return rc;

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

        rc = scanCheckLease(ctx);
        if (rc != SOLARI_OK) { free(fds); free(jobs); free(openMap); return rc; }

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
        if (progress) {
            uint32_t hostsDone = (uint32_t)((start + batch) / nports);
            if (hostsDone > nhosts) hostsDone = nhosts;
            rc = progress(userData, hostsDone, (uint32_t)found);
            if (rc != SOLARI_OK) { free(openMap); return rc; }
        }
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
                rc = scanCheckLease(ctx);
                if (rc == SOLARI_OK)
                    rc = scanRecordHost(ctx, baseHost + h, openPorts, nopen, prefix,
                                        progress, userData, nhosts, (uint32_t)found);
                if (rc != SOLARI_OK) { free(openMap); return rc; }
                found++;
                if (progress) {
                    rc = progress(userData, nhosts, (uint32_t)found);
                    if (rc != SOLARI_OK) { free(openMap); return rc; }
                }
            }
        }
    }

    if (progress) {
        rc = progress(userData, nhosts, (uint32_t)found);
        if (rc != SOLARI_OK) { free(openMap); return rc; }
    }

    free(openMap);
    solariLogf(SOLARI_LOG_INFO, "scan: complete, %zu host(s) recorded", found);
    if (foundOut) *foundOut = found;
    return SOLARI_OK;
}
