// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "datatype_date_projection.hpp"

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
constexpr byte kZoneMagic[8] = {'S','B','D','A','T','Z','0','1'};
constexpr byte kStatisticsMagic[8] = {'S','B','D','A','T','S','0','1'};
constexpr byte kBackupMagic[8] = {'S','B','D','A','T','B','0','1'};
constexpr char kZoneDomain[] = "ScratchBird.BaseDate.ZoneMap.V1";
constexpr char kStatisticsDomain[] = "ScratchBird.BaseDate.StatisticsRecords.V1";
constexpr char kBackupDomain[] = "ScratchBird.BaseDate.BackupTuple.V1";

constexpr platform::Uuid Uuid(std::array<byte,16> b) noexcept { return {b}; }
constexpr platform::Uuid kPrivacyUuid = Uuid({
  0x01,0xa1,0x00,0x8e,0xb8,0x05,0x7e,0x31,
  0xa9,0xc4,0x5d,0x76,0x2f,0x83,0x01,0xab});

Status Ok() noexcept { return {StatusCode::ok, Severity::info, Subsystem::datatypes}; }
Status Error() noexcept { return {StatusCode::platform_required_feature_missing,
                                  Severity::error, Subsystem::datatypes}; }
Status BudgetError() noexcept { return {StatusCode::memory_allocation_failed,
                                        Severity::error, Subsystem::datatypes}; }
bool Cancelled(const DateExecutionControlV1& c) noexcept {
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

DateBytesResultV1 FailBytes(std::string_view code,
                            std::string_view detail = {}) noexcept {
  DateBytesResultV1 r;
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
DateIndexProjectionResultV1 FailIndex(DateIndexResolutionV1 resolution,
                                      std::string_view code,
                                      std::string_view detail = {}) noexcept {
  DateIndexProjectionResultV1 r; r.resolution=resolution;
  r.status=code=="RESOURCE.BUDGET_EXCEEDED"?BudgetError():Error();
  r.diagnostic = {r.status, code, detail};
  return r;
}
DateIndexProjectionResultV1 OwnerFact(DateIndexResolutionV1 r) noexcept {
  DateIndexProjectionResultV1 out; out.status=Error(); out.resolution=r;
  out.owner_fact_only=true; return out;
}

bool SameResolution(const DateIndexResolutionV1& a,
                    const DateIndexResolutionV1& b) noexcept {
  return a.family==b.family && a.compatibility_uuid==b.compatibility_uuid &&
      a.compatibility_generation==b.compatibility_generation &&
      a.disposition==b.disposition && a.projection==b.projection &&
      a.exact_recheck_required==b.exact_recheck_required &&
      a.receiving_owner_resolution_required==b.receiving_owner_resolution_required;
}
bool ValidFamily(DateIndexFamilyV1 family) noexcept {
  return static_cast<unsigned>(family) < 16;
}
bool ModeAllowed(DateIndexFamilyV1 family, DateProjectionModeV1 mode) noexcept {
  using F = DateIndexFamilyV1;
  using M = DateProjectionModeV1;
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
  if (!h.ok()) return false; *out=h.digest; return true;
}
bool HashEquals(std::string_view domain,std::span<const byte> record,
                std::size_t offset) noexcept {
  std::array<byte,32> h{}; ArrayClearGuard<32> clear(&h);
  return HashRecord(domain,record,offset,&h) &&
      std::memcmp(h.data(),record.data()+offset,32)==0;
}

void PutCommonIdentity(byte* p,const DateValidatedProfileHandleV1& h) noexcept {
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
                          const DateValidatedProfileHandleV1& h) noexcept {
  const auto& i=h.identity.legacy_fields;
  return LoadUuid(p)==h.receipt.catalog_snapshot_uuid &&
      LoadLittle64(p+16)==h.receipt.catalog_generation &&
      LoadLittle64(p+24)==h.receipt.registry_generation &&
      LoadUuid(p+32)==i.descriptor_uuid && LoadLittle64(p+48)==i.descriptor_generation &&
      LoadUuid(p+56)==i.type_uuid && LoadLittle64(p+72)==i.type_generation &&
      LoadUuid(p+80)==i.codec_uuid && LoadLittle32(p+96)==i.codec_version &&
      LoadLittle32(p+100)==0 && LoadLittle64(p+104)==i.codec_generation;
}
bool ValidProfile(const DateValidatedProfileHandleV1& h) noexcept {
  return ValidateDateProfileHandleV1(h).ok();
}
bool SameProfile(const DateValidatedProfileHandleV1& a,
                 const DateValidatedProfileHandleV1& b) noexcept {
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
  explicit DecodedStatisticsClearGuard(DateDecodedStatisticsV1* value) noexcept
      : value_(value) {}
  ~DecodedStatisticsClearGuard(){
    if(!active_||!value_)return;
    if(!value_->histogram.empty()){
      volatile byte* p=reinterpret_cast<volatile byte*>(value_->histogram.data());
      for(std::size_t i=0;i<value_->histogram.size()*sizeof(DateStatisticsHistogramRecordV1);++i)p[i]=0;
      value_->histogram.clear();
    }
    if(!value_->mcv.empty()){
      volatile byte* p=reinterpret_cast<volatile byte*>(value_->mcv.data());
      for(std::size_t i=0;i<value_->mcv.size()*sizeof(DateStatisticsMcvRecordV1);++i)p[i]=0;
      value_->mcv.clear();
    }
    ClearPlainObject(&value_->statistics_uuid);ClearPlainObject(&value_->source_object_uuid);
    ClearPlainObject(&value_->provider_evidence_uuid);value_->schema_epoch=0;
    value_->collection_epoch=0;value_->security_epoch=0;value_->row_count=0;
    value_->null_count=0;value_->value_count=0;value_->minimum_maximum_present=false;
    value_->minimum_day=0;value_->maximum_day=0;value_->profile.reset();
  }
  void Disarm() noexcept{active_=false;}
 private:
  DateDecodedStatisticsV1* value_;
  bool active_=true;
};

bool Reserve(std::vector<byte>* out,u64 n) noexcept {
  if (!out || n>std::numeric_limits<std::size_t>::max()) return false;
  try { out->assign(static_cast<std::size_t>(n),0); return true; }
  catch (...) { return false; }
}

void EncodeSortKeyRaw(const DateValueViewV1& value,
                      DateSortDirectionV1 direction,
                      DateNullModeV1 null_mode, byte* output) noexcept {
  std::memcpy(output, "SBDATK01", 8);
  StoreUuid(output + 8, value.profile->receipt.catalog_snapshot_uuid);
  StoreLittle64(output + 24, value.profile->receipt.catalog_generation);
  StoreLittle64(output + 32, value.profile->receipt.registry_generation);
  std::memcpy(output + 40, value.profile->comparison_fingerprint.data(), 32);
  StoreUuid(output + 72, value.profile->identity.ordering_policy.uuid);
  StoreLittle64(output + 88,
                value.profile->identity.ordering_policy.generation);
  output[96] = direction == DateSortDirectionV1::descending ? 1 : 0;
  output[97] = null_mode == DateNullModeV1::nulls_last ? 1 : 0;
  output[98] = value.state == DateValueStateV1::value ? 1
      : (null_mode == DateNullModeV1::nulls_first ? 0 : 2);
  output[99] = value.state == DateValueStateV1::value ? 4 : 0;
  if (value.state == DateValueStateV1::value) {
    const u32 sortable = static_cast<u32>(value.day) ^ 0x80000000u;
    output[100] = static_cast<byte>(sortable >> 24);
    output[101] = static_cast<byte>(sortable >> 16);
    output[102] = static_cast<byte>(sortable >> 8);
    output[103] = static_cast<byte>(sortable);
    if (direction == DateSortDirectionV1::descending)
      for (std::size_t i = 100; i != 104; ++i)
        output[i] = static_cast<byte>(~output[i]);
  }
}

bool EncodeHashRaw(const DateValueViewV1& value,
                   std::array<byte,32>* output) noexcept {
  if(output==nullptr||value.profile==nullptr)return false;
  std::array<byte,105> preimage{};ArrayClearGuard<105> clear_preimage(&preimage);
  std::memcpy(preimage.data(),"SBDATH01",8);
  StoreUuid(preimage.data()+8,value.profile->receipt.catalog_snapshot_uuid);
  StoreLittle64(preimage.data()+24,value.profile->receipt.catalog_generation);
  StoreLittle64(preimage.data()+32,value.profile->receipt.registry_generation);
  std::memcpy(preimage.data()+40,value.profile->comparison_fingerprint.data(),32);
  StoreUuid(preimage.data()+72,value.profile->identity.hash_policy.uuid);
  StoreLittle64(preimage.data()+88,value.profile->identity.hash_policy.generation);
  preimage[96]=value.state==DateValueStateV1::sql_null?0:1;
  StoreLittle32(preimage.data()+97,value.state==DateValueStateV1::sql_null?0:4);
  std::size_t extent=101;
  if(value.state==DateValueStateV1::value){StoreLittle32(preimage.data()+101,static_cast<u32>(value.day));extent=105;}
  const hash::HashDigestSegment segment{preimage.data(),extent};
  auto digest=hash::ComputeSha256DigestPartsNative(&segment,1);
  ArrayClearGuard<32> clear_digest(&digest.digest);
  if(!digest.ok())return false;
  *output=digest.digest;return true;
}

DateDiagnosticFactV1 ValidateProjectionValueMetadata(
    const DateOwnedValueV1& value,bool null_allowed,
    const DateValidatedProfileHandleV1* expected_profile=nullptr) noexcept {
  if(!value.profile||!ValidProfile(*value.profile)||
     (expected_profile&&!SameProfile(*value.profile,*expected_profile)))
    return {Error(),"CTI.TEMPORAL.DESCRIPTOR_INVALID","projection_profile"};
  if(value.state!=DateValueStateV1::value&&
     value.state!=DateValueStateV1::sql_null)
    return {Error(),"DATATYPE.NULL_STATE.INVALID","projection_state"};
  if(value.state==DateValueStateV1::sql_null){
    if(value.day!=0)return {Error(),"DATATYPE.NULL_STATE.INVALID","projection_dirty_null"};
    if(!null_allowed)return {Error(),"DATATYPE.NULL_NOT_ADMITTED","projection_null_not_admitted"};
  }
  return {Ok(),{}, {}};
}
} // namespace

DateIndexResolutionV1 ResolveDateIndexFamilyV1(DateIndexFamilyV1 family) noexcept {
  using D=DateIndexDispositionV1; using P=DateProjectionKindV1;
  static constexpr std::array<platform::Uuid,16> ids{{
    Uuid({0x01,0xa1,0x00,0x8e,0xc1,0x00,0x70,0x01,0x80,0,0,0,0,0,0,1}),
    Uuid({0x01,0xa1,0x00,0x8e,0xc1,0x00,0x70,0x02,0x80,0,0,0,0,0,0,2}),
    Uuid({0x01,0xa1,0x00,0x8e,0xc1,0x00,0x70,0x03,0x80,0,0,0,0,0,0,3}),
    Uuid({0x01,0xa1,0x00,0x8e,0xc1,0x00,0x70,0x04,0x80,0,0,0,0,0,0,4}),
    Uuid({0x01,0xa1,0x00,0x8e,0xc1,0x00,0x70,0x05,0x80,0,0,0,0,0,0,5}),
    Uuid({0x01,0xa1,0x00,0x8e,0xc1,0x00,0x70,0x06,0x80,0,0,0,0,0,0,6}),
    Uuid({0x01,0xa1,0x00,0x8e,0xc1,0x00,0x70,0x07,0x80,0,0,0,0,0,0,7}),
    Uuid({0x01,0xa1,0x00,0x8e,0xc1,0x00,0x70,0x08,0x80,0,0,0,0,0,0,8}),
    Uuid({0x01,0xa1,0x00,0x8e,0xc1,0x00,0x70,0x09,0x80,0,0,0,0,0,0,9}),
    Uuid({0x01,0xa1,0x00,0x8e,0xc1,0x00,0x70,0x0a,0x80,0,0,0,0,0,0,0x0a}),
    Uuid({0x01,0xa1,0x00,0x8e,0xc1,0x00,0x70,0x0b,0x80,0,0,0,0,0,0,0x0b}),
    Uuid({0x01,0xa1,0x00,0x8e,0xc1,0x00,0x70,0x0c,0x80,0,0,0,0,0,0,0x0c}),
    Uuid({0x01,0xa1,0x00,0x8e,0xc1,0x00,0x70,0x0d,0x80,0,0,0,0,0,0,0x0d}),
    Uuid({0x01,0xa1,0x00,0x8e,0xc1,0x00,0x70,0x0e,0x80,0,0,0,0,0,0,0x0e}),
    Uuid({0x01,0xa1,0x00,0x8e,0xc1,0x00,0x70,0x0f,0x80,0,0,0,0,0,0,0x0f}),
    Uuid({0x01,0xa1,0x00,0x8e,0xc1,0x00,0x70,0x10,0x80,0,0,0,0,0,0,0x10})}};
  if (!ValidFamily(family)) {
    DateIndexResolutionV1 invalid;
    invalid.family = family;
    invalid.disposition = D::not_applicable;
    invalid.receiving_owner_resolution_required = true;
    return invalid;
  }
  DateIndexResolutionV1 r{family,ids[static_cast<std::size_t>(family)],1};
  switch(family) {
    case DateIndexFamilyV1::bitmap: case DateIndexFamilyV1::hash:
      r.disposition=D::admitted_with_recheck;r.projection=P::equality_hash;r.exact_recheck_required=true;break;
    case DateIndexFamilyV1::brin_like: case DateIndexFamilyV1::columnar_zone_map:
      r.disposition=D::admitted_with_recheck;r.projection=P::zone_map;r.exact_recheck_required=true;break;
    case DateIndexFamilyV1::btree:
      r.disposition=D::admitted_exact_if_budget;r.projection=P::sort_key;break;
    case DateIndexFamilyV1::covering_included:
      r.disposition=D::admitted_payload_only;r.projection=P::covering_value;break;
    case DateIndexFamilyV1::range_exclusion:
      r.disposition=D::admitted_exact_if_budget;r.projection=P::range_pair;break;
    case DateIndexFamilyV1::temporary_work:
      r.disposition=D::admitted_exact_selected_mode;r.projection=P::none;break;
    case DateIndexFamilyV1::expression:
      r.disposition=D::conditional;r.projection=P::conditional_result;r.exact_recheck_required=true;r.receiving_owner_resolution_required=true;break;
    case DateIndexFamilyV1::partial_filtered:
      r.disposition=D::conditional;r.projection=P::conditional_underlying;r.exact_recheck_required=true;r.receiving_owner_resolution_required=true;break;
    default:
      r.disposition=D::not_applicable;r.projection=P::none;r.receiving_owner_resolution_required=true;break;
  }
  return r;
}

DateIndexPredicateResolutionV1 ResolveDateIndexPredicateV1(
    DateIndexFamilyV1 family, DateIndexPredicateOperationV1 operation) noexcept {
  using F=DateIndexFamilyV1;using O=DateIndexPredicateOperationV1;
  using P=DateIndexPredicateFactV1;
  auto exact=[](){return DateIndexPredicateResolutionV1{true,P::exact_no_recheck_after_decode_reencode,{}};};
  auto hash=[](){return DateIndexPredicateResolutionV1{true,P::requires_exact_state_day_recheck_never_final_match,{}};};
  auto prune=[](){return DateIndexPredicateResolutionV1{true,P::prune_only_mandatory_source_row_predicate_recheck_never_final_match,{}};};
  switch(family){
    case F::bitmap:if(operation==O::equality||operation==O::in||operation==O::is_null)return hash();break;
    case F::hash:if(operation==O::equality||operation==O::in)return hash();break;
    case F::brin_like:if(operation==O::equality||operation==O::range||operation==O::order)return prune();break;
    case F::columnar_zone_map:if(operation==O::prune_range||operation==O::equality)return prune();break;
    case F::btree:if(operation==O::equality||operation==O::range||operation==O::order)return exact();break;
    case F::covering_included:if(operation==O::include_payload)return exact();break;
    case F::range_exclusion:if(operation==O::range||operation==O::overlap||operation==O::exclusion)return exact();break;
    case F::temporary_work:if(operation==O::equality)return hash();if(operation==O::order)return exact();break;
    default:break;
  }
  return {false,P::refused,"CTI.TEMPORAL.INDEX_KEY_REFUSED"};
}

DateBytesResultV1 EncodeDateCoveringValueV1(const DateOwnedValueV1& value,
                                             const DateExecutionControlV1& c) noexcept {
  auto v=ValidateDateValueViewV1(value.view(),true); if(!v.ok()) return FailBytes(v.diagnostic.diagnostic_code,v.diagnostic.detail);
  const u64 n=value.state==DateValueStateV1::sql_null?1:5;
  auto profile_pin=value.profile;
  if(n>c.maximum_allocation_bytes) return FailBytes("RESOURCE.BUDGET_EXCEEDED","covering_allocation");
  if(Cancelled(c)) return FailBytes("PROCESS.CANCELLED","covering_cancelled");
  DateBytesResultV1 r; VectorClearGuard clear(&r.bytes);
  if(!Reserve(&r.bytes,n)) return FailBytes("RESOURCE.BUDGET_EXCEEDED","covering_allocation");
  r.bytes[0]=value.state==DateValueStateV1::sql_null?0:2;
  if(n==5) StoreLittle32(r.bytes.data()+1,static_cast<u32>(value.day));
  if(!DecodeDateCoveringValueNoAllocV1(*value.profile,true,r.bytes).ok()) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","covering_self_check");
  if(Cancelled(c)) return FailBytes("PROCESS.CANCELLED","covering_prepublication");
  r.status=Ok();clear.Disarm();return r;
}

DateCoveringValueViewResultV1 DecodeDateCoveringValueNoAllocV1(
    const DateValidatedProfileHandleV1& h,bool null_allowed,
    std::span<const byte> e) noexcept {
  if(e.empty()) return FailView<DateCoveringValueViewResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","minimum_extent");
  if(!ValidProfile(h)) return FailView<DateCoveringValueViewResultV1>("CTI.TEMPORAL.DESCRIPTOR_INVALID","profile");
  DateValueStateV1 state=DateValueStateV1::value;i32 day=0;
  PlainObjectClearGuard<DateValueStateV1> clear_state(&state);
  PlainObjectClearGuard<i32> clear_day(&day);
  if(e[0]==0) { if(e.size()!=1) return FailView<DateCoveringValueViewResultV1>("DATATYPE.NULL_STATE.INVALID","null_extent");
    if(!null_allowed) return FailView<DateCoveringValueViewResultV1>("DATATYPE.NULL_NOT_ADMITTED"); state=DateValueStateV1::sql_null;
  } else if(e[0]==2) { if(e.size()!=5) return FailView<DateCoveringValueViewResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","value_extent");
    state=DateValueStateV1::value;day=static_cast<i32>(LoadLittle32(e.data()+1));
  } else return FailView<DateCoveringValueViewResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","tag");
  std::array<byte,5> canonical{};ArrayClearGuard<5> clear_canonical(&canonical);
  canonical[0]=state==DateValueStateV1::sql_null?0:2;
  const std::size_t canonical_extent=state==DateValueStateV1::sql_null?1:5;
  if(canonical_extent==5)StoreLittle32(canonical.data()+1,static_cast<u32>(day));
  if(e.size()!=canonical_extent||std::memcmp(e.data(),canonical.data(),canonical_extent))
    return FailView<DateCoveringValueViewResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","covering_reencode");
  DateCoveringValueViewResultV1 r;r.value={&h,state,day};r.status=Ok();return r;
}

DateBytesResultV1 EncodeDateRangePairV1(const DateOwnedValueV1& lower,
                                        const DateOwnedValueV1& upper,
                                        const DateExecutionControlV1& c) noexcept {
  auto l=ValidateDateValueViewV1(lower.view(),false); if(!l.ok()) return FailBytes(l.diagnostic.diagnostic_code,l.diagnostic.detail);
  auto u=ValidateDateValueViewV1(upper.view(),false); if(!u.ok()) return FailBytes(u.diagnostic.diagnostic_code,u.diagnostic.detail);
  if(!SameProfile(*lower.profile,*upper.profile) || lower.day>upper.day) return FailBytes("CTI.TEMPORAL.INDEX_KEY_REFUSED","range_order_or_profile");
  auto profile_pin=lower.profile;
  if(208>c.maximum_allocation_bytes) return FailBytes("RESOURCE.BUDGET_EXCEEDED","range_allocation");
  if(Cancelled(c)) return FailBytes("PROCESS.CANCELLED","range_cancelled");
  DateBytesResultV1 r;VectorClearGuard clear(&r.bytes);
  if(!Reserve(&r.bytes,208)) return FailBytes("RESOURCE.BUDGET_EXCEEDED","range_allocation");
  EncodeSortKeyRaw(lower.view(),DateSortDirectionV1::ascending,DateNullModeV1::nulls_last,r.bytes.data());
  EncodeSortKeyRaw(upper.view(),DateSortDirectionV1::ascending,DateNullModeV1::nulls_last,r.bytes.data()+104);
  if(!DecodeDateRangePairNoAllocV1(*lower.profile,r.bytes).ok()) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","range_self_check");
  if(Cancelled(c)) return FailBytes("PROCESS.CANCELLED","range_prepublication");
  r.status=Ok();clear.Disarm();return r;
}

DateRangePairViewResultV1 DecodeDateRangePairNoAllocV1(
    const DateValidatedProfileHandleV1& h,std::span<const byte> e) noexcept {
  if(e.size()!=208) return FailView<DateRangePairViewResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","extent");
  auto l=DecodeDateSortKeyNoAllocV1(h,e.first(104)); if(!l.ok()) return FailView<DateRangePairViewResultV1>(l.diagnostic.diagnostic_code,l.diagnostic.detail);
  auto u=DecodeDateSortKeyNoAllocV1(h,e.subspan(104)); if(!u.ok()) return FailView<DateRangePairViewResultV1>(u.diagnostic.diagnostic_code,u.diagnostic.detail);
  if(l.value.state!=DateValueStateV1::value||u.value.state!=DateValueStateV1::value||l.value.direction!=DateSortDirectionV1::ascending||u.value.direction!=DateSortDirectionV1::ascending||l.value.null_mode!=DateNullModeV1::nulls_last||u.value.null_mode!=DateNullModeV1::nulls_last||l.value.day>u.value.day)
    return FailView<DateRangePairViewResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","range_invariant");
  DateRangePairViewResultV1 r;r.value={l.value.day,u.value.day,e.first(104),e.subspan(104)};r.status=Ok();return r;
}

static DateBytesResultV1 EncodeDateZoneMapImpl(
    const DateZoneMapRequestV1& q,u64 provider_max_key_bytes,
    const DateExecutionControlV1& control,bool acquire_profile_pin) noexcept {
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
  const u64 n=has?304:96;
  std::shared_ptr<const DateValidatedProfileHandleV1> profile_pin;
  if(acquire_profile_pin)profile_pin=q.profile;
  if(n>provider_max_key_bytes) return FailBytes("CTI.TEMPORAL.INDEX_KEY_REFUSED","zone_provider_budget");
  if(n>control.maximum_allocation_bytes) return FailBytes("RESOURCE.BUDGET_EXCEEDED","zone_allocation");
  if(Cancelled(control)) return FailBytes("PROCESS.CANCELLED","zone_cancelled");
  if(has&&q.minimum->day>q.maximum->day)
    return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_extrema_order");
  DateBytesResultV1 r;VectorClearGuard clear(&r.bytes);
  if(!Reserve(&r.bytes,n)) return FailBytes("RESOURCE.BUDGET_EXCEEDED","zone_allocation");
  std::memcpy(r.bytes.data(),kZoneMagic,8);StoreLittle16(r.bytes.data()+8,1);StoreLittle16(r.bytes.data()+10,96);StoreLittle32(r.bytes.data()+12,static_cast<u32>(n));
  StoreLittle32(r.bytes.data()+16,has?5u:4u);StoreLittle64(r.bytes.data()+24,q.null_count);StoreLittle64(r.bytes.data()+32,q.value_count);
  if(has) {
    StoreLittle32(r.bytes.data()+40,static_cast<u32>(q.minimum->day));StoreLittle32(r.bytes.data()+44,static_cast<u32>(q.maximum->day));
    StoreLittle32(r.bytes.data()+48,104);StoreLittle32(r.bytes.data()+52,104);
    EncodeSortKeyRaw(q.minimum->view(),DateSortDirectionV1::ascending,DateNullModeV1::nulls_last,r.bytes.data()+96);
    EncodeSortKeyRaw(q.maximum->view(),DateSortDirectionV1::ascending,DateNullModeV1::nulls_last,r.bytes.data()+200);
  }
  std::array<byte,32> digest{};ArrayClearGuard<32> clear_digest(&digest);if(!HashRecord(kZoneDomain,r.bytes,64,&digest)) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_hash_provider");
  std::memcpy(r.bytes.data()+64,digest.data(),32);
  if(!DecodeDateZoneMapNoAllocV1(*q.profile,r.bytes).ok()) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_self_check");
  if(Cancelled(control)) return FailBytes("PROCESS.CANCELLED","zone_prepublication");
  r.status=Ok();clear.Disarm();return r;
}

DateBytesResultV1 EncodeDateZoneMapV1(const DateZoneMapRequestV1& q) noexcept {
  return EncodeDateZoneMapImpl(q,q.provider_max_key_bytes,q.control,true);
}

DateZoneMapViewResultV1 DecodeDateZoneMapNoAllocV1(
    const DateValidatedProfileHandleV1& h,std::span<const byte> e) noexcept {
  if((e.size()!=96&&e.size()!=304)||std::memcmp(e.data(),kZoneMagic,8)||LoadLittle16(e.data()+8)!=1||LoadLittle16(e.data()+10)!=96||LoadLittle32(e.data()+12)!=e.size()||LoadLittle32(e.data()+20)!=0||LoadLittle64(e.data()+56)!=0)
    return FailView<DateZoneMapViewResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_structure");
  const u32 flags=LoadLittle32(e.data()+16);
  if((flags&~u32{5})!=0)
    return FailView<DateZoneMapViewResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_reserved_flags");
  if(!HashEquals(kZoneDomain,e,64)) return FailView<DateZoneMapViewResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_hash");
  if(!ValidProfile(h)) return FailView<DateZoneMapViewResultV1>("CTI.TEMPORAL.DESCRIPTOR_INVALID","profile");
  const u64 nc=LoadLittle64(e.data()+24),vc=LoadLittle64(e.data()+32);
  if(vc>std::numeric_limits<u64>::max()-nc||flags!=(vc?5u:4u)) return FailView<DateZoneMapViewResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_counts_flags");
  i32 minimum_day=0,maximum_day=0;PlainObjectClearGuard<i32> clear_minimum(&minimum_day);PlainObjectClearGuard<i32> clear_maximum(&maximum_day);std::span<const byte> minimum_key,maximum_key;
  if(!vc) {
    if(e.size()!=96||LoadLittle32(e.data()+40)||LoadLittle32(e.data()+44)||LoadLittle32(e.data()+48)||LoadLittle32(e.data()+52)) return FailView<DateZoneMapViewResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_empty");
  } else {
    if(e.size()!=304||LoadLittle32(e.data()+48)!=104||LoadLittle32(e.data()+52)!=104) return FailView<DateZoneMapViewResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_offsets");
    auto lo=DecodeDateSortKeyNoAllocV1(h,e.subspan(96,104));auto hi=DecodeDateSortKeyNoAllocV1(h,e.subspan(200,104));
    if(!lo.ok())return FailView<DateZoneMapViewResultV1>(lo.diagnostic.diagnostic_code,lo.diagnostic.detail);
    if(!hi.ok())return FailView<DateZoneMapViewResultV1>(hi.diagnostic.diagnostic_code,hi.diagnostic.detail);
    if(lo.value.state!=DateValueStateV1::value||hi.value.state!=DateValueStateV1::value||lo.value.direction!=DateSortDirectionV1::ascending||hi.value.direction!=DateSortDirectionV1::ascending||lo.value.null_mode!=DateNullModeV1::nulls_last||hi.value.null_mode!=DateNullModeV1::nulls_last)
      return FailView<DateZoneMapViewResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_nested_key");
    const i32 ld=static_cast<i32>(LoadLittle32(e.data()+40)),ud=static_cast<i32>(LoadLittle32(e.data()+44));
    if(ld!=lo.value.day||ud!=hi.value.day||ld>ud) return FailView<DateZoneMapViewResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_extrema");
    minimum_day=ld;maximum_day=ud;minimum_key=e.subspan(96,104);maximum_key=e.subspan(200,104);
  }
  std::array<byte,304> canonical{};ArrayClearGuard<304> clear_canonical(&canonical);
  std::memcpy(canonical.data(),kZoneMagic,8);StoreLittle16(canonical.data()+8,1);
  StoreLittle16(canonical.data()+10,96);StoreLittle32(canonical.data()+12,static_cast<u32>(e.size()));
  StoreLittle32(canonical.data()+16,vc?5u:4u);StoreLittle64(canonical.data()+24,nc);StoreLittle64(canonical.data()+32,vc);
  if(vc){
    StoreLittle32(canonical.data()+40,static_cast<u32>(minimum_day));
    StoreLittle32(canonical.data()+44,static_cast<u32>(maximum_day));
    StoreLittle32(canonical.data()+48,104);StoreLittle32(canonical.data()+52,104);
    EncodeSortKeyRaw({&h,DateValueStateV1::value,minimum_day},DateSortDirectionV1::ascending,DateNullModeV1::nulls_last,canonical.data()+96);
    EncodeSortKeyRaw({&h,DateValueStateV1::value,maximum_day},DateSortDirectionV1::ascending,DateNullModeV1::nulls_last,canonical.data()+200);
  }
  std::array<byte,32> canonical_hash{};ArrayClearGuard<32> clear_hash(&canonical_hash);
  if(!HashRecord(kZoneDomain,std::span<const byte>(canonical.data(),e.size()),64,&canonical_hash))
    return FailView<DateZoneMapViewResultV1>("RESOURCE.BUDGET_EXCEEDED","zone_reencode_hash");
  std::memcpy(canonical.data()+64,canonical_hash.data(),32);
  if(std::memcmp(e.data(),canonical.data(),e.size()))
    return FailView<DateZoneMapViewResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_reencode");
  DateZoneMapViewResultV1 r;r.value={&h,nc,vc,minimum_day,maximum_day,minimum_key,maximum_key};r.status=Ok();return r;
}

namespace {
void PutStatisticsIdentity(byte* p,const DateValidatedProfileHandleV1& h) noexcept {
  PutCommonIdentity(p,h);
  StoreUuid(p+112,h.statistics_policy.uuid);StoreLittle64(p+128,h.statistics_policy.generation);
  StoreUuid(p+136,h.calendar_policy.uuid);StoreLittle64(p+152,h.calendar_policy.generation);
  StoreUuid(p+160,h.storage_epoch_policy.uuid);StoreLittle64(p+176,h.storage_epoch_policy.generation);
  StoreUuid(p+184,h.identity.ordering_policy.uuid);StoreLittle64(p+200,h.identity.ordering_policy.generation);
  StoreUuid(p+208,h.identity.hash_policy.uuid);StoreLittle64(p+224,h.identity.hash_policy.generation);
}
bool StatisticsIdentityEquals(const byte* p,const DateValidatedProfileHandleV1& h) noexcept {
  return CommonIdentityEquals(p,h)&&LoadUuid(p+112)==h.statistics_policy.uuid&&LoadLittle64(p+128)==h.statistics_policy.generation&&
      LoadUuid(p+136)==h.calendar_policy.uuid&&LoadLittle64(p+152)==h.calendar_policy.generation&&
      LoadUuid(p+160)==h.storage_epoch_policy.uuid&&LoadLittle64(p+176)==h.storage_epoch_policy.generation&&
      LoadUuid(p+184)==h.identity.ordering_policy.uuid&&LoadLittle64(p+200)==h.identity.ordering_policy.generation&&
      LoadUuid(p+208)==h.identity.hash_policy.uuid&&LoadLittle64(p+224)==h.identity.hash_policy.generation;
}

bool EncodeStatisticsRaw(const DateStatisticsProjectionV1& p,
                         const DateValidatedProfileHandleV1& profile,
                         const platform::Uuid& provider_evidence_uuid,
                         std::vector<byte>* bytes,
                         const DateExecutionControlV1* control = nullptr,
                         bool* cancelled_between_groups = nullptr) noexcept {
  if(cancelled_between_groups) *cancelled_between_groups=false;
  const u64 n=512+16*p.histogram.size()+48*p.mcv.size();
  if(!Reserve(bytes,n)) return false;
  std::memcpy(bytes->data(),kStatisticsMagic,8);StoreLittle16(bytes->data()+8,1);StoreLittle16(bytes->data()+10,512);StoreLittle32(bytes->data()+12,static_cast<u32>(n));
  u32 flags=(p.histogram.empty()?0:1)|(p.mcv.empty()?0:2)|(p.value_count?4:0)|(p.mcv.empty()?0:32);StoreLittle32(bytes->data()+16,flags);StoreLittle32(bytes->data()+20,p.histogram.size());StoreLittle32(bytes->data()+24,p.mcv.size());
  PutStatisticsIdentity(bytes->data()+32,profile);StoreUuid(bytes->data()+264,p.statistics_uuid);StoreUuid(bytes->data()+280,p.source_object_uuid);
  StoreLittle64(bytes->data()+296,p.schema_epoch);StoreLittle64(bytes->data()+304,p.collection_epoch);StoreLittle64(bytes->data()+312,p.security_epoch);StoreLittle64(bytes->data()+320,p.row_count);StoreLittle64(bytes->data()+328,p.null_count);StoreLittle64(bytes->data()+336,p.value_count);
  if(p.value_count){StoreLittle32(bytes->data()+360,static_cast<u32>(p.minimum_day));StoreLittle32(bytes->data()+364,static_cast<u32>(p.maximum_day));}
  StoreLittle32(bytes->data()+368,512);StoreLittle32(bytes->data()+372,512+16*p.histogram.size());StoreUuid(bytes->data()+376,provider_evidence_uuid);std::memcpy(bytes->data()+424,profile.profile_fingerprint.data(),32);
  StoreLittle64(bytes->data()+456,0x8000000000000000ULL);StoreUuid(bytes->data()+488,kPrivacyUuid);StoreLittle64(bytes->data()+504,1);
  std::size_t off=512;for(auto& x:p.histogram){StoreLittle32(bytes->data()+off,static_cast<u32>(x.inclusive_upper_day));StoreLittle64(bytes->data()+off+8,x.noncumulative_count);off+=16;}
  if(control&&Cancelled(*control)){if(cancelled_between_groups)*cancelled_between_groups=true;return false;}
  for(auto& x:p.mcv){std::memcpy(bytes->data()+off,x.value_hash.data(),32);StoreLittle64(bytes->data()+off+32,x.frequency);StoreLittle32(bytes->data()+off+40,1);off+=48;}
  std::array<byte,32> d{};ArrayClearGuard<32> clear_digest(&d);if(!HashRecord(kStatisticsDomain,*bytes,392,&d))return false;std::memcpy(bytes->data()+392,d.data(),32);
  return true;
}

bool ValidateStatisticsRecordNoAlloc(
    const DateValidatedProfileHandleV1& profile,
    std::span<const byte> e) noexcept {
  if(e.size()<512||e.size()>4608||std::memcmp(e.data(),kStatisticsMagic,8)||
     LoadLittle16(e.data()+8)!=1||LoadLittle16(e.data()+10)!=512||
     LoadLittle32(e.data()+12)!=e.size()||LoadLittle32(e.data()+28))return false;
  const u32 hc=LoadLittle32(e.data()+20),mc=LoadLittle32(e.data()+24);
  const u64 expected=512ull+16ull*hc+48ull*mc;
  if(hc>64||mc>64||expected!=e.size()||LoadLittle32(e.data()+368)!=512||
     LoadLittle32(e.data()+372)!=512+16*hc||!HashEquals(kStatisticsDomain,e,392)||
     !StatisticsIdentityEquals(e.data()+32,profile)||
     std::memcmp(e.data()+424,profile.profile_fingerprint.data(),32)||
     LoadUuid(e.data()+488)!=kPrivacyUuid||LoadLittle64(e.data()+504)!=1)return false;
  const auto statistics_uuid=LoadUuid(e.data()+264);
  const auto source_uuid=LoadUuid(e.data()+280);
  const auto provider_uuid=LoadUuid(e.data()+376);
  const u64 schema_epoch=LoadLittle64(e.data()+296);
  const u64 collection_epoch=LoadLittle64(e.data()+304);
  const u64 security_epoch=LoadLittle64(e.data()+312);
  const u64 row_count=LoadLittle64(e.data()+320);
  const u64 null_count=LoadLittle64(e.data()+328);
  const u64 value_count=LoadLittle64(e.data()+336);
  if(!IsV7(statistics_uuid)||Nil(source_uuid)||!IsV7(provider_uuid)||
     statistics_uuid==source_uuid||statistics_uuid==provider_uuid||
     source_uuid==provider_uuid||!schema_epoch||!collection_epoch||!security_epoch)return false;
  const u32 flags=LoadLittle32(e.data()+16);
  const u32 want=(hc?1:0)|(mc?2:0)|(value_count?4:0)|(mc?32:0);
  if(flags!=want||LoadLittle64(e.data()+344)||e[352]||
     std::any_of(e.begin()+353,e.begin()+360,[](byte b){return b!=0;})||
     LoadLittle64(e.data()+456)!=0x8000000000000000ULL||
     !LoadUuid(e.data()+464).is_nil()||LoadLittle64(e.data()+480)||
     null_count>std::numeric_limits<u64>::max()-value_count||
     row_count!=null_count+value_count)return false;
  const i32 minimum=static_cast<i32>(LoadLittle32(e.data()+360));
  const i32 maximum=static_cast<i32>(LoadLittle32(e.data()+364));
  if((!value_count&&(minimum||maximum))||(value_count&&minimum>maximum))return false;
  std::array<byte,4608> canonical{};ArrayClearGuard<4608> clear_canonical(&canonical);
  std::memcpy(canonical.data(),kStatisticsMagic,8);StoreLittle16(canonical.data()+8,1);
  StoreLittle16(canonical.data()+10,512);StoreLittle32(canonical.data()+12,static_cast<u32>(expected));
  StoreLittle32(canonical.data()+16,flags);StoreLittle32(canonical.data()+20,hc);StoreLittle32(canonical.data()+24,mc);
  PutStatisticsIdentity(canonical.data()+32,profile);StoreUuid(canonical.data()+264,statistics_uuid);
  StoreUuid(canonical.data()+280,source_uuid);StoreLittle64(canonical.data()+296,schema_epoch);
  StoreLittle64(canonical.data()+304,collection_epoch);StoreLittle64(canonical.data()+312,security_epoch);
  StoreLittle64(canonical.data()+320,row_count);StoreLittle64(canonical.data()+328,null_count);
  StoreLittle64(canonical.data()+336,value_count);
  if(value_count){StoreLittle32(canonical.data()+360,static_cast<u32>(minimum));StoreLittle32(canonical.data()+364,static_cast<u32>(maximum));}
  StoreLittle32(canonical.data()+368,512);StoreLittle32(canonical.data()+372,512+16*hc);
  StoreUuid(canonical.data()+376,provider_uuid);
  std::memcpy(canonical.data()+424,profile.profile_fingerprint.data(),32);
  StoreLittle64(canonical.data()+456,0x8000000000000000ULL);
  StoreUuid(canonical.data()+488,kPrivacyUuid);StoreLittle64(canonical.data()+504,1);
  std::size_t off=512;u64 sum=0;i32 prior=std::numeric_limits<i32>::min();bool first=true;
  for(u32 i=0;i<hc;++i,off+=16){const i32 day=static_cast<i32>(LoadLittle32(e.data()+off));const u64 n=LoadLittle64(e.data()+off+8);if(LoadLittle32(e.data()+off+4)||!n||day<minimum||day>maximum||(!first&&day<=prior)||n>std::numeric_limits<u64>::max()-sum)return false;sum+=n;prior=day;first=false;StoreLittle32(canonical.data()+off,static_cast<u32>(day));StoreLittle64(canonical.data()+off+8,n);}
  if(hc&&(prior!=maximum||sum!=value_count))return false;
  sum=0;std::array<byte,32> prior_hash{};ArrayClearGuard<32> clear_prior(&prior_hash);first=true;
  for(u32 i=0;i<mc;++i,off+=48){const byte* current=e.data()+off;const u64 frequency=LoadLittle64(current+32);if(!frequency||LoadLittle32(current+40)!=1||LoadLittle32(current+44)||(!first&&!std::lexicographical_compare(prior_hash.begin(),prior_hash.end(),current,current+32))||frequency>std::numeric_limits<u64>::max()-sum)return false;sum+=frequency;std::memcpy(prior_hash.data(),current,32);first=false;std::memcpy(canonical.data()+off,current,32);StoreLittle64(canonical.data()+off+32,frequency);StoreLittle32(canonical.data()+off+40,1);}
  if(sum>value_count)return false;
  std::array<byte,32> digest{};ArrayClearGuard<32> clear_digest(&digest);
  if(!HashRecord(kStatisticsDomain,std::span<const byte>(canonical.data(),expected),392,&digest))return false;
  std::memcpy(canonical.data()+392,digest.data(),32);
  return std::memcmp(e.data(),canonical.data(),expected)==0;
}
}

static DateStatisticsDecodeResultV1 DecodeDateStatisticsProjectionImpl(
    const std::shared_ptr<const DateValidatedProfileHandleV1>& profile,
    std::span<const byte> encoded, const DateExecutionControlV1& control,
    bool retain_profile) noexcept;

DateBytesResultV1 EncodeDateStatisticsProjectionV1(
    const DateStatisticsProjectionV1& p,const DateExecutionControlV1& c) noexcept {
  if(!p.profile||!ValidProfile(*p.profile)) return FailBytes("CTI.TEMPORAL.DESCRIPTOR_INVALID","statistics_profile");
  if(!p.provider_evidence||!IsV7(p.provider_evidence->provider_evidence_uuid)) return FailBytes("CTI.TEMPORAL.DESCRIPTOR_INVALID","statistics_provider_evidence");
  const auto provider_uuid=p.provider_evidence->provider_evidence_uuid;
  if(p.histogram.size()>64||p.mcv.size()>64) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","statistics_count");
  if(!IsV7(p.statistics_uuid)||Nil(p.source_object_uuid)||p.statistics_uuid==p.source_object_uuid||p.statistics_uuid==provider_uuid||p.source_object_uuid==provider_uuid||!p.schema_epoch||!p.collection_epoch||!p.security_epoch)
    return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","statistics_instance_identity");
  if(p.null_count>std::numeric_limits<u64>::max()-p.value_count||p.row_count!=p.null_count+p.value_count||p.minimum_maximum_present!=(p.value_count!=0)||(p.value_count&&p.minimum_day>p.maximum_day))
    return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","statistics_counts");
  u64 hsum=0; i32 prior=std::numeric_limits<i32>::min(); bool first=true;
  for(auto& x:p.histogram) { if(!x.noncumulative_count||x.inclusive_upper_day<p.minimum_day||x.inclusive_upper_day>p.maximum_day||(!first&&x.inclusive_upper_day<=prior)||x.noncumulative_count>std::numeric_limits<u64>::max()-hsum) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","histogram"); hsum+=x.noncumulative_count;prior=x.inclusive_upper_day;first=false; }
  if(!p.histogram.empty()&&(prior!=p.maximum_day||hsum!=p.value_count)) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","histogram_total");
  u64 msum=0; std::array<byte,32> ph{}; ArrayClearGuard<32> clear_prior_hash(&ph); first=true;
  for(auto& x:p.mcv) { if(!x.frequency||(!first&&!std::lexicographical_compare(ph.begin(),ph.end(),x.value_hash.begin(),x.value_hash.end()))||x.frequency>std::numeric_limits<u64>::max()-msum) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","mcv");msum+=x.frequency;ph=x.value_hash;first=false; }
  if(msum>p.value_count) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","mcv_total");
  auto profile_pin=p.profile;auto evidence_pin=p.provider_evidence;
  const u64 n=512+16*p.histogram.size()+48*p.mcv.size();if(n>c.maximum_allocation_bytes) return FailBytes("RESOURCE.BUDGET_EXCEEDED","statistics_allocation");if(Cancelled(c)) return FailBytes("PROCESS.CANCELLED","statistics_cancelled");
  DateBytesResultV1 r;VectorClearGuard clear(&r.bytes);
  bool cancelled_between_groups=false;
  if(!EncodeStatisticsRaw(p,*profile_pin,provider_uuid,&r.bytes,&c,
                          &cancelled_between_groups))
    return cancelled_between_groups
        ? FailBytes("PROCESS.CANCELLED","statistics_between_histogram_and_mcv")
        : FailBytes("RESOURCE.BUDGET_EXCEEDED","statistics_allocation");
  if(!ValidateStatisticsRecordNoAlloc(*profile_pin,r.bytes))
    return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                     "statistics_self_check");
  if(Cancelled(c))return FailBytes("PROCESS.CANCELLED","statistics_prepublication");r.status=Ok();clear.Disarm();return r;
}

static DateStatisticsDecodeResultV1 DecodeDateStatisticsProjectionImpl(
    const std::shared_ptr<const DateValidatedProfileHandleV1>& profile,std::span<const byte> e,
    const DateExecutionControlV1& c, bool retain_profile) noexcept {
  if(e.size()<512||e.size()>4608||std::memcmp(e.data(),kStatisticsMagic,8)||LoadLittle16(e.data()+8)!=1||LoadLittle16(e.data()+10)!=512||LoadLittle32(e.data()+12)!=e.size()||LoadLittle32(e.data()+28))return FailView<DateStatisticsDecodeResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","statistics_structure");
  const u32 hc=LoadLittle32(e.data()+20),mc=LoadLittle32(e.data()+24);const u64 expected=512ull+16ull*hc+48ull*mc;
  if(hc>64||mc>64||expected!=e.size()||LoadLittle32(e.data()+368)!=512||LoadLittle32(e.data()+372)!=512+16*hc)return FailView<DateStatisticsDecodeResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","statistics_offsets");
  if(!HashEquals(kStatisticsDomain,e,392))return FailView<DateStatisticsDecodeResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","statistics_hash");
  if(!profile||!ValidProfile(*profile))return FailView<DateStatisticsDecodeResultV1>("CTI.TEMPORAL.DESCRIPTOR_INVALID","profile");
  if(!StatisticsIdentityEquals(e.data()+32,*profile)||std::memcmp(e.data()+424,profile->profile_fingerprint.data(),32)||LoadUuid(e.data()+488)!=kPrivacyUuid||LoadLittle64(e.data()+504)!=1)return FailView<DateStatisticsDecodeResultV1>("CTI.TEMPORAL.DESCRIPTOR_INVALID","statistics_identity");
  const auto statistics_uuid=LoadUuid(e.data()+264),source_object_uuid=LoadUuid(e.data()+280),provider_evidence_uuid=LoadUuid(e.data()+376);
  const u64 schema_epoch=LoadLittle64(e.data()+296),collection_epoch=LoadLittle64(e.data()+304),security_epoch=LoadLittle64(e.data()+312),row_count=LoadLittle64(e.data()+320),null_count=LoadLittle64(e.data()+328),value_count=LoadLittle64(e.data()+336);
  if(!IsV7(statistics_uuid)||Nil(source_object_uuid)||!IsV7(provider_evidence_uuid)||statistics_uuid==source_object_uuid||statistics_uuid==provider_evidence_uuid||source_object_uuid==provider_evidence_uuid||!schema_epoch||!collection_epoch||!security_epoch)return FailView<DateStatisticsDecodeResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","statistics_instance_identity");
  const u32 flags=LoadLittle32(e.data()+16);const u32 want=(hc?1:0)|(mc?2:0)|(value_count?4:0)|(mc?32:0);
  if(flags!=want||LoadLittle64(e.data()+344)||e[352]||std::any_of(e.begin()+353,e.begin()+360,[](byte b){return b!=0;})||LoadLittle64(e.data()+456)!=0x8000000000000000ULL||!LoadUuid(e.data()+464).is_nil()||LoadLittle64(e.data()+480))
    return FailView<DateStatisticsDecodeResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","statistics_reserved_models");
  const i32 minimum_day=static_cast<i32>(LoadLittle32(e.data()+360)),maximum_day=static_cast<i32>(LoadLittle32(e.data()+364));
  if(null_count>std::numeric_limits<u64>::max()-value_count||row_count!=null_count+value_count||(!value_count&&(minimum_day||maximum_day))||(value_count&&minimum_day>maximum_day))return FailView<DateStatisticsDecodeResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","statistics_counts");
  std::shared_ptr<const DateValidatedProfileHandleV1> profile_pin;
  if(retain_profile) profile_pin=profile;
  DateStatisticsDecodeResultV1 r;auto& s=r.statistics;
  s.statistics_uuid=statistics_uuid;s.source_object_uuid=source_object_uuid;s.provider_evidence_uuid=provider_evidence_uuid;s.schema_epoch=schema_epoch;s.collection_epoch=collection_epoch;s.security_epoch=security_epoch;s.row_count=row_count;s.null_count=null_count;s.value_count=value_count;s.minimum_maximum_present=value_count!=0;s.minimum_day=minimum_day;s.maximum_day=maximum_day;
  DecodedStatisticsClearGuard clear_statistics(&s);
  if(expected>c.maximum_allocation_bytes)return FailView<DateStatisticsDecodeResultV1>("RESOURCE.BUDGET_EXCEEDED","statistics_allocation");if(Cancelled(c))return FailView<DateStatisticsDecodeResultV1>("PROCESS.CANCELLED");
  try {s.histogram.reserve(hc);s.mcv.reserve(mc);}catch(...){return FailView<DateStatisticsDecodeResultV1>("RESOURCE.BUDGET_EXCEEDED");}
  std::size_t off=512;u64 sum=0;i32 prior=std::numeric_limits<i32>::min();bool first=true;
  for(u32 i=0;i<hc;++i,off+=16){i32 day=static_cast<i32>(LoadLittle32(e.data()+off));u64 n=LoadLittle64(e.data()+off+8);if(LoadLittle32(e.data()+off+4)||!n||day<s.minimum_day||day>s.maximum_day||(!first&&day<=prior)||n>std::numeric_limits<u64>::max()-sum)return FailView<DateStatisticsDecodeResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","histogram");sum+=n;prior=day;first=false;s.histogram.push_back({day,n});}
  if(hc&&(prior!=s.maximum_day||sum!=s.value_count))return FailView<DateStatisticsDecodeResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","histogram_total");
  if(Cancelled(c))return FailView<DateStatisticsDecodeResultV1>("PROCESS.CANCELLED","statistics_between_histogram_and_mcv");
  sum=0;std::array<byte,32> ph{};ArrayClearGuard<32> clear_prior_hash(&ph);first=true;
  for(u32 i=0;i<mc;++i,off+=48){s.mcv.emplace_back();auto& x=s.mcv.back();std::memcpy(x.value_hash.data(),e.data()+off,32);x.frequency=LoadLittle64(e.data()+off+32);if(!x.frequency||LoadLittle32(e.data()+off+40)!=1||LoadLittle32(e.data()+off+44)||(!first&&!std::lexicographical_compare(ph.begin(),ph.end(),x.value_hash.begin(),x.value_hash.end()))||x.frequency>std::numeric_limits<u64>::max()-sum)return FailView<DateStatisticsDecodeResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","mcv");sum+=x.frequency;ph=x.value_hash;first=false;}
  if(sum>s.value_count)return FailView<DateStatisticsDecodeResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","mcv_total");
  if(!ValidateStatisticsRecordNoAlloc(*profile,e))
    return FailView<DateStatisticsDecodeResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","statistics_reencode");
  if(Cancelled(c))return FailView<DateStatisticsDecodeResultV1>("PROCESS.CANCELLED","statistics_prepublication");
  if(retain_profile) s.profile=std::move(profile_pin);
  r.status=Ok();clear_statistics.Disarm();return r;
}

DateStatisticsDecodeResultV1 DecodeDateStatisticsProjectionV1(
    const std::shared_ptr<const DateValidatedProfileHandleV1>& profile,
    std::span<const byte> encoded,
    const DateExecutionControlV1& control) noexcept {
  return DecodeDateStatisticsProjectionImpl(profile,encoded,control,true);
}

DateStatisticsReceivingDispositionV1 ResolveDateStatisticsReceivingFactsV1(
    const DateDecodedStatisticsV1& d,const DateStatisticsReceivingFactsV1& c) noexcept {
  if(!c.security_visible)return DateStatisticsReceivingDispositionV1::security_denied;
  if(!c.privacy_admitted)return DateStatisticsReceivingDispositionV1::privacy_denied;
  if(c.schema_epoch!=d.schema_epoch)return DateStatisticsReceivingDispositionV1::schema_epoch_mismatch;
  if(c.collection_epoch!=d.collection_epoch)return DateStatisticsReceivingDispositionV1::collection_epoch_mismatch;
  if(c.security_epoch!=d.security_epoch)return DateStatisticsReceivingDispositionV1::security_epoch_mismatch;
  if(c.provider_evidence_uuid!=d.provider_evidence_uuid)return DateStatisticsReceivingDispositionV1::provider_evidence_mismatch;
  return DateStatisticsReceivingDispositionV1::admitted;
}

DateStatisticsReceivingDispositionV1 ResolveDateStatisticsEvidenceV1(
    DateStatisticsEvidenceClassV1 evidence) noexcept {
  using E=DateStatisticsEvidenceClassV1;
  using D=DateStatisticsReceivingDispositionV1;
  switch(evidence){
    case E::full_visible_population:return D::admitted;
    case E::partial_scan:case E::weighted_input:case E::sampled_input:return D::full_recollection_required;
    case E::snapshot_merge:return D::manual_review_required;
    case E::histogram_source_contradiction:return D::provider_evidence_mismatch;
  }
  return D::provider_evidence_mismatch;
}

namespace {
void PutBackupIdentity(byte* p,const DateValidatedProfileHandleV1& h) noexcept {
  PutCommonIdentity(p,h);std::memcpy(p+112,h.profile_fingerprint.data(),32);
  StoreUuid(p+144,h.calendar_policy.uuid);StoreLittle64(p+160,h.calendar_policy.generation);
  StoreUuid(p+168,h.storage_epoch_policy.uuid);StoreLittle64(p+184,h.storage_epoch_policy.generation);
  StoreUuid(p+192,h.backup_transport_policy.uuid);StoreLittle64(p+208,h.backup_transport_policy.generation);
  StoreUuid(p+216,h.protection_policy.uuid);StoreLittle64(p+232,h.protection_policy.generation);
}
bool BackupIdentityEquals(const byte* p,const DateValidatedProfileHandleV1& h) noexcept {
  return CommonIdentityEquals(p,h)&&!std::memcmp(p+112,h.profile_fingerprint.data(),32)&&LoadUuid(p+144)==h.calendar_policy.uuid&&LoadLittle64(p+160)==h.calendar_policy.generation&&LoadUuid(p+168)==h.storage_epoch_policy.uuid&&LoadLittle64(p+184)==h.storage_epoch_policy.generation&&LoadUuid(p+192)==h.backup_transport_policy.uuid&&LoadLittle64(p+208)==h.backup_transport_policy.generation&&LoadUuid(p+216)==h.protection_policy.uuid&&LoadLittle64(p+232)==h.protection_policy.generation;
}
}

DateBytesResultV1 EncodeDateBackupTupleV1(const DateOwnedValueV1& v,
                                          const DateExecutionControlV1& c) noexcept {
  auto chk=ValidateDateValueViewV1(v.view(),true);if(!chk.ok())return FailBytes(chk.diagnostic.diagnostic_code,chk.diagnostic.detail);const u64 n=v.state==DateValueStateV1::sql_null?296:300;
  auto profile_pin=v.profile;
  if(n>c.maximum_allocation_bytes)return FailBytes("RESOURCE.BUDGET_EXCEEDED","backup_allocation");if(Cancelled(c))return FailBytes("PROCESS.CANCELLED","backup_cancelled");
  DateBytesResultV1 r;VectorClearGuard clear(&r.bytes);
  if(!Reserve(&r.bytes,n))return FailBytes("RESOURCE.BUDGET_EXCEEDED","backup_allocation");
  std::memcpy(r.bytes.data(),kBackupMagic,8);StoreLittle16(r.bytes.data()+8,1);StoreLittle16(r.bytes.data()+10,296);StoreLittle32(r.bytes.data()+12,n);PutBackupIdentity(r.bytes.data()+16,*v.profile);
  r.bytes[256]=v.state==DateValueStateV1::sql_null?0:1;r.bytes[257]=v.state==DateValueStateV1::sql_null?0:4;if(n==300)StoreLittle32(r.bytes.data()+296,static_cast<u32>(v.day));
  std::array<byte,32>d{};ArrayClearGuard<32> clear_digest(&d);if(!HashRecord(kBackupDomain,r.bytes,264,&d))return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","backup_hash_provider");std::memcpy(r.bytes.data()+264,d.data(),32);
  if(!DecodeDateBackupTupleNoAllocV1(*v.profile,true,r.bytes).ok())return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","backup_self_check");
  if(Cancelled(c))return FailBytes("PROCESS.CANCELLED","backup_prepublication");
  r.status=Ok();clear.Disarm();return r;
}

DateBackupTupleViewResultV1 DecodeDateBackupTupleNoAllocV1(
    const DateValidatedProfileHandleV1& h,bool null_allowed,std::span<const byte> e) noexcept {
  if((e.size()!=296&&e.size()!=300)||std::memcmp(e.data(),kBackupMagic,8)||LoadLittle16(e.data()+8)!=1||LoadLittle16(e.data()+10)!=296||LoadLittle32(e.data()+12)!=e.size()||LoadLittle32(e.data()+116)||e[258]||std::any_of(e.begin()+259,e.begin()+264,[](byte b){return b!=0;}))return FailView<DateBackupTupleViewResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","backup_structure");
  if(!HashEquals(kBackupDomain,e,264))return FailView<DateBackupTupleViewResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","backup_hash");
  if(!ValidProfile(h))return FailView<DateBackupTupleViewResultV1>("CTI.TEMPORAL.DESCRIPTOR_INVALID","profile");
  if(!BackupIdentityEquals(e.data()+16,h))return FailView<DateBackupTupleViewResultV1>("CTI.TEMPORAL.DESCRIPTOR_INVALID","backup_identity");
  DateValueStateV1 state=DateValueStateV1::value;i32 day=0;PlainObjectClearGuard<DateValueStateV1> clear_state(&state);PlainObjectClearGuard<i32> clear_day(&day);
  if(e[256]==0){if(e[257]!=0||e.size()!=296)return FailView<DateBackupTupleViewResultV1>("DATATYPE.NULL_STATE.INVALID","backup_null_pair");if(!null_allowed)return FailView<DateBackupTupleViewResultV1>("DATATYPE.NULL_NOT_ADMITTED");state=DateValueStateV1::sql_null;}
  else if(e[256]==1){if(e[257]!=4||e.size()!=300)return FailView<DateBackupTupleViewResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","backup_value_pair");state=DateValueStateV1::value;day=static_cast<i32>(LoadLittle32(e.data()+296));}
  else return FailView<DateBackupTupleViewResultV1>("DATATYPE.NULL_STATE.INVALID","backup_state");
  const auto component=state==DateValueStateV1::sql_null?std::span<const byte>{}:e.subspan(296,4);
  const auto decoded_component=DecodeCanonicalDateComponentNoAllocV1(
      h,state,null_allowed,component);
  if(!decoded_component.ok()||decoded_component.value.state!=state||
     decoded_component.value.day!=day)
    return FailView<DateBackupTupleViewResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","backup_component_decode");
  std::array<byte,300> canonical{};ArrayClearGuard<300> clear_canonical(&canonical);
  std::memcpy(canonical.data(),kBackupMagic,8);StoreLittle16(canonical.data()+8,1);
  StoreLittle16(canonical.data()+10,296);StoreLittle32(canonical.data()+12,static_cast<u32>(e.size()));
  PutBackupIdentity(canonical.data()+16,h);canonical[256]=state==DateValueStateV1::sql_null?0:1;
  canonical[257]=state==DateValueStateV1::sql_null?0:4;
  if(e.size()==300)StoreLittle32(canonical.data()+296,static_cast<u32>(day));
  std::array<byte,32> canonical_hash{};ArrayClearGuard<32> clear_hash(&canonical_hash);
  if(!HashRecord(kBackupDomain,std::span<const byte>(canonical.data(),e.size()),264,&canonical_hash))
    return FailView<DateBackupTupleViewResultV1>("RESOURCE.BUDGET_EXCEEDED","backup_reencode_hash");
  std::memcpy(canonical.data()+264,canonical_hash.data(),32);
  if(std::memcmp(e.data(),canonical.data(),e.size()))
    return FailView<DateBackupTupleViewResultV1>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","backup_reencode");
  DateBackupTupleViewResultV1 r;r.tuple={&h,state,day};r.status=Ok();return r;
}

DateIndexProjectionResultV1 ProjectDateIndexValueV1(
    const DateIndexProjectionRequestV1& q) noexcept {
  DateIndexResolutionV1 resolution;
  resolution.family=q.family;
  if(!q.value||!q.value->profile)return FailIndex(resolution,"CTI.TEMPORAL.DESCRIPTOR_INVALID","value_or_profile_missing");
  auto profile_checked=ValidateDateProfileHandleV1(*q.value->profile);
  if(!profile_checked.ok())return FailIndex(resolution,"CTI.TEMPORAL.DESCRIPTOR_INVALID","profile_invalid");
  if (!ValidFamily(q.family))
    return FailIndex(resolution,"CTI.TEMPORAL.INDEX_KEY_REFUSED", "unknown_family");
  resolution=ResolveDateIndexFamilyV1(q.family);
  if(resolution.disposition==DateIndexDispositionV1::not_applicable)return OwnerFact(resolution);
  DateIndexFamilyV1 effective_family=q.family;
  if(resolution.disposition==DateIndexDispositionV1::conditional){
    if(!q.selected_conditional_family)return OwnerFact(resolution);
    if(!ValidFamily(q.selected_conditional_family->family))return OwnerFact(resolution);
    auto expected=ResolveDateIndexFamilyV1(q.selected_conditional_family->family);
    if(!SameResolution(expected,*q.selected_conditional_family)||expected.disposition==DateIndexDispositionV1::conditional||expected.disposition==DateIndexDispositionV1::not_applicable)return OwnerFact(resolution);
    resolution=*q.selected_conditional_family;effective_family=resolution.family;
  }
  if(!ModeAllowed(effective_family,q.mode))return FailIndex(resolution,"CTI.TEMPORAL.INDEX_KEY_REFUSED","selected_mode_invalid");
  if(effective_family==DateIndexFamilyV1::temporary_work){
    switch(q.mode){
      case DateProjectionModeV1::equality_hash:resolution.projection=DateProjectionKindV1::equality_hash;resolution.exact_recheck_required=true;break;
      case DateProjectionModeV1::ascending_nulls_first:case DateProjectionModeV1::ascending_nulls_last:case DateProjectionModeV1::descending_nulls_first:case DateProjectionModeV1::descending_nulls_last:resolution.projection=DateProjectionKindV1::sort_key;resolution.exact_recheck_required=false;break;
      default:return FailIndex(resolution,"CTI.TEMPORAL.INDEX_KEY_REFUSED","temporary_mode");
    }
  }
  const auto checked=ValidateProjectionValueMetadata(*q.value,true);
  if(!checked.status.ok())return FailIndex(resolution,checked.diagnostic_code,checked.detail);
  if(resolution.projection==DateProjectionKindV1::range_pair){
    if(!q.upper_value)return FailIndex(resolution,"CTI.TEMPORAL.INDEX_KEY_REFUSED","upper_missing");
    const auto upper=ValidateProjectionValueMetadata(*q.upper_value,false,q.value->profile.get());
    if(!upper.status.ok())return FailIndex(resolution,upper.diagnostic_code,upper.detail);
    if(q.value->state!=DateValueStateV1::value)
      return FailIndex(resolution,"DATATYPE.NULL_NOT_ADMITTED","range_lower_null");
  }
  if(resolution.projection==DateProjectionKindV1::zone_map){
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
    case DateProjectionKindV1::equality_hash:need=32;break;
    case DateProjectionKindV1::sort_key:need=q.value->state==DateValueStateV1::sql_null?100:104;break;
    case DateProjectionKindV1::covering_value:need=q.value->state==DateValueStateV1::sql_null?1:5;break;
    case DateProjectionKindV1::range_pair:need=208;break;
    case DateProjectionKindV1::zone_map:need=q.zone_map->value_count?304:96;break;
    default:return OwnerFact(resolution);
  }
  auto operation_pin=q.value->profile;
  const u64 provider_limit=resolution.projection==DateProjectionKindV1::zone_map
      ?std::min(q.provider_max_key_bytes,q.zone_map->provider_max_key_bytes)
      :q.provider_max_key_bytes;
  if(provider_limit<need)return FailIndex(resolution,"CTI.TEMPORAL.INDEX_KEY_REFUSED","provider_budget");
  if(q.control.maximum_allocation_bytes<need)return FailIndex(resolution,"RESOURCE.BUDGET_EXCEEDED","allocation_grant");
  if(Cancelled(q.control))return FailIndex(resolution,"PROCESS.CANCELLED","before_allocation");
  DateBytesResultV1 b;VectorClearGuard clear_bytes(&b.bytes);
  if(resolution.projection==DateProjectionKindV1::zone_map){
    DateExecutionControlV1 inner=q.control;inner.cancelled=nullptr;inner.cancellation_context=nullptr;
    b=EncodeDateZoneMapImpl(*q.zone_map,provider_limit,inner,false);
  }else{
    if(!Reserve(&b.bytes,need))return FailIndex(resolution,"RESOURCE.BUDGET_EXCEEDED","allocation");
    switch(resolution.projection){
      case DateProjectionKindV1::equality_hash:{
        std::array<byte,32> hash{};ArrayClearGuard<32> clear_hash(&hash);
        if(!EncodeHashRaw(q.value->view(),&hash))return FailIndex(resolution,"RESOURCE.BUDGET_EXCEEDED","hash_provider");
        std::memcpy(b.bytes.data(),hash.data(),32);break;}
      case DateProjectionKindV1::sort_key:{
        DateSortDirectionV1 d=DateSortDirectionV1::ascending;DateNullModeV1 n=DateNullModeV1::nulls_last;
        if(q.mode==DateProjectionModeV1::ascending_nulls_first)n=DateNullModeV1::nulls_first;
        else if(q.mode==DateProjectionModeV1::descending_nulls_first){d=DateSortDirectionV1::descending;n=DateNullModeV1::nulls_first;}
        else if(q.mode==DateProjectionModeV1::descending_nulls_last)d=DateSortDirectionV1::descending;
        EncodeSortKeyRaw(q.value->view(),d,n,b.bytes.data());
        if(!DecodeDateSortKeyNoAllocV1(*q.value->profile,b.bytes).ok())return FailIndex(resolution,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","sort_key_self_check");break;}
      case DateProjectionKindV1::covering_value:
        b.bytes[0]=q.value->state==DateValueStateV1::sql_null?0:2;
        if(need==5)StoreLittle32(b.bytes.data()+1,static_cast<u32>(q.value->day));
        if(!DecodeDateCoveringValueNoAllocV1(*q.value->profile,true,b.bytes).ok())return FailIndex(resolution,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","covering_self_check");break;
      case DateProjectionKindV1::range_pair:
        if(q.value->day>q.upper_value->day)return FailIndex(resolution,"CTI.TEMPORAL.INDEX_KEY_REFUSED","range_order");
        EncodeSortKeyRaw(q.value->view(),DateSortDirectionV1::ascending,DateNullModeV1::nulls_last,b.bytes.data());
        EncodeSortKeyRaw(q.upper_value->view(),DateSortDirectionV1::ascending,DateNullModeV1::nulls_last,b.bytes.data()+104);
        if(!DecodeDateRangePairNoAllocV1(*q.value->profile,b.bytes).ok())return FailIndex(resolution,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","range_self_check");break;
      default:break;
    }
    b.status=Ok();
  }
  if(!b.ok()){DateIndexProjectionResultV1 r;r.status=b.status;r.diagnostic=std::move(b.diagnostic);r.resolution=resolution;return r;}
  if(Cancelled(q.control))return FailIndex(resolution,"PROCESS.CANCELLED","before_publication");
  DateIndexProjectionResultV1 r;r.status=Ok();r.resolution=resolution;r.bytes=std::move(b.bytes);clear_bytes.Disarm();return r;
}

DateProtectionResolutionV1 ResolveDateProtectionV1(DateProtectionCellV1 c) noexcept {
  using C=DateProtectionCellV1;
  switch(c){
    case C::canonical_plain:return {c,true,false,"admitted_datatype_component","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID"};
    case C::inner_compression:case C::inner_encryption:return {c,false,false,"forbidden","CTI.TEMPORAL.PROTECTION_UNSUPPORTED"};
    case C::protected_index:return {c,false,false,"forbidden_current_core","CTI.TEMPORAL.PROTECTION_UNSUPPORTED"};
    case C::outer_compression:case C::outer_authenticated_protection:case C::outer_compress_then_protect:case C::outer_protect_then_compress:return {c,false,true,"unsupported_current_core","receiving_outer_owner_typed_handoff_no_datatype_algorithm_or_order_claim"};
    case C::page_or_filespace_crypto:case C::transport_tls:return {c,false,true,"not_applicable_datatype_layer","owner_diagnostic_before_datatype"};
  }
  return {c,false,true,"not_applicable_datatype_layer","owner_diagnostic_before_datatype"};
}

DateWireLaneResolutionV1 ResolveDateWireLaneV1(DateWireLaneV1 l) noexcept {
  const bool native=static_cast<unsigned>(l)<=static_cast<unsigned>(DateWireLaneV1::canonical_sblr);
  return {l,false,native,true,native?"unsupported_current_outer_version_unallocated_datatype_component_mapping_complete":"unsupported_no_manifest_listed_compatibility_profile","CTI.TRANSPORT.UNSUPPORTED"};
}

std::span<const DateDiagnosticRouteV1> DateDiagnosticRoutesV1() noexcept {
  static constexpr std::array<DateDiagnosticRouteV1,42> rows{{
    {DateDiagnosticAxisV1::input_parse,"SBLR.OPERAND_INVALID",Uuid({0xf7,0xc7,0x36,0x87,0xc7,0x8d,0x54,0x6a,0xbc,0x93,0x02,0x8b,0x52,0xb4,0x35,0xcf}),"input_extent_u32;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::input_parse,"CTI.TEMPORAL.INVALID_LITERAL",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x02,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x02}),"input_extent_u32;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::input_parse,"CTI.TEMPORAL.ZERO_DATE_REFUSED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x0a,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0a}),"input_extent_u32;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::input_parse,"CTI.TEMPORAL.RANGE_EXCEEDED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x04,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x04}),"input_extent_u32;failure_offset_u32;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::descriptor_codec,"SECURITY.ACCESS_DENIED",Uuid({0x1a,0x8d,0x26,0xb0,0x87,0x55,0x56,0xad,0xa1,0x70,0xb2,0x4a,0x3a,0xaf,0x14,0x07}),"request_uuid;operation_uuid;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::descriptor_codec,"CTI.TEMPORAL.DESCRIPTOR_INVALID",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x01,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x01}),"descriptor_uuid;expected_generation_u64;actual_generation_u64;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::storage_read_write,"STORAGE.PAGE_CHECKSUM_FAILED",Uuid({0x0c,0x7d,0x9a,0xaf,0x92,0xe6,0x59,0x05,0x9d,0xaf,0x09,0x74,0xb8,0x71,0xf9,0x4a}),"boundary_enum;offset_u64;extent_u64;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::storage_read_write,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x03,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x03}),"boundary_enum;offset_u64;extent_u64;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::cast_invalid,"DATATYPE.CAST_FORBIDDEN",Uuid({0x0d,0xc5,0x1a,0x1c,0xad,0x21,0x57,0xb5,0xae,0x65,0xe0,0xfe,0x55,0x89,0x02,0x1a}),"source_type_uuid;target_type_uuid;cast_context_enum;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::cast_invalid,"CTI.TEMPORAL.INVALID_LITERAL",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x02,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x02}),"operation_uuid;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::cast_invalid,"CTI.TEMPORAL.ZERO_DATE_REFUSED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x0a,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0a}),"operation_uuid;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::cast_range_loss,"CTI.TEMPORAL.RANGE_EXCEEDED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x04,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x04}),"bound_enum;operation_uuid;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::cast_range_loss,"CTB.TEXT.LENGTH_EXCEEDED",Uuid({0xb3,0x99,0xb5,0x14,0xf7,0x71,0x50,0x14,0xa5,0xb4,0x90,0x77,0xf5,0xde,0xe3,0x69}),"required_extent_u64;available_extent_u64;operation_uuid;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::operation_invalid,"CTI.TEMPORAL.OPERATION_REFUSED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x05,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x05}),"operation_uuid;policy_uuid;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::operation_invalid,"CTI.INTERVAL.CALENDAR_OPERATION_REFUSED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x0c,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0c}),"operation_uuid;policy_uuid;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::operation_invalid,"CTI.TEMPORAL.ORDERING_REFUSED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x06,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x06}),"operation_uuid;policy_uuid;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::bounds_overflow_underflow,"CTI.TEMPORAL.RANGE_EXCEEDED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x04,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x04}),"bound_enum;operation_uuid;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::bounds_overflow_underflow,"RESOURCE.BUDGET_EXCEEDED",Uuid({0xde,0xdc,0x1c,0x9b,0x38,0x79,0x57,0x70,0x8f,0x8e,0xc0,0xc9,0x27,0xa0,0xc3,0x4d}),"operation_uuid;requested_extent_u64;granted_extent_u64;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::resource_cancellation,"RESOURCE.BUDGET_EXCEEDED",Uuid({0xde,0xdc,0x1c,0x9b,0x38,0x79,0x57,0x70,0x8f,0x8e,0xc0,0xc9,0x27,0xa0,0xc3,0x4d}),"operation_uuid;requested_extent_u64;granted_extent_u64;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::resource_cancellation,"PROCESS.CANCELLED",Uuid({0x0f,0x42,0x5d,0xe2,0x6b,0xc7,0x51,0xd4,0x8d,0x34,0x72,0xd1,0x07,0xad,0x45,0xe7}),"operation_uuid;checkpoint_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::compression_corruption,"SBLR.ENVELOPE.CHECKSUM_INVALID",Uuid({0x26,0x0f,0x5e,0xac,0xb2,0x86,0x5a,0x00,0x81,0x6c,0xc4,0xc2,0xa8,0xf1,0xa2,0x5e}),"boundary_enum;profile_uuid;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::compression_corruption,"CTI.TEMPORAL.PROTECTION_UNSUPPORTED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x09,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x09}),"boundary_enum;profile_uuid;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::encryption_auth_key,"SECURITY.ACCESS_DENIED",Uuid({0x1a,0x8d,0x26,0xb0,0x87,0x55,0x56,0xad,0xa1,0x70,0xb2,0x4a,0x3a,0xaf,0x14,0x07}),"request_uuid;operation_uuid;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::encryption_auth_key,"CTI.TEMPORAL.PROTECTION_UNSUPPORTED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x09,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x09}),"layer_enum;profile_uuid_or_zero;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::wire_decode_encode,"SBLR.OPERAND_INVALID",Uuid({0xf7,0xc7,0x36,0x87,0xc7,0x8d,0x54,0x6a,0xbc,0x93,0x02,0x8b,0x52,0xb4,0x35,0xcf}),"lane_group_enum;offset_u64;extent_u64;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::wire_decode_encode,"SBLR.ENVELOPE.CHECKSUM_INVALID",Uuid({0x26,0x0f,0x5e,0xac,0xb2,0x86,0x5a,0x00,0x81,0x6c,0xc4,0xc2,0xa8,0xf1,0xa2,0x5e}),"lane_group_enum;offset_u64;extent_u64;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::wire_decode_encode,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x03,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x03}),"lane_group_enum;offset_u64;extent_u64;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::wire_decode_encode,"CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x08,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x08}),"lane_group_enum;offset_u64;extent_u64;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::wire_decode_encode,"CTI.TRANSPORT.UNSUPPORTED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x0d,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0d}),"lane_group_enum;offset_u64;extent_u64;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::index_key,"CTI.TEMPORAL.ORDERING_REFUSED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x06,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x06}),"descriptor_uuid;index_family_enum;required_extent_u64;available_extent_u64;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::index_key,"OPTIMIZER.INDEX_COMPATIBILITY_MISSING",Uuid({0x01,0xa1,0x0c,0x00,0x20,0x00,0x70,0x0d,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0d}),"descriptor_uuid;index_family_enum;operation_uuid","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::index_key,"CTI.TEMPORAL.INDEX_KEY_REFUSED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x07,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x07}),"descriptor_uuid;index_family_enum;required_extent_u64;available_extent_u64;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::index_key,"RESOURCE.BUDGET_EXCEEDED",Uuid({0xde,0xdc,0x1c,0x9b,0x38,0x79,0x57,0x70,0x8f,0x8e,0xc0,0xc9,0x27,0xa0,0xc3,0x4d}),"operation_uuid;requested_extent_u64;granted_extent_u64;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::domain_validation,"DATATYPE.NULL_STATE.INVALID",Uuid({0x58,0x8f,0xaf,0xd0,0x63,0x86,0x55,0xcf,0x84,0x4e,0xf5,0xb0,0xdc,0x22,0xc7,0x00}),"descriptor_uuid;domain_uuid;constraint_uuid;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::domain_validation,"DATATYPE.NULL_NOT_ADMITTED",Uuid({0xab,0xa0,0x82,0x8d,0x77,0xc9,0x50,0xde,0x98,0x42,0x7f,0x20,0x47,0x10,0x02,0x14}),"descriptor_uuid;domain_uuid;constraint_uuid;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::domain_validation,"DOMAIN.CONSTRAINT_FAILED",Uuid({0x62,0xbe,0x50,0x52,0xad,0xbc,0x59,0xaa,0xb8,0x4b,0xae,0x60,0xa8,0xc4,0x0c,0x0f}),"descriptor_uuid;domain_uuid;constraint_uuid;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::recovery_corruption,"STORAGE.PAGE_CHECKSUM_FAILED",Uuid({0x0c,0x7d,0x9a,0xaf,0x92,0xe6,0x59,0x05,0x9d,0xaf,0x09,0x74,0xb8,0x71,0xf9,0x4a}),"object_uuid;offset_u64;receipt_uuid;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::recovery_corruption,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x03,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x03}),"object_uuid;offset_u64;receipt_uuid;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::recovery_corruption,"CTI.MERGE.MANUAL_REVIEW_REQUIRED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x0e,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0e}),"object_uuid;offset_u64;receipt_uuid;reason_enum","no_value_component_text_key_hash_preimage_secret_or_address"},
    {DateDiagnosticAxisV1::statistics_read,"SECURITY.ACCESS_DENIED",Uuid({0x1a,0x8d,0x26,0xb0,0x87,0x55,0x56,0xad,0xa1,0x70,0xb2,0x4a,0x3a,0xaf,0x14,0x07}),"request_uuid;operation_uuid;reason_enum","no_payload_no_rendered_date_no_key_material_no_secret_no_memory_address"},
    {DateDiagnosticAxisV1::statistics_read,"OPTIMIZER.STATISTICS_PRIVACY_DENIED",Uuid({0x01,0xa1,0x0c,0x00,0x20,0x00,0x70,0x0c,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0c}),"statistics_uuid;privacy_policy_uuid;reason_enum","no_payload_no_rendered_date_no_key_material_no_secret_no_memory_address"},
    {DateDiagnosticAxisV1::statistics_read,"OPTIMIZER.STATISTICS_EPOCH_MISMATCH",Uuid({0x01,0xa1,0x0c,0x00,0x20,0x00,0x70,0x0b,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0b}),"statistics_uuid;epoch_class_enum;expected_epoch_u64;actual_epoch_u64","no_payload_no_rendered_date_no_key_material_no_secret_no_memory_address"},
  }}; return rows;
}

std::span<const DateMetricEvidenceTypeDescriptorV1> DateMetricEvidenceTypesV1() noexcept {
  static constexpr std::array<DateMetricEvidenceTypeDescriptorV1,19> rows{{
    {DateMetricEvidenceTypeV1::descriptor_admissions,Uuid({0x01,0xa1,0x00,0x8e,0xe1,0x00,0x70,0x01,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x01}),1,"sb_date_descriptor_admissions_total","date_descriptor","admission_committed_final"},
    {DateMetricEvidenceTypeV1::invalid_literals,Uuid({0x01,0xa1,0x00,0x8e,0xe1,0x00,0x70,0x02,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x02}),1,"sb_date_invalid_literals_total","date_cast","refusal_committed_final"},
    {DateMetricEvidenceTypeV1::range_refusals,Uuid({0x01,0xa1,0x00,0x8e,0xe1,0x00,0x70,0x03,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x03}),1,"sb_date_range_refusals_total","date_runtime","refusal_committed_final"},
    {DateMetricEvidenceTypeV1::operation_attempts,Uuid({0x01,0xa1,0x00,0x8e,0xe1,0x00,0x70,0x04,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x04}),1,"sb_date_operation_attempts_total","date_runtime","attempt_admitted_final"},
    {DateMetricEvidenceTypeV1::operation_success,Uuid({0x01,0xa1,0x00,0x8e,0xe1,0x00,0x70,0x05,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x05}),1,"sb_date_operation_success_total","date_runtime","result_published_final"},
    {DateMetricEvidenceTypeV1::operation_refusals,Uuid({0x01,0xa1,0x00,0x8e,0xe1,0x00,0x70,0x06,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x06}),1,"sb_date_operation_refusals_total","date_runtime","refusal_committed_final"},
    {DateMetricEvidenceTypeV1::cast_attempts,Uuid({0x01,0xa1,0x00,0x8e,0xe1,0x00,0x70,0x07,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x07}),1,"sb_date_cast_attempts_total","date_cast","admission_committed_final"},
    {DateMetricEvidenceTypeV1::cast_success,Uuid({0x01,0xa1,0x00,0x8e,0xe1,0x00,0x70,0x08,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x08}),1,"sb_date_cast_success_total","date_cast","result_published_final"},
    {DateMetricEvidenceTypeV1::index_admission_refusals,Uuid({0x01,0xa1,0x00,0x8e,0xe1,0x00,0x70,0x09,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x09}),1,"sb_date_index_admission_refusals_total","date_projection","refusal_committed_final"},
    {DateMetricEvidenceTypeV1::statistics_stale,Uuid({0x01,0xa1,0x00,0x8e,0xe1,0x00,0x70,0x0a,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0a}),1,"sb_date_statistics_stale_total","date_statistics","typed_fact_committed_final"},
    {DateMetricEvidenceTypeV1::reference_mapping_misses,Uuid({0x01,0xa1,0x00,0x8e,0xe1,0x00,0x70,0x0b,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0b}),1,"sb_date_reference_mapping_misses_total","date_boundary","typed_fact_committed_final"},
    {DateMetricEvidenceTypeV1::transport_refusals,Uuid({0x01,0xa1,0x00,0x8e,0xe1,0x00,0x70,0x0c,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0c}),1,"sb_date_transport_refusals_total","transport_owner","refusal_committed_final"},
    {DateMetricEvidenceTypeV1::serialization_refusals,Uuid({0x01,0xa1,0x00,0x8e,0xe1,0x00,0x70,0x0d,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0d}),1,"sb_date_serialization_refusals_total","date_boundary","refusal_committed_final"},
    {DateMetricEvidenceTypeV1::protection_refusals,Uuid({0x01,0xa1,0x00,0x8e,0xe1,0x00,0x70,0x0e,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0e}),1,"sb_date_protection_refusals_total","date_protection","refusal_committed_final"},
    {DateMetricEvidenceTypeV1::merge_manual_review,Uuid({0x01,0xa1,0x00,0x8e,0xe1,0x00,0x70,0x0f,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0f}),1,"sb_date_merge_manual_review_total","merge_owner","refusal_committed_final"},
    {DateMetricEvidenceTypeV1::resource_budget_refusals,Uuid({0x01,0xa1,0x00,0x8e,0xe1,0x00,0x70,0x10,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x10}),1,"sb_date_resource_budget_refusals_total","resource_owner","refusal_committed_final"},
    {DateMetricEvidenceTypeV1::cancellations,Uuid({0x01,0xa1,0x00,0x8e,0xe1,0x00,0x70,0x11,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x11}),1,"sb_date_cancellations_total","process_owner","refusal_committed_final"},
    {DateMetricEvidenceTypeV1::decode_corruption,Uuid({0x01,0xa1,0x00,0x8e,0xe1,0x00,0x70,0x12,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x12}),1,"sb_date_decode_corruption_total","date_boundary","refusal_committed_final"},
    {DateMetricEvidenceTypeV1::diagnostic_family,Uuid({0x01,0xa1,0x00,0x8e,0xe1,0x00,0x70,0x13,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x13}),1,"sb_date_diagnostic_family_total","metrics_diagnostic_router","diagnostic_aggregate_committed_final"},
  }}; return rows;
}

} // namespace scratchbird::core::datatypes
