/*
 * solariReporter.h - shared push-or-spool report transport (sec 6.5, 6.6).
 *
 * The fault-tolerance pattern common to the client and the monitor: dial the
 * active server (primary, then failover), frame outgoing messages stamped with
 * this node's id and a monotonic seqNo, and either push them live or - when the
 * server is unreachable - durably spool and replay them in order on reconnect.
 *
 * Both solariClient and solariMonitor build their reporting on this one module
 * so the transport, retry, and store-and-forward logic lives in exactly one
 * place. Compiled into libsolari when the nng transport and the SQLite spool are
 * present (SOLARI_WITH_IO && SOLARI_WITH_SQLITE).
 *
 * Handle pattern: state lives in an opaque solariReporter threaded through calls.
 */
#ifndef SOLARI_REPORTER_H
#define SOLARI_REPORTER_H

#include "solari/solariCommon.h"

typedef struct solariReporter solariReporter;

typedef struct {
    const char *primaryUrl;     /* "tls+tcp://host:port" or "tcp://..."; required */
    const char *failoverUrl;    /* optional second server                        */
    bool        useTls;
    const char *caFile;         /* TLS material (NULL when not using TLS)         */
    const char *certFile;
    const char *keyFile;
    const char *spoolDb;        /* store-and-forward DB path; NULL/"" = no spool  */
    uint64_t    nodeId;         /* stamped into every frame's sourceNodeId        */
} solariReporterOpts;

/* Open a reporter (opening the spool if spoolDb is set). Does not connect.
 * Copies the option strings, so they need not outlive the call. */
solariStatus solariReporterOpen(const solariReporterOpts *opts, solariReporter **out);
/* Close the connection and spool; free the handle (NULL-safe). */
void solariReporterClose(solariReporter *r);

/* Best-effort connect to primary, then failover. SOLARI_OK if a dialer exists
 * (nng connects asynchronously), ERR_CONN_RETRY if no URL is usable. */
solariStatus solariReporterConnect(solariReporter *r);
/* Whether a live dialer currently exists. */
bool         solariReporterConnected(const solariReporter *r);

/* Frame a TLV payload into a complete SCP frame, stamping header fields
 * (sourceNodeId, sendTimeUnixMs, and the next seqNo). payload may be NULL iff
 * payloadLen == 0 (header-only frames such as heartbeats). */
solariStatus solariReporterFrame(solariReporter *r, uint8_t msgType, uint8_t flags,
                                 const uint8_t *payload, size_t payloadLen,
                                 uint16_t tlvCount,
                                 uint8_t *out, size_t cap, size_t *outLen);

/* Send a prebuilt frame durably: push if connected; on transient/offline
 * failure, spool it (when a spool exists). On a successful live send,
 * opportunistically drains the spool. Returns SOLARI_OK when the frame was sent
 * OR spooled; ERR_CONN_RETRY when offline with no spool. */
solariStatus solariReporterSend(solariReporter *r, const uint8_t *frame, size_t len);

/* Send a frame best-effort only - never spooled. For low-value periodic traffic
 * (heartbeats, discovery/topology adverts) that is simply re-sent next cycle.
 * Returns ERR_CONN_RETRY if not connected. */
solariStatus solariReporterSendEphemeral(solariReporter *r, const uint8_t *frame, size_t len);

/* Replay queued frames to the server in order (peek -> send -> ack, or nack and
 * stop on failure). Bounded per call. No-op if not connected or no spool. */
solariStatus solariReporterDrain(solariReporter *r);

/* Current spool depth (0 if no spool). */
size_t solariReporterSpoolDepth(const solariReporter *r);

#endif /* SOLARI_REPORTER_H */
