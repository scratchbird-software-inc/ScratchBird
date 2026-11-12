// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_agent_bootstrap.hpp"
#include "catalog_named_creation.hpp"
#include "native_creation_workspace.hpp"
#include "native_creation_catalog_input.hpp"
#include "physical_mga_cow_store.hpp"
#include "bootstrap_policy_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <new>
#include <source_location>
#include <stdexcept>
thread_local long allocation_budget=-1;
thread_local unsigned allocation_count=0;
void* operator new(std::size_t size){++allocation_count;if(allocation_budget>=0&&allocation_budget--==0)throw std::bad_alloc();
  if(auto* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
namespace c=scratchbird::core::catalog;namespace p=scratchbird::core::platform;namespace f=scratchbird::tests::bootstrap_policy;
namespace db=scratchbird::storage::database;namespace d=scratchbird::storage::disk;namespace u=scratchbird::core::uuid;namespace mga=scratchbird::transaction::mga;
namespace {
using Body=c::CatalogAgentBootstrapDefinition;
unsigned checks=0;
void Check(bool ok,const char* why,std::source_location loc=std::source_location::current()){++checks;
 if(!ok){allocation_budget=-1;throw std::runtime_error(std::string(why)+" line="+std::to_string(loc.line()));}}
template<class Call>auto NoHeap(Call&& call){allocation_budget=0;allocation_count=0;auto result=call();const auto calls=allocation_count;allocation_budget=-1;
 Check(!calls,"bounded native agent decoder never allocates");return result;}
Body Value(){Body r;r.definition_uuid=r.policy_uuid=f::Id(100);r.database_uuid=f::Id(1);
 r.kind=c::AgentBootstrapDefinitionKind::policy;r.agent_type="agent_coordinator";r.properties="config.startup_verification_class=startup_full_verify\n";
 r.origin_transaction_uuid={p::UuidKind::transaction,f::Id(5)};r.origin_local_transaction_id=r.generation=1;
 r.startup_verification=c::CatalogAgentStartupVerificationSelection{f::Id(200),f::Id(201),f::Id(202),f::Id(203),3};return r;}
std::string Golden(const Body& r){f::Golden g(65824);if(r.startup_verification)f::Put(g.bytes,20,2,2);
 g.Identity(1,r.definition_uuid);g.Identity(2,r.database_uuid);g.Integer(3,p::u64(r.kind));g.Field(4,3,r.agent_type);g.Field(5,3,r.properties);
 g.Identity(6,r.policy_uuid);g.Identity(7,r.origin_transaction_uuid.value);g.Integer(8,r.origin_local_transaction_id);g.Integer(9,r.generation);
 if(r.startup_verification){const auto& v=*r.startup_verification;g.Identity(10,v.repair_policy_uuid);g.Identity(11,v.quarantine_policy_uuid);
  g.Identity(12,v.salvage_policy_uuid);g.Identity(13,v.evidence_policy_uuid);g.Integer(14,v.verification_levels);}return g.Finish();}
void Roundtrip(const Body& r){const auto bytes=Golden(r);const auto encoded=c::EncodeCatalogAgentBootstrapDefinition(r);
 Check(encoded.ok()&&std::string(encoded.bytes.begin(),encoded.bytes.end())==bytes,"exact independently framed native agent fields and version");
 const auto decoded=NoHeap([&]{return c::DecodeCatalogAgentBootstrapDefinition(bytes);});
 Check(decoded.ok()&&Golden(*decoded.record)==bytes&&decoded.record->startup_verification==r.startup_verification,"bounded decoder retains every typed coordinator reference");}
void Refuse(const Body& r,bool wire=true){const auto encoded=c::EncodeCatalogAgentBootstrapDefinition(r);Check(!encoded.ok()&&encoded.bytes.empty(),"invalid startup selection does not encode a partial record");
 if(wire){const auto bytes=Golden(r);Check(!NoHeap([&]{return c::DecodeCatalogAgentBootstrapDefinition(bytes);}).ok(),"malformed binary selection cannot enter through decode");}}
void Values(){auto value=Value();Roundtrip(value);auto legacy=value;legacy.startup_verification.reset();Roundtrip(legacy);
 const auto old=c::DecodeCatalogAgentBootstrapDefinition(Golden(legacy));Check(old.ok()&&!old.record->startup_verification,"version1 remains declaration-only with no invented references");
 auto worker=legacy;worker.agent_type="memory_governor";Roundtrip(worker);
 worker.kind=c::AgentBootstrapDefinitionKind::registration;worker.definition_uuid=f::Id(101);Roundtrip(worker);
 for(unsigned n=0;n<14;++n){auto bad=value;switch(n){case 0:bad.startup_verification->verification_levels=0;break;
  case 1:bad.startup_verification->verification_levels=1024;break;case 2:bad.startup_verification->repair_policy_uuid={};break;
  case 3:bad.startup_verification->quarantine_policy_uuid=bad.startup_verification->repair_policy_uuid;break;
  case 4:bad.startup_verification->salvage_policy_uuid=bad.database_uuid;break;case 5:bad.startup_verification->evidence_policy_uuid=bad.definition_uuid;break;
  case 6:bad.startup_verification->repair_policy_uuid=bad.origin_transaction_uuid.value;break;case 7:bad.agent_type="startup_agent";break;
  case 8:bad.kind=c::AgentBootstrapDefinitionKind::registration;break;case 9:bad.startup_verification->repair_policy_uuid.bytes[6]=0x40;break;
  case 10:bad.policy_uuid=f::Id(102);break;case 11:bad.generation=0;break;case 12:bad.origin_local_transaction_id=0;break;case 13:bad.properties={};break;}Refuse(bad);}
 for(unsigned bit=0;bit<10;++bit){auto one=value;one.startup_verification->verification_levels=p::u64{1}<<bit;Roundtrip(one);}
 auto maximum=value;maximum.startup_verification->verification_levels=1023;Roundtrip(maximum);
 auto wrong_domain=value;wrong_domain.origin_transaction_uuid.kind=p::UuidKind::object;Refuse(wrong_domain,false);
 const auto bytes=Golden(value);for(std::size_t n=0;n<bytes.size();++n)Check(!NoHeap([&]{return c::DecodeCatalogAgentBootstrapDefinition(std::string_view(bytes).substr(0,n));}).ok(),"every truncated typed selection refused");
 for(unsigned version:{0u,3u,65535u}){auto bad=bytes;f::Put(bad,20,version,2);Check(!c::DecodeCatalogAgentBootstrapDefinition(bad).ok(),"unknown schema version refused");}
 auto downgraded=bytes;f::Put(downgraded,20,1,2);Check(!c::DecodeCatalogAgentBootstrapDefinition(downgraded).ok(),"version1 cannot carry unregistered extra selection fields");
 auto missing=Golden(legacy);f::Put(missing,20,2,2);Check(!c::DecodeCatalogAgentBootstrapDefinition(missing).ok(),"version2 cannot omit complete required selection");
 auto trailing=bytes;trailing.push_back(0);Check(!c::DecodeCatalogAgentBootstrapDefinition(trailing).ok(),"trailing carrier bytes refused");
 auto m=f::Metadata(0,value.origin_transaction_uuid,f::Id(60),bytes);m.record.header.object_uuid={p::UuidKind::object,value.definition_uuid};m.object_subtype=scratchbird::core::catalog::CatalogObjectSubtype::agent_policy;
 Check(!c::ValidateCatalogMetadataVersionView(c::BorrowCatalogMetadataVersion(m)),"common native dispatcher validates version2 policy carrier");
 std::vector<p::Uuid> reserved;db::ReserveNativeCreationCatalogReferences({m,{}},[&](auto id){reserved.push_back(id);});
 for(const auto& id:{value.database_uuid,value.policy_uuid,value.origin_transaction_uuid.value,value.startup_verification->repair_policy_uuid,
     value.startup_verification->quarantine_policy_uuid,value.startup_verification->salvage_policy_uuid,value.startup_verification->evidence_policy_uuid})
  Check(std::find(reserved.begin(),reserved.end(),id)!=reserved.end(),"creation identity owner reserves exact persisted coordinator policy references");
}
}
int main(){try{Values();std::cout<<checks<<" coordinator policy codec checks passed\n";return 0;}
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<e.what()<<'\n';return 1;}}
