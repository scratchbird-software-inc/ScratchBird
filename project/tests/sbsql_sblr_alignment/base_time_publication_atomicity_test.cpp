// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../../src/core/datatypes/datatype_time.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <new>
#include <numeric>
#include <string_view>
#include <thread>
#include <vector>
namespace allocation_probe{bool count=false;
bool fail=false;std::size_t fail_at=0;std::size_t fail_calls=0;
std::size_t allocations=0;
}
void*operator new(std::size_t n){if(allocation_probe::count)++allocation_probe::allocations;
if(allocation_probe::fail&&++allocation_probe::fail_calls==allocation_probe::fail_at)throw std::bad_alloc();
if(void*p=std::malloc(n?n:1))return p;
throw std::bad_alloc();
}
void*operator new[](std::size_t n){return::operator new(n);
}void operator delete(void*p)noexcept{std::free(p);
}void operator delete[](void*p)noexcept{std::free(p);
}void operator delete(void*p,std::size_t)noexcept{std::free(p);
}void operator delete[](void*p,std::size_t)noexcept{std::free(p);
}
namespace dt=scratchbird::core::datatypes;
namespace p=scratchbird::core::platform;
namespace{
unsigned checks=0;
[[noreturn]]void Fail(std::string_view m){std::cerr<<"FAIL "<<m<<'\n';
std::exit(1);
}void Check(bool v,std::string_view m){++checks;
if(!v)Fail(m);
}p::Uuid D708(){return{{1,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,8}};
}std::shared_ptr<const dt::TimeValidatedProfileHandleV3>Profile(){auto r=dt::BuildCurrentTimeValidatedProfileHandleV3(D708());
Check(r.ok(),"profile");
return std::make_shared<const dt::TimeValidatedProfileHandleV3>(r.profile);
}
const dt::DatatypeTypeCodecIdentityRowV3* Identity(dt::CanonicalTypeId type){for(const auto& row:dt::CurrentDatatypeTypeCodecIdentityRowsV3())if(row.legacy_fields.catalog_snapshot_uuid==D708()&&row.legacy_fields.catalog_generation==8&&row.legacy_fields.registry_generation==8&&row.legacy_fields.canonical_binary_type_code==static_cast<p::u32>(type))return &row;return nullptr;}
scratchbird::engine::ExecutionTypeDescriptor Descriptor(dt::CanonicalTypeId type){auto manifest=dt::LoadCurrentCoreDatatypeCatalogManifest();Check(manifest.ok(),"catalog");auto row=dt::LookupDatatypeCatalogRow(manifest.manifest,type);Check(row.ok()&&row.manifest.descriptor_rows.size()==1,"descriptor row");dt::CatalogExecutionTypeMetadata metadata;metadata.descriptor_uuid=row.manifest.descriptor_rows.front().descriptor_uuid;metadata.descriptor_epoch=row.manifest.descriptor_rows.front().descriptor_epoch;auto result=dt::LookupExecutionTypeDescriptorFromCatalog(type,metadata);Check(result.ok(),"execution descriptor");return result.descriptor;}
void BeginHeapFailure(std::size_t at=1){allocation_probe::fail_calls=0;allocation_probe::fail_at=at;allocation_probe::fail=true;}
void EndHeapFailure(){allocation_probe::fail=false;}
struct Cancel{unsigned calls=0,at=0;
};
bool Stop(void*raw)noexcept{auto&c=*static_cast<Cancel*>(raw);
return++c.calls==c.at;
}dt::TimeExecutionControlV3 Control(Cancel&c){return{~p::u64{0},Stop,&c};
}
struct ScrubObservation{std::array<unsigned,static_cast<std::size_t>(dt::TimeScrubClassV3::count)> calls{};std::array<p::u64,static_cast<std::size_t>(dt::TimeScrubClassV3::count)> maximum_extent{};bool all_zero=true;unsigned total()const{return std::accumulate(calls.begin(),calls.end(),0u);}};
void ObserveScrub(void*raw,dt::TimeScrubClassV3 scrub_class,const p::byte*bytes,p::u64 extent)noexcept{auto& observation=*static_cast<ScrubObservation*>(raw);const auto index=static_cast<std::size_t>(scrub_class);++observation.calls[index];observation.maximum_extent[index]=std::max(observation.maximum_extent[index],extent);for(p::u64 i=0;i<extent;++i)observation.all_zero=observation.all_zero&&bytes[i]==0;}
void NoAlloc(const std::shared_ptr<const dt::TimeValidatedProfileHandleV3>&profile){dt::TimeOwnedValueV3 v{profile,dt::TimeValueStateV3::value,45'296'100'000'000ull};
std::array<p::byte,8>component{};
std::array<p::byte,32>hash{};
std::array<p::byte,108>key_bytes{};
std::array<char,25>text{};
auto key=dt::MakeTimeSortKeyV3(v,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first);
auto val=dt::EncodeTimeSbdvalComposedV3(v,true);
auto pv=dt::EncodeTimeSbdpvComposedV3(v,true);
allocation_probe::allocations=0;
allocation_probe::count=true;
auto c=dt::EncodeCanonicalTimeComponentIntoNoAllocV3(v,component.data(),component.size());
auto r=dt::RenderCanonicalTimeIntoNoAllocV3(v,false,text.data(),text.size());
auto h=dt::HashTimeValueIntoNoAllocV3(v,hash.data(),hash.size());
auto k=dt::MakeTimeSortKeyIntoNoAllocV3(v,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first,key_bytes.data(),key_bytes.size());
auto cd=dt::DecodeCanonicalTimeComponentNoAllocV3(*profile,dt::TimeValueStateV3::value,true,component);
auto kd=dt::DecodeTimeSortKeyNoAllocV3(*profile,key.bytes);
auto vd=dt::DecodeTimeSbdvalComposedNoAllocV3(*profile,true,val.bytes);
auto pd=dt::DecodeTimeSbdpvComposedNoAllocV3(*profile,true,pv.bytes);
auto profile_check=dt::ValidateTimeProfileHandleV3(*profile);
allocation_probe::count=false;
Check(c.ok()&&r.ok()&&h.ok()&&k.ok()&&cd.ok()&&kd.ok()&&vd.ok()&&pd.ok()&&profile_check.ok()&&allocation_probe::allocations==0,"bounded APIs and profile SHA allocate no C++ heap");
dt::TimeOwnedValueV3 nullv{profile,dt::TimeValueStateV3::sql_null,0};Cancel dc1{0,1},dc2{0,1},cc1{0,1},cc2{0,1};
Check(dt::DecodeCanonicalTimeComponentNoAllocV3(*profile,dt::TimeValueStateV3::value,true,component,Control(dc1)).diagnostic.diagnostic_code=="PROCESS.CANCELLED","raw PRESENT decode cancellation");
Check(dt::DecodeCanonicalTimeComponentNoAllocV3(*profile,dt::TimeValueStateV3::sql_null,true,{},Control(dc2)).diagnostic.diagnostic_code=="PROCESS.CANCELLED","raw NULL decode cancellation");
Check(dt::CompareTimeValuesV3(v.view(),v.view(),Control(cc1)).diagnostic.diagnostic_code=="PROCESS.CANCELLED","PRESENT comparison cancellation");
Check(dt::CompareTimeValuesV3(nullv.view(),v.view(),Control(cc2)).diagnostic.diagnostic_code=="PROCESS.CANCELLED","NULL comparison cancellation");
BeginHeapFailure();auto denied_profile=dt::ValidateTimeProfileHandleV3(*profile);auto denied_raw=dt::DecodeCanonicalTimeComponentNoAllocV3(*profile,dt::TimeValueStateV3::value,true,component);auto denied_compare=dt::CompareTimeValuesV3(v.view(),v.view());auto denied_write=dt::HashTimeValueIntoNoAllocV3(v,hash.data(),hash.size());EndHeapFailure();
Check(denied_profile.ok()&&denied_raw.ok()&&denied_compare.ok()&&denied_write.ok()&&allocation_probe::fail_calls==0,"bounded paths survive active heap denial without allocation");
}
template<class Invoke>void ExactGuardMatrix(std::string_view label,std::span<const p::byte> expected,Invoke invoke){
 const std::size_t required=expected.size();auto fresh=[&]{return std::vector<p::byte>(3+required+5,0xa5);};
 auto success=fresh();auto result=invoke(success.data()+3,required,dt::TimeExecutionControlV3{});Check(result.ok()&&result.bytes_written==required,label);Check(std::equal(expected.begin(),expected.end(),success.begin()+3),"exact bounded bytes");for(std::size_t i=0;i<3;++i)Check(success[i]==0xa5,"three prefix guards");for(std::size_t i=3+required;i<success.size();++i)Check(success[i]==0xa5,"five suffix guards");
 auto short_buffer=fresh();const auto short_before=short_buffer;Check(!invoke(short_buffer.data()+3,required-1,dt::TimeExecutionControlV3{}).ok()&&short_buffer==short_before,"short capacity preserves all guards and payload");
 auto cancelled=fresh();const auto cancelled_before=cancelled;Cancel cancel{0,1};Check(!invoke(cancelled.data()+3,required,Control(cancel)).ok()&&cancelled==cancelled_before,"cancellation preserves all guards and payload");
 auto resource=fresh();const auto resource_before=resource;dt::TimeExecutionControlV3 limited{};limited.maximum_allocation_bytes=required-1;Check(!invoke(resource.data()+3,required,limited).ok()&&resource==resource_before,"resource refusal preserves all guards and payload");
}
void FixedOutputGuardContract(const std::shared_ptr<const dt::TimeValidatedProfileHandleV3>&profile){
 dt::TimeOwnedValueV3 present{profile,dt::TimeValueStateV3::value,45'296'100'000'000ull},nullv{profile,dt::TimeValueStateV3::sql_null,0};
 const auto component=dt::EncodeCanonicalTimeComponentV3(present);const auto rendered=dt::RenderCanonicalTimeV3(present);const auto hash=dt::HashTimeValueV3(present);const auto present_key=dt::MakeTimeSortKeyV3(present,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first);const auto null_key=dt::MakeTimeSortKeyV3(nullv,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first);Check(component.ok()&&rendered.ok()&&hash.ok()&&present_key.ok()&&null_key.ok(),"guard oracle construction");
 std::vector<p::byte> render_bytes(rendered.text.begin(),rendered.text.end());
 ExactGuardMatrix("component exact 3/5 guards",component.bytes,[&](p::byte*out,p::u64 cap,const dt::TimeExecutionControlV3&control){return dt::EncodeCanonicalTimeComponentIntoNoAllocV3(present,out,cap,control);});
 ExactGuardMatrix("render exact 3/5 guards",render_bytes,[&](p::byte*out,p::u64 cap,const dt::TimeExecutionControlV3&control){return dt::RenderCanonicalTimeIntoNoAllocV3(present,false,reinterpret_cast<char*>(out),cap,control);});
 ExactGuardMatrix("hash exact 3/5 guards",hash.bytes,[&](p::byte*out,p::u64 cap,const dt::TimeExecutionControlV3&control){return dt::HashTimeValueIntoNoAllocV3(present,out,cap,control);});
 ExactGuardMatrix("PRESENT key exact 3/5 guards",present_key.bytes,[&](p::byte*out,p::u64 cap,const dt::TimeExecutionControlV3&control){return dt::MakeTimeSortKeyIntoNoAllocV3(present,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first,out,cap,control);});
 ExactGuardMatrix("NULL key exact 3/5 guards",null_key.bytes,[&](p::byte*out,p::u64 cap,const dt::TimeExecutionControlV3&control){return dt::MakeTimeSortKeyIntoNoAllocV3(nullv,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first,out,cap,control);});
 auto* mutable_profile=const_cast<dt::TimeValidatedProfileHandleV3*>(profile.get());const auto material_before=mutable_profile->profile_material;
 Check(!dt::EncodeCanonicalTimeComponentIntoNoAllocV3(present,mutable_profile->profile_material.data()+3,component.bytes.size()).ok()&&mutable_profile->profile_material==material_before,"component overlap preserves exact profile and guards");
 Check(!dt::RenderCanonicalTimeIntoNoAllocV3(present,false,reinterpret_cast<char*>(mutable_profile->profile_material.data()+3),render_bytes.size()).ok()&&mutable_profile->profile_material==material_before,"render overlap preserves exact profile and guards");
 Check(!dt::HashTimeValueIntoNoAllocV3(present,mutable_profile->profile_material.data()+3,hash.bytes.size()).ok()&&mutable_profile->profile_material==material_before,"hash overlap preserves exact profile and guards");
 Check(!dt::MakeTimeSortKeyIntoNoAllocV3(present,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first,mutable_profile->profile_material.data()+3,present_key.bytes.size()).ok()&&mutable_profile->profile_material==material_before,"PRESENT key overlap preserves exact profile and guards");
 Check(!dt::MakeTimeSortKeyIntoNoAllocV3(nullv,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first,mutable_profile->profile_material.data()+3,null_key.bytes.size()).ok()&&mutable_profile->profile_material==material_before,"NULL key overlap preserves exact profile and guards");Check(dt::ValidateTimeProfileHandleV3(*profile).ok(),"overlap guard attempts retain profile authority");
}
void ScrubClassCoverage(const std::shared_ptr<const dt::TimeValidatedProfileHandleV3>&profile){
 ScrubObservation observation;dt::TimeExecutionControlV3 control{};control.observe_scrubbed=ObserveScrub;control.scrub_observer_context=&observation;dt::TimeOwnedValueV3 value{profile,dt::TimeValueStateV3::value,45'296'100'000'000ull};
 Check(dt::ValidateTimeProfileHandleV3(*profile,control).ok(),"profile staging scrub success");
 std::array<p::byte,8> component{};Check(dt::EncodeCanonicalTimeComponentIntoNoAllocV3(value,component.data(),component.size(),control).ok(),"component staging scrub success");Check(dt::DecodeCanonicalTimeComponentNoAllocV3(*profile,dt::TimeValueStateV3::value,true,component,control).ok(),"component reencode scrub success");
 std::array<char,18> text{};Check(dt::RenderCanonicalTimeIntoNoAllocV3(value,false,text.data(),text.size(),control).ok()&&dt::RenderCanonicalTimeV3(value,false,control).ok(),"render staging scrub success");
 std::array<p::byte,32> hash{};Check(dt::HashTimeValueIntoNoAllocV3(value,hash.data(),hash.size(),control).ok()&&dt::HashTimeValueV3(value,control).ok(),"hash staging scrub success");
 std::array<p::byte,108> key{};Check(dt::MakeTimeSortKeyIntoNoAllocV3(value,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first,key.data(),key.size(),control).ok()&&dt::MakeTimeSortKeyV3(value,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first,control).ok()&&dt::DecodeTimeSortKeyNoAllocV3(*profile,key,control).ok(),"ordered key staging scrub success");
 auto* character_identity=Identity(dt::CanonicalTypeId::character);auto character_descriptor=Descriptor(dt::CanonicalTypeId::character);dt::TimeCastRequestV3 cast;cast.one_based_policy_row=50;cast.time_source=&value;cast.scalar_target=dt::CanonicalTypeId::character;cast.scalar_target_descriptor=character_descriptor;cast.scalar_target_identity=character_identity;cast.context=dt::DatatypeCastContext::explicit_cast;cast.control=control;Check(dt::CastTimeValueV3(cast).ok(),"character render staging scrub success");
 std::array<p::u64,2> input{{0,1}},output{};std::array<p::byte,1> bitmap{{0}},output_bitmap{};Check(dt::MaterializeTimeBatchIntoV3(profile,input,bitmap,output.data(),sizeof(output),output_bitmap.data(),output_bitmap.size(),control).ok(),"batch staging scrub success");
 const auto val=dt::EncodeTimeSbdvalComposedV3(value,true,control),pv=dt::EncodeTimeSbdpvComposedV3(value,true,control);Check(val.ok()&&pv.ok()&&dt::DecodeTimeSbdvalComposedNoAllocV3(*profile,true,val.bytes,control).ok()&&dt::DecodeTimeSbdpvComposedNoAllocV3(*profile,true,pv.bytes,control).ok(),"composed staging scrub success");
 Cancel component_final{0,2};auto component_control=Control(component_final);component_control.observe_scrubbed=ObserveScrub;component_control.scrub_observer_context=&observation;Check(!dt::EncodeCanonicalTimeComponentV3(value,component_control).ok(),"owned component final cancellation scrub");
 Check(observation.all_zero,"every observed staging extent is zero after clear");
 constexpr std::array core_classes{dt::TimeScrubClassV3::sha_state,dt::TimeScrubClassV3::sha_schedule,dt::TimeScrubClassV3::sha_tail,dt::TimeScrubClassV3::profile_material,dt::TimeScrubClassV3::comparison_material,dt::TimeScrubClassV3::profile_digest,dt::TimeScrubClassV3::comparison_digest,dt::TimeScrubClassV3::component_decode_reencode,dt::TimeScrubClassV3::component_encode_staging,dt::TimeScrubClassV3::component_owned_buffer,dt::TimeScrubClassV3::render_staging,dt::TimeScrubClassV3::render_owned_buffer,dt::TimeScrubClassV3::hash_preimage,dt::TimeScrubClassV3::hash_digest,dt::TimeScrubClassV3::hash_owned_buffer,dt::TimeScrubClassV3::ordered_key_staging,dt::TimeScrubClassV3::ordered_key_owned_buffer,dt::TimeScrubClassV3::ordered_key_decode_reencode,dt::TimeScrubClassV3::character_render_staging,dt::TimeScrubClassV3::batch_values_staging,dt::TimeScrubClassV3::batch_bitmap_staging,dt::TimeScrubClassV3::sbdval_reencode,dt::TimeScrubClassV3::sbdval_component_staging,dt::TimeScrubClassV3::sbdpv_reencode,dt::TimeScrubClassV3::sbdpv_component_staging};for(const auto scrub_class:core_classes)Check(observation.calls[static_cast<std::size_t>(scrub_class)]!=0,"every time core scrub class observed");
 auto extent=[&](dt::TimeScrubClassV3 c){return observation.maximum_extent[static_cast<std::size_t>(c)];};
 Check(extent(dt::TimeScrubClassV3::profile_material)==592&&extent(dt::TimeScrubClassV3::comparison_material)==352&&extent(dt::TimeScrubClassV3::profile_digest)==32&&extent(dt::TimeScrubClassV3::comparison_digest)==32,"exact profile staging extents");
 Check(extent(dt::TimeScrubClassV3::component_decode_reencode)==8&&extent(dt::TimeScrubClassV3::component_encode_staging)==8&&extent(dt::TimeScrubClassV3::component_owned_buffer)==8&&extent(dt::TimeScrubClassV3::render_staging)==25&&extent(dt::TimeScrubClassV3::render_owned_buffer)==25,"exact component/render staging extents");
 Check(extent(dt::TimeScrubClassV3::hash_preimage)==109&&extent(dt::TimeScrubClassV3::hash_digest)==32&&extent(dt::TimeScrubClassV3::hash_owned_buffer)==32,"exact hash staging extents");
 Check(extent(dt::TimeScrubClassV3::ordered_key_staging)==108&&extent(dt::TimeScrubClassV3::ordered_key_owned_buffer)==108&&extent(dt::TimeScrubClassV3::ordered_key_decode_reencode)==108,"exact ordered-key staging extents");
 Check(extent(dt::TimeScrubClassV3::character_render_staging)==18&&extent(dt::TimeScrubClassV3::batch_values_staging)==16&&extent(dt::TimeScrubClassV3::batch_bitmap_staging)==1,"exact cast/batch staging extents");
 Check(extent(dt::TimeScrubClassV3::sbdval_reencode)==40&&extent(dt::TimeScrubClassV3::sbdval_component_staging)==8&&extent(dt::TimeScrubClassV3::sbdpv_reencode)==32&&extent(dt::TimeScrubClassV3::sbdpv_component_staging)==8,"exact composed staging extents");
}
bool Seen(const ScrubObservation& observation,dt::TimeScrubClassV3 scrub_class){return observation.calls[static_cast<std::size_t>(scrub_class)]!=0;}
dt::TimeExecutionControlV3 Observed(ScrubObservation& observation){dt::TimeExecutionControlV3 control{};control.observe_scrubbed=ObserveScrub;control.scrub_observer_context=&observation;return control;}
void ScrubFailureCoverage(const std::shared_ptr<const dt::TimeValidatedProfileHandleV3>&profile){
 dt::TimeOwnedValueV3 value{profile,dt::TimeValueStateV3::value,45'296'100'000'000ull};std::array<p::byte,8> component{};p::StoreLittle64(component.data(),value.nanoseconds_since_midnight);std::array<p::byte,108> output{};
 ScrubObservation refused;auto refusal=Observed(refused);auto wrong=*profile;wrong.profile_material[0]^=1;Check(!dt::ValidateTimeProfileHandleV3(wrong,refusal).ok(),"profile refusal scrub");Check(!dt::EncodeCanonicalTimeComponentIntoNoAllocV3(value,output.data(),7,refusal).ok(),"component refusal scrub");Check(!dt::RenderCanonicalTimeIntoNoAllocV3(value,false,reinterpret_cast<char*>(output.data()),9,refusal).ok(),"render refusal scrub");Check(!dt::HashTimeValueIntoNoAllocV3(value,output.data(),31,refusal).ok(),"hash refusal scrub");Check(!dt::MakeTimeSortKeyIntoNoAllocV3(value,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first,output.data(),107,refusal).ok(),"key refusal scrub");auto key=dt::MakeTimeSortKeyV3(value,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first);refusal.force_reencode_mismatch_for_conformance=true;Check(!dt::DecodeTimeSortKeyNoAllocV3(*profile,key.bytes,refusal).ok(),"key decode refusal scrub");auto val=dt::EncodeTimeSbdvalComposedV3(value,true),pv=dt::EncodeTimeSbdpvComposedV3(value,true);Check(!dt::DecodeTimeSbdvalComposedNoAllocV3(*profile,true,val.bytes,refusal).ok()&&!dt::DecodeTimeSbdpvComposedNoAllocV3(*profile,true,pv.bytes,refusal).ok(),"composed refusal scrub");
 auto* character_identity=Identity(dt::CanonicalTypeId::character);auto character_descriptor=Descriptor(dt::CanonicalTypeId::character);dt::TimeOwnedValueV3 cast_value{profile,dt::TimeValueStateV3::value,dt::kTimeMaximumNanosecondsV3};std::array<char,1> cast_buffer{};dt::TimeCastRequestV3 cast;cast.one_based_policy_row=50;cast.time_source=&cast_value;cast.scalar_target=dt::CanonicalTypeId::character;cast.scalar_target_descriptor=character_descriptor;cast.scalar_target_identity=character_identity;cast.context=dt::DatatypeCastContext::explicit_cast;cast.use_character_output_buffer=true;cast.character_output=cast_buffer.data();cast.character_output_capacity=0;cast.control=refusal;Check(!dt::CastTimeValueV3(cast).ok(),"cast refusal scrub");
 Check(refused.all_zero&&Seen(refused,dt::TimeScrubClassV3::profile_material)&&Seen(refused,dt::TimeScrubClassV3::comparison_material)&&Seen(refused,dt::TimeScrubClassV3::component_encode_staging)&&Seen(refused,dt::TimeScrubClassV3::render_staging)&&Seen(refused,dt::TimeScrubClassV3::hash_preimage)&&Seen(refused,dt::TimeScrubClassV3::hash_digest)&&Seen(refused,dt::TimeScrubClassV3::ordered_key_staging)&&Seen(refused,dt::TimeScrubClassV3::ordered_key_decode_reencode)&&Seen(refused,dt::TimeScrubClassV3::character_render_staging)&&Seen(refused,dt::TimeScrubClassV3::sbdval_reencode)&&Seen(refused,dt::TimeScrubClassV3::sbdpv_reencode),"identified refusal cleanup classes");
 ScrubObservation cancelled;Cancel stop{0,99};auto cancellation=Control(stop);cancellation.observe_scrubbed=ObserveScrub;cancellation.scrub_observer_context=&cancelled;Check(dt::ValidateTimeProfileHandleV3(*profile,cancellation).ok(),"profile high checkpoint control");stop={0,1};Check(!dt::ValidateTimeProfileHandleV3(*profile,cancellation).ok(),"profile cancellation scrub");stop={0,1};Check(!dt::DecodeCanonicalTimeComponentNoAllocV3(*profile,dt::TimeValueStateV3::value,true,component,cancellation).ok(),"decode cancellation scrub");stop={0,1};Check(!dt::EncodeCanonicalTimeComponentIntoNoAllocV3(value,output.data(),8,cancellation).ok(),"component cancellation scrub");stop={0,1};Check(!dt::RenderCanonicalTimeIntoNoAllocV3(value,false,reinterpret_cast<char*>(output.data()),18,cancellation).ok(),"render cancellation scrub");stop={0,1};Check(!dt::HashTimeValueIntoNoAllocV3(value,output.data(),32,cancellation).ok(),"hash cancellation scrub");stop={0,1};Check(!dt::MakeTimeSortKeyIntoNoAllocV3(value,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first,output.data(),108,cancellation).ok(),"key cancellation scrub");stop={0,1};Check(!dt::DecodeTimeSortKeyNoAllocV3(*profile,key.bytes,cancellation).ok(),"key decode cancellation scrub");cast.use_character_output_buffer=false;cast.control=cancellation;stop={0,1};Check(!dt::CastTimeValueV3(cast).ok(),"cast cancellation scrub");std::array<p::u64,2> input{{0,1}},batch_output{};std::array<p::byte,1> bitmap{{0}},batch_bitmap{};stop={0,2};Check(!dt::MaterializeTimeBatchIntoV3(profile,input,bitmap,batch_output.data(),sizeof(batch_output),batch_bitmap.data(),batch_bitmap.size(),cancellation).ok(),"batch cancellation scrub");stop={0,1};Check(!dt::DecodeTimeSbdvalComposedNoAllocV3(*profile,true,val.bytes,cancellation).ok(),"SBDVAL cancellation scrub");stop={0,1};Check(!dt::DecodeTimeSbdpvComposedNoAllocV3(*profile,true,pv.bytes,cancellation).ok(),"SBDPV cancellation scrub");
 Check(cancelled.all_zero&&Seen(cancelled,dt::TimeScrubClassV3::profile_digest)&&Seen(cancelled,dt::TimeScrubClassV3::component_decode_reencode)&&Seen(cancelled,dt::TimeScrubClassV3::component_encode_staging)&&Seen(cancelled,dt::TimeScrubClassV3::render_staging)&&Seen(cancelled,dt::TimeScrubClassV3::hash_preimage)&&Seen(cancelled,dt::TimeScrubClassV3::hash_digest)&&Seen(cancelled,dt::TimeScrubClassV3::ordered_key_staging)&&Seen(cancelled,dt::TimeScrubClassV3::ordered_key_decode_reencode)&&Seen(cancelled,dt::TimeScrubClassV3::character_render_staging)&&Seen(cancelled,dt::TimeScrubClassV3::batch_values_staging)&&Seen(cancelled,dt::TimeScrubClassV3::batch_bitmap_staging)&&Seen(cancelled,dt::TimeScrubClassV3::sbdval_reencode)&&Seen(cancelled,dt::TimeScrubClassV3::sbdpv_reencode),"identified cancellation cleanup classes");
 ScrubObservation allocation;auto allocation_control=Observed(allocation);BeginHeapFailure();auto render=dt::RenderCanonicalTimeV3(cast_value,false,allocation_control);EndHeapFailure();BeginHeapFailure();auto hash=dt::HashTimeValueV3(value,allocation_control);EndHeapFailure();BeginHeapFailure();auto owned_key=dt::MakeTimeSortKeyV3(value,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first,allocation_control);EndHeapFailure();BeginHeapFailure();auto owned_val=dt::EncodeTimeSbdvalComposedV3(value,true,allocation_control);EndHeapFailure();BeginHeapFailure();auto owned_pv=dt::EncodeTimeSbdpvComposedV3(value,true,allocation_control);EndHeapFailure();cast.control=allocation_control;BeginHeapFailure();auto owned_cast=dt::CastTimeValueV3(cast);EndHeapFailure();BeginHeapFailure(2);auto batch=dt::MaterializeTimeBatchIntoV3(profile,input,bitmap,batch_output.data(),sizeof(batch_output),batch_bitmap.data(),batch_bitmap.size(),allocation_control);EndHeapFailure();Check(!render.ok()&&!hash.ok()&&!owned_key.ok()&&!owned_val.ok()&&!owned_pv.ok()&&!owned_cast.ok()&&!batch.ok(),"allocation failures injected across staging classes");Check(allocation.all_zero&&Seen(allocation,dt::TimeScrubClassV3::render_owned_buffer)&&Seen(allocation,dt::TimeScrubClassV3::render_staging)&&Seen(allocation,dt::TimeScrubClassV3::hash_owned_buffer)&&Seen(allocation,dt::TimeScrubClassV3::hash_preimage)&&Seen(allocation,dt::TimeScrubClassV3::hash_digest)&&Seen(allocation,dt::TimeScrubClassV3::ordered_key_owned_buffer)&&Seen(allocation,dt::TimeScrubClassV3::ordered_key_staging)&&Seen(allocation,dt::TimeScrubClassV3::sbdval_component_staging)&&Seen(allocation,dt::TimeScrubClassV3::sbdpv_component_staging)&&Seen(allocation,dt::TimeScrubClassV3::character_render_staging)&&Seen(allocation,dt::TimeScrubClassV3::batch_values_staging),"identified allocation-failure cleanup classes");
}
void CapacityOverlapCancellation(const std::shared_ptr<const dt::TimeValidatedProfileHandleV3>&profile){dt::TimeOwnedValueV3 v{profile,dt::TimeValueStateV3::value,45'296'100'000'000ull};
std::array<p::byte,108>b{};
b.fill(0xa5);
auto before=b;
Check(!dt::EncodeCanonicalTimeComponentIntoNoAllocV3(v,b.data(),7).ok()&&b==before,"component short unchanged");
Check(!dt::RenderCanonicalTimeIntoNoAllocV3(v,false,reinterpret_cast<char*>(b.data()),9).ok()&&b==before,"render short unchanged");
Check(!dt::HashTimeValueIntoNoAllocV3(v,b.data(),31).ok()&&b==before,"hash short unchanged");
Check(!dt::MakeTimeSortKeyIntoNoAllocV3(v,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first,b.data(),107).ok()&&b==before,"key short unchanged");
  auto*cprofile=const_cast<dt::TimeValidatedProfileHandleV3*>(profile.get());
Check(!dt::EncodeCanonicalTimeComponentIntoNoAllocV3(v,cprofile->profile_material.data(),8).ok(),"component profile overlap");
Check(!dt::RenderCanonicalTimeIntoNoAllocV3(v,false,reinterpret_cast<char*>(cprofile->profile_material.data()),25).ok(),"render profile overlap");
Check(!dt::HashTimeValueIntoNoAllocV3(v,cprofile->profile_material.data(),32).ok(),"hash profile overlap");
Check(!dt::MakeTimeSortKeyIntoNoAllocV3(v,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first,cprofile->profile_material.data(),108).ok(),"key profile overlap");
Check(dt::ValidateTimeProfileHandleV3(*profile).ok(),"overlap attempts preserve profile");
  dt::TimeExecutionControlV3 zero;
zero.maximum_allocation_bytes=0;
Check(!dt::EncodeCanonicalTimeComponentIntoNoAllocV3(v,reinterpret_cast<p::byte*>(&zero),8,zero).ok(),"component control overlap");
Check(!dt::RenderCanonicalTimeIntoNoAllocV3(v,false,reinterpret_cast<char*>(&zero),18,zero).ok(),"render control overlap");
Check(!dt::HashTimeValueIntoNoAllocV3(v,reinterpret_cast<p::byte*>(&zero),32,zero).ok(),"hash control overlap");
Check(!dt::MakeTimeSortKeyIntoNoAllocV3(v,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first,reinterpret_cast<p::byte*>(&zero),108,zero).ok(),"key control overlap");
  for(unsigned which=0;
which<4;
++which){b.fill(0xa5);
Cancel c{0,1};
auto ctl=Control(c);
bool refused=false;
if(which==0)refused=!dt::EncodeCanonicalTimeComponentIntoNoAllocV3(v,b.data(),8,ctl).ok();
if(which==1)refused=!dt::RenderCanonicalTimeIntoNoAllocV3(v,false,reinterpret_cast<char*>(b.data()),18,ctl).ok();
if(which==2)refused=!dt::HashTimeValueIntoNoAllocV3(v,b.data(),32,ctl).ok();
if(which==3)refused=!dt::MakeTimeSortKeyIntoNoAllocV3(v,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first,b.data(),108,ctl).ok();
Check(refused&&b==before,"noalloc cancellation unchanged");
}
auto val=dt::EncodeTimeSbdvalComposedV3(v,true);ScrubObservation observed;dt::TimeExecutionControlV3 scrub{};scrub.observe_scrubbed=ObserveScrub;scrub.scrub_observer_context=&observed;
Check(dt::DecodeTimeSbdvalComposedNoAllocV3(*profile,true,val.bytes,scrub).ok()&&observed.total()!=0&&observed.all_zero,"observable stack staging scrub");
ScrubObservation refused_scrub;dt::TimeExecutionControlV3 refuse{};refuse.observe_scrubbed=ObserveScrub;refuse.scrub_observer_context=&refused_scrub;refuse.force_reencode_mismatch_for_conformance=true;Check(!dt::DecodeTimeSbdvalComposedNoAllocV3(*profile,true,val.bytes,refuse).ok()&&refused_scrub.total()!=0&&refused_scrub.all_zero,"observable refusal staging scrub");
ScrubObservation cancel_scrub;Cancel final{0,1};auto cancelled=Control(final);cancelled.observe_scrubbed=ObserveScrub;cancelled.scrub_observer_context=&cancel_scrub;Check(!dt::DecodeTimeSbdvalComposedNoAllocV3(*profile,true,val.bytes,cancelled).ok()&&cancel_scrub.total()!=0&&cancel_scrub.all_zero,"observable cancellation staging scrub");
}
void OwnedFinalCancellation(const std::shared_ptr<const dt::TimeValidatedProfileHandleV3>&profile){dt::TimeOwnedValueV3 v{profile,dt::TimeValueStateV3::value,45'296'100'000'000ull};
Cancel c1{0,2};
auto component=dt::EncodeCanonicalTimeComponentV3(v,Control(c1));
Check(!component.ok()&&component.bytes.empty()&&c1.calls==2,"component final cancellation");
Cancel c2{0,3};
auto render=dt::RenderCanonicalTimeV3(v,false,Control(c2));
Check(!render.ok()&&render.text.empty()&&c2.calls==3,"render final cancellation");
Cancel c3{0,2};
auto hash=dt::HashTimeValueV3(v,Control(c3));
Check(!hash.ok()&&hash.bytes.empty()&&c3.calls==2,"hash final cancellation");
Cancel c4{0,2};
auto key=dt::MakeTimeSortKeyV3(v,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first,Control(c4));
Check(!key.ok()&&key.bytes.empty()&&c4.calls==2,"key final cancellation");
Cancel c5{0,2};
auto val=dt::EncodeTimeSbdvalComposedV3(v,true,Control(c5));
Check(!val.ok()&&val.bytes.empty()&&c5.calls==2,"SBDVAL final cancellation");
Cancel c6{0,2};
auto pv=dt::EncodeTimeSbdpvComposedV3(v,true,Control(c6));
Check(!pv.ok()&&pv.bytes.empty()&&c6.calls==2,"SBDPV final cancellation");
Check(!dt::EncodeCanonicalTimeComponentV3(v,{7}).ok()&&!dt::RenderCanonicalTimeV3(v,false,{9}).ok()&&!dt::HashTimeValueV3(v,{31}).ok()&&!dt::MakeTimeSortKeyV3(v,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first,{107}).ok()&&!dt::EncodeTimeSbdvalComposedV3(v,true,{39}).ok()&&!dt::EncodeTimeSbdpvComposedV3(v,true,{31}).ok(),"owned resource denials");
}
void AllCheckpointCancellation(const std::shared_ptr<const dt::TimeValidatedProfileHandleV3>&profile){
 dt::TimeOwnedValueV3 v{profile,dt::TimeValueStateV3::value,45'296'100'000'000ull};
 for(unsigned at=1;at<=2;++at){Cancel c{0,at};Check(!dt::EncodeCanonicalTimeComponentV3(v,Control(c)).ok()&&c.calls==at,"component each cancellation checkpoint");}
 for(unsigned at=1;at<=3;++at){Cancel c{0,at};Check(!dt::RenderCanonicalTimeV3(v,false,Control(c)).ok()&&c.calls==at,"render each cancellation checkpoint");}
 for(unsigned at=1;at<=2;++at){Cancel c{0,at};Check(!dt::HashTimeValueV3(v,Control(c)).ok()&&c.calls==at,"hash each cancellation checkpoint");}
 for(unsigned at=1;at<=2;++at){Cancel c{0,at};Check(!dt::MakeTimeSortKeyV3(v,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first,Control(c)).ok()&&c.calls==at,"key each cancellation checkpoint");}
 for(unsigned at=1;at<=2;++at){Cancel c{0,at};Check(!dt::EncodeTimeSbdvalComposedV3(v,true,Control(c)).ok()&&c.calls==at,"SBDVAL each cancellation checkpoint");}
 for(unsigned at=1;at<=2;++at){Cancel c{0,at};Check(!dt::EncodeTimeSbdpvComposedV3(v,true,Control(c)).ok()&&c.calls==at,"SBDPV each cancellation checkpoint");}
 Cancel retry{0,99};Check(dt::EncodeCanonicalTimeComponentV3(v,Control(retry)).ok()&&retry.calls==2,"component retry exhausts deterministic checkpoints");
}
void BatchAtomicity(const std::shared_ptr<const dt::TimeValidatedProfileHandleV3>&profile){std::array<p::u64,9>input{{0,1,0,100,1'000,10'000,100'000,1'000'000,dt::kTimeMaximumNanosecondsV3}};std::array<p::byte,2>bits{{0x04,0x00}};std::array<p::u64,9>out{};out.fill(0xdeadbeef);std::array<p::byte,2>outbits{{0xa5,0xa5}};auto before=out;auto bits_before=outbits;auto ok=dt::MaterializeTimeBatchIntoV3(profile,input,bits,out.data(),out.size()*8,outbits.data(),outbits.size());Check(ok.ok()&&out==input&&outbits==bits,"batch publish");auto empty=dt::MaterializeTimeBatchIntoV3(profile,std::span<const p::u64>{},std::span<const p::byte>{},nullptr,0,nullptr,0);Check(empty.ok()&&empty.values_bytes==0&&empty.bitmap_bytes==0&&empty.combined_bytes==0,"empty batch publishes zero extents without output storage");out=before;outbits=bits_before;Check(!dt::MaterializeTimeBatchIntoV3(profile,input,bits,out.data(),out.size()*8-1,outbits.data(),outbits.size()).ok()&&out==before&&outbits==bits_before,"batch short unchanged");auto dirty=input;dirty[2]=1;Check(!dt::MaterializeTimeBatchIntoV3(profile,dirty,bits,out.data(),out.size()*8,outbits.data(),outbits.size()).ok()&&out==before&&outbits==bits_before,"batch dirty NULL unchanged");auto tail=bits;tail[1]=0x80;Check(!dt::MaterializeTimeBatchIntoV3(profile,input,tail,out.data(),out.size()*8,outbits.data(),outbits.size()).ok()&&out==before&&outbits==bits_before,"batch tail bits unchanged");Check(!dt::MaterializeTimeBatchIntoV3(profile,input,bits,const_cast<p::u64*>(input.data()),input.size()*8,outbits.data(),outbits.size()).ok()&&input[0]==0,"batch input overlap");for(unsigned at=1;at<=2;++at){out=before;outbits=bits_before;Cancel c{0,at};Check(!dt::MaterializeTimeBatchIntoV3(profile,input,bits,out.data(),out.size()*8,outbits.data(),outbits.size(),Control(c)).ok()&&out==before&&outbits==bits_before&&c.calls==at,"batch each cancellation checkpoint unchanged");}}
void HeapFailureAndRetry(const std::shared_ptr<const dt::TimeValidatedProfileHandleV3>&profile){
 dt::TimeOwnedValueV3 value{profile,dt::TimeValueStateV3::value,45'296'100'000'000ull};
 auto* character_identity=Identity(dt::CanonicalTypeId::character);Check(character_identity!=nullptr,"character identity");auto character_descriptor=Descriptor(dt::CanonicalTypeId::character);dt::TimeOwnedValueV3 cast_value{profile,dt::TimeValueStateV3::value,dt::kTimeMaximumNanosecondsV3};dt::TimeCastRequestV3 cast;cast.one_based_policy_row=50;cast.time_source=&cast_value;cast.scalar_target=dt::CanonicalTypeId::character;cast.scalar_target_descriptor=character_descriptor;cast.scalar_target_identity=character_identity;cast.context=dt::DatatypeCastContext::explicit_cast;cast.target_null_allowed=character_descriptor.nullable_allowed;
 BeginHeapFailure();auto component=dt::EncodeCanonicalTimeComponentV3(value);EndHeapFailure();Check(!component.ok()&&component.diagnostic.diagnostic_code=="RESOURCE.BUDGET_EXCEEDED"&&component.bytes.empty(),"component heap failure mapped");
 BeginHeapFailure();auto render=dt::RenderCanonicalTimeV3(value,true);EndHeapFailure();Check(!render.ok()&&render.diagnostic.diagnostic_code=="RESOURCE.BUDGET_EXCEEDED"&&render.text.empty(),"render heap failure mapped");
 BeginHeapFailure();auto hash=dt::HashTimeValueV3(value);EndHeapFailure();Check(!hash.ok()&&hash.diagnostic.diagnostic_code=="RESOURCE.BUDGET_EXCEEDED"&&hash.bytes.empty(),"hash heap failure mapped");
 BeginHeapFailure();auto key=dt::MakeTimeSortKeyV3(value,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first);EndHeapFailure();Check(!key.ok()&&key.diagnostic.diagnostic_code=="RESOURCE.BUDGET_EXCEEDED"&&key.bytes.empty(),"key heap failure mapped");
 BeginHeapFailure();auto val=dt::EncodeTimeSbdvalComposedV3(value,true);EndHeapFailure();Check(!val.ok()&&val.diagnostic.diagnostic_code=="RESOURCE.BUDGET_EXCEEDED"&&val.bytes.empty(),"SBDVAL heap failure mapped");
 BeginHeapFailure();auto pv=dt::EncodeTimeSbdpvComposedV3(value,true);EndHeapFailure();Check(!pv.ok()&&pv.diagnostic.diagnostic_code=="RESOURCE.BUDGET_EXCEEDED"&&pv.bytes.empty(),"SBDPV heap failure mapped");
 std::array<p::u64,2> values{{0,1}};std::array<p::byte,1> bitmap{{0}};BeginHeapFailure();auto batch=dt::MaterializeTimeBatchV3(profile,values,bitmap);EndHeapFailure();Check(!batch.ok()&&batch.diagnostic.diagnostic_code=="RESOURCE.BUDGET_EXCEEDED"&&batch.batch.values.empty(),"batch heap failure mapped");
 ScrubObservation cast_scrub;cast.control.observe_scrubbed=ObserveScrub;cast.control.scrub_observer_context=&cast_scrub;BeginHeapFailure();auto cast_failed=dt::CastTimeValueV3(cast);EndHeapFailure();Check(!cast_failed.ok()&&cast_failed.diagnostic.diagnostic_code=="RESOURCE.BUDGET_EXCEEDED"&&cast_failed.scalar_value.encoded_value.empty()&&cast_scrub.total()!=0&&cast_scrub.all_zero,"owning character cast heap failure mapped and staging scrubbed");cast.control={};
 for(unsigned at=1;at<=2;++at){Cancel c{0,at};cast.control=Control(c);auto cancelled=dt::CastTimeValueV3(cast);Check(!cancelled.ok()&&cancelled.diagnostic.diagnostic_code=="PROCESS.CANCELLED"&&cancelled.scalar_value.encoded_value.empty()&&c.calls==at,"cast each cancellation checkpoint");}cast.control={};
 Check(dt::EncodeCanonicalTimeComponentV3(value).ok()&&dt::RenderCanonicalTimeV3(value).text=="12:34:56.1"&&dt::HashTimeValueV3(value).ok()&&dt::MakeTimeSortKeyV3(value,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first).ok()&&dt::EncodeTimeSbdvalComposedV3(value,true).ok()&&dt::EncodeTimeSbdpvComposedV3(value,true).ok()&&dt::MaterializeTimeBatchV3(profile,values,bitmap).ok()&&dt::CastTimeValueV3(cast).ok(),"complete deterministic retry after heap failure");
}
void SharedProfileConcurrency(const std::shared_ptr<const dt::TimeValidatedProfileHandleV3>&profile){
 const auto pins=profile.use_count();std::array<bool,8> accepted{};std::array<std::thread,8> workers;
 for(std::size_t i=0;i<workers.size();++i)workers[i]=std::thread([&,i]{dt::TimeOwnedValueV3 value{profile,dt::TimeValueStateV3::value,static_cast<p::u64>(i)};std::array<p::byte,8> bytes{};accepted[i]=dt::EncodeCanonicalTimeComponentIntoNoAllocV3(value,bytes.data(),bytes.size()).ok()&&p::LoadLittle64(bytes.data())==i;});
 for(auto& worker:workers)worker.join();
 Check(std::all_of(accepted.begin(),accepted.end(),[](bool value){return value;}),"shared profile concurrent publication");Check(profile.use_count()==pins,"shared profile pins released");
}

}
int main(){auto p0=Profile();NoAlloc(p0);FixedOutputGuardContract(p0);ScrubClassCoverage(p0);ScrubFailureCoverage(p0);CapacityOverlapCancellation(p0);OwnedFinalCancellation(p0);AllCheckpointCancellation(p0);BatchAtomicity(p0);HeapFailureAndRetry(p0);SharedProfileConcurrency(p0);std::cout<<"PASS base.time V3 publication atomicity checks="<<checks<<"\n";}
