#include "approvals.h"

#include <ArduinoJson.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/base64.h>

#include "config.h"
#include "mqttbus.h"
#include "approval_protocol.h"

// ===========================================================================
// SCAFFOLD STATUS
// The device verifies broker-signed v2 requests before display and signs
// responses bound to the verified request digest. Broker trust is imported via
// the attached USB serial console and stored in encrypted NVS.
// ===========================================================================

namespace solari {

static Approvals g_approvals;
Approvals& approvals() { return g_approvals; }

namespace {
constexpr int kProtoVersion = 2;
constexpr uint32_t kMaxRequestBytes = 2048;
constexpr int64_t kAllowedClockSkewSec = 30;

std::string asUtf8(const String& value) {
  return std::string(value.c_str(), value.length());
}

std::vector<uint8_t> canonicalRequest(const ApprovalRequest& r) {
  return approval_protocol::canonicalRequest({{
      std::to_string(kProtoVersion), asUtf8(r.brokerKeyId), asUtf8(r.id),
      std::to_string((long)r.ts),
      std::to_string(r.ttl), asUtf8(r.action), asUtf8(r.subject),
      asUtf8(r.detail), asUtf8(r.sourceIp), asUtf8(r.nonce),
      asUtf8(r.targetDevice),
  }});
}

std::vector<uint8_t> canonicalResponse(const ApprovalRequest& r,
                                       const char* decision,
                                       const String& deviceId) {
  return approval_protocol::canonicalResponse({{
      std::to_string(kProtoVersion), asUtf8(r.id), decision,
      asUtf8(r.nonce), asUtf8(deviceId), asUtf8(r.reqDigest),
  }});
}

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool decodeHex(const String& hex, std::vector<uint8_t>& out) {
  if ((hex.length() & 1) != 0) return false;
  out.clear();
  out.reserve(hex.length() / 2);
  for (size_t i = 0; i < hex.length(); i += 2) {
    int hi = hexValue(hex[i]), lo = hexValue(hex[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.push_back((uint8_t)((hi << 4) | lo));
  }
  return true;
}

bool isHex(const String& value, size_t exactLength) {
  if (value.length() != exactLength) return false;
  for (size_t i = 0; i < exactLength; ++i)
    if (hexValue(value[i]) < 0) return false;
  return true;
}

// mbedTLS RNG seeded from the ESP hardware TRNG (esp_random via entropy source).
struct Rng {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  bool ok = false;
  Rng() {
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    const char* pers = "solari-approvals";
    ok = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                               (const uint8_t*)pers, strlen(pers)) == 0;
  }
};
Rng& rng() { static Rng r; return r; }

String toHex(const uint8_t* b, size_t n) {
  static const char* h = "0123456789abcdef";
  String s; s.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) { s += h[b[i] >> 4]; s += h[b[i] & 0xF]; }
  return s;
}
// mbedTLS base64 without pulling the header everywhere.
String b64encode(const uint8_t* data, size_t len) {
  size_t need = 4 * ((len + 2) / 3) + 1;
  std::vector<uint8_t> out(need);
  size_t olen = 0;
  if (mbedtls_base64_encode(out.data(), need, &olen, data, len) != 0) return "";
  return String((const char*)out.data());
}
}  // namespace

String Approvals::deviceId() const {
  const auto& s = config().settings();
  if (!s.deviceId.isEmpty()) return s.deviceId;
  return "tab5-" + String((uint32_t)ESP.getEfuseMac(), HEX);
}

void Approvals::begin() {
  // Ensure a device key exists (first boot generates one; see provisionDeviceKey
  // and the enrollment note in DESIGN.md). We do NOT force-regenerate here.
  std::vector<uint8_t> priv;
  if (!config().loadDeviceKey(priv)) {
    // TODO(enroll-ui): drive this from a setup step so the operator can copy the
    // public key to the broker. For now a key is minted on first begin().
    provisionDeviceKey(/*force=*/false);
  }

  mqttBus().subscribe("auth/request/#",
                      [this](const char* topic, const uint8_t* p, unsigned l) {
                        handleRequest_(topic, p, l);
                      });
}

void Approvals::loop() {
  // Drop requests whose TTL has elapsed without an operator decision.
  uint32_t now = millis();
  while (!queue_.empty()) {
    const auto& r = queue_.front();
    if (now - r.receivedMs >= r.ttl * 1000UL) queue_.pop_front();
    else break;
  }
}

const ApprovalRequest* Approvals::pending() const {
  return queue_.empty() ? nullptr : &queue_.front();
}

uint32_t Approvals::secondsLeft(const ApprovalRequest& r) const {
  uint32_t elapsed = (millis() - r.receivedMs) / 1000;
  return elapsed >= r.ttl ? 0 : r.ttl - elapsed;
}

void Approvals::handleRequest_(const char* topic, const uint8_t* payload,
                               unsigned len) {
  if (!payload || len == 0 || len > kMaxRequestBytes) return;
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) return;  // ignore malformed

  ApprovalRequest r;
  if (!verifyRequest_(doc, r, topic)) return;

  // De-dupe (QoS re-delivery) and bound the queue.
  for (const auto& q : queue_) if (q.id == r.id) return;
  queue_.push_back(r);
  while (queue_.size() > kMaxQueue) queue_.pop_front();

  if (alert_) alert_(queue_.back());
}

bool Approvals::verifyRequest_(JsonDocument& doc, ApprovalRequest& r,
                               const char* topic) const {
  if (!doc.is<JsonObject>() || doc.as<JsonObjectConst>().size() != 12)
    return false;
  for (JsonPairConst member : doc.as<JsonObjectConst>()) {
    const char* key = member.key().c_str();
    if (strcmp(key, "v") && strcmp(key, "kid") && strcmp(key, "id") && strcmp(key, "ts") &&
        strcmp(key, "ttl") && strcmp(key, "action") && strcmp(key, "subject") &&
        strcmp(key, "detail") && strcmp(key, "source_ip") &&
        strcmp(key, "nonce") && strcmp(key, "device") && strcmp(key, "sig"))
      return false;
  }
  if (!doc["v"].is<int>() || doc["v"].as<int>() != kProtoVersion ||
      !doc["kid"].is<const char*>() || !doc["id"].is<const char*>() ||
      !doc["ts"].is<uint32_t>() ||
      !doc["ttl"].is<uint32_t>() || !doc["action"].is<const char*>() ||
      !doc["subject"].is<const char*>() || !doc["detail"].is<const char*>() ||
      !doc["source_ip"].is<const char*>() || !doc["nonce"].is<const char*>() ||
      !doc["device"].is<const char*>() ||
      !doc["sig"].is<const char*>()) return false;

  r.brokerKeyId = doc["kid"].as<String>();
  r.id = doc["id"].as<String>();
  r.action = doc["action"].as<String>();
  r.subject = doc["subject"].as<String>();
  r.detail = doc["detail"].as<String>();
  r.sourceIp = doc["source_ip"].as<String>();
  r.nonce = doc["nonce"].as<String>();
  r.targetDevice = doc["device"].as<String>();

  const uint32_t issuedAt = doc["ts"].as<uint32_t>();
  const uint32_t ttl = doc["ttl"].as<uint32_t>();
  if (r.brokerKeyId.isEmpty() || r.brokerKeyId.length() > 14 ||
      !isHex(r.id, 32) || r.action.isEmpty() || r.action.length() > 32 ||
      r.subject.isEmpty() || r.subject.length() > 128 ||
      r.detail.length() > 256 || r.sourceIp.length() > 64 ||
      r.nonce.length() != 44 || r.targetDevice.length() > 64 ||
      ttl == 0 || ttl > 120) return false;
  for (size_t i = 0; i < r.nonce.length(); ++i) {
    const char c = r.nonce[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '='))
      return false;
  }
  if (!topic || String(topic) != "auth/request/" + r.id) return false;
  if (!r.targetDevice.isEmpty() && r.targetDevice != deviceId()) return false;

  // The RTC/SNTP clock is part of the approval trust boundary. Never reset a
  // delayed request's countdown on receipt or accept a request without time.
  const time_t now = time(nullptr);
  if (now < 1700000000) return false;
  const int64_t delta = (int64_t)issuedAt - (int64_t)now;
  if (delta > kAllowedClockSkewSec ||
      (int64_t)now >= (int64_t)issuedAt + ttl) return false;
  const int64_t remaining = (int64_t)issuedAt + ttl - (int64_t)now;
  if (remaining <= 0) return false;
  r.ts = (time_t)issuedAt;
  r.ttl = ttl;  // canonical request uses the original signed TTL

  for (size_t i = 0; i < r.brokerKeyId.length(); ++i) {
    const char c = r.brokerKeyId[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '_' || c == '-')) return false;
  }
  const String brokerKeyHex = config().loadBrokerRequestPubHex(r.brokerKeyId);
  const String signatureB64 = doc["sig"].as<String>();
  std::vector<uint8_t> brokerKey, signature;
  if (!decodeHex(brokerKeyHex, brokerKey) || brokerKey.size() != 65 ||
      brokerKey[0] != 0x04 || signatureB64.isEmpty() ||
      signatureB64.length() > 128) return false;
  signature.resize(96);
  size_t signatureLen = 0;
  if (mbedtls_base64_decode(signature.data(), signature.size(), &signatureLen,
                            (const uint8_t*)signatureB64.c_str(),
                            signatureB64.length()) != 0 ||
      signatureLen == 0 || signatureLen > 72) return false;
  signature.resize(signatureLen);

  const std::vector<uint8_t> canonical = canonicalRequest(r);
  uint8_t digest[32];
  if (mbedtls_sha256(canonical.data(), canonical.size(), digest, 0) != 0)
    return false;

  mbedtls_ecdsa_context ctx;
  mbedtls_ecdsa_init(&ctx);
  bool valid = false;
  do {
    if (mbedtls_ecp_group_load(&ctx.MBEDTLS_PRIVATE(grp),
                               MBEDTLS_ECP_DP_SECP256R1) != 0) break;
    if (mbedtls_ecp_point_read_binary(&ctx.MBEDTLS_PRIVATE(grp),
                                      &ctx.MBEDTLS_PRIVATE(Q), brokerKey.data(),
                                      brokerKey.size()) != 0) break;
    if (mbedtls_ecdsa_read_signature(&ctx, digest, sizeof(digest),
                                     signature.data(), signature.size()) != 0)
      break;
    valid = true;
  } while (false);
  mbedtls_ecdsa_free(&ctx);
  if (!valid) return false;

  r.reqDigest = toHex(digest, sizeof(digest));
  r.ttl = (uint32_t)remaining;
  r.receivedMs = millis();
  return true;
}

void Approvals::approve() {
  if (queue_.empty()) return;
  ApprovalRequest r = queue_.front();
  queue_.pop_front();
  respond_(r, "approve");
}

void Approvals::deny() {
  if (queue_.empty()) return;
  ApprovalRequest r = queue_.front();
  queue_.pop_front();
  respond_(r, "deny");
}

bool Approvals::respond_(const ApprovalRequest& r, const char* decision) {
  String dev = deviceId();
  const std::vector<uint8_t> canonical = canonicalResponse(r, decision, dev);
  String sig;
  if (!signCanonical_(canonical, sig)) return false;

  JsonDocument doc;
  doc["v"] = kProtoVersion;
  doc["id"] = r.id;
  doc["ts"] = (long)time(nullptr);
  doc["decision"] = decision;
  doc["device_id"] = dev;
  doc["nonce"] = r.nonce;
  doc["req_digest"] = r.reqDigest;
  doc["sig"] = sig;
  String out;
  serializeJson(doc, out);
  return mqttBus().publish("auth/response/" + r.id, out, /*qos=*/1);
}

// ---- ECDSA-P256/SHA-256 signing -------------------------------------------
bool Approvals::signCanonical_(const std::vector<uint8_t>& canonical,
                               String& sigB64Out) const {
  std::vector<uint8_t> priv;
  if (!config().loadDeviceKey(priv) || priv.size() != 32) return false;
  if (!rng().ok) return false;

  uint8_t hash[32];
  if (mbedtls_sha256(canonical.data(), canonical.size(), hash, 0) != 0)
    return false;

  mbedtls_ecdsa_context ctx;
  mbedtls_ecdsa_init(&ctx);
  bool ok = false;
  do {
    if (mbedtls_ecp_group_load(&ctx.MBEDTLS_PRIVATE(grp),
                               MBEDTLS_ECP_DP_SECP256R1) != 0) break;
    if (mbedtls_mpi_read_binary(&ctx.MBEDTLS_PRIVATE(d), priv.data(), 32) != 0)
      break;

    uint8_t der[MBEDTLS_ECDSA_MAX_LEN];
    size_t derLen = 0;
    // Deterministic-friendly signing; RNG guards against nonce reuse.
    if (mbedtls_ecdsa_write_signature(&ctx, MBEDTLS_MD_SHA256, hash, sizeof(hash),
                                      der, sizeof(der), &derLen,
                                      mbedtls_ctr_drbg_random,
                                      &rng().drbg) != 0)
      break;
    sigB64Out = b64encode(der, derLen);  // broker verifies DER over the hash
    ok = !sigB64Out.isEmpty();
  } while (false);
  mbedtls_ecdsa_free(&ctx);
  // Wipe the private scalar copy off the heap.
  if (!priv.empty()) memset(priv.data(), 0, priv.size());
  return ok;
}

// ---- key provisioning ------------------------------------------------------
String Approvals::provisionDeviceKey(bool force) {
  std::vector<uint8_t> existing;
  if (!force && config().loadDeviceKey(existing) && existing.size() == 32)
    return publicKeyHex();
  if (!rng().ok) return "";

  mbedtls_ecp_keypair kp;
  mbedtls_ecp_keypair_init(&kp);
  String pubHex;
  if (mbedtls_ecp_group_load(&kp.MBEDTLS_PRIVATE(grp),
                             MBEDTLS_ECP_DP_SECP256R1) == 0 &&
      mbedtls_ecp_gen_keypair(&kp.MBEDTLS_PRIVATE(grp),
                              &kp.MBEDTLS_PRIVATE(d), &kp.MBEDTLS_PRIVATE(Q),
                              mbedtls_ctr_drbg_random, &rng().drbg) == 0) {
    uint8_t priv[32];
    if (mbedtls_mpi_write_binary(&kp.MBEDTLS_PRIVATE(d), priv, sizeof(priv)) == 0
        && config().saveDeviceKey(priv, sizeof(priv))) {
      uint8_t pub[65];
      size_t plen = 0;
      if (mbedtls_ecp_point_write_binary(&kp.MBEDTLS_PRIVATE(grp),
                                         &kp.MBEDTLS_PRIVATE(Q),
                                         MBEDTLS_ECP_PF_UNCOMPRESSED, &plen,
                                         pub, sizeof(pub)) == 0) {
        pubHex = toHex(pub, plen);
        config().saveDevicePubHex(pubHex);
      }
    }
    memset(priv, 0, sizeof(priv));
  }
  mbedtls_ecp_keypair_free(&kp);
  return pubHex;
}

String Approvals::publicKeyHex() const { return config().loadDevicePubHex(); }

}  // namespace solari
