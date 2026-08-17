/* panelNet.c — poll-mode CYW43/lwIP/mbedTLS device glue (CONTRACT-SW D6-D9). */

#include "panelNet.h"

#include <string.h>

bool panelNetKnownHostType(uint8_t type) {
  return type == PANEL_FT_SNAPSHOT || type == PANEL_FT_PING ||
         type == PANEL_FT_HELLOREQ || type == PANEL_FT_CONTROL ||
         type == PANEL_FT_PROVISION;
}

bool panelNetEpochPlausible(uint64_t wallEpoch, uint64_t buildEpoch) {
  return wallEpoch > buildEpoch;
}

bool panelNetUseWifiTx(PanelNetState state, bool tlsUp) {
  return state == PANEL_NET_LINKED && tlsUp;
}

#ifdef SOLARI_PANEL_DEVICE_NET

#include <time.h>

#include "pico/aon_timer.h"
#include "pico/cyw43_arch.h"
#include "pico/rand.h"

#include "lwip/altcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/apps/sntp.h"
#include "lwip/dns.h"
#include "lwip/netif.h"

#include "mbedtls/oid.h"
#include "mbedtls/asn1.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#define PANEL_NET_OP_TIMEOUT_MS 15000u
#define PANEL_NET_PEM_CAP 6144u

typedef enum {
  NET_IDLE = 0,
  NET_JOIN,
  NET_ADDRESS,
  NET_SNTP,
  NET_DNS,
  NET_TLS
} NetOperation;

struct PanelNet {
  const PanelProvStore *store;
  PanelParser *parser;
  PanelFrameCb frameCb;
  void *frameUser;
  PanelLink *link;
  PanelNetFsm *fsm;
  struct altcp_tls_config *tlsConfig;
  struct altcp_pcb *pcb;
  PanelProvCrypto crypto;
  NetOperation operation;
  uint32_t operationAtMs;
  uint64_t sntpEpoch;
  bool cyw43Ready;
  bool tlsUp;
  bool sntpReady;
  bool closing;
  char serverHost[254];
  char ntpHost[254];
  uint16_t serverPort;
  uint8_t clientPem[PANEL_NET_PEM_CAP];
};

static PanelNet gPanelNet;

static mbedtls_time_t mbedTime(mbedtls_time_t *out) {
  struct timespec ts;
  mbedtls_time_t value = (mbedtls_time_t)-1;
  if (aon_timer_is_running() && aon_timer_get_time(&ts) && ts.tv_sec >= 0)
    value = (mbedtls_time_t)ts.tv_sec;
  if (out != NULL) *out = value;
  return value;
}

static bool elapsed(uint32_t now, uint32_t then, uint32_t duration) {
  return (uint32_t)(now - then) >= duration;
}

static uint32_t deviceNowMs(void) {
  return to_ms_since_boot(get_absolute_time());
}

static const uint8_t *storeItem(const PanelNet *net, uint8_t id,
                                uint16_t *len) {
  return panelProvStoreItem(net->store, id, len);
}

static bool copyStoreString(const PanelNet *net, uint8_t id, char *out,
                            size_t cap) {
  uint16_t len = 0u;
  const uint8_t *item = storeItem(net, id, &len);
  if (item == NULL || (size_t)len + 1u > cap) return false;
  memcpy(out, item, len);
  out[len] = '\0';
  return true;
}

static bool derObjectLen(const uint8_t *der, size_t available,
                         size_t *objectLen) {
  size_t len = 0u, hdr = 0u, i;
  uint8_t count;
  if (der == NULL || objectLen == NULL || available < 2u || der[0] != 0x30u)
    return false;
  if ((der[1] & 0x80u) == 0u) {
    len = der[1];
    hdr = 2u;
  } else {
    count = der[1] & 0x7fu;
    if (count == 0u || count > sizeof(size_t) || available < 2u + count)
      return false;
    hdr = 2u + count;
    for (i = 0u; i < count; ++i) len = (len << 8) | der[2u + i];
  }
  if (len > available - hdr) return false;
  *objectLen = hdr + len;
  return true;
}

static const char kB64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static bool appendPem(const uint8_t *der, size_t derLen, uint8_t *out,
                      size_t cap, size_t *used) {
  static const char begin[] = "-----BEGIN CERTIFICATE-----\n";
  static const char end[] = "-----END CERTIFICATE-----\n";
  size_t i, line = 0u;
  if (*used + sizeof(begin) - 1u > cap) return false;
  memcpy(out + *used, begin, sizeof(begin) - 1u);
  *used += sizeof(begin) - 1u;
  for (i = 0u; i < derLen; i += 3u) {
    uint32_t v = (uint32_t)der[i] << 16;
    size_t remain = derLen - i;
    uint8_t enc[4];
    if (remain > 1u) v |= (uint32_t)der[i + 1u] << 8;
    if (remain > 2u) v |= der[i + 2u];
    enc[0] = (uint8_t)kB64[(v >> 18) & 63u];
    enc[1] = (uint8_t)kB64[(v >> 12) & 63u];
    enc[2] = remain > 1u ? (uint8_t)kB64[(v >> 6) & 63u] : '=';
    enc[3] = remain > 2u ? (uint8_t)kB64[v & 63u] : '=';
    if (*used + 4u + (line == 60u ? 1u : 0u) > cap) return false;
    memcpy(out + *used, enc, sizeof(enc));
    *used += sizeof(enc);
    line += 4u;
    if (line == 64u) { out[(*used)++] = '\n'; line = 0u; }
  }
  if (line != 0u) {
    if (*used == cap) return false;
    out[(*used)++] = '\n';
  }
  if (*used + sizeof(end) > cap) return false;
  memcpy(out + *used, end, sizeof(end) - 1u);
  *used += sizeof(end) - 1u;
  out[(*used)++] = '\0';
  return true;
}

/* altcp's helper accepts one DER certificate but multiple PEM certificates.
 * D8 provisions concatenated DER, so convert the bounded chain in-place to a
 * bounded static PEM buffer before giving it to the TLS stack. */
static size_t clientChainPem(PanelNet *net, const uint8_t *der, size_t len) {
  size_t at = 0u, used = 0u, objectLen;
  if (der == NULL || len == 0u) return 0u;
  while (at < len) {
    if (!derObjectLen(der + at, len - at, &objectLen) ||
        !appendPem(der + at, objectLen, net->clientPem,
                   sizeof(net->clientPem), &used)) return 0u;
    at += objectLen;
    if (used > 0u) --used; /* replace prior NUL when another cert follows */
  }
  return used + 1u;
}

static int cryptoValidCert(void *user, const uint8_t *der, uint16_t len,
                           int isCa) {
  mbedtls_x509_crt chain;
  size_t at = 0u, objectLen;
  int ok = 1;
  (void)user;
  mbedtls_x509_crt_init(&chain);
  while (at < len) {
    if (!derObjectLen(der + at, (size_t)len - at, &objectLen) ||
        mbedtls_x509_crt_parse_der(&chain, der + at, objectLen) != 0) {
      ok = 0;
      break;
    }
    at += objectLen;
  }
  if (at == 0u || (isCa && !chain.ca_istrue)) ok = 0;
  mbedtls_x509_crt_free(&chain);
  return ok;
}

static int cryptoValidKey(void *user, const uint8_t *der, uint16_t len) {
  mbedtls_pk_context key;
  int ok;
  (void)user;
  mbedtls_pk_init(&key);
  ok = mbedtls_pk_parse_key(&key, der, len, NULL, 0) == 0 &&
       mbedtls_pk_get_type(&key) != MBEDTLS_PK_NONE;
  mbedtls_pk_free(&key); /* mbedTLS zeroizes private material. */
  return ok;
}

static int cryptoKeyMatches(void *user, const uint8_t *keyDer,
                            uint16_t keyLen, const uint8_t *certDer,
                            uint16_t certLen) {
  mbedtls_pk_context key;
  mbedtls_x509_crt cert;
  size_t leafLen;
  int ok = 0;
  (void)user;
  mbedtls_pk_init(&key);
  mbedtls_x509_crt_init(&cert);
  if (derObjectLen(certDer, certLen, &leafLen) &&
      mbedtls_x509_crt_parse_der(&cert, certDer, leafLen) == 0 &&
      mbedtls_pk_parse_key(&key, keyDer, keyLen, NULL, 0) == 0 &&
      mbedtls_pk_check_pair(&cert.pk, &key) == 0) ok = 1;
  mbedtls_pk_free(&key);
  mbedtls_x509_crt_free(&cert);
  return ok;
}

static uint8_t randomByte(void *user) {
  (void)user;
  return (uint8_t)get_rand_32();
}

static void setOperation(PanelNet *net, NetOperation op) {
  net->operation = op;
  net->operationAtMs = deviceNowMs();
}

static void startJoin(void *user) {
  PanelNet *net = user;
  uint16_t ssidLen = 0u, pskLen = 0u;
  const uint8_t *ssidItem = storeItem(net, PANEL_PROV_SSID, &ssidLen);
  const uint8_t *pskItem = storeItem(net, PANEL_PROV_PSK, &pskLen);
  char ssid[33], psk[64];
  int result = -1;
  if (net->cyw43Ready && ssidItem != NULL && pskItem != NULL &&
      ssidLen < sizeof(ssid) &&
      pskLen < sizeof(psk)) {
    memcpy(ssid, ssidItem, ssidLen); ssid[ssidLen] = '\0';
    memcpy(psk, pskItem, pskLen); psk[pskLen] = '\0';
    result = cyw43_arch_wifi_connect_async(ssid, psk,
                                           CYW43_AUTH_WPA2_AES_PSK);
  }
  mbedtls_platform_zeroize(psk, sizeof(psk));
  mbedtls_platform_zeroize(ssid, sizeof(ssid));
  if (result != 0) panelNetFsmJoinResult(net->fsm, false, deviceNowMs());
  else setOperation(net, NET_JOIN);
}

static void startAddressing(void *user) { setOperation(user, NET_ADDRESS); }

static void startTimeSync(void *user) {
  PanelNet *net = user;
  net->sntpReady = false;
  sntp_stop();
  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  sntp_setservername(0, net->ntpHost);
  sntp_init();
  setOperation(net, NET_SNTP);
}

static bool hasServerAuth(const mbedtls_x509_crt *crt) {
  const mbedtls_x509_sequence *eku;
  if (crt == NULL ||
      (crt->ext_types & MBEDTLS_X509_EXT_EXTENDED_KEY_USAGE) == 0) return false;
  for (eku = &crt->ext_key_usage; eku != NULL; eku = eku->next) {
    if (MBEDTLS_OID_CMP(MBEDTLS_OID_SERVER_AUTH, &eku->buf) == 0) return true;
  }
  return false;
}

static bool hasExactDnsSan(const mbedtls_x509_crt *crt, const char *host) {
  const mbedtls_x509_sequence *san;
  size_t hostLen, i;
  if (crt == NULL || host == NULL ||
      (crt->ext_types & MBEDTLS_X509_EXT_SUBJECT_ALT_NAME) == 0) return false;
  hostLen = strlen(host);
  for (san = &crt->subject_alt_names; san != NULL; san = san->next) {
    if (san->buf.tag != (MBEDTLS_ASN1_CONTEXT_SPECIFIC | 2) ||
        san->buf.len != hostLen) continue;
    for (i = 0u; i < hostLen; ++i) {
      uint8_t a = san->buf.p[i], b = (uint8_t)host[i];
      if (a >= 'A' && a <= 'Z') a = (uint8_t)(a + ('a' - 'A'));
      if (b >= 'A' && b <= 'Z') b = (uint8_t)(b + ('a' - 'A'));
      if (a != b) break;
    }
    if (i == hostLen) return true;
  }
  return false;
}

static err_t tlsConnected(void *arg, struct altcp_pcb *pcb, err_t err) {
  PanelNet *net = arg;
  mbedtls_ssl_context *ssl = altcp_tls_context(pcb);
  const mbedtls_x509_crt *peer = ssl == NULL ? NULL :
                                      mbedtls_ssl_get_peer_cert(ssl);
  /* D6/D8: the handshake enforces chain + hostname, but mbedTLS 2.x permits
   * CN fallback and does not enforce EKU.  Require an exact DNS SAN and
   * serverAuth here before LINKED can accept any application bytes. */
  if (err != ERR_OK || ssl == NULL || mbedtls_ssl_get_verify_result(ssl) != 0u ||
      !hasExactDnsSan(peer, net->serverHost) || !hasServerAuth(peer)) {
    panelNetClose(net);
    panelNetFsmTlsResult(net->fsm, false,
                         to_ms_since_boot(get_absolute_time()));
    return ERR_ABRT;
  }
  net->tlsUp = true;
  net->operation = NET_IDLE;
  panelLinkSetActiveTransport(net->link, PANEL_LINK_WIFI);
  panelLinkResetTransportSeq(net->link, PANEL_LINK_WIFI);
  panelNetFsmTlsResult(net->fsm, true, to_ms_since_boot(get_absolute_time()));
  return ERR_OK;
}

static err_t tlsRecv(void *arg, struct altcp_pcb *pcb, struct pbuf *p,
                     err_t err) {
  PanelNet *net = arg;
  struct pbuf *part;
  uint32_t ms = to_ms_since_boot(get_absolute_time());
  if (p == NULL || err != ERR_OK) {
    if (p != NULL) pbuf_free(p);
    net->tlsUp = false;
    if (!net->closing) {
      panelNetFsmServerClosed(net->fsm, ms); /* aborts/frees the live PCB */
      return ERR_ABRT;
    }
    return ERR_OK;
  }
  for (part = p; part != NULL; part = part->next)
    panelParserFeed(net->parser, part->payload, part->len, ms,
                    net->frameCb, net->frameUser);
  altcp_recved(pcb, p->tot_len);
  pbuf_free(p);
  return ERR_OK;
}

static void tlsError(void *arg, err_t err) {
  PanelNet *net = arg;
  (void)err;
  net->pcb = NULL;
  net->tlsUp = false;
  if (!net->closing)
    panelNetFsmServerClosed(net->fsm, to_ms_since_boot(get_absolute_time()));
}

static void dnsResult(const char *name, const ip_addr_t *address, void *arg) {
  PanelNet *net = arg;
  (void)name;
  if (address == NULL || net->operation != NET_DNS) {
    if (net->operation == NET_DNS)
      panelNetFsmTlsResult(net->fsm, false,
                           to_ms_since_boot(get_absolute_time()));
    return;
  }
  net->pcb = altcp_tls_new(net->tlsConfig, IP_GET_TYPE(address));
  if (net->pcb == NULL) {
    panelNetFsmTlsResult(net->fsm, false,
                         to_ms_since_boot(get_absolute_time()));
    return;
  }
  altcp_arg(net->pcb, net);
  altcp_recv(net->pcb, tlsRecv);
  altcp_err(net->pcb, tlsError);
  if (mbedtls_ssl_set_hostname(altcp_tls_context(net->pcb),
                               net->serverHost) != 0 ||
      altcp_connect(net->pcb, address, net->serverPort, tlsConnected) != ERR_OK) {
    panelNetClose(net);
    panelNetFsmTlsResult(net->fsm, false,
                         to_ms_since_boot(get_absolute_time()));
    return;
  }
  setOperation(net, NET_TLS);
}

static void startTls(void *user) {
  PanelNet *net = user;
  const uint8_t *ca, *cert, *key;
  uint16_t caLen = 0u, certLen = 0u, keyLen = 0u;
  size_t pemLen;
  ip_addr_t address;
  err_t result;
  ca = storeItem(net, PANEL_PROV_CACERT, &caLen);
  cert = storeItem(net, PANEL_PROV_CLIENTCERT, &certLen);
  key = storeItem(net, PANEL_PROV_CLIENTKEY, &keyLen);
  pemLen = clientChainPem(net, cert, certLen);
  if (ca == NULL || key == NULL || pemLen == 0u) {
    panelNetFsmTlsResult(net->fsm, false, deviceNowMs());
    return;
  }
  net->tlsConfig = altcp_tls_create_config_client_2wayauth(
      ca, caLen, key, keyLen, NULL, 0u, net->clientPem, pemLen);
  mbedtls_platform_zeroize(net->clientPem, sizeof(net->clientPem));
  if (net->tlsConfig == NULL) {
    panelNetFsmTlsResult(net->fsm, false, deviceNowMs());
    return;
  }
  setOperation(net, NET_DNS);
  result = dns_gethostbyname(net->serverHost, &address, dnsResult, net);
  if (result == ERR_OK) dnsResult(net->serverHost, &address, net);
  else if (result != ERR_INPROGRESS)
    panelNetFsmTlsResult(net->fsm, false,
                         to_ms_since_boot(get_absolute_time()));
}

static void closeOp(void *user) { panelNetClose(user); }

PanelNet *panelNetInit(const PanelProvStore *store, PanelParser *wifiParser,
                       PanelFrameCb frameCb, void *frameUser,
                       PanelLink *link) {
  PanelNet *net = &gPanelNet;
  memset(net, 0, sizeof(*net));
  net->store = store;
  net->parser = wifiParser;
  net->frameCb = frameCb;
  net->frameUser = frameUser;
  net->link = link;
  net->crypto.validCert = cryptoValidCert;
  net->crypto.validKey = cryptoValidKey;
  net->crypto.keyMatchesCert = cryptoKeyMatches;
  /* D7: certificate validity checks read the powered-lifetime AON clock. */
  (void)mbedtls_platform_set_time(mbedTime);
  /* D18: this is unconditional; lack of credentials never skips CYW43 init. */
  net->cyw43Ready = cyw43_arch_init() == 0;
  if (net->cyw43Ready) cyw43_arch_enable_sta_mode();
  panelNetCredentialsChanged(net);
  return net;
}

void panelNetAttachFsm(PanelNet *net, PanelNetFsm *fsm) {
  if (net != NULL) net->fsm = fsm;
}

PanelNetOps panelNetOps(PanelNet *net) {
  PanelNetOps ops = { startJoin, startAddressing, startTimeSync, startTls,
                      closeOp, randomByte, net };
  return ops;
}

void panelNetCredentialsChanged(PanelNet *net) {
  uint16_t portLen = 0u;
  const uint8_t *port;
  if (net == NULL) return;
  memset(net->serverHost, 0, sizeof(net->serverHost));
  memset(net->ntpHost, 0, sizeof(net->ntpHost));
  net->serverPort = 0u;
  if (net->store == NULL || !net->store->valid ||
      !copyStoreString(net, PANEL_PROV_SERVERHOST, net->serverHost,
                       sizeof(net->serverHost))) return;
  port = storeItem(net, PANEL_PROV_SERVERPORT, &portLen);
  if (port != NULL && portLen == 2u)
    net->serverPort = (uint16_t)(port[0] | ((uint16_t)port[1] << 8));
  if (!copyStoreString(net, PANEL_PROV_NTPHOST, net->ntpHost,
                       sizeof(net->ntpHost)))
    memcpy(net->ntpHost, net->serverHost, sizeof(net->ntpHost));
}

void panelNetClose(PanelNet *net) {
  if (net == NULL) return;
  net->closing = true;
  sntp_stop();
  if (net->pcb != NULL) {
    altcp_abort(net->pcb); /* D14: synchronous close, never linger. */
    net->pcb = NULL;
  }
  if (net->tlsConfig != NULL) {
    altcp_tls_free_config(net->tlsConfig);
    net->tlsConfig = NULL;
  }
  net->tlsUp = false;
  net->operation = NET_IDLE;
  net->closing = false;
}

void panelNetPoll(PanelNet *net, uint32_t nowMs) {
  int link;
  if (net == NULL || !net->cyw43Ready || net->fsm == NULL) return;
  cyw43_arch_poll(); /* poll mode is bounded and matches the single 40 ms loop. */
  link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
  if (net->operation == NET_JOIN) {
    if (link >= CYW43_LINK_NOIP) {
      net->operation = NET_IDLE;
      panelNetFsmJoinResult(net->fsm, true, nowMs);
    } else if (link < 0 || elapsed(nowMs, net->operationAtMs,
                                  PANEL_NET_OP_TIMEOUT_MS)) {
      net->operation = NET_IDLE;
      panelNetFsmJoinResult(net->fsm, false, nowMs);
    }
  } else if (net->operation == NET_ADDRESS) {
    if (netif_default != NULL &&
        !ip4_addr_isany_val(*netif_ip4_addr(netif_default))) {
      net->operation = NET_IDLE;
      panelNetFsmAddressResult(net->fsm, true, nowMs);
    } else if (elapsed(nowMs, net->operationAtMs, PANEL_NET_OP_TIMEOUT_MS)) {
      net->operation = NET_IDLE;
      panelNetFsmAddressResult(net->fsm, false, nowMs);
    }
  } else if (net->operation == NET_SNTP) {
    if (net->sntpReady) {
      sntp_stop(); net->operation = NET_IDLE;
      panelNetFsmTimeResult(net->fsm, true, net->sntpEpoch, nowMs);
    } else if (elapsed(nowMs, net->operationAtMs, PANEL_NET_OP_TIMEOUT_MS)) {
      sntp_stop(); net->operation = NET_IDLE;
      panelNetFsmTimeResult(net->fsm, false, 0u, nowMs);
    }
  } else if ((net->operation == NET_DNS || net->operation == NET_TLS) &&
             elapsed(nowMs, net->operationAtMs, PANEL_NET_OP_TIMEOUT_MS)) {
    panelNetClose(net);
    panelNetFsmTlsResult(net->fsm, false, nowMs);
  } else if (net->fsm->state == PANEL_NET_LINKED && link < CYW43_LINK_NOIP) {
    panelNetFsmApLost(net->fsm, nowMs);
  }
}

bool panelNetTlsUp(const PanelNet *net) { return net != NULL && net->tlsUp; }

bool panelNetWrite(PanelNet *net, const uint8_t *bytes, size_t len) {
  if (!panelNetTlsUp(net) || bytes == NULL || len == 0u || len > 0xffffu)
    return false;
  if (altcp_write(net->pcb, bytes, (u16_t)len, TCP_WRITE_FLAG_COPY) != ERR_OK)
    return false;
  return altcp_output(net->pcb) == ERR_OK;
}

bool panelNetWallTime(const PanelNet *net, uint64_t *epochOut) {
  struct timespec ts;
  (void)net;
  if (epochOut == NULL || !aon_timer_is_running() || !aon_timer_get_time(&ts) ||
      ts.tv_sec < 0) return false;
  *epochOut = (uint64_t)ts.tv_sec;
  return true;
}

const PanelProvCrypto *panelNetProvCrypto(const PanelNet *net) {
  return net == NULL ? NULL : &net->crypto;
}

/* lwIP SNTP calls this macro target from its own translation unit.  AON keeps
 * advancing through WiFi/USB sessions while the board remains powered (D7). */
void panelNetSntpSetTime(uint32_t seconds) {
  struct timespec ts = { (time_t)seconds, 0 };
  PanelNet *net = &gPanelNet;
  if (net->fsm == NULL ||
      !panelNetEpochPlausible(seconds, net->fsm->buildEpoch)) return;
  if (aon_timer_is_running()) (void)aon_timer_set_time(&ts);
  else (void)aon_timer_start(&ts);
  net->sntpEpoch = seconds;
  net->sntpReady = true;
}

#else /* SOLARI_PANEL_DEVICE_NET */

/* Host builds only compile the pure decisions above. */

#endif
