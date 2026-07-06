#include "config.h"

namespace solari {

namespace {
constexpr const char* kCfgNs = "cfg";
constexpr const char* kTotpNs = "totp";
constexpr const char* kPmNs = "pm";
// The Preferences library selects a partition by label. "secrets" matches the
// encrypted NVS partition declared in partitions.csv.
constexpr const char* kSecretsPart = "secrets";
constexpr const char* kTotpCountKey = "count";
}  // namespace

static Config g_config;
Config& config() { return g_config; }

bool Config::begin() {
  if (!prefsCfg_.begin(kCfgNs, /*readOnly=*/false)) return false;
  // Load non-secret settings, falling back to struct defaults.
  s_.provisioned = prefsCfg_.getBool("prov", false);
  s_.wifiSsid = prefsCfg_.getString("ssid", s_.wifiSsid);
  s_.wifiPass = prefsCfg_.getString("wpass", s_.wifiPass);
  s_.ntpServer = prefsCfg_.getString("ntp", s_.ntpServer);
  s_.tz = prefsCfg_.getString("tz", s_.tz);
  s_.mqttHost = prefsCfg_.getString("mqtth", s_.mqttHost);
  s_.mqttPort = prefsCfg_.getUShort("mqttp", s_.mqttPort);
  s_.mqttUser = prefsCfg_.getString("mqttu", s_.mqttUser);
  s_.mqttPass = prefsCfg_.getString("mqttpw", s_.mqttPass);
  s_.mqttTopic = prefsCfg_.getString("mqttt", s_.mqttTopic);
  s_.pmBaseUrl = prefsCfg_.getString("pmurl", s_.pmBaseUrl);
  s_.pmEmail = prefsCfg_.getString("pmmail", s_.pmEmail);

  // Secret namespaces live in the encrypted partition. begin(partition_label)
  // overload selects it. TODO(idf): confirm this Preferences overload exists
  // in the pinned core; otherwise use nvs_flash_init_partition + raw nvs API.
  prefsTotp_.begin(kTotpNs, false, kSecretsPart);
  prefsPm_.begin(kPmNs, false, kSecretsPart);
  return true;
}

bool Config::saveSettings() {
  bool ok = true;
  ok &= prefsCfg_.putBool("prov", s_.provisioned);
  ok &= prefsCfg_.putString("ssid", s_.wifiSsid) >= 0;
  ok &= prefsCfg_.putString("wpass", s_.wifiPass) >= 0;
  ok &= prefsCfg_.putString("ntp", s_.ntpServer) >= 0;
  ok &= prefsCfg_.putString("tz", s_.tz) >= 0;
  ok &= prefsCfg_.putString("mqtth", s_.mqttHost) >= 0;
  ok &= prefsCfg_.putUShort("mqttp", s_.mqttPort);
  ok &= prefsCfg_.putString("mqttu", s_.mqttUser) >= 0;
  ok &= prefsCfg_.putString("mqttpw", s_.mqttPass) >= 0;
  ok &= prefsCfg_.putString("mqttt", s_.mqttTopic) >= 0;
  ok &= prefsCfg_.putString("pmurl", s_.pmBaseUrl) >= 0;
  ok &= prefsCfg_.putString("pmmail", s_.pmEmail) >= 0;
  return ok;
}

// ---- TOTP accounts ---------------------------------------------------------
// Stored as indexed keys a0..aN holding a compact packed record. Kept simple
// and human-auditable; a JSON blob would also work but wastes NVS space.
static String packTotp(const TotpAccount& a) {
  // issuer\x1flabel\x1fsecret\x1fdigits\x1fperiod\x1falgo
  String s;
  s.reserve(96);
  s += a.issuer; s += '\x1f';
  s += a.label; s += '\x1f';
  s += a.secretB32; s += '\x1f';
  s += String(a.digits); s += '\x1f';
  s += String(a.period); s += '\x1f';
  s += String(a.algo);
  return s;
}

static bool unpackTotp(const String& s, TotpAccount& a) {
  int f = 0, start = 0;
  String parts[6];
  for (int i = 0; i <= s.length() && f < 6; ++i) {
    if (i == s.length() || s[i] == '\x1f') {
      parts[f++] = s.substring(start, i);
      start = i + 1;
    }
  }
  if (f < 6) return false;
  a.issuer = parts[0];
  a.label = parts[1];
  a.secretB32 = parts[2];
  a.digits = (uint8_t)parts[3].toInt();
  a.period = (uint16_t)parts[4].toInt();
  a.algo = (uint8_t)parts[5].toInt();
  return true;
}

std::vector<TotpAccount> Config::loadTotpAccounts() {
  std::vector<TotpAccount> out;
  uint32_t n = prefsTotp_.getUInt(kTotpCountKey, 0);
  out.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    String key = "a" + String(i);
    String packed = prefsTotp_.getString(key.c_str(), "");
    TotpAccount a;
    if (!packed.isEmpty() && unpackTotp(packed, a)) out.push_back(a);
  }
  return out;
}

bool Config::addTotpAccount(const TotpAccount& a) {
  uint32_t n = prefsTotp_.getUInt(kTotpCountKey, 0);
  String key = "a" + String(n);
  if (prefsTotp_.putString(key.c_str(), packTotp(a)) == 0) return false;
  return prefsTotp_.putUInt(kTotpCountKey, n + 1);
}

bool Config::removeTotpAccount(size_t index) {
  // Compact by shifting later entries down, then drop the tail key.
  uint32_t n = prefsTotp_.getUInt(kTotpCountKey, 0);
  if (index >= n) return false;
  for (uint32_t i = index; i + 1 < n; ++i) {
    String src = "a" + String(i + 1);
    String dst = "a" + String(i);
    prefsTotp_.putString(dst.c_str(), prefsTotp_.getString(src.c_str(), ""));
  }
  prefsTotp_.remove(("a" + String(n - 1)).c_str());
  return prefsTotp_.putUInt(kTotpCountKey, n - 1);
}

// ---- PM key blob -----------------------------------------------------------
bool Config::savePmKeyBlob(const uint8_t* data, size_t len) {
  return prefsPm_.putBytes("keyblob", data, len) == len;
}

bool Config::loadPmKeyBlob(std::vector<uint8_t>& out) {
  size_t len = prefsPm_.getBytesLength("keyblob");
  if (len == 0) return false;
  out.resize(len);
  return prefsPm_.getBytes("keyblob", out.data(), len) == len;
}

bool Config::clearPmKeyBlob() { return prefsPm_.remove("keyblob"); }

void Config::panicWipeSecrets() {
  prefsTotp_.clear();
  prefsPm_.clear();
}

}  // namespace solari
