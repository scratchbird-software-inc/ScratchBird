// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "datatype_operations.hpp"
#include "datatype_type_codec_identity_v3.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace scratchbird::core::datatypes {

using platform::byte;
using platform::u8;
using platform::u32;
using platform::u64;

inline constexpr u32 kIntervalProfileMaterialBytesV3 = 608;
inline constexpr u32 kIntervalEqualityMaterialBytesV3 = 344;
inline constexpr u32 kIntervalComponentBytesV3 = 16;
inline constexpr u32 kIntervalHashBytesV3 = 32;
inline constexpr u32 kIntervalCanonicalIdentityCountV3 = 110;
inline constexpr u32 kIntervalUuidIncidentPairCountV3 = 219;
inline constexpr u32 kIntervalClosedCastPolicyRowsV3 = 221;
inline constexpr u32 kIntervalClosedCastDecisionsV3 = 663;
inline constexpr u32 kIntervalIntrinsicOperationRowsV3 = 13;
static_assert(kIntervalCanonicalIdentityCountV3 == 110);
static_assert(kIntervalUuidIncidentPairCountV3 == 219);
static_assert(kIntervalClosedCastPolicyRowsV3 == 221);
static_assert(kIntervalClosedCastDecisionsV3 == 663);
static_assert(kIntervalIntrinsicOperationRowsV3 == 13);

struct IntervalAuthorityReceiptV3 {
  platform::Uuid statement_receipt_uuid;
  platform::Uuid catalog_snapshot_uuid;
  u64 catalog_generation = 0;
  u64 registry_generation = 0;
};

struct IntervalValidatedProfileHandleV3 {
  IntervalAuthorityReceiptV3 receipt;
  DatatypeTypeCodecIdentityRowV3 identity;
  DatatypePolicyIdentityV3 render_policy, cast_policy, subtype_policy,
      precision_policy, normalization_policy,
      temporal_application_boundary_policy, storage_epoch_policy, index_policy,
      statistics_policy, backup_transport_policy, protection_policy,
      component_adapter_policy, diagnostic_policy, metric_policy;
  std::array<byte, kIntervalProfileMaterialBytesV3> profile_material{};
  std::array<byte, kIntervalEqualityMaterialBytesV3> equality_material{};
  std::array<byte, 32> profile_fingerprint{};
  std::array<byte, 32> equality_fingerprint{};
};

struct alignas(8) IntervalComponentV3 {
  std::int32_t months = 0;
  std::int32_t civil_days = 0;
  std::int64_t fixed_nanoseconds = 0;
};
static_assert(std::is_standard_layout_v<IntervalComponentV3>);
static_assert(std::is_trivially_copyable_v<IntervalComponentV3>);
static_assert(sizeof(IntervalComponentV3) == 16);
static_assert(alignof(IntervalComponentV3) == 8);
static_assert(offsetof(IntervalComponentV3, months) == 0);
static_assert(offsetof(IntervalComponentV3, civil_days) == 4);
static_assert(offsetof(IntervalComponentV3, fixed_nanoseconds) == 8);

enum class IntervalValueStateV3 : u8 { value = 0, sql_null = 1 };
enum class IntervalI32CarrierKindV3 : u8 {
  signed_i32 = 0, wrong_host_type = 1, wrong_host_boolean = 2,
  wrong_host_string = 3, below_i32 = 4, above_i32 = 5,
};
enum class IntervalI64CarrierKindV3 : u8 {
  signed_i64 = 0, wrong_host_type = 1, below_i64 = 2, above_i64 = 3,
};
enum class IntervalTextCarrierKindV3 : u8 { utf8_bytes = 0, wrong_host_type = 1 };

struct IntervalNullableI32FactV3 {
  IntervalI32CarrierKindV3 carrier = IntervalI32CarrierKindV3::signed_i32;
  IntervalValueStateV3 state = IntervalValueStateV3::value;
  std::int64_t value = 0;
};
struct IntervalNullableI64FactV3 {
  IntervalI64CarrierKindV3 carrier = IntervalI64CarrierKindV3::signed_i64;
  IntervalValueStateV3 state = IntervalValueStateV3::value;
  std::int64_t value = 0;
};
struct IntervalTextOperandV3 {
  const DatatypeTypeCodecIdentityRowV3* identity = nullptr;
  const scratchbird::engine::ExecutionTypeDescriptor* descriptor = nullptr;
  IntervalTextCarrierKindV3 carrier = IntervalTextCarrierKindV3::utf8_bytes;
  IntervalValueStateV3 state = IntervalValueStateV3::value;
  std::string_view bytes;
  u64 extent = 0;
};
struct IntervalValueViewV3 {
  const IntervalValidatedProfileHandleV3* profile = nullptr;
  IntervalValueStateV3 state = IntervalValueStateV3::value;
  std::int32_t months = 0;
  std::int32_t civil_days = 0;
  std::int64_t fixed_nanoseconds = 0;
};
struct IntervalOwnedValueV3 {
  std::shared_ptr<const IntervalValidatedProfileHandleV3> profile;
  IntervalValueStateV3 state = IntervalValueStateV3::value;
  std::int32_t months = 0;
  std::int32_t civil_days = 0;
  std::int64_t fixed_nanoseconds = 0;
  IntervalValueViewV3 view() const noexcept;
};
struct IntervalOperandV3 {
  std::shared_ptr<const IntervalValidatedProfileHandleV3> profile;
  IntervalValueStateV3 state = IntervalValueStateV3::value;
  IntervalI32CarrierKindV3 months_carrier = IntervalI32CarrierKindV3::signed_i32;
  std::int64_t months = 0;
  IntervalI32CarrierKindV3 civil_days_carrier = IntervalI32CarrierKindV3::signed_i32;
  std::int64_t civil_days = 0;
  IntervalI64CarrierKindV3 fixed_nanoseconds_carrier = IntervalI64CarrierKindV3::signed_i64;
  std::int64_t fixed_nanoseconds = 0;
};

enum class IntervalScrubClassV3 : u8 {
  sha_state, sha_schedule, sha_tail, profile_material, equality_material,
  profile_digest, equality_digest, component_decode_reencode,
  component_encode_staging, component_owned_buffer, render_staging,
  render_owned_buffer, hash_preimage, hash_digest, hash_owned_buffer,
  sbdval_reencode, sbdval_component_staging, sbdval_owned_buffer,
  sbdpv_reencode, sbdpv_component_staging, sbdpv_owned_buffer,
  equality_projection, covering_decode_reencode, statistics_decode_reencode,
  statistics_prior_hash, backup_decode_reencode, projection_owned_buffer,
  batch_month_staging, batch_day_staging, batch_nanosecond_staging,
  batch_bitmap_staging, count,
};
struct IntervalExecutionControlV3 {
  u64 maximum_allocation_bytes = ~u64{0};
  bool (*cancelled)(void*) noexcept = nullptr;
  void* cancellation_context = nullptr;
  void (*observe_scrubbed)(void*, IntervalScrubClassV3, const byte*, u64) noexcept = nullptr;
  void* scrub_observer_context = nullptr;
  bool force_reencode_mismatch_for_conformance = false;
};
enum class IntervalDiagnosticParameterKindV3 : u8 {
  none = 0, unsigned_u64 = 1, signed_i64 = 2, uuid = 3, token = 4,
};
struct IntervalDiagnosticParameterV3 {
  IntervalDiagnosticParameterKindV3 kind = IntervalDiagnosticParameterKindV3::none;
  std::string_view name;
  u64 unsigned_value = 0;
  std::int64_t signed_value = 0;
  platform::Uuid uuid_value;
  std::string_view token_value;
};
inline constexpr u8 kIntervalDiagnosticParameterCapacityV3 = 5;
struct IntervalDiagnosticFactV3 {
  Status status;
  std::string_view diagnostic_code;
  std::string_view detail;
  std::array<IntervalDiagnosticParameterV3, kIntervalDiagnosticParameterCapacityV3> parameters{};
  u8 parameter_count = 0;
};

#define SB_INTERVAL_RESULT(NAME, BODY) struct NAME { Status status; IntervalDiagnosticFactV3 diagnostic; BODY bool ok() const noexcept { return status.ok(); } }
SB_INTERVAL_RESULT(IntervalValidationResultV3, );
SB_INTERVAL_RESULT(IntervalProfileResultV3, IntervalValidatedProfileHandleV3 profile;);
SB_INTERVAL_RESULT(IntervalViewResultV3, IntervalValueViewV3 value;);
SB_INTERVAL_RESULT(IntervalValueResultV3, IntervalOwnedValueV3 value;);
SB_INTERVAL_RESULT(IntervalBytesResultV3, std::vector<byte> bytes;);
SB_INTERVAL_RESULT(IntervalTextResultV3, std::string text; bool containing_null = false;);
SB_INTERVAL_RESULT(IntervalNoAllocWriteResultV3, u64 bytes_required = 0; u64 bytes_written = 0; bool containing_null = false;);
SB_INTERVAL_RESULT(IntervalComponentResultV3, bool is_null = false; IntervalComponentV3 component;);
#undef SB_INTERVAL_RESULT

enum class IntervalScalarKindV3 : u8 { months_i32 = 0, civil_days_i32 = 1, fixed_nanoseconds_i64 = 2 };
struct IntervalScalarResultV3 {
  Status status; IntervalDiagnosticFactV3 diagnostic; bool is_null = false;
  IntervalScalarKindV3 kind = IntervalScalarKindV3::months_i32;
  std::int32_t signed_i32_value = 0; std::int64_t signed_i64_value = 0;
  bool ok() const noexcept { return status.ok(); }
};
enum class IntervalEqualityFactV3 : u8 { equal, not_equal, unordered_null };
struct IntervalEqualityResultV3 {
  Status status; IntervalDiagnosticFactV3 diagnostic;
  IntervalEqualityFactV3 fact = IntervalEqualityFactV3::not_equal;
  bool grouping_equivalent = false; bool null_equivalent = false;
  bool ok() const noexcept { return status.ok(); }
};

enum class IntervalRequiredAliasAuditCaseV3 : u8 {
  canonical_interval = 0, catalog_base_interval = 1,
  normative_mixed_interval = 2, inventory_spec_only_mixed_interval = 3,
  descriptive_mixed_component_interval = 4,
};
struct IntervalAliasResolutionEvidenceV3 {
  IntervalRequiredAliasAuditCaseV3 audit_case = IntervalRequiredAliasAuditCaseV3::canonical_interval;
  bool registry_resolution_succeeded = false;
  IntervalAuthorityReceiptV3 registry_receipt;
  DatatypeTypeCodecIdentityRowV3 resolved_identity;
};
enum class IntervalAliasAuditDispositionV3 : u8 {
  same_authenticated_interval_cohort = 0, registry_unresolved = 1,
  receipt_mismatch = 2, identity_mismatch = 3, profile_invalid = 4,
  unknown_audit_case = 5,
};
struct IntervalAliasAuditResultV3 {
  Status status; IntervalDiagnosticFactV3 diagnostic;
  IntervalAliasAuditDispositionV3 disposition = IntervalAliasAuditDispositionV3::registry_unresolved;
  bool same_interval_cohort = false; bool converter_required = false;
  bool runtime_name_authority_granted = false;
  bool ok() const noexcept { return status.ok(); }
};

IntervalAliasAuditResultV3 AuditIntervalRequiredAliasResolutionV3(const IntervalAliasResolutionEvidenceV3&, const IntervalValidatedProfileHandleV3&) noexcept;
IntervalProfileResultV3 BuildCurrentIntervalValidatedProfileHandleV3(const platform::Uuid&) noexcept;
IntervalProfileResultV3 BuildIntervalValidatedProfileHandleV3(const IntervalAuthorityReceiptV3&, const DatatypeTypeCodecIdentityRowV3&) noexcept;
IntervalValidationResultV3 ValidateIntervalProfileHandleV3(const IntervalValidatedProfileHandleV3&, const IntervalExecutionControlV3& = {}) noexcept;
IntervalViewResultV3 ValidateIntervalValueViewV3(const IntervalValueViewV3&, bool null_allowed = true) noexcept;
IntervalViewResultV3 AdmitIntervalOperandV3(const IntervalOperandV3&, bool null_allowed = true) noexcept;
IntervalViewResultV3 DecodeCanonicalIntervalComponentNoAllocV3(const IntervalValidatedProfileHandleV3&, IntervalValueStateV3, bool, std::span<const byte>, const IntervalExecutionControlV3& = {}) noexcept;
IntervalBytesResultV3 EncodeCanonicalIntervalComponentV3(const IntervalOwnedValueV3&, const IntervalExecutionControlV3& = {}) noexcept;
IntervalNoAllocWriteResultV3 EncodeCanonicalIntervalComponentIntoNoAllocV3(const IntervalOwnedValueV3&, byte*, u64, const IntervalExecutionControlV3& = {}) noexcept;

IntervalValueResultV3 ConstructIntervalV3(const std::shared_ptr<const IntervalValidatedProfileHandleV3>&, std::int64_t, std::int64_t, std::int64_t, bool = true, const IntervalExecutionControlV3& = {}) noexcept;
IntervalValueResultV3 ConstructIntervalV3(const std::shared_ptr<const IntervalValidatedProfileHandleV3>&, const IntervalNullableI32FactV3&, const IntervalNullableI32FactV3&, const IntervalNullableI64FactV3&, bool = true, const IntervalExecutionControlV3& = {}) noexcept;
IntervalComponentResultV3 DecomposeIntervalV3(const IntervalValueViewV3&, bool = true, const IntervalExecutionControlV3& = {}) noexcept;
IntervalValueResultV3 ParseCanonicalIntervalV3(const std::shared_ptr<const IntervalValidatedProfileHandleV3>&, std::string_view, bool = true, const IntervalExecutionControlV3& = {}) noexcept;
IntervalValueResultV3 ParseCanonicalIntervalOperandV3(const std::shared_ptr<const IntervalValidatedProfileHandleV3>&, const IntervalTextOperandV3&, bool = true, const IntervalExecutionControlV3& = {}) noexcept;
IntervalTextResultV3 RenderCanonicalIntervalV3(const IntervalOwnedValueV3&, const IntervalExecutionControlV3& = {}) noexcept;
IntervalNoAllocWriteResultV3 RenderCanonicalIntervalIntoNoAllocV3(const IntervalOwnedValueV3&, char*, u64, const IntervalExecutionControlV3& = {}) noexcept;
IntervalValueResultV3 CanonicalizeIntervalIdentityV3(const IntervalOwnedValueV3&, bool = true, const IntervalExecutionControlV3& = {}) noexcept;
IntervalValueResultV3 NegateIntervalCheckedV3(const IntervalOwnedValueV3&, bool = true, const IntervalExecutionControlV3& = {}) noexcept;
IntervalValueResultV3 AddIntervalCheckedV3(const IntervalOwnedValueV3&, const IntervalOwnedValueV3&, bool = true, const IntervalExecutionControlV3& = {}) noexcept;
IntervalValueResultV3 SubtractIntervalCheckedV3(const IntervalOwnedValueV3&, const IntervalOwnedValueV3&, bool = true, const IntervalExecutionControlV3& = {}) noexcept;
IntervalScalarResultV3 ExtractIntervalMonthsV3(const IntervalValueViewV3&, bool = true, const IntervalExecutionControlV3& = {}) noexcept;
IntervalScalarResultV3 ExtractIntervalCivilDaysV3(const IntervalValueViewV3&, bool = true, const IntervalExecutionControlV3& = {}) noexcept;
IntervalScalarResultV3 ExtractIntervalFixedNanosecondsV3(const IntervalValueViewV3&, bool = true, const IntervalExecutionControlV3& = {}) noexcept;
IntervalEqualityResultV3 EqualIntervalValuesV3(const IntervalValueViewV3&, const IntervalValueViewV3&, const IntervalExecutionControlV3& = {}) noexcept;
IntervalEqualityResultV3 DistinctIntervalValuesV3(const IntervalValueViewV3&, const IntervalValueViewV3&, const IntervalExecutionControlV3& = {}) noexcept;
IntervalBytesResultV3 HashIntervalValueV3(const IntervalOwnedValueV3&, const IntervalExecutionControlV3& = {}) noexcept;
IntervalNoAllocWriteResultV3 HashIntervalValueIntoNoAllocV3(const IntervalOwnedValueV3&, byte*, u64, const IntervalExecutionControlV3& = {}) noexcept;

struct IntervalBatchViewV3 {
  const IntervalValidatedProfileHandleV3* profile = nullptr;
  std::span<const std::int32_t> months;
  std::span<const std::int32_t> civil_days;
  std::span<const std::int64_t> fixed_nanoseconds;
  std::span<const byte> null_bitmap_lsb0;
};
struct IntervalOwnedBatchV3 {
  std::shared_ptr<const IntervalValidatedProfileHandleV3> profile;
  std::vector<std::int32_t> months;
  std::vector<std::int32_t> civil_days;
  std::vector<std::int64_t> fixed_nanoseconds;
  std::vector<byte> null_bitmap_lsb0;
  IntervalBatchViewV3 view() const noexcept {
    return {profile.get(), months, civil_days, fixed_nanoseconds,
            null_bitmap_lsb0};
  }
};
struct IntervalBatchExtentsResultV3 {
  Status status; IntervalDiagnosticFactV3 diagnostic;
  u64 months_bytes = 0, civil_days_bytes = 0, fixed_nanoseconds_bytes = 0;
  u64 bitmap_bytes = 0, combined_bytes = 0;
  bool ok() const noexcept { return status.ok(); }
};
struct IntervalBatchResultV3 {
  Status status; IntervalDiagnosticFactV3 diagnostic;
  IntervalOwnedBatchV3 batch;
  bool ok() const noexcept { return status.ok(); }
};
IntervalValidationResultV3 ValidateIntervalBatchViewV3(const IntervalBatchViewV3&) noexcept;
IntervalBatchExtentsResultV3 ComputeIntervalBatchExtentsV3(u64 row_count, u64 size_limit) noexcept;
IntervalBatchExtentsResultV3 MaterializeIntervalBatchIntoV3(
    const std::shared_ptr<const IntervalValidatedProfileHandleV3>&,
    std::span<const std::int32_t>, std::span<const std::int32_t>,
    std::span<const std::int64_t>, std::span<const byte>,
    std::int32_t*, u64, std::int32_t*, u64, std::int64_t*, u64,
    byte*, u64, const IntervalExecutionControlV3& = {}) noexcept;
IntervalBatchResultV3 MaterializeIntervalBatchV3(
    std::shared_ptr<const IntervalValidatedProfileHandleV3>,
    std::span<const std::int32_t>, std::span<const std::int32_t>,
    std::span<const std::int64_t>, std::span<const byte>,
    const IntervalExecutionControlV3& = {}) noexcept;
IntervalViewResultV3 DecodeIntervalSbdvalComposedNoAllocV3(
    const IntervalValidatedProfileHandleV3&, bool, std::span<const byte>,
    const IntervalExecutionControlV3& = {}) noexcept;
IntervalBytesResultV3 EncodeIntervalSbdvalComposedV3(
    const IntervalOwnedValueV3&, bool,
    const IntervalExecutionControlV3& = {}) noexcept;
IntervalViewResultV3 DecodeIntervalSbdpvComposedNoAllocV3(
    const IntervalValidatedProfileHandleV3&, bool, std::span<const byte>,
    const IntervalExecutionControlV3& = {}) noexcept;
IntervalBytesResultV3 EncodeIntervalSbdpvComposedV3(
    const IntervalOwnedValueV3&, bool,
    const IntervalExecutionControlV3& = {}) noexcept;

enum class IntervalIntrinsicOperationV3 : u8 {
  validate = 0, canonicalize = 1, construct = 2, decompose = 3,
  negate = 4, add = 5, subtract = 6, apply_to_temporal = 7,
  compare_order = 8, min = 9, max = 10, successor = 11, predecessor = 12,
};
enum class IntervalIntrinsicDispositionV3 : u8 { admitted = 0, registered_refused = 1, unknown = 2 };
IntervalIntrinsicDispositionV3 ClassifyIntervalIntrinsicOperationV3(IntervalIntrinsicOperationV3) noexcept;
IntervalValueResultV3 RefuseIntervalIntrinsicOperationV3(const IntervalValueViewV3&, IntervalIntrinsicOperationV3) noexcept;

enum class IntervalCastPolicyDispositionV3 : u8 {
  contextual_null, identity, explicit_character_to_interval,
  explicit_interval_to_character, forbidden,
};
IntervalCastPolicyDispositionV3 ClassifyIntervalCastPolicyRowV3(u32, DatatypeCastContext) noexcept;
struct IntervalCastRequestV3 {
  u32 one_based_policy_row = 0;
  const IntervalOwnedValueV3* interval_source = nullptr;
  const IntervalOperandV3* dynamic_interval_source = nullptr;
  const DatatypeOperationValue* scalar_source = nullptr;
  const DatatypeTypeCodecIdentityRowV3* scalar_source_identity = nullptr;
  const std::shared_ptr<const IntervalValidatedProfileHandleV3>* interval_target = nullptr;
  const scratchbird::engine::ExecutionTypeDescriptor* interval_target_descriptor = nullptr;
  CanonicalTypeId scalar_target = CanonicalTypeId::unknown;
  scratchbird::engine::ExecutionTypeDescriptor scalar_target_descriptor;
  const DatatypeTypeCodecIdentityRowV3* scalar_target_identity = nullptr;
  DatatypeCastContext context = DatatypeCastContext::implicit;
  bool target_null_allowed = true;
  bool use_character_output_buffer = false;
  char* character_output = nullptr;
  u64 character_output_capacity = 0;
  IntervalExecutionControlV3 control;
};
struct IntervalCastResultV3 {
  Status status; IntervalDiagnosticFactV3 diagnostic;
  DatatypeCastCategory category = DatatypeCastCategory::forbidden;
  bool produced_interval = false; bool used_character_output_buffer = false;
  u64 bytes_required = 0; u64 bytes_written = 0;
  IntervalOwnedValueV3 interval_value; DatatypeOperationValue scalar_value;
  bool ok() const noexcept { return status.ok(); }
};
IntervalCastResultV3 CastIntervalValueV3(const IntervalCastRequestV3&) noexcept;

}  // namespace scratchbird::core::datatypes
