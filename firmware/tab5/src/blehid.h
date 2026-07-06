// blehid.h — BLE HID keyboard (autotype) for the SolariNet Authenticator.
//
// The device advertises as a Bluetooth Low Energy HID keyboard and TYPES a
// selected password or a live TOTP code directly into the paired host, so the
// secret never touches an OS clipboard on the target machine. Used by the vault
// / password-generator "Type it" action and the TOTP "Type code" action.
//
// HARDWARE PATH (Tab5-specific, documented honestly):
//   The Tab5 is an ESP32-P4 with NO native radio. BLE 5 lives on the on-board
//   ESP32-C6 co-processor, reached from the P4 over the esp-hosted / co-proc
//   link (the same transport WiFi.h already rides). NimBLE runs on the C6; the
//   P4 uses the Bluetooth HCI-over-hosted VHCI controller so the standard
//   NimBLE-Arduino API works. This REQUIRES:
//     - esp-hosted co-processor firmware on the C6 with BT/HCI enabled
//       (Wi-Fi-only hosted builds do NOT expose the controller — see DESIGN.md
//       "BLE on the co-processor"); and
//     - the NimBLE-Arduino lib + CONFIG_BT_ENABLED / CONFIG_BT_NIMBLE_ENABLED.
//   TODO(hosted-bt): confirm the C6 hosted firmware on THIS unit exposes the BT
//   controller; if it is a Wi-Fi-only hosted build, reflash the C6 with a
//   hosted build that includes BT before autotype can work. Until then begin()
//   reports unavailable and the UI disables the "Type" actions.
//
// KEYMAP: US QWERTY. typeString() maps ASCII to HID usage codes, applying the
// Shift modifier for uppercase + shifted symbols (see the table in blehid.cpp).
#pragma once

#include <Arduino.h>

namespace solari {

enum class BleState { Unavailable, Idle, Advertising, Connected };

class BleHid {
 public:
  void begin();          // bring up the NimBLE HID keyboard (if BT is available)
  void loop();           // service connection state
  BleState state() const { return state_; }
  bool available() const { return state_ != BleState::Unavailable; }
  bool connected() const { return state_ == BleState::Connected; }

  // Start/stop advertising so a host can pair. No-op if unavailable.
  void advertise(bool on);

  // Type `text` into the paired host as keystrokes (US layout). Returns false
  // if not connected. `pressEnter` appends a Return at the end (handy for TOTP).
  bool typeString(const String& text, bool pressEnter = false);

  // Convenience wrappers used by the UI actions.
  bool typePassword(const String& pw) { return typeString(pw, false); }
  bool typeTotp(const String& code)   { return typeString(code, true); }

 private:
  void sendKey_(uint8_t modifier, uint8_t keycode);  // press + release one key

  BleState state_ = BleState::Unavailable;
};

BleHid& bleHid();

}  // namespace solari
