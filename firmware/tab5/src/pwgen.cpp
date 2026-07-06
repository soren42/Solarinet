#include "pwgen.h"

#include <esp_random.h>

namespace solari {

static PwGen g_pwgen;
PwGen& pwgen() { return g_pwgen; }

namespace {
const char* kUpper = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
const char* kLower = "abcdefghijklmnopqrstuvwxyz";
const char* kDigit = "0123456789";
const char* kSym   = "!@#$%^&*()-_=+[]{};:,.?/";
// Characters that look alike in most fonts (dropped when avoidAmbiguous).
const char* kAmbig = "O0oiIl1|`'\"{}[]:;.,";

// Unbiased index in [0,n) from the hardware CSPRNG via rejection sampling.
uint32_t uniform(uint32_t n) {
  if (n == 0) return 0;
  uint32_t limit = UINT32_MAX - (UINT32_MAX % n);  // reject the biased tail
  uint32_t r;
  do { r = esp_random(); } while (r >= limit);
  return r % n;
}

String filterAmbig(const String& in, bool drop) {
  if (!drop) return in;
  String out; out.reserve(in.length());
  for (size_t i = 0; i < in.length(); ++i)
    if (!strchr(kAmbig, in[i])) out += in[i];
  return out;
}
}  // namespace

String PwGen::generate(const PwGenPolicy& policy) {
  uint8_t len = policy.length;
  if (len < 8) len = 8;
  if (len > 64) len = 64;

  // Build the per-class alphabets (post ambiguity filter) and the full pool.
  String classes[4];
  int nClasses = 0;
  auto addClass = [&](bool on, const char* set) {
    if (!on) return;
    String s = filterAmbig(set, policy.avoidAmbiguous);
    if (!s.isEmpty()) classes[nClasses++] = s;
  };
  addClass(policy.upper, kUpper);
  addClass(policy.lower, kLower);
  addClass(policy.digits, kDigit);
  addClass(policy.symbols, kSym);
  if (nClasses == 0) { last_ = ""; return last_; }

  String pool;
  for (int i = 0; i < nClasses; ++i) pool += classes[i];

  // Reserve one slot per enabled class so the result always satisfies the
  // policy, then fill the rest from the full pool, then shuffle.
  std::vector<char> chars;
  chars.reserve(len);
  for (int i = 0; i < nClasses && (int)chars.size() < len; ++i) {
    const String& c = classes[i];
    chars.push_back(c[uniform(c.length())]);
  }
  while ((int)chars.size() < len) chars.push_back(pool[uniform(pool.length())]);

  // Fisher-Yates shuffle so the guaranteed class chars aren't front-loaded.
  for (int i = (int)chars.size() - 1; i > 0; --i) {
    uint32_t j = uniform(i + 1);
    std::swap(chars[i], chars[j]);
  }

  String out; out.reserve(len);
  for (char c : chars) out += c;
  // Wipe the working buffer.
  for (auto& c : chars) c = 0;
  last_ = out;
  return last_;
}

void PwGen::clear() {
  // Overwrite before releasing so the plaintext doesn't linger in the heap.
  for (size_t i = 0; i < last_.length(); ++i) last_[i] = 0;
  last_ = "";
}

}  // namespace solari
