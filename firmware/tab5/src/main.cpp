// main.cpp — SolariNet Authenticator firmware entry point (M5Stack Tab5 / P4).
//
// The Tab5 is a dedicated authentication & authorization device. Feature set:
//   TOTP · push-approval (2FA/access) · vault (PM client) · password generator
//   · BLE-keyboard autotype.
//
// Boot order:
//   1. M5Unified init (display, touch, RTC, PSRAM).
//   2. Config load from NVS (non-secret + encrypted secrets, incl. device key).
//   3. Seed the system clock from the RTC (so TOTP works pre-network).
//   4. Bring up Wi-Fi via the ESP32-C6 co-processor, then SNTP discipline.
//   5. Start the shared MQTT bus + approvals + notifications + BLE HID + UI.
#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>

#include "config.h"
#include "totp.h"
#include "vault.h"
#include "mqttbus.h"
#include "approvals.h"
#include "notify.h"
#include "blehid.h"
#include "ui.h"

using namespace solari;

static void wifiBegin() {
  const auto& s = config().settings();
  if (s.wifiSsid.isEmpty()) return;  // setup wizard will provision
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("tab5-solari");
  WiFi.begin(s.wifiSsid.c_str(), s.wifiPass.c_str());
  // Non-blocking: the loop() checks WiFi.status(); SNTP starts once connected.
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);  // display, touch, RTC (RX8130), I2C, PSRAM

  Serial.begin(115200);
  Serial.println("SolariNet Authenticator booting");

  if (!config().begin()) {
    Serial.println("FATAL: NVS/config init failed");
    // TODO: show a hard-error screen; secrets partition may be uninitialized
    // (first boot after flashing with flash-encryption just enabled).
  }

  totp::seedSystemFromRtc();  // codes usable within seconds, pre-network

  ui().begin();

  wifiBegin();

  // Shared MQTT transport, then the features that ride it.
  mqttBus().begin();
  approvals().begin();   // subscribes auth/request/#, ensures device key
  notify().begin();      // subscribes notify/# for background toasts
  bleHid().begin();      // BLE HID keyboard (Unavailable unless SOLARI_HAS_BLE)

  // A new approval request wakes the screen with a toast + jumps to the prompt.
  approvals().onAlert([](const ApprovalRequest& r) {
    ui().toast("Approval: " + r.subject + " -> " + r.detail, TFT_YELLOW, 4000);
    ui().goTo(Screen::Approval);
  });

  // Wire crit/warn notifications to a UI toast (secondary surface).
  notify().onToast([](const Notification& n) {
    uint16_t c = (n.severity == Severity::Crit) ? TFT_RED : TFT_ORANGE;
    ui().toast(n.title.isEmpty() ? n.body : n.title, c, 5000);
  });
}

static bool s_ntpStarted = false;

void loop() {
  // Once Wi-Fi is up, start SNTP exactly once (then RTC stays disciplined).
  if (!s_ntpStarted && WiFi.status() == WL_CONNECTED) {
    const auto& s = config().settings();
    totp::beginTimeSync(s.ntpServer, s.tz);
    s_ntpStarted = true;
  }

  mqttBus().loop();     // one connection: approvals + notifications
  approvals().loop();   // expire stale approval requests
  bleHid().loop();      // BLE connection state
  ui().loop();          // touch + render

  // TODO(power): the Tab5 is mains-powered on a desk here, so we keep the panel
  // awake. If battery use matters, add a dim/sleep timeout and touch-to-wake.
  delay(5);
}
