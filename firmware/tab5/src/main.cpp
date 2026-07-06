// main.cpp — SolariNet Companion firmware entry point (M5Stack Tab5 / ESP32-P4).
//
// Boot order:
//   1. M5Unified init (display, touch, RTC, PSRAM).
//   2. Config load from NVS (non-secret + encrypted secrets).
//   3. Seed the system clock from the RTC (so TOTP works pre-network).
//   4. Bring up Wi-Fi via the ESP32-C6 co-processor (WiFi.h through
//      esp_wifi_remote), then start SNTP time discipline.
//   5. Start the notification (MQTT) client + UI.
//
// The three features live in totp.*, vault.*, notify.*; ui.* renders them.
#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>

#include "config.h"
#include "totp.h"
#include "vault.h"
#include "notify.h"
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
  Serial.println("SolariNet Companion booting");

  if (!config().begin()) {
    Serial.println("FATAL: NVS/config init failed");
    // TODO: show a hard-error screen; secrets partition may be uninitialized
    // (first boot after flashing with flash-encryption just enabled).
  }

  totp::seedSystemFromRtc();  // codes usable within seconds, pre-network

  ui().begin();

  wifiBegin();
  notify().begin();

  // Wire crit/warn notifications to a UI toast.
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

  notify().loop();  // MQTT connect/reconnect + inbound
  ui().loop();      // touch + render

  // TODO(power): the Tab5 is mains-powered on a desk here, so we keep the
  // panel awake. If battery use matters, add a dim/sleep timeout and a
  // touch-to-wake, and lower CPU freq when idle.
  delay(5);
}
