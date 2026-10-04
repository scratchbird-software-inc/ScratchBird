// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../../src/core/datatypes/datatype_time.hpp"
#include "../../../src/core/time/time.hpp"
#include "disk_device.hpp"
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>
namespace dt=scratchbird::core::datatypes;
namespace p=scratchbird::core::platform;
namespace disk=scratchbird::storage::disk;
namespace fs=std::filesystem;
namespace { unsigned file_device_clock_calls=0;
 }
extern "C" scratchbird::core::time::ClockSnapshotResult __real__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv();
extern "C" scratchbird::core::time::ClockSnapshotResult __wrap__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv(){++file_device_clock_calls;
return __real__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv();
}
namespace{
unsigned checks=0;unsigned exact_sbdval_mutations=0,exact_sbdpv_mutations=0;
[[noreturn]]void Fail(std::string_view m){std::cerr<<"FAIL "<<m<<'\n';
std::exit(1);
}void Check(bool v,std::string_view m){++checks;
if(!v)Fail(m);
}
p::Uuid D709(){return{{1,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,9}};
}
std::shared_ptr<const dt::TimeValidatedProfileHandleV3> Profile(){auto r=dt::BuildCurrentTimeValidatedProfileHandleV3(D709());
Check(r.ok(),"profile");
return std::make_shared<const dt::TimeValidatedProfileHandleV3>(r.profile);
}

struct ExactFrame { p::u64 value; bool is_null; std::string_view hex; };
std::vector<p::byte> HexBytes(std::string_view text){std::vector<p::byte> out(text.size()/2);auto n=[](char c){return unsigned(c<='9'?c-'0':(c|32)-'a'+10);};for(std::size_t i=0;i<out.size();++i)out[i]=static_cast<p::byte>((n(text[2*i])<<4)|n(text[2*i+1]));return out;}
constexpr std::array<ExactFrame,21> kExactSbdval{{
{0ull,false,"53424456414c303191010000000020000800000000000000e3518eaf7e0dfe470000000000000000"},
{1ull,false,"53424456414c303191010000000020000800000000000000c2079fa4754603290100000000000000"},
{10ull,false,"53424456414c303191010000000020000800000000000000a90e2af1b4b7de010a00000000000000"},
{100ull,false,"53424456414c303191010000000020000800000000000000c74607f506cdf3616400000000000000"},
{1000ull,false,"53424456414c303191010000000020000800000000000000a631f3fa2fe0cd25e803000000000000"},
{10000ull,false,"53424456414c3031910100000000200008000000000000009a6d20320d5996bc1027000000000000"},
{100000ull,false,"53424456414c303191010000000020000800000000000000ec1dc17c4eb08916a086010000000000"},
{1000000ull,false,"53424456414c303191010000000020000800000000000000fed95aafd7fa587040420f0000000000"},
{10000000ull,false,"53424456414c3031910100000000200008000000000000007dc0fea77dfc53c58096980000000000"},
{100000000ull,false,"53424456414c30319101000000002000080000000000000086e0fa01733cb0ad00e1f50500000000"},
{1000000000ull,false,"53424456414c303191010000000020000800000000000000968eafa5eeaa844400ca9a3b00000000"},
{60000000000ull,false,"53424456414c303191010000000020000800000000000000b1eac324ad0766d6005847f80d000000"},
{3600000000000ull,false,"53424456414c3031910100000000200008000000000000005cbb2a08c40e14a900a0b83046030000"},
{999999999ull,false,"53424456414c3031910100000000200008000000000000001491c1d78b0405bfffc99a3b00000000"},
{59999999999ull,false,"53424456414c303191010000000020000800000000000000bbd7bed03cf77377ff5747f80d000000"},
{3599999999999ull,false,"53424456414c30319101000000002000080000000000000042c5f987eb4b3aceff9fb83046030000"},
{43200000000000ull,false,"53424456414c303191010000000020000800000000000000df6222e2be59bdb00080a7484a270000"},
{45296100000000ull,false,"53424456414c303191010000000020000800000000000000056f306c7db4be9d0041f35132290000"},
{86399000000000ull,false,"53424456414c303191010000000020000800000000000000ecb731c83552e51d0036b455944e0000"},
{86399999999999ull,false,"53424456414c3031910100000000200008000000000000009aaaa036d35003a2ffff4e91944e0000"},
{0ull,true,"53424456414c30319101000001002000000000000000000083039d73b00f6514"}
}};
constexpr std::array<ExactFrame,3> kExactSbdpv{{
{0ull,true,"53424450563030319101000000000000000000007426e447"},
{0ull,false,"5342445056303031910100000100000008000000e7c1eac50000000000000000"},
{86399999999999ull,false,"5342445056303031910100000100000008000000160691e6ffff4e91944e0000"}
}};
void ResealSbdval(std::vector<p::byte>* bytes){p::u64 h=1469598103934665603ull;for(std::size_t i=32;i<bytes->size();++i){h^=(*bytes)[i];h*=1099511628211ull;}p::StoreLittle64(bytes->data()+24,h);}
void ResealSbdpv(std::vector<p::byte>* bytes){p::u32 h=2166136261u;auto mix=[&](p::u32 v){h^=v;h*=16777619u;};mix(p::LoadLittle32(bytes->data()+8));mix(p::LoadLittle16(bytes->data()+12));for(std::size_t i=24;i<bytes->size();++i)mix((*bytes)[i]);p::StoreLittle32(bytes->data()+20,h);}
void Append(std::vector<p::byte>*out,std::span<const p::byte> record){std::array<p::byte,4>n{};
p::StoreLittle32(n.data(),static_cast<p::u32>(record.size()));
out->insert(out->end(),n.begin(),n.end());
out->insert(out->end(),record.begin(),record.end());
}
std::vector<p::byte> Reopen(const fs::path&path,std::span<const p::byte>image){file_device_clock_calls=0;
std::error_code ec;
fs::remove(path,ec);
fs::remove(path.string()+".sb.owner.lock",ec);
disk::FileDevice w;
Check(w.Open(path.string(),disk::FileOpenMode::create_new).ok(),"FileDevice create");
auto wr=w.WriteAt(0,image.data(),image.size());
Check(wr.ok()&&wr.bytes_transferred==image.size(),"FileDevice write");
Check(w.Sync().ok()&&w.Close().ok(),"FileDevice sync close");
disk::FileDevice r;
Check(r.Open(path.string(),disk::FileOpenMode::open_existing_read_only).ok(),"FileDevice reopen");
std::vector<p::byte> restored(image.size());
auto rd=r.ReadAt(0,restored.data(),restored.size());
Check(rd.ok()&&rd.bytes_transferred==restored.size()&&r.Close().ok(),"FileDevice read close");
Check(file_device_clock_calls==0,"FileDevice byte persistence has no clock dependency");
fs::remove(path,ec);
fs::remove(path.string()+".sb.owner.lock",ec);
return restored;
}
void CheckDiagnosticBound(const dt::TimeDiagnosticFactV3&d){Check(d.parameter_count<=d.parameters.size(),"bounded diagnostic parameter count");
Check(!d.diagnostic_code.empty(),"static diagnostic code");
}
bool Stop(void*) noexcept{return true;}
void Mutations(const std::shared_ptr<const dt::TimeValidatedProfileHandleV3>&profile,const std::vector<p::byte>&val,const std::vector<p::byte>&pv){
 unsigned val_executed=0,pv_executed=0;
 auto val_case=[&](std::string_view id,const dt::TimeViewResultV3& result,std::string_view code){Check(!result.ok()&&result.diagnostic.diagnostic_code==code,id);CheckDiagnosticBound(result.diagnostic);++val_executed;};
 auto pv_case=[&](std::string_view id,const dt::TimeViewResultV3& result,std::string_view code){Check(!result.ok()&&result.diagnostic.diagnostic_code==code,id);CheckDiagnosticBound(result.diagnostic);++pv_executed;};
 auto bad=val;bad.pop_back();val_case("SBDVAL01.mutation_001.extent",dt::DecodeTimeSbdvalComposedNoAllocV3(*profile,true,bad),"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID");
 bad=val;bad[0]^=1;val_case("SBDVAL01.mutation_002.magic",dt::DecodeTimeSbdvalComposedNoAllocV3(*profile,true,bad),"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID");
 bad=val;bad[7]^=1;val_case("SBDVAL01.mutation_003.version",dt::DecodeTimeSbdvalComposedNoAllocV3(*profile,true,bad),"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID");
 bad=val;bad[20]^=1;val_case("SBDVAL01.mutation_004.reserved",dt::DecodeTimeSbdvalComposedNoAllocV3(*profile,true,bad),"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID");
 bad=val;bad[32]^=1;val_case("SBDVAL01.mutation_005.integrity",dt::DecodeTimeSbdvalComposedNoAllocV3(*profile,true,bad),"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID");
 auto wrong=*profile;wrong.receipt.catalog_snapshot_uuid.bytes[0]^=1;val_case("SBDVAL01.mutation_006.receipt",dt::DecodeTimeSbdvalComposedNoAllocV3(wrong,true,val),"CTI.TEMPORAL.DESCRIPTOR_INVALID");
 wrong=*profile;wrong.identity.legacy_fields.descriptor_uuid.bytes[0]^=1;val_case("SBDVAL01.mutation_007.descriptor",dt::DecodeTimeSbdvalComposedNoAllocV3(wrong,true,val),"CTI.TEMPORAL.DESCRIPTOR_INVALID");
 bad=val;bad[8]^=1;val_case("SBDVAL01.mutation_008.type",dt::DecodeTimeSbdvalComposedNoAllocV3(*profile,true,bad),"CTI.TEMPORAL.DESCRIPTOR_INVALID");
 wrong=*profile;wrong.identity.legacy_fields.codec_uuid.bytes[0]^=1;val_case("SBDVAL01.mutation_009.codec",dt::DecodeTimeSbdvalComposedNoAllocV3(wrong,true,val),"CTI.TEMPORAL.DESCRIPTOR_INVALID");
 wrong=*profile;wrong.profile_material[0]^=1;val_case("SBDVAL01.mutation_010.profile",dt::DecodeTimeSbdvalComposedNoAllocV3(wrong,true,val),"CTI.TEMPORAL.DESCRIPTOR_INVALID");
 wrong=*profile;wrong.component_adapter_policy.uuid.bytes[0]^=1;val_case("SBDVAL01.mutation_011.policy",dt::DecodeTimeSbdvalComposedNoAllocV3(wrong,true,val),"CTI.TEMPORAL.DESCRIPTOR_INVALID");
 bad=val;bad[12]=1;val_case("SBDVAL01.mutation_012.state",dt::DecodeTimeSbdvalComposedNoAllocV3(*profile,true,bad),"DATATYPE.NULL_STATE.INVALID");
 bad=val;p::StoreLittle32(bad.data()+16,7);val_case("SBDVAL01.mutation_013.length",dt::DecodeTimeSbdvalComposedNoAllocV3(*profile,true,bad),"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID");
 bad=val;p::StoreLittle64(bad.data()+32,dt::kTimeMaximumNanosecondsV3+1);ResealSbdval(&bad);val_case("SBDVAL01.mutation_014.range",dt::DecodeTimeSbdvalComposedNoAllocV3(*profile,true,bad),"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID");
 dt::TimeExecutionControlV3 reencode{};reencode.force_reencode_mismatch_for_conformance=true;val_case("SBDVAL01.mutation_015.reencode",dt::DecodeTimeSbdvalComposedNoAllocV3(*profile,true,val,reencode),"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID");
 val_case("SBDVAL01.mutation_016.resource",dt::DecodeTimeSbdvalComposedNoAllocV3(*profile,true,val,{39,nullptr,nullptr}),"RESOURCE.BUDGET_EXCEEDED");
 val_case("SBDVAL01.mutation_017.cancellation",dt::DecodeTimeSbdvalComposedNoAllocV3(*profile,true,val,{~p::u64{0},Stop,nullptr}),"PROCESS.CANCELLED");

 bad=pv;bad.pop_back();pv_case("SBDPV001.mutation_001.extent",dt::DecodeTimeSbdpvComposedNoAllocV3(*profile,true,bad),"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID");
 bad=pv;bad[0]^=1;pv_case("SBDPV001.mutation_002.magic",dt::DecodeTimeSbdpvComposedNoAllocV3(*profile,true,bad),"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID");
 bad=pv;bad[7]^=1;pv_case("SBDPV001.mutation_003.version",dt::DecodeTimeSbdpvComposedNoAllocV3(*profile,true,bad),"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID");
 bad=pv;bad[14]^=1;pv_case("SBDPV001.mutation_004.reserved",dt::DecodeTimeSbdpvComposedNoAllocV3(*profile,true,bad),"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID");
 bad=pv;bad[24]^=1;pv_case("SBDPV001.mutation_005.integrity",dt::DecodeTimeSbdpvComposedNoAllocV3(*profile,true,bad),"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID");
 wrong=*profile;wrong.receipt.catalog_snapshot_uuid.bytes[0]^=1;pv_case("SBDPV001.mutation_006.receipt",dt::DecodeTimeSbdpvComposedNoAllocV3(wrong,true,pv),"CTI.TEMPORAL.DESCRIPTOR_INVALID");
 wrong=*profile;wrong.identity.legacy_fields.descriptor_uuid.bytes[0]^=1;pv_case("SBDPV001.mutation_007.descriptor",dt::DecodeTimeSbdpvComposedNoAllocV3(wrong,true,pv),"CTI.TEMPORAL.DESCRIPTOR_INVALID");
 bad=pv;bad[8]^=1;ResealSbdpv(&bad);pv_case("SBDPV001.mutation_008.type",dt::DecodeTimeSbdpvComposedNoAllocV3(*profile,true,bad),"CTI.TEMPORAL.DESCRIPTOR_INVALID");
 wrong=*profile;wrong.identity.legacy_fields.codec_uuid.bytes[0]^=1;pv_case("SBDPV001.mutation_009.codec",dt::DecodeTimeSbdpvComposedNoAllocV3(wrong,true,pv),"CTI.TEMPORAL.DESCRIPTOR_INVALID");
 wrong=*profile;wrong.profile_material[0]^=1;pv_case("SBDPV001.mutation_010.profile",dt::DecodeTimeSbdpvComposedNoAllocV3(wrong,true,pv),"CTI.TEMPORAL.DESCRIPTOR_INVALID");
 wrong=*profile;wrong.component_adapter_policy.uuid.bytes[0]^=1;pv_case("SBDPV001.mutation_011.policy",dt::DecodeTimeSbdpvComposedNoAllocV3(wrong,true,pv),"CTI.TEMPORAL.DESCRIPTOR_INVALID");
 bad=pv;p::StoreLittle16(bad.data()+12,2);ResealSbdpv(&bad);pv_case("SBDPV001.mutation_012.state",dt::DecodeTimeSbdpvComposedNoAllocV3(*profile,true,bad),"DATATYPE.NULL_STATE.INVALID");
 bad=pv;p::StoreLittle32(bad.data()+16,7);pv_case("SBDPV001.mutation_013.length",dt::DecodeTimeSbdpvComposedNoAllocV3(*profile,true,bad),"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID");
 bad=pv;p::StoreLittle64(bad.data()+24,dt::kTimeMaximumNanosecondsV3+1);ResealSbdpv(&bad);pv_case("SBDPV001.mutation_014.range",dt::DecodeTimeSbdpvComposedNoAllocV3(*profile,true,bad),"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID");
 pv_case("SBDPV001.mutation_015.reencode",dt::DecodeTimeSbdpvComposedNoAllocV3(*profile,true,pv,reencode),"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID");
 pv_case("SBDPV001.mutation_016.resource",dt::DecodeTimeSbdpvComposedNoAllocV3(*profile,true,pv,{31,nullptr,nullptr}),"RESOURCE.BUDGET_EXCEEDED");
 pv_case("SBDPV001.mutation_017.cancellation",dt::DecodeTimeSbdpvComposedNoAllocV3(*profile,true,pv,{~p::u64{0},Stop,nullptr}),"PROCESS.CANCELLED");
 Check(val_executed==17&&pv_executed==17,"exact 17 SBDVAL and 17 SBDPV mutation gates executed");exact_sbdval_mutations=val_executed;exact_sbdpv_mutations=pv_executed;
}

}
int main(){auto profile=Profile();

for(const auto& fixture:kExactSbdval){dt::TimeOwnedValueV3 value{profile,fixture.is_null?dt::TimeValueStateV3::sql_null:dt::TimeValueStateV3::value,fixture.value};auto encoded=dt::EncodeTimeSbdvalComposedV3(value,true);auto expected=HexBytes(fixture.hex);Check(encoded.ok()&&encoded.bytes==expected,"exact independent SBDVAL01 frame");auto decoded=dt::DecodeTimeSbdvalComposedNoAllocV3(*profile,true,expected);Check(decoded.ok()&&decoded.value.state==value.state&&decoded.value.nanoseconds_since_midnight==fixture.value,"exact SBDVAL01 decode roster");}
for(const auto& fixture:kExactSbdpv){dt::TimeOwnedValueV3 value{profile,fixture.is_null?dt::TimeValueStateV3::sql_null:dt::TimeValueStateV3::value,fixture.value};auto encoded=dt::EncodeTimeSbdpvComposedV3(value,true);auto expected=HexBytes(fixture.hex);Check(encoded.ok()&&encoded.bytes==expected,"exact independent SBDPV001 frame");auto decoded=dt::DecodeTimeSbdpvComposedNoAllocV3(*profile,true,expected);Check(decoded.ok()&&decoded.value.state==value.state&&decoded.value.nanoseconds_since_midnight==fixture.value,"exact SBDPV001 decode roster");}
std::array<p::u64,4> values{{0,45'296'100'000'000ull,86'399'999'999'999ull,0}};
std::array<dt::TimeValueStateV3,4>states{{dt::TimeValueStateV3::value,dt::TimeValueStateV3::value,dt::TimeValueStateV3::value,dt::TimeValueStateV3::sql_null}};
std::vector<p::byte>image;
std::vector<p::byte>sample_val,sample_pv;
for(size_t i=0;
i<4;
++i){dt::TimeOwnedValueV3 v{profile,states[i],values[i]};
auto component=dt::EncodeCanonicalTimeComponentV3(v);
Check(component.ok()&&component.bytes.size()==(states[i]==dt::TimeValueStateV3::value?8:0),"LE8 component extent");
if(states[i]==dt::TimeValueStateV3::value)Check(p::LoadLittle64(component.bytes.data())==values[i],"LE8 component bytes");
auto cv=dt::DecodeCanonicalTimeComponentNoAllocV3(*profile,states[i],true,component.bytes);
Check(cv.ok()&&cv.value.nanoseconds_since_midnight==values[i],"LE8 component roundtrip");
auto val=dt::EncodeTimeSbdvalComposedV3(v,true),pv=dt::EncodeTimeSbdpvComposedV3(v,true);
Check(val.ok()&&val.bytes.size()==(states[i]==dt::TimeValueStateV3::value?40:32),"SBDVAL extent");
Check(pv.ok()&&pv.bytes.size()==(states[i]==dt::TimeValueStateV3::value?32:24),"SBDPV extent");
auto vd=dt::DecodeTimeSbdvalComposedNoAllocV3(*profile,true,val.bytes),pd=dt::DecodeTimeSbdpvComposedNoAllocV3(*profile,true,pv.bytes);
Check(vd.ok()&&pd.ok()&&vd.value.state==states[i]&&pd.value.state==states[i]&&vd.value.nanoseconds_since_midnight==values[i]&&pd.value.nanoseconds_since_midnight==values[i],"composed roundtrip");
Append(&image,val.bytes);
Append(&image,pv.bytes);
if(i==1){sample_val=val.bytes;
sample_pv=pv.bytes;
}}
auto restored=Reopen(fs::temp_directory_path()/"sb-base-time-component-v3.test",image);
Check(restored==image,"FileDevice byte-identical reopen");
size_t offset=0,records=0;
while(offset<restored.size()){Check(restored.size()-offset>=4,"restored prefix");
auto n=p::LoadLittle32(restored.data()+offset);
offset+=4;
Check(n<=restored.size()-offset,"restored extent");
auto record=std::span<const p::byte>(restored.data()+offset,n);
dt::TimeViewResultV3 result;
if(n>=8&&std::memcmp(record.data(),"SBDVAL01",8)==0)result=dt::DecodeTimeSbdvalComposedNoAllocV3(*profile,true,record);
else result=dt::DecodeTimeSbdpvComposedNoAllocV3(*profile,true,record);
Check(result.ok(),"restored record decode");
offset+=n;
++records;
}Check(offset==restored.size()&&records==8,"restored exact roster");
Mutations(profile,sample_val,sample_pv);
std::cout<<"PASS base.time V3 component persistence checks="<<checks<<" records=8 sbdval_mutations="<<exact_sbdval_mutations<<" sbdpv_mutations="<<exact_sbdpv_mutations<<" not_page_mga\n";
}
