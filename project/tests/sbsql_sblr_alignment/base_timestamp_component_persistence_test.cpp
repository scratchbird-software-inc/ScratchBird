// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../../src/core/datatypes/datatype_timestamp.hpp"
#include "../../../src/core/time/time.hpp"
#include "disk_device.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace dt = scratchbird::core::datatypes;
namespace p = scratchbird::core::platform;
namespace disk = scratchbird::storage::disk;
namespace fs = std::filesystem;
namespace { unsigned file_device_clock_calls=0; }
extern "C" scratchbird::core::time::ClockSnapshotResult __real__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv();
extern "C" scratchbird::core::time::ClockSnapshotResult __wrap__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv(){++file_device_clock_calls;return __real__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv();}
namespace {
unsigned checks=0;
void Check(bool value,std::string_view message){++checks;if(!value){std::cerr<<"FAIL "<<message<<'\n';std::exit(1);}}
bool AlwaysCancel(void*) noexcept{return true;}
p::Uuid D709(){return p::Uuid{{1,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,9}};}
std::shared_ptr<const dt::TimestampValidatedProfileHandleV3> Profile(){auto r=dt::BuildCurrentTimestampValidatedProfileHandleV3(D709());Check(r.ok(),"profile");return std::make_shared<const dt::TimestampValidatedProfileHandleV3>(std::move(r.profile));}
std::vector<p::byte> Hex(std::string_view text){std::vector<p::byte> out(text.size()/2);auto n=[](char c){return static_cast<unsigned>(c<='9'?c-'0':(c|32)-'a'+10);};Check((text.size()&1u)==0,"hex width");for(std::size_t i=0;i<out.size();++i)out[i]=static_cast<p::byte>((n(text[2*i])<<4)|n(text[2*i+1]));return out;}

struct Fixture { dt::TimestampValueStateV3 state; std::int32_t day; p::u64 nanos; std::string_view component; };
constexpr std::array<Fixture,7> kFixtures{{
 {dt::TimestampValueStateV3::sql_null,0,0,""},
 {dt::TimestampValueStateV3::value,INT32_MIN,0,"000000004057ffff0000000000000000"},
 {dt::TimestampValueStateV3::value,INT32_MIN,86'399'999'999'999ull,"7f5101004057ffffffc99a3b00000000"},
 {dt::TimestampValueStateV3::value,-1,86'399'999'999'999ull,"ffffffffffffffffffc99a3b00000000"},
 {dt::TimestampValueStateV3::value,0,0,"00000000000000000000000000000000"},
 {dt::TimestampValueStateV3::value,0,1,"00000000000000000100000000000000"},
 {dt::TimestampValueStateV3::value,INT32_MAX,86'399'999'999'999ull,"ffffffffbfa80000ffc99a3b00000000"},
}};

std::vector<p::byte> Reopen(const fs::path& path,std::span<const p::byte> image){file_device_clock_calls=0;std::error_code error;fs::remove(path,error);disk::FileDevice writer;Check(writer.Open(path.string(),disk::FileOpenMode::create_new).ok(),"FileDevice create");auto write=writer.WriteAt(0,image.data(),image.size());Check(write.ok()&&write.bytes_transferred==image.size()&&writer.Sync().ok()&&writer.Close().ok(),"FileDevice write sync close");disk::FileDevice reader;Check(reader.Open(path.string(),disk::FileOpenMode::open_existing_read_only).ok(),"FileDevice independent reopen");std::vector<p::byte> restored(image.size());auto read=reader.ReadAt(0,restored.data(),restored.size());Check(read.ok()&&read.bytes_transferred==restored.size()&&reader.Close().ok(),"FileDevice read close");fs::remove(path,error);Check(file_device_clock_calls==0,"local-civil timestamp persistence has no clock dependency");return restored;}
void Append(std::vector<p::byte>* image,std::span<const p::byte> record){std::array<p::byte,4> extent{};p::StoreLittle32(extent.data(),static_cast<p::u32>(record.size()));image->insert(image->end(),extent.begin(),extent.end());image->insert(image->end(),record.begin(),record.end());}

void ComponentsAndFrames(const std::shared_ptr<const dt::TimestampValidatedProfileHandleV3>& profile){std::vector<p::byte> image;for(const auto& fixture:kFixtures){dt::TimestampOwnedValueV3 value{profile,fixture.state,fixture.day,fixture.nanos};auto component=dt::EncodeCanonicalTimestampComponentV3(value);Check(component.ok()&&component.bytes==Hex(fixture.component),"exact zero-or-LE16 component");auto decoded=dt::DecodeCanonicalTimestampComponentNoAllocV3(*profile,fixture.state,true,component.bytes);Check(decoded.ok()&&decoded.value.civil_day==fixture.day&&decoded.value.nanoseconds_since_midnight==fixture.nanos,"component roundtrip");auto sbdval=dt::EncodeTimestampSbdvalComposedV3(value,true);Check(sbdval.ok()&&sbdval.bytes.size()==(fixture.state==dt::TimestampValueStateV3::sql_null?32:48),"SBDVAL composed extent");auto decoded_sbdval=dt::DecodeTimestampSbdvalComposedNoAllocV3(*profile,true,sbdval.bytes);Check(decoded_sbdval.ok()&&decoded_sbdval.value.state==fixture.state&&decoded_sbdval.value.civil_day==fixture.day&&decoded_sbdval.value.nanoseconds_since_midnight==fixture.nanos,"SBDVAL profile-aware roundtrip");auto sbdpv=dt::EncodeTimestampSbdpvComposedV3(value,true);Check(sbdpv.ok()&&sbdpv.bytes.size()==(fixture.state==dt::TimestampValueStateV3::sql_null?24:40),"SBDPV composed extent");auto decoded_sbdpv=dt::DecodeTimestampSbdpvComposedNoAllocV3(*profile,true,sbdpv.bytes);Check(decoded_sbdpv.ok()&&decoded_sbdpv.value.state==fixture.state&&decoded_sbdpv.value.civil_day==fixture.day&&decoded_sbdpv.value.nanoseconds_since_midnight==fixture.nanos,"SBDPV profile-aware roundtrip");Append(&image,sbdval.bytes);Append(&image,sbdpv.bytes);}
  auto restored=Reopen(fs::temp_directory_path()/"sb-base-timestamp-component-v3.test",image);Check(restored==image,"component frames byte-identical after independent reopen");
}

std::array<p::byte,24> OracleRecord(const dt::TimestampOwnedValueV3& value){std::array<p::byte,24> record{};record[0]=value.state==dt::TimestampValueStateV3::value?1:0;record[1]=value.state==dt::TimestampValueStateV3::value?16:0;auto component=dt::EncodeCanonicalTimestampComponentV3(value);Check(component.ok(),"oracle component encode");if(!component.bytes.empty())std::copy(component.bytes.begin(),component.bytes.end(),record.begin()+8);return record;}
void PersistenceOracle(const std::shared_ptr<const dt::TimestampValidatedProfileHandleV3>& profile){std::array<p::byte,128> header{};std::memcpy(header.data(),"SBTSPOR1",8);p::StoreLittle16(header.data()+8,1);p::StoreLittle16(header.data()+10,128);p::StoreLittle32(header.data()+12,3);std::memcpy(header.data()+16,D709().bytes.data(),16);p::StoreLittle64(header.data()+32,9);p::StoreLittle64(header.data()+40,9);std::memcpy(header.data()+48,profile->identity.legacy_fields.type_uuid.bytes.data(),16);std::memcpy(header.data()+64,profile->identity.legacy_fields.codec_uuid.bytes.data(),16);std::memcpy(header.data()+80,profile->profile_fingerprint.data(),32);
  std::array<dt::TimestampOwnedValueV3,3> values{{{profile,dt::TimestampValueStateV3::sql_null,0,0},{profile,dt::TimestampValueStateV3::value,INT32_MIN,0},{profile,dt::TimestampValueStateV3::value,INT32_MAX,86'399'999'999'999ull}}};std::vector<p::byte> image(header.begin(),header.end());for(const auto& value:values){auto record=OracleRecord(value);image.insert(image.end(),record.begin(),record.end());}Check(image.size()==128+3*24,"SBTSPOR1 exact header and record extents");auto restored=Reopen(fs::temp_directory_path()/"sb-base-timestamp-sbtspor1.test",image);Check(restored==image,"SBTSPOR1 byte-identical close/reopen");for(std::size_t i=0;i<values.size();++i){auto record=std::span<const p::byte>(restored.data()+128+i*24,24);auto state=record[0]==0?dt::TimestampValueStateV3::sql_null:dt::TimestampValueStateV3::value;auto component=record.subspan(8,record[1]);auto decoded=dt::DecodeCanonicalTimestampComponentNoAllocV3(*profile,state,true,component);Check(decoded.ok()&&decoded.value.civil_day==values[i].civil_day&&decoded.value.nanoseconds_since_midnight==values[i].nanoseconds_since_midnight,"SBTSPOR1 restored record decode");std::array<p::byte,24> expected{};std::copy(record.begin(),record.end(),expected.begin());Check(OracleRecord({profile,state,decoded.value.civil_day,decoded.value.nanoseconds_since_midnight})==expected,"SBTSPOR1 byte-identical record reencode");}}

void Mutations(const std::shared_ptr<const dt::TimestampValidatedProfileHandleV3>& profile){dt::TimestampOwnedValueV3 value{profile,dt::TimestampValueStateV3::value,-1,86'399'999'999'999ull};auto val=dt::EncodeTimestampSbdvalComposedV3(value,true);auto pv=dt::EncodeTimestampSbdpvComposedV3(value,true);Check(val.ok()&&pv.ok(),"mutation baselines");for(auto* encoded:{&val.bytes,&pv.bytes}){auto short_value=*encoded;short_value.pop_back();if(encoded==&val.bytes)Check(!dt::DecodeTimestampSbdvalComposedNoAllocV3(*profile,true,short_value).ok(),"short SBDVAL refuses");else Check(!dt::DecodeTimestampSbdpvComposedNoAllocV3(*profile,true,short_value).ok(),"short SBDPV refuses");}
  dt::TimestampExecutionControlV3 zero_budget;zero_budget.maximum_allocation_bytes=0;
  auto val_budget=dt::DecodeTimestampSbdvalComposedNoAllocV3(
      *profile,true,val.bytes,zero_budget);
  Check(!val_budget.ok()&&val_budget.diagnostic.diagnostic_code==
            "RESOURCE.BUDGET_EXCEEDED"&&
            val_budget.diagnostic.detail=="sbdval_decode_budget",
        "SBDVAL parent resource gate follows nested canonical validation");
  auto pv_budget=dt::DecodeTimestampSbdpvComposedNoAllocV3(
      *profile,true,pv.bytes,zero_budget);
  Check(!pv_budget.ok()&&pv_budget.diagnostic.diagnostic_code==
            "RESOURCE.BUDGET_EXCEEDED"&&
            pv_budget.diagnostic.detail=="sbdpv_decode_budget",
        "SBDPV parent resource gate follows nested canonical validation");
  dt::TimestampExecutionControlV3 reencode_before_cancel;reencode_before_cancel.force_reencode_mismatch_for_conformance=true;reencode_before_cancel.cancelled=AlwaysCancel;auto val_precedence=dt::DecodeTimestampSbdvalComposedNoAllocV3(*profile,true,val.bytes,reencode_before_cancel);Check(!val_precedence.ok()&&val_precedence.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","SBDVAL outer reencode validation precedes final cancellation");auto pv_precedence=dt::DecodeTimestampSbdpvComposedNoAllocV3(*profile,true,pv.bytes,reencode_before_cancel);Check(!pv_precedence.ok()&&pv_precedence.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","SBDPV outer reencode validation precedes final cancellation");
  std::array<p::byte,16> dirty{};dirty[0]=1;Check(!dt::DecodeCanonicalTimestampComponentNoAllocV3(*profile,dt::TimestampValueStateV3::sql_null,true,dirty).ok(),"dirty NULL refuses");dirty.fill(0);p::StoreLittle32(dirty.data()+8,1'000'000'000u);Check(!dt::DecodeCanonicalTimestampComponentNoAllocV3(*profile,dt::TimestampValueStateV3::value,true,dirty).ok(),"bad durable nanoseconds refuses");dirty.fill(0);dirty[12]=1;Check(!dt::DecodeCanonicalTimestampComponentNoAllocV3(*profile,dt::TimestampValueStateV3::value,true,dirty).ok(),"bad reserved bytes refuse");
  dirty.fill(0);p::StoreLittle64(dirty.data(),static_cast<p::u64>(INT64_MAX));
  auto day_range=dt::DecodeCanonicalTimestampComponentNoAllocV3(
      *profile,dt::TimestampValueStateV3::value,true,dirty);
  Check(!day_range.ok()&&day_range.diagnostic.diagnostic_code==
            "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "out-of-domain durable component is canonical corruption");
  auto bad_profile=*profile;bad_profile.profile_fingerprint[0]^=1;
  auto combined=dt::DecodeCanonicalTimestampComponentNoAllocV3(
      bad_profile,static_cast<dt::TimestampValueStateV3>(255),true,dirty);
  Check(!combined.ok()&&combined.diagnostic.diagnostic_code==
            "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "component state shape precedes invalid caller profile");
  combined=dt::DecodeCanonicalTimestampComponentNoAllocV3(
      bad_profile,dt::TimestampValueStateV3::value,true,
      std::span<const p::byte>(dirty.data(),15));
  Check(!combined.ok()&&combined.diagnostic.diagnostic_code==
            "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "component exact extent precedes invalid caller profile");
  dirty.fill(0);
  combined=dt::DecodeCanonicalTimestampComponentNoAllocV3(
      bad_profile,dt::TimestampValueStateV3::value,true,dirty);
  Check(!combined.ok()&&combined.diagnostic.diagnostic_code==
            "CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "component validates caller profile after exact state and extent");
  auto historical=*profile;historical.identity.legacy_fields.codec_uuid=p::Uuid{{1,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x21}};Check(!dt::ValidateTimestampProfileHandleV3(historical).ok(),"historical d821 UTC codec refuses current local-civil decode");
}
}  // namespace
int main(){auto profile=Profile();ComponentsAndFrames(profile);PersistenceOracle(profile);Mutations(profile);std::cout<<"PASS base.timestamp V3 component/persistence checks="<<checks<<"\n";}
