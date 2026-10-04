// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "datatype_bit_string.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace scratchbird::core::datatypes {

enum class BitStringIndexFamilyV3 : std::uint8_t {
  aggregate_sketch, bitmap, brin_like, btree, columnar_zone_map,
  covering_included, document_path, expression, full_text, graph, hash,
  partial_filtered, range_exclusion, spatial, temporary_work, vector_ann,
};
enum class BitStringProjectionKindV3 : std::uint8_t {
  none, cohort_hash32, sort_key, covering_value, zone_map,
  conditional_result, conditional_underlying,
};
enum class BitStringIndexDispositionV3 : std::uint8_t {
  admitted_projection_only, admitted_with_recheck, admitted_exact_if_budget,
  admitted_payload_only, admitted_exact_selected_mode, conditional, not_applicable,
};
enum class BitStringTemporaryProjectionModeV3 : std::uint8_t {
  unspecified, equality_hash, ordered_sort,
};

struct BitStringIndexResolutionV3 {
  BitStringIndexFamilyV3 family = BitStringIndexFamilyV3::btree;
  BitStringIndexDispositionV3 disposition =
      BitStringIndexDispositionV3::not_applicable;
  BitStringProjectionKindV3 projection = BitStringProjectionKindV3::none;
  bool exact_recheck_required = false;
  std::string_view refusal_diagnostic;
};

struct BitStringIndexProjectionRequestV3 {
  BitStringIndexFamilyV3 family = BitStringIndexFamilyV3::btree;
  const BitStringValueViewV3* value = nullptr;
  u64 provider_max_key_bytes = ~u64{0};
  BitStringSortDirectionV3 direction = BitStringSortDirectionV3::ascending;
  BitStringNullModeV3 null_mode = BitStringNullModeV3::nulls_first;
  BitStringTemporaryProjectionModeV3 temporary_mode =
      BitStringTemporaryProjectionModeV3::unspecified;
  const BitStringIndexResolutionV3* selected_conditional_family = nullptr;
  BitStringExecutionControlV3 control;
};

struct BitStringIndexProjectionResultV3 {
  Status status;
  DiagnosticRecord diagnostic;
  BitStringIndexResolutionV3 resolution;
  std::vector<byte> bytes;
  bool ok() const noexcept { return status.ok(); }
};

BitStringIndexResolutionV3 ResolveBitStringIndexFamilyV3(
    BitStringIndexFamilyV3 family) noexcept;
BitStringIndexProjectionResultV3 ProjectBitStringIndexValueV3(
    const BitStringIndexProjectionRequestV3& request) noexcept;
BitStringViewResultV3 DecodeBitStringCoveringValueNoAllocV3(
    const BitStringDescriptorProfileV3& profile,
    std::span<const byte> encoded) noexcept;

struct BitStringZoneMapRequestV3 {
  const BitStringDescriptorProfileV3* profile = nullptr;
  const BitStringValueViewV3* minimum = nullptr;
  const BitStringValueViewV3* maximum = nullptr;
  u64 null_count = 0;
  u64 present_count = 0;
  u64 empty_present_count = 0;
  BitStringNullModeV3 null_mode = BitStringNullModeV3::nulls_first;
  u64 provider_max_key_bytes = ~u64{0};
  BitStringExecutionControlV3 control;
};
struct BitStringZoneMapViewV3 {
  const BitStringDescriptorProfileV3* profile = nullptr;
  BitStringNullModeV3 null_mode = BitStringNullModeV3::nulls_first;
  u64 null_count = 0;
  u64 present_count = 0;
  u64 empty_present_count = 0;
  u32 minimum_logical_bits = 0;
  u32 maximum_logical_bits = 0;
  std::span<const byte> minimum_sort_key;
  std::span<const byte> maximum_sort_key;
};
struct BitStringZoneMapViewResultV3 {
  Status status;
  struct {
    Status status;
    std::string_view diagnostic_code;
    std::string_view detail;
  } diagnostic;
  BitStringZoneMapViewV3 value;
  bool ok() const noexcept { return status.ok(); }
};
BitStringBytesResultV3 EncodeBitStringZoneMapV3(
    const BitStringZoneMapRequestV3& request) noexcept;
BitStringZoneMapViewResultV3 DecodeBitStringZoneMapNoAllocV3(
    const BitStringDescriptorProfileV3& profile,
    std::span<const byte> encoded) noexcept;

inline constexpr std::array<u32, 22> kBitStringLengthHistogramBoundsV3{
    0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 64, 256, 1024, 4096,
    16384, 65536, 262144, 1048576, 4194304, 16777216};

struct BitStringStatisticsMcvV3 {
  std::array<byte, 32> cohort_hash{};
  u64 frequency = 0;
  u32 logical_bits = 0;
};
struct BitStringStatisticsProjectionV3 {
  const BitStringDescriptorProfileV3* profile = nullptr;
  platform::Uuid statistics_uuid;
  platform::Uuid source_object_uuid;
  u64 schema_epoch = 0;
  u64 sample_epoch = 0;
  u64 security_epoch = 0;
  u64 resource_epoch = 0;
  u64 row_count = 0;
  u64 null_count = 0;
  u64 present_count = 0;
  u64 empty_present_count = 0;
  u64 distinct_count = 0;
  std::uint8_t distinct_kind = 0;
  u32 minimum_logical_bits = 0;
  u32 maximum_logical_bits = 0;
  std::array<byte, 16> sum_logical_bits_u128_le{};
  std::array<u64, 22> length_histogram_counts{};
  std::span<const BitStringStatisticsMcvV3> mcv;
  std::int64_t correlation_scaled_1e9 = INT64_MIN;
  platform::Uuid provider_evidence_uuid;
};
BitStringBytesResultV3 EncodeBitStringStatisticsProjectionV3(
    const BitStringStatisticsProjectionV3& projection) noexcept;

struct BitStringDecodedStatisticsV3 {
  platform::Uuid statistics_uuid;
  platform::Uuid source_object_uuid;
  u64 schema_epoch = 0;
  u64 sample_epoch = 0;
  u64 security_epoch = 0;
  u64 resource_epoch = 0;
  u64 row_count = 0;
  u64 null_count = 0;
  u64 present_count = 0;
  u64 empty_present_count = 0;
  u32 minimum_logical_bits = 0;
  u32 maximum_logical_bits = 0;
  std::array<byte, 16> sum_logical_bits_u128_le{};
  std::array<u64, 22> length_histogram_counts{};
  std::vector<BitStringStatisticsMcvV3> mcv;
  std::int64_t correlation_scaled_1e9 = INT64_MIN;
  platform::Uuid provider_evidence_uuid;
  platform::Uuid selectivity_model_uuid{};
  u64 selectivity_model_generation = 0;
  bool ordered_value_histogram_available = false;
};
struct BitStringStatisticsDecodeResultV3 {
  Status status;
  DiagnosticRecord diagnostic;
  BitStringDecodedStatisticsV3 statistics;
  bool ok() const noexcept { return status.ok(); }
};
BitStringStatisticsDecodeResultV3 DecodeBitStringStatisticsProjectionV3(
    const BitStringDescriptorProfileV3& profile,
    std::span<const byte> encoded) noexcept;

struct BitStringOrderedSelectivityAbsenceV3 {
  platform::Uuid selectivity_model_uuid{};
  u64 selectivity_model_generation = 0;
  bool point_estimate_present = false;
  u32 lower_probability_ppb = 0;
  u32 upper_probability_ppb = 1'000'000'000;
  u32 confidence_ppb = 0;
  bool full_scan_exact_recheck = true;
  std::string_view reason = "ordered_value_histogram_unavailable";
  std::string_view owner_diagnostic = "ISO.SELECTIVITY.MODEL_MISSING";
};
constexpr BitStringOrderedSelectivityAbsenceV3
BitStringOrderedSelectivityAbsenceFactV3() noexcept { return {}; }

struct BitStringBackupTupleV3 {
  platform::Uuid catalog_snapshot_uuid;
  u64 catalog_generation = 0;
  u64 registry_generation = 0;
  std::array<byte, 32> profile_fingerprint{};
  BitStringValueStateV3 state = BitStringValueStateV3::present;
  u32 logical_bit_count = 0;
  std::span<const byte> packed_msb0;
};
struct BitStringBackupTupleResultV3 {
  Status status;
  DiagnosticRecord diagnostic;
  BitStringBackupTupleV3 tuple;
  bool ok() const noexcept { return status.ok(); }
};
BitStringBackupTupleResultV3 ProjectBitStringBackupTupleV3(
    const BitStringValueViewV3& value) noexcept;

struct BitStringOwnedBackupTupleV3 {
  platform::Uuid catalog_snapshot_uuid;
  u64 catalog_generation = 0;
  u64 registry_generation = 0;
  std::array<byte, 32> profile_fingerprint{};
  BitStringValueStateV3 state = BitStringValueStateV3::present;
  u32 logical_bit_count = 0;
  std::vector<byte> packed_msb0;
};
struct BitStringOwnedBackupTupleResultV3 {
  Status status;
  DiagnosticRecord diagnostic;
  BitStringOwnedBackupTupleV3 tuple;
  bool ok() const noexcept { return status.ok(); }
};
BitStringOwnedBackupTupleResultV3 EncodeBitStringBackupTupleV3(
    const BitStringValueViewV3& value,
    const BitStringExecutionControlV3& control = {}) noexcept;
BitStringResultV3 DecodeBitStringBackupTupleV3(
    const BitStringDescriptorProfileV3& profile,
    const BitStringOwnedBackupTupleV3& tuple, bool null_allowed,
    const BitStringExecutionControlV3& control = {}) noexcept;

enum class BitStringProtectionCellV3 : std::uint8_t {
  canonical_plain, inner_compression, inner_encryption, outer_compression,
  outer_authenticated_protection, outer_compress_then_protect,
  outer_protect_then_compress, page_or_filespace_crypto, transport_tls,
  protected_index,
};
struct BitStringProtectionResolutionV3 {
  BitStringProtectionCellV3 cell = BitStringProtectionCellV3::canonical_plain;
  bool datatype_admitted = false;
  bool owner_outside_datatype = false;
  std::string_view disposition;
  std::string_view diagnostic;
};
BitStringProtectionResolutionV3 ResolveBitStringProtectionV3(
    BitStringProtectionCellV3 cell) noexcept;

enum class BitStringWireLaneV3 : std::uint8_t {
  native_sbwp, parser_server_ipc, canonical_sblr, apache_ignite, cassandra,
  clickhouse, cockroachdb, dolt, duckdb, firebird, foundationdb, immudb,
  influxdb, mariadb, milvus, mongodb, mysql, neo4j, opensearch, postgresql,
  redis, sqlite, tidb, tikv, vitess, xtdb, yugabytedb,
};
struct BitStringWireLaneResolutionV3 {
  BitStringWireLaneV3 lane = BitStringWireLaneV3::native_sbwp;
  bool admitted = false;
  bool native_candidate = false;
  std::string_view disposition;
  std::string_view diagnostic = "CTB.TRANSPORT.UNSUPPORTED";
};
BitStringWireLaneResolutionV3 ResolveBitStringWireLaneV3(
    BitStringWireLaneV3 lane) noexcept;

}  // namespace scratchbird::core::datatypes
