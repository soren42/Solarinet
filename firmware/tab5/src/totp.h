// totp.h — RFC 6238 TOTP generation for the SolariNet Tab5.
//
// Codes are derived on-device from Base32 seeds held in encrypted NVS
// (see config.*). Time comes from the on-board RTC (RX8130), which we keep
// disciplined via SNTP over Wi-Fi (see rtc/sntp helpers below and DESIGN.md).
#pragma once

#include <Arduino.h>
#include "config.h"

namespace solari {
namespace totp {

// Result of a single code computation.
struct Code {
  String digits;        // zero-padded, `account.digits` wide
  uint32_t secondsLeft; // until this code rolls over (for the countdown ring)
  uint32_t period;      // account period (usually 30) — for ring geometry
};

// Compute the current code for `account` at unix time `now` (seconds).
// Returns false if the Base32 secret is malformed.
bool compute(const TotpAccount& account, time_t now, Code& out);

// Base32 (RFC 4648, no padding required) decode. Returns decoded length, or
// -1 on an invalid character. `out` must have room for ceil(len*5/8) bytes.
int base32Decode(const String& in, uint8_t* out, size_t outCap);

// ---- Time discipline -------------------------------------------------------
// Kick off SNTP. Call once after Wi-Fi is up. Non-blocking; the ESP SNTP
// service updates the system clock, and syncRtcFromSystem() copies it into the
// RX8130 so codes stay correct across reboots / brief Wi-Fi outages.
void beginTimeSync(const String& ntpServer, const String& posixTz);

// True once SNTP has set a plausible wall-clock (year >= 2024).
bool timeIsValid();

// Push the current system time into the hardware RTC (call after a sync).
void syncRtcFromSystem();

// Seed the system clock FROM the RTC at boot (before Wi-Fi/SNTP), so codes are
// usable within seconds of power-on even before the network is up.
void seedSystemFromRtc();

}  // namespace totp
}  // namespace solari
