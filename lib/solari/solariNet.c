/*
 * solariNet.c - nng transport wrapper with mbedTLS (section 6.5).
 *
 * BUILD NOTE: this translation unit requires the vendored nng library (and its
 * mbedTLS TLS engine). It is compiled only when the I/O layer is enabled
 * (CMake option SOLARI_WITH_IO) and nng is present under third_party/. nng is
 * not available in the initial build sandbox, so this file is written to the
 * published nng API and will compile once nng is vendored per section 3.
 *
 * The code maps nng's rich error space onto the small solariStatus contract:
 * transient conditions (timeout / would-block / peer momentarily gone) become
 * ERR_CONN_RETRY so callers spool and retry; TLS problems become ERR_TLS; the
 * rest become ERR_CONN_FATAL.
 */
#include "solari/solariNet.h"
#include "solari/solariLog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nng/nng.h>
#include <nng/protocol/pipeline0/push.h>
#include <nng/protocol/pipeline0/pull.h>
#include <nng/protocol/survey0/survey.h>
#include <nng/protocol/survey0/respond.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>
#include <nng/protocol/reqrep0/req.h>
#include <nng/protocol/reqrep0/rep.h>
#include <nng/supplemental/tls/tls.h>

struct solariConn {
    nng_socket sock;
    bool       sockOpen;
    nng_msg   *rxmsg;     /* last received message; freed on next recv/close      */
    nng_pipe   rxpipe;    /* pipe the last message arrived on (peer TLS identity) */
};

/* Map an nng return code to solariStatus for send/recv paths. */
static solariStatus mapTransient(int rv)
{
    switch (rv) {
    case 0:            return SOLARI_OK;
    case NNG_ETIMEDOUT:
    case NNG_EAGAIN:   return ERR_CONN_RETRY;
    default:           return ERR_CONN_FATAL;
    }
}

static int openSocketFor(solariPattern p, nng_socket *s)
{
    switch (p) {
    case SOLARI_PATTERN_PUSH:     return nng_push0_open(s);
    case SOLARI_PATTERN_PULL:     return nng_pull0_open(s);
    case SOLARI_PATTERN_SURVEYOR: return nng_surveyor0_open(s);
    case SOLARI_PATTERN_RESPOND:  return nng_respondent0_open(s);
    case SOLARI_PATTERN_PUB:      return nng_pub0_open(s);
    case SOLARI_PATTERN_SUB:      return nng_sub0_open(s);
    case SOLARI_PATTERN_REQ:      return nng_req0_open(s);
    case SOLARI_PATTERN_REP:      return nng_rep0_open(s);
    default:                      return NNG_EINVAL;
    }
}

/* Read an entire PEM file into a NUL-terminated heap buffer (caller frees).
 * nng's own-cert API takes the PEM *contents* as ASCIIZ strings, not a path
 * (unlike nng_tls_config_ca_file), so the cert/key files are slurped here. */
static solariStatus slurpPem(const char *path, char **out)
{
    FILE  *f;
    long   sz;
    char  *buf;
    size_t rd;

    *out = NULL;
    if (!path) return ERR_INVALID_ARG;

    f = fopen(path, "rb");
    if (!f) return ERR_TLS;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) { fclose(f); return ERR_TLS; }

    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return ERR_PLATFORM; }

    rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(buf); return ERR_TLS; }

    buf[sz] = '\0';
    *out = buf;
    return SOLARI_OK;
}

/* Build an nng TLS config from the opts and attach it to a dialer/listener.
 * Returns SOLARI_OK / ERR_TLS / ERR_INVALID_ARG. */
static solariStatus buildTlsConfig(const solariConnOpts *opts, nng_tls_config **outCfg,
                                   bool asClient)
{
    nng_tls_config *cfg = NULL;
    int rv;

    rv = nng_tls_config_alloc(&cfg, asClient ? NNG_TLS_MODE_CLIENT : NNG_TLS_MODE_SERVER);
    if (rv != 0) return ERR_TLS;

    if (opts->caFile) {
        rv = nng_tls_config_ca_file(cfg, opts->caFile);
        if (rv != 0) { nng_tls_config_free(cfg); return ERR_TLS; }
    }
    if (opts->certFile && opts->keyFile) {
        char        *certPem = NULL, *keyPem = NULL;
        solariStatus st = slurpPem(opts->certFile, &certPem);
        if (st == SOLARI_OK) st = slurpPem(opts->keyFile, &keyPem);
        if (st == SOLARI_OK)
            rv = nng_tls_config_own_cert(cfg, certPem, keyPem, NULL);
        free(certPem);
        free(keyPem);
        if (st != SOLARI_OK) { nng_tls_config_free(cfg); return st; }
        if (rv != 0)         { nng_tls_config_free(cfg); return ERR_TLS; }
    }
    /* Require mutual auth: peers must present a cert chaining to our CA. */
    rv = nng_tls_config_auth_mode(cfg, NNG_TLS_AUTH_MODE_REQUIRED);
    if (rv != 0) { nng_tls_config_free(cfg); return ERR_TLS; }

    *outCfg = cfg;
    return SOLARI_OK;
}

static void applyTimeouts(nng_socket s, const solariConnOpts *opts)
{
    if (opts->sendTimeoutMs > 0)
        nng_socket_set_ms(s, NNG_OPT_SENDTIMEO, opts->sendTimeoutMs);
    if (opts->recvTimeoutMs > 0)
        nng_socket_set_ms(s, NNG_OPT_RECVTIMEO, opts->recvTimeoutMs);
}

/* Extract the host of "scheme://host:port/..." into out (NUL-terminated). Used
 * to set the TLS server name on a dialer: when we attach our own nng_tls_config,
 * nng does not auto-populate the verification hostname, so without this mbedTLS
 * flags a CN/SAN mismatch and aborts the handshake under AUTH_MODE_REQUIRED. */
static void urlHost(const char *url, char *out, size_t cap)
{
    const char *p, *e;
    size_t n;

    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!url) return;

    p = strstr(url, "://");
    p = p ? p + 3 : url;
    for (e = p; *e && *e != ':' && *e != '/'; e++) { }

    n = (size_t)(e - p);
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

/* Common open path; dial=true builds a dialer, otherwise a listener. */
static solariStatus openCommon(const solariConnOpts *opts, solariConn **out, bool dial)
{
    solariConn *c;
    int rv;

    if (!opts || !opts->url || !out) return ERR_INVALID_ARG;

    c = (solariConn *)calloc(1, sizeof *c);
    if (!c) return ERR_PLATFORM;

    rv = openSocketFor(opts->pattern, &c->sock);
    if (rv != 0) {
        solariLogf(SOLARI_LOG_ERROR, "net: socket open failed: %s", nng_strerror(rv));
        free(c);
        return ERR_CONN_FATAL;
    }
    c->sockOpen = true;
    applyTimeouts(c->sock, opts);

    if (opts->useTls) {
        nng_tls_config *tls = NULL;
        solariStatus st = buildTlsConfig(opts, &tls, dial);
        if (st != SOLARI_OK) { solariConnClose(c); return st; }

        if (dial) {
            nng_dialer d;
            char host[256];
            rv = nng_dialer_create(&d, c->sock, opts->url);
            if (rv == 0) rv = nng_dialer_set_ptr(d, NNG_OPT_TLS_CONFIG, tls);
            if (rv == 0) {
                /* Tell mbedTLS which name to verify the server cert against. */
                urlHost(opts->url, host, sizeof host);
                if (host[0])
                    (void)nng_dialer_set_string(d, NNG_OPT_TLS_SERVER_NAME, host);
                rv = nng_dialer_start(d, 0);
            }
        } else {
            nng_listener l;
            rv = nng_listener_create(&l, c->sock, opts->url);
            if (rv == 0) rv = nng_listener_set_ptr(l, NNG_OPT_TLS_CONFIG, tls);
            if (rv == 0) rv = nng_listener_start(l, 0);
        }
        nng_tls_config_free(tls);   /* endpoint holds its own reference */
        if (rv != 0) {
            solariLogf(SOLARI_LOG_ERROR, "net: TLS %s failed: %s",
                       dial ? "dial" : "listen", nng_strerror(rv));
            solariConnClose(c);
            return ERR_TLS;
        }
    } else {
        rv = dial ? nng_dial(c->sock, opts->url, NULL, 0)
                  : nng_listen(c->sock, opts->url, NULL, 0);
        if (rv != 0) {
            solariLogf(SOLARI_LOG_ERROR, "net: %s %s failed: %s",
                       dial ? "dial" : "listen", opts->url, nng_strerror(rv));
            solariConnClose(c);
            return ERR_CONN_FATAL;
        }
    }

    *out = c;
    return SOLARI_OK;
}

solariStatus solariConnOpen(const solariConnOpts *opts, solariConn **out)
{
    return openCommon(opts, out, true);
}

solariStatus solariConnListen(const solariConnOpts *opts, solariConn **out)
{
    return openCommon(opts, out, false);
}

solariStatus solariConnSend(solariConn *c, const uint8_t *frame, size_t len)
{
    int rv;
    if (!c || !frame || len == 0) return ERR_INVALID_ARG;
    /* Copy semantics: nng_send without NNG_FLAG_ALLOC does not take ownership. */
    rv = nng_send(c->sock, (void *)frame, len, 0);
    if (rv != 0 && rv != NNG_ETIMEDOUT && rv != NNG_EAGAIN) {
        solariLogf(SOLARI_LOG_WARN, "net: send failed: %s", nng_strerror(rv));
    }
    return mapTransient(rv);
}

solariStatus solariConnRecv(solariConn *c, const uint8_t **frame, size_t *len)
{
    int rv;
    nng_msg *msg = NULL;

    if (!c || !frame || !len) return ERR_INVALID_ARG;

    /* release the message handed out by the previous recv. We receive into an
     * nng_msg (rather than NNG_FLAG_ALLOC) so the pipe the frame arrived on - and
     * thus the sender's TLS-verified CN - stays recoverable for role auth. */
    if (c->rxmsg) { nng_msg_free(c->rxmsg); c->rxmsg = NULL; }

    rv = nng_recvmsg(c->sock, &msg, 0);
    if (rv != 0) {
        *frame = NULL; *len = 0;
        return mapTransient(rv);
    }
    c->rxmsg  = msg;
    c->rxpipe = nng_msg_get_pipe(msg);
    *frame = (const uint8_t *)nng_msg_body(msg);
    *len   = nng_msg_len(msg);
    return SOLARI_OK;
}

solariStatus solariConnPeerCn(solariConn *c, char *buf, size_t cap)
{
    char *cn = NULL;
    int   rv;

    if (buf && cap > 0) buf[0] = '\0';
    if (!c || !buf || cap == 0) return ERR_INVALID_ARG;
    if (c->rxmsg == NULL) return ERR_CONN_RETRY;   /* no frame received yet */

    /* The peer CN is a property of the pipe the last message arrived on. On a
     * plaintext transport (or an unverified peer) nng has no CN to report. */
    rv = nng_pipe_get_string(c->rxpipe, NNG_OPT_TLS_PEER_CN, &cn);
    if (rv != 0 || cn == NULL) return ERR_TLS;

    /* Normalize: some TLS engines return the whole subject DN (optionally
     * "CN=<value>, O=..., OU=...") rather than the bare CN value. Strip a leading
     * "CN=" and cut at the first RDN separator so callers get just the CN. */
    {
        const char *s = cn;
        size_t      n;
        if (strncmp(s, "CN=", 3) == 0) s += 3;
        n = strcspn(s, ",");                 /* CN value ends at the first comma */
        if (n >= cap) n = cap - 1;
        memcpy(buf, s, n);
        buf[n] = '\0';
    }
    nng_strfree(cn);
    return SOLARI_OK;
}

void solariConnClose(solariConn *c)
{
    if (!c) return;
    if (c->rxmsg) nng_msg_free(c->rxmsg);
    if (c->sockOpen) nng_close(c->sock);
    free(c);
}
