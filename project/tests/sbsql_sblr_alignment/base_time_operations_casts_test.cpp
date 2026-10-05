// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../../src/core/datatypes/datatype_time.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>
namespace dt=scratchbird::core::datatypes;
namespace p=scratchbird::core::platform;
namespace{
unsigned checks=0;
[[noreturn]]void Fail(std::string_view m){std::cerr<<"FAIL "<<m<<'\n';
std::exit(1);
}void Check(bool v,std::string_view m){++checks;
if(!v)Fail(m);
}
p::Uuid D710(){return{{1,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x10}};
}
std::shared_ptr<const dt::TimeValidatedProfileHandleV3> Profile(){auto r=dt::BuildCurrentTimeValidatedProfileHandleV3(D710());
Check(r.ok(),"profile");
return std::make_shared<const dt::TimeValidatedProfileHandleV3>(r.profile);
}
const dt::DatatypeTypeCodecIdentityRowV3* Identity(dt::CanonicalTypeId t){for(const auto&r:dt::CurrentDatatypeTypeCodecIdentityRowsV3())if(r.legacy_fields.canonical_binary_type_code==static_cast<p::u32>(t)&&r.legacy_fields.catalog_snapshot_uuid==D710()&&r.legacy_fields.catalog_generation==10&&r.legacy_fields.registry_generation==10)return&r;
return nullptr;
}
scratchbird::engine::ExecutionTypeDescriptor Descriptor(dt::CanonicalTypeId type){auto m=dt::LoadCurrentCoreDatatypeCatalogManifest();
Check(m.ok(),"catalog");
auto row=dt::LookupDatatypeCatalogRow(m.manifest,type);
Check(row.ok()&&row.manifest.descriptor_rows.size()==1,"descriptor row");
dt::CatalogExecutionTypeMetadata md;
md.descriptor_uuid=row.manifest.descriptor_rows.front().descriptor_uuid;
md.descriptor_epoch=row.manifest.descriptor_rows.front().descriptor_epoch;
auto d=dt::LookupExecutionTypeDescriptorFromCatalog(type,md);
Check(d.ok(),"execution descriptor");
return d.descriptor;
}
std::array<p::byte,32> Hex32(std::string_view s){std::array<p::byte,32>r{};
auto n=[](char c){return unsigned(c<='9'?c-'0':(c|32)-'a'+10);
};
Check(s.size()==64,"hash width");
for(size_t i=0;
i<32;
++i)r[i]=p::byte((n(s[2*i])<<4)|n(s[2*i+1]));
return r;
}
bool ExactHex(std::span<const p::byte> actual,std::string_view expected){
  auto n=[](char c){return unsigned(c<='9'?c-'0':(c|32)-'a'+10);};
  if(expected.size()!=actual.size()*2)return false;
  for(std::size_t i=0;i<actual.size();++i)
    if(actual[i]!=static_cast<p::byte>((n(expected[2*i])<<4)|n(expected[2*i+1])))return false;
  return true;
}
bool Stop(void*)noexcept{return true;
}
constexpr std::array<p::u64,20> values{{0,1,10,100,1'000,10'000,100'000,1'000'000,10'000'000,100'000'000,1'000'000'000,60'000'000'000ull,3'600'000'000'000ull,999'999'999,59'999'999'999ull,3'599'999'999'999ull,43'200'000'000'000ull,45'296'100'000'000ull,86'399'000'000'000ull,86'399'999'999'999ull}};
constexpr std::array<std::string_view,21> hashes{{
"2c1fdf1dd89c4d9fc1b4f077136778e585424a924f6e8455300e611d02cb262b","44643d545e8e835aeaa292471bd7e61b7ffe6dd77e12ceac96d47374d4e25275","9e46d230f21151e8a04540e124b0597a7744fda2c953c6dd2425d6d9611f3400","100d14e7e82549b59df55767a8dcd154f9c6fb89413309fc338d13b08c552e4b","0dd2f4551b81e60449c364b1649e017c610e8e0b0a188b8cabf744e1c9f50dc0","fd0641ff87b4a66842448d63e5fed3137ea9623d8b334e6412a76900f3bd709d","c873b146bb01d08af466a5d284b8e94bc18af5f0e39e9d192bd19cddf6dea484","29652e927e4e0883a9d8f9f18f225f7b5bb4f012fa9253332d12817331cd3f35","b6d1353db5ad95212f53a9ff9fe7c2c8eedd806321a97cd0ce470225a685afb1","590331b607e60623f6714f4b74b89638be7d10c8906f514cc2aeffa2d220a81a","3c6b332a8ab529a19fe16594f4af65a459c16d36dda28844a2011fd6148f629f","cca1c99d878df225fc3f8b436b0c74042f0eed1d17441709dc95ea7829092b96","427904b644b607163e2d6216809d902a167555bcdf29d1ad864ada4c1b3b6555","61447f75c8993a1b039b90fc6dbec2bd623ff40e8b10f507e6ead81076889d68","8092cf134efed70a130f2e76f08c129b46a7051c19edf5af4ec88f611fd81f4a","3c5c59295c24f27fe66c26a791f50032d99aabad2e8a46b9593735e4ed8c4294","a4a255e2c523d899712e7b6a4b8c6ed8edcf1d2c118f121fd959e13bc533c656","e1cacbb7a9d370529fcf7569db6742a0c36945911918c47499a0682ba587c3a5","570fd8a3cd3490514781265c4fe3deaf05b70dfbeae9d9e86c72f35c07b7e7c8","c1d486923747a042bb9b4ad53e672ba92335fb8cb465f4f57a28f7b0e7fd24ee","42ea4ffbedd00ad81689a2171185ce77a7ac2d8b01ebede5c7bfb91e99deefdb"}};
constexpr std::array<std::string_view,84> exact_key_hex{{
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000000000000",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000000010200",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001000000",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001010200",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff360100000000000000000001080000000000000000",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff360100000000000000000101080000000000000000",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001000108ffffffffffffffff",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001010108ffffffffffffffff",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff360100000000000000000001080000000000000001",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff360100000000000000000101080000000000000001",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001000108fffffffffffffffe",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001010108fffffffffffffffe",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000000000108000000000000000a",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000000010108000000000000000a",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001000108fffffffffffffff5",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001010108fffffffffffffff5",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff360100000000000000000001080000000000000064",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff360100000000000000000101080000000000000064",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001000108ffffffffffffff9b",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001010108ffffffffffffff9b",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff3601000000000000000000010800000000000003e8",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff3601000000000000000001010800000000000003e8",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001000108fffffffffffffc17",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001010108fffffffffffffc17",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff360100000000000000000001080000000000002710",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff360100000000000000000101080000000000002710",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001000108ffffffffffffd8ef",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001010108ffffffffffffd8ef",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff3601000000000000000000010800000000000186a0",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff3601000000000000000001010800000000000186a0",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001000108fffffffffffe795f",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001010108fffffffffffe795f",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff3601000000000000000000010800000000000f4240",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff3601000000000000000001010800000000000f4240",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001000108fffffffffff0bdbf",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001010108fffffffffff0bdbf",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff360100000000000000000001080000000000989680",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff360100000000000000000101080000000000989680",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001000108ffffffffff67697f",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001010108ffffffffff67697f",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff360100000000000000000001080000000005f5e100",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff360100000000000000000101080000000005f5e100",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001000108fffffffffa0a1eff",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001010108fffffffffa0a1eff",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000000000108000000003b9aca00",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000000010108000000003b9aca00",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001000108ffffffffc46535ff",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001010108ffffffffc46535ff",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff360100000000000000000001080000000df8475800",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff360100000000000000000101080000000df8475800",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001000108fffffff207b8a7ff",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001010108fffffff207b8a7ff",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff360100000000000000000001080000034630b8a000",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff360100000000000000000101080000034630b8a000",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001000108fffffcb9cf475fff",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001010108fffffcb9cf475fff",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000000000108000000003b9ac9ff",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000000010108000000003b9ac9ff",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001000108ffffffffc4653600",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001010108ffffffffc4653600",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff360100000000000000000001080000000df84757ff",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff360100000000000000000101080000000df84757ff",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001000108fffffff207b8a800",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001010108fffffff207b8a800",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff360100000000000000000001080000034630b89fff",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff360100000000000000000101080000034630b89fff",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001000108fffffcb9cf476000",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001010108fffffcb9cf476000",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff360100000000000000000001080000274a48a78000",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff360100000000000000000101080000274a48a78000",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001000108ffffd8b5b7587fff",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001010108ffffd8b5b7587fff",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff360100000000000000000001080000293251f34100",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff360100000000000000000101080000293251f34100",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001000108ffffd6cdae0cbeff",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001010108ffffd6cdae0cbeff",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff3601000000000000000000010800004e9455b43600",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff3601000000000000000001010800004e9455b43600",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001000108ffffb16baa4bc9ff",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001010108ffffb16baa4bc9ff",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff3601000000000000000000010800004e94914effff",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff3601000000000000000001010800004e94914effff",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001000108ffffb16b6eb10000",
"534254494d4b3031019d000000007000800000000000d7100a000000000000000a0000000000000075deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d601a1032b9f517229977b45730f3dff36010000000000000001010108ffffb16b6eb10000"
}};
void HashComparison(const std::shared_ptr<const dt::TimeValidatedProfileHandleV3>&p0){std::array<dt::TimeOwnedValueV3,21>c{};
c[0]={p0,dt::TimeValueStateV3::sql_null,0};
for(size_t i=0;
i<20;
++i)c[i+1]={p0,dt::TimeValueStateV3::value,values[i]};
for(size_t i=0;
i<21;
++i){auto g=dt::HashTimeValueV3(c[i]);
auto e=Hex32(hashes[i]);
Check(g.ok()&&g.bytes.size()==32&&std::equal(g.bytes.begin(),g.bytes.end(),e.begin()),"21 exact SBTIMH01 hashes");
}Check(dt::CompareTimeValuesV3(c[1].view(),c[2].view()).fact==dt::TimeComparisonFactV3::less,"less");
Check(dt::CompareTimeValuesV3(c[20].view(),c[1].view()).fact==dt::TimeComparisonFactV3::greater,"greater");
auto n=dt::CompareTimeValuesV3(c[0].view(),c[0].view());
Check(n.ok()&&n.fact==dt::TimeComparisonFactV3::unordered_null&&n.grouping_equivalent&&n.null_equivalent,"NULL comparison");
dt::TimeValueViewV3 poison{p0.get(),dt::TimeValueStateV3::value,~p::u64{0}};
Check(dt::CompareTimeValuesV3(c[0].view(),poison).fact==dt::TimeComparisonFactV3::unordered_null,"null-present poison no payload read");
Check(dt::CompareTimeValuesV3(poison,c[0].view()).fact==dt::TimeComparisonFactV3::unordered_null,"present-null poison no payload read");
Check(dt::DifferenceTimeNanosecondsV3(c[0].view(),poison).is_null&&dt::DifferenceTimeNanosecondsV3(poison,c[0].view()).is_null,"difference NULL poison precedence");
for(std::size_t i=0;i<c.size();++i)for(std::size_t j=0;j<c.size();++j){
auto relation=dt::CompareTimeValuesV3(c[i].view(),c[j].view());Check(relation.ok(),"21x21 comparison accepted");
if(i==0||j==0)Check(relation.fact==dt::TimeComparisonFactV3::unordered_null,"21x21 NULL unordered");
else{const auto expected=c[i].nanoseconds_since_midnight<c[j].nanoseconds_since_midnight?dt::TimeComparisonFactV3::less:c[i].nanoseconds_since_midnight>c[j].nanoseconds_since_midnight?dt::TimeComparisonFactV3::greater:dt::TimeComparisonFactV3::equal;Check(relation.fact==expected,"21x21 exact unsigned relation");}
}
auto bad=*p0;
bad.comparison_fingerprint[0]^=1;
Check(!dt::CompareTimeValuesWithValidatedCohortForConformanceV3(c[1].view(),{&bad,dt::TimeValueStateV3::value,1},bad.comparison_fingerprint).ok(),"cohort mismatch");
dt::TimeValueViewV3 dirty_left{p0.get(),dt::TimeValueStateV3::sql_null,1},bad_right{&bad,dt::TimeValueStateV3::value,~p::u64{0}};
Check(dt::CompareTimeValuesV3(dirty_left,bad_right).diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","comparison completes right authority before left state");
Check(dt::DifferenceTimeNanosecondsV3(dirty_left,bad_right).diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","difference completes right authority before left state");
}
void Keys(const std::shared_ptr<const dt::TimeValidatedProfileHandleV3>&p0){std::array<dt::TimeOwnedValueV3,21>c{};
c[0]={p0,dt::TimeValueStateV3::sql_null,0};
for(size_t i=0;
i<20;
++i)c[i+1]={p0,dt::TimeValueStateV3::value,values[i]};
unsigned count=0;
for(auto d:{dt::TimeSortDirectionV3::ascending,dt::TimeSortDirectionV3::descending})for(auto m:{dt::TimeNullModeV3::nulls_first,dt::TimeNullModeV3::nulls_last})for(const auto&v:c){auto k=dt::MakeTimeSortKeyV3(v,d,m);
Check(k.ok()&&k.bytes.size()==(v.state==dt::TimeValueStateV3::sql_null?100:108),"key extent");
Check(std::memcmp(k.bytes.data(),"SBTIMK01",8)==0&&std::memcmp(k.bytes.data()+8,D710().bytes.data(),16)==0&&p::LoadLittle64(k.bytes.data()+24)==10&&p::LoadLittle64(k.bytes.data()+32)==10,"key authority header");
Check(std::memcmp(k.bytes.data()+40,p0->comparison_fingerprint.data(),32)==0,"key fingerprint");
Check(k.bytes[96]==(d==dt::TimeSortDirectionV3::descending)&&k.bytes[97]==(m==dt::TimeNullModeV3::nulls_last),"key mode");
if(v.state==dt::TimeValueStateV3::value)for(unsigned j=0;
j<8;
++j){auto e=p::byte(v.nanoseconds_since_midnight>>(56-8*j));
if(d==dt::TimeSortDirectionV3::descending)e=p::byte(~e);
Check(k.bytes[100+j]==e,"BE8 key suffix");
}else Check(k.bytes[98]==(m==dt::TimeNullModeV3::nulls_first?0:2)&&k.bytes[99]==0,"NULL key");
auto x=dt::DecodeTimeSortKeyNoAllocV3(*p0,k.bytes);
Check(x.ok()&&x.value.state==v.state&&x.value.nanoseconds_since_midnight==v.nanoseconds_since_midnight,"key decode");
++count;
}Check(count==84,"84 key rows");
for(std::size_t i=0;i<c.size();++i)for(unsigned mode=0;mode<4;++mode){
auto d=mode<2?dt::TimeSortDirectionV3::ascending:dt::TimeSortDirectionV3::descending;
auto m=(mode%2)==0?dt::TimeNullModeV3::nulls_first:dt::TimeNullModeV3::nulls_last;
auto key=dt::MakeTimeSortKeyV3(c[i],d,m);Check(key.ok()&&ExactHex(key.bytes,exact_key_hex[i*4+mode]),"exact Core ordered-key bytes");
}
for(auto d:{dt::TimeSortDirectionV3::ascending,dt::TimeSortDirectionV3::descending})for(auto m:{dt::TimeNullModeV3::nulls_first,dt::TimeNullModeV3::nulls_last}){
std::array<std::vector<p::byte>,21> keys{};for(std::size_t i=0;i<c.size();++i)keys[i]=dt::MakeTimeSortKeyV3(c[i],d,m).bytes;
for(std::size_t i=0;i<c.size();++i)for(std::size_t j=0;j<c.size();++j){
int expected=0;if(i==0||j==0){if(i!=j)expected=(i==0)==(m==dt::TimeNullModeV3::nulls_first)?-1:1;}else if(c[i].nanoseconds_since_midnight!=c[j].nanoseconds_since_midnight){expected=c[i].nanoseconds_since_midnight<c[j].nanoseconds_since_midnight?-1:1;if(d==dt::TimeSortDirectionV3::descending)expected=-expected;}
const int actual=keys[i]<keys[j]?-1:keys[j]<keys[i]?1:0;Check(actual==expected,"441 exact key relations per mode");
}}
auto ordered=values;
std::sort(ordered.begin(),ordered.end());
for(size_t i=1;
i<20;
++i){dt::TimeOwnedValueV3 a{p0,dt::TimeValueStateV3::value,ordered[i-1]},b{p0,dt::TimeValueStateV3::value,ordered[i]};
auto x=dt::MakeTimeSortKeyV3(a,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first),y=dt::MakeTimeSortKeyV3(b,dt::TimeSortDirectionV3::ascending,dt::TimeNullModeV3::nulls_first);
Check(std::lexicographical_compare(x.bytes.begin()+100,x.bytes.end(),y.bytes.begin()+100,y.bytes.end()),"ascending suffix order");
}
auto bad_profile=std::make_shared<dt::TimeValidatedProfileHandleV3>(*p0);bad_profile->profile_material[0]^=1;
dt::TimeOwnedValueV3 bad_value{bad_profile,dt::TimeValueStateV3::value,0};
Check(dt::MakeTimeSortKeyV3(bad_value,static_cast<dt::TimeSortDirectionV3>(9),static_cast<dt::TimeNullModeV3>(9)).diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","key profile precedes invalid modes");
}
void Casts(const std::shared_ptr<const dt::TimeValidatedProfileHandleV3>&p0){auto*ci=Identity(dt::CanonicalTypeId::character);
Check(ci,"character identity");
auto cd=Descriptor(dt::CanonicalTypeId::character);
auto td=Descriptor(dt::CanonicalTypeId::time);
dt::DatatypeOperationValue text{dt::CanonicalTypeId::character,"12:34:56.1",false,cd},nullv{dt::CanonicalTypeId::null_type,"",true,{}};
dt::TimeOwnedValueV3 time{p0,dt::TimeValueStateV3::value,45'296'100'000'000ull};
unsigned decisions=0,admitted=0;
for(unsigned row=1;
row<=221;
++row)for(auto c:{dt::DatatypeCastContext::implicit,dt::DatatypeCastContext::assignment,dt::DatatypeCastContext::explicit_cast}){auto want=dt::TimeCastPolicyDispositionV3::forbidden;
if(row==1)want=dt::TimeCastPolicyDispositionV3::contextual_null;
else if(row==24&&c==dt::DatatypeCastContext::explicit_cast)want=dt::TimeCastPolicyDispositionV3::explicit_character_to_time;
else if(row==50&&c==dt::DatatypeCastContext::explicit_cast)want=dt::TimeCastPolicyDispositionV3::explicit_time_to_character;
else if(row==53)want=dt::TimeCastPolicyDispositionV3::identity;
Check(dt::ClassifyTimeCastPolicyRowV3(row,c)==want,"221x3 classify");
dt::TimeCastRequestV3 q;
q.one_based_policy_row=row;
q.context=c;
if(row==1){q.scalar_source=&nullv;
q.time_target=&p0;
q.time_target_descriptor=&td;
}else if(row==24){q.scalar_source=&text;
q.scalar_source_identity=ci;
q.time_target=&p0;
q.time_target_descriptor=&td;
}else if(row==50){q.time_source=&time;
q.scalar_target=dt::CanonicalTypeId::character;
q.scalar_target_identity=ci;
q.scalar_target_descriptor=cd;
}else if(row==53){q.time_source=&time;
q.time_target=&p0;
q.time_target_descriptor=&td;
}auto r=dt::CastTimeValueV3(q);
++decisions;
if(want==dt::TimeCastPolicyDispositionV3::forbidden)Check(!r.ok()&&r.diagnostic.diagnostic_code=="DATATYPE.CAST_FORBIDDEN","runtime forbidden");
else{Check(r.ok(),"runtime admitted");
++admitted;
}}Check(decisions==663&&admitted==8,"663 decisions eight admitted");
dt::TimeCastRequestV3 out;
out.one_based_policy_row=50;
out.context=dt::DatatypeCastContext::explicit_cast;
out.time_source=&time;
out.scalar_target=dt::CanonicalTypeId::character;
out.scalar_target_identity=ci;
out.scalar_target_descriptor=cd;
auto own=dt::CastTimeValueV3(out);
Check(own.ok()&&own.scalar_value.encoded_value=="12:34:56.1","owned character cast");
std::array<char,18>b{};
b.fill('x');
out.use_character_output_buffer=true;
out.character_output=b.data();
out.character_output_capacity=b.size();
auto exact=dt::CastTimeValueV3(out);
Check(exact.ok()&&exact.bytes_written==10&&std::string_view(b.data(),10)=="12:34:56.1","buffer character cast");
b.fill('x');
out.character_output_capacity=9;
auto shortv=dt::CastTimeValueV3(out);
Check(!shortv.ok()&&shortv.bytes_written==0&&std::all_of(b.begin(),b.end(),[](char x){return x=='x';
}),"short cast unchanged");
b.fill('x');
out.character_output_capacity=b.size();
out.control={~p::u64{0},Stop,nullptr};
auto cancel=dt::CastTimeValueV3(out);
Check(!cancel.ok()&&cancel.diagnostic.diagnostic_code=="PROCESS.CANCELLED"&&std::all_of(b.begin(),b.end(),[](char x){return x=='x';
}),"cancel cast unchanged");
text.encoded_value="23:59:59.999999999";
dt::TimeCastRequestV3 in;
in.one_based_policy_row=24;
in.context=dt::DatatypeCastContext::explicit_cast;
in.scalar_source=&text;
in.scalar_source_identity=ci;
in.time_target=&p0;
in.time_target_descriptor=&td;
Check(dt::CastTimeValueV3(in).time_value.nanoseconds_since_midnight==dt::kTimeMaximumNanosecondsV3,"character maximum");
text.encoded_value="23:59:60";
Check(dt::CastTimeValueV3(in).diagnostic.diagnostic_code=="CTI.TEMPORAL.LEAP_SECOND_REFUSED","leap cast");
auto nonnull=cd;
nonnull.nullable_allowed=false;
text.encoded_value="00:00:00";
text.descriptor=nonnull;
Check(dt::CastTimeValueV3(in).ok(),"PRESENT nonnullable character accepted");
for(unsigned field=0;
field<21;
++field){auto bad=cd;
switch(field){case 0:bad.descriptor_uuid.bytes[0]^=1;
break;
case 1:++bad.descriptor_epoch;
break;
case 2:++bad.canonical_type_id;
break;
case 3:bad.family=scratchbird::engine::ExecutionTypeFamily::binary;
break;
case 4:bad.width_class=scratchbird::engine::ExecutionTypeWidthClass::fixed;
break;
case 5:++bad.bit_width;
break;
case 6:++bad.precision;
break;
case 7:++bad.scale;
break;
case 8:++bad.length;
break;
case 9:++bad.vector_dimensions;
break;
case 10:++bad.container_rank;
break;
case 11:bad.modifier_flags^=1ull<<63;
break;
case 12:bad.domain_uuid.bytes[0]=1;
break;
case 13:bad.domain_stack.push_back(bad.descriptor_uuid);
break;
case 14:bad.charset_uuid.bytes[0]=1;
break;
case 15:bad.collation_uuid.bytes[0]=1;
break;
case 16:bad.timezone_uuid.bytes[0]=1;
break;
case 17:bad.element_descriptor_uuid.bytes[0]=1;
break;
case 18:bad.security_policy_uuid.bytes[0]=1;
break;
case 19:bad.descriptor_authoritative=false;
break;
case 20:bad.parser_independent=false;
break;
}text.descriptor=bad;
Check(!dt::CastTimeValueV3(in).ok(),"character descriptor mutation");
}auto alias=cd;
alias.stable_name="CHAR localized alias";
text.descriptor=alias;
Check(dt::CastTimeValueV3(in).ok(),"character descriptor label is presentation only");
text.descriptor=cd;
dt::TimeCastRequestV3 identity;
identity.one_based_policy_row=53;identity.context=dt::DatatypeCastContext::explicit_cast;
identity.time_source=&time;identity.time_target=&p0;identity.time_target_descriptor=&td;
Check(dt::CastTimeValueV3(identity).ok(),"identity exact target descriptor");
identity.control={~p::u64{0},Stop,nullptr};auto identity_cancelled=dt::CastTimeValueV3(identity);Check(!identity_cancelled.ok()&&identity_cancelled.diagnostic.diagnostic_code=="PROCESS.CANCELLED"&&!identity_cancelled.produced_time&&!identity_cancelled.time_value.profile,"identity cancellation before publication");identity.control={};
identity.time_target_descriptor=nullptr;Check(dt::CastTimeValueV3(identity).diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","identity requires target descriptor");identity.time_target_descriptor=&td;
auto nonnullable_time=td;nonnullable_time.nullable_allowed=false;identity.time_target_descriptor=&nonnullable_time;
Check(dt::CastTimeValueV3(identity).diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","target descriptor nullability matches request");identity.target_null_allowed=false;Check(dt::CastTimeValueV3(identity).ok(),"PRESENT time target accepts exact nonnullable descriptor");identity.target_null_allowed=true;identity.time_target_descriptor=&td;
for(unsigned field=0;field<21;++field){auto bad=td;switch(field){case 0:bad.descriptor_uuid.bytes[0]^=1;break;case 1:++bad.descriptor_epoch;break;case 2:++bad.canonical_type_id;break;case 3:bad.family=scratchbird::engine::ExecutionTypeFamily::character;break;case 4:bad.width_class=scratchbird::engine::ExecutionTypeWidthClass::variable;break;case 5:++bad.bit_width;break;case 6:++bad.precision;break;case 7:++bad.scale;break;case 8:++bad.length;break;case 9:++bad.vector_dimensions;break;case 10:++bad.container_rank;break;case 11:bad.modifier_flags=1;break;case 12:bad.domain_uuid.bytes[0]=1;break;case 13:bad.domain_stack.push_back(bad.descriptor_uuid);break;case 14:bad.charset_uuid.bytes[0]=1;break;case 15:bad.collation_uuid.bytes[0]=1;break;case 16:bad.timezone_uuid.bytes[0]=1;break;case 17:bad.element_descriptor_uuid.bytes[0]=1;break;case 18:bad.security_policy_uuid.bytes[0]=1;break;case 19:bad.descriptor_authoritative=false;break;case 20:bad.parser_independent=false;break;}identity.time_target_descriptor=&bad;Check(dt::CastTimeValueV3(identity).diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","time target descriptor authority mutation");}
identity.time_target_descriptor=&td;
auto poisoned=time;poisoned.nanoseconds_since_midnight=~p::u64{0};identity.time_source=&poisoned;auto invalid_target_mutable=std::make_shared<dt::TimeValidatedProfileHandleV3>(*p0);invalid_target_mutable->profile_material[0]^=1;std::shared_ptr<const dt::TimeValidatedProfileHandleV3> invalid_target=invalid_target_mutable;identity.time_target=&invalid_target;
Check(dt::CastTimeValueV3(identity).diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","cast target authority precedes source payload");
identity.time_source=&time;identity.time_target=&p0;
out=dt::TimeCastRequestV3{};out.one_based_policy_row=50;out.context=dt::DatatypeCastContext::explicit_cast;out.time_source=&time;out.scalar_target=dt::CanonicalTypeId::character;out.scalar_target_identity=ci;out.scalar_target_descriptor=nonnull;out.target_null_allowed=false;
Check(dt::CastTimeValueV3(out).ok(),"PRESENT time casts to nonnullable character target");out.control.maximum_allocation_bytes=9;Check(dt::CastTimeValueV3(out).diagnostic.diagnostic_code=="RESOURCE.BUDGET_EXCEEDED","character owning allocation budget");
dt::TimeCastRequestV3 n;
n.one_based_policy_row=1;
n.scalar_source=&nullv;
n.time_target=&p0;
n.time_target_descriptor=&td;
n.control={~p::u64{0},Stop,nullptr};auto contextual_cancelled=dt::CastTimeValueV3(n);Check(!contextual_cancelled.ok()&&contextual_cancelled.diagnostic.diagnostic_code=="PROCESS.CANCELLED"&&!contextual_cancelled.produced_time&&!contextual_cancelled.time_value.profile,"contextual NULL cancellation before publication");n.control={};
n.target_null_allowed=false;auto contextual_not_admitted=dt::CastTimeValueV3(n);Check(!contextual_not_admitted.ok()&&contextual_not_admitted.diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID"&&!contextual_not_admitted.produced_time,"contextual NULL requires target descriptor nullability to match request");n.time_target_descriptor=&td;n.target_null_allowed=true;
auto nonnullable_null_target=td;nonnullable_null_target.nullable_allowed=false;n.time_target_descriptor=&nonnullable_null_target;n.target_null_allowed=false;auto contextual_null_refused=dt::CastTimeValueV3(n);Check(!contextual_null_refused.ok()&&contextual_null_refused.diagnostic.diagnostic_code=="DATATYPE.NULL_NOT_ADMITTED"&&!contextual_null_refused.produced_time,"contextual NULL refused by nonnullable target");n.time_target_descriptor=&td;n.target_null_allowed=true;
nullv.encoded_value="dirty";
Check(dt::CastTimeValueV3(n).diagnostic.diagnostic_code=="DATATYPE.NULL_STATE.INVALID","dirty contextual NULL");
n.time_target=&invalid_target;Check(dt::CastTimeValueV3(n).diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","contextual target authority precedes dirty NULL");
}
}
int main(){auto p0=Profile();
HashComparison(p0);
Keys(p0);
Casts(p0);
std::cout<<"PASS base.time V3 operations/casts checks="<<checks<<" hashes=21 keys=84 decisions=663\n";
}
