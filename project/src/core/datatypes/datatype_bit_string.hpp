// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "datatype_operations.hpp"
#include "datatype_type_codec_identity_v3.hpp"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::core::datatypes {

using platform::u8;

inline constexpr u32 kBitStringMaximumLogicalBitsV3 = 16'777'216;
inline constexpr u32 kBitStringProfileMaterialBytesV3 = 432;
inline constexpr u32 kBitStringComparisonMaterialBytesV3 = 216;
inline constexpr u32 kBitStringSortKeyHeaderBytesV3 = 104;
inline constexpr u32 kBitStringMaximumSortKeyBytesV3 = 16'777'321;
inline constexpr u32 kBitStringClosedCastPolicyRowsV3 = 221;

struct BitStringAuthorityReceiptV3 {
  platform::Uuid statement_receipt_uuid;
  platform::Uuid catalog_snapshot_uuid;
  u64 catalog_generation = 0;
  u64 registry_generation = 0;
};

struct BitStringDescriptorProfileV3 {
  BitStringAuthorityReceiptV3 receipt;
  DatatypeTypeCodecIdentityRowV3 identity;
  u32 length_bits = 0;
  bool fixed_length = false;
  DatatypePolicyIdentityV3 padding;
  DatatypePolicyIdentityV3 render;
  DatatypePolicyIdentityV3 cast;
  DatatypePolicyIdentityV3 boolean_alias;
  DatatypePolicyIdentityV3 no_auto_lob;
  DatatypePolicyIdentityV3 index;
  DatatypePolicyIdentityV3 statistics;
  DatatypePolicyIdentityV3 backup_transport;
  DatatypePolicyIdentityV3 protection;
  std::array<byte, kBitStringProfileMaterialBytesV3> canonical_profile_material{};
  std::array<byte, 32> profile_fingerprint{};
  std::array<byte, 32> comparison_cohort_fingerprint{};
};

enum class BitStringSurfaceProfileKindV3 : u8 {
  unqualified = 0,
  fixed = 1,
  varying = 2,
};

struct BitStringProfileRequestV3 {
  BitStringAuthorityReceiptV3 receipt;
  DatatypeTypeCodecIdentityRowV3 identity;
  BitStringSurfaceProfileKindV3 kind =
      BitStringSurfaceProfileKindV3::unqualified;
  u32 length_bits = kBitStringMaximumLogicalBitsV3;
};

enum class BitStringValueStateV3 : u8 { present = 0, sql_null = 1 };
enum class BitStringOwnershipV3 : u8 { borrowed = 0, owned = 1 };

struct BitStringValueViewV3 {
  const BitStringDescriptorProfileV3* profile = nullptr;
  BitStringValueStateV3 state = BitStringValueStateV3::present;
  u32 logical_bit_count = 0;
  std::span<const byte> packed_msb0;
  BitStringOwnershipV3 ownership = BitStringOwnershipV3::borrowed;
};

struct BitStringOwnedValueV3 {
  BitStringDescriptorProfileV3 profile;
  BitStringValueStateV3 state = BitStringValueStateV3::present;
  u32 logical_bit_count = 0;
  std::vector<byte> packed_msb0;

  BitStringValueViewV3 view() const noexcept {
    return {&profile, state, logical_bit_count, packed_msb0,
            BitStringOwnershipV3::owned};
  }
};

struct BitStringExecutionControlV3 {
  u64 maximum_allocation_bytes = ~u64{0};
  bool (*cancelled)(void*) noexcept = nullptr;
  void* cancellation_context = nullptr;
};

struct BitStringResultV3 {
  Status status;
  DiagnosticRecord diagnostic;
  BitStringOwnedValueV3 value;
  bool ok() const noexcept { return status.ok(); }
};

struct BitStringViewResultV3 {
  Status status;
  struct {
    Status status;
    std::string_view diagnostic_code;
    std::string_view detail;
  } diagnostic;
  BitStringValueViewV3 value;
  bool ok() const noexcept { return status.ok(); }
};

struct BitStringProfileResultV3 {
  Status status;
  DiagnosticRecord diagnostic;
  BitStringDescriptorProfileV3 profile;
  bool ok() const noexcept { return status.ok(); }
};

struct BitStringBytesResultV3 {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<byte> bytes;
  bool ok() const noexcept { return status.ok(); }
};

BitStringProfileResultV3 BuildBitStringDescriptorProfileV3(
    const BitStringProfileRequestV3& request) noexcept;
BitStringProfileResultV3 DecodeBitStringDescriptorProfileMaterialV3(
    const BitStringAuthorityReceiptV3& receipt,
    const DatatypeTypeCodecIdentityRowV3& expected_identity,
    std::span<const byte> material) noexcept;
BitStringProfileResultV3 ValidateBitStringDescriptorProfileV3(
    const BitStringDescriptorProfileV3& profile) noexcept;

BitStringViewResultV3 ValidateBitStringValueViewV3(
    const BitStringValueViewV3& value, bool null_allowed) noexcept;
BitStringViewResultV3 DecodeCanonicalBitStringComponentNoAllocV3(
    const BitStringDescriptorProfileV3& profile, BitStringValueStateV3 state,
    bool null_allowed, std::span<const byte> component) noexcept;
BitStringBytesResultV3 EncodeCanonicalBitStringComponentV3(
    const BitStringValueViewV3& value,
    const BitStringExecutionControlV3& control = {}) noexcept;
BitStringResultV3 MaterializeBitStringValueV3(
    const BitStringValueViewV3& value, bool null_allowed,
    const BitStringExecutionControlV3& control = {}) noexcept;

enum class BitStringIntrinsicOperationV3 : u8 {
  validate,
  logical_length,
  at,
  set,
  count,
  concatenate,
  slice,
  bitwise_not,
  bitwise_and,
  bitwise_or,
  bitwise_xor,
  shift_left,
  shift_right,
};

struct BitStringScalarResultV3 {
  Status status;
  DiagnosticRecord diagnostic;
  bool is_null = false;
  u64 unsigned_value = 0;
  bool boolean_value = false;
  bool ok() const noexcept { return status.ok(); }
};

BitStringScalarResultV3 BitStringLogicalLengthV3(
    const BitStringValueViewV3& value, bool null_allowed) noexcept;
BitStringScalarResultV3 BitStringAtV3(const BitStringValueViewV3& value,
                                     bool null_allowed, u64 index) noexcept;
BitStringScalarResultV3 BitStringCountV3(
    const BitStringValueViewV3& value, bool null_allowed,
    const BitStringExecutionControlV3& control = {}) noexcept;
BitStringResultV3 BitStringSetV3(
    const BitStringValueViewV3& value, bool null_allowed, u64 index,
    bool bit, const BitStringExecutionControlV3& control = {}) noexcept;
BitStringResultV3 BitStringConcatenateV3(
    const BitStringValueViewV3& left, const BitStringValueViewV3& right,
    const BitStringDescriptorProfileV3& target_profile, bool null_allowed,
    const BitStringExecutionControlV3& control = {}) noexcept;
BitStringResultV3 BitStringSliceV3(
    const BitStringValueViewV3& value,
    const BitStringDescriptorProfileV3& target_profile, bool null_allowed,
    u64 start, u64 count,
    const BitStringExecutionControlV3& control = {}) noexcept;
BitStringResultV3 BitStringNotV3(
    const BitStringValueViewV3& value, bool null_allowed,
    const BitStringExecutionControlV3& control = {}) noexcept;
BitStringResultV3 BitStringAndV3(
    const BitStringValueViewV3& left, const BitStringValueViewV3& right,
    bool null_allowed,
    const BitStringExecutionControlV3& control = {}) noexcept;
BitStringResultV3 BitStringOrV3(
    const BitStringValueViewV3& left, const BitStringValueViewV3& right,
    bool null_allowed,
    const BitStringExecutionControlV3& control = {}) noexcept;
BitStringResultV3 BitStringXorV3(
    const BitStringValueViewV3& left, const BitStringValueViewV3& right,
    bool null_allowed,
    const BitStringExecutionControlV3& control = {}) noexcept;
BitStringResultV3 BitStringShiftLeftV3(
    const BitStringValueViewV3& value, bool null_allowed, u64 count,
    const BitStringExecutionControlV3& control = {}) noexcept;
BitStringResultV3 BitStringShiftRightV3(
    const BitStringValueViewV3& value, bool null_allowed, u64 count,
    const BitStringExecutionControlV3& control = {}) noexcept;

struct BitStringSearchResultV3 {
  Status status;
  struct {
    Status status;
    std::string_view diagnostic_code;
    std::string_view detail;
  } diagnostic;
  bool is_null = false;
  bool found = false;
  u64 zero_based_position = 0;
  bool ok() const noexcept { return status.ok(); }
};

// Allocation-free logical search. Empty needles are refused. Later public
// position owners convert a found zero-based position to exact int64 one-based
// form and use zero for absence; this primitive registers no public function.
BitStringSearchResultV3 SearchBitStringValueNoAllocV3(
    const BitStringValueViewV3& haystack,
    const BitStringValueViewV3& needle, bool null_allowed,
    const BitStringExecutionControlV3& control = {}) noexcept;

enum class BitStringComparisonModeV3 : u8 {
  scalar_3vl,
  distinct,
  grouping,
  ordered,
  present_only,
};
enum class BitStringSortDirectionV3 : u8 { ascending = 0, descending = 1 };
enum class BitStringNullModeV3 : u8 { nulls_first = 0, nulls_last = 1 };

struct BitStringComparisonResultV3 {
  Status status;
  DiagnosticRecord diagnostic;
  bool is_null = false;
  bool boolean_value = false;
  int comparison = 0;
  bool ok() const noexcept { return status.ok(); }
};

BitStringComparisonResultV3 CompareBitStringValuesV3(
    const BitStringValueViewV3& left, const BitStringValueViewV3& right,
    BitStringComparisonModeV3 mode) noexcept;
BitStringBytesResultV3 HashBitStringValueV3(
    const BitStringValueViewV3& value) noexcept;
BitStringBytesResultV3 MakeBitStringSortKeyV3(
    const BitStringValueViewV3& value, BitStringSortDirectionV3 direction,
    BitStringNullModeV3 null_mode,
    const BitStringExecutionControlV3& control = {}) noexcept;

struct BitStringSortKeyViewV3 {
  const BitStringDescriptorProfileV3* profile = nullptr;
  BitStringSortDirectionV3 direction = BitStringSortDirectionV3::ascending;
  BitStringNullModeV3 null_mode = BitStringNullModeV3::nulls_first;
  BitStringValueStateV3 state = BitStringValueStateV3::present;
  u32 logical_bit_count = 0;
  std::span<const byte> encoded_suffix;
};
struct BitStringSortKeyViewResultV3 {
  Status status;
  struct {
    Status status;
    std::string_view diagnostic_code;
    std::string_view detail;
  } diagnostic;
  BitStringSortKeyViewV3 value;
  bool ok() const noexcept { return status.ok(); }
};
BitStringSortKeyViewResultV3 DecodeBitStringSortKeyNoAllocV3(
    const BitStringDescriptorProfileV3& expected_profile,
    std::span<const byte> encoded) noexcept;

struct BitStringRenderResultV3 {
  Status status;
  DiagnosticRecord diagnostic;
  std::string text;
  bool containing_null = false;
  bool ok() const noexcept { return status.ok(); }
};
BitStringRenderResultV3 RenderBitStringValueV3(
    const BitStringValueViewV3& value, bool export_literal,
    const BitStringExecutionControlV3& control = {}) noexcept;

struct BitStringCastRequestV3 {
  const BitStringValueViewV3* bit_source = nullptr;
  const DatatypeOperationValue* scalar_source = nullptr;
  const BitStringDescriptorProfileV3* bit_target = nullptr;
  CanonicalTypeId scalar_target = CanonicalTypeId::unknown;
  scratchbird::engine::ExecutionTypeDescriptor scalar_target_descriptor;
  DatatypeCastContext context = DatatypeCastContext::implicit;
  bool target_null_allowed = true;
  BitStringExecutionControlV3 control;
};

struct BitStringCastResultV3 {
  Status status;
  DiagnosticRecord diagnostic;
  DatatypeCastCategory category = DatatypeCastCategory::forbidden;
  bool produced_bit_string = false;
  BitStringOwnedValueV3 bit_value;
  DatatypeOperationValue scalar_value;
  bool ok() const noexcept { return status.ok(); }
};
BitStringCastResultV3 CastBitStringValueV3(
    const BitStringCastRequestV3& request) noexcept;

// The composed adapters are the only admitted semantic use of type code 302.
// They accept an exact live profile and wrap or unwrap the unchanged structural
// SBDVAL01 frame. Generic SBDV1/SBDTV001 APIs remain refused.
BitStringViewResultV3 DecodeBitStringSbdvalComposedNoAllocV3(
    const BitStringDescriptorProfileV3& profile, bool null_allowed,
    std::span<const byte> encoded) noexcept;
BitStringBytesResultV3 EncodeBitStringSbdvalComposedV3(
    const BitStringValueViewV3& value, bool null_allowed,
    const BitStringExecutionControlV3& control = {}) noexcept;
BitStringViewResultV3 DecodeBitStringSbdpvComposedNoAllocV3(
    const BitStringDescriptorProfileV3& profile, bool null_allowed,
    std::span<const byte> encoded) noexcept;
BitStringBytesResultV3 EncodeBitStringSbdpvComposedV3(
    const BitStringValueViewV3& value, bool null_allowed,
    const BitStringExecutionControlV3& control = {}) noexcept;

enum class BitStringMetricV3 : u8 {
  descriptor_admissions,
  descriptor_refusals,
  values_admitted,
  empty_values_admitted,
  logical_bits,
  logical_length,
  owned_bytes,
  length_refusals,
  padding_refusals,
  canonical_encoding_refusals,
  index_admission_refusals,
  statistics_stale,
  compatibility_mapping_misses,
  transport_refusals,
  serialization_refusals,
  protection_refusals,
  merge_manual_review,
  operation_attempts,
  operation_success,
  operation_refusals,
  operation_input_bits,
};

enum class BitStringMetricUpdateV3 : u8 {
  counter_add,
  histogram_observe,
  gauge_set,
};
enum class BitStringMetricValueStateLabelV3 : u8 {
  present, sql_null, not_applicable = 0xff,
};
enum class BitStringMetricResultLabelV3 : u8 {
  admitted, not_applicable = 0xff,
};
enum class BitStringMetricAllocationClassV3 : u8 {
  owned_value, decode_scratch, encode_scratch, key_scratch,
  statistics_scratch, not_applicable = 0xff,
};
enum class BitStringMetricOperationV3 : u8 {
  validate, canonicalize, logical_bit_length, bit_at_zero_based, concatenate,
  contained_slice_zero_based, bitwise_not, equal_length_bitwise_and,
  equal_length_bitwise_or, equal_length_bitwise_xor, compare, hash, sort_key,
  cast, serialize, deserialize, bit_set_zero_based, bit_count,
  shift_left_uint64, shift_right_uint64, cast_character, cast_integer,
  not_applicable = 0xff,
};
enum class BitStringMetricReasonV3 : u8 {
  absent, stale, multiple, mismatch, short_value, trailing, count_extent,
  dirty_tail, maximum, padding, policy, provider_budget, profile_missing,
  unsupported, privacy, epoch, manual_review, not_applicable = 0xff,
};
enum class BitStringMetricIndexFamilyV3 : u8 {
  aggregate_sketch, bitmap, brin_like, btree, columnar_zone_map,
  covering_included, document_path, expression, full_text, graph, hash,
  partial_filtered, range_exclusion, spatial, temporary_work, vector_ann,
  not_applicable = 0xff,
};
enum class BitStringMetricLaneClassV3 : u8 {
  native_sbwp, parser_server_ipc, canonical_sblr,
  reference_engine_compatibility, not_applicable = 0xff,
};
enum class BitStringMetricBoundaryV3 : u8 {
  component, sblr, parser_server_ipc, native_sbwp, backup, restore,
  replication, cluster, index, statistics, not_applicable = 0xff,
};
enum class BitStringMetricLayerV3 : u8 {
  canonical, inner_compression, inner_encryption, outer_compression,
  outer_protection, protected_index, not_applicable = 0xff,
};

struct BitStringMetricRecordV3 {
  BitStringMetricV3 metric = BitStringMetricV3::operation_attempts;
  BitStringMetricUpdateV3 update = BitStringMetricUpdateV3::counter_add;
  platform::Uuid database_uuid;
  platform::Uuid node_uuid;
  u64 value = 0;
  bool represented_event_committed = false;
  bool cluster_series = false;
  BitStringMetricResultLabelV3 result =
      BitStringMetricResultLabelV3::not_applicable;
  BitStringMetricValueStateLabelV3 value_state =
      BitStringMetricValueStateLabelV3::not_applicable;
  BitStringMetricAllocationClassV3 allocation_class =
      BitStringMetricAllocationClassV3::not_applicable;
  BitStringMetricOperationV3 operation =
      BitStringMetricOperationV3::not_applicable;
  BitStringMetricReasonV3 reason = BitStringMetricReasonV3::not_applicable;
  BitStringMetricIndexFamilyV3 index_family =
      BitStringMetricIndexFamilyV3::not_applicable;
  BitStringMetricLaneClassV3 lane_class =
      BitStringMetricLaneClassV3::not_applicable;
  BitStringMetricBoundaryV3 boundary =
      BitStringMetricBoundaryV3::not_applicable;
  BitStringMetricLayerV3 layer = BitStringMetricLayerV3::not_applicable;
};

enum class BitStringMetricRecordDispositionV3 : u8 {
  recorded,
  event_not_committed,
  identity_invalid,
  update_invalid,
  label_combination_invalid,
  cluster_series_forbidden,
  recursive_emission_suppressed,
  sink_unavailable,
  sink_failure_isolated,
};
using BitStringMetricSinkV3 = bool (*)(const BitStringMetricRecordV3&,
                                      void*) noexcept;
void SetBitStringMetricSinkV3(BitStringMetricSinkV3 sink,
                              void* context) noexcept;
BitStringMetricRecordDispositionV3 RecordBitStringMetricAfterCommitV3(
    const BitStringMetricRecordV3& record) noexcept;

DiagnosticRecord MakeBitStringDiagnosticV3(Status status,
                                           std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail = {});

}  // namespace scratchbird::core::datatypes
