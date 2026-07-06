// pwgen.h — secure password generator for the SolariNet Authenticator.
//
// Generates strong passwords from a CSPRNG (esp_random, backed by the ESP32-P4
// hardware TRNG — NEVER rand()). Configurable length + character classes, with
// an "avoid ambiguous characters" option. Output can be fed to the vault (store
// a new credential) or typed straight into a host over BLE-HID (blehid.*), so
// the plaintext never touches an OS clipboard.
#pragma once

#include <Arduino.h>

namespace solari {

struct PwGenPolicy {
  uint8_t length = 20;      // 8..64 (clamped)
  bool upper = true;        // A-Z
  bool lower = true;        // a-z
  bool digits = true;       // 0-9
  bool symbols = true;      // punctuation
  bool avoidAmbiguous = false;  // drop 0/O/1/l/I and lookalikes
};

class PwGen {
 public:
  // Generate a password meeting `policy`. Guarantees at least one character
  // from every enabled class (unbiased rejection sampling over the CSPRNG).
  // Returns "" if no classes are enabled.
  String generate(const PwGenPolicy& policy);

  // The most recently generated value (held so the UI can show / type / store
  // it). Cleared by clear() and by an idle/auto-lock timeout in the UI.
  const String& last() const { return last_; }
  void clear();

 private:
  String last_;
};

PwGen& pwgen();

}  // namespace solari
