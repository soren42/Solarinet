/* testJson.c — recursive-descent JSON reader for the host test harness.
 * See testJson.h for why this exists. Host-only; never linked into firmware. */
#include "testJson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *start;
  const char *p;
  const char *end;
  char *err;
  size_t errCap;
  int failed;
} JParse;

static JVal *parseValue(JParse *P);

/* Purpose: record the first parse error. Input: parser, message. Output: NULL. */
static JVal *fail(JParse *P, const char *msg) {
  if (!P->failed) {
    P->failed = 1;
    if (P->err && P->errCap) {
      snprintf(P->err, P->errCap, "%s at byte offset %ld", msg,
               (long)(P->p - P->start));
    }
  }
  return NULL;
}

/* Purpose: allocate a zeroed node. Input: type. Output: node (aborts on OOM). */
static JVal *newVal(JType t) {
  JVal *v = (JVal *)calloc(1, sizeof(JVal));
  if (!v) { fprintf(stderr, "testJson: out of memory\n"); exit(2); }
  v->type = t;
  return v;
}

/* Purpose: skip whitespace. Input: parser. Output: none. */
static void skipWs(JParse *P) {
  while (P->p < P->end) {
    char c = *P->p;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') P->p++;
    else break;
  }
}

/* Purpose: append one byte to a growing buffer. Input: buf/len/cap, byte. */
static void push(char **buf, size_t *len, size_t *cap, char c) {
  if (*len + 1 >= *cap) {
    *cap = *cap ? *cap * 2 : 32;
    *buf = (char *)realloc(*buf, *cap);
    if (!*buf) { fprintf(stderr, "testJson: out of memory\n"); exit(2); }
  }
  (*buf)[(*len)++] = c;
  (*buf)[*len] = '\0';
}

/* Purpose: encode a code point as UTF-8. Input: buffer state, cp. Output: none.
 * The fixture is ASCII plus a few typographic marks in the comment strings. */
static void pushUtf8(char **buf, size_t *len, size_t *cap, unsigned cp) {
  if (cp < 0x80u) {
    push(buf, len, cap, (char)cp);
  } else if (cp < 0x800u) {
    push(buf, len, cap, (char)(0xC0u | (cp >> 6)));
    push(buf, len, cap, (char)(0x80u | (cp & 0x3Fu)));
  } else {
    push(buf, len, cap, (char)(0xE0u | (cp >> 12)));
    push(buf, len, cap, (char)(0x80u | ((cp >> 6) & 0x3Fu)));
    push(buf, len, cap, (char)(0x80u | (cp & 0x3Fu)));
  }
}

/* Purpose: read 4 hex digits. Input: parser. Output: value, -1 on error. */
static int hex4(JParse *P) {
  int v = 0, i;
  for (i = 0; i < 4; i++) {
    int d;
    char c;
    if (P->p >= P->end) return -1;
    c = *P->p++;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return -1;
    v = v * 16 + d;
  }
  return v;
}

/* Purpose: parse a quoted string body. Input: parser. Output: malloc'd string. */
static char *parseString(JParse *P) {
  char *buf = NULL;
  size_t len = 0, cap = 0;
  if (P->p >= P->end || *P->p != '"') { fail(P, "expected string"); return NULL; }
  P->p++;
  push(&buf, &len, &cap, '\0');
  len = 0;
  buf[0] = '\0';
  while (P->p < P->end) {
    char c = *P->p++;
    if (c == '"') return buf;
    if (c != '\\') { push(&buf, &len, &cap, c); continue; }
    if (P->p >= P->end) break;
    c = *P->p++;
    switch (c) {
      case '"':  push(&buf, &len, &cap, '"');  break;
      case '\\': push(&buf, &len, &cap, '\\'); break;
      case '/':  push(&buf, &len, &cap, '/');  break;
      case 'b':  push(&buf, &len, &cap, '\b'); break;
      case 'f':  push(&buf, &len, &cap, '\f'); break;
      case 'n':  push(&buf, &len, &cap, '\n'); break;
      case 'r':  push(&buf, &len, &cap, '\r'); break;
      case 't':  push(&buf, &len, &cap, '\t'); break;
      case 'u': {
        int cp = hex4(P);
        if (cp < 0) { free(buf); fail(P, "bad \\u escape"); return NULL; }
        /* Surrogate pairs: the fixture has none, so pass the unit through. */
        pushUtf8(&buf, &len, &cap, (unsigned)cp);
        break;
      }
      default:
        free(buf);
        fail(P, "bad string escape");
        return NULL;
    }
  }
  free(buf);
  fail(P, "unterminated string");
  return NULL;
}

/* Purpose: parse one array. Input: parser positioned at '['. Output: node. */
static JVal *parseArray(JParse *P) {
  JVal *v = newVal(J_ARR);
  P->p++; /* '[' */
  skipWs(P);
  if (P->p < P->end && *P->p == ']') { P->p++; return v; }
  for (;;) {
    JVal *e;
    JVal **grown;
    skipWs(P);
    e = parseValue(P);
    if (!e) { jsonFree(v); return NULL; }
    grown = (JVal **)realloc(v->items, (size_t)(v->count + 1) * sizeof(JVal *));
    if (!grown) { fprintf(stderr, "testJson: out of memory\n"); exit(2); }
    v->items = grown;
    v->items[v->count++] = e;
    skipWs(P);
    if (P->p < P->end && *P->p == ',') { P->p++; continue; }
    if (P->p < P->end && *P->p == ']') { P->p++; return v; }
    jsonFree(v);
    return fail(P, "expected ',' or ']'");
  }
}

/* Purpose: parse one object. Input: parser positioned at '{'. Output: node. */
static JVal *parseObject(JParse *P) {
  JVal *v = newVal(J_OBJ);
  P->p++; /* '{' */
  skipWs(P);
  if (P->p < P->end && *P->p == '}') { P->p++; return v; }
  for (;;) {
    char *k;
    JVal *e;
    JVal **gi;
    char **gk;
    skipWs(P);
    k = parseString(P);
    if (!k) { jsonFree(v); return NULL; }
    skipWs(P);
    if (P->p >= P->end || *P->p != ':') {
      free(k); jsonFree(v); return fail(P, "expected ':'");
    }
    P->p++;
    skipWs(P);
    e = parseValue(P);
    if (!e) { free(k); jsonFree(v); return NULL; }
    gi = (JVal **)realloc(v->items, (size_t)(v->count + 1) * sizeof(JVal *));
    gk = (char **)realloc(v->keys, (size_t)(v->count + 1) * sizeof(char *));
    if (!gi || !gk) { fprintf(stderr, "testJson: out of memory\n"); exit(2); }
    v->items = gi;
    v->keys = gk;
    v->keys[v->count] = k;
    v->items[v->count] = e;
    v->count++;
    skipWs(P);
    if (P->p < P->end && *P->p == ',') { P->p++; continue; }
    if (P->p < P->end && *P->p == '}') { P->p++; return v; }
    jsonFree(v);
    return fail(P, "expected ',' or '}'");
  }
}

/* Purpose: parse any value. Input: parser. Output: node, NULL on error. */
static JVal *parseValue(JParse *P) {
  skipWs(P);
  if (P->p >= P->end) return fail(P, "unexpected end of input");
  switch (*P->p) {
    case '{': return parseObject(P);
    case '[': return parseArray(P);
    case '"': {
      JVal *v;
      char *s = parseString(P);
      if (!s) return NULL;
      v = newVal(J_STR);
      v->str = s;
      return v;
    }
    case 't':
      if ((size_t)(P->end - P->p) >= 4 && !memcmp(P->p, "true", 4)) {
        JVal *v = newVal(J_BOOL);
        v->num = 1.0;
        P->p += 4;
        return v;
      }
      return fail(P, "bad literal");
    case 'f':
      if ((size_t)(P->end - P->p) >= 5 && !memcmp(P->p, "false", 5)) {
        JVal *v = newVal(J_BOOL);
        P->p += 5;
        return v;
      }
      return fail(P, "bad literal");
    case 'n':
      if ((size_t)(P->end - P->p) >= 4 && !memcmp(P->p, "null", 4)) {
        P->p += 4;
        return newVal(J_NULL);
      }
      return fail(P, "bad literal");
    default: {
      char *stop = NULL;
      double d = strtod(P->p, &stop);
      JVal *v;
      if (!stop || stop == P->p) return fail(P, "bad number");
      P->p = stop;
      v = newVal(J_NUM);
      v->num = d;
      return v;
    }
  }
}

JVal *jsonParseFile(const char *path, char *err, size_t errCap) {
  FILE *f = fopen(path, "rb");
  char *buf;
  long size;
  size_t got;
  JParse P;
  JVal *root;

  if (err && errCap) err[0] = '\0';
  if (!f) {
    if (err && errCap) snprintf(err, errCap, "cannot open %s", path);
    return NULL;
  }
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  size = ftell(f);
  if (size < 0) { fclose(f); return NULL; }
  rewind(f);
  buf = (char *)malloc((size_t)size + 1);
  if (!buf) { fclose(f); fprintf(stderr, "testJson: out of memory\n"); exit(2); }
  got = fread(buf, 1, (size_t)size, f);
  fclose(f);
  buf[got] = '\0';

  P.start = buf;
  P.p = buf;
  P.end = buf + got;
  P.err = err;
  P.errCap = errCap;
  P.failed = 0;
  root = parseValue(&P);
  if (root) {
    skipWs(&P);
    if (P.p != P.end) {
      jsonFree(root);
      root = NULL;
      if (err && errCap) snprintf(err, errCap, "trailing bytes after root value");
    }
  }
  free(buf);
  return root;
}

void jsonFree(JVal *v) {
  int i;
  if (!v) return;
  for (i = 0; i < v->count; i++) {
    jsonFree(v->items[i]);
    if (v->keys) free(v->keys[i]);
  }
  free(v->items);
  free(v->keys);
  free(v->str);
  free(v);
}

const JVal *jGet(const JVal *v, const char *key) {
  int i;
  if (!v || v->type != J_OBJ) return NULL;
  for (i = 0; i < v->count; i++)
    if (strcmp(v->keys[i], key) == 0) return v->items[i];
  return NULL;
}

const JVal *jAt(const JVal *v, int i) {
  if (!v || v->type != J_ARR || i < 0 || i >= v->count) return NULL;
  return v->items[i];
}

int jLen(const JVal *v) {
  if (!v || (v->type != J_ARR && v->type != J_OBJ)) return 0;
  return v->count;
}

double jNum(const JVal *v, double dflt) {
  if (!v || (v->type != J_NUM && v->type != J_BOOL)) return dflt;
  return v->num;
}

const char *jStr(const JVal *v, const char *dflt) {
  if (!v || v->type != J_STR) return dflt;
  return v->str;
}

int jBool(const JVal *v, int dflt) {
  if (!v || v->type != J_BOOL) return dflt;
  return v->num != 0.0 ? 1 : 0;
}
