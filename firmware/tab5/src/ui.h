// ui.h — screen manager + rendering for the SolariNet Authenticator (M5GFX).
//
// The Tab5 panel is 1280x720 landscape, capacitive multitouch. The UI is a
// simple screen state machine driven from the main loop: a home screen with
// three primary tiles (TOTP / Approvals / Vault) plus a password-generator
// screen, a per-request approval prompt, per-feature screens, and a first-boot
// setup wizard. Rendering is immediate-mode against M5.Display.
#pragma once

#include <Arduino.h>

namespace solari {

enum class Screen {
  Boot,
  Setup,      // first-boot provisioning wizard (Wi-Fi + URLs + device enroll)
  Home,       // three tiles + generator/BLE footer
  Totp,
  Approval,   // push-approval prompt (Approve / Deny + TTL)
  Vault,
  VaultUnlock,
  ItemDetail, // vault item detail / reveal
  PwGen,      // secure password generator
};

class Ui {
 public:
  void begin();
  void loop();                    // poll touch, animate, redraw as needed
  void goTo(Screen s);
  Screen current() const { return screen_; }

  // Toast overlay (used by notify + approvals alert + copy feedback).
  void toast(const String& text, uint16_t color, uint32_t ms = 3000);

 private:
  void drawHome_();
  void drawTotp_();
  void drawApproval_();
  void drawVaultUnlock_();
  void drawVault_();
  void drawSetup_();
  void drawItemDetail_();
  void drawPwGen_();
  void drawToast_();
  void drawStatusBar_();          // clock, wifi, mqtt, ble, lock state

  void handleTouch_(int x, int y);
  void handlePwGenTouch_(int x, int y);
  void regeneratePassword_();

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
