// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "datatype_time_projection.hpp"

#include "hash_digest_parts.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace scratchbird::core::datatypes {
namespace {
using platform::LoadLittle16;
using platform::LoadLittle32;
using platform::LoadLittle64;
using platform::Severity;
using platform::StatusCode;
using platform::StoreLittle16;
using platform::StoreLittle32;
using platform::StoreLittle64;
using platform::Subsystem;

constexpr byte kZero32[32]{};
constexpr byte kZoneMagic[8] = {'S','B','T','I','M','Z','0','1'};
constexpr byte kStatisticsMagic[8] = {'S','B','T','I','M','S','0','1'};
constexpr byte kBackupMagic[8] = {'S','B','T','I','M','B','0','1'};
constexpr char kZoneDomain[] = "ScratchBird.BaseTime.ZoneMap.V1";
constexpr char kStatisticsDomain[] = "ScratchBird.BaseTime.StatisticsRecords.V1";
constexpr char kBackupDomain[] = "ScratchBird.BaseTime.BackupTuple.V1";

constexpr platform::Uuid Uuid(std::array<byte,16> b) noexcept { return {b}; }
constexpr platform::Uuid kPrivacyUuid = Uuid({
  0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,
  0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x41});

Status Ok() noexcept { return {StatusCode::ok, Severity::info, Subsystem::datatypes}; }
Status Error() noexcept { return {StatusCode::platform_required_feature_missing,
                                  Severity::error, Subsystem::datatypes}; }
Status BudgetError() noexcept { return {StatusCode::memory_allocation_failed,
                                        Severity::error, Subsystem::datatypes}; }
bool Cancelled(const TimeExecutionControlV3& c) noexcept {
  return c.cancelled && c.cancelled(c.cancellation_context);
}
bool Nil(const platform::Uuid& u) noexcept { return u.is_nil(); }
void StoreUuid(byte* p,const platform::Uuid& u) noexcept {
  std::memcpy(p,u.bytes.data(),16);
}
platform::Uuid LoadUuid(const byte* p) noexcept {
  platform::Uuid u{}; std::memcpy(u.bytes.data(),p,16); return u;
}
bool IsV7(const platform::Uuid& u) noexcept {
  return !u.is_nil() && (u.bytes[6] >> 4) == 7 && (u.bytes[8] & 0xc0) == 0x80;
}

TimeBytesResultV3 FailBytes(std::string_view code,
                            std::string_view detail = {}) noexcept {
  TimeBytesResultV3 r;
  r.status = code == "RESOURCE.BUDGET_EXCEEDED" ? BudgetError() : Error();
  r.diagnostic = {r.status, code, detail};
  return r;
}
template<class R> R FailView(std::string_view code,
                              std::string_view detail = {}) noexcept {
  R r; r.status = code=="RESOURCE.BUDGET_EXCEEDED"?BudgetError():Error();
  r.diagnostic = {r.status, code, detail};
  return r;
}
TimeIndexProjectionResultV3 FailIndex(TimeIndexResolutionV3 resolution,
                                      std::string_view code,
                                      std::string_view detail = {}) noexcept {
  TimeIndexProjectionResultV3 r; r.resolution=resolution;
  r.requested_resolution=resolution;
  r.status=code=="RESOURCE.BUDGET_EXCEEDED"?BudgetError():Error();
  r.diagnostic = {r.status, code, detail};
  return r;
}
TimeIndexProjectionResultV3 OwnerFact(TimeIndexResolutionV3 r) noexcept {
  TimeIndexProjectionResultV3 out; out.status=Error(); out.resolution=r;
  out.requested_resolution=r;
  out.diagnostic = {out.status, "OPTIMIZER.INDEX_COMPATIBILITY_MISSING",
                    "conditional_receiving_owner_selection"};
  out.owner_fact_only=true; return out;
}

bool SameResolution(const TimeIndexResolutionV3& a,
                    const TimeIndexResolutionV3& b) noexcept {
  return a.family==b.family && a.compatibility_uuid==b.compatibility_uuid &&
      a.compatibility_generation==b.compatibility_generation &&
      a.disposition==b.disposition && a.projection==b.projection &&
      a.exact_recheck_required==b.exact_recheck_required &&
      a.receiving_owner_resolution_required==b.receiving_owner_resolution_required;
}
bool ValidFamily(TimeIndexFamilyV3 family) noexcept {
  return static_cast<unsigned>(family) < 16;
}
bool ModeAllowed(TimeIndexFamilyV3 family, TimeProjectionModeV3 mode) noexcept {
  using F = TimeIndexFamilyV3;
  using M = TimeProjectionModeV3;
  switch (family) {
    case F::bitmap:
    case F::hash:
      return mode == M::equality_hash;
    case F::brin_like:
    case F::columnar_zone_map:
      return mode == M::zone_map;
    case F::btree:
      return mode == M::ascending_nulls_first ||
             mode == M::ascending_nulls_last ||
             mode == M::descending_nulls_first ||
             mode == M::descending_nulls_last;
    case F::covering_included:
      return mode == M::payload;
    case F::range_exclusion:
      return mode == M::ascending_nulls_last_pair;
    case F::temporary_work:
      return mode == M::equality_hash ||
             mode == M::ascending_nulls_first ||
             mode == M::ascending_nulls_last ||
             mode == M::descending_nulls_first ||
             mode == M::descending_nulls_last;
    default:
      return false;
  }
}

template <std::size_t N>
class ArrayClearGuard {
 public:
  explicit ArrayClearGuard(std::array<byte, N>* value) noexcept : value_(value) {}
  ~ArrayClearGuard() {
    volatile byte* p = value_ ? value_->data() : nullptr;
    for (std::size_t i = 0; p && i < N; ++i) p[i] = 0;
  }
 private:
  std::array<byte, N>* value_;
};

bool HashRecord(std::string_view domain, std::span<const byte> record,
                std::size_t zero_offset, std::array<byte,32>* out) noexcept {
  if (!out || zero_offset+32>record.size()) return false;
  const hash::HashDigestSegment parts[]{
    {reinterpret_cast<const byte*>(domain.data()),domain.size()},
    {record.data(),zero_offset},{kZero32,32},
    {record.data()+zero_offset+32,record.size()-zero_offset-32}};
  auto h=hash::ComputeSha256DigestPartsNative(parts,4);
  ArrayClearGuard<32> clear_digest(&h.digest);
  if (!h.ok()) return false;
  *out=h.digest;
  return true;
}
bool HashEquals(std::string_view domain,std::span<const byte> record,
                std::size_t offset) noexcept {
  std::array<byte,32> h{}; ArrayClearGuard<32> clear(&h);
  return HashRecord(domain,record,offset,&h) &&
      std::memcmp(h.data(),record.data()+offset,32)==0;
}

void PutCommonIdentity(byte* p,const TimeValidatedProfileHandleV3& h) noexcept {
  const auto& i=h.identity.legacy_fields;
  StoreUuid(p+0,h.receipt.catalog_snapshot_uuid);
  StoreLittle64(p+16,h.receipt.catalog_generation);
  StoreLittle64(p+24,h.receipt.registry_generation);
  StoreUuid(p+32,i.descriptor_uuid); StoreLittle64(p+48,i.descriptor_generation);
  StoreUuid(p+56,i.type_uuid); StoreLittle64(p+72,i.type_generation);
  StoreUuid(p+80,i.codec_uuid); StoreLittle32(p+96,i.codec_version);
  StoreLittle32(p+100,0); StoreLittle64(p+104,i.codec_generation);
}
bool CommonIdentityEquals(const byte* p,
                          const TimeValidatedProfileHandleV3& h) noexcept {
  const auto& i=h.identity.legacy_fields;
  return LoadUuid(p)==h.receipt.catalog_snapshot_uuid &&
      LoadLittle64(p+16)==h.receipt.catalog_generation &&
      LoadLittle64(p+24)==h.receipt.registry_generation &&
      LoadUuid(p+32)==i.descriptor_uuid && LoadLittle64(p+48)==i.descriptor_generation &&
      LoadUuid(p+56)==i.type_uuid && LoadLittle64(p+72)==i.type_generation &&
      LoadUuid(p+80)==i.codec_uuid && LoadLittle32(p+96)==i.codec_version &&
      LoadLittle32(p+100)==0 && LoadLittle64(p+104)==i.codec_generation;
}
bool ValidProfile(const TimeValidatedProfileHandleV3& h) noexcept {
  return ValidateTimeProfileHandleV3(h).ok();
}
bool SameProfile(const TimeValidatedProfileHandleV3& a,
                 const TimeValidatedProfileHandleV3& b) noexcept {
  return a.profile_material==b.profile_material &&
      a.comparison_material==b.comparison_material &&
      a.profile_fingerprint==b.profile_fingerprint &&
      a.comparison_fingerprint==b.comparison_fingerprint;
}

class VectorClearGuard {
 public:
  explicit VectorClearGuard(std::vector<byte>* value) noexcept : value_(value) {}
  ~VectorClearGuard() {
    if (active_ && value_) {
      volatile byte* p = value_->empty() ? nullptr : value_->data();
      for (std::size_t i = 0; p && i < value_->size(); ++i) p[i] = 0;
      value_->clear();
    }
  }
  void Disarm() noexcept { active_ = false; }
 private:
  std::vector<byte>* value_;
  bool active_ = true;
};

template <typename T>
class TypedVectorClearGuard {
 public:
  explicit TypedVectorClearGuard(std::vector<T>* value) noexcept : value_(value) {}
  ~TypedVectorClearGuard() {
    if (!active_ || value_ == nullptr || value_->empty()) return;
    volatile byte* p = reinterpret_cast<volatile byte*>(value_->data());
    for (std::size_t i = 0; i < value_->size() * sizeof(T); ++i) p[i] = 0;
    value_->clear();
  }
  void Disarm() noexcept { active_ = false; }
 private:
  std::vector<T>* value_;
  bool active_ = true;
};

template <typename T>
void ClearPlainObject(T* value) noexcept {
  volatile byte* p=reinterpret_cast<volatile byte*>(value);
  for(std::size_t i=0;i<sizeof(T);++i)p[i]=0;
}

template <typename T>
class PlainObjectClearGuard {
 public:
  explicit PlainObjectClearGuard(T* value) noexcept:value_(value){}
  ~PlainObjectClearGuard(){if(value_)ClearPlainObject(value_);}
 private:T* value_;
};

class DecodedStatisticsClearGuard {
 public:
  explicit DecodedStatisticsClearGuard(TimeDecodedStatisticsV3* value) noexcept
      : value_(value) {}
  ~DecodedStatisticsClearGuard(){
    if(!active_||!value_)return;
    if(!value_->histogram.empty()){
      volatile byte* p=reinterpret_cast<volatile byte*>(value_->histogram.data());
      for(std::size_t i=0;i<value_->histogram.size()*sizeof(TimeStatisticsHistogramRecordV3);++i)p[i]=0;
      value_->histogram.clear();
    }
    if(!value_->mcv.empty()){
      volatile byte* p=reinterpret_cast<volatile byte*>(value_->mcv.data());
      for(std::size_t i=0;i<value_->mcv.size()*sizeof(TimeStatisticsMcvRecordV3);++i)p[i]=0;
      value_->mcv.clear();
    }
    ClearPlainObject(&value_->statistics_uuid);ClearPlainObject(&value_->source_object_uuid);
    ClearPlainObject(&value_->provider_evidence_uuid);value_->schema_epoch=0;
    value_->collection_epoch=0;value_->security_epoch=0;value_->row_count=0;
    value_->null_count=0;value_->value_count=0;value_->minimum_maximum_present=false;
    value_->minimum_nanoseconds=0;value_->maximum_nanoseconds=0;value_->profile.reset();
  }
  void Disarm() noexcept{active_=false;}
 private:
  TimeDecodedStatisticsV3* value_;
  bool active_=true;
};

bool Reserve(std::vector<byte>* out,u64 n) noexcept {
  if (!out || n>std::numeric_limits<std::size_t>::max()) return false;
  try { out->assign(static_cast<std::size_t>(n),0); return true; }
  catch (...) { return false; }
}

void EncodeSortKeyRaw(const TimeValueViewV3& value,
                      TimeSortDirectionV3 direction,
                      TimeNullModeV3 null_mode, byte* output) noexcept {
  std::memcpy(output, "SBTIMK01", 8);
  StoreUuid(output + 8, value.profile->receipt.catalog_snapshot_uuid);
  StoreLittle64(output + 24, value.profile->receipt.catalog_generation);
  StoreLittle64(output + 32, value.profile->receipt.registry_generation);
  std::memcpy(output + 40, value.profile->comparison_fingerprint.data(), 32);
  StoreUuid(output + 72, value.profile->identity.ordering_policy.uuid);
  StoreLittle64(output + 88,
                value.profile->identity.ordering_policy.generation);
  output[96] = direction == TimeSortDirectionV3::descending ? 1 : 0;
  output[97] = null_mode == TimeNullModeV3::nulls_last ? 1 : 0;
  output[98] = value.state == TimeValueStateV3::value ? 1
      : (null_mode == TimeNullModeV3::nulls_first ? 0 : 2);
  output[99] = value.state == TimeValueStateV3::value ? 8 : 0;
  if (value.state == TimeValueStateV3::value) {
    const u64 sortable = value.nanoseconds_since_midnight;
    for (unsigned i = 0; i != 8; ++i)
      output[100 + i] = static_cast<byte>(sortable >> (56u - 8u * i));
    if (direction == TimeSortDirectionV3::descending)
      for (std::size_t i = 100; i != 108; ++i)
        output[i] = static_cast<byte>(~output[i]);
  }
}

TimeDiagnosticFactV3 ValidateProjectionValueMetadata(
    const TimeOwnedValueV3& value,bool null_allowed,
    const TimeValidatedProfileHandleV3* expected_profile=nullptr) noexcept {
  if(!value.profile||!ValidProfile(*value.profile)||
     (expected_profile&&!SameProfile(*value.profile,*expected_profile)))
    return {Error(),"CTI.TEMPORAL.DESCRIPTOR_INVALID","projection_profile"};
  if(value.state!=TimeValueStateV3::value&&
     value.state!=TimeValueStateV3::sql_null)
    return {Error(),"DATATYPE.NULL_STATE.INVALID","projection_state"};
  if(value.state==TimeValueStateV3::sql_null){
    if(value.nanoseconds_since_midnight!=0)return {Error(),"DATATYPE.NULL_STATE.INVALID","projection_dirty_null"};
    if(!null_allowed)return {Error(),"DATATYPE.NULL_NOT_ADMITTED","projection_null_not_admitted"};
  }
  return {Ok(),{}, {}};
}
} // namespace

TimeIndexResolutionV3 ResolveTimeIndexFamilyV3(TimeIndexFamilyV3 family) noexcept {
  using D=TimeIndexDispositionV3; using P=TimeProjectionKindV3;
  static constexpr std::array<platform::Uuid,16> ids{{
    Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x48}),
    Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x49}),
    Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x4a}),
    Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x4b}),
    Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x4c}),
    Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x4d}),
    Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x4e}),
    Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x4f}),
    Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x50}),
    Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x51}),
    Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x52}),
    Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x53}),
    Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x54}),
    Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x55}),
    Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x56}),
    Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x57})
  }};
  if (!ValidFamily(family)) {
    TimeIndexResolutionV3 invalid;
    invalid.family = family;
    invalid.disposition = D::not_applicable;
    invalid.receiving_owner_resolution_required = true;
    return invalid;
  }
  TimeIndexResolutionV3 r{family,ids[static_cast<std::size_t>(family)],1};
  switch(family) {
    case TimeIndexFamilyV3::bitmap: case TimeIndexFamilyV3::hash:
      r.disposition=D::admitted_with_recheck;r.projection=P::equality_hash;r.exact_recheck_required=true;break;
    case TimeIndexFamilyV3::brin_like: case TimeIndexFamilyV3::columnar_zone_map:
      r.disposition=D::admitted_with_recheck;r.projection=P::zone_map;r.exact_recheck_required=true;break;
    case TimeIndexFamilyV3::btree:
      r.disposition=D::admitted_exact_if_budget;r.projection=P::sort_key;break;
    case TimeIndexFamilyV3::covering_included:
      r.disposition=D::admitted_payload_only;r.projection=P::covering_value;break;
    case TimeIndexFamilyV3::range_exclusion:
      r.disposition=D::admitted_exact_if_budget;r.projection=P::range_pair;break;
    case TimeIndexFamilyV3::temporary_work:
      r.disposition=D::admitted_exact_selected_mode;r.projection=P::none;break;
    case TimeIndexFamilyV3::expression:
      r.disposition=D::conditional;r.projection=P::conditional_result;r.exact_recheck_required=true;r.receiving_owner_resolution_required=true;break;
    case TimeIndexFamilyV3::partial_filtered:
      r.disposition=D::conditional;r.projection=P::conditional_underlying;r.exact_recheck_required=true;r.receiving_owner_resolution_required=true;break;
    default:
      r.disposition=D::not_applicable;r.projection=P::none;r.receiving_owner_resolution_required=true;break;
  }
  return r;
}

TimeIndexCompatibilityIdentityV3 LookupTimeIndexCompatibilityIdentityV3(
    TimeIndexFamilyV3 family) noexcept {
  const auto resolution = ResolveTimeIndexFamilyV3(family);
  return {family, resolution.compatibility_uuid,
          resolution.compatibility_generation};
}

TimeIndexAdmissionResultV3 AdmitTimeIndexCompatibilityV3(
    const TimeIndexCompatibilityIdentityV3& supplied) noexcept {
  TimeIndexAdmissionResultV3 result;
  result.resolution.family = supplied.family;
  if (!ValidFamily(supplied.family) || supplied.compatibility_uuid.is_nil() ||
      supplied.compatibility_generation == 0) {
    return result;
  }
  const auto expected = ResolveTimeIndexFamilyV3(supplied.family);
  if (supplied.compatibility_uuid != expected.compatibility_uuid ||
      supplied.compatibility_generation != expected.compatibility_generation) {
    return result;
  }
  result.admitted = true;
  result.resolution = expected;
  result.diagnostic = {};
  return result;
}

TimeIndexPredicateResolutionV3 ResolveTimeIndexPredicateV3(
    const TimeIndexPredicateRequestV3& request) noexcept {
  using F=TimeIndexFamilyV3;using O=TimeIndexPredicateOperationV3;
  using P=TimeIndexPredicateFactV3;
  TimeIndexPredicateResolutionV3 result;
  const auto requested = AdmitTimeIndexCompatibilityV3(request.requested);
  result.requested_resolution = requested.resolution;
  if (!requested.admitted) {
    result.diagnostic = "OPTIMIZER.INDEX_COMPATIBILITY_MISSING";
    return result;
  }
  auto effective = requested.resolution;
  if (effective.disposition == TimeIndexDispositionV3::conditional) {
    result.predicate_owner_handoff_preserved =
        effective.family == F::partial_filtered;
    if (!request.selected_conditional) {
      result.diagnostic = "OPTIMIZER.INDEX_COMPATIBILITY_MISSING";
      return result;
    }
    const auto selected =
        AdmitTimeIndexCompatibilityV3(*request.selected_conditional);
    if (!selected.admitted ||
        selected.resolution.disposition == TimeIndexDispositionV3::conditional ||
        selected.resolution.disposition ==
            TimeIndexDispositionV3::not_applicable) {
      result.diagnostic = "OPTIMIZER.INDEX_COMPATIBILITY_MISSING";
      return result;
    }
    effective = selected.resolution;
  }
  result.effective_resolution = effective;
  auto admit = [&](P fact) {
    result.admitted = true;
    result.fact = fact;
    result.diagnostic = {};
    return result;
  };
  const auto operation = request.operation;
  switch(effective.family){
    case F::bitmap:if(operation==O::equality||operation==O::in||operation==O::is_null)return admit(P::requires_exact_state_component_recheck_never_final_match);break;
    case F::hash:if(operation==O::equality||operation==O::in)return admit(P::requires_exact_state_component_recheck_never_final_match);break;
    case F::brin_like:if(operation==O::equality||operation==O::range||operation==O::order)return admit(P::prune_only_mandatory_source_row_predicate_recheck_never_final_match);break;
    case F::columnar_zone_map:if(operation==O::prune_range||operation==O::equality)return admit(P::prune_only_mandatory_source_row_predicate_recheck_never_final_match);break;
    case F::btree:if(operation==O::equality||operation==O::range||operation==O::order)return admit(P::exact_no_recheck_after_decode_reencode);break;
    case F::covering_included:if(operation==O::include_payload)return admit(P::exact_no_recheck_after_decode_reencode);break;
    case F::range_exclusion:if(operation==O::range||operation==O::overlap||operation==O::exclusion)return admit(P::exact_no_recheck_after_decode_reencode);break;
    case F::temporary_work:if(operation==O::equality)return admit(P::requires_exact_state_component_recheck_never_final_match);if(operation==O::order)return admit(P::exact_no_recheck_after_decode_reencode);break;
    default:break;
  }
  result.diagnostic =
      effective.disposition == TimeIndexDispositionV3::not_applicable
          ? "OPTIMIZER.INDEX_COMPATIBILITY_MISSING"
          : "CTI.TEMPORAL.INDEX_KEY_REFUSED";
  return result;
}

TimeIndexCandidateFactsV3 ClassifyTimeIndexCandidateV3(
    TimeIndexPredicateFactV3 predicate_fact, bool projection_candidate_equal,
    bool exact_state_component_equal,
    bool source_row_predicate_equal) noexcept {
  using D = TimeIndexCandidateDispositionV3;
  using F = TimeIndexPredicateFactV3;
  TimeIndexCandidateFactsV3 result;
  if (!projection_candidate_equal || predicate_fact == F::refused) {
    result.disposition = D::not_a_candidate;
    return result;
  }
  if (predicate_fact == F::exact_no_recheck_after_decode_reencode) {
    result.disposition = D::exact_match;
    result.final_match = true;
    return result;
  }
  if (predicate_fact ==
      F::requires_exact_state_component_recheck_never_final_match) {
    result.exact_state_component_recheck_required = true;
    result.final_match = exact_state_component_equal;
    result.disposition = exact_state_component_equal
        ? D::exact_match : D::rejected_after_required_recheck;
    return result;
  }
  result.source_row_predicate_recheck_required = true;
  result.final_match = source_row_predicate_equal;
  result.disposition = source_row_predicate_equal
      ? D::exact_match : D::rejected_after_required_recheck;
  return result;
}

TimeBytesResultV3 EncodeTimeCoveringValueV3(const TimeOwnedValueV3& value,
                                             const TimeExecutionControlV3& c) noexcept {
  auto v=ValidateTimeValueViewV3(value.view(),true); if(!v.ok()) return FailBytes(v.diagnostic.diagnostic_code,v.diagnostic.detail);
  const u64 n=value.state==TimeValueStateV3::sql_null?1:9;
  auto profile_pin=value.profile;
  if(n>c.maximum_allocation_bytes) return FailBytes("RESOURCE.BUDGET_EXCEEDED","covering_allocation");
  if(Cancelled(c)) return FailBytes("PROCESS.CANCELLED","covering_cancelled");
  TimeBytesResultV3 r; VectorClearGuard clear(&r.bytes);
  if(!Reserve(&r.bytes,n)) return FailBytes("RESOURCE.BUDGET_EXCEEDED","covering_allocation");
  r.bytes[0]=value.state==TimeValueStateV3::sql_null?0:2;
  if(n==9) StoreLittle64(r.bytes.data()+1,value.nanoseconds_since_midnight);
  if(!DecodeTimeCoveringValueNoAllocV3(*value.profile,true,r.bytes).ok()) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","covering_self_check");
  if(Cancelled(c)) return FailBytes("PROCESS.CANCELLED","covering_prepublication");
  r.status=Ok();clear.Disarm();return r;
}

TimeCoveringValueViewResultV3 DecodeTimeCoveringValueNoAllocV3(
    const TimeValidatedProfileHandleV3& h,bool null_allowed,
    std::span<const byte> e) noexcept {
  if(e.empty()) return FailView<TimeCoveringValueViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","minimum_extent");
  if(!ValidProfile(h)) return FailView<TimeCoveringValueViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","profile");
  TimeValueStateV3 state=TimeValueStateV3::value;u64 value=0;
  PlainObjectClearGuard<TimeValueStateV3> clear_state(&state);
  PlainObjectClearGuard<u64> clear_value(&value);
  if(e[0]==0) { if(e.size()!=1) return FailView<TimeCoveringValueViewResultV3>("DATATYPE.NULL_STATE.INVALID","null_extent");
    if(!null_allowed) return FailView<TimeCoveringValueViewResultV3>("DATATYPE.NULL_NOT_ADMITTED");
    state=TimeValueStateV3::sql_null;
  } else if(e[0]==2) { if(e.size()!=9) return FailView<TimeCoveringValueViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","value_extent");
    state=TimeValueStateV3::value;value=LoadLittle64(e.data()+1);
    if(value>kTimeMaximumNanosecondsV3) return FailView<TimeCoveringValueViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","value_range");
  } else return FailView<TimeCoveringValueViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","tag");
  std::array<byte,9> canonical{};ArrayClearGuard<9> clear_canonical(&canonical);
  canonical[0]=state==TimeValueStateV3::sql_null?0:2;
  const std::size_t canonical_extent=state==TimeValueStateV3::sql_null?1:9;
  if(canonical_extent==9)StoreLittle64(canonical.data()+1,value);
  if(e.size()!=canonical_extent||std::memcmp(e.data(),canonical.data(),canonical_extent))
    return FailView<TimeCoveringValueViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","covering_reencode");
  TimeCoveringValueViewResultV3 r;r.value={&h,state,value};r.status=Ok();return r;
}

TimeBytesResultV3 EncodeTimeRangePairV3(const TimeOwnedValueV3& lower,
                                        const TimeOwnedValueV3& upper,
                                        const TimeExecutionControlV3& c) noexcept {
  auto l=ValidateTimeValueViewV3(lower.view(),false); if(!l.ok()) return FailBytes(l.diagnostic.diagnostic_code,l.diagnostic.detail);
  auto u=ValidateTimeValueViewV3(upper.view(),false); if(!u.ok()) return FailBytes(u.diagnostic.diagnostic_code,u.diagnostic.detail);
  if(!SameProfile(*lower.profile,*upper.profile) || lower.nanoseconds_since_midnight>upper.nanoseconds_since_midnight) return FailBytes("CTI.TEMPORAL.INDEX_KEY_REFUSED","range_order_or_profile");
  auto profile_pin=lower.profile;
  if(216>c.maximum_allocation_bytes) return FailBytes("RESOURCE.BUDGET_EXCEEDED","range_allocation");
  if(Cancelled(c)) return FailBytes("PROCESS.CANCELLED","range_cancelled");
  TimeBytesResultV3 r;VectorClearGuard clear(&r.bytes);
  if(!Reserve(&r.bytes,216)) return FailBytes("RESOURCE.BUDGET_EXCEEDED","range_allocation");
  EncodeSortKeyRaw(lower.view(),TimeSortDirectionV3::ascending,TimeNullModeV3::nulls_last,r.bytes.data());
  EncodeSortKeyRaw(upper.view(),TimeSortDirectionV3::ascending,TimeNullModeV3::nulls_last,r.bytes.data()+108);
  if(!DecodeTimeRangePairNoAllocV3(*lower.profile,r.bytes).ok()) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","range_self_check");
  if(Cancelled(c)) return FailBytes("PROCESS.CANCELLED","range_prepublication");
  r.status=Ok();clear.Disarm();return r;
}

TimeRangePairViewResultV3 DecodeTimeRangePairNoAllocV3(
    const TimeValidatedProfileHandleV3& h,std::span<const byte> e) noexcept {
  if(e.size()!=216) return FailView<TimeRangePairViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","extent");
  auto l=DecodeTimeSortKeyNoAllocV3(h,e.first(108)); if(!l.ok()) return FailView<TimeRangePairViewResultV3>(l.diagnostic.diagnostic_code,l.diagnostic.detail);
  auto u=DecodeTimeSortKeyNoAllocV3(h,e.subspan(108)); if(!u.ok()) return FailView<TimeRangePairViewResultV3>(u.diagnostic.diagnostic_code,u.diagnostic.detail);
  if(l.value.state!=TimeValueStateV3::value||u.value.state!=TimeValueStateV3::value||l.value.direction!=TimeSortDirectionV3::ascending||u.value.direction!=TimeSortDirectionV3::ascending||l.value.null_mode!=TimeNullModeV3::nulls_last||u.value.null_mode!=TimeNullModeV3::nulls_last||l.value.nanoseconds_since_midnight>u.value.nanoseconds_since_midnight)
    return FailView<TimeRangePairViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","range_invariant");
  TimeRangePairViewResultV3 r;r.value={l.value.nanoseconds_since_midnight,u.value.nanoseconds_since_midnight,e.first(108),e.subspan(108)};r.status=Ok();return r;
}

static TimeBytesResultV3 EncodeTimeZoneMapImpl(
    const TimeZoneMapRequestV3& q,u64 provider_max_key_bytes,
    const TimeExecutionControlV3& control,bool acquire_profile_pin) noexcept {
  if(!q.profile||!ValidProfile(*q.profile)) return FailBytes("CTI.TEMPORAL.DESCRIPTOR_INVALID","zone_profile");
  if(q.value_count>std::numeric_limits<u64>::max()-q.null_count) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_count_overflow");
  const bool has=q.value_count!=0;
  if(has && (!q.minimum||!q.maximum)) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_extrema_missing");
  if(!has && (q.minimum||q.maximum)) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_extrema_for_empty");
  if(has) {
    const auto lo=ValidateProjectionValueMetadata(*q.minimum,false,q.profile.get());
    if(!lo.status.ok())return FailBytes(lo.diagnostic_code,lo.detail);
    const auto hi=ValidateProjectionValueMetadata(*q.maximum,false,q.profile.get());
    if(!hi.status.ok())return FailBytes(hi.diagnostic_code,hi.detail);
  }
  const u64 n=has?312:96;
  std::shared_ptr<const TimeValidatedProfileHandleV3> profile_pin;
  if(acquire_profile_pin)profile_pin=q.profile;
  if(n>provider_max_key_bytes) return FailBytes("CTI.TEMPORAL.INDEX_KEY_REFUSED","zone_provider_budget");
  if(n>control.maximum_allocation_bytes) return FailBytes("RESOURCE.BUDGET_EXCEEDED","zone_allocation");
  if(Cancelled(control)) return FailBytes("PROCESS.CANCELLED","zone_cancelled");
  if(has&&q.minimum->nanoseconds_since_midnight>q.maximum->nanoseconds_since_midnight)
    return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_extrema_order");
  TimeBytesResultV3 r;VectorClearGuard clear(&r.bytes);
  if(!Reserve(&r.bytes,n)) return FailBytes("RESOURCE.BUDGET_EXCEEDED","zone_allocation");
  std::memcpy(r.bytes.data(),kZoneMagic,8);StoreLittle16(r.bytes.data()+8,1);StoreLittle16(r.bytes.data()+10,96);StoreLittle32(r.bytes.data()+12,static_cast<u32>(n));
  StoreLittle32(r.bytes.data()+16,has?5u:4u);StoreLittle64(r.bytes.data()+24,q.null_count);StoreLittle64(r.bytes.data()+32,q.value_count);
  if(has) {
    StoreLittle64(r.bytes.data()+40,q.minimum->nanoseconds_since_midnight);StoreLittle64(r.bytes.data()+48,q.maximum->nanoseconds_since_midnight);
    StoreLittle32(r.bytes.data()+56,108);StoreLittle32(r.bytes.data()+60,108);
    EncodeSortKeyRaw(q.minimum->view(),TimeSortDirectionV3::ascending,TimeNullModeV3::nulls_last,r.bytes.data()+96);
    EncodeSortKeyRaw(q.maximum->view(),TimeSortDirectionV3::ascending,TimeNullModeV3::nulls_last,r.bytes.data()+204);
  }
  std::array<byte,32> digest{};ArrayClearGuard<32> clear_digest(&digest);if(!HashRecord(kZoneDomain,r.bytes,64,&digest)) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_hash_provider");
  std::memcpy(r.bytes.data()+64,digest.data(),32);
  if(!DecodeTimeZoneMapNoAllocV3(*q.profile,r.bytes).ok()) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_self_check");
  if(Cancelled(control)) return FailBytes("PROCESS.CANCELLED","zone_prepublication");
  r.status=Ok();clear.Disarm();return r;
}

TimeBytesResultV3 EncodeTimeZoneMapV3(const TimeZoneMapRequestV3& q) noexcept {
  return EncodeTimeZoneMapImpl(q,q.provider_max_key_bytes,q.control,true);
}

TimeZoneMapViewResultV3 DecodeTimeZoneMapNoAllocV3(
    const TimeValidatedProfileHandleV3& h,std::span<const byte> e) noexcept {
  if((e.size()!=96&&e.size()!=312)||std::memcmp(e.data(),kZoneMagic,8)||LoadLittle16(e.data()+8)!=1||LoadLittle16(e.data()+10)!=96||LoadLittle32(e.data()+12)!=e.size()||LoadLittle32(e.data()+20)!=0)
    return FailView<TimeZoneMapViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_structure");
  const u32 flags=LoadLittle32(e.data()+16);
  if((flags&~u32{5})!=0)
    return FailView<TimeZoneMapViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_reserved_flags");
  if(!HashEquals(kZoneDomain,e,64)) return FailView<TimeZoneMapViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_hash");
  if(!ValidProfile(h)) return FailView<TimeZoneMapViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","profile");
  const u64 nc=LoadLittle64(e.data()+24),vc=LoadLittle64(e.data()+32);
  if(vc>std::numeric_limits<u64>::max()-nc||flags!=(vc?5u:4u)) return FailView<TimeZoneMapViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_counts_flags");
  u64 minimum_nanoseconds=0,maximum_nanoseconds=0;PlainObjectClearGuard<u64> clear_minimum(&minimum_nanoseconds);PlainObjectClearGuard<u64> clear_maximum(&maximum_nanoseconds);std::span<const byte> minimum_key,maximum_key;
  if(!vc) {
    if(e.size()!=96||LoadLittle64(e.data()+40)||LoadLittle64(e.data()+48)||LoadLittle32(e.data()+56)||LoadLittle32(e.data()+60)) return FailView<TimeZoneMapViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_empty");
  } else {
    if(e.size()!=312||LoadLittle32(e.data()+56)!=108||LoadLittle32(e.data()+60)!=108) return FailView<TimeZoneMapViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_offsets");
    auto lo=DecodeTimeSortKeyNoAllocV3(h,e.subspan(96,108));auto hi=DecodeTimeSortKeyNoAllocV3(h,e.subspan(204,108));
    if(!lo.ok())return FailView<TimeZoneMapViewResultV3>(lo.diagnostic.diagnostic_code,lo.diagnostic.detail);
    if(!hi.ok())return FailView<TimeZoneMapViewResultV3>(hi.diagnostic.diagnostic_code,hi.diagnostic.detail);
    if(lo.value.state!=TimeValueStateV3::value||hi.value.state!=TimeValueStateV3::value||lo.value.direction!=TimeSortDirectionV3::ascending||hi.value.direction!=TimeSortDirectionV3::ascending||lo.value.null_mode!=TimeNullModeV3::nulls_last||hi.value.null_mode!=TimeNullModeV3::nulls_last)
      return FailView<TimeZoneMapViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_nested_key");
    const u64 ld=LoadLittle64(e.data()+40),ud=LoadLittle64(e.data()+48);
    if(ld!=lo.value.nanoseconds_since_midnight||ud!=hi.value.nanoseconds_since_midnight||ld>ud) return FailView<TimeZoneMapViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_extrema");
    minimum_nanoseconds=ld;maximum_nanoseconds=ud;minimum_key=e.subspan(96,108);maximum_key=e.subspan(204,108);
  }
  std::array<byte,312> canonical{};ArrayClearGuard<312> clear_canonical(&canonical);
  std::memcpy(canonical.data(),kZoneMagic,8);StoreLittle16(canonical.data()+8,1);
  StoreLittle16(canonical.data()+10,96);StoreLittle32(canonical.data()+12,static_cast<u32>(e.size()));
  StoreLittle32(canonical.data()+16,vc?5u:4u);StoreLittle64(canonical.data()+24,nc);StoreLittle64(canonical.data()+32,vc);
  if(vc){
    StoreLittle64(canonical.data()+40,minimum_nanoseconds);
    StoreLittle64(canonical.data()+48,maximum_nanoseconds);
    StoreLittle32(canonical.data()+56,108);StoreLittle32(canonical.data()+60,108);
    EncodeSortKeyRaw({&h,TimeValueStateV3::value,minimum_nanoseconds},TimeSortDirectionV3::ascending,TimeNullModeV3::nulls_last,canonical.data()+96);
    EncodeSortKeyRaw({&h,TimeValueStateV3::value,maximum_nanoseconds},TimeSortDirectionV3::ascending,TimeNullModeV3::nulls_last,canonical.data()+204);
  }
  std::array<byte,32> canonical_hash{};ArrayClearGuard<32> clear_hash(&canonical_hash);
  if(!HashRecord(kZoneDomain,std::span<const byte>(canonical.data(),e.size()),64,&canonical_hash))
    return FailView<TimeZoneMapViewResultV3>("RESOURCE.BUDGET_EXCEEDED","zone_reencode_hash");
  std::memcpy(canonical.data()+64,canonical_hash.data(),32);
  if(std::memcmp(e.data(),canonical.data(),e.size()))
    return FailView<TimeZoneMapViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_reencode");
  TimeZoneMapViewResultV3 r;r.value={&h,nc,vc,minimum_nanoseconds,maximum_nanoseconds,minimum_key,maximum_key};r.status=Ok();return r;
}

namespace {
void PutStatisticsIdentity(byte* p,const TimeValidatedProfileHandleV3& h) noexcept {
  PutCommonIdentity(p,h);
  StoreUuid(p+112,h.statistics_policy.uuid);StoreLittle64(p+128,h.statistics_policy.generation);
  StoreUuid(p+136,h.civil_day_policy.uuid);StoreLittle64(p+152,h.civil_day_policy.generation);
  StoreUuid(p+160,h.storage_epoch_policy.uuid);StoreLittle64(p+176,h.storage_epoch_policy.generation);
  StoreUuid(p+184,h.identity.ordering_policy.uuid);StoreLittle64(p+200,h.identity.ordering_policy.generation);
  StoreUuid(p+208,h.identity.hash_policy.uuid);StoreLittle64(p+224,h.identity.hash_policy.generation);
}
bool StatisticsIdentityEquals(const byte* p,const TimeValidatedProfileHandleV3& h) noexcept {
  return CommonIdentityEquals(p,h)&&LoadUuid(p+112)==h.statistics_policy.uuid&&LoadLittle64(p+128)==h.statistics_policy.generation&&
      LoadUuid(p+136)==h.civil_day_policy.uuid&&LoadLittle64(p+152)==h.civil_day_policy.generation&&
      LoadUuid(p+160)==h.storage_epoch_policy.uuid&&LoadLittle64(p+176)==h.storage_epoch_policy.generation&&
      LoadUuid(p+184)==h.identity.ordering_policy.uuid&&LoadLittle64(p+200)==h.identity.ordering_policy.generation&&
      LoadUuid(p+208)==h.identity.hash_policy.uuid&&LoadLittle64(p+224)==h.identity.hash_policy.generation;
}

bool EncodeStatisticsRaw(const TimeStatisticsProjectionV3& p,
                         const TimeValidatedProfileHandleV3& profile,
                         const platform::Uuid& provider_evidence_uuid,
                         std::vector<byte>* bytes,
                         const TimeExecutionControlV3* control = nullptr,
                         bool* cancelled_between_groups = nullptr) noexcept {
  if (cancelled_between_groups) *cancelled_between_groups = false;
  const u64 n = kTimeStatisticsHeaderBytesV3 + 16 * p.histogram.size() +
                48 * p.mcv.size();
  if (!Reserve(bytes, n)) return false;
  byte* b = bytes->data();
  std::memcpy(b, kStatisticsMagic, 8);
  StoreLittle16(b + 8, 1);
  StoreLittle16(b + 10, kTimeStatisticsHeaderBytesV3);
  StoreLittle32(b + 12, static_cast<u32>(n));
  const u32 flags = (p.histogram.empty() ? 0u : 1u) |
                    (p.mcv.empty() ? 0u : 2u) |
                    (p.value_count ? 4u : 0u) |
                    (p.mcv.empty() ? 0u : 32u);
  StoreLittle32(b + 16, flags);
  StoreLittle32(b + 20, p.histogram.size());
  StoreLittle32(b + 24, p.mcv.size());
  PutStatisticsIdentity(b + 32, profile);
  // Bytes 264..279 are reserved zero by the SBTIMS01 authority.
  StoreUuid(b + 280, p.statistics_uuid);
  StoreUuid(b + 296, p.source_object_uuid);
  StoreLittle64(b + 312, p.schema_epoch);
  StoreLittle64(b + 320, p.collection_epoch);
  StoreLittle64(b + 328, p.security_epoch);
  StoreLittle64(b + 336, p.row_count);
  StoreLittle64(b + 344, p.null_count);
  StoreLittle64(b + 352, p.value_count);
  // Generation 1 does not admit a distinct-count model.
  StoreLittle32(b + 360, 0);
  StoreLittle64(b + 368, 0);
  b[376] = p.value_count ? 1 : 0;
  b[377] = p.value_count ? 1 : 0;
  b[378] = 0;  // correlation unavailable
  b[379] = 1;  // authenticated complete-visible-population evidence supplied
  if (p.value_count) {
    StoreLittle64(b + 384, p.minimum_nanoseconds);
    StoreLittle64(b + 392, p.maximum_nanoseconds);
  }
  StoreLittle64(b + 400, 0x8000000000000000ULL);
  StoreUuid(b + 408, provider_evidence_uuid);
  std::memcpy(b + 456, profile.profile_fingerprint.data(), 32);
  StoreUuid(b + 488, kPrivacyUuid);
  StoreLittle64(b + 504, 1);
  // Bytes 512..527 are reserved zero by the SBTIMS01 authority.
  std::size_t off = kTimeStatisticsHeaderBytesV3;
  for (const auto& x : p.histogram) {
    StoreLittle64(b + off, x.inclusive_upper_nanoseconds);
    StoreLittle64(b + off + 8, x.cumulative_count);
    off += 16;
  }
  if (control && Cancelled(*control)) {
    if (cancelled_between_groups) *cancelled_between_groups = true;
    return false;
  }
  u32 rank = 1;
  for (const auto& x : p.mcv) {
    std::memcpy(b + off, x.value_hash.data(), 32);
    StoreLittle64(b + off + 32, x.frequency);
    StoreLittle32(b + off + 40, rank++);
    StoreLittle32(b + off + 44, 1);  // bit0: value_redacted
    off += 48;
  }
  std::array<byte, 32> digest{};
  ArrayClearGuard<32> clear_digest(&digest);
  if (!HashRecord(kStatisticsDomain, *bytes, 424, &digest)) return false;
  std::memcpy(b + 424, digest.data(), 32);
  return true;
}

bool ValidateStatisticsRecordNoAlloc(
    const TimeValidatedProfileHandleV3& profile,
    std::span<const byte> e) noexcept {
  if (e.size() < kTimeStatisticsHeaderBytesV3 ||
      e.size() > kTimeStatisticsMaximumBytesV3 ||
      std::memcmp(e.data(), kStatisticsMagic, 8) ||
      LoadLittle16(e.data() + 8) != 1 ||
      LoadLittle16(e.data() + 10) != kTimeStatisticsHeaderBytesV3 ||
      LoadLittle32(e.data() + 12) != e.size() || LoadLittle32(e.data() + 28))
    return false;
  const u32 hc = LoadLittle32(e.data() + 20), mc = LoadLittle32(e.data() + 24);
  const u64 expected = kTimeStatisticsHeaderBytesV3 + 16ull * hc + 48ull * mc;
  if (hc > 64 || mc > 64 || expected != e.size() ||
      !HashEquals(kStatisticsDomain, e, 424) ||
      !StatisticsIdentityEquals(e.data() + 32, profile) ||
      std::any_of(e.begin() + 264, e.begin() + 280,
                  [](byte value) { return value != 0; }) ||
      std::memcmp(e.data() + 456, profile.profile_fingerprint.data(), 32) ||
      LoadUuid(e.data() + 488) != kPrivacyUuid ||
      LoadLittle64(e.data() + 504) != 1 ||
      std::any_of(e.begin() + 512, e.begin() + 528,
                  [](byte value) { return value != 0; })) return false;
  const auto statistics_uuid = LoadUuid(e.data() + 280);
  const auto source_uuid = LoadUuid(e.data() + 296);
  const auto provider_uuid = LoadUuid(e.data() + 408);
  const u64 schema_epoch = LoadLittle64(e.data() + 312);
  const u64 collection_epoch = LoadLittle64(e.data() + 320);
  const u64 security_epoch = LoadLittle64(e.data() + 328);
  const u64 row_count = LoadLittle64(e.data() + 336);
  const u64 null_count = LoadLittle64(e.data() + 344);
  const u64 value_count = LoadLittle64(e.data() + 352);
  const u64 minimum = LoadLittle64(e.data() + 384);
  const u64 maximum = LoadLittle64(e.data() + 392);
  if (!IsV7(statistics_uuid) || Nil(source_uuid) || !IsV7(provider_uuid) ||
      statistics_uuid == source_uuid || statistics_uuid == provider_uuid ||
      source_uuid == provider_uuid || !schema_epoch || !collection_epoch ||
      !security_epoch || LoadLittle32(e.data() + 360) ||
      LoadLittle32(e.data() + 364) || LoadLittle64(e.data() + 368) ||
      LoadLittle32(e.data() + 380) ||
      LoadLittle64(e.data() + 400) != 0x8000000000000000ULL ||
      null_count > std::numeric_limits<u64>::max() - value_count ||
      row_count != null_count + value_count ||
      maximum > kTimeMaximumNanosecondsV3 ||
      (!value_count && (minimum || maximum)) ||
      (value_count && minimum > maximum)) return false;
  const u32 flags = LoadLittle32(e.data() + 16);
  const u32 want = (hc ? 1u : 0u) | (mc ? 2u : 0u) |
                   (value_count ? 4u : 0u) | (mc ? 32u : 0u);
  if (flags != want || e[376] != (value_count ? 1 : 0) ||
      e[377] != (value_count ? 1 : 0) || e[378] != 0 || e[379] != 1)
    return false;
  std::size_t off = kTimeStatisticsHeaderBytesV3;
  u64 prior_bound = 0, prior_count = 0;
  bool first = true;
  for (u32 i = 0; i < hc; ++i, off += 16) {
    const u64 bound = LoadLittle64(e.data() + off);
    const u64 cumulative = LoadLittle64(e.data() + off + 8);
    if (bound < minimum || bound > maximum || (!first && bound <= prior_bound) ||
        cumulative < prior_count || cumulative > value_count) return false;
    prior_bound = bound; prior_count = cumulative; first = false;
  }
  if (hc && (prior_bound != maximum || prior_count != value_count)) return false;
  u64 prior_frequency = std::numeric_limits<u64>::max();
  std::array<byte, 32> prior_hash{};
  first = true;
  u64 frequency_sum = 0;
  for (u32 i = 0; i < mc; ++i, off += 48) {
    const byte* hash = e.data() + off;
    const u64 frequency = LoadLittle64(hash + 32);
    if (!frequency || LoadLittle32(hash + 40) != i + 1 ||
        LoadLittle32(hash + 44) != 1 || frequency > prior_frequency ||
        (!first && frequency == prior_frequency &&
         !std::lexicographical_compare(prior_hash.begin(), prior_hash.end(),
                                       hash, hash + 32)) ||
        frequency > std::numeric_limits<u64>::max() - frequency_sum) return false;
    frequency_sum += frequency; prior_frequency = frequency;
    std::memcpy(prior_hash.data(), hash, 32); first = false;
  }
  return frequency_sum <= value_count;
}

}  // namespace


static TimeStatisticsDecodeResultV3 DecodeTimeStatisticsProjectionImpl(
    const std::shared_ptr<const TimeValidatedProfileHandleV3>& profile,
    std::span<const byte> encoded, const TimeExecutionControlV3& control,
    bool retain_profile) noexcept;

TimeBytesResultV3 EncodeTimeStatisticsProjectionV3(
    const TimeStatisticsProjectionV3& p, const TimeExecutionControlV3& c) noexcept {
  if (!p.profile || !ValidProfile(*p.profile)) return FailBytes("CTI.TEMPORAL.DESCRIPTOR_INVALID", "statistics_profile");
  if (!p.provider_evidence || !IsV7(p.provider_evidence->provider_evidence_uuid)) return FailBytes("CTI.TEMPORAL.DESCRIPTOR_INVALID", "statistics_provider_evidence");
  const auto provider_uuid = p.provider_evidence->provider_evidence_uuid;
  if (p.histogram.size() > 64 || p.mcv.size() > 64) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "statistics_count");
  if (!IsV7(p.statistics_uuid) || Nil(p.source_object_uuid) || p.statistics_uuid == p.source_object_uuid || p.statistics_uuid == provider_uuid || p.source_object_uuid == provider_uuid || !p.schema_epoch || !p.collection_epoch || !p.security_epoch)
    return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "statistics_instance_identity");
  if (p.null_count > std::numeric_limits<u64>::max() - p.value_count ||
      p.row_count != p.null_count + p.value_count ||
      p.minimum_maximum_present != (p.value_count != 0) ||
      p.maximum_nanoseconds > kTimeMaximumNanosecondsV3 ||
      (!p.value_count && (p.minimum_nanoseconds || p.maximum_nanoseconds)) ||
      (p.value_count && p.minimum_nanoseconds > p.maximum_nanoseconds))
    return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "statistics_counts");
  u64 prior_bound = 0, prior_count = 0; bool first = true;
  for (const auto& x : p.histogram) {
    if (x.inclusive_upper_nanoseconds < p.minimum_nanoseconds || x.inclusive_upper_nanoseconds > p.maximum_nanoseconds || (!first && x.inclusive_upper_nanoseconds <= prior_bound) || x.cumulative_count < prior_count || x.cumulative_count > p.value_count)
      return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "histogram");
    prior_bound = x.inclusive_upper_nanoseconds; prior_count = x.cumulative_count; first = false;
  }
  if (!p.histogram.empty() && (prior_bound != p.maximum_nanoseconds || prior_count != p.value_count)) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "histogram_total");
  u64 sum = 0, prior_frequency = std::numeric_limits<u64>::max(); std::array<byte,32> prior_hash{}; first = true;
  for (const auto& x : p.mcv) {
    if (!x.frequency || x.frequency > prior_frequency || (!first && x.frequency == prior_frequency && !std::lexicographical_compare(prior_hash.begin(), prior_hash.end(), x.value_hash.begin(), x.value_hash.end())) || x.frequency > std::numeric_limits<u64>::max() - sum)
      return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "mcv");
    sum += x.frequency; prior_frequency = x.frequency; prior_hash = x.value_hash; first = false;
  }
  if (sum > p.value_count) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "mcv_total");
  const u64 n = kTimeStatisticsHeaderBytesV3 + 16 * p.histogram.size() + 48 * p.mcv.size();
  if (n > c.maximum_allocation_bytes) return FailBytes("RESOURCE.BUDGET_EXCEEDED", "statistics_allocation");
  if (Cancelled(c)) return FailBytes("PROCESS.CANCELLED", "statistics_cancelled");
  auto profile_pin = p.profile; auto evidence_pin = p.provider_evidence;
  TimeBytesResultV3 r; VectorClearGuard clear(&r.bytes); bool cancelled_between_groups = false;
  if (!EncodeStatisticsRaw(p, *profile_pin, provider_uuid, &r.bytes, &c, &cancelled_between_groups))
    return cancelled_between_groups ? FailBytes("PROCESS.CANCELLED", "statistics_between_histogram_and_mcv") : FailBytes("RESOURCE.BUDGET_EXCEEDED", "statistics_allocation");
  if (!ValidateStatisticsRecordNoAlloc(*profile_pin, r.bytes)) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "statistics_self_check");
  if (Cancelled(c)) return FailBytes("PROCESS.CANCELLED", "statistics_prepublication");
  r.status = Ok(); clear.Disarm(); return r;
}

static TimeStatisticsDecodeResultV3 DecodeTimeStatisticsProjectionImpl(
    const std::shared_ptr<const TimeValidatedProfileHandleV3>& profile,
    std::span<const byte> e, const TimeExecutionControlV3& c,
    bool retain_profile) noexcept {
  if (!profile || !ValidProfile(*profile)) return FailView<TimeStatisticsDecodeResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID", "profile");
  if (!ValidateStatisticsRecordNoAlloc(*profile, e)) return FailView<TimeStatisticsDecodeResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "statistics_record");
  const u32 hc = LoadLittle32(e.data() + 20), mc = LoadLittle32(e.data() + 24);
  if (e.size() > c.maximum_allocation_bytes) return FailView<TimeStatisticsDecodeResultV3>("RESOURCE.BUDGET_EXCEEDED", "statistics_allocation");
  if (Cancelled(c)) return FailView<TimeStatisticsDecodeResultV3>("PROCESS.CANCELLED");
  std::shared_ptr<const TimeValidatedProfileHandleV3> profile_pin; if (retain_profile) profile_pin = profile;
  TimeStatisticsDecodeResultV3 r; auto& s = r.statistics; DecodedStatisticsClearGuard clear(&s);
  s.statistics_uuid = LoadUuid(e.data() + 280);
  s.source_object_uuid = LoadUuid(e.data() + 296);
  s.provider_evidence_uuid = LoadUuid(e.data() + 408);
  s.schema_epoch = LoadLittle64(e.data() + 312);
  s.collection_epoch = LoadLittle64(e.data() + 320);
  s.security_epoch = LoadLittle64(e.data() + 328);
  s.row_count = LoadLittle64(e.data() + 336);
  s.null_count = LoadLittle64(e.data() + 344);
  s.value_count = LoadLittle64(e.data() + 352);
  s.minimum_maximum_present = s.value_count != 0;
  s.minimum_nanoseconds = LoadLittle64(e.data() + 384);
  s.maximum_nanoseconds = LoadLittle64(e.data() + 392);
  try { s.histogram.reserve(hc); s.mcv.reserve(mc); } catch (...) { return FailView<TimeStatisticsDecodeResultV3>("RESOURCE.BUDGET_EXCEEDED"); }
  std::size_t off = kTimeStatisticsHeaderBytesV3;
  for (u32 i = 0; i < hc; ++i, off += 16) s.histogram.push_back({LoadLittle64(e.data() + off), LoadLittle64(e.data() + off + 8)});
  if (Cancelled(c)) return FailView<TimeStatisticsDecodeResultV3>("PROCESS.CANCELLED", "statistics_between_histogram_and_mcv");
  for (u32 i = 0; i < mc; ++i, off += 48) { s.mcv.emplace_back(); std::memcpy(s.mcv.back().value_hash.data(), e.data() + off, 32); s.mcv.back().frequency = LoadLittle64(e.data() + off + 32); }
  if (Cancelled(c)) return FailView<TimeStatisticsDecodeResultV3>("PROCESS.CANCELLED", "statistics_prepublication");
  if (retain_profile) s.profile = std::move(profile_pin);
  r.status = Ok();
  clear.Disarm();
  return r;
}

TimeStatisticsDecodeResultV3 DecodeTimeStatisticsProjectionV3(
    const std::shared_ptr<const TimeValidatedProfileHandleV3>& profile,
    std::span<const byte> encoded,
    const TimeExecutionControlV3& control) noexcept {
  return DecodeTimeStatisticsProjectionImpl(profile, encoded, control, true);
}

TimeStatisticsReceivingDispositionV3 ResolveTimeStatisticsReceivingFactsV3(
    const TimeDecodedStatisticsV3& d,const TimeStatisticsReceivingFactsV3& c) noexcept {
  if(!c.security_visible)return TimeStatisticsReceivingDispositionV3::security_denied;
  if(!c.privacy_admitted)return TimeStatisticsReceivingDispositionV3::privacy_denied;
  if(c.schema_epoch!=d.schema_epoch)return TimeStatisticsReceivingDispositionV3::schema_epoch_mismatch;
  if(c.collection_epoch!=d.collection_epoch)return TimeStatisticsReceivingDispositionV3::collection_epoch_mismatch;
  if(c.security_epoch!=d.security_epoch)return TimeStatisticsReceivingDispositionV3::security_epoch_mismatch;
  if(c.provider_evidence_uuid!=d.provider_evidence_uuid)return TimeStatisticsReceivingDispositionV3::provider_evidence_mismatch;
  return TimeStatisticsReceivingDispositionV3::admitted;
}

TimeStatisticsReceivingDispositionV3 ResolveTimeStatisticsEvidenceV3(
    TimeStatisticsEvidenceClassV3 evidence) noexcept {
  using E=TimeStatisticsEvidenceClassV3;
  using D=TimeStatisticsReceivingDispositionV3;
  switch(evidence){
    case E::full_visible_population:return D::admitted;
    case E::partial_scan:case E::weighted_input:case E::sampled_input:return D::full_recollection_required;
    case E::snapshot_merge:return D::manual_review_required;
    case E::histogram_source_contradiction:return D::provider_evidence_mismatch;
  }
  return D::provider_evidence_mismatch;
}

TimeStatisticsReadAdmissionResultV3 AdmitTimeStatisticsReadV3(
    const std::shared_ptr<const TimeValidatedProfileHandleV3>& profile,
    std::span<const byte> encoded,
    const TimeStatisticsReceivingFactsV3& current,
    const TimeExecutionControlV3& control) noexcept {
  using D = TimeStatisticsReadAdmissionDispositionV3;
  TimeStatisticsReadAdmissionResultV3 result;
  const auto refuse_before_decode = [&](D disposition, std::string_view code,
                                        std::string_view detail) {
    result.disposition = disposition;
    result.diagnostic = {Error(), code, detail};
    return result;
  };
  if (!current.security_visible) {
    return refuse_before_decode(D::security_denied, "SECURITY.ACCESS_DENIED",
                                "statistics_read_security");
  }
  if (!current.privacy_admitted) {
    return refuse_before_decode(D::privacy_denied,
                                "OPTIMIZER.STATISTICS_PRIVACY_DENIED",
                                "statistics_read_privacy");
  }

  result.decode_attempted = true;
  auto decoded = DecodeTimeStatisticsProjectionV3(profile, encoded, control);
  if (!decoded.ok()) {
    if (decoded.diagnostic.diagnostic_code == "CTI.TEMPORAL.DESCRIPTOR_INVALID")
      result.disposition = D::profile_refused;
    else if (decoded.diagnostic.diagnostic_code == "RESOURCE.BUDGET_EXCEEDED")
      result.disposition = D::resource_refused;
    else if (decoded.diagnostic.diagnostic_code == "PROCESS.CANCELLED")
      result.disposition = D::cancelled;
    else
      result.disposition = D::format_refused;
    result.diagnostic = decoded.diagnostic;
    return result;
  }
  switch (ResolveTimeStatisticsReceivingFactsV3(decoded.statistics, current)) {
    case TimeStatisticsReceivingDispositionV3::admitted:
      result.disposition = D::admitted;
      result.statistics = std::move(decoded.statistics);
      return result;
    case TimeStatisticsReceivingDispositionV3::schema_epoch_mismatch:
      result.disposition = D::schema_epoch_mismatch;
      break;
    case TimeStatisticsReceivingDispositionV3::collection_epoch_mismatch:
      result.disposition = D::collection_epoch_mismatch;
      break;
    case TimeStatisticsReceivingDispositionV3::security_epoch_mismatch:
      result.disposition = D::security_epoch_mismatch;
      break;
    case TimeStatisticsReceivingDispositionV3::provider_evidence_mismatch:
      result.disposition = D::provider_evidence_mismatch;
      result.diagnostic = {Error(), "CTI.TEMPORAL.DESCRIPTOR_INVALID",
                           "statistics_provider_evidence"};
      return result;
    case TimeStatisticsReceivingDispositionV3::security_denied:
    case TimeStatisticsReceivingDispositionV3::privacy_denied:
    case TimeStatisticsReceivingDispositionV3::full_recollection_required:
    case TimeStatisticsReceivingDispositionV3::manual_review_required:
      result.disposition = D::provider_evidence_mismatch;
      result.diagnostic = {Error(), "CTI.TEMPORAL.DESCRIPTOR_INVALID",
                           "statistics_receiving_facts"};
      return result;
  }
  result.diagnostic = {Error(), "OPTIMIZER.STATISTICS_EPOCH_MISMATCH",
                       "statistics_epoch"};
  return result;
}

namespace {
void PutBackupIdentity(byte* p,const TimeValidatedProfileHandleV3& h) noexcept {
  PutCommonIdentity(p,h);std::memcpy(p+112,h.profile_fingerprint.data(),32);
  StoreUuid(p+144,h.civil_day_policy.uuid);StoreLittle64(p+160,h.civil_day_policy.generation);
  StoreUuid(p+168,h.storage_epoch_policy.uuid);StoreLittle64(p+184,h.storage_epoch_policy.generation);
  StoreUuid(p+192,h.backup_transport_policy.uuid);StoreLittle64(p+208,h.backup_transport_policy.generation);
  StoreUuid(p+216,h.protection_policy.uuid);StoreLittle64(p+232,h.protection_policy.generation);
}
bool BackupIdentityEquals(const byte* p,const TimeValidatedProfileHandleV3& h) noexcept {
  return CommonIdentityEquals(p,h)&&!std::memcmp(p+112,h.profile_fingerprint.data(),32)&&LoadUuid(p+144)==h.civil_day_policy.uuid&&LoadLittle64(p+160)==h.civil_day_policy.generation&&LoadUuid(p+168)==h.storage_epoch_policy.uuid&&LoadLittle64(p+184)==h.storage_epoch_policy.generation&&LoadUuid(p+192)==h.backup_transport_policy.uuid&&LoadLittle64(p+208)==h.backup_transport_policy.generation&&LoadUuid(p+216)==h.protection_policy.uuid&&LoadLittle64(p+232)==h.protection_policy.generation;
}
}

TimeBytesResultV3 EncodeTimeBackupTupleV3(const TimeOwnedValueV3& v,
                                          const TimeExecutionControlV3& c) noexcept {
  auto chk=ValidateTimeValueViewV3(v.view(),true);if(!chk.ok())return FailBytes(chk.diagnostic.diagnostic_code,chk.diagnostic.detail);const u64 n=v.state==TimeValueStateV3::sql_null?296:304;
  auto profile_pin=v.profile;
  if(n>c.maximum_allocation_bytes)return FailBytes("RESOURCE.BUDGET_EXCEEDED","backup_allocation");
  if(Cancelled(c))return FailBytes("PROCESS.CANCELLED","backup_cancelled");
  TimeBytesResultV3 r;VectorClearGuard clear(&r.bytes);
  if(!Reserve(&r.bytes,n))return FailBytes("RESOURCE.BUDGET_EXCEEDED","backup_allocation");
  std::memcpy(r.bytes.data(),kBackupMagic,8);StoreLittle16(r.bytes.data()+8,1);StoreLittle16(r.bytes.data()+10,296);StoreLittle32(r.bytes.data()+12,n);PutBackupIdentity(r.bytes.data()+16,*v.profile);
  r.bytes[256]=v.state==TimeValueStateV3::sql_null?0:1;r.bytes[257]=v.state==TimeValueStateV3::sql_null?0:8;if(n==304)StoreLittle64(r.bytes.data()+296,v.nanoseconds_since_midnight);
  std::array<byte,32>d{};ArrayClearGuard<32> clear_digest(&d);if(!HashRecord(kBackupDomain,r.bytes,264,&d))return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","backup_hash_provider");std::memcpy(r.bytes.data()+264,d.data(),32);
  if(!DecodeTimeBackupTupleNoAllocV3(*v.profile,true,r.bytes).ok())return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","backup_self_check");
  if(Cancelled(c))return FailBytes("PROCESS.CANCELLED","backup_prepublication");
  r.status=Ok();clear.Disarm();return r;
}

TimeBackupTupleViewResultV3 DecodeTimeBackupTupleNoAllocV3(
    const TimeValidatedProfileHandleV3& h,bool null_allowed,std::span<const byte> e) noexcept {
  if((e.size()!=296&&e.size()!=304)||std::memcmp(e.data(),kBackupMagic,8)||LoadLittle16(e.data()+8)!=1||LoadLittle16(e.data()+10)!=296||LoadLittle32(e.data()+12)!=e.size()||LoadLittle32(e.data()+116)||e[258]||std::any_of(e.begin()+259,e.begin()+264,[](byte b){return b!=0;}))return FailView<TimeBackupTupleViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","backup_structure");
  if(!HashEquals(kBackupDomain,e,264))return FailView<TimeBackupTupleViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","backup_hash");
  if(!ValidProfile(h))return FailView<TimeBackupTupleViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","profile");
  if(!BackupIdentityEquals(e.data()+16,h))return FailView<TimeBackupTupleViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","backup_identity");
  TimeValueStateV3 state=TimeValueStateV3::value;u64 value=0;PlainObjectClearGuard<TimeValueStateV3> clear_state(&state);PlainObjectClearGuard<u64> clear_value(&value);
  if(e[256]==0){if(e[257]!=0||e.size()!=296)return FailView<TimeBackupTupleViewResultV3>("DATATYPE.NULL_STATE.INVALID","backup_null_pair");if(!null_allowed)return FailView<TimeBackupTupleViewResultV3>("DATATYPE.NULL_NOT_ADMITTED");state=TimeValueStateV3::sql_null;}
  else if(e[256]==1){if(e[257]!=8||e.size()!=304)return FailView<TimeBackupTupleViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","backup_value_pair");state=TimeValueStateV3::value;value=LoadLittle64(e.data()+296);if(value>kTimeMaximumNanosecondsV3)return FailView<TimeBackupTupleViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","backup_range");}
  else return FailView<TimeBackupTupleViewResultV3>("DATATYPE.NULL_STATE.INVALID","backup_state");
  const auto component=state==TimeValueStateV3::sql_null?std::span<const byte>{}:e.subspan(296,8);
  const auto decoded_component=DecodeCanonicalTimeComponentNoAllocV3(
      h,state,null_allowed,component);
  if(!decoded_component.ok()||decoded_component.value.state!=state||
     decoded_component.value.nanoseconds_since_midnight!=value)
    return FailView<TimeBackupTupleViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","backup_component_decode");
  std::array<byte,304> canonical{};ArrayClearGuard<304> clear_canonical(&canonical);
  std::memcpy(canonical.data(),kBackupMagic,8);StoreLittle16(canonical.data()+8,1);
  StoreLittle16(canonical.data()+10,296);StoreLittle32(canonical.data()+12,static_cast<u32>(e.size()));
  PutBackupIdentity(canonical.data()+16,h);canonical[256]=state==TimeValueStateV3::sql_null?0:1;
  canonical[257]=state==TimeValueStateV3::sql_null?0:8;
  if(e.size()==304)StoreLittle64(canonical.data()+296,value);
  std::array<byte,32> canonical_hash{};ArrayClearGuard<32> clear_hash(&canonical_hash);
  if(!HashRecord(kBackupDomain,std::span<const byte>(canonical.data(),e.size()),264,&canonical_hash))
    return FailView<TimeBackupTupleViewResultV3>("RESOURCE.BUDGET_EXCEEDED","backup_reencode_hash");
  std::memcpy(canonical.data()+264,canonical_hash.data(),32);
  if(std::memcmp(e.data(),canonical.data(),e.size()))
    return FailView<TimeBackupTupleViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","backup_reencode");
  TimeBackupTupleViewResultV3 r;r.tuple={&h,state,value};r.status=Ok();return r;
}

TimeIndexProjectionResultV3 ProjectTimeIndexValueV3(
    const TimeIndexProjectionRequestV3& q) noexcept {
  TimeIndexResolutionV3 resolution;
  resolution.family=q.compatibility.family;
  if(!q.value||!q.value->profile)return FailIndex(resolution,"CTI.TEMPORAL.DESCRIPTOR_INVALID","value_or_profile_missing");
  auto profile_checked=ValidateTimeProfileHandleV3(*q.value->profile);
  if(!profile_checked.ok())return FailIndex(resolution,"CTI.TEMPORAL.DESCRIPTOR_INVALID","profile_invalid");
  const auto requested_admission =
      AdmitTimeIndexCompatibilityV3(q.compatibility);
  if (!requested_admission.admitted)
    return FailIndex(resolution,"OPTIMIZER.INDEX_COMPATIBILITY_MISSING",
                     "index_compatibility_identity");
  const auto requested_resolution = requested_admission.resolution;
  resolution=requested_resolution;
  if(resolution.disposition==TimeIndexDispositionV3::not_applicable) {
    auto refused = FailIndex(resolution,"OPTIMIZER.INDEX_COMPATIBILITY_MISSING",
                             "family_not_applicable");
    refused.owner_fact_only = true;
    return refused;
  }
  TimeIndexFamilyV3 effective_family=q.compatibility.family;
  if(resolution.disposition==TimeIndexDispositionV3::conditional){
    if(!q.selected_conditional_family)return OwnerFact(resolution);
    const auto selected_identity = TimeIndexCompatibilityIdentityV3{
        q.selected_conditional_family->family,
        q.selected_conditional_family->compatibility_uuid,
        q.selected_conditional_family->compatibility_generation};
    const auto selected = AdmitTimeIndexCompatibilityV3(selected_identity);
    if(!selected.admitted ||
       !SameResolution(selected.resolution,*q.selected_conditional_family)||
       selected.resolution.disposition==TimeIndexDispositionV3::conditional||
       selected.resolution.disposition==TimeIndexDispositionV3::not_applicable)
      return OwnerFact(resolution);
    resolution=selected.resolution;effective_family=resolution.family;
  }
  if(!ModeAllowed(effective_family,q.mode))return FailIndex(resolution,"CTI.TEMPORAL.INDEX_KEY_REFUSED","selected_mode_invalid");
  if(effective_family==TimeIndexFamilyV3::temporary_work){
    switch(q.mode){
      case TimeProjectionModeV3::equality_hash:resolution.projection=TimeProjectionKindV3::equality_hash;resolution.exact_recheck_required=true;break;
      case TimeProjectionModeV3::ascending_nulls_first:case TimeProjectionModeV3::ascending_nulls_last:case TimeProjectionModeV3::descending_nulls_first:case TimeProjectionModeV3::descending_nulls_last:resolution.projection=TimeProjectionKindV3::sort_key;resolution.exact_recheck_required=false;break;
      default:return FailIndex(resolution,"CTI.TEMPORAL.INDEX_KEY_REFUSED","temporary_mode");
    }
  }
  const auto checked=ValidateProjectionValueMetadata(*q.value,true);
  if(!checked.status.ok())return FailIndex(resolution,checked.diagnostic_code,checked.detail);
  if(resolution.projection==TimeProjectionKindV3::range_pair){
    if(!q.upper_value)return FailIndex(resolution,"CTI.TEMPORAL.INDEX_KEY_REFUSED","upper_missing");
    const auto upper=ValidateProjectionValueMetadata(*q.upper_value,false,q.value->profile.get());
    if(!upper.status.ok())return FailIndex(resolution,upper.diagnostic_code,upper.detail);
    if(q.value->state!=TimeValueStateV3::value)
      return FailIndex(resolution,"DATATYPE.NULL_NOT_ADMITTED","range_lower_null");
  }
  if(resolution.projection==TimeProjectionKindV3::zone_map){
    if(!q.zone_map)return FailIndex(resolution,"CTI.TEMPORAL.INDEX_KEY_REFUSED","zone_request_missing");
    if(!q.zone_map->profile||!SameProfile(*q.value->profile,*q.zone_map->profile))
      return FailIndex(resolution,"CTI.TEMPORAL.DESCRIPTOR_INVALID","zone_profile_mismatch");
    if(q.zone_map->value_count>std::numeric_limits<u64>::max()-q.zone_map->null_count)
      return FailIndex(resolution,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_count_overflow");
    if(!q.zone_map->value_count&&(q.zone_map->minimum||q.zone_map->maximum))
      return FailIndex(resolution,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_extrema_for_empty");
    if(q.zone_map->value_count){
      if(!q.zone_map->minimum||!q.zone_map->maximum)
        return FailIndex(resolution,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_extrema_missing");
      const auto lo=ValidateProjectionValueMetadata(*q.zone_map->minimum,false,q.zone_map->profile.get());
      if(!lo.status.ok())return FailIndex(resolution,lo.diagnostic_code,lo.detail);
      const auto hi=ValidateProjectionValueMetadata(*q.zone_map->maximum,false,q.zone_map->profile.get());
      if(!hi.status.ok())return FailIndex(resolution,hi.diagnostic_code,hi.detail);
    }
  }
  u64 need=0;
  switch(resolution.projection){
    case TimeProjectionKindV3::equality_hash:need=32;break;
    case TimeProjectionKindV3::sort_key:need=q.value->state==TimeValueStateV3::sql_null?100:108;break;
    case TimeProjectionKindV3::covering_value:need=q.value->state==TimeValueStateV3::sql_null?1:9;break;
    case TimeProjectionKindV3::range_pair:need=216;break;
    case TimeProjectionKindV3::zone_map:need=q.zone_map->value_count?312:96;break;
    default:return OwnerFact(resolution);
  }
  auto operation_pin=q.value->profile;
  const u64 provider_limit=resolution.projection==TimeProjectionKindV3::zone_map
      ?std::min(q.provider_max_key_bytes,q.zone_map->provider_max_key_bytes)
      :q.provider_max_key_bytes;
  if(provider_limit<need)return FailIndex(resolution,"CTI.TEMPORAL.INDEX_KEY_REFUSED","provider_budget");
  if(q.control.maximum_allocation_bytes<need)return FailIndex(resolution,"RESOURCE.BUDGET_EXCEEDED","allocation_grant");
  if(Cancelled(q.control))return FailIndex(resolution,"PROCESS.CANCELLED","before_allocation");
  TimeBytesResultV3 b;VectorClearGuard clear_bytes(&b.bytes);
  if(resolution.projection==TimeProjectionKindV3::zone_map){
    TimeExecutionControlV3 inner=q.control;inner.cancelled=nullptr;inner.cancellation_context=nullptr;
    b=EncodeTimeZoneMapImpl(*q.zone_map,provider_limit,inner,false);
  }else{
    if(!Reserve(&b.bytes,need))return FailIndex(resolution,"RESOURCE.BUDGET_EXCEEDED","allocation");
    switch(resolution.projection){
      case TimeProjectionKindV3::equality_hash:{
        TimeExecutionControlV3 inner{32, nullptr, nullptr};
        const auto written = HashTimeValueIntoNoAllocV3(
            *q.value, b.bytes.data(), b.bytes.size(), inner);
        if(!written.ok())return FailIndex(resolution,
            written.diagnostic.diagnostic_code,written.diagnostic.detail);
        break;}
      case TimeProjectionKindV3::sort_key:{
        TimeSortDirectionV3 d=TimeSortDirectionV3::ascending;TimeNullModeV3 n=TimeNullModeV3::nulls_last;
        if(q.mode==TimeProjectionModeV3::ascending_nulls_first)n=TimeNullModeV3::nulls_first;
        else if(q.mode==TimeProjectionModeV3::descending_nulls_first){d=TimeSortDirectionV3::descending;n=TimeNullModeV3::nulls_first;}
        else if(q.mode==TimeProjectionModeV3::descending_nulls_last)d=TimeSortDirectionV3::descending;
        EncodeSortKeyRaw(q.value->view(),d,n,b.bytes.data());
        if(!DecodeTimeSortKeyNoAllocV3(*q.value->profile,b.bytes).ok())return FailIndex(resolution,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","sort_key_self_check");
        break;}
      case TimeProjectionKindV3::covering_value:
        b.bytes[0]=q.value->state==TimeValueStateV3::sql_null?0:2;
        if(need==9)StoreLittle64(b.bytes.data()+1,q.value->nanoseconds_since_midnight);
        if(!DecodeTimeCoveringValueNoAllocV3(*q.value->profile,true,b.bytes).ok())return FailIndex(resolution,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","covering_self_check");
        break;
      case TimeProjectionKindV3::range_pair:
        if(q.value->nanoseconds_since_midnight>q.upper_value->nanoseconds_since_midnight)return FailIndex(resolution,"CTI.TEMPORAL.INDEX_KEY_REFUSED","range_order");
        EncodeSortKeyRaw(q.value->view(),TimeSortDirectionV3::ascending,TimeNullModeV3::nulls_last,b.bytes.data());
        EncodeSortKeyRaw(q.upper_value->view(),TimeSortDirectionV3::ascending,TimeNullModeV3::nulls_last,b.bytes.data()+108);
        if(!DecodeTimeRangePairNoAllocV3(*q.value->profile,b.bytes).ok())return FailIndex(resolution,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","range_self_check");
        break;
      default:break;
    }
    b.status=Ok();
  }
  if(!b.ok()){TimeIndexProjectionResultV3 r;r.status=b.status;r.diagnostic=std::move(b.diagnostic);r.resolution=resolution;return r;}
  if(Cancelled(q.control))return FailIndex(resolution,"PROCESS.CANCELLED","before_publication");
  TimeIndexProjectionResultV3 r;r.status=Ok();
  r.requested_resolution=requested_resolution;r.resolution=resolution;
  r.bytes=std::move(b.bytes);clear_bytes.Disarm();return r;
}

TimeProtectionResolutionV3 ResolveTimeProtectionV3(TimeProtectionCellV3 c) noexcept {
  using C=TimeProtectionCellV3;
  switch(c){
    case C::canonical_plain:return {c,true,false,"admitted_datatype_component","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID"};
    case C::inner_compression:case C::inner_encryption:return {c,false,false,"forbidden","CTI.TEMPORAL.PROTECTION_UNSUPPORTED"};
    case C::protected_index:return {c,false,false,"forbidden_current_core","CTI.TEMPORAL.PROTECTION_UNSUPPORTED"};
    case C::outer_compression:case C::outer_authenticated_protection:case C::outer_compress_then_protect:case C::outer_protect_then_compress:return {c,false,true,"unsupported_current_core","receiving_outer_owner_typed_handoff_no_datatype_algorithm_or_order_claim"};
    case C::page_or_filespace_crypto:case C::transport_tls:return {c,false,true,"not_applicable_datatype_layer","owner_diagnostic_before_datatype"};
  }
  return {c,false,true,"not_applicable_datatype_layer","owner_diagnostic_before_datatype"};
}

TimeWireLaneResolutionV3 ResolveTimeWireLaneV3(TimeWireLaneV3 l) noexcept {
  if (static_cast<unsigned>(l) >= 27)
    return {l,false,false,true,"unknown_lane_fail_closed",
            "CTI.TRANSPORT.UNSUPPORTED"};
  const bool native=static_cast<unsigned>(l)<=static_cast<unsigned>(TimeWireLaneV3::canonical_sblr);
  return {l,false,native,true,native?"unsupported_current_outer_version_unallocated_datatype_component_mapping_complete":"unsupported_no_manifest_listed_compatibility_profile","CTI.TRANSPORT.UNSUPPORTED"};
}

std::span<const TimeDiagnosticRouteV3> TimeDiagnosticRoutesV3() noexcept {
  static constexpr std::array<TimeDiagnosticRouteV3,42> rows{{
    {TimeDiagnosticAxisV3::input_parse,"SBLR.OPERAND_INVALID",Uuid({0xf7,0xc7,0x36,0x87,0xc7,0x8d,0x54,0x6a,0xbc,0x93,0x02,0x8b,0x52,0xb4,0x35,0xcf}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::input_parse,"CTI.TEMPORAL.INVALID_LITERAL",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x02,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x02}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::input_parse,"CTI.TEMPORAL.LEAP_SECOND_REFUSED",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x59}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::input_parse,"CTI.TEMPORAL.PRECISION_LOSS",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x58}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::descriptor_codec,"SECURITY.ACCESS_DENIED",Uuid({0x1a,0x8d,0x26,0xb0,0x87,0x55,0x56,0xad,0xa1,0x70,0xb2,0x4a,0x3a,0xaf,0x14,0x07}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::descriptor_codec,"CTI.TEMPORAL.DESCRIPTOR_INVALID",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x01,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x01}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::storage_read_write,"STORAGE.PAGE_CHECKSUM_FAILED",Uuid({0x0c,0x7d,0x9a,0xaf,0x92,0xe6,0x59,0x05,0x9d,0xaf,0x09,0x74,0xb8,0x71,0xf9,0x4a}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::storage_read_write,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x03,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x03}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::cast_invalid,"DATATYPE.CAST_FORBIDDEN",Uuid({0x0d,0xc5,0x1a,0x1c,0xad,0x21,0x57,0xb5,0xae,0x65,0xe0,0xfe,0x55,0x89,0x02,0x1a}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::cast_invalid,"CTI.TEMPORAL.INVALID_LITERAL",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x02,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x02}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::cast_invalid,"CTI.TEMPORAL.LEAP_SECOND_REFUSED",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x59}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::cast_range_loss,"CTI.TEMPORAL.PRECISION_LOSS",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x58}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::cast_range_loss,"CTB.TEXT.LENGTH_EXCEEDED",Uuid({0xb3,0x99,0xb5,0x14,0xf7,0x71,0x50,0x14,0xa5,0xb4,0x90,0x77,0xf5,0xde,0xe3,0x69}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::operation_invalid,"CTI.TEMPORAL.OPERATION_REFUSED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x05,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x05}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::operation_invalid,"CTI.INTERVAL.CALENDAR_OPERATION_REFUSED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x0c,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0c}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::operation_invalid,"CTI.TEMPORAL.ORDERING_REFUSED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x06,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x06}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::bounds_overflow_underflow,"CTI.TEMPORAL.RANGE_EXCEEDED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x04,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x04}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::bounds_overflow_underflow,"RESOURCE.BUDGET_EXCEEDED",Uuid({0xde,0xdc,0x1c,0x9b,0x38,0x79,0x57,0x70,0x8f,0x8e,0xc0,0xc9,0x27,0xa0,0xc3,0x4d}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::resource_cancellation,"RESOURCE.BUDGET_EXCEEDED",Uuid({0xde,0xdc,0x1c,0x9b,0x38,0x79,0x57,0x70,0x8f,0x8e,0xc0,0xc9,0x27,0xa0,0xc3,0x4d}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::resource_cancellation,"PROCESS.CANCELLED",Uuid({0x0f,0x42,0x5d,0xe2,0x6b,0xc7,0x51,0xd4,0x8d,0x34,0x72,0xd1,0x07,0xad,0x45,0xe7}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::compression_corruption,"SBLR.ENVELOPE.CHECKSUM_INVALID",Uuid({0x26,0x0f,0x5e,0xac,0xb2,0x86,0x5a,0x00,0x81,0x6c,0xc4,0xc2,0xa8,0xf1,0xa2,0x5e}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::compression_corruption,"CTI.TEMPORAL.PROTECTION_UNSUPPORTED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x09,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x09}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::encryption_auth_key,"SECURITY.ACCESS_DENIED",Uuid({0x1a,0x8d,0x26,0xb0,0x87,0x55,0x56,0xad,0xa1,0x70,0xb2,0x4a,0x3a,0xaf,0x14,0x07}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::encryption_auth_key,"CTI.TEMPORAL.PROTECTION_UNSUPPORTED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x09,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x09}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::wire_decode_encode,"SBLR.OPERAND_INVALID",Uuid({0xf7,0xc7,0x36,0x87,0xc7,0x8d,0x54,0x6a,0xbc,0x93,0x02,0x8b,0x52,0xb4,0x35,0xcf}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::wire_decode_encode,"SBLR.ENVELOPE.CHECKSUM_INVALID",Uuid({0x26,0x0f,0x5e,0xac,0xb2,0x86,0x5a,0x00,0x81,0x6c,0xc4,0xc2,0xa8,0xf1,0xa2,0x5e}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::wire_decode_encode,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x03,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x03}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::wire_decode_encode,"CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x08,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x08}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::wire_decode_encode,"CTI.TRANSPORT.UNSUPPORTED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x0d,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0d}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::index_key,"CTI.TEMPORAL.ORDERING_REFUSED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x06,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x06}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::index_key,"OPTIMIZER.INDEX_COMPATIBILITY_MISSING",Uuid({0x01,0xa1,0x0c,0x00,0x20,0x00,0x70,0x0d,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0d}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::index_key,"CTI.TEMPORAL.INDEX_KEY_REFUSED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x07,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x07}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::index_key,"RESOURCE.BUDGET_EXCEEDED",Uuid({0xde,0xdc,0x1c,0x9b,0x38,0x79,0x57,0x70,0x8f,0x8e,0xc0,0xc9,0x27,0xa0,0xc3,0x4d}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::domain_validation,"DATATYPE.NULL_STATE.INVALID",Uuid({0x58,0x8f,0xaf,0xd0,0x63,0x86,0x55,0xcf,0x84,0x4e,0xf5,0xb0,0xdc,0x22,0xc7,0x00}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::domain_validation,"DATATYPE.NULL_NOT_ADMITTED",Uuid({0xab,0xa0,0x82,0x8d,0x77,0xc9,0x50,0xde,0x98,0x42,0x7f,0x20,0x47,0x10,0x02,0x14}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::domain_validation,"DOMAIN.CONSTRAINT_FAILED",Uuid({0x62,0xbe,0x50,0x52,0xad,0xbc,0x59,0xaa,0xb8,0x4b,0xae,0x60,0xa8,0xc4,0x0c,0x0f}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::recovery_corruption,"STORAGE.PAGE_CHECKSUM_FAILED",Uuid({0x0c,0x7d,0x9a,0xaf,0x92,0xe6,0x59,0x05,0x9d,0xaf,0x09,0x74,0xb8,0x71,0xf9,0x4a}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::recovery_corruption,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x03,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x03}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::recovery_corruption,"CTI.MERGE.MANUAL_REVIEW_REQUIRED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x0e,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0e}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::statistics_read,"SECURITY.ACCESS_DENIED",Uuid({0x1a,0x8d,0x26,0xb0,0x87,0x55,0x56,0xad,0xa1,0x70,0xb2,0x4a,0x3a,0xaf,0x14,0x07}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::statistics_read,"OPTIMIZER.STATISTICS_PRIVACY_DENIED",Uuid({0x01,0xa1,0x0c,0x00,0x20,0x00,0x70,0x0c,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0c}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {TimeDiagnosticAxisV3::statistics_read,"OPTIMIZER.STATISTICS_EPOCH_MISMATCH",Uuid({0x01,0xa1,0x0c,0x00,0x20,0x00,0x70,0x0b,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0b}),"request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
  }}; return rows;
}

TimeDiagnosticRouteExecutionFactV3 ExecuteTimeDiagnosticRouteV3(
    const TimeDiagnosticRouteExecutionRequestV3& request) noexcept {
  using D = TimeDiagnosticRouteExecutionDispositionV3;
  using S = TimeDiagnosticRouteScalarTypeV3;
  TimeDiagnosticRouteExecutionFactV3 result;
  const auto refuse = [&](D disposition) {
    result.disposition = disposition;
    return result;
  };

  const TimeDiagnosticRouteV3* route = nullptr;
  bool axis_code_seen = false;
  bool axis_uuid_seen = false;
  for (const auto& candidate : TimeDiagnosticRoutesV3()) {
    axis_code_seen |= candidate.axis == request.axis &&
                      candidate.code == request.diagnostic_code;
    axis_uuid_seen |= candidate.axis == request.axis &&
                      candidate.diagnostic_uuid == request.diagnostic_uuid;
    if (candidate.axis == request.axis &&
        candidate.code == request.diagnostic_code &&
        candidate.diagnostic_uuid == request.diagnostic_uuid) {
      route = &candidate;
      break;
    }
  }
  if (!route) {
    return refuse(axis_code_seen || axis_uuid_seen ? D::code_uuid_mismatch
                                                   : D::unknown_route);
  }
  if (request.redaction != route->redaction) return refuse(D::redaction_mismatch);

  static constexpr std::array<std::string_view, 5> expected_tokens{{
      "request_uuid", "operation_uuid", "statement_receipt_uuid",
      "failure_offset_u32", "reason_enum"}};
  static constexpr std::array<S, 5> expected_types{{
      S::uuid, S::uuid, S::uuid, S::u32, S::closed_enum}};
  static constexpr std::array<std::string_view, 8> known_tokens{{
      "request_uuid", "operation_uuid", "statement_receipt_uuid",
      "failure_offset_u32", "reason_enum", "input_extent_u32",
      "maximum_fraction_digits_u8", "second_u8"}};
  static constexpr std::array<std::string_view, 7> reasons{{
      "fixture", "grammar", "precision", "leap_second", "range",
      "resource", "corruption"}};
  static constexpr platform::Uuid receipt = Uuid({
      0x01,0x9d,0x00,0x00,0x00,0x00,0x70,0x00,
      0x80,0x00,0x00,0x00,0x00,0x00,0xd7,0x08});

  if (request.ordered_payload.size() < expected_tokens.size())
    return refuse(D::missing_token);
  if (request.ordered_payload.size() > expected_tokens.size())
    return refuse(D::extra_token);

  bool token_order_exact = true;
  std::array<bool, 5> expected_seen{};
  for (std::size_t i = 0; i != expected_tokens.size(); ++i) {
    const auto token = request.ordered_payload[i].token;
    token_order_exact &= token == expected_tokens[i];
    const auto expected = std::find(expected_tokens.begin(),
                                    expected_tokens.end(), token);
    if (expected != expected_tokens.end()) {
      const auto index = static_cast<std::size_t>(expected - expected_tokens.begin());
      if (expected_seen[index]) return refuse(D::mismatched_known_token);
      expected_seen[index] = true;
      continue;
    }
    if (std::find(known_tokens.begin(), known_tokens.end(), token) ==
        known_tokens.end()) return refuse(D::unknown_token);
    return refuse(D::mismatched_known_token);
  }
  if (!token_order_exact) {
    if (std::all_of(expected_seen.begin(), expected_seen.end(),
                    [](bool value) { return value; }))
      return refuse(D::reordered_tokens);
    return refuse(D::mismatched_known_token);
  }

  for (std::size_t i = 0; i != expected_types.size(); ++i) {
    if (request.ordered_payload[i].scalar_type != expected_types[i])
      return refuse(D::wrong_scalar_type);
  }
  const auto reason = std::find(reasons.begin(), reasons.end(),
                                request.ordered_payload[4].enum_value);
  if (request.ordered_payload[0].uuid_value.is_nil() ||
      request.ordered_payload[1].uuid_value.is_nil() ||
      request.ordered_payload[2].uuid_value != receipt ||
      request.ordered_payload[3].unsigned_value >
          std::numeric_limits<std::uint32_t>::max() ||
      reason == reasons.end())
    return refuse(D::invalid_scalar_value);

  const auto metric = ResolveTimeDiagnosticMetricRouteV3(
      {request.axis, request.diagnostic_uuid});
  if (metric.disposition != TimeDiagnosticRouteFactDispositionV3::admitted)
    return refuse(D::unknown_route);
  result.disposition = D::admitted;
  result.axis = request.axis;
  result.diagnostic_code = route->code;
  result.diagnostic_uuid = route->diagnostic_uuid;
  for (std::size_t i = 0; i != result.ordered_payload.size(); ++i) {
    result.ordered_payload[i].token = expected_tokens[i];
    result.ordered_payload[i].scalar_type = expected_types[i];
  }
  result.ordered_payload[0].uuid_value = request.ordered_payload[0].uuid_value;
  result.ordered_payload[1].uuid_value = request.ordered_payload[1].uuid_value;
  result.ordered_payload[2].uuid_value = request.ordered_payload[2].uuid_value;
  result.ordered_payload[3].unsigned_value =
      request.ordered_payload[3].unsigned_value;
  result.ordered_payload[4].enum_value = *reason;
  result.parameter_count = static_cast<std::uint8_t>(expected_tokens.size());
  result.redaction = route->redaction;
  result.metric_evidence_type_uuid = metric.route.metric_evidence_type_uuid;
  result.metric_name = metric.route.metric_name;
  result.diagnostic_fact_emitted = true;
  result.receiving_owner_evidence_handoff_ready = true;
  return result;
}

std::span<const std::string_view> TimeDiagnosticPrecedenceV3(
    TimeDiagnosticPrecedenceScopeV3 scope) noexcept {
  static constexpr std::array<std::string_view, 28> global{{
      "SBLR.OPERAND_INVALID", "SBLR.ENVELOPE.CHECKSUM_INVALID",
      "STORAGE.PAGE_CHECKSUM_FAILED", "SECURITY.ACCESS_DENIED",
      "CTI.TEMPORAL.DESCRIPTOR_INVALID", "DATATYPE.NULL_STATE.INVALID",
      "DATATYPE.NULL_NOT_ADMITTED", "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
      "DATATYPE.CAST_FORBIDDEN", "CTI.TEMPORAL.INVALID_LITERAL",
      "CTI.TEMPORAL.LEAP_SECOND_REFUSED", "CTI.TEMPORAL.PRECISION_LOSS",
      "CTI.TEMPORAL.RANGE_EXCEEDED", "CTB.TEXT.LENGTH_EXCEEDED",
      "CTI.TEMPORAL.OPERATION_REFUSED",
      "CTI.INTERVAL.CALENDAR_OPERATION_REFUSED",
      "CTI.TEMPORAL.ORDERING_REFUSED", "DOMAIN.CONSTRAINT_FAILED",
      "OPTIMIZER.STATISTICS_PRIVACY_DENIED",
      "OPTIMIZER.STATISTICS_EPOCH_MISMATCH",
      "OPTIMIZER.INDEX_COMPATIBILITY_MISSING",
      "CTI.TEMPORAL.INDEX_KEY_REFUSED",
      "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
      "CTI.TEMPORAL.PROTECTION_UNSUPPORTED", "CTI.TRANSPORT.UNSUPPORTED",
      "RESOURCE.BUDGET_EXCEEDED", "PROCESS.CANCELLED",
      "CTI.MERGE.MANUAL_REVIEW_REQUIRED"}};
  static constexpr std::array<std::string_view, 12> cast{{
      "SECURITY.ACCESS_DENIED", "CTI.TEMPORAL.DESCRIPTOR_INVALID",
      "DATATYPE.NULL_STATE.INVALID", "DATATYPE.NULL_NOT_ADMITTED",
      "DATATYPE.CAST_FORBIDDEN", "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
      "CTI.TEMPORAL.INVALID_LITERAL", "CTI.TEMPORAL.LEAP_SECOND_REFUSED",
      "CTI.TEMPORAL.PRECISION_LOSS", "CTB.TEXT.LENGTH_EXCEEDED",
      "RESOURCE.BUDGET_EXCEEDED", "PROCESS.CANCELLED"}};
  static constexpr std::array<std::string_view, 12> operation{{
      "SECURITY.ACCESS_DENIED", "CTI.TEMPORAL.DESCRIPTOR_INVALID",
      "DATATYPE.NULL_STATE.INVALID", "DATATYPE.NULL_NOT_ADMITTED",
      "CTI.TEMPORAL.OPERATION_REFUSED",
      "CTI.INTERVAL.CALENDAR_OPERATION_REFUSED",
      "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
      "CTI.TEMPORAL.INVALID_LITERAL", "CTI.TEMPORAL.LEAP_SECOND_REFUSED",
      "CTI.TEMPORAL.RANGE_EXCEEDED", "RESOURCE.BUDGET_EXCEEDED",
      "PROCESS.CANCELLED"}};
  static constexpr std::array<std::string_view, 7> text_fields{{
      "shape", "hour", "minute", "second_leap", "fraction_syntax",
      "precision", "trailing_zero"}};
  static constexpr std::array<std::string_view, 6> intrinsic_operation_gates{{
      "authority", "state", "classification", "admitted NULL propagation",
      "PRESENT payload", "resource cancellation publication"}};
  switch (scope) {
    case TimeDiagnosticPrecedenceScopeV3::global: return global;
    case TimeDiagnosticPrecedenceScopeV3::cast: return cast;
    case TimeDiagnosticPrecedenceScopeV3::operation: return operation;
    case TimeDiagnosticPrecedenceScopeV3::text_fields: return text_fields;
    case TimeDiagnosticPrecedenceScopeV3::intrinsic_operation_gates:
      return intrinsic_operation_gates;
  }
  return {};
}

std::span<const TimeDiagnosticFormatOrderV3>
TimeDiagnosticFormatOrdersV3() noexcept {
  static constexpr std::array<TimeDiagnosticFormatOrderV3, 5> rows{{
      {"SBTIMP01", "base-time-profile-registry.yaml"},
      {"SBTIMC01", "base-time-profile-registry.yaml"},
      {"SBTIMK01", "base-time-index-projection-layouts.yaml"},
      {"SBTIMS01", "base-time-statistics-layout.yaml"},
      {"SBTIMB01", "base-time-backup-transport-layout.yaml"}}};
  return rows;
}

std::span<const TimeDiagnosticCombinedFaultV3>
TimeDiagnosticTextCombinedFaultsV3() noexcept {
  static constexpr std::array<TimeDiagnosticCombinedFaultV3, 5> rows{{
      {"12:34:60.1234567890", "leap"},
      {"12:60:60.1234567890", "invalid"},
      {"12:34:59.1234567890", "precision"},
      {"12:34:60.", "leap"}, {"12:34:60.X", "leap"}}};
  return rows;
}

std::span<const TimeDiagnosticClassificationRuleV3>
TimeDiagnosticClassificationRulesV3() noexcept {
  static constexpr std::array<TimeDiagnosticClassificationRuleV3, 2> rows{{
      {"forbidden_cast", "classification before PRESENT payload"},
      {"operation", "classification before PRESENT payload"}}};
  return rows;
}

std::string_view TimeDiagnosticSelectionRuleV3() noexcept {
  return "named binary format exact sequential gates win; scoped order applies within one gate; global order is fallback";
}

std::span<const TimeDiagnosticMetricRouteV3>
TimeDiagnosticMetricRoutesV3() noexcept {
  static constexpr auto metric_uuid = Uuid({
      0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,
      0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x6d});
  static constexpr std::array<TimeDiagnosticMetricRouteV3, 15> rows{{
      {TimeDiagnosticAxisV3::input_parse, metric_uuid},
      {TimeDiagnosticAxisV3::descriptor_codec, metric_uuid},
      {TimeDiagnosticAxisV3::storage_read_write, metric_uuid},
      {TimeDiagnosticAxisV3::cast_invalid, metric_uuid},
      {TimeDiagnosticAxisV3::cast_range_loss, metric_uuid},
      {TimeDiagnosticAxisV3::operation_invalid, metric_uuid},
      {TimeDiagnosticAxisV3::bounds_overflow_underflow, metric_uuid},
      {TimeDiagnosticAxisV3::resource_cancellation, metric_uuid},
      {TimeDiagnosticAxisV3::compression_corruption, metric_uuid},
      {TimeDiagnosticAxisV3::encryption_auth_key, metric_uuid},
      {TimeDiagnosticAxisV3::wire_decode_encode, metric_uuid},
      {TimeDiagnosticAxisV3::index_key, metric_uuid},
      {TimeDiagnosticAxisV3::domain_validation, metric_uuid},
      {TimeDiagnosticAxisV3::recovery_corruption, metric_uuid},
      {TimeDiagnosticAxisV3::statistics_read, metric_uuid}}};
  return rows;
}

TimeDiagnosticMetricRouteResultV3 ResolveTimeDiagnosticMetricRouteV3(
    const TimeDiagnosticRouteKeyV3& key) noexcept {
  TimeDiagnosticMetricRouteResultV3 result;
  bool exact_key = false;
  for (const auto& row : TimeDiagnosticRoutesV3()) {
    if (row.axis == key.axis && row.diagnostic_uuid == key.diagnostic_uuid) {
      exact_key = true;
      break;
    }
  }
  if (!exact_key) return result;
  const auto axis = static_cast<std::size_t>(key.axis);
  const auto routes = TimeDiagnosticMetricRoutesV3();
  if (axis >= routes.size()) return result;
  result.disposition = TimeDiagnosticRouteFactDispositionV3::admitted;
  result.route = routes[axis];
  return result;
}

TimeDiagnosticRouteFactDispositionV3 ValidateTimeDiagnosticMetricRouteTableV3(
    std::span<const TimeDiagnosticMetricRouteV3> supplied,
    bool scalar_types_valid) noexcept {
  using D = TimeDiagnosticRouteFactDispositionV3;
  if (!scalar_types_valid) return D::wrong_type;
  const auto expected = TimeDiagnosticMetricRoutesV3();
  if (supplied.size() < expected.size()) return D::missing;
  std::array<bool, 15> seen{};
  for (std::size_t i = 0; i != supplied.size(); ++i) {
    const auto axis = static_cast<std::size_t>(supplied[i].diagnostic_family);
    if (axis >= expected.size()) return D::unknown;
    if (seen[axis]) return D::duplicate;
    seen[axis] = true;
    if (supplied[i].metric_evidence_type_uuid !=
            expected[axis].metric_evidence_type_uuid ||
        supplied[i].metric_name != expected[axis].metric_name ||
        supplied[i].rule != expected[axis].rule) return D::wrong_type;
    if (axis != i) return D::reordered;
  }
  if (supplied.size() != expected.size()) return D::unknown;
  return D::admitted;
}

std::span<const TimeMetricEvidenceTypeDescriptorV3> TimeMetricEvidenceTypesV3() noexcept {
  static constexpr std::array<TimeMetricEvidenceTypeDescriptorV3,21> rows{{
    {TimeMetricEvidenceTypeV3::descriptor_admissions,Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x5b}),1,"sb_time_descriptor_admissions_total","time_descriptor","time_descriptor_admissions_evidence","metrics_registry_manager",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x72}),"database_uuid,node_uuid,result","<=256 series/local_node exact 256_database_times_1_declared_tuple bound","after_admission","exact_d708_profile_admitted","after successful exact d708 profile admission only; tuple must belong to the exact declared tuple union","missing_or_stale_unknown_never_changes_datatype_result; undeclared tuple rejected without metric mutation","one_typed_evidence_emission_after_primary_outcome_commit"},
    {TimeMetricEvidenceTypeV3::invalid_literals,Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x5c}),1,"sb_time_invalid_literals_total","time_text_and_constructor","time_invalid_literals_evidence","metrics_registry_manager",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x73}),"database_uuid,node_uuid,reason","<=1792 series/local_node exact 256_database_times_7_declared_tuple bound","after_primary_diagnostic_commit","CTI.TEMPORAL.INVALID_LITERAL","after final CTI.TEMPORAL.INVALID_LITERAL only; leap second and precision use their dedicated metrics; tuple must belong to the exact declared tuple union","missing_or_stale_unknown_never_changes_datatype_result; undeclared tuple rejected without metric mutation","one_typed_evidence_emission_after_primary_outcome_commit"},
    {TimeMetricEvidenceTypeV3::range_refusals,Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x5d}),1,"sb_time_range_refusals_total","time_arithmetic","time_range_refusals_evidence","metrics_registry_manager",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x74}),"database_uuid,node_uuid,operation","<=1024 series/local_node exact 256_database_times_4_declared_tuple bound","after_primary_diagnostic_commit","CTI.TEMPORAL.RANGE_EXCEEDED","after final CTI.TEMPORAL.RANGE_EXCEEDED from admitted checked non-wrapping arithmetic only; tuple must belong to the exact declared tuple union","missing_or_stale_unknown_never_changes_datatype_result; undeclared tuple rejected without metric mutation","one_typed_evidence_emission_after_primary_outcome_commit"},
    {TimeMetricEvidenceTypeV3::operation_attempts,Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x5e}),1,"sb_time_operation_attempts_total","time_runtime","time_operation_attempts_evidence","metrics_registry_manager",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x75}),"database_uuid,node_uuid,operation","<=6912 series/local_node exact 256_database_times_27_declared_tuple bound","after_input_admission_before_work","classified_attempt_operation_id","once after exact input admission and operation classification, before operation work; tuple must belong to the exact declared tuple union","missing_or_stale_unknown_never_changes_datatype_result; undeclared tuple rejected without metric mutation","one_typed_evidence_emission_after_primary_outcome_commit"},
    {TimeMetricEvidenceTypeV3::operation_success,Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x5f}),1,"sb_time_operation_success_total","time_runtime","time_operation_success_evidence","metrics_registry_manager",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x76}),"database_uuid,node_uuid,operation","<=6144 series/local_node exact 256_database_times_24_declared_tuple bound","after_atomic_publication","admitted_success_operation_id","after atomic result or boundary publication for an admitted success-capable operation; tuple must belong to the exact declared tuple union","missing_or_stale_unknown_never_changes_datatype_result; undeclared tuple rejected without metric mutation","one_typed_evidence_emission_after_primary_outcome_commit"},
    {TimeMetricEvidenceTypeV3::operation_refusals,Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x60}),1,"sb_time_operation_refusals_total","time_runtime","time_operation_refusals_evidence","metrics_registry_manager",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x77}),"database_uuid,node_uuid,operation,reason","<=8192 series/local_node exact 256_database_times_32_declared_tuple bound","after_primary_diagnostic_commit","declared_operation_reason_tuple","after exactly one final range, resource budget, cancellation, or registered unsupported-operation refusal tuple; tuple must belong to the exact declared tuple union","missing_or_stale_unknown_never_changes_datatype_result; undeclared tuple rejected without metric mutation","one_typed_evidence_emission_after_primary_outcome_commit"},
    {TimeMetricEvidenceTypeV3::cast_attempts,Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x61}),1,"sb_time_cast_attempts_total","time_cast","time_cast_attempts_evidence","metrics_registry_manager",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x78}),"database_uuid,node_uuid,cast_context","<=768 series/local_node exact 256_database_times_3_declared_tuple bound","after_input_admission_before_work","admitted_cast_pair_and_context","after exact pair, context, and source admission; tuple must belong to the exact declared tuple union","missing_or_stale_unknown_never_changes_datatype_result; undeclared tuple rejected without metric mutation","one_typed_evidence_emission_after_primary_outcome_commit"},
    {TimeMetricEvidenceTypeV3::cast_success,Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x62}),1,"sb_time_cast_success_total","time_cast","time_cast_success_evidence","metrics_registry_manager",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x79}),"database_uuid,node_uuid,cast_context","<=768 series/local_node exact 256_database_times_3_declared_tuple bound","after_atomic_publication","admitted_cast_target","after atomic target publication for an admitted cast; tuple must belong to the exact declared tuple union","missing_or_stale_unknown_never_changes_datatype_result; undeclared tuple rejected without metric mutation","one_typed_evidence_emission_after_primary_outcome_commit"},
    {TimeMetricEvidenceTypeV3::index_admission_refusals,Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x63}),1,"sb_time_index_admission_refusals_total","time_projection","time_index_admission_refusals_evidence","metrics_registry_manager",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x7a}),"database_uuid,node_uuid,index_family,reason","<=4096 series/local_node exact 256_database_times_16_declared_tuple bound","after_primary_diagnostic_commit","CTI.TEMPORAL.INDEX_KEY_REFUSED","after final CTI.TEMPORAL.INDEX_KEY_REFUSED for one of eight directly selected families and exact provider-budget or unsupported-mode cause; expression and partial-filtered providers delegate and do not emit this metric; tuple must belong to the exact declared tuple union","missing_or_stale_unknown_never_changes_datatype_result; undeclared tuple rejected without metric mutation","one_typed_evidence_emission_after_primary_outcome_commit"},
    {TimeMetricEvidenceTypeV3::statistics_stale,Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x64}),1,"sb_time_statistics_stale_total","time_statistics","time_statistics_stale_evidence","metrics_registry_manager",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x7b}),"database_uuid,node_uuid,reason","<=1024 series/local_node exact 256_database_times_4_declared_tuple bound","after_staleness_commit","exact_profile_or_epoch_mismatch","after exact profile, schema epoch, collection epoch, or security epoch mismatch; privacy denial is diagnostic-only; tuple must belong to the exact declared tuple union","missing_or_stale_unknown_never_changes_datatype_result; undeclared tuple rejected without metric mutation","one_typed_evidence_emission_after_primary_outcome_commit"},
    {TimeMetricEvidenceTypeV3::reference_mapping_misses,Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x65}),1,"sb_time_reference_mapping_misses_total","time_boundary","time_reference_mapping_misses_evidence","metrics_registry_manager",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x7c}),"database_uuid,node_uuid,lane_group,reason","<=256 series/local_node exact 256_database_times_1_declared_tuple bound","after_lookup_commit","manifest_listed_reference_profile_absent","after compatibility lookup proves no manifest-listed reference profile; tuple must belong to the exact declared tuple union","missing_or_stale_unknown_never_changes_datatype_result; undeclared tuple rejected without metric mutation","one_typed_evidence_emission_after_primary_outcome_commit"},
    {TimeMetricEvidenceTypeV3::transport_refusals,Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x66}),1,"sb_time_transport_refusals_total","transport_owner","time_transport_refusals_evidence","metrics_registry_manager",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x7d}),"database_uuid,node_uuid,lane_group,reason","<=512 series/local_node exact 256_database_times_2_declared_tuple bound","after_primary_diagnostic_commit","CTI.TRANSPORT.UNSUPPORTED","after committed unsupported native outer or absent reference profile refusal; tuple must belong to the exact declared tuple union","missing_or_stale_unknown_never_changes_datatype_result; undeclared tuple rejected without metric mutation","one_typed_evidence_emission_after_primary_outcome_commit"},
    {TimeMetricEvidenceTypeV3::serialization_refusals,Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x67}),1,"sb_time_serialization_refusals_total","time_boundary","time_serialization_refusals_evidence","metrics_registry_manager",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x7e}),"database_uuid,node_uuid,boundary,reason","<=12032 series/local_node exact 256_database_times_47_declared_tuple bound","after_primary_diagnostic_commit","reachable_product_boundary_serialization_refusal","after final operational serialization or projection format refusal at component, SBDVAL01, SBDPV001, ordered-key, hash, statistics, or backup; oracle is conformance-only and wire refuses in transport before datatype serialization; tuple must belong to the exact declared tuple union","missing_or_stale_unknown_never_changes_datatype_result; undeclared tuple rejected without metric mutation","one_typed_evidence_emission_after_primary_outcome_commit"},
    {TimeMetricEvidenceTypeV3::protection_refusals,Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x68}),1,"sb_time_protection_refusals_total","time_protection","time_protection_refusals_evidence","metrics_registry_manager",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x7f}),"database_uuid,node_uuid,layer,reason","<=768 series/local_node exact 256_database_times_3_declared_tuple bound","after_primary_diagnostic_commit","CTI.TEMPORAL.PROTECTION_UNSUPPORTED","after final CTI.TEMPORAL.PROTECTION_UNSUPPORTED for a datatype-owned layer; tuple must belong to the exact declared tuple union","missing_or_stale_unknown_never_changes_datatype_result; undeclared tuple rejected without metric mutation","one_typed_evidence_emission_after_primary_outcome_commit"},
    {TimeMetricEvidenceTypeV3::merge_manual_review,Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x69}),1,"sb_time_merge_manual_review_total","merge_owner","time_merge_manual_review_evidence","metrics_registry_manager",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x80}),"database_uuid,node_uuid,reason","<=256 series/local_node exact 256_database_times_1_declared_tuple bound","after_primary_diagnostic_commit","CTI.MERGE.MANUAL_REVIEW_REQUIRED","after committed CTI.MERGE.MANUAL_REVIEW_REQUIRED caused by receipt, profile, or storage disagreement; tuple must belong to the exact declared tuple union","missing_or_stale_unknown_never_changes_datatype_result; undeclared tuple rejected without metric mutation","one_typed_evidence_emission_after_primary_outcome_commit"},
    {TimeMetricEvidenceTypeV3::resource_budget_refusals,Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x6a}),1,"sb_time_resource_budget_refusals_total","resource_owner","time_resource_budget_refusals_evidence","metrics_registry_manager",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x81}),"database_uuid,node_uuid,operation,reason","<=256 series/local_node exact 256_database_times_1_declared_tuple bound","after_primary_diagnostic_commit","RESOURCE.BUDGET_EXCEEDED","after final RESOURCE.BUDGET_EXCEEDED for batch_materialize only; tuple must belong to the exact declared tuple union","missing_or_stale_unknown_never_changes_datatype_result; undeclared tuple rejected without metric mutation","one_typed_evidence_emission_after_primary_outcome_commit"},
    {TimeMetricEvidenceTypeV3::cancellations,Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x6b}),1,"sb_time_cancellations_total","process_owner","time_cancellations_evidence","metrics_registry_manager",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x82}),"database_uuid,node_uuid,operation","<=6144 series/local_node exact 256_database_times_24_declared_tuple bound","after_primary_diagnostic_commit","PROCESS.CANCELLED","after final PROCESS.CANCELLED for an admitted success-capable operation; tuple must belong to the exact declared tuple union","missing_or_stale_unknown_never_changes_datatype_result; undeclared tuple rejected without metric mutation","one_typed_evidence_emission_after_primary_outcome_commit"},
    {TimeMetricEvidenceTypeV3::decode_corruption,Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x6c}),1,"sb_time_decode_corruption_total","time_boundary","time_decode_corruption_evidence","metrics_registry_manager",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x83}),"database_uuid,node_uuid,boundary,reason","<=6400 series/local_node exact 256_database_times_25_declared_tuple bound","after_primary_diagnostic_commit","reachable_product_boundary_decode_corruption","after final operational decode corruption at component, SBDVAL01, SBDPV001, ordered-key, statistics, or backup; hash has no decode path, oracle is conformance-only, and wire refuses in transport; tuple must belong to the exact declared tuple union","missing_or_stale_unknown_never_changes_datatype_result; undeclared tuple rejected without metric mutation","one_typed_evidence_emission_after_primary_outcome_commit"},
    {TimeMetricEvidenceTypeV3::diagnostic_family,Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x6d}),1,"sb_time_diagnostic_family_total","metrics_diagnostic_router","time_diagnostic_family_evidence","metrics_registry_manager",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x84}),"database_uuid,node_uuid,diagnostic_family","<=3840 series/local_node exact 256_database_times_15_declared_tuple bound","after_primary_diagnostic_commit","exact_selected_diagnostic_family","after final primary diagnostic selection and commit; exactly one family increment; tuple must belong to the exact declared tuple union","absent or ambiguous family or undeclared tuple refuses the sample; undeclared tuple rejected without metric mutation; emit metrics evidence without aggregate mutation; datatype result unchanged","one_typed_evidence_emission_after_primary_outcome_commit"},
    {TimeMetricEvidenceTypeV3::precision_loss_refusals,Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x6e}),1,"sb_time_precision_loss_refusals_total","time_text_and_constructor","time_precision_loss_refusals_evidence","metrics_registry_manager",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x85}),"database_uuid,node_uuid,operation,reason","<=512 series/local_node exact 256_database_times_2_declared_tuple bound","after_primary_diagnostic_commit","CTI.TEMPORAL.PRECISION_LOSS","after final CTI.TEMPORAL.PRECISION_LOSS from canonical parse or character-to-time cast only; tuple must belong to the exact declared tuple union","missing_or_stale_unknown_never_changes_datatype_result; undeclared tuple rejected without metric mutation","one_typed_evidence_emission_after_primary_outcome_commit"},
    {TimeMetricEvidenceTypeV3::leap_second_refusals,Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x6f}),1,"sb_time_leap_second_refusals_total","time_text_and_constructor","time_leap_second_refusals_evidence","metrics_registry_manager",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x86}),"database_uuid,node_uuid,operation,reason","<=768 series/local_node exact 256_database_times_3_declared_tuple bound","after_primary_diagnostic_commit","CTI.TEMPORAL.LEAP_SECOND_REFUSED","after final CTI.TEMPORAL.LEAP_SECOND_REFUSED from civil construction, canonical parse, or character-to-time cast only; tuple must belong to the exact declared tuple union","missing_or_stale_unknown_never_changes_datatype_result; undeclared tuple rejected without metric mutation","one_typed_evidence_emission_after_primary_outcome_commit"},
  }}; return rows;
}


std::span<const TimeMetricClosedLabelTupleV3>
TimeMetricClosedLabelTuplesV3() noexcept {
  static constexpr std::array<TimeMetricClosedLabelTupleV3, 245> rows{{
      {static_cast<TimeMetricEvidenceTypeV3>(0), {{{"result", "admitted"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(1), {{{"reason", "grammar"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(1), {{{"reason", "hour"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(1), {{{"reason", "minute"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(1), {{{"reason", "second"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(1), {{{"reason", "nanosecond"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(1), {{{"reason", "fraction_syntax"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(1), {{{"reason", "trailing_zero"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(2), {{{"operation", "add_nanoseconds"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(2), {{{"operation", "subtract_nanoseconds"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(2), {{{"operation", "successor"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(2), {{{"operation", "predecessor"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "validate_canonicalize"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "civil_construct"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "decompose_civil"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "parse_canonical"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "render_canonical"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "add_nanoseconds"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "subtract_nanoseconds"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "successor"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "predecessor"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "difference_nanoseconds"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "extract_hour"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "extract_minute"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "extract_second"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "extract_nanosecond"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "nanosecond_of_day"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "truncate_nanosecond"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "round_nanosecond"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "larger_truncate_round_or_bucket"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "duration_interval_or_modulo_day"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "date_timestamp_or_timezone_cross_temporal"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "compare"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "hash"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "sort_key"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "cast"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "serialize"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "deserialize"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(3), {{{"operation", "batch_materialize"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(4), {{{"operation", "validate_canonicalize"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(4), {{{"operation", "civil_construct"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(4), {{{"operation", "decompose_civil"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(4), {{{"operation", "parse_canonical"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(4), {{{"operation", "render_canonical"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(4), {{{"operation", "add_nanoseconds"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(4), {{{"operation", "subtract_nanoseconds"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(4), {{{"operation", "successor"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(4), {{{"operation", "predecessor"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(4), {{{"operation", "difference_nanoseconds"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(4), {{{"operation", "extract_hour"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(4), {{{"operation", "extract_minute"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(4), {{{"operation", "extract_second"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(4), {{{"operation", "extract_nanosecond"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(4), {{{"operation", "nanosecond_of_day"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(4), {{{"operation", "truncate_nanosecond"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(4), {{{"operation", "round_nanosecond"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(4), {{{"operation", "compare"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(4), {{{"operation", "hash"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(4), {{{"operation", "sort_key"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(4), {{{"operation", "cast"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(4), {{{"operation", "serialize"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(4), {{{"operation", "deserialize"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(4), {{{"operation", "batch_materialize"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "add_nanoseconds"}, {"reason", "range"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "subtract_nanoseconds"}, {"reason", "range"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "successor"}, {"reason", "range"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "predecessor"}, {"reason", "range"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "batch_materialize"}, {"reason", "allocation_budget"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "validate_canonicalize"}, {"reason", "cancelled"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "civil_construct"}, {"reason", "cancelled"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "decompose_civil"}, {"reason", "cancelled"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "parse_canonical"}, {"reason", "cancelled"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "render_canonical"}, {"reason", "cancelled"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "add_nanoseconds"}, {"reason", "cancelled"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "subtract_nanoseconds"}, {"reason", "cancelled"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "successor"}, {"reason", "cancelled"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "predecessor"}, {"reason", "cancelled"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "difference_nanoseconds"}, {"reason", "cancelled"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "extract_hour"}, {"reason", "cancelled"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "extract_minute"}, {"reason", "cancelled"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "extract_second"}, {"reason", "cancelled"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "extract_nanosecond"}, {"reason", "cancelled"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "nanosecond_of_day"}, {"reason", "cancelled"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "truncate_nanosecond"}, {"reason", "cancelled"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "round_nanosecond"}, {"reason", "cancelled"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "compare"}, {"reason", "cancelled"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "hash"}, {"reason", "cancelled"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "sort_key"}, {"reason", "cancelled"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "cast"}, {"reason", "cancelled"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "serialize"}, {"reason", "cancelled"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "deserialize"}, {"reason", "cancelled"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "batch_materialize"}, {"reason", "cancelled"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "larger_truncate_round_or_bucket"}, {"reason", "unsupported"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "duration_interval_or_modulo_day"}, {"reason", "unsupported"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(5), {{{"operation", "date_timestamp_or_timezone_cross_temporal"}, {"reason", "unsupported"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(6), {{{"cast_context", "implicit"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(6), {{{"cast_context", "assignment"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(6), {{{"cast_context", "explicit"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(7), {{{"cast_context", "implicit"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(7), {{{"cast_context", "assignment"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(7), {{{"cast_context", "explicit"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(8), {{{"index_family", "bitmap"}, {"reason", "provider_budget"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(8), {{{"index_family", "bitmap"}, {"reason", "unsupported"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(8), {{{"index_family", "brin_like"}, {"reason", "provider_budget"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(8), {{{"index_family", "brin_like"}, {"reason", "unsupported"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(8), {{{"index_family", "btree"}, {"reason", "provider_budget"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(8), {{{"index_family", "btree"}, {"reason", "unsupported"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(8), {{{"index_family", "columnar_zone_map"}, {"reason", "provider_budget"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(8), {{{"index_family", "columnar_zone_map"}, {"reason", "unsupported"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(8), {{{"index_family", "covering_included"}, {"reason", "provider_budget"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(8), {{{"index_family", "covering_included"}, {"reason", "unsupported"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(8), {{{"index_family", "hash"}, {"reason", "provider_budget"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(8), {{{"index_family", "hash"}, {"reason", "unsupported"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(8), {{{"index_family", "range_exclusion"}, {"reason", "provider_budget"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(8), {{{"index_family", "range_exclusion"}, {"reason", "unsupported"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(8), {{{"index_family", "temporary_work"}, {"reason", "provider_budget"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(8), {{{"index_family", "temporary_work"}, {"reason", "unsupported"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(9), {{{"reason", "stale"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(9), {{{"reason", "schema_epoch"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(9), {{{"reason", "collection_epoch"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(9), {{{"reason", "security_epoch"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(10), {{{"lane_group", "reference_profile_absent"}, {"reason", "absent"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(11), {{{"lane_group", "native_outer_unallocated"}, {"reason", "unsupported"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(11), {{{"lane_group", "reference_profile_absent"}, {"reason", "absent"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "component"}, {"reason", "extent"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "component"}, {"reason", "stale"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "component"}, {"reason", "mismatch"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "component"}, {"reason", "policy"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "SBDVAL01"}, {"reason", "short"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "SBDVAL01"}, {"reason", "trailing"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "SBDVAL01"}, {"reason", "checksum"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "SBDVAL01"}, {"reason", "reserved"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "SBDVAL01"}, {"reason", "extent"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "SBDVAL01"}, {"reason", "stale"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "SBDVAL01"}, {"reason", "mismatch"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "SBDVAL01"}, {"reason", "policy"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "SBDPV001"}, {"reason", "short"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "SBDPV001"}, {"reason", "trailing"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "SBDPV001"}, {"reason", "checksum"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "SBDPV001"}, {"reason", "reserved"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "SBDPV001"}, {"reason", "extent"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "SBDPV001"}, {"reason", "stale"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "SBDPV001"}, {"reason", "mismatch"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "SBDPV001"}, {"reason", "policy"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "backup"}, {"reason", "short"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "backup"}, {"reason", "trailing"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "backup"}, {"reason", "checksum"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "backup"}, {"reason", "reserved"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "backup"}, {"reason", "extent"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "backup"}, {"reason", "stale"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "backup"}, {"reason", "mismatch"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "backup"}, {"reason", "policy"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "statistics"}, {"reason", "short"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "statistics"}, {"reason", "trailing"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "statistics"}, {"reason", "checksum"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "statistics"}, {"reason", "reserved"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "statistics"}, {"reason", "extent"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "statistics"}, {"reason", "stale"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "statistics"}, {"reason", "mismatch"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "statistics"}, {"reason", "policy"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "ordered_key"}, {"reason", "short"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "ordered_key"}, {"reason", "trailing"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "ordered_key"}, {"reason", "reserved"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "ordered_key"}, {"reason", "extent"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "ordered_key"}, {"reason", "stale"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "ordered_key"}, {"reason", "mismatch"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "ordered_key"}, {"reason", "policy"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "hash"}, {"reason", "extent"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "hash"}, {"reason", "stale"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "hash"}, {"reason", "mismatch"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(12), {{{"boundary", "hash"}, {"reason", "policy"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(13), {{{"layer", "inner_compression"}, {"reason", "unsupported"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(13), {{{"layer", "inner_encryption"}, {"reason", "unsupported"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(13), {{{"layer", "protected_index"}, {"reason", "unsupported"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(14), {{{"reason", "manual_review"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(15), {{{"operation", "batch_materialize"}, {"reason", "allocation_budget"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(16), {{{"operation", "validate_canonicalize"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(16), {{{"operation", "civil_construct"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(16), {{{"operation", "decompose_civil"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(16), {{{"operation", "parse_canonical"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(16), {{{"operation", "render_canonical"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(16), {{{"operation", "add_nanoseconds"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(16), {{{"operation", "subtract_nanoseconds"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(16), {{{"operation", "successor"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(16), {{{"operation", "predecessor"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(16), {{{"operation", "difference_nanoseconds"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(16), {{{"operation", "extract_hour"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(16), {{{"operation", "extract_minute"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(16), {{{"operation", "extract_second"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(16), {{{"operation", "extract_nanosecond"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(16), {{{"operation", "nanosecond_of_day"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(16), {{{"operation", "truncate_nanosecond"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(16), {{{"operation", "round_nanosecond"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(16), {{{"operation", "compare"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(16), {{{"operation", "hash"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(16), {{{"operation", "sort_key"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(16), {{{"operation", "cast"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(16), {{{"operation", "serialize"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(16), {{{"operation", "deserialize"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(16), {{{"operation", "batch_materialize"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(17), {{{"boundary", "component"}, {"reason", "extent"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(17), {{{"boundary", "SBDVAL01"}, {"reason", "short"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(17), {{{"boundary", "SBDVAL01"}, {"reason", "trailing"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(17), {{{"boundary", "SBDVAL01"}, {"reason", "checksum"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(17), {{{"boundary", "SBDVAL01"}, {"reason", "reserved"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(17), {{{"boundary", "SBDVAL01"}, {"reason", "extent"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(17), {{{"boundary", "SBDPV001"}, {"reason", "short"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(17), {{{"boundary", "SBDPV001"}, {"reason", "trailing"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(17), {{{"boundary", "SBDPV001"}, {"reason", "checksum"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(17), {{{"boundary", "SBDPV001"}, {"reason", "reserved"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(17), {{{"boundary", "SBDPV001"}, {"reason", "extent"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(17), {{{"boundary", "backup"}, {"reason", "short"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(17), {{{"boundary", "backup"}, {"reason", "trailing"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(17), {{{"boundary", "backup"}, {"reason", "checksum"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(17), {{{"boundary", "backup"}, {"reason", "reserved"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(17), {{{"boundary", "backup"}, {"reason", "extent"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(17), {{{"boundary", "statistics"}, {"reason", "short"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(17), {{{"boundary", "statistics"}, {"reason", "trailing"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(17), {{{"boundary", "statistics"}, {"reason", "checksum"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(17), {{{"boundary", "statistics"}, {"reason", "reserved"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(17), {{{"boundary", "statistics"}, {"reason", "extent"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(17), {{{"boundary", "ordered_key"}, {"reason", "short"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(17), {{{"boundary", "ordered_key"}, {"reason", "trailing"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(17), {{{"boundary", "ordered_key"}, {"reason", "reserved"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(17), {{{"boundary", "ordered_key"}, {"reason", "extent"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(18), {{{"diagnostic_family", "input_parse"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(18), {{{"diagnostic_family", "descriptor_codec"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(18), {{{"diagnostic_family", "storage_read_write"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(18), {{{"diagnostic_family", "cast_invalid"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(18), {{{"diagnostic_family", "cast_range_loss"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(18), {{{"diagnostic_family", "operation_invalid"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(18), {{{"diagnostic_family", "bounds_overflow_underflow"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(18), {{{"diagnostic_family", "resource_cancellation"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(18), {{{"diagnostic_family", "compression_corruption"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(18), {{{"diagnostic_family", "encryption_auth_key"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(18), {{{"diagnostic_family", "wire_decode_encode"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(18), {{{"diagnostic_family", "index_key"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(18), {{{"diagnostic_family", "domain_validation"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(18), {{{"diagnostic_family", "recovery_corruption"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(18), {{{"diagnostic_family", "statistics_read"}, {"", ""}}}, 1},
      {static_cast<TimeMetricEvidenceTypeV3>(19), {{{"operation", "parse_canonical"}, {"reason", "precision"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(19), {{{"operation", "cast"}, {"reason", "precision"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(20), {{{"operation", "civil_construct"}, {"reason", "leap_second"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(20), {{{"operation", "parse_canonical"}, {"reason", "leap_second"}}}, 2},
      {static_cast<TimeMetricEvidenceTypeV3>(20), {{{"operation", "cast"}, {"reason", "leap_second"}}}, 2},
  }};
  return rows;
}

TimeMetricEvidenceEnvelopeIdentityV3
TimeMetricEvidenceEnvelopeIdentityV3Value() noexcept {
  return {Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,
                0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x5a}), 1};
}

TimeMetricEvidenceValidationResultV3 ValidateTimeMetricEvidenceV3(
    const TimeMetricEvidenceEnvelopeV3& evidence,
    const TimeMetricEvidenceValidationContextV3& context) noexcept {
  using D = TimeMetricEvidenceDispositionV3;
  TimeMetricEvidenceValidationResultV3 result;
  auto refuse = [&](D disposition) {
    result.disposition = disposition;
    return result;
  };
  const auto exact_envelope = TimeMetricEvidenceEnvelopeIdentityV3Value();
  if (!evidence.all_required_fields_present ||
      evidence.envelope_identity.schema_uuid.is_nil() ||
      !evidence.envelope_identity.generation || evidence.evidence_type_uuid.is_nil() ||
      evidence.source_event_uuid.is_nil() || evidence.database_uuid.is_nil() ||
      evidence.node_uuid.is_nil() || !evidence.process_epoch ||
      !evidence.event_sequence || !evidence.monotonic_timestamp_ns ||
      evidence.closed_label_tuple.empty()) return refuse(D::missing_field);
  if (!evidence.scalar_types_valid) return refuse(D::wrong_scalar);
  if (evidence.envelope_identity.schema_uuid != exact_envelope.schema_uuid ||
      evidence.envelope_identity.generation != exact_envelope.generation)
    return refuse(D::unknown_evidence_type);
  const auto descriptors = TimeMetricEvidenceTypesV3();
  std::size_t type_index = descriptors.size();
  for (std::size_t i = 0; i != descriptors.size(); ++i) {
    if (descriptors[i].evidence_type_uuid == evidence.evidence_type_uuid) {
      type_index = i;
      break;
    }
  }
  if (type_index == descriptors.size()) return refuse(D::unknown_evidence_type);
  result.type = static_cast<TimeMetricEvidenceTypeV3>(type_index);
  bool tuple_admitted = false;
  for (const auto& tuple : TimeMetricClosedLabelTuplesV3()) {
    if (tuple.type != result.type ||
        tuple.member_count != evidence.closed_label_tuple.size()) continue;
    bool exact = true;
    for (std::size_t i = 0; i != tuple.member_count; ++i) {
      exact = exact && tuple.members[i].name == evidence.closed_label_tuple[i].name &&
              tuple.members[i].value == evidence.closed_label_tuple[i].value;
    }
    if (exact) {
      tuple_admitted = true;
      break;
    }
  }
  if (!tuple_admitted) return refuse(D::undeclared_label_tuple);
  if (!evidence.outcome_committed) return refuse(D::outcome_not_committed);
  if (evidence.counter_delta != 1) return refuse(D::invalid_counter_delta);
  if (!context.current_process_epoch ||
      evidence.process_epoch != context.current_process_epoch)
    return refuse(D::stale_process_epoch);
  if (context.current_counter_value == std::numeric_limits<u64>::max())
    return refuse(D::counter_overflow);
  result.disposition = D::admitted;
  result.idempotency = {evidence.process_epoch, evidence.event_sequence,
                        evidence.source_event_uuid,
                        evidence.evidence_type_uuid};
  result.receiving_owner_update_admitted = true;
  return result;
}

TimeMetricSameSourceFactV3 ClassifyTimeMetricSameSourceEmissionV3(
    const TimeMetricEvidenceIdempotencyFactV3& first,
    const TimeMetricEvidenceIdempotencyFactV3& second) noexcept {
  using D = TimeMetricSameSourceDispositionV3;
  TimeMetricSameSourceFactV3 result;
  if (!first.process_epoch || first.process_epoch != second.process_epoch) {
    result.disposition = D::stale_process_epoch_refused;
    return result;
  }
  if (first.source_event_uuid != second.source_event_uuid) {
    result.disposition = D::independent_sources;
    result.second_emission_admitted = true;
    result.receiving_owner_update_admitted = true;
    return result;
  }
  if (first.evidence_type_uuid == second.evidence_type_uuid) {
    result.disposition = D::duplicate_type_refused;
    return result;
  }
  const auto descriptors = TimeMetricEvidenceTypesV3();
  auto type_index = [&](const platform::Uuid& uuid) {
    for (std::size_t i = 0; i != descriptors.size(); ++i)
      if (descriptors[i].evidence_type_uuid == uuid) return i;
    return descriptors.size();
  };
  const auto first_type = type_index(first.evidence_type_uuid);
  const auto second_type = type_index(second.evidence_type_uuid);
  const bool registered_forward =
      (first_type == 10 && second_type == 11) ||
      (first_type == 20 && second_type == 18);
  const bool registered_reverse =
      (first_type == 11 && second_type == 10) ||
      (first_type == 18 && second_type == 20);
  if (registered_reverse ||
      (registered_forward && first.event_sequence >= second.event_sequence)) {
    result.disposition = D::reversed_order_refused;
    return result;
  }
  if (!registered_forward) {
    result.disposition = D::unregistered_pair_refused;
    return result;
  }
  result.disposition = D::admitted_ordered_pair;
  result.second_emission_admitted = true;
  result.receiving_owner_update_admitted = true;
  return result;
}

} // namespace scratchbird::core::datatypes
