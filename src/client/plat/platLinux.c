/*
 * platLinux.c - the reference PAL implementation (sec 7.2).
 *
 * Linux-only: /proc + /sys + POSIX. Compiled when CMAKE_SYSTEM_NAME is Linux.
 * Covers ARM32/ARM64/x86/x86_64 uniformly - nothing here is arch-specific.
 * This is a Linux source file, so _GNU_SOURCE is acceptable to expose the
 * POSIX/GNU library calls used below; the code itself stays plain C99.
 */
#define _GNU_SOURCE

#include "../platOS.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <mntent.h>
#include <netdb.h>
#include <regex.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/utsname.h>

/* ---- small helpers -------------------------------------------------------- */

static void copyStr(char *out, size_t cap, const char *src)
{
    if (!cap) return;
    strncpy(out, src ? src : "", cap - 1);
    out[cap - 1] = '\0';
}

/* Sleep for a measurement window; never the wall clock for timing decisions. */
static void sampleSleep(long ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ---- identity & catalogue ------------------------------------------------- */

solariStatus platHostFqdn(char *out, size_t cap)
{
    char host[256];
    struct addrinfo hints, *res = NULL;

    if (!out || cap == 0) return ERR_INVALID_ARG;
    if (gethostname(host, sizeof host) != 0) return ERR_PLATFORM;
    host[sizeof host - 1] = '\0';

    /* Already qualified, or resolution unavailable -> use the short name. */
    if (strchr(host, '.')) { copyStr(out, cap, host); return SOLARI_OK; }

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags  = AI_CANONNAME;
    if (getaddrinfo(host, NULL, &hints, &res) == 0 && res && res->ai_canonname) {
        copyStr(out, cap, res->ai_canonname);
        freeaddrinfo(res);
        return SOLARI_OK;
    }
    if (res) freeaddrinfo(res);
    copyStr(out, cap, host);
    return SOLARI_OK;
}

solariStatus platOsName(char *out, size_t cap)
{
    struct utsname u;
    if (!out || cap == 0) return ERR_INVALID_ARG;
    if (uname(&u) != 0) return ERR_PLATFORM;
    snprintf(out, cap, "%s %s", u.sysname, u.release);
    return SOLARI_OK;
}

solariStatus platArch(char *out, size_t cap)
{
    struct utsname u;
    if (!out || cap == 0) return ERR_INVALID_ARG;
    if (uname(&u) != 0) return ERR_PLATFORM;
    copyStr(out, cap, u.machine);
    return SOLARI_OK;
}

solariStatus platCpuCount(uint8_t *out)
{
    long n;
    if (!out) return ERR_INVALID_ARG;
    n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) return ERR_PLATFORM;
    if (n > SOLARI_MAX_CORES) n = SOLARI_MAX_CORES;
    *out = (uint8_t)n;
    return SOLARI_OK;
}

/* ---- CPU load (per-core, /proc/stat deltas) ------------------------------- */

/* Read per-core busy/total jiffies from /proc/stat. Fills up to `cap` cores;
 * sets *n to how many "cpuN" lines were seen. Returns SOLARI_OK or ERR_PLATFORM. */
static solariStatus readCpuJiffies(uint64_t *busy, uint64_t *total,
                                   uint8_t cap, uint8_t *n)
{
    FILE *f = fopen("/proc/stat", "r");
    char line[512];
    uint8_t count = 0;
    if (!f) return ERR_PLATFORM;

    while (fgets(line, sizeof line, f)) {
        unsigned idx;
        unsigned long long v[8] = {0,0,0,0,0,0,0,0};
        if (strncmp(line, "cpu", 3) != 0 || !isdigit((unsigned char)line[3]))
            continue;                       /* skip the "cpu " aggregate line */
        if (sscanf(line, "cpu%u %llu %llu %llu %llu %llu %llu %llu %llu",
                   &idx, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) < 5)
            continue;
        if (count >= cap) { count++; continue; }   /* still count for *n */
        {
            uint64_t idle = v[3] + v[4];            /* idle + iowait        */
            uint64_t tot  = 0; int i;
            for (i = 0; i < 8; i++) tot += v[i];
            busy[count]  = tot - idle;
            total[count] = tot;
        }
        count++;
    }
    fclose(f);
    if (count == 0) return ERR_PLATFORM;
    *n = (uint8_t)(count > cap ? cap : count);
    return SOLARI_OK;
}

solariStatus platCpuLoad(uint32_t *loadMilli, uint8_t cap, uint8_t *coreCount)
{
    uint64_t b0[SOLARI_MAX_CORES], t0[SOLARI_MAX_CORES];
    uint64_t b1[SOLARI_MAX_CORES], t1[SOLARI_MAX_CORES];
    uint8_t n0 = 0, n1 = 0, i, use;
    solariStatus rc;

    if (!loadMilli || !coreCount || cap == 0) return ERR_INVALID_ARG;
    if (cap > SOLARI_MAX_CORES) cap = SOLARI_MAX_CORES;

    rc = readCpuJiffies(b0, t0, cap, &n0);
    if (rc != SOLARI_OK) return rc;
    sampleSleep(100);
    rc = readCpuJiffies(b1, t1, cap, &n1);
    if (rc != SOLARI_OK) return rc;

    use = n0 < n1 ? n0 : n1;
    for (i = 0; i < use; i++) {
        uint64_t db = b1[i] - b0[i];
        uint64_t dt = t1[i] - t0[i];
        loadMilli[i] = dt ? (uint32_t)((db * 1000u) / dt) : 0u;
    }
    *coreCount = use;
    return SOLARI_OK;
}

/* ---- memory (/proc/meminfo) ----------------------------------------------- */

solariStatus platMemInfo(uint64_t *ramUsedKb, uint64_t *ramTotalKb,
                         uint64_t *swapUsedKb, uint64_t *swapTotalKb)
{
    FILE *f;
    char key[64];
    unsigned long long val;
    uint64_t memTotal = 0, memAvail = 0, swapTotal = 0, swapFree = 0;
    int got = 0;

    if (!ramUsedKb || !ramTotalKb || !swapUsedKb || !swapTotalKb)
        return ERR_INVALID_ARG;
    f = fopen("/proc/meminfo", "r");
    if (!f) return ERR_PLATFORM;

    while (got < 4 && fscanf(f, "%63[^:]: %llu kB\n", key, &val) == 2) {
        if      (!strcmp(key, "MemTotal"))     { memTotal  = val; got++; }
        else if (!strcmp(key, "MemAvailable")) { memAvail  = val; got++; }
        else if (!strcmp(key, "SwapTotal"))    { swapTotal = val; got++; }
        else if (!strcmp(key, "SwapFree"))     { swapFree  = val; got++; }
    }
    fclose(f);
    if (memTotal == 0) return ERR_PLATFORM;

    *ramTotalKb  = memTotal;
    *ramUsedKb   = memAvail <= memTotal ? memTotal - memAvail : 0;
    *swapTotalKb = swapTotal;
    *swapUsedKb  = swapFree <= swapTotal ? swapTotal - swapFree : 0;
    return SOLARI_OK;
}

/* ---- disks (/proc/mounts + statvfs) --------------------------------------- */

static int isPseudoFs(const char *type)
{
    static const char *skip[] = {
        "proc","sysfs","cgroup","cgroup2","devtmpfs","devpts","mqueue","debugfs",
        "tracefs","securityfs","pstore","bpf","configfs","fusectl","hugetlbfs",
        "autofs","binfmt_misc","ramfs","nsfs","selinuxfs","overlay","squashfs", NULL
    };
    int i;
    for (i = 0; skip[i]; i++) if (!strcmp(type, skip[i])) return 1;
    return 0;
}

solariStatus platDiskFree(solariDiskEntry *disks, uint8_t cap, uint8_t *count)
{
    FILE *m;
    struct mntent *e;
    uint8_t n = 0;

    if (!disks || !count || cap == 0) return ERR_INVALID_ARG;
    m = setmntent("/proc/mounts", "r");
    if (!m) return ERR_PLATFORM;

    while (n < cap && (e = getmntent(m)) != NULL) {
        struct statvfs s;
        uint64_t bs;
        uint64_t totalKb;
        if (isPseudoFs(e->mnt_type)) continue;
        if (statvfs(e->mnt_dir, &s) != 0) continue;
        bs = s.f_frsize ? s.f_frsize : s.f_bsize;
        totalKb = ((uint64_t)s.f_blocks * bs) / 1024u;
        if (totalKb < 1024u) continue;          /* skip sub-MiB pseudo mounts */
        copyStr(disks[n].mount, sizeof disks[n].mount, e->mnt_dir);
        disks[n].totalKb = totalKb;
        disks[n].freeKb  = ((uint64_t)s.f_bavail * bs) / 1024u;
        n++;
    }
    endmntent(m);
    *count = n;
    return SOLARI_OK;
}

/* ---- network interfaces (/proc/net/dev deltas + /sys speed) --------------- */

struct ifSnap { char name[SOLARI_IFNAME_MAX]; uint64_t rx, tx; };

static uint8_t readNetDev(struct ifSnap *snap, uint8_t cap)
{
    FILE *f = fopen("/proc/net/dev", "r");
    char line[512];
    uint8_t n = 0;
    if (!f) return 0;
    while (n < cap && fgets(line, sizeof line, f)) {
        char *colon = strchr(line, ':');
        char *name; unsigned long long rx, tx, d;
        if (!colon) continue;                   /* header lines have no ':' */
        *colon = '\0';
        name = line; while (*name == ' ') name++;
        if (!strcmp(name, "lo")) continue;
        /* rx bytes is field 1 after ':'; tx bytes is field 9. */
        if (sscanf(colon + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &rx, &d, &d, &d, &d, &d, &d, &d, &tx) < 9)
            continue;
        copyStr(snap[n].name, sizeof snap[n].name, name);
        snap[n].rx = rx; snap[n].tx = tx;
        n++;
    }
    fclose(f);
    return n;
}

static uint64_t ifaceCapacityKbps(const char *name)
{
    char path[128];
    FILE *f;
    long mbps = 0;
    snprintf(path, sizeof path, "/sys/class/net/%s/speed", name);
    f = fopen(path, "r");
    if (!f) return 0;
    if (fscanf(f, "%ld", &mbps) != 1 || mbps < 0) mbps = 0;
    fclose(f);
    return (uint64_t)mbps * 1000u;              /* Mbps -> Kbps */
}

solariStatus platNetIfaces(solariIfaceEntry *ifaces, uint8_t cap, uint8_t *count)
{
    struct ifSnap a[SOLARI_MAX_IFACES], b[SOLARI_MAX_IFACES];
    uint8_t na, nb, n = 0, i;

    if (!ifaces || !count || cap == 0) return ERR_INVALID_ARG;
    if (cap > SOLARI_MAX_IFACES) cap = SOLARI_MAX_IFACES;

    na = readNetDev(a, cap);
    sampleSleep(100);
    nb = readNetDev(b, cap);
    if (na == 0) return ERR_PLATFORM;

    for (i = 0; i < na && n < cap; i++) {
        uint64_t drx = 0, dtx = 0; uint8_t j;
        for (j = 0; j < nb; j++) {
            if (strcmp(a[i].name, b[j].name) != 0) continue;
            drx = b[j].rx >= a[i].rx ? b[j].rx - a[i].rx : 0;
            dtx = b[j].tx >= a[i].tx ? b[j].tx - a[i].tx : 0;
            break;
        }
        copyStr(ifaces[n].name, sizeof ifaces[n].name, a[i].name);
        ifaces[n].rxKbps = (drx * 8u) / 100u;   /* bytes over 100ms -> Kbps */
        ifaces[n].txKbps = (dtx * 8u) / 100u;
        ifaces[n].capacityKbps = ifaceCapacityKbps(a[i].name);
        n++;
    }
    *count = n;
    return SOLARI_OK;
}

solariStatus platUsbThroughput(solariUsbEntry *bus, uint8_t cap, uint8_t *count)
{
    /* Per-bus USB throughput is an enhancement (sec 7.1); the reference
     * reports none rather than guessing. Honest empty set, not an error. */
    SOLARI_UNUSED(bus);
    SOLARI_UNUSED(cap);
    if (!count) return ERR_INVALID_ARG;
    *count = 0;
    return SOLARI_OK;
}

/* ---- process inspection (/proc/<pid>/...) --------------------------------- */

static int procCommMatches(const char *pidDir, const char *want)
{
    char path[300], comm[256];
    FILE *f;
    size_t len;
    snprintf(path, sizeof path, "/proc/%s/comm", pidDir);
    f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(comm, sizeof comm, f)) { fclose(f); return 0; }
    fclose(f);
    len = strlen(comm);
    if (len && comm[len - 1] == '\n') comm[len - 1] = '\0';
    return strcmp(comm, want) == 0;
}

static void procCountFds(const char *pidDir, uint32_t *nFiles, uint32_t *nSockets)
{
    char path[300], link[300], target[300];
    DIR *d;
    struct dirent *de;
    uint32_t files = 0, socks = 0;
    snprintf(path, sizeof path, "/proc/%s/fd", pidDir);
    d = opendir(path);
    if (!d) { *nFiles = 0; *nSockets = 0; return; }
    while ((de = readdir(d)) != NULL) {
        ssize_t r;
        if (de->d_name[0] == '.') continue;
        files++;
        snprintf(link, sizeof link, "/proc/%s/fd/%s", pidDir, de->d_name);
        r = readlink(link, target, sizeof target - 1);
        if (r > 0) {
            target[r] = '\0';
            if (strncmp(target, "socket:", 7) == 0) socks++;
        }
    }
    closedir(d);
    *nFiles = files; *nSockets = socks;
}

solariStatus platProcInspect(const char *procName, solariProcEntry *out)
{
    DIR *proc;
    struct dirent *de;
    char found[32] = "";

    if (!procName || !out) return ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    copyStr(out->name, sizeof out->name, procName);
    out->pid = -1;                              /* sentinel: not running */

    proc = opendir("/proc");
    if (!proc) return ERR_PLATFORM;
    while ((de = readdir(proc)) != NULL) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        if (procCommMatches(de->d_name, procName)) {
            copyStr(found, sizeof found, de->d_name);
            break;
        }
    }
    closedir(proc);
    if (found[0] == '\0') return SOLARI_OK;     /* watched but down */

    {
        char path[300], buf[1024];
        FILE *f;
        out->pid = (int32_t)strtol(found, NULL, 10);

        /* state + rss from /proc/<pid>/stat: "pid (comm) STATE ...".
         * rss is field 24 (1-based), in pages. Parse after the ')' to dodge
         * spaces/parens inside comm. */
        snprintf(path, sizeof path, "/proc/%s/stat", found);
        f = fopen(path, "r");
        if (f && fgets(buf, sizeof buf, f)) {
            char *rp = strrchr(buf, ')');
            if (rp) {
                char st = 0; long rssPages = 0; int fld;
                /* after ") " the fields are: state ppid ... ; rss is the
                 * 22nd field following the comm (i.e. overall field 24). */
                rp += 2;
                if (sscanf(rp, "%c", &st) == 1) out->state = (uint8_t)st;
                /* walk to the rss field (the 22nd whitespace token after state) */
                for (fld = 0; fld < 21 && rp; fld++) {
                    rp = strchr(rp, ' ');
                    if (rp) rp++;
                }
                if (rp) rssPages = strtol(rp, NULL, 10);
                out->rssKb = (uint64_t)(rssPages > 0 ? rssPages : 0)
                             * (uint64_t)(sysconf(_SC_PAGESIZE) / 1024);
            }
        }
        if (f) fclose(f);
        procCountFds(found, &out->nFiles, &out->nSockets);
    }
    return SOLARI_OK;
}

/* ---- listening-service identification (/proc/net/tcp{,6}) ----------------- */

/* Best-effort IANA-ish label for a listening port. */
static const char *platSvcName(uint16_t p)
{
    switch (p) {
    case 21: return "ftp";    case 22: return "ssh";   case 23: return "telnet";
    case 25: return "smtp";   case 53: return "dns";   case 80: return "http";
    case 110: return "pop3";  case 123: return "ntp";  case 143: return "imap";
    case 161: return "snmp";  case 389: return "ldap"; case 443: return "https";
    case 445: return "smb";   case 587: return "submission"; case 631: return "ipp";
    case 993: return "imaps"; case 995: return "pop3s"; case 1883: return "mqtt";
    case 3306: return "mysql"; case 3389: return "rdp"; case 5432: return "postgres";
    case 5900: return "vnc";  case 6379: return "redis"; case 8080: return "http-alt";
    case 8443: return "https-alt"; case 9090: return "http-mgmt"; case 9100: return "jetdirect";
    case 7701: return "solari-ingest"; case 7702: return "solari-control"; case 7703: return "solari-pub";
    default: return "tcp";
    }
}

solariStatus platListenServices(solariProcEntry *out, uint8_t cap, uint8_t *count)
{
    static const char *paths[2] = { "/proc/net/tcp", "/proc/net/tcp6" };
    uint16_t ports[64];
    int      np = 0, pi;
    uint8_t  n = 0;

    if (!out || !count || cap == 0) return ERR_INVALID_ARG;
    *count = 0;

    for (pi = 0; pi < 2; pi++) {
        FILE *f = fopen(paths[pi], "r");
        char  line[512];
        if (!f) continue;
        if (!fgets(line, sizeof line, f)) { fclose(f); continue; }  /* header */
        while (fgets(line, sizeof line, f) &&
               np < (int)(sizeof ports / sizeof ports[0])) {
            unsigned lport = 0, rport = 0, stt = 0;
            /* "sl: LOCALHEX:PORT REMHEX:PORT ST ..."; ST 0x0A == LISTEN. */
            if (sscanf(line, " %*d: %*[0-9A-Fa-f]:%x %*[0-9A-Fa-f]:%x %x",
                       &lport, &rport, &stt) == 3 && stt == 0x0A) {
                int k, dup = 0;
                for (k = 0; k < np; k++) if (ports[k] == (uint16_t)lport) { dup = 1; break; }
                if (!dup) ports[np++] = (uint16_t)lport;
            }
        }
        fclose(f);
    }

    for (pi = 0; pi < np && n < cap; pi++) {
        solariProcEntry *e = &out[n++];
        memset(e, 0, sizeof *e);
        snprintf(e->name, sizeof e->name, "%s:%u",
                 platSvcName(ports[pi]), (unsigned)ports[pi]);
        e->pid   = 0;
        e->state = 'L';   /* listening service (not a watched process) */
    }
    *count = n;
    return SOLARI_OK;
}

/* ---- log file stat -------------------------------------------------------- */

solariStatus platLogStat(const char *path, const char *regex,
                         uint64_t *sizeNow, uint32_t *matchCount,
                         uint64_t *lastOffsetInOut)
{
    struct stat st;
    FILE *f;
    regex_t re;
    int haveRe = 0;
    uint32_t matches = 0;
    char line[2048];

    if (!path || !sizeNow || !matchCount || !lastOffsetInOut)
        return ERR_INVALID_ARG;
    if (stat(path, &st) != 0) return ERR_PLATFORM;
    *sizeNow = (uint64_t)st.st_size;

    /* rotation/truncation -> re-scan from the top */
    if (*lastOffsetInOut > (uint64_t)st.st_size) *lastOffsetInOut = 0;

    if ((uint64_t)st.st_size == *lastOffsetInOut) { *matchCount = 0; return SOLARI_OK; }

    if (regex && *regex) {
        if (regcomp(&re, regex, REG_EXTENDED | REG_NOSUB) != 0) return ERR_INVALID_ARG;
        haveRe = 1;
    }
    f = fopen(path, "r");
    if (!f) { if (haveRe) regfree(&re); return ERR_PLATFORM; }
    if (fseeko(f, (off_t)*lastOffsetInOut, SEEK_SET) != 0) {
        fclose(f); if (haveRe) regfree(&re); return ERR_PLATFORM;
    }
    while (fgets(line, sizeof line, f)) {
        if (!haveRe || regexec(&re, line, 0, NULL, 0) == 0) matches++;
    }
    fclose(f);
    if (haveRe) regfree(&re);

    *matchCount = matches;
    *lastOffsetInOut = (uint64_t)st.st_size;
    return SOLARI_OK;
}

/* ---- discovery & topology ------------------------------------------------- */

solariStatus platArpNeighbors(platNeighbor *out, uint8_t cap, uint8_t *count)
{
    FILE *f;
    char line[256];
    uint8_t n = 0;

    if (!out || !count || cap == 0) return ERR_INVALID_ARG;
    f = fopen("/proc/net/arp", "r");
    if (!f) return ERR_PLATFORM;
    if (!fgets(line, sizeof line, f)) { fclose(f); *count = 0; return SOLARI_OK; } /* header */

    while (n < cap && fgets(line, sizeof line, f)) {
        char ip[46], hwtype[16], flags[16], mac[18], mask[16], dev[SOLARI_IFNAME_MAX];
        if (sscanf(line, "%45s %15s %15s %17s %15s %31s",
                   ip, hwtype, flags, mac, mask, dev) < 6) continue;
        if (!strcmp(flags, "0x0")) continue;             /* incomplete entry */
        if (!strcmp(mac, "00:00:00:00:00:00")) continue;
        copyStr(out[n].ip,    sizeof out[n].ip,    ip);
        copyStr(out[n].mac,   sizeof out[n].mac,   mac);
        copyStr(out[n].iface, sizeof out[n].iface, dev);
        n++;
    }
    fclose(f);
    *count = n;
    return SOLARI_OK;
}

solariStatus platIfaceCidrs(platIfaceCidr *out, uint8_t cap, uint8_t *count)
{
    struct ifaddrs *ifa = NULL, *p;
    uint8_t n = 0;

    if (!out || !count || cap == 0) return ERR_INVALID_ARG;
    if (getifaddrs(&ifa) != 0) return ERR_PLATFORM;

    for (p = ifa; p && n < cap; p = p->ifa_next) {
        struct sockaddr_in *addr, *mask;
        uint32_t a, m, net, mm;
        int prefix = 0;
        char netstr[INET_ADDRSTRLEN];
        struct in_addr na;
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (!p->ifa_netmask) continue;
        if (!strcmp(p->ifa_name, "lo")) continue;
        addr = (struct sockaddr_in *)(void *)p->ifa_addr;
        mask = (struct sockaddr_in *)(void *)p->ifa_netmask;
        a = ntohl(addr->sin_addr.s_addr);
        m = ntohl(mask->sin_addr.s_addr);
        net = a & m;
        for (mm = m; mm; mm >>= 1) prefix += (int)(mm & 1u);
        na.s_addr = htonl(net);
        if (!inet_ntop(AF_INET, &na, netstr, sizeof netstr)) continue;
        copyStr(out[n].ifName, sizeof out[n].ifName, p->ifa_name);
        snprintf(out[n].cidr, sizeof out[n].cidr, "%s/%d", netstr, prefix);
        out[n].speedMbps = ifaceCapacityKbps(p->ifa_name) / 1000u;
        n++;
    }
    freeifaddrs(ifa);
    *count = n;
    return SOLARI_OK;
}

solariStatus platDefaultUplink(platUplink *out)
{
    FILE *f;
    char line[256];
    int found = 0;

    if (!out) return ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    f = fopen("/proc/net/route", "r");
    if (!f) return ERR_PLATFORM;
    if (!fgets(line, sizeof line, f)) { fclose(f); return ERR_PLATFORM; }  /* header */

    while (fgets(line, sizeof line, f)) {
        char iface[SOLARI_IFNAME_MAX];
        unsigned long dest = 0, gw = 0;
        struct in_addr a;
        if (sscanf(line, "%31s %lx %lx", iface, &dest, &gw) < 3) continue;
        if (dest != 0) continue;                         /* default route only */
        a.s_addr = (in_addr_t)gw;                        /* /proc value is net-order */
        copyStr(out->localIf, sizeof out->localIf, iface);
        inet_ntop(AF_INET, &a, out->gatewayIp, sizeof out->gatewayIp);
        out->speedMbps = ifaceCapacityKbps(iface) / 1000u;
        found = 1;
        break;
    }
    fclose(f);
    return found ? SOLARI_OK : ERR_PLATFORM;
}

/* ---- watchdog support ----------------------------------------------------- */

solariStatus platSpawnSelf(const char *argv0, int argc, char *const argv[])
{
    pid_t pid;
    SOLARI_UNUSED(argc);
    if (!argv0 || !argv) return ERR_INVALID_ARG;
    pid = fork();
    if (pid < 0) return ERR_PLATFORM;
    if (pid == 0) {                             /* child: become the new agent */
        execv(argv0, argv);
        _exit(127);                             /* exec failed */
    }
    return SOLARI_OK;                           /* parent */
}

solariStatus platProcessAlive(int64_t pid, bool *alive)
{
    if (!alive || pid <= 0) return ERR_INVALID_ARG;
    if (kill((pid_t)pid, 0) == 0)      { *alive = true;  return SOLARI_OK; }
    if (errno == EPERM)                { *alive = true;  return SOLARI_OK; }
    *alive = false;                    /* ESRCH: gone */
    return SOLARI_OK;
}
