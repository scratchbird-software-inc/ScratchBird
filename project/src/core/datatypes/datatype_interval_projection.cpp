// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "datatype_interval_projection.hpp"

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
constexpr byte kStatisticsMagic[8] = {'S','B','I','N','T','S','0','1'};
constexpr byte kBackupMagic[8] = {'S','B','I','N','B','K','0','1'};
constexpr char kStatisticsDomain[] = "ScratchBird.BaseInterval.StatisticsRecords.V1";
constexpr char kBackupDomain[] = "ScratchBird.BaseInterval.BackupTuple.V1";
constexpr u32 kEqualityProjectionBytes = 160;
constexpr u32 kBackupNullBytes = 360;
constexpr u32 kBackupValueBytes = 376;

constexpr platform::Uuid Uuid(std::array<byte,16> b) noexcept { return {b}; }

Status Ok() noexcept { return {StatusCode::ok, Severity::info, Subsystem::datatypes}; }
Status Error() noexcept { return {StatusCode::platform_required_feature_missing,
                                  Severity::error, Subsystem::datatypes}; }
Status BudgetError() noexcept { return {StatusCode::memory_allocation_failed,
                                        Severity::error, Subsystem::datatypes}; }
bool Cancelled(const IntervalExecutionControlV3& c) noexcept {
  return c.cancelled != nullptr && c.cancelled(c.cancellation_context);
}
void StoreUuid(byte* p, const platform::Uuid& u) noexcept {
  std::memcpy(p, u.bytes.data(), 16);
}
platform::Uuid LoadUuid(const byte* p) noexcept {
  platform::Uuid u{};
  std::memcpy(u.bytes.data(), p, 16);
  return u;
}
void StorePolicy(byte* p, const DatatypePolicyIdentityV3& policy) noexcept {
  StoreUuid(p, policy.uuid);
  StoreLittle64(p + 16, policy.generation);
}
bool PolicyEquals(const byte* p, const DatatypePolicyIdentityV3& policy) noexcept {
  return LoadUuid(p) == policy.uuid && LoadLittle64(p + 16) == policy.generation;
}
void PutCommonIdentity(byte* p, const IntervalValidatedProfileHandleV3& h) noexcept {
  const auto& i = h.identity.legacy_fields;
  StoreUuid(p + 0, h.receipt.catalog_snapshot_uuid);
  StoreLittle64(p + 16, h.receipt.catalog_generation);
  StoreLittle64(p + 24, h.receipt.registry_generation);
  StoreUuid(p + 32, i.descriptor_uuid);
  StoreLittle64(p + 48, i.descriptor_generation);
  StoreUuid(p + 56, i.type_uuid);
  StoreLittle64(p + 72, i.type_generation);
  StoreUuid(p + 80, i.codec_uuid);
  StoreLittle32(p + 96, i.codec_version);
  StoreLittle32(p + 100, 0);
  StoreLittle64(p + 104, i.codec_generation);
}
bool CommonIdentityEquals(const byte* p,
                          const IntervalValidatedProfileHandleV3& h) noexcept {
  const auto& i = h.identity.legacy_fields;
  return LoadUuid(p) == h.receipt.catalog_snapshot_uuid &&
      LoadLittle64(p + 16) == h.receipt.catalog_generation &&
      LoadLittle64(p + 24) == h.receipt.registry_generation &&
      LoadUuid(p + 32) == i.descriptor_uuid &&
      LoadLittle64(p + 48) == i.descriptor_generation &&
      LoadUuid(p + 56) == i.type_uuid &&
      LoadLittle64(p + 72) == i.type_generation &&
      LoadUuid(p + 80) == i.codec_uuid &&
      LoadLittle32(p + 96) == i.codec_version &&
      LoadLittle32(p + 100) == 0 &&
      LoadLittle64(p + 104) == i.codec_generation;
}
bool ValidProfile(const IntervalValidatedProfileHandleV3& h,
                  const IntervalExecutionControlV3* c = nullptr) noexcept {
  if (c == nullptr) return ValidateIntervalProfileHandleV3(h).ok();
  auto nested = *c;
  nested.cancelled = nullptr;
  nested.cancellation_context = nullptr;
  return ValidateIntervalProfileHandleV3(h, nested).ok();
}
bool SameProfile(const IntervalValidatedProfileHandleV3& a,
                 const IntervalValidatedProfileHandleV3& b) noexcept {
  return a.profile_material == b.profile_material &&
      a.equality_material == b.equality_material &&
      a.profile_fingerprint == b.profile_fingerprint &&
      a.equality_fingerprint == b.equality_fingerprint;
}

void SecureClear(void* pointer, std::size_t bytes) noexcept {
  auto* output = static_cast<volatile byte*>(pointer);
  while (bytes != 0) { *output++ = 0; --bytes; }
}
class VectorClearGuard final {
 public:
  VectorClearGuard(std::vector<byte>* value, IntervalScrubClassV3 scrub,
                   const IntervalExecutionControlV3* control) noexcept
      : value_(value), scrub_(scrub), control_(control) {}
  ~VectorClearGuard() {
    if (!active_ || value_ == nullptr || value_->empty()) return;
    SecureClear(value_->data(), value_->size());
    if (control_ != nullptr && control_->observe_scrubbed != nullptr)
      control_->observe_scrubbed(control_->scrub_observer_context, scrub_,
                                 value_->data(), value_->size());
    value_->clear();
  }
  void Disarm() noexcept { active_ = false; }
 private:
  std::vector<byte>* value_;
  IntervalScrubClassV3 scrub_;
  const IntervalExecutionControlV3* control_;
  bool active_ = true;
};
template <std::size_t N>
class ArrayClearGuard final {
 public:
  ArrayClearGuard(std::array<byte,N>* value, IntervalScrubClassV3 scrub,
                  const IntervalExecutionControlV3* control) noexcept
      : value_(value), scrub_(scrub), control_(control) {}
  ~ArrayClearGuard() {
    if (value_ == nullptr) return;
    SecureClear(value_->data(), value_->size());
    if (control_ != nullptr && control_->observe_scrubbed != nullptr)
      control_->observe_scrubbed(control_->scrub_observer_context, scrub_,
                                 value_->data(), value_->size());
  }
 private:
  std::array<byte,N>* value_;
  IntervalScrubClassV3 scrub_;
  const IntervalExecutionControlV3* control_;
};

struct HashSegment { const byte* data = nullptr; std::size_t size = 0; };
bool Sha256Stack(std::span<const HashSegment> segments,
                 std::array<byte,32>* out,
                 const IntervalExecutionControlV3* control = nullptr) noexcept {
  if (out == nullptr) return false;
  std::array<u32,8> state{{0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                           0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u}};
  std::array<byte,128> tail{};
  constexpr std::array<u32,64> constants{{
      0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
      0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
      0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
      0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
      0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
      0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
      0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
      0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u}};
  const auto rotate=[](u32 value,unsigned bits) noexcept {
    return static_cast<u32>((value>>bits)|(value<<(32-bits)));
  };
  const auto transform=[&](const byte* block) noexcept {
    std::array<u32,64> words{};
    for(unsigned i=0;i<16;++i)
      words[i]=(static_cast<u32>(block[i*4])<<24)|
          (static_cast<u32>(block[i*4+1])<<16)|
          (static_cast<u32>(block[i*4+2])<<8)|block[i*4+3];
    for(unsigned i=16;i<64;++i){
      const u32 s0=rotate(words[i-15],7)^rotate(words[i-15],18)^(words[i-15]>>3);
      const u32 s1=rotate(words[i-2],17)^rotate(words[i-2],19)^(words[i-2]>>10);
      words[i]=words[i-16]+s0+words[i-7]+s1;
    }
    u32 a=state[0],b=state[1],c=state[2],d=state[3];
    u32 e=state[4],f=state[5],g=state[6],h=state[7];
    for(unsigned i=0;i<64;++i){
      const u32 s1=rotate(e,6)^rotate(e,11)^rotate(e,25);
      const u32 choose=(e&f)^((~e)&g);
      const u32 first=h+s1+choose+constants[i]+words[i];
      const u32 s0=rotate(a,2)^rotate(a,13)^rotate(a,22);
      const u32 majority=(a&b)^(a&c)^(b&c);
      const u32 second=s0+majority;
      h=g;g=f;f=e;e=d+first;d=c;c=b;b=a;a=first+second;
    }
    state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;
    state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
    SecureClear(words.data(), words.size()*sizeof(u32));
    if(control!=nullptr&&control->observe_scrubbed!=nullptr)
      control->observe_scrubbed(control->scrub_observer_context,
          IntervalScrubClassV3::sha_schedule,
          reinterpret_cast<const byte*>(words.data()),words.size()*sizeof(u32));
  };
  u64 total=0; std::size_t buffered=0;
  for(const auto& segment:segments){
    if(segment.size!=0&&segment.data==nullptr)return false;
    constexpr u64 limit=std::numeric_limits<u64>::max()/8;
    if(segment.size>limit-total)return false;
    total+=static_cast<u64>(segment.size);
    const byte* input=segment.data;std::size_t remaining=segment.size;
    if(buffered!=0){
      const std::size_t take=std::min<std::size_t>(64-buffered,remaining);
      if(take!=0){std::memcpy(tail.data()+buffered,input,take);input+=take;}
      buffered+=take;remaining-=take;
      if(buffered==64){transform(tail.data());SecureClear(tail.data(),64);buffered=0;}
    }
    while(remaining>=64){transform(input);input+=64;remaining-=64;}
    if(remaining!=0){std::memcpy(tail.data(),input,remaining);buffered=remaining;}
  }
  tail[buffered]=0x80;const std::size_t padded=buffered<56?64:128;
  const u64 bits=total*8;
  for(unsigned i=0;i<8;++i)tail[padded-1-i]=static_cast<byte>(bits>>(i*8));
  transform(tail.data());if(padded==128)transform(tail.data()+64);
  for(unsigned i=0;i<8;++i){
    (*out)[i*4]=static_cast<byte>(state[i]>>24);
    (*out)[i*4+1]=static_cast<byte>(state[i]>>16);
    (*out)[i*4+2]=static_cast<byte>(state[i]>>8);
    (*out)[i*4+3]=static_cast<byte>(state[i]);
  }
  SecureClear(state.data(),state.size()*sizeof(u32));
  SecureClear(tail.data(),tail.size());
  if(control!=nullptr&&control->observe_scrubbed!=nullptr){
    control->observe_scrubbed(control->scrub_observer_context,
        IntervalScrubClassV3::sha_state,reinterpret_cast<const byte*>(state.data()),state.size()*sizeof(u32));
    control->observe_scrubbed(control->scrub_observer_context,
        IntervalScrubClassV3::sha_tail,tail.data(),tail.size());
  }
  return true;
}
bool HashRecord(std::string_view domain, std::span<const byte> record,
                std::size_t zero_offset, bool nul_after_domain,
                std::array<byte,32>* out,
                const IntervalExecutionControlV3* control = nullptr) noexcept {
  if(out==nullptr||zero_offset+32>record.size())return false;
  const byte zero=0;
  const HashSegment parts_with_nul[]={{reinterpret_cast<const byte*>(domain.data()),domain.size()},{&zero,1},{record.data(),zero_offset},{kZero32,32},{record.data()+zero_offset+32,record.size()-zero_offset-32}};
  const HashSegment parts[]={{reinterpret_cast<const byte*>(domain.data()),domain.size()},{record.data(),zero_offset},{kZero32,32},{record.data()+zero_offset+32,record.size()-zero_offset-32}};
  return nul_after_domain?Sha256Stack(parts_with_nul,out,control):Sha256Stack(parts,out,control);
}
bool ConstantTimeEqual(const byte* a,const byte* b,std::size_t n) noexcept {
  byte difference=0;for(std::size_t i=0;i<n;++i)difference|=static_cast<byte>(a[i]^b[i]);return difference==0;
}
bool Reserve(std::vector<byte>* out,u64 n) noexcept {
  if(out==nullptr||n>std::numeric_limits<std::size_t>::max())return false;
  try{out->assign(static_cast<std::size_t>(n),0);return true;}catch(...){return false;}
}

IntervalBytesResultV3 FailBytes(std::string_view code,std::string_view detail={}) noexcept {
  IntervalBytesResultV3 r;r.status=code=="RESOURCE.BUDGET_EXCEEDED"?BudgetError():Error();r.diagnostic={r.status,code,detail};return r;
}
template<class R> R FailView(std::string_view code,std::string_view detail={}) noexcept {
  R r;r.status=code=="RESOURCE.BUDGET_EXCEEDED"?BudgetError():Error();r.diagnostic={r.status,code,detail};return r;
}
IntervalIndexProjectionResultV3 FailIndex(const IntervalIndexResolutionV3& requested,
    const IntervalIndexResolutionV3& effective,std::string_view code,
    std::string_view detail={},bool owner=false) noexcept {
  IntervalIndexProjectionResultV3 r;r.requested_resolution=requested;r.resolution=effective;
  r.status=code=="RESOURCE.BUDGET_EXCEEDED"?BudgetError():Error();r.diagnostic={r.status,code,detail};r.owner_fact_only=owner;return r;
}
bool ValidFamily(IntervalIndexFamilyV3 f) noexcept {return static_cast<unsigned>(f)<16;}
bool SameResolution(const IntervalIndexResolutionV3& a,const IntervalIndexResolutionV3& b) noexcept {
 return a.family==b.family&&a.compatibility_uuid==b.compatibility_uuid&&a.compatibility_generation==b.compatibility_generation&&a.disposition==b.disposition&&a.projection==b.projection&&a.exact_state_component_recheck_required==b.exact_state_component_recheck_required&&a.receiving_owner_resolution_required==b.receiving_owner_resolution_required;
}
IntervalDiagnosticFactV3 ValidateValue(const IntervalOwnedValueV3& v,
    const IntervalExecutionControlV3* control=nullptr) noexcept {
  if(!v.profile||!ValidProfile(*v.profile,control))return {Error(),"CTI.INTERVAL.DESCRIPTOR_INVALID","projection_profile"};
  if(v.state!=IntervalValueStateV3::value&&v.state!=IntervalValueStateV3::sql_null)return {Error(),"DATATYPE.NULL_STATE.INVALID","projection_state"};
  if(v.state==IntervalValueStateV3::sql_null&&(v.months!=0||v.civil_days!=0||v.fixed_nanoseconds!=0))return {Error(),"DATATYPE.NULL_STATE.INVALID","projection_dirty_null"};
  return {Ok(),{}, {}};
}
void EncodeComponentRaw(const IntervalOwnedValueV3& v,byte* p) noexcept {
  StoreLittle32(p,static_cast<u32>(v.months));StoreLittle32(p+4,static_cast<u32>(v.civil_days));StoreLittle64(p+8,static_cast<u64>(v.fixed_nanoseconds));
}
void EncodeComponentRaw(const IntervalValueViewV3& v,byte* p) noexcept {
  StoreLittle32(p,static_cast<u32>(v.months));StoreLittle32(p+4,static_cast<u32>(v.civil_days));StoreLittle64(p+8,static_cast<u64>(v.fixed_nanoseconds));
}

bool CanonicalMcv(std::span<const IntervalStatisticsMcvRecordV3> rows,u64 value_count) noexcept {
  for(std::size_t i=0;i<rows.size();++i){
    const auto& row=rows[i];
    if(row.frequency>value_count||row.rank!=i+1||row.flags!=1)return false;
    for(std::size_t j=0;j<i;++j)if(row.value_hash==rows[j].value_hash)return false;
    if(i!=0){const auto& previous=rows[i-1];if(previous.frequency<row.frequency)return false;if(previous.frequency==row.frequency&&!std::lexicographical_compare(previous.value_hash.begin(),previous.value_hash.end(),row.value_hash.begin(),row.value_hash.end()))return false;}
  }
  return true;
}
bool CountsValid(u64 rows,u64 nulls,u64 values,u64 distinct) noexcept {
  return nulls<=std::numeric_limits<u64>::max()-values&&rows==nulls+values&&distinct<=values;
}

}  // namespace

IntervalIndexResolutionV3 ResolveIntervalIndexFamilyV3(IntervalIndexFamilyV3 family) noexcept {
  using D=IntervalIndexDispositionV3;using P=IntervalProjectionKindV3;
  static constexpr std::array<platform::Uuid,16> ids{{
    Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xba}),
    Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xbb}),
    Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xbc}),
    Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xbd}),
    Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xbe}),
    Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xbf}),
    Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xc0}),
    Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xc1}),
    Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xc2}),
    Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xc3}),
    Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xc4}),
    Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xc5}),
    Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xc6}),
    Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xc7}),
    Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xc8}),
    Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xc9})}};
  IntervalIndexResolutionV3 r;r.family=family;
  if(!ValidFamily(family)){r.receiving_owner_resolution_required=true;return r;}
  r.compatibility_uuid=ids[static_cast<std::size_t>(family)];r.compatibility_generation=1;
  switch(family){
    case IntervalIndexFamilyV3::bitmap:case IntervalIndexFamilyV3::hash:r.disposition=D::admitted_with_recheck;r.projection=P::equality_hash;r.exact_state_component_recheck_required=true;break;
    case IntervalIndexFamilyV3::covering_included:r.disposition=D::admitted_payload_only;r.projection=P::covering_value;break;
    case IntervalIndexFamilyV3::expression:r.disposition=D::conditional;r.projection=P::conditional_result;r.exact_state_component_recheck_required=true;r.receiving_owner_resolution_required=true;break;
    case IntervalIndexFamilyV3::partial_filtered:r.disposition=D::conditional;r.projection=P::conditional_underlying;r.exact_state_component_recheck_required=true;r.receiving_owner_resolution_required=true;break;
    case IntervalIndexFamilyV3::temporary_work:r.disposition=D::conditional;r.projection=P::conditional_result;r.exact_state_component_recheck_required=true;r.receiving_owner_resolution_required=true;break;
    default:r.disposition=D::not_applicable;r.projection=P::none;r.receiving_owner_resolution_required=true;break;
  }
  return r;
}
IntervalIndexCompatibilityIdentityV3 LookupIntervalIndexCompatibilityIdentityV3(IntervalIndexFamilyV3 family) noexcept {const auto r=ResolveIntervalIndexFamilyV3(family);return {family,r.compatibility_uuid,r.compatibility_generation};}
IntervalIndexAdmissionResultV3 AdmitIntervalIndexCompatibilityV3(const IntervalIndexCompatibilityIdentityV3& supplied) noexcept {
  IntervalIndexAdmissionResultV3 r;r.resolution.family=supplied.family;if(!ValidFamily(supplied.family))return r;const auto expected=ResolveIntervalIndexFamilyV3(supplied.family);if(supplied.compatibility_uuid!=expected.compatibility_uuid||supplied.compatibility_generation!=1)return r;r.admitted=true;r.resolution=expected;r.diagnostic={};return r;
}

IntervalIndexPredicateResolutionV3 ResolveIntervalIndexPredicateV3(const IntervalIndexPredicateRequestV3& q) noexcept {
  using F=IntervalIndexFamilyV3;using O=IntervalIndexPredicateOperationV3;using P=IntervalIndexPredicateFactV3;
  IntervalIndexPredicateResolutionV3 r;const auto requested=AdmitIntervalIndexCompatibilityV3(q.requested);r.requested_resolution=requested.resolution;if(!requested.admitted){r.diagnostic="OPTIMIZER.INDEX_COMPATIBILITY_MISSING";return r;}auto effective=requested.resolution;
  const auto op=q.operation;
  if(op==O::range||op==O::order||op==O::prune_range||op==O::overlap||op==O::exclusion){r.effective_resolution=effective;r.diagnostic="CTI.TEMPORAL.ORDERING_REFUSED";return r;}
  if(op!=O::equality&&op!=O::in&&op!=O::is_null&&op!=O::include_payload){r.effective_resolution=effective;r.diagnostic="OPTIMIZER.INDEX_COMPATIBILITY_MISSING";return r;}
  if(effective.disposition==IntervalIndexDispositionV3::conditional){
    r.predicate_owner_handoff_preserved=effective.family==F::partial_filtered;
    if(q.selected_conditional==nullptr){r.effective_resolution=effective;return r;}
    const auto selected=AdmitIntervalIndexCompatibilityV3(*q.selected_conditional);
    const bool expression_ok=effective.family!=F::expression||(selected.resolution.family==F::bitmap||selected.resolution.family==F::hash);
    if(!selected.admitted||selected.resolution.disposition==IntervalIndexDispositionV3::conditional||selected.resolution.disposition==IntervalIndexDispositionV3::not_applicable||!expression_ok){r.diagnostic="OPTIMIZER.INDEX_COMPATIBILITY_MISSING";return r;}
    const bool partial=effective.family==F::partial_filtered;
    effective=selected.resolution;
    if(partial){r.effective_resolution=effective;r.admitted=true;r.fact=P::prune_only_mandatory_source_row_predicate_recheck_never_final_match;return r;}
  }
  r.effective_resolution=effective;
  auto admit=[&](P p){r.admitted=true;r.fact=p;r.diagnostic={};return r;};
  if((effective.family==F::bitmap||effective.family==F::hash)&&(op==O::equality||op==O::in||op==O::is_null))return admit(P::requires_exact_state_component_recheck_never_final_match);
  if(effective.family==F::covering_included&&op==O::include_payload)return admit(P::exact_no_recheck_after_decode_reencode);
  r.diagnostic="OPTIMIZER.INDEX_COMPATIBILITY_MISSING";return r;
}
IntervalIndexCandidateFactsV3 ClassifyIntervalIndexCandidateV3(IntervalIndexPredicateFactV3 fact,bool candidate,bool exact,bool source) noexcept {
  using D=IntervalIndexCandidateDispositionV3;using F=IntervalIndexPredicateFactV3;IntervalIndexCandidateFactsV3 r;if(!candidate||fact==F::refused)return r;if(fact==F::exact_no_recheck_after_decode_reencode){r.disposition=D::exact_match;r.final_match=true;return r;}if(fact==F::requires_exact_state_component_recheck_never_final_match){r.exact_state_component_recheck_required=true;r.final_match=exact;r.disposition=exact?D::exact_match:D::exact_state_component_recheck_required;return r;}if(fact==F::prune_only_mandatory_source_row_predicate_recheck_never_final_match){r.source_row_predicate_recheck_required=true;r.final_match=source;r.disposition=source?D::exact_match:D::source_row_predicate_recheck_required;}return r;
}

IntervalBytesResultV3 EncodeIntervalCoveringValueV3(const IntervalOwnedValueV3& input,const IntervalExecutionControlV3& c) noexcept {
  const auto value=input;const auto checked=ValidateValue(value,&c);if(!checked.status.ok())return FailBytes(checked.diagnostic_code,checked.detail);const u64 extent=value.state==IntervalValueStateV3::sql_null?1:17;if(extent>c.maximum_allocation_bytes)return FailBytes("RESOURCE.BUDGET_EXCEEDED","covering_allocation");if(Cancelled(c))return FailBytes("PROCESS.CANCELLED","covering_cancelled");IntervalBytesResultV3 r;VectorClearGuard clear(&r.bytes,IntervalScrubClassV3::projection_owned_buffer,&c);if(!Reserve(&r.bytes,extent))return FailBytes("RESOURCE.BUDGET_EXCEEDED","covering_allocation");r.bytes[0]=value.state==IntervalValueStateV3::sql_null?1:0;if(extent==17)EncodeComponentRaw(value,r.bytes.data()+1);auto nested=c;nested.cancelled=nullptr;nested.cancellation_context=nullptr;if(!DecodeIntervalCoveringValueNoAllocV3(*value.profile,true,r.bytes,nested).ok())return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","covering_self_check");if(Cancelled(c))return FailBytes("PROCESS.CANCELLED","covering_prepublication");r.status=Ok();clear.Disarm();return r;
}
IntervalCoveringValueViewResultV3 DecodeIntervalCoveringValueNoAllocV3(const IntervalValidatedProfileHandleV3& h,bool null_allowed,std::span<const byte> e,const IntervalExecutionControlV3& c) noexcept {
  if(!ValidProfile(h,&c))return FailView<IntervalCoveringValueViewResultV3>("CTI.INTERVAL.DESCRIPTOR_INVALID","profile");
  if(e.empty())return FailView<IntervalCoveringValueViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","minimum_extent");
  IntervalValueStateV3 state;std::int32_t months=0,days=0;std::int64_t ns=0;
  if(e[0]==1){if(e.size()!=1)return FailView<IntervalCoveringValueViewResultV3>("DATATYPE.NULL_STATE.INVALID","null_extent");if(!null_allowed)return FailView<IntervalCoveringValueViewResultV3>("DATATYPE.NULL_NOT_ADMITTED");state=IntervalValueStateV3::sql_null;}
  else if(e[0]==0){if(e.size()!=17)return FailView<IntervalCoveringValueViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","value_extent");auto nested=c;nested.maximum_allocation_bytes=~u64{0};nested.cancelled=nullptr;nested.cancellation_context=nullptr;auto d=DecodeCanonicalIntervalComponentNoAllocV3(h,IntervalValueStateV3::value,false,e.subspan(1),nested);if(!d.ok())return FailView<IntervalCoveringValueViewResultV3>(d.diagnostic.diagnostic_code,d.diagnostic.detail);state=d.value.state;months=d.value.months;days=d.value.civil_days;ns=d.value.fixed_nanoseconds;}
  else return FailView<IntervalCoveringValueViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","state");
  std::array<byte,17> canonical{};ArrayClearGuard<17> clear(&canonical,IntervalScrubClassV3::covering_decode_reencode,&c);canonical[0]=state==IntervalValueStateV3::sql_null?1:0;if(state==IntervalValueStateV3::value)EncodeComponentRaw({&h,state,months,days,ns},canonical.data()+1);
  if(c.force_reencode_mismatch_for_conformance||std::memcmp(e.data(),canonical.data(),e.size())!=0)return FailView<IntervalCoveringValueViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","covering_reencode");
  if(e.size()>c.maximum_allocation_bytes)return FailView<IntervalCoveringValueViewResultV3>("RESOURCE.BUDGET_EXCEEDED","covering_extent_budget");
  if(Cancelled(c))return FailView<IntervalCoveringValueViewResultV3>("PROCESS.CANCELLED","before_publication");
  IntervalCoveringValueViewResultV3 r;r.status=Ok();r.value={&h,state,months,days,ns};return r;
}

IntervalIndexProjectionResultV3 ProjectIntervalIndexValueV3(const IntervalIndexProjectionRequestV3& input) noexcept {
  auto q=input;IntervalOwnedValueV3 value_pin;IntervalIndexResolutionV3 selected_pin;if(input.value){value_pin=*input.value;q.value=&value_pin;}if(input.selected_conditional_family){selected_pin=*input.selected_conditional_family;q.selected_conditional_family=&selected_pin;}
  IntervalIndexResolutionV3 base;base.family=q.compatibility.family;if(q.value==nullptr||q.value->profile==nullptr)return FailIndex(base,base,"CTI.INTERVAL.DESCRIPTOR_INVALID","value_or_profile_missing");if(!ValidProfile(*q.value->profile,&q.control))return FailIndex(base,base,"CTI.INTERVAL.DESCRIPTOR_INVALID","profile_invalid");const auto admitted=AdmitIntervalIndexCompatibilityV3(q.compatibility);if(!admitted.admitted)return FailIndex(base,base,"OPTIMIZER.INDEX_COMPATIBILITY_MISSING","index_compatibility_identity");const auto requested=admitted.resolution;auto effective=requested;
  if(requested.disposition==IntervalIndexDispositionV3::not_applicable)return FailIndex(requested,effective,"OPTIMIZER.INDEX_COMPATIBILITY_MISSING","family_not_applicable",true);
  using F=IntervalIndexFamilyV3;using M=IntervalProjectionModeV3;using P=IntervalProjectionKindV3;
  if(requested.disposition==IntervalIndexDispositionV3::conditional){
    if(requested.family==F::temporary_work){if(q.selected_conditional_family!=nullptr)return FailIndex(requested,effective,"OPTIMIZER.INDEX_COMPATIBILITY_MISSING","temporary_selection_mode",true);effective=requested;if(q.mode==M::equality_hash){effective.projection=P::equality_hash;effective.exact_state_component_recheck_required=true;}else if(q.mode==M::payload){effective.projection=P::covering_value;effective.exact_state_component_recheck_required=false;}else return FailIndex(requested,effective,"OPTIMIZER.INDEX_COMPATIBILITY_MISSING","temporary_mode");}
    else {if(q.selected_conditional_family==nullptr)return FailIndex(requested,effective,"OPTIMIZER.INDEX_COMPATIBILITY_MISSING","conditional_selection",true);const auto identity=IntervalIndexCompatibilityIdentityV3{q.selected_conditional_family->family,q.selected_conditional_family->compatibility_uuid,q.selected_conditional_family->compatibility_generation};const auto chosen=AdmitIntervalIndexCompatibilityV3(identity);const bool expression_ok=requested.family!=F::expression||(chosen.resolution.family==F::bitmap||chosen.resolution.family==F::hash);if(!chosen.admitted||!SameResolution(chosen.resolution,*q.selected_conditional_family)||chosen.resolution.disposition==IntervalIndexDispositionV3::conditional||chosen.resolution.disposition==IntervalIndexDispositionV3::not_applicable||!expression_ok)return FailIndex(requested,effective,"OPTIMIZER.INDEX_COMPATIBILITY_MISSING","conditional_selection",true);effective=chosen.resolution;}
  }
  if((effective.projection==P::equality_hash&&q.mode!=M::equality_hash)||(effective.projection==P::covering_value&&q.mode!=M::payload))return FailIndex(requested,effective,"CTI.TEMPORAL.INDEX_KEY_REFUSED","selected_mode_invalid");
  const auto checked=ValidateValue(*q.value,&q.control);if(!checked.status.ok())return FailIndex(requested,effective,checked.diagnostic_code,checked.detail);
  const u64 need=effective.projection==P::equality_hash?kEqualityProjectionBytes:(q.value->state==IntervalValueStateV3::sql_null?1:17);
  if(q.provider_max_key_bytes<need)return FailIndex(requested,effective,"CTI.TEMPORAL.INDEX_KEY_REFUSED","provider_budget");
  if(q.control.maximum_allocation_bytes<need)return FailIndex(requested,effective,"RESOURCE.BUDGET_EXCEEDED","allocation_grant");
  if(Cancelled(q.control))return FailIndex(requested,effective,"PROCESS.CANCELLED","before_allocation");
  IntervalIndexProjectionResultV3 out;out.requested_resolution=requested;out.resolution=effective;VectorClearGuard clear(&out.bytes,IntervalScrubClassV3::projection_owned_buffer,&q.control);if(!Reserve(&out.bytes,need))return FailIndex(requested,effective,"RESOURCE.BUDGET_EXCEEDED","allocation");
  if(effective.projection==P::covering_value){out.bytes[0]=q.value->state==IntervalValueStateV3::sql_null?1:0;if(need==17)EncodeComponentRaw(*q.value,out.bytes.data()+1);auto self_control=q.control;self_control.maximum_allocation_bytes=~u64{0};self_control.cancelled=nullptr;self_control.cancellation_context=nullptr;if(!DecodeIntervalCoveringValueNoAllocV3(*q.value->profile,true,out.bytes,self_control).ok())return FailIndex(requested,effective,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","covering_self_check");}else if(effective.projection==P::equality_hash){auto* b=out.bytes.data();std::memcpy(b,"SBINIH01",8);StoreUuid(b+8,q.value->profile->receipt.catalog_snapshot_uuid);StoreLittle64(b+24,q.value->profile->receipt.catalog_generation);StoreLittle64(b+32,q.value->profile->receipt.registry_generation);std::memcpy(b+40,q.value->profile->equality_fingerprint.data(),32);StorePolicy(b+72,q.value->profile->index_policy);StoreUuid(b+96,effective.compatibility_uuid);StoreLittle64(b+112,effective.compatibility_generation);b[120]=q.value->state==IntervalValueStateV3::sql_null?1:0;std::array<byte,32> hash{};ArrayClearGuard<32> hash_clear(&hash,IntervalScrubClassV3::equality_projection,&q.control);auto hash_control=q.control;hash_control.maximum_allocation_bytes=~u64{0};const auto hr=HashIntervalValueIntoNoAllocV3(*q.value,hash.data(),hash.size(),hash_control);if(!hr.ok()||hr.bytes_written!=32)return FailIndex(requested,effective,hr.diagnostic.diagnostic_code,hr.diagnostic.detail);std::memcpy(b+128,hash.data(),32);if(q.control.force_reencode_mismatch_for_conformance)return FailIndex(requested,effective,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","equality_projection_reencode");}else return FailIndex(requested,effective,"OPTIMIZER.INDEX_COMPATIBILITY_MISSING","projection_absent",true);
  if(Cancelled(q.control))return FailIndex(requested,effective,"PROCESS.CANCELLED","before_publication");
  out.status=Ok();clear.Disarm();return out;
}

namespace {
bool IsAuthorityUuid(const IntervalValidatedProfileHandleV3& h,const platform::Uuid& u) noexcept {
  if(u.is_nil())return false;
  const auto& l=h.identity.legacy_fields;
  if(u==h.receipt.statement_receipt_uuid||u==h.receipt.catalog_snapshot_uuid||u==l.descriptor_uuid||u==l.type_uuid||u==l.codec_uuid)return true;
  const DatatypePolicyIdentityV3* policies[]={&h.identity.descriptor_policy,&h.identity.canonicalization_policy,&h.identity.ordering_policy,&h.identity.hash_policy,&h.identity.operation_policy,&h.render_policy,&h.cast_policy,&h.subtype_policy,&h.precision_policy,&h.normalization_policy,&h.temporal_application_boundary_policy,&h.storage_epoch_policy,&h.index_policy,&h.statistics_policy,&h.backup_transport_policy,&h.protection_policy,&h.component_adapter_policy,&h.diagnostic_policy,&h.metric_policy};
  for(const auto* p:policies)if(u==p->uuid)return true;
  const auto bytes=u.bytes;
  const std::array<byte,15> prefix{{0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a}};
  if(std::equal(prefix.begin(),prefix.end(),bytes.begin())&&((bytes[15]>=0xb1&&bytes[15]<=0xb6)||(bytes[15]>=0xba&&bytes[15]<=0xc9)))return true;
  return false;
}
bool ValidInstanceUuids(const IntervalValidatedProfileHandleV3& h,const platform::Uuid& statistics,const platform::Uuid& source,const platform::Uuid& provider) noexcept {
  return !statistics.is_nil()&&!source.is_nil()&&!provider.is_nil()&&statistics!=source&&statistics!=provider&&source!=provider&&!IsAuthorityUuid(h,statistics)&&!IsAuthorityUuid(h,source)&&!IsAuthorityUuid(h,provider);
}
void WriteStatisticsFixed(byte* b,const IntervalValidatedProfileHandleV3& h,u32 total,u32 count,const platform::Uuid& statistics,const platform::Uuid& source,const platform::Uuid& provider,u64 schema,u64 collection,u64 security,u64 rows,u64 nulls,u64 values,u64 distinct) noexcept {
  std::memcpy(b,kStatisticsMagic,8);StoreLittle16(b+8,1);StoreLittle16(b+10,kIntervalStatisticsHeaderBytesV3);StoreLittle32(b+12,total);StoreLittle32(b+16,1);StoreLittle32(b+20,count);PutCommonIdentity(b+32,h);StorePolicy(b+144,h.statistics_policy);StorePolicy(b+168,h.identity.hash_policy);StorePolicy(b+192,h.identity.ordering_policy);StorePolicy(b+216,h.storage_epoch_policy);StoreUuid(b+264,statistics);StoreUuid(b+280,source);StoreUuid(b+296,provider);StoreLittle64(b+312,schema);StoreLittle64(b+320,collection);StoreLittle64(b+328,security);StoreLittle64(b+336,rows);StoreLittle64(b+344,nulls);StoreLittle64(b+352,values);StoreLittle64(b+360,distinct);std::memcpy(b+400,h.profile_fingerprint.data(),32);
}
void WriteMcv(byte* b,const IntervalStatisticsMcvRecordV3& row) noexcept {std::memcpy(b,row.value_hash.data(),32);StoreLittle64(b+32,row.frequency);StoreLittle32(b+40,row.rank);StoreLittle32(b+44,row.flags);}
IntervalStatisticsReadAdmissionResultV3 StatsRefuse(IntervalStatisticsReadAdmissionDispositionV3 disposition,std::string_view code,std::string_view detail={},bool attempted=false,u32 read=0) noexcept {IntervalStatisticsReadAdmissionResultV3 r;r.disposition=disposition;r.diagnostic={code=="RESOURCE.BUDGET_EXCEEDED"?BudgetError():Error(),code,detail};r.decode_attempted=attempted;r.mcv_records_read=read;return r;}
IntervalStatisticsReadAdmissionResultV3 DecodeStatisticsInternal(const std::shared_ptr<const IntervalValidatedProfileHandleV3>& profile,std::span<const byte> e,const IntervalStatisticsReceivingFactsV3& current,const IntervalExecutionControlV3& c) noexcept {
  using D=IntervalStatisticsReadAdmissionDispositionV3;
  if(!current.security_visible)return StatsRefuse(D::security_denied,"SECURITY.ACCESS_DENIED","statistics_visibility");
  if(!current.privacy_admitted)return StatsRefuse(D::privacy_denied,"OPTIMIZER.STATISTICS_PRIVACY_DENIED","statistics_privacy");
  if(!profile)return StatsRefuse(D::profile_refused,"SBLR.OPERAND_INVALID","statistics_profile");
  if(!ValidProfile(*profile,&c))return StatsRefuse(D::profile_refused,"CTI.INTERVAL.DESCRIPTOR_INVALID","statistics_profile");
  if(e.size()<kIntervalStatisticsHeaderBytesV3||e.size()>std::numeric_limits<u32>::max())return StatsRefuse(D::format_refused,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","statistics_extent");
  if(std::memcmp(e.data(),kStatisticsMagic,8)||LoadLittle16(e.data()+8)!=1||LoadLittle16(e.data()+10)!=kIntervalStatisticsHeaderBytesV3)return StatsRefuse(D::format_refused,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","statistics_structure");
  const u32 total=LoadLittle32(e.data()+12),flags=LoadLittle32(e.data()+16),count=LoadLittle32(e.data()+20);
  if(flags!=1||count>kIntervalStatisticsMaximumMcvRecordsV3||count>(std::numeric_limits<u32>::max()-kIntervalStatisticsHeaderBytesV3)/kIntervalStatisticsMcvRecordBytesV3||total!=kIntervalStatisticsHeaderBytesV3+count*kIntervalStatisticsMcvRecordBytesV3||total!=e.size()||total>kIntervalStatisticsMaximumBytesV3)return StatsRefuse(D::format_refused,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","statistics_structure");
  byte structural=0;for(std::size_t i=24;i<32;++i)structural|=e[i];for(std::size_t i=432;i<448;++i)structural|=e[i];if(structural!=0)return StatsRefuse(D::format_refused,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","statistics_reserved");
  std::array<byte,32> digest{};ArrayClearGuard<32> digest_clear(&digest,IntervalScrubClassV3::statistics_prior_hash,&c);if(!HashRecord(kStatisticsDomain,e,368,false,&digest,&c))return StatsRefuse(D::resource_refused,"RESOURCE.BUDGET_EXCEEDED","statistics_hash");if(!ConstantTimeEqual(digest.data(),e.data()+368,32))return StatsRefuse(D::format_refused,"SBLR.ENVELOPE.CHECKSUM_INVALID","statistics_integrity");
  if(!CommonIdentityEquals(e.data()+32,*profile)||!PolicyEquals(e.data()+144,profile->statistics_policy)||!PolicyEquals(e.data()+168,profile->identity.hash_policy)||!PolicyEquals(e.data()+192,profile->identity.ordering_policy)||!PolicyEquals(e.data()+216,profile->storage_epoch_policy)||std::memcmp(e.data()+400,profile->profile_fingerprint.data(),32))return StatsRefuse(D::profile_refused,"CTI.INTERVAL.DESCRIPTOR_INVALID","statistics_authority");
  byte covered=0;for(std::size_t i=132;i<136;++i)covered|=e[i];for(std::size_t i=240;i<264;++i)covered|=e[i];if(covered!=0)return StatsRefuse(D::format_refused,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","statistics_reserved");
  IntervalDecodedStatisticsV3 decoded;decoded.profile=profile;decoded.statistics_snapshot_uuid=LoadUuid(e.data()+264);decoded.source_object_uuid=LoadUuid(e.data()+280);decoded.provider_evidence_uuid=LoadUuid(e.data()+296);decoded.schema_epoch=LoadLittle64(e.data()+312);decoded.collection_epoch=LoadLittle64(e.data()+320);decoded.security_epoch=LoadLittle64(e.data()+328);decoded.row_count=LoadLittle64(e.data()+336);decoded.null_count=LoadLittle64(e.data()+344);decoded.value_count=LoadLittle64(e.data()+352);decoded.equality_distinct_estimate=LoadLittle64(e.data()+360);
  if(decoded.source_object_uuid!=current.source_object_uuid)return StatsRefuse(D::provider_evidence_mismatch,"CTI.INTERVAL.DESCRIPTOR_INVALID","statistics_provider_evidence");
  if(decoded.schema_epoch!=current.schema_epoch)return StatsRefuse(D::schema_epoch_mismatch,"OPTIMIZER.STATISTICS_EPOCH_MISMATCH","statistics_schema_epoch");
  if(decoded.collection_epoch!=current.collection_epoch)return StatsRefuse(D::collection_epoch_mismatch,"OPTIMIZER.STATISTICS_EPOCH_MISMATCH","statistics_collection_epoch");
  if(decoded.security_epoch!=current.security_epoch)return StatsRefuse(D::security_epoch_mismatch,"OPTIMIZER.STATISTICS_EPOCH_MISMATCH","statistics_security_epoch");
  if(decoded.provider_evidence_uuid!=current.provider_evidence_uuid)return StatsRefuse(D::provider_evidence_mismatch,"CTI.INTERVAL.DESCRIPTOR_INVALID","statistics_provider_evidence");
  if(!ValidInstanceUuids(*profile,decoded.statistics_snapshot_uuid,decoded.source_object_uuid,decoded.provider_evidence_uuid)||decoded.schema_epoch==0||decoded.collection_epoch==0||decoded.security_epoch==0||!CountsValid(decoded.row_count,decoded.null_count,decoded.value_count,decoded.equality_distinct_estimate))return StatsRefuse(D::format_refused,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","statistics_population");
  if(e.size()>c.maximum_allocation_bytes)return StatsRefuse(D::resource_refused,"RESOURCE.BUDGET_EXCEEDED","statistics_allocation");
  if(Cancelled(c))return StatsRefuse(D::cancelled,"PROCESS.CANCELLED","statistics_before_allocation");
  try{decoded.mcv.reserve(count);}catch(...){return StatsRefuse(D::resource_refused,"RESOURCE.BUDGET_EXCEEDED","statistics_allocation");}
  u32 read=0;
  for(u32 i=0;i<count;++i){const byte* b=e.data()+kIntervalStatisticsHeaderBytesV3+i*kIntervalStatisticsMcvRecordBytesV3;IntervalStatisticsMcvRecordV3 row;std::memcpy(row.value_hash.data(),b,32);row.frequency=LoadLittle64(b+32);row.rank=LoadLittle32(b+40);row.flags=LoadLittle32(b+44);try{decoded.mcv.push_back(row);}catch(...){SecureClear(decoded.mcv.data(),decoded.mcv.size()*sizeof(row));return StatsRefuse(D::resource_refused,"RESOURCE.BUDGET_EXCEEDED","statistics_allocation",true,read);}++read;if(Cancelled(c)){SecureClear(decoded.mcv.data(),decoded.mcv.size()*sizeof(row));return StatsRefuse(D::cancelled,"PROCESS.CANCELLED","statistics_record_decode",true,read);}}
  if(!CanonicalMcv(decoded.mcv,decoded.value_count)){SecureClear(decoded.mcv.data(),decoded.mcv.size()*sizeof(IntervalStatisticsMcvRecordV3));return StatsRefuse(D::format_refused,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","statistics_mcv",true,read);}
  std::array<byte,kIntervalStatisticsMaximumBytesV3> canonical{};ArrayClearGuard<kIntervalStatisticsMaximumBytesV3> canonical_clear(&canonical,IntervalScrubClassV3::statistics_decode_reencode,&c);WriteStatisticsFixed(canonical.data(),*profile,total,count,decoded.statistics_snapshot_uuid,decoded.source_object_uuid,decoded.provider_evidence_uuid,decoded.schema_epoch,decoded.collection_epoch,decoded.security_epoch,decoded.row_count,decoded.null_count,decoded.value_count,decoded.equality_distinct_estimate);for(u32 i=0;i<count;++i)WriteMcv(canonical.data()+kIntervalStatisticsHeaderBytesV3+i*kIntervalStatisticsMcvRecordBytesV3,decoded.mcv[i]);std::array<byte,32> canonical_hash{};ArrayClearGuard<32> hash_clear(&canonical_hash,IntervalScrubClassV3::statistics_prior_hash,&c);HashRecord(kStatisticsDomain,std::span<const byte>(canonical.data(),total),368,false,&canonical_hash,&c);std::memcpy(canonical.data()+368,canonical_hash.data(),32);if(c.force_reencode_mismatch_for_conformance||std::memcmp(canonical.data(),e.data(),total)){SecureClear(decoded.mcv.data(),decoded.mcv.size()*sizeof(IntervalStatisticsMcvRecordV3));return StatsRefuse(D::format_refused,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","statistics_reencode",true,read);}
  if(ResolveIntervalStatisticsReceivingFactsV3(decoded,current)!=IntervalStatisticsReceivingDispositionV3::admitted){SecureClear(decoded.mcv.data(),decoded.mcv.size()*sizeof(IntervalStatisticsMcvRecordV3));return StatsRefuse(D::provider_evidence_mismatch,"CTI.INTERVAL.DESCRIPTOR_INVALID","statistics_receiving_facts",true,read);}
  if(Cancelled(c)){SecureClear(decoded.mcv.data(),decoded.mcv.size()*sizeof(IntervalStatisticsMcvRecordV3));return StatsRefuse(D::cancelled,"PROCESS.CANCELLED","statistics_before_publication",true,read);}
  IntervalStatisticsReadAdmissionResultV3 out;out.disposition=D::admitted;out.statistics=std::move(decoded);out.decode_attempted=true;out.mcv_records_read=read;return out;
}
}  // namespace

IntervalBytesResultV3 EncodeIntervalStatisticsProjectionV3(const IntervalStatisticsProjectionV3& p,const IntervalExecutionControlV3& c) noexcept {
  auto provider=p.provider_evidence;if(!provider)return FailBytes("CTI.INTERVAL.DESCRIPTOR_INVALID","statistics_provider_evidence");if(!provider->source_authorized)return FailBytes("SECURITY.ACCESS_DENIED","statistics_source_authorization");if(!provider->privacy_admitted)return FailBytes("OPTIMIZER.STATISTICS_PRIVACY_DENIED","statistics_privacy");auto profile=p.profile;if(!profile||!ValidProfile(*profile,&c))return FailBytes("CTI.INTERVAL.DESCRIPTOR_INVALID","statistics_profile");
  if(provider->provider_evidence_uuid.is_nil()||provider->source_object_uuid.is_nil()||!provider->authenticated_population)return FailBytes("OPTIMIZER.STATISTICS_EPOCH_MISMATCH","statistics_population_evidence");
  const bool same_mcv=p.mcv.size()==provider->mcv.size()&&std::equal(p.mcv.begin(),p.mcv.end(),provider->mcv.begin());if(p.source_object_uuid!=provider->source_object_uuid||p.schema_epoch!=provider->schema_epoch||p.collection_epoch!=provider->collection_epoch||p.security_epoch!=provider->security_epoch||p.row_count!=provider->row_count||p.null_count!=provider->null_count||p.value_count!=provider->value_count||p.equality_distinct_estimate!=provider->equality_distinct_estimate||!same_mcv)return FailBytes("OPTIMIZER.STATISTICS_EPOCH_MISMATCH","statistics_population_evidence");
  if(!ValidInstanceUuids(*profile,p.statistics_snapshot_uuid,p.source_object_uuid,provider->provider_evidence_uuid))return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","statistics_instance_identity");
  if(p.schema_epoch==0||p.collection_epoch==0||p.security_epoch==0)return FailBytes("OPTIMIZER.STATISTICS_EPOCH_MISMATCH","statistics_population_evidence");
  if(p.mcv.size()>kIntervalStatisticsMaximumMcvRecordsV3||!CountsValid(p.row_count,p.null_count,p.value_count,p.equality_distinct_estimate)||!CanonicalMcv(p.mcv,p.value_count))return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","statistics_population_evidence");
  const u64 total=kIntervalStatisticsHeaderBytesV3+kIntervalStatisticsMcvRecordBytesV3*p.mcv.size();if(total>kIntervalStatisticsMaximumBytesV3||total>c.maximum_allocation_bytes)return FailBytes("RESOURCE.BUDGET_EXCEEDED","statistics_allocation");if(Cancelled(c))return FailBytes("PROCESS.CANCELLED","statistics_before_allocation");IntervalBytesResultV3 r;VectorClearGuard clear(&r.bytes,IntervalScrubClassV3::projection_owned_buffer,&c);if(!Reserve(&r.bytes,total))return FailBytes("RESOURCE.BUDGET_EXCEEDED","statistics_allocation");WriteStatisticsFixed(r.bytes.data(),*profile,static_cast<u32>(total),static_cast<u32>(p.mcv.size()),p.statistics_snapshot_uuid,p.source_object_uuid,provider->provider_evidence_uuid,p.schema_epoch,p.collection_epoch,p.security_epoch,p.row_count,p.null_count,p.value_count,p.equality_distinct_estimate);for(std::size_t i=0;i<p.mcv.size();++i){WriteMcv(r.bytes.data()+kIntervalStatisticsHeaderBytesV3+i*kIntervalStatisticsMcvRecordBytesV3,p.mcv[i]);if(Cancelled(c))return FailBytes("PROCESS.CANCELLED","statistics_record_encode");}std::array<byte,32> digest{};ArrayClearGuard<32> digest_clear(&digest,IntervalScrubClassV3::statistics_prior_hash,&c);if(!HashRecord(kStatisticsDomain,r.bytes,368,false,&digest,&c))return FailBytes("RESOURCE.BUDGET_EXCEEDED","statistics_hash");std::memcpy(r.bytes.data()+368,digest.data(),32);if(c.force_reencode_mismatch_for_conformance)return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","statistics_reencode");if(Cancelled(c))return FailBytes("PROCESS.CANCELLED","statistics_before_publication");r.status=Ok();clear.Disarm();return r;
}

IntervalStatisticsReceivingDispositionV3 ResolveIntervalStatisticsReceivingFactsV3(const IntervalDecodedStatisticsV3& d,const IntervalStatisticsReceivingFactsV3& c) noexcept {using D=IntervalStatisticsReceivingDispositionV3;if(!c.security_visible)return D::security_denied;if(!c.privacy_admitted)return D::privacy_denied;if(d.source_object_uuid!=c.source_object_uuid)return D::provider_evidence_mismatch;if(d.schema_epoch!=c.schema_epoch)return D::schema_epoch_mismatch;if(d.collection_epoch!=c.collection_epoch)return D::collection_epoch_mismatch;if(d.security_epoch!=c.security_epoch)return D::security_epoch_mismatch;if(d.provider_evidence_uuid!=c.provider_evidence_uuid)return D::provider_evidence_mismatch;return D::admitted;}
IntervalStatisticsReadAdmissionResultV3 AdmitIntervalStatisticsReadV3(const std::shared_ptr<const IntervalValidatedProfileHandleV3>& profile,std::span<const byte> encoded,const IntervalStatisticsReceivingFactsV3& current,const IntervalExecutionControlV3& control) noexcept {return DecodeStatisticsInternal(profile,encoded,current,control);}
IntervalStatisticsDecodeResultV3 DecodeIntervalStatisticsProjectionV3(const std::shared_ptr<const IntervalValidatedProfileHandleV3>& profile,std::span<const byte> encoded,const IntervalStatisticsReceivingFactsV3& current,const IntervalExecutionControlV3& control) noexcept {auto admitted=DecodeStatisticsInternal(profile,encoded,current,control);IntervalStatisticsDecodeResultV3 r;r.diagnostic=admitted.diagnostic;r.mcv_records_read=admitted.mcv_records_read;if(admitted.ok()){r.status=Ok();r.statistics=std::move(admitted.statistics);}else r.status=admitted.diagnostic.status;return r;}


namespace {
void PutBackupIdentity(byte* p, const IntervalValidatedProfileHandleV3& h) noexcept {
  PutCommonIdentity(p, h);
  std::memcpy(p + 112, h.profile_fingerprint.data(), 32);
  const DatatypePolicyIdentityV3* policies[] = {
      &h.subtype_policy, &h.normalization_policy,
      &h.temporal_application_boundary_policy, &h.storage_epoch_policy,
      &h.backup_transport_policy, &h.protection_policy};
  for (std::size_t i = 0; i < 6; ++i) StorePolicy(p + 144 + i * 24, *policies[i]);
}
bool BackupIdentityEquals(const byte* p, const IntervalValidatedProfileHandleV3& h) noexcept {
  if (!CommonIdentityEquals(p, h) ||
      std::memcmp(p + 112, h.profile_fingerprint.data(), 32) != 0) return false;
  const DatatypePolicyIdentityV3* policies[] = {
      &h.subtype_policy, &h.normalization_policy,
      &h.temporal_application_boundary_policy, &h.storage_epoch_policy,
      &h.backup_transport_policy, &h.protection_policy};
  for (std::size_t i = 0; i < 6; ++i)
    if (!PolicyEquals(p + 144 + i * 24, *policies[i])) return false;
  return true;
}
void WriteBackup(byte* p, std::size_t size,
                 const IntervalValidatedProfileHandleV3& h,
                 IntervalValueStateV3 state, std::int32_t months,
                 std::int32_t days, std::int64_t nanoseconds) noexcept {
  std::memcpy(p, kBackupMagic, 8);
  StoreLittle16(p + 8, 1); StoreLittle16(p + 10, kBackupNullBytes);
  StoreLittle32(p + 12, static_cast<u32>(size)); PutBackupIdentity(p + 16, h);
  p[304] = state == IntervalValueStateV3::sql_null ? 0 : 1;
  p[305] = state == IntervalValueStateV3::sql_null ? 0 : 16;
  if (state == IntervalValueStateV3::value) {
    StoreLittle32(p + 360, static_cast<u32>(months));
    StoreLittle32(p + 364, static_cast<u32>(days));
    StoreLittle64(p + 368, static_cast<u64>(nanoseconds));
  }
}
}  // namespace

IntervalBytesResultV3 EncodeIntervalBackupTupleV3(
    const IntervalOwnedValueV3& input, const IntervalExecutionControlV3& control) noexcept {
  const auto value = input;
  const auto checked = ValidateValue(value, &control);
  if (!checked.status.ok()) return FailBytes(checked.diagnostic_code, checked.detail);
  const u64 extent = value.state == IntervalValueStateV3::sql_null
      ? kBackupNullBytes : kBackupValueBytes;
  if (extent > control.maximum_allocation_bytes)
    return FailBytes("RESOURCE.BUDGET_EXCEEDED", "backup_allocation");
  if (Cancelled(control)) return FailBytes("PROCESS.CANCELLED", "backup_before_allocation");
  IntervalBytesResultV3 result;
  VectorClearGuard clear(&result.bytes, IntervalScrubClassV3::projection_owned_buffer, &control);
  if (!Reserve(&result.bytes, extent))
    return FailBytes("RESOURCE.BUDGET_EXCEEDED", "backup_allocation");
  WriteBackup(result.bytes.data(), result.bytes.size(), *value.profile, value.state,
              value.months, value.civil_days, value.fixed_nanoseconds);
  std::array<byte, 32> digest{};
  ArrayClearGuard<32> digest_clear(&digest, IntervalScrubClassV3::hash_digest, &control);
  if (!HashRecord(kBackupDomain, result.bytes, 328, true, &digest, &control))
    return FailBytes("RESOURCE.BUDGET_EXCEEDED", "backup_hash");
  std::memcpy(result.bytes.data() + 328, digest.data(), 32);
  auto self_control = control; self_control.cancelled = nullptr;
  self_control.cancellation_context = nullptr;
  if (!DecodeIntervalBackupTupleNoAllocV3(*value.profile, true, result.bytes, self_control).ok())
    return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "backup_self_check");
  if (Cancelled(control)) return FailBytes("PROCESS.CANCELLED", "backup_before_publication");
  result.status = Ok(); clear.Disarm(); return result;
}

IntervalBackupTupleViewResultV3 DecodeIntervalBackupTupleNoAllocV3(
    const IntervalValidatedProfileHandleV3& profile, bool null_allowed,
    std::span<const byte> encoded, const IntervalExecutionControlV3& control) noexcept {
  if (!ValidProfile(profile, &control))
    return FailView<IntervalBackupTupleViewResultV3>("CTI.INTERVAL.DESCRIPTOR_INVALID", "backup_profile");
  if ((encoded.size() != kBackupNullBytes && encoded.size() != kBackupValueBytes) ||
      std::memcmp(encoded.data(), kBackupMagic, 8) != 0 ||
      LoadLittle16(encoded.data() + 8) != 1 ||
      LoadLittle16(encoded.data() + 10) != kBackupNullBytes ||
      LoadLittle32(encoded.data() + 12) != encoded.size())
    return FailView<IntervalBackupTupleViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "backup_structure");
  byte reserved = 0;
  for (std::size_t i = 116; i < 120; ++i) reserved |= encoded[i];
  for (std::size_t i = 306; i < 328; ++i) reserved |= encoded[i];
  if (reserved != 0)
    return FailView<IntervalBackupTupleViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "backup_reserved");
  std::array<byte, 32> digest{};
  ArrayClearGuard<32> digest_clear(&digest, IntervalScrubClassV3::hash_digest, &control);
  if (!HashRecord(kBackupDomain, encoded, 328, true, &digest, &control))
    return FailView<IntervalBackupTupleViewResultV3>("RESOURCE.BUDGET_EXCEEDED", "backup_hash");
  if (!ConstantTimeEqual(digest.data(), encoded.data() + 328, 32))
    return FailView<IntervalBackupTupleViewResultV3>("SBLR.ENVELOPE.CHECKSUM_INVALID", "backup_integrity");
  if (!BackupIdentityEquals(encoded.data() + 16, profile))
    return FailView<IntervalBackupTupleViewResultV3>("CTI.INTERVAL.DESCRIPTOR_INVALID", "backup_identity");
  IntervalValueStateV3 state{};
  if (encoded[304] == 0) {
    if (encoded[305] != 0 || encoded.size() != kBackupNullBytes)
      return FailView<IntervalBackupTupleViewResultV3>("DATATYPE.NULL_STATE.INVALID", "backup_null_pair");
    if (!null_allowed)
      return FailView<IntervalBackupTupleViewResultV3>("DATATYPE.NULL_NOT_ADMITTED", "backup_null_not_admitted");
    state = IntervalValueStateV3::sql_null;
  } else if (encoded[304] == 1) {
    if (encoded[305] != 16 || encoded.size() != kBackupValueBytes)
      return FailView<IntervalBackupTupleViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "backup_value_pair");
    state = IntervalValueStateV3::value;
  } else {
    return FailView<IntervalBackupTupleViewResultV3>("DATATYPE.NULL_STATE.INVALID", "backup_state");
  }
  const auto component = state == IntervalValueStateV3::sql_null
      ? std::span<const byte>{} : encoded.subspan(360, 16);
  auto nested = control; nested.maximum_allocation_bytes = ~u64{0};
  nested.cancelled = nullptr; nested.cancellation_context = nullptr;
  const auto decoded = DecodeCanonicalIntervalComponentNoAllocV3(
      profile, state, null_allowed, component, nested);
  if (!decoded.ok())
    return FailView<IntervalBackupTupleViewResultV3>(
        decoded.diagnostic.diagnostic_code, decoded.diagnostic.detail);
  std::array<byte, kBackupValueBytes> canonical{};
  ArrayClearGuard<kBackupValueBytes> canonical_clear(
      &canonical, IntervalScrubClassV3::backup_decode_reencode, &control);
  WriteBackup(canonical.data(), encoded.size(), profile, state,
              decoded.value.months, decoded.value.civil_days,
              decoded.value.fixed_nanoseconds);
  std::array<byte, 32> canonical_digest{};
  ArrayClearGuard<32> canonical_digest_clear(
      &canonical_digest, IntervalScrubClassV3::hash_digest, &control);
  if (!HashRecord(kBackupDomain,
                  std::span<const byte>(canonical.data(), encoded.size()),
                  328, true, &canonical_digest, &control))
    return FailView<IntervalBackupTupleViewResultV3>("RESOURCE.BUDGET_EXCEEDED", "backup_reencode_hash");
  std::memcpy(canonical.data() + 328, canonical_digest.data(), 32);
  if (control.force_reencode_mismatch_for_conformance ||
      std::memcmp(canonical.data(), encoded.data(), encoded.size()) != 0)
    return FailView<IntervalBackupTupleViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "backup_reencode");
  if (encoded.size() > control.maximum_allocation_bytes)
    return FailView<IntervalBackupTupleViewResultV3>("RESOURCE.BUDGET_EXCEEDED", "backup_extent_budget");
  if (Cancelled(control))
    return FailView<IntervalBackupTupleViewResultV3>("PROCESS.CANCELLED", "backup_before_publication");
  IntervalBackupTupleViewResultV3 result; result.status = Ok();
  result.tuple = {&profile, state, decoded.value.months,
                  decoded.value.civil_days, decoded.value.fixed_nanoseconds};
  return result;
}

IntervalProtectionResolutionV3 ResolveIntervalProtectionV3(
    IntervalProtectionCellV3 cell) noexcept {
  using C = IntervalProtectionCellV3;
  switch (cell) {
    case C::canonical_plain:
      return {cell, true, false, "admitted_datatype_component", {}};
    case C::inner_compression: case C::inner_encryption:
      return {cell, false, false, "forbidden", "CTI.TEMPORAL.PROTECTION_UNSUPPORTED"};
    case C::outer_compression: case C::outer_authenticated_protection:
    case C::outer_compress_then_protect: case C::outer_protect_then_compress:
    case C::page_or_filespace_crypto: case C::transport_tls:
      return {cell, false, true, "receiving_owner_handoff", {}};
    case C::protected_index:
      return {cell, false, false, "forbidden", "CTI.TEMPORAL.PROTECTION_UNSUPPORTED"};
  }
  return {cell, false, false, "unknown_cell_fail_closed", "CTI.INTERVAL.DESCRIPTOR_INVALID"};
}

IntervalProtectionCombinationResolutionV3 ResolveIntervalProtectionCombinationV3(
    IntervalProtectionModeV3 storage, IntervalProtectionModeV3 stream,
    IntervalProtectionIndexModeV3 index) noexcept {
  if (static_cast<unsigned>(storage) >= 4 || static_cast<unsigned>(stream) >= 4 ||
      static_cast<unsigned>(index) >= 2)
    return {storage, stream, index, false, "unknown_combination_fail_closed", "CTI.INTERVAL.DESCRIPTOR_INVALID"};
  if (storage == IntervalProtectionModeV3::clear && stream == IntervalProtectionModeV3::clear)
    return {storage, stream, index, true, "admitted", {}};
  return {storage, stream, index, false, "refused", "CTI.TEMPORAL.PROTECTION_UNSUPPORTED"};
}
IntervalProtectionCombinationResolutionV3 AdmitIntervalProtectionCombinationV3(
    const IntervalValidatedProfileHandleV3& profile,
    IntervalProtectionModeV3 storage, IntervalProtectionModeV3 stream,
    IntervalProtectionIndexModeV3 index) noexcept {
  if (!ValidProfile(profile))
    return {storage, stream, index, false, "profile_refused", "CTI.INTERVAL.DESCRIPTOR_INVALID"};
  return ResolveIntervalProtectionCombinationV3(storage, stream, index);
}

IntervalWireLaneResolutionV3 ResolveIntervalWireLaneV3(IntervalWireLaneV3 lane) noexcept {
  if (static_cast<unsigned>(lane) >= 27)
    return {lane, false, false, true, "unknown_lane_fail_closed", "CTI.TRANSPORT.UNSUPPORTED"};
  const bool native = static_cast<unsigned>(lane) <=
      static_cast<unsigned>(IntervalWireLaneV3::canonical_sblr);
  return {lane, false, native, true,
          native ? "unsupported_current_outer_version_unallocated"
                 : "unsupported_no_manifest_listed_compatibility_profile",
          "CTI.TRANSPORT.UNSUPPORTED"};
}

namespace {
constexpr std::array<platform::Uuid, 5> kMetricIds{{
    Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}),
    Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb3}),
    Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb4}),
    Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb5}),
    Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb6})}};
constexpr std::array<std::string_view, 5> kMetricNames{{
    "sys.metrics.datatype.base_interval.validation_failure_total",
    "sys.metrics.datatype.base_interval.cast_refusal_total",
    "sys.metrics.datatype.base_interval.arithmetic_overflow_total",
    "sys.metrics.datatype.base_interval.ordering_refusal_total",
    "sys.metrics.datatype.base_interval.temporal_application_refusal_total"}};
bool ValidRouteKey(const IntervalDiagnosticRouteKeyV3& key) noexcept {
  return static_cast<unsigned>(key.axis) < 15 && !key.diagnostic_uuid.is_nil();
}
bool SameRoute(const IntervalDiagnosticMetricRouteV3& a,
               const IntervalDiagnosticMetricRouteV3& b) noexcept {
  return a.key == b.key && a.type == b.type && a.metric_uuid == b.metric_uuid &&
      a.canonical_name == b.canonical_name &&
      a.permitted_operation_mask == b.permitted_operation_mask &&
      a.permitted_source_family_mask == b.permitted_source_family_mask &&
      a.permitted_state_mask == b.permitted_state_mask && a.reason == b.reason;
}
}  // namespace

std::span<const IntervalMetricEvidenceTypeDescriptorV3>
IntervalMetricEvidenceTypesV3() noexcept {
  static constexpr std::array<IntervalMetricEvidenceTypeDescriptorV3, 5> rows{{
#define METRIC_ROW(i, t) {IntervalMetricEvidenceTypeV3::t, kMetricIds[i], kMetricIds[i], 1, kMetricNames[i], "counter", "base.interval datatype immutable typed evidence", "metrics_registry_manager receiving owner", "after final failure outcome and diagnostic selection", "u64 counter", "events", "operation,source_family,state,reason", "bounded_closed_enum", "receiving owner validates idempotency then increments once", "exactly_one_evidence_result_after_final_diagnostic_selection_no_value_data", "metrics_registry_manager only"}
    METRIC_ROW(0, validation_failure_total),
    METRIC_ROW(1, cast_refusal_total),
    METRIC_ROW(2, arithmetic_overflow_total),
    METRIC_ROW(3, ordering_refusal_total),
    METRIC_ROW(4, temporal_application_refusal_total),
#undef METRIC_ROW
  }};
  return rows;
}

std::span<const IntervalDiagnosticMetricRouteV3>
IntervalDiagnosticMetricRoutesV3() noexcept {
  static constexpr std::array<IntervalDiagnosticMetricRouteV3, 37> rows{{
    {{IntervalDiagnosticAxisV3::input_parse, Uuid({0xf7,0xc7,0x36,0x87,0xc7,0x8d,0x54,0x6a,0xbc,0x93,0x02,0x8b,0x52,0xb4,0x35,0xcf})}, IntervalMetricEvidenceTypeV3::cast_refusal_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb3}), "sys.metrics.datatype.base_interval.cast_refusal_total", 0x0004, 0x02, 0x0f, IntervalMetricReasonV3::validation_failure},
    {{IntervalDiagnosticAxisV3::input_parse, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x02,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x02})}, IntervalMetricEvidenceTypeV3::cast_refusal_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb3}), "sys.metrics.datatype.base_interval.cast_refusal_total", 0x0004, 0x02, 0x0f, IntervalMetricReasonV3::invalid_literal},
    {{IntervalDiagnosticAxisV3::descriptor_codec, Uuid({0x1a,0x8d,0x26,0xb0,0x87,0x55,0x56,0xad,0xa1,0x70,0xb2,0x4a,0x3a,0xaf,0x14,0x07})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x0001, 0x01, 0x0f, IntervalMetricReasonV3::validation_failure},
    {{IntervalDiagnosticAxisV3::descriptor_codec, Uuid({0x60,0x2f,0x2d,0x54,0x14,0xaa,0x56,0x33,0x9e,0x0b,0xb4,0x79,0x0c,0x5a,0xe7,0x18})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x0001, 0x01, 0x0f, IntervalMetricReasonV3::descriptor_invalid},
    {{IntervalDiagnosticAxisV3::descriptor_codec, Uuid({0x50,0xc4,0x99,0x3e,0x5d,0xcf,0x58,0xf8,0xb6,0x48,0x43,0x6f,0x9e,0x8a,0xde,0x40})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x0001, 0x01, 0x0f, IntervalMetricReasonV3::subtype_mismatch},
    {{IntervalDiagnosticAxisV3::storage_read_write, Uuid({0x0c,0x7d,0x9a,0xaf,0x92,0xe6,0x59,0x05,0x9d,0xaf,0x09,0x74,0xb8,0x71,0xf9,0x4a})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x1000, 0x04, 0x0f, IntervalMetricReasonV3::validation_failure},
    {{IntervalDiagnosticAxisV3::storage_read_write, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x03,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x03})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x1000, 0x04, 0x0f, IntervalMetricReasonV3::canonical_encoding_invalid},
    {{IntervalDiagnosticAxisV3::cast_invalid, Uuid({0x0d,0xc5,0x1a,0x1c,0xad,0x21,0x57,0xb5,0xae,0x65,0xe0,0xfe,0x55,0x89,0x02,0x1a})}, IntervalMetricEvidenceTypeV3::cast_refusal_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb3}), "sys.metrics.datatype.base_interval.cast_refusal_total", 0x0004, 0x02, 0x0f, IntervalMetricReasonV3::cast_forbidden},
    {{IntervalDiagnosticAxisV3::cast_invalid, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x02,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x02})}, IntervalMetricEvidenceTypeV3::cast_refusal_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb3}), "sys.metrics.datatype.base_interval.cast_refusal_total", 0x0004, 0x02, 0x0f, IntervalMetricReasonV3::invalid_literal},
    {{IntervalDiagnosticAxisV3::cast_range_loss, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x04,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x04})}, IntervalMetricEvidenceTypeV3::cast_refusal_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb3}), "sys.metrics.datatype.base_interval.cast_refusal_total", 0x0004, 0x02, 0x0f, IntervalMetricReasonV3::range_exceeded},
    {{IntervalDiagnosticAxisV3::cast_range_loss, Uuid({0xb3,0x99,0xb5,0x14,0xf7,0x71,0x50,0x14,0xa5,0xb4,0x90,0x77,0xf5,0xde,0xe3,0x69})}, IntervalMetricEvidenceTypeV3::cast_refusal_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb3}), "sys.metrics.datatype.base_interval.cast_refusal_total", 0x0004, 0x02, 0x0f, IntervalMetricReasonV3::validation_failure},
    {{IntervalDiagnosticAxisV3::operation_invalid, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x0c,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0c})}, IntervalMetricEvidenceTypeV3::temporal_application_refusal_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb6}), "sys.metrics.datatype.base_interval.temporal_application_refusal_total", 0x0800, 0x01, 0x0f, IntervalMetricReasonV3::calendar_operation_refused},
    {{IntervalDiagnosticAxisV3::operation_invalid, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x06,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x06})}, IntervalMetricEvidenceTypeV3::ordering_refusal_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb5}), "sys.metrics.datatype.base_interval.ordering_refusal_total", 0x0400, 0x01, 0x0f, IntervalMetricReasonV3::ordering_refused},
    {{IntervalDiagnosticAxisV3::bounds_overflow_underflow, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x04,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x04})}, IntervalMetricEvidenceTypeV3::arithmetic_overflow_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb4}), "sys.metrics.datatype.base_interval.arithmetic_overflow_total", 0x00e8, 0x01, 0x0f, IntervalMetricReasonV3::range_exceeded},
    {{IntervalDiagnosticAxisV3::resource_cancellation, Uuid({0xde,0xdc,0x1c,0x9b,0x38,0x79,0x57,0x70,0x8f,0x8e,0xc0,0xc9,0x27,0xa0,0xc3,0x4d})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x0001, 0x01, 0x0f, IntervalMetricReasonV3::resource_failure},
    {{IntervalDiagnosticAxisV3::resource_cancellation, Uuid({0x0f,0x42,0x5d,0xe2,0x6b,0xc7,0x51,0xd4,0x8d,0x34,0x72,0xd1,0x07,0xad,0x45,0xe7})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x0001, 0x01, 0x0f, IntervalMetricReasonV3::cancelled},
    {{IntervalDiagnosticAxisV3::compression_corruption, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x09,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x09})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x4000, 0x01, 0x0f, IntervalMetricReasonV3::protection_unsupported},
    {{IntervalDiagnosticAxisV3::compression_corruption, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x03,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x03})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x4000, 0x01, 0x0f, IntervalMetricReasonV3::canonical_encoding_invalid},
    {{IntervalDiagnosticAxisV3::encryption_auth_key, Uuid({0x1a,0x8d,0x26,0xb0,0x87,0x55,0x56,0xad,0xa1,0x70,0xb2,0x4a,0x3a,0xaf,0x14,0x07})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x4000, 0x01, 0x0f, IntervalMetricReasonV3::validation_failure},
    {{IntervalDiagnosticAxisV3::encryption_auth_key, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x09,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x09})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x4000, 0x01, 0x0f, IntervalMetricReasonV3::protection_unsupported},
    {{IntervalDiagnosticAxisV3::wire_decode_encode, Uuid({0xf7,0xc7,0x36,0x87,0xc7,0x8d,0x54,0x6a,0xbc,0x93,0x02,0x8b,0x52,0xb4,0x35,0xcf})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x2000, 0x18, 0x0f, IntervalMetricReasonV3::validation_failure},
    {{IntervalDiagnosticAxisV3::wire_decode_encode, Uuid({0x26,0x0f,0x5e,0xac,0xb2,0x86,0x5a,0x00,0x81,0x6c,0xc4,0xc2,0xa8,0xf1,0xa2,0x5e})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x2000, 0x18, 0x0f, IntervalMetricReasonV3::validation_failure},
    {{IntervalDiagnosticAxisV3::wire_decode_encode, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x03,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x03})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x2000, 0x18, 0x0f, IntervalMetricReasonV3::canonical_encoding_invalid},
    {{IntervalDiagnosticAxisV3::wire_decode_encode, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x08,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x08})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x2000, 0x18, 0x0f, IntervalMetricReasonV3::validation_failure},
    {{IntervalDiagnosticAxisV3::wire_decode_encode, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x0d,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0d})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x2000, 0x18, 0x0f, IntervalMetricReasonV3::transport_unsupported},
    {{IntervalDiagnosticAxisV3::index_key, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x06,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x06})}, IntervalMetricEvidenceTypeV3::ordering_refusal_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb5}), "sys.metrics.datatype.base_interval.ordering_refusal_total", 0x0400, 0x01, 0x0f, IntervalMetricReasonV3::ordering_refused},
    {{IntervalDiagnosticAxisV3::index_key, Uuid({0x01,0xa1,0x0c,0x00,0x20,0x00,0x70,0x0d,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0d})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x0001, 0x01, 0x0f, IntervalMetricReasonV3::validation_failure},
    {{IntervalDiagnosticAxisV3::index_key, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x07,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x07})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x0001, 0x01, 0x0f, IntervalMetricReasonV3::validation_failure},
    {{IntervalDiagnosticAxisV3::domain_validation, Uuid({0x58,0x8f,0xaf,0xd0,0x63,0x86,0x55,0xcf,0x84,0x4e,0xf5,0xb0,0xdc,0x22,0xc7,0x00})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x0001, 0x01, 0x0f, IntervalMetricReasonV3::validation_failure},
    {{IntervalDiagnosticAxisV3::domain_validation, Uuid({0xab,0xa0,0x82,0x8d,0x77,0xc9,0x50,0xde,0x98,0x42,0x7f,0x20,0x47,0x10,0x02,0x14})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x0001, 0x01, 0x0f, IntervalMetricReasonV3::validation_failure},
    {{IntervalDiagnosticAxisV3::domain_validation, Uuid({0x62,0xbe,0x50,0x52,0xad,0xbc,0x59,0xaa,0xb8,0x4b,0xae,0x60,0xa8,0xc4,0x0c,0x0f})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x0001, 0x01, 0x0f, IntervalMetricReasonV3::validation_failure},
    {{IntervalDiagnosticAxisV3::recovery_corruption, Uuid({0x0c,0x7d,0x9a,0xaf,0x92,0xe6,0x59,0x05,0x9d,0xaf,0x09,0x74,0xb8,0x71,0xf9,0x4a})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x1000, 0x04, 0x0f, IntervalMetricReasonV3::validation_failure},
    {{IntervalDiagnosticAxisV3::recovery_corruption, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x03,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x03})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x1000, 0x04, 0x0f, IntervalMetricReasonV3::canonical_encoding_invalid},
    {{IntervalDiagnosticAxisV3::recovery_corruption, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x0e,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0e})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x1000, 0x04, 0x0f, IntervalMetricReasonV3::validation_failure},
    {{IntervalDiagnosticAxisV3::statistics_read, Uuid({0x1a,0x8d,0x26,0xb0,0x87,0x55,0x56,0xad,0xa1,0x70,0xb2,0x4a,0x3a,0xaf,0x14,0x07})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x0001, 0x01, 0x0f, IntervalMetricReasonV3::validation_failure},
    {{IntervalDiagnosticAxisV3::statistics_read, Uuid({0x01,0xa1,0x0c,0x00,0x20,0x00,0x70,0x0c,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0c})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x0001, 0x01, 0x0f, IntervalMetricReasonV3::validation_failure},
    {{IntervalDiagnosticAxisV3::statistics_read, Uuid({0x01,0xa1,0x0c,0x00,0x20,0x00,0x70,0x0b,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0b})}, IntervalMetricEvidenceTypeV3::validation_failure_total, Uuid({0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xb2}), "sys.metrics.datatype.base_interval.validation_failure_total", 0x0001, 0x01, 0x0f, IntervalMetricReasonV3::validation_failure},
  }};
  return rows;
}

IntervalDiagnosticMetricRouteResultV3 ResolveIntervalDiagnosticMetricRouteV3(
    const IntervalDiagnosticRouteKeyV3& key) noexcept {
  IntervalDiagnosticMetricRouteResultV3 result;
  if (!ValidRouteKey(key)) return result;
  for (const auto& route : IntervalDiagnosticMetricRoutesV3()) {
    if (route.key == key) {
      result.disposition = IntervalDiagnosticMetricRouteDispositionV3::admitted;
      result.route = route; result.evidence_type_selected = true; return result;
    }
  }
  result.disposition = IntervalDiagnosticMetricRouteDispositionV3::missing;
  return result;
}

IntervalDiagnosticMetricRouteDispositionV3 ValidateIntervalDiagnosticMetricRouteTableV3(
    std::span<const IntervalDiagnosticMetricRouteV3> supplied,
    bool scalar_types_valid) noexcept {
  using D = IntervalDiagnosticMetricRouteDispositionV3;
  if (!scalar_types_valid) return D::wrong_type;
  const auto expected = IntervalDiagnosticMetricRoutesV3();
  if (supplied.size() < expected.size()) return D::missing;
  if (supplied.size() > expected.size()) return D::unknown;
  for (std::size_t i = 0; i < supplied.size(); ++i)
    for (std::size_t j = 0; j < i; ++j)
      if (supplied[i].key == supplied[j].key) return D::duplicate;
  for (std::size_t i = 0; i < supplied.size(); ++i) {
    if (SameRoute(supplied[i], expected[i])) continue;
    for (std::size_t j = 0; j < expected.size(); ++j)
      if (supplied[i].key == expected[j].key)
        return i == j ? D::wrong_type : D::reordered;
    return D::unknown;
  }
  return D::admitted;
}

bool IsIntervalMetricLabelTupleAdmittedV3(
    const IntervalDiagnosticMetricRouteV3& route,
    const IntervalMetricClosedLabelTupleV3& labels) noexcept {
  const auto operation = static_cast<unsigned>(labels.operation);
  const auto source = static_cast<unsigned>(labels.source_family);
  const auto state = static_cast<unsigned>(labels.state);
  if (operation >= 15 || source >= 6 || state >= 4 ||
      static_cast<unsigned>(labels.reason) >= 15 ||
      labels.reason == IntervalMetricReasonV3::success) return false;
  return (route.permitted_operation_mask & (std::uint16_t{1} << operation)) != 0 &&
      (route.permitted_source_family_mask & (std::uint8_t{1} << source)) != 0 &&
      (route.permitted_state_mask & (std::uint8_t{1} << state)) != 0 &&
      labels.reason == route.reason;
}

IntervalMetricEvidenceProductionResultV3 ProduceIntervalMetricEvidenceV3(
    const IntervalValidatedProfileHandleV3& profile,
    const IntervalMetricEvidenceProductionRequestV3& request) noexcept {
  using D = IntervalMetricEvidenceProductionDispositionV3;
  IntervalMetricEvidenceProductionResultV3 result;
  if (!ValidProfile(profile)) { result.disposition = D::profile_refused; return result; }
  const auto resolved = ResolveIntervalDiagnosticMetricRouteV3(request.selected_final_diagnostic);
  if (resolved.disposition != IntervalDiagnosticMetricRouteDispositionV3::admitted) {
    result.disposition = D::route_mismatch; return result;
  }
  if (request.source_event_uuid.is_nil() || request.database_uuid.is_nil() ||
      request.node_uuid.is_nil() || request.process_epoch == 0 ||
      request.event_sequence == 0 || request.monotonic_timestamp_ns == 0) {
    result.disposition = D::missing_field; return result;
  }
  const IntervalMetricClosedLabelTupleV3 labels{
      request.operation, request.source_family, request.state, resolved.route.reason};
  if (!IsIntervalMetricLabelTupleAdmittedV3(resolved.route, labels)) {
    result.disposition = D::undeclared_label_tuple; return result;
  }
  const auto index = static_cast<std::size_t>(resolved.route.type);
  result.evidence = {kMetricIds[index], resolved.route.metric_uuid,
      request.source_event_uuid, request.database_uuid, request.node_uuid,
      request.process_epoch, request.event_sequence, request.monotonic_timestamp_ns,
      1, labels, true, true, true};
  result.disposition = D::admitted;
  result.receiving_owner_evidence_handoff_ready = true;
  return result;
}

IntervalMetricEvidenceValidationResultV3 ValidateIntervalMetricEvidenceV3(
    const IntervalValidatedProfileHandleV3& profile,
    const IntervalDiagnosticRouteKeyV3& selected,
    const IntervalMetricEvidenceV3& evidence,
    const IntervalMetricEvidenceValidationContextV3& context) noexcept {
  using D = IntervalMetricEvidenceDispositionV3;
  IntervalMetricEvidenceValidationResultV3 result;
  if (!ValidProfile(profile)) { result.disposition = D::profile_refused; return result; }
  if (!evidence.all_required_fields_present || evidence.evidence_type_uuid.is_nil() ||
      evidence.metric_uuid.is_nil() || evidence.source_event_uuid.is_nil() ||
      evidence.database_uuid.is_nil() || evidence.node_uuid.is_nil() ||
      evidence.process_epoch == 0 || evidence.event_sequence == 0 ||
      evidence.monotonic_timestamp_ns == 0) {
    result.disposition = D::missing_field; return result;
  }
  if (!evidence.scalar_types_valid) { result.disposition = D::wrong_scalar; return result; }
  const auto resolved = ResolveIntervalDiagnosticMetricRouteV3(selected);
  if (resolved.disposition != IntervalDiagnosticMetricRouteDispositionV3::admitted) {
    result.disposition = D::route_mismatch; return result;
  }
  const auto descriptors = IntervalMetricEvidenceTypesV3();
  const IntervalMetricEvidenceTypeDescriptorV3* descriptor = nullptr;
  for (const auto& candidate : descriptors)
    if (candidate.evidence_type_uuid == evidence.evidence_type_uuid) { descriptor = &candidate; break; }
  if (descriptor == nullptr) { result.disposition = D::unknown_evidence_type; return result; }
  result.type = descriptor->type;
  if (evidence.metric_uuid != descriptor->metric_uuid ||
      evidence.metric_uuid != resolved.route.metric_uuid) {
    result.disposition = D::metric_identity_mismatch; return result;
  }
  if (descriptor->type != resolved.route.type) { result.disposition = D::route_mismatch; return result; }
  if (!IsIntervalMetricLabelTupleAdmittedV3(resolved.route, evidence.labels)) {
    result.disposition = D::undeclared_label_tuple; return result;
  }
  if (!evidence.outcome_committed) { result.disposition = D::outcome_not_committed; return result; }
  if (evidence.counter_delta != 1) { result.disposition = D::invalid_counter_delta; return result; }
  if (context.current_process_epoch == 0 ||
      context.current_process_epoch != evidence.process_epoch) {
    result.disposition = D::stale_process_epoch; return result;
  }
  if (context.current_counter_value == std::numeric_limits<u64>::max()) {
    result.disposition = D::counter_overflow; return result;
  }
  result.disposition = D::admitted;
  result.idempotency = {evidence.process_epoch, evidence.event_sequence,
                        evidence.source_event_uuid, evidence.evidence_type_uuid};
  result.receiving_owner_update_admitted = true;
  return result;
}

IntervalMetricReplayFactV3 ClassifyIntervalMetricReplayV3(
    const IntervalMetricEvidenceV3& first,
    const IntervalMetricEvidenceV3& second) noexcept {
  using D = IntervalMetricReplayDispositionV3;
  IntervalMetricReplayFactV3 result;
  if (first.process_epoch == 0 || second.process_epoch == 0 ||
      first.process_epoch != second.process_epoch) {
    result.disposition = D::stale_process_epoch_refused; return result;
  }
  if (first.source_event_uuid == second.source_event_uuid) {
    if (first.evidence_type_uuid != second.evidence_type_uuid) {
      result.disposition = D::distinct_type_same_source_refused; return result;
    }
    if (first == second) {
      result.disposition = D::exact_duplicate_idempotent;
      result.idempotent_without_increment = true;
      return result;
    }
    result.disposition = D::same_identity_payload_conflict; return result;
  }
  if (first.database_uuid != second.database_uuid || first.node_uuid != second.node_uuid) {
    result.disposition = D::independent_source_admitted;
    result.second_emission_admitted = true;
    result.receiving_owner_update_admitted = true;
    return result;
  }
  if (second.event_sequence <= first.event_sequence) {
    result.disposition = D::replay_or_nonmonotonic_sequence_refused; return result;
  }
  result.disposition = D::independent_source_admitted;
  result.second_emission_admitted = true;
  result.receiving_owner_update_admitted = true;
  return result;
}

}  // namespace scratchbird::core::datatypes
