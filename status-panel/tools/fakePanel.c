/* fakePanel.c — pty test double for panelProv.
 *
 * Opens a pseudo-terminal, prints the slave path on stdout (line 1), then
 * serves the REAL provisioning stack — protocol.c parser + panelProv.c
 * handler + panelProvStore.c over a RAM fake flash — until stdin closes or a
 * commit and a wipe have both been observed. This lets the smoke test drive
 * the actual panelProv binary end-to-end with no hardware:
 *
 *   slave=$(./fakePanel & read line; echo $line)   # see smoke.sh
 *   ./panelProv --dev $slave --conf throwaway.conf
 *
 * Exit code 0 iff at least one commit landed a CRC-valid record. */

#define _XOPEN_SOURCE 600

#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../protocol.h"
#include "../firmware/panelProv.h"
#include "../firmware/panelProvStore.h"

static uint8_t gSectors[2][PANEL_PROV_SECTOR_SIZE];

static bool ramRead(void *user, uint8_t slot, uint8_t *out, size_t len) {
  (void)user;
  if (slot > 1u || len > PANEL_PROV_SECTOR_SIZE) return false;
  memcpy(out, gSectors[slot], len);
  return true;
}

static bool ramWrite(void *user, uint8_t slot, const uint8_t *data,
                     size_t len) {
  (void)user;
  if (slot > 1u || len > PANEL_PROV_SECTOR_SIZE) return false;
  memset(gSectors[slot], 0xff, PANEL_PROV_SECTOR_SIZE);
  memcpy(gSectors[slot], data, len);
  return true;
}

typedef struct {
  int fd;
  PanelProv prov;
  PanelProvStore store;
  PanelProvFlash flash;
  PanelProvCrypto crypto;
  int commits, wipes;
  /* Fault injection (final review MUST-8): exercise panelProv's timeout
   * retransmit and offset-mismatch resume paths against the REAL handler.
   *   dropAck: swallow the PROVACK of the Nth PROVISION frame (1-based) —
   *            the tool must time out and retransmit; the handler's
   *            idempotent re-ack then advances it.
   *   desync:  answer the Nth DATA frame (1-based) with a FORGED ok WITHOUT
   *            handling it — the tool advances past the handler's watermark,
   *            the next real frame draws a genuine OFFSET_MISMATCH, and the
   *            tool must resume at the returned watermark. */
  int dropAck, desync, desyncDone;
  int provSeen, dataSeen;
} Fake;

/* Accept-all crypto seam: the handler is fail-closed (a NULL seam refuses
 * every commit with _CRYPTO_INVALID), and the smoke stages pattern blobs, not
 * real DER, so this double accepts structure it never inspects. */
static int okCert(void *user, const uint8_t *der, uint16_t len, int isCa) {
  (void)user;
  (void)der;
  (void)len;
  (void)isCa;
  return 1;
}

static int okKey(void *user, const uint8_t *der, uint16_t len) {
  (void)user;
  (void)der;
  (void)len;
  return 1;
}

static int okMatch(void *user, const uint8_t *keyDer, uint16_t keyLen,
                   const uint8_t *certDer, uint16_t certLen) {
  (void)user;
  (void)keyDer;
  (void)keyLen;
  (void)certDer;
  (void)certLen;
  return 1;
}

static void onFrame(uint8_t type, const uint8_t *payload, size_t len,
                    void *user) {
  Fake *fk = (Fake *)user;
  uint8_t ack[PANEL_PROVACK_SIZE];
  uint8_t frame[PANEL_HDR_SIZE + PANEL_PROVACK_SIZE + PANEL_CRC_SIZE];
  size_t an, fn;
  if (type != PANEL_FT_PROVISION) return;
  ++fk->provSeen;
  if (len >= 1u && payload[0] != PANEL_PROV_COMMIT &&
      payload[0] != PANEL_PROV_WIPE) {
    ++fk->dataSeen;
    if (fk->desync > 0 && !fk->desyncDone && fk->dataSeen >= fk->desync) {
      uint8_t itemId, generation;
      uint16_t offset, dataLen;
      const uint8_t *data;
      /* Fire only on a FULL chunk: a full chunk is never its item's last
       * (no staged item is an exact multiple of PANEL_PROV_CHUNK in the
       * smoke), so the tool's NEXT frame continues the same item and the
       * handler's stale watermark produces a genuine OFFSET_MISMATCH. A
       * short final chunk would instead silently skip the whole item and
       * surface only at commit. */
      if (panelDecodeProvision(payload, len, &itemId, &generation, &offset,
                               &data, &dataLen) == 0 &&
          dataLen == PANEL_PROV_CHUNK) {
        fk->desyncDone = 1;
        an = panelEncodeProvAck(itemId, PANEL_PROVST_OK, generation,
                                (uint16_t)(offset + dataLen), ack, sizeof(ack));
        fn = panelEncodeFrame(PANEL_FT_PROVACK, ack, an, frame, sizeof(frame));
        if (fn) (void)!write(fk->fd, frame, fn);
        return;   /* handler never saw it: its watermark is now behind */
      }
    }
  }
  an = panelProvHandle(&fk->prov, payload, len, false, &fk->store, &fk->flash,
                       &fk->crypto, ack, sizeof(ack));
  if (an == 0u) return;
  if (fk->dropAck > 0 && fk->provSeen == fk->dropAck) return;
  if (len >= 1u && payload[0] == PANEL_PROV_COMMIT && ack[1] == PANEL_PROVST_OK)
    ++fk->commits;
  if (len >= 1u && payload[0] == PANEL_PROV_WIPE && ack[1] == PANEL_PROVST_OK)
    ++fk->wipes;
  fn = panelEncodeFrame(PANEL_FT_PROVACK, ack, an, frame, sizeof(frame));
  if (fn) (void)!write(fk->fd, frame, fn);
}

int main(int argc, char **argv) {
  PanelParser parser;
  Fake fk;
  int master = posix_openpt(O_RDWR | O_NOCTTY);
  const char *slave;
  long long served = 0;
  int i, dropAck = 0, desync = 0;

  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--drop-ack") == 0 && i + 1 < argc)
      dropAck = atoi(argv[++i]);
    else if (strcmp(argv[i], "--desync") == 0 && i + 1 < argc)
      desync = atoi(argv[++i]);
    else {
      fprintf(stderr, "fakePanel: usage: fakePanel [--drop-ack N] "
                      "[--desync N]\n");
      return 2;
    }
  }

  if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0 ||
      (slave = ptsname(master)) == NULL) {
    perror("fakePanel: pty");
    return 2;
  }
  /* Hold a slave fd ourselves so the pty survives between the smoke's two
   * panelProv invocations — otherwise the first close() tears it down. */
  int heldSlave = open(slave, O_RDWR | O_NOCTTY);
  if (heldSlave < 0) {
    perror("fakePanel: slave hold");
    return 2;
  }
  /* Line 1 of stdout is the contract with smoke.sh. */
  printf("%s\n", slave);
  fflush(stdout);

  memset(gSectors, 0xff, sizeof(gSectors));
  memset(&fk, 0, sizeof(fk));
  fk.fd = master;
  fk.dropAck = dropAck;
  fk.desync = desync;
  fk.flash.readSlot = ramRead;
  fk.flash.writeSlot = ramWrite;
  fk.crypto.validCert = okCert;
  fk.crypto.validKey = okKey;
  fk.crypto.keyMatchesCert = okMatch;
  fk.crypto.user = NULL;
  panelProvInit(&fk.prov);
  panelProvStoreLoad(&fk.store, &fk.flash);
  panelParserInit(&parser);

  /* Serve until a commit AND a wipe have both been seen (the smoke drives
   * both), with a hard byte budget so a wedged test cannot hang forever. */
  while ((fk.commits == 0 || fk.wipes == 0) && served < (1 << 22)) {
    uint8_t buf[512];
    ssize_t n = read(master, buf, sizeof(buf));
    if (n <= 0) break;
    served += n;
    panelParserFeed(&parser, buf, (size_t)n, (uint32_t)served, onFrame, &fk);
  }
  /* Teardown race guard: the final PROVACK may still sit unread in the
   * slave input queue when the loop condition goes false. Closing the
   * master then destroys the pty and the client sees EIO instead of the
   * ack. FIONREAD on our held slave fd reports the shared queue, so wait
   * (bounded) until the client has actually consumed it. */
  {
    int spins;
    for (spins = 0; spins < 200; ++spins) {
      int queued = 0;
      struct timespec nap = {0, 10000000L};
      if (ioctl(heldSlave, FIONREAD, &queued) != 0 || queued == 0) break;
      nanosleep(&nap, NULL);
    }
  }
  fprintf(stderr, "fakePanel: %d commit(s), %d wipe(s), store %s\n",
          fk.commits, fk.wipes, fk.store.valid ? "valid" : "empty");
  return (fk.commits > 0 && fk.wipes > 0 && !fk.store.valid) ? 0 : 1;
}
