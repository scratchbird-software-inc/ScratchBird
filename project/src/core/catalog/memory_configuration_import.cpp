// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_memory_configuration.hpp"
#include "../datatypes/structured_value_codec.hpp"
#include "uuid.hpp"
#include <array>
#include <charconv>
#include <cstring>
#include <limits>

namespace scratchbird::core::catalog {
namespace {
using E=CatalogValueError;
constexpr std::size_t maximum_import_bytes=16384;
void Skip(std::string_view s,std::size_t& at){while(at<s.size()&&(s[at]==' '||s[at]=='\t'||s[at]=='\r'||s[at]=='\n'))++at;}
// Input has already passed complete UTF-8/JSON syntax validation. This scanner
// only locates exact members; it cannot accept malformed skipped values.
std::size_t End(std::string_view s,std::size_t at) {
  bool quoted=false,escaped=false;unsigned depth=0;
  for(std::size_t n=at;n<s.size();++n) {
    const char c=s[n];
    if(quoted){if(escaped)escaped=false;else if(c=='\\')escaped=true;else if(c=='"'){quoted=false;if(!depth)return n+1;}continue;}
    if(c=='"')quoted=true;
    else if(c=='{'||c=='[')++depth;
    else if(c=='}'||c==']'){if(!depth)return n;if(!--depth)return n+1;}
    else if(c==','&&!depth)return n;
  }
  return s.size();
}
bool String(std::string_view s,std::span<char> out,std::size_t& used) {
  used=0;
  if(s.size()<2||s.front()!='"'||s.back()!='"')return false;
  const auto put=[&](char value){if(used==out.size())return false;out[used++]=value;return true;};
  const auto hex=[](char c)->unsigned{return c<='9'?c-'0':c<='F'?c-'A'+10:c-'a'+10;};
  for(std::size_t at=1;at+1<s.size();++at) {
    char c=s[at];
    if(c!='\\'){if(!put(c))return false;continue;}
    c=s[++at];
    if(c!='u') {
      switch(c){case 'b':c='\b';break;case 'f':c='\f';break;case 'n':c='\n';break;case 'r':c='\r';break;case 't':c='\t';break;}
      if(!put(c))return false;continue;
    }
    unsigned scalar=0;for(unsigned n=0;n<4;++n)scalar=(scalar<<4)|hex(s[++at]);
    if(scalar>=0xd800&&scalar<=0xdbff) {
      at+=2;unsigned low=0;for(unsigned n=0;n<4;++n)low=(low<<4)|hex(s[++at]);
      scalar=0x10000+((scalar-0xd800)<<10)+(low-0xdc00);
    }
    if(scalar<0x80){if(!put(static_cast<char>(scalar)))return false;}
    else if(scalar<0x800){if(!put(0xc0|(scalar>>6))||!put(0x80|(scalar&63)))return false;}
    else if(scalar<0x10000){if(!put(0xe0|(scalar>>12))||!put(0x80|((scalar>>6)&63))||!put(0x80|(scalar&63)))return false;}
    else if(!put(0xf0|(scalar>>18))||!put(0x80|((scalar>>12)&63))||!put(0x80|((scalar>>6)&63))||!put(0x80|(scalar&63)))return false;
  }
  return true;
}
template<std::size_t N> E Object(std::string_view json,const std::array<std::string_view,N>& names,
                               std::array<std::string_view,N>& values,u64 required=(u64{1}<<N)-1) {
  std::size_t at=0;Skip(json,at);if(at==json.size()||json[at++]!='{')return E::type_mismatch;
  u64 seen=0;
  while(true) {
    Skip(json,at);if(at==json.size())return E::invalid_framing;
    if(json[at]=='}') {++at;Skip(json,at);return at==json.size()&&(seen&required)==required?E::none:E::missing_field;}
    const auto key_end=End(json,at);std::array<char,64> key;std::size_t size=0;
    if(!String(json.substr(at,key_end-at),key,size))return E::unknown_field;
    std::size_t index=0;while(index<N&&names[index]!=std::string_view(key.data(),size))++index;
    if(index==N||seen&(u64{1}<<index))return E::unknown_field;
    at=key_end;Skip(json,at);if(at==json.size()||json[at++]!=':')return E::invalid_framing;
    Skip(json,at);const auto end=End(json,at);auto value=json.substr(at,end-at);
    while(!value.empty()&&(value.back()==' '||value.back()=='\t'||value.back()=='\r'||value.back()=='\n'))value.remove_suffix(1);
    values[index]=value;seen|=u64{1}<<index;at=end;Skip(json,at);
    if(at<json.size()&&json[at]==','){++at;continue;}
    if(at==json.size()||json[at]!='}')return E::invalid_framing;
  }
}
bool Number(std::string_view s,u64& value){const auto r=std::from_chars(s.data(),s.data()+s.size(),value);return !s.empty()&&r.ec==std::errc{}&&r.ptr==s.data()+s.size();}
bool Boolean(std::string_view s,bool& value){if(s=="true"){value=true;return true;}if(s=="false"){value=false;return true;}return false;}
bool Label(std::string_view encoded,std::string_view expected){std::array<char,64> text;std::size_t size=0;return String(encoded,text,size)&&std::string_view(text.data(),size)==expected;}
} // namespace
MemoryConfigurationImportResult ImportMemoryConfigurationInto(std::string_view json,std::span<char> backing) {
  if(json.size()>maximum_import_bytes)return {E::size_limit,{}};
  if(!datatypes::ValidateJsonValue(json))return {E::invalid_framing,{}};
  constexpr std::array<std::string_view,10> root_names{"schema_version","policy_generation","policy_key","profile_area","profile_mode","policy_name","provenance","limits","allocation","adaptive_cache"};
  constexpr std::array<std::string_view,6> limit_names{"hard_limit_bytes","soft_limit_bytes","per_context_limit_bytes","page_buffer_pool_limit_bytes","openssl_budget_bytes","min_startup_available_bytes"};
  constexpr std::array<std::string_view,7> allocation_names{"failure_mode","track_allocations","zero_memory_on_allocate","zero_memory_on_release","reject_over_soft_limit","enable_platform_memory_probe","require_platform_memory_ceiling"};
  constexpr std::array<std::string_view,9> cache_names{"enabled","page_buffer_growth","index_read_optimization","ordinary_disconnect_heap_trim","dirty_writeback_required","write_flush_authority","cache_finality_authority","cache_visibility_authority","wal_or_redo_authority"};
  std::array<std::string_view,10> root;std::array<std::string_view,6> limits;
  std::array<std::string_view,7> allocation;std::array<std::string_view,9> cache;
  E error=Object(json,root_names,root);if(error!=E::none)return {error,{}};
  if((error=Object(root[7],limit_names,limits))!=E::none||
      (error=Object(root[8],allocation_names,allocation))!=E::none||
      (error=Object(root[9],cache_names,cache))!=E::none)return {error,{}};
  CatalogMemoryConfiguration r;
  if(!Number(root[0],r.imported_schema_version)||!Number(root[1],r.generation)||
      !Number(limits[0],r.hard_limit_bytes)||!Number(limits[1],r.soft_limit_bytes)||
      !Number(limits[2],r.per_context_limit_bytes)||!Number(limits[3],r.page_buffer_pool_limit_bytes)||
      !Number(limits[4],r.openssl_budget_bytes)||!Number(limits[5],r.min_startup_available_bytes)||
      !Boolean(allocation[1],r.track_allocations)||!Boolean(allocation[2],r.zero_memory_on_allocate)||
      !Boolean(allocation[3],r.zero_memory_on_release)||!Boolean(allocation[4],r.reject_over_soft_limit)||
      !Boolean(allocation[5],r.enable_platform_memory_probe)||!Boolean(allocation[6],r.require_platform_memory_ceiling)||
      !Boolean(cache[0],r.adaptive_cache_enabled)||!Boolean(cache[2],r.index_read_optimization)||
      !Boolean(cache[3],r.ordinary_disconnect_heap_trim)||!Boolean(cache[4],r.dirty_writeback_required)||
      !Boolean(cache[6],r.cache_finality_authority)||!Boolean(cache[7],r.cache_visibility_authority)||!Boolean(cache[8],r.wal_or_redo_authority))return {E::type_mismatch,{}};
  if(r.imported_schema_version!=1)return {E::unsupported_version,{}};
  for(const auto field: {root[3],root[4],allocation[0],cache[1],cache[5]})
    if(field.size()<2||field.front()!='"')return {E::type_mismatch,{}};
  if(Label(allocation[0],"return_error"))r.failure_mode=MemoryConfigurationFailureMode::return_error;
  else if(Label(allocation[0],"fatal_status"))r.failure_mode=MemoryConfigurationFailureMode::fatal_status;
  if(Label(cache[1],"adaptive_with_policy_limit"))r.page_buffer_growth=MemoryConfigurationGrowth::adaptive_with_policy_limit;
  if(Label(cache[5],"mga_inventory_and_page_writeback"))r.write_flush_authority=MemoryConfigurationWriteAuthority::mga_inventory_and_page_writeback;
  std::array<char,768> staged;std::array<std::size_t,3> lengths{};std::size_t used=0;
  const std::array text{root[2],root[5],root[6]};
  for(std::size_t n=0;n<3;++n){
    if(text[n].size()<2||text[n].front()!='"')return {E::type_mismatch,{}};
    if(!String(text[n],std::span<char>(staged).subspan(used,256),lengths[n]))return {E::size_limit,{}};used+=lengths[n];}
  r.policy_key={staged.data(),lengths[0]};r.policy_name={staged.data()+lengths[0],lengths[1]};r.provenance={staged.data()+lengths[0]+lengths[1],lengths[2]};
  if(ValidateCatalogMemoryConfigurationSettings(r)!=E::none||
      !Label(root[3],"memory_resource_governance")||!Label(root[4],"configured_policy_required")) {
    MemoryConfigurationImportDiagnostic context;
    std::memcpy(context.labels.data(),staged.data(),used);context.lengths=lengths;
    return {E::invalid_value,{},context};
  }
  if(backing.size()<used)return {E::invalid_backing,{}};
  const auto input=reinterpret_cast<std::uintptr_t>(json.data()),output=reinterpret_cast<std::uintptr_t>(backing.data());
  if(input>std::numeric_limits<std::uintptr_t>::max()-json.size()||output>std::numeric_limits<std::uintptr_t>::max()-used||
      (input<output+used&&output<input+json.size()))return {E::invalid_backing,{}};
  std::memcpy(backing.data(),staged.data(),used);
  r.policy_key={backing.data(),lengths[0]};r.policy_name={backing.data()+lengths[0],lengths[1]};r.provenance={backing.data()+lengths[0]+lengths[1],lengths[2]};
  return {E::none,r};
}
BootstrapPolicyProfileImportResult ImportBootstrapPolicyProfileInto(std::string_view json,std::span<char> backing) {
  if(json.size()>maximum_import_bytes)return {E::size_limit,{}};
  if(!datatypes::ValidateJsonValue(json))return {E::invalid_framing,{}};
  constexpr std::array<std::string_view,4> names{"profile_uuid","area","mode","resource_file"};
  std::array<std::string_view,4> values{};
  const auto error=Object(json,names,values,7);if(error!=E::none)return {error,{}};
  std::array<char,36> identity;std::size_t identity_size=0;
  if(!String(values[0],identity,identity_size)||identity_size!=36)return {E::invalid_value,{}};
  for(const auto at: {8,13,18,23})if(identity[at]!='-')return {E::invalid_value,{}};
  BootstrapPolicyProfileImport r;std::size_t at=0;
  for(auto& byte:r.profile_uuid.bytes) {
    if(at==8||at==13||at==18||at==23)++at;
    unsigned number=0;const auto parsed=std::from_chars(identity.data()+at,identity.data()+at+2,number,16);
    if(parsed.ec!=std::errc{}||parsed.ptr!=identity.data()+at+2)return {E::invalid_value,{}};
    byte=static_cast<platform::byte>(number);at+=2;
  }
  if(!uuid::IsEngineIdentityUuid(r.profile_uuid))return {E::invalid_value,{}};
  std::array<char,768> staged;std::array<std::size_t,3> lengths{};std::size_t used=0;
  for(std::size_t n=0;n<3;++n) {
    if(n==2&&values[3].empty())continue;
    if(values[n+1].size()<2||values[n+1].front()!='"')return {E::type_mismatch,{}};
    if(!String(values[n+1],std::span<char>(staged).subspan(used,256),lengths[n]))return {E::size_limit,{}};
    if(!lengths[n]||std::string_view(staged.data()+used,lengths[n]).find('\0')!=std::string_view::npos)return {E::invalid_value,{}};
    used+=lengths[n];
  }
  if(backing.size()<used)return {E::invalid_backing,{}};
  const auto input=reinterpret_cast<std::uintptr_t>(json.data()),output=reinterpret_cast<std::uintptr_t>(backing.data());
  if(input>std::numeric_limits<std::uintptr_t>::max()-json.size()||output>std::numeric_limits<std::uintptr_t>::max()-used||
      (input<output+used&&output<input+json.size()))return {E::invalid_backing,{}};
  std::memcpy(backing.data(),staged.data(),used);
  r.area={backing.data(),lengths[0]};r.mode={backing.data()+lengths[0],lengths[1]};
  if(!values[3].empty())r.resource_file=std::string_view(backing.data()+lengths[0]+lengths[1],lengths[2]);
  return {E::none,r};
}
} // namespace scratchbird::core::catalog
