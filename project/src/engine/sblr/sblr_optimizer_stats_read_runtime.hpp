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

inline constexpr std::uint16_t kSblrOptimizerStatsReadOpcodeCode = 4866;
inline constexpr std::size_t kSblrOptimizerStatsReadRequestBytes = 64;
inline constexpr std::size_t kSblrOptimizerStatsReadDescriptorBytes = 256;
inline constexpr std::size_t kSblrOptimizerStatsReadResultBytes = 224;
inline constexpr std::uint32_t kSblrOptimizerStatsReadCatalogFlags = 0x7;

using SblrOptimizerStatsReadUuidV1 = std::array<std::uint8_t, 16>;
using SblrOptimizerStatsReadSha256V1 = std::array<std::uint8_t, 32>;

struct SblrOptimizerStatsReadRequestV1 {
  SblrOptimizerStatsReadUuidV1 statement_receipt_uuid{};
  std::uint64_t occurrence = 0;
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
};

// Exact engine-issued authority for the narrow catalog-statistics reader.
// This descriptor neither enables optimizer capability nor carries a plan,
// rule, cost, candidate, memo, or rewrite request.
struct SblrOptimizerStatsReadDescriptorV1 {
  SblrOptimizerStatsReadUuidV1 statistics_snapshot_uuid{};
  SblrOptimizerStatsReadUuidV1 statement_receipt_uuid{};
  SblrOptimizerStatsReadUuidV1 statement_uuid{};
  SblrOptimizerStatsReadUuidV1 statement_snapshot_uuid{};
  SblrOptimizerStatsReadUuidV1 catalog_epoch_uuid{};
  std::uint64_t catalog_generation = 0;
  SblrOptimizerStatsReadUuidV1 security_context_uuid{};
  std::uint64_t security_epoch = 0;
  SblrOptimizerStatsReadUuidV1 resource_admission_uuid{};
  std::uint64_t resource_epoch = 0;
  SblrOptimizerStatsReadUuidV1 owning_transaction_uuid{};
  std::uint64_t owning_local_transaction_id = 0;
  std::uint64_t inventory_generation = 0;
  std::uint32_t flags = kSblrOptimizerStatsReadCatalogFlags;
  SblrOptimizerStatsReadSha256V1 descriptor_sha256{};
  SblrOptimizerStatsReadUuidV1 parser_package_uuid{};
  std::uint64_t executor_availability_generation = 0;
  std::uint64_t optimizer_statistics_epoch = 0;
};

struct SblrOptimizerStatsReadResultV1 {
  SblrOptimizerStatsReadUuidV1 statistics_snapshot_uuid{};
  SblrOptimizerStatsReadUuidV1 statement_receipt_uuid{};
  SblrOptimizerStatsReadUuidV1 statement_snapshot_uuid{};
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t inventory_generation = 0;
  std::uint64_t visible_row_estimate = 0;
  std::uint64_t retained_row_version_count = 0;
  std::uint64_t row_store_bytes = 0;
  std::uint64_t index_store_bytes = 0;
  std::uint64_t table_size_bytes = 0;
  std::uint32_t flags = kSblrOptimizerStatsReadCatalogFlags;
  SblrOptimizerStatsReadSha256V1 result_material_sha256{};
  SblrOptimizerStatsReadSha256V1 executor_evidence_sha256{};
  std::uint64_t executor_availability_generation = 0;
  std::uint64_t optimizer_statistics_epoch = 0;
};

namespace optimizer_stats_read_detail {

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

inline SblrOptimizerStatsReadSha256V1 Hash(
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

inline bool DescriptorFieldsValid(
    const SblrOptimizerStatsReadDescriptorV1& value) {
  return NonZero(value.statistics_snapshot_uuid) &&
         NonZero(value.statement_receipt_uuid) &&
         NonZero(value.statement_uuid) &&
         NonZero(value.statement_snapshot_uuid) &&
         NonZero(value.catalog_epoch_uuid) && value.catalog_generation != 0 &&
         NonZero(value.security_context_uuid) && value.security_epoch != 0 &&
         NonZero(value.resource_admission_uuid) && value.resource_epoch != 0 &&
         NonZero(value.owning_transaction_uuid) &&
         value.owning_local_transaction_id != 0 &&
         value.inventory_generation != 0 &&
         value.flags == kSblrOptimizerStatsReadCatalogFlags &&
         NonZero(value.parser_package_uuid) &&
         value.executor_availability_generation != 0 &&
         value.optimizer_statistics_epoch != 0;
}

inline bool ResultFieldsValid(const SblrOptimizerStatsReadResultV1& value) {
  return NonZero(value.statistics_snapshot_uuid) &&
         NonZero(value.statement_receipt_uuid) &&
         NonZero(value.statement_snapshot_uuid) &&
         value.catalog_generation != 0 && value.security_epoch != 0 &&
         value.resource_epoch != 0 && value.inventory_generation != 0 &&
         value.flags == kSblrOptimizerStatsReadCatalogFlags &&
         value.row_store_bytes <=
             std::numeric_limits<std::uint64_t>::max() -
                 value.index_store_bytes &&
         value.table_size_bytes ==
             value.row_store_bytes + value.index_store_bytes &&
         value.executor_availability_generation != 0 &&
         value.optimizer_statistics_epoch != 0;
}

inline std::vector<std::uint8_t> ResultMaterial(
    const SblrOptimizerStatsReadResultV1& value) {
  std::vector<std::uint8_t> material;
  const std::string_view domain =
      "ScratchBird.SblrOptimizerStatsReadResult.V1";
  material.insert(material.end(), domain.begin(), domain.end());
  Put(&material, value.statistics_snapshot_uuid);
  Put(&material, value.statement_receipt_uuid);
  Put(&material, value.statement_snapshot_uuid);
  PutLe(&material, value.catalog_generation, 8);
  PutLe(&material, value.security_epoch, 8);
  PutLe(&material, value.resource_epoch, 8);
  PutLe(&material, value.inventory_generation, 8);
  PutLe(&material, value.visible_row_estimate, 8);
  PutLe(&material, value.retained_row_version_count, 8);
  PutLe(&material, value.row_store_bytes, 8);
  PutLe(&material, value.index_store_bytes, 8);
  PutLe(&material, value.table_size_bytes, 8);
  PutLe(&material, value.flags, 4);
  PutLe(&material, value.executor_availability_generation, 8);
  PutLe(&material, value.optimizer_statistics_epoch, 8);
  return material;
}

}  // namespace optimizer_stats_read_detail

inline SblrOptimizerStatsReadSha256V1
OptimizerStatsReadResultMaterialSha256V1(
    const SblrOptimizerStatsReadResultV1& value) {
  return optimizer_stats_read_detail::Hash(
      optimizer_stats_read_detail::ResultMaterial(value));
}

inline SblrOptimizerStatsReadSha256V1
OptimizerStatsReadExecutorEvidenceSha256V1(
    const SblrOptimizerStatsReadSha256V1& result_material_sha256,
    std::uint64_t availability_generation) {
  using namespace optimizer_stats_read_detail;
  std::vector<std::uint8_t> material;
  const std::string_view domain =
      "ScratchBird.SblrOptimizerStatsReadExecutorEvidence.V1";
  material.insert(material.end(), domain.begin(), domain.end());
  Put(&material, result_material_sha256);
  PutLe(&material, availability_generation, 8);
  return Hash(material);
}

inline std::vector<std::uint8_t> EncodeSblrOptimizerStatsReadRequestV1(
    const SblrOptimizerStatsReadRequestV1& value) {
  using namespace optimizer_stats_read_detail;
  if (!NonZero(value.statement_receipt_uuid) || value.occurrence == 0 ||
      value.catalog_generation == 0 || value.security_epoch == 0 ||
      value.resource_epoch == 0) {
    return {};
  }
  auto out = Header("OSRQ", kSblrOptimizerStatsReadRequestBytes);
  Put(&out, value.statement_receipt_uuid);
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_epoch, 8);
  return out;
}

inline bool DecodeSblrOptimizerStatsReadRequestV1(
    const std::uint8_t* in, std::size_t size,
    SblrOptimizerStatsReadRequestV1* output, std::string* detail = nullptr) {
  using namespace optimizer_stats_read_detail;
  if (output == nullptr ||
      !HeaderValid(in, size, "OSRQ", kSblrOptimizerStatsReadRequestBytes)) {
    if (detail != nullptr) *detail = "optimizer_stats_request_header_invalid";
    return false;
  }
  SblrOptimizerStatsReadRequestV1 value;
  Get(in + 16, &value.statement_receipt_uuid);
  value.occurrence = GetLe(in + 32, 8);
  value.catalog_generation = GetLe(in + 40, 8);
  value.security_epoch = GetLe(in + 48, 8);
  value.resource_epoch = GetLe(in + 56, 8);
  if (EncodeSblrOptimizerStatsReadRequestV1(value) !=
      std::vector<std::uint8_t>(in, in + size)) {
    if (detail != nullptr) *detail = "optimizer_stats_request_fields_invalid";
    return false;
  }
  *output = value;
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrOptimizerStatsReadDescriptorV1(
    const SblrOptimizerStatsReadDescriptorV1& value) {
  using namespace optimizer_stats_read_detail;
  if (!DescriptorFieldsValid(value)) return {};
  auto out = Header("OSRD", kSblrOptimizerStatsReadDescriptorBytes);
  Put(&out, value.statistics_snapshot_uuid);
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
  PutLe(&out, value.flags, 4);
  PutLe(&out, 0, 4);
  out.insert(out.end(), 32, 0);
  Put(&out, value.parser_package_uuid);
  PutLe(&out, value.executor_availability_generation, 8);
  PutLe(&out, value.optimizer_statistics_epoch, 8);
  const auto evidence = Hash(out);
  if (NonZero(value.descriptor_sha256) &&
      value.descriptor_sha256 != evidence) {
    return {};
  }
  std::copy(evidence.begin(), evidence.end(), out.begin() + 192);
  return out;
}

inline bool DecodeSblrOptimizerStatsReadDescriptorV1(
    const std::uint8_t* in, std::size_t size,
    SblrOptimizerStatsReadDescriptorV1* output,
    std::string* detail = nullptr) {
  using namespace optimizer_stats_read_detail;
  if (output == nullptr ||
      !HeaderValid(in, size, "OSRD", kSblrOptimizerStatsReadDescriptorBytes) ||
      !Zero(in + 188, in + 192)) {
    if (detail != nullptr)
      *detail = "optimizer_stats_descriptor_header_invalid";
    return false;
  }
  SblrOptimizerStatsReadDescriptorV1 value;
  Get(in + 16, &value.statistics_snapshot_uuid);
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
  value.flags = static_cast<std::uint32_t>(GetLe(in + 184, 4));
  Get(in + 192, &value.descriptor_sha256);
  Get(in + 224, &value.parser_package_uuid);
  value.executor_availability_generation = GetLe(in + 240, 8);
  value.optimizer_statistics_epoch = GetLe(in + 248, 8);
  auto material = std::vector<std::uint8_t>(in, in + size);
  std::fill(material.begin() + 192, material.begin() + 224, 0);
  if (!DescriptorFieldsValid(value) || value.descriptor_sha256 != Hash(material) ||
      EncodeSblrOptimizerStatsReadDescriptorV1(value) !=
          std::vector<std::uint8_t>(in, in + size)) {
    if (detail != nullptr)
      *detail = "optimizer_stats_descriptor_fields_invalid";
    return false;
  }
  *output = value;
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrOptimizerStatsReadResultV1(
    const SblrOptimizerStatsReadResultV1& value) {
  using namespace optimizer_stats_read_detail;
  if (!ResultFieldsValid(value)) return {};
  const auto result_hash = OptimizerStatsReadResultMaterialSha256V1(value);
  const auto executor_hash = OptimizerStatsReadExecutorEvidenceSha256V1(
      result_hash, value.executor_availability_generation);
  if ((NonZero(value.result_material_sha256) &&
       value.result_material_sha256 != result_hash) ||
      (NonZero(value.executor_evidence_sha256) &&
       value.executor_evidence_sha256 != executor_hash)) {
    return {};
  }
  auto out = Header("OSRR", kSblrOptimizerStatsReadResultBytes);
  Put(&out, value.statistics_snapshot_uuid);
  Put(&out, value.statement_receipt_uuid);
  Put(&out, value.statement_snapshot_uuid);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_epoch, 8);
  PutLe(&out, value.inventory_generation, 8);
  PutLe(&out, value.visible_row_estimate, 8);
  PutLe(&out, value.retained_row_version_count, 8);
  PutLe(&out, value.row_store_bytes, 8);
  PutLe(&out, value.index_store_bytes, 8);
  PutLe(&out, value.table_size_bytes, 8);
  PutLe(&out, value.flags, 4);
  PutLe(&out, 0, 4);
  Put(&out, result_hash);
  Put(&out, executor_hash);
  PutLe(&out, value.executor_availability_generation, 8);
  PutLe(&out, value.optimizer_statistics_epoch, 8);
  return out;
}

inline bool DecodeSblrOptimizerStatsReadResultV1(
    const std::uint8_t* in, std::size_t size,
    SblrOptimizerStatsReadResultV1* output, std::string* detail = nullptr) {
  using namespace optimizer_stats_read_detail;
  if (output == nullptr ||
      !HeaderValid(in, size, "OSRR", kSblrOptimizerStatsReadResultBytes) ||
      !Zero(in + 140, in + 144)) {
    if (detail != nullptr) *detail = "optimizer_stats_result_header_invalid";
    return false;
  }
  SblrOptimizerStatsReadResultV1 value;
  Get(in + 16, &value.statistics_snapshot_uuid);
  Get(in + 32, &value.statement_receipt_uuid);
  Get(in + 48, &value.statement_snapshot_uuid);
  value.catalog_generation = GetLe(in + 64, 8);
  value.security_epoch = GetLe(in + 72, 8);
  value.resource_epoch = GetLe(in + 80, 8);
  value.inventory_generation = GetLe(in + 88, 8);
  value.visible_row_estimate = GetLe(in + 96, 8);
  value.retained_row_version_count = GetLe(in + 104, 8);
  value.row_store_bytes = GetLe(in + 112, 8);
  value.index_store_bytes = GetLe(in + 120, 8);
  value.table_size_bytes = GetLe(in + 128, 8);
  value.flags = static_cast<std::uint32_t>(GetLe(in + 136, 4));
  Get(in + 144, &value.result_material_sha256);
  Get(in + 176, &value.executor_evidence_sha256);
  value.executor_availability_generation = GetLe(in + 208, 8);
  value.optimizer_statistics_epoch = GetLe(in + 216, 8);
  if (!ResultFieldsValid(value) ||
      value.result_material_sha256 !=
          OptimizerStatsReadResultMaterialSha256V1(value) ||
      value.executor_evidence_sha256 !=
          OptimizerStatsReadExecutorEvidenceSha256V1(
              value.result_material_sha256,
              value.executor_availability_generation) ||
      EncodeSblrOptimizerStatsReadResultV1(value) !=
          std::vector<std::uint8_t>(in, in + size)) {
    if (detail != nullptr) *detail = "optimizer_stats_result_fields_invalid";
    return false;
  }
  *output = value;
  return true;
}

}  // namespace scratchbird::engine::sblr
