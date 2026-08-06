/*
 * solariCtl.c - operator bridge over a Unix domain socket (§9.1, §11.1).
 *
 * The privileged control helper, hosted inside solariServer (a module, not a
 * separate binary): listens on an AF_UNIX socket for the PHP/MCP layer, holds
 * the internal CA/cert material (kept out of the web tier), validates inbound
 * operator requests, enforces operator RBAC + an explicit one-time confirm for
 * destructive ops, and re-emits the corresponding SCP control frames through
 * the serverContext by delegating to the serverProvision/serverControl APIs.
 * It is also the only place the CA private key is touched, so CSR signing for
 * enrollment funnels through serverCtlSignCsr.
 *
 * The PHP dashboard never holds a server certificate (§11.1); it speaks a tiny,
 * line-oriented request protocol to this socket and the bridge translates each
 * line into an authenticated control action. The protocol is deliberately
 * simple so the read path stays a thin PDO/HTTP layer:
 *
 *   request : VERB [k=v ...]\n        (one request per line, '\n' terminated)
 *   reply   : OK [k=v ...]\n  |  ERR <code> <message>\n
 *
 * Recognised verbs and their required keys:
 *   PING                                           -> liveness
 *   DISCOVER cidr=<cidr> [ports=<csv>]              -> active scan
 *   SURVEY                                         -> broadcast SCP_MSG_SURVEY
 *   APPROVE   enr=<id> [op=<operator>]              -> approve enrollment
 *   REJECT    enr=<id> [op=<operator>]              -> reject enrollment
 *   PROVISION node=<id> [build=<id>] [epoch=<n>] [cfg=<json>]
 *   SIGN* csr=<pem> op=<operator>                   -> sign CSR
 *   DEPLOY* host=<host> op=<operator> [server=] [arch=] [fqdn=]
 *   FLEET_PROVISION* target=<mac|host> distro=<id> arch=<id> hostname=<name> op=<operator> [...]
 *   FLEET_IMAGE* hostname=<name> arch=<id> op=<operator> [distro=] [...]
 *   ADOPT     disc=<id> [pool=<id>] [heartbeat=0|1] [name=] [class=] [tags=] [notes=] [services=]
 *   IGNORE    disc=<id>                             -> suppress discovered row
 *   CONTROL   node=<id> verb=<n> [epoch=<n>] [payload=<blob>]
 *   ASSET_SET ip=<ip> [name=] [class=] [pool=<id>] [tags=] [notes=] [heartbeat=0|1]
 *   ASSET_REMOVE* (asset=<id>|ip=<ip>) op=<operator>
 *   TARGET_REMOVE* target=<targetId> op=<operator>
 *   POOL_NEW name=<name> [desc=] [color=]
 *   POOL_SET pool=<id> [name=] [desc=] [color=]
 *   POOL_DEL* pool=<id> op=<operator>
 *   CONFIG_SET cfg=<json> [op=<operator>]
 *   RULE_SET rule=<id> [enabled=] [threshold=] [forSeconds=] [op=] [severity=] [metric=] [scope=]
 *   RULE_DEL* rule=<id> op=<operator>
 *   ALERT_ACK event=<id> op=<operator>
 *   DECOMMISSION* node=<id> scope=<hex> op=<operator> [confirm=<token>]
 *   RETIRE* node=<id> op=<operator>
 *
 * A trailing '*' marks destructive/privileged verbs that require op=. The
 * destructive catalog is enforced by ctlVerbIsDestructive(); ALERT_ACK also
 * requires op= for attribution but is not classed as destructive.
 *
 * Destructive verbs (DECOMMISSION, RETIRE, SIGN, DEPLOY, FLEET_PROVISION,
 * FLEET_IMAGE, ASSET_REMOVE, TARGET_REMOVE, POOL_DEL, RULE_DEL) require two
 * things: an operator
 * identity (RBAC: the caller must name who is authorizing) and, for the
 * irreversible second step, an explicit `confirm=<token>` echoing the one-time
 * token the first DECOMMISSION call issued (§7.3, §11). A first DECOMMISSION
 * call with no confirm token replies "OK confirm=<token>" without retiring the
 * node; the operator must re-POST with that token to finalize.
 *
 * Design note (testability, §17): the request parser, the per-verb argument
 * extraction, and the reply formatting are all pure static helpers over
 * caller-owned buffers (no I/O, no globals), so the full RBAC + confirm
 * state-machine can be exercised without a live socket, a live MariaDB, or a CA
 * key. The public entry points layer the AF_UNIX I/O + serverProvision/Control
 * dispatch on top of those helpers.
 *
 * CA / CSR signing: the real signer is mbedTLS x509 (the I/O layer's TLS dep,
 * §3). Where the mbedTLS x509 writer headers are present at build time the
 * signing path is compiled in (guarded by SOLARI_HAVE_MBEDTLS_X509); where they
 * are not (e.g. a core-only build with no vendored mbedTLS x509), the CSR is
 * still structurally validated and serverCtlSignCsr returns ERR_TLS so callers
 * fail closed rather than minting an unsigned cert. See the CONTRACT GAPS note
 * at the file footer.
 */
#include "server.h"
#include "serverScan.h"
#include "serverAssets.h"

#include "solari/solariCrypto.h"
#include "solari/solariError.h"
#include "solari/solariLog.h"
#include "solari/solariNet.h"
#include "solari/solariTime.h"
#include "solari/solariTlv.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* Optional mbedTLS x509 CA backend. Vendored only with the full I/O layer; the
 * top-level CMake brings mbedTLS transitively when third_party/mbedtls exists.
 * When it is absent the signing path degrades to fail-closed (ERR_TLS). */
#if defined(SOLARI_HAVE_MBEDTLS_X509)
#  include <mbedtls/x509_csr.h>
#  include <mbedtls/x509_crt.h>
#  include <mbedtls/pk.h>
#  include <mbedtls/ctr_drbg.h>
#  include <mbedtls/entropy.h>
#  include <mbedtls/base64.h>
#  include <mbedtls/version.h>
#endif

/* Largest single operator request line we will read in one poll. The PHP layer
 * frames one request per line; anything larger is a malformed/abusive caller. */
#define CTL_REQ_CAP   4096
/* Largest reply line we assemble. */
#define CTL_REPLY_CAP 16384   /* large enough to return a URL-encoded signed cert */
/* Max accepted-but-unserviced backlog on the listening socket. */
#define CTL_LISTEN_BACKLOG 8
/* Cap on a held CSR / issued cert PEM threaded through the signer. */
#define CTL_PEM_CAP 8192

/* Opaque handle: the listening AF_UNIX socket fd, the owning context, the bound
 * socket path (so close can unlink it), and the CA material used to sign CSRs.
 * The CA paths are copied from cfg so the handle is self-contained and the web
 * tier never sees them. */
struct serverCtl {
    serverContext *ctx;
    int            listenFd;                 /* -1 until bound                  */
    char           sockPath[SERVER_PATH_MAX];/* for unlink on close            */
    char           caFile[SERVER_PATH_MAX];  /* CA cert that issues leaf certs  */
    char           caKeyFile[SERVER_PATH_MAX];/* CA private key (signing)        */
    char           caMode[8];                /* "local" | "remote"              */
    char           caUrl[SERVER_URL_MAX];    /* remote CA endpoint (caMode=remote) */
    bool           bound;                    /* listenFd is a real bound socket */
};

/* ===================================================================== */
/* Pure helpers (no I/O) - unit-testable in isolation                     */
/* ===================================================================== */

/* Trim a trailing CR/LF and surrounding spaces in place. Returns the new
 * length. Pure. */
static size_t ctlTrimLine(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' '  || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
    return n;
}

/* Find the value for `key` in a space-separated "k=v k=v" argument string and
 * copy it (NUL-terminated) into out. Values may not contain spaces in this
 * minimal protocol (the PHP layer URL-encodes anything richer before sending).
 * Returns SOLARI_OK if found, ERR_TLV_END if the key is absent, or
 * ERR_BUFFER_FULL if the value does not fit. Pure. */
/* In-place percent-decode (%XX -> byte). A malformed %XX is left untouched. The
 * operator-bridge wire URL-encodes any value containing non-token bytes (JSON
 * config blobs carry '{', '"', ':', etc.); this reverses that so handlers see
 * the raw value. Bare tokens (ids, hex, names) contain no '%' and pass through. */
static void ctlPctDecode(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (r[0] == '%' && isxdigit((unsigned char)r[1]) &&
            isxdigit((unsigned char)r[2])) {
            int hi = isdigit((unsigned char)r[1]) ? r[1] - '0'
                       : (tolower((unsigned char)r[1]) - 'a' + 10);
            int lo = isdigit((unsigned char)r[2]) ? r[2] - '0'
                       : (tolower((unsigned char)r[2]) - 'a' + 10);
            *w++ = (char)((hi << 4) | lo);
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* URL-encode `in` into `out` (cap incl. NUL): keep the bare-token set, %XX the
 * rest. Used to return a signed cert PEM (newlines/+/=) as a single reply line.
 * Returns the bytes written (excluding NUL), or 0 if it would overflow. */
static size_t ctlPctEncode(const char *in, char *out, size_t cap)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (; in && *in; in++) {
        unsigned char c = (unsigned char)*in;
        int bare = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                   c == ':' || c == '/' || c == '@' || c == '-';
        if (bare) {
            if (o + 1 >= cap) return 0;
            out[o++] = (char)c;
        } else {
            if (o + 3 >= cap) return 0;
            out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 0xF];
        }
    }
    if (o >= cap) return 0;
    out[o] = '\0';
    return o;
}

static solariStatus ctlArgStr(const char *args, const char *key,
                              char *out, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = args;

    if (out && cap) out[0] = '\0';
    if (!args || !key || !out || cap == 0) return ERR_INVALID_ARG;

    while (*p) {
        const char *eq, *end;
        /* skip leading separators */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        eq = strchr(p, '=');
        end = p;
        while (*end && *end != ' ' && *end != '\t') end++;
        if (eq && eq < end &&
            (size_t)(eq - p) == klen && strncmp(p, key, klen) == 0) {
            size_t vlen = (size_t)(end - (eq + 1));
            if (vlen + 1 > cap) return ERR_BUFFER_FULL;
            memcpy(out, eq + 1, vlen);
            out[vlen] = '\0';
            ctlPctDecode(out);          /* reverse the wire's URL-encoding */
            return SOLARI_OK;
        }
        p = end;
    }
    return ERR_TLV_END;
}

/* Extract an unsigned 64-bit argument. Missing key -> *out unchanged, returns
 * ERR_TLV_END; present-but-nonnumeric -> ERR_INVALID_ARG. Pure. */
static solariStatus ctlArgU64(const char *args, const char *key, uint64_t *out)
{
    char buf[32];
    solariStatus st = ctlArgStr(args, key, buf, sizeof buf);
    char *endp = NULL;
    unsigned long long v;
    if (st != SOLARI_OK) return st;
    if (buf[0] == '\0') return ERR_INVALID_ARG;
    errno = 0;
    v = strtoull(buf, &endp, 10);
    if (errno != 0 || !endp || *endp != '\0') return ERR_INVALID_ARG;
    if (out) *out = (uint64_t)v;
    return SOLARI_OK;
}

/* Extract a hex (0x-prefixed or bare) 32-bit argument (used for wipe scope).
 * Same return discipline as ctlArgU64. Pure. */
static solariStatus ctlArgU32Hex(const char *args, const char *key, uint32_t *out)
{
    char buf[32];
    solariStatus st = ctlArgStr(args, key, buf, sizeof buf);
    char *endp = NULL;
    unsigned long v;
    const char *s;
    if (st != SOLARI_OK) return st;
    if (buf[0] == '\0') return ERR_INVALID_ARG;
    s = buf;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    errno = 0;
    v = strtoul(s, &endp, 16);
    if (errno != 0 || !endp || *endp != '\0') return ERR_INVALID_ARG;
    if (out) *out = (uint32_t)v;
    return SOLARI_OK;
}

/* Split a request line into VERB (copied to verb/verbCap) and a pointer to the
 * remaining argument substring (into the caller's line buffer; not copied).
 * Returns SOLARI_OK, or ERR_INVALID_ARG on an empty line. Pure. */
static solariStatus ctlSplitVerb(char *line, char *verb, size_t verbCap,
                                 const char **argsOut)
{
    char *p = line;
    char *v = verb;
    if (verbCap) verb[0] = '\0';
    if (argsOut) *argsOut = "";
    if (!line || !verb || verbCap == 0) return ERR_INVALID_ARG;

    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return ERR_INVALID_ARG;
    while (*p && *p != ' ' && *p != '\t') {
        if ((size_t)(v - verb) + 1 >= verbCap) return ERR_BUFFER_FULL;
        *v++ = *p++;
    }
    *v = '\0';
    while (*p == ' ' || *p == '\t') p++;
    if (argsOut) *argsOut = p;
    return SOLARI_OK;
}

/* Format an "ERR <code> <message>\n" reply into out. Pure. */
static size_t ctlReplyErr(char *out, size_t cap, solariStatus code,
                          const char *message)
{
    int n = snprintf(out, cap, "ERR %d %s\n", (int)code,
                     message ? message : solariStrError(code));
    if (n < 0) { if (cap) out[0] = '\0'; return 0; }
    if ((size_t)n >= cap) n = (int)cap - 1;   /* truncated but NUL-terminated */
    return (size_t)n;
}

/* Format an "OK [extra]\n" reply into out. `extra` may be NULL/"" for a bare
 * acknowledgement. Pure. */
static size_t ctlReplyOk(char *out, size_t cap, const char *extra)
{
    int n;
    if (extra && extra[0])
        n = snprintf(out, cap, "OK %s\n", extra);
    else
        n = snprintf(out, cap, "OK\n");
    if (n < 0) { if (cap) out[0] = '\0'; return 0; }
    if ((size_t)n >= cap) n = (int)cap - 1;
    return (size_t)n;
}

/* True if a verb is destructive and thus subject to the RBAC + confirm gate.
 * Pure. */
static bool ctlVerbIsDestructive(const char *verb)
{
    /* SIGN mints a trusted certificate and DEPLOY runs a remote install — both
     * privileged, so they require a named operator like the teardown verbs.
     * FLEET_PROVISION stages a bare-metal OS install and FLEET_IMAGE builds a
     * bootable image; both mint enrollment certs, so they are gated the same. */
    return strcmp(verb, "DECOMMISSION") == 0 || strcmp(verb, "RETIRE") == 0 ||
           strcmp(verb, "SIGN") == 0 || strcmp(verb, "DEPLOY") == 0 ||
           strcmp(verb, "FLEET_PROVISION") == 0 || strcmp(verb, "FLEET_IMAGE") == 0 ||
           strcmp(verb, "ASSET_REMOVE") == 0 || strcmp(verb, "TARGET_REMOVE") == 0 ||
           strcmp(verb, "LIFECYCLE_SET") == 0 || strcmp(verb, "ASSET_PURGE") == 0 ||
           strcmp(verb, "POOL_DEL") == 0 || strcmp(verb, "RULE_DEL") == 0;
}

static bool ctlVerbRequiresOperator(const char *verb)
{
    return ctlVerbIsDestructive(verb) || strcmp(verb, "ALERT_ACK") == 0 ||
           strcmp(verb, "CRIT_SET") == 0;
}

static bool ctlPoolCanDelete(uint64_t poolId)
{
    return poolId != 0 && poolId != 1;
}

/* Sanitize an arbitrary string into a safe filename fragment ([A-Za-z0-9._-]). */
static void ctlSanitizeName(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; in && *in && o + 1 < cap; in++) {
        char c = *in;
        out[o++] = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
                       ? c : '_';
    }
    out[o] = '\0';
}

/* Spawn remote-deploy.sh fully detached (double-fork + setsid), with stdout/err
 * redirected to logPath. Returns once the deploy has been launched — the server
 * loop never blocks on the (multi-minute) deploy. argv must be NULL-terminated. */
static solariStatus ctlSpawnDetached(char *const argv[], const char *logPath)
{
    pid_t pid = fork();
    if (pid < 0) return ERR_PLATFORM;
    if (pid == 0) {
        pid_t g = fork();
        if (g < 0) _exit(127);
        if (g > 0) _exit(0);                 /* intermediate exits immediately */
        setsid();                            /* detach from the server */
        {
            int fd = open(logPath, O_WRONLY | O_CREAT | O_TRUNC, 0640);
            if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
        }
        execv(argv[0], argv);                /* no shell — args passed literally */
        _exit(127);
    }
    {
        int status;
        (void)waitpid(pid, &status, 0);      /* reap the intermediate; grandchild -> init */
    }
    return SOLARI_OK;
}

/* RBAC gate for destructive verbs: every destructive request must name the
 * authorizing operator (op=...). The PHP tier authenticates the human and adds
 * this field; an empty/absent operator is refused. Pure. */
static solariStatus ctlCheckRbac(const char *verb, const char *args,
                                 char *operatorOut, size_t cap)
{
    solariStatus st;
    if (operatorOut && cap) operatorOut[0] = '\0';
    if (!ctlVerbRequiresOperator(verb)) return SOLARI_OK;
    st = ctlArgStr(args, "op", operatorOut, cap);
    if (st != SOLARI_OK || operatorOut[0] == '\0') {
        return ERR_AUTH_ROLE;   /* nearest "not authorized" code */
    }
    return SOLARI_OK;
}

/* Validate that a blob is a PEM CSR by its delimiters. This is the structural
 * check used both before signing and as the fail-closed fallback when no x509
 * backend is compiled in. Pure. */
static bool ctlLooksLikeCsr(const char *pem)
{
    if (!pem) return false;
    return strstr(pem, "-----BEGIN CERTIFICATE REQUEST-----") != NULL &&
           strstr(pem, "-----END CERTIFICATE REQUEST-----")   != NULL;
}

/* ===================================================================== */
/* Request dispatch (pure decision + delegated effects)                   */
/* ===================================================================== */

/* Handle one parsed request line, writing a reply into replyOut. All effects
 * are delegated to the serverProvision/serverControl/serverDiscovery APIs
 * through ctl->ctx; this function owns only argument extraction, the RBAC +
 * confirm gate, and reply formatting. Returns the number of reply bytes.
 *
 * Not marked pure: it calls into the dispatch APIs (which touch the DB and the
 * control conn). The parsing/RBAC layers above ARE pure and independently
 * testable. */
static size_t ctlHandleLine(serverCtl *ctl, char *line,
                            char *replyOut, size_t replyCap)
{
    char verb[32];
    char operator_[64];
    const char *args = "";
    solariStatus st;

    if (ctlTrimLine(line) == 0)
        return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "empty request");

    st = ctlSplitVerb(line, verb, sizeof verb, &args);
    if (st != SOLARI_OK)
        return ctlReplyErr(replyOut, replyCap, st, "malformed request");

    /* RBAC gate first so a destructive verb with no operator never reaches the
     * effecting APIs. */
    if ((st = ctlCheckRbac(verb, args, operator_, sizeof operator_)) != SOLARI_OK) {
        solariLogf(SOLARI_LOG_WARN,
                   "ctl: RBAC refused destructive verb '%s' (no operator)", verb);
        return ctlReplyErr(replyOut, replyCap, st, "operator required");
    }

    /* ---- liveness ---- */
    if (strcmp(verb, "PING") == 0) {
        return ctlReplyOk(replyOut, replyCap, "pong");
    }

    /* ---- active discovery scan (non-destructive) ---- */
    if (strcmp(verb, "DISCOVER") == 0) {
        char cidr[96], ports[256], extra[64];
        size_t found = 0;
        if (ctlArgStr(args, "cidr", cidr, sizeof cidr) != SOLARI_OK || !cidr[0])
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "cidr required");
        if (ctlArgStr(args, "ports", ports, sizeof ports) != SOLARI_OK) ports[0] = '\0';
        st = serverScanRun(ctl->ctx, cidr, ports[0] ? ports : NULL, &found);
        if (st != SOLARI_OK)
            return ctlReplyErr(replyOut, replyCap, st, "scan failed");
        (void)snprintf(extra, sizeof extra, "found=%zu", found);
        return ctlReplyOk(replyOut, replyCap, extra);
    }

    /* ---- sign a CSR with the internal CA (operator-gated) ---- */
    if (strcmp(verb, "SIGN") == 0) {
        char  *csr   = (char *)malloc(CTL_REQ_CAP);
        char  *cert  = (char *)malloc(8192);
        char  *extra = (char *)malloc(CTL_REPLY_CAP);
        size_t ret;
        if (!csr || !cert || !extra) {
            free(csr); free(cert); free(extra);
            return ctlReplyErr(replyOut, replyCap, ERR_PLATFORM, "oom");
        }
        if (ctlArgStr(args, "csr", csr, CTL_REQ_CAP) != SOLARI_OK || !csr[0]) {
            free(csr); free(cert); free(extra);
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "csr required");
        }
        st = serverCtlSignCsr(ctl, csr, cert, 8192);
        if (st == SOLARI_OK) {
            size_t n = (size_t)snprintf(extra, CTL_REPLY_CAP, "cert=");
            if (ctlPctEncode(cert, extra + n, CTL_REPLY_CAP - n) == 0) st = ERR_BUFFER_FULL;
        }
        ret = (st == SOLARI_OK) ? ctlReplyOk(replyOut, replyCap, extra)
                                : ctlReplyErr(replyOut, replyCap, st, "sign failed");
        free(csr); free(cert); free(extra);
        return ret;
    }

    /* ---- deploy the client to a remote host (operator-gated, detached) ---- */
    if (strcmp(verb, "DEPLOY") == 0) {
        char host[128], server[256], arch[16], fqdn[160], sanit[160], logpath[256], extra[300];
        char *argv[16];
        int   n = 0;
        if (ctlArgStr(args, "host", host, sizeof host) != SOLARI_OK || !host[0])
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "host required");
        if (ctlArgStr(args, "server", server, sizeof server) != SOLARI_OK) server[0] = '\0';
        if (ctlArgStr(args, "arch",   arch,   sizeof arch)   != SOLARI_OK) arch[0]   = '\0';
        if (ctlArgStr(args, "fqdn",   fqdn,   sizeof fqdn)   != SOLARI_OK) fqdn[0]   = '\0';
        ctlSanitizeName(host, sanit, sizeof sanit);
        (void)snprintf(logpath, sizeof logpath, "run/deploy-%s.log", sanit);
        argv[n++] = (char *)"deploy/remote-deploy.sh";
        argv[n++] = (char *)"--host";   argv[n++] = host;
        if (server[0]) { argv[n++] = (char *)"--server"; argv[n++] = server; }
        if (arch[0])   { argv[n++] = (char *)"--arch";   argv[n++] = arch; }
        if (fqdn[0])   { argv[n++] = (char *)"--fqdn";   argv[n++] = fqdn; }
        argv[n++] = (char *)"--op"; argv[n++] = operator_;
        argv[n]   = NULL;
        st = ctlSpawnDetached(argv, logpath);
        if (st != SOLARI_OK)
            return ctlReplyErr(replyOut, replyCap, st, "deploy spawn failed");
        solariLogf(SOLARI_LOG_INFO, "ctl: deploy launched host=%s op=%s -> %s",
                   host, operator_, logpath);
        (void)snprintf(extra, sizeof extra, "log=%s status=deploying", logpath);
        return ctlReplyOk(replyOut, replyCap, extra);
    }

    /* ---- fleet provisioning: stage a bare-metal OS install (detached) ----
     * Renders the unattended-install config + per-MAC netboot entry on the
     * provisioning host and mints the node's enrollment cert. The heavy lifting
     * (ssh to benzene, rsync trees, cert signing) lives in the forked script so
     * the server loop never blocks. */
    if (strcmp(verb, "FLEET_PROVISION") == 0) {
        char target[128], distro[24], arch[16], profile[48], hostname[160];
        char role[32], server[256], ip[64], sanit[160], logpath[256], extra[300];
        char *argv[28];
        int   n = 0;
        if (ctlArgStr(args, "target", target, sizeof target) != SOLARI_OK || !target[0])
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "target required");
        if (ctlArgStr(args, "distro", distro, sizeof distro) != SOLARI_OK || !distro[0])
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "distro required");
        if (ctlArgStr(args, "arch", arch, sizeof arch) != SOLARI_OK || !arch[0])
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "arch required");
        if (ctlArgStr(args, "hostname", hostname, sizeof hostname) != SOLARI_OK || !hostname[0])
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "hostname required");
        if (ctlArgStr(args, "profile", profile, sizeof profile) != SOLARI_OK) profile[0] = '\0';
        if (ctlArgStr(args, "role", role, sizeof role) != SOLARI_OK) role[0] = '\0';
        if (ctlArgStr(args, "server", server, sizeof server) != SOLARI_OK) server[0] = '\0';
        if (ctlArgStr(args, "ip", ip, sizeof ip) != SOLARI_OK) ip[0] = '\0';
        ctlSanitizeName(hostname, sanit, sizeof sanit);
        (void)snprintf(logpath, sizeof logpath, "run/provision-%s.log", sanit);
        argv[n++] = (char *)"deploy/fleet/fleet-provision.sh";
        argv[n++] = (char *)"--target";   argv[n++] = target;
        argv[n++] = (char *)"--distro";   argv[n++] = distro;
        argv[n++] = (char *)"--arch";     argv[n++] = arch;
        argv[n++] = (char *)"--hostname"; argv[n++] = hostname;
        if (profile[0]) { argv[n++] = (char *)"--profile"; argv[n++] = profile; }
        if (role[0])    { argv[n++] = (char *)"--role";    argv[n++] = role; }
        if (server[0])  { argv[n++] = (char *)"--server";  argv[n++] = server; }
        if (ip[0])      { argv[n++] = (char *)"--server-ip"; argv[n++] = ip; }
        argv[n++] = (char *)"--op"; argv[n++] = operator_;
        argv[n]   = NULL;
        st = ctlSpawnDetached(argv, logpath);
        if (st != SOLARI_OK)
            return ctlReplyErr(replyOut, replyCap, st, "provision spawn failed");
        solariLogf(SOLARI_LOG_INFO, "ctl: fleet provision target=%s distro=%s arch=%s op=%s -> %s",
                   target, distro, arch, operator_, logpath);
        (void)snprintf(extra, sizeof extra, "log=%s status=provisioning", logpath);
        return ctlReplyOk(replyOut, replyCap, extra);
    }

    /* ---- fleet imaging: build a bootable image (Raspberry Pi etc., detached) ---- */
    if (strcmp(verb, "FLEET_IMAGE") == 0) {
        char hostname[160], arch[16], distro[24], profile[48], role[32];
        char server[256], ip[64], sanit[160], logpath[256], extra[300];
        char *argv[26];
        int   n = 0;
        if (ctlArgStr(args, "hostname", hostname, sizeof hostname) != SOLARI_OK || !hostname[0])
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "hostname required");
        if (ctlArgStr(args, "arch", arch, sizeof arch) != SOLARI_OK || !arch[0])
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "arch required");
        if (ctlArgStr(args, "distro", distro, sizeof distro) != SOLARI_OK) (void)snprintf(distro, sizeof distro, "raspios");
        if (ctlArgStr(args, "profile", profile, sizeof profile) != SOLARI_OK) profile[0] = '\0';
        if (ctlArgStr(args, "role", role, sizeof role) != SOLARI_OK) role[0] = '\0';
        if (ctlArgStr(args, "server", server, sizeof server) != SOLARI_OK) server[0] = '\0';
        if (ctlArgStr(args, "ip", ip, sizeof ip) != SOLARI_OK) ip[0] = '\0';
        ctlSanitizeName(hostname, sanit, sizeof sanit);
        (void)snprintf(logpath, sizeof logpath, "run/image-%s.log", sanit);
        argv[n++] = (char *)"deploy/fleet/fleet-image.sh";
        argv[n++] = (char *)"--hostname"; argv[n++] = hostname;
        argv[n++] = (char *)"--arch";     argv[n++] = arch;
        argv[n++] = (char *)"--distro";   argv[n++] = distro;
        if (profile[0]) { argv[n++] = (char *)"--profile"; argv[n++] = profile; }
        if (role[0])    { argv[n++] = (char *)"--role";    argv[n++] = role; }
        if (server[0])  { argv[n++] = (char *)"--server";  argv[n++] = server; }
        if (ip[0])      { argv[n++] = (char *)"--server-ip"; argv[n++] = ip; }
        argv[n++] = (char *)"--op"; argv[n++] = operator_;
        argv[n]   = NULL;
        st = ctlSpawnDetached(argv, logpath);
        if (st != SOLARI_OK)
            return ctlReplyErr(replyOut, replyCap, st, "image spawn failed");
        solariLogf(SOLARI_LOG_INFO, "ctl: fleet image hostname=%s arch=%s op=%s -> %s",
                   hostname, arch, operator_, logpath);
        (void)snprintf(extra, sizeof extra, "log=%s status=imaging", logpath);
        return ctlReplyOk(replyOut, replyCap, extra);
    }

    /* ---- broadcast survey (non-destructive) ---- */
    if (strcmp(verb, "SURVEY") == 0) {
        uint8_t frame[CTL_REQ_CAP];
        size_t  frameLen = 0;
        st = serverControlBuildSurvey(ctl->ctx, frame, sizeof frame, &frameLen);
        /* Server->fleet demands ride the PUB channel, not the REP control socket
         * (which can only answer node-initiated requests). */
        if (st == SOLARI_OK && ctl->ctx->pub)
            st = solariConnSend(ctl->ctx->pub, frame, frameLen);
        if (st == SOLARI_OK || st == ERR_CONN_RETRY)
            return ctlReplyOk(replyOut, replyCap, "survey=sent");
        return ctlReplyErr(replyOut, replyCap, st, "survey failed");
    }

    /* ---- enrollment decisions ---- */
    if (strcmp(verb, "APPROVE") == 0 || strcmp(verb, "REJECT") == 0) {
        uint64_t enrId = 0;
        char op[64];
        if (ctlArgU64(args, "enr", &enrId) != SOLARI_OK || enrId == 0)
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "bad enr id");
        if (ctlArgStr(args, "op", op, sizeof op) != SOLARI_OK) op[0] = '\0';
        st = (verb[0] == 'A')
                 ? serverProvisionApprove(ctl->ctx, enrId, op)
                 : serverProvisionReject(ctl->ctx, enrId, op);
        if (st == SOLARI_OK) return ctlReplyOk(replyOut, replyCap, NULL);
        return ctlReplyErr(replyOut, replyCap, st, "enroll decision failed");
    }

    /* ---- first-time bring-up ---- */
    if (strcmp(verb, "PROVISION") == 0) {
        uint64_t nodeId = 0, buildId = 0, epoch = 0;
        char cfg[CTL_REQ_CAP];
        if (ctlArgU64(args, "node", &nodeId) != SOLARI_OK || nodeId == 0)
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "bad node id");
        (void)ctlArgU64(args, "build", &buildId);
        (void)ctlArgU64(args, "epoch", &epoch);
        if (ctlArgStr(args, "cfg", cfg, sizeof cfg) != SOLARI_OK) cfg[0] = '\0';
        st = serverProvisionNode(ctl->ctx, nodeId, buildId, cfg, epoch);
        if (st == SOLARI_OK) return ctlReplyOk(replyOut, replyCap, NULL);
        return ctlReplyErr(replyOut, replyCap, st, "provision failed");
    }

    /* ---- adopt a discovered entity into a monitored asset ---- */
    if (strcmp(verb, "ADOPT") == 0) {
        serverAdoptOpts o;
        uint64_t hb = 1, assetId = 0;
        char extra[64];
        memset(&o, 0, sizeof o);
        if (ctlArgU64(args, "disc", &o.discId) != SOLARI_OK || o.discId == 0)
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "bad disc id");
        (void)ctlArgU64(args, "pool", &o.poolId);
        if (ctlArgU64(args, "heartbeat", &hb) != SOLARI_OK) hb = 1;
        o.heartbeat = (hb != 0);
        (void)ctlArgStr(args, "name",     o.displayName, sizeof o.displayName);
        (void)ctlArgStr(args, "class",    o.className,   sizeof o.className);
        (void)ctlArgStr(args, "tags",     o.tagsJson,    sizeof o.tagsJson);
        (void)ctlArgStr(args, "notes",    o.notes,       sizeof o.notes);
        (void)ctlArgStr(args, "services", o.servicesCsv, sizeof o.servicesCsv);
        st = serverAssetsAdopt(ctl->ctx, &o, &assetId);
        if (st != SOLARI_OK)
            return ctlReplyErr(replyOut, replyCap, st, "adopt failed");
        (void)snprintf(extra, sizeof extra, "assetId=%llu", (unsigned long long)assetId);
        return ctlReplyOk(replyOut, replyCap, extra);
    }

    /* ---- update a monitored asset's metadata (keyed by ip) ---- */
    if (strcmp(verb, "ASSET_SET") == 0) {
        char ip[64], name[128], cls[32], tags[256], notes[256];
        uint64_t poolId = 0, hb = 1;
        if (ctlArgStr(args, "ip", ip, sizeof ip) != SOLARI_OK || !ip[0])
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "ip required");
        (void)ctlArgU64(args, "pool", &poolId);
        if (ctlArgU64(args, "heartbeat", &hb) != SOLARI_OK) hb = 1;
        if (ctlArgStr(args, "name",  name,  sizeof name)  != SOLARI_OK) name[0]  = '\0';
        if (ctlArgStr(args, "class", cls,   sizeof cls)   != SOLARI_OK) cls[0]   = '\0';
        if (ctlArgStr(args, "tags",  tags,  sizeof tags)  != SOLARI_OK) tags[0]  = '\0';
        if (ctlArgStr(args, "notes", notes, sizeof notes) != SOLARI_OK) notes[0] = '\0';
        st = serverAssetsSetMeta(ctl->ctx, ip, name, cls, poolId, tags, notes, hb != 0);
        if (st == SOLARI_OK) return ctlReplyOk(replyOut, replyCap, NULL);
        return ctlReplyErr(replyOut, replyCap, st, "asset update failed");
    }

    /* ---- remove a monitored asset and all owned probe target state ---- */
    if (strcmp(verb, "ASSET_REMOVE") == 0) {
        char key[80], extra[64];
        size_t removed = 0;
        bool byIp = false;
        if (ctlArgStr(args, "asset", key, sizeof key) != SOLARI_OK || !key[0]) {
            if (ctlArgStr(args, "ip", key, sizeof key) != SOLARI_OK || !key[0])
                return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "asset or ip required");
            byIp = true;
        }
        st = serverAssetsRemove(ctl->ctx, key, byIp, operator_, &removed);
        if (st != SOLARI_OK)
            return ctlReplyErr(replyOut, replyCap, st, "asset remove failed");
        (void)snprintf(extra, sizeof extra, "removed=%zu", removed);
        return ctlReplyOk(replyOut, replyCap, extra);
    }

    /* ---- remove one probe target and its current/history rows ---- */
    if (strcmp(verb, "TARGET_REMOVE") == 0) {
        char target[SERVER_TARGETID_MAX], detail[256];
        if (ctlArgStr(args, "target", target, sizeof target) != SOLARI_OK || !target[0])
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "target required");
        (void)snprintf(detail, sizeof detail, "target removed by %s target=%s",
                       operator_, target);
        st = serverDbWriteAlertEvent(ctl->ctx->db, 0, 0, target, "warn",
                                     detail, solariNowUnixMs(), "audit", 0, 0, 2, "suppress", NULL);
        if (st != SOLARI_OK)
            return ctlReplyErr(replyOut, replyCap, st, "target audit failed");
        if ((st = serverDbTombstoneProbeTarget(ctl->ctx->db, target, operator_)) != SOLARI_OK)
            return ctlReplyErr(replyOut, replyCap, st, "target tombstone failed");
        if ((st = serverDbPurgeProbeState(ctl->ctx->db, target)) != SOLARI_OK)
            return ctlReplyErr(replyOut, replyCap, st, "target purge failed");
        st = serverDbDeleteProbeTarget(ctl->ctx->db, target);
        if (st == SOLARI_OK) return ctlReplyOk(replyOut, replyCap, NULL);
        return ctlReplyErr(replyOut, replyCap, st, "target remove failed");
    }

    /* ---- lifecycle tombstone/restore for an asset ---- */
    if (strcmp(verb, "LIFECYCLE_SET") == 0) {
        uint64_t assetId = 0; char to[24]; char targets[512][SERVER_TARGETID_MAX]; size_t count=0, i;
        if (ctlArgU64(args, "asset", &assetId) != SOLARI_OK || assetId == 0 ||
            ctlArgStr(args, "to", to, sizeof to) != SOLARI_OK ||
            (strcmp(to,"active") && strcmp(to,"decommissioned") && strcmp(to,"deleted")))
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "asset and valid to required");
        if (strcmp(to, "active") != 0) {
            st=serverDbListAssetTargets(ctl->ctx->db,assetId,targets,512,&count);
            if(st!=SOLARI_OK)return ctlReplyErr(replyOut,replyCap,st,"target list failed");
        }
        st=serverDbLifecycleTransition(ctl->ctx->db,assetId,to,targets,count);
        if(st==SOLARI_OK && strcmp(to,"active")!=0)for(i=0;i<count;i++)serverAlertDropTargetState(targets[i]);
        /* Restore resumes probing immediately: the heartbeat sync is otherwise
         * event-driven and nothing else touches the asset (A1 deploy gap). */
        if(st==SOLARI_OK && strcmp(to,"active")==0 &&
           serverAssetsResyncHeartbeat(ctl->ctx,assetId)!=SOLARI_OK)
            solariLogf(SOLARI_LOG_WARN,"lifecycle: restore heartbeat resync failed asset=%llu",(unsigned long long)assetId);
        return st==SOLARI_OK?ctlReplyOk(replyOut,replyCap,NULL):ctlReplyErr(replyOut,replyCap,st,"lifecycle update failed");
    }

    /* ---- true asset purge; alert history is retained as cleared transitions ---- */
    if (strcmp(verb, "ASSET_PURGE") == 0) {
        uint64_t assetId=0; char confirm[128],displayName[128],host[SOLARI_FQDN_MAX],ip[SERVER_IP_MAX],targets[512][SERVER_TARGETID_MAX]; size_t removed=0,count=0,i;
        if(ctlArgU64(args,"asset",&assetId)!=SOLARI_OK || assetId==0 ||
           ctlArgStr(args,"confirm",confirm,sizeof confirm)!=SOLARI_OK || !confirm[0])
            return ctlReplyErr(replyOut,replyCap,ERR_INVALID_ARG,"asset and confirm required");
        if((st=serverDbGetAssetConfirmValues(ctl->ctx->db,assetId,displayName,sizeof displayName,host,sizeof host,ip,sizeof ip))!=SOLARI_OK)
            return ctlReplyErr(replyOut,replyCap,st,"asset lookup failed");
        if((!displayName[0] || strcmp(confirm,displayName)!=0) &&
           (!host[0] || strcmp(confirm,host)!=0) &&
           (!ip[0] || strcmp(confirm,ip)!=0))
            return ctlReplyErr(replyOut,replyCap,ERR_INVALID_ARG,"purge confirmation does not match asset");
        if((st=serverDbListAssetTargets(ctl->ctx->db,assetId,targets,512,&count))!=SOLARI_OK)
            return ctlReplyErr(replyOut,replyCap,st,"target list failed");
        { char id[32]; snprintf(id,sizeof id,"%llu",(unsigned long long)assetId);
          st=serverAssetsRemove(ctl->ctx,id,false,operator_,&removed); }
        if(st==SOLARI_OK)for(i=0;i<count;i++)serverAlertDropTargetState(targets[i]);
        return st==SOLARI_OK?ctlReplyOk(replyOut,replyCap,"purged=1"):ctlReplyErr(replyOut,replyCap,st,"asset purge failed");
    }

    if (strcmp(verb, "CRIT_SET") == 0) {
        uint64_t assetId=0,nodeId=0,tier=0;
        (void)ctlArgU64(args,"asset",&assetId); (void)ctlArgU64(args,"node",&nodeId);
        if(ctlArgU64(args,"tier",&tier)!=SOLARI_OK || tier>4 || ((assetId==0)==(nodeId==0)))
            return ctlReplyErr(replyOut,replyCap,ERR_INVALID_ARG,"one entity and tier 0..4 required");
        st=serverDbSetCriticality(ctl->ctx->db,assetId,nodeId,(int)tier);
        return st==SOLARI_OK?ctlReplyOk(replyOut,replyCap,NULL):ctlReplyErr(replyOut,replyCap,st,"criticality update failed");
    }

    /* ---- create a functional pool ---- */
    if (strcmp(verb, "POOL_NEW") == 0) {
        char name[64], desc[256], color[16], extra[48];
        uint64_t poolId = 0;
        if (ctlArgStr(args, "name", name, sizeof name) != SOLARI_OK || !name[0])
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "name required");
        if (ctlArgStr(args, "desc",  desc,  sizeof desc)  != SOLARI_OK) desc[0]  = '\0';
        if (ctlArgStr(args, "color", color, sizeof color) != SOLARI_OK) color[0] = '\0';
        st = serverDbCreatePool(ctl->ctx->db, name, desc[0] ? desc : NULL,
                                color[0] ? color : NULL, &poolId);
        if (st != SOLARI_OK)
            return ctlReplyErr(replyOut, replyCap, st, "pool create failed");
        (void)snprintf(extra, sizeof extra, "poolId=%llu", (unsigned long long)poolId);
        return ctlReplyOk(replyOut, replyCap, extra);
    }

    /* ---- update a functional pool ---- */
    if (strcmp(verb, "POOL_SET") == 0) {
        char name[64], desc[256], color[16];
        uint64_t poolId = 0;
        if (ctlArgU64(args, "pool", &poolId) != SOLARI_OK || poolId == 0)
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "bad pool id");
        if (ctlArgStr(args, "name",  name,  sizeof name)  != SOLARI_OK) name[0]  = '\0';
        if (ctlArgStr(args, "desc",  desc,  sizeof desc)  != SOLARI_OK) desc[0]  = '\0';
        if (ctlArgStr(args, "color", color, sizeof color) != SOLARI_OK) color[0] = '\0';
        st = serverDbUpdatePool(ctl->ctx->db, poolId, name[0] ? name : NULL,
                                desc[0] ? desc : NULL, color[0] ? color : NULL);
        if (st == SOLARI_OK) return ctlReplyOk(replyOut, replyCap, NULL);
        return ctlReplyErr(replyOut, replyCap, st, "pool update failed");
    }

    /* ---- delete a functional pool, moving members back to Unassigned ---- */
    if (strcmp(verb, "POOL_DEL") == 0) {
        uint64_t poolId = 0;
        size_t reassigned = 0;
        char detail[160], extra[64];
        if (ctlArgU64(args, "pool", &poolId) != SOLARI_OK || !ctlPoolCanDelete(poolId))
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "bad pool id");
        (void)snprintf(detail, sizeof detail, "pool deleted by %s pool=%llu",
                       operator_, (unsigned long long)poolId);
        st = serverDbWriteAlertEvent(ctl->ctx->db, 0, 0, NULL, "warn",
                                     detail, solariNowUnixMs(), "audit", 0, 0, 2, "suppress", NULL);
        if (st != SOLARI_OK)
            return ctlReplyErr(replyOut, replyCap, st, "pool audit failed");
        if ((st = serverDbReassignPoolAssets(ctl->ctx->db, poolId, 1, &reassigned)) != SOLARI_OK)
            return ctlReplyErr(replyOut, replyCap, st, "pool reassign failed");
        st = serverDbDeletePool(ctl->ctx->db, poolId);
        if (st != SOLARI_OK)
            return ctlReplyErr(replyOut, replyCap, st, "pool delete failed");
        (void)snprintf(extra, sizeof extra, "reassigned=%zu", reassigned);
        return ctlReplyOk(replyOut, replyCap, extra);
    }

    /* ---- persist the fleet-wide config document ---- */
    if (strcmp(verb, "CONFIG_SET") == 0) {
        char *cfg = (char *)malloc(CTL_REQ_CAP);
        char  op[64], extra[48];
        uint64_t epoch = 0;
        if (!cfg) return ctlReplyErr(replyOut, replyCap, ERR_PLATFORM, "oom");
        if (ctlArgStr(args, "cfg", cfg, CTL_REQ_CAP) != SOLARI_OK || !cfg[0]) {
            free(cfg);
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "cfg required");
        }
        if (ctlArgStr(args, "op", op, sizeof op) != SOLARI_OK) op[0] = '\0';
        st = serverDbSetGlobalConfig(ctl->ctx->db, cfg, op[0] ? op : NULL, &epoch);
        free(cfg);
        if (st != SOLARI_OK)
            return ctlReplyErr(replyOut, replyCap, st, "config set failed");
        (void)snprintf(extra, sizeof extra, "epoch=%llu", (unsigned long long)epoch);
        return ctlReplyOk(replyOut, replyCap, extra);
    }

    /* ---- edit an alert rule ---- */
    if (strcmp(verb, "RULE_SET") == 0) {
        serverAlertRuleEdit e;
        uint64_t ruleId = 0, vU = 0;
        char buf[64];
        memset(&e, 0, sizeof e);
        if (ctlArgU64(args, "rule", &ruleId) != SOLARI_OK || ruleId == 0)
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "bad rule id");
        e.ruleId = (int)ruleId;
        if (ctlArgU64(args, "enabled", &vU) == SOLARI_OK) { e.hasEnabled = true; e.enabled = (vU != 0); }
        if (ctlArgStr(args, "threshold", buf, sizeof buf) == SOLARI_OK) { e.hasThreshold = true; e.threshold = atof(buf); }
        if (ctlArgU64(args, "forSeconds", &vU) == SOLARI_OK) { e.hasForSeconds = true; e.forSeconds = (int)vU; }
        if (ctlArgStr(args, "op", e.op, sizeof e.op) == SOLARI_OK && e.op[0]) e.hasOp = true;
        if (ctlArgStr(args, "severity", e.severity, sizeof e.severity) == SOLARI_OK && e.severity[0]) e.hasSeverity = true;
        if (ctlArgStr(args, "metric", e.metric, sizeof e.metric) == SOLARI_OK && e.metric[0]) e.hasMetric = true;
        if (ctlArgStr(args, "scope", e.scope, sizeof e.scope) == SOLARI_OK && e.scope[0]) e.hasScope = true;
        st = serverDbUpdateAlertRule(ctl->ctx->db, &e);
        if (st == SOLARI_OK) return ctlReplyOk(replyOut, replyCap, NULL);
        return ctlReplyErr(replyOut, replyCap, st, "rule update failed");
    }

    /* ---- delete an alert rule ---- */
    if (strcmp(verb, "RULE_DEL") == 0) {
        uint64_t ruleId = 0;
        char detail[160];
        if (ctlArgU64(args, "rule", &ruleId) != SOLARI_OK || ruleId == 0)
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "bad rule id");
        (void)snprintf(detail, sizeof detail, "rule deleted by %s rule=%llu",
                       operator_, (unsigned long long)ruleId);
        st = serverDbWriteAlertEvent(ctl->ctx->db, 0, 0, NULL, "warn",
                                     detail, solariNowUnixMs(), "audit", 0, 0, 2, "suppress", NULL);
        if (st != SOLARI_OK)
            return ctlReplyErr(replyOut, replyCap, st, "rule audit failed");
        st = serverDbDeleteAlertRule(ctl->ctx->db, ruleId);
        if (st == SOLARI_OK) return ctlReplyOk(replyOut, replyCap, NULL);
        return ctlReplyErr(replyOut, replyCap, st, "rule delete failed");
    }

    /* ---- acknowledge/clear an alert event (operator attributed) ---- */
    if (strcmp(verb, "ALERT_ACK") == 0) {
        uint64_t eventId = 0;
        if (ctlArgU64(args, "event", &eventId) != SOLARI_OK || eventId == 0)
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "bad event id");
        st = serverDbAckAlertEvent(ctl->ctx->db, eventId);
        if (st == SOLARI_OK) {
            solariLogf(SOLARI_LOG_INFO, "ctl: alert ack event=%llu by=%s",
                       (unsigned long long)eventId, operator_);
            return ctlReplyOk(replyOut, replyCap, NULL);
        }
        return ctlReplyErr(replyOut, replyCap, st, "alert ack failed");
    }

    /* ---- suppress a discovered candidate ---- */
    if (strcmp(verb, "IGNORE") == 0) {
        uint64_t discId = 0;
        if (ctlArgU64(args, "disc", &discId) != SOLARI_OK || discId == 0)
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "bad disc id");
        st = serverDiscoveryIgnore(ctl->ctx, discId);
        if (st == SOLARI_OK) return ctlReplyOk(replyOut, replyCap, NULL);
        return ctlReplyErr(replyOut, replyCap, st, "ignore failed");
    }

    /* ---- generic control directive ---- */
    if (strcmp(verb, "CONTROL") == 0) {
        uint64_t nodeId = 0, epoch = 0, verbNum = 0;
        char payload[CTL_REQ_CAP];
        uint8_t frame[CTL_REQ_CAP];
        size_t  frameLen = 0;
        size_t  payLen;
        if (ctlArgU64(args, "node", &nodeId) != SOLARI_OK || nodeId == 0)
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "bad node id");
        if (ctlArgU64(args, "verb", &verbNum) != SOLARI_OK || verbNum == 0 ||
            verbNum > 0xFF)
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "bad verb");
        (void)ctlArgU64(args, "epoch", &epoch);
        if (ctlArgStr(args, "payload", payload, sizeof payload) != SOLARI_OK)
            payload[0] = '\0';
        payLen = strlen(payload);
        st = serverControlBuild(ctl->ctx, nodeId, (solariCtrlVerb)verbNum, epoch,
                                payLen ? (const uint8_t *)payload : NULL,
                                (uint16_t)payLen, frame, sizeof frame, &frameLen);
        /* Directives are published on the fleet PUB channel (see SURVEY note). */
        if (st == SOLARI_OK && ctl->ctx->pub)
            st = solariConnSend(ctl->ctx->pub, frame, frameLen);
        if (st == SOLARI_OK || st == ERR_CONN_RETRY)
            return ctlReplyOk(replyOut, replyCap, "control=sent");
        return ctlReplyErr(replyOut, replyCap, st, "control failed");
    }

    /* ---- guarded decommission (RBAC already enforced; confirm-gated) ---- */
    if (strcmp(verb, "DECOMMISSION") == 0) {
        uint64_t nodeId = 0, confirm = 0, issued = 0;
        uint32_t scope = 0;
        char extra[96];
        bool haveConfirm;
        if (ctlArgU64(args, "node", &nodeId) != SOLARI_OK || nodeId == 0)
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "bad node id");
        if (ctlArgU32Hex(args, "scope", &scope) != SOLARI_OK || (scope & 0x1Fu) == 0)
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "bad wipe scope");
        haveConfirm = (ctlArgU64(args, "confirm", &confirm) == SOLARI_OK &&
                       confirm != 0);

        /* Two-step explicit-confirm protocol (§7.3, §11): the first call issues
         * the one-time token (the SCP_MSG_DECOMMISSION is sent, but the node
         * will not act until it echoes the token, and we do NOT retire the node
         * yet). The operator must re-POST with confirm=<token> to finalize. */
        st = serverProvisionDecommission(ctl->ctx, nodeId, scope, operator_,
                                         &issued);
        if (st != SOLARI_OK)
            return ctlReplyErr(replyOut, replyCap, st, "decommission failed");

        if (!haveConfirm) {
            (void)snprintf(extra, sizeof extra, "confirm=%llu",
                           (unsigned long long)issued);
            return ctlReplyOk(replyOut, replyCap, extra);
        }
        /* A confirm token was supplied: it must match a freshly-derivable token
         * for this exact request, proving the operator saw the first reply. */
        if (confirm != issued) {
            solariLogf(SOLARI_LOG_WARN,
                       "ctl: decommission confirm mismatch for node %llu",
                       (unsigned long long)nodeId);
            return ctlReplyErr(replyOut, replyCap, ERR_AUTH_ROLE,
                               "confirm token mismatch");
        }
        st = serverProvisionRetire(ctl->ctx, nodeId);
        if (st == SOLARI_OK) return ctlReplyOk(replyOut, replyCap, "retired=1");
        return ctlReplyErr(replyOut, replyCap, st, "retire failed");
    }

    /* ---- force-retire (RBAC enforced; an operator-driven terminal state) ---- */
    if (strcmp(verb, "RETIRE") == 0) {
        uint64_t nodeId = 0;
        if (ctlArgU64(args, "node", &nodeId) != SOLARI_OK || nodeId == 0)
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "bad node id");
        st = serverProvisionRetire(ctl->ctx, nodeId);
        if (st != SOLARI_OK) return ctlReplyErr(replyOut, replyCap, st, "retire failed");
        st = serverDbClearOpenNodeAlerts(ctl->ctx->db, nodeId);
        if (st != SOLARI_OK)
            solariLogf(SOLARI_LOG_WARN, "ctl: node retire alert cleanup failed node=%llu: %s",
                       (unsigned long long)nodeId, solariStrError(st));
        return ctlReplyOk(replyOut, replyCap, "retired=1");
    }

    solariLogf(SOLARI_LOG_WARN, "ctl: unknown verb '%s'", verb);
    return ctlReplyErr(replyOut, replyCap, ERR_UNKNOWN_MSG, "unknown verb");
}

/* ===================================================================== */
/* AF_UNIX socket lifecycle + poll                                        */
/* ===================================================================== */

solariStatus serverCtlOpen(const serverConfig *cfg, serverContext *ctx,
                           serverCtl **out)
{
    serverCtl *ctl;
    struct sockaddr_un addr;
    int fd;
    size_t pathLen;

    if (out) *out = NULL;
    if (!cfg || !ctx || !out) return ERR_INVALID_ARG;
    if (cfg->ctlSocket[0] == '\0') {
        solariLogf(SOLARI_LOG_ERROR, "ctl: no ctlSocket path configured");
        return ERR_INVALID_ARG;
    }
    pathLen = strlen(cfg->ctlSocket);
    if (pathLen >= sizeof addr.sun_path) {
        solariLogf(SOLARI_LOG_ERROR,
                   "ctl: socket path too long (%zu B) for sockaddr_un", pathLen);
        return ERR_INVALID_ARG;
    }

    ctl = (serverCtl *)calloc(1, sizeof *ctl);
    if (!ctl) return ERR_PLATFORM;
    ctl->ctx      = ctx;
    ctl->listenFd = -1;
    ctl->bound    = false;
    /* Copy the CA material paths so the handle is self-contained; the web tier
     * never receives these (they live only in the server config + this fd). */
    (void)snprintf(ctl->sockPath,  sizeof ctl->sockPath,  "%s", cfg->ctlSocket);
    /* CA cert + key for signing — separate from the server's own TLS material
     * (cfg->certFile/keyFile) so the CA can relocate. caCertFile defaults to the
     * verify root when unset (handled by the config loader). */
    (void)snprintf(ctl->caFile,    sizeof ctl->caFile,    "%s",
                   cfg->caCertFile[0] ? cfg->caCertFile : cfg->caFile);
    (void)snprintf(ctl->caKeyFile, sizeof ctl->caKeyFile, "%s", cfg->caKeyFile);
    (void)snprintf(ctl->caMode,    sizeof ctl->caMode,    "%s",
                   cfg->caMode[0] ? cfg->caMode : "local");
    (void)snprintf(ctl->caUrl,     sizeof ctl->caUrl,     "%s", cfg->caUrl);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        solariLogf(SOLARI_LOG_ERROR, "ctl: socket() failed: %s", strerror(errno));
        free(ctl);
        return ERR_PLATFORM;
    }

    /* A stale socket file from a prior run would make bind() fail with EADDRINUSE;
     * unlink it first (the path is server-owned). */
    (void)unlink(ctl->sockPath);

    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, ctl->sockPath, pathLen);

    if (bind(fd, (const struct sockaddr *)&addr, (socklen_t)sizeof addr) != 0) {
        solariLogf(SOLARI_LOG_ERROR, "ctl: bind(%s) failed: %s",
                   ctl->sockPath, strerror(errno));
        close(fd);
        free(ctl);
        return ERR_PLATFORM;
    }
    if (listen(fd, CTL_LISTEN_BACKLOG) != 0) {
        solariLogf(SOLARI_LOG_ERROR, "ctl: listen(%s) failed: %s",
                   ctl->sockPath, strerror(errno));
        close(fd);
        (void)unlink(ctl->sockPath);
        free(ctl);
        return ERR_PLATFORM;
    }

    /* Non-blocking accept: serverCtlPoll() is documented "non-blocking; processes
     * what is ready", and ctlServiceOne() already treats EAGAIN/EWOULDBLOCK as
     * "nothing pending". Mark the listener O_NONBLOCK so the server's main loop
     * can poll it every iteration without stalling on accept() when no operator
     * request is waiting. */
    {
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0)
            solariLogf(SOLARI_LOG_WARN,
                       "ctl: could not set listener non-blocking: %s "
                       "(poll may stall without a select() gate)", strerror(errno));
    }

    ctl->listenFd = fd;
    ctl->bound    = true;

    /* Verify the CA material is at least present/readable so a later CSR sign
     * does not surprise the operator. Absence is a hard TLS error: this bridge
     * is the only holder of the CA key and must fail closed. */
    if (ctl->caKeyFile[0] == '\0') {
        solariLogf(SOLARI_LOG_WARN,
                   "ctl: no CA key configured; CSR signing will be unavailable");
    } else {
        FILE *kf = fopen(ctl->caKeyFile, "rb");
        if (!kf) {
            solariLogf(SOLARI_LOG_WARN,
                       "ctl: CA key '%s' not readable: %s (signing unavailable)",
                       ctl->caKeyFile, strerror(errno));
        } else {
            fclose(kf);
        }
    }

    solariLogf(SOLARI_LOG_INFO, "ctl: operator bridge listening on %s",
               ctl->sockPath);
    *out = ctl;
    return SOLARI_OK;
}

void serverCtlClose(serverCtl *ctl)
{
    if (!ctl) return;
    if (ctl->listenFd >= 0) close(ctl->listenFd);
    if (ctl->bound && ctl->sockPath[0]) (void)unlink(ctl->sockPath);
    /* Scrub the in-memory copies of the CA paths (defence in depth; the key
     * bytes themselves are never held here, only the path). */
    memset(ctl->caFile,    0, sizeof ctl->caFile);
    memset(ctl->caKeyFile, 0, sizeof ctl->caKeyFile);
    free(ctl);
}

/* Accept and service exactly one ready connection: read one request line,
 * dispatch it, write the reply, close. Returns SOLARI_OK if a request was
 * serviced, ERR_CONN_RETRY if nothing was ready, or a platform error. The
 * listening socket is non-blocking so this never stalls the server loop. */
static solariStatus ctlServiceOne(serverCtl *ctl)
{
    char    req[CTL_REQ_CAP];
    char    reply[CTL_REPLY_CAP];
    size_t  replyLen;
    ssize_t got;
    int     cfd;

    cfd = accept(ctl->listenFd, NULL, NULL);
    if (cfd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return ERR_CONN_RETRY;            /* nothing pending */
        if (errno == EINTR)
            return ERR_CONN_RETRY;
        solariLogf(SOLARI_LOG_WARN, "ctl: accept() failed: %s", strerror(errno));
        return ERR_PLATFORM;
    }

    got = read(cfd, req, sizeof req - 1);
    if (got <= 0) {
        if (got < 0)
            solariLogf(SOLARI_LOG_WARN, "ctl: read() failed: %s", strerror(errno));
        close(cfd);
        return ERR_CONN_RETRY;
    }
    req[got] = '\0';

    replyLen = ctlHandleLine(ctl, req, reply, sizeof reply);

    /* Best-effort reply; a closed peer is not the server's problem. */
    {
        size_t off = 0;
        while (off < replyLen) {
            ssize_t w = write(cfd, reply + off, replyLen - off);
            if (w <= 0) {
                if (w < 0 && errno == EINTR) continue;
                break;
            }
            off += (size_t)w;
        }
    }
    close(cfd);
    return SOLARI_OK;
}

solariStatus serverCtlPoll(serverCtl *ctl)
{
    int serviced = 0;

    if (!ctl) return ERR_INVALID_ARG;
    if (!ctl->bound || ctl->listenFd < 0) return SOLARI_OK;

    /* Make the listener non-blocking on first poll without relying on a header
     * we did not include at open time. fcntl is unavailable here without
     * <fcntl.h>; instead we rely on accept() returning EWOULDBLOCK only if the
     * socket was set non-blocking. To keep this self-contained and portable we
     * service whatever is immediately acceptable, bounded per poll so a flood
     * cannot starve the main loop. accept() on a blocking socket would stall;
     * the integrator binds this fd into the server's event loop (select/poll)
     * and only calls serverCtlPoll when the fd is readable, so a single
     * non-stalling accept is the correct unit of work. */
    while (serviced < CTL_LISTEN_BACKLOG) {
        solariStatus st = ctlServiceOne(ctl);
        if (st == ERR_CONN_RETRY) break;      /* drained / nothing ready */
        if (st != SOLARI_OK) return st;       /* platform error */
        serviced++;
    }
    return SOLARI_OK;
}

/* ===================================================================== */
/* CSR signing with the internal CA                                       */
/* ===================================================================== */

solariStatus serverCtlSignCsr(serverCtl *ctl, const char *csrPem,
                              char *certBuf, size_t cap)
{
    if (certBuf && cap) certBuf[0] = '\0';
    if (!ctl || !csrPem || !certBuf || cap == 0) return ERR_INVALID_ARG;

    /* Structural validation up front: reject anything that is not a PEM CSR
     * before touching the CA key. */
    if (!ctlLooksLikeCsr(csrPem)) {
        solariLogf(SOLARI_LOG_ERROR, "ctl: sign refused; payload is not a CSR PEM");
        return ERR_TLS;
    }
    /* Modularity seam: when the CA is relocated to a dedicated host, caMode is
     * "remote" and we forward the CSR to ctl->caUrl instead of signing here.
     * Not yet implemented — fail closed so we never mint an unsigned cert. */
    if (strcmp(ctl->caMode, "remote") == 0) {
        solariLogf(SOLARI_LOG_ERROR,
                   "ctl: caMode=remote (caUrl=%s) not yet implemented; sign refused",
                   ctl->caUrl[0] ? ctl->caUrl : "(unset)");
        return ERR_TLS;
    }
    if (ctl->caKeyFile[0] == '\0' || ctl->caFile[0] == '\0') {
        solariLogf(SOLARI_LOG_ERROR,
                   "ctl: sign refused; CA material not configured "
                   "(set [ca] keyFile + certFile)");
        return ERR_TLS;
    }

#if defined(SOLARI_HAVE_MBEDTLS_X509)
    {
        /* Real signing path: parse the CSR, load the CA cert + key, mint a leaf
         * certificate copying the CSR subject/pubkey, and PEM-encode it into
         * certBuf. Compiled only when the mbedTLS x509 writer is vendored. */
        mbedtls_x509_csr      csr;
        mbedtls_x509write_cert crt;
        mbedtls_pk_context    caKey;
        mbedtls_x509_crt      caCrt;
        mbedtls_ctr_drbg_context drbg;
        mbedtls_entropy_context  entropy;
        mbedtls_mpi serial;
        char  subjName[256];
        char  issName[256];
        int   rc;
        solariStatus st = ERR_TLS;
        static const char *pers = "solariCtl-csr-sign";

        mbedtls_x509_csr_init(&csr);
        mbedtls_x509write_crt_init(&crt);
        mbedtls_pk_init(&caKey);
        mbedtls_x509_crt_init(&caCrt);
        mbedtls_ctr_drbg_init(&drbg);
        mbedtls_entropy_init(&entropy);
        mbedtls_mpi_init(&serial);

        do {
            if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                      (const unsigned char *)pers,
                                      strlen(pers)) != 0) break;
            if (mbedtls_x509_csr_parse(&csr,
                                       (const unsigned char *)csrPem,
                                       strlen(csrPem) + 1) != 0) break;
            if (mbedtls_x509_crt_parse_file(&caCrt, ctl->caFile) != 0) break;
#if MBEDTLS_VERSION_MAJOR >= 3
            if (mbedtls_pk_parse_keyfile(&caKey, ctl->caKeyFile, NULL,
                                         mbedtls_ctr_drbg_random, &drbg) != 0) break;
#else
            if (mbedtls_pk_parse_keyfile(&caKey, ctl->caKeyFile, NULL) != 0) break;
#endif
            if (mbedtls_x509_dn_gets(subjName, sizeof subjName,
                                     &csr.subject) < 0) break;
            /* Issuer must be the CA cert's actual subject DN (not a literal) or
             * the chain won't verify against the configured CA root. */
            if (mbedtls_x509_dn_gets(issName, sizeof issName,
                                     &caCrt.subject) < 0) break;

            mbedtls_x509write_crt_set_subject_key(&crt, &csr.pk);
            mbedtls_x509write_crt_set_issuer_key(&crt, &caKey);
            if (mbedtls_x509write_crt_set_subject_name(&crt, subjName) != 0) break;
            if (mbedtls_x509write_crt_set_issuer_name(&crt, issName) != 0) break;
            mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
            mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
            /* Serial: the first 20 bytes of SHA-256(CSR) (reproducible), forced
             * positive and non-zero per RFC 5280. (A full 32-byte digest does
             * not fit a 20-octet serial, which previously yielded a zero/invalid
             * serial and an unparsable cert.) */
            {
                uint8_t dg[32], sbin[20];
                solariSha256(csrPem, strlen(csrPem), dg);
                memcpy(sbin, dg, sizeof sbin);
                sbin[0] &= 0x7F;                 /* clear sign bit -> positive */
                if (sbin[0] == 0) sbin[0] = 0x01;/* no leading-zero / non-zero */
#if MBEDTLS_VERSION_MAJOR >= 3
                if (mbedtls_x509write_crt_set_serial_raw(&crt, sbin, sizeof sbin) != 0) break;
#else
                if (mbedtls_mpi_read_binary(&serial, sbin, sizeof sbin) != 0) break;
                if (mbedtls_x509write_crt_set_serial(&crt, &serial) != 0) break;
#endif
            }
            if (mbedtls_x509write_crt_set_validity(
                    &crt, "20260101000000", "20360101000000") != 0) break;

            rc = mbedtls_x509write_crt_pem(&crt, (unsigned char *)certBuf, cap,
                                           mbedtls_ctr_drbg_random, &drbg);
            if (rc != 0) {
                st = (rc == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL)
                         ? ERR_BUFFER_FULL : ERR_TLS;
                break;
            }
            st = SOLARI_OK;
        } while (0);

        mbedtls_mpi_free(&serial);
        mbedtls_entropy_free(&entropy);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_x509_crt_free(&caCrt);
        mbedtls_pk_free(&caKey);
        mbedtls_x509write_crt_free(&crt);
        mbedtls_x509_csr_free(&csr);

        if (st != SOLARI_OK) {
            certBuf[0] = '\0';
            solariLogf(SOLARI_LOG_ERROR, "ctl: CSR sign failed (st=%d)", (int)st);
        } else {
            solariLogf(SOLARI_LOG_INFO, "ctl: CSR signed by internal CA");
        }
        return st;
    }
#else
    /* No x509 backend compiled in: fail closed rather than emit an unsigned
     * cert. The CSR was structurally valid and the CA material is present, so
     * this is purely a build-capability gap, surfaced to the integrator. */
    solariLogf(SOLARI_LOG_ERROR,
               "ctl: CSR sign unavailable; built without mbedTLS x509 "
               "(define SOLARI_HAVE_MBEDTLS_X509 with the vendored CA backend)");
    return ERR_TLS;
#endif
}

/*
 * CONTRACT GAPS (for the integrator - do not edit server.h here):
 *
 * 1. Operator request wire format: the shared contract fixes the C signatures
 *    but not the byte protocol the PHP/MCP layer speaks over cfg->ctlSocket.
 *    This file defines a minimal, documented line protocol (see the file header)
 *    "VERB k=v..\n" -> "OK..\n" / "ERR code msg\n". If the dashboard team has a
 *    different framing (e.g. length-prefixed JSON), only ctlHandleLine() and the
 *    pure ctlArg../ctlSplitVerb helpers need to change; the socket + RBAC + the
 *    serverProvision/Control dispatch are protocol-agnostic. Values in this
 *    protocol cannot contain spaces (the PHP layer is expected to URL-encode
 *    richer config blobs before sending them as cfg=/payload=).
 *
 * 2. Non-blocking accept: serverCtlPoll() is documented "non-blocking;
 *    processes what is ready", but neither server.h nor the libsolari net layer
 *    exposes the listening fd or an event-loop hook, and this file intentionally
 *    avoids editing shared headers. We therefore rely on the integrator binding
 *    ctl->listenFd into the server's select/poll set and calling serverCtlPoll
 *    only when it is readable. To make the fd genuinely non-blocking the
 *    integrator should add O_NONBLOCK (the bridge cannot include <fcntl.h>
 *    cleanly without risking other subsystems' assumptions; setting it at
 *    open-time is the one-line addition once the event-loop owner is settled).
 *    As written, accept() drains the backlog and stops on EWOULDBLOCK/EAGAIN.
 *    Recommend adding e.g. int serverCtlListenFd(const serverCtl*) so the main
 *    loop can register it without reaching into the opaque struct.
 *
 * 3. CSR signing backend: the real signer needs the mbedTLS x509 *writer*
 *    headers (x509_csr.h / x509_crt.h / pk.h). The top-level CMake brings
 *    mbedTLS transitively only when third_party/mbedtls is vendored; this tree
 *    has no x509 headers on the include path, so the signing block is guarded by
 *    SOLARI_HAVE_MBEDTLS_X509 and the default build fails closed with ERR_TLS
 *    (never an unsigned cert). When mbedTLS x509 is vendored, define
 *    SOLARI_HAVE_MBEDTLS_X509 for servercore and the full path compiles for both
 *    mbedTLS 2.x and 3.x (version-guarded API differences handled inline). A
 *    cleaner long-term shape is a libsolari wrapper (e.g. solariCaSignCsr in
 *    solariCrypto/an I/O-tier CA module) so this bridge does not embed mbedTLS
 *    API surface directly.
 *
 * 4. Decommission confirm: this bridge implements the two-step explicit-confirm
 *    UX (first call issues the token, second call with confirm=<token>
 *    finalizes via serverProvisionRetire). It re-invokes
 *    serverProvisionDecommission on the confirm step and compares the supplied
 *    token to the freshly issued one; because serverProvision derives the token
 *    from per-call clock stamps, the two derivations differ across calls, so the
 *    confirm comparison here is necessarily best-effort within a single call's
 *    issue+echo. A durable confirm-token store (e.g. serverDb persisting the
 *    issued token against the node, checked on the confirm step) would let the
 *    operator confirm across separate POSTs robustly; recommend a
 *    serverDb confirm-token getter/clearer for a fully stateless PHP tier.
 */
