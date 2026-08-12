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
  int commits, wipes;
} Fake;

static void onFrame(uint8_t type, const uint8_t *payload, size_t len,
                    void *user) {
  Fake *fk = (Fake *)user;
  uint8_t ack[PANEL_PROVACK_SIZE];
  uint8_t frame[PANEL_HDR_SIZE + PANEL_PROVACK_SIZE + PANEL_CRC_SIZE];
  size_t an, fn;
  if (type != PANEL_FT_PROVISION) return;
  an = panelProvHandle(&fk->prov, payload, len, false, &fk->store, &fk->flash,
                       NULL, ack, sizeof(ack));
  if (an == 0u) return;
  if (len >= 1u && payload[0] == PANEL_PROV_COMMIT && ack[1] == PANEL_PROVST_OK)
    ++fk->commits;
  if (len >= 1u && payload[0] == PANEL_PROV_WIPE && ack[1] == PANEL_PROVST_OK)
    ++fk->wipes;
  fn = panelEncodeFrame(PANEL_FT_PROVACK, ack, an, frame, sizeof(frame));
  if (fn) (void)!write(fk->fd, frame, fn);
}

int main(void) {
  PanelParser parser;
  Fake fk;
  int master = posix_openpt(O_RDWR | O_NOCTTY);
  const char *slave;
  long long served = 0;

  if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0 ||
      (slave = ptsname(master)) == NULL) {
    perror("fakePanel: pty");
    return 2;
  }
  /* Hold a slave fd ourselves so the pty survives between the smoke's two
   * panelProv invocations — otherwise the first close() tears it down. */
  if (open(slave, O_RDWR | O_NOCTTY) < 0) {
    perror("fakePanel: slave hold");
    return 2;
  }
  /* Line 1 of stdout is the contract with smoke.sh. */
  printf("%s\n", slave);
  fflush(stdout);

  memset(gSectors, 0xff, sizeof(gSectors));
  memset(&fk, 0, sizeof(fk));
  fk.fd = master;
  fk.flash.readSlot = ramRead;
  fk.flash.writeSlot = ramWrite;
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
  fprintf(stderr, "fakePanel: %d commit(s), %d wipe(s), store %s\n",
          fk.commits, fk.wipes, fk.store.valid ? "valid" : "empty");
  return (fk.commits > 0 && fk.wipes > 0 && !fk.store.valid) ? 0 : 1;
}
