// ui.h — screen manager + rendering for the SolariNet Tab5 (M5GFX/M5Unified).
//
// The Tab5 panel is 1280x720 landscape, capacitive multitouch. The UI is a
// simple screen state machine driven from the main loop: a home screen with
// three tiles (TOTP / Vault / Alerts) plus one screen per feature and a
// first-boot setup wizard. Rendering is immediate-mode against M5.Display with
// a sprite/canvas for flicker-free partial redraws.
#pragma once

#include <Arduino.h>

namespace solari {

enum class Screen {
  Boot,
  Setup,      // first-boot provisioning wizard (Wi-Fi + URLs + master pw)
  Home,       // three tiles
  Totp,
  Vault,
  VaultUnlock,
  Alerts,
  ItemDetail, // vault item detail / reveal
};

class Ui {
 public:
  void begin();
  void loop();                    // poll touch, animate, redraw as needed
  void goTo(Screen s);
  Screen current() const { return screen_; }

  // Toast overlay (used by notify + copy-to-clipboard feedback).
  void toast(const String& text, uint16_t color, uint32_t ms = 3000);

 private:
  void drawHome_();
  void drawTotp_();
  void drawVaultUnlock_();
  void drawVault_();
  void drawAlerts_();
  void drawSetup_();
  void drawItemDetail_();
  void drawToast_();
  void drawStatusBar_();          // clock, wifi, mqtt, lock state (all screens)

  void handleTouch_(int x, int y);

  Screen screen_ = Screen::Boot;
  bool dirty_ = true;             // needs full redraw
  uint32_t lastTickMs_ = 0;

  // toast state
  String toastText_;
  uint16_t toastColor_ = 0;
  uint32_t toastUntilMs_ = 0;

  // vault item currently open in ItemDetail
  String openItemId_;
};

Ui& ui();

// Clipboard buffer shared with vault.cpp copyField(). "Copy" on a device with
// no OS clipboard = hold the value in a buffer, show it, and auto-clear.
void ui_setClipboard(const String& text, uint32_t clearAfterMs);

}  // namespace solari
