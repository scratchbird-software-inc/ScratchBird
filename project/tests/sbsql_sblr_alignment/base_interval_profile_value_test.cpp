// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "datatype_interval.hpp"
#include "hash_digest.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string_view>

namespace dt = scratchbird::core::datatypes;
namespace p = scratchbird::core::platform;
namespace h = scratchbird::core::hash;
namespace {
unsigned checks;
void Check(bool value, std::string_view what) { ++checks; if (!value) { std::cerr << "FAIL " << what << '\n'; std::exit(EXIT_FAILURE); } }
p::Uuid D710() { return p::Uuid(std::array<p::byte,16>{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x10}); }
void Put16(p::byte* out, p::u16 v) { out[0]=p::byte(v); out[1]=p::byte(v>>8); }
void Put32(p::byte* out, p::u32 v) { for (unsigned i=0;i<4;++i) out[i]=p::byte(v>>(8*i)); }
void Put64(p::byte* out, p::u64 v) { for (unsigned i=0;i<8;++i) out[i]=p::byte(v>>(8*i)); }
void PutUuid(p::byte* out, const p::Uuid& v) { std::copy(v.bytes.begin(),v.bytes.end(),out); }
void PutPolicy(p::byte* out, const dt::DatatypePolicyIdentityV3& v) { PutUuid(out,v.uuid); Put64(out+16,v.generation); }
std::shared_ptr<const dt::IntervalValidatedProfileHandleV3> Profile() {
  auto result=dt::BuildCurrentIntervalValidatedProfileHandleV3(D710()); Check(result.ok(),"build current d710 profile");
  return std::make_shared<const dt::IntervalValidatedProfileHandleV3>(std::move(result.profile));
}
std::array<p::byte,dt::kIntervalProfileMaterialBytesV3> RebuildProfile(const dt::IntervalValidatedProfileHandleV3& x) {
  std::array<p::byte,dt::kIntervalProfileMaterialBytesV3> b{}; std::memcpy(b.data(),"SBINPP01",8); Put16(b.data()+8,1);Put16(b.data()+10,608);Put32(b.data()+12,608);
  PutUuid(b.data()+16,x.receipt.catalog_snapshot_uuid);Put64(b.data()+32,x.receipt.catalog_generation);Put64(b.data()+40,x.receipt.registry_generation);
  const auto& i=x.identity.legacy_fields;PutUuid(b.data()+48,i.descriptor_uuid);Put64(b.data()+64,i.descriptor_generation);PutUuid(b.data()+72,i.type_uuid);Put64(b.data()+88,i.type_generation);PutUuid(b.data()+96,i.codec_uuid);Put32(b.data()+112,i.codec_version);Put64(b.data()+120,i.codec_generation);
  std::array<dt::DatatypePolicyIdentityV3,19> q{{x.identity.descriptor_policy,x.identity.canonicalization_policy,x.identity.ordering_policy,x.identity.hash_policy,x.identity.operation_policy,x.render_policy,x.cast_policy,x.subtype_policy,x.precision_policy,x.normalization_policy,x.temporal_application_boundary_policy,x.storage_epoch_policy,x.index_policy,x.statistics_policy,x.backup_transport_policy,x.protection_policy,x.component_adapter_policy,x.diagnostic_policy,x.metric_policy}};
  for (std::size_t n=0;n<q.size();++n) PutPolicy(b.data()+128+n*24,q[n]);
  Put32(b.data()+584,1);Put32(b.data()+588,16);Put32(b.data()+592,7);Put32(b.data()+596,1);Put32(b.data()+600,221);Put32(b.data()+604,13);return b;
}
std::array<p::byte,dt::kIntervalEqualityMaterialBytesV3> RebuildEquality(const dt::IntervalValidatedProfileHandleV3& x) {
  std::array<p::byte,dt::kIntervalEqualityMaterialBytesV3> b{}; std::memcpy(b.data(),"SBINEQ01",8);Put16(b.data()+8,1);Put16(b.data()+10,344);Put32(b.data()+12,344);
  PutUuid(b.data()+16,x.receipt.catalog_snapshot_uuid);Put64(b.data()+32,x.receipt.catalog_generation);Put64(b.data()+40,x.receipt.registry_generation);
  const auto&i=x.identity.legacy_fields;PutUuid(b.data()+48,i.descriptor_uuid);Put64(b.data()+64,i.descriptor_generation);PutUuid(b.data()+72,i.type_uuid);Put64(b.data()+88,i.type_generation);PutUuid(b.data()+96,i.codec_uuid);Put32(b.data()+112,i.codec_version);Put64(b.data()+120,i.codec_generation);
  std::array<dt::DatatypePolicyIdentityV3,8> q{{x.identity.descriptor_policy,x.identity.canonicalization_policy,x.identity.ordering_policy,x.identity.hash_policy,x.subtype_policy,x.precision_policy,x.normalization_policy,x.storage_epoch_policy}};
  for(std::size_t n=0;n<q.size();++n) PutPolicy(b.data()+128+n*24,q[n]);
  Put32(b.data()+320,63);Put32(b.data()+324,16);Put32(b.data()+328,1);Put32(b.data()+332,1);Put32(b.data()+336,1);Put32(b.data()+340,0);return b;
}
void Authority(const std::shared_ptr<const dt::IntervalValidatedProfileHandleV3>& x) {
  auto pm=RebuildProfile(*x); auto em=RebuildEquality(*x); Check(pm==x->profile_material,"independently rebuilt SBINPP01");Check(em==x->equality_material,"independently rebuilt SBINEQ01");
  auto pd=h::ComputeSha256DigestNative(pm.data(),pm.size()),ed=h::ComputeSha256DigestNative(em.data(),em.size());Check(pd.ok()&&pd.digest==x->profile_fingerprint,"profile digest from rebuilt bytes");Check(ed.ok()&&ed.digest==x->equality_fingerprint,"equality digest from rebuilt bytes");
  Check(h::HexLower(pd.digest)=="db1e0236a8dd8a04561a57868ccd1ab8bad508f3c1c751aa5d6ba8b5b42823eb","frozen profile SHA");Check(h::HexLower(ed.digest)=="c0783942e7db60aa79346ddc223e48fabcc197b1894cc81490a4f48b90d498f6","frozen equality SHA");
  Check(dt::kIntervalCanonicalIdentityCountV3==110&&dt::kIntervalUuidIncidentPairCountV3==219&&dt::kIntervalClosedCastPolicyRowsV3==221&&dt::kIntervalClosedCastDecisionsV3==663&&dt::kIntervalIntrinsicOperationRowsV3==13,"frozen audit cardinalities");
  for(std::size_t n=0;n<pm.size();++n){auto bad=*x;bad.profile_material[n]^=1;Check(!dt::ValidateIntervalProfileHandleV3(bad).ok(),"every profile byte authenticated");}
  for(std::size_t n=0;n<em.size();++n){auto bad=*x;bad.equality_material[n]^=1;Check(!dt::ValidateIntervalProfileHandleV3(bad).ok(),"every equality byte authenticated");}
}
void Values(const std::shared_ptr<const dt::IntervalValidatedProfileHandleV3>& profile) {
  constexpr std::array<std::array<std::int64_t,3>,7> cases{{{{INT32_MIN,INT32_MIN,INT64_MIN}},{{INT32_MAX,INT32_MAX,INT64_MAX}},{{-1,-1,-1}},{{0,0,0}},{{1,1,1}},{{INT32_MIN,INT32_MAX,0}},{{INT32_MAX,INT32_MIN,0}}}};
  for(const auto& c:cases){auto v=dt::ConstructIntervalV3(profile,c[0],c[1],c[2]);Check(v.ok(),"construct extrema");auto b=dt::EncodeCanonicalIntervalComponentV3(v.value);Check(b.ok()&&b.bytes.size()==16,"LE16 encode");auto d=dt::DecodeCanonicalIntervalComponentNoAllocV3(*profile,dt::IntervalValueStateV3::value,true,b.bytes);Check(d.ok()&&d.value.months==c[0]&&d.value.civil_days==c[1]&&d.value.fixed_nanoseconds==c[2],"LE16 roundtrip");auto t=dt::RenderCanonicalIntervalV3(v.value);Check(t.ok()&&t.text.size()<=63,"canonical text bound");auto q=dt::ParseCanonicalIntervalV3(profile,t.text);Check(q.ok()&&q.value.months==c[0]&&q.value.civil_days==c[1]&&q.value.fixed_nanoseconds==c[2],"text roundtrip");}
  auto longest=dt::ConstructIntervalV3(profile,INT32_MIN,INT32_MIN,INT64_MIN);auto text=dt::RenderCanonicalIntervalV3(longest.value);Check(text.ok()&&text.text.size()==63,"exact maximum text extent");
  std::array<p::byte,17> bytes{};Check(!dt::DecodeCanonicalIntervalComponentNoAllocV3(*profile,dt::IntervalValueStateV3::value,true,bytes).ok(),"trailing byte refused");Check(!dt::DecodeCanonicalIntervalComponentNoAllocV3(*profile,dt::IntervalValueStateV3::sql_null,true,std::span<const p::byte>(bytes.data(),1)).ok(),"dirty NULL refused");Check(dt::DecodeCanonicalIntervalComponentNoAllocV3(*profile,dt::IntervalValueStateV3::sql_null,true,{}).ok(),"clean NULL admitted");
}
}
int main(){auto profile=Profile();Authority(profile);Values(profile);std::cout<<"PASS checks="<<checks<<" profile_bytes=608 equality_bytes=344 identities=110 uuid_pairs=219 cast_rows=221 cast_decisions=663 intrinsics=13\n";}
