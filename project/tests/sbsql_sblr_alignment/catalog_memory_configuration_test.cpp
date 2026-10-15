// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "memory_configuration_fixture.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>

thread_local bool heap_denied=false;
void* operator new(std::size_t n){if(heap_denied)throw std::bad_alloc();if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
namespace c=scratchbird::core::catalog;namespace p=scratchbird::core::platform;
namespace f=scratchbird::tests::bootstrap_policy;namespace m=scratchbird::tests::memory_configuration;
namespace {
unsigned checks=0;
void Check(bool valid,const char* why){++checks;if(!valid){heap_denied=false;throw std::runtime_error(why);}}
void Replace(std::string& s,std::string_view from,std::string_view to){const auto at=s.find(from);Check(at!=s.npos,"actual fixture replacement must exist");s.replace(at,from.size(),to);}
void Roundtrip(const c::CatalogMemoryConfiguration& r) {
  const auto golden=m::Golden(r);std::array<p::byte,2048> backing;backing.fill(0x5a);
  heap_denied=true;const auto encoded=c::EncodeCatalogMemoryConfigurationInto(r,backing);
  const auto decoded=c::DecodeCatalogMemoryConfiguration(golden);heap_denied=false;
  Check(encoded.ok()&&decoded.ok()&&std::string_view(reinterpret_cast<const char*>(encoded.bytes.data()),encoded.bytes.size())==golden&&
      m::Golden(*decoded.record)==golden,"independent native bytes and no-heap roundtrip");
  for(std::size_t n=0;n<golden.size();++n) {
    backing.fill(0x5a);heap_denied=true;
    const auto short_out=c::EncodeCatalogMemoryConfigurationInto(r,std::span<p::byte>(backing).first(n));
    const auto short_in=c::DecodeCatalogMemoryConfiguration(std::string_view(golden).substr(0,n));heap_denied=false;
    Check(!short_out.ok()&&short_out.bytes.empty()&&!short_in.ok()&&!short_in.record&&
        std::all_of(backing.begin(),backing.end(),[](auto v){return v==0x5a;}),"every truncated frame/backing refuses without writes or partial record");
  }
}
void Refuse(const c::CatalogMemoryConfiguration& r) {
  const auto golden=m::Golden(r);std::array<p::byte,2048> backing;backing.fill(0x5a);
  heap_denied=true;const auto encoded=c::EncodeCatalogMemoryConfigurationInto(r,backing);
  const auto decoded=c::DecodeCatalogMemoryConfiguration(golden);heap_denied=false;
  Check(!encoded.ok()&&!decoded.ok()&&!decoded.record&&
      std::all_of(backing.begin(),backing.end(),[](auto v){return v==0x5a;}),"invalid native values refuse independently encoded input and preserve output");
}
void Tests() {
  const p::TypedUuid tx{p::UuidKind::transaction,f::Id(12003)};const auto base=m::Values(tx);Roundtrip(base);
  auto wide=base;wide.generation=wide.policy_pack_generation=wide.origin_local_transaction_id=wide.hard_limit_bytes=
      wide.soft_limit_bytes=wide.per_context_limit_bytes=wide.page_buffer_pool_limit_bytes=wide.min_startup_available_bytes=std::numeric_limits<p::u64>::max();
  wide.openssl_budget_bytes=wide.hard_limit_bytes-1;wide.failure_mode=c::MemoryConfigurationFailureMode::fatal_status;Roundtrip(wide);
  for(unsigned scenario=0;scenario<20;++scenario) {
    auto bad=base;
    if(scenario==0)bad.configuration_uuid={};if(scenario==1)bad.policy_pack_uuid=bad.profile_uuid;
    if(scenario==2)bad.profile_uuid=bad.origin_transaction_uuid.value;if(scenario==3)bad.generation=0;
    if(scenario==4)bad.policy_pack_generation=0;if(scenario==5)bad.imported_schema_version=2;
    if(scenario==6)bad.origin_transaction_uuid.value={};if(scenario==7)bad.origin_local_transaction_id=0;
    if(scenario==8)bad.hard_limit_bytes=0;if(scenario==9)bad.soft_limit_bytes=bad.hard_limit_bytes+1;
    if(scenario==10)bad.per_context_limit_bytes=bad.hard_limit_bytes+1;if(scenario==11)bad.page_buffer_pool_limit_bytes=bad.hard_limit_bytes+1;
    if(scenario==12)bad.openssl_budget_bytes=0;if(scenario==13)bad.openssl_budget_bytes=bad.hard_limit_bytes;
    if(scenario==14)bad.min_startup_available_bytes=bad.hard_limit_bytes+1;
    if(scenario==15)bad.failure_mode=static_cast<c::MemoryConfigurationFailureMode>(3);
    if(scenario==16)bad.page_buffer_growth=static_cast<c::MemoryConfigurationGrowth>(0);
    if(scenario==17)bad.write_flush_authority=static_cast<c::MemoryConfigurationWriteAuthority>(0);
    if(scenario==18)bad.dirty_writeback_required=false;if(scenario==19){bad.require_platform_memory_ceiling=true;bad.enable_platform_memory_probe=false;}
    Refuse(bad);
  }
  for(unsigned i=0;i<3;++i){auto bad=base;if(i==0)bad.cache_finality_authority=true;if(i==1)bad.cache_visibility_authority=true;if(i==2)bad.wal_or_redo_authority=true;Refuse(bad);}
  for(unsigned version: {0,1,4,6,8}) {auto bad=base;bad.configuration_uuid.bytes[6]=version<<4;Refuse(bad);}
  {
    auto wrong_kind=base;wrong_kind.origin_transaction_uuid.kind=p::UuidKind::object;
    std::array<p::byte,2048> backing;backing.fill(0x5a);heap_denied=true;
    const auto refused=c::EncodeCatalogMemoryConfigurationInto(wrong_kind,backing);heap_denied=false;
    Check(!refused.ok()&&std::all_of(backing.begin(),backing.end(),[](auto b){return b==0x5a;}),"wrong in-memory identity domain refuses before encoding");
    const auto decoded=c::DecodeCatalogMemoryConfiguration(m::Golden(wrong_kind));
    Check(decoded.ok()&&decoded.record->origin_transaction_uuid.kind==p::UuidKind::transaction,
        "canonical stored raw16 identity receives its declared field domain, not a hidden text/kind tag");
  }
  std::ifstream input(SB_MEMORY_CONFIGURATION_RESOURCE_PATH);Check(bool(input),"actual memory resource readable");
  const std::string source{std::istreambuf_iterator<char>(input),{}};Check(!source.empty(),"actual memory source nonempty");
  std::array<char,768> labels;
  heap_denied=true;const auto imported=c::ImportMemoryConfigurationInto(source,labels);heap_denied=false;
  Check(imported.ok()&&imported.settings->configuration_uuid.is_nil()&&imported.settings->profile_uuid.is_nil()&&
      imported.settings->origin_transaction_uuid.value.is_nil(),"import yields only typed settings, never synthetic owner authority");
  auto bound=*imported.settings;bound.configuration_uuid=base.configuration_uuid;bound.policy_pack_uuid=base.policy_pack_uuid;
  bound.policy_pack_generation=base.policy_pack_generation;bound.profile_uuid=base.profile_uuid;
  bound.origin_transaction_uuid=tx;bound.origin_local_transaction_id=1;
  Check(m::Golden(bound)==m::Golden(base),"every actual imported limit/flag/label matches independent expected native bytes");
  const auto used=bound.policy_key.size()+bound.policy_name.size()+bound.provenance.size();
  for(std::size_t n=0;n<used;++n){labels.fill('Z');heap_denied=true;
    const auto refused=c::ImportMemoryConfigurationInto(source,std::span<char>(labels).first(n));heap_denied=false;
    Check(!refused.ok()&&!refused.settings&&std::all_of(labels.begin(),labels.end(),[](char c){return c=='Z';}),"every truncated import string grant preserves backing");}
  for(unsigned scenario=0;scenario<14;++scenario) {
    auto bad=source;
    if(scenario==0)Replace(bad,"\"hard_limit_bytes\": 1073741824","\"hard_limit_bytes\": 18446744073709551616");
    if(scenario==1)Replace(bad,"\"track_allocations\": true","\"track_allocations\": 1");
    if(scenario==2)Replace(bad,"\"openssl_budget_bytes\": 4194304","\"openssl_budget_bytes\": 0");
    if(scenario==3)Replace(bad,"\"dirty_writeback_required\": true","\"dirty_writeback_required\": false");
    if(scenario==4)Replace(bad,"\"cache_finality_authority\": false","\"cache_finality_authority\": true");
    if(scenario==5)Replace(bad,"\"page_buffer_growth\": \"adaptive_with_policy_limit\"","\"page_buffer_growth\": \"unknown\"");
    if(scenario==6)Replace(bad,"\"profile_mode\": \"configured_policy_required\"","\"profile_mode\": \"ignore_policy\"");
    if(scenario==7)Replace(bad,"\"schema_version\": 1,","\"schema_version\": 1, \"unknown\": 1,");
    if(scenario==8)Replace(bad,"\"schema_version\": 1,","\"schema_version\": 1, \"schema_version\": 1,");
    if(scenario==9)Replace(bad,"\"hard_limit_bytes\": 1073741824,","\"hard_limit_bytes\": 1073741824, \"hard\\u005flimit_bytes\": 1073741824,");
    if(scenario==10)Replace(bad,"\"policy_name\": \"default_local_server_memory_cache_v1\"","\"policy_name\": null");
    if(scenario==11)Replace(bad,"\"schema_version\": 1,","\"schema_version\": 1e0,");
    if(scenario==12)Replace(bad,"\"policy_name\": \"default_local_server_memory_cache_v1\"","\"policy_name\": \"\\u0000\"");
    if(scenario==13)bad.pop_back(),bad.pop_back();
    labels.fill('Z');heap_denied=true;const auto refused=c::ImportMemoryConfigurationInto(bad,labels);heap_denied=false;
    Check(!refused.ok()&&!refused.settings&&std::all_of(labels.begin(),labels.end(),[](char c){return c=='Z';}),"malformed/contradictory/duplicate imports refuse before writes");
    if(scenario==2) {
      Check(refused.diagnostic&&refused.diagnostic->policy_name()==base.policy_name&&refused.diagnostic->provenance()==base.provenance,
          "semantic refusal retains exact bounded diagnostic labels without partial settings or output writes");
      const auto copied=refused;
      Check(copied.diagnostic->policy_name()==base.policy_name&&copied.diagnostic->provenance()==base.provenance,
          "copied diagnostic owns labels rather than retaining stack views");
    }
  }
  auto escaped=source;Replace(escaped,"\"limits\"","\"\\u006cimits\"");
  Replace(escaped,"\"policy_name\": \"default_local_server_memory_cache_v1\"","\"policy_name\": \"A\\n\\u03a9\\ud834\\udd1e\"");
  heap_denied=true;const auto unicode=c::ImportMemoryConfigurationInto(escaped,labels);heap_denied=false;
  Check(unicode.ok()&&unicode.settings->policy_name=="A\n\xce\xa9\xf0\x9d\x84\x9e","decoded Unicode keys/labels preserve exact scalar bytes");
  auto overlap=source;const auto before=overlap;heap_denied=true;
  const auto refused_overlap=c::ImportMemoryConfigurationInto(overlap,{overlap.data(),overlap.size()});heap_denied=false;
  Check(!refused_overlap.ok()&&overlap==before,"import backing cannot overlap immutable input");
  const std::string profile_source=R"({"profile_uuid":"01900000-0000-7000-8000-000000000001","area":"memory_resource_governance","mode":"configured_policy_required","resource_file":"policies/server_memory_cache_policy.json"})";
  labels.fill('Z');heap_denied=true;
  const auto profile=c::ImportBootstrapPolicyProfileInto(profile_source,labels);heap_denied=false;
  Check(profile.ok()&&profile.profile->profile_uuid.bytes[0]==1&&profile.profile->profile_uuid.bytes[1]==0x90&&
      profile.profile->profile_uuid.bytes[15]==1&&profile.profile->area=="memory_resource_governance"&&
      profile.profile->mode=="configured_policy_required"&&profile.profile->resource_file=="policies/server_memory_cache_policy.json",
      "profile import resolves canonical raw16 identity and scoped settings without allocation");
  const auto profile_used=profile.profile->area.size()+profile.profile->mode.size()+profile.profile->resource_file->size();
  for(std::size_t n=0;n<profile_used;++n){labels.fill('Z');heap_denied=true;
    const auto refused=c::ImportBootstrapPolicyProfileInto(profile_source,std::span<char>(labels).first(n));heap_denied=false;
    Check(!refused.ok()&&!refused.profile&&std::all_of(labels.begin(),labels.end(),[](char c){return c=='Z';}),"every profile backing truncation refuses atomically");}
  for(unsigned scenario=0;scenario<12;++scenario){auto bad=profile_source;
    if(scenario==0)Replace(bad,"7000","4000");if(scenario==1)Replace(bad,"8000","0000");
    if(scenario==2)Replace(bad,"01900000-","01900000_");
    if(scenario==3)Replace(bad,"\"resource_file\":\"policies/server_memory_cache_policy.json\"","\"resource_file\":null");
    if(scenario==4)Replace(bad,"\"resource_file\":\"policies/server_memory_cache_policy.json\"","\"resource_file\":\"\"");
    if(scenario==5)Replace(bad,"\"area\":\"memory_resource_governance\"","\"area\":1");
    if(scenario==6)Replace(bad,"\"area\":\"memory_resource_governance\"","\"area\":\"\\u0000\"");
    if(scenario==7)Replace(bad,"\"mode\":\"configured_policy_required\"","\"mode\":\"configured_policy_required\",\"m\\u006fde\":\"different\"");
    if(scenario==8)Replace(bad,"\"resource_file\"","\"unknown\"");
    if(scenario==9)Replace(bad,"\"area\":\"memory_resource_governance\",","");
    if(scenario==10)bad.pop_back();if(scenario==11)Replace(bad,"\"mode\":\"configured_policy_required\"","\"mode\":\"\\ud834\"");
    labels.fill('Z');heap_denied=true;const auto refused=c::ImportBootstrapPolicyProfileInto(bad,labels);heap_denied=false;
    Check(!refused.ok()&&!refused.profile&&std::all_of(labels.begin(),labels.end(),[](char c){return c=='Z';}),"malformed, duplicate, missing and ill-typed profile members refuse without writes");}
  auto without_resource=profile_source;Replace(without_resource,",\"resource_file\":\"policies/server_memory_cache_policy.json\"","");
  heap_denied=true;const auto absent=c::ImportBootstrapPolicyProfileInto(without_resource,labels);heap_denied=false;
  Check(absent.ok()&&!absent.profile->resource_file,"absent optional resource remains absent, not an inferred runtime resource");
  auto profile_overlap=profile_source;const auto profile_before=profile_overlap;heap_denied=true;
  const auto overlapping_profile=c::ImportBootstrapPolicyProfileInto(profile_overlap,{profile_overlap.data(),profile_overlap.size()});heap_denied=false;
  Check(!overlapping_profile.ok()&&profile_overlap==profile_before,"profile input/output overlap preserves source bytes");
  const auto metadata=m::Metadata(base,f::Id(60));heap_denied=true;
  const auto sized=c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(metadata));heap_denied=false;
  Check(sized.has_value(),"shared native metadata dispatch admits actual memory family without heap");
  const auto encoded=c::EncodeCatalogMetadataVersion(metadata);Check(encoded.ok(),"owning native memory envelope encoded");
  const auto reopened=c::DecodeCatalogMetadataVersion(encoded.bytes);Check(reopened.ok()&&reopened.record.record.payload==metadata.record.payload,"complete native envelope roundtrip");
  for(unsigned scenario=0;scenario<7;++scenario){auto bad=metadata;
    if(scenario==0)bad.object_subtype="other";if(scenario==1)bad.record.header.kind=c::CatalogRecordKind::policy;
    if(scenario==2)bad.record.header.object_uuid.value=f::Id(100);if(scenario==3)bad.default_name_uuid={};
    if(scenario==4)bad.name_vector_uuid={};if(scenario==5)bad.owning_schema_uuid={};if(scenario==6)bad.definition_version=2;
    heap_denied=true;const auto size=c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(bad));heap_denied=false;
    Check(!size,"shared family/header/name/generation/ownership confusion refuses");}
  auto next=metadata;auto replacement=base;replacement.generation=2;replacement.soft_limit_bytes=700000000;
  next.definition_version=2;next.creator_transaction_uuid.value=f::Id(12004);next.creator_local_transaction_id=2;
  next.record.payload=m::Golden(replacement);Check(c::CatalogMetadataPreservesFamilyOrigin(metadata,next),"genuine later version retains immutable family origin");
  replacement.origin_transaction_uuid=next.creator_transaction_uuid;replacement.origin_local_transaction_id=2;next.record.payload=m::Golden(replacement);
  Check(c::CatalogMemoryConfigurationMatchesMetadata(c::BorrowCatalogMetadataVersion(next))&&
      !c::CatalogMetadataPreservesFamilyOrigin(metadata,next),"independently valid later version cannot rewrite original transaction");
}
} // namespace
int main()try{Tests();std::cout<<"PASS native memory configuration "<<checks<<" checks\n";return 0;}
catch(const std::exception& e){heap_denied=false;std::cerr<<e.what()<<'\n';return 1;}
