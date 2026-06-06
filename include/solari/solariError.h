/*
 * solariError.h - central error enum (Appendix A) and human-readable strings.
 *
 * Every libsolari and component function returns solariStatus. SOLARI_OK is 0;
 * all error codes are negative and defined here once so a single switch maps
 * any status to a stable message. Adding a code: append it, give it a unique
 * negative value, and extend solariStrError().
 */
#ifndef SOLARI_ERROR_H
#define SOLARI_ERROR_H

#include "solari/solariCommon.h"

enum solariError {
    /* SOLARI_OK == 0 is defined in solariCommon.h */
    ERR_INVALID_ARG       = -1,   /* Bad argument / null where non-null required. */
    ERR_BUFFER_FULL       = -2,   /* Write would overflow caller buffer.          */

    ERR_FRAME_INCOMPLETE  = -10,  /* Stream holds less than one full frame.       */
    ERR_FRAME_BAD_MAGIC   = -11,  /* Sentinel mismatch.                           */
    ERR_FRAME_BAD_CRC     = -12,  /* CRC-32 verification failed.                  */
    ERR_PROTO_VERSION     = -13,  /* Unsupported protocol version.                */

    ERR_TLV_END           = -20,  /* Reader reached end of payload (loop term).   */
    ERR_TLV_TRUNCATED     = -21,  /* TLV length exceeds remaining bytes.          */

    ERR_CONN_RETRY        = -30,  /* Transient send/recv failure; spool & retry.  */
    ERR_CONN_FATAL        = -31,  /* Unrecoverable transport error.               */
    ERR_TLS               = -32,  /* TLS handshake/verify failure.                */

    ERR_AUTH_ROLE         = -40,  /* Message type illegal for presenting role.    */

    ERR_DB                = -50,  /* Database error (detail logged).              */
    ERR_LEASE_LOST        = -51,  /* Active server failed to renew lease; demote. */

    ERR_UNKNOWN_MSG       = -60,  /* Unrecognized message type that must parse.   */

    ERR_PLATFORM          = -70   /* PAL call failed (OS-specific detail logged). */
};

/* Map a solariStatus to a short, stable, human-readable string. Never returns
 * NULL; unknown codes yield "unknown error". The returned pointer is to static
 * storage - do not free, valid for the program lifetime. */
const char *solariStrError(solariStatus st);

#endif /* SOLARI_ERROR_H */
