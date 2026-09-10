/* mbedTLS 2.28 configuration: TLS 1.2, X.509 mTLS, bounded record buffers. */
#ifndef SOLARI_MBEDTLS_CONFIG_H
#define SOLARI_MBEDTLS_CONFIG_H

/* Start with the vendor-tested 2.28 feature dependency set, then remove
 * unused/high-risk surfaces and cap the dominant per-connection buffers. */
#include "mbedtls/config.h"

#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_TIME_ALT
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C

#undef MBEDTLS_SSL_PROTO_TLS1
#undef MBEDTLS_SSL_PROTO_TLS1_1
#define MBEDTLS_SSL_PROTO_TLS1_2

#undef MBEDTLS_SSL_MAX_CONTENT_LEN
#define MBEDTLS_SSL_MAX_CONTENT_LEN 4096
#undef MBEDTLS_MPI_MAX_SIZE
#define MBEDTLS_MPI_MAX_SIZE 512

#undef MBEDTLS_FS_IO
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_SSL_CACHE_C
#undef MBEDTLS_SSL_TICKET_C
#undef MBEDTLS_SSL_SESSION_TICKETS
#undef MBEDTLS_X509_CRL_PARSE_C
#undef MBEDTLS_X509_CSR_PARSE_C
#undef MBEDTLS_X509_CREATE_C
#undef MBEDTLS_X509_CRT_WRITE_C
#undef MBEDTLS_X509_CSR_WRITE_C
#undef MBEDTLS_PEM_WRITE_C

/* No CBC suites: the panel needs only TLS-1.2 AEAD (GCM/CCM) suites. */
#undef MBEDTLS_CIPHER_MODE_CBC
#undef MBEDTLS_SSL_CBC_RECORD_SPLITTING
#undef MBEDTLS_SSL_ENCRYPT_THEN_MAC

#include "mbedtls/check_config.h"

#endif /* SOLARI_MBEDTLS_CONFIG_H */
