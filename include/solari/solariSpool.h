/*
 * solariSpool.h - SQLite-backed store-and-forward queue (section 6.6).
 *
 * When solariConnSend returns ERR_CONN_RETRY (server unreachable), the caller
 * enqueues the fully-built frame here. A drain loop later peeks the oldest ready
 * frame, sends it, then Acks on success or Nacks on failure (which bumps the
 * attempt count and applies exponential backoff before the frame becomes ready
 * again). This durable queue is what makes clients and monitors fault tolerant:
 * data survives a server outage and a process restart, then drains in order.
 *
 * Handle pattern: state (the DB connection + a reusable read buffer) lives in an
 * opaque solariSpool threaded through the calls.
 */
#ifndef SOLARI_SPOOL_H
#define SOLARI_SPOOL_H

#include "solari/solariCommon.h"

typedef struct solariSpool solariSpool;

/* Open (creating if needed) the spool database at dbPath. *out gets the handle.
 * Returns ERR_DB on failure (detail logged). */
solariStatus solariSpoolOpen(const char *dbPath, solariSpool **out);

/* Append a frame to the queue, ready to send immediately. Returns ERR_DB on
 * failure, ERR_INVALID_ARG on bad args. */
solariStatus solariSpoolPush(solariSpool *s, const uint8_t *frame, size_t len);

/* Fetch the oldest frame whose backoff has elapsed. On success the out-params
 * rowId, frame, and len describe it, and frame points into a buffer owned by the
 * spool, valid only until the next Peek/Push/Close on this handle (do not free).
 *
 * When the queue has no ready frame, returns SOLARI_OK with *frame == NULL,
 * *len == 0, *rowId == 0 (this is the normal "nothing to do" case, not an
 * error). Returns ERR_DB on a database error. */
solariStatus solariSpoolPeek(solariSpool *s, uint64_t *rowId,
                             const uint8_t **frame, size_t *len);

/* Remove a delivered frame from the queue (call after a successful send). */
solariStatus solariSpoolAck(solariSpool *s, uint64_t rowId);

/* Mark a failed send: increments the attempt count and pushes nextRetryAt out
 * by an exponential backoff so Peek will skip it until then. */
solariStatus solariSpoolNack(solariSpool *s, uint64_t rowId);

/* Total number of frames currently queued (ready or backing off). */
size_t solariSpoolDepth(solariSpool *s);

/* Close the database and release the handle (NULL-safe). */
void solariSpoolClose(solariSpool *s);

#endif /* SOLARI_SPOOL_H */
