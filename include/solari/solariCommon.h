/*
 * solariCommon.h - shared types, return conventions, static-assert macro.
 *
 * All libsolari functions return solariStatus; SOLARI_OK (0) is success,
 * negative values are errors enumerated in solariError.h. Out-params are
 * written only on success unless documented otherwise.
 *
 * This header pulls in nothing heavier than the fixed-width integer and
 * boolean headers, so it is safe to include from every translation unit on
 * every target.
 */
#ifndef SOLARI_COMMON_H
#define SOLARI_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef int32_t solariStatus;   /* SOLARI_OK or a negative solariError code */

#define SOLARI_OK 0
#define SCP_PROTO_VERSION 1
#define SOLARI_MAX_FRAME (1u << 20)   /* 1 MiB hard cap on a single frame */

/* Fixed bound for the SCP frame header on the wire (§5.2). */
#define SOLARI_FRAME_HEADER_LEN 32

/* Collection bounds for the in-memory report structs (§6.4). Sized for an
 * intranet host; reports that would exceed these are truncated with a log. */
#define SOLARI_MAX_CORES   64
#define SOLARI_MAX_DISKS   24
#define SOLARI_MAX_IFACES  16
#define SOLARI_MAX_USB     16
#define SOLARI_MAX_PROCS   32
#define SOLARI_MAX_LOGS    16
#define SOLARI_MAX_PROBES  128

/* Compile-time assertion usable in C99 (no _Static_assert dependency).
 * Produces a negative-size typedef - and therefore a compile error - when
 * cond is false. The tag keeps the typedef name unique per use site. */
#define SOLARI_STATIC_ASSERT(cond, tag) \
    typedef char solariStaticAssert_##tag[(cond) ? 1 : -1]

/* Suppress unused-parameter warnings for documented-but-unused args. */
#define SOLARI_UNUSED(x) ((void)(x))

typedef enum {
    ROLE_CLIENT  = 1,
    ROLE_MONITOR = 2,
    ROLE_SERVER  = 3
} solariRole;

/* Error codes (Appendix A) are part of the common vocabulary: every function
 * returns a solariStatus drawn from them. Pulled in here so any translation
 * unit that includes solariCommon.h can name ERR_* directly. Include guards
 * make the mutual include between these two headers safe. */
#include "solari/solariError.h"

#endif /* SOLARI_COMMON_H */
