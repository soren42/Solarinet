/* lwIP configuration for the single-threaded 40 ms poll-mode firmware. */
#ifndef SOLARI_LWIPOPTS_H
#define SOLARI_LWIPOPTS_H

#include <stdint.h>
#include "pico/rand.h"

#define NO_SYS                          1
#define SYS_LIGHTWEIGHT_PROT            0
#define LWIP_SOCKET                     0
#define LWIP_NETCONN                    0
#define LWIP_RAW                        1
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_DHCP                       1
#define LWIP_DNS                        1
#define LWIP_ALTCP                      1
#define LWIP_ALTCP_TLS                  1
#define LWIP_ALTCP_TLS_MBEDTLS          1
#define ALTCP_MBEDTLS_AUTHMODE          MBEDTLS_SSL_VERIFY_REQUIRED
#define LWIP_SNTP                       1

#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (24 * 1024)
#define MEMP_NUM_TCP_PCB                4
#define MEMP_NUM_TCP_SEG                16
#define MEMP_NUM_ALTCP_PCB              4
#define MEMP_NUM_SYS_TIMEOUT            (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 1)
#define PBUF_POOL_SIZE                  12
#define PBUF_POOL_BUFSIZE               1536
#define TCP_MSS                         1460
#define TCP_WND                         5840
#define TCP_SND_BUF                     5840
#define TCP_SND_QUEUELEN                16

#define LWIP_TCP_KEEPALIVE              1
#define LWIP_STATS                      0
#define LWIP_DEBUG                      0
#define LWIP_RAND()                     get_rand_32()

#define SNTP_MAX_SERVERS                1
#define SNTP_SERVER_DNS                 1
#define SNTP_UPDATE_DELAY               3600000u
#define SNTP_CHECK_RESPONSE             2
/* Roundtrip compensation needs SNTP_GET_SYSTEM_TIME, whose default stub
 * returns 0 — and AON is not running on first boot, precisely when SNTP
 * first runs. A zero destination timestamp corrupts the computed epoch and
 * D7's plausibility gate would then refuse it forever. Sub-second accuracy
 * is irrelevant here; only plausibility (> build epoch) matters.          */
#define SNTP_COMP_ROUNDTRIP             0

/* Implemented in panelNet.c.  D7 rejects implausible epochs before setting
 * AON time; declaring it here keeps lwIP's sntp.c warning-clean under C11. */
void panelNetSntpSetTime(uint32_t seconds);
#define SNTP_SET_SYSTEM_TIME(sec) panelNetSntpSetTime((uint32_t)(sec))

#endif /* SOLARI_LWIPOPTS_H */
