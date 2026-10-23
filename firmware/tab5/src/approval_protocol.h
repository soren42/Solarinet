// Shared v2 signature canonicalization for the Tab5 and host tests.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace solari {
namespace approval_protocol {

inline void appendField(std::vector<uint8_t>& out, const std::string& value) {
  const uint32_t n = static_cast<uint32_t>(value.size());
  out.push_back(static_cast<uint8_t>(n >> 24));
  out.push_back(static_cast<uint8_t>(n >> 16));
  out.push_back(static_cast<uint8_t>(n >> 8));
  out.push_back(static_cast<uint8_t>(n));
  out.insert(out.end(), value.begin(), value.end());
}

template <size_t N>
inline void appendFields(std::vector<uint8_t>& out,
                         const std::array<std::string, N>& fields) {
  for (const auto& field : fields) appendField(out, field);
}

inline std::vector<uint8_t> canonicalRequest(
    const std::array<std::string, 11>& fields) {
  static const uint8_t domain[] = {
      'S','O','L','A','R','I','-','A','U','T','H','-','R','E','Q','U','E','S','T',0,2};
  std::vector<uint8_t> out(domain, domain + sizeof(domain));
  appendFields(out, fields);
  return out;
}

inline std::vector<uint8_t> canonicalResponse(
    const std::array<std::string, 6>& fields) {
  static const uint8_t domain[] = {
      'S','O','L','A','R','I','-','A','U','T','H','-','R','E','S','P','O','N','S','E',0,2};
  std::vector<uint8_t> out(domain, domain + sizeof(domain));
  appendFields(out, fields);
  return out;
}

}  // namespace approval_protocol
}  // namespace solari
