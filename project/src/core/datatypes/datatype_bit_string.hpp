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

inline constexpr u32 kBitStringMaximumLogicalBitsV1 = 16'777'216;
inline constexpr u32 kBitStringProfileMaterialBytesV1 = 432;
inline constexpr u32 kBitStringComparisonMaterialBytesV1 = 216;
inline constexpr u32 kBitStringSortKeyHeaderBytesV1 = 104;
inline constexpr u32 kBitStringMaximumSortKeyBytesV1 = 16'777'321;
inline constexpr u32 kBitStringClosedCastPolicyRowsV1 = 221;

struct BitStringAuthorityReceiptV1 {
  platform::Uuid statement_receipt_uuid;
  platform::Uuid catalog_snapshot_uuid;
  u64 catalog_generation = 0;
  u64 registry_generation = 0;
};

struct BitStringDescriptorProfileV1 {
  BitStringAuthorityReceiptV1 receipt;
  DatatypeTypeCodecIdentityRowV3 identity;
  u32 length_bits = 0;
  bool fixed_length = false;
  DatatypePolicyIdentityV1 padding;
  DatatypePolicyIdentityV1 render;
  DatatypePolicyIdentityV1 cast;
  DatatypePolicyIdentityV1 boolean_alias;
  DatatypePolicyIdentityV1 no_auto_lob;
  DatatypePolicyIdentityV1 index;
  DatatypePolicyIdentityV1 statistics;
  DatatypePolicyIdentityV1 backup_transport;
  DatatypePolicyIdentityV1 protection;
  std::array<byte, kBitStringProfileMaterialBytesV1> canonical_profile_material{};
  std::array<byte, 32> profile_fingerprint{};
  std::array<byte, 32> comparison_cohort_fingerprint{};
};

enum class BitStringSurfaceProfileKindV1 : u8 {
  unqualified = 0,
  fixed = 1,
  varying = 2,
};

struct BitStringProfileRequestV1 {
  BitStringAuthorityReceiptV1 receipt;
  DatatypeTypeCodecIdentityRowV3 identity;
  BitStringSurfaceProfileKindV1 kind =
      BitStringSurfaceProfileKindV1::unqualified;
  u32 length_bits = kBitStringMaximumLogicalBitsV1;
};

enum class BitStringValueStateV1 : u8 { present = 0, sql_null = 1 };
enum class BitStringOwnershipV1 : u8 { borrowed = 0, owned = 1 };

struct BitStringValueViewV1 {
  const BitStringDescriptorProfileV1* profile = nullptr;
  BitStringValueStateV1 state = BitStringValueStateV1::present;
  u32 logical_bit_count = 0;
  std::span<const byte> packed_msb0;
  BitStringOwnershipV1 ownership = BitStringOwnershipV1::borrowed;
};

struct BitStringOwnedValueV1 {
  BitStringDescriptorProfileV1 profile;
  BitStringValueStateV1 state = BitStringValueStateV1::present;
  u32 logical_bit_count = 0;
  std::vector<byte> packed_msb0;

  BitStringValueViewV1 view() const noexcept {
    return {&profile, state, logical_bit_count, packed_msb0,
            BitStringOwnershipV1::owned};
  }
};

struct BitStringExecutionControlV1 {
  u64 maximum_allocation_bytes = ~u64{0};
  bool (*cancelled)(void*) noexcept = nullptr;
  void* cancellation_context = nullptr;
};

struct BitStringResultV1 {
  Status status;
  DiagnosticRecord diagnostic;
  BitStringOwnedValueV1 value;
  bool ok() const noexcept { return status.ok(); }
};

struct BitStringViewResultV1 {
  Status status;
  struct {
    Status status;
    std::string_view diagnostic_code;
    std::string_view detail;
  } diagnostic;
  BitStringValueViewV1 value;
  bool ok() const noexcept { return status.ok(); }
};

struct BitStringProfileResultV1 {
  Status status;
  DiagnosticRecord diagnostic;
  BitStringDescriptorProfileV1 profile;
  bool ok() const noexcept { return status.ok(); }
};

struct BitStringBytesResultV1 {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<byte> bytes;
  bool ok() const noexcept { return status.ok(); }
};

BitStringProfileResultV1 BuildBitStringDescriptorProfileV1(
    const BitStringProfileRequestV1& request) noexcept;
BitStringProfileResultV1 DecodeBitStringDescriptorProfileMaterialV1(
    const BitStringAuthorityReceiptV1& receipt,
    const DatatypeTypeCodecIdentityRowV3& expected_identity,
    std::span<const byte> material) noexcept;
BitStringProfileResultV1 ValidateBitStringDescriptorProfileV1(
    const BitStringDescriptorProfileV1& profile) noexcept;

BitStringViewResultV1 ValidateBitStringValueViewV1(
    const BitStringValueViewV1& value, bool null_allowed) noexcept;
BitStringViewResultV1 DecodeCanonicalBitStringComponentNoAllocV1(
    const BitStringDescriptorProfileV1& profile, BitStringValueStateV1 state,
    bool null_allowed, std::span<const byte> component) noexcept;
BitStringBytesResultV1 EncodeCanonicalBitStringComponentV1(
    const BitStringValueViewV1& value,
    const BitStringExecutionControlV1& control = {}) noexcept;
BitStringResultV1 MaterializeBitStringValueV1(
    const BitStringValueViewV1& value, bool null_allowed,
    const BitStringExecutionControlV1& control = {}) noexcept;

enum class BitStringIntrinsicOperationV1 : u8 {
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

struct BitStringScalarResultV1 {
  Status status;
  DiagnosticRecord diagnostic;
  bool is_null = false;
  u64 unsigned_value = 0;
  bool boolean_value = false;
  bool ok() const noexcept { return status.ok(); }
};

BitStringScalarResultV1 BitStringLogicalLengthV1(
    const BitStringValueViewV1& value, bool null_allowed) noexcept;
BitStringScalarResultV1 BitStringAtV1(const BitStringValueViewV1& value,
                                     bool null_allowed, u64 index) noexcept;
BitStringScalarResultV1 BitStringCountV1(
    const BitStringValueViewV1& value, bool null_allowed,
    const BitStringExecutionControlV1& control = {}) noexcept;
BitStringResultV1 BitStringSetV1(
    const BitStringValueViewV1& value, bool null_allowed, u64 index,
    bool bit, const BitStringExecutionControlV1& control = {}) noexcept;
BitStringResultV1 BitStringConcatenateV1(
    const BitStringValueViewV1& left, const BitStringValueViewV1& right,
    const BitStringDescriptorProfileV1& target_profile, bool null_allowed,
    const BitStringExecutionControlV1& control = {}) noexcept;
BitStringResultV1 BitStringSliceV1(
    const BitStringValueViewV1& value,
    const BitStringDescriptorProfileV1& target_profile, bool null_allowed,
    u64 start, u64 count,
    const BitStringExecutionControlV1& control = {}) noexcept;
BitStringResultV1 BitStringNotV1(
    const BitStringValueViewV1& value, bool null_allowed,
    const BitStringExecutionControlV1& control = {}) noexcept;
BitStringResultV1 BitStringAndV1(
    const BitStringValueViewV1& left, const BitStringValueViewV1& right,
    bool null_allowed,
    const BitStringExecutionControlV1& control = {}) noexcept;
BitStringResultV1 BitStringOrV1(
    const BitStringValueViewV1& left, const BitStringValueViewV1& right,
    bool null_allowed,
    const BitStringExecutionControlV1& control = {}) noexcept;
BitStringResultV1 BitStringXorV1(
    const BitStringValueViewV1& left, const BitStringValueViewV1& right,
    bool null_allowed,
    const BitStringExecutionControlV1& control = {}) noexcept;
BitStringResultV1 BitStringShiftLeftV1(
    const BitStringValueViewV1& value, bool null_allowed, u64 count,
    const BitStringExecutionControlV1& control = {}) noexcept;
BitStringResultV1 BitStringShiftRightV1(
    const BitStringValueViewV1& value, bool null_allowed, u64 count,
    const BitStringExecutionControlV1& control = {}) noexcept;

struct BitStringSearchResultV1 {
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
BitStringSearchResultV1 SearchBitStringValueNoAllocV1(
    const BitStringValueViewV1& haystack,
    const BitStringValueViewV1& needle, bool null_allowed,
    const BitStringExecutionControlV1& control = {}) noexcept;

enum class BitStringComparisonModeV1 : u8 {
  scalar_3vl,
  distinct,
  grouping,
  ordered,
  present_only,
};
enum class BitStringSortDirectionV1 : u8 { ascending = 0, descending = 1 };
enum class BitStringNullModeV1 : u8 { nulls_first = 0, nulls_last = 1 };

struct BitStringComparisonResultV1 {
  Status status;
  DiagnosticRecord diagnostic;
  bool is_null = false;
  bool boolean_value = false;
  int comparison = 0;
  bool ok() const noexcept { return status.ok(); }
};

BitStringComparisonResultV1 CompareBitStringValuesV1(
    const BitStringValueViewV1& left, const BitStringValueViewV1& right,
    BitStringComparisonModeV1 mode) noexcept;
BitStringBytesResultV1 HashBitStringValueV1(
    const BitStringValueViewV1& value) noexcept;
BitStringBytesResultV1 MakeBitStringSortKeyV1(
    const BitStringValueViewV1& value, BitStringSortDirectionV1 direction,
    BitStringNullModeV1 null_mode,
    const BitStringExecutionControlV1& control = {}) noexcept;

struct BitStringSortKeyViewV1 {
  const BitStringDescriptorProfileV1* profile = nullptr;
  BitStringSortDirectionV1 direction = BitStringSortDirectionV1::ascending;
  BitStringNullModeV1 null_mode = BitStringNullModeV1::nulls_first;
  BitStringValueStateV1 state = BitStringValueStateV1::present;
  u32 logical_bit_count = 0;
  std::span<const byte> encoded_suffix;
};
struct BitStringSortKeyViewResultV1 {
  Status status;
  struct {
    Status status;
    std::string_view diagnostic_code;
    std::string_view detail;
  } diagnostic;
  BitStringSortKeyViewV1 value;
  bool ok() const noexcept { return status.ok(); }
};
BitStringSortKeyViewResultV1 DecodeBitStringSortKeyNoAllocV1(
    const BitStringDescriptorProfileV1& expected_profile,
    std::span<const byte> encoded) noexcept;

struct BitStringRenderResultV1 {
  Status status;
  DiagnosticRecord diagnostic;
  std::string text;
  bool containing_null = false;
  bool ok() const noexcept { return status.ok(); }
};
BitStringRenderResultV1 RenderBitStringValueV1(
    const BitStringValueViewV1& value, bool export_literal,
    const BitStringExecutionControlV1& control = {}) noexcept;

struct BitStringCastRequestV1 {
  const BitStringValueViewV1* bit_source = nullptr;
  const DatatypeOperationValue* scalar_source = nullptr;
  const BitStringDescriptorProfileV1* bit_target = nullptr;
  CanonicalTypeId scalar_target = CanonicalTypeId::unknown;
  scratchbird::engine::ExecutionTypeDescriptor scalar_target_descriptor;
  DatatypeCastContext context = DatatypeCastContext::implicit;
  bool target_null_allowed = true;
  BitStringExecutionControlV1 control;
};

struct BitStringCastResultV1 {
  Status status;
  DiagnosticRecord diagnostic;
  DatatypeCastCategory category = DatatypeCastCategory::forbidden;
  bool produced_bit_string = false;
  BitStringOwnedValueV1 bit_value;
  DatatypeOperationValue scalar_value;
  bool ok() const noexcept { return status.ok(); }
};
BitStringCastResultV1 CastBitStringValueV1(
    const BitStringCastRequestV1& request) noexcept;

// The composed adapters are the only admitted semantic use of type code 302.
// They accept an exact live profile and wrap or unwrap the unchanged structural
// SBDVAL01 frame. Generic SBDV1/SBDTV001 APIs remain refused.
BitStringViewResultV1 DecodeBitStringSbdvalComposedNoAllocV1(
    const BitStringDescriptorProfileV1& profile, bool null_allowed,
    std::span<const byte> encoded) noexcept;
BitStringBytesResultV1 EncodeBitStringSbdvalComposedV1(
    const BitStringValueViewV1& value, bool null_allowed,
    const BitStringExecutionControlV1& control = {}) noexcept;
BitStringViewResultV1 DecodeBitStringSbdpvComposedNoAllocV1(
    const BitStringDescriptorProfileV1& profile, bool null_allowed,
    std::span<const byte> encoded) noexcept;
BitStringBytesResultV1 EncodeBitStringSbdpvComposedV1(
    const BitStringValueViewV1& value, bool null_allowed,
    const BitStringExecutionControlV1& control = {}) noexcept;

enum class BitStringMetricV1 : u8 {
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

enum class BitStringMetricUpdateV1 : u8 {
  counter_add,
  histogram_observe,
  gauge_set,
};
enum class BitStringMetricValueStateLabelV1 : u8 {
  present, sql_null, not_applicable = 0xff,
};
enum class BitStringMetricResultLabelV1 : u8 {
  admitted, not_applicable = 0xff,
};
enum class BitStringMetricAllocationClassV1 : u8 {
  owned_value, decode_scratch, encode_scratch, key_scratch,
  statistics_scratch, not_applicable = 0xff,
};
enum class BitStringMetricOperationV1 : u8 {
  validate, canonicalize, logical_bit_length, bit_at_zero_based, concatenate,
  contained_slice_zero_based, bitwise_not, equal_length_bitwise_and,
  equal_length_bitwise_or, equal_length_bitwise_xor, compare, hash, sort_key,
  cast, serialize, deserialize, bit_set_zero_based, bit_count,
  shift_left_uint64, shift_right_uint64, cast_character, cast_integer,
  not_applicable = 0xff,
};
enum class BitStringMetricReasonV1 : u8 {
  absent, stale, multiple, mismatch, short_value, trailing, count_extent,
  dirty_tail, maximum, padding, policy, provider_budget, profile_missing,
  unsupported, privacy, epoch, manual_review, not_applicable = 0xff,
};
enum class BitStringMetricIndexFamilyV1 : u8 {
  aggregate_sketch, bitmap, brin_like, btree, columnar_zone_map,
  covering_included, document_path, expression, full_text, graph, hash,
  partial_filtered, range_exclusion, spatial, temporary_work, vector_ann,
  not_applicable = 0xff,
};
enum class BitStringMetricLaneClassV1 : u8 {
  native_sbwp, parser_server_ipc, canonical_sblr,
  reference_engine_compatibility, not_applicable = 0xff,
};
enum class BitStringMetricBoundaryV1 : u8 {
  component, sblr, parser_server_ipc, native_sbwp, backup, restore,
  replication, cluster, index, statistics, not_applicable = 0xff,
};
enum class BitStringMetricLayerV1 : u8 {
  canonical, inner_compression, inner_encryption, outer_compression,
  outer_protection, protected_index, not_applicable = 0xff,
};

struct BitStringMetricRecordV1 {
  BitStringMetricV1 metric = BitStringMetricV1::operation_attempts;
  BitStringMetricUpdateV1 update = BitStringMetricUpdateV1::counter_add;
  platform::Uuid database_uuid;
  platform::Uuid node_uuid;
  u64 value = 0;
  bool represented_event_committed = false;
  bool cluster_series = false;
  BitStringMetricResultLabelV1 result =
      BitStringMetricResultLabelV1::not_applicable;
  BitStringMetricValueStateLabelV1 value_state =
      BitStringMetricValueStateLabelV1::not_applicable;
  BitStringMetricAllocationClassV1 allocation_class =
      BitStringMetricAllocationClassV1::not_applicable;
  BitStringMetricOperationV1 operation =
      BitStringMetricOperationV1::not_applicable;
  BitStringMetricReasonV1 reason = BitStringMetricReasonV1::not_applicable;
  BitStringMetricIndexFamilyV1 index_family =
      BitStringMetricIndexFamilyV1::not_applicable;
  BitStringMetricLaneClassV1 lane_class =
      BitStringMetricLaneClassV1::not_applicable;
  BitStringMetricBoundaryV1 boundary =
      BitStringMetricBoundaryV1::not_applicable;
  BitStringMetricLayerV1 layer = BitStringMetricLayerV1::not_applicable;
};

enum class BitStringMetricRecordDispositionV1 : u8 {
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
using BitStringMetricSinkV1 = bool (*)(const BitStringMetricRecordV1&,
                                      void*) noexcept;
void SetBitStringMetricSinkV1(BitStringMetricSinkV1 sink,
                              void* context) noexcept;
BitStringMetricRecordDispositionV1 RecordBitStringMetricAfterCommitV1(
    const BitStringMetricRecordV1& record) noexcept;

DiagnosticRecord MakeBitStringDiagnosticV1(Status status,
                                           std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail = {});

}  // namespace scratchbird::core::datatypes
