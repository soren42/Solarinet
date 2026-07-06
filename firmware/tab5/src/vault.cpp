#include "vault.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>

#include "config.h"

// ===========================================================================
// SCAFFOLD STATUS
// This file lays out the full Bitwarden/Vaultwarden unlock + sync + decrypt
// flow with correct crypto primitives, but several steps are TODO-marked where
// they need finishing/verification against the actual server. It is structured
// so each piece can be filled and unit-tested independently. Do NOT ship as-is
// for real secrets until the TODOs are closed and tested against the server.
// ===========================================================================

namespace solari {

static Vault g_vault;
Vault& vault() { return g_vault; }

// ---- helpers ---------------------------------------------------------------
static bool b64decode(const String& in, std::vector<uint8_t>& out) {
  size_t need = 0;
  mbedtls_base64_decode(nullptr, 0, &need,
                        (const uint8_t*)in.c_str(), in.length());
  out.resize(need);
  size_t got = 0;
  int rc = mbedtls_base64_decode(out.data(), out.size(), &got,
                                 (const uint8_t*)in.c_str(), in.length());
  if (rc != 0) return false;
  out.resize(got);
  return true;
}

// Parse a Bitwarden "EncString": "<type>.<b64iv>|<b64ct>|<b64mac>".
// Only type 2 (AesCbc256_HmacSha256_B64) is supported — the modern default.
struct EncString {
  int type = -1;
  std::vector<uint8_t> iv, ct, mac;
};
static bool parseEncString(const String& s, EncString& e) {
  int dot = s.indexOf('.');
  if (dot < 0) return false;
  e.type = s.substring(0, dot).toInt();
  if (e.type != 2) return false;  // TODO: support type 0 if any legacy items
  String rest = s.substring(dot + 1);
  int p1 = rest.indexOf('|');
  int p2 = rest.indexOf('|', p1 + 1);
  if (p1 < 0 || p2 < 0) return false;
  return b64decode(rest.substring(0, p1), e.iv) &&
         b64decode(rest.substring(p1 + 1, p2), e.ct) &&
         b64decode(rest.substring(p2 + 1), e.mac);
}

// ---- key derivation --------------------------------------------------------
// masterKey is a member scratch buffer; kept only long enough to unwrap the
// user key, then overwritten.
static uint8_t s_masterKey[32];

bool Vault::deriveMasterKey_(const String& password) {
  const String& email = config().settings().pmEmail;  // salt = lowercase email
  String salt = email;
  salt.toLowerCase();

  if (kdfType_ == 0) {
    // PBKDF2-HMAC-SHA256, `kdfIterations_` rounds.
    const mbedtls_md_info_t* md =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(
        MBEDTLS_MD_SHA256,
        (const uint8_t*)password.c_str(), password.length(),
        (const uint8_t*)salt.c_str(), salt.length(),
        kdfIterations_, sizeof(s_masterKey), s_masterKey);
    return rc == 0;
  }
  // kdfType_ == 1: Argon2id(password, salt=email, iterations, memoryKiB, par).
  // TODO(argon2): mbedTLS proper doesn't expose Argon2; use the ESP-IDF PSA
  // crypto (psa_key_derivation with PSA_ALG_ARGON2ID) or vendor a small
  // argon2 component. Fill s_masterKey (32B) here. Until then, Argon2 accounts
  // cannot unlock — surface a clear "Argon2 not yet supported" error in the UI.
  return false;
}

bool Vault::decryptUserKey_() {
  // HKDF-Expand master_key -> 64B: first 32 enc key, next 32 mac key
  // (Bitwarden "stretch" step). Then AES-CBC/HMAC-decrypt the account's
  // protected_symmetric_key (an EncString fetched during login/sync) to get
  // the real 64B user key (userKeyEnc_ || userKeyMac_).
  uint8_t stretched[64];
  const uint8_t infoEnc[] = "enc";
  const uint8_t infoMac[] = "mac";
  // HKDF-Expand only (salt empty) per Bitwarden.
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (mbedtls_hkdf_expand(md, s_masterKey, 32, infoEnc, 3, stretched, 32) != 0)
    return false;
  if (mbedtls_hkdf_expand(md, s_masterKey, 32, infoMac, 3, stretched + 32, 32) != 0)
    return false;

  // TODO(protected-key): fetch protected_symmetric_key ("Key" in /api/sync
  // profile, or from the token response), parse as EncString, verify HMAC with
  // stretched[32..63], AES-256-CBC decrypt with stretched[0..31] -> 64B user
  // key, split into userKeyEnc_/userKeyMac_.
  memset(s_masterKey, 0, sizeof(s_masterKey));  // done with master key
  memset(stretched, 0, sizeof(stretched));
  return false;  // TODO: return true once the unwrap above is implemented
}

bool Vault::decryptEncString_(const String& enc, std::vector<uint8_t>& out) const {
  EncString e;
  if (!parseEncString(enc, e)) return false;

  // 1) Verify HMAC-SHA256 over (iv || ct) with userKeyMac_ — constant-time.
  uint8_t calc[32];
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, md, /*hmac=*/1);
  mbedtls_md_hmac_starts(&ctx, userKeyMac_, 32);
  mbedtls_md_hmac_update(&ctx, e.iv.data(), e.iv.size());
  mbedtls_md_hmac_update(&ctx, e.ct.data(), e.ct.size());
  mbedtls_md_hmac_finish(&ctx, calc);
  mbedtls_md_free(&ctx);
  uint8_t diff = 0;
  for (int i = 0; i < 32 && i < (int)e.mac.size(); ++i) diff |= calc[i] ^ e.mac[i];
  if (diff != 0 || e.mac.size() != 32) return false;  // tamper / wrong key

  // 2) AES-256-CBC decrypt.
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_dec(&aes, userKeyEnc_, 256);
  out.resize(e.ct.size());
  uint8_t iv[16];
  memcpy(iv, e.iv.data(), 16);
  int rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, e.ct.size(),
                                 iv, e.ct.data(), out.data());
  mbedtls_aes_free(&aes);
  if (rc != 0) return false;
  // Strip PKCS#7 padding.
  if (!out.empty()) {
    uint8_t pad = out.back();
    if (pad >= 1 && pad <= 16 && pad <= out.size()) out.resize(out.size() - pad);
  }
  return true;
}

// ---- API -------------------------------------------------------------------
UnlockResult Vault::login_(const String& password) {
  // POST {pmBaseUrl}/identity/connect/token
  //   grant_type=password, username=email,
  //   password=<b64(master_password_hash)>, scope=api offline_access,
  //   client_id=... , deviceType/deviceIdentifier/deviceName headers.
  // First, GET /identity/accounts/prelogin {email} to learn kdfType/params.
  // TODO(http): implement with WiFiClientSecure + HTTPClient; set the CA /
  // pinned cert for the self-hosted server (see config: pmBaseUrl). Parse the
  // access_token + Key (protected symmetric key) from the JSON response.
  (void)password;
  return UnlockResult::ServerError;  // TODO
}

bool Vault::sync_() {
  // GET {pmBaseUrl}/api/sync (Bearer accessToken_) -> big JSON. Stream-parse
  // with ArduinoJson (filter to ciphers[].{id,type,name,login.{username,uris,
  // password,totp}}), decrypt name/username/uri now for the list, and write
  // the raw encrypted payload to encVaultCachePath_ on LittleFS for offline
  // unlock. Passwords/TOTP stay encrypted until revealPassword()/revealTotp().
  // TODO(http+parse): implement streaming parse to stay within RAM budget.
  return false;
}

// ---- public API ------------------------------------------------------------
UnlockResult Vault::unlock(const String& secret, bool preferOffline) {
  if (!config().settings().provisioned || config().settings().pmEmail.isEmpty())
    return UnlockResult::NeedsProvision;

  // prelogin/kdf params: online path fetches them; offline path reads cached
  // kdfType_/iterations_ persisted at first successful online unlock.
  if (!preferOffline) {
    UnlockResult r = login_(secret);  // sets tokens + fetches KDF params
    if (r != UnlockResult::Ok) return r;
  }
  if (!deriveMasterKey_(secret)) return UnlockResult::BadPassword;
  if (!decryptUserKey_()) return UnlockResult::BadPassword;  // wrong pw fails HMAC
  if (!preferOffline && !sync_()) return UnlockResult::Network;
  // TODO(offline): load + decrypt encVaultCachePath_ into items_ here.

  unlocked_ = true;
  return UnlockResult::Ok;
}

void Vault::lock() {
  unlocked_ = false;
  memset(userKeyEnc_, 0, sizeof(userKeyEnc_));
  memset(userKeyMac_, 0, sizeof(userKeyMac_));
  accessToken_ = "";
  items_.clear();
}

std::vector<VaultItem> Vault::search(const String& query) const {
  if (query.isEmpty()) return items_;
  std::vector<VaultItem> out;
  String q = query; q.toLowerCase();
  for (const auto& it : items_) {
    String hay = it.name + " " + it.username + " " + it.uri;
    hay.toLowerCase();
    if (hay.indexOf(q) >= 0) out.push_back(it);
  }
  return out;
}

bool Vault::revealPassword(const String& itemId, String& out) {
  if (!unlocked_) return false;
  // TODO: look up the cached EncString password for itemId, decryptEncString_.
  (void)itemId; (void)out;
  return false;
}

bool Vault::revealTotp(const String& itemId, String& out) {
  if (!unlocked_) return false;
  // TODO: decrypt the item's TOTP secret, then reuse solari::totp::compute.
  (void)itemId; (void)out;
  return false;
}

bool Vault::copyField(const String& text, uint32_t clearAfterMs) {
  if (!unlocked_) return false;
  // Delegated to ui.* clipboard buffer with auto-clear (Tab5 has no OS
  // clipboard; "copy" means show + hold in a screen buffer we clear on timer).
  extern void ui_setClipboard(const String&, uint32_t);
  ui_setClipboard(text, clearAfterMs);
  return true;
}

}  // namespace solari
