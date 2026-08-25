#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <algorithm>

namespace scratchbird::engine::sblr {

// SPRE v1 is the canonical operand carried by SBLR_STMT_PREPARE.  The
// prepared object is identified by the client UUID; the statement body is
// immutable canonical SBLR bytes and is never reconstructed from SQL text.
struct SblrStmtPrepareDescriptorV1 {
  std::array<std::uint8_t, 16> client_statement_uuid{};
  std::uint64_t catalog_generation{0};
  std::uint64_t security_epoch{0};
  std::uint64_t policy_generation{0};
  std::vector<std::uint8_t> canonical_sblr_envelope;
};

inline std::vector<std::uint8_t> EncodeSblrStmtPrepareDescriptorV1(
    const SblrStmtPrepareDescriptorV1& value) {
  if (value.client_statement_uuid == std::array<std::uint8_t, 16>{} ||
      value.catalog_generation == 0 || value.security_epoch == 0 ||
      value.policy_generation == 0 || value.canonical_sblr_envelope.empty() ||
      value.canonical_sblr_envelope.size() > 0x00ffffffu) return {};
  std::vector<std::uint8_t> out;
  out.reserve(56 + value.canonical_sblr_envelope.size());
  out.insert(out.end(), {'S','P','R','E'});
  out.push_back(1); out.push_back(0);
  out.push_back(0); out.push_back(0);
  const std::uint32_t total = static_cast<std::uint32_t>(56 + value.canonical_sblr_envelope.size());
  for (int shift = 0; shift < 4; shift++) out.push_back(static_cast<std::uint8_t>(total >> (shift * 8)));
  out.insert(out.end(), value.client_statement_uuid.begin(), value.client_statement_uuid.end());
  for (const auto field : {value.catalog_generation, value.security_epoch, value.policy_generation})
    for (int shift = 0; shift < 8; shift++) out.push_back(static_cast<std::uint8_t>(field >> (shift * 8)));
  const auto n = static_cast<std::uint32_t>(value.canonical_sblr_envelope.size());
  for (int shift = 0; shift < 4; shift++) out.push_back(static_cast<std::uint8_t>(n >> (shift * 8)));
  out.insert(out.end(), value.canonical_sblr_envelope.begin(), value.canonical_sblr_envelope.end());
  return out;
}

inline bool DecodeSblrStmtPrepareDescriptorV1(
    const std::uint8_t* data, std::size_t size,
    SblrStmtPrepareDescriptorV1* out, std::string* detail) {
  if (!data || !out || size < 56 || data[0] != 'S' || data[1] != 'P' ||
      data[2] != 'R' || data[3] != 'E' || data[4] != 1) {
    if (detail) *detail = "stmt_prepare_descriptor_header_invalid";
    return false;
  }
  const auto u32 = [&](std::size_t at) { return static_cast<std::uint32_t>(data[at]) |
      (static_cast<std::uint32_t>(data[at+1]) << 8) |
      (static_cast<std::uint32_t>(data[at+2]) << 16) |
      (static_cast<std::uint32_t>(data[at+3]) << 24); };
  std::copy(data + 12, data + 28, out->client_statement_uuid.begin());
  if (u32(8) != size || u32(52) != size - 56 ||
      out->client_statement_uuid == std::array<std::uint8_t, 16>{}) {
    if (detail) *detail = "stmt_prepare_descriptor_extent_invalid";
    return false;
  }
  const auto u64 = [&](std::size_t at) { std::uint64_t v = 0; for (int i=0;i<8;i++) v |= static_cast<std::uint64_t>(data[at+i]) << (i*8); return v; };
  out->catalog_generation = u64(28); out->security_epoch = u64(36); out->policy_generation = u64(44);
  if (out->catalog_generation == 0 || out->security_epoch == 0 || out->policy_generation == 0) return false;
  out->canonical_sblr_envelope.assign(data + 56, data + size);
  return !out->canonical_sblr_envelope.empty();
}

} // namespace scratchbird::engine::sblr
