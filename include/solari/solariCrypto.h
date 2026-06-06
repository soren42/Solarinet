/*
 * solariCrypto.h - small, self-contained cryptographic & hashing primitives.
 *
 * These are deliberately dependency-free (no OpenSSL/mbedTLS) because they run
 * in the Phase 1 core on every target: FNV-1a-64 derives the stable nodeId
 * (§5.5), CRC-32 protects every frame (§5.1), SHA-256 verifies deploy blobs
 * (§15.3), and solariCertCn parses a certificate subject CN for role auth
 * (§2 security model). SHA-256 here is for integrity, not secrecy; TLS for
 * confidentiality is mbedTLS's job in the I/O layer.
 */
#ifndef SOLARI_CRYPTO_H
#define SOLARI_CRYPTO_H

#include "solari/solariCommon.h"

/* FNV-1a 64-bit hash over len bytes. Used to derive nodeId and for cheap,
 * non-cryptographic keying (e.g. HRW weights in the monitor). */
uint64_t solariFnv1a64(const void *data, size_t len);

/* Convenience: FNV-1a-64 of a NUL-terminated string (excludes the NUL). */
uint64_t solariFnv1a64Str(const char *s);

/* CRC-32 (IEEE 802.3, reflected, poly 0xEDB88820) over len bytes. Matches the
 * frame trailer in §5.1. Call with seed 0 for a one-shot; the returned value
 * may be fed back as seed to checksum a stream incrementally. */
uint32_t solariCrc32(uint32_t seed, const void *data, size_t len);

/* SHA-256. Writes 32 raw bytes to digest. One-shot convenience over the
 * streaming context below. */
void solariSha256(const void *data, size_t len, uint8_t digest[32]);

/* Streaming SHA-256 for large deploy blobs that need not be held in memory. */
typedef struct {
    uint32_t state[8];
    uint64_t bitLen;
    uint8_t  buf[64];
    size_t   bufLen;
} solariSha256Ctx;

void solariSha256Init  (solariSha256Ctx *c);
void solariSha256Update(solariSha256Ctx *c, const void *data, size_t len);
void solariSha256Final (solariSha256Ctx *c, uint8_t digest[32]);

/* Hex-encode n bytes into out (lowercase). out must hold 2*n + 1 bytes; the
 * result is NUL-terminated. Handy for logging digests and nodeIds. */
void solariHexEncode(const uint8_t *bytes, size_t n, char *out, size_t outCap);

/* Extract the Common Name (CN) from an X.509 subject DN string of the form
 * "CN=monitor.akoria.net,O=Akoria,..." (RFC 4514-ish, as exposed by mbedTLS).
 * Writes the CN value to out (NUL-terminated) and returns SOLARI_OK, or
 * ERR_INVALID_ARG if no CN component is present. Whitespace around the value
 * is trimmed; escaped commas (\,) inside the value are honored. */
solariStatus solariCertCn(const char *subjectDn, char *out, size_t outCap);

#endif /* SOLARI_CRYPTO_H */
