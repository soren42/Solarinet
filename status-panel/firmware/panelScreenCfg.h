/*
 * panelScreenCfg.h — persistent per-screen rotation configuration.
 *
 * CONTRACT-AW.md §3.4 / §4. This unit keeps the 12 flattened theme*3+screen
 * settings and all decision logic hardware-free. The optional flash seam lets
 * the host suite exercise exactly the same validation, debounce and record
 * format as firmware without importing pico-sdk headers.
 */

#ifndef SOLARI_PANEL_SCREEN_CFG_H
#define SOLARI_PANEL_SCREEN_CFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PANEL_SCREEN_COUNT 12u
#define PANEL_SCREEN_CFG_DEBOUNCE_MS 2000u

typedef struct {
  bool (*read)(void *user, uint8_t *out, size_t len);
  bool (*writeSector)(void *user, const uint8_t *data, size_t len);
  void *user;
} PanelScreenCfgFlash;

typedef struct {
  uint8_t screenCfg[PANEL_SCREEN_COUNT];
  uint32_t changedMs;
  bool dirty;
  bool persisted; /* current image was loaded from or written to flash */
} PanelScreenCfg;

/* panelScreenCfgLoad — load a CRC-checked config or factory defaults.
 * Input: config, optional flash seam. Output: config ready for use. */
void panelScreenCfgLoad(PanelScreenCfg *cfg, const PanelScreenCfgFlash *flash,
                        uint32_t nowMs);

/* panelScreenCfgSet — change one packed config byte and begin debounce.
 * Input: config/index/new packed value/time. Output: true only on a change. */
bool panelScreenCfgSet(PanelScreenCfg *cfg, uint8_t index, uint8_t value,
                       uint32_t nowMs);

/* panelScreenCfgSaveDue — write a changed config no sooner than two seconds.
 * Input: config/flash/current ms. Output: true when a write succeeded. */
bool panelScreenCfgSaveDue(PanelScreenCfg *cfg, const PanelScreenCfgFlash *flash,
                           uint32_t nowMs);

/* panelScreenCfgEnabled — apply the per-theme all-disabled safety fallback.
 * Input: config/theme/screen. Output: whether rotation should include it. */
bool panelScreenCfgEnabled(const PanelScreenCfg *cfg, uint8_t theme,
                           uint8_t screen);

/* panelScreenCfgNext — next rotation screen, skipping disabled entries.
 * Input: config/theme/current screen. Output: a legal screen 0..2. */
uint8_t panelScreenCfgNext(const PanelScreenCfg *cfg, uint8_t theme,
                           uint8_t screen);

/* panelScreenCfgDwellSec — base dwell multiplied by the configured weight.
 * Input: config/flattened index/base seconds. Output: weighted seconds. */
float panelScreenCfgDwellSec(const PanelScreenCfg *cfg, uint8_t index,
                             float baseSec);

/* panelScreenCfgRescaleDwell — CONTRACT-AW §10 D5: preserve the fraction of
 * dwell already waited across a weight change. Input: elapsed seconds, old
 * and new dwell (seconds), advanced out-flag. Output: the rescaled elapsed
 * (0 when an immediate advance fired). Never resets: 10x->1/4x at 299 s
 * advances immediately instead of holding another 7.5 s. */
float panelScreenCfgRescaleDwell(float elapsed, float oldDwell,
                                 float newDwell, int *advanced);

/* panelScreenCfgDeviceFlash — RP2350 last-sector flash seam.
 * Input: none. Output: device flash operations, or an inert seam on hosts. */
PanelScreenCfgFlash panelScreenCfgDeviceFlash(void);

#endif /* SOLARI_PANEL_SCREEN_CFG_H */
