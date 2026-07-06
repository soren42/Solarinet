#include "blehid.h"

// ===========================================================================
// SCAFFOLD STATUS — BLE HID keyboard autotype.
// This module is structured against NimBLE-Arduino's HID keyboard profile. It
// is guarded by SOLARI_HAS_BLE because BLE on the Tab5 depends on the ESP32-C6
// co-processor exposing a Bluetooth HCI controller over the esp-hosted link
// (see blehid.h + DESIGN.md "BLE on the co-processor"). When BLE is not built
// in (or the hosted firmware is Wi-Fi-only), begin() reports Unavailable and
// the UI disables the "Type" actions — the rest of the app is unaffected.
//
// The US-QWERTY ASCII->HID keymap below is complete; the NimBLE wiring in the
// SOLARI_HAS_BLE branch is TODO-marked where a concrete NimBLE HID report map /
// service objects must be instantiated on the pinned lib version.
// ===========================================================================

#ifndef SOLARI_HAS_BLE
#define SOLARI_HAS_BLE 0
#endif

#if SOLARI_HAS_BLE
// TODO(nimble): #include <NimBLEDevice.h> and an HID keyboard helper. On the
// Tab5, NimBLE talks to the C6 controller via the hosted VHCI — confirm the
// controller is present at init (NimBLEDevice::init returns/So asserts).
#endif

namespace solari {

static BleHid g_ble;
BleHid& bleHid() { return g_ble; }

namespace {
// USB HID usage IDs (keyboard/keypad page 0x07). Modifier bit 0x02 = Left Shift.
constexpr uint8_t MOD_NONE = 0x00;
constexpr uint8_t MOD_SHIFT = 0x02;

struct KeyMap { uint8_t mod; uint8_t code; };

// Map a US-layout printable ASCII char to a HID (modifier, usage) pair.
// Returns {0,0} for characters we can't type.
KeyMap asciiToHid(char c) {
  // Letters
  if (c >= 'a' && c <= 'z') return {MOD_NONE, (uint8_t)(0x04 + (c - 'a'))};
  if (c >= 'A' && c <= 'Z') return {MOD_SHIFT, (uint8_t)(0x04 + (c - 'A'))};
  // Digits 1-9,0
  if (c >= '1' && c <= '9') return {MOD_NONE, (uint8_t)(0x1E + (c - '1'))};
  if (c == '0') return {MOD_NONE, 0x27};
  switch (c) {
    case ' ':  return {MOD_NONE,  0x2C};
    case '-':  return {MOD_NONE,  0x2D};
    case '_':  return {MOD_SHIFT, 0x2D};
    case '=':  return {MOD_NONE,  0x2E};
    case '+':  return {MOD_SHIFT, 0x2E};
    case '[':  return {MOD_NONE,  0x2F};
    case '{':  return {MOD_SHIFT, 0x2F};
    case ']':  return {MOD_NONE,  0x30};
    case '}':  return {MOD_SHIFT, 0x30};
    case '\\': return {MOD_NONE,  0x31};
    case '|':  return {MOD_SHIFT, 0x31};
    case ';':  return {MOD_NONE,  0x33};
    case ':':  return {MOD_SHIFT, 0x33};
    case '\'': return {MOD_NONE,  0x34};
    case '"':  return {MOD_SHIFT, 0x34};
    case '`':  return {MOD_NONE,  0x35};
    case '~':  return {MOD_SHIFT, 0x35};
    case ',':  return {MOD_NONE,  0x36};
    case '<':  return {MOD_SHIFT, 0x36};
    case '.':  return {MOD_NONE,  0x37};
    case '>':  return {MOD_SHIFT, 0x37};
    case '/':  return {MOD_NONE,  0x38};
    case '?':  return {MOD_SHIFT, 0x38};
    case '!':  return {MOD_SHIFT, 0x1E};
    case '@':  return {MOD_SHIFT, 0x1F};
    case '#':  return {MOD_SHIFT, 0x20};
    case '$':  return {MOD_SHIFT, 0x21};
    case '%':  return {MOD_SHIFT, 0x22};
    case '^':  return {MOD_SHIFT, 0x23};
    case '&':  return {MOD_SHIFT, 0x24};
    case '*':  return {MOD_SHIFT, 0x25};
    case '(':  return {MOD_SHIFT, 0x26};
    case ')':  return {MOD_SHIFT, 0x27};
    default:   return {MOD_NONE,  0x00};  // untypable
  }
}
constexpr uint8_t KEY_ENTER = 0x28;
}  // namespace

void BleHid::begin() {
#if SOLARI_HAS_BLE
  // TODO(nimble): NimBLEDevice::init("SolariNet Auth"); build the HID service
  // with a standard keyboard report map, start the HID device, and set
  // state_ = Idle once the C6 controller is up. If init fails (hosted BT not
  // present) leave state_ = Unavailable.
  state_ = BleState::Idle;
#else
  state_ = BleState::Unavailable;
#endif
}

void BleHid::loop() {
#if SOLARI_HAS_BLE
  // TODO(nimble): reflect connection/advertising callbacks into state_.
#endif
}

void BleHid::advertise(bool on) {
#if SOLARI_HAS_BLE
  if (state_ == BleState::Unavailable) return;
  // TODO(nimble): NimBLEDevice::getAdvertising()->start()/stop().
  state_ = on ? BleState::Advertising : BleState::Idle;
#else
  (void)on;
#endif
}

void BleHid::sendKey_(uint8_t modifier, uint8_t keycode) {
#if SOLARI_HAS_BLE
  // TODO(nimble): build an 8-byte boot keyboard report {mod,0,key,0,0,0,0,0},
  // notify the input report characteristic, then send an all-zero report to
  // release. A ~5ms gap between press and release is plenty for most hosts.
  (void)modifier; (void)keycode;
#else
  (void)modifier; (void)keycode;
#endif
}

bool BleHid::typeString(const String& text, bool pressEnter) {
  if (!connected()) return false;
  for (size_t i = 0; i < text.length(); ++i) {
    KeyMap k = asciiToHid(text[i]);
    if (k.code == 0x00) continue;  // skip untypable chars rather than mistype
    sendKey_(k.mod, k.code);
    delay(6);  // pacing so fast hosts don't drop keystrokes
  }
  if (pressEnter) sendKey_(MOD_NONE, KEY_ENTER);
  return true;
}

}  // namespace solari
