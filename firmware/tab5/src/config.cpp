#include "config.h"

namespace solari {

namespace {
constexpr const char* kCfgNs = "cfg";
constexpr const char* kTotpNs = "totp";
constexpr const char* kPmNs = "pm";
constexpr const char* kDevNs = "dev";
constexpr const char* kTrustNs = "trust";
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
  s_.notifyTopic = prefsCfg_.getString("ntfyt", s_.notifyTopic);
  s_.deviceId = prefsCfg_.getString("devid", s_.deviceId);
  s_.pmType = prefsCfg_.getUChar("pmtype", s_.pmType);
  s_.pmBaseUrl = prefsCfg_.getString("pmurl", s_.pmBaseUrl);
  s_.pmEmail = prefsCfg_.getString("pmmail", s_.pmEmail);

  // Secret namespaces live in the encrypted partition. begin(partition_label)
  // overload selects it. TODO(idf): confirm this Preferences overload exists
  // in the pinned core; otherwise use nvs_flash_init_partition + raw nvs API.
  if (!prefsTotp_.begin(kTotpNs, false, kSecretsPart)) {
    prefsCfg_.end();
    return false;
  }
  if (!prefsPm_.begin(kPmNs, false, kSecretsPart)) {
    prefsTotp_.end(); prefsCfg_.end();
    return false;
  }
  if (!prefsDev_.begin(kDevNs, false, kSecretsPart)) {
    prefsPm_.end(); prefsTotp_.end(); prefsCfg_.end();
    return false;
  }
  if (!prefsTrust_.begin(kTrustNs, false, kSecretsPart)) {
    prefsDev_.end(); prefsPm_.end(); prefsTotp_.end(); prefsCfg_.end();
    return false;
  }
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
  ok &= prefsCfg_.putString("ntfyt", s_.notifyTopic) >= 0;
  ok &= prefsCfg_.putString("devid", s_.deviceId) >= 0;
  ok &= prefsCfg_.putUChar("pmtype", s_.pmType);
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

// ---- Approval device key ---------------------------------------------------
bool Config::saveDeviceKey(const uint8_t* priv32, size_t len) {
  return prefsDev_.putBytes("privkey", priv32, len) == len;
}

bool Config::loadDeviceKey(std::vector<uint8_t>& out) {
  size_t len = prefsDev_.getBytesLength("privkey");
  if (len == 0) return false;
  out.resize(len);
  return prefsDev_.getBytes("privkey", out.data(), len) == len;
}

bool Config::saveDevicePubHex(const String& hex) {
  return prefsDev_.putString("pubhex", hex) >= 0;
}

String Config::loadDevicePubHex() { return prefsDev_.getString("pubhex", ""); }

namespace {
bool validBrokerKeyId(const String& keyId) {
  if (keyId.isEmpty() || keyId.length() > 14) return false;
  for (size_t i = 0; i < keyId.length(); ++i) {
    const char c = keyId[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '_' || c == '-')) return false;
  }
  return true;
}
}

std::vector<String> Config::brokerRequestKeyIds() {
  std::vector<String> ids;
  const String list = prefsTrust_.getString("brokerkids", "");
  int start = 0;
  while (start < list.length()) {
    int end = list.indexOf(',', start);
    if (end < 0) end = list.length();
    String id = list.substring(start, end);
    if (validBrokerKeyId(id)) ids.push_back(id);
    start = end + 1;
  }
  return ids;
}

bool Config::saveBrokerRequestPubHex(const String& keyId, const String& hex) {
  if (!validBrokerKeyId(keyId)) return false;
  if (hex.length() != 130 || !hex.startsWith("04")) return false;
  for (size_t i = 0; i < hex.length(); ++i) {
    const char c = hex[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'))) return false;
  }
  auto ids = brokerRequestKeyIds();
  bool found = false;
  for (const auto& id : ids) if (id == keyId) found = true;
  if (!found && ids.size() >= 8) return false;
  if (prefsTrust_.putString(("k" + keyId).c_str(), hex) != hex.length())
    return false;
  if (!found) {
    if (!ids.empty()) ids.emplace_back(",");
    ids.push_back(keyId);
    String list;
    for (const auto& id : ids) list += id;
    if (prefsTrust_.putString("brokerkids", list) != list.length()) return false;
  }
  return true;
}

String Config::loadBrokerRequestPubHex(const String& keyId) {
  if (!validBrokerKeyId(keyId)) return "";
  return prefsTrust_.getString(("k" + keyId).c_str(), "");
}

bool Config::removeBrokerRequestKey(const String& keyId) {
  if (!validBrokerKeyId(keyId)) return false;
  auto ids = brokerRequestKeyIds();
  std::vector<String> kept;
  bool found = false;
  for (const auto& id : ids) {
    if (id == keyId) found = true;
    else kept.push_back(id);
  }
  if (!found) return false;
  prefsTrust_.remove(("k" + keyId).c_str());
  String list;
  for (size_t i = 0; i < kept.size(); ++i) {
    if (i) list += ",";
    list += kept[i];
  }
  if (kept.empty()) return prefsTrust_.remove("brokerkids");
  return prefsTrust_.putString("brokerkids", list) == list.length();
}

bool Config::saveMqttCaPem(const String& pem) {
  if (pem.length() < 64 || pem.length() > 4096 ||
      !pem.startsWith("-----BEGIN CERTIFICATE-----") ||
      pem.indexOf("-----END CERTIFICATE-----") < 0) return false;
  return prefsTrust_.putString("mqttca", pem) == pem.length();
}

String Config::loadMqttCaPem() {
  return prefsTrust_.getString("mqttca", "");
}

bool Config::clearApprovalTrust() {
  return prefsTrust_.clear();
}

void Config::panicWipeSecrets() {
  prefsTotp_.clear();
  prefsPm_.clear();
  prefsDev_.clear();
  prefsTrust_.clear();
}

}  // namespace solari
