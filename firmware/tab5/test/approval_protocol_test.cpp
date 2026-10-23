#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "../src/approval_protocol.h"

static std::string hex(const std::vector<uint8_t>& bytes) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (uint8_t b : bytes) {
    out.push_back(digits[b >> 4]);
    out.push_back(digits[b & 15]);
  }
  return out;
}

static bool fieldsMatch(const std::vector<uint8_t>& bytes,
                        const std::array<std::string, 11>& fields) {
  static const uint8_t domain[] = {
      'S','O','L','A','R','I','-','A','U','T','H','-','R','E','Q','U','E','S','T',0,2};
  if (bytes.size() < sizeof(domain) ||
      std::memcmp(bytes.data(), domain, sizeof(domain)) != 0) return false;
  size_t offset = sizeof(domain);
  for (const auto& field : fields) {
    if (bytes.size() - offset < 4) return false;
    const uint32_t size = (static_cast<uint32_t>(bytes[offset]) << 24) |
                          (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
                          (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
                          static_cast<uint32_t>(bytes[offset + 3]);
    offset += 4;
    if (size != field.size() || bytes.size() - offset < size ||
        std::memcmp(bytes.data() + offset, field.data(), size) != 0) return false;
    offset += size;
  }
  return offset == bytes.size();
}

int main() {
  const std::string id = "0123456789abcdef0123456789abcdef";
  const std::string nonce = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
  const std::string requestExpected =
      "534f4c4152492d415554482d524551554553540002000000013200000008746573742d6b65790000002030313233343536373839"
      "616263646566303132333435363738396162636465660000000a3137363132323038303000000002"
      "3630000000056c6f67696e0000000b616c6963650a61646d696e0000000c536f6c6172694e657420"
      "cf800000000831302e302e302e370000002c41414141414141414141414141414141414141414141"
      "4141414141414141414141414141414141414141413d00000009746162352d6465736b";
  const auto request = solari::approval_protocol::canonicalRequest({{
      "2", "test-key", id, "1761220800", "60", "login", "alice\nadmin", "SolariNet \xcf\x80",
      "10.0.0.7", nonce, "tab5-desk",
  }});
  if (hex(request) != requestExpected) {
    std::cerr << "request canonical vector mismatch\nactual=" << hex(request)
              << "\nexpected=" << requestExpected << '\n';
    return 1;
  }

  const std::array<std::string, 11> boundaryFields = {{
      "2", "primary", id, "1761220800", "60", "login", std::string(128, 's'),
      std::string(256, 'd'), "", nonce, "",
  }};
  if (!fieldsMatch(solari::approval_protocol::canonicalRequest(boundaryFields),
                   boundaryFields)) {
    std::cerr << "request empty/max field length-prefix mismatch\n";
    return 1;
  }

  const std::string responseExpected =
      "534f4c4152492d415554482d524553504f4e53450002000000013200000020303132333435363738"
      "396162636465663031323334353637383961626364656600000007617070726f76650000002c4141"
      "41414141414141414141414141414141414141414141414141414141414141414141414141414141"
      "413d00000009746162352d6465736b00000040313131313131313131313131313131313131313131"
      "31313131313131313131313131313131313131313131313131313131313131313131313131313131"
      "313131";
  const auto response = solari::approval_protocol::canonicalResponse({{
      "2", id, "approve", nonce, "tab5-desk", std::string(64, '1'),
  }});
  if (hex(response) != responseExpected) {
    std::cerr << "response canonical vector mismatch\n";
    return 1;
  }
  return 0;
}
