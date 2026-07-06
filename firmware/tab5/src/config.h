// config.h — runtime configuration + secret storage for the SolariNet Tab5.
//
// Nothing here is compiled-in: all values are provisioned at first boot and
// persisted to NVS. Non-secret settings (Wi-Fi SSID, server URLs) live in the
// default `nvs` partition; secret material (TOTP seeds, PM master-key data)
// lives in the encrypted `secrets` partition. See DESIGN.md.
#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <vector>

namespace solari {

// ---- Non-secret settings (namespace "cfg" in default NVS) ------------------
struct Settings {
  String wifiSsid;            // default: akoria area SSID
  String wifiPass;            // stored in encrypted flash; still treat as secret
  String ntpServer = "pool.ntp.org";   // TODO: prefer a local akoria.net NTP
  String tz = "PST8PDT,M3.2.0,M11.1.0"; // POSIX TZ; TODO confirm home TZ

  // Notification transport (see notify.*). MQTT over TLS is the default path.
  String mqttHost = "10.5.2.50";  // benzene; TODO confirm broker host/DNS name
  uint16_t mqttPort = 8883;       // TLS; 1883 for plaintext dev only
  String mqttUser = "tab5";
  String mqttPass;                // provisioned
  String mqttTopic = "notify/#";  // server-side shim publishes here

  // Password manager (Vaultwarden/Bitwarden-compatible). OPEN QUESTION: which
  // PM does the user actually run? See README. Base URL is fully configurable.
  String pmBaseUrl = "https://vault.akoria.net"; // TODO confirm
  String pmEmail;                 // account email (identity endpoint)

  bool provisioned = false;       // false => run first-boot setup wizard
};

// A single TOTP account. `secretB32` is the RFC 6238 shared secret, Base32.
struct TotpAccount {
  String issuer;     // e.g. "GitHub"
  String label;      // e.g. "jason@thefourcats.com"
  String secretB32;  // Base32, no padding required
  uint8_t digits = 6;
  uint16_t period = 30;
  // algorithm: 0=SHA1 (default/RFC), 1=SHA256, 2=SHA512
  uint8_t algo = 0;
};

class Config {
 public:
  bool begin();                       // opens NVS handles; returns false on fault
  Settings& settings() { return s_; }
  bool saveSettings();                // persist s_ to non-secret NVS

  // ---- Secret store (namespace "totp"/"pm" in encrypted `secrets` NVS) -----
  std::vector<TotpAccount> loadTotpAccounts();
  bool addTotpAccount(const TotpAccount& a);
  bool removeTotpAccount(size_t index);

  // PM master-key material. We do NOT store the master password. We store the
  // KDF-derived pieces needed to unlock offline + refresh tokens (encrypted at
  // rest by flash encryption). See vault.* + DESIGN.md for the crypto.
  bool savePmKeyBlob(const uint8_t* data, size_t len);
  bool loadPmKeyBlob(std::vector<uint8_t>& out);
  bool clearPmKeyBlob();

  // Panic-wipe: erase only the encrypted secrets partition contents.
  void panicWipeSecrets();

 private:
  Preferences prefsCfg_;    // default nvs, namespace "cfg"
  Preferences prefsTotp_;   // secrets partition, namespace "totp"
  Preferences prefsPm_;     // secrets partition, namespace "pm"
  Settings s_;
};

// Global accessor (constructed in main.cpp).
Config& config();

}  // namespace solari
