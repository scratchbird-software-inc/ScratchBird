#pragma once

#include "engine/sblr/sblr_stmt_execute_direct_runtime.hpp"

#include "core/hash/hash_digest.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

inline constexpr std::uint16_t kSblrStmtExecuteOpcodeCode = 4609;

using SblrStmtExecuteUuidV1 = std::array<std::uint8_t, 16>;
using SblrStmtExecuteSha256V1 = std::array<std::uint8_t, 32>;

// Authenticated syntax-only request. The statement name is resolved only
// against the prepared-statement registry owned by the live engine session.
struct SblrStmtExecuteRequestV1 {
  SblrStmtExecuteUuidV1 statement_receipt_uuid{};
  std::uint64_t occurrence = 0;
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::string statement_name;
  bool quoted = false;
  std::vector<std::uint8_t> canonical_parameter_bytes;
};

// Exact engine-issued operand for SBLR_STMT_EXECUTE. The prepared body is
// retained behind the session-owned statement identity and is deliberately
// absent from this parser-visible carrier.
struct SblrStmtExecuteDescriptorV1 {
  SblrStmtExecuteUuidV1 execution_uuid{};
  SblrStmtExecuteUuidV1 statement_uuid{};
  SblrStmtExecuteUuidV1 statement_name_uuid{};
  SblrStmtExecuteUuidV1 statement_receipt_uuid{};
  SblrStmtExecuteUuidV1 catalog_snapshot_uuid{};
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  SblrStmtExecuteUuidV1 mga_snapshot_uuid{};
  std::uint64_t prepared_generation = 0;
  SblrStmtExecuteSha256V1 prepared_descriptor_sha256{};
  SblrStmtExecuteUuidV1 parameter_set_uuid{};
  std::uint64_t parameter_set_generation = 0;
  SblrStmtExecuteUuidV1 result_descriptor_uuid{};
  SblrStmtExecuteUuidV1 parser_package_uuid{};
  SblrStmtExecuteSha256V1 canonical_parameter_sha256{};
  std::uint64_t executor_availability_generation = 0;
  SblrStmtExecuteSha256V1 descriptor_sha256{};
  std::vector<std::uint8_t> canonical_parameter_bytes;
};

// Both prepared and direct execution publish the same stmt_execute_result.v1
// carrier. The operation identity in the enclosing SBOP distinguishes them.
using SblrStmtExecuteResultV1 = SblrStmtExecuteDirectResultV1;

namespace stmt_execute_detail {

inline void PutLe(std::vector<std::uint8_t>* out, std::uint64_t value,
                  std::size_t bytes) {
  for (std::size_t i = 0; i < bytes; ++i) {
    out->push_back(static_cast<std::uint8_t>(value >> (i * 8U)));
  }
}

inline std::uint64_t GetLe(const std::uint8_t* in, std::size_t bytes) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < bytes; ++i) {
    value |= static_cast<std::uint64_t>(in[i]) << (i * 8U);
  }
  return value;
}

template <std::size_t N>
inline bool NonZero(const std::array<std::uint8_t, N>& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

template <std::size_t N>
inline void Put(std::vector<std::uint8_t>* out,
                const std::array<std::uint8_t, N>& value) {
  out->insert(out->end(), value.begin(), value.end());
}

template <std::size_t N>
inline void Get(const std::uint8_t* in, std::array<std::uint8_t, N>* value) {
  std::copy_n(in, N, value->begin());
}

inline bool Zero(const std::uint8_t* begin, const std::uint8_t* end) {
  return std::all_of(begin, end,
                     [](std::uint8_t byte) { return byte == 0; });
}

inline std::vector<std::uint8_t> Header(std::string_view magic,
                                        std::size_t header,
                                        std::size_t total) {
  std::vector<std::uint8_t> out(magic.begin(), magic.end());
  PutLe(&out, 1, 2);
  PutLe(&out, header, 2);
  PutLe(&out, total, 4);
  PutLe(&out, 0, 4);
  return out;
}

inline bool HeaderValid(const std::uint8_t* in, std::size_t size,
                        std::string_view magic, std::size_t header,
                        std::size_t total) {
  return in != nullptr && size == total && magic.size() == 4 &&
         std::equal(magic.begin(), magic.end(), in) &&
         GetLe(in + 4, 2) == 1 && GetLe(in + 6, 2) == header &&
         GetLe(in + 8, 4) == total && Zero(in + 12, in + 16);
}

inline SblrStmtExecuteSha256V1 Hash(const std::uint8_t* in,
                                   std::size_t size) {
  std::vector<std::uint8_t> bytes;
  if (size != 0) bytes.assign(in, in + size);
  return scratchbird::core::hash::ComputeSha256Digest(bytes).digest;
}

inline bool ValidUtf8(std::string_view text) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
  std::size_t i = 0;
  while (i < text.size()) {
    const auto c = bytes[i++];
    if (c == 0) return false;
    if (c < 0x80) continue;
    std::uint32_t value = 0;
    std::size_t continuation = 0;
    if ((c & 0xe0) == 0xc0) {
      value = c & 0x1f;
      continuation = 1;
      if (value < 2) return false;
    } else if ((c & 0xf0) == 0xe0) {
      value = c & 0x0f;
      continuation = 2;
    } else if ((c & 0xf8) == 0xf0) {
      value = c & 0x07;
      continuation = 3;
    } else {
      return false;
    }
    if (i + continuation > text.size()) return false;
    for (std::size_t j = 0; j < continuation; ++j) {
      const auto next = bytes[i++];
      if ((next & 0xc0) != 0x80) return false;
      value = (value << 6) | (next & 0x3f);
    }
    if ((continuation == 2 && value < 0x800) ||
        (continuation == 3 && value < 0x10000) || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff)) {
      return false;
    }
  }
  return true;
}

inline bool OptionalPairValid(const SblrStmtExecuteUuidV1& uuid,
                              std::uint64_t generation) {
  return NonZero(uuid) == (generation != 0);
}

inline bool DescriptorFieldsValid(const SblrStmtExecuteDescriptorV1& value) {
  const auto parameter_hash =
      Hash(value.canonical_parameter_bytes.data(),
           value.canonical_parameter_bytes.size());
  return NonZero(value.execution_uuid) && NonZero(value.statement_uuid) &&
         NonZero(value.statement_name_uuid) &&
         NonZero(value.statement_receipt_uuid) &&
         NonZero(value.catalog_snapshot_uuid) &&
         value.catalog_generation != 0 && value.security_epoch != 0 &&
         value.resource_epoch != 0 && NonZero(value.mga_snapshot_uuid) &&
         value.prepared_generation != 0 &&
         NonZero(value.prepared_descriptor_sha256) &&
         OptionalPairValid(value.parameter_set_uuid,
                           value.parameter_set_generation) &&
         NonZero(value.parser_package_uuid) &&
         value.executor_availability_generation != 0 &&
         value.canonical_parameter_bytes.size() <=
             std::numeric_limits<std::uint32_t>::max() &&
         (!NonZero(value.canonical_parameter_sha256) ||
          value.canonical_parameter_sha256 == parameter_hash);
}

}  // namespace stmt_execute_detail

inline std::vector<std::uint8_t> EncodeSblrStmtExecuteRequestV1(
    const SblrStmtExecuteRequestV1& value) {
  using namespace stmt_execute_detail;
  if (!NonZero(value.statement_receipt_uuid) || value.occurrence == 0 ||
      value.catalog_generation == 0 || value.security_epoch == 0 ||
      value.resource_epoch == 0 || value.statement_name.empty() ||
      value.statement_name.size() > 256 || !ValidUtf8(value.statement_name)) {
    return {};
  }
  if (value.canonical_parameter_bytes.size() > 65536) return {};
  const std::size_t total = 112 + value.statement_name.size() +
                            value.canonical_parameter_bytes.size();
  auto out = Header("SBXQ", 112, total);
  Put(&out, value.statement_receipt_uuid);
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_epoch, 8);
  PutLe(&out, value.statement_name.size(), 2);
  out.push_back(value.quoted ? 1 : 0);
  out.insert(out.end(), 5, 0);
  PutLe(&out, value.canonical_parameter_bytes.size(), 4);
  PutLe(&out, 0, 4);
  Put(&out, Hash(value.canonical_parameter_bytes.data(),
                 value.canonical_parameter_bytes.size()));
  out.insert(out.end(), value.statement_name.begin(), value.statement_name.end());
  out.insert(out.end(), value.canonical_parameter_bytes.begin(),
             value.canonical_parameter_bytes.end());
  return out;
}

inline bool DecodeSblrStmtExecuteRequestV1(
    const std::uint8_t* in, std::size_t size, SblrStmtExecuteRequestV1* output,
    std::string* detail = nullptr) {
  using namespace stmt_execute_detail;
  if (output == nullptr || in == nullptr || size < 113 ||
      !HeaderValid(in, size, "SBXQ", 112, size) ||
      !Zero(in + 67, in + 72) || !Zero(in + 76, in + 80)) {
    if (detail != nullptr) *detail = "stmt_execute_request_header_invalid";
    return false;
  }
  const auto name_size = GetLe(in + 64, 2);
  const auto parameter_size = GetLe(in + 72, 4);
  if (name_size == 0 || name_size > 256 || parameter_size > 65536 ||
      size != 112 + name_size + parameter_size ||
      in[66] > 1) {
    if (detail != nullptr) *detail = "stmt_execute_request_extent_invalid";
    return false;
  }
  SblrStmtExecuteRequestV1 value;
  Get(in + 16, &value.statement_receipt_uuid);
  value.occurrence = GetLe(in + 32, 8);
  value.catalog_generation = GetLe(in + 40, 8);
  value.security_epoch = GetLe(in + 48, 8);
  value.resource_epoch = GetLe(in + 56, 8);
  value.quoted = in[66] == 1;
  value.statement_name.assign(reinterpret_cast<const char*>(in + 112),
                              name_size);
  value.canonical_parameter_bytes.assign(in + 112 + name_size, in + size);
  const auto parameter_hash =
      Hash(value.canonical_parameter_bytes.data(),
           value.canonical_parameter_bytes.size());
  if (!std::equal(parameter_hash.begin(), parameter_hash.end(), in + 80)) {
    if (detail != nullptr) *detail = "stmt_execute_request_parameter_hash_invalid";
    return false;
  }
  if (EncodeSblrStmtExecuteRequestV1(value).empty()) {
    if (detail != nullptr) *detail = "stmt_execute_request_fields_invalid";
    return false;
  }
  *output = std::move(value);
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrStmtExecuteDescriptorV1(
    const SblrStmtExecuteDescriptorV1& value) {
  using namespace stmt_execute_detail;
  if (!DescriptorFieldsValid(value)) return {};
  const std::size_t total = 320 + value.canonical_parameter_bytes.size();
  if (total > std::numeric_limits<std::uint32_t>::max()) return {};
  auto out = Header("SBXD", 320, total);
  Put(&out, value.execution_uuid);
  Put(&out, value.statement_uuid);
  Put(&out, value.statement_name_uuid);
  Put(&out, value.statement_receipt_uuid);
  Put(&out, value.catalog_snapshot_uuid);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_epoch, 8);
  Put(&out, value.mga_snapshot_uuid);
  PutLe(&out, value.prepared_generation, 8);
  Put(&out, value.prepared_descriptor_sha256);
  Put(&out, value.parameter_set_uuid);
  PutLe(&out, value.parameter_set_generation, 8);
  Put(&out, value.result_descriptor_uuid);
  Put(&out, value.parser_package_uuid);
  PutLe(&out, value.canonical_parameter_bytes.size(), 4);
  PutLe(&out, 0, 4);
  const auto parameter_hash =
      Hash(value.canonical_parameter_bytes.data(),
           value.canonical_parameter_bytes.size());
  Put(&out, parameter_hash);
  PutLe(&out, value.executor_availability_generation, 8);
  out.insert(out.end(), 32, 0);
  out.insert(out.end(), 8, 0);
  out.insert(out.end(), value.canonical_parameter_bytes.begin(),
             value.canonical_parameter_bytes.end());
  auto evidence_material = out;
  const auto descriptor_hash = Hash(evidence_material.data(),
                                    evidence_material.size());
  if (NonZero(value.descriptor_sha256) &&
      value.descriptor_sha256 != descriptor_hash) {
    return {};
  }
  std::copy(descriptor_hash.begin(), descriptor_hash.end(), out.begin() + 280);
  return out;
}

inline bool DecodeSblrStmtExecuteDescriptorV1(
    const std::uint8_t* in, std::size_t size,
    SblrStmtExecuteDescriptorV1* output, std::string* detail = nullptr) {
  using namespace stmt_execute_detail;
  if (output == nullptr || in == nullptr || size < 320 ||
      !HeaderValid(in, size, "SBXD", 320, size) ||
      !Zero(in + 236, in + 240) || !Zero(in + 312, in + 320)) {
    if (detail != nullptr) *detail = "stmt_execute_descriptor_header_invalid";
    return false;
  }
  const auto parameter_size = GetLe(in + 232, 4);
  if (size != 320 + parameter_size) {
    if (detail != nullptr) *detail = "stmt_execute_descriptor_extent_invalid";
    return false;
  }
  SblrStmtExecuteDescriptorV1 value;
  Get(in + 16, &value.execution_uuid);
  Get(in + 32, &value.statement_uuid);
  Get(in + 48, &value.statement_name_uuid);
  Get(in + 64, &value.statement_receipt_uuid);
  Get(in + 80, &value.catalog_snapshot_uuid);
  value.catalog_generation = GetLe(in + 96, 8);
  value.security_epoch = GetLe(in + 104, 8);
  value.resource_epoch = GetLe(in + 112, 8);
  Get(in + 120, &value.mga_snapshot_uuid);
  value.prepared_generation = GetLe(in + 136, 8);
  Get(in + 144, &value.prepared_descriptor_sha256);
  Get(in + 176, &value.parameter_set_uuid);
  value.parameter_set_generation = GetLe(in + 192, 8);
  Get(in + 200, &value.result_descriptor_uuid);
  Get(in + 216, &value.parser_package_uuid);
  Get(in + 240, &value.canonical_parameter_sha256);
  value.executor_availability_generation = GetLe(in + 272, 8);
  Get(in + 280, &value.descriptor_sha256);
  value.canonical_parameter_bytes.assign(in + 320, in + size);
  if (!DescriptorFieldsValid(value) ||
      value.canonical_parameter_sha256 !=
          Hash(value.canonical_parameter_bytes.data(),
               value.canonical_parameter_bytes.size()) ||
      !NonZero(value.descriptor_sha256)) {
    if (detail != nullptr) *detail = "stmt_execute_descriptor_fields_invalid";
    return false;
  }
  std::vector<std::uint8_t> material(in, in + size);
  std::fill(material.begin() + 280, material.begin() + 312, 0);
  if (value.descriptor_sha256 != Hash(material.data(), material.size())) {
    if (detail != nullptr) *detail = "stmt_execute_descriptor_evidence_invalid";
    return false;
  }
  *output = std::move(value);
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrStmtExecuteResultV1(
    const SblrStmtExecuteResultV1& value) {
  return EncodeSblrStmtExecuteDirectResultV1(value);
}

inline bool DecodeSblrStmtExecuteResultV1(
    const std::uint8_t* in, std::size_t size, SblrStmtExecuteResultV1* output,
    std::string* detail = nullptr) {
  return DecodeSblrStmtExecuteDirectResultV1(in, size, output, detail);
}

}  // namespace scratchbird::engine::sblr
