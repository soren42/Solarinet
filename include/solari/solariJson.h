/*
 * solariJson.h - minimal, dependency-free JSON field extraction (§6, §13).
 *
 * The control plane carries per-node config as a small JSON blob
 * (nodeConfig.configBlob). Agents need to pull a handful of scalar/array
 * fields out of it defensively - the blob is untrusted wire input - without
 * vendoring a full JSON library into every static agent build (cJSON remains
 * an optional server-side overlay dep; see SOLARI_WITH_JSON).
 *
 * This is a bounded scanner, not a full parser:
 *   - solariJsonValidate does a structural sanity pass (balanced braces and
 *     brackets outside strings, sane string escapes, non-empty object). Apply
 *     paths call it first so malformed blobs are rejected as a whole (no
 *     partial application).
 *   - the getters locate the FIRST occurrence of the quoted key anywhere in
 *     the document (so both flat {"k":v} and nested {"sec":{"k":v}} layouts
 *     resolve) and decode the value that follows the colon.
 * All functions are pure over caller memory: no I/O, no allocation.
 */
#ifndef SOLARI_JSON_H
#define SOLARI_JSON_H

#include "solari/solariCommon.h"

/* Structural sanity check. Returns SOLARI_OK for a plausibly well-formed JSON
 * object/array; ERR_INVALID_ARG otherwise. Bounded single pass; never reads
 * past json[len-1]. */
solariStatus solariJsonValidate(const char *json, size_t len);

/* Read an unsigned integer value for `key` ("key": 123). Accepts a quoted
 * number too ("key": "123"). Returns SOLARI_OK and sets *out, or
 * ERR_UNKNOWN_MSG when the key is absent / the value is not a number. */
solariStatus solariJsonGetU64(const char *json, size_t len,
                              const char *key, uint64_t *out);

/* Read a string value for `key` into out (NUL-terminated, truncated to cap).
 * Common escapes (\" \\ \/ \n \t \r) are decoded; other escapes are copied
 * verbatim. ERR_UNKNOWN_MSG when absent or not a string. */
solariStatus solariJsonGetStr(const char *json, size_t len,
                              const char *key, char *out, size_t cap);

/* True if `key` exists with an array value. */
bool solariJsonHasArray(const char *json, size_t len, const char *key);

/* Read the index-th STRING element of the array value of `key` into out.
 * Non-string elements count toward the index but yield ERR_INVALID_ARG.
 * Returns SOLARI_OK, ERR_TLV_END past the last element, or ERR_UNKNOWN_MSG
 * when the key is absent / not an array. */
solariStatus solariJsonGetStrAt(const char *json, size_t len, const char *key,
                                size_t index, char *out, size_t cap);

#endif /* SOLARI_JSON_H */
