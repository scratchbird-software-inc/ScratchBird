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

inline constexpr std::uint16_t kSblrQueryExplainOpcodeCode = 4616;
inline constexpr std::size_t kSblrQueryExplainDescriptorPrefixBytes = 320;
inline constexpr std::size_t kSblrQueryExplainMaximumQueryBytes =
    64U * 1024U * 1024U;

using SblrQueryExplainUuidV1 = std::array<std::uint8_t, 16>;
using SblrQueryExplainSha256V1 = std::array<std::uint8_t, 32>;

struct SblrQueryExplainRequestV1 {
  SblrQueryExplainUuidV1 statement_receipt_uuid{};
  std::uint64_t occurrence = 0;
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
};

struct SblrQueryExplainDescriptorV1 {
  SblrQueryExplainUuidV1 explain_uuid{};
  SblrQueryExplainUuidV1 statement_receipt_uuid{};
  SblrQueryExplainUuidV1 query_snapshot_uuid{};
  SblrQueryExplainUuidV1 catalog_snapshot_uuid{};
  std::uint64_t catalog_generation = 0;
  SblrQueryExplainUuidV1 security_context_uuid{};
  SblrQueryExplainUuidV1 policy_snapshot_uuid{};
  std::uint64_t policy_generation = 0;
  SblrQueryExplainUuidV1 parameter_set_uuid{};
  std::uint64_t parameter_set_generation = 0;
  SblrQueryExplainUuidV1 plan_policy_uuid{};
  SblrQueryExplainUuidV1 resource_budget_uuid{};
  std::uint64_t resource_budget_generation = 0;
  SblrQueryExplainUuidV1 redaction_profile_uuid{};
  bool verbose = false;
  std::uint8_t format = 1;
  std::vector<std::uint8_t> canonical_query_sblr_bytes;
  SblrQueryExplainSha256V1 canonical_query_sblr_sha256{};
  std::uint64_t executor_availability_generation = 0;
  SblrQueryExplainSha256V1 descriptor_sha256{};
  SblrQueryExplainUuidV1 parser_package_uuid{};
  SblrQueryExplainUuidV1 language_profile_uuid{};
};

struct SblrQueryExplainResultV1 {
  SblrQueryExplainUuidV1 explain_uuid{};
  SblrQueryExplainUuidV1 plan_uuid{};
  SblrQueryExplainUuidV1 plan_descriptor_uuid{};
  std::uint64_t plan_descriptor_generation = 0;
  SblrQueryExplainUuidV1 query_snapshot_uuid{};
  SblrQueryExplainUuidV1 deterministic_snapshot_uuid{};
  std::uint64_t catalog_generation = 0;
  std::uint64_t policy_generation = 0;
  SblrQueryExplainUuidV1 redaction_profile_uuid{};
  std::uint8_t completion_state = 1;
  std::uint8_t plan_state = 1;
  SblrQueryExplainSha256V1 plan_material_sha256{};
  SblrQueryExplainSha256V1 executor_evidence_sha256{};
  SblrQueryExplainUuidV1 publication_barrier_uuid{};
  SblrQueryExplainUuidV1 result_evidence_uuid{};
};

namespace query_explain_detail {

inline void PutLe(std::vector<std::uint8_t>* out, std::uint64_t value,
                  std::size_t bytes) {
  for (std::size_t index = 0; index < bytes; ++index) {
    out->push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
  }
}

inline std::uint64_t GetLe(const std::uint8_t* in, std::size_t bytes) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < bytes; ++index) {
    value |= static_cast<std::uint64_t>(in[index]) << (index * 8U);
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
                                        std::size_t header_bytes,
                                        std::size_t total_bytes) {
  std::vector<std::uint8_t> out(magic.begin(), magic.end());
  PutLe(&out, 1, 2);
  PutLe(&out, header_bytes, 2);
  PutLe(&out, total_bytes, 4);
  PutLe(&out, 0, 4);
  return out;
}

inline bool HeaderValid(const std::uint8_t* in, std::size_t size,
                        std::string_view magic, std::size_t header_bytes,
                        bool fixed) {
  return in != nullptr && size >= header_bytes && magic.size() == 4 &&
         std::equal(magic.begin(), magic.end(), in) &&
         GetLe(in + 4, 2) == 1 && GetLe(in + 6, 2) == header_bytes &&
         GetLe(in + 8, 4) == size && Zero(in + 12, in + 16) &&
         (!fixed || size == header_bytes);
}

inline SblrQueryExplainSha256V1 Hash(const std::uint8_t* in,
                                     std::size_t size) {
  std::vector<std::uint8_t> bytes;
  if (size != 0) bytes.assign(in, in + size);
  return scratchbird::core::hash::ComputeSha256Digest(bytes).digest;
}

inline bool OptionalIdentityValid(const SblrQueryExplainUuidV1& uuid,
                                  std::uint64_t generation) {
  return NonZero(uuid) == (generation != 0);
}

inline bool DescriptorFieldsValid(const SblrQueryExplainDescriptorV1& value) {
  return NonZero(value.explain_uuid) &&
         NonZero(value.statement_receipt_uuid) &&
         NonZero(value.query_snapshot_uuid) &&
         NonZero(value.catalog_snapshot_uuid) &&
         value.catalog_generation != 0 &&
         NonZero(value.security_context_uuid) &&
         NonZero(value.policy_snapshot_uuid) && value.policy_generation != 0 &&
         OptionalIdentityValid(value.parameter_set_uuid,
                               value.parameter_set_generation) &&
         NonZero(value.plan_policy_uuid) &&
         NonZero(value.resource_budget_uuid) &&
         value.resource_budget_generation != 0 &&
         NonZero(value.redaction_profile_uuid) &&
         (value.format == 1 || value.format == 2) &&
         !value.canonical_query_sblr_bytes.empty() &&
         value.canonical_query_sblr_bytes.size() <=
             kSblrQueryExplainMaximumQueryBytes &&
         value.canonical_query_sblr_bytes.size() <=
             std::numeric_limits<std::uint32_t>::max() &&
         value.executor_availability_generation != 0 &&
         NonZero(value.parser_package_uuid) &&
         NonZero(value.language_profile_uuid);
}

inline bool ResultFieldsValid(const SblrQueryExplainResultV1& value) {
  return NonZero(value.explain_uuid) && NonZero(value.plan_uuid) &&
         NonZero(value.plan_descriptor_uuid) &&
         value.plan_descriptor_generation != 0 &&
         NonZero(value.query_snapshot_uuid) &&
         NonZero(value.deterministic_snapshot_uuid) &&
         value.catalog_generation != 0 && value.policy_generation != 0 &&
         NonZero(value.redaction_profile_uuid) &&
         value.completion_state == 1 && value.plan_state == 1 &&
         NonZero(value.plan_material_sha256) &&
         NonZero(value.executor_evidence_sha256) &&
         NonZero(value.publication_barrier_uuid) &&
         NonZero(value.result_evidence_uuid);
}

}  // namespace query_explain_detail

inline std::vector<std::uint8_t> EncodeSblrQueryExplainRequestV1(
    const SblrQueryExplainRequestV1& value) {
  using namespace query_explain_detail;
  if (!NonZero(value.statement_receipt_uuid) || value.occurrence == 0 ||
      value.catalog_generation == 0 || value.security_epoch == 0 ||
      value.resource_epoch == 0) {
    return {};
  }
  auto out = Header("SBEQ", 64, 64);
  Put(&out, value.statement_receipt_uuid);
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_epoch, 8);
  return out;
}

inline bool DecodeSblrQueryExplainRequestV1(
    const std::uint8_t* in, std::size_t size,
    SblrQueryExplainRequestV1* output, std::string* detail = nullptr) {
  using namespace query_explain_detail;
  if (output == nullptr || !HeaderValid(in, size, "SBEQ", 64, true)) {
    if (detail != nullptr) *detail = "query_explain_request_header_invalid";
    return false;
  }
  SblrQueryExplainRequestV1 value;
  Get(in + 16, &value.statement_receipt_uuid);
  value.occurrence = GetLe(in + 32, 8);
  value.catalog_generation = GetLe(in + 40, 8);
  value.security_epoch = GetLe(in + 48, 8);
  value.resource_epoch = GetLe(in + 56, 8);
  if (EncodeSblrQueryExplainRequestV1(value) !=
      std::vector<std::uint8_t>(in, in + size)) {
    if (detail != nullptr) *detail = "query_explain_request_fields_invalid";
    return false;
  }
  *output = value;
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrQueryExplainDescriptorV1(
    const SblrQueryExplainDescriptorV1& value) {
  using namespace query_explain_detail;
  if (!DescriptorFieldsValid(value)) return {};
  const auto query_hash = Hash(value.canonical_query_sblr_bytes.data(),
                               value.canonical_query_sblr_bytes.size());
  if (NonZero(value.canonical_query_sblr_sha256) &&
      value.canonical_query_sblr_sha256 != query_hash) {
    return {};
  }
  auto out = Header("SBXD", kSblrQueryExplainDescriptorPrefixBytes,
                    kSblrQueryExplainDescriptorPrefixBytes +
                        value.canonical_query_sblr_bytes.size());
  Put(&out, value.explain_uuid);
  Put(&out, value.statement_receipt_uuid);
  Put(&out, value.query_snapshot_uuid);
  Put(&out, value.catalog_snapshot_uuid);
  PutLe(&out, value.catalog_generation, 8);
  Put(&out, value.security_context_uuid);
  Put(&out, value.policy_snapshot_uuid);
  PutLe(&out, value.policy_generation, 8);
  Put(&out, value.parameter_set_uuid);
  PutLe(&out, value.parameter_set_generation, 8);
  Put(&out, value.plan_policy_uuid);
  Put(&out, value.resource_budget_uuid);
  PutLe(&out, value.resource_budget_generation, 8);
  Put(&out, value.redaction_profile_uuid);
  out.push_back(value.verbose ? 1 : 0);
  out.push_back(value.format);
  PutLe(&out, 0, 2);
  PutLe(&out, value.canonical_query_sblr_bytes.size(), 4);
  Put(&out, query_hash);
  PutLe(&out, value.executor_availability_generation, 8);
  out.insert(out.end(), 32, 0);
  Put(&out, value.parser_package_uuid);
  Put(&out, value.language_profile_uuid);
  out.insert(out.end(), value.canonical_query_sblr_bytes.begin(),
             value.canonical_query_sblr_bytes.end());
  const auto descriptor_hash = Hash(out.data(), out.size());
  if (NonZero(value.descriptor_sha256) &&
      value.descriptor_sha256 != descriptor_hash) {
    return {};
  }
  std::copy(descriptor_hash.begin(), descriptor_hash.end(), out.begin() + 256);
  return out;
}

inline bool DecodeSblrQueryExplainDescriptorV1(
    const std::uint8_t* in, std::size_t size,
    SblrQueryExplainDescriptorV1* output, std::string* detail = nullptr) {
  using namespace query_explain_detail;
  if (output == nullptr ||
      !HeaderValid(in, size, "SBXD", kSblrQueryExplainDescriptorPrefixBytes,
                   false) ||
      !Zero(in + 210, in + 212)) {
    if (detail != nullptr) *detail = "query_explain_descriptor_header_invalid";
    return false;
  }
  const auto query_size = GetLe(in + 212, 4);
  if (query_size == 0 || query_size > kSblrQueryExplainMaximumQueryBytes ||
      query_size != size - kSblrQueryExplainDescriptorPrefixBytes) {
    if (detail != nullptr) *detail = "query_explain_descriptor_extent_invalid";
    return false;
  }
  SblrQueryExplainDescriptorV1 value;
  Get(in + 16, &value.explain_uuid);
  Get(in + 32, &value.statement_receipt_uuid);
  Get(in + 48, &value.query_snapshot_uuid);
  Get(in + 64, &value.catalog_snapshot_uuid);
  value.catalog_generation = GetLe(in + 80, 8);
  Get(in + 88, &value.security_context_uuid);
  Get(in + 104, &value.policy_snapshot_uuid);
  value.policy_generation = GetLe(in + 120, 8);
  Get(in + 128, &value.parameter_set_uuid);
  value.parameter_set_generation = GetLe(in + 144, 8);
  Get(in + 152, &value.plan_policy_uuid);
  Get(in + 168, &value.resource_budget_uuid);
  value.resource_budget_generation = GetLe(in + 184, 8);
  Get(in + 192, &value.redaction_profile_uuid);
  if (in[208] > 1) {
    if (detail != nullptr) *detail = "query_explain_descriptor_options_invalid";
    return false;
  }
  value.verbose = in[208] != 0;
  value.format = in[209];
  Get(in + 216, &value.canonical_query_sblr_sha256);
  value.executor_availability_generation = GetLe(in + 248, 8);
  Get(in + 256, &value.descriptor_sha256);
  Get(in + 288, &value.parser_package_uuid);
  Get(in + 304, &value.language_profile_uuid);
  value.canonical_query_sblr_bytes.assign(
      in + kSblrQueryExplainDescriptorPrefixBytes, in + size);
  if (!DescriptorFieldsValid(value) ||
      value.canonical_query_sblr_sha256 !=
          Hash(value.canonical_query_sblr_bytes.data(),
               value.canonical_query_sblr_bytes.size()) ||
      !NonZero(value.descriptor_sha256)) {
    if (detail != nullptr) *detail = "query_explain_descriptor_fields_invalid";
    return false;
  }
  std::vector<std::uint8_t> material(in, in + size);
  std::fill(material.begin() + 256, material.begin() + 288, 0);
  if (value.descriptor_sha256 != Hash(material.data(), material.size())) {
    if (detail != nullptr) *detail = "query_explain_descriptor_hash_invalid";
    return false;
  }
  *output = std::move(value);
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrQueryExplainResultV1(
    const SblrQueryExplainResultV1& value) {
  using namespace query_explain_detail;
  if (!ResultFieldsValid(value)) return {};
  auto out = Header("SBXR", 256, 256);
  Put(&out, value.explain_uuid);
  Put(&out, value.plan_uuid);
  Put(&out, value.plan_descriptor_uuid);
  PutLe(&out, value.plan_descriptor_generation, 8);
  Put(&out, value.query_snapshot_uuid);
  Put(&out, value.deterministic_snapshot_uuid);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.policy_generation, 8);
  Put(&out, value.redaction_profile_uuid);
  out.push_back(value.completion_state);
  out.push_back(value.plan_state);
  PutLe(&out, 0, 2);
  Put(&out, value.plan_material_sha256);
  Put(&out, value.executor_evidence_sha256);
  Put(&out, value.publication_barrier_uuid);
  Put(&out, value.result_evidence_uuid);
  out.insert(out.end(), 20, 0);
  return out;
}

inline bool DecodeSblrQueryExplainResultV1(
    const std::uint8_t* in, std::size_t size,
    SblrQueryExplainResultV1* output, std::string* detail = nullptr) {
  using namespace query_explain_detail;
  if (output == nullptr || !HeaderValid(in, size, "SBXR", 256, true) ||
      !Zero(in + 138, in + 140) || !Zero(in + 236, in + 256)) {
    if (detail != nullptr) *detail = "query_explain_result_header_invalid";
    return false;
  }
  SblrQueryExplainResultV1 value;
  Get(in + 16, &value.explain_uuid);
  Get(in + 32, &value.plan_uuid);
  Get(in + 48, &value.plan_descriptor_uuid);
  value.plan_descriptor_generation = GetLe(in + 64, 8);
  Get(in + 72, &value.query_snapshot_uuid);
  Get(in + 88, &value.deterministic_snapshot_uuid);
  value.catalog_generation = GetLe(in + 104, 8);
  value.policy_generation = GetLe(in + 112, 8);
  Get(in + 120, &value.redaction_profile_uuid);
  value.completion_state = in[136];
  value.plan_state = in[137];
  Get(in + 140, &value.plan_material_sha256);
  Get(in + 172, &value.executor_evidence_sha256);
  Get(in + 204, &value.publication_barrier_uuid);
  Get(in + 220, &value.result_evidence_uuid);
  if (!ResultFieldsValid(value)) {
    if (detail != nullptr) *detail = "query_explain_result_fields_invalid";
    return false;
  }
  *output = value;
  return true;
}

}  // namespace scratchbird::engine::sblr
