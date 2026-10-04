// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "datatype_bit_string_projection.hpp"

#include "hash_digest.hpp"
#include "hash_digest_parts.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace scratchbird::core::datatypes {
namespace {
using platform::LoadLittle32;
using platform::LoadLittle16;
using platform::LoadLittle64;
using platform::Severity;
using platform::StatusCode;
using platform::StoreLittle16;
using platform::StoreLittle32;
using platform::StoreLittle64;
using platform::Subsystem;

Status Ok() noexcept { return {StatusCode::ok, Severity::info, Subsystem::datatypes}; }
Status Error() noexcept {
  return {StatusCode::platform_required_feature_missing, Severity::error,
          Subsystem::datatypes};
}
Status BudgetError() noexcept {
  return {StatusCode::memory_allocation_failed, Severity::error,
          Subsystem::datatypes};
}

BitStringBytesResultV3 BytesFailure(std::string_view code, std::string_view key,
                                    std::string_view detail = {}) noexcept {
  BitStringBytesResultV3 r;
  r.status = code=="RESOURCE.BUDGET_EXCEEDED"?BudgetError():Error();
  try { r.diagnostic = MakeBitStringDiagnosticV3(
      r.status,std::string(code),std::string(key),std::string(detail)); }
  catch (...) {}
  return r;
}
BitStringIndexProjectionResultV3 IndexFailure(
    BitStringIndexResolutionV3 resolution, std::string_view code,
    std::string_view detail = {}) noexcept {
  BitStringIndexProjectionResultV3 r;
  r.status = code=="RESOURCE.BUDGET_EXCEEDED"?BudgetError():Error();
  r.resolution = resolution;
  try { r.diagnostic = MakeBitStringDiagnosticV3(
      r.status,std::string(code),"datatype.bit_string.index_projection_refused",
      std::string(detail)); } catch (...) {}
  return r;
}
BitStringIndexProjectionResultV3 IndexBudgetFailure(
    BitStringIndexResolutionV3 resolution, u64 required, u64 maximum) noexcept {
  auto r = IndexFailure(resolution, "CTB.BIT.INDEX_KEY_REFUSED",
                        "provider_key_budget_exceeded");
  (void)required;(void)maximum;
  return r;
}
BitStringIndexProjectionResultV3 IndexOwnerFactFailure(
    BitStringIndexResolutionV3 resolution) noexcept {
  BitStringIndexProjectionResultV3 r;
  r.status=Error();r.resolution=resolution;
  return r;
}
BitStringBytesResultV3 ViewFailureBytes(
    const BitStringViewResultV3& source) noexcept {
  BitStringBytesResultV3 r; r.status=source.status;
  try { r.diagnostic=MakeBitStringDiagnosticV3(
      r.status,std::string(source.diagnostic.diagnostic_code),
      "datatype.bit_string.projection_input_invalid",
      std::string(source.diagnostic.detail)); } catch (...) {}
  return r;
}
BitStringIndexProjectionResultV3 ViewFailureIndex(
    const BitStringViewResultV3& source,
    BitStringIndexResolutionV3 resolution) noexcept {
  BitStringIndexProjectionResultV3 r; r.status=source.status;
  r.resolution=resolution;
  try { r.diagnostic=MakeBitStringDiagnosticV3(
      r.status,std::string(source.diagnostic.diagnostic_code),
      "datatype.bit_string.index_projection_refused",
      std::string(source.diagnostic.detail)); } catch (...) {}
  return r;
}
bool IsNil(const platform::Uuid& uuid) noexcept {
  for (byte b : uuid.bytes) if (b != 0) return false;
  return true;
}
void StoreUuid(byte* out, const platform::Uuid& uuid) noexcept {
  std::memcpy(out, uuid.bytes.data(), uuid.bytes.size());
}
platform::Uuid LoadUuid(const byte* input) noexcept {
  platform::Uuid uuid{};
  std::memcpy(uuid.bytes.data(),input,uuid.bytes.size());
  return uuid;
}
bool CheckedAdd(u64 a, u64 b, u64* out) noexcept {
  if (a > std::numeric_limits<u64>::max() - b) return false;
  *out = a + b; return true;
}
bool ExactResolution(const BitStringIndexResolutionV3& left,
                     const BitStringIndexResolutionV3& right) noexcept {
  return left.family==right.family && left.disposition==right.disposition &&
      left.projection==right.projection &&
      left.exact_recheck_required==right.exact_recheck_required &&
      left.refusal_diagnostic==right.refusal_diagnostic;
}
bool ControlCancelled(const BitStringExecutionControlV3& control) noexcept {
  return control.cancelled!=nullptr &&
      control.cancelled(control.cancellation_context);
}
bool ExactProfileBinding(const BitStringDescriptorProfileV3& left,
                         const BitStringDescriptorProfileV3& right) noexcept {
  return left.receipt.statement_receipt_uuid==right.receipt.statement_receipt_uuid &&
      left.receipt.catalog_snapshot_uuid==right.receipt.catalog_snapshot_uuid &&
      left.receipt.catalog_generation==right.receipt.catalog_generation &&
      left.receipt.registry_generation==right.receipt.registry_generation &&
      left.canonical_profile_material==right.canonical_profile_material &&
      left.profile_fingerprint==right.profile_fingerprint &&
      left.comparison_cohort_fingerprint==right.comparison_cohort_fingerprint;
}

BitStringBytesResultV3 CoveringValue(const BitStringValueViewV3& value,
                                     u64 maximum,
                                     const BitStringExecutionControlV3& control) noexcept {
  auto checked = ValidateBitStringValueViewV3(value, true);
  if (!checked.ok()) return ViewFailureBytes(checked);
  const u64 required = value.state == BitStringValueStateV3::sql_null
      ? 1 : static_cast<u64>(5) + value.packed_msb0.size();
  if (required > maximum) {
    return BytesFailure("CTB.BIT.INDEX_KEY_REFUSED",
                        "datatype.bit_string.index_key_refused",
                        "covering_value_provider_budget");
  }
  if (required > control.maximum_allocation_bytes)
    return BytesFailure("RESOURCE.BUDGET_EXCEEDED",
                        "datatype.bit_string.resource_budget_exceeded",
                        "covering_value_allocation_grant");
  if (ControlCancelled(control))
    return BytesFailure("PROCESS.CANCELLED",
                        "datatype.bit_string.projection_cancelled");
  try {
    BitStringBytesResultV3 r; r.status = Ok(); r.bytes.resize(required);
    r.bytes[0] = value.state == BitStringValueStateV3::sql_null ? 0 : 2;
    if (value.state == BitStringValueStateV3::present) {
      StoreLittle32(r.bytes.data() + 1, value.logical_bit_count);
      constexpr std::size_t kCheckpointBytes=65'536/8;
      std::size_t copied=0;
      while(copied<value.packed_msb0.size()) {
        const auto chunk=std::min(kCheckpointBytes,
                                  value.packed_msb0.size()-copied);
        std::memcpy(r.bytes.data()+5+copied,
                    value.packed_msb0.data()+copied,chunk);
        copied+=chunk;
        if(copied%kCheckpointBytes==0 && ControlCancelled(control))
          return BytesFailure("PROCESS.CANCELLED",
                              "datatype.bit_string.projection_cancelled",
                              "covering_value_copy_checkpoint");
      }
    }
    if (!DecodeBitStringCoveringValueNoAllocV3(*value.profile,r.bytes).ok())
      return BytesFailure("CTB.BIT.INDEX_KEY_REFUSED",
                          "datatype.bit_string.index_key_refused",
                          "covering_value_self_recheck_failed");
    return r;
  } catch (const std::bad_alloc&) {
    return BytesFailure("RESOURCE.BUDGET_EXCEEDED",
                        "datatype.bit_string.resource_budget_exceeded");
  }
}

constexpr platform::Uuid kStatisticsPrivacyUuid{{
    0x01,0xa1,0x0c,0x00,0x40,0x00,0x70,0x01,
    0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x01}};

}  // namespace

BitStringIndexResolutionV3 ResolveBitStringIndexFamilyV3(
    BitStringIndexFamilyV3 family) noexcept {
  using D = BitStringIndexDispositionV3; using P = BitStringProjectionKindV3;
  switch (family) {
    case BitStringIndexFamilyV3::aggregate_sketch:
      return {family,D::admitted_projection_only,P::cohort_hash32,true,"CTB.BIT.INDEX_KEY_REFUSED"};
    case BitStringIndexFamilyV3::bitmap:
    case BitStringIndexFamilyV3::hash:
      return {family,D::admitted_with_recheck,P::cohort_hash32,true,"CTB.BIT.INDEX_KEY_REFUSED"};
    case BitStringIndexFamilyV3::brin_like:
    case BitStringIndexFamilyV3::columnar_zone_map:
      return {family,D::admitted_with_recheck,P::zone_map,true,"CTB.BIT.INDEX_KEY_REFUSED"};
    case BitStringIndexFamilyV3::btree:
      return {family,D::admitted_exact_if_budget,P::sort_key,false,"CTB.BIT.INDEX_KEY_REFUSED"};
    case BitStringIndexFamilyV3::covering_included:
      return {family,D::admitted_payload_only,P::covering_value,false,"CTB.BIT.INDEX_KEY_REFUSED"};
    case BitStringIndexFamilyV3::expression:
      return {family,D::conditional,P::conditional_result,true,"OPTIMIZER.INDEX_COMPATIBILITY_MISSING"};
    case BitStringIndexFamilyV3::partial_filtered:
      return {family,D::conditional,P::conditional_underlying,true,"OPTIMIZER.INDEX_COMPATIBILITY_MISSING"};
    case BitStringIndexFamilyV3::temporary_work:
      return {family,D::admitted_exact_selected_mode,P::none,true,"CTB.BIT.INDEX_KEY_REFUSED"};
    case BitStringIndexFamilyV3::document_path:
    case BitStringIndexFamilyV3::full_text:
    case BitStringIndexFamilyV3::graph:
    case BitStringIndexFamilyV3::range_exclusion:
    case BitStringIndexFamilyV3::spatial:
    case BitStringIndexFamilyV3::vector_ann:
      return {family,D::not_applicable,P::none,false,"OPTIMIZER.INDEX_COMPATIBILITY_MISSING"};
  }
  return {family,D::not_applicable,P::none,false,"OPTIMIZER.INDEX_COMPATIBILITY_MISSING"};
}

BitStringIndexProjectionResultV3 ProjectBitStringIndexValueV3(
    const BitStringIndexProjectionRequestV3& request) noexcept {
  auto resolution = ResolveBitStringIndexFamilyV3(request.family);
  if (request.value == nullptr)
    return IndexFailure(resolution, "CTB.BIT.DESCRIPTOR_INVALID", "value_missing");
  const auto checked = ValidateBitStringValueViewV3(*request.value, true);
  if (!checked.ok()) return ViewFailureIndex(checked, resolution);
  if (resolution.disposition == BitStringIndexDispositionV3::not_applicable)
    return IndexOwnerFactFailure(resolution);
  if (request.family == BitStringIndexFamilyV3::temporary_work) {
    if (request.temporary_mode == BitStringTemporaryProjectionModeV3::equality_hash)
      resolution.projection = BitStringProjectionKindV3::cohort_hash32;
    else if (request.temporary_mode == BitStringTemporaryProjectionModeV3::ordered_sort) {
      resolution.projection = BitStringProjectionKindV3::sort_key;
      resolution.exact_recheck_required = false;
    } else return IndexFailure(resolution, "CTB.BIT.INDEX_KEY_REFUSED",
                               "temporary_projection_mode_missing");
  }
  if (resolution.disposition == BitStringIndexDispositionV3::conditional) {
    if (request.selected_conditional_family == nullptr)
      return IndexOwnerFactFailure(resolution);
    const auto canonical=ResolveBitStringIndexFamilyV3(
        request.selected_conditional_family->family);
    if (!ExactResolution(*request.selected_conditional_family,canonical) ||
        canonical.disposition==BitStringIndexDispositionV3::conditional ||
        canonical.disposition==BitStringIndexDispositionV3::not_applicable)
      return IndexOwnerFactFailure(resolution);
    resolution = *request.selected_conditional_family;
  }

  BitStringBytesResultV3 bytes;
  switch (resolution.projection) {
    case BitStringProjectionKindV3::cohort_hash32:
      if (request.provider_max_key_bytes < 32)
        return IndexBudgetFailure(resolution, 32, request.provider_max_key_bytes);
      if (request.control.maximum_allocation_bytes < 32)
        return IndexFailure(resolution, "RESOURCE.BUDGET_EXCEEDED",
                            "cohort_hash_allocation_grant");
      if (ControlCancelled(request.control))
        return IndexFailure(resolution,"PROCESS.CANCELLED",
                            "cohort_hash_prepublication_cancelled");
      bytes = HashBitStringValueV3(*request.value);
      break;
    case BitStringProjectionKindV3::sort_key: {
      u64 required = kBitStringSortKeyHeaderBytesV3;
      if (request.value->state == BitStringValueStateV3::present &&
          !CheckedAdd(required, static_cast<u64>(request.value->logical_bit_count) + 1,
                      &required))
        return IndexFailure(resolution, "CTB.BIT.INDEX_KEY_REFUSED", "size_overflow");
      if (required > request.provider_max_key_bytes)
        return IndexBudgetFailure(resolution, required, request.provider_max_key_bytes);
      bytes = MakeBitStringSortKeyV3(*request.value, request.direction,
                                    request.null_mode, request.control);
      break;
    }
    case BitStringProjectionKindV3::covering_value:
      bytes = CoveringValue(*request.value, request.provider_max_key_bytes,
                            request.control);
      break;
    case BitStringProjectionKindV3::zone_map:
      return IndexFailure(resolution, "CTB.BIT.INDEX_KEY_REFUSED",
                          "zone_map_requires_aggregate_request");
    default:
      return IndexOwnerFactFailure(resolution);
  }
  if (!bytes.ok()) return {bytes.status, bytes.diagnostic, resolution, {}};
  return {bytes.status, {}, resolution, std::move(bytes.bytes)};
}

BitStringViewResultV3 DecodeBitStringCoveringValueNoAllocV3(
    const BitStringDescriptorProfileV3& profile,
    std::span<const byte> encoded) noexcept {
  if (!ValidateBitStringDescriptorProfileV3(profile).ok())
    return {Error(), {Error(), "CTB.BIT.DESCRIPTOR_INVALID",
                      "covering_value_profile_invalid"}, {}};
  if (encoded.empty() || encoded.size() > 2'097'157)
    return {Error(), {Error(), "CTB.BIT.INDEX_KEY_REFUSED",
                      "covering_value_extent_invalid"}, {}};
  if (encoded[0] == 0) {
    if (encoded.size() != 1)
      return {Error(), {Error(), "DATATYPE.NULL_STATE.INVALID",
                        "covering_null_has_component"}, {}};
    return DecodeCanonicalBitStringComponentNoAllocV3(
        profile, BitStringValueStateV3::sql_null, true, {});
  }
  if (encoded[0] != 2)
    return {Error(), {Error(), "CTB.BIT.CANONICAL_ENCODING_INVALID",
                      "covering_state_invalid"}, {}};
  // The canonical component has one exact representation.  Its decoder checks
  // the count/extent equation and unused tail bits, which is the allocation-free
  // equivalent of decode/re-encode byte equality for this projection.
  return DecodeCanonicalBitStringComponentNoAllocV3(
      profile, BitStringValueStateV3::present, true, encoded.subspan(1));
}

BitStringZoneMapViewResultV3 DecodeBitStringZoneMapNoAllocV3(
    const BitStringDescriptorProfileV3& profile,
    std::span<const byte> encoded) noexcept {
  const auto fail=[](std::string_view detail) noexcept {
    const auto status=Error();
    return BitStringZoneMapViewResultV3{
        status,{status,"CTB.BIT.INDEX_KEY_REFUSED",detail},{}};
  };
  if (!ValidateBitStringDescriptorProfileV3(profile).ok())
    return {Error(), {Error(), "CTB.BIT.DESCRIPTOR_INVALID",
                      "zone_map_profile_invalid"}, {}};
  if (encoded.size() < 64 || encoded.size() > 33'554'706 ||
      std::memcmp(encoded.data(),"SBBITZ01",8)!=0 ||
      LoadLittle16(encoded.data()+8)!=1 ||
      LoadLittle16(encoded.data()+10)!=64 ||
      LoadLittle32(encoded.data()+12)!=encoded.size())
    return fail("zone_map_header_or_extent_invalid");
  const u32 flags=LoadLittle32(encoded.data()+16);
  const bool has_present=(flags&1)!=0;
  if ((flags&~u32{7})!=0 || ((flags&2)!=0)==((flags&4)!=0) ||
      LoadLittle32(encoded.data()+20)!=0)
    return fail("zone_map_flags_or_reserved_invalid");
  const u64 null_count=LoadLittle64(encoded.data()+24);
  const u64 present_count=LoadLittle64(encoded.data()+32);
  const u64 empty_count=LoadLittle64(encoded.data()+40);
  const u32 minimum_bits=LoadLittle32(encoded.data()+48);
  const u32 maximum_bits=LoadLittle32(encoded.data()+52);
  const u32 minimum_bytes=LoadLittle32(encoded.data()+56);
  const u32 maximum_bytes=LoadLittle32(encoded.data()+60);
  if (empty_count>present_count || has_present!=(present_count!=0))
    return fail("zone_map_count_flags_invalid");
  BitStringZoneMapViewResultV3 result;
  result.status=Ok();result.value.profile=&profile;
  result.value.null_mode=(flags&2)!=0?BitStringNullModeV3::nulls_first:
                                      BitStringNullModeV3::nulls_last;
  result.value.null_count=null_count;result.value.present_count=present_count;
  result.value.empty_present_count=empty_count;
  result.value.minimum_logical_bits=minimum_bits;
  result.value.maximum_logical_bits=maximum_bits;
  if (!has_present) {
    if (encoded.size()!=64 || minimum_bits!=0 || maximum_bits!=0 ||
        minimum_bytes!=0 || maximum_bytes!=0 || empty_count!=0)
      return fail("zone_map_empty_set_invalid");
    return result;
  }
  if (minimum_bits>maximum_bits || maximum_bits>kBitStringMaximumLogicalBitsV3 ||
      minimum_bytes!=static_cast<u64>(kBitStringSortKeyHeaderBytesV3)+minimum_bits+1 ||
      maximum_bytes!=static_cast<u64>(kBitStringSortKeyHeaderBytesV3)+maximum_bits+1)
    return fail("zone_map_length_invalid");
  const u64 expected=64ull+minimum_bytes+maximum_bytes;
  if (expected!=encoded.size()) return fail("zone_map_key_extent_invalid");
  const auto minimum=encoded.subspan(64,minimum_bytes);
  const auto maximum=encoded.subspan(64+minimum_bytes,maximum_bytes);
  const auto min_view=DecodeBitStringSortKeyNoAllocV3(profile,minimum);
  const auto max_view=DecodeBitStringSortKeyNoAllocV3(profile,maximum);
  if (!min_view.ok() || !max_view.ok() ||
      min_view.value.state!=BitStringValueStateV3::present ||
      max_view.value.state!=BitStringValueStateV3::present ||
      min_view.value.direction!=BitStringSortDirectionV3::ascending ||
      max_view.value.direction!=BitStringSortDirectionV3::ascending ||
      min_view.value.null_mode!=result.value.null_mode ||
      max_view.value.null_mode!=result.value.null_mode ||
      min_view.value.logical_bit_count!=minimum_bits ||
      max_view.value.logical_bit_count!=maximum_bits ||
      std::lexicographical_compare(maximum.begin(),maximum.end(),
                                   minimum.begin(),minimum.end()))
    return fail("zone_map_key_invalid");
  result.value.minimum_sort_key=minimum;
  result.value.maximum_sort_key=maximum;
  return result;
}

BitStringBytesResultV3 EncodeBitStringZoneMapV3(
    const BitStringZoneMapRequestV3& request) noexcept {
  if (request.profile == nullptr ||
      !ValidateBitStringDescriptorProfileV3(*request.profile).ok())
    return BytesFailure("CTB.BIT.DESCRIPTOR_INVALID",
                        "datatype.bit_string.index_key_refused",
                        "zone_map_profile_missing_or_invalid");
  if (request.null_mode!=BitStringNullModeV3::nulls_first &&
      request.null_mode!=BitStringNullModeV3::nulls_last)
    return BytesFailure("CTB.BIT.INDEX_KEY_REFUSED",
                        "datatype.bit_string.index_key_refused",
                        "zone_map_null_mode_invalid");
  if (request.empty_present_count > request.present_count)
    return BytesFailure("CTB.BIT.INDEX_KEY_REFUSED",
                        "datatype.bit_string.index_key_refused", "count_invariant");
  if (request.present_count == 0) {
    if (request.minimum != nullptr || request.maximum != nullptr)
      return BytesFailure("CTB.BIT.INDEX_KEY_REFUSED",
                          "datatype.bit_string.index_key_refused", "empty_set_has_keys");
    if (64 > request.provider_max_key_bytes)
      return BytesFailure("CTB.BIT.INDEX_KEY_REFUSED",
                          "datatype.bit_string.index_key_refused", "provider_budget");
    if (64 > request.control.maximum_allocation_bytes)
      return BytesFailure("RESOURCE.BUDGET_EXCEEDED",
                          "datatype.bit_string.resource_budget_exceeded",
                          "zone_map_allocation_grant");
    if (request.control.cancelled != nullptr &&
        request.control.cancelled(request.control.cancellation_context))
      return BytesFailure("PROCESS.CANCELLED",
                          "datatype.bit_string.projection_cancelled");
    try {
      BitStringBytesResultV3 r; r.status=Ok(); r.bytes.assign(64,0);
      std::memcpy(r.bytes.data(),"SBBITZ01",8); StoreLittle16(r.bytes.data()+8,1);
      StoreLittle16(r.bytes.data()+10,64); StoreLittle32(r.bytes.data()+12,64);
      StoreLittle32(r.bytes.data()+16, request.null_mode==BitStringNullModeV3::nulls_first?2:4);
      StoreLittle64(r.bytes.data()+24,request.null_count);
      if (!DecodeBitStringZoneMapNoAllocV3(*request.profile,r.bytes).ok())
        return BytesFailure("CTB.BIT.INDEX_KEY_REFUSED",
                            "datatype.bit_string.index_key_refused",
                            "zone_map_self_recheck_failed");
      return r;
    } catch(const std::bad_alloc&) {
      return BytesFailure("RESOURCE.BUDGET_EXCEEDED",
                          "datatype.bit_string.resource_budget_exceeded");
    }
  }
  if (request.minimum == nullptr || request.maximum == nullptr ||
      request.minimum->state != BitStringValueStateV3::present ||
      request.maximum->state != BitStringValueStateV3::present)
    return BytesFailure("CTB.BIT.INDEX_KEY_REFUSED",
                        "datatype.bit_string.index_key_refused", "present_extrema_missing");
  auto min_checked=ValidateBitStringValueViewV3(*request.minimum,false);
  if(!min_checked.ok()) return ViewFailureBytes(min_checked);
  auto max_checked=ValidateBitStringValueViewV3(*request.maximum,false);
  if(!max_checked.ok()) return ViewFailureBytes(max_checked);
  if(!ExactProfileBinding(*request.profile,*request.minimum->profile) ||
     !ExactProfileBinding(*request.profile,*request.maximum->profile))
    return BytesFailure("CTB.BIT.DESCRIPTOR_INVALID",
                        "datatype.bit_string.index_key_refused",
                        "zone_map_extrema_profile_mismatch");
  auto comparison=CompareBitStringValuesV3(*request.minimum,*request.maximum,
                                           BitStringComparisonModeV3::present_only);
  if(!comparison.ok() || comparison.comparison>0)
    return BytesFailure("CTB.BIT.INDEX_KEY_REFUSED",
                        "datatype.bit_string.index_key_refused", "extrema_order_invalid");
  u64 min_bytes=105ull+request.minimum->logical_bit_count;
  u64 max_bytes=105ull+request.maximum->logical_bit_count;
  u64 required=64;
  if(!CheckedAdd(required,min_bytes,&required)||!CheckedAdd(required,max_bytes,&required)||
     required>std::numeric_limits<u32>::max())
    return BytesFailure("CTB.BIT.INDEX_KEY_REFUSED",
                        "datatype.bit_string.index_key_refused", "size_overflow");
  if(required>request.provider_max_key_bytes)
    return BytesFailure("CTB.BIT.INDEX_KEY_REFUSED",
                        "datatype.bit_string.index_key_refused", "provider_budget");
  if(required>request.control.maximum_allocation_bytes)
    return BytesFailure("RESOURCE.BUDGET_EXCEEDED",
                        "datatype.bit_string.resource_budget_exceeded",
                        "zone_map_allocation_grant");
  auto min_key=MakeBitStringSortKeyV3(*request.minimum,BitStringSortDirectionV3::ascending,
                                      request.null_mode,request.control);
  if(!min_key.ok()) return min_key;
  auto max_key=MakeBitStringSortKeyV3(*request.maximum,BitStringSortDirectionV3::ascending,
                                      request.null_mode,request.control);
  if(!max_key.ok()) return max_key;
  if(ControlCancelled(request.control))
    return BytesFailure("PROCESS.CANCELLED",
                        "datatype.bit_string.projection_cancelled",
                        "zone_map_before_outer_allocation");
  try {
    BitStringBytesResultV3 r; r.status=Ok(); r.bytes.assign(required,0);
    std::memcpy(r.bytes.data(),"SBBITZ01",8); StoreLittle16(r.bytes.data()+8,1);
    StoreLittle16(r.bytes.data()+10,64); StoreLittle32(r.bytes.data()+12,required);
    StoreLittle32(r.bytes.data()+16,1|(request.null_mode==BitStringNullModeV3::nulls_first?2:4));
    StoreLittle64(r.bytes.data()+24,request.null_count);
    StoreLittle64(r.bytes.data()+32,request.present_count);
    StoreLittle64(r.bytes.data()+40,request.empty_present_count);
    StoreLittle32(r.bytes.data()+48,request.minimum->logical_bit_count);
    StoreLittle32(r.bytes.data()+52,request.maximum->logical_bit_count);
    StoreLittle32(r.bytes.data()+56,min_key.bytes.size());
    StoreLittle32(r.bytes.data()+60,max_key.bytes.size());
    constexpr std::size_t kCheckpointBytes=65'536;
    auto copy_key=[&](std::size_t destination,
                      const std::vector<byte>& source) noexcept {
      std::size_t copied=0;
      while(copied<source.size()) {
        const auto chunk=std::min(kCheckpointBytes,source.size()-copied);
        std::memcpy(r.bytes.data()+destination+copied,
                    source.data()+copied,chunk);
        copied+=chunk;
        if(copied%kCheckpointBytes==0 && ControlCancelled(request.control))
          return false;
      }
      return true;
    };
    if(!copy_key(64,min_key.bytes) ||
       !copy_key(64+min_key.bytes.size(),max_key.bytes))
      return BytesFailure("PROCESS.CANCELLED",
                          "datatype.bit_string.projection_cancelled",
                          "zone_map_copy_checkpoint");
    if (!DecodeBitStringZoneMapNoAllocV3(*request.profile,r.bytes).ok())
      return BytesFailure("CTB.BIT.INDEX_KEY_REFUSED",
                          "datatype.bit_string.index_key_refused",
                          "zone_map_self_recheck_failed");
    if(ControlCancelled(request.control))
      return BytesFailure("PROCESS.CANCELLED",
                          "datatype.bit_string.projection_cancelled",
                          "zone_map_before_publication");
    return r;
  } catch(const std::bad_alloc&) {
    return BytesFailure("RESOURCE.BUDGET_EXCEEDED","datatype.bit_string.resource_budget_exceeded");
  }
}

namespace {
BitStringBytesResultV3 StatisticsFailure(std::string_view detail) noexcept {
  return BytesFailure("CTB.BIT.CANONICAL_ENCODING_INVALID",
                      "datatype.bit_string.statistics_projection_invalid",
                      detail);
}
}

BitStringBytesResultV3 EncodeBitStringStatisticsProjectionV3(
    const BitStringStatisticsProjectionV3& p) noexcept {
  if (p.profile == nullptr) return StatisticsFailure("profile_missing");
  auto profile = ValidateBitStringDescriptorProfileV3(*p.profile);
  if (!profile.ok()) return {profile.status, profile.diagnostic, {}};
  if (p.mcv.size() > 64 || IsNil(p.statistics_uuid) ||
      IsNil(p.source_object_uuid) || IsNil(p.provider_evidence_uuid))
    return StatisticsFailure("required_identity_or_mcv_bound_invalid");
  if (p.row_count != p.null_count + p.present_count ||
      p.empty_present_count > p.present_count)
    return StatisticsFailure("row_count_invariant");
  u64 histogram_total=0;
  for(u64 count:p.length_histogram_counts) {
    if(histogram_total>std::numeric_limits<u64>::max()-count)
      return StatisticsFailure("histogram_count_overflow");
    histogram_total+=count;
  }
  if(histogram_total!=p.present_count) return StatisticsFailure("histogram_total_mismatch");
  if ((p.present_count==0 && (p.minimum_logical_bits!=0||p.maximum_logical_bits!=0)) ||
      (p.present_count!=0 && p.minimum_logical_bits>p.maximum_logical_bits) ||
      p.maximum_logical_bits>kBitStringMaximumLogicalBitsV3)
    return StatisticsFailure("length_extrema_invalid");
  // SBBITS01 generation 1 has an explicit zero selectivity-model identity.
  // Therefore it cannot claim an exact or estimated distinct model either;
  // a later registered model/profile must carry that authority.
  if (p.distinct_kind != 0 || p.distinct_count != 0)
    return StatisticsFailure("distinct_model_invalid");
  if (p.correlation_scaled_1e9!=INT64_MIN &&
      (p.correlation_scaled_1e9 < -1'000'000'000ll ||
       p.correlation_scaled_1e9 > 1'000'000'000ll))
    return StatisticsFailure("correlation_invalid");
  u64 mcv_frequency_total=0;
  for(std::size_t i=0;i<p.mcv.size();++i) {
    if(p.mcv[i].frequency==0 ||
       mcv_frequency_total>std::numeric_limits<u64>::max()-p.mcv[i].frequency ||
       p.mcv[i].logical_bits>kBitStringMaximumLogicalBitsV3 ||
       (i && (p.mcv[i-1].frequency<p.mcv[i].frequency ||
        (p.mcv[i-1].frequency==p.mcv[i].frequency &&
         p.mcv[i-1].cohort_hash>=p.mcv[i].cohort_hash))))
      return StatisticsFailure("mcv_order_or_length_invalid");
    for(std::size_t j=0;j<i;++j)
      if(p.mcv[j].cohort_hash==p.mcv[i].cohort_hash)
        return StatisticsFailure("mcv_hash_duplicate_or_collision");
    mcv_frequency_total+=p.mcv[i].frequency;
  }
  if(mcv_frequency_total>p.present_count)
    return StatisticsFailure("mcv_frequency_total_invalid");
  const u32 total_bytes=832u+static_cast<u32>(p.mcv.size())*64u;
  try {
    BitStringBytesResultV3 r; r.status=Ok(); r.bytes.assign(total_bytes,0);
    auto* b=r.bytes.data(); std::memcpy(b,"SBBITS01",8);
    StoreLittle16(b+8,1); StoreLittle16(b+10,480); StoreLittle32(b+12,total_bytes);
    u32 flags=1;
    if(!p.mcv.empty()) flags|=2|32;
    if(p.present_count) flags|=4;
    if(p.correlation_scaled_1e9!=INT64_MIN) flags|=8;
    if(p.distinct_kind==2) flags|=16;
    StoreLittle32(b+16,flags); StoreLittle32(b+20,22);
    StoreLittle32(b+24,static_cast<u32>(p.mcv.size()));
    const auto& id=p.profile->identity.legacy_fields;
    StoreUuid(b+32,p.profile->receipt.catalog_snapshot_uuid);
    StoreLittle64(b+48,p.profile->receipt.catalog_generation);
    StoreLittle64(b+56,p.profile->receipt.registry_generation);
    StoreUuid(b+64,id.descriptor_uuid); StoreLittle64(b+80,id.descriptor_generation);
    StoreUuid(b+88,id.type_uuid); StoreLittle64(b+104,id.type_generation);
    StoreUuid(b+112,id.codec_uuid); StoreLittle32(b+128,id.codec_version);
    StoreLittle64(b+136,id.codec_generation);
    StoreUuid(b+144,p.profile->statistics.uuid);
    StoreLittle64(b+160,p.profile->statistics.generation);
    StoreUuid(b+168,p.statistics_uuid); StoreUuid(b+184,p.source_object_uuid);
    StoreLittle64(b+200,p.schema_epoch); StoreLittle64(b+208,p.sample_epoch);
    StoreLittle64(b+216,p.security_epoch); StoreLittle64(b+224,p.resource_epoch);
    StoreLittle64(b+232,p.row_count); StoreLittle64(b+240,p.null_count);
    StoreLittle64(b+256,p.present_count); StoreLittle64(b+264,p.empty_present_count);
    StoreLittle64(b+272,p.distinct_count); b[280]=p.distinct_kind;
    StoreLittle32(b+288,p.minimum_logical_bits); StoreLittle32(b+292,p.maximum_logical_bits);
    std::memcpy(b+296,p.sum_logical_bits_u128_le.data(),16);
    StoreLittle32(b+312,480); StoreLittle32(b+316,832);
    StoreLittle32(b+320,total_bytes);
    // Generation 1 declares ordered-value selectivity unavailable with an
    // all-zero model identity and generation.  This datatype projection emits
    // the fact only; the optimizer owns ISO.SELECTIVITY.MODEL_MISSING.
    StoreUuid(b+352,kStatisticsPrivacyUuid); StoreLittle64(b+368,1);
    std::memcpy(b+408,p.profile->profile_fingerprint.data(),32);
    StoreLittle64(b+440,static_cast<u64>(p.correlation_scaled_1e9));
    StoreUuid(b+456,p.provider_evidence_uuid);
    for(std::size_t i=0;i<22;++i) {
      byte* h=b+480+i*16; StoreLittle32(h,kBitStringLengthHistogramBoundsV3[i]);
      StoreLittle64(h+8,p.length_histogram_counts[i]);
    }
    for(std::size_t i=0;i<p.mcv.size();++i) {
      byte* m=b+832+i*64; std::memcpy(m,p.mcv[i].cohort_hash.data(),32);
      StoreLittle64(m+32,p.mcv[i].frequency); StoreLittle32(m+40,p.mcv[i].logical_bits);
      StoreLittle32(m+44,2); // redacted=1, exact_component_present=0
    }
    static constexpr char domain[]="ScratchBird.BaseBitString.StatisticsRecords.V1";
    std::vector<byte> hash_material;
    hash_material.reserve(sizeof(domain)-1+r.bytes.size());
    hash_material.insert(hash_material.end(),domain,domain+sizeof(domain)-1);
    hash_material.insert(hash_material.end(),r.bytes.begin(),r.bytes.end());
    auto digest=hash::ComputeSha256Digest(hash_material);
    if(!digest.ok()) return {digest.status,digest.diagnostic,{}};
    std::memcpy(b+376,digest.digest.data(),digest.digest.size());
    return r;
  } catch(const std::bad_alloc&) {
    return BytesFailure("RESOURCE.BUDGET_EXCEEDED","datatype.bit_string.resource_budget_exceeded");
  }
}

BitStringStatisticsDecodeResultV3 DecodeBitStringStatisticsProjectionV3(
    const BitStringDescriptorProfileV3& profile,
    std::span<const byte> encoded) noexcept {
  const auto fail=[&](std::string_view detail) noexcept {
    BitStringStatisticsDecodeResultV3 r;r.status=Error();
    try { r.diagnostic=MakeBitStringDiagnosticV3(
        r.status,"CTB.BIT.CANONICAL_ENCODING_INVALID",
        "datatype.bit_string.statistics_projection_invalid",std::string(detail)); }
    catch (...) {}
    return r;
  };
  const auto profile_ok=ValidateBitStringDescriptorProfileV3(profile);
  if(!profile_ok.ok()) {
    BitStringStatisticsDecodeResultV3 r;r.status=profile_ok.status;
    r.diagnostic=profile_ok.diagnostic;return r;
  }
  if(encoded.size()<832 || encoded.size()>4928 ||
     std::memcmp(encoded.data(),"SBBITS01",8)!=0 ||
     LoadLittle16(encoded.data()+8)!=1 || LoadLittle16(encoded.data()+10)!=480 ||
     LoadLittle32(encoded.data()+12)!=encoded.size()) return fail("header_or_extent_invalid");
  const u32 flags=LoadLittle32(encoded.data()+16);
  const u32 mcv_count=LoadLittle32(encoded.data()+24);
  if((flags&~u32{0x3f})!=0 || (flags&1)==0 ||
     LoadLittle32(encoded.data()+20)!=22 || mcv_count>64 ||
     LoadLittle32(encoded.data()+28)!=0 || LoadLittle32(encoded.data()+132)!=0 ||
     LoadLittle32(encoded.data()+312)!=480 || LoadLittle32(encoded.data()+316)!=832 ||
     LoadLittle32(encoded.data()+320)!=encoded.size() ||
     LoadLittle32(encoded.data()+324)!=0 ||
     encoded.size()!=832u+static_cast<std::size_t>(mcv_count)*64u ||
     ((flags&2)!=0)!=(mcv_count!=0) || ((flags&32)!=0)!=(mcv_count!=0) ||
     LoadLittle32(encoded.data()+448)!=0 || LoadLittle32(encoded.data()+452)!=0 ||
     LoadLittle64(encoded.data()+472)!=0)
    return fail("flags_offsets_or_reserved_invalid");
  for(std::size_t i=281;i<288;++i) if(encoded[i]) return fail("distinct_reserved_invalid");
  const auto& id=profile.identity.legacy_fields;
  const auto uuid_equal=[&](std::size_t offset,const platform::Uuid& uuid){
    return std::memcmp(encoded.data()+offset,uuid.bytes.data(),16)==0;};
  if(!uuid_equal(32,profile.receipt.catalog_snapshot_uuid) ||
     LoadLittle64(encoded.data()+48)!=profile.receipt.catalog_generation ||
     LoadLittle64(encoded.data()+56)!=profile.receipt.registry_generation ||
     !uuid_equal(64,id.descriptor_uuid) || LoadLittle64(encoded.data()+80)!=id.descriptor_generation ||
     !uuid_equal(88,id.type_uuid) || LoadLittle64(encoded.data()+104)!=id.type_generation ||
     !uuid_equal(112,id.codec_uuid) || LoadLittle32(encoded.data()+128)!=id.codec_version ||
     LoadLittle64(encoded.data()+136)!=id.codec_generation ||
     !uuid_equal(144,profile.statistics.uuid) ||
     LoadLittle64(encoded.data()+160)!=profile.statistics.generation ||
     !uuid_equal(352,kStatisticsPrivacyUuid) || LoadLittle64(encoded.data()+368)!=1 ||
     std::memcmp(encoded.data()+408,profile.profile_fingerprint.data(),32)!=0)
    return fail("identity_or_profile_stale");
  const u64 row_count=LoadLittle64(encoded.data()+232);
  const u64 null_count=LoadLittle64(encoded.data()+240);
  const u64 missing_count=LoadLittle64(encoded.data()+248);
  const u64 present_count=LoadLittle64(encoded.data()+256);
  const u64 empty_count=LoadLittle64(encoded.data()+264);
  const u64 distinct_count=LoadLittle64(encoded.data()+272);
  const byte distinct_kind=encoded[280];
  const u32 min_bits=LoadLittle32(encoded.data()+288),max_bits=LoadLittle32(encoded.data()+292);
  const auto statistics_uuid=LoadUuid(encoded.data()+168);
  const auto source_object_uuid=LoadUuid(encoded.data()+184);
  const auto model_uuid=LoadUuid(encoded.data()+328);
  const auto provider_uuid=LoadUuid(encoded.data()+456);
  if(missing_count!=0 || null_count>row_count || row_count-null_count!=present_count ||
     empty_count>present_count || distinct_kind!=0 || distinct_count!=0 ||
     min_bits>max_bits || max_bits>kBitStringMaximumLogicalBitsV3 ||
     (present_count==0&&(min_bits!=0||max_bits!=0)) ||
     IsNil(statistics_uuid) || IsNil(source_object_uuid) || !IsNil(model_uuid) ||
     LoadLittle64(encoded.data()+344)!=0 || IsNil(provider_uuid))
    return fail("count_length_model_or_provider_invalid");
  const std::int64_t correlation=static_cast<std::int64_t>(LoadLittle64(encoded.data()+440));
  if(((flags&4)!=0)!=(present_count!=0) ||
     ((flags&8)!=0)!=(correlation!=INT64_MIN) ||
     (flags&16)!=0 ||
     (correlation!=INT64_MIN&&(correlation< -1'000'000'000ll||correlation>1'000'000'000ll)))
    return fail("statistics_flag_semantics_invalid");
  u64 histogram_total=0;
  for(std::size_t i=0;i<22;++i){const auto* h=encoded.data()+480+i*16;
    const u64 count=LoadLittle64(h+8);
    if(LoadLittle32(h)!=kBitStringLengthHistogramBoundsV3[i]||LoadLittle32(h+4)!=0||
       histogram_total>std::numeric_limits<u64>::max()-count)
      return fail("histogram_invalid");
    histogram_total+=count;
  }
  if(histogram_total!=present_count) return fail("histogram_total_mismatch");
  u64 mcv_total=0;
  for(std::size_t i=0;i<mcv_count;++i){const auto* m=encoded.data()+832+i*64;
    const u64 frequency=LoadLittle64(m+32);const u32 logical=LoadLittle32(m+40);
    if(frequency==0||logical>kBitStringMaximumLogicalBitsV3||LoadLittle32(m+44)!=2||
       LoadLittle32(m+48)!=0||LoadLittle32(m+52)!=0||LoadLittle64(m+56)!=0||
       mcv_total>std::numeric_limits<u64>::max()-frequency) return fail("mcv_invalid");
    if(i){const auto* prev=m-64;const u64 previous_frequency=LoadLittle64(prev+32);
      const int hash_order=std::memcmp(prev,m,32);
      if(previous_frequency<frequency||(previous_frequency==frequency&&hash_order>=0))
        return fail("mcv_order_invalid");}
    for(std::size_t j=0;j<i;++j)if(std::memcmp(encoded.data()+832+j*64,m,32)==0)
      return fail("mcv_hash_duplicate_or_collision");
    mcv_total+=frequency;
  }
  if(mcv_total>present_count) return fail("mcv_frequency_total_invalid");
  static constexpr char domain[]="ScratchBird.BaseBitString.StatisticsRecords.V1";
  const std::array<byte,32> zeros{};
  const hash::HashDigestSegment parts[]{
      {reinterpret_cast<const byte*>(domain),sizeof(domain)-1},
      {encoded.data(),376},{zeros.data(),zeros.size()},
      {encoded.data()+408,encoded.size()-408}};
  const auto digest=hash::ComputeSha256DigestPartsNative(parts,4);
  if(!digest.ok()||std::memcmp(digest.digest.data(),encoded.data()+376,32)!=0)
    return fail("records_hash_invalid");
  try {
    BitStringStatisticsDecodeResultV3 r;r.status=Ok();auto& out=r.statistics;
    out.statistics_uuid=statistics_uuid;
    out.source_object_uuid=source_object_uuid;
    out.schema_epoch=LoadLittle64(encoded.data()+200);out.sample_epoch=LoadLittle64(encoded.data()+208);
    out.security_epoch=LoadLittle64(encoded.data()+216);out.resource_epoch=LoadLittle64(encoded.data()+224);
    out.row_count=row_count;out.null_count=null_count;out.present_count=present_count;
    out.empty_present_count=empty_count;out.minimum_logical_bits=min_bits;out.maximum_logical_bits=max_bits;
    std::memcpy(out.sum_logical_bits_u128_le.data(),encoded.data()+296,16);
    for(std::size_t i=0;i<22;++i)out.length_histogram_counts[i]=LoadLittle64(encoded.data()+488+i*16);
    out.mcv.reserve(mcv_count);
    for(std::size_t i=0;i<mcv_count;++i){const auto* m=encoded.data()+832+i*64;BitStringStatisticsMcvV3 row;
      std::memcpy(row.cohort_hash.data(),m,32);row.frequency=LoadLittle64(m+32);row.logical_bits=LoadLittle32(m+40);out.mcv.push_back(row);}
    out.correlation_scaled_1e9=correlation;out.provider_evidence_uuid=provider_uuid;
    return r;
  } catch(const std::bad_alloc&) {
    BitStringStatisticsDecodeResultV3 r;r.status=BudgetError();
    try{r.diagnostic=MakeBitStringDiagnosticV3(r.status,"RESOURCE.BUDGET_EXCEEDED","datatype.bit_string.resource_budget_exceeded","statistics_decode_allocation");}catch(...){}
    return r;
  }
}

BitStringBackupTupleResultV3 ProjectBitStringBackupTupleV3(
    const BitStringValueViewV3& value) noexcept {
  BitStringBackupTupleResultV3 r;
  auto checked=ValidateBitStringValueViewV3(value,true);
  r.status=checked.status;
  if(!checked.ok()) {
    try { r.diagnostic=MakeBitStringDiagnosticV3(
        checked.status,std::string(checked.diagnostic.diagnostic_code),
        "datatype.bit_string.backup_projection_invalid",
        std::string(checked.diagnostic.detail)); } catch (...) {}
    return r;
  }
  r.tuple.catalog_snapshot_uuid=value.profile->receipt.catalog_snapshot_uuid;
  r.tuple.catalog_generation=value.profile->receipt.catalog_generation;
  r.tuple.registry_generation=value.profile->receipt.registry_generation;
  r.tuple.profile_fingerprint=value.profile->profile_fingerprint;
  r.tuple.state=value.state;
  r.tuple.logical_bit_count=value.state==BitStringValueStateV3::present?value.logical_bit_count:0;
  r.tuple.packed_msb0=value.state==BitStringValueStateV3::present?value.packed_msb0:std::span<const byte>{};
  return r;
}

BitStringOwnedBackupTupleResultV3 EncodeBitStringBackupTupleV3(
    const BitStringValueViewV3& value,
    const BitStringExecutionControlV3& control) noexcept {
  BitStringOwnedBackupTupleResultV3 r;
  auto projected=ProjectBitStringBackupTupleV3(value);
  r.status=projected.status;r.diagnostic=projected.diagnostic;
  if(!projected.ok()) return r;
  if(projected.tuple.packed_msb0.size()>control.maximum_allocation_bytes) {
    r.status=BudgetError();
    try{r.diagnostic=MakeBitStringDiagnosticV3(r.status,"RESOURCE.BUDGET_EXCEEDED","datatype.bit_string.resource_budget_exceeded","backup_tuple_allocation_grant");}catch(...){}
    return r;
  }
  if(control.cancelled&&control.cancelled(control.cancellation_context)) {
    r.status=Error();
    try{r.diagnostic=MakeBitStringDiagnosticV3(r.status,"PROCESS.CANCELLED","datatype.bit_string.projection_cancelled");}catch(...){}
    return r;
  }
  try {
    BitStringOwnedBackupTupleV3 candidate;
    candidate.catalog_snapshot_uuid=projected.tuple.catalog_snapshot_uuid;
    candidate.catalog_generation=projected.tuple.catalog_generation;
    candidate.registry_generation=projected.tuple.registry_generation;
    candidate.profile_fingerprint=projected.tuple.profile_fingerprint;
    candidate.state=projected.tuple.state;
    candidate.logical_bit_count=projected.tuple.logical_bit_count;
    candidate.packed_msb0.resize(projected.tuple.packed_msb0.size());
    constexpr std::size_t kCheckpointBytes=65'536/8;
    std::size_t copied=0;
    while(copied<projected.tuple.packed_msb0.size()) {
      const auto chunk=std::min(kCheckpointBytes,
                                projected.tuple.packed_msb0.size()-copied);
      std::memcpy(candidate.packed_msb0.data()+copied,
                  projected.tuple.packed_msb0.data()+copied,chunk);
      copied+=chunk;
      if(copied%kCheckpointBytes==0 && ControlCancelled(control)) {
        r={};r.status=Error();
        try{r.diagnostic=MakeBitStringDiagnosticV3(
            r.status,"PROCESS.CANCELLED",
            "datatype.bit_string.projection_cancelled",
            "backup_tuple_copy_checkpoint");}catch(...){}
        return r;
      }
    }
    if(ControlCancelled(control)) {
      r={};r.status=Error();
      try{r.diagnostic=MakeBitStringDiagnosticV3(
          r.status,"PROCESS.CANCELLED",
          "datatype.bit_string.projection_cancelled",
          "backup_tuple_before_publication");}catch(...){}
      return r;
    }
    r.tuple=std::move(candidate);
    return r;
  } catch(const std::bad_alloc&) {
    r={};r.status=BudgetError();
    try{r.diagnostic=MakeBitStringDiagnosticV3(r.status,"RESOURCE.BUDGET_EXCEEDED","datatype.bit_string.resource_budget_exceeded","backup_tuple_allocation");}catch(...){}
    return r;
  }
}

BitStringResultV3 DecodeBitStringBackupTupleV3(
    const BitStringDescriptorProfileV3& profile,
    const BitStringOwnedBackupTupleV3& tuple, bool null_allowed,
    const BitStringExecutionControlV3& control) noexcept {
  const auto profile_ok=ValidateBitStringDescriptorProfileV3(profile);
  if(!profile_ok.ok()) {
    BitStringResultV3 r;r.status=profile_ok.status;r.diagnostic=profile_ok.diagnostic;return r;
  }
  if(tuple.catalog_snapshot_uuid!=profile.receipt.catalog_snapshot_uuid ||
     tuple.catalog_generation!=profile.receipt.catalog_generation ||
     tuple.registry_generation!=profile.receipt.registry_generation ||
     tuple.profile_fingerprint!=profile.profile_fingerprint) {
    BitStringResultV3 r;r.status=Error();
    try{r.diagnostic=MakeBitStringDiagnosticV3(r.status,"CTB.BIT.DESCRIPTOR_INVALID","datatype.bit_string.backup_tuple_profile_invalid");}catch(...){}
    return r;
  }
  const BitStringValueViewV3 view{
      &profile,tuple.state,tuple.logical_bit_count,tuple.packed_msb0,
      BitStringOwnershipV3::borrowed};
  return MaterializeBitStringValueV3(view,null_allowed,control);
}

BitStringProtectionResolutionV3 ResolveBitStringProtectionV3(
    BitStringProtectionCellV3 cell) noexcept {
  using C=BitStringProtectionCellV3;
  switch(cell) {
    case C::canonical_plain:
      return {cell,true,false,"admitted_datatype_component","CTB.BIT.CANONICAL_ENCODING_INVALID"};
    case C::inner_compression: case C::inner_encryption:
      return {cell,false,false,"forbidden","CTB.BIT.PROTECTION_UNSUPPORTED"};
    case C::outer_compression: case C::outer_authenticated_protection:
    case C::outer_compress_then_protect:
      return {cell,false,false,"unsupported_current_core","CTB.BIT.PROTECTION_UNSUPPORTED"};
    case C::outer_protect_then_compress:
      return {cell,false,false,"forbidden_order","CTB.BIT.PROTECTION_UNSUPPORTED"};
    case C::page_or_filespace_crypto: case C::transport_tls:
      return {cell,false,true,"not_applicable_datatype_layer","owner_diagnostic_before_datatype"};
    case C::protected_index:
      return {cell,false,false,"forbidden_current_core","CTB.BIT.PROTECTION_UNSUPPORTED"};
  }
  return {cell,false,false,"forbidden","CTB.BIT.PROTECTION_UNSUPPORTED"};
}

BitStringWireLaneResolutionV3 ResolveBitStringWireLaneV3(
    BitStringWireLaneV3 lane) noexcept {
  const bool native=static_cast<u8>(lane)<=static_cast<u8>(BitStringWireLaneV3::canonical_sblr);
  return {lane,false,native,
      native?"unsupported_current_v1_exact_v3_behavior_candidate_outer_version_unallocated":
             "unsupported_no_manifest_listed_compatibility_profile",
      "CTB.TRANSPORT.UNSUPPORTED"};
}

}  // namespace scratchbird::core::datatypes
