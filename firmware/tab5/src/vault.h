// vault.h — Vaultwarden/Bitwarden-compatible password-manager client.
//
// OPEN QUESTION (see README): confirm the user actually runs Vaultwarden /
// Bitwarden. The base URL + endpoints are configurable so a different
// Bitwarden-API-compatible server can be pointed at without code changes.
//
// Crypto summary (Bitwarden model — see DESIGN.md for the full chain):
//   master_key      = KDF(master_password, email, kdfParams)   PBKDF2 or Argon2id
//   master_password_hash = PBKDF2(master_key, master_password, 1)  -> sent to server to auth
//   stretched_key   = HKDF-Expand(master_key) -> 32B enc + 32B mac
//   user_key (sym)  = AES-CBC/HMAC decrypt(protected_symmetric_key, stretched_key)
//   cipher fields   = AES-256-CBC + HMAC-SHA256 (EncString "2.iv|ct|mac"), user_key
//
// All symmetric crypto uses mbedTLS (bundled in the IDF). Argon2id needs the
// mbedtls PSA or a small argon2 component — see TODO in vault.cpp.
#pragma once

#include <Arduino.h>
#include <vector>

namespace solari {

struct VaultItem {
  String id;
  String name;      // decrypted
  String username;  // decrypted
  String uri;       // decrypted (first URI), for search/match
  // Password is decrypted on demand (revealItem) to minimize plaintext dwell.
  bool hasTotp = false;
  bool hasPassword = false;
};

enum class UnlockResult { Ok, BadPassword, Network, ServerError, NeedsProvision };

class Vault {
 public:
  // Unlock flow. Two modes:
  //  - online: authenticate to the server (password grant), pull the vault
  //    sync, derive keys, cache the encrypted vault locally.
  //  - offline: re-derive keys from a locally cached protected key + the
  //    master password / PIN, decrypting the cached vault without network.
  // `secret` is the master password (or PIN, if a PIN was set up).
  UnlockResult unlock(const String& secret, bool preferOffline);

  void lock();               // zeroize derived keys + plaintext caches
  bool isUnlocked() const { return unlocked_; }

  // Search the (decrypted-name) item list. Empty query returns all.
  std::vector<VaultItem> search(const String& query) const;

  // Reveal a single field on demand. Returns false if locked / not found.
  bool revealPassword(const String& itemId, String& out);
  bool revealTotp(const String& itemId, String& out);  // if item stores a TOTP

  // Copy helper: puts text on the on-screen clipboard buffer and starts an
  // auto-clear timer (see ui.*). Returns false if locked.
  bool copyField(const String& text, uint32_t clearAfterMs = 20000);

 private:
  // --- crypto (vault.cpp) ---
  bool deriveMasterKey_(const String& password);        // KDF per kdfType
  bool decryptUserKey_();                                // unwrap protected key
  bool decryptEncString_(const String& enc, std::vector<uint8_t>& out) const;

  // --- API (vault.cpp) ---
  UnlockResult login_(const String& password);          // /identity/connect/token
  bool sync_();                                          // /api/sync -> cache

  bool unlocked_ = false;
  int kdfType_ = 0;         // 0=PBKDF2-SHA256, 1=Argon2id (Bitwarden enum)
  int kdfIterations_ = 600000;
  int kdfMemoryMiB_ = 64;   // Argon2 only
  int kdfParallelism_ = 4;  // Argon2 only

  uint8_t userKeyEnc_[32];  // 32B AES key  (zeroized on lock)
  uint8_t userKeyMac_[32];  // 32B HMAC key (zeroized on lock)
  String accessToken_;      // bearer for /api/* (short-lived; refresh cached)

  std::vector<VaultItem> items_;   // decrypted metadata (names/usernames/uris)
  String encVaultCachePath_ = "/vault.enc.json";  // LittleFS cache of /api/sync
};

Vault& vault();

}  // namespace solari
