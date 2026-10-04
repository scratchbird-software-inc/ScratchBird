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
#include <string>
#include <string_view>
#include <vector>

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
  struct Vector { std::int32_t m,d; std::int64_t n; std::string_view text,hex,hash; };
  constexpr std::array<Vector,15> cases{{
    {0,0,0,"SBINTERVAL1:M=0;D=0;NS=0","00000000000000000000000000000000","cffb45920a9a28260bce692712cbced439acec376b26382e789c4024d9991904"},
    {INT32_MIN,0,0,"SBINTERVAL1:M=-2147483648;D=0;NS=0","00000080000000000000000000000000","154ca7644d3a9fba8ebe08a6e210e83474ca9f98e09b716e1486c0edff4199c7"},
    {INT32_MAX,0,0,"SBINTERVAL1:M=2147483647;D=0;NS=0","ffffff7f000000000000000000000000","a1f5ba5da06e70c2beaa047a8fb3413053e84d23433669b9886285fdcd9fc369"},
    {0,INT32_MIN,0,"SBINTERVAL1:M=0;D=-2147483648;NS=0","00000000000000800000000000000000","d96c0a6c243e11fa4c3bcd5184948c5e5b45ee73c516ba573af935928019736c"},
    {0,INT32_MAX,0,"SBINTERVAL1:M=0;D=2147483647;NS=0","00000000ffffff7f0000000000000000","2d40934e54ac946597eb59d3af302f9d10701c0e75922cd38e02d8bd0831f728"},
    {0,0,INT64_MIN,"SBINTERVAL1:M=0;D=0;NS=-9223372036854775808","00000000000000000000000000000080","5d3b755af3eba2d58935dd6cf5b9c32ecfb2139a9a2db2d00065093262f6c9e9"},
    {0,0,INT64_MAX,"SBINTERVAL1:M=0;D=0;NS=9223372036854775807","0000000000000000ffffffffffffff7f","f57135f2f7bb83150515e457c0a8ef24ec12845cf6159937cdc4227f6155d8bb"},
    {INT32_MIN,INT32_MIN,INT64_MIN,"SBINTERVAL1:M=-2147483648;D=-2147483648;NS=-9223372036854775808","00000080000000800000000000000080","78175b7bffdd914f79a69a007f668eeaeeced6edc61fc0ff12a8a7b8cb30eef2"},
    {INT32_MAX,INT32_MAX,INT64_MAX,"SBINTERVAL1:M=2147483647;D=2147483647;NS=9223372036854775807","ffffff7fffffff7fffffffffffffff7f","8abae88cb0cba3746ab86b95f9f9329a47e07c1cd864c2f43718565f68188ce5"},
    {17,-23,86400000000000ll,"SBINTERVAL1:M=17;D=-23;NS=86400000000000","11000000e9ffffff00004f91944e0000","8979b91d012d260bb6276f206e463cd7d924d3dae2c37a2dc29eb0a7714b1f81"},
    {-17,23,-86400000000000ll,"SBINTERVAL1:M=-17;D=23;NS=-86400000000000","efffffff170000000000b16e6bb1ffff","da214d761146ed1ec75b6af3e68343392e5159e81cfc48b149cc65026393093f"},
    {0,0,86400000000000ll,"SBINTERVAL1:M=0;D=0;NS=86400000000000","000000000000000000004f91944e0000","d70523cfd69c74ef63403c59b0c857944c1652a7dd94332cd13a6b1d30011bcb"},
    {0,31,0,"SBINTERVAL1:M=0;D=31;NS=0","000000001f0000000000000000000000","a8c1040f67b96b37e58a4c4b4b13476a02b932ad9e0996fa82623c9a49f76dfe"},
    {0,9,10,"SBINTERVAL1:M=0;D=9;NS=10","00000000090000000a00000000000000","93344024e28b6087b1d95b43374c177888f03f9b188eeb56ea6dd7d71461e71a"},
    {7,0,0,"SBINTERVAL1:M=7;D=0;NS=0","07000000000000000000000000000000","3794aca49f220db0164dc59abbe12d9cd55eef7750f4fa9efba8d4fd42524bfc"}
  }};
  auto hex=[](std::span<const p::byte> bytes){static constexpr char x[]="0123456789abcdef";std::string s;s.reserve(bytes.size()*2);for(auto b:bytes){s.push_back(x[b>>4]);s.push_back(x[b&15]);}return s;};
  for(const auto& c:cases){auto v=dt::ConstructIntervalV3(profile,c.m,c.d,c.n);Check(v.ok(),"construct sealed vector");auto b=dt::EncodeCanonicalIntervalComponentV3(v.value);Check(b.ok()&&b.bytes.size()==16&&hex(b.bytes)==c.hex,"sealed LE16 vector");auto d=dt::DecodeCanonicalIntervalComponentNoAllocV3(*profile,dt::IntervalValueStateV3::value,true,b.bytes);Check(d.ok()&&d.value.months==c.m&&d.value.civil_days==c.d&&d.value.fixed_nanoseconds==c.n,"sealed LE16 roundtrip");auto t=dt::RenderCanonicalIntervalV3(v.value);Check(t.ok()&&t.text==c.text&&t.text.size()<=63,"sealed canonical text");auto q=dt::ParseCanonicalIntervalV3(profile,t.text);Check(q.ok()&&q.value.months==c.m&&q.value.civil_days==c.d&&q.value.fixed_nanoseconds==c.n,"sealed text roundtrip");auto hash=dt::HashIntervalValueV3(v.value);Check(hash.ok()&&hex(hash.bytes)==c.hash,"sealed hash vector");}
  struct InvalidText { std::string_view text, diagnostic; };constexpr std::array<InvalidText,25> invalid_text{{{"","CTI.TEMPORAL.INVALID_LITERAL"},{"sbinterval1:M=0;D=0;NS=0","CTI.TEMPORAL.INVALID_LITERAL"},{"INTERVAL:M=0;D=0;NS=0","CTI.TEMPORAL.INVALID_LITERAL"},{"SBINTERVAL1:M=+1;D=0;NS=0","CTI.TEMPORAL.INVALID_LITERAL"},{"SBINTERVAL1:M=-0;D=0;NS=0","CTI.TEMPORAL.INVALID_LITERAL"},{"SBINTERVAL1:M=01;D=0;NS=0","CTI.TEMPORAL.INVALID_LITERAL"},{"SBINTERVAL1:M=0;D=+1;NS=0","CTI.TEMPORAL.INVALID_LITERAL"},{"SBINTERVAL1:M=0;D=-0;NS=0","CTI.TEMPORAL.INVALID_LITERAL"},{"SBINTERVAL1:M=0;D=01;NS=0","CTI.TEMPORAL.INVALID_LITERAL"},{"SBINTERVAL1:M=0;D=0;NS=+1","CTI.TEMPORAL.INVALID_LITERAL"},{"SBINTERVAL1:M=0;D=0;NS=-0","CTI.TEMPORAL.INVALID_LITERAL"},{"SBINTERVAL1:M=0;D=0;NS=01","CTI.TEMPORAL.INVALID_LITERAL"},{"SBINTERVAL1: M=0;D=0;NS=0","CTI.TEMPORAL.INVALID_LITERAL"},{"SBINTERVAL1:M=0;D=0;NS=0 ","CTI.TEMPORAL.INVALID_LITERAL"},{"SBINTERVAL1:D=0;M=0;NS=0","CTI.TEMPORAL.INVALID_LITERAL"},{"SBINTERVAL1:M=0;D=0","CTI.TEMPORAL.INVALID_LITERAL"},{"SBINTERVAL1:M=0;D=0;NS=0;X=0","CTI.TEMPORAL.INVALID_LITERAL"},{"SBINTERVAL1:M=-2147483649;D=0;NS=0","CTI.TEMPORAL.RANGE_EXCEEDED"},{"SBINTERVAL1:M=2147483648;D=0;NS=0","CTI.TEMPORAL.RANGE_EXCEEDED"},{"SBINTERVAL1:M=0;D=-2147483649;NS=0","CTI.TEMPORAL.RANGE_EXCEEDED"},{"SBINTERVAL1:M=0;D=2147483648;NS=0","CTI.TEMPORAL.RANGE_EXCEEDED"},{"SBINTERVAL1:M=0;D=0;NS=-9223372036854775809","CTI.TEMPORAL.RANGE_EXCEEDED"},{"SBINTERVAL1:M=0;D=0;NS=9223372036854775808","CTI.TEMPORAL.RANGE_EXCEEDED"},{"P1M2DT3S","CTI.TEMPORAL.INVALID_LITERAL"},{"1 year 2 days","CTI.TEMPORAL.INVALID_LITERAL"}}};for(const auto& row:invalid_text){auto result=dt::ParseCanonicalIntervalV3(profile,row.text);Check(!result.ok()&&result.diagnostic.diagnostic_code==row.diagnostic&&result.value.profile==nullptr,"25 sealed invalid text vectors");}
  std::array<std::string,5> exceptional{{std::string("SBINTERVAL1:M=0;D=0;NS=\0" "0",25),"SBINTERVAL1:M=0;D=0;NS=0\n","SBINTERVAL1:M=0;D=0;NS=0\r\n","SBINTERVAL1:M=\xe2\x88\x92" "1;D=0;NS=0","SBINTERVAL1:M=\xd9\xa1;D=0;NS=0"}};for(const auto& text:exceptional)Check(!dt::ParseCanonicalIntervalV3(profile,std::string_view(text.data(),text.size())).ok(),"five sealed exceptional byte vectors");for(unsigned octet=0;octet<256;++octet){if(octet>='0'&&octet<='9')continue;std::string text="SBINTERVAL1:M=";text.push_back(static_cast<char>(octet));text.append(";D=0;NS=0");Check(!dt::ParseCanonicalIntervalV3(profile,std::string_view(text.data(),text.size())).ok(),"246 exhaustive single-octet invalid vectors");}
  auto longest=dt::ConstructIntervalV3(profile,INT32_MIN,INT32_MIN,INT64_MIN);auto text=dt::RenderCanonicalIntervalV3(longest.value);Check(text.ok()&&text.text.size()==63,"exact maximum text extent");
  std::array<p::byte,17> bytes{};Check(!dt::DecodeCanonicalIntervalComponentNoAllocV3(*profile,dt::IntervalValueStateV3::value,true,bytes).ok(),"trailing byte refused");Check(!dt::DecodeCanonicalIntervalComponentNoAllocV3(*profile,dt::IntervalValueStateV3::sql_null,true,std::span<const p::byte>(bytes.data(),1)).ok(),"dirty NULL refused");Check(dt::DecodeCanonicalIntervalComponentNoAllocV3(*profile,dt::IntervalValueStateV3::sql_null,true,{}).ok(),"clean NULL admitted");
  std::array<p::byte,17> misaligned{};auto source=dt::ConstructIntervalV3(profile,17,-23,86400000000000ll);Check(source.ok(),"misalignment source");auto raw=dt::EncodeCanonicalIntervalComponentV3(source.value);Check(raw.ok(),"misalignment encode");std::copy(raw.bytes.begin(),raw.bytes.end(),misaligned.begin()+1);auto md=dt::DecodeCanonicalIntervalComponentNoAllocV3(*profile,dt::IntervalValueStateV3::value,true,std::span<const p::byte>(misaligned.data()+1,16));Check(md.ok()&&md.value.months==17&&md.value.civil_days==-23,"misaligned LE16 admitted without unaligned access");
  dt::IntervalOwnedValueV3 nullv{profile,dt::IntervalValueStateV3::sql_null,0,0,0};auto nh=dt::HashIntervalValueV3(nullv);Check(nh.ok()&&hex(nh.bytes)=="bc67ce9998c0a782fa11ca0f9ce418ef25047fc4fc38fbbcb8caa6c6cf0327cf","sealed SQL NULL hash");
}
void Aliases(const std::shared_ptr<const dt::IntervalValidatedProfileHandleV3>& profile){
  for(unsigned n=0;n<5;++n){dt::IntervalAliasResolutionEvidenceV3 e;e.audit_case=static_cast<dt::IntervalRequiredAliasAuditCaseV3>(n);e.registry_resolution_succeeded=true;e.registry_receipt=profile->receipt;e.resolved_identity=profile->identity;auto r=dt::AuditIntervalRequiredAliasResolutionV3(e,*profile);Check(r.ok()&&r.same_interval_cohort&&!r.converter_required&&!r.runtime_name_authority_granted,"five aliases resolve to one authenticated cohort");}
  dt::IntervalAliasResolutionEvidenceV3 bad;bad.registry_resolution_succeeded=false;Check(!dt::AuditIntervalRequiredAliasResolutionV3(bad,*profile).ok(),"unresolved display name has no authority");
}
}
int main(){auto profile=Profile();Authority(profile);Values(profile);Aliases(profile);std::cout<<"PASS checks="<<checks<<" profile_bytes=608 equality_bytes=344 fixed_vectors=16 aliases=5 identities=110 uuid_pairs=219 cast_rows=221 cast_decisions=663 intrinsics=13\n";}
