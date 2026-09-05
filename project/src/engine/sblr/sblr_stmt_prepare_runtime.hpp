#pragma once

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

using SblrStmtPrepareUuidV1 = std::array<std::uint8_t, 16>;
using SblrStmtPrepareSha256V1 = std::array<std::uint8_t, 32>;

struct SblrStmtPrepareWireLayoutV1 {
  static constexpr std::size_t request_size = 64;
  static constexpr std::size_t descriptor_prefix_size = 256;
  static constexpr std::size_t result_size = 160;
};

// SBPQ is the parser's authority-preserving request. It identifies the
// already authenticated statement receipt and the exact epoch cohort the
// parser observed; it never carries SQL text or client-created authority.
struct SblrStmtPrepareRequestV1 {
  SblrStmtPrepareUuidV1 statement_receipt_uuid{};
  std::uint64_t occurrence = 0;
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
};

// SBPD is the engine-coordinated operand copied by the parser into the sole
// ordinal-1 operand of SBLR_STMT_PREPARE. The canonical body is binary SBLR;
// source SQL and parser-authored object identities are not admitted.
struct SblrStmtPrepareDescriptorV1 {
  SblrStmtPrepareUuidV1 statement_uuid{};
  SblrStmtPrepareUuidV1 statement_name_uuid{};
  SblrStmtPrepareUuidV1 statement_receipt_uuid{};
  SblrStmtPrepareUuidV1 catalog_snapshot_uuid{};
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  SblrStmtPrepareUuidV1 mga_snapshot_uuid{};
  std::uint8_t statement_kind = 0;  // 1 query, 2 mutation, 3 management
  std::uint8_t parameter_mode = 0;  // 0 inferred, 1 declared
  std::uint8_t result_mode = 0;     // 0 none, 1 rowset
  SblrStmtPrepareSha256V1 canonical_sblr_sha256{};
  SblrStmtPrepareUuidV1 parameter_descriptor_uuid{};
  SblrStmtPrepareUuidV1 result_descriptor_uuid{};
  std::uint64_t prepared_generation = 0;
  std::uint64_t executor_availability_generation = 0;
  SblrStmtPrepareSha256V1 descriptor_sha256{};
  SblrStmtPrepareUuidV1 parser_package_uuid{};
  std::vector<std::uint8_t> canonical_sblr_bytes;
};

// SBPR is published only after the session-owned prepared descriptor is
// durable. Its evidence is over the complete fixed result with the evidence
// field zeroed.
struct SblrStmtPrepareResultV1 {
  SblrStmtPrepareUuidV1 statement_uuid{};
  std::uint64_t prepared_generation = 0;
  SblrStmtPrepareUuidV1 parameter_descriptor_uuid{};
  SblrStmtPrepareUuidV1 result_descriptor_uuid{};
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  SblrStmtPrepareUuidV1 mga_snapshot_uuid{};
  std::uint64_t executor_availability_generation = 0;
  std::uint8_t status = 1;
  std::uint8_t publication_barrier = 1;
  SblrStmtPrepareSha256V1 effect_evidence_sha256{};
};

namespace stmt_prepare_detail {

inline void AppendLe(std::vector<std::uint8_t>* output, std::uint64_t value,
                     std::size_t extent) {
  for (std::size_t index = 0; index < extent; ++index) {
    output->push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
  }
}

inline std::uint64_t ReadLe(const std::uint8_t* bytes, std::size_t extent) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < extent; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
  }
  return value;
}

template <std::size_t N>
inline bool NonZero(const std::array<std::uint8_t, N>& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

inline bool ZeroRange(const std::uint8_t* begin, const std::uint8_t* end) {
  return std::all_of(begin, end,
                     [](std::uint8_t byte) { return byte == 0; });
}

inline std::vector<std::uint8_t> Header(std::string_view magic,
                                        std::size_t header_bytes,
                                        std::size_t total_bytes) {
  std::vector<std::uint8_t> output(magic.begin(), magic.end());
  AppendLe(&output, 1, 2);
  AppendLe(&output, header_bytes, 2);
  AppendLe(&output, total_bytes, 4);
  AppendLe(&output, 0, 4);
  return output;
}

inline bool HeaderValid(const std::uint8_t* bytes, std::size_t size,
                        std::string_view magic, std::size_t header_bytes,
                        std::size_t total_bytes) {
  return bytes != nullptr && size == total_bytes && magic.size() == 4 &&
         std::equal(magic.begin(), magic.end(), bytes) &&
         ReadLe(bytes + 4, 2) == 1 &&
         ReadLe(bytes + 6, 2) == header_bytes &&
         ReadLe(bytes + 8, 4) == total_bytes &&
         ZeroRange(bytes + 12, bytes + 16);
}

inline SblrStmtPrepareSha256V1 Hash(const std::uint8_t* bytes,
                                   std::size_t size) {
  std::vector<std::uint8_t> material(bytes, bytes + size);
  return scratchbird::core::hash::ComputeSha256Digest(material).digest;
}

template <std::size_t N>
inline void Put(std::vector<std::uint8_t>* output,
                const std::array<std::uint8_t, N>& value) {
  output->insert(output->end(), value.begin(), value.end());
}

template <std::size_t N>
inline void Get(const std::uint8_t* bytes,
                std::array<std::uint8_t, N>* value) {
  std::copy_n(bytes, N, value->begin());
}

inline bool DescriptorSemanticValid(
    const SblrStmtPrepareDescriptorV1& value, std::string* detail) {
  // The descriptor has no parameter count. A zero UUID means the already
  // bound body has no parameters; otherwise it names the frozen descriptor.
  const bool parameter_identity_valid = value.parameter_mode <= 1;
  const bool result_identity_valid =
      (value.result_mode == 0 && !NonZero(value.result_descriptor_uuid)) ||
      (value.result_mode == 1 && NonZero(value.result_descriptor_uuid));
  const bool valid =
      NonZero(value.statement_uuid) && NonZero(value.statement_name_uuid) &&
      NonZero(value.statement_receipt_uuid) &&
      NonZero(value.catalog_snapshot_uuid) && value.catalog_generation != 0 &&
      value.security_epoch != 0 && value.resource_epoch != 0 &&
      NonZero(value.mga_snapshot_uuid) && value.statement_kind >= 1 &&
      value.statement_kind <= 3 && parameter_identity_valid &&
      value.result_mode <= 1 && result_identity_valid &&
      value.prepared_generation == 0 &&
      value.executor_availability_generation != 0 &&
      NonZero(value.parser_package_uuid) &&
      !value.canonical_sblr_bytes.empty() &&
      value.canonical_sblr_bytes.size() <=
          std::numeric_limits<std::uint32_t>::max();
  if (!valid && detail != nullptr) {
    *detail = "stmt_prepare_descriptor_semantic_fields_invalid";
  }
  return valid;
}

inline bool ResultSemanticValid(const SblrStmtPrepareResultV1& value,
                                std::string* detail) {
  const bool valid =
      NonZero(value.statement_uuid) && value.prepared_generation != 0 &&
      value.catalog_generation != 0 && value.security_epoch != 0 &&
      value.resource_epoch != 0 && NonZero(value.mga_snapshot_uuid) &&
      value.executor_availability_generation != 0 && value.status == 1 &&
      value.publication_barrier == 1;
  if (!valid && detail != nullptr) {
    *detail = "stmt_prepare_result_semantic_fields_invalid";
  }
  return valid;
}

}  // namespace stmt_prepare_detail

inline std::vector<std::uint8_t> EncodeSblrStmtPrepareRequestV1(
    const SblrStmtPrepareRequestV1& value) {
  using namespace stmt_prepare_detail;
  if (!NonZero(value.statement_receipt_uuid) || value.occurrence == 0 ||
      value.catalog_generation == 0 || value.security_epoch == 0 ||
      value.resource_epoch == 0) {
    return {};
  }
  auto output = Header("SBPQ", SblrStmtPrepareWireLayoutV1::request_size,
                       SblrStmtPrepareWireLayoutV1::request_size);
  Put(&output, value.statement_receipt_uuid);
  AppendLe(&output, value.occurrence, 8);
  AppendLe(&output, value.catalog_generation, 8);
  AppendLe(&output, value.security_epoch, 8);
  AppendLe(&output, value.resource_epoch, 8);
  return output;
}

inline bool DecodeSblrStmtPrepareRequestV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrStmtPrepareRequestV1* output, std::string* detail) {
  using namespace stmt_prepare_detail;
  if (output == nullptr ||
      !HeaderValid(bytes, size, "SBPQ",
                   SblrStmtPrepareWireLayoutV1::request_size,
                   SblrStmtPrepareWireLayoutV1::request_size)) {
    if (detail != nullptr) *detail = "stmt_prepare_request_header_invalid";
    return false;
  }
  SblrStmtPrepareRequestV1 value;
  Get(bytes + 16, &value.statement_receipt_uuid);
  value.occurrence = ReadLe(bytes + 32, 8);
  value.catalog_generation = ReadLe(bytes + 40, 8);
  value.security_epoch = ReadLe(bytes + 48, 8);
  value.resource_epoch = ReadLe(bytes + 56, 8);
  if (EncodeSblrStmtPrepareRequestV1(value).empty()) {
    if (detail != nullptr) *detail = "stmt_prepare_request_fields_invalid";
    return false;
  }
  *output = value;
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrStmtPrepareDescriptorV1(
    const SblrStmtPrepareDescriptorV1& value) {
  using namespace stmt_prepare_detail;
  if (!DescriptorSemanticValid(value, nullptr)) return {};
  const std::size_t total =
      SblrStmtPrepareWireLayoutV1::descriptor_prefix_size +
      value.canonical_sblr_bytes.size();
  if (total > std::numeric_limits<std::uint32_t>::max()) return {};

  auto output = Header("SBPD",
                       SblrStmtPrepareWireLayoutV1::descriptor_prefix_size,
                       total);
  Put(&output, value.statement_uuid);
  Put(&output, value.statement_name_uuid);
  Put(&output, value.statement_receipt_uuid);
  Put(&output, value.catalog_snapshot_uuid);
  AppendLe(&output, value.catalog_generation, 8);
  AppendLe(&output, value.security_epoch, 8);
  AppendLe(&output, value.resource_epoch, 8);
  Put(&output, value.mga_snapshot_uuid);
  output.push_back(value.statement_kind);
  output.push_back(value.parameter_mode);
  output.push_back(value.result_mode);
  output.push_back(0);
  AppendLe(&output, value.canonical_sblr_bytes.size(), 4);
  const auto body_hash =
      Hash(value.canonical_sblr_bytes.data(), value.canonical_sblr_bytes.size());
  if (NonZero(value.canonical_sblr_sha256) &&
      value.canonical_sblr_sha256 != body_hash) {
    return {};
  }
  Put(&output, body_hash);
  Put(&output, value.parameter_descriptor_uuid);
  Put(&output, value.result_descriptor_uuid);
  AppendLe(&output, value.prepared_generation, 8);
  AppendLe(&output, value.executor_availability_generation, 8);
  output.insert(output.end(), 32, 0);
  Put(&output, value.parser_package_uuid);
  output.insert(output.end(), value.canonical_sblr_bytes.begin(),
                value.canonical_sblr_bytes.end());
  const auto descriptor_hash = Hash(output.data(), output.size());
  if (NonZero(value.descriptor_sha256) &&
      value.descriptor_sha256 != descriptor_hash) {
    return {};
  }
  std::copy(descriptor_hash.begin(), descriptor_hash.end(),
            output.begin() + 208);
  return output;
}

inline bool DecodeSblrStmtPrepareDescriptorV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrStmtPrepareDescriptorV1* output, std::string* detail) {
  using namespace stmt_prepare_detail;
  if (output == nullptr || bytes == nullptr ||
      size < SblrStmtPrepareWireLayoutV1::descriptor_prefix_size ||
      !HeaderValid(bytes, size, "SBPD",
                   SblrStmtPrepareWireLayoutV1::descriptor_prefix_size,
                   size) || bytes[123] != 0) {
    if (detail != nullptr) *detail = "stmt_prepare_descriptor_header_invalid";
    return false;
  }
  const std::uint64_t body_size = ReadLe(bytes + 124, 4);
  if (body_size !=
      size - SblrStmtPrepareWireLayoutV1::descriptor_prefix_size) {
    if (detail != nullptr) *detail = "stmt_prepare_descriptor_extent_invalid";
    return false;
  }

  SblrStmtPrepareDescriptorV1 value;
  Get(bytes + 16, &value.statement_uuid);
  Get(bytes + 32, &value.statement_name_uuid);
  Get(bytes + 48, &value.statement_receipt_uuid);
  Get(bytes + 64, &value.catalog_snapshot_uuid);
  value.catalog_generation = ReadLe(bytes + 80, 8);
  value.security_epoch = ReadLe(bytes + 88, 8);
  value.resource_epoch = ReadLe(bytes + 96, 8);
  Get(bytes + 104, &value.mga_snapshot_uuid);
  value.statement_kind = bytes[120];
  value.parameter_mode = bytes[121];
  value.result_mode = bytes[122];
  Get(bytes + 128, &value.canonical_sblr_sha256);
  Get(bytes + 160, &value.parameter_descriptor_uuid);
  Get(bytes + 176, &value.result_descriptor_uuid);
  value.prepared_generation = ReadLe(bytes + 192, 8);
  value.executor_availability_generation = ReadLe(bytes + 200, 8);
  Get(bytes + 208, &value.descriptor_sha256);
  Get(bytes + 240, &value.parser_package_uuid);
  value.canonical_sblr_bytes.assign(
      bytes + SblrStmtPrepareWireLayoutV1::descriptor_prefix_size,
      bytes + size);

  if (!DescriptorSemanticValid(value, detail)) return false;
  const auto body_hash =
      Hash(value.canonical_sblr_bytes.data(), value.canonical_sblr_bytes.size());
  if (value.canonical_sblr_sha256 != body_hash ||
      !NonZero(value.descriptor_sha256)) {
    if (detail != nullptr) *detail = "stmt_prepare_descriptor_body_hash_invalid";
    return false;
  }
  std::vector<std::uint8_t> evidence_material(bytes, bytes + size);
  std::fill(evidence_material.begin() + 208,
            evidence_material.begin() + 240, 0);
  if (value.descriptor_sha256 !=
      Hash(evidence_material.data(), evidence_material.size())) {
    if (detail != nullptr) *detail = "stmt_prepare_descriptor_hash_invalid";
    return false;
  }
  *output = std::move(value);
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrStmtPrepareResultV1(
    const SblrStmtPrepareResultV1& value) {
  using namespace stmt_prepare_detail;
  if (!ResultSemanticValid(value, nullptr)) return {};
  auto output = Header("SBPR", SblrStmtPrepareWireLayoutV1::result_size,
                       SblrStmtPrepareWireLayoutV1::result_size);
  Put(&output, value.statement_uuid);
  AppendLe(&output, value.prepared_generation, 8);
  Put(&output, value.parameter_descriptor_uuid);
  Put(&output, value.result_descriptor_uuid);
  AppendLe(&output, value.catalog_generation, 8);
  AppendLe(&output, value.security_epoch, 8);
  AppendLe(&output, value.resource_epoch, 8);
  Put(&output, value.mga_snapshot_uuid);
  AppendLe(&output, value.executor_availability_generation, 8);
  output.push_back(value.status);
  output.push_back(value.publication_barrier);
  output.insert(output.end(), 2, 0);
  output.insert(output.end(), 32, 0);
  output.insert(output.end(), 4, 0);
  const auto evidence = Hash(output.data(), output.size());
  if (NonZero(value.effect_evidence_sha256) &&
      value.effect_evidence_sha256 != evidence) {
    return {};
  }
  std::copy(evidence.begin(), evidence.end(), output.begin() + 124);
  return output;
}

inline bool DecodeSblrStmtPrepareResultV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrStmtPrepareResultV1* output, std::string* detail) {
  using namespace stmt_prepare_detail;
  if (output == nullptr ||
      !HeaderValid(bytes, size, "SBPR",
                   SblrStmtPrepareWireLayoutV1::result_size,
                   SblrStmtPrepareWireLayoutV1::result_size) ||
      !ZeroRange(bytes + 122, bytes + 124) ||
      !ZeroRange(bytes + 156, bytes + 160)) {
    if (detail != nullptr) *detail = "stmt_prepare_result_header_invalid";
    return false;
  }
  SblrStmtPrepareResultV1 value;
  Get(bytes + 16, &value.statement_uuid);
  value.prepared_generation = ReadLe(bytes + 32, 8);
  Get(bytes + 40, &value.parameter_descriptor_uuid);
  Get(bytes + 56, &value.result_descriptor_uuid);
  value.catalog_generation = ReadLe(bytes + 72, 8);
  value.security_epoch = ReadLe(bytes + 80, 8);
  value.resource_epoch = ReadLe(bytes + 88, 8);
  Get(bytes + 96, &value.mga_snapshot_uuid);
  value.executor_availability_generation = ReadLe(bytes + 112, 8);
  value.status = bytes[120];
  value.publication_barrier = bytes[121];
  Get(bytes + 124, &value.effect_evidence_sha256);
  if (!ResultSemanticValid(value, detail) ||
      !NonZero(value.effect_evidence_sha256)) {
    return false;
  }
  std::vector<std::uint8_t> evidence_material(bytes, bytes + size);
  std::fill(evidence_material.begin() + 124,
            evidence_material.begin() + 156, 0);
  if (value.effect_evidence_sha256 !=
      Hash(evidence_material.data(), evidence_material.size())) {
    if (detail != nullptr) *detail = "stmt_prepare_result_evidence_invalid";
    return false;
  }
  *output = value;
  return true;
}

}  // namespace scratchbird::engine::sblr
