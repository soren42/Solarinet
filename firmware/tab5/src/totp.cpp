#include "totp.h"

#include <M5Unified.h>
#include <time.h>
#include <esp_sntp.h>
#include <mbedtls/md.h>

namespace solari {
namespace totp {

int base32Decode(const String& in, uint8_t* out, size_t outCap) {
  static const char* kAlpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
  uint32_t buffer = 0;
  int bitsLeft = 0;
  size_t count = 0;
  for (size_t i = 0; i < in.length(); ++i) {
    char c = in[i];
    if (c == '=' || c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
    // Case-insensitive; map to 5-bit value.
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    const char* p = strchr(kAlpha, c);
    if (!p) return -1;  // invalid character
    buffer = (buffer << 5) | (uint32_t)(p - kAlpha);
    bitsLeft += 5;
    if (bitsLeft >= 8) {
      if (count >= outCap) return -1;
      bitsLeft -= 8;
      out[count++] = (uint8_t)((buffer >> bitsLeft) & 0xFF);
    }
  }
  return (int)count;
}

static mbedtls_md_type_t mdFor(uint8_t algo) {
  switch (algo) {
    case 1: return MBEDTLS_MD_SHA256;
    case 2: return MBEDTLS_MD_SHA512;
    default: return MBEDTLS_MD_SHA1;  // RFC 6238 default
  }
}

bool compute(const TotpAccount& account, time_t now, Code& out) {
  uint8_t key[128];
  int keyLen = base32Decode(account.secretB32, key, sizeof(key));
  if (keyLen <= 0) return false;

  uint16_t period = account.period ? account.period : 30;
  uint64_t counter = (uint64_t)(now / period);

  // 8-byte big-endian counter (HOTP moving factor).
  uint8_t msg[8];
  for (int i = 7; i >= 0; --i) {
    msg[i] = (uint8_t)(counter & 0xFF);
    counter >>= 8;
  }

  uint8_t hmac[64];  // large enough for SHA-512
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(mdFor(account.algo));
  if (!info) return false;
  if (mbedtls_md_hmac(info, key, keyLen, msg, sizeof(msg), hmac) != 0) return false;
  size_t hlen = mbedtls_md_get_size(info);

  // Dynamic truncation (RFC 4226 §5.3).
  int offset = hmac[hlen - 1] & 0x0F;
  uint32_t bin = ((uint32_t)(hmac[offset] & 0x7F) << 24) |
                 ((uint32_t)hmac[offset + 1] << 16) |
                 ((uint32_t)hmac[offset + 2] << 8) |
                 ((uint32_t)hmac[offset + 3]);

  uint8_t digits = account.digits ? account.digits : 6;
  uint32_t mod = 1;
  for (uint8_t i = 0; i < digits; ++i) mod *= 10;
  uint32_t code = bin % mod;

  char buf[12];
  snprintf(buf, sizeof(buf), "%0*u", (int)digits, code);
  out.digits = buf;
  out.period = period;
  out.secondsLeft = period - (uint32_t)(now % period);

  // Wipe key material from the stack promptly.
  memset(key, 0, sizeof(key));
  memset(hmac, 0, sizeof(hmac));
  return true;
}

// ---- Time discipline -------------------------------------------------------
void beginTimeSync(const String& ntpServer, const String& posixTz) {
  setenv("TZ", posixTz.c_str(), 1);
  tzset();
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, ntpServer.c_str());
  // On sync, mirror into the RTC so we survive reboots without network.
  sntp_set_time_sync_notification_cb([](struct timeval*) { syncRtcFromSystem(); });
  esp_sntp_init();
}

bool timeIsValid() {
  time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);
  return (tmv.tm_year + 1900) >= 2024;
}

void syncRtcFromSystem() {
  time_t now = time(nullptr);
  struct tm tmv;
  gmtime_r(&now, &tmv);  // RTC holds UTC; TZ handled in software
  m5::rtc_datetime_t dt;
  dt.date.year = tmv.tm_year + 1900;
  dt.date.month = tmv.tm_mon + 1;
  dt.date.date = tmv.tm_mday;
  dt.time.hours = tmv.tm_hour;
  dt.time.minutes = tmv.tm_min;
  dt.time.seconds = tmv.tm_sec;
  M5.Rtc.setDateTime(dt);
}

void seedSystemFromRtc() {
  if (!M5.Rtc.isEnabled()) return;
  auto dt = M5.Rtc.getDateTime();
  struct tm tmv = {};
  tmv.tm_year = dt.date.year - 1900;
  tmv.tm_mon = dt.date.month - 1;
  tmv.tm_mday = dt.date.date;
  tmv.tm_hour = dt.time.hours;
  tmv.tm_min = dt.time.minutes;
  tmv.tm_sec = dt.time.seconds;
  time_t t = timegm(&tmv);  // RTC is UTC
  struct timeval tv = {.tv_sec = t, .tv_usec = 0};
  settimeofday(&tv, nullptr);
}

}  // namespace totp
}  // namespace solari
