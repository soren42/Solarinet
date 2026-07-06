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

// ===========================================================================
// SCAFFOLD STATUS
// The approval flow (subscribe, prompt, sign, publish) is implemented against
// the same canonical signing bytes the broker verifies (deploy/authbroker/).
// Signing uses mbedTLS ECDSA over secp256r1 (P-256) + SHA-256, which is present
// in the IDF's bundled mbedTLS. Two steps are TODO-marked: (a) first-boot key
// GENERATION UI wiring, and (b) confirming the exact NVS private-key encoding
// on the pinned core. The private scalar is stored in the encrypted `secrets`
// NVS partition (namespace "dev"); the broker only ever holds the public key.
// ===========================================================================

namespace solari {

static Approvals g_approvals;
Approvals& approvals() { return g_approvals; }

namespace {
constexpr int kProtoVersion = 1;

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
                      [this](const char*, const uint8_t* p, unsigned l) {
                        handleRequest_(p, l);
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

void Approvals::handleRequest_(const uint8_t* payload, unsigned len) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) return;  // ignore malformed
  if ((int)(doc["v"] | 0) != kProtoVersion) return;

  ApprovalRequest r;
  r.id = String((const char*)(doc["id"] | ""));
  if (r.id.isEmpty()) return;
  r.action = String((const char*)(doc["action"] | "login"));
  r.subject = String((const char*)(doc["subject"] | ""));
  r.detail = String((const char*)(doc["detail"] | ""));
  r.sourceIp = String((const char*)(doc["source_ip"] | ""));
  r.nonce = String((const char*)(doc["nonce"] | ""));
  if (r.nonce.isEmpty()) return;  // no anti-replay nonce => reject
  r.ts = doc["ts"] | (long)time(nullptr);
  r.ttl = doc["ttl"] | 60;
  r.receivedMs = millis();

  // If a specific device was targeted, ignore requests for other devices.
  const char* target = doc["device"] | "";
  if (target[0] && deviceId() != target) return;

  // De-dupe (QoS re-delivery) and bound the queue.
  for (const auto& q : queue_) if (q.id == r.id) return;
  queue_.push_back(r);
  while (queue_.size() > kMaxQueue) queue_.pop_front();

  if (alert_) alert_(queue_.back());
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
  // Canonical bytes MUST match the broker: "<v>\n<id>\n<decision>\n<nonce>\n<dev>"
  String dev = deviceId();
  String canonical = String(kProtoVersion) + "\n" + r.id + "\n" + decision +
                     "\n" + r.nonce + "\n" + dev;
  String sig;
  if (!signCanonical_(canonical, sig)) return false;

  JsonDocument doc;
  doc["v"] = kProtoVersion;
  doc["id"] = r.id;
  doc["ts"] = (long)time(nullptr);
  doc["decision"] = decision;
  doc["device_id"] = dev;
  doc["nonce"] = r.nonce;
  doc["sig"] = sig;
  String out;
  serializeJson(doc, out);
  return mqttBus().publish("auth/response/" + r.id, out, /*qos=*/1);
}

// ---- ECDSA-P256/SHA-256 signing -------------------------------------------
bool Approvals::signCanonical_(const String& canonical, String& sigB64Out) const {
  std::vector<uint8_t> priv;
  if (!config().loadDeviceKey(priv) || priv.size() != 32) return false;
  if (!rng().ok) return false;

  uint8_t hash[32];
  mbedtls_sha256((const uint8_t*)canonical.c_str(), canonical.length(), hash, 0);

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
