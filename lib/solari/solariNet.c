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
    void      *rxbuf;     /* last NNG_FLAG_ALLOC buffer; freed on next recv/close */
    size_t     rxlen;
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
        rv = nng_tls_config_own_cert(cfg, opts->certFile, opts->keyFile, NULL);
        if (rv != 0) { nng_tls_config_free(cfg); return ERR_TLS; }
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
            rv = nng_dialer_create(&d, c->sock, opts->url);
            if (rv == 0) rv = nng_dialer_set_ptr(d, NNG_OPT_TLS_CONFIG, tls);
            if (rv == 0) rv = nng_dialer_start(d, 0);
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
    void *buf = NULL;
    size_t sz = 0;

    if (!c || !frame || !len) return ERR_INVALID_ARG;

    /* free any buffer handed out by the previous recv */
    if (c->rxbuf) { nng_free(c->rxbuf, c->rxlen); c->rxbuf = NULL; c->rxlen = 0; }

    rv = nng_recv(c->sock, &buf, &sz, NNG_FLAG_ALLOC);
    if (rv != 0) {
        *frame = NULL; *len = 0;
        return mapTransient(rv);
    }
    c->rxbuf = buf;
    c->rxlen = sz;
    *frame = (const uint8_t *)buf;
    *len = sz;
    return SOLARI_OK;
}

void solariConnClose(solariConn *c)
{
    if (!c) return;
    if (c->rxbuf) nng_free(c->rxbuf, c->rxlen);
    if (c->sockOpen) nng_close(c->sock);
    free(c);
}
