/*
 * panelHw.h — C-callable façade over the Pimoroni GalacticUnicorn C++ driver.
 *
 * The rest of the firmware is standard C (operator language policy). The
 * Pimoroni driver is a C++ class, so ALL C++ in this project is confined to
 * panelHw.cpp behind this header — nothing else includes a .hpp.
 *
 * Hardware: Galactic Unicorn, 53x11 RGB matrix, Pico 2 W / RP2350.
 * DESIGN-BRIEF "Display model": grid 53x11, origin (0,0) top-left.
 */

#ifndef SOLARI_PANEL_HW_H
#define SOLARI_PANEL_HW_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PANEL_W 53
#define PANEL_H 11

/* Logical button indices — also the arg of PANEL_EV_BUTTON on the wire.
 * DESIGN-BRIEF "Interaction (physical controls)"; the Unicorn exposes 9
 * user buttons and the design names A/B/C/D + LUX-pair + VOL-pair + ZZZ. */
typedef enum {
  PANEL_BTN_A     = 0,   /* theme ABSTRACT    */
  PANEL_BTN_B     = 1,   /* theme TEXT        */
  PANEL_BTN_C     = 2,   /* theme CHARTS      */
  PANEL_BTN_D     = 3,   /* theme INSTRUMENTS */
  PANEL_BTN_LUXUP = 4,   /* brightness +5%    */
  PANEL_BTN_LUXDN = 5,   /* brightness -5%    */
  PANEL_BTN_VOLUP = 6,
  PANEL_BTN_VOLDN = 7,
  PANEL_BTN_SLEEP = 8,   /* ZZZ: display sleep toggle */
  PANEL_BTN_COUNT = 9
} PanelButton;

/* panelHwInit — bring up the matrix driver, DMA bitstream and I2S synth.
 * Input:  none.  Output: none. Must run once before any other panelHw call. */
void panelHwInit(void);

/* panelHwSetPixel — write one LED. Coordinates outside the grid are dropped
 * by the driver. Channel values are 0..255 post-gamma-input (the driver
 * applies its own 14-bit gamma table and the global brightness scalar).      */
void panelHwSetPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b);

/* panelHwSetBrightness — global scalar, DESIGN-BRIEF "Brightness / day-night"
 * (range 0.25..1.00, default 0.85). Clamped internally.                      */
void panelHwSetBrightness(float value);
float panelHwGetBrightness(void);

/* panelHwSetVolume — 0.0..1.0. DESIGN-BRIEF models sound as on/off only; we
 * keep a level because the hardware supports one.                            */
void panelHwSetVolume(float value);
float panelHwGetVolume(void);

/* panelHwButton — debounced-at-caller raw read of one logical button.
 * Output: true while held.                                                   */
bool panelHwButton(PanelButton b);

/* panelHwLight — ambient phototransistor reading, 0..4095 (raw ADC).
 * DESIGN-BRIEF Ambiguity #5: auto-brightness is a SHOULD, not a MUST.        */
uint16_t panelHwLight(void);

/* panelHwToneOn / panelHwToneOff — single square-wave voice for the alarm
 * triad. DESIGN-BRIEF "Animation": 990/660/990 Hz square, 20 ms attack to
 * 0.08 gain, decayed to silence by ~170 ms, 200 ms note length.              */
void panelHwToneOn(uint16_t freqHz);
void panelHwToneOff(void);

#ifdef __cplusplus
}
#endif
#endif /* SOLARI_PANEL_HW_H */
