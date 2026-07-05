/*
 * clientControl.h - the client's control-plane receive + apply path (§7.3, §9.1).
 *
 * The server steers a live client by PUBLISHING SCP_MSG_CONTROL directives
 * (CTRL_SET_CONFIG / CTRL_PROVISION carrying a JSON config blob + a target
 * epoch) on its fleet PUB channel. The client SUBscribes to that channel,
 * applies addressed directives, persists the applied state, and answers with
 * an SCP_MSG_CONTROL_RESULT over its existing ingest (push) connection so the
 * server folds the applied epoch into convergence (serverControlOnResult).
 *
 * Layering mirrors serverControl.c: all parse/apply/decide logic is pure over
 * caller memory (no sockets, no files) and unit-testable; the transport glue
 * (SUB dial + poll + reply send) and the tiny state file are separate and the
 * transport is compiled only with CLIENT_WITH_REPORTING.
 */
#ifndef SOLARI_CLIENT_CONTROL_H
#define SOLARI_CLIENT_CONTROL_H

#include "client.h"

/* Result-payload scratch: verb + errCode + appliedEpoch TLVs. */
#define CLIENT_CTRL_RESULT_CAP 64

/* Control-plane runtime state: the epoch this node has applied. Directives at
 * or below it are acknowledged as already-converged, never re-applied. */
typedef struct {
    uint64_t appliedEpoch;
} clientControlState;

/* Outcome of one handled directive, for the I/O glue. */
typedef struct {
    bool           wantReply;   /* a CONTROL_RESULT payload was built        */
    bool           applied;     /* cfg changed: persist blob + epoch         */
    uint8_t        verb;        /* directive verb (0 if unparseable)         */
    const uint8_t *blob;        /* applied config blob (aliases the inbound  */
    uint16_t       blobLen;     /*   payload; valid until the next recv)     */
} clientControlOutcome;

/* ---- pure logic (compiled in every build) ---- */

/* Apply a JSON config blob onto cfg. Validates the whole document first, so a
 * malformed blob is a no-op error. Keys applied (all optional):
 *   sampleIntervalSec, watchdogIntervalSec  - numbers (clamped to sane ranges)
 *   processes  (alias procs)                - array of process names
 *   logfiles   (alias logs)                 - array of "path[ : regex]"
 * Unknown keys are ignored (§5.6 spirit). Pure; no I/O. */
solariStatus clientControlApplyBlob(clientConfig *cfg, const char *json, size_t len);

/* Handle one inbound SCP_MSG_CONTROL payload addressed over the broadcast PUB
 * channel: parse (tolerantly - wire input is untrusted), drop frames addressed
 * to another node, enforce epoch monotonicity (targetEpoch <= appliedEpoch is
 * acknowledged as converged without re-applying), apply CTRL_SET_CONFIG /
 * CTRL_PROVISION blobs onto cfg, and build the CONTROL_RESULT payload
 * (TLV_CTRL_VERB + TLV_ERROR_CODE + TLV_CTRL_TARGET_EPOCH = applied epoch)
 * into resultBuf. Pure; the caller sends the reply and persists state.
 * Returns the directive's own outcome (SOLARI_OK also for a clean drop). */
solariStatus clientControlHandle(clientControlState *st, clientConfig *cfg,
                                 uint64_t selfNodeId,
                                 const uint8_t *payload, size_t payloadLen,
                                 uint8_t *resultBuf, size_t resultCap,
                                 size_t *resultLen, uint16_t *resultTlvCount,
                                 clientControlOutcome *out);

/* ---- applied-state persistence (file I/O only; no sockets) ---- */

/* Resolve the state-file path for cfg into out ("" when persistence is off:
 * no ctrlStateFile and no spoolDb to derive from). */
void clientControlStatePath(const clientConfig *cfg, char *out, size_t cap);

/* Load the persisted applied state ("<epoch>\n<blob>"): sets st->appliedEpoch
 * (floored at cfg->configEpoch) and re-applies the stored blob when its epoch
 * is newer than the .conf's, so a restart keeps pushed config. Missing or
 * unreadable state degrades to the .conf (SOLARI_OK). */
solariStatus clientControlStateLoad(clientControlState *st, clientConfig *cfg);

/* Persist the applied state atomically-ish (write + rename). No-op (OK) when
 * persistence is off. */
solariStatus clientControlStateSave(const clientConfig *cfg,
                                    const clientControlState *st,
                                    const uint8_t *blob, uint16_t blobLen);

/* ---- transport glue (SUB dial + poll + reply) ---- */
#ifdef CLIENT_WITH_REPORTING

#include "solari/solariNet.h"

typedef struct {
    solariConn *sub;                    /* SUB dialer to the server PUB channel */
    char        subUrl[CLIENT_URL_MAX];
} clientControlIo;

/* Dial the server PUB channel (cfg->subUrl, or primaryUrl re-pointed at the
 * standard pub port 7703) as a subscriber. Best-effort: on failure io->sub is
 * NULL and polling degrades to plain sleeping. */
solariStatus clientControlOpen(const clientConfig *cfg, clientControlIo *io);
void         clientControlClose(clientControlIo *io);

/* Service the control channel for ~waitMs (this doubles as the sample-loop
 * sleep so directives are handled promptly WITHOUT disturbing the sample
 * cadence): receive published frames, dispatch SCP_MSG_CONTROL through
 * clientControlHandle, persist applied state, and push the CONTROL_RESULT
 * (correlationId = the directive frame's seqNo) over ctx's reporter - the
 * server consumes it on the ingest channel (serverControlOnResult). Never
 * blocks materially past waitMs (worst case one ~250ms recv slice). */
void clientControlPoll(clientControlIo *io, clientContext *ctx,
                       clientConfig *cfg, clientControlState *st,
                       uint32_t waitMs);

#endif /* CLIENT_WITH_REPORTING */

#endif /* SOLARI_CLIENT_CONTROL_H */
