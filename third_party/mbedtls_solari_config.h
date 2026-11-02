/* Appended to the vendored Mbed TLS default configuration
 * (MBEDTLS_USER_CONFIG_FILE). nng drives TLS and the PSA crypto core from
 * several threads, so the library must be built thread-safe, as the distro
 * packages it replaces were. */
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD
