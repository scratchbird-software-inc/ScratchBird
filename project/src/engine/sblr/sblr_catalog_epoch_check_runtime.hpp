// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/hash/hash_digest.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

inline constexpr std::uint16_t kSblrCatalogEpochCheckOpcodeCode = 4869;
inline constexpr std::size_t kSblrCatalogEpochCheckRequestBytes = 64;
inline constexpr std::size_t kSblrCatalogEpochCheckDescriptorBytes = 256;
inline constexpr std::size_t kSblrCatalogEpochCheckResultBytes = 192;
inline constexpr std::uint32_t kSblrCatalogEpochCheckObjectScopedFlag = 1U;

using SblrCatalogEpochCheckUuidV1 = std::array<std::uint8_t, 16>;
using SblrCatalogEpochCheckSha256V1 = std::array<std::uint8_t, 32>;

struct SblrCatalogEpochCheckRequestV1 {
  SblrCatalogEpochCheckUuidV1 statement_receipt_uuid{};
  std::uint64_t occurrence = 0;
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  bool object_scoped = false;
};

struct SblrCatalogEpochCheckDescriptorV1 {
  bool object_scoped = false;
  SblrCatalogEpochCheckUuidV1 check_uuid{};
  SblrCatalogEpochCheckUuidV1 statement_receipt_uuid{};
  SblrCatalogEpochCheckUuidV1 requested_catalog_epoch_uuid{};
  std::uint64_t requested_catalog_generation = 0;
  SblrCatalogEpochCheckUuidV1 database_uuid{};
  SblrCatalogEpochCheckUuidV1 schema_tree_uuid{};
  std::uint64_t schema_tree_generation = 0;
  SblrCatalogEpochCheckUuidV1 security_context_uuid{};
  SblrCatalogEpochCheckUuidV1 policy_snapshot_uuid{};
  std::uint64_t policy_generation = 0;
  SblrCatalogEpochCheckUuidV1 catalog_snapshot_uuid{};
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t executor_availability_generation = 0;
  SblrCatalogEpochCheckSha256V1 descriptor_sha256{};
  SblrCatalogEpochCheckSha256V1 visibility_scope_sha256{};
};

struct SblrCatalogEpochCheckResultV1 {
  bool object_scoped = false;
  SblrCatalogEpochCheckUuidV1 check_uuid{};
  SblrCatalogEpochCheckUuidV1 observed_catalog_epoch_uuid{};
  std::uint64_t observed_catalog_generation = 0;
  SblrCatalogEpochCheckUuidV1 database_uuid{};
  SblrCatalogEpochCheckUuidV1 schema_tree_uuid{};
  std::uint64_t schema_tree_generation = 0;
  std::uint8_t status = 0;      // 1 current, 2 stale, 3 hidden.
  std::uint8_t visibility = 0;  // 1 visible, 2 redacted.
  std::uint64_t observed_security_epoch = 0;
  std::uint64_t observed_resource_epoch = 0;
  SblrCatalogEpochCheckUuidV1 redaction_profile_uuid{};
  SblrCatalogEpochCheckUuidV1 publication_evidence_uuid{};
  SblrCatalogEpochCheckSha256V1 result_material_sha256{};
  std::uint64_t executor_availability_generation = 0;
};

namespace catalog_epoch_check_detail {

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

inline SblrCatalogEpochCheckSha256V1 Hash(
    std::string_view domain, const std::vector<std::uint8_t>& bytes) {
  std::vector<std::uint8_t> material(domain.begin(), domain.end());
  material.insert(material.end(), bytes.begin(), bytes.end());
  return scratchbird::core::hash::ComputeSha256Digest(material).digest;
}

inline std::vector<std::uint8_t> Header(std::string_view magic,
                                        std::size_t bytes,
                                        std::uint32_t flags) {
  std::vector<std::uint8_t> out(magic.begin(), magic.end());
  PutLe(&out, 1, 2);
  PutLe(&out, bytes, 2);
  PutLe(&out, bytes, 4);
  PutLe(&out, flags, 4);
  return out;
}

inline bool HeaderValid(const std::uint8_t* in, std::size_t size,
                        std::string_view magic, std::size_t bytes,
                        std::uint32_t* flags) {
  if (in == nullptr || size != bytes || magic.size() != 4 ||
      !std::equal(magic.begin(), magic.end(), in) ||
      GetLe(in + 4, 2) != 1 || GetLe(in + 6, 2) != bytes ||
      GetLe(in + 8, 4) != bytes) {
    return false;
  }
  const auto decoded_flags = static_cast<std::uint32_t>(GetLe(in + 12, 4));
  if ((decoded_flags & ~kSblrCatalogEpochCheckObjectScopedFlag) != 0) {
    return false;
  }
  if (flags != nullptr) *flags = decoded_flags;
  return true;
}

inline bool DescriptorFieldsValid(
    const SblrCatalogEpochCheckDescriptorV1& value) {
  return NonZero(value.check_uuid) &&
         NonZero(value.statement_receipt_uuid) &&
         NonZero(value.requested_catalog_epoch_uuid) &&
         value.requested_catalog_generation != 0 &&
         NonZero(value.database_uuid) && NonZero(value.schema_tree_uuid) &&
         value.schema_tree_generation != 0 &&
         NonZero(value.security_context_uuid) &&
         NonZero(value.policy_snapshot_uuid) && value.policy_generation != 0 &&
         NonZero(value.catalog_snapshot_uuid) && value.security_epoch != 0 &&
         value.resource_epoch != 0 &&
         value.executor_availability_generation != 0 &&
         NonZero(value.visibility_scope_sha256);
}

inline bool ResultFieldsValid(const SblrCatalogEpochCheckResultV1& value) {
  if (!NonZero(value.check_uuid) || value.observed_security_epoch == 0 ||
      value.observed_resource_epoch == 0 ||
      !NonZero(value.redaction_profile_uuid) ||
      !NonZero(value.publication_evidence_uuid) ||
      value.executor_availability_generation == 0) {
    return false;
  }
  if ((value.status == 1 || value.status == 2) && value.visibility == 1) {
    return NonZero(value.observed_catalog_epoch_uuid) &&
           value.observed_catalog_generation != 0 &&
           NonZero(value.database_uuid) && NonZero(value.schema_tree_uuid) &&
           value.schema_tree_generation != 0;
  }
  if (value.status == 3 && value.visibility == 2) {
    return !NonZero(value.observed_catalog_epoch_uuid) &&
           value.observed_catalog_generation == 0 &&
           !NonZero(value.database_uuid) && !NonZero(value.schema_tree_uuid) &&
           value.schema_tree_generation == 0;
  }
  return false;
}

}  // namespace catalog_epoch_check_detail

inline SblrCatalogEpochCheckSha256V1
SblrCatalogEpochCheckVisibilityScopeSha256V1(
    bool object_scoped, const SblrCatalogEpochCheckUuidV1& database_uuid,
    const SblrCatalogEpochCheckUuidV1& schema_tree_uuid,
    std::uint64_t schema_tree_generation,
    const SblrCatalogEpochCheckUuidV1& object_uuid = {},
    std::uint64_t object_generation = 0) {
  using namespace catalog_epoch_check_detail;
  if (!NonZero(database_uuid) || !NonZero(schema_tree_uuid) ||
      schema_tree_generation == 0 ||
      (object_scoped && (!NonZero(object_uuid) || object_generation == 0)) ||
      (!object_scoped && (NonZero(object_uuid) || object_generation != 0))) {
    return {};
  }
  std::vector<std::uint8_t> material;
  material.push_back(object_scoped ? 1 : 0);
  Put(&material, database_uuid);
  Put(&material, schema_tree_uuid);
  PutLe(&material, schema_tree_generation, 8);
  Put(&material, object_uuid);
  PutLe(&material, object_generation, 8);
  return Hash("ScratchBird.CatalogEpochVisibilityScope.V1", material);
}

inline std::vector<std::uint8_t> EncodeSblrCatalogEpochCheckRequestV1(
    const SblrCatalogEpochCheckRequestV1& value) {
  using namespace catalog_epoch_check_detail;
  if (!NonZero(value.statement_receipt_uuid) || value.occurrence == 0 ||
      value.catalog_generation == 0 || value.security_epoch == 0 ||
      value.resource_epoch == 0) {
    return {};
  }
  auto out = Header("SBCQ", kSblrCatalogEpochCheckRequestBytes,
                    value.object_scoped
                        ? kSblrCatalogEpochCheckObjectScopedFlag
                        : 0);
  Put(&out, value.statement_receipt_uuid);
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_epoch, 8);
  return out;
}

inline bool DecodeSblrCatalogEpochCheckRequestV1(
    const std::uint8_t* in, std::size_t size,
    SblrCatalogEpochCheckRequestV1* output, std::string* detail = nullptr) {
  using namespace catalog_epoch_check_detail;
  std::uint32_t flags = 0;
  if (output == nullptr ||
      !HeaderValid(in, size, "SBCQ", kSblrCatalogEpochCheckRequestBytes,
                   &flags)) {
    if (detail != nullptr) *detail = "catalog_epoch_check_request_header_invalid";
    return false;
  }
  SblrCatalogEpochCheckRequestV1 value;
  value.object_scoped =
      (flags & kSblrCatalogEpochCheckObjectScopedFlag) != 0;
  Get(in + 16, &value.statement_receipt_uuid);
  value.occurrence = GetLe(in + 32, 8);
  value.catalog_generation = GetLe(in + 40, 8);
  value.security_epoch = GetLe(in + 48, 8);
  value.resource_epoch = GetLe(in + 56, 8);
  if (EncodeSblrCatalogEpochCheckRequestV1(value) !=
      std::vector<std::uint8_t>(in, in + size)) {
    if (detail != nullptr) *detail = "catalog_epoch_check_request_fields_invalid";
    return false;
  }
  *output = value;
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrCatalogEpochCheckDescriptorV1(
    const SblrCatalogEpochCheckDescriptorV1& value) {
  using namespace catalog_epoch_check_detail;
  if (!DescriptorFieldsValid(value)) return {};
  auto out = Header("SECD", kSblrCatalogEpochCheckDescriptorBytes,
                    value.object_scoped
                        ? kSblrCatalogEpochCheckObjectScopedFlag
                        : 0);
  Put(&out, value.check_uuid);
  Put(&out, value.statement_receipt_uuid);
  Put(&out, value.requested_catalog_epoch_uuid);
  PutLe(&out, value.requested_catalog_generation, 8);
  Put(&out, value.database_uuid);
  Put(&out, value.schema_tree_uuid);
  PutLe(&out, value.schema_tree_generation, 8);
  Put(&out, value.security_context_uuid);
  Put(&out, value.policy_snapshot_uuid);
  PutLe(&out, value.policy_generation, 8);
  Put(&out, value.catalog_snapshot_uuid);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_epoch, 8);
  PutLe(&out, value.executor_availability_generation, 8);
  out.insert(out.end(), 32, 0);
  Put(&out, value.visibility_scope_sha256);
  const auto digest = Hash("ScratchBird.SblrCatalogEpochCheckDescriptor.V1", out);
  if (NonZero(value.descriptor_sha256) && value.descriptor_sha256 != digest) {
    return {};
  }
  std::copy(digest.begin(), digest.end(), out.begin() + 192);
  return out;
}

inline bool DecodeSblrCatalogEpochCheckDescriptorV1(
    const std::uint8_t* in, std::size_t size,
    SblrCatalogEpochCheckDescriptorV1* output,
    std::string* detail = nullptr) {
  using namespace catalog_epoch_check_detail;
  std::uint32_t flags = 0;
  if (output == nullptr ||
      !HeaderValid(in, size, "SECD", kSblrCatalogEpochCheckDescriptorBytes,
                   &flags)) {
    if (detail != nullptr) *detail = "catalog_epoch_check_descriptor_header_invalid";
    return false;
  }
  SblrCatalogEpochCheckDescriptorV1 value;
  value.object_scoped =
      (flags & kSblrCatalogEpochCheckObjectScopedFlag) != 0;
  Get(in + 16, &value.check_uuid);
  Get(in + 32, &value.statement_receipt_uuid);
  Get(in + 48, &value.requested_catalog_epoch_uuid);
  value.requested_catalog_generation = GetLe(in + 64, 8);
  Get(in + 72, &value.database_uuid);
  Get(in + 88, &value.schema_tree_uuid);
  value.schema_tree_generation = GetLe(in + 104, 8);
  Get(in + 112, &value.security_context_uuid);
  Get(in + 128, &value.policy_snapshot_uuid);
  value.policy_generation = GetLe(in + 144, 8);
  Get(in + 152, &value.catalog_snapshot_uuid);
  value.security_epoch = GetLe(in + 168, 8);
  value.resource_epoch = GetLe(in + 176, 8);
  value.executor_availability_generation = GetLe(in + 184, 8);
  Get(in + 192, &value.descriptor_sha256);
  Get(in + 224, &value.visibility_scope_sha256);
  auto canonical = std::vector<std::uint8_t>(in, in + size);
  std::fill(canonical.begin() + 192, canonical.begin() + 224, 0);
  if (!DescriptorFieldsValid(value) ||
      value.descriptor_sha256 !=
          Hash("ScratchBird.SblrCatalogEpochCheckDescriptor.V1", canonical) ||
      EncodeSblrCatalogEpochCheckDescriptorV1(value) !=
          std::vector<std::uint8_t>(in, in + size)) {
    if (detail != nullptr) *detail = "catalog_epoch_check_descriptor_fields_invalid";
    return false;
  }
  *output = value;
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrCatalogEpochCheckResultV1(
    const SblrCatalogEpochCheckResultV1& value) {
  using namespace catalog_epoch_check_detail;
  if (!ResultFieldsValid(value)) return {};
  auto out = Header("SECR", kSblrCatalogEpochCheckResultBytes,
                    value.object_scoped
                        ? kSblrCatalogEpochCheckObjectScopedFlag
                        : 0);
  Put(&out, value.check_uuid);
  Put(&out, value.observed_catalog_epoch_uuid);
  PutLe(&out, value.observed_catalog_generation, 8);
  Put(&out, value.database_uuid);
  Put(&out, value.schema_tree_uuid);
  PutLe(&out, value.schema_tree_generation, 8);
  out.push_back(value.status);
  out.push_back(value.visibility);
  out.insert(out.end(), 2, 0);
  PutLe(&out, value.observed_security_epoch, 8);
  PutLe(&out, value.observed_resource_epoch, 8);
  Put(&out, value.redaction_profile_uuid);
  Put(&out, value.publication_evidence_uuid);
  out.insert(out.end(), 32, 0);
  PutLe(&out, value.executor_availability_generation, 8);
  out.insert(out.end(), 4, 0);
  const auto digest = Hash("ScratchBird.SblrCatalogEpochCheckResult.V1", out);
  if (NonZero(value.result_material_sha256) &&
      value.result_material_sha256 != digest) {
    return {};
  }
  std::copy(digest.begin(), digest.end(), out.begin() + 148);
  return out;
}

inline bool DecodeSblrCatalogEpochCheckResultV1(
    const std::uint8_t* in, std::size_t size,
    SblrCatalogEpochCheckResultV1* output,
    std::string* detail = nullptr) {
  using namespace catalog_epoch_check_detail;
  std::uint32_t flags = 0;
  if (output == nullptr ||
      !HeaderValid(in, size, "SECR", kSblrCatalogEpochCheckResultBytes,
                   &flags) || !Zero(in + 98, in + 100) ||
      !Zero(in + 188, in + 192)) {
    if (detail != nullptr) *detail = "catalog_epoch_check_result_header_invalid";
    return false;
  }
  SblrCatalogEpochCheckResultV1 value;
  value.object_scoped =
      (flags & kSblrCatalogEpochCheckObjectScopedFlag) != 0;
  Get(in + 16, &value.check_uuid);
  Get(in + 32, &value.observed_catalog_epoch_uuid);
  value.observed_catalog_generation = GetLe(in + 48, 8);
  Get(in + 56, &value.database_uuid);
  Get(in + 72, &value.schema_tree_uuid);
  value.schema_tree_generation = GetLe(in + 88, 8);
  value.status = in[96];
  value.visibility = in[97];
  value.observed_security_epoch = GetLe(in + 100, 8);
  value.observed_resource_epoch = GetLe(in + 108, 8);
  Get(in + 116, &value.redaction_profile_uuid);
  Get(in + 132, &value.publication_evidence_uuid);
  Get(in + 148, &value.result_material_sha256);
  value.executor_availability_generation = GetLe(in + 180, 8);
  auto canonical = std::vector<std::uint8_t>(in, in + size);
  std::fill(canonical.begin() + 148, canonical.begin() + 180, 0);
  if (!ResultFieldsValid(value) ||
      value.result_material_sha256 !=
          Hash("ScratchBird.SblrCatalogEpochCheckResult.V1", canonical) ||
      EncodeSblrCatalogEpochCheckResultV1(value) !=
          std::vector<std::uint8_t>(in, in + size)) {
    if (detail != nullptr) *detail = "catalog_epoch_check_result_fields_invalid";
    return false;
  }
  *output = value;
  return true;
}

}  // namespace scratchbird::engine::sblr
