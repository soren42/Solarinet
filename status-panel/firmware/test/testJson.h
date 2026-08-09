/* testJson.h — tiny read-only JSON DOM, host test harness only.
 *
 * Exists for one reason: the framebuffer-parity fixture
 * (status-panel/fixtures/parity-fixture.json) has to be read by BOTH the C
 * host harness and node, so the C side needs to parse the same bytes rather
 * than carry a hand-transcribed copy of them. That is the exact failure mode
 * RETURN-AW3 UNVERIFIED #3 calls out.
 *
 * Deliberately small: no streaming, no writing, no number formats beyond what
 * the fixture uses. NOT firmware — never linked into the panel binary.        */
#ifndef TEST_JSON_H
#define TEST_JSON_H

#include <stddef.h>

typedef enum {
  J_NULL,
  J_BOOL,
  J_NUM,
  J_STR,
  J_ARR,
  J_OBJ
} JType;

typedef struct JVal JVal;
struct JVal {
  JType type;
  double num;   /* J_NUM, and 0/1 for J_BOOL */
  char *str;    /* J_STR, owned */
  JVal **items; /* J_ARR/J_OBJ element values, owned */
  char **keys;  /* J_OBJ keys, owned, parallel to items */
  int count;
};

/* Purpose: parse a whole JSON file into a DOM.
 * Input: path; err buffer for a message. Output: root, or NULL on failure. */
JVal *jsonParseFile(const char *path, char *err, size_t errCap);

/* Purpose: parse a JSON document already in memory. Mainly so the malformed-
 * input corpus can be asserted without scattering temp files.
 * Input: text and its length; err buffer. Output: root, or NULL on failure. */
JVal *jsonParseMem(const char *text, size_t len, char *err, size_t errCap);

/* Purpose: release a DOM. Input: root (NULL ok). Output: none. */
void jsonFree(JVal *v);

/* Purpose: look up an object member. Input: object, key.
 * Output: member value, or NULL when absent or v is not an object. */
const JVal *jGet(const JVal *v, const char *key);

/* Purpose: index an array. Input: array, index.
 * Output: element, or NULL when out of range or v is not an array. */
const JVal *jAt(const JVal *v, int i);

/* Purpose: element count. Input: any value. Output: count, 0 for scalars. */
int jLen(const JVal *v);

/* Purpose: coerce to number. Input: value (NULL ok), fallback. Output: number. */
double jNum(const JVal *v, double dflt);

/* Purpose: coerce to string. Input: value (NULL ok), fallback. Output: string. */
const char *jStr(const JVal *v, const char *dflt);

/* Purpose: coerce to bool. Input: value (NULL ok), fallback. Output: 0 or 1. */
int jBool(const JVal *v, int dflt);

#endif /* TEST_JSON_H */
