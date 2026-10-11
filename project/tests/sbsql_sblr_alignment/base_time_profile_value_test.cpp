// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../../src/core/datatypes/datatype_time.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>

namespace dt = scratchbird::core::datatypes;
namespace p = scratchbird::core::platform;

namespace {
void Check(bool value,std::string_view message){if(!value){std::cerr<<"FAIL "<<message<<'\n';std::exit(1);}}
p::Uuid D710(){return p::Uuid{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x10}};}
std::shared_ptr<const dt::TimeValidatedProfileHandleV3> Profile(){
  auto result=dt::BuildCurrentTimeValidatedProfileHandleV3(D710());Check(result.ok(),"build profile");
  return std::make_shared<const dt::TimeValidatedProfileHandleV3>(result.profile);
}
template<class F> void Mutate(const dt::TimeValidatedProfileHandleV3& valid,F mutation,std::string_view label){
  auto bad=valid;mutation(bad);Check(!dt::ValidateTimeProfileHandleV3(bad).ok(),label);
}

std::vector<p::byte> Hex(std::string_view text){
  Check(text.size()%2==0,"hex width");std::vector<p::byte> out(text.size()/2);
  auto nibble=[](char c){return static_cast<unsigned>(c<='9'?c-'0':(c|32)-'a'+10);};
  for(std::size_t i=0;i<out.size();++i)out[i]=static_cast<p::byte>((nibble(text[2*i])<<4)|nibble(text[2*i+1]));
  return out;
}
constexpr std::string_view kProfileMaterialHex=
    "534254494d5030310100500250020000019d000000007000800000000000d7100a000000000000000a000000000000009101000074697d6580000000000000000100000000000000019d000000007000800000000000d81e0100000000000000019d000000007000800000000000d81f0100000000000000010000000000000001a1032b9f517229977b45730f3dff34010000000000000001a1032b9f517229977b45730f3dff35010000000000000001a1032b9f517229977b45730f3dff36010000000000000001a1032b9f517229977b45730f3dff37010000000000000001a1032b9f517229977b45730f3dff3a010000000000000001a1032b9f517229977b45730f3dff38010000000000000001a1032b9f517229977b45730f3dff39010000000000000001a1032b9f517229977b45730f3dff3b010000000000000001a1032b9f517229977b45730f3dff3c010000000000000001a1032b9f517229977b45730f3dff3d010000000000000001a1032b9f517229977b45730f3dff3e010000000000000001a1032b9f517229977b45730f3dff3f010000000000000001a1032b9f517229977b45730f3dff40010000000000000001a1032b9f517229977b45730f3dff42010000000000000001a1032b9f517229977b45730f3dff43010000000000000001a1032b9f517229977b45730f3dff44010000000000000001a1032b9f517229977b45730f3dff46010000000000000001a1032b9f517229977b45730f3dff4701000000000000000000000000000000ffff4e91944e00000900000008000000ff0f000000000000";
constexpr std::string_view kComparisonMaterialHex=
    "534254494d4330310100600100000000019d000000007000800000000000d7100a000000000000000a000000000000009101000074697d6580000000000000000100000000000000019d000000007000800000000000d81e0100000000000000019d000000007000800000000000d81f0100000000000000010000000000000001a1032b9f517229977b45730f3dff34010000000000000001a1032b9f517229977b45730f3dff35010000000000000001a1032b9f517229977b45730f3dff36010000000000000001a1032b9f517229977b45730f3dff3b010000000000000001a1032b9f517229977b45730f3dff3c010000000000000001a1032b9f517229977b45730f3dff3d010000000000000001a1032b9f517229977b45730f3dff3e010000000000000001a1032b9f517229977b45730f3dff370100000000000000ff000000090000000000000000000000ffff4e91944e00000800000000000000";
struct MutationField{std::string_view case_id;std::string_view field;std::size_t offset;};
constexpr std::array<MutationField,57> kProfileMutations{{
{"profile_field_001","magic_ASCII_SBTIMP01",0},
{"profile_field_002","version_u16_le_1",8},
{"profile_field_003","material_bytes_u16_le_592",10},
{"profile_field_004","total_bytes_u32_le_592",12},
{"profile_field_005","snapshot_uuid_raw16",16},
{"profile_field_006","catalog_generation_u64_le_8",32},
{"profile_field_007","registry_generation_u64_le_8",40},
{"profile_field_008","descriptor_uuid_raw16",48},
{"profile_field_009","descriptor_generation_u64_le_1",64},
{"profile_field_010","type_uuid_raw16",72},
{"profile_field_011","type_generation_u64_le_1",88},
{"profile_field_012","codec_uuid_raw16",96},
{"profile_field_013","codec_version_u32_le_1",112},
{"profile_field_014","reserved_zero",116},
{"profile_field_015","codec_generation_u64_le_1",120},
{"profile_field_016","descriptor_policy_uuid_raw16",128},
{"profile_field_017","descriptor_policy_generation_u64_le_1",144},
{"profile_field_018","canonicalization_policy_uuid_raw16",152},
{"profile_field_019","canonicalization_policy_generation_u64_le_1",168},
{"profile_field_020","ordering_policy_uuid_raw16",176},
{"profile_field_021","ordering_policy_generation_u64_le_1",192},
{"profile_field_022","hash_policy_uuid_raw16",200},
{"profile_field_023","hash_policy_generation_u64_le_1",216},
{"profile_field_024","operation_policy_uuid_raw16",224},
{"profile_field_025","operation_policy_generation_u64_le_1",240},
{"profile_field_026","render_policy_uuid_raw16",248},
{"profile_field_027","render_policy_generation_u64_le_1",264},
{"profile_field_028","cast_policy_uuid_raw16",272},
{"profile_field_029","cast_policy_generation_u64_le_1",288},
{"profile_field_030","civil_day_policy_uuid_raw16",296},
{"profile_field_031","civil_day_policy_generation_u64_le_1",312},
{"profile_field_032","storage_epoch_policy_uuid_raw16",320},
{"profile_field_033","storage_epoch_policy_generation_u64_le_1",336},
{"profile_field_034","timezone_none_policy_uuid_raw16",344},
{"profile_field_035","timezone_none_policy_generation_u64_le_1",360},
{"profile_field_036","leap_reject_policy_uuid_raw16",368},
{"profile_field_037","leap_reject_policy_generation_u64_le_1",384},
{"profile_field_038","index_policy_uuid_raw16",392},
{"profile_field_039","index_policy_generation_u64_le_1",408},
{"profile_field_040","statistics_policy_uuid_raw16",416},
{"profile_field_041","statistics_policy_generation_u64_le_1",432},
{"profile_field_042","backup_policy_uuid_raw16",440},
{"profile_field_043","backup_policy_generation_u64_le_1",456},
{"profile_field_044","protection_policy_uuid_raw16",464},
{"profile_field_045","protection_policy_generation_u64_le_1",480},
{"profile_field_046","component_policy_uuid_raw16",488},
{"profile_field_047","component_policy_generation_u64_le_1",504},
{"profile_field_048","diagnostic_policy_uuid_raw16",512},
{"profile_field_049","diagnostic_policy_generation_u64_le_1",528},
{"profile_field_050","metric_policy_uuid_raw16",536},
{"profile_field_051","metric_policy_generation_u64_le_1",552},
{"profile_field_052","minimum_nanoseconds_u64_le_0",560},
{"profile_field_053","maximum_nanoseconds_u64_le_86399999999999",568},
{"profile_field_054","fractional_precision_u32_le_9",576},
{"profile_field_055","component_bytes_u32_le_8",580},
{"profile_field_056","flags_u32_le_4095_bits0_through11_frozen",584},
{"profile_field_057","reserved_zero",588}
}};
constexpr std::array<MutationField,37> kComparisonMutations{{
{"comparison_field_001","magic_ASCII_SBTIMC01",0},
{"comparison_field_002","version_u16_le_1",8},
{"comparison_field_003","material_bytes_u16_le_352",10},
{"comparison_field_004","reserved_zero",12},
{"comparison_field_005","snapshot_uuid_raw16",16},
{"comparison_field_006","catalog_generation_u64_le_8",32},
{"comparison_field_007","registry_generation_u64_le_8",40},
{"comparison_field_008","descriptor_uuid_raw16",48},
{"comparison_field_009","descriptor_generation_u64_le_1",64},
{"comparison_field_010","type_uuid_raw16",72},
{"comparison_field_011","type_generation_u64_le_1",88},
{"comparison_field_012","codec_uuid_raw16",96},
{"comparison_field_013","codec_version_u32_le_1",112},
{"comparison_field_014","reserved_zero",116},
{"comparison_field_015","codec_generation_u64_le_1",120},
{"comparison_field_016","descriptor_policy_uuid_raw16",128},
{"comparison_field_017","descriptor_policy_generation_u64_le_1",144},
{"comparison_field_018","canonicalization_policy_uuid_raw16",152},
{"comparison_field_019","canonicalization_policy_generation_u64_le_1",168},
{"comparison_field_020","ordering_policy_uuid_raw16",176},
{"comparison_field_021","ordering_policy_generation_u64_le_1",192},
{"comparison_field_022","civil_day_policy_uuid_raw16",200},
{"comparison_field_023","civil_day_policy_generation_u64_le_1",216},
{"comparison_field_024","storage_epoch_policy_uuid_raw16",224},
{"comparison_field_025","storage_epoch_policy_generation_u64_le_1",240},
{"comparison_field_026","timezone_none_policy_uuid_raw16",248},
{"comparison_field_027","timezone_none_policy_generation_u64_le_1",264},
{"comparison_field_028","leap_reject_policy_uuid_raw16",272},
{"comparison_field_029","leap_reject_policy_generation_u64_le_1",288},
{"comparison_field_030","hash_policy_uuid_raw16",296},
{"comparison_field_031","hash_policy_generation_u64_le_1",312},
{"comparison_field_032","flags_u32_le_255_bits0_through7_frozen",320},
{"comparison_field_033","fractional_precision_u32_le_9",324},
{"comparison_field_034","minimum_nanoseconds_u64_le_0",328},
{"comparison_field_035","maximum_nanoseconds_u64_le_86399999999999",336},
{"comparison_field_036","component_bytes_u32_le_8",344},
{"comparison_field_037","reserved_zero",348}
}};
struct Vector{p::u64 value;std::string_view text;};
constexpr std::array<Vector,20> kVectors{{
 {0,"00:00:00"},{1,"00:00:00.000000001"},{10,"00:00:00.00000001"},
 {100,"00:00:00.0000001"},{1'000,"00:00:00.000001"},
 {10'000,"00:00:00.00001"},{100'000,"00:00:00.0001"},
 {1'000'000,"00:00:00.001"},{10'000'000,"00:00:00.01"},
 {100'000'000,"00:00:00.1"},{1'000'000'000,"00:00:01"},
 {60'000'000'000ull,"00:01:00"},{3'600'000'000'000ull,"01:00:00"},
 {999'999'999,"00:00:00.999999999"},{59'999'999'999ull,"00:00:59.999999999"},
 {3'599'999'999'999ull,"00:59:59.999999999"},{43'200'000'000'000ull,"12:00:00"},
 {45'296'100'000'000ull,"12:34:56.1"},{86'399'000'000'000ull,"23:59:59"},
 {86'399'999'999'999ull,"23:59:59.999999999"}
}};
}

void TestD711Successor() {
  const dt::DatatypeTypeCodecIdentityRowV3* row = nullptr;
  for (const auto& candidate : dt::CurrentDatatypeTypeCodecIdentityRowsV3())
    if (candidate.legacy_fields.catalog_snapshot_uuid == dt::kDatatypeCohortV11 &&
        candidate.legacy_fields.canonical_binary_type_code == static_cast<p::u32>(dt::CanonicalTypeId::time)) row = &candidate;
  Check(row != nullptr,"registered D711 TIME row");
  const dt::TimeAuthorityReceiptV3 receipt{dt::kDatatypeCohortV11,dt::kDatatypeCohortV11,11,11};
  const auto built = dt::BuildTimeValidatedProfileHandleV3(receipt,*row);
  Check(built.ok(),"explicit D711 TIME profile");
  auto profile = std::make_shared<const dt::TimeValidatedProfileHandleV3>(built.profile);
  auto old = Profile();
  auto expected_profile = Hex(kProfileMaterialHex), expected_comparison = Hex(kComparisonMaterialHex);
  for (auto* bytes : {&expected_profile,&expected_comparison}) {
    const auto uuid = Hex("01a1095ff20572d3abea15c6d41a4ad4");
    std::copy(uuid.begin(),uuid.end(),bytes->begin()+16);
    p::StoreLittle64(bytes->data()+32,11);p::StoreLittle64(bytes->data()+40,11);
  }
  Check(std::equal(expected_profile.begin(),expected_profile.end(),profile->profile_material.begin()) &&
        std::equal(expected_comparison.begin(),expected_comparison.end(),profile->comparison_material.begin()),
        "D711 material changes only its explicit cohort");
  const auto profile_hash=Hex("f17f7861ee31364530c880dc01b1836a8629d4c5c6ea5d287b4f9bef1882528d");
  const auto comparison_hash=Hex("1673475a46395937c1eef4e90bdc92e72eec21a47404394bfe272224cf983f44");
  Check(std::equal(profile_hash.begin(),profile_hash.end(),profile->profile_fingerprint.begin()) &&
        std::equal(comparison_hash.begin(),comparison_hash.end(),profile->comparison_fingerprint.begin()),"independent D711 SHA256 seals");
  for (const auto& mutation:kProfileMutations) Mutate(*profile,[&](auto& p){p.profile_material[mutation.offset]^=1;},mutation.case_id);
  for (const auto& mutation:kComparisonMutations) Mutate(*profile,[&](auto& p){p.comparison_material[mutation.offset]^=1;},mutation.case_id);
  Mutate(*profile,[](auto& p){p.identity.native_fields.canonical_value_transport_width++;},"native extent is authority");
  Mutate(*profile,[](auto& p){p.identity.native_fields.profile_fingerprint_sha256[0]^=1;},"native fingerprint is authority");
  Check(!dt::BuildTimeValidatedProfileHandleV3(receipt,old->identity).ok() &&
        !dt::BuildTimeValidatedProfileHandleV3(old->receipt,*row).ok(),"no successor relabeling");
  for(const auto& vector:kVectors) {
    auto parsed=dt::ParseCanonicalTimeV3(profile,vector.text);
    Check(parsed.ok()&&parsed.value.nanoseconds_since_midnight==vector.value,"D711 parse canonical vector");
    auto component=dt::EncodeCanonicalTimeComponentV3(parsed.value);
    Check(component.ok()&&component.bytes.size()==8&&p::LoadLittle64(component.bytes.data())==vector.value,"D711 LE8 native component");
    auto decoded=dt::DecodeCanonicalTimeComponentNoAllocV3(*profile,dt::TimeValueStateV3::value,true,component.bytes);
    Check(decoded.ok()&&decoded.value.profile==profile.get()&&decoded.value.nanoseconds_since_midnight==vector.value,"D711 decoded source profile");
    for(auto direction:{dt::TimeSortDirectionV3::ascending,dt::TimeSortDirectionV3::descending})
      for(auto mode:{dt::TimeNullModeV3::nulls_first,dt::TimeNullModeV3::nulls_last}) {
        auto key=dt::MakeTimeSortKeyV3(parsed.value,direction,mode);
        Check(key.ok()&&key.bytes.size()==108&&p::LoadLittle64(key.bytes.data()+24)==11&&
              p::LoadLittle64(key.bytes.data()+32)==11&&
              std::equal(dt::kDatatypeCohortV11.bytes.begin(),dt::kDatatypeCohortV11.bytes.end(),key.bytes.begin()+8),"D711 key exact cohort");
        auto restored=dt::DecodeTimeSortKeyNoAllocV3(*profile,key.bytes);
        Check(restored.ok()&&restored.value.nanoseconds_since_midnight==vector.value&&restored.value.direction==direction&&restored.value.null_mode==mode,"D711 key roundtrip");
        Check(!dt::DecodeTimeSortKeyNoAllocV3(*old,key.bytes).ok(),"D710 may not consume D711 key");
        dt::TimeOwnedValueV3 historical{old,dt::TimeValueStateV3::value,vector.value};
        auto historical_key=dt::MakeTimeSortKeyV3(historical,direction,mode);
        Check(historical_key.ok()&&!dt::DecodeTimeSortKeyNoAllocV3(*profile,historical_key.bytes).ok(),"D711 may not consume D710 key");
      }
  }
  for(bool is_null:{false,true}) {
    dt::TimeOwnedValueV3 value{profile,is_null?dt::TimeValueStateV3::sql_null:dt::TimeValueStateV3::value,0};
    const auto hash=dt::HashTimeValueV3(value);
    Check(hash.ok()&&hash.bytes==Hex(is_null?"fd0596f8149a17e97f0256ddd1de3f325bfe063eac9ed8bb9ea2ae641fa33ff7":"dbe0780ad39860d34469e755e6fc68abd5b18dae2e1189bf48423d6f650531dd"),"independent D711 NULL/value hash oracle");
    auto key=dt::MakeTimeSortKeyV3(value,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first);
    Check(key.ok()&&dt::DecodeTimeSortKeyNoAllocV3(*profile,key.bytes).ok(),"D711 NULL/value key");
  }
}

int main(){
  TestD711Successor();
  auto profile=Profile();const auto& valid=*profile;
  Check(valid.profile_material.size()==592&&std::memcmp(valid.profile_material.data(),"SBTIMP01",8)==0,"SBTIMP01");
  Check(valid.comparison_material.size()==352&&std::memcmp(valid.comparison_material.data(),"SBTIMC01",8)==0,"SBTIMC01");
  Check(p::LoadLittle32(valid.profile_material.data()+12)==592,"profile total bytes");
  Check(p::LoadLittle32(valid.profile_material.data()+584)==4095&&p::LoadLittle32(valid.profile_material.data()+588)==0,"profile flags/reserved");
  Check(p::LoadLittle32(valid.comparison_material.data()+12)==0,"comparison offset12 reserved");
  Check(p::LoadLittle32(valid.comparison_material.data()+320)==255&&p::LoadLittle32(valid.comparison_material.data()+348)==0,"comparison flags/reserved");
  const auto expected_profile_material=Hex(kProfileMaterialHex);
  const auto expected_comparison_material=Hex(kComparisonMaterialHex);
  Check(expected_profile_material.size()==valid.profile_material.size()&&
        std::memcmp(valid.profile_material.data(),expected_profile_material.data(),valid.profile_material.size())==0,
        "exact SBTIMP01 material");
  Check(expected_comparison_material.size()==valid.comparison_material.size()&&
        std::memcmp(valid.comparison_material.data(),expected_comparison_material.data(),valid.comparison_material.size())==0,
        "exact SBTIMC01 material");
  constexpr std::array<p::byte,32> expected_profile{{0x98,0x6d,0xff,0x7c,0x60,0xb8,0xf3,0xeb,0x6f,0x70,0x1c,0xe8,0xc7,0xb7,0xf0,0x99,0x7f,0x94,0xa1,0x8d,0x3a,0x0a,0x9b,0xcb,0x73,0x6c,0x11,0xf3,0x5e,0x47,0x6f,0x48}};
  constexpr std::array<p::byte,32> expected_comparison{{0x75,0xde,0xab,0xa7,0x96,0xda,0x7e,0x20,0x08,0x60,0x02,0x37,0xa5,0xeb,0xfb,0x0d,0xd8,0xa7,0x6f,0x2b,0x62,0xea,0x66,0x4b,0x66,0x87,0x6b,0xa4,0x7f,0x05,0xb3,0xd6}};
  Check(valid.profile_fingerprint==expected_profile&&valid.comparison_fingerprint==expected_comparison,"exact fingerprints");
  unsigned exact_profile_mutations=0;
  for(const auto& fixture:kProfileMutations){auto mutated=valid;mutated.profile_material[fixture.offset]^=1;Check(mutated.profile_fingerprint==valid.profile_fingerprint,"copied profile digest remains frozen");auto result=dt::ValidateTimeProfileHandleV3(mutated);Check(!result.ok()&&result.diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID",fixture.case_id);++exact_profile_mutations;}
  for(const auto& fixture:kComparisonMutations){auto mutated=valid;mutated.comparison_material[fixture.offset]^=1;Check(mutated.comparison_fingerprint==valid.comparison_fingerprint,"copied comparison digest remains frozen");auto result=dt::ValidateTimeProfileHandleV3(mutated);Check(!result.ok()&&result.diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID",fixture.case_id);++exact_profile_mutations;}
  Check(exact_profile_mutations==kProfileMutations.size()+kComparisonMutations.size()&&exact_profile_mutations==94,"all 94 Core field mutations executed");

  Mutate(valid,[](auto&x){x.receipt.catalog_generation++;},"receipt generation");
  Mutate(valid,[](auto&x){x.receipt.registry_generation++;},"registry generation");
  Mutate(valid,[](auto&x){x.receipt.catalog_snapshot_uuid.bytes[0]^=1;},"receipt UUID");
  Mutate(valid,[](auto&x){x.identity.legacy_fields.descriptor_uuid.bytes[0]^=1;},"descriptor UUID");
  Mutate(valid,[](auto&x){x.identity.legacy_fields.type_uuid.bytes[0]^=1;},"type UUID");
  Mutate(valid,[](auto&x){x.identity.legacy_fields.codec_uuid.bytes[0]^=1;},"codec UUID");
  Mutate(valid,[](auto&x){x.identity.legacy_fields.codec_generation++;},"codec generation");
  Mutate(valid,[](auto&x){x.identity.descriptor_policy.uuid.bytes[0]^=1;},"descriptor policy");
  Mutate(valid,[](auto&x){x.identity.canonicalization_policy.uuid.bytes[0]^=1;},"canonical policy");
  Mutate(valid,[](auto&x){x.identity.ordering_policy.uuid.bytes[0]^=1;},"ordering policy");
  Mutate(valid,[](auto&x){x.identity.hash_policy.uuid.bytes[0]^=1;},"hash policy");
  Mutate(valid,[](auto&x){x.identity.operation_policy.uuid.bytes[0]^=1;},"operation policy");
  Mutate(valid,[](auto&x){x.render_policy.uuid.bytes[0]^=1;},"render policy");
  Mutate(valid,[](auto&x){x.cast_policy.uuid.bytes[0]^=1;},"cast policy");
  Mutate(valid,[](auto&x){x.civil_day_policy.uuid.bytes[0]^=1;},"civil policy");
  Mutate(valid,[](auto&x){x.storage_epoch_policy.uuid.bytes[0]^=1;},"epoch policy");
  Mutate(valid,[](auto&x){x.timezone_none_policy.uuid.bytes[0]^=1;},"timezone policy");
  Mutate(valid,[](auto&x){x.leap_second_policy.uuid.bytes[0]^=1;},"leap policy");
  Mutate(valid,[](auto&x){x.index_policy.uuid.bytes[0]^=1;},"index policy");
  Mutate(valid,[](auto&x){x.statistics_policy.uuid.bytes[0]^=1;},"statistics policy");
  Mutate(valid,[](auto&x){x.backup_transport_policy.uuid.bytes[0]^=1;},"backup policy");
  Mutate(valid,[](auto&x){x.protection_policy.uuid.bytes[0]^=1;},"protection policy");
  Mutate(valid,[](auto&x){x.component_adapter_policy.uuid.bytes[0]^=1;},"component policy");
  Mutate(valid,[](auto&x){x.diagnostic_policy.uuid.bytes[0]^=1;},"diagnostic policy");
  Mutate(valid,[](auto&x){x.metric_policy.uuid.bytes[0]^=1;},"metric policy");
  Mutate(valid,[](auto&x){x.profile_material[12]^=1;},"profile material");
  Mutate(valid,[](auto&x){x.comparison_material[12]=1;},"comparison reserved");
  Mutate(valid,[](auto&x){x.profile_fingerprint[0]^=1;},"profile fingerprint");
  Mutate(valid,[](auto&x){x.comparison_fingerprint[0]^=1;},"comparison fingerprint");
  auto identity_alias=valid.identity;identity_alias.legacy_fields.canonical_name="heure";identity_alias.legacy_fields.codec_id="codec-temps-natif";
  auto alias_build=dt::BuildTimeValidatedProfileHandleV3(valid.receipt,identity_alias);Check(alias_build.ok(),"name and codec text are presentation only");
  Check(dt::ValidateTimeProfileHandleV3(alias_build.profile).ok(),"alias profile validates");

  for(const auto& vector:kVectors){
    dt::TimeOwnedValueV3 owned{profile,dt::TimeValueStateV3::value,vector.value};
    auto encoded=dt::EncodeCanonicalTimeComponentV3(owned);Check(encoded.ok()&&encoded.bytes.size()==8,"LE8 encode");
    Check(p::LoadLittle64(encoded.bytes.data())==vector.value,"LE8 bytes");
    auto decoded=dt::DecodeCanonicalTimeComponentNoAllocV3(*profile,dt::TimeValueStateV3::value,true,encoded.bytes);
    Check(decoded.ok()&&decoded.value.nanoseconds_since_midnight==vector.value,"LE8 decode");
    auto rendered=dt::RenderCanonicalTimeV3(owned);Check(rendered.ok()&&rendered.text==vector.text,"canonical render");
    auto parsed=dt::ParseCanonicalTimeV3(profile,vector.text);Check(parsed.ok()&&parsed.value.nanoseconds_since_midnight==vector.value,"canonical parse");
  }
  std::array<p::byte,8> over{};p::StoreLittle64(over.data(),dt::kTimeMaximumNanosecondsV3+1);
  Check(!dt::DecodeCanonicalTimeComponentNoAllocV3(*profile,dt::TimeValueStateV3::value,true,over).ok(),"above maximum");
  Check(!dt::DecodeCanonicalTimeComponentNoAllocV3(*profile,dt::TimeValueStateV3::value,true,{over.data(),7}).ok(),"short component");
  Check(!dt::DecodeCanonicalTimeComponentNoAllocV3(*profile,dt::TimeValueStateV3::value,true,{over.data(),0}).ok(),"empty present component");
  Check(dt::DecodeCanonicalTimeComponentNoAllocV3(*profile,dt::TimeValueStateV3::sql_null,true,{}).ok(),"clean NULL");
  Check(!dt::DecodeCanonicalTimeComponentNoAllocV3(*profile,dt::TimeValueStateV3::sql_null,true,over).ok(),"dirty NULL component");
  Check(!dt::ValidateTimeValueViewV3({profile.get(),dt::TimeValueStateV3::sql_null,1}).ok(),"dirty NULL scalar");
  Check(!dt::ValidateTimeValueViewV3({profile.get(),static_cast<dt::TimeValueStateV3>(9),0}).ok(),"unknown state");

  std::array<p::u64,3> values{{0,0,dt::kTimeMaximumNanosecondsV3}};std::array<p::byte,1> bitmap{{2}};
  Check(dt::ValidateTimeBatchViewV3({profile.get(),values,bitmap}).ok(),"batch valid");
  values[1]=1;Check(!dt::ValidateTimeBatchViewV3({profile.get(),values,bitmap}).ok(),"batch dirty NULL");values[1]=0;
  bitmap[0]=0x82;Check(!dt::ValidateTimeBatchViewV3({profile.get(),values,bitmap}).ok(),"batch tail bits");bitmap[0]=2;
  auto batch=dt::MaterializeTimeBatchV3(profile,values,bitmap);Check(batch.ok()&&batch.batch.values==std::vector<p::u64>(values.begin(),values.end()),"batch lifetime");
  Check(dt::ComputeTimeBatchExtentsV3(3,25).ok()&&!dt::ComputeTimeBatchExtentsV3(3,24).ok(),"batch resource extent");
  std::cout<<"PASS base.time V3 profile/value vectors=21 profile_mutations="<<kProfileMutations.size()<<" comparison_mutations="<<kComparisonMutations.size()<<" total_mutations="<<exact_profile_mutations<<"\n";
}
