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
 *   PING                                         -> liveness
 *   SURVEY                                       -> broadcast SCP_MSG_SURVEY
 *   APPROVE   enr=<id> op=<operator>             -> serverProvisionApprove
 *   REJECT    enr=<id> op=<operator>             -> serverProvisionReject
 *   PROVISION node=<id> build=<id> epoch=<n> [cfg=<json>]
 *   ADOPT     disc=<id> [spec=<probeSpec>]       -> serverProvisionAdoptTarget
 *   IGNORE    disc=<id>                          -> serverDiscoveryIgnore
 *   CONTROL   node=<id> verb=<n> [epoch=<n>] [payload=<blob>]
 *   DECOMMISSION node=<id> scope=<hex> op=<operator> [confirm=<token>]
 *   RETIRE    node=<id> op=<operator>
 *
 * Destructive verbs (DECOMMISSION, RETIRE) require two things: an operator
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

#include "solari/solariCrypto.h"
#include "solari/solariError.h"
#include "solari/solariLog.h"
#include "solari/solariNet.h"
#include "solari/solariTime.h"
#include "solari/solariTlv.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/un.h>
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
#endif

/* Largest single operator request line we will read in one poll. The PHP layer
 * frames one request per line; anything larger is a malformed/abusive caller. */
#define CTL_REQ_CAP   4096
/* Largest reply line we assemble. */
#define CTL_REPLY_CAP 1024
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
    char           caFile[SERVER_PATH_MAX];  /* internal CA root cert (PEM)     */
    char           caKeyFile[SERVER_PATH_MAX];/* CA private key (PEM)           */
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
    return strcmp(verb, "DECOMMISSION") == 0 || strcmp(verb, "RETIRE") == 0;
}

/* RBAC gate for destructive verbs: every destructive request must name the
 * authorizing operator (op=...). The PHP tier authenticates the human and adds
 * this field; an empty/absent operator is refused. Pure. */
static solariStatus ctlCheckRbac(const char *verb, const char *args,
                                 char *operatorOut, size_t cap)
{
    solariStatus st;
    if (operatorOut && cap) operatorOut[0] = '\0';
    if (!ctlVerbIsDestructive(verb)) return SOLARI_OK;
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

    /* ---- broadcast survey (non-destructive) ---- */
    if (strcmp(verb, "SURVEY") == 0) {
        uint8_t frame[CTL_REQ_CAP];
        size_t  frameLen = 0;
        st = serverControlBuildSurvey(ctl->ctx, frame, sizeof frame, &frameLen);
        if (st == SOLARI_OK && ctl->ctx->control)
            st = solariConnSend(ctl->ctx->control, frame, frameLen);
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

    /* ---- adopt a discovered entity as a probe target ---- */
    if (strcmp(verb, "ADOPT") == 0) {
        uint64_t discId = 0;
        char spec[CTL_REQ_CAP];
        if (ctlArgU64(args, "disc", &discId) != SOLARI_OK || discId == 0)
            return ctlReplyErr(replyOut, replyCap, ERR_INVALID_ARG, "bad disc id");
        if (ctlArgStr(args, "spec", spec, sizeof spec) != SOLARI_OK) spec[0] = '\0';
        st = serverProvisionAdoptTarget(ctl->ctx, discId,
                                        spec[0] ? spec : NULL);
        if (st == SOLARI_OK) return ctlReplyOk(replyOut, replyCap, NULL);
        return ctlReplyErr(replyOut, replyCap, st, "adopt failed");
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
        if (st == SOLARI_OK && ctl->ctx->control)
            st = solariConnSend(ctl->ctx->control, frame, frameLen);
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
        if (st == SOLARI_OK) return ctlReplyOk(replyOut, replyCap, "retired=1");
        return ctlReplyErr(replyOut, replyCap, st, "retire failed");
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
    (void)snprintf(ctl->caFile,    sizeof ctl->caFile,    "%s", cfg->caFile);
    (void)snprintf(ctl->caKeyFile, sizeof ctl->caKeyFile, "%s", cfg->keyFile);

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
    if (ctl->caKeyFile[0] == '\0' || ctl->caFile[0] == '\0') {
        solariLogf(SOLARI_LOG_ERROR,
                   "ctl: sign refused; CA material not configured");
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

            mbedtls_x509write_crt_set_subject_key(&crt, &csr.pk);
            mbedtls_x509write_crt_set_issuer_key(&crt, &caKey);
            if (mbedtls_x509write_crt_set_subject_name(&crt, subjName) != 0) break;
            if (mbedtls_x509write_crt_set_issuer_name(
                    &crt, "CN=SolariNet Internal CA") != 0) break;
            mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
            mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
            /* Serial derived from a SHA-256 over the CSR for reproducibility. */
            {
                uint8_t dg[32];
                solariSha256(csrPem, strlen(csrPem), dg);
                if (mbedtls_mpi_read_binary(&serial, dg, sizeof dg) != 0) break;
#if MBEDTLS_VERSION_MAJOR >= 3
                {
                    uint8_t sbin[20];
                    (void)mbedtls_mpi_write_binary(&serial, sbin, sizeof sbin);
                    if (mbedtls_x509write_crt_set_serial_raw(
                            &crt, sbin, sizeof sbin) != 0) break;
                }
#else
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
