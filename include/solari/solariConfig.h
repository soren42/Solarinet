/*
 * solariConfig.h - configuration loader for the SolariNet .conf format (section 13).
 *
 * Format:
 *   [section]            section header
 *   key = value          assignment (value may contain ':' and spaces)
 *   # comment            '#' starts a comment to end of line (inline or whole-line)
 *   key = value          a key may repeat (e.g. several `process =` / `target =`
 *   key = value          lines); repeats are addressable by index.
 *
 * Values keep their internal structure verbatim, so a line like
 *   target = tcp:hydrogen.akoria.net:443 : web
 * yields the value "tcp:hydrogen.akoria.net:443 : web" for the caller to split.
 *
 * A server-pushed JSON overlay can be merged on top (configOverlayJson); that
 * path is compiled only when cJSON is available (SOLARI_WITH_JSON). The base
 * parser has zero external dependencies.
 *
 * Handle pattern: state lives in an opaque solariConfig the caller threads
 * through calls and releases with solariConfigFree.
 */
#ifndef SOLARI_CONFIG_H
#define SOLARI_CONFIG_H

#include "solari/solariCommon.h"

typedef struct solariConfig solariConfig;

/* Load and parse a .conf file. *out receives a new handle on success. */
solariStatus solariConfigLoad(const char *path, solariConfig **out);

/* Parse configuration directly from an in-memory NUL-terminated string. Useful
 * for tests and for embedding defaults. */
solariStatus solariConfigParse(const char *text, solariConfig **out);

/* Release a config handle (NULL-safe). */
void solariConfigFree(solariConfig *c);

/* Merge a JSON object overlay over the parsed config: each top-level key may be
 * "section.key" = value, overriding or adding entries. Requires SOLARI_WITH_JSON;
 * otherwise returns ERR_PLATFORM. */
solariStatus solariConfigOverlayJson(solariConfig *c, const char *json);

/* ---- scalar getters (first value if the key repeats) ---- */
/* Returns the stored value, or dflt if section.key is absent. The returned
 * pointer is owned by the config and valid until solariConfigFree. */
const char *solariConfigGetStr(const solariConfig *c, const char *section,
                               const char *key, const char *dflt);
/* Parses the value as a base-10 long; returns dflt if absent or unparseable. */
long solariConfigGetInt(const solariConfig *c, const char *section,
                        const char *key, long dflt);
/* True for "1","true","yes","on" (case-insensitive); dflt if absent. */
bool solariConfigGetBool(const solariConfig *c, const char *section,
                         const char *key, bool dflt);

/* ---- repeated-key access ---- */
/* Number of values stored for section.key (0 if none). */
size_t solariConfigCount(const solariConfig *c, const char *section, const char *key);
/* The index-th value for a repeated key, or dflt if out of range. */
const char *solariConfigGetStrAt(const solariConfig *c, const char *section,
                                 const char *key, size_t index, const char *dflt);

/* The configuration epoch: value of [identity] configEpoch if present, else 0. */
uint64_t solariConfigEpoch(const solariConfig *c);

#endif /* SOLARI_CONFIG_H */
