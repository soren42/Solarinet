/*
 * solariCrypto.c - FNV-1a-64, CRC-32 (IEEE), SHA-256, hex, and CN parsing.
 * All implementations are public-domain-style and endian-independent.
 */
#include "solari/solariCrypto.h"

#include <string.h>

/* ----------------------------------------------------------------- FNV-1a */

#define FNV64_OFFSET 14695981039346656037ULL   /* 0xcbf29ce484222325 */
#define FNV64_PRIME  1099511628211ULL           /* 0x100000001b3      */

uint64_t solariFnv1a64(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = FNV64_OFFSET;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= FNV64_PRIME;
    }
    return h;
}

uint64_t solariFnv1a64Str(const char *s)
{
    return solariFnv1a64(s, s ? strlen(s) : 0);
}

/* ------------------------------------------------------------------ CRC-32 */
/* Reflected CRC-32 (poly 0xEDB88820), matching zlib/IEEE 802.3. The table is
 * built lazily on first use so there is no static initializer to maintain. */

static uint32_t gCrcTable[256];
static int gCrcReady = 0;

static void crc32BuildTable(void)
{
    uint32_t i, j, c;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++) {
            /* 0xEDB88320 is the reflected CRC-32 (IEEE 802.3) polynomial. */
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        gCrcTable[i] = c;
    }
    gCrcReady = 1;
}

uint32_t solariCrc32(uint32_t seed, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc;
    size_t i;
    if (!gCrcReady) {
        crc32BuildTable();
    }
    crc = seed ^ 0xFFFFFFFFu;
    for (i = 0; i < len; i++) {
        crc = gCrcTable[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ SHA-256 */

#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static const uint32_t kSha256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256Block(solariSha256Ctx *c, const uint8_t *p)
{
    uint32_t w[64];
    uint32_t a, b, cc, d, e, f, g, h, t1, t2;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROTR32(w[i - 15], 7) ^ ROTR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR32(w[i - 2], 17) ^ ROTR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = c->state[0]; b = c->state[1]; cc = c->state[2]; d = c->state[3];
    e = c->state[4]; f = c->state[5]; g = c->state[6]; h = c->state[7];

    for (i = 0; i < 64; i++) {
        uint32_t S1 = ROTR32(e, 6) ^ ROTR32(e, 11) ^ ROTR32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        t1 = h + S1 + ch + kSha256[i] + w[i];
        uint32_t S0 = ROTR32(a, 2) ^ ROTR32(a, 13) ^ ROTR32(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

void solariSha256Init(solariSha256Ctx *c)
{
    c->state[0] = 0x6a09e667; c->state[1] = 0xbb67ae85;
    c->state[2] = 0x3c6ef372; c->state[3] = 0xa54ff53a;
    c->state[4] = 0x510e527f; c->state[5] = 0x9b05688c;
    c->state[6] = 0x1f83d9ab; c->state[7] = 0x5be0cd19;
    c->bitLen = 0;
    c->bufLen = 0;
}

void solariSha256Update(solariSha256Ctx *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t i = 0;
    c->bitLen += (uint64_t)len * 8ULL;
    while (i < len) {
        c->buf[c->bufLen++] = p[i++];
        if (c->bufLen == 64) {
            sha256Block(c, c->buf);
            c->bufLen = 0;
        }
    }
}

void solariSha256Final(solariSha256Ctx *c, uint8_t digest[32])
{
    uint64_t bits = c->bitLen;
    int i;
    /* Append 0x80 then zero-pad to 56 mod 64, then 8-byte big-endian length. */
    c->buf[c->bufLen++] = 0x80;
    if (c->bufLen > 56) {
        while (c->bufLen < 64) c->buf[c->bufLen++] = 0;
        sha256Block(c, c->buf);
        c->bufLen = 0;
    }
    while (c->bufLen < 56) c->buf[c->bufLen++] = 0;
    for (i = 7; i >= 0; i--) {
        c->buf[c->bufLen++] = (uint8_t)((bits >> (i * 8)) & 0xFFu);
    }
    sha256Block(c, c->buf);

    for (i = 0; i < 8; i++) {
        digest[i * 4]     = (uint8_t)(c->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(c->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(c->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)(c->state[i]);
    }
}

void solariSha256(const void *data, size_t len, uint8_t digest[32])
{
    solariSha256Ctx c;
    solariSha256Init(&c);
    solariSha256Update(&c, data, len);
    solariSha256Final(&c, digest);
}

/* -------------------------------------------------------------------- hex */

void solariHexEncode(const uint8_t *bytes, size_t n, char *out, size_t outCap)
{
    static const char hexd[] = "0123456789abcdef";
    size_t i;
    if (outCap == 0) {
        return;
    }
    for (i = 0; i < n && (2 * i + 2) < outCap; i++) {
        out[2 * i]     = hexd[(bytes[i] >> 4) & 0xF];
        out[2 * i + 1] = hexd[bytes[i] & 0xF];
    }
    out[2 * i] = '\0';
}

/* --------------------------------------------------------------- cert CN */

solariStatus solariCertCn(const char *subjectDn, char *out, size_t outCap)
{
    const char *p = subjectDn;
    if (!subjectDn || !out || outCap == 0) {
        return ERR_INVALID_ARG;
    }
    /* Scan attribute-type-and-value pairs separated by unescaped commas; find
     * the (case-insensitive) "CN=" component. */
    while (*p) {
        const char *eq;
        /* skip leading whitespace */
        while (*p == ' ' || *p == '\t') p++;
        /* is this component a CN? */
        if ((p[0] == 'C' || p[0] == 'c') && (p[1] == 'N' || p[1] == 'n') && p[2] == '=') {
            size_t o = 0;
            eq = p + 3;
            /* trim leading whitespace of the value */
            while (*eq == ' ' || *eq == '\t') eq++;
            while (*eq) {
                if (*eq == '\\' && eq[1] != '\0') {   /* escaped char */
                    eq++;
                    if (o + 1 < outCap) out[o++] = *eq;
                    eq++;
                    continue;
                }
                if (*eq == ',') {                     /* unescaped: end of RDN */
                    break;
                }
                if (o + 1 < outCap) out[o++] = *eq;
                eq++;
            }
            /* trim trailing whitespace */
            while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\t')) o--;
            out[o] = '\0';
            return SOLARI_OK;
        }
        /* advance to the next component (past the next unescaped comma) */
        while (*p) {
            if (*p == '\\' && p[1] != '\0') { p += 2; continue; }
            if (*p == ',') { p++; break; }
            p++;
        }
    }
    return ERR_INVALID_ARG;   /* no CN present */
}
