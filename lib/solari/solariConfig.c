/*
 * solariConfig.c - .conf parser (section 13). Zero external dependencies for the
 * base format; the optional JSON overlay is gated behind SOLARI_WITH_JSON.
 *
 * Uses POSIX strtok_r; request it before any header under strict -std=c99.
 */
#define _POSIX_C_SOURCE 200809L

#include "solari/solariConfig.h"
#include "solari/solariLog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    char *section;
    char *key;
    char *value;
} configEntry;

struct solariConfig {
    configEntry *entries;
    size_t count;
    size_t cap;
};

/* ---- small string helpers ---- */

static char *dupStr(const char *s)
{
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

/* Trim leading/trailing ASCII whitespace in place; returns the new start. */
static char *trim(char *s)
{
    char *end;
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

static int caseEq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

static solariStatus pushEntry(solariConfig *c, const char *section,
                              const char *key, const char *value)
{
    if (c->count == c->cap) {
        size_t ncap = c->cap ? c->cap * 2 : 16;
        configEntry *ne = (configEntry *)realloc(c->entries, ncap * sizeof *ne);
        if (!ne) return ERR_PLATFORM;
        c->entries = ne;
        c->cap = ncap;
    }
    configEntry *e = &c->entries[c->count];
    e->section = dupStr(section);
    e->key = dupStr(key);
    e->value = dupStr(value);
    if (!e->section || !e->key || !e->value) {
        free(e->section); free(e->key); free(e->value);
        return ERR_PLATFORM;
    }
    c->count++;
    return SOLARI_OK;
}

/* ---- parsing ---- */

solariStatus solariConfigParse(const char *text, solariConfig **out)
{
    solariConfig *c;
    char *buf, *line, *saveptr = NULL;
    char section[128] = "";

    if (!text || !out) return ERR_INVALID_ARG;

    c = (solariConfig *)calloc(1, sizeof *c);
    if (!c) return ERR_PLATFORM;

    buf = dupStr(text);
    if (!buf) { free(c); return ERR_PLATFORM; }

    for (line = strtok_r(buf, "\n", &saveptr); line; line = strtok_r(NULL, "\n", &saveptr)) {
        char *hash, *eq, *k, *v, *t;
        /* strip inline/whole-line comment */
        hash = strchr(line, '#');
        if (hash) *hash = '\0';

        t = trim(line);
        if (*t == '\0') continue;

        if (*t == '[') {                       /* section header */
            char *close = strchr(t, ']');
            if (close) {
                *close = '\0';
                char *name = trim(t + 1);
                strncpy(section, name, sizeof section - 1);
                section[sizeof section - 1] = '\0';
            }
            continue;
        }

        eq = strchr(t, '=');
        if (!eq) continue;                     /* not an assignment; ignore */
        *eq = '\0';
        k = trim(t);
        v = trim(eq + 1);
        if (*k == '\0') continue;

        if (pushEntry(c, section, k, v) != SOLARI_OK) {
            free(buf);
            solariConfigFree(c);
            return ERR_PLATFORM;
        }
    }

    free(buf);
    *out = c;
    return SOLARI_OK;
}

solariStatus solariConfigLoad(const char *path, solariConfig **out)
{
    FILE *f;
    long size;
    char *buf;
    solariStatus st;

    if (!path || !out) return ERR_INVALID_ARG;

    f = fopen(path, "rb");
    if (!f) {
        solariLogf(SOLARI_LOG_ERROR, "config: cannot open %s", path);
        return ERR_PLATFORM;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return ERR_PLATFORM; }
    size = ftell(f);
    if (size < 0) { fclose(f); return ERR_PLATFORM; }
    rewind(f);

    buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return ERR_PLATFORM; }
    if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf); fclose(f); return ERR_PLATFORM;
    }
    buf[size] = '\0';
    fclose(f);

    st = solariConfigParse(buf, out);
    free(buf);
    return st;
}

void solariConfigFree(solariConfig *c)
{
    size_t i;
    if (!c) return;
    for (i = 0; i < c->count; i++) {
        free(c->entries[i].section);
        free(c->entries[i].key);
        free(c->entries[i].value);
    }
    free(c->entries);
    free(c);
}

/* ---- lookups ---- */

static const configEntry *findNth(const solariConfig *c, const char *section,
                                  const char *key, size_t index)
{
    size_t i, seen = 0;
    if (!c) return NULL;
    for (i = 0; i < c->count; i++) {
        if (caseEq(c->entries[i].section, section) && caseEq(c->entries[i].key, key)) {
            if (seen == index) return &c->entries[i];
            seen++;
        }
    }
    return NULL;
}

const char *solariConfigGetStr(const solariConfig *c, const char *section,
                               const char *key, const char *dflt)
{
    const configEntry *e = findNth(c, section, key, 0);
    return e ? e->value : dflt;
}

long solariConfigGetInt(const solariConfig *c, const char *section,
                        const char *key, long dflt)
{
    const configEntry *e = findNth(c, section, key, 0);
    char *endp;
    long v;
    if (!e || e->value[0] == '\0') return dflt;
    v = strtol(e->value, &endp, 10);
    if (endp == e->value) return dflt;     /* nothing parsed */
    return v;
}

bool solariConfigGetBool(const solariConfig *c, const char *section,
                         const char *key, bool dflt)
{
    const configEntry *e = findNth(c, section, key, 0);
    if (!e) return dflt;
    if (caseEq(e->value, "1") || caseEq(e->value, "true") ||
        caseEq(e->value, "yes") || caseEq(e->value, "on")) return true;
    if (caseEq(e->value, "0") || caseEq(e->value, "false") ||
        caseEq(e->value, "no") || caseEq(e->value, "off")) return false;
    return dflt;
}

size_t solariConfigCount(const solariConfig *c, const char *section, const char *key)
{
    size_t i, n = 0;
    if (!c) return 0;
    for (i = 0; i < c->count; i++) {
        if (caseEq(c->entries[i].section, section) && caseEq(c->entries[i].key, key)) n++;
    }
    return n;
}

const char *solariConfigGetStrAt(const solariConfig *c, const char *section,
                                 const char *key, size_t index, const char *dflt)
{
    const configEntry *e = findNth(c, section, key, index);
    return e ? e->value : dflt;
}

uint64_t solariConfigEpoch(const solariConfig *c)
{
    const configEntry *e = findNth(c, "identity", "configEpoch", 0);
    if (!e || e->value[0] == '\0') return 0;
    return strtoull(e->value, NULL, 10);
}

/* ---- optional JSON overlay ---- */

#ifdef SOLARI_WITH_JSON
#include "cJSON.h"

solariStatus solariConfigOverlayJson(solariConfig *c, const char *json)
{
    cJSON *root, *item;
    if (!c || !json) return ERR_INVALID_ARG;
    root = cJSON_Parse(json);
    if (!root) return ERR_INVALID_ARG;

    cJSON_ArrayForEach(item, root) {
        /* key is "section.key"; value coerced to string form */
        const char *dotted = item->string;
        char sect[128], *dot;
        char valbuf[256];
        if (!dotted) continue;
        strncpy(sect, dotted, sizeof sect - 1);
        sect[sizeof sect - 1] = '\0';
        dot = strchr(sect, '.');
        if (!dot) continue;
        *dot = '\0';
        const char *keyname = dot + 1;

        if (cJSON_IsString(item)) {
            pushEntry(c, sect, keyname, item->valuestring);
        } else if (cJSON_IsNumber(item)) {
            snprintf(valbuf, sizeof valbuf, "%g", item->valuedouble);
            pushEntry(c, sect, keyname, valbuf);
        } else if (cJSON_IsBool(item)) {
            pushEntry(c, sect, keyname, cJSON_IsTrue(item) ? "true" : "false");
        }
    }
    cJSON_Delete(root);
    return SOLARI_OK;
}

#else  /* JSON overlay not compiled in */

solariStatus solariConfigOverlayJson(solariConfig *c, const char *json)
{
    SOLARI_UNUSED(c);
    SOLARI_UNUSED(json);
    return ERR_PLATFORM;   /* built without cJSON; vendor it + define SOLARI_WITH_JSON */
}

#endif
