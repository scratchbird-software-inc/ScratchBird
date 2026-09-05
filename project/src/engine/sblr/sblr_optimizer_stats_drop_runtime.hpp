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

inline constexpr std::uint16_t kSblrOptimizerStatsDropOpcodeCode = 4867;
inline constexpr std::size_t kSblrOptimizerStatsDropRequestBytes = 64;
inline constexpr std::size_t kSblrOptimizerStatsDropDescriptorBytes = 320;
inline constexpr std::size_t kSblrOptimizerStatsDropResultBytes = 224;
inline constexpr std::uint32_t kSblrOptimizerStatsDropAllScopesFlag = 0x1;
inline constexpr std::uint32_t kSblrOptimizerStatsDropPublishedStatus = 0x1;

using SblrOptimizerStatsDropUuidV1 = std::array<std::uint8_t, 16>;
using SblrOptimizerStatsDropSha256V1 = std::array<std::uint8_t, 32>;

struct SblrOptimizerStatsDropRequestV1 {
  SblrOptimizerStatsDropUuidV1 statement_receipt_uuid{};
  std::uint64_t occurrence = 0;
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
};

// Engine-issued authority for one database-wide optimizer-statistics epoch
// advance. The parser may copy this descriptor but cannot select an epoch,
// authorization identity, transaction, publication generation, or executor.
struct SblrOptimizerStatsDropDescriptorV1 {
  SblrOptimizerStatsDropUuidV1 effect_uuid{};
  SblrOptimizerStatsDropUuidV1 statement_receipt_uuid{};
  SblrOptimizerStatsDropUuidV1 statement_uuid{};
  SblrOptimizerStatsDropUuidV1 statement_snapshot_uuid{};
  SblrOptimizerStatsDropUuidV1 catalog_epoch_uuid{};
  std::uint64_t catalog_generation = 0;
  SblrOptimizerStatsDropUuidV1 security_context_uuid{};
  std::uint64_t security_epoch = 0;
  SblrOptimizerStatsDropUuidV1 resource_admission_uuid{};
  std::uint64_t resource_epoch = 0;
  SblrOptimizerStatsDropUuidV1 owning_transaction_uuid{};
  std::uint64_t owning_local_transaction_id = 0;
  std::uint64_t inventory_generation = 0;
  std::uint64_t expected_statistics_epoch = 0;
  std::uint64_t expected_journal_generation = 0;
  SblrOptimizerStatsDropUuidV1 authorization_authority_uuid{};
  std::uint64_t authorization_generation = 0;
  std::uint64_t authorization_policy_epoch = 0;
  std::uint32_t flags = kSblrOptimizerStatsDropAllScopesFlag;
  SblrOptimizerStatsDropSha256V1 descriptor_sha256{};
  SblrOptimizerStatsDropUuidV1 parser_package_uuid{};
  std::uint64_t executor_availability_generation = 0;
  std::uint64_t proposed_effect_generation = 0;
  std::uint64_t next_statistics_epoch = 0;
};

struct SblrOptimizerStatsDropResultV1 {
  SblrOptimizerStatsDropUuidV1 effect_uuid{};
  SblrOptimizerStatsDropUuidV1 statement_receipt_uuid{};
  SblrOptimizerStatsDropUuidV1 durable_publication_uuid{};
  std::uint64_t prior_statistics_epoch = 0;
  std::uint64_t statistics_epoch = 0;
  std::uint64_t effect_generation = 0;
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t inventory_generation = 0;
  std::uint64_t cache_invalidation_generation = 0;
  std::uint32_t flags = kSblrOptimizerStatsDropAllScopesFlag;
  std::uint32_t status = kSblrOptimizerStatsDropPublishedStatus;
  SblrOptimizerStatsDropSha256V1 result_material_sha256{};
  SblrOptimizerStatsDropSha256V1 executor_evidence_sha256{};
  std::uint64_t executor_availability_generation = 0;
  std::uint64_t publication_barrier_generation = 0;
};

namespace optimizer_stats_drop_detail {

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

inline SblrOptimizerStatsDropSha256V1 Hash(
    const std::vector<std::uint8_t>& bytes) {
  return scratchbird::core::hash::ComputeSha256Digest(bytes).digest;
}

inline std::vector<std::uint8_t> Header(std::string_view magic,
                                        std::size_t bytes) {
  std::vector<std::uint8_t> out(magic.begin(), magic.end());
  PutLe(&out, 1, 2);
  PutLe(&out, bytes, 2);
  PutLe(&out, bytes, 4);
  PutLe(&out, 0, 4);
  return out;
}

inline bool HeaderValid(const std::uint8_t* in, std::size_t size,
                        std::string_view magic, std::size_t bytes) {
  return in != nullptr && size == bytes && magic.size() == 4 &&
         std::equal(magic.begin(), magic.end(), in) &&
         GetLe(in + 4, 2) == 1 && GetLe(in + 6, 2) == bytes &&
         GetLe(in + 8, 4) == bytes && Zero(in + 12, in + 16);
}

inline bool Incremented(std::uint64_t prior, std::uint64_t next) {
  return prior != 0 && prior != std::numeric_limits<std::uint64_t>::max() &&
         next == prior + 1;
}

inline bool DescriptorFieldsValid(
    const SblrOptimizerStatsDropDescriptorV1& value) {
  return NonZero(value.effect_uuid) &&
         NonZero(value.statement_receipt_uuid) &&
         NonZero(value.statement_uuid) &&
         NonZero(value.statement_snapshot_uuid) &&
         NonZero(value.catalog_epoch_uuid) && value.catalog_generation != 0 &&
         NonZero(value.security_context_uuid) && value.security_epoch != 0 &&
         NonZero(value.resource_admission_uuid) && value.resource_epoch != 0 &&
         NonZero(value.owning_transaction_uuid) &&
         value.owning_local_transaction_id != 0 &&
         value.inventory_generation != 0 &&
         value.expected_statistics_epoch != 0 &&
         value.proposed_effect_generation ==
             value.expected_journal_generation + 1 &&
         value.proposed_effect_generation != 0 &&
         Incremented(value.expected_statistics_epoch,
                     value.next_statistics_epoch) &&
         NonZero(value.authorization_authority_uuid) &&
         value.authorization_generation != 0 &&
         value.authorization_policy_epoch != 0 &&
         value.flags == kSblrOptimizerStatsDropAllScopesFlag &&
         NonZero(value.parser_package_uuid) &&
         value.executor_availability_generation != 0;
}

inline bool ResultFieldsValid(const SblrOptimizerStatsDropResultV1& value) {
  return NonZero(value.effect_uuid) &&
         NonZero(value.statement_receipt_uuid) &&
         NonZero(value.durable_publication_uuid) &&
         value.durable_publication_uuid != value.effect_uuid &&
         Incremented(value.prior_statistics_epoch, value.statistics_epoch) &&
         value.effect_generation != 0 && value.catalog_generation != 0 &&
         value.security_epoch != 0 && value.resource_epoch != 0 &&
         value.inventory_generation != 0 &&
         value.cache_invalidation_generation == value.statistics_epoch &&
         value.flags == kSblrOptimizerStatsDropAllScopesFlag &&
         value.status == kSblrOptimizerStatsDropPublishedStatus &&
         value.executor_availability_generation != 0 &&
         value.publication_barrier_generation == value.effect_generation;
}

inline std::vector<std::uint8_t> ResultMaterial(
    const SblrOptimizerStatsDropResultV1& value) {
  std::vector<std::uint8_t> material;
  const std::string_view domain =
      "ScratchBird.SblrOptimizerStatsDropResult.V1";
  material.insert(material.end(), domain.begin(), domain.end());
  Put(&material, value.effect_uuid);
  Put(&material, value.statement_receipt_uuid);
  Put(&material, value.durable_publication_uuid);
  PutLe(&material, value.prior_statistics_epoch, 8);
  PutLe(&material, value.statistics_epoch, 8);
  PutLe(&material, value.effect_generation, 8);
  PutLe(&material, value.catalog_generation, 8);
  PutLe(&material, value.security_epoch, 8);
  PutLe(&material, value.resource_epoch, 8);
  PutLe(&material, value.inventory_generation, 8);
  PutLe(&material, value.cache_invalidation_generation, 8);
  PutLe(&material, value.flags, 4);
  PutLe(&material, value.status, 4);
  PutLe(&material, value.executor_availability_generation, 8);
  PutLe(&material, value.publication_barrier_generation, 8);
  return material;
}

}  // namespace optimizer_stats_drop_detail

inline SblrOptimizerStatsDropSha256V1
OptimizerStatsDropResultMaterialSha256V1(
    const SblrOptimizerStatsDropResultV1& value) {
  return optimizer_stats_drop_detail::Hash(
      optimizer_stats_drop_detail::ResultMaterial(value));
}

inline SblrOptimizerStatsDropSha256V1
OptimizerStatsDropExecutorEvidenceSha256V1(
    const SblrOptimizerStatsDropSha256V1& result_material_sha256,
    std::uint64_t availability_generation) {
  using namespace optimizer_stats_drop_detail;
  std::vector<std::uint8_t> material;
  const std::string_view domain =
      "ScratchBird.SblrOptimizerStatsDropExecutorEvidence.V1";
  material.insert(material.end(), domain.begin(), domain.end());
  Put(&material, result_material_sha256);
  PutLe(&material, availability_generation, 8);
  return Hash(material);
}

inline std::vector<std::uint8_t> EncodeSblrOptimizerStatsDropRequestV1(
    const SblrOptimizerStatsDropRequestV1& value) {
  using namespace optimizer_stats_drop_detail;
  if (!NonZero(value.statement_receipt_uuid) || value.occurrence == 0 ||
      value.catalog_generation == 0 || value.security_epoch == 0 ||
      value.resource_epoch == 0) {
    return {};
  }
  auto out = Header("OSDQ", kSblrOptimizerStatsDropRequestBytes);
  Put(&out, value.statement_receipt_uuid);
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_epoch, 8);
  return out;
}

inline bool DecodeSblrOptimizerStatsDropRequestV1(
    const std::uint8_t* in, std::size_t size,
    SblrOptimizerStatsDropRequestV1* output, std::string* detail = nullptr) {
  using namespace optimizer_stats_drop_detail;
  if (output == nullptr ||
      !HeaderValid(in, size, "OSDQ", kSblrOptimizerStatsDropRequestBytes)) {
    if (detail != nullptr) *detail = "optimizer_stats_drop_request_header_invalid";
    return false;
  }
  SblrOptimizerStatsDropRequestV1 value;
  Get(in + 16, &value.statement_receipt_uuid);
  value.occurrence = GetLe(in + 32, 8);
  value.catalog_generation = GetLe(in + 40, 8);
  value.security_epoch = GetLe(in + 48, 8);
  value.resource_epoch = GetLe(in + 56, 8);
  if (EncodeSblrOptimizerStatsDropRequestV1(value) !=
      std::vector<std::uint8_t>(in, in + size)) {
    if (detail != nullptr) *detail = "optimizer_stats_drop_request_fields_invalid";
    return false;
  }
  *output = value;
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrOptimizerStatsDropDescriptorV1(
    const SblrOptimizerStatsDropDescriptorV1& value) {
  using namespace optimizer_stats_drop_detail;
  if (!DescriptorFieldsValid(value)) return {};
  auto out = Header("OSDD", kSblrOptimizerStatsDropDescriptorBytes);
  Put(&out, value.effect_uuid);
  Put(&out, value.statement_receipt_uuid);
  Put(&out, value.statement_uuid);
  Put(&out, value.statement_snapshot_uuid);
  Put(&out, value.catalog_epoch_uuid);
  PutLe(&out, value.catalog_generation, 8);
  Put(&out, value.security_context_uuid);
  PutLe(&out, value.security_epoch, 8);
  Put(&out, value.resource_admission_uuid);
  PutLe(&out, value.resource_epoch, 8);
  Put(&out, value.owning_transaction_uuid);
  PutLe(&out, value.owning_local_transaction_id, 8);
  PutLe(&out, value.inventory_generation, 8);
  PutLe(&out, value.expected_statistics_epoch, 8);
  PutLe(&out, value.expected_journal_generation, 8);
  Put(&out, value.authorization_authority_uuid);
  PutLe(&out, value.authorization_generation, 8);
  PutLe(&out, value.authorization_policy_epoch, 8);
  PutLe(&out, value.flags, 4);
  PutLe(&out, 0, 4);
  out.insert(out.end(), 32, 0);
  Put(&out, value.parser_package_uuid);
  PutLe(&out, value.executor_availability_generation, 8);
  PutLe(&out, value.proposed_effect_generation, 8);
  PutLe(&out, value.next_statistics_epoch, 8);
  PutLe(&out, 0, 8);
  const auto evidence = Hash(out);
  if (NonZero(value.descriptor_sha256) &&
      value.descriptor_sha256 != evidence) {
    return {};
  }
  std::copy(evidence.begin(), evidence.end(), out.begin() + 240);
  return out;
}

inline bool DecodeSblrOptimizerStatsDropDescriptorV1(
    const std::uint8_t* in, std::size_t size,
    SblrOptimizerStatsDropDescriptorV1* output,
    std::string* detail = nullptr) {
  using namespace optimizer_stats_drop_detail;
  if (output == nullptr ||
      !HeaderValid(in, size, "OSDD", kSblrOptimizerStatsDropDescriptorBytes) ||
      !Zero(in + 236, in + 240) || !Zero(in + 312, in + 320)) {
    if (detail != nullptr) *detail = "optimizer_stats_drop_descriptor_header_invalid";
    return false;
  }
  SblrOptimizerStatsDropDescriptorV1 value;
  Get(in + 16, &value.effect_uuid);
  Get(in + 32, &value.statement_receipt_uuid);
  Get(in + 48, &value.statement_uuid);
  Get(in + 64, &value.statement_snapshot_uuid);
  Get(in + 80, &value.catalog_epoch_uuid);
  value.catalog_generation = GetLe(in + 96, 8);
  Get(in + 104, &value.security_context_uuid);
  value.security_epoch = GetLe(in + 120, 8);
  Get(in + 128, &value.resource_admission_uuid);
  value.resource_epoch = GetLe(in + 144, 8);
  Get(in + 152, &value.owning_transaction_uuid);
  value.owning_local_transaction_id = GetLe(in + 168, 8);
  value.inventory_generation = GetLe(in + 176, 8);
  value.expected_statistics_epoch = GetLe(in + 184, 8);
  value.expected_journal_generation = GetLe(in + 192, 8);
  Get(in + 200, &value.authorization_authority_uuid);
  value.authorization_generation = GetLe(in + 216, 8);
  value.authorization_policy_epoch = GetLe(in + 224, 8);
  value.flags = static_cast<std::uint32_t>(GetLe(in + 232, 4));
  Get(in + 240, &value.descriptor_sha256);
  Get(in + 272, &value.parser_package_uuid);
  value.executor_availability_generation = GetLe(in + 288, 8);
  value.proposed_effect_generation = GetLe(in + 296, 8);
  value.next_statistics_epoch = GetLe(in + 304, 8);
  auto material = std::vector<std::uint8_t>(in, in + size);
  std::fill(material.begin() + 240, material.begin() + 272, 0);
  if (!DescriptorFieldsValid(value) || value.descriptor_sha256 != Hash(material) ||
      EncodeSblrOptimizerStatsDropDescriptorV1(value) !=
          std::vector<std::uint8_t>(in, in + size)) {
    if (detail != nullptr) *detail = "optimizer_stats_drop_descriptor_fields_invalid";
    return false;
  }
  *output = value;
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrOptimizerStatsDropResultV1(
    const SblrOptimizerStatsDropResultV1& value) {
  using namespace optimizer_stats_drop_detail;
  if (!ResultFieldsValid(value)) return {};
  const auto result_hash = OptimizerStatsDropResultMaterialSha256V1(value);
  const auto executor_hash = OptimizerStatsDropExecutorEvidenceSha256V1(
      result_hash, value.executor_availability_generation);
  if ((NonZero(value.result_material_sha256) &&
       value.result_material_sha256 != result_hash) ||
      (NonZero(value.executor_evidence_sha256) &&
       value.executor_evidence_sha256 != executor_hash)) {
    return {};
  }
  auto out = Header("OSDR", kSblrOptimizerStatsDropResultBytes);
  Put(&out, value.effect_uuid);
  Put(&out, value.statement_receipt_uuid);
  Put(&out, value.durable_publication_uuid);
  PutLe(&out, value.prior_statistics_epoch, 8);
  PutLe(&out, value.statistics_epoch, 8);
  PutLe(&out, value.effect_generation, 8);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_epoch, 8);
  PutLe(&out, value.inventory_generation, 8);
  PutLe(&out, value.cache_invalidation_generation, 8);
  PutLe(&out, value.flags, 4);
  PutLe(&out, value.status, 4);
  Put(&out, result_hash);
  Put(&out, executor_hash);
  PutLe(&out, value.executor_availability_generation, 8);
  PutLe(&out, value.publication_barrier_generation, 8);
  PutLe(&out, 0, 8);
  return out;
}

inline bool DecodeSblrOptimizerStatsDropResultV1(
    const std::uint8_t* in, std::size_t size,
    SblrOptimizerStatsDropResultV1* output,
    std::string* detail = nullptr) {
  using namespace optimizer_stats_drop_detail;
  if (output == nullptr ||
      !HeaderValid(in, size, "OSDR", kSblrOptimizerStatsDropResultBytes) ||
      !Zero(in + 216, in + 224)) {
    if (detail != nullptr) *detail = "optimizer_stats_drop_result_header_invalid";
    return false;
  }
  SblrOptimizerStatsDropResultV1 value;
  Get(in + 16, &value.effect_uuid);
  Get(in + 32, &value.statement_receipt_uuid);
  Get(in + 48, &value.durable_publication_uuid);
  value.prior_statistics_epoch = GetLe(in + 64, 8);
  value.statistics_epoch = GetLe(in + 72, 8);
  value.effect_generation = GetLe(in + 80, 8);
  value.catalog_generation = GetLe(in + 88, 8);
  value.security_epoch = GetLe(in + 96, 8);
  value.resource_epoch = GetLe(in + 104, 8);
  value.inventory_generation = GetLe(in + 112, 8);
  value.cache_invalidation_generation = GetLe(in + 120, 8);
  value.flags = static_cast<std::uint32_t>(GetLe(in + 128, 4));
  value.status = static_cast<std::uint32_t>(GetLe(in + 132, 4));
  Get(in + 136, &value.result_material_sha256);
  Get(in + 168, &value.executor_evidence_sha256);
  value.executor_availability_generation = GetLe(in + 200, 8);
  value.publication_barrier_generation = GetLe(in + 208, 8);
  if (!ResultFieldsValid(value) ||
      value.result_material_sha256 !=
          OptimizerStatsDropResultMaterialSha256V1(value) ||
      value.executor_evidence_sha256 !=
          OptimizerStatsDropExecutorEvidenceSha256V1(
              value.result_material_sha256,
              value.executor_availability_generation) ||
      EncodeSblrOptimizerStatsDropResultV1(value) !=
          std::vector<std::uint8_t>(in, in + size)) {
    if (detail != nullptr) *detail = "optimizer_stats_drop_result_fields_invalid";
    return false;
  }
  *output = value;
  return true;
}

}  // namespace scratchbird::engine::sblr
