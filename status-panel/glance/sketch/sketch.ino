/*
 * solari-glance — MCU half of the UNO Q on-board 8x13 matrix glance.
 *
 * The Linux side (python/main.py) renders panel health into a 104-byte
 * grayscale frame (values 0..7, row-major, 13 columns x 8 rows) and pushes
 * it here over the Arduino router bridge as the "draw" provider.
 *
 * Input:  "draw" bridge calls carrying the frame bytes.
 * Output: the frame on the LED matrix; if Linux goes quiet for 10 s the
 *         sketch breathes a single corner pixel so a dead feed is visibly
 *         different from an all-quiet fleet.
 */

#include <Arduino_RouterBridge.h>
#include <Arduino_LED_Matrix.h>
#include <vector>
#include <zephyr/kernel.h>

#define GLANCE_W 13
#define GLANCE_H 8
#define GLANCE_BYTES (GLANCE_W * GLANCE_H)
#define STALE_MS 10000UL

Arduino_LED_Matrix matrix;

/* Bridge providers run on their own thread; serialize matrix writes. */
K_MUTEX_DEFINE(drawMtx);

static volatile unsigned long lastDrawMs = 0;

/* draw — bridge provider: blit one full frame from the Linux side. */
void draw(std::vector<uint8_t> frame) {
  if (frame.size() < GLANCE_BYTES) return;
  k_mutex_lock(&drawMtx, K_FOREVER);
  matrix.draw(frame.data());
  lastDrawMs = millis();
  k_mutex_unlock(&drawMtx);
}

void setup() {
  matrix.begin();
  matrix.setGrayscaleBits(3);   /* accept 0..7 brightness per pixel */
  matrix.clear();
  Bridge.begin();
  Bridge.provide("draw", draw);
}

void loop() {
  /* Stale watchdog: no frame for STALE_MS -> breathe the top-left pixel.
     A triangle wave, period ~2 s, so it reads as "alive but unfed". */
  static unsigned long lastTick = 0;
  unsigned long now = millis();
  if (now - lastTick < 100) return;
  lastTick = now;
  if (now - lastDrawMs <= STALE_MS) return;

  uint8_t frame[GLANCE_BYTES];
  memset(frame, 0, sizeof(frame));
  unsigned long phase = (now / 100) % 20;          /* 0..19 over 2 s   */
  uint8_t level = (uint8_t)(phase < 10 ? phase : 19 - phase) * 7 / 9;
  frame[0] = level;
  k_mutex_lock(&drawMtx, K_FOREVER);
  if (now - lastDrawMs > STALE_MS) matrix.draw(frame);
  k_mutex_unlock(&drawMtx);
}
