/*
 * solariNet.h - tier-to-tier transport (section 6.5).
 *
 * A thin wrapper over nng (nanomsg-next-gen) carrying SCP frames, with TLS
 * provided by mbedTLS as nng's TLS engine. The wrapper exposes exactly the
 * scalability patterns SolariNet needs and hides nng's socket/dialer/listener
 * and TLS-config plumbing behind one opaque handle.
 *
 * Pattern map (section 2 data flow):
 *   PUSH/PULL            client & monitor reports -> server ingest
 *   SURVEYOR/RESPONDENT  server demands an immediate round from the fleet
 *   PUB/SUB              server -> dashboards / notifiers
 *   REQ/REP             who-is-primary, control acks
 *
 * Handle pattern: state lives in a heap solariConn the caller threads through
 * calls; there are no globals and no hidden statics.
 */
#ifndef SOLARI_NET_H
#define SOLARI_NET_H

#include "solari/solariCommon.h"

typedef struct solariConn solariConn;

typedef enum {
    SOLARI_PATTERN_PUSH,     /* client/monitor -> server ingest   */
    SOLARI_PATTERN_PULL,     /* server ingest sink                */
    SOLARI_PATTERN_SURVEYOR, /* server -> fleet demand            */
    SOLARI_PATTERN_RESPOND,  /* node answers surveys              */
    SOLARI_PATTERN_PUB,      /* server -> dashboards/notifiers    */
    SOLARI_PATTERN_SUB,
    SOLARI_PATTERN_REQ,      /* who-is-primary, control acks      */
    SOLARI_PATTERN_REP
} solariPattern;

typedef struct {
    const char   *url;          /* "tls+tcp://host:port" or "tcp://host:port" */
    solariPattern pattern;
    bool          useTls;
    const char   *caFile;       /* internal CA root (verify peers)            */
    const char   *certFile;     /* this node's certificate                    */
    const char   *keyFile;      /* this node's private key                    */
    int           dialTimeoutMs;
    int           sendTimeoutMs;
    int           recvTimeoutMs;
} solariConnOpts;

/* Open an outbound connection (dialer) for the given pattern. *out gets the
 * handle. Returns ERR_TLS on TLS setup/verify failure, ERR_CONN_FATAL on an
 * unrecoverable transport error, ERR_INVALID_ARG on bad opts. */
solariStatus solariConnOpen(const solariConnOpts *opts, solariConn **out);

/* Bind a listener for the given pattern (server side). Same error contract. */
solariStatus solariConnListen(const solariConnOpts *opts, solariConn **out);

/* Send one fully-built SCP frame. Honors sendTimeoutMs. On a transient failure
 * (peer not currently reachable, would-block) returns ERR_CONN_RETRY so the
 * caller knows to spool and retry; ERR_CONN_FATAL on an unrecoverable error. */
solariStatus solariConnSend(solariConn *c, const uint8_t *frame, size_t len);

/* Receive one frame into a library-managed buffer; *frame is valid until the
 * next recv on the same conn (do not free). Blocks up to recvTimeoutMs and
 * returns ERR_CONN_RETRY on timeout with nothing available. */
solariStatus solariConnRecv(solariConn *c, const uint8_t **frame, size_t *len);

/* Common name (CN) of the TLS-verified peer that sent the frame returned by the
 * most recent successful solariConnRecv on this conn. Writes a NUL-terminated
 * string into buf (truncated to cap). Lets the server derive the presenting
 * node's role from its certificate (serverIngestValidate). Returns
 * ERR_INVALID_ARG on bad args, ERR_CONN_RETRY if no frame has been received yet,
 * or ERR_TLS when the peer presented no verifiable certificate (e.g. a non-TLS
 * transport). buf is set to "" in every non-OK case. The CN reflects the last
 * recv and is invalidated by the next recv/close on this conn. */
solariStatus solariConnPeerCn(solariConn *c, char *buf, size_t cap);

/* Close the connection and release the handle (NULL-safe). */
void solariConnClose(solariConn *c);

#endif /* SOLARI_NET_H */
