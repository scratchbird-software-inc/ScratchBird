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

enum class BitStringIndexFamilyV1 : std::uint8_t {
  aggregate_sketch, bitmap, brin_like, btree, columnar_zone_map,
  covering_included, document_path, expression, full_text, graph, hash,
  partial_filtered, range_exclusion, spatial, temporary_work, vector_ann,
};
enum class BitStringProjectionKindV1 : std::uint8_t {
  none, cohort_hash32, sort_key, covering_value, zone_map,
  conditional_result, conditional_underlying,
};
enum class BitStringIndexDispositionV1 : std::uint8_t {
  admitted_projection_only, admitted_with_recheck, admitted_exact_if_budget,
  admitted_payload_only, admitted_exact_selected_mode, conditional, not_applicable,
};
enum class BitStringTemporaryProjectionModeV1 : std::uint8_t {
  unspecified, equality_hash, ordered_sort,
};

struct BitStringIndexResolutionV1 {
  BitStringIndexFamilyV1 family = BitStringIndexFamilyV1::btree;
  BitStringIndexDispositionV1 disposition =
      BitStringIndexDispositionV1::not_applicable;
  BitStringProjectionKindV1 projection = BitStringProjectionKindV1::none;
  bool exact_recheck_required = false;
  std::string_view refusal_diagnostic;
};

struct BitStringIndexProjectionRequestV1 {
  BitStringIndexFamilyV1 family = BitStringIndexFamilyV1::btree;
  const BitStringValueViewV1* value = nullptr;
  u64 provider_max_key_bytes = ~u64{0};
  BitStringSortDirectionV1 direction = BitStringSortDirectionV1::ascending;
  BitStringNullModeV1 null_mode = BitStringNullModeV1::nulls_first;
  BitStringTemporaryProjectionModeV1 temporary_mode =
      BitStringTemporaryProjectionModeV1::unspecified;
  const BitStringIndexResolutionV1* selected_conditional_family = nullptr;
  BitStringExecutionControlV1 control;
};

struct BitStringIndexProjectionResultV1 {
  Status status;
  DiagnosticRecord diagnostic;
  BitStringIndexResolutionV1 resolution;
  std::vector<byte> bytes;
  bool ok() const noexcept { return status.ok(); }
};

BitStringIndexResolutionV1 ResolveBitStringIndexFamilyV1(
    BitStringIndexFamilyV1 family) noexcept;
BitStringIndexProjectionResultV1 ProjectBitStringIndexValueV1(
    const BitStringIndexProjectionRequestV1& request) noexcept;
BitStringViewResultV1 DecodeBitStringCoveringValueNoAllocV1(
    const BitStringDescriptorProfileV1& profile,
    std::span<const byte> encoded) noexcept;

struct BitStringZoneMapRequestV1 {
  const BitStringDescriptorProfileV1* profile = nullptr;
  const BitStringValueViewV1* minimum = nullptr;
  const BitStringValueViewV1* maximum = nullptr;
  u64 null_count = 0;
  u64 present_count = 0;
  u64 empty_present_count = 0;
  BitStringNullModeV1 null_mode = BitStringNullModeV1::nulls_first;
  u64 provider_max_key_bytes = ~u64{0};
  BitStringExecutionControlV1 control;
};
struct BitStringZoneMapViewV1 {
  const BitStringDescriptorProfileV1* profile = nullptr;
  BitStringNullModeV1 null_mode = BitStringNullModeV1::nulls_first;
  u64 null_count = 0;
  u64 present_count = 0;
  u64 empty_present_count = 0;
  u32 minimum_logical_bits = 0;
  u32 maximum_logical_bits = 0;
  std::span<const byte> minimum_sort_key;
  std::span<const byte> maximum_sort_key;
};
struct BitStringZoneMapViewResultV1 {
  Status status;
  struct {
    Status status;
    std::string_view diagnostic_code;
    std::string_view detail;
  } diagnostic;
  BitStringZoneMapViewV1 value;
  bool ok() const noexcept { return status.ok(); }
};
BitStringBytesResultV1 EncodeBitStringZoneMapV1(
    const BitStringZoneMapRequestV1& request) noexcept;
BitStringZoneMapViewResultV1 DecodeBitStringZoneMapNoAllocV1(
    const BitStringDescriptorProfileV1& profile,
    std::span<const byte> encoded) noexcept;

inline constexpr std::array<u32, 22> kBitStringLengthHistogramBoundsV1{
    0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 64, 256, 1024, 4096,
    16384, 65536, 262144, 1048576, 4194304, 16777216};

struct BitStringStatisticsMcvV1 {
  std::array<byte, 32> cohort_hash{};
  u64 frequency = 0;
  u32 logical_bits = 0;
};
struct BitStringStatisticsProjectionV1 {
  const BitStringDescriptorProfileV1* profile = nullptr;
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
  std::span<const BitStringStatisticsMcvV1> mcv;
  std::int64_t correlation_scaled_1e9 = INT64_MIN;
  platform::Uuid provider_evidence_uuid;
};
BitStringBytesResultV1 EncodeBitStringStatisticsProjectionV1(
    const BitStringStatisticsProjectionV1& projection) noexcept;

struct BitStringDecodedStatisticsV1 {
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
  std::vector<BitStringStatisticsMcvV1> mcv;
  std::int64_t correlation_scaled_1e9 = INT64_MIN;
  platform::Uuid provider_evidence_uuid;
  platform::Uuid selectivity_model_uuid{};
  u64 selectivity_model_generation = 0;
  bool ordered_value_histogram_available = false;
};
struct BitStringStatisticsDecodeResultV1 {
  Status status;
  DiagnosticRecord diagnostic;
  BitStringDecodedStatisticsV1 statistics;
  bool ok() const noexcept { return status.ok(); }
};
BitStringStatisticsDecodeResultV1 DecodeBitStringStatisticsProjectionV1(
    const BitStringDescriptorProfileV1& profile,
    std::span<const byte> encoded) noexcept;

struct BitStringOrderedSelectivityAbsenceV1 {
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
constexpr BitStringOrderedSelectivityAbsenceV1
BitStringOrderedSelectivityAbsenceFactV1() noexcept { return {}; }

struct BitStringBackupTupleV1 {
  platform::Uuid catalog_snapshot_uuid;
  u64 catalog_generation = 0;
  u64 registry_generation = 0;
  std::array<byte, 32> profile_fingerprint{};
  BitStringValueStateV1 state = BitStringValueStateV1::present;
  u32 logical_bit_count = 0;
  std::span<const byte> packed_msb0;
};
struct BitStringBackupTupleResultV1 {
  Status status;
  DiagnosticRecord diagnostic;
  BitStringBackupTupleV1 tuple;
  bool ok() const noexcept { return status.ok(); }
};
BitStringBackupTupleResultV1 ProjectBitStringBackupTupleV1(
    const BitStringValueViewV1& value) noexcept;

struct BitStringOwnedBackupTupleV1 {
  platform::Uuid catalog_snapshot_uuid;
  u64 catalog_generation = 0;
  u64 registry_generation = 0;
  std::array<byte, 32> profile_fingerprint{};
  BitStringValueStateV1 state = BitStringValueStateV1::present;
  u32 logical_bit_count = 0;
  std::vector<byte> packed_msb0;
};
struct BitStringOwnedBackupTupleResultV1 {
  Status status;
  DiagnosticRecord diagnostic;
  BitStringOwnedBackupTupleV1 tuple;
  bool ok() const noexcept { return status.ok(); }
};
BitStringOwnedBackupTupleResultV1 EncodeBitStringBackupTupleV1(
    const BitStringValueViewV1& value,
    const BitStringExecutionControlV1& control = {}) noexcept;
BitStringResultV1 DecodeBitStringBackupTupleV1(
    const BitStringDescriptorProfileV1& profile,
    const BitStringOwnedBackupTupleV1& tuple, bool null_allowed,
    const BitStringExecutionControlV1& control = {}) noexcept;

enum class BitStringProtectionCellV1 : std::uint8_t {
  canonical_plain, inner_compression, inner_encryption, outer_compression,
  outer_authenticated_protection, outer_compress_then_protect,
  outer_protect_then_compress, page_or_filespace_crypto, transport_tls,
  protected_index,
};
struct BitStringProtectionResolutionV1 {
  BitStringProtectionCellV1 cell = BitStringProtectionCellV1::canonical_plain;
  bool datatype_admitted = false;
  bool owner_outside_datatype = false;
  std::string_view disposition;
  std::string_view diagnostic;
};
BitStringProtectionResolutionV1 ResolveBitStringProtectionV1(
    BitStringProtectionCellV1 cell) noexcept;

enum class BitStringWireLaneV1 : std::uint8_t {
  native_sbwp, parser_server_ipc, canonical_sblr, apache_ignite, cassandra,
  clickhouse, cockroachdb, dolt, duckdb, firebird, foundationdb, immudb,
  influxdb, mariadb, milvus, mongodb, mysql, neo4j, opensearch, postgresql,
  redis, sqlite, tidb, tikv, vitess, xtdb, yugabytedb,
};
struct BitStringWireLaneResolutionV1 {
  BitStringWireLaneV1 lane = BitStringWireLaneV1::native_sbwp;
  bool admitted = false;
  bool native_candidate = false;
  std::string_view disposition;
  std::string_view diagnostic = "CTB.TRANSPORT.UNSUPPORTED";
};
BitStringWireLaneResolutionV1 ResolveBitStringWireLaneV1(
    BitStringWireLaneV1 lane) noexcept;

}  // namespace scratchbird::core::datatypes
