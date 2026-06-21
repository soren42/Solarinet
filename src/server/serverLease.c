/*
 * serverLease.c - primary/standby failover lease (§9.2).
 *
 * SolariNet runs one or more server instances against a shared MariaDB. Exactly
 * one of them must own the authoritative write path (ingest sink, control REP,
 * alert PUB) at a time; the rest stand by, read-only, ready to take over. There
 * is no external coordinator: the election IS a single conditional UPDATE of the
 * shared `serverLease` row (serverDbLeaseClaim), so whoever wins the row wins the
 * lease (§2, §9.2). This file is the state machine wrapped around that primitive.
 *
 * Three public entry points:
 *
 *   serverLeaseTick - called every cfg->leaseRenewSec. Runs serverDbLeaseClaim
 *     (claim if the lease is free/expired, renew if we already hold it). On a win
 *     it drives the role to SRV_ACTIVE; on a loss it drives to SRV_STANDBY. The
 *     critical failover contract: if we were ACTIVE and the claim call FAILED
 *     (DB error - we can no longer prove we hold the lease), we must assume we
 *     lost it, demote to read-only, and return ERR_LEASE_LOST so the caller stops
 *     writing. A clean "someone else holds it" is NOT an error - it returns
 *     SOLARI_OK with *state == SRV_STANDBY.
 *
 *   serverApplyRole - the idempotent transport switch. STANDBY->ACTIVE binds the
 *     ingest (PULL), control (REP) and alert (PUB) listeners; ACTIVE->STANDBY
 *     tears them back down so a demoted server stops accepting fleet traffic.
 *     Safe to call repeatedly with the current role (no-op).
 *
 *   serverLeaseAnswerWhoIsPrimary - builds the PRIMARY_IS (0x41) reply payload in
 *     answer to a WHO_IS_PRIMARY (0x40) query, naming the current lease holder
 *     (from serverDbLeaseRead) and the lease epoch so a node can find and pin the
 *     active server across a failover.
 *
 * The lease-state decision (serverLeaseDecide) and the PRIMARY_IS payload builder
 * are factored into small static helpers with no I/O so they are unit-testable
 * without a live MariaDB or any sockets; only serverApplyRole and the two DB
 * calls touch the outside world.
 */
#include "server.h"

#include "solari/solariNet.h"
#include "solari/solariTlv.h"
#include "solari/solariLog.h"

#include <string.h>

/* ------------------------------------------------------------- role helpers */

/* Short label for a role, for log lines only. */
static const char *serverLeaseRoleName(serverRoleState role)
{
    return (role == SRV_ACTIVE) ? "active" : "standby";
}

/* The pure state-machine kernel: given where we are (wasActive), what the
 * conditional UPDATE returned (claimRc / won), decide the new role and the
 * status to surface. Separated from all I/O so the failover policy is testable.
 *
 *   - claim succeeded & won            -> ACTIVE, SOLARI_OK
 *   - claim succeeded & lost           -> STANDBY, SOLARI_OK (someone else holds)
 *   - claim FAILED while we were ACTIVE-> STANDBY, ERR_LEASE_LOST (must demote)
 *   - claim FAILED while we were standby-> STANDBY, claimRc (stay down, surface
 *                                          the DB error so the caller can retry)
 *
 * *newRole is always written; the return value is the status to propagate. */
static solariStatus serverLeaseDecide(bool wasActive, solariStatus claimRc,
                                      bool won, serverRoleState *newRole)
{
    if (claimRc == SOLARI_OK) {
        *newRole = won ? SRV_ACTIVE : SRV_STANDBY;
        return SOLARI_OK;
    }

    /* The claim/renew did not complete. We can no longer assert we hold the
     * lease, so we must fall back to read-only standby. */
    *newRole = SRV_STANDBY;
    return wasActive ? ERR_LEASE_LOST : claimRc;
}

/* --------------------------------------------------------- transport binding */

/* Populate a solariConnOpts listener spec from cfg: copy the TLS material the
 * way every other subsystem does (empty path -> NULL so the transport treats it
 * as "unset"). The url and pattern are filled by the caller. */
static void serverLeaseListenOpts(const serverConfig *cfg, const char *url,
                                  solariPattern pattern, solariConnOpts *o)
{
    memset(o, 0, sizeof *o);
    o->url           = url;
    o->pattern       = pattern;
    o->useTls        = cfg->useTls;
    o->caFile        = cfg->caFile[0]   ? cfg->caFile   : NULL;
    o->certFile      = cfg->certFile[0] ? cfg->certFile : NULL;
    o->keyFile       = cfg->keyFile[0]  ? cfg->keyFile  : NULL;
    o->recvTimeoutMs = 100;   /* non-blocking-ish drain for the main loop */
    o->sendTimeoutMs = 500;
}

/* Bind one listener into *slot if it is not already bound and a url is set.
 * Idempotent: an already-bound slot or an empty url is a no-op success. A bind
 * failure is logged and propagated so serverApplyRole can roll back. */
static solariStatus serverLeaseBindOne(const serverConfig *cfg, const char *url,
                                       solariPattern pattern, const char *what,
                                       solariConn **slot)
{
    solariConnOpts o;
    solariStatus   rc;

    if (*slot != NULL) return SOLARI_OK;        /* already bound (idempotent) */
    if (!url || url[0] == '\0') return SOLARI_OK;/* endpoint not configured    */

    serverLeaseListenOpts(cfg, url, pattern, &o);
    rc = solariConnListen(&o, slot);
    if (rc != SOLARI_OK) {
        *slot = NULL;
        solariLogf(SOLARI_LOG_ERROR, "lease: bind %s (%s) failed: %s",
                   what, url, solariStrError(rc));
        return rc;
    }
    solariLogf(SOLARI_LOG_INFO, "lease: bound %s on %s", what, url);
    return SOLARI_OK;
}

/* Close one listener if bound, NULLing the slot. Idempotent. */
static void serverLeaseUnbindOne(solariConn **slot, const char *what)
{
    if (*slot == NULL) return;
    solariConnClose(*slot);
    *slot = NULL;
    solariLogf(SOLARI_LOG_INFO, "lease: unbound %s", what);
}

/* Tear down all three authoritative listeners (used on demotion and on a
 * partial-bind rollback). */
static void serverLeaseUnbindAll(serverContext *ctx)
{
    serverLeaseUnbindOne(&ctx->ingest,  "ingest");
    serverLeaseUnbindOne(&ctx->control, "control");
    serverLeaseUnbindOne(&ctx->pub,     "pub");
}

/* ------------------------------------------------------------- serverApplyRole */

solariStatus serverApplyRole(serverContext *ctx, serverRoleState newState)
{
    const serverConfig *cfg;
    solariStatus rc;

    if (!ctx || !ctx->cfg) return ERR_INVALID_ARG;
    cfg = ctx->cfg;

    if (newState == SRV_ACTIVE) {
        /* STANDBY->ACTIVE (or re-affirming ACTIVE): bind the ingest sink, the
         * control REP (who-is-primary + control acks) and the alert PUB. Bind
         * order: ingest, control, pub. Any failure rolls the whole set back so
         * we never come up half-active. */
        rc = serverLeaseBindOne(cfg, cfg->ingestUrl,  SOLARI_PATTERN_PULL,
                                "ingest", &ctx->ingest);
        if (rc != SOLARI_OK) { serverLeaseUnbindAll(ctx); return rc; }

        rc = serverLeaseBindOne(cfg, cfg->controlUrl, SOLARI_PATTERN_REP,
                                "control", &ctx->control);
        if (rc != SOLARI_OK) { serverLeaseUnbindAll(ctx); return rc; }

        rc = serverLeaseBindOne(cfg, cfg->pubUrl,     SOLARI_PATTERN_PUB,
                                "pub", &ctx->pub);
        if (rc != SOLARI_OK) { serverLeaseUnbindAll(ctx); return rc; }

        if (ctx->role != SRV_ACTIVE)
            solariLogf(SOLARI_LOG_INFO, "lease: role standby->active (nodeId=0x%llx)",
                       (unsigned long long)ctx->nodeId);
        ctx->role = SRV_ACTIVE;
        return SOLARI_OK;
    }

    /* ACTIVE->STANDBY (or re-affirming STANDBY): unbind everything, go
     * read-only. Idempotent - unbinding an unbound slot is a no-op. */
    if (ctx->role == SRV_ACTIVE)
        solariLogf(SOLARI_LOG_INFO, "lease: role active->standby (nodeId=0x%llx)",
                   (unsigned long long)ctx->nodeId);
    serverLeaseUnbindAll(ctx);
    ctx->role = SRV_STANDBY;
    return SOLARI_OK;
}

/* -------------------------------------------------------------- serverLeaseTick */

solariStatus serverLeaseTick(serverContext *ctx, serverRoleState *state,
                             uint64_t *epoch)
{
    bool            wasActive;
    bool            won = false;
    uint64_t        leaseEpoch = 0;
    serverRoleState newRole = SRV_STANDBY;
    solariStatus    claimRc, decideRc, applyRc;

    if (state) *state = SRV_STANDBY;        /* defensive pre-zero */
    if (epoch) *epoch = 0;
    if (!ctx || !ctx->cfg || !ctx->db) return ERR_INVALID_ARG;

    wasActive = (ctx->role == SRV_ACTIVE);

    /* The election: claim or renew the single lease row. *won reflects whether
     * we hold the lease afterward; *epoch advances on a fresh claim. */
    claimRc = serverDbLeaseClaim(ctx->db, ctx->nodeId, ctx->cfg->leaseTtlSec,
                                 &won, &leaseEpoch);

    decideRc = serverLeaseDecide(wasActive, claimRc, won, &newRole);

    if (claimRc != SOLARI_OK) {
        solariLogf(wasActive ? SOLARI_LOG_ERROR : SOLARI_LOG_WARN,
                   "lease: claim/renew failed (was %s): %s%s",
                   serverLeaseRoleName(wasActive ? SRV_ACTIVE : SRV_STANDBY),
                   solariStrError(claimRc),
                   wasActive ? " -> demoting (ERR_LEASE_LOST)" : "");
    }

    /* Drive the transport to match the decided role. serverApplyRole is
     * idempotent, so a steady-state renew that keeps us ACTIVE is a cheap no-op.
     * A demotion always tears the listeners down. */
    applyRc = serverApplyRole(ctx, newRole);
    if (applyRc != SOLARI_OK) {
        /* We could not bind the active listeners: we are not safely primary.
         * Force read-only and report the failure rather than the claim status. */
        solariLogf(SOLARI_LOG_ERROR, "lease: applyRole(%s) failed: %s -> standby",
                   serverLeaseRoleName(newRole), solariStrError(applyRc));
        serverApplyRole(ctx, SRV_STANDBY);
        if (state) *state = SRV_STANDBY;
        if (epoch) *epoch = ctx->epoch;
        return applyRc;
    }

    /* On a held lease, adopt the epoch the row reports (advances on a fresh
     * claim from a different/expired holder). On standby, keep whatever epoch we
     * last knew so WHO_IS_PRIMARY answers stay coherent until a real read. */
    if (newRole == SRV_ACTIVE) {
        if (leaseEpoch != ctx->epoch && ctx->epoch != 0)
            solariLogf(SOLARI_LOG_INFO, "lease: epoch %llu->%llu",
                       (unsigned long long)ctx->epoch,
                       (unsigned long long)leaseEpoch);
        ctx->epoch = leaseEpoch;
    }

    if (state) *state = newRole;
    if (epoch) *epoch = ctx->epoch;
    return decideRc;
}

/* ----------------------------------------------- serverLeaseAnswerWhoIsPrimary */

/* TLV type carrying the lease holder's stable node id inside a PRIMARY_IS body.
 * The wire contract (solariTlv.h) reserves 0x01xx for identity/session and lets
 * readers skip unknown types, so we mint a holder-id TLV in that category. It is
 * defined here (not in the shared header) because only this answer emits it and
 * only the failover client reads it; if the integrator wants it promoted to a
 * named constant in solariTlv.h, see the contract note in the handoff. */
#define TLV_LEASE_HOLDER_ID 0x0105u   /* uint64: current lease holder node id */

/* Build the PRIMARY_IS (0x41) TLV payload into [out, out+cap):
 *   TLV_ROLE            (uint8)  = 3 (ROLE_SERVER) - the holder is a server
 *   TLV_LEASE_HOLDER_ID (uint64) = the current lease holder's node id (0 if none)
 *   TLV_CONFIG_EPOCH    (uint64) = the lease/failover epoch
 *   TLV_HOST_FQDN       (utf8)   = this server's fqdn, when known (best effort)
 *
 * A node parsing only the payload thus learns both who holds the lease and at
 * what epoch, independent of the carrying frame's sourceNodeId. Pure aside from
 * reading cfg; *outLen and *tlvCount are written on success. */
static solariStatus serverLeaseBuildPrimaryIs(const serverConfig *cfg,
                                              uint64_t holder, uint64_t epoch,
                                              uint8_t *out, size_t cap,
                                              size_t *outLen, uint16_t *tlvCount)
{
    solariTlvWriter w;
    solariStatus    rc;

    solariTlvWriterInit(&w, out, cap);

    rc = solariTlvAppendU8(&w, TLV_ROLE, (uint8_t)ROLE_SERVER);
    if (rc != SOLARI_OK) return rc;

    rc = solariTlvAppendU64(&w, TLV_LEASE_HOLDER_ID, holder);
    if (rc != SOLARI_OK) return rc;

    rc = solariTlvAppendU64(&w, TLV_CONFIG_EPOCH, epoch);
    if (rc != SOLARI_OK) return rc;

    if (cfg && cfg->hostFqdn[0]) {
        rc = solariTlvAppendStr(&w, TLV_HOST_FQDN, cfg->hostFqdn);
        if (rc != SOLARI_OK) return rc;
    }

    *outLen   = w.len;
    *tlvCount = w.count;
    return SOLARI_OK;
}

solariStatus serverLeaseAnswerWhoIsPrimary(serverContext *ctx,
                                           uint8_t *out, size_t cap,
                                           size_t *outLen, uint16_t *tlvCount)
{
    uint64_t     holder = 0, epoch = 0;
    solariStatus rc;

    if (outLen)   *outLen = 0;
    if (tlvCount) *tlvCount = 0;
    if (!ctx || !ctx->db || !out || !outLen || !tlvCount)
        return ERR_INVALID_ARG;

    /* Read the authoritative holder from the shared lease row. holder==0 means
     * the lease is currently unheld/expired - we still answer (with our own
     * view) so a node is not left without a target during a brief gap. */
    rc = serverDbLeaseRead(ctx->db, &holder, &epoch);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_WARN, "lease: read for WHO_IS_PRIMARY failed: %s",
                   solariStrError(rc));
        return rc;
    }

    if (holder == 0) {
        /* No durable holder yet: fall back to our local view so the reply is
         * still well-formed (epoch from ctx, holder = self only if we are the
         * active instance). */
        if (ctx->role == SRV_ACTIVE) holder = ctx->nodeId;
        if (epoch == 0) epoch = ctx->epoch;
    }

    rc = serverLeaseBuildPrimaryIs(ctx->cfg, holder, epoch,
                                   out, cap, outLen, tlvCount);
    if (rc != SOLARI_OK) {
        *outLen = 0;
        *tlvCount = 0;
        solariLogf(SOLARI_LOG_WARN, "lease: build PRIMARY_IS failed: %s",
                   solariStrError(rc));
        return rc;
    }

    solariLogf(SOLARI_LOG_DEBUG,
               "lease: PRIMARY_IS holder=0x%llx epoch=%llu (%u TLVs, %zu bytes)",
               (unsigned long long)holder, (unsigned long long)epoch,
               (unsigned)*tlvCount, *outLen);
    return SOLARI_OK;
}
