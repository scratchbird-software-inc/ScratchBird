// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "datatype_timestamp_projection.hpp"

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
constexpr byte kZoneMagic[8] = {'S','B','T','S','P','Z','0','1'};
constexpr byte kStatisticsMagic[8] = {'S','B','T','S','T','S','0','1'};
constexpr byte kBackupMagic[8] = {'S','B','T','S','P','B','0','1'};
constexpr char kZoneDomain[] = "ScratchBird.BaseTimestamp.ZoneMap.V1";
constexpr char kStatisticsDomain[] = "ScratchBird.BaseTimestamp.StatisticsRecords.V1";
constexpr char kBackupDomain[] = "ScratchBird.BaseTimestamp.BackupTuple.V1";

constexpr platform::Uuid Uuid(std::array<byte,16> b) noexcept { return {b}; }
constexpr platform::Uuid kPrivacyUuid = Uuid({
    0x01,0xa1,0x04,0xec,0x8e,0x3c,0x7d,0xff,
    0x8e,0x89,0x86,0x8e,0xcc,0x2a,0x8a,0x17});

Status Ok() noexcept { return {StatusCode::ok, Severity::info, Subsystem::datatypes}; }
Status Error() noexcept { return {StatusCode::platform_required_feature_missing,
                                  Severity::error, Subsystem::datatypes}; }
Status BudgetError() noexcept { return {StatusCode::memory_allocation_failed,
                                        Severity::error, Subsystem::datatypes}; }
bool Cancelled(const TimestampExecutionControlV3& c) noexcept {
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

TimestampBytesResultV3 FailBytes(std::string_view code,
                            std::string_view detail = {}) noexcept {
  TimestampBytesResultV3 r;
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
TimestampIndexProjectionResultV3 FailIndex(TimestampIndexResolutionV3 resolution,
                                      std::string_view code,
                                      std::string_view detail = {}) noexcept {
  TimestampIndexProjectionResultV3 r; r.resolution=resolution;
  r.requested_resolution=resolution;
  r.status=code=="RESOURCE.BUDGET_EXCEEDED"?BudgetError():Error();
  r.diagnostic = {r.status, code, detail};
  return r;
}
TimestampIndexProjectionResultV3 OwnerFact(TimestampIndexResolutionV3 r) noexcept {
  TimestampIndexProjectionResultV3 out; out.status=Error(); out.resolution=r;
  out.requested_resolution=r;
  out.diagnostic = {out.status, "OPTIMIZER.INDEX_COMPATIBILITY_MISSING",
                    "conditional_receiving_owner_selection"};
  out.owner_fact_only=true; return out;
}

bool SameResolution(const TimestampIndexResolutionV3& a,
                    const TimestampIndexResolutionV3& b) noexcept {
  return a.family==b.family && a.compatibility_uuid==b.compatibility_uuid &&
      a.compatibility_generation==b.compatibility_generation &&
      a.disposition==b.disposition && a.projection==b.projection &&
      a.exact_recheck_required==b.exact_recheck_required &&
      a.receiving_owner_resolution_required==b.receiving_owner_resolution_required;
}
bool ValidFamily(TimestampIndexFamilyV3 family) noexcept {
  return static_cast<unsigned>(family) < 16;
}
bool ModeAllowed(TimestampIndexFamilyV3 family, TimestampProjectionModeV3 mode) noexcept {
  using F = TimestampIndexFamilyV3;
  using M = TimestampProjectionModeV3;
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

void SecureClearBytes(void* pointer, std::size_t bytes) noexcept {
  auto* output = static_cast<volatile byte*>(pointer);
  while (bytes != 0) { *output++ = 0; --bytes; }
}

class ScopedSecureClear final {
 public:
  ScopedSecureClear(void* pointer, std::size_t bytes,
                    TimestampScrubClassV3 scrub_class = TimestampScrubClassV3::count,
                    const TimestampExecutionControlV3* control = nullptr) noexcept
      : pointer_(pointer), bytes_(bytes), scrub_class_(scrub_class),
        observer_(control == nullptr ? nullptr : control->observe_scrubbed),
        observer_context_(control == nullptr ? nullptr
                                             : control->scrub_observer_context) {}
  ScopedSecureClear(const ScopedSecureClear&) = delete;
  ScopedSecureClear& operator=(const ScopedSecureClear&) = delete;
  ~ScopedSecureClear() {
    SecureClearBytes(pointer_, bytes_);
    if (observer_ != nullptr && scrub_class_ != TimestampScrubClassV3::count)
      observer_(observer_context_, scrub_class_,
                static_cast<const byte*>(pointer_), bytes_);
  }
 private:
  void* pointer_;
  std::size_t bytes_;
  TimestampScrubClassV3 scrub_class_;
  void (*observer_)(void*, TimestampScrubClassV3, const byte*, u64) noexcept;
  void* observer_context_;
};

template <std::size_t N>
class ArrayClearGuard {
 public:
  explicit ArrayClearGuard(
      std::array<byte, N>* value,
      TimestampScrubClassV3 scrub_class = TimestampScrubClassV3::count,
      const TimestampExecutionControlV3* control = nullptr) noexcept
      : value_(value), scrub_class_(scrub_class), control_(control) {}
  ~ArrayClearGuard() {
    if (value_ == nullptr) return;
    SecureClearBytes(value_->data(), value_->size());
    if (control_ != nullptr && control_->observe_scrubbed != nullptr &&
        scrub_class_ != TimestampScrubClassV3::count)
      control_->observe_scrubbed(control_->scrub_observer_context,
                                 scrub_class_, value_->data(), value_->size());
  }
 private:
  std::array<byte, N>* value_;
  TimestampScrubClassV3 scrub_class_;
  const TimestampExecutionControlV3* control_;
};

struct HashSegment {
  const byte* data = nullptr;
  std::size_t size = 0;
};

bool Sha256Stack(std::span<const HashSegment> segments,
                 std::array<byte,32>* out,
                 const TimestampExecutionControlV3* control) noexcept {
  if (out == nullptr) return false;
  std::array<u32,8> state{{0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                           0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u}};
  ScopedSecureClear clear_state(state.data(),state.size()*sizeof(u32),
                                TimestampScrubClassV3::sha_state,control);
  std::array<byte,128> tail{};
  ScopedSecureClear clear_tail(tail.data(),tail.size(),
                               TimestampScrubClassV3::sha_tail,control);
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
    ScopedSecureClear clear_words(words.data(),words.size()*sizeof(u32),
                                  TimestampScrubClassV3::sha_schedule,control);
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
  };
  u64 total=0;
  std::size_t buffered=0;
  for(const auto& segment:segments){
    if(segment.size!=0&&segment.data==nullptr)return false;
    constexpr u64 kMaximumSha256InputBytes=
        std::numeric_limits<u64>::max()/8;
    if(segment.size>kMaximumSha256InputBytes-total)return false;
    total+=static_cast<u64>(segment.size);
    const byte* input=segment.data;
    std::size_t remaining=segment.size;
    if(buffered!=0){
      const std::size_t take=std::min<std::size_t>(64-buffered,remaining);
      if(take!=0){std::memcpy(tail.data()+buffered,input,take);input+=take;}
      buffered+=take;remaining-=take;
      if(buffered==64){transform(tail.data());SecureClearBytes(tail.data(),64);buffered=0;}
    }
    while(remaining>=64){transform(input);input+=64;remaining-=64;}
    if(remaining!=0){std::memcpy(tail.data(),input,remaining);buffered=remaining;}
  }
  tail[buffered]=0x80;
  const std::size_t padded=buffered<56?64:128;
  const u64 bits=total*8;
  for(unsigned i=0;i<8;++i)tail[padded-1-i]=static_cast<byte>(bits>>(i*8));
  transform(tail.data());
  if(padded==128)transform(tail.data()+64);
  for(unsigned i=0;i<8;++i){
    (*out)[i*4]=static_cast<byte>(state[i]>>24);
    (*out)[i*4+1]=static_cast<byte>(state[i]>>16);
    (*out)[i*4+2]=static_cast<byte>(state[i]>>8);
    (*out)[i*4+3]=static_cast<byte>(state[i]);
  }
  return true;
}

bool HashRecord(std::string_view domain, std::span<const byte> record,
                std::size_t zero_offset, std::array<byte,32>* out,
                const TimestampExecutionControlV3* control = nullptr) noexcept {
  if (!out || zero_offset+32>record.size()) return false;
  const HashSegment parts[]{
    {reinterpret_cast<const byte*>(domain.data()),domain.size()},
    {record.data(),zero_offset},{kZero32,32},
    {record.data()+zero_offset+32,record.size()-zero_offset-32}};
  return Sha256Stack(parts,out,control);
}
bool HashEquals(std::string_view domain,std::span<const byte> record,
                std::size_t offset,
                const TimestampExecutionControlV3* control = nullptr) noexcept {
  std::array<byte,32> h{};
  ArrayClearGuard<32> clear(&h,TimestampScrubClassV3::hash_digest,control);
  return HashRecord(domain,record,offset,&h,control) &&
      std::memcmp(h.data(),record.data()+offset,32)==0;
}

void PutCommonIdentity(byte* p,const TimestampValidatedProfileHandleV3& h) noexcept {
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
                          const TimestampValidatedProfileHandleV3& h) noexcept {
  const auto& i=h.identity.legacy_fields;
  return LoadUuid(p)==h.receipt.catalog_snapshot_uuid &&
      LoadLittle64(p+16)==h.receipt.catalog_generation &&
      LoadLittle64(p+24)==h.receipt.registry_generation &&
      LoadUuid(p+32)==i.descriptor_uuid && LoadLittle64(p+48)==i.descriptor_generation &&
      LoadUuid(p+56)==i.type_uuid && LoadLittle64(p+72)==i.type_generation &&
      LoadUuid(p+80)==i.codec_uuid && LoadLittle32(p+96)==i.codec_version &&
      LoadLittle32(p+100)==0 && LoadLittle64(p+104)==i.codec_generation;
}
bool ValidProfile(
    const TimestampValidatedProfileHandleV3& h,
    const TimestampExecutionControlV3* control = nullptr) noexcept {
  if(control==nullptr)return ValidateTimestampProfileHandleV3(h).ok();
  auto scrub_control=*control;
  scrub_control.cancelled=nullptr;
  scrub_control.cancellation_context=nullptr;
  return ValidateTimestampProfileHandleV3(h,scrub_control).ok();
}
bool SameProfile(const TimestampValidatedProfileHandleV3& a,
                 const TimestampValidatedProfileHandleV3& b) noexcept {
  return a.profile_material==b.profile_material &&
      a.comparison_material==b.comparison_material &&
      a.profile_fingerprint==b.profile_fingerprint &&
      a.comparison_fingerprint==b.comparison_fingerprint;
}

class VectorClearGuard {
 public:
  explicit VectorClearGuard(
      std::vector<byte>* value,
      const TimestampExecutionControlV3* control = nullptr) noexcept
      : value_(value), control_(control) {}
  ~VectorClearGuard() {
    if (!active_ || value_ == nullptr || value_->empty()) return;
    SecureClearBytes(value_->data(),value_->size());
    if(control_!=nullptr&&control_->observe_scrubbed!=nullptr)
      control_->observe_scrubbed(control_->scrub_observer_context,
          TimestampScrubClassV3::projection_owned_buffer,value_->data(),value_->size());
    value_->clear();
  }
  void Disarm() noexcept { active_ = false; }
 private:
  std::vector<byte>* value_;
  const TimestampExecutionControlV3* control_;
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
  explicit DecodedStatisticsClearGuard(
      TimestampDecodedStatisticsV3* value,
      const TimestampExecutionControlV3* control = nullptr) noexcept
      : value_(value), control_(control) {}
  ~DecodedStatisticsClearGuard(){
    if(!active_||!value_)return;
    if(!value_->histogram.empty()){
      const auto extent=value_->histogram.size()*sizeof(TimestampStatisticsHistogramRecordV3);
      SecureClearBytes(value_->histogram.data(),extent);
      if(control_!=nullptr&&control_->observe_scrubbed!=nullptr)
        control_->observe_scrubbed(control_->scrub_observer_context,
            TimestampScrubClassV3::projection_owned_buffer,
            reinterpret_cast<const byte*>(value_->histogram.data()),extent);
      value_->histogram.clear();
    }
    if(!value_->mcv.empty()){
      const auto extent=value_->mcv.size()*sizeof(TimestampStatisticsMcvRecordV3);
      SecureClearBytes(value_->mcv.data(),extent);
      if(control_!=nullptr&&control_->observe_scrubbed!=nullptr)
        control_->observe_scrubbed(control_->scrub_observer_context,
            TimestampScrubClassV3::projection_owned_buffer,
            reinterpret_cast<const byte*>(value_->mcv.data()),extent);
      value_->mcv.clear();
    }
    ClearPlainObject(&value_->statistics_uuid);ClearPlainObject(&value_->source_object_uuid);
    ClearPlainObject(&value_->provider_evidence_uuid);value_->schema_epoch=0;
    value_->collection_epoch=0;value_->security_epoch=0;value_->row_count=0;
    value_->null_count=0;value_->value_count=0;value_->minimum_maximum_present=false;
    value_->minimum_civil_day=0;value_->minimum_nanoseconds_since_midnight=0;
    value_->maximum_civil_day=0;value_->maximum_nanoseconds_since_midnight=0;
    value_->profile.reset();
  }
  void Disarm() noexcept{active_=false;}
 private:
  TimestampDecodedStatisticsV3* value_;
  const TimestampExecutionControlV3* control_;
  bool active_=true;
};

bool Reserve(std::vector<byte>* out,u64 n) noexcept {
  if (!out || n>std::numeric_limits<std::size_t>::max()) return false;
  try { out->assign(static_cast<std::size_t>(n),0); return true; }
  catch (...) { return false; }
}

void EncodeSortKeyRaw(const TimestampValueViewV3& value,
                      TimestampSortDirectionV3 direction,
                      TimestampNullModeV3 null_mode, byte* output) noexcept {
  std::memcpy(output, "SBTSPK01", 8);
  StoreUuid(output + 8, value.profile->receipt.catalog_snapshot_uuid);
  StoreLittle64(output + 24, value.profile->receipt.catalog_generation);
  StoreLittle64(output + 32, value.profile->receipt.registry_generation);
  std::memcpy(output + 40, value.profile->comparison_fingerprint.data(), 32);
  StoreUuid(output + 72, value.profile->identity.ordering_policy.uuid);
  StoreLittle64(output + 88,
                value.profile->identity.ordering_policy.generation);
  output[96] = direction == TimestampSortDirectionV3::descending ? 1 : 0;
  output[97] = null_mode == TimestampNullModeV3::nulls_last ? 1 : 0;
  output[98] = value.state == TimestampValueStateV3::value ? 1
      : (null_mode == TimestampNullModeV3::nulls_first ? 0 : 2);
  output[99] = value.state == TimestampValueStateV3::value ? 12 : 0;
  if (value.state == TimestampValueStateV3::value) {
    const std::int64_t civil_second =
        static_cast<std::int64_t>(value.civil_day) * 86'400ll +
        static_cast<std::int64_t>(value.nanoseconds_since_midnight /
                                  1'000'000'000ull);
    const u64 sortable = static_cast<u64>(civil_second) ^ 0x8000000000000000ull;
    for (unsigned i = 0; i != 8; ++i)
      output[100 + i] = static_cast<byte>(sortable >> (56u - 8u * i));
    const u32 nanosecond_of_second = static_cast<u32>(
        value.nanoseconds_since_midnight % 1'000'000'000ull);
    output[108] = static_cast<byte>(nanosecond_of_second >> 24);
    output[109] = static_cast<byte>(nanosecond_of_second >> 16);
    output[110] = static_cast<byte>(nanosecond_of_second >> 8);
    output[111] = static_cast<byte>(nanosecond_of_second);
    if (direction == TimestampSortDirectionV3::descending)
      for (std::size_t i = 100; i != 112; ++i)
        output[i] = static_cast<byte>(~output[i]);
  }
}

void EncodeComponentRaw(const TimestampValueViewV3& value, byte* output) noexcept {
  const std::int64_t civil_second =
      static_cast<std::int64_t>(value.civil_day) * 86'400ll +
      static_cast<std::int64_t>(value.nanoseconds_since_midnight /
                                1'000'000'000ull);
  StoreLittle64(output, static_cast<u64>(civil_second));
  StoreLittle32(output + 8, static_cast<u32>(
      value.nanoseconds_since_midnight % 1'000'000'000ull));
  StoreLittle32(output + 12, 0);
}

int CompareTuple(std::int32_t left_day, u64 left_time,
                 std::int32_t right_day, u64 right_time) noexcept {
  if (left_day != right_day) return left_day < right_day ? -1 : 1;
  if (left_time != right_time) return left_time < right_time ? -1 : 1;
  return 0;
}

bool EncodeHashRaw(const TimestampValueViewV3& value,
                   std::array<byte,32>* output,
                   const TimestampExecutionControlV3* control = nullptr) noexcept {
  if(output==nullptr||value.profile==nullptr)return false;
  std::array<byte,117> preimage{};
  ArrayClearGuard<117> clear_preimage(
      &preimage,TimestampScrubClassV3::hash_preimage,control);
  std::memcpy(preimage.data(),"SBTSPH01",8);
  StoreUuid(preimage.data()+8,value.profile->receipt.catalog_snapshot_uuid);
  StoreLittle64(preimage.data()+24,value.profile->receipt.catalog_generation);
  StoreLittle64(preimage.data()+32,value.profile->receipt.registry_generation);
  std::memcpy(preimage.data()+40,value.profile->comparison_fingerprint.data(),32);
  StoreUuid(preimage.data()+72,value.profile->identity.hash_policy.uuid);
  StoreLittle64(preimage.data()+88,value.profile->identity.hash_policy.generation);
  preimage[96]=value.state==TimestampValueStateV3::sql_null?0:1;
  StoreLittle32(preimage.data()+97,value.state==TimestampValueStateV3::sql_null?0:16);
  std::size_t extent=101;
  if(value.state==TimestampValueStateV3::value){
    EncodeComponentRaw(value, preimage.data() + 101);
    extent=117;
  }
  const HashSegment segment{preimage.data(),extent};
  return Sha256Stack(std::span<const HashSegment>(&segment,1),output,control);
}

TimestampDiagnosticFactV3 ValidateProjectionValueState(
    const TimestampOwnedValueV3& value,bool null_allowed) noexcept {
  if(value.state!=TimestampValueStateV3::value&&
     value.state!=TimestampValueStateV3::sql_null)
    return {Error(),"DATATYPE.NULL_STATE.INVALID","projection_state"};
  if(value.state==TimestampValueStateV3::sql_null){
    if(value.civil_day!=0||value.nanoseconds_since_midnight!=0)
      return {Error(),"DATATYPE.NULL_STATE.INVALID","projection_dirty_null"};
    if(!null_allowed)return {Error(),"DATATYPE.NULL_NOT_ADMITTED","projection_null_not_admitted"};
  }
  return {Ok(),{}, {}};
}
TimestampDiagnosticFactV3 ValidateProjectionPresentPayload(
    const TimestampOwnedValueV3& value) noexcept {
  if(value.state==TimestampValueStateV3::value&&
     value.nanoseconds_since_midnight>kTimestampMaximumNanosecondsV3){
    return {Error(),"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","projection_range"};
  }
  return {Ok(),{}, {}};
}
TimestampDiagnosticFactV3 ValidateProjectionValueMetadata(
    const TimestampOwnedValueV3& value,bool null_allowed,
    const TimestampValidatedProfileHandleV3* expected_profile=nullptr,
    const TimestampExecutionControlV3* control=nullptr) noexcept {
  if(!value.profile||!ValidProfile(*value.profile,control)||
     (expected_profile&&!SameProfile(*value.profile,*expected_profile)))
    return {Error(),"CTI.TEMPORAL.DESCRIPTOR_INVALID","projection_profile"};
  const auto state=ValidateProjectionValueState(value,null_allowed);
  return state.status.ok()?ValidateProjectionPresentPayload(value):state;
}
} // namespace

TimestampIndexResolutionV3 ResolveTimestampIndexFamilyV3(TimestampIndexFamilyV3 family) noexcept {
  using D=TimestampIndexDispositionV3; using P=TimestampProjectionKindV3;
  static constexpr std::array<platform::Uuid,16> ids{{
    Uuid({0x01,0xa1,0x04,0xfb,0x3b,0xb7,0x77,0x16,0x93,0x9b,0x99,0x85,0x45,0xbd,0xbd,0x7c}),
    Uuid({0x01,0xa1,0x04,0xfb,0x3b,0xb7,0x77,0x16,0x93,0x9b,0x99,0x85,0x45,0xbd,0xbd,0x6f}),
    Uuid({0x01,0xa1,0x04,0xfb,0x3b,0xb7,0x77,0x16,0x93,0x9b,0x99,0x85,0x45,0xbd,0xbd,0x70}),
    Uuid({0x01,0xa1,0x04,0xfb,0x3b,0xb7,0x77,0x16,0x93,0x9b,0x99,0x85,0x45,0xbd,0xbd,0x6d}),
    Uuid({0x01,0xa1,0x04,0xfb,0x3b,0xb7,0x77,0x16,0x93,0x9b,0x99,0x85,0x45,0xbd,0xbd,0x71}),
    Uuid({0x01,0xa1,0x04,0xfb,0x3b,0xb7,0x77,0x16,0x93,0x9b,0x99,0x85,0x45,0xbd,0xbd,0x72}),
    Uuid({0x01,0xa1,0x04,0xfb,0x3b,0xb7,0x77,0x16,0x93,0x9b,0x99,0x85,0x45,0xbd,0xbd,0x77}),
    Uuid({0x01,0xa1,0x04,0xfb,0x3b,0xb7,0x77,0x16,0x93,0x9b,0x99,0x85,0x45,0xbd,0xbd,0x73}),
    Uuid({0x01,0xa1,0x04,0xfb,0x3b,0xb7,0x77,0x16,0x93,0x9b,0x99,0x85,0x45,0xbd,0xbd,0x78}),
    Uuid({0x01,0xa1,0x04,0xfb,0x3b,0xb7,0x77,0x16,0x93,0x9b,0x99,0x85,0x45,0xbd,0xbd,0x79}),
    Uuid({0x01,0xa1,0x04,0xfb,0x3b,0xb7,0x77,0x16,0x93,0x9b,0x99,0x85,0x45,0xbd,0xbd,0x6e}),
    Uuid({0x01,0xa1,0x04,0xfb,0x3b,0xb7,0x77,0x16,0x93,0x9b,0x99,0x85,0x45,0xbd,0xbd,0x74}),
    Uuid({0x01,0xa1,0x04,0xfb,0x3b,0xb7,0x77,0x16,0x93,0x9b,0x99,0x85,0x45,0xbd,0xbd,0x75}),
    Uuid({0x01,0xa1,0x04,0xfb,0x3b,0xb7,0x77,0x16,0x93,0x9b,0x99,0x85,0x45,0xbd,0xbd,0x7a}),
    Uuid({0x01,0xa1,0x04,0xfb,0x3b,0xb7,0x77,0x16,0x93,0x9b,0x99,0x85,0x45,0xbd,0xbd,0x76}),
    Uuid({0x01,0xa1,0x04,0xfb,0x3b,0xb7,0x77,0x16,0x93,0x9b,0x99,0x85,0x45,0xbd,0xbd,0x7b})
  }};
  if (!ValidFamily(family)) {
    TimestampIndexResolutionV3 invalid;
    invalid.family = family;
    invalid.disposition = D::not_applicable;
    invalid.receiving_owner_resolution_required = true;
    return invalid;
  }
  TimestampIndexResolutionV3 r{family,ids[static_cast<std::size_t>(family)],1};
  switch(family) {
    case TimestampIndexFamilyV3::bitmap: case TimestampIndexFamilyV3::hash:
      r.disposition=D::admitted_with_recheck;r.projection=P::equality_hash;r.exact_recheck_required=true;break;
    case TimestampIndexFamilyV3::brin_like: case TimestampIndexFamilyV3::columnar_zone_map:
      r.disposition=D::admitted_with_recheck;r.projection=P::zone_map;r.exact_recheck_required=true;break;
    case TimestampIndexFamilyV3::btree:
      r.disposition=D::admitted_exact_if_budget;r.projection=P::sort_key;break;
    case TimestampIndexFamilyV3::covering_included:
      r.disposition=D::admitted_payload_only;r.projection=P::covering_value;break;
    case TimestampIndexFamilyV3::range_exclusion:
      r.disposition=D::admitted_exact_if_budget;r.projection=P::range_pair;break;
    case TimestampIndexFamilyV3::temporary_work:
      r.disposition=D::admitted_exact_selected_mode;r.projection=P::none;break;
    case TimestampIndexFamilyV3::expression:
      r.disposition=D::conditional;r.projection=P::conditional_result;r.exact_recheck_required=true;r.receiving_owner_resolution_required=true;break;
    case TimestampIndexFamilyV3::partial_filtered:
      r.disposition=D::conditional;r.projection=P::conditional_underlying;r.exact_recheck_required=true;r.receiving_owner_resolution_required=true;break;
    default:
      r.disposition=D::not_applicable;r.projection=P::none;r.receiving_owner_resolution_required=true;break;
  }
  return r;
}

TimestampIndexCompatibilityIdentityV3 LookupTimestampIndexCompatibilityIdentityV3(
    TimestampIndexFamilyV3 family) noexcept {
  const auto resolution = ResolveTimestampIndexFamilyV3(family);
  return {family, resolution.compatibility_uuid,
          resolution.compatibility_generation};
}

TimestampIndexAdmissionResultV3 AdmitTimestampIndexCompatibilityV3(
    const TimestampIndexCompatibilityIdentityV3& supplied) noexcept {
  TimestampIndexAdmissionResultV3 result;
  result.resolution.family = supplied.family;
  if (!ValidFamily(supplied.family) || supplied.compatibility_uuid.is_nil() ||
      supplied.compatibility_generation == 0) {
    return result;
  }
  const auto expected = ResolveTimestampIndexFamilyV3(supplied.family);
  if (supplied.compatibility_uuid != expected.compatibility_uuid ||
      supplied.compatibility_generation != expected.compatibility_generation) {
    return result;
  }
  result.admitted = true;
  result.resolution = expected;
  result.diagnostic = {};
  return result;
}

TimestampIndexPredicateResolutionV3 ResolveTimestampIndexPredicateV3(
    const TimestampIndexPredicateRequestV3& request) noexcept {
  using F=TimestampIndexFamilyV3;using O=TimestampIndexPredicateOperationV3;
  using P=TimestampIndexPredicateFactV3;
  TimestampIndexPredicateResolutionV3 result;
  const auto requested = AdmitTimestampIndexCompatibilityV3(request.requested);
  result.requested_resolution = requested.resolution;
  if (!requested.admitted) {
    result.diagnostic = "OPTIMIZER.INDEX_COMPATIBILITY_MISSING";
    return result;
  }
  auto effective = requested.resolution;
  if (effective.disposition == TimestampIndexDispositionV3::conditional) {
    result.predicate_owner_handoff_preserved =
        effective.family == F::partial_filtered;
    if (!request.selected_conditional) {
      result.diagnostic = "OPTIMIZER.INDEX_COMPATIBILITY_MISSING";
      return result;
    }
    const auto selected =
        AdmitTimestampIndexCompatibilityV3(*request.selected_conditional);
    if (!selected.admitted ||
        selected.resolution.disposition == TimestampIndexDispositionV3::conditional ||
        selected.resolution.disposition ==
            TimestampIndexDispositionV3::not_applicable) {
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
      effective.disposition == TimestampIndexDispositionV3::not_applicable
          ? "OPTIMIZER.INDEX_COMPATIBILITY_MISSING"
          : "CTI.TEMPORAL.INDEX_KEY_REFUSED";
  return result;
}

TimestampIndexCandidateFactsV3 ClassifyTimestampIndexCandidateV3(
    TimestampIndexPredicateFactV3 predicate_fact, bool projection_candidate_equal,
    bool exact_state_component_equal,
    bool source_row_predicate_equal) noexcept {
  using D = TimestampIndexCandidateDispositionV3;
  using F = TimestampIndexPredicateFactV3;
  TimestampIndexCandidateFactsV3 result;
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
  if (predicate_fact !=
      F::prune_only_mandatory_source_row_predicate_recheck_never_final_match) {
    result.disposition = D::not_a_candidate;
    return result;
  }
  result.source_row_predicate_recheck_required = true;
  result.final_match = source_row_predicate_equal;
  result.disposition = source_row_predicate_equal
      ? D::exact_match : D::rejected_after_required_recheck;
  return result;
}

TimestampBytesResultV3 EncodeTimestampCoveringValueV3(const TimestampOwnedValueV3& value,
                                             const TimestampExecutionControlV3& c) noexcept {
  const auto pinned_value=value;
  const auto v=ValidateProjectionValueMetadata(pinned_value,true,nullptr,&c);
  if(!v.status.ok())return FailBytes(v.diagnostic_code,v.detail);
  const u64 n=pinned_value.state==TimestampValueStateV3::sql_null?1:17;
  if(n>c.maximum_allocation_bytes) return FailBytes("RESOURCE.BUDGET_EXCEEDED","covering_allocation");
  if(Cancelled(c)) return FailBytes("PROCESS.CANCELLED","covering_cancelled");
  TimestampBytesResultV3 r; VectorClearGuard clear(&r.bytes,&c);
  if(!Reserve(&r.bytes,n)) return FailBytes("RESOURCE.BUDGET_EXCEEDED","covering_allocation");
  r.bytes[0]=pinned_value.state==TimestampValueStateV3::sql_null?0:2;
  if(n==17) EncodeComponentRaw(pinned_value.view(),r.bytes.data()+1);
  auto self_check_control=c;self_check_control.cancelled=nullptr;self_check_control.cancellation_context=nullptr;
  if(!DecodeTimestampCoveringValueNoAllocV3(*pinned_value.profile,true,r.bytes,self_check_control).ok()) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","covering_self_check");
  if(Cancelled(c)) return FailBytes("PROCESS.CANCELLED","covering_prepublication");
  r.status=Ok();clear.Disarm();return r;
}

TimestampCoveringValueViewResultV3 DecodeTimestampCoveringValueNoAllocV3(
    const TimestampValidatedProfileHandleV3& h,bool null_allowed,
    std::span<const byte> e,const TimestampExecutionControlV3& control) noexcept {
  if(e.empty()) return FailView<TimestampCoveringValueViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","minimum_extent");
  if(!ValidProfile(h,&control)) return FailView<TimestampCoveringValueViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","profile");
  TimestampValueStateV3 state=TimestampValueStateV3::value;
  std::int32_t civil_day=0;u64 nanoseconds=0;
  PlainObjectClearGuard<TimestampValueStateV3> clear_state(&state);
  PlainObjectClearGuard<std::int32_t> clear_day(&civil_day);
  PlainObjectClearGuard<u64> clear_value(&nanoseconds);
  if(e[0]==0) { if(e.size()!=1) return FailView<TimestampCoveringValueViewResultV3>("DATATYPE.NULL_STATE.INVALID","null_extent");
    if(!null_allowed) return FailView<TimestampCoveringValueViewResultV3>("DATATYPE.NULL_NOT_ADMITTED");
    state=TimestampValueStateV3::sql_null;
  } else if(e[0]==2) { if(e.size()!=17) return FailView<TimestampCoveringValueViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","value_extent");
    auto nested_control=control;
    nested_control.maximum_allocation_bytes=~u64{0};
    nested_control.cancelled=nullptr;
    nested_control.cancellation_context=nullptr;
    auto decoded=DecodeCanonicalTimestampComponentNoAllocV3(
        h,TimestampValueStateV3::value,false,e.subspan(1),nested_control);
    if(!decoded.ok())return FailView<TimestampCoveringValueViewResultV3>(
        decoded.diagnostic.diagnostic_code,decoded.diagnostic.detail);
    state=TimestampValueStateV3::value;civil_day=decoded.value.civil_day;
    nanoseconds=decoded.value.nanoseconds_since_midnight;
  } else return FailView<TimestampCoveringValueViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","tag");
  std::array<byte,17> canonical{};ArrayClearGuard<17> clear_canonical(
      &canonical,TimestampScrubClassV3::covering_decode_reencode,&control);
  canonical[0]=state==TimestampValueStateV3::sql_null?0:2;
  const std::size_t canonical_extent=state==TimestampValueStateV3::sql_null?1:17;
  if(canonical_extent==17)EncodeComponentRaw({&h,state,civil_day,nanoseconds},canonical.data()+1);
  if(e.size()!=canonical_extent||std::memcmp(e.data(),canonical.data(),canonical_extent))
    return FailView<TimestampCoveringValueViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","covering_reencode");
  if(e.size()>control.maximum_allocation_bytes)
    return FailView<TimestampCoveringValueViewResultV3>("RESOURCE.BUDGET_EXCEEDED","covering_extent_budget");
  if(Cancelled(control))
    return FailView<TimestampCoveringValueViewResultV3>("PROCESS.CANCELLED","before_publication");
  TimestampCoveringValueViewResultV3 r;r.value={&h,state,civil_day,nanoseconds};r.status=Ok();return r;
}

TimestampBytesResultV3 EncodeTimestampRangePairV3(const TimestampOwnedValueV3& lower,
                                        const TimestampOwnedValueV3& upper,
                                        const TimestampExecutionControlV3& c) noexcept {
  auto lower_profile_pin=lower.profile;
  auto upper_profile_pin=upper.profile.get()==lower_profile_pin.get()
      ?std::shared_ptr<const TimestampValidatedProfileHandleV3>{}:upper.profile;
  const auto* upper_profile=upper_profile_pin?upper_profile_pin.get():lower_profile_pin.get();
  const TimestampValueViewV3 lower_value{lower_profile_pin.get(),lower.state,
                                         lower.civil_day,lower.nanoseconds_since_midnight};
  const TimestampValueViewV3 upper_value{upper_profile,upper.state,
                                         upper.civil_day,upper.nanoseconds_since_midnight};
  if(!lower_profile_pin||!ValidProfile(*lower_profile_pin,&c))
    return FailBytes("CTI.TEMPORAL.DESCRIPTOR_INVALID","projection_profile");
  if(!upper_profile||!ValidProfile(*upper_profile,&c)||
     !SameProfile(*lower_profile_pin,*upper_profile))
    return FailBytes("CTI.TEMPORAL.DESCRIPTOR_INVALID","projection_profile");
  const auto lower_state=ValidateProjectionValueState(
      {lower_profile_pin,lower_value.state,lower_value.civil_day,
       lower_value.nanoseconds_since_midnight},false);
  if(!lower_state.status.ok())return FailBytes(lower_state.diagnostic_code,lower_state.detail);
  const auto upper_state=ValidateProjectionValueState(
      {upper_profile_pin?upper_profile_pin:lower_profile_pin,upper_value.state,
       upper_value.civil_day,upper_value.nanoseconds_since_midnight},false);
  if(!upper_state.status.ok())return FailBytes(upper_state.diagnostic_code,upper_state.detail);
  const auto lower_payload=ValidateProjectionPresentPayload(
      {lower_profile_pin,lower_value.state,lower_value.civil_day,
       lower_value.nanoseconds_since_midnight});
  if(!lower_payload.status.ok())return FailBytes(lower_payload.diagnostic_code,lower_payload.detail);
  const auto upper_payload=ValidateProjectionPresentPayload(
      {upper_profile_pin?upper_profile_pin:lower_profile_pin,upper_value.state,
       upper_value.civil_day,upper_value.nanoseconds_since_midnight});
  if(!upper_payload.status.ok())return FailBytes(upper_payload.diagnostic_code,upper_payload.detail);
  if(CompareTuple(lower_value.civil_day,lower_value.nanoseconds_since_midnight,
                  upper_value.civil_day,upper_value.nanoseconds_since_midnight)>0)
    return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","range_order");
  if(224>c.maximum_allocation_bytes) return FailBytes("RESOURCE.BUDGET_EXCEEDED","range_allocation");
  if(Cancelled(c)) return FailBytes("PROCESS.CANCELLED","range_cancelled");
  TimestampBytesResultV3 r;VectorClearGuard clear(&r.bytes,&c);
  if(!Reserve(&r.bytes,224)) return FailBytes("RESOURCE.BUDGET_EXCEEDED","range_allocation");
  EncodeSortKeyRaw(lower_value,TimestampSortDirectionV3::ascending,TimestampNullModeV3::nulls_last,r.bytes.data());
  EncodeSortKeyRaw(upper_value,TimestampSortDirectionV3::ascending,TimestampNullModeV3::nulls_last,r.bytes.data()+112);
  auto self_check_control=c;self_check_control.cancelled=nullptr;self_check_control.cancellation_context=nullptr;
  if(!DecodeTimestampRangePairNoAllocV3(*lower_profile_pin,r.bytes,self_check_control).ok()) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","range_self_check");
  if(Cancelled(c)) return FailBytes("PROCESS.CANCELLED","range_prepublication");
  r.status=Ok();clear.Disarm();return r;
}

TimestampRangePairViewResultV3 DecodeTimestampRangePairNoAllocV3(
    const TimestampValidatedProfileHandleV3& h,std::span<const byte> e,
    const TimestampExecutionControlV3& control) noexcept {
  if(e.size()!=224) return FailView<TimestampRangePairViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","extent");
  auto nested_control=control;nested_control.maximum_allocation_bytes=~u64{0};nested_control.cancelled=nullptr;nested_control.cancellation_context=nullptr;
  auto l=DecodeTimestampSortKeyNoAllocV3(h,e.first(112),nested_control); if(!l.ok()) return FailView<TimestampRangePairViewResultV3>(l.diagnostic.diagnostic_code,l.diagnostic.detail);
  auto u=DecodeTimestampSortKeyNoAllocV3(h,e.subspan(112),nested_control); if(!u.ok()) return FailView<TimestampRangePairViewResultV3>(u.diagnostic.diagnostic_code,u.diagnostic.detail);
  if(l.value.state!=TimestampValueStateV3::value||u.value.state!=TimestampValueStateV3::value||l.value.direction!=TimestampSortDirectionV3::ascending||u.value.direction!=TimestampSortDirectionV3::ascending||l.value.null_mode!=TimestampNullModeV3::nulls_last||u.value.null_mode!=TimestampNullModeV3::nulls_last||CompareTuple(l.value.civil_day,l.value.nanoseconds_since_midnight,u.value.civil_day,u.value.nanoseconds_since_midnight)>0)
    return FailView<TimestampRangePairViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","range_invariant");
  if(e.size()>control.maximum_allocation_bytes)
    return FailView<TimestampRangePairViewResultV3>("RESOURCE.BUDGET_EXCEEDED","range_extent_budget");
  if(Cancelled(control))
    return FailView<TimestampRangePairViewResultV3>("PROCESS.CANCELLED","before_publication");
  TimestampRangePairViewResultV3 r;r.value={l.value.civil_day,l.value.nanoseconds_since_midnight,u.value.civil_day,u.value.nanoseconds_since_midnight,e.first(112),e.subspan(112)};r.status=Ok();return r;
}

static TimestampBytesResultV3 EncodeTimestampZoneMapImpl(
    const TimestampZoneMapRequestV3& input,u64 provider_max_key_bytes,
    const TimestampExecutionControlV3& control,bool acquire_profile_pin) noexcept {
  auto q=input;
  (void)acquire_profile_pin;
  auto minimum_profile_pin=input.minimum&&input.minimum->profile.get()!=q.profile.get()
      ?input.minimum->profile:std::shared_ptr<const TimestampValidatedProfileHandleV3>{};
  auto maximum_profile_pin=input.maximum&&input.maximum->profile.get()!=q.profile.get()&&
      input.maximum->profile.get()!=minimum_profile_pin.get()
      ?input.maximum->profile:std::shared_ptr<const TimestampValidatedProfileHandleV3>{};
  const auto* minimum_profile=minimum_profile_pin?minimum_profile_pin.get():q.profile.get();
  const auto* maximum_profile=maximum_profile_pin?maximum_profile_pin.get():
      (minimum_profile_pin&&input.maximum&&input.maximum->profile.get()==minimum_profile_pin.get()
           ?minimum_profile_pin.get():q.profile.get());
  const TimestampValueViewV3 minimum_value{
      input.minimum?minimum_profile:nullptr,
      input.minimum?input.minimum->state:TimestampValueStateV3::value,
      input.minimum?input.minimum->civil_day:0,
      input.minimum?input.minimum->nanoseconds_since_midnight:0};
  const TimestampValueViewV3 maximum_value{
      input.maximum?maximum_profile:nullptr,
      input.maximum?input.maximum->state:TimestampValueStateV3::value,
      input.maximum?input.maximum->civil_day:0,
      input.maximum?input.maximum->nanoseconds_since_midnight:0};
  if(!q.profile||!ValidProfile(*q.profile,&control)) return FailBytes("CTI.TEMPORAL.DESCRIPTOR_INVALID","zone_profile");
  for(const auto* extremum:{&minimum_value,&maximum_value}){
    if(extremum->profile&&(!ValidProfile(*extremum->profile,&control)||
       !SameProfile(*q.profile,*extremum->profile)))
      return FailBytes("CTI.TEMPORAL.DESCRIPTOR_INVALID","projection_profile");
  }
  if(q.value_count>std::numeric_limits<u64>::max()-q.null_count) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_count_overflow");
  const bool has=q.value_count!=0;
  if ((!has && (q.null_count != 1 || q.value_count != 0)) ||
      (has && (q.null_count != 0 || q.value_count != 2)))
    return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_exact_population");
  if(has && (!minimum_value.profile||!maximum_value.profile)) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_extrema_missing");
  if(!has && (minimum_value.profile||maximum_value.profile)) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_extrema_for_empty");
  if(has) {
    for(const auto* extremum:{&minimum_value,&maximum_value}){
      if(extremum->state!=TimestampValueStateV3::value&&
         extremum->state!=TimestampValueStateV3::sql_null)
        return FailBytes("DATATYPE.NULL_STATE.INVALID","projection_state");
      if(extremum->state==TimestampValueStateV3::sql_null)
        return FailBytes(
            (extremum->civil_day!=0||extremum->nanoseconds_since_midnight!=0)
                ?"DATATYPE.NULL_STATE.INVALID":"DATATYPE.NULL_NOT_ADMITTED",
            "zone_extremum_null");
    }
  }
  const u64 n=has?336:112;
  if(n>provider_max_key_bytes) return FailBytes("CTI.TEMPORAL.INDEX_KEY_REFUSED","zone_provider_budget");
  if(n>control.maximum_allocation_bytes) return FailBytes("RESOURCE.BUDGET_EXCEEDED","zone_allocation");
  if(has){
    const auto lo=ValidateProjectionPresentPayload(
        {q.profile,minimum_value.state,minimum_value.civil_day,
         minimum_value.nanoseconds_since_midnight});
    if(!lo.status.ok())return FailBytes(lo.diagnostic_code,lo.detail);
    const auto hi=ValidateProjectionPresentPayload(
        {q.profile,maximum_value.state,maximum_value.civil_day,
         maximum_value.nanoseconds_since_midnight});
    if(!hi.status.ok())return FailBytes(hi.diagnostic_code,hi.detail);
    if(CompareTuple(minimum_value.civil_day,minimum_value.nanoseconds_since_midnight,
                    maximum_value.civil_day,maximum_value.nanoseconds_since_midnight)>0)
      return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_extrema_order");
  }
  if(Cancelled(control)) return FailBytes("PROCESS.CANCELLED","zone_cancelled");
  TimestampBytesResultV3 r;VectorClearGuard clear(&r.bytes,&control);
  if(!Reserve(&r.bytes,n)) return FailBytes("RESOURCE.BUDGET_EXCEEDED","zone_allocation");
  std::memcpy(r.bytes.data(),kZoneMagic,8);StoreLittle16(r.bytes.data()+8,1);StoreLittle16(r.bytes.data()+10,112);StoreLittle32(r.bytes.data()+12,static_cast<u32>(n));
  StoreLittle32(r.bytes.data()+16,has?5u:4u);StoreLittle64(r.bytes.data()+24,q.null_count);StoreLittle64(r.bytes.data()+32,q.value_count);
  if(has) {
    EncodeComponentRaw(minimum_value,r.bytes.data()+40);
    EncodeComponentRaw(maximum_value,r.bytes.data()+56);
    StoreLittle32(r.bytes.data()+72,112);StoreLittle32(r.bytes.data()+76,112);
    EncodeSortKeyRaw(minimum_value,TimestampSortDirectionV3::ascending,TimestampNullModeV3::nulls_last,r.bytes.data()+112);
    EncodeSortKeyRaw(maximum_value,TimestampSortDirectionV3::ascending,TimestampNullModeV3::nulls_last,r.bytes.data()+224);
  }
  std::array<byte,32> digest{};ArrayClearGuard<32> clear_digest(
      &digest,TimestampScrubClassV3::hash_digest,&control);
  if(!HashRecord(kZoneDomain,r.bytes,80,&digest,&control)) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_hash_provider");
  std::memcpy(r.bytes.data()+80,digest.data(),32);
  auto self_check_control=control;self_check_control.cancelled=nullptr;self_check_control.cancellation_context=nullptr;
  if(!DecodeTimestampZoneMapNoAllocV3(*q.profile,r.bytes,self_check_control).ok()) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_self_check");
  if(Cancelled(control)) return FailBytes("PROCESS.CANCELLED","zone_prepublication");
  r.status=Ok();clear.Disarm();return r;
}

TimestampBytesResultV3 EncodeTimestampZoneMapV3(const TimestampZoneMapRequestV3& q) noexcept {
  return EncodeTimestampZoneMapImpl(q,q.provider_max_key_bytes,q.control,true);
}

TimestampZoneMapViewResultV3 DecodeTimestampZoneMapNoAllocV3(
    const TimestampValidatedProfileHandleV3& h,std::span<const byte> e,
    const TimestampExecutionControlV3& control) noexcept {
  if((e.size()!=112&&e.size()!=336)||std::memcmp(e.data(),kZoneMagic,8)||LoadLittle16(e.data()+8)!=1||LoadLittle16(e.data()+10)!=112||LoadLittle32(e.data()+12)!=e.size()||LoadLittle32(e.data()+20)!=0)
    return FailView<TimestampZoneMapViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_structure");
  const u32 flags=LoadLittle32(e.data()+16);
  if((flags&~u32{5})!=0)
    return FailView<TimestampZoneMapViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_reserved_flags");
  if(!HashEquals(kZoneDomain,e,80,&control)) return FailView<TimestampZoneMapViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_hash");
  if(!ValidProfile(h,&control)) return FailView<TimestampZoneMapViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","profile");
  const u64 nc=LoadLittle64(e.data()+24),vc=LoadLittle64(e.data()+32);
  if(vc>std::numeric_limits<u64>::max()-nc||flags!=(vc?5u:4u)||
     (vc==0&&(nc!=1||vc!=0))||(vc!=0&&(nc!=0||vc!=2)))
    return FailView<TimestampZoneMapViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_counts_flags");
  std::int32_t minimum_day=0,maximum_day=0;
  u64 minimum_nanoseconds=0,maximum_nanoseconds=0;
  PlainObjectClearGuard<std::int32_t> clear_minimum_day(&minimum_day);
  PlainObjectClearGuard<std::int32_t> clear_maximum_day(&maximum_day);
  PlainObjectClearGuard<u64> clear_minimum(&minimum_nanoseconds);
  PlainObjectClearGuard<u64> clear_maximum(&maximum_nanoseconds);
  std::span<const byte> minimum_key,maximum_key;
  if(!vc) {
    if(e.size()!=112||std::any_of(e.begin()+40,e.begin()+72,[](byte b){return b!=0;})||LoadLittle32(e.data()+72)||LoadLittle32(e.data()+76)) return FailView<TimestampZoneMapViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_empty");
  } else {
    if(e.size()!=336||LoadLittle32(e.data()+72)!=112||LoadLittle32(e.data()+76)!=112) return FailView<TimestampZoneMapViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_offsets");
    auto nested_control=control;nested_control.maximum_allocation_bytes=~u64{0};nested_control.cancelled=nullptr;nested_control.cancellation_context=nullptr;
    auto lo_component=DecodeCanonicalTimestampComponentNoAllocV3(h,TimestampValueStateV3::value,false,e.subspan(40,16),nested_control);
    auto hi_component=DecodeCanonicalTimestampComponentNoAllocV3(h,TimestampValueStateV3::value,false,e.subspan(56,16),nested_control);
    if(!lo_component.ok()||!hi_component.ok())return FailView<TimestampZoneMapViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_component");
    auto lo=DecodeTimestampSortKeyNoAllocV3(h,e.subspan(112,112),nested_control);auto hi=DecodeTimestampSortKeyNoAllocV3(h,e.subspan(224,112),nested_control);
    if(!lo.ok())return FailView<TimestampZoneMapViewResultV3>(lo.diagnostic.diagnostic_code,lo.diagnostic.detail);
    if(!hi.ok())return FailView<TimestampZoneMapViewResultV3>(hi.diagnostic.diagnostic_code,hi.diagnostic.detail);
    if(lo.value.state!=TimestampValueStateV3::value||hi.value.state!=TimestampValueStateV3::value||lo.value.direction!=TimestampSortDirectionV3::ascending||hi.value.direction!=TimestampSortDirectionV3::ascending||lo.value.null_mode!=TimestampNullModeV3::nulls_last||hi.value.null_mode!=TimestampNullModeV3::nulls_last)
      return FailView<TimestampZoneMapViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_nested_key");
    if(lo_component.value.civil_day!=lo.value.civil_day||lo_component.value.nanoseconds_since_midnight!=lo.value.nanoseconds_since_midnight||hi_component.value.civil_day!=hi.value.civil_day||hi_component.value.nanoseconds_since_midnight!=hi.value.nanoseconds_since_midnight||CompareTuple(lo.value.civil_day,lo.value.nanoseconds_since_midnight,hi.value.civil_day,hi.value.nanoseconds_since_midnight)>0) return FailView<TimestampZoneMapViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_extrema");
    minimum_day=lo.value.civil_day;minimum_nanoseconds=lo.value.nanoseconds_since_midnight;
    maximum_day=hi.value.civil_day;maximum_nanoseconds=hi.value.nanoseconds_since_midnight;
    minimum_key=e.subspan(112,112);maximum_key=e.subspan(224,112);
  }
  std::array<byte,336> canonical{};ArrayClearGuard<336> clear_canonical(
      &canonical,TimestampScrubClassV3::zone_map_decode_reencode,&control);
  std::memcpy(canonical.data(),kZoneMagic,8);StoreLittle16(canonical.data()+8,1);
  StoreLittle16(canonical.data()+10,112);StoreLittle32(canonical.data()+12,static_cast<u32>(e.size()));
  StoreLittle32(canonical.data()+16,vc?5u:4u);StoreLittle64(canonical.data()+24,nc);StoreLittle64(canonical.data()+32,vc);
  if(vc){
    EncodeComponentRaw({&h,TimestampValueStateV3::value,minimum_day,minimum_nanoseconds},canonical.data()+40);
    EncodeComponentRaw({&h,TimestampValueStateV3::value,maximum_day,maximum_nanoseconds},canonical.data()+56);
    StoreLittle32(canonical.data()+72,112);StoreLittle32(canonical.data()+76,112);
    EncodeSortKeyRaw({&h,TimestampValueStateV3::value,minimum_day,minimum_nanoseconds},TimestampSortDirectionV3::ascending,TimestampNullModeV3::nulls_last,canonical.data()+112);
    EncodeSortKeyRaw({&h,TimestampValueStateV3::value,maximum_day,maximum_nanoseconds},TimestampSortDirectionV3::ascending,TimestampNullModeV3::nulls_last,canonical.data()+224);
  }
  std::array<byte,32> canonical_hash{};ArrayClearGuard<32> clear_hash(
      &canonical_hash,TimestampScrubClassV3::hash_digest,&control);
  if(!HashRecord(kZoneDomain,std::span<const byte>(canonical.data(),e.size()),80,&canonical_hash,&control))
    return FailView<TimestampZoneMapViewResultV3>("RESOURCE.BUDGET_EXCEEDED","zone_reencode_hash");
  std::memcpy(canonical.data()+80,canonical_hash.data(),32);
  if(std::memcmp(e.data(),canonical.data(),e.size()))
    return FailView<TimestampZoneMapViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_reencode");
  if(e.size()>control.maximum_allocation_bytes)
    return FailView<TimestampZoneMapViewResultV3>("RESOURCE.BUDGET_EXCEEDED","zone_extent_budget");
  if(Cancelled(control))
    return FailView<TimestampZoneMapViewResultV3>("PROCESS.CANCELLED","before_publication");
  TimestampZoneMapViewResultV3 r;r.value={&h,nc,vc,minimum_day,minimum_nanoseconds,maximum_day,maximum_nanoseconds,minimum_key,maximum_key};r.status=Ok();return r;
}

namespace {
void PutStatisticsIdentity(byte* p,const TimestampValidatedProfileHandleV3& h) noexcept {
  PutCommonIdentity(p,h);
  StoreUuid(p+112,h.statistics_policy.uuid);StoreLittle64(p+128,h.statistics_policy.generation);
  StoreUuid(p+136,h.calendar_policy.uuid);StoreLittle64(p+152,h.calendar_policy.generation);
  StoreUuid(p+160,h.storage_epoch_policy.uuid);StoreLittle64(p+176,h.storage_epoch_policy.generation);
  StoreUuid(p+184,h.identity.ordering_policy.uuid);StoreLittle64(p+200,h.identity.ordering_policy.generation);
  StoreUuid(p+208,h.identity.hash_policy.uuid);StoreLittle64(p+224,h.identity.hash_policy.generation);
}
bool StatisticsIdentityEquals(const byte* p,const TimestampValidatedProfileHandleV3& h) noexcept {
  return CommonIdentityEquals(p,h)&&LoadUuid(p+112)==h.statistics_policy.uuid&&LoadLittle64(p+128)==h.statistics_policy.generation&&
      LoadUuid(p+136)==h.calendar_policy.uuid&&LoadLittle64(p+152)==h.calendar_policy.generation&&
      LoadUuid(p+160)==h.storage_epoch_policy.uuid&&LoadLittle64(p+176)==h.storage_epoch_policy.generation&&
      LoadUuid(p+184)==h.identity.ordering_policy.uuid&&LoadLittle64(p+200)==h.identity.ordering_policy.generation&&
      LoadUuid(p+208)==h.identity.hash_policy.uuid&&LoadLittle64(p+224)==h.identity.hash_policy.generation;
}

bool EncodeStatisticsRaw(const TimestampStatisticsProjectionV3& p,
                         const TimestampValidatedProfileHandleV3& profile,
                         const platform::Uuid& provider_evidence_uuid,
                         std::vector<byte>* bytes,
                         const TimestampExecutionControlV3* control = nullptr,
                         bool* cancelled_between_groups = nullptr) noexcept {
  if (cancelled_between_groups) *cancelled_between_groups = false;
  const u64 n = kTimestampStatisticsHeaderBytesV3 + 24 * p.histogram.size() +
                48 * p.mcv.size();
  if (!Reserve(bytes, n)) return false;
  byte* b = bytes->data();
  std::memcpy(b, kStatisticsMagic, 8);
  StoreLittle16(b + 8, 1);
  StoreLittle16(b + 10, kTimestampStatisticsHeaderBytesV3);
  StoreLittle32(b + 12, static_cast<u32>(n));
  const u32 flags = (p.histogram.empty() ? 0u : 1u) |
                    (p.mcv.empty() ? 0u : 2u) |
                    (p.value_count ? 4u : 0u) |
                    (p.mcv.empty() ? 0u : 32u);
  StoreLittle32(b + 16, flags);
  StoreLittle32(b + 20, static_cast<u32>(p.histogram.size()));
  StoreLittle32(b + 24, static_cast<u32>(p.mcv.size()));
  PutStatisticsIdentity(b + 32, profile);
  // Bytes 264..279 are reserved zero by the SBTSTS01 authority.
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
    EncodeComponentRaw({&profile,TimestampValueStateV3::value,
                        p.minimum_civil_day,p.minimum_nanoseconds_since_midnight},b+384);
    EncodeComponentRaw({&profile,TimestampValueStateV3::value,
                        p.maximum_civil_day,p.maximum_nanoseconds_since_midnight},b+400);
  }
  StoreLittle64(b + 416, 0x8000000000000000ULL);
  StoreUuid(b + 424, provider_evidence_uuid);
  std::memcpy(b + 472, profile.profile_fingerprint.data(), 32);
  StoreUuid(b + 504, kPrivacyUuid);
  StoreLittle64(b + 520, 1);
  // Bytes 528..543 are reserved zero by the SBTSTS01 authority.
  std::size_t off = kTimestampStatisticsHeaderBytesV3;
  for (const auto& x : p.histogram) {
    EncodeComponentRaw({&profile,TimestampValueStateV3::value,
                        x.inclusive_upper_civil_day,
                        x.inclusive_upper_nanoseconds_since_midnight},b+off);
    StoreLittle64(b + off + 16, x.cumulative_count);
    off += 24;
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
  ArrayClearGuard<32> clear_digest(
      &digest,TimestampScrubClassV3::hash_digest,control);
  if (!HashRecord(kStatisticsDomain, *bytes, 440, &digest,control)) return false;
  std::memcpy(b + 440, digest.data(), 32);
  return true;
}

bool ValidateStatisticsStructureNoAlloc(std::span<const byte> e) noexcept {
  if (e.size() < kTimestampStatisticsHeaderBytesV3 ||
      e.size() > kTimestampStatisticsMaximumBytesV3 ||
      std::memcmp(e.data(), kStatisticsMagic, 8) ||
      LoadLittle16(e.data() + 8) != 1 ||
      LoadLittle16(e.data() + 10) != kTimestampStatisticsHeaderBytesV3 ||
      LoadLittle32(e.data() + 12) != e.size() || LoadLittle32(e.data() + 28))
    return false;
  const u32 histogram_count = LoadLittle32(e.data() + 20);
  const u32 mcv_count = LoadLittle32(e.data() + 24);
  const u64 expected = kTimestampStatisticsHeaderBytesV3 +
                       24ull * histogram_count + 48ull * mcv_count;
  if (histogram_count > 64 || mcv_count > 64 || expected != e.size() ||
      std::any_of(e.begin() + 264, e.begin() + 280,
                  [](byte value) { return value != 0; }) ||
      std::any_of(e.begin() + 528, e.begin() + 544,
                  [](byte value) { return value != 0; }))
    return false;
  const u64 value_count = LoadLittle64(e.data() + 352);
  const u32 expected_flags = (histogram_count ? 1u : 0u) |
                             (mcv_count ? 2u : 0u) |
                             (value_count ? 4u : 0u) |
                             (mcv_count ? 32u : 0u);
  return LoadLittle32(e.data() + 16) == expected_flags;
}

bool ValidateStatisticsIntegrityNoAlloc(
    std::span<const byte> e,
    const TimestampExecutionControlV3* control = nullptr) noexcept {
  return HashEquals(kStatisticsDomain, e, 440, control);
}

bool ValidateStatisticsAuthorityNoAlloc(
    const TimestampValidatedProfileHandleV3& profile,
    std::span<const byte> e) noexcept {
  return StatisticsIdentityEquals(e.data() + 32, profile) &&
      std::memcmp(e.data() + 472, profile.profile_fingerprint.data(), 32) == 0;
}

bool ValidateStatisticsPrivacyNoAlloc(std::span<const byte> e) noexcept {
  return LoadUuid(e.data() + 504) == kPrivacyUuid &&
      LoadLittle64(e.data() + 520) == 1;
}

bool ValidateStatisticsEpochsNoAlloc(std::span<const byte> e) noexcept {
  return LoadLittle64(e.data() + 312) != 0 &&
      LoadLittle64(e.data() + 320) != 0 &&
      LoadLittle64(e.data() + 328) != 0;
}

bool ValidateStatisticsRecordNoAlloc(
    const TimestampValidatedProfileHandleV3& profile,
    std::span<const byte> e,
    const TimestampExecutionControlV3* control = nullptr) noexcept {
  if (!ValidateStatisticsStructureNoAlloc(e) ||
      !ValidateStatisticsIntegrityNoAlloc(e, control) ||
      !ValidateStatisticsAuthorityNoAlloc(profile, e) ||
      !ValidateStatisticsPrivacyNoAlloc(e) ||
      !ValidateStatisticsEpochsNoAlloc(e))
    return false;
  const u32 hc = LoadLittle32(e.data() + 20), mc = LoadLittle32(e.data() + 24);
  const u64 expected = kTimestampStatisticsHeaderBytesV3 + 24ull * hc + 48ull * mc;
  if (expected != e.size()) return false;
  const auto statistics_uuid = LoadUuid(e.data() + 280);
  const auto source_uuid = LoadUuid(e.data() + 296);
  const auto provider_uuid = LoadUuid(e.data() + 424);
  const u64 schema_epoch = LoadLittle64(e.data() + 312);
  const u64 collection_epoch = LoadLittle64(e.data() + 320);
  const u64 security_epoch = LoadLittle64(e.data() + 328);
  const u64 row_count = LoadLittle64(e.data() + 336);
  const u64 null_count = LoadLittle64(e.data() + 344);
  const u64 value_count = LoadLittle64(e.data() + 352);
  std::int32_t minimum_day=0,maximum_day=0;u64 minimum_time=0,maximum_time=0;
  if(value_count){
    auto minimum=DecodeCanonicalTimestampComponentNoAllocV3(profile,TimestampValueStateV3::value,false,e.subspan(384,16));
    auto maximum=DecodeCanonicalTimestampComponentNoAllocV3(profile,TimestampValueStateV3::value,false,e.subspan(400,16));
    if(!minimum.ok()||!maximum.ok())return false;
    minimum_day=minimum.value.civil_day;minimum_time=minimum.value.nanoseconds_since_midnight;
    maximum_day=maximum.value.civil_day;maximum_time=maximum.value.nanoseconds_since_midnight;
  }else if(std::any_of(e.begin()+384,e.begin()+416,[](byte value){return value!=0;}))return false;
  if (!IsV7(statistics_uuid) || Nil(source_uuid) || !IsV7(provider_uuid) ||
      statistics_uuid == source_uuid || statistics_uuid == provider_uuid ||
      source_uuid == provider_uuid || !schema_epoch || !collection_epoch ||
      !security_epoch || LoadLittle32(e.data() + 360) ||
      LoadLittle32(e.data() + 364) || LoadLittle64(e.data() + 368) ||
      LoadLittle32(e.data() + 380) ||
      LoadLittle64(e.data() + 416) != 0x8000000000000000ULL ||
      null_count > std::numeric_limits<u64>::max() - value_count ||
      row_count != null_count + value_count ||
      (value_count&&CompareTuple(minimum_day,minimum_time,maximum_day,maximum_time)>0)) return false;
  const u32 flags = LoadLittle32(e.data() + 16);
  const u32 want = (hc ? 1u : 0u) | (mc ? 2u : 0u) |
                   (value_count ? 4u : 0u) | (mc ? 32u : 0u);
  if (flags != want || e[376] != (value_count ? 1 : 0) ||
      e[377] != (value_count ? 1 : 0) || e[378] != 0 || e[379] != 1)
    return false;
  std::array<byte,kTimestampStatisticsMaximumBytesV3> canonical{};
  ArrayClearGuard<kTimestampStatisticsMaximumBytesV3> clear_canonical(
      &canonical,TimestampScrubClassV3::statistics_decode_reencode,control);
  std::memcpy(canonical.data(),kStatisticsMagic,8);
  StoreLittle16(canonical.data()+8,1);
  StoreLittle16(canonical.data()+10,kTimestampStatisticsHeaderBytesV3);
  StoreLittle32(canonical.data()+12,static_cast<u32>(expected));
  StoreLittle32(canonical.data()+16,flags);
  StoreLittle32(canonical.data()+20,hc);
  StoreLittle32(canonical.data()+24,mc);
  PutStatisticsIdentity(canonical.data()+32,profile);
  StoreUuid(canonical.data()+280,statistics_uuid);
  StoreUuid(canonical.data()+296,source_uuid);
  StoreLittle64(canonical.data()+312,schema_epoch);
  StoreLittle64(canonical.data()+320,collection_epoch);
  StoreLittle64(canonical.data()+328,security_epoch);
  StoreLittle64(canonical.data()+336,row_count);
  StoreLittle64(canonical.data()+344,null_count);
  StoreLittle64(canonical.data()+352,value_count);
  canonical[376]=value_count?1:0;
  canonical[377]=value_count?1:0;
  canonical[379]=1;
  if(value_count){
    EncodeComponentRaw({&profile,TimestampValueStateV3::value,minimum_day,minimum_time},canonical.data()+384);
    EncodeComponentRaw({&profile,TimestampValueStateV3::value,maximum_day,maximum_time},canonical.data()+400);
  }
  StoreLittle64(canonical.data()+416,0x8000000000000000ULL);
  StoreUuid(canonical.data()+424,provider_uuid);
  std::memcpy(canonical.data()+472,profile.profile_fingerprint.data(),32);
  StoreUuid(canonical.data()+504,kPrivacyUuid);
  StoreLittle64(canonical.data()+520,1);
  std::size_t off = kTimestampStatisticsHeaderBytesV3;
  std::int32_t prior_bound_day=0;u64 prior_bound_time=0,prior_count=0;
  bool first = true;
  for (u32 i = 0; i < hc; ++i, off += 24) {
    auto bound=DecodeCanonicalTimestampComponentNoAllocV3(profile,TimestampValueStateV3::value,false,e.subspan(off,16));
    if(!bound.ok())return false;
    const auto bound_day=bound.value.civil_day;const auto bound_time=bound.value.nanoseconds_since_midnight;
    const u64 cumulative = LoadLittle64(e.data() + off + 16);
    if (CompareTuple(bound_day,bound_time,minimum_day,minimum_time)<0 || CompareTuple(bound_day,bound_time,maximum_day,maximum_time)>0 || (!first && CompareTuple(bound_day,bound_time,prior_bound_day,prior_bound_time)<=0) ||
        cumulative < prior_count || cumulative > value_count) return false;
    prior_bound_day=bound_day;prior_bound_time=bound_time;prior_count=cumulative;first=false;
    EncodeComponentRaw({&profile,TimestampValueStateV3::value,bound_day,bound_time},canonical.data()+off);
    StoreLittle64(canonical.data()+off+16,cumulative);
  }
  if (hc && (CompareTuple(prior_bound_day,prior_bound_time,maximum_day,maximum_time)!=0 || prior_count != value_count)) return false;
  u64 prior_frequency = std::numeric_limits<u64>::max();
  std::array<byte, 32> prior_hash{};
  ArrayClearGuard<32> clear_prior_hash(
      &prior_hash,TimestampScrubClassV3::statistics_prior_hash,control);
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
    std::memcpy(canonical.data()+off,hash,32);
    StoreLittle64(canonical.data()+off+32,frequency);
    StoreLittle32(canonical.data()+off+40,i+1);
    StoreLittle32(canonical.data()+off+44,1);
  }
  if(frequency_sum>value_count)return false;
  std::array<byte,32> digest{};
  ArrayClearGuard<32> clear_digest(
      &digest,TimestampScrubClassV3::hash_digest,control);
  if(!HashRecord(kStatisticsDomain,
       std::span<const byte>(canonical.data(),static_cast<std::size_t>(expected)),
       440,&digest,control))return false;
  std::memcpy(canonical.data()+440,digest.data(),32);
  return std::memcmp(e.data(),canonical.data(),static_cast<std::size_t>(expected))==0;
}

}  // namespace


static TimestampStatisticsDecodeResultV3 DecodeTimestampStatisticsProjectionImpl(
    const std::shared_ptr<const TimestampValidatedProfileHandleV3>& profile,
    std::span<const byte> encoded, const TimestampExecutionControlV3& control,
    bool retain_profile) noexcept;

TimestampBytesResultV3 EncodeTimestampStatisticsProjectionV3(
    const TimestampStatisticsProjectionV3& p, const TimestampExecutionControlV3& c) noexcept {
  auto profile_pin=p.profile; auto evidence_pin=p.provider_evidence;
  if (!evidence_pin) return FailBytes("CTI.TEMPORAL.DESCRIPTOR_INVALID", "statistics_provider_evidence");
  if (!evidence_pin->source_authorized)
    return FailBytes("SECURITY.ACCESS_DENIED", "statistics_source_authorization");
  if (!evidence_pin->privacy_admitted)
    return FailBytes("OPTIMIZER.STATISTICS_PRIVACY_DENIED", "statistics_privacy");
  if (!profile_pin || !ValidProfile(*profile_pin,&c)) return FailBytes("CTI.TEMPORAL.DESCRIPTOR_INVALID", "statistics_profile");
  if (!IsV7(evidence_pin->provider_evidence_uuid)) return FailBytes("CTI.TEMPORAL.DESCRIPTOR_INVALID", "statistics_provider_evidence");
  if (!evidence_pin->authenticated_population ||
      evidence_pin->evidence_class !=
          TimestampStatisticsEvidenceClassV3::full_visible_population ||
      evidence_pin->schema_epoch != p.schema_epoch ||
      evidence_pin->collection_epoch != p.collection_epoch ||
      evidence_pin->security_epoch != p.security_epoch ||
      evidence_pin->source_object_uuid != p.source_object_uuid ||
      evidence_pin->row_count != p.row_count ||
      evidence_pin->null_count != p.null_count ||
      evidence_pin->value_count != p.value_count ||
      evidence_pin->minimum_maximum_present !=
          p.minimum_maximum_present ||
      evidence_pin->minimum_civil_day != p.minimum_civil_day ||
      evidence_pin->minimum_nanoseconds_since_midnight !=
          p.minimum_nanoseconds_since_midnight ||
      evidence_pin->maximum_civil_day != p.maximum_civil_day ||
      evidence_pin->maximum_nanoseconds_since_midnight !=
          p.maximum_nanoseconds_since_midnight ||
      evidence_pin->histogram.size() != p.histogram.size() ||
      !std::equal(evidence_pin->histogram.begin(),
                  evidence_pin->histogram.end(), p.histogram.begin()) ||
      evidence_pin->mcv.size() != p.mcv.size() ||
      !std::equal(evidence_pin->mcv.begin(),
                  evidence_pin->mcv.end(), p.mcv.begin()))
    return FailBytes("OPTIMIZER.STATISTICS_EPOCH_MISMATCH", "statistics_population_evidence");
  const auto provider_uuid = evidence_pin->provider_evidence_uuid;
  if (p.histogram.size() > 64 || p.mcv.size() > 64) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "statistics_count");
  if (!IsV7(p.statistics_uuid) || Nil(p.source_object_uuid) || p.statistics_uuid == p.source_object_uuid || p.statistics_uuid == provider_uuid || p.source_object_uuid == provider_uuid || !p.schema_epoch || !p.collection_epoch || !p.security_epoch)
    return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "statistics_instance_identity");
  if (p.null_count > std::numeric_limits<u64>::max() - p.value_count ||
      p.row_count != p.null_count + p.value_count ||
      p.minimum_maximum_present != (p.value_count != 0) ||
      p.minimum_nanoseconds_since_midnight > kTimestampMaximumNanosecondsV3 ||
      p.maximum_nanoseconds_since_midnight > kTimestampMaximumNanosecondsV3 ||
      (!p.value_count && (p.minimum_civil_day||p.minimum_nanoseconds_since_midnight||p.maximum_civil_day||p.maximum_nanoseconds_since_midnight)) ||
      (p.value_count && CompareTuple(p.minimum_civil_day,p.minimum_nanoseconds_since_midnight,p.maximum_civil_day,p.maximum_nanoseconds_since_midnight)>0))
    return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "statistics_counts");
  std::int32_t prior_bound_day=0;u64 prior_bound_time=0,prior_count=0;bool first=true;
  for (const auto& x : p.histogram) {
    if(x.inclusive_upper_nanoseconds_since_midnight>kTimestampMaximumNanosecondsV3||CompareTuple(x.inclusive_upper_civil_day,x.inclusive_upper_nanoseconds_since_midnight,p.minimum_civil_day,p.minimum_nanoseconds_since_midnight)<0||CompareTuple(x.inclusive_upper_civil_day,x.inclusive_upper_nanoseconds_since_midnight,p.maximum_civil_day,p.maximum_nanoseconds_since_midnight)>0||(!first&&CompareTuple(x.inclusive_upper_civil_day,x.inclusive_upper_nanoseconds_since_midnight,prior_bound_day,prior_bound_time)<=0)||x.cumulative_count<prior_count||x.cumulative_count>p.value_count)
      return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "histogram");
    prior_bound_day=x.inclusive_upper_civil_day;prior_bound_time=x.inclusive_upper_nanoseconds_since_midnight;prior_count=x.cumulative_count;first=false;
  }
  if(!p.histogram.empty()&&(CompareTuple(prior_bound_day,prior_bound_time,p.maximum_civil_day,p.maximum_nanoseconds_since_midnight)!=0||prior_count!=p.value_count))return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","histogram_total");
  u64 sum = 0, prior_frequency = std::numeric_limits<u64>::max();
  std::array<byte,32> prior_hash{};
  ArrayClearGuard<32> clear_prior_hash(
      &prior_hash,TimestampScrubClassV3::statistics_prior_hash,&c);
  first = true;
  for (const auto& x : p.mcv) {
    if (!x.frequency || x.frequency > prior_frequency || (!first && x.frequency == prior_frequency && !std::lexicographical_compare(prior_hash.begin(), prior_hash.end(), x.value_hash.begin(), x.value_hash.end())) || x.frequency > std::numeric_limits<u64>::max() - sum)
      return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "mcv");
    sum += x.frequency; prior_frequency = x.frequency; prior_hash = x.value_hash; first = false;
  }
  if (sum > p.value_count) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "mcv_total");
  const u64 n = kTimestampStatisticsHeaderBytesV3 + 24 * p.histogram.size() + 48 * p.mcv.size();
  if (n > c.maximum_allocation_bytes) return FailBytes("RESOURCE.BUDGET_EXCEEDED", "statistics_allocation");
  if (Cancelled(c)) return FailBytes("PROCESS.CANCELLED", "statistics_cancelled");
  TimestampBytesResultV3 r; VectorClearGuard clear(&r.bytes,&c); bool cancelled_between_groups = false;
  if (!EncodeStatisticsRaw(p, *profile_pin, provider_uuid, &r.bytes, &c, &cancelled_between_groups))
    return cancelled_between_groups ? FailBytes("PROCESS.CANCELLED", "statistics_between_histogram_and_mcv") : FailBytes("RESOURCE.BUDGET_EXCEEDED", "statistics_allocation");
  if (!ValidateStatisticsRecordNoAlloc(*profile_pin, r.bytes,&c)) return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "statistics_self_check");
  if (Cancelled(c)) return FailBytes("PROCESS.CANCELLED", "statistics_prepublication");
  r.status = Ok(); clear.Disarm(); return r;
}

static TimestampStatisticsDecodeResultV3 DecodeTimestampStatisticsProjectionImpl(
    const std::shared_ptr<const TimestampValidatedProfileHandleV3>& profile,
    std::span<const byte> e, const TimestampExecutionControlV3& c,
    bool retain_profile) noexcept {
  auto profile_pin=profile;
  if (!ValidateStatisticsStructureNoAlloc(e))
    return FailView<TimestampStatisticsDecodeResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "statistics_structure");
  if (!ValidateStatisticsIntegrityNoAlloc(e,&c))
    return FailView<TimestampStatisticsDecodeResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "statistics_integrity");
  if (!profile_pin || !ValidProfile(*profile_pin,&c))
    return FailView<TimestampStatisticsDecodeResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID", "profile");
  if (!ValidateStatisticsAuthorityNoAlloc(*profile_pin,e))
    return FailView<TimestampStatisticsDecodeResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID", "statistics_authority");
  if (!ValidateStatisticsPrivacyNoAlloc(e))
    return FailView<TimestampStatisticsDecodeResultV3>("OPTIMIZER.STATISTICS_PRIVACY_DENIED", "statistics_privacy_policy");
  if (!ValidateStatisticsEpochsNoAlloc(e))
    return FailView<TimestampStatisticsDecodeResultV3>("OPTIMIZER.STATISTICS_EPOCH_MISMATCH", "statistics_epoch_zero");
  if (!ValidateStatisticsRecordNoAlloc(*profile_pin, e,&c))
    return FailView<TimestampStatisticsDecodeResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "statistics_record");
  const u32 hc = LoadLittle32(e.data() + 20), mc = LoadLittle32(e.data() + 24);
  if (e.size() > c.maximum_allocation_bytes) return FailView<TimestampStatisticsDecodeResultV3>("RESOURCE.BUDGET_EXCEEDED", "statistics_allocation");
  if (Cancelled(c)) return FailView<TimestampStatisticsDecodeResultV3>("PROCESS.CANCELLED");
  TimestampStatisticsDecodeResultV3 r; auto& s = r.statistics; DecodedStatisticsClearGuard clear(&s,&c);
  s.statistics_uuid = LoadUuid(e.data() + 280);
  s.source_object_uuid = LoadUuid(e.data() + 296);
  s.provider_evidence_uuid = LoadUuid(e.data() + 424);
  s.schema_epoch = LoadLittle64(e.data() + 312);
  s.collection_epoch = LoadLittle64(e.data() + 320);
  s.security_epoch = LoadLittle64(e.data() + 328);
  s.row_count = LoadLittle64(e.data() + 336);
  s.null_count = LoadLittle64(e.data() + 344);
  s.value_count = LoadLittle64(e.data() + 352);
  s.minimum_maximum_present = s.value_count != 0;
  if(s.value_count){
    auto minimum=DecodeCanonicalTimestampComponentNoAllocV3(*profile_pin,TimestampValueStateV3::value,false,e.subspan(384,16));
    auto maximum=DecodeCanonicalTimestampComponentNoAllocV3(*profile_pin,TimestampValueStateV3::value,false,e.subspan(400,16));
    if(!minimum.ok()||!maximum.ok())return FailView<TimestampStatisticsDecodeResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","statistics_extrema");
    s.minimum_civil_day=minimum.value.civil_day;s.minimum_nanoseconds_since_midnight=minimum.value.nanoseconds_since_midnight;
    s.maximum_civil_day=maximum.value.civil_day;s.maximum_nanoseconds_since_midnight=maximum.value.nanoseconds_since_midnight;
  }
  try { s.histogram.reserve(hc); s.mcv.reserve(mc); } catch (...) { return FailView<TimestampStatisticsDecodeResultV3>("RESOURCE.BUDGET_EXCEEDED"); }
  std::size_t off = kTimestampStatisticsHeaderBytesV3;
  for(u32 i=0;i<hc;++i,off+=24){auto bound=DecodeCanonicalTimestampComponentNoAllocV3(*profile_pin,TimestampValueStateV3::value,false,e.subspan(off,16));if(!bound.ok())return FailView<TimestampStatisticsDecodeResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","statistics_histogram");s.histogram.push_back({bound.value.civil_day,bound.value.nanoseconds_since_midnight,LoadLittle64(e.data()+off+16)});}
  if (Cancelled(c)) return FailView<TimestampStatisticsDecodeResultV3>("PROCESS.CANCELLED", "statistics_between_histogram_and_mcv");
  for (u32 i = 0; i < mc; ++i, off += 48) { s.mcv.emplace_back(); std::memcpy(s.mcv.back().value_hash.data(), e.data() + off, 32); s.mcv.back().frequency = LoadLittle64(e.data() + off + 32); }
  if (Cancelled(c)) return FailView<TimestampStatisticsDecodeResultV3>("PROCESS.CANCELLED", "statistics_prepublication");
  if (retain_profile) s.profile = std::move(profile_pin);
  r.status = Ok();
  clear.Disarm();
  return r;
}

TimestampStatisticsDecodeResultV3 DecodeTimestampStatisticsProjectionV3(
    const std::shared_ptr<const TimestampValidatedProfileHandleV3>& profile,
    std::span<const byte> encoded,
    const TimestampExecutionControlV3& control) noexcept {
  return DecodeTimestampStatisticsProjectionImpl(profile, encoded, control, true);
}

TimestampStatisticsReceivingDispositionV3 ResolveTimestampStatisticsReceivingFactsV3(
    const TimestampDecodedStatisticsV3& d,const TimestampStatisticsReceivingFactsV3& c) noexcept {
  if(!c.security_visible)return TimestampStatisticsReceivingDispositionV3::security_denied;
  if(!c.privacy_admitted)return TimestampStatisticsReceivingDispositionV3::privacy_denied;
  if(c.source_object_uuid!=d.source_object_uuid)return TimestampStatisticsReceivingDispositionV3::provider_evidence_mismatch;
  if(c.schema_epoch!=d.schema_epoch)return TimestampStatisticsReceivingDispositionV3::schema_epoch_mismatch;
  if(c.collection_epoch!=d.collection_epoch)return TimestampStatisticsReceivingDispositionV3::collection_epoch_mismatch;
  if(c.security_epoch!=d.security_epoch)return TimestampStatisticsReceivingDispositionV3::security_epoch_mismatch;
  if(c.provider_evidence_uuid!=d.provider_evidence_uuid)return TimestampStatisticsReceivingDispositionV3::provider_evidence_mismatch;
  return TimestampStatisticsReceivingDispositionV3::admitted;
}

TimestampStatisticsReceivingDispositionV3 ResolveTimestampStatisticsEvidenceV3(
    TimestampStatisticsEvidenceClassV3 evidence) noexcept {
  using E=TimestampStatisticsEvidenceClassV3;
  using D=TimestampStatisticsReceivingDispositionV3;
  switch(evidence){
    case E::full_visible_population:return D::admitted;
    case E::partial_scan:case E::weighted_input:case E::sampled_input:return D::full_recollection_required;
    case E::snapshot_merge:return D::manual_review_required;
    case E::histogram_source_contradiction:return D::provider_evidence_mismatch;
  }
  return D::provider_evidence_mismatch;
}

TimestampStatisticsReadAdmissionResultV3 AdmitTimestampStatisticsReadV3(
    const std::shared_ptr<const TimestampValidatedProfileHandleV3>& profile,
    std::span<const byte> encoded,
    const TimestampStatisticsReceivingFactsV3& current,
    const TimestampExecutionControlV3& control) noexcept {
  using D = TimestampStatisticsReadAdmissionDispositionV3;
  TimestampStatisticsReadAdmissionResultV3 result;
  auto profile_pin=profile;
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

  if (!profile_pin) {
    return refuse_before_decode(D::profile_refused, "SBLR.OPERAND_INVALID",
                                "statistics_profile_missing");
  }
  if (!ValidProfile(*profile_pin,&control)) {
    return refuse_before_decode(D::profile_refused,
                                "CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                "statistics_profile");
  }
  if (!ValidateStatisticsStructureNoAlloc(encoded)) {
    return refuse_before_decode(D::format_refused,
                                "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                                "statistics_structure");
  }
  if (!ValidateStatisticsIntegrityNoAlloc(encoded,&control)) {
    return refuse_before_decode(D::format_refused,
                                "SBLR.ENVELOPE.CHECKSUM_INVALID",
                                "statistics_integrity");
  }
  if (!ValidateStatisticsAuthorityNoAlloc(*profile_pin,encoded)) {
    return refuse_before_decode(D::profile_refused,
                                "CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                "statistics_authority");
  }
  if (!ValidateStatisticsPrivacyNoAlloc(encoded)) {
    return refuse_before_decode(D::privacy_denied,
                                "OPTIMIZER.STATISTICS_PRIVACY_DENIED",
                                "statistics_privacy_policy");
  }
  if (!ValidateStatisticsEpochsNoAlloc(encoded)) {
    return refuse_before_decode(D::schema_epoch_mismatch,
                                "OPTIMIZER.STATISTICS_EPOCH_MISMATCH",
                                "statistics_epoch_zero");
  }
  const auto encoded_source_object_uuid=LoadUuid(encoded.data()+296);
  const auto encoded_schema_epoch=LoadLittle64(encoded.data()+312);
  const auto encoded_collection_epoch=LoadLittle64(encoded.data()+320);
  const auto encoded_security_epoch=LoadLittle64(encoded.data()+328);
  const auto encoded_provider_evidence_uuid=LoadUuid(encoded.data()+424);
  if(current.source_object_uuid!=encoded_source_object_uuid||
     current.provider_evidence_uuid!=encoded_provider_evidence_uuid){
    return refuse_before_decode(D::provider_evidence_mismatch,
                                "CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                "statistics_provider_evidence");
  }
  if(current.schema_epoch!=encoded_schema_epoch){
    return refuse_before_decode(D::schema_epoch_mismatch,
                                "OPTIMIZER.STATISTICS_EPOCH_MISMATCH",
                                "statistics_schema_epoch");
  }
  if(current.collection_epoch!=encoded_collection_epoch){
    return refuse_before_decode(D::collection_epoch_mismatch,
                                "OPTIMIZER.STATISTICS_EPOCH_MISMATCH",
                                "statistics_collection_epoch");
  }
  if(current.security_epoch!=encoded_security_epoch){
    return refuse_before_decode(D::security_epoch_mismatch,
                                "OPTIMIZER.STATISTICS_EPOCH_MISMATCH",
                                "statistics_security_epoch");
  }

  result.decode_attempted = true;
  result.payload_reads = 1;
  auto decoded = DecodeTimestampStatisticsProjectionV3(profile_pin, encoded, control);
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
  DecodedStatisticsClearGuard decoded_refusal_clear(
      &decoded.statistics,&control);
  switch (ResolveTimestampStatisticsReceivingFactsV3(decoded.statistics, current)) {
    case TimestampStatisticsReceivingDispositionV3::admitted:
      result.disposition = D::admitted;
      result.statistics = std::move(decoded.statistics);
      decoded_refusal_clear.Disarm();
      return result;
    case TimestampStatisticsReceivingDispositionV3::schema_epoch_mismatch:
      result.disposition = D::schema_epoch_mismatch;
      break;
    case TimestampStatisticsReceivingDispositionV3::collection_epoch_mismatch:
      result.disposition = D::collection_epoch_mismatch;
      break;
    case TimestampStatisticsReceivingDispositionV3::security_epoch_mismatch:
      result.disposition = D::security_epoch_mismatch;
      break;
    case TimestampStatisticsReceivingDispositionV3::provider_evidence_mismatch:
      result.disposition = D::provider_evidence_mismatch;
      result.diagnostic = {Error(), "CTI.TEMPORAL.DESCRIPTOR_INVALID",
                           "statistics_provider_evidence"};
      return result;
    case TimestampStatisticsReceivingDispositionV3::security_denied:
    case TimestampStatisticsReceivingDispositionV3::privacy_denied:
    case TimestampStatisticsReceivingDispositionV3::full_recollection_required:
    case TimestampStatisticsReceivingDispositionV3::manual_review_required:
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
void PutBackupIdentity(byte* p,const TimestampValidatedProfileHandleV3& h) noexcept {
  PutCommonIdentity(p,h);std::memcpy(p+112,h.profile_fingerprint.data(),32);
  StoreUuid(p+144,h.calendar_policy.uuid);StoreLittle64(p+160,h.calendar_policy.generation);
  StoreUuid(p+168,h.storage_epoch_policy.uuid);StoreLittle64(p+184,h.storage_epoch_policy.generation);
  StoreUuid(p+192,h.backup_transport_policy.uuid);StoreLittle64(p+208,h.backup_transport_policy.generation);
  StoreUuid(p+216,h.protection_policy.uuid);StoreLittle64(p+232,h.protection_policy.generation);
}
bool BackupIdentityEquals(const byte* p,const TimestampValidatedProfileHandleV3& h) noexcept {
  return CommonIdentityEquals(p,h)&&!std::memcmp(p+112,h.profile_fingerprint.data(),32)&&LoadUuid(p+144)==h.calendar_policy.uuid&&LoadLittle64(p+160)==h.calendar_policy.generation&&LoadUuid(p+168)==h.storage_epoch_policy.uuid&&LoadLittle64(p+184)==h.storage_epoch_policy.generation&&LoadUuid(p+192)==h.backup_transport_policy.uuid&&LoadLittle64(p+208)==h.backup_transport_policy.generation&&LoadUuid(p+216)==h.protection_policy.uuid&&LoadLittle64(p+232)==h.protection_policy.generation;
}
}

TimestampBytesResultV3 EncodeTimestampBackupTupleV3(const TimestampOwnedValueV3& v,
                                          const TimestampExecutionControlV3& c) noexcept {
  const auto pinned_value=v;
  const auto chk=ValidateProjectionValueMetadata(pinned_value,true,nullptr,&c);if(!chk.status.ok())return FailBytes(chk.diagnostic_code,chk.detail);const u64 n=pinned_value.state==TimestampValueStateV3::sql_null?296:312;
  if(n>c.maximum_allocation_bytes)return FailBytes("RESOURCE.BUDGET_EXCEEDED","backup_allocation");
  if(Cancelled(c))return FailBytes("PROCESS.CANCELLED","backup_cancelled");
  TimestampBytesResultV3 r;VectorClearGuard clear(&r.bytes,&c);
  if(!Reserve(&r.bytes,n))return FailBytes("RESOURCE.BUDGET_EXCEEDED","backup_allocation");
  std::memcpy(r.bytes.data(),kBackupMagic,8);StoreLittle16(r.bytes.data()+8,1);StoreLittle16(r.bytes.data()+10,296);StoreLittle32(r.bytes.data()+12,static_cast<u32>(n));PutBackupIdentity(r.bytes.data()+16,*pinned_value.profile);
  r.bytes[256]=pinned_value.state==TimestampValueStateV3::sql_null?0:1;r.bytes[257]=pinned_value.state==TimestampValueStateV3::sql_null?0:16;if(n==312)EncodeComponentRaw(pinned_value.view(),r.bytes.data()+296);
  std::array<byte,32>d{};ArrayClearGuard<32> clear_digest(
      &d,TimestampScrubClassV3::hash_digest,&c);
  if(!HashRecord(kBackupDomain,r.bytes,264,&d,&c))return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","backup_hash_provider");
  std::memcpy(r.bytes.data()+264,d.data(),32);
  auto self_check_control=c;self_check_control.cancelled=nullptr;self_check_control.cancellation_context=nullptr;
  if(!DecodeTimestampBackupTupleNoAllocV3(*pinned_value.profile,true,r.bytes,self_check_control).ok())return FailBytes("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","backup_self_check");
  if(Cancelled(c))return FailBytes("PROCESS.CANCELLED","backup_prepublication");
  r.status=Ok();clear.Disarm();return r;
}

TimestampBackupTupleViewResultV3 DecodeTimestampBackupTupleNoAllocV3(
    const TimestampValidatedProfileHandleV3& h,bool null_allowed,
    std::span<const byte> e,const TimestampExecutionControlV3& control) noexcept {
  if((e.size()!=296&&e.size()!=312)||std::memcmp(e.data(),kBackupMagic,8)||LoadLittle16(e.data()+8)!=1||LoadLittle16(e.data()+10)!=296||LoadLittle32(e.data()+12)!=e.size()||LoadLittle32(e.data()+116)||e[258]||std::any_of(e.begin()+259,e.begin()+264,[](byte b){return b!=0;}))return FailView<TimestampBackupTupleViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","backup_structure");
  if(!HashEquals(kBackupDomain,e,264,&control))return FailView<TimestampBackupTupleViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","backup_hash");
  if(!ValidProfile(h,&control))return FailView<TimestampBackupTupleViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","profile");
  if(!BackupIdentityEquals(e.data()+16,h))return FailView<TimestampBackupTupleViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","backup_identity");
  TimestampValueStateV3 state=TimestampValueStateV3::value;std::int32_t civil_day=0;u64 nanoseconds=0;PlainObjectClearGuard<TimestampValueStateV3> clear_state(&state);PlainObjectClearGuard<std::int32_t> clear_day(&civil_day);PlainObjectClearGuard<u64> clear_value(&nanoseconds);
  if(e[256]==0){if(e[257]!=0||e.size()!=296)return FailView<TimestampBackupTupleViewResultV3>("DATATYPE.NULL_STATE.INVALID","backup_null_pair");if(!null_allowed)return FailView<TimestampBackupTupleViewResultV3>("DATATYPE.NULL_NOT_ADMITTED");state=TimestampValueStateV3::sql_null;}
  else if(e[256]==1){if(e[257]!=16||e.size()!=312)return FailView<TimestampBackupTupleViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","backup_value_pair");state=TimestampValueStateV3::value;}
  else return FailView<TimestampBackupTupleViewResultV3>("DATATYPE.NULL_STATE.INVALID","backup_state");
  const auto component=state==TimestampValueStateV3::sql_null?std::span<const byte>{}:e.subspan(296,16);
  auto nested_control=control;nested_control.maximum_allocation_bytes=~u64{0};nested_control.cancelled=nullptr;nested_control.cancellation_context=nullptr;
  const auto decoded_component=DecodeCanonicalTimestampComponentNoAllocV3(
      h,state,null_allowed,component,nested_control);
  if(!decoded_component.ok())
    return FailView<TimestampBackupTupleViewResultV3>(
        decoded_component.diagnostic.diagnostic_code,
        decoded_component.diagnostic.detail);
  civil_day=decoded_component.value.civil_day;nanoseconds=decoded_component.value.nanoseconds_since_midnight;
  if(decoded_component.value.state!=state)
    return FailView<TimestampBackupTupleViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","backup_component_decode");
  std::array<byte,312> canonical{};ArrayClearGuard<312> clear_canonical(
      &canonical,TimestampScrubClassV3::backup_decode_reencode,&control);
  std::memcpy(canonical.data(),kBackupMagic,8);StoreLittle16(canonical.data()+8,1);
  StoreLittle16(canonical.data()+10,296);StoreLittle32(canonical.data()+12,static_cast<u32>(e.size()));
  PutBackupIdentity(canonical.data()+16,h);canonical[256]=state==TimestampValueStateV3::sql_null?0:1;
  canonical[257]=state==TimestampValueStateV3::sql_null?0:16;
  if(e.size()==312)EncodeComponentRaw({&h,state,civil_day,nanoseconds},canonical.data()+296);
  std::array<byte,32> canonical_hash{};ArrayClearGuard<32> clear_hash(
      &canonical_hash,TimestampScrubClassV3::hash_digest,&control);
  if(!HashRecord(kBackupDomain,std::span<const byte>(canonical.data(),e.size()),264,&canonical_hash,&control))
    return FailView<TimestampBackupTupleViewResultV3>("RESOURCE.BUDGET_EXCEEDED","backup_reencode_hash");
  std::memcpy(canonical.data()+264,canonical_hash.data(),32);
  if(std::memcmp(e.data(),canonical.data(),e.size()))
    return FailView<TimestampBackupTupleViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","backup_reencode");
  if(e.size()>control.maximum_allocation_bytes)
    return FailView<TimestampBackupTupleViewResultV3>("RESOURCE.BUDGET_EXCEEDED","backup_extent_budget");
  if(Cancelled(control))
    return FailView<TimestampBackupTupleViewResultV3>("PROCESS.CANCELLED","before_publication");
  TimestampBackupTupleViewResultV3 r;r.tuple={&h,state,civil_day,nanoseconds};r.status=Ok();return r;
}

TimestampIndexProjectionResultV3 ProjectTimestampIndexValueV3(
    const TimestampIndexProjectionRequestV3& input) noexcept {
  auto q=input;
  TimestampOwnedValueV3 value_pin;
  TimestampOwnedValueV3 upper_pin;
  TimestampZoneMapRequestV3 zone_pin;
  TimestampOwnedValueV3 zone_minimum_pin;
  TimestampOwnedValueV3 zone_maximum_pin;
  TimestampIndexResolutionV3 selected_pin;
  if(input.value){value_pin=*input.value;q.value=&value_pin;}
  if(input.upper_value){upper_pin=*input.upper_value;q.upper_value=&upper_pin;}
  if(input.zone_map){
    zone_pin=*input.zone_map;
    if(input.zone_map->minimum){zone_minimum_pin=*input.zone_map->minimum;zone_pin.minimum=&zone_minimum_pin;}
    if(input.zone_map->maximum){zone_maximum_pin=*input.zone_map->maximum;zone_pin.maximum=&zone_maximum_pin;}
    q.zone_map=&zone_pin;
  }
  if(input.selected_conditional_family){selected_pin=*input.selected_conditional_family;q.selected_conditional_family=&selected_pin;}
  TimestampIndexResolutionV3 resolution;
  resolution.family=q.compatibility.family;
  if(!q.value||!q.value->profile)return FailIndex(resolution,"CTI.TEMPORAL.DESCRIPTOR_INVALID","value_or_profile_missing");
  if(!ValidProfile(*q.value->profile,&q.control))
    return FailIndex(resolution,"CTI.TEMPORAL.DESCRIPTOR_INVALID","profile_invalid");
  const auto requested_admission =
      AdmitTimestampIndexCompatibilityV3(q.compatibility);
  if (!requested_admission.admitted)
    return FailIndex(resolution,"OPTIMIZER.INDEX_COMPATIBILITY_MISSING",
                     "index_compatibility_identity");
  const auto requested_resolution = requested_admission.resolution;
  resolution=requested_resolution;
  if(resolution.disposition==TimestampIndexDispositionV3::not_applicable) {
    auto refused = FailIndex(resolution,"OPTIMIZER.INDEX_COMPATIBILITY_MISSING",
                             "family_not_applicable");
    refused.owner_fact_only = true;
    return refused;
  }
  TimestampIndexFamilyV3 effective_family=q.compatibility.family;
  if(resolution.disposition==TimestampIndexDispositionV3::conditional){
    if(!q.selected_conditional_family)return OwnerFact(resolution);
    const auto selected_identity = TimestampIndexCompatibilityIdentityV3{
        q.selected_conditional_family->family,
        q.selected_conditional_family->compatibility_uuid,
        q.selected_conditional_family->compatibility_generation};
    const auto selected = AdmitTimestampIndexCompatibilityV3(selected_identity);
    if(!selected.admitted ||
       !SameResolution(selected.resolution,*q.selected_conditional_family)||
       selected.resolution.disposition==TimestampIndexDispositionV3::conditional||
       selected.resolution.disposition==TimestampIndexDispositionV3::not_applicable)
      return OwnerFact(resolution);
    resolution=selected.resolution;effective_family=resolution.family;
  }
  if(!ModeAllowed(effective_family,q.mode))return FailIndex(resolution,"CTI.TEMPORAL.INDEX_KEY_REFUSED","selected_mode_invalid");
  if(effective_family==TimestampIndexFamilyV3::temporary_work){
    switch(q.mode){
      case TimestampProjectionModeV3::equality_hash:resolution.projection=TimestampProjectionKindV3::equality_hash;resolution.exact_recheck_required=true;break;
      case TimestampProjectionModeV3::ascending_nulls_first:case TimestampProjectionModeV3::ascending_nulls_last:case TimestampProjectionModeV3::descending_nulls_first:case TimestampProjectionModeV3::descending_nulls_last:resolution.projection=TimestampProjectionKindV3::sort_key;resolution.exact_recheck_required=false;break;
      default:return FailIndex(resolution,"CTI.TEMPORAL.INDEX_KEY_REFUSED","temporary_mode");
    }
  }
  if(resolution.projection==TimestampProjectionKindV3::range_pair){
    if(!q.upper_value)return FailIndex(resolution,"SBLR.OPERAND_INVALID","upper_missing");
    if(!q.upper_value->profile||
       !ValidProfile(*q.upper_value->profile,&q.control)||
       !SameProfile(*q.value->profile,*q.upper_value->profile))
      return FailIndex(resolution,"CTI.TEMPORAL.DESCRIPTOR_INVALID","projection_profile");
  }
  if(resolution.projection==TimestampProjectionKindV3::zone_map){
    if(!q.zone_map)return FailIndex(resolution,"SBLR.OPERAND_INVALID","zone_request_missing");
    if(!q.zone_map->profile||!ValidProfile(*q.zone_map->profile,&q.control)||
       !SameProfile(*q.value->profile,*q.zone_map->profile))
      return FailIndex(resolution,"CTI.TEMPORAL.DESCRIPTOR_INVALID","zone_profile_mismatch");
    for(const auto* extremum:{q.zone_map->minimum,q.zone_map->maximum}){
      if(extremum&&(!extremum->profile||
         !ValidProfile(*extremum->profile,&q.control)||
         !SameProfile(*q.zone_map->profile,*extremum->profile)))
        return FailIndex(resolution,"CTI.TEMPORAL.DESCRIPTOR_INVALID","projection_profile");
    }
  }
  const auto value_state=ValidateProjectionValueState(*q.value,true);
  if(!value_state.status.ok())
    return FailIndex(resolution,value_state.diagnostic_code,value_state.detail);
  if(resolution.projection==TimestampProjectionKindV3::range_pair){
    const auto upper_state=ValidateProjectionValueState(*q.upper_value,false);
    if(!upper_state.status.ok())
      return FailIndex(resolution,upper_state.diagnostic_code,upper_state.detail);
    if(q.value->state!=TimestampValueStateV3::value)
      return FailIndex(resolution,"DATATYPE.NULL_NOT_ADMITTED","range_lower_null");
  }
  if(resolution.projection==TimestampProjectionKindV3::zone_map){
    if(q.zone_map->value_count>std::numeric_limits<u64>::max()-q.zone_map->null_count)
      return FailIndex(resolution,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_count_overflow");
    if((q.zone_map->value_count==0&&q.zone_map->null_count!=1)||
       (q.zone_map->value_count!=0&&
        (q.zone_map->null_count!=0||q.zone_map->value_count!=2)))
      return FailIndex(resolution,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_exact_population");
    if(!q.zone_map->value_count&&(q.zone_map->minimum||q.zone_map->maximum))
      return FailIndex(resolution,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_extrema_for_empty");
    if(q.zone_map->value_count){
      if(!q.zone_map->minimum||!q.zone_map->maximum)
        return FailIndex(resolution,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_extrema_missing");
      for(const auto* extremum:{q.zone_map->minimum,q.zone_map->maximum}){
        const auto state=ValidateProjectionValueState(*extremum,false);
        if(!state.status.ok())
          return FailIndex(resolution,state.diagnostic_code,state.detail);
      }
    }
  }
  u64 need=0;
  switch(resolution.projection){
    case TimestampProjectionKindV3::equality_hash:need=32;break;
    case TimestampProjectionKindV3::sort_key:need=q.value->state==TimestampValueStateV3::sql_null?100:112;break;
    case TimestampProjectionKindV3::covering_value:need=q.value->state==TimestampValueStateV3::sql_null?1:17;break;
    case TimestampProjectionKindV3::range_pair:need=224;break;
    case TimestampProjectionKindV3::zone_map:need=q.zone_map->value_count?336:112;break;
    default:return OwnerFact(resolution);
  }
  const u64 provider_limit=resolution.projection==TimestampProjectionKindV3::zone_map
      ?std::min(q.provider_max_key_bytes,q.zone_map->provider_max_key_bytes)
      :q.provider_max_key_bytes;
  if(provider_limit<need)return FailIndex(resolution,"CTI.TEMPORAL.INDEX_KEY_REFUSED","provider_budget");
  if(q.control.maximum_allocation_bytes<need)return FailIndex(resolution,"RESOURCE.BUDGET_EXCEEDED","allocation_grant");
  const auto checked=ValidateProjectionPresentPayload(*q.value);
  if(!checked.status.ok())return FailIndex(resolution,checked.diagnostic_code,checked.detail);
  if(resolution.projection==TimestampProjectionKindV3::range_pair){
    const auto upper=ValidateProjectionPresentPayload(*q.upper_value);
    if(!upper.status.ok())return FailIndex(resolution,upper.diagnostic_code,upper.detail);
    if(CompareTuple(q.value->civil_day,q.value->nanoseconds_since_midnight,
                    q.upper_value->civil_day,
                    q.upper_value->nanoseconds_since_midnight)>0)
      return FailIndex(resolution,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","range_order");
  }
  if(resolution.projection==TimestampProjectionKindV3::zone_map&&
     q.zone_map->value_count){
    const auto lo=ValidateProjectionPresentPayload(*q.zone_map->minimum);
    if(!lo.status.ok())return FailIndex(resolution,lo.diagnostic_code,lo.detail);
    const auto hi=ValidateProjectionPresentPayload(*q.zone_map->maximum);
    if(!hi.status.ok())return FailIndex(resolution,hi.diagnostic_code,hi.detail);
    if(CompareTuple(q.zone_map->minimum->civil_day,
                    q.zone_map->minimum->nanoseconds_since_midnight,
                    q.zone_map->maximum->civil_day,
                    q.zone_map->maximum->nanoseconds_since_midnight)>0)
      return FailIndex(resolution,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","zone_extrema_order");
  }
  if(Cancelled(q.control))return FailIndex(resolution,"PROCESS.CANCELLED","before_allocation");
  TimestampBytesResultV3 b;VectorClearGuard clear_bytes(&b.bytes,&q.control);
  if(resolution.projection==TimestampProjectionKindV3::zone_map){
    TimestampExecutionControlV3 inner=q.control;inner.cancelled=nullptr;inner.cancellation_context=nullptr;
    b=EncodeTimestampZoneMapImpl(*q.zone_map,provider_limit,inner,false);
  }else{
    if(!Reserve(&b.bytes,need))return FailIndex(resolution,"RESOURCE.BUDGET_EXCEEDED","allocation");
    auto self_check_control=q.control;self_check_control.cancelled=nullptr;self_check_control.cancellation_context=nullptr;
    switch(resolution.projection){
      case TimestampProjectionKindV3::equality_hash:{
        std::array<byte,32> hash{};ArrayClearGuard<32> clear_hash(
            &hash,TimestampScrubClassV3::hash_digest,&q.control);
        if(!EncodeHashRaw(q.value->view(),&hash,&q.control))
          return FailIndex(resolution,"RESOURCE.BUDGET_EXCEEDED","hash_provider");
        std::memcpy(b.bytes.data(),hash.data(),32);break;}
      case TimestampProjectionKindV3::sort_key:{
        TimestampSortDirectionV3 d=TimestampSortDirectionV3::ascending;TimestampNullModeV3 n=TimestampNullModeV3::nulls_last;
        if(q.mode==TimestampProjectionModeV3::ascending_nulls_first)n=TimestampNullModeV3::nulls_first;
        else if(q.mode==TimestampProjectionModeV3::descending_nulls_first){d=TimestampSortDirectionV3::descending;n=TimestampNullModeV3::nulls_first;}
        else if(q.mode==TimestampProjectionModeV3::descending_nulls_last)d=TimestampSortDirectionV3::descending;
        EncodeSortKeyRaw(q.value->view(),d,n,b.bytes.data());
        if(!DecodeTimestampSortKeyNoAllocV3(*q.value->profile,b.bytes,self_check_control).ok())return FailIndex(resolution,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","sort_key_self_check");
        break;}
      case TimestampProjectionKindV3::covering_value:
        b.bytes[0]=q.value->state==TimestampValueStateV3::sql_null?0:2;
        if(need==17)EncodeComponentRaw(q.value->view(),b.bytes.data()+1);
        if(!DecodeTimestampCoveringValueNoAllocV3(*q.value->profile,true,b.bytes,self_check_control).ok())return FailIndex(resolution,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","covering_self_check");
        break;
      case TimestampProjectionKindV3::range_pair:
        EncodeSortKeyRaw(q.value->view(),TimestampSortDirectionV3::ascending,TimestampNullModeV3::nulls_last,b.bytes.data());
        EncodeSortKeyRaw(q.upper_value->view(),TimestampSortDirectionV3::ascending,TimestampNullModeV3::nulls_last,b.bytes.data()+112);
        if(!DecodeTimestampRangePairNoAllocV3(*q.value->profile,b.bytes,self_check_control).ok())return FailIndex(resolution,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","range_self_check");
        break;
      default:break;
    }
    b.status=Ok();
  }
  if(!b.ok()){TimestampIndexProjectionResultV3 r;r.status=b.status;r.diagnostic=std::move(b.diagnostic);r.resolution=resolution;return r;}
  if(Cancelled(q.control))return FailIndex(resolution,"PROCESS.CANCELLED","before_publication");
  TimestampIndexProjectionResultV3 r;r.status=Ok();
  r.requested_resolution=requested_resolution;r.resolution=resolution;
  r.bytes=std::move(b.bytes);clear_bytes.Disarm();return r;
}

TimestampProtectionResolutionV3 ResolveTimestampProtectionV3(TimestampProtectionCellV3 c) noexcept {
  using C=TimestampProtectionCellV3;
  switch(c){
    case C::canonical_plain:return {c,true,false,"admitted_datatype_component","CTI.TEMPORAL.CANONICAL_ENCODING_INVALID"};
    case C::inner_compression:case C::inner_encryption:return {c,false,false,"forbidden","CTI.TEMPORAL.PROTECTION_UNSUPPORTED"};
    case C::protected_index:return {c,false,false,"forbidden_current_core","CTI.TEMPORAL.PROTECTION_UNSUPPORTED"};
    case C::outer_compression:case C::outer_authenticated_protection:case C::outer_compress_then_protect:case C::outer_protect_then_compress:return {c,false,true,"unsupported_current_core","receiving_outer_owner_typed_handoff_no_datatype_algorithm_or_order_claim"};
    case C::page_or_filespace_crypto:case C::transport_tls:return {c,false,true,"not_applicable_datatype_layer","owner_diagnostic_before_datatype"};
  }
  return {c,false,false,"unknown_cell_fail_closed","CTI.TEMPORAL.DESCRIPTOR_INVALID"};
}

TimestampProtectionCombinationResolutionV3
ResolveTimestampProtectionCombinationV3(
    TimestampProtectionModeV3 storage_mode,
    TimestampProtectionModeV3 stream_mode,
    TimestampProtectionIndexModeV3 index_mode) noexcept {
  const auto storage = static_cast<unsigned>(storage_mode);
  const auto stream = static_cast<unsigned>(stream_mode);
  const auto index = static_cast<unsigned>(index_mode);
  if (storage >= 4 || stream >= 4 || index >= 2) {
    return {storage_mode, stream_mode, index_mode, false,
            "unknown_combination_fail_closed",
            "CTI.TEMPORAL.DESCRIPTOR_INVALID"};
  }
  if (storage_mode == TimestampProtectionModeV3::clear &&
      stream_mode == TimestampProtectionModeV3::clear) {
    return {storage_mode, stream_mode, index_mode, true, "admitted", {}};
  }
  return {storage_mode, stream_mode, index_mode, false, "refused",
          "CTI.TEMPORAL.PROTECTION_UNSUPPORTED"};
}

TimestampProtectionCombinationResolutionV3
AdmitTimestampProtectionCombinationV3(
    const TimestampValidatedProfileHandleV3& profile,
    TimestampProtectionModeV3 storage_mode,
    TimestampProtectionModeV3 stream_mode,
    TimestampProtectionIndexModeV3 index_mode) noexcept {
  if (!ValidProfile(profile)) {
    return {storage_mode, stream_mode, index_mode, false,
            "profile_refused", "CTI.TEMPORAL.DESCRIPTOR_INVALID"};
  }
  return ResolveTimestampProtectionCombinationV3(
      storage_mode, stream_mode, index_mode);
}

TimestampWireLaneResolutionV3 ResolveTimestampWireLaneV3(TimestampWireLaneV3 l) noexcept {
  if (static_cast<unsigned>(l) >= 27)
    return {l,false,false,true,"unknown_lane_fail_closed",
            "CTI.TRANSPORT.UNSUPPORTED"};
  const bool native=static_cast<unsigned>(l)<=static_cast<unsigned>(TimestampWireLaneV3::canonical_sblr);
  return {l,false,native,true,native?"unsupported_current_outer_version_unallocated":"unsupported_no_manifest_listed_compatibility_profile","CTI.TRANSPORT.UNSUPPORTED"};
}

std::span<const TimestampDiagnosticRouteV3> TimestampDiagnosticRoutesV3() noexcept {
  static constexpr std::array<TimestampDiagnosticRouteV3,44> rows{{
    {TimestampDiagnosticAxisV3::input_parse,"SBLR.OPERAND_INVALID",Uuid({0xf7,0xc7,0x36,0x87,0xc7,0x8d,0x54,0x6a,0xbc,0x93,0x02,0x8b,0x52,0xb4,0x35,0xcf}),"input_extent_u32;failure_offset_u32;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::input_parse,"CTI.TEMPORAL.INVALID_LITERAL",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x02,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x02}),"input_extent_u32;failure_offset_u32;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::input_parse,"CTI.TEMPORAL.ZERO_DATE_REFUSED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x0a,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0a}),"input_extent_u32;failure_offset_u32;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::input_parse,"CTI.TEMPORAL.LEAP_SECOND_REFUSED",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x59}),"input_extent_u32;failure_offset_u32;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::input_parse,"CTI.TEMPORAL.PRECISION_LOSS",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x58}),"input_extent_u32;failure_offset_u32;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::descriptor_codec,"SECURITY.ACCESS_DENIED",Uuid({0x1a,0x8d,0x26,0xb0,0x87,0x55,0x56,0xad,0xa1,0x70,0xb2,0x4a,0x3a,0xaf,0x14,0x07}),"request_uuid;operation_uuid;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::descriptor_codec,"CTI.TEMPORAL.DESCRIPTOR_INVALID",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x01,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x01}),"descriptor_uuid;expected_generation_u64;actual_generation_u64;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::storage_read_write,"STORAGE.PAGE_CHECKSUM_FAILED",Uuid({0x0c,0x7d,0x9a,0xaf,0x92,0xe6,0x59,0x05,0x9d,0xaf,0x09,0x74,0xb8,0x71,0xf9,0x4a}),"boundary_enum;offset_u64;extent_u64;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::storage_read_write,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x03,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x03}),"boundary_enum;offset_u64;extent_u64;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::cast_invalid,"DATATYPE.CAST_FORBIDDEN",Uuid({0x0d,0xc5,0x1a,0x1c,0xad,0x21,0x57,0xb5,0xae,0x65,0xe0,0xfe,0x55,0x89,0x02,0x1a}),"source_type_uuid;target_type_uuid;cast_context_enum;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::cast_invalid,"CTI.TEMPORAL.INVALID_LITERAL",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x02,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x02}),"operation_uuid;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::cast_invalid,"CTI.TEMPORAL.ZERO_DATE_REFUSED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x0a,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0a}),"operation_uuid;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::cast_invalid,"CTI.TEMPORAL.LEAP_SECOND_REFUSED",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x59}),"operation_uuid;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::cast_range_loss,"CTI.TEMPORAL.PRECISION_LOSS",Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x58}),"operation_uuid;maximum_fraction_digits_u8;actual_fraction_digits_u32;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::cast_range_loss,"CTB.TEXT.LENGTH_EXCEEDED",Uuid({0xb3,0x99,0xb5,0x14,0xf7,0x71,0x50,0x14,0xa5,0xb4,0x90,0x77,0xf5,0xde,0xe3,0x69}),"required_extent_u64;available_extent_u64;operation_uuid;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::operation_invalid,"CTI.TEMPORAL.OPERATION_REFUSED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x05,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x05}),"operation_uuid;policy_uuid;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::operation_invalid,"CTI.INTERVAL.CALENDAR_OPERATION_REFUSED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x0c,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0c}),"operation_uuid;policy_uuid;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::operation_invalid,"CTI.TEMPORAL.ORDERING_REFUSED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x06,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x06}),"operation_uuid;policy_uuid;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::bounds_overflow_underflow,"CTI.TEMPORAL.RANGE_EXCEEDED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x04,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x04}),"bound_enum;operation_uuid;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::bounds_overflow_underflow,"RESOURCE.BUDGET_EXCEEDED",Uuid({0xde,0xdc,0x1c,0x9b,0x38,0x79,0x57,0x70,0x8f,0x8e,0xc0,0xc9,0x27,0xa0,0xc3,0x4d}),"operation_uuid;requested_extent_u64;granted_extent_u64;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::resource_cancellation,"RESOURCE.BUDGET_EXCEEDED",Uuid({0xde,0xdc,0x1c,0x9b,0x38,0x79,0x57,0x70,0x8f,0x8e,0xc0,0xc9,0x27,0xa0,0xc3,0x4d}),"operation_uuid;requested_extent_u64;granted_extent_u64;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::resource_cancellation,"PROCESS.CANCELLED",Uuid({0x0f,0x42,0x5d,0xe2,0x6b,0xc7,0x51,0xd4,0x8d,0x34,0x72,0xd1,0x07,0xad,0x45,0xe7}),"operation_uuid;checkpoint_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::compression_corruption,"SBLR.ENVELOPE.CHECKSUM_INVALID",Uuid({0x26,0x0f,0x5e,0xac,0xb2,0x86,0x5a,0x00,0x81,0x6c,0xc4,0xc2,0xa8,0xf1,0xa2,0x5e}),"boundary_enum;profile_uuid;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::compression_corruption,"CTI.TEMPORAL.PROTECTION_UNSUPPORTED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x09,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x09}),"boundary_enum;profile_uuid;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::encryption_auth_key,"SECURITY.ACCESS_DENIED",Uuid({0x1a,0x8d,0x26,0xb0,0x87,0x55,0x56,0xad,0xa1,0x70,0xb2,0x4a,0x3a,0xaf,0x14,0x07}),"request_uuid;operation_uuid;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::encryption_auth_key,"CTI.TEMPORAL.PROTECTION_UNSUPPORTED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x09,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x09}),"layer_enum;profile_uuid_or_zero;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::wire_decode_encode,"SBLR.OPERAND_INVALID",Uuid({0xf7,0xc7,0x36,0x87,0xc7,0x8d,0x54,0x6a,0xbc,0x93,0x02,0x8b,0x52,0xb4,0x35,0xcf}),"lane_group_enum;offset_u64;extent_u64;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::wire_decode_encode,"SBLR.ENVELOPE.CHECKSUM_INVALID",Uuid({0x26,0x0f,0x5e,0xac,0xb2,0x86,0x5a,0x00,0x81,0x6c,0xc4,0xc2,0xa8,0xf1,0xa2,0x5e}),"lane_group_enum;offset_u64;extent_u64;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::wire_decode_encode,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x03,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x03}),"lane_group_enum;offset_u64;extent_u64;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::wire_decode_encode,"CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x08,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x08}),"lane_group_enum;offset_u64;extent_u64;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::wire_decode_encode,"CTI.TRANSPORT.UNSUPPORTED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x0d,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0d}),"lane_group_enum;offset_u64;extent_u64;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::index_key,"CTI.TEMPORAL.ORDERING_REFUSED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x06,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x06}),"descriptor_uuid;index_family_enum;required_extent_u64;available_extent_u64;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::index_key,"OPTIMIZER.INDEX_COMPATIBILITY_MISSING",Uuid({0x01,0xa1,0x0c,0x00,0x20,0x00,0x70,0x0d,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0d}),"descriptor_uuid;index_family_enum;operation_uuid","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::index_key,"CTI.TEMPORAL.INDEX_KEY_REFUSED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x07,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x07}),"descriptor_uuid;index_family_enum;required_extent_u64;available_extent_u64;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::index_key,"RESOURCE.BUDGET_EXCEEDED",Uuid({0xde,0xdc,0x1c,0x9b,0x38,0x79,0x57,0x70,0x8f,0x8e,0xc0,0xc9,0x27,0xa0,0xc3,0x4d}),"operation_uuid;requested_extent_u64;granted_extent_u64;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::domain_validation,"DATATYPE.NULL_STATE.INVALID",Uuid({0x58,0x8f,0xaf,0xd0,0x63,0x86,0x55,0xcf,0x84,0x4e,0xf5,0xb0,0xdc,0x22,0xc7,0x00}),"descriptor_uuid;domain_uuid;constraint_uuid;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::domain_validation,"DATATYPE.NULL_NOT_ADMITTED",Uuid({0xab,0xa0,0x82,0x8d,0x77,0xc9,0x50,0xde,0x98,0x42,0x7f,0x20,0x47,0x10,0x02,0x14}),"descriptor_uuid;domain_uuid;constraint_uuid;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::domain_validation,"DOMAIN.CONSTRAINT_FAILED",Uuid({0x62,0xbe,0x50,0x52,0xad,0xbc,0x59,0xaa,0xb8,0x4b,0xae,0x60,0xa8,0xc4,0x0c,0x0f}),"descriptor_uuid;domain_uuid;constraint_uuid;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::recovery_corruption,"STORAGE.PAGE_CHECKSUM_FAILED",Uuid({0x0c,0x7d,0x9a,0xaf,0x92,0xe6,0x59,0x05,0x9d,0xaf,0x09,0x74,0xb8,0x71,0xf9,0x4a}),"object_uuid;offset_u64;receipt_uuid;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::recovery_corruption,"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x03,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x03}),"object_uuid;offset_u64;receipt_uuid;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::recovery_corruption,"CTI.MERGE.MANUAL_REVIEW_REQUIRED",Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x0e,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0e}),"object_uuid;offset_u64;receipt_uuid;reason_enum","no_component_rendered_timestamp_key_hash_preimage_secret_or_address"},
    {TimestampDiagnosticAxisV3::statistics_read,"SECURITY.ACCESS_DENIED",Uuid({0x1a,0x8d,0x26,0xb0,0x87,0x55,0x56,0xad,0xa1,0x70,0xb2,0x4a,0x3a,0xaf,0x14,0x07}),"request_uuid;operation_uuid;reason_enum","no_payload_no_rendered_timestamp_no_key_material_no_secret_no_memory_address"},
    {TimestampDiagnosticAxisV3::statistics_read,"OPTIMIZER.STATISTICS_PRIVACY_DENIED",Uuid({0x01,0xa1,0x0c,0x00,0x20,0x00,0x70,0x0c,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0c}),"statistics_uuid;privacy_policy_uuid;reason_enum","no_payload_no_rendered_timestamp_no_key_material_no_secret_no_memory_address"},
    {TimestampDiagnosticAxisV3::statistics_read,"OPTIMIZER.STATISTICS_EPOCH_MISMATCH",Uuid({0x01,0xa1,0x0c,0x00,0x20,0x00,0x70,0x0b,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0b}),"statistics_uuid;epoch_class_enum;expected_epoch_u64;actual_epoch_u64","no_payload_no_rendered_timestamp_no_key_material_no_secret_no_memory_address"},
  }}; return rows;
}

TimestampDiagnosticRouteExecutionFactV3 ExecuteTimestampDiagnosticRouteV3(
    const TimestampDiagnosticRouteExecutionRequestV3& request) noexcept {
  using D = TimestampDiagnosticRouteExecutionDispositionV3;
  using S = TimestampDiagnosticRouteScalarTypeV3;
  TimestampDiagnosticRouteExecutionFactV3 result;
  auto refuse = [&](D disposition) {
    result.disposition = disposition;
    return result;
  };

  const TimestampDiagnosticRouteV3* route = nullptr;
  bool axis_code_seen = false;
  bool axis_uuid_seen = false;
  for (const auto& candidate : TimestampDiagnosticRoutesV3()) {
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
  if (!route)
    return refuse(axis_code_seen || axis_uuid_seen ? D::code_uuid_mismatch
                                                   : D::unknown_route);
  if (request.redaction != route->redaction) return refuse(D::redaction_mismatch);

  std::array<std::string_view, 5> tokens{};
  std::size_t token_count = 0;
  std::string_view schema = route->ordered_parameter_schema;
  while (!schema.empty()) {
    if (token_count == tokens.size()) return refuse(D::extra_token);
    const auto separator = schema.find(';');
    tokens[token_count++] = schema.substr(0, separator);
    if (separator == std::string_view::npos) break;
    schema.remove_prefix(separator + 1);
  }
  if (request.ordered_payload.size() < token_count) return refuse(D::missing_token);
  if (request.ordered_payload.size() > token_count) return refuse(D::extra_token);

  auto expected_type = [](std::string_view token) {
    if (token.ends_with("_uuid") || token.ends_with("_uuid_or_zero"))
      return S::uuid;
    if (token.ends_with("_u8")) return S::u8;
    if (token.ends_with("_u32")) return S::u32;
    if (token.ends_with("_u64")) return S::u64;
    return S::closed_enum;
  };
  auto valid_enum = [](std::string_view token, std::string_view value) {
    const auto one_of = [value](auto values) {
      return std::find(values.begin(), values.end(), value) != values.end();
    };
    if (token == "bound_enum")
      return one_of(std::array<std::string_view,2>{"minimum","maximum"});
    if (token == "cast_context_enum")
      return one_of(std::array<std::string_view,3>{"implicit","assignment","explicit"});
    if (token == "checkpoint_enum")
      return one_of(std::array<std::string_view,3>{"before_allocation","after_private_stage","before_publication"});
    if (token == "epoch_class_enum")
      return one_of(std::array<std::string_view,3>{"schema_epoch","collection_epoch","security_epoch"});
    if (token == "lane_group_enum")
      return one_of(std::array<std::string_view,4>{"native_sbwp","parser_ipc","backup_transport","reference_engine_compatibility"});
    if (token == "layer_enum")
      return one_of(std::array<std::string_view,4>{"canonical","inner_compression","inner_encryption","protected_index"});
    if (token == "index_family_enum")
      return one_of(std::array<std::string_view,10>{"btree","hash","bitmap","brin_like","columnar_zone_map","covering_included","range_exclusion","temporary_work","expression","partial_filtered"});
    if (token == "boundary_enum")
      return one_of(std::array<std::string_view,10>{"component","profile_value","datatype_value","ordered_key","hash","covering","range_pair","zone_map","statistics","backup"});
    if (token == "reason_enum")
      return one_of(std::array<std::string_view,18>{"absent","authority","grammar","zero_date","leap_second","precision","range","capacity","resource","cancelled","corruption","checksum","profile","policy","unsupported","privacy","stale","manual_review"});
    return false;
  };
  for (std::size_t i = 0; i != token_count; ++i) {
    if (request.ordered_payload[i].token != tokens[i]) {
      const bool known = std::find(tokens.begin(), tokens.begin() + token_count,
                                   request.ordered_payload[i].token) !=
                         tokens.begin() + token_count;
      const bool known_shape = request.ordered_payload[i].token.ends_with("_uuid") ||
          request.ordered_payload[i].token.ends_with("_uuid_or_zero") ||
          request.ordered_payload[i].token.ends_with("_u8") ||
          request.ordered_payload[i].token.ends_with("_u32") ||
          request.ordered_payload[i].token.ends_with("_u64") ||
          request.ordered_payload[i].token.ends_with("_enum");
      return refuse(known ? D::reordered_tokens :
                    (known_shape ? D::mismatched_known_token : D::unknown_token));
    }
    const auto type = expected_type(tokens[i]);
    if (request.ordered_payload[i].scalar_type != type)
      return refuse(D::wrong_scalar_type);
    const auto& value = request.ordered_payload[i];
    const bool inactive_members_canonical =
        (type == S::uuid && value.unsigned_value == 0 &&
         value.enum_value.empty()) ||
        ((type == S::u8 || type == S::u32 || type == S::u64) &&
         value.uuid_value.is_nil() && value.enum_value.empty()) ||
        (type == S::closed_enum && value.uuid_value.is_nil() &&
         value.unsigned_value == 0);
    if (!inactive_members_canonical)
      return refuse(D::invalid_scalar_value);
    if (type == S::uuid &&
        value.uuid_value.is_nil() && !tokens[i].ends_with("_uuid_or_zero"))
      return refuse(D::invalid_scalar_value);
    if (type == S::u8 &&
        value.unsigned_value > std::numeric_limits<std::uint8_t>::max())
      return refuse(D::invalid_scalar_value);
    if (tokens[i] == "maximum_fraction_digits_u8" &&
        value.unsigned_value != 9)
      return refuse(D::invalid_scalar_value);
    if (type == S::u32 &&
        value.unsigned_value > std::numeric_limits<std::uint32_t>::max())
      return refuse(D::invalid_scalar_value);
    if (type == S::closed_enum && !valid_enum(tokens[i], value.enum_value))
      return refuse(D::invalid_scalar_value);
  }

  const auto metric = ResolveTimestampDiagnosticMetricRouteV3(
      {request.axis, request.diagnostic_uuid});
  if (metric.disposition != TimestampDiagnosticRouteFactDispositionV3::admitted)
    return refuse(D::unknown_route);
  result.disposition = D::admitted;
  result.axis = request.axis;
  result.diagnostic_code = route->code;
  result.diagnostic_uuid = route->diagnostic_uuid;
  for (std::size_t i = 0; i != token_count; ++i) {
    const auto& source = request.ordered_payload[i];
    auto& published = result.ordered_payload[i];
    published.token = source.token;
    published.scalar_type = source.scalar_type;
    switch (source.scalar_type) {
      case S::uuid:
        published.uuid_value = source.uuid_value;
        break;
      case S::u8:
      case S::u32:
      case S::u64:
        published.unsigned_value = source.unsigned_value;
        break;
      case S::closed_enum:
        published.enum_value = source.enum_value;
        break;
    }
  }
  result.parameter_count = static_cast<std::uint8_t>(token_count);
  result.redaction = route->redaction;
  result.metric_evidence_type_uuid = metric.route.metric_evidence_type_uuid;
  result.metric_name = metric.route.metric_name;
  result.diagnostic_fact_emitted = true;
  result.receiving_owner_evidence_handoff_ready = true;
  return result;
}

std::span<const std::string_view> TimestampDiagnosticPrecedenceV3(
    TimestampDiagnosticPrecedenceScopeV3 scope) noexcept {
  static constexpr std::array<std::string_view, 29> global{{
      "SBLR.OPERAND_INVALID",
      "SBLR.ENVELOPE.CHECKSUM_INVALID",
      "STORAGE.PAGE_CHECKSUM_FAILED",
      "SECURITY.ACCESS_DENIED",
      "CTI.TEMPORAL.DESCRIPTOR_INVALID",
      "DATATYPE.NULL_STATE.INVALID",
      "DATATYPE.NULL_NOT_ADMITTED",
      "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
      "DATATYPE.CAST_FORBIDDEN",
      "CTI.TEMPORAL.INVALID_LITERAL",
      "CTI.TEMPORAL.ZERO_DATE_REFUSED",
      "CTI.TEMPORAL.LEAP_SECOND_REFUSED",
      "CTI.TEMPORAL.PRECISION_LOSS",
      "CTI.TEMPORAL.RANGE_EXCEEDED",
      "CTB.TEXT.LENGTH_EXCEEDED",
      "CTI.TEMPORAL.OPERATION_REFUSED",
      "CTI.INTERVAL.CALENDAR_OPERATION_REFUSED",
      "CTI.TEMPORAL.ORDERING_REFUSED",
      "DOMAIN.CONSTRAINT_FAILED",
      "OPTIMIZER.STATISTICS_PRIVACY_DENIED",
      "OPTIMIZER.STATISTICS_EPOCH_MISMATCH",
      "OPTIMIZER.INDEX_COMPATIBILITY_MISSING",
      "CTI.TEMPORAL.INDEX_KEY_REFUSED",
      "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
      "CTI.TEMPORAL.PROTECTION_UNSUPPORTED",
      "CTI.TRANSPORT.UNSUPPORTED",
      "RESOURCE.BUDGET_EXCEEDED",
      "PROCESS.CANCELLED",
      "CTI.MERGE.MANUAL_REVIEW_REQUIRED",
  }};
  static constexpr std::array<std::string_view, 6> artifact_decode{{
      "SBLR.OPERAND_INVALID",
      "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
      "CTI.TEMPORAL.DESCRIPTOR_INVALID",
      "DATATYPE.NULL_STATE.INVALID",
      "DATATYPE.NULL_NOT_ADMITTED",
      "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
  }};
  static constexpr std::array<std::string_view, 4> projection_decode{{
      "SBLR.OPERAND_INVALID",
      "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
      "CTI.TEMPORAL.DESCRIPTOR_INVALID",
      "DATATYPE.NULL_STATE.INVALID",
  }};
  static constexpr std::array<std::string_view, 13> boundary_decode{{
      "SBLR.OPERAND_INVALID",
      "SBLR.ENVELOPE.CHECKSUM_INVALID",
      "STORAGE.PAGE_CHECKSUM_FAILED",
      "SECURITY.ACCESS_DENIED",
      "CTI.TEMPORAL.DESCRIPTOR_INVALID",
      "DATATYPE.NULL_STATE.INVALID",
      "DATATYPE.NULL_NOT_ADMITTED",
      "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
      "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
      "CTI.TEMPORAL.PROTECTION_UNSUPPORTED",
      "CTI.TRANSPORT.UNSUPPORTED",
      "RESOURCE.BUDGET_EXCEEDED",
      "PROCESS.CANCELLED",
  }};
  static constexpr std::array<std::string_view, 15> cast{{
      "SECURITY.ACCESS_DENIED",
      "CTI.TEMPORAL.DESCRIPTOR_INVALID",
      "DATATYPE.NULL_STATE.INVALID",
      "DATATYPE.NULL_NOT_ADMITTED",
      "DATATYPE.CAST_FORBIDDEN",
      "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
      "CTI.TEMPORAL.INVALID_LITERAL",
      "CTI.TEMPORAL.ZERO_DATE_REFUSED",
      "CTI.TEMPORAL.LEAP_SECOND_REFUSED",
      "CTI.TEMPORAL.PRECISION_LOSS",
      "CTI.TEMPORAL.RANGE_EXCEEDED",
      "CTB.TEXT.LENGTH_EXCEEDED",
      "DOMAIN.CONSTRAINT_FAILED",
      "RESOURCE.BUDGET_EXCEEDED",
      "PROCESS.CANCELLED",
  }};
  static constexpr std::array<std::string_view, 16> operation{{
      "SECURITY.ACCESS_DENIED",
      "CTI.TEMPORAL.DESCRIPTOR_INVALID",
      "DATATYPE.NULL_STATE.INVALID",
      "DATATYPE.NULL_NOT_ADMITTED",
      "CTI.TEMPORAL.OPERATION_REFUSED",
      "CTI.INTERVAL.CALENDAR_OPERATION_REFUSED",
      "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
      "CTI.TEMPORAL.INVALID_LITERAL",
      "CTI.TEMPORAL.ZERO_DATE_REFUSED",
      "CTI.TEMPORAL.LEAP_SECOND_REFUSED",
      "CTI.TEMPORAL.PRECISION_LOSS",
      "CTI.TEMPORAL.RANGE_EXCEEDED",
      "CTI.TEMPORAL.ORDERING_REFUSED",
      "DOMAIN.CONSTRAINT_FAILED",
      "RESOURCE.BUDGET_EXCEEDED",
      "PROCESS.CANCELLED",
  }};
  static constexpr std::array<std::string_view, 10> index_admission{{
      "SECURITY.ACCESS_DENIED",
      "CTI.TEMPORAL.DESCRIPTOR_INVALID",
      "DATATYPE.NULL_STATE.INVALID",
      "DATATYPE.NULL_NOT_ADMITTED",
      "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
      "CTI.TEMPORAL.ORDERING_REFUSED",
      "OPTIMIZER.INDEX_COMPATIBILITY_MISSING",
      "CTI.TEMPORAL.INDEX_KEY_REFUSED",
      "RESOURCE.BUDGET_EXCEEDED",
      "PROCESS.CANCELLED",
  }};
  static constexpr std::array<std::string_view, 12> recovery_merge{{
      "SBLR.OPERAND_INVALID",
      "SBLR.ENVELOPE.CHECKSUM_INVALID",
      "STORAGE.PAGE_CHECKSUM_FAILED",
      "SECURITY.ACCESS_DENIED",
      "CTI.TEMPORAL.DESCRIPTOR_INVALID",
      "DATATYPE.NULL_STATE.INVALID",
      "DATATYPE.NULL_NOT_ADMITTED",
      "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
      "DOMAIN.CONSTRAINT_FAILED",
      "RESOURCE.BUDGET_EXCEEDED",
      "PROCESS.CANCELLED",
      "CTI.MERGE.MANUAL_REVIEW_REQUIRED",
  }};
  static constexpr std::array<std::string_view, 3> statistics_read{{
      "SECURITY.ACCESS_DENIED",
      "OPTIMIZER.STATISTICS_PRIVACY_DENIED",
      "OPTIMIZER.STATISTICS_EPOCH_MISMATCH",
  }};
  static constexpr std::array<std::string_view, 15> text_fields{{
      "shape",
      "year_sign",
      "year_digits",
      "month_zero",
      "day_zero",
      "civil_combination",
      "literal_range",
      "separator_T",
      "hour",
      "minute",
      "second_leap",
      "fraction_syntax",
      "precision",
      "trailing_zero",
      "timezone_suffix_forbidden",
  }};
  static constexpr std::array<std::string_view, 8> intrinsic_operation_gates{{
      "authority",
      "state",
      "classification",
      "admitted NULL propagation",
      "PRESENT payload",
      "resource",
      "cancellation",
      "atomic publication",
  }};
  switch (scope) {
    case TimestampDiagnosticPrecedenceScopeV3::global: return global;
    case TimestampDiagnosticPrecedenceScopeV3::artifact_decode: return artifact_decode;
    case TimestampDiagnosticPrecedenceScopeV3::projection_decode: return projection_decode;
    case TimestampDiagnosticPrecedenceScopeV3::boundary_decode: return boundary_decode;
    case TimestampDiagnosticPrecedenceScopeV3::cast: return cast;
    case TimestampDiagnosticPrecedenceScopeV3::operation: return operation;
    case TimestampDiagnosticPrecedenceScopeV3::index_admission: return index_admission;
    case TimestampDiagnosticPrecedenceScopeV3::recovery_merge: return recovery_merge;
    case TimestampDiagnosticPrecedenceScopeV3::statistics_read: return statistics_read;
    case TimestampDiagnosticPrecedenceScopeV3::text_fields: return text_fields;
    case TimestampDiagnosticPrecedenceScopeV3::intrinsic_operation_gates: return intrinsic_operation_gates;
  }
  return {};
}

std::span<const TimestampDiagnosticFormatOrderV3>
TimestampDiagnosticFormatOrdersV3() noexcept {
  static constexpr std::array<TimestampDiagnosticFormatOrderV3, 5> rows{{
      {"SBTSPP01", "registries/base-timestamp-profile-registry.yaml"},
      {"SBTSPC01", "registries/base-timestamp-profile-registry.yaml"},
      {"SBTSPK01", "registries/base-timestamp-index-projection-layouts.yaml"},
      {"SBTSTS01", "registries/base-timestamp-statistics-layout.yaml"},
      {"SBTSPB01", "registries/base-timestamp-backup-transport-layout.yaml"},
  }};
  return rows;
}

std::span<const TimestampDiagnosticCombinedFaultV3>
TimestampDiagnosticTextCombinedFaultsV3() noexcept {
  static constexpr std::array<TimestampDiagnosticCombinedFaultV3, 5> rows{{
      {"1970-00-01T12:34:60.1234567890", "CTI.TEMPORAL.ZERO_DATE_REFUSED"},
      {"1970-01-01T12:60:60.1234567890", "CTI.TEMPORAL.INVALID_LITERAL"},
      {"1970-01-01T12:34:60.1234567890", "CTI.TEMPORAL.LEAP_SECOND_REFUSED"},
      {"1970-01-01T12:34:59.1234567890", "CTI.TEMPORAL.PRECISION_LOSS"},
      {"1970-01-01T12:34:59Z", "CTI.TEMPORAL.INVALID_LITERAL"},
  }};
  return rows;
}

std::span<const TimestampDiagnosticClassificationRuleV3>
TimestampDiagnosticClassificationRulesV3() noexcept {
  static constexpr std::array<TimestampDiagnosticClassificationRuleV3, 2> rows{{
      {"forbidden_cast", "pair and context classification precedes PRESENT payload"},
      {"operation", "operation classification precedes PRESENT payload"}}};
  return rows;
}

std::string_view TimestampDiagnosticSelectionRuleV3() noexcept {
  return "named binary format exact sequential gates select first failure; scoped order applies within one gate or operation; global total order is final fallback";
}

std::span<const TimestampDiagnosticMetricRouteV3>
TimestampDiagnosticMetricRoutesV3() noexcept {
  static constexpr std::array<TimestampDiagnosticMetricRouteV3, 44> rows{{
      {TimestampDiagnosticAxisV3::input_parse, Uuid({0xf7,0xc7,0x36,0x87,0xc7,0x8d,0x54,0x6a,0xbc,0x93,0x02,0x8b,0x52,0xb4,0x35,0xcf}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x41}), "sys.metrics.datatype.base_timestamp.operation_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::input_parse, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x02,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x02}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x41}), "sys.metrics.datatype.base_timestamp.operation_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::input_parse, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x0a,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0a}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x41}), "sys.metrics.datatype.base_timestamp.operation_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::input_parse, Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x59}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x41}), "sys.metrics.datatype.base_timestamp.operation_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::input_parse, Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x58}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x41}), "sys.metrics.datatype.base_timestamp.operation_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::descriptor_codec, Uuid({0x1a,0x8d,0x26,0xb0,0x87,0x55,0x56,0xad,0xa1,0x70,0xb2,0x4a,0x3a,0xaf,0x14,0x07}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x45}), "sys.metrics.datatype.base_timestamp.profile_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::descriptor_codec, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x01,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x01}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x45}), "sys.metrics.datatype.base_timestamp.profile_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::storage_read_write, Uuid({0x0c,0x7d,0x9a,0xaf,0x92,0xe6,0x59,0x05,0x9d,0xaf,0x09,0x74,0xb8,0x71,0xf9,0x4a}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x44}), "sys.metrics.datatype.base_timestamp.decode_corruption_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::storage_read_write, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x03,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x03}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x44}), "sys.metrics.datatype.base_timestamp.decode_corruption_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::cast_invalid, Uuid({0x0d,0xc5,0x1a,0x1c,0xad,0x21,0x57,0xb5,0xae,0x65,0xe0,0xfe,0x55,0x89,0x02,0x1a}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x46}), "sys.metrics.datatype.base_timestamp.cast_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::cast_invalid, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x02,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x02}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x46}), "sys.metrics.datatype.base_timestamp.cast_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::cast_invalid, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x0a,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0a}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x46}), "sys.metrics.datatype.base_timestamp.cast_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::cast_invalid, Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x59}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x46}), "sys.metrics.datatype.base_timestamp.cast_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::cast_range_loss, Uuid({0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x58}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x46}), "sys.metrics.datatype.base_timestamp.cast_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::cast_range_loss, Uuid({0xb3,0x99,0xb5,0x14,0xf7,0x71,0x50,0x14,0xa5,0xb4,0x90,0x77,0xf5,0xde,0xe3,0x69}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x46}), "sys.metrics.datatype.base_timestamp.cast_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::operation_invalid, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x05,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x05}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x41}), "sys.metrics.datatype.base_timestamp.operation_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::operation_invalid, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x0c,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0c}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x41}), "sys.metrics.datatype.base_timestamp.operation_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::operation_invalid, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x06,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x06}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x41}), "sys.metrics.datatype.base_timestamp.operation_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::bounds_overflow_underflow, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x04,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x04}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x41}), "sys.metrics.datatype.base_timestamp.operation_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::bounds_overflow_underflow, Uuid({0xde,0xdc,0x1c,0x9b,0x38,0x79,0x57,0x70,0x8f,0x8e,0xc0,0xc9,0x27,0xa0,0xc3,0x4d}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x41}), "sys.metrics.datatype.base_timestamp.operation_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::resource_cancellation, Uuid({0xde,0xdc,0x1c,0x9b,0x38,0x79,0x57,0x70,0x8f,0x8e,0xc0,0xc9,0x27,0xa0,0xc3,0x4d}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x41}), "sys.metrics.datatype.base_timestamp.operation_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::resource_cancellation, Uuid({0x0f,0x42,0x5d,0xe2,0x6b,0xc7,0x51,0xd4,0x8d,0x34,0x72,0xd1,0x07,0xad,0x45,0xe7}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x42}), "sys.metrics.datatype.base_timestamp.cancellation_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::compression_corruption, Uuid({0x26,0x0f,0x5e,0xac,0xb2,0x86,0x5a,0x00,0x81,0x6c,0xc4,0xc2,0xa8,0xf1,0xa2,0x5e}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x44}), "sys.metrics.datatype.base_timestamp.decode_corruption_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::compression_corruption, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x09,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x09}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x43}), "sys.metrics.datatype.base_timestamp.serialization_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::encryption_auth_key, Uuid({0x1a,0x8d,0x26,0xb0,0x87,0x55,0x56,0xad,0xa1,0x70,0xb2,0x4a,0x3a,0xaf,0x14,0x07}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x43}), "sys.metrics.datatype.base_timestamp.serialization_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::encryption_auth_key, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x09,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x09}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x43}), "sys.metrics.datatype.base_timestamp.serialization_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::wire_decode_encode, Uuid({0xf7,0xc7,0x36,0x87,0xc7,0x8d,0x54,0x6a,0xbc,0x93,0x02,0x8b,0x52,0xb4,0x35,0xcf}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x43}), "sys.metrics.datatype.base_timestamp.serialization_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::wire_decode_encode, Uuid({0x26,0x0f,0x5e,0xac,0xb2,0x86,0x5a,0x00,0x81,0x6c,0xc4,0xc2,0xa8,0xf1,0xa2,0x5e}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x44}), "sys.metrics.datatype.base_timestamp.decode_corruption_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::wire_decode_encode, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x03,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x03}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x44}), "sys.metrics.datatype.base_timestamp.decode_corruption_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::wire_decode_encode, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x08,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x08}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x43}), "sys.metrics.datatype.base_timestamp.serialization_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::wire_decode_encode, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x0d,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0d}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x43}), "sys.metrics.datatype.base_timestamp.serialization_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::index_key, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x06,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x06}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x47}), "sys.metrics.datatype.base_timestamp.projection_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::index_key, Uuid({0x01,0xa1,0x0c,0x00,0x20,0x00,0x70,0x0d,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0d}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x47}), "sys.metrics.datatype.base_timestamp.projection_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::index_key, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x07,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x07}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x47}), "sys.metrics.datatype.base_timestamp.projection_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::index_key, Uuid({0xde,0xdc,0x1c,0x9b,0x38,0x79,0x57,0x70,0x8f,0x8e,0xc0,0xc9,0x27,0xa0,0xc3,0x4d}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x47}), "sys.metrics.datatype.base_timestamp.projection_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::domain_validation, Uuid({0x58,0x8f,0xaf,0xd0,0x63,0x86,0x55,0xcf,0x84,0x4e,0xf5,0xb0,0xdc,0x22,0xc7,0x00}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x41}), "sys.metrics.datatype.base_timestamp.operation_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::domain_validation, Uuid({0xab,0xa0,0x82,0x8d,0x77,0xc9,0x50,0xde,0x98,0x42,0x7f,0x20,0x47,0x10,0x02,0x14}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x41}), "sys.metrics.datatype.base_timestamp.operation_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::domain_validation, Uuid({0x62,0xbe,0x50,0x52,0xad,0xbc,0x59,0xaa,0xb8,0x4b,0xae,0x60,0xa8,0xc4,0x0c,0x0f}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x41}), "sys.metrics.datatype.base_timestamp.operation_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::recovery_corruption, Uuid({0x0c,0x7d,0x9a,0xaf,0x92,0xe6,0x59,0x05,0x9d,0xaf,0x09,0x74,0xb8,0x71,0xf9,0x4a}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x44}), "sys.metrics.datatype.base_timestamp.decode_corruption_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::recovery_corruption, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x03,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x03}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x44}), "sys.metrics.datatype.base_timestamp.decode_corruption_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::recovery_corruption, Uuid({0x01,0xa1,0x00,0x8e,0xc2,0x00,0x70,0x0e,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0e}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x44}), "sys.metrics.datatype.base_timestamp.decode_corruption_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::statistics_read, Uuid({0x1a,0x8d,0x26,0xb0,0x87,0x55,0x56,0xad,0xa1,0x70,0xb2,0x4a,0x3a,0xaf,0x14,0x07}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x41}), "sys.metrics.datatype.base_timestamp.operation_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::statistics_read, Uuid({0x01,0xa1,0x0c,0x00,0x20,0x00,0x70,0x0c,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0c}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x41}), "sys.metrics.datatype.base_timestamp.operation_refusal_total", "after final primary diagnostic selection exactly once"},
      {TimestampDiagnosticAxisV3::statistics_read, Uuid({0x01,0xa1,0x0c,0x00,0x20,0x00,0x70,0x0b,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x0b}), Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x41}), "sys.metrics.datatype.base_timestamp.operation_refusal_total", "after final primary diagnostic selection exactly once"},
  }};
  return rows;
}

TimestampDiagnosticMetricRouteResultV3 ResolveTimestampDiagnosticMetricRouteV3(
    const TimestampDiagnosticRouteKeyV3& key) noexcept {
  TimestampDiagnosticMetricRouteResultV3 result;
  for (const auto& route : TimestampDiagnosticMetricRoutesV3()) {
    if (route.diagnostic_family == key.axis &&
        route.diagnostic_uuid == key.diagnostic_uuid) {
      result.disposition = TimestampDiagnosticRouteFactDispositionV3::admitted;
      result.route = route;
      return result;
    }
  }
  return result;
}

TimestampDiagnosticRouteFactDispositionV3 ValidateTimestampDiagnosticMetricRouteTableV3(
    std::span<const TimestampDiagnosticMetricRouteV3> supplied,
    bool scalar_types_valid) noexcept {
  using D = TimestampDiagnosticRouteFactDispositionV3;
  if (!scalar_types_valid) return D::wrong_type;
  const auto expected = TimestampDiagnosticMetricRoutesV3();
  if (supplied.size() < expected.size()) return D::missing;
  if (supplied.size() > expected.size()) return D::unknown;
  for (std::size_t i = 0; i != supplied.size(); ++i) {
    for (std::size_t j = 0; j != i; ++j)
      if (supplied[i].diagnostic_family == supplied[j].diagnostic_family &&
          supplied[i].diagnostic_uuid == supplied[j].diagnostic_uuid)
        return D::duplicate;
    if (supplied[i].diagnostic_family != expected[i].diagnostic_family ||
        supplied[i].diagnostic_uuid != expected[i].diagnostic_uuid) {
      for (const auto& row : expected)
        if (supplied[i].diagnostic_family == row.diagnostic_family &&
            supplied[i].diagnostic_uuid == row.diagnostic_uuid)
          return D::reordered;
      return D::unknown;
    }
    if (supplied[i].metric_evidence_type_uuid !=
            expected[i].metric_evidence_type_uuid ||
        supplied[i].metric_name != expected[i].metric_name ||
        supplied[i].rule != expected[i].rule) return D::wrong_type;
  }
  return D::admitted;
}

std::span<const TimestampMetricEvidenceTypeDescriptorV3> TimestampMetricEvidenceTypesV3() noexcept {
  static constexpr std::array<TimestampMetricEvidenceTypeDescriptorV3,9> rows{{
    {TimestampMetricEvidenceTypeV3::operation_attempt,Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x3f}),1,"sys.metrics.datatype.base_timestamp.operation_attempt_total","timestamp_runtime","base_timestamp_operation_attempt_evidence_v1","metrics_registry_manager",Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x3f}),"obligation_axis","exact_25_declared_obligation_axis_tuples","after_declared_commit_point","registry_declared_obligation_axis","admit_once_after_declared_commit_point","no aggregate mutation and primary datatype outcome unchanged","outcome_committed=false"},
    {TimestampMetricEvidenceTypeV3::operation_success,Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x40}),1,"sys.metrics.datatype.base_timestamp.operation_success_total","timestamp_runtime","base_timestamp_operation_success_evidence_v1","metrics_registry_manager",Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x40}),"obligation_axis","exact_25_declared_obligation_axis_tuples","after_declared_commit_point","registry_declared_obligation_axis","admit_once_after_declared_commit_point","no aggregate mutation and primary datatype outcome unchanged","outcome_committed=true"},
    {TimestampMetricEvidenceTypeV3::operation_refusal,Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x41}),1,"sys.metrics.datatype.base_timestamp.operation_refusal_total","timestamp_diagnostic_router","base_timestamp_operation_refusal_evidence_v1","metrics_registry_manager",Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x41}),"obligation_axis","exact_25_declared_obligation_axis_tuples","after_declared_commit_point","registry_declared_obligation_axis","admit_once_after_declared_commit_point","no aggregate mutation and primary datatype outcome unchanged","outcome_committed=true"},
    {TimestampMetricEvidenceTypeV3::cancellation,Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x42}),1,"sys.metrics.datatype.base_timestamp.cancellation_total","timestamp_cancellation_checkpoint","base_timestamp_cancellation_evidence_v1","metrics_registry_manager",Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x42}),"obligation_axis","exact_25_declared_obligation_axis_tuples","after_declared_commit_point","registry_declared_obligation_axis","admit_once_after_declared_commit_point","no aggregate mutation and primary datatype outcome unchanged","outcome_committed=true"},
    {TimestampMetricEvidenceTypeV3::serialization_refusal,Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x43}),1,"sys.metrics.datatype.base_timestamp.serialization_refusal_total","timestamp_boundary_adapters","base_timestamp_serialization_refusal_evidence_v1","metrics_registry_manager",Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x43}),"obligation_axis","exact_12_declared_obligation_axis_tuples","after_declared_commit_point","registry_declared_obligation_axis","admit_once_after_declared_commit_point","no aggregate mutation and primary datatype outcome unchanged","outcome_committed=true"},
    {TimestampMetricEvidenceTypeV3::decode_corruption,Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x44}),1,"sys.metrics.datatype.base_timestamp.decode_corruption_total","timestamp_binary_decoders","base_timestamp_decode_corruption_evidence_v1","metrics_registry_manager",Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x44}),"obligation_axis","exact_14_declared_obligation_axis_tuples","after_declared_commit_point","registry_declared_obligation_axis","admit_once_after_declared_commit_point","no aggregate mutation and primary datatype outcome unchanged","outcome_committed=true"},
    {TimestampMetricEvidenceTypeV3::profile_refusal,Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x45}),1,"sys.metrics.datatype.base_timestamp.profile_refusal_total","timestamp_profile_validator","base_timestamp_profile_refusal_evidence_v1","metrics_registry_manager",Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x45}),"obligation_axis","exact_25_declared_obligation_axis_tuples","after_declared_commit_point","registry_declared_obligation_axis","admit_once_after_declared_commit_point","no aggregate mutation and primary datatype outcome unchanged","outcome_committed=true"},
    {TimestampMetricEvidenceTypeV3::cast_refusal,Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x46}),1,"sys.metrics.datatype.base_timestamp.cast_refusal_total","timestamp_cast_owner","base_timestamp_cast_refusal_evidence_v1","metrics_registry_manager",Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x46}),"obligation_axis","exact_7_declared_obligation_axis_tuples","after_declared_commit_point","registry_declared_obligation_axis","admit_once_after_declared_commit_point","no aggregate mutation and primary datatype outcome unchanged","outcome_committed=true"},
    {TimestampMetricEvidenceTypeV3::projection_refusal,Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x47}),1,"sys.metrics.datatype.base_timestamp.projection_refusal_total","timestamp_projection_provider","base_timestamp_projection_refusal_evidence_v1","metrics_registry_manager",Uuid({0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,0x47}),"obligation_axis","exact_6_declared_obligation_axis_tuples","after_declared_commit_point","registry_declared_obligation_axis","admit_once_after_declared_commit_point","no aggregate mutation and primary datatype outcome unchanged","outcome_committed=true"},
  }}; return rows;
}

std::span<const TimestampMetricClosedLabelTupleV3>
TimestampMetricClosedLabelTuplesV3() noexcept {
  static constexpr std::array<TimestampMetricClosedLabelTupleV3, 164> rows{{
      {TimestampMetricEvidenceTypeV3::operation_attempt, {{{"obligation_axis", "canonical_specification"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_attempt, {{{"obligation_axis", "identity"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_attempt, {{{"obligation_axis", "memory"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_attempt, {{{"obligation_axis", "disk"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_attempt, {{{"obligation_axis", "sblr"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_attempt, {{{"obligation_axis", "persistence"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_attempt, {{{"obligation_axis", "rollback"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_attempt, {{{"obligation_axis", "concurrency"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_attempt, {{{"obligation_axis", "operations"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_attempt, {{{"obligation_axis", "comparison_hash"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_attempt, {{{"obligation_axis", "bounds"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_attempt, {{{"obligation_axis", "loss"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_attempt, {{{"obligation_axis", "cast"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_attempt, {{{"obligation_axis", "index"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_attempt, {{{"obligation_axis", "wire"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_attempt, {{{"obligation_axis", "compression"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_attempt, {{{"obligation_axis", "encryption"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_attempt, {{{"obligation_axis", "protection_combinations"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_attempt, {{{"obligation_axis", "diagnostics"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_attempt, {{{"obligation_axis", "malformed"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_attempt, {{{"obligation_axis", "resources"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_attempt, {{{"obligation_axis", "recovery"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_attempt, {{{"obligation_axis", "backup"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_attempt, {{{"obligation_axis", "boundary_integration"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_attempt, {{{"obligation_axis", "regression"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_success, {{{"obligation_axis", "canonical_specification"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_success, {{{"obligation_axis", "identity"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_success, {{{"obligation_axis", "memory"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_success, {{{"obligation_axis", "disk"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_success, {{{"obligation_axis", "sblr"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_success, {{{"obligation_axis", "persistence"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_success, {{{"obligation_axis", "rollback"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_success, {{{"obligation_axis", "concurrency"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_success, {{{"obligation_axis", "operations"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_success, {{{"obligation_axis", "comparison_hash"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_success, {{{"obligation_axis", "bounds"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_success, {{{"obligation_axis", "loss"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_success, {{{"obligation_axis", "cast"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_success, {{{"obligation_axis", "index"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_success, {{{"obligation_axis", "wire"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_success, {{{"obligation_axis", "compression"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_success, {{{"obligation_axis", "encryption"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_success, {{{"obligation_axis", "protection_combinations"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_success, {{{"obligation_axis", "diagnostics"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_success, {{{"obligation_axis", "malformed"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_success, {{{"obligation_axis", "resources"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_success, {{{"obligation_axis", "recovery"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_success, {{{"obligation_axis", "backup"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_success, {{{"obligation_axis", "boundary_integration"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_success, {{{"obligation_axis", "regression"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_refusal, {{{"obligation_axis", "canonical_specification"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_refusal, {{{"obligation_axis", "identity"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_refusal, {{{"obligation_axis", "memory"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_refusal, {{{"obligation_axis", "disk"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_refusal, {{{"obligation_axis", "sblr"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_refusal, {{{"obligation_axis", "persistence"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_refusal, {{{"obligation_axis", "rollback"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_refusal, {{{"obligation_axis", "concurrency"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_refusal, {{{"obligation_axis", "operations"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_refusal, {{{"obligation_axis", "comparison_hash"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_refusal, {{{"obligation_axis", "bounds"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_refusal, {{{"obligation_axis", "loss"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_refusal, {{{"obligation_axis", "cast"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_refusal, {{{"obligation_axis", "index"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_refusal, {{{"obligation_axis", "wire"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_refusal, {{{"obligation_axis", "compression"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_refusal, {{{"obligation_axis", "encryption"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_refusal, {{{"obligation_axis", "protection_combinations"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_refusal, {{{"obligation_axis", "diagnostics"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_refusal, {{{"obligation_axis", "malformed"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_refusal, {{{"obligation_axis", "resources"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_refusal, {{{"obligation_axis", "recovery"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_refusal, {{{"obligation_axis", "backup"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_refusal, {{{"obligation_axis", "boundary_integration"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::operation_refusal, {{{"obligation_axis", "regression"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cancellation, {{{"obligation_axis", "canonical_specification"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cancellation, {{{"obligation_axis", "identity"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cancellation, {{{"obligation_axis", "memory"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cancellation, {{{"obligation_axis", "disk"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cancellation, {{{"obligation_axis", "sblr"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cancellation, {{{"obligation_axis", "persistence"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cancellation, {{{"obligation_axis", "rollback"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cancellation, {{{"obligation_axis", "concurrency"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cancellation, {{{"obligation_axis", "operations"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cancellation, {{{"obligation_axis", "comparison_hash"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cancellation, {{{"obligation_axis", "bounds"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cancellation, {{{"obligation_axis", "loss"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cancellation, {{{"obligation_axis", "cast"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cancellation, {{{"obligation_axis", "index"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cancellation, {{{"obligation_axis", "wire"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cancellation, {{{"obligation_axis", "compression"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cancellation, {{{"obligation_axis", "encryption"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cancellation, {{{"obligation_axis", "protection_combinations"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cancellation, {{{"obligation_axis", "diagnostics"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cancellation, {{{"obligation_axis", "malformed"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cancellation, {{{"obligation_axis", "resources"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cancellation, {{{"obligation_axis", "recovery"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cancellation, {{{"obligation_axis", "backup"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cancellation, {{{"obligation_axis", "boundary_integration"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cancellation, {{{"obligation_axis", "regression"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::serialization_refusal, {{{"obligation_axis", "sblr"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::serialization_refusal, {{{"obligation_axis", "persistence"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::serialization_refusal, {{{"obligation_axis", "wire"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::serialization_refusal, {{{"obligation_axis", "compression"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::serialization_refusal, {{{"obligation_axis", "encryption"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::serialization_refusal, {{{"obligation_axis", "protection_combinations"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::serialization_refusal, {{{"obligation_axis", "diagnostics"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::serialization_refusal, {{{"obligation_axis", "malformed"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::serialization_refusal, {{{"obligation_axis", "recovery"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::serialization_refusal, {{{"obligation_axis", "backup"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::serialization_refusal, {{{"obligation_axis", "boundary_integration"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::serialization_refusal, {{{"obligation_axis", "regression"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::decode_corruption, {{{"obligation_axis", "disk"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::decode_corruption, {{{"obligation_axis", "sblr"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::decode_corruption, {{{"obligation_axis", "persistence"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::decode_corruption, {{{"obligation_axis", "rollback"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::decode_corruption, {{{"obligation_axis", "concurrency"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::decode_corruption, {{{"obligation_axis", "wire"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::decode_corruption, {{{"obligation_axis", "compression"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::decode_corruption, {{{"obligation_axis", "encryption"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::decode_corruption, {{{"obligation_axis", "protection_combinations"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::decode_corruption, {{{"obligation_axis", "malformed"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::decode_corruption, {{{"obligation_axis", "recovery"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::decode_corruption, {{{"obligation_axis", "backup"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::decode_corruption, {{{"obligation_axis", "boundary_integration"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::decode_corruption, {{{"obligation_axis", "regression"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::profile_refusal, {{{"obligation_axis", "canonical_specification"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::profile_refusal, {{{"obligation_axis", "identity"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::profile_refusal, {{{"obligation_axis", "memory"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::profile_refusal, {{{"obligation_axis", "disk"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::profile_refusal, {{{"obligation_axis", "sblr"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::profile_refusal, {{{"obligation_axis", "persistence"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::profile_refusal, {{{"obligation_axis", "rollback"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::profile_refusal, {{{"obligation_axis", "concurrency"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::profile_refusal, {{{"obligation_axis", "operations"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::profile_refusal, {{{"obligation_axis", "comparison_hash"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::profile_refusal, {{{"obligation_axis", "bounds"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::profile_refusal, {{{"obligation_axis", "loss"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::profile_refusal, {{{"obligation_axis", "cast"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::profile_refusal, {{{"obligation_axis", "index"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::profile_refusal, {{{"obligation_axis", "wire"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::profile_refusal, {{{"obligation_axis", "compression"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::profile_refusal, {{{"obligation_axis", "encryption"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::profile_refusal, {{{"obligation_axis", "protection_combinations"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::profile_refusal, {{{"obligation_axis", "diagnostics"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::profile_refusal, {{{"obligation_axis", "malformed"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::profile_refusal, {{{"obligation_axis", "resources"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::profile_refusal, {{{"obligation_axis", "recovery"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::profile_refusal, {{{"obligation_axis", "backup"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::profile_refusal, {{{"obligation_axis", "boundary_integration"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::profile_refusal, {{{"obligation_axis", "regression"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cast_refusal, {{{"obligation_axis", "bounds"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cast_refusal, {{{"obligation_axis", "loss"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cast_refusal, {{{"obligation_axis", "cast"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cast_refusal, {{{"obligation_axis", "diagnostics"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cast_refusal, {{{"obligation_axis", "malformed"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cast_refusal, {{{"obligation_axis", "resources"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::cast_refusal, {{{"obligation_axis", "regression"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::projection_refusal, {{{"obligation_axis", "comparison_hash"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::projection_refusal, {{{"obligation_axis", "index"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::projection_refusal, {{{"obligation_axis", "diagnostics"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::projection_refusal, {{{"obligation_axis", "resources"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::projection_refusal, {{{"obligation_axis", "boundary_integration"}, {"", ""}}}, 1},
      {TimestampMetricEvidenceTypeV3::projection_refusal, {{{"obligation_axis", "regression"}, {"", ""}}}, 1},
  }};
  return rows;
}

TimestampMetricEvidenceEnvelopeIdentityV3
TimestampMetricEvidenceEnvelopeIdentityV3Value() noexcept {
  return {Uuid({0x01,0xa1,0x04,0xec,0x8e,0x3c,0x7d,0xff,
                0x8e,0x89,0x86,0x8e,0xcc,0x2a,0x8a,0x1c}), 1};
}

TimestampMetricEvidenceValidationResultV3 ValidateTimestampMetricEvidenceV3(
    const TimestampMetricEvidenceEnvelopeV3& evidence,
    const TimestampMetricEvidenceValidationContextV3& context) noexcept {
  using D = TimestampMetricEvidenceDispositionV3;
  TimestampMetricEvidenceValidationResultV3 result;
  auto refuse = [&](D disposition) {
    result.disposition = disposition;
    return result;
  };
  const auto exact_envelope = TimestampMetricEvidenceEnvelopeIdentityV3Value();
  if (!evidence.all_required_fields_present ||
      evidence.envelope_identity.schema_uuid.is_nil() ||
      !evidence.envelope_identity.generation || evidence.evidence_type_uuid.is_nil() ||
      evidence.metric_uuid.is_nil() || evidence.source_event_uuid.is_nil() ||
      evidence.database_uuid.is_nil() ||
      evidence.node_uuid.is_nil() || !evidence.process_epoch ||
      !evidence.event_sequence || !evidence.monotonic_timestamp_ns ||
      evidence.closed_label_tuple.empty()) return refuse(D::missing_field);
  if (!evidence.scalar_types_valid) return refuse(D::wrong_scalar);
  if (evidence.envelope_identity.schema_uuid != exact_envelope.schema_uuid ||
      evidence.envelope_identity.generation != exact_envelope.generation)
    return refuse(D::unknown_evidence_type);
  const auto descriptors = TimestampMetricEvidenceTypesV3();
  std::size_t type_index = descriptors.size();
  for (std::size_t i = 0; i != descriptors.size(); ++i) {
    if (descriptors[i].evidence_type_uuid == evidence.evidence_type_uuid) {
      type_index = i;
      break;
    }
  }
  if (type_index == descriptors.size()) return refuse(D::unknown_evidence_type);
  if (evidence.metric_uuid != descriptors[type_index].metric_uuid)
    return refuse(D::unknown_evidence_type);
  result.type = descriptors[type_index].type;
  bool tuple_admitted = false;
  for (const auto& tuple : TimestampMetricClosedLabelTuplesV3()) {
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
  const bool expected_committed =
      result.type != TimestampMetricEvidenceTypeV3::operation_attempt;
  if (evidence.outcome_committed != expected_committed)
    return refuse(D::outcome_not_committed);
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

TimestampMetricSameSourceFactV3 ClassifyTimestampMetricSameSourceEmissionV3(
    const TimestampDiagnosticRouteKeyV3& route,
    const TimestampMetricEvidenceIdempotencyFactV3& first,
    const TimestampMetricEvidenceIdempotencyFactV3& second) noexcept {
  using D = TimestampMetricSameSourceDispositionV3;
  TimestampMetricSameSourceFactV3 result;
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
  const auto descriptors = TimestampMetricEvidenceTypesV3();
  auto type_index = [&](const platform::Uuid& uuid) {
    for (std::size_t i = 0; i != descriptors.size(); ++i)
      if (descriptors[i].evidence_type_uuid == uuid) return i;
    return descriptors.size();
  };
  const auto first_type = type_index(first.evidence_type_uuid);
  const auto second_type = type_index(second.evidence_type_uuid);
  const auto operation_refusal = static_cast<std::size_t>(
      TimestampMetricEvidenceTypeV3::operation_refusal);
  const auto specialized = [](std::size_t type) {
    return type >= static_cast<std::size_t>(
                       TimestampMetricEvidenceTypeV3::serialization_refusal) &&
           type <= static_cast<std::size_t>(
                       TimestampMetricEvidenceTypeV3::projection_refusal);
  };
  const auto registered_route = ResolveTimestampDiagnosticMetricRouteV3(route);
  const bool route_admitted =
      registered_route.disposition ==
          TimestampDiagnosticRouteFactDispositionV3::admitted;
  const bool forward_route_exact = route_admitted &&
      registered_route.route.metric_evidence_type_uuid ==
          second.evidence_type_uuid;
  const bool reverse_route_exact = route_admitted &&
      registered_route.route.metric_evidence_type_uuid ==
          first.evidence_type_uuid;
  const bool registered_forward = first_type == operation_refusal &&
                                  specialized(second_type) &&
                                  forward_route_exact;
  const bool registered_reverse =
      second_type == operation_refusal && specialized(first_type) &&
      reverse_route_exact;
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
