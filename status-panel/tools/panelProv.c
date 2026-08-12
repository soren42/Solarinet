/* panelProv.c — USB provisioning tool for the SolariNet status panel.
 * CONTRACT-SW §13 D1-D4: lockstep PROVISION/PROVACK over the panel's USB-CDC
 * frame protocol (protocol.c).
 *
 * Usage:
 *   panelProv --dev /dev/serial/by-id/... --conf panel-prov.conf
 *   panelProv --dev /dev/serial/by-id/... --wipe
 *
 * Conf file, key=value one per line, '#' comments. SECRETS STAY IN FILES —
 * the tool never echoes psk or key material, and the conf references the
 * sensitive items by path:
 *   ssid=SolariNet-Panel
 *   pskFile=/path/to/psk.txt          # 8..63 bytes, trailing newline trimmed
 *   serverHost=xenon.akoria.net
 *   serverPort=8443
 *   caCert=/path/to/ca.der            # DER, not PEM
 *   clientCert=/path/to/client.der    # leaf then intermediates, concatenated
 *   clientKey=/path/to/client.key.der
 *   ntpHost=chlorine.akoria.net       # optional
 *
 * Every item is sent in PANEL_PROV_CHUNK slices; each slice waits for its
 * PROVACK (2 s timeout, 3 tries). On PANEL_PROVST_OFFSET_MISMATCH the tool
 * resumes at the acked nextOffset. The run ends with commit (or is --wipe). */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "../protocol.h"

#define ACK_TIMEOUT_MS 2000
#define ACK_TRIES 3

typedef struct {
  uint8_t itemId;
  const char *name;
  uint8_t *data;
  uint16_t len;
} Item;

static const char *statusName(uint8_t s) {
  switch (s) {
    case PANEL_PROVST_OK:              return "ok";
    case PANEL_PROVST_FORMAT_INVALID:  return "format-invalid";
    case PANEL_PROVST_STORAGE_FULL:    return "storage-full";
    case PANEL_PROVST_REJECTED_WIFI:   return "rejected-on-wifi";
    case PANEL_PROVST_COMMIT_INVALID:  return "commit-invalid";
    case PANEL_PROVST_OFFSET_MISMATCH: return "offset-mismatch";
    case PANEL_PROVST_FLASH_IO:        return "flash-io";
    case PANEL_PROVST_CRYPTO_INVALID:  return "crypto-invalid";
    case PANEL_PROVST_BUSY:            return "busy";
    default:                           return "unknown-status";
  }
}

/* ---- serial -------------------------------------------------------------- */

static int openSerial(const char *dev) {
  struct termios tio;
  int fd = open(dev, O_RDWR | O_NOCTTY);
  if (fd < 0) {
    fprintf(stderr, "panelProv: open %s: %s\n", dev, strerror(errno));
    return -1;
  }
  if (tcgetattr(fd, &tio) != 0) {
    fprintf(stderr, "panelProv: tcgetattr: %s\n", strerror(errno));
    close(fd);
    return -1;
  }
  cfmakeraw(&tio);
  /* USB-CDC ignores the baud rate, but termios wants one set. */
  cfsetispeed(&tio, B115200);
  cfsetospeed(&tio, B115200);
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 1; /* 100 ms read granularity; the ack loop keeps time */
  if (tcsetattr(fd, TCSANOW, &tio) != 0) {
    fprintf(stderr, "panelProv: tcsetattr: %s\n", strerror(errno));
    close(fd);
    return -1;
  }
  tcflush(fd, TCIOFLUSH);
  return fd;
}

static long long nowMsClock(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* PROVACK receiver state: the panel interleaves HELLO/STATE/EVENT/LOG traffic
 * with our acks, so run the real parser and keep only PROVACK frames. */
typedef struct {
  int got;
  uint8_t itemId, status, generation;
  uint16_t nextOffset;
} AckWait;

static void onFrame(uint8_t type, const uint8_t *payload, size_t len,
                    void *user) {
  AckWait *w = (AckWait *)user;
  if (type != PANEL_FT_PROVACK || w->got) return;
  if (panelDecodeProvAck(payload, len, &w->itemId, &w->status, &w->generation,
                         &w->nextOffset) == 0)
    w->got = 1;
}

/* Send one PROVISION and wait for its PROVACK. Returns the status, or -1 on
 * timeout/IO error. */
static int transact(int fd, PanelParser *parser, uint8_t itemId,
                    uint8_t generation, uint16_t offset, const uint8_t *data,
                    uint16_t dataLen, AckWait *ack) {
  uint8_t payload[PANEL_MAX_PAYLOAD];
  uint8_t frame[PANEL_HDR_SIZE + PANEL_MAX_PAYLOAD + PANEL_CRC_SIZE];
  int attempt;
  size_t pn = panelEncodeProvision(itemId, generation, offset, data, dataLen,
                                   payload, sizeof(payload));
  size_t fn = pn ? panelEncodeFrame(PANEL_FT_PROVISION, payload, pn, frame,
                                    sizeof(frame))
                 : 0;
  if (fn == 0) {
    fprintf(stderr, "panelProv: internal encode failure (item %u)\n", itemId);
    return -1;
  }
  for (attempt = 0; attempt < ACK_TRIES; ++attempt) {
    long long deadline = nowMsClock() + ACK_TIMEOUT_MS;
    memset(ack, 0, sizeof(*ack));
    if (write(fd, frame, fn) != (ssize_t)fn) {
      fprintf(stderr, "panelProv: write: %s\n", strerror(errno));
      return -1;
    }
    while (nowMsClock() < deadline) {
      uint8_t buf[256];
      ssize_t n = read(fd, buf, sizeof(buf));
      if (n < 0) {
        fprintf(stderr, "panelProv: read: %s\n", strerror(errno));
        return -1;
      }
      if (n > 0)
        panelParserFeed(parser, buf, (size_t)n, (uint32_t)nowMsClock(),
                        onFrame, ack);
      if (ack->got) {
        if (ack->itemId != itemId)
          continue; /* stale ack from a previous run; keep waiting */
        return ack->status;
      }
    }
    fprintf(stderr, "panelProv: item %u offset %u: no PROVACK, retry %d/%d\n",
            itemId, offset, attempt + 1, ACK_TRIES);
  }
  return -1;
}

/* Send one whole item, resuming at acked watermarks. */
static int sendItem(int fd, PanelParser *parser, uint8_t generation,
                    const Item *item) {
  uint16_t offset = 0u;
  AckWait ack;
  do {
    uint16_t chunk = (uint16_t)(item->len - offset);
    int status;
    if (chunk > PANEL_PROV_CHUNK) chunk = PANEL_PROV_CHUNK;
    status = transact(fd, parser, item->itemId, generation, offset,
                      item->data + offset, chunk, &ack);
    if (status < 0) return -1;
    if (status == PANEL_PROVST_OFFSET_MISMATCH && ack.nextOffset < item->len) {
      fprintf(stderr, "panelProv: %s: resuming at offset %u\n", item->name,
              ack.nextOffset);
      offset = ack.nextOffset;
      continue;
    }
    if (status != PANEL_PROVST_OK) {
      fprintf(stderr, "panelProv: %s: panel refused: %s\n", item->name,
              statusName((uint8_t)status));
      return -1;
    }
    offset = ack.nextOffset;
  } while (offset < item->len);
  printf("panelProv: %-10s %5u bytes staged\n", item->name, item->len);
  return 0;
}

/* ---- conf loading -------------------------------------------------------- */

static uint8_t *readWholeFile(const char *path, uint16_t maxLen,
                              uint16_t *lenOut, int trimNewline) {
  FILE *f = fopen(path, "rb");
  long size;
  uint8_t *buf;
  if (f == NULL) {
    fprintf(stderr, "panelProv: %s: %s\n", path, strerror(errno));
    return NULL;
  }
  if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
      fseek(f, 0, SEEK_SET) != 0) {
    fprintf(stderr, "panelProv: %s: seek failed\n", path);
    fclose(f);
    return NULL;
  }
  if (size > (long)maxLen + 2) { /* +2: tolerated trailing newline/CR */
    fprintf(stderr, "panelProv: %s: %ld bytes exceeds the %u-byte bound\n",
            path, size, maxLen);
    fclose(f);
    return NULL;
  }
  buf = malloc((size_t)size + 1);
  if (buf == NULL || fread(buf, 1, (size_t)size, f) != (size_t)size) {
    fprintf(stderr, "panelProv: %s: read failed\n", path);
    free(buf);
    fclose(f);
    return NULL;
  }
  fclose(f);
  while (trimNewline && size > 0 &&
         (buf[size - 1] == '\n' || buf[size - 1] == '\r'))
    --size;
  if (size > (long)maxLen) {
    fprintf(stderr, "panelProv: %s: exceeds the %u-byte bound\n", path, maxLen);
    free(buf);
    return NULL;
  }
  *lenOut = (uint16_t)size;
  return buf;
}

static uint8_t *dupString(const char *s, uint16_t maxLen, uint16_t *lenOut) {
  size_t n = strlen(s);
  uint8_t *buf;
  if (n == 0u || n > maxLen) {
    fprintf(stderr, "panelProv: value '%s' violates the 1..%u-byte bound\n", s,
            maxLen);
    return NULL;
  }
  buf = malloc(n);
  if (buf == NULL) return NULL;
  memcpy(buf, s, n);
  *lenOut = (uint16_t)n;
  return buf;
}

typedef struct {
  Item items[8];
  int count;
} ItemSet;

static int confAdd(ItemSet *set, uint8_t itemId, const char *name,
                   uint8_t *data, uint16_t len) {
  if (data == NULL) return -1;
  set->items[set->count].itemId = itemId;
  set->items[set->count].name = name;
  set->items[set->count].data = data;
  set->items[set->count].len = len;
  ++set->count;
  return 0;
}

static int loadConf(const char *path, ItemSet *set) {
  char line[512];
  int haveSsid = 0, havePsk = 0, haveHost = 0, havePort = 0, haveCa = 0,
      haveCert = 0, haveKey = 0;
  FILE *f = fopen(path, "r");
  if (f == NULL) {
    fprintf(stderr, "panelProv: conf %s: %s\n", path, strerror(errno));
    return -1;
  }
  set->count = 0;
  while (fgets(line, sizeof(line), f) != NULL) {
    char *eq, *key = line, *val;
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (line[0] == '#' || line[0] == 0) continue;
    eq = strchr(line, '=');
    if (eq == NULL) {
      fprintf(stderr, "panelProv: conf line without '=': %s\n", line);
      fclose(f);
      return -1;
    }
    *eq = 0;
    val = eq + 1;
    if (strcmp(key, "ssid") == 0) {
      uint16_t len;
      uint8_t *d = dupString(val, PANEL_PROV_MAX_SSID, &len);
      if (confAdd(set, PANEL_PROV_SSID, "ssid", d, len)) goto fail;
      haveSsid = 1;
    } else if (strcmp(key, "pskFile") == 0) {
      uint16_t len;
      uint8_t *d = readWholeFile(val, PANEL_PROV_MAX_PSK, &len, 1);
      if (d != NULL && len < PANEL_PROV_MIN_PSK) {
        fprintf(stderr, "panelProv: psk shorter than %u bytes\n",
                PANEL_PROV_MIN_PSK);
        free(d);
        d = NULL;
      }
      if (confAdd(set, PANEL_PROV_PSK, "psk", d, len)) goto fail;
      havePsk = 1;
    } else if (strcmp(key, "serverHost") == 0) {
      uint16_t len;
      uint8_t *d = dupString(val, PANEL_PROV_MAX_HOST, &len);
      if (confAdd(set, PANEL_PROV_SERVERHOST, "serverHost", d, len)) goto fail;
      haveHost = 1;
    } else if (strcmp(key, "serverPort") == 0) {
      char *end = NULL;
      long port = strtol(val, &end, 10);
      uint8_t *d;
      if (end == val || *end != 0 || port < 1 || port > 65535) {
        fprintf(stderr, "panelProv: bad serverPort '%s'\n", val);
        goto fail;
      }
      d = malloc(2);
      if (d == NULL) goto fail;
      d[0] = (uint8_t)(port & 0xff); /* u16 LE on the wire */
      d[1] = (uint8_t)(port >> 8);
      if (confAdd(set, PANEL_PROV_SERVERPORT, "serverPort", d, 2u)) goto fail;
      havePort = 1;
    } else if (strcmp(key, "caCert") == 0) {
      uint16_t len;
      uint8_t *d = readWholeFile(val, PANEL_PROV_MAX_CACERT, &len, 0);
      if (confAdd(set, PANEL_PROV_CACERT, "caCert", d, len)) goto fail;
      haveCa = 1;
    } else if (strcmp(key, "clientCert") == 0) {
      uint16_t len;
      uint8_t *d = readWholeFile(val, PANEL_PROV_MAX_CLIENTCERT, &len, 0);
      if (confAdd(set, PANEL_PROV_CLIENTCERT, "clientCert", d, len)) goto fail;
      haveCert = 1;
    } else if (strcmp(key, "clientKey") == 0) {
      uint16_t len;
      uint8_t *d = readWholeFile(val, PANEL_PROV_MAX_CLIENTKEY, &len, 0);
      if (confAdd(set, PANEL_PROV_CLIENTKEY, "clientKey", d, len)) goto fail;
      haveKey = 1;
    } else if (strcmp(key, "ntpHost") == 0) {
      uint16_t len;
      uint8_t *d = dupString(val, PANEL_PROV_MAX_HOST, &len);
      if (confAdd(set, PANEL_PROV_NTPHOST, "ntpHost", d, len)) goto fail;
    } else {
      fprintf(stderr, "panelProv: unknown conf key '%s'\n", key);
      goto fail;
    }
  }
  fclose(f);
  if (!haveSsid || !havePsk || !haveHost || !havePort || !haveCa ||
      !haveCert || !haveKey) {
    fprintf(stderr,
            "panelProv: conf incomplete — required: ssid pskFile serverHost "
            "serverPort caCert clientCert clientKey\n");
    return -1;
  }
  return 0;
fail:
  fclose(f);
  return -1;
}

/* ---- main ---------------------------------------------------------------- */

static void usage(void) {
  fprintf(stderr,
          "usage: panelProv --dev /dev/serial/by-id/... --conf FILE\n"
          "       panelProv --dev /dev/serial/by-id/... --wipe\n");
}

int main(int argc, char **argv) {
  const char *dev = NULL, *conf = NULL;
  int wipe = 0, i, fd, status;
  uint8_t generation;
  PanelParser parser;
  ItemSet set;
  AckWait ack;

  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--dev") == 0 && i + 1 < argc) dev = argv[++i];
    else if (strcmp(argv[i], "--conf") == 0 && i + 1 < argc) conf = argv[++i];
    else if (strcmp(argv[i], "--wipe") == 0) wipe = 1;
    else { usage(); return 2; }
  }
  if (dev == NULL || (conf == NULL && !wipe) || (conf != NULL && wipe)) {
    usage();
    return 2;
  }

  fd = openSerial(dev);
  if (fd < 0) return 1;
  panelParserInit(&parser);

  /* The generation only has to differ from whatever session the panel may
   * have staged before; wall-clock seconds is plenty for a hand-run tool. */
  generation = (uint8_t)(time(NULL) & 0xffu);

  if (wipe) {
    status = transact(fd, &parser, PANEL_PROV_WIPE, generation, 0u, NULL, 0u,
                      &ack);
    close(fd);
    if (status == PANEL_PROVST_OK) {
      printf("panelProv: wipe acknowledged — credentials cleared\n");
      return 0;
    }
    fprintf(stderr, "panelProv: wipe failed: %s\n",
            status < 0 ? "no PROVACK" : statusName((uint8_t)status));
    return 1;
  }

  if (loadConf(conf, &set) != 0) {
    close(fd);
    return 1;
  }
  printf("panelProv: provisioning via %s (generation %u)\n", dev, generation);
  for (i = 0; i < set.count; ++i) {
    if (sendItem(fd, &parser, generation, &set.items[i]) != 0) {
      close(fd);
      return 1;
    }
  }
  status = transact(fd, &parser, PANEL_PROV_COMMIT, generation, 0u, NULL, 0u,
                    &ack);
  close(fd);
  if (status == PANEL_PROVST_OK) {
    printf("panelProv: COMMIT ok — credentials persisted (A/B store)\n");
    return 0;
  }
  fprintf(stderr, "panelProv: commit failed: %s\n",
          status < 0 ? "no PROVACK" : statusName((uint8_t)status));
  return 1;
}
