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
#include <cstring>
#include <mbedtls/sha256.h>
#include <vector>

#include "config.h"
#include "totp.h"
#include "vault.h"
#include "mqttbus.h"
#include "approvals.h"
#include "notify.h"
#include "blehid.h"
#include "ui.h"

using namespace solari;

namespace {
String fingerprint(const uint8_t* data, size_t len) {
  uint8_t hash[32];
  if (mbedtls_sha256(data, len, hash, 0) != 0) return "";
  static const char* digits = "0123456789abcdef";
  String out;
  out.reserve(64);
  for (uint8_t b : hash) {
    out += digits[b >> 4];
    out += digits[b & 15];
  }
  return out;
}

bool decodeHex(const String& hex, std::vector<uint8_t>& out) {
  if ((hex.length() & 1) != 0) return false;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  out.clear();
  for (size_t i = 0; i < hex.length(); i += 2) {
    int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.push_back((uint8_t)((hi << 4) | lo));
  }
  return true;
}

void showTrustFingerprints() {
  const String deviceKey = approvals().publicKeyHex();
  const auto brokerIds = config().brokerRequestKeyIds();
  const String ca = config().loadMqttCaPem();
  std::vector<uint8_t> point, devicePoint;
  Serial.println("Trust fingerprints (public material only):");
  if (!deviceKey.isEmpty() && decodeHex(deviceKey, devicePoint))
    Serial.printf("device_response_key_sha256=%s\n",
                  fingerprint(devicePoint.data(), devicePoint.size()).c_str());
  else
    Serial.println("device_response_key_sha256=missing");
  if (!brokerIds.empty()) {
    for (const auto& kid : brokerIds) {
      const String brokerKey = config().loadBrokerRequestPubHex(kid);
      if (!decodeHex(brokerKey, point)) continue;
      Serial.printf("broker_request_key[%s]_sha256=%s\n", kid.c_str(),
                    fingerprint(point.data(), point.size()).c_str());
    }
  } else {
    Serial.println("broker_request_key_sha256=missing");
  }
  if (!ca.isEmpty())
    Serial.printf("mqtt_ca_pem_sha256=%s\n",
                  fingerprint((const uint8_t*)ca.c_str(), ca.length()).c_str());
  else
    Serial.println("mqtt_ca_pem_sha256=missing");
  if (!deviceKey.isEmpty())
    Serial.printf("device_response_public_key_hex=%s\n", deviceKey.c_str());
}

String s_serialLine;
String s_caImport;
bool s_readingCa = false;

void handleSerialLine(const String& line) {
  if (s_readingCa) {
    if (line == "SOLARI:CA-END") {
      s_readingCa = false;
      if (config().saveMqttCaPem(s_caImport))
        Serial.println("MQTT CA saved; restart to apply.");
      else
        Serial.println("MQTT CA rejected; trust state unchanged.");
      s_caImport = "";
    } else if (s_caImport.length() + line.length() + 1 > 4096) {
      s_readingCa = false;
      s_caImport = "";
      Serial.println("MQTT CA too large; import cancelled.");
    } else {
      s_caImport += line;
      s_caImport += '\n';
    }
    return;
  }
  if (line == "SOLARI:SHOW_KEYS") {
    showTrustFingerprints();
  } else if (line == "SOLARI:CA-BEGIN") {
    s_caImport = "";
    s_readingCa = true;
    Serial.println("Paste the PEM certificate, then SOLARI:CA-END.");
  } else if (line.startsWith("SOLARI:SET_BROKER_KEY:")) {
    String args = line.substring(strlen("SOLARI:SET_BROKER_KEY:"));
    const int separator = args.indexOf(':');
    if (separator < 1) {
      Serial.println("Use SOLARI:SET_BROKER_KEY:<kid>:<130-char-uncompressed-P256-hex>.");
      return;
    }
    const String kid = args.substring(0, separator);
    const String hex = args.substring(separator + 1);
    if (config().saveBrokerRequestPubHex(kid, hex)) {
      Serial.println("Broker request key saved; restart to apply.");
      showTrustFingerprints();
    } else {
      Serial.println("Broker request key rejected; trust state unchanged.");
    }
  } else if (line.startsWith("SOLARI:REMOVE_BROKER_KEY:")) {
    const String kid = line.substring(strlen("SOLARI:REMOVE_BROKER_KEY:"));
    Serial.println(config().removeBrokerRequestKey(kid)
        ? "Broker request key removed; restart to apply."
        : "Broker request key ID was not found.");
  } else if (line == "SOLARI:CLEAR_TRUST") {
    Serial.println(config().clearApprovalTrust()
        ? "Approval trust cleared; restart to apply."
        : "Unable to clear approval trust.");
  }
}

void serialProvisioningLoop() {
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      handleSerialLine(s_serialLine);
      s_serialLine = "";
    } else if (s_serialLine.length() < 8192) {
      s_serialLine += c;
    } else {
      s_serialLine = "";
      s_readingCa = false;
      s_caImport = "";
      Serial.println("Serial input too large; command discarded.");
    }
  }
}
}  // namespace

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
  serialProvisioningLoop();  // trust imports are accepted only over attached USB
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
