/*
 * panelHw.cpp — the ONLY C++ translation unit in this firmware.
 *
 * Wraps pimoroni::GalacticUnicorn (which is unavoidably C++) behind the plain
 * C interface in panelHw.h. Nothing here makes rendering decisions; it is a
 * transport for pixels, buttons and one audio voice.
 *
 * Note on rendering path: GalacticUnicorn::set_pixel writes straight into the
 * DMA-streamed BCD bitstream, so the firmware never needs a PicoGraphics
 * surface or an explicit update() call — our own float framebuffer (panelFb.c)
 * is the single source of truth and is flushed pixel-by-pixel each frame.
 */

#include "libraries/galactic_unicorn/galactic_unicorn.hpp"

extern "C" {
#include "panelHw.h"
}

using namespace pimoroni;

namespace {

GalacticUnicorn gUnicorn;
float gBrightness = 0.85f;   /* DESIGN-BRIEF default 85% */
float gVolume     = 0.6f;

/* Map our logical button index onto the driver's GPIO constants. */
uint8_t buttonGpio(PanelButton b) {
  switch (b) {
    case PANEL_BTN_A:     return GalacticUnicorn::SWITCH_A;
    case PANEL_BTN_B:     return GalacticUnicorn::SWITCH_B;
    case PANEL_BTN_C:     return GalacticUnicorn::SWITCH_C;
    case PANEL_BTN_D:     return GalacticUnicorn::SWITCH_D;
    case PANEL_BTN_LUXUP: return GalacticUnicorn::SWITCH_BRIGHTNESS_UP;
    case PANEL_BTN_LUXDN: return GalacticUnicorn::SWITCH_BRIGHTNESS_DOWN;
    case PANEL_BTN_VOLUP: return GalacticUnicorn::SWITCH_VOLUME_UP;
    case PANEL_BTN_VOLDN: return GalacticUnicorn::SWITCH_VOLUME_DOWN;
    case PANEL_BTN_SLEEP: return GalacticUnicorn::SWITCH_SLEEP;
    default:              return GalacticUnicorn::SWITCH_A;
  }
}

} /* namespace */

extern "C" void panelHwInit(void) {
  gUnicorn.init();
  gUnicorn.set_brightness(gBrightness);
  gUnicorn.set_volume(gVolume);
  gUnicorn.clear();

  /* One square-wave voice for the alarm triad. Envelope numbers come straight
   * from DESIGN-BRIEF "Animation / motion specifics": 20 ms ramp in, decayed
   * to near zero by 170 ms, note length 200 ms. */
  AudioChannel &ch = gUnicorn.synth_channel(0);
  ch.waveforms  = Waveform::SQUARE;
  ch.attack_ms  = 20;
  ch.decay_ms   = 150;
  ch.sustain    = 0;
  ch.release_ms = 30;
  ch.volume     = 0x28ff;      /* ~0.08 of full scale, per the design */
  ch.frequency  = 990;
}

extern "C" void panelHwSetPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  gUnicorn.set_pixel(x, y, r, g, b);
}

extern "C" void panelHwSetBrightness(float value) {
  if (value < 0.25f) value = 0.25f;
  if (value > 1.00f) value = 1.00f;
  gBrightness = value;
  gUnicorn.set_brightness(value);
}

extern "C" float panelHwGetBrightness(void) { return gBrightness; }

extern "C" void panelHwSetVolume(float value) {
  if (value < 0.0f) value = 0.0f;
  if (value > 1.0f) value = 1.0f;
  gVolume = value;
  gUnicorn.set_volume(value);
}

extern "C" float panelHwGetVolume(void) { return gVolume; }

extern "C" bool panelHwButton(PanelButton b) {
  return gUnicorn.is_pressed(buttonGpio(b));
}

extern "C" uint16_t panelHwLight(void) { return gUnicorn.light(); }

extern "C" void panelHwToneOn(uint16_t freqHz) {
  AudioChannel &ch = gUnicorn.synth_channel(0);
  ch.frequency = freqHz;
  gUnicorn.play_synth();
  ch.trigger_attack();
}

extern "C" void panelHwToneOff(void) {
  gUnicorn.synth_channel(0).trigger_release();
}
