/*
 * solariJson.c - minimal, dependency-free JSON field extraction (§6, §13).
 *
 * See solariJson.h for the contract. Design notes:
 *   - every routine is a bounded forward scan over [json, json+len); there is
 *     no recursion and no allocation, so a hostile blob cannot blow the stack
 *     or the heap.
 *   - key lookup matches the exact quoted key ("key") anywhere outside of a
 *     string VALUE position; because config blobs are small flat-ish documents
 *     authored by the dashboard, first-match semantics are the documented
 *     contract (nested {"schedule":{"sampleIntervalSec":15}} resolves the
 *     same as a flat document).
 */
#include "solari/solariJson.h"

#include <string.h>

/* ------------------------------------------------------------- validation */

solariStatus solariJsonValidate(const char *json, size_t len)
{
    size_t i;
    long   depth = 0;
    bool   inStr = false;
    bool   sawTop = false;

    if (!json || len == 0) return ERR_INVALID_ARG;

    for (i = 0; i < len; i++) {
        char c = json[i];
        if (inStr) {
            if (c == '\\') {
                if (i + 1 >= len) return ERR_INVALID_ARG;  /* dangling escape */
                i++;                                        /* skip escaped ch */
            } else if (c == '"') {
                inStr = false;
            } else if ((unsigned char)c < 0x20) {
                return ERR_INVALID_ARG;      /* raw control char in a string */
            }
            continue;
        }
        switch (c) {
        case '"': inStr = true; break;
        case '{': case '[':
            depth++; sawTop = true; break;
        case '}': case ']':
            depth--;
            if (depth < 0) return ERR_INVALID_ARG;
            break;
        default:
            break;
        }
    }
    if (inStr || depth != 0 || !sawTop) return ERR_INVALID_ARG;
    return SOLARI_OK;
}

/* ------------------------------------------------------------ key locating */

/* Find `"key"` (a quoted token followed, after whitespace, by ':') and return
 * the index of the first byte of its VALUE, or (size_t)-1 if absent. Skips
 * matches that occur inside string values by tracking string state. */
static size_t jsonFindValue(const char *json, size_t len, const char *key)
{
    size_t klen, i;
    bool   inStr = false;

    if (!json || !key) return (size_t)-1;
    klen = strlen(key);
    if (klen == 0 || klen + 2 > len) return (size_t)-1;

    for (i = 0; i + klen + 1 < len; i++) {
        char c = json[i];
        if (inStr) {
            if (c == '\\') { i++; continue; }
            if (c != '"') continue;
            /* closing quote: check whether the string we just closed was the
             * key. The opening quote was found when inStr flipped; simpler to
             * re-check here: the token ended at i. */
            inStr = false;
            continue;
        }
        if (c == '"') {
            /* candidate token start */
            if (i + 1 + klen < len &&
                memcmp(json + i + 1, key, klen) == 0 &&
                json[i + 1 + klen] == '"') {
                size_t j = i + klen + 2;
                while (j < len && (json[j] == ' ' || json[j] == '\t' ||
                                   json[j] == '\r' || json[j] == '\n')) j++;
                if (j < len && json[j] == ':') {
                    j++;
                    while (j < len && (json[j] == ' ' || json[j] == '\t' ||
                                       json[j] == '\r' || json[j] == '\n')) j++;
                    if (j < len) return j;
                    return (size_t)-1;
                }
            }
            /* not our key (or not a key at all): consume the string token */
            inStr = true;
            continue;
        }
    }
    return (size_t)-1;
}

/* Decode a JSON string starting at json[at] (which must be '"') into out.
 * Returns the index just past the closing quote, or (size_t)-1 on error. */
static size_t jsonDecodeString(const char *json, size_t len, size_t at,
                               char *out, size_t cap)
{
    size_t i = at, o = 0;

    if (i >= len || json[i] != '"') return (size_t)-1;
    i++;
    while (i < len && json[i] != '"') {
        char c = json[i];
        if (c == '\\') {
            if (i + 1 >= len) return (size_t)-1;
            i++;
            switch (json[i]) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '"': case '\\': case '/': c = json[i]; break;
            default:  c = json[i]; break;   /* \uXXXX etc: copy verbatim */
            }
        }
        if (out && o + 1 < cap) out[o] = c;
        o++;
        i++;
    }
    if (i >= len) return (size_t)-1;         /* unterminated */
    if (out && cap > 0) out[o < cap ? o : cap - 1] = '\0';
    return i + 1;
}

/* ------------------------------------------------------------------ getters */

solariStatus solariJsonGetU64(const char *json, size_t len,
                              const char *key, uint64_t *out)
{
    size_t   at;
    uint64_t v = 0;
    bool     any = false;

    if (!out) return ERR_INVALID_ARG;
    at = jsonFindValue(json, len, key);
    if (at == (size_t)-1) return ERR_UNKNOWN_MSG;

    if (json[at] == '"') at++;               /* tolerate "123" */
    while (at < len && json[at] >= '0' && json[at] <= '9') {
        uint64_t nv = v * 10u + (uint64_t)(json[at] - '0');
        if (nv < v) return ERR_INVALID_ARG;  /* overflow */
        v = nv;
        any = true;
        at++;
    }
    if (!any) return ERR_UNKNOWN_MSG;        /* value is not a number */
    *out = v;
    return SOLARI_OK;
}

solariStatus solariJsonGetStr(const char *json, size_t len,
                              const char *key, char *out, size_t cap)
{
    size_t at;

    if (!out || cap == 0) return ERR_INVALID_ARG;
    out[0] = '\0';
    at = jsonFindValue(json, len, key);
    if (at == (size_t)-1 || json[at] != '"') return ERR_UNKNOWN_MSG;
    if (jsonDecodeString(json, len, at, out, cap) == (size_t)-1) {
        out[0] = '\0';
        return ERR_INVALID_ARG;
    }
    return SOLARI_OK;
}

bool solariJsonHasArray(const char *json, size_t len, const char *key)
{
    size_t at = jsonFindValue(json, len, key);
    return at != (size_t)-1 && json[at] == '[';
}

/* Advance past one JSON value starting at json[i]; returns the index of the
 * first byte after the value, or (size_t)-1 on a malformed value. Handles
 * strings, nested objects/arrays (iteratively, by depth counter), and bare
 * scalars (numbers / true / false / null). */
static size_t jsonSkipValue(const char *json, size_t len, size_t i)
{
    long depth = 0;
    bool inStr = false;

    if (i >= len) return (size_t)-1;
    if (json[i] == '"') return jsonDecodeString(json, len, i, NULL, 0);
    if (json[i] == '{' || json[i] == '[') {
        for (; i < len; i++) {
            char c = json[i];
            if (inStr) {
                if (c == '\\') { i++; continue; }
                if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') inStr = true;
            else if (c == '{' || c == '[') depth++;
            else if (c == '}' || c == ']') {
                depth--;
                if (depth == 0) return i + 1;
                if (depth < 0) return (size_t)-1;
            }
        }
        return (size_t)-1;
    }
    /* bare scalar: runs to the next delimiter */
    while (i < len && json[i] != ',' && json[i] != ']' && json[i] != '}' &&
           json[i] != ' ' && json[i] != '\t' && json[i] != '\r' && json[i] != '\n')
        i++;
    return i;
}

solariStatus solariJsonGetStrAt(const char *json, size_t len, const char *key,
                                size_t index, char *out, size_t cap)
{
    size_t at, idx = 0;

    if (!out || cap == 0) return ERR_INVALID_ARG;
    out[0] = '\0';
    at = jsonFindValue(json, len, key);
    if (at == (size_t)-1 || json[at] != '[') return ERR_UNKNOWN_MSG;
    at++;                                    /* into the array body */

    for (;;) {
        /* skip whitespace and element separators */
        while (at < len && (json[at] == ' ' || json[at] == '\t' ||
                            json[at] == '\r' || json[at] == '\n' ||
                            json[at] == ',')) at++;
        if (at >= len) return ERR_INVALID_ARG;   /* unterminated array */
        if (json[at] == ']') return ERR_TLV_END; /* exhausted */

        if (idx == index) {
            if (json[at] != '"') return ERR_INVALID_ARG;  /* non-string elem */
            if (jsonDecodeString(json, len, at, out, cap) == (size_t)-1) {
                out[0] = '\0';
                return ERR_INVALID_ARG;
            }
            return SOLARI_OK;
        }
        at = jsonSkipValue(json, len, at);
        if (at == (size_t)-1) return ERR_INVALID_ARG;
        idx++;
    }
}
