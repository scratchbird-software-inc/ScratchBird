// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_transaction_policy.hpp"
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
using Body=c::CatalogTransactionPolicyBody;
unsigned checks=0;
void Check(bool ok,const char* why,std::source_location loc=std::source_location::current()){++checks;
 if(!ok){allocation_budget=-1;throw std::runtime_error(std::string(why)+" line="+std::to_string(loc.line()));}}
template<class Call>auto NoHeap(Call&& call){allocation_budget=0;allocation_count=0;auto result=call();const auto calls=allocation_count;allocation_budget=-1;
 Check(!calls,"bounded transaction policy codecs never allocate");return result;}
Body Value(unsigned n=0){Body b;b.body_uuid=f::Id(100+n);b.database_uuid=f::Id(1);b.generation=1;
 b.default_isolation_profile_uuid=f::Id(300);b.startup_isolation_profile_uuid=f::Id(301);
 b.allowed_isolation_profiles[0]=b.default_isolation_profile_uuid;b.allowed_isolation_profiles[1]=b.startup_isolation_profile_uuid;
 b.allowed_isolation_profile_count=2;b.read_mode_mask=3;b.maximum_transaction_microseconds=60000000;
 b.maximum_snapshot_microseconds=10000000;b.maximum_active_transactions=64;b.maximum_savepoints=16;
 b.savepoint_mode=c::TransactionSavepointMode::engine_transaction_local;b.lock_policy_uuid=f::Id(400);b.evidence_policy_uuid=f::Id(401);
 b.origin_transaction_uuid=b.creating_transaction_uuid={p::UuidKind::transaction,f::Id(5)};
 b.origin_local_transaction_id=b.creating_local_transaction_id=1;return b;}
std::string Golden(const Body& r){f::Golden g(65624);
 g.Identity(1,r.body_uuid);g.Identity(2,r.database_uuid);g.Integer(3,r.generation);
 g.Identity(4,r.default_isolation_profile_uuid);g.Identity(5,r.startup_isolation_profile_uuid);
 std::string profiles;for(std::size_t i=0;i<r.allowed_isolation_profile_count;++i)profiles+=f::Bytes(r.allowed_isolation_profiles[i]);g.Field(6,7,profiles);
 g.Integer(7,r.read_mode_mask);g.Integer(8,r.maximum_transaction_microseconds);g.Integer(9,r.maximum_snapshot_microseconds);
 g.Integer(10,r.maximum_active_transactions);g.Integer(11,r.maximum_savepoints);g.Integer(12,r.maximum_prepared_transactions);
 g.Identity(13,r.lock_policy_uuid);g.Integer(14,r.deadlock_priority);g.Integer(15,p::u64(r.savepoint_mode));
 g.Integer(16,p::u64(r.autocommit_mode));g.Integer(17,p::u64(r.cluster_eligibility));g.Integer(18,p::u64(r.retention_behavior));
 g.Integer(19,r.reload_trigger_mask);g.Integer(20,p::u64(r.limbo_behavior));g.Integer(21,p::u64(r.commit_ordering));
 g.Integer(22,p::u64(r.restart_behavior));g.Integer(23,r.maximum_statement_restarts);g.Identity(24,r.evidence_policy_uuid);g.Integer(25,r.flags);
 g.Identity(26,r.origin_transaction_uuid.value);g.Integer(27,r.origin_local_transaction_id);
 g.Identity(28,r.creating_transaction_uuid.value);g.Integer(29,r.creating_local_transaction_id);if(r.cluster_uuid)g.Identity(30,*r.cluster_uuid);return g.Finish();}
c::CatalogMetadataVersion Metadata(const Body& r,unsigned n=0){auto m=f::Metadata(0,r.creating_transaction_uuid,f::Id(60),Golden(r));
 m.record.header.object_uuid={p::UuidKind::object,r.body_uuid};m.record.header.row_uuid={p::UuidKind::row,f::Id(1000+3*n)};
 m.default_name_uuid={p::UuidKind::object,f::Id(2000+2*n)};m.name_vector_uuid={p::UuidKind::object,f::Id(2001+2*n)};
 m.object_subtype=scratchbird::core::catalog::CatalogObjectSubtype::transaction_policy_body;m.creator_local_transaction_id=r.creating_local_transaction_id;m.definition_version=r.generation;
 if(r.cluster_uuid)m.authority_scope=c::CatalogAuthorityScope::cluster;return m;}
void Roundtrip(const Body& value){const auto expected=Golden(value);std::array<p::byte,c::kCatalogTransactionPolicyMaxBytes+2> output;output.fill(0xa7);
 const auto size=NoHeap([&]{return c::CatalogTransactionPolicyEncodedBytes(value);});Check(size&&*size==expected.size()&&*size<=c::kCatalogTransactionPolicyMaxBytes,"independent complete byte count");
 const auto encoded=NoHeap([&]{return c::EncodeCatalogTransactionPolicyInto(value,std::span(output).subspan(1));});
 Check(encoded.ok()&&std::string_view(reinterpret_cast<const char*>(encoded.bytes.data()),encoded.bytes.size())==expected,"exact independent SBCV transaction fields");
 Check(output.front()==0xa7&&std::all_of(output.begin()+expected.size()+1,output.end(),[](auto b){return b==0xa7;}),"unaligned destination preserves guards");
 const auto decoded=NoHeap([&]{return c::DecodeCatalogTransactionPolicy(expected);});Check(decoded.ok()&&Golden(*decoded.record)==expected,"all transaction policy fields retained");}
void Refuse(const Body& value,bool wire=true){std::array<p::byte,c::kCatalogTransactionPolicyMaxBytes> output;output.fill(0xa7);const auto before=output;
 const auto encoded=NoHeap([&]{return c::EncodeCatalogTransactionPolicyInto(value,output);});
 Check(!encoded.ok()&&encoded.bytes.empty()&&output==before&&!NoHeap([&]{return c::CatalogTransactionPolicyEncodedBytes(value);}),"invalid body leaves output unchanged");
 if(wire){const auto raw=Golden(value);const auto decoded=NoHeap([&]{return c::DecodeCatalogTransactionPolicy(raw);});Check(!decoded.ok()&&!decoded.record,"invalid transaction policy bytes refused");}}
void Values(){const auto base=Value();Roundtrip(base);Check(Golden(base).size()==576,"exact standalone two-profile body size");
 auto maximum=base;maximum.allowed_isolation_profile_count=64;for(unsigned i=0;i<64;++i)maximum.allowed_isolation_profiles[i]=f::Id(300+i);
 maximum.cluster_uuid=f::Id(7);maximum.cluster_eligibility=c::TransactionClusterEligibility::require_cluster_admission;
 maximum.maximum_transaction_microseconds=maximum.maximum_snapshot_microseconds=maximum.maximum_active_transactions=
 maximum.maximum_savepoints=maximum.maximum_prepared_transactions=UINT64_MAX;maximum.deadlock_priority=255;
 maximum.restart_behavior=c::TransactionRestartBehavior::bounded_statement_restart;maximum.maximum_statement_restarts=UINT64_MAX;
 maximum.autocommit_mode=c::TransactionAutocommitMode::statement_transaction;Roundtrip(maximum);
 Check(Golden(maximum).size()==1592,"maximum canonical profile list and optional cluster have exact registered size");
 auto single=base;single.allowed_isolation_profile_count=1;single.startup_isolation_profile_uuid=single.default_isolation_profile_uuid;
 single.savepoint_mode=c::TransactionSavepointMode::disabled;single.maximum_savepoints=0;Roundtrip(single);
 for(unsigned mask=0;mask<32;++mask){auto b=base;b.reload_trigger_mask=mask;if(mask==7||mask==15)Roundtrip(b);else Refuse(b);}
 for(unsigned bit=0;bit<64;++bit){auto b=base;b.flags^=p::u64{1}<<bit;Refuse(b);}
 for(unsigned field=0;field<38;++field){auto b=base;switch(field){
  case 0:b.body_uuid={};break;case 1:b.database_uuid.bytes[6]=0x40;break;case 2:b.generation=0;break;
  case 3:b.default_isolation_profile_uuid=f::Id(302);break;case 4:b.startup_isolation_profile_uuid={};break;
  case 5:b.allowed_isolation_profile_count=0;break;case 6:std::swap(b.allowed_isolation_profiles[0],b.allowed_isolation_profiles[1]);break;
  case 7:b.allowed_isolation_profiles[1]=b.allowed_isolation_profiles[0];break;case 8:b.allowed_isolation_profiles[0].bytes[6]=0x40;break;
  case 9:b.read_mode_mask=0;break;case 10:b.read_mode_mask=4;break;
  // SB_DRIFT_TRANSACTION_POLICY_ZERO_IS_NO_LIMIT (owner 2026-11-04): zero was
  // refused here; it now means no limit and must round-trip.
  case 11:b.maximum_transaction_microseconds=0;Roundtrip(b);continue;
  case 12:b.maximum_snapshot_microseconds=0;Roundtrip(b);continue;
  case 13:b.maximum_active_transactions=0;Roundtrip(b);continue;
  case 14:b.lock_policy_uuid={};break;case 15:b.lock_policy_uuid=b.body_uuid;break;case 16:b.evidence_policy_uuid=b.lock_policy_uuid;break;
  case 17:b.deadlock_priority=256;break;case 18:b.savepoint_mode=c::TransactionSavepointMode(3);break;
  case 19:b.maximum_savepoints=0;break;case 20:b.savepoint_mode=c::TransactionSavepointMode::disabled;break;
  case 21:b.autocommit_mode=c::TransactionAutocommitMode(0);break;case 22:b.autocommit_mode=c::TransactionAutocommitMode(3);break;
  case 23:b.cluster_eligibility=c::TransactionClusterEligibility(3);break;case 24:b.retention_behavior=c::TransactionRetentionBehavior(2);break;
  case 25:b.limbo_behavior=c::TransactionLimboBehavior(0);break;case 26:b.commit_ordering=c::TransactionCommitOrdering(2);break;
  case 27:b.restart_behavior=c::TransactionRestartBehavior(3);break;case 28:b.maximum_statement_restarts=1;break;
  case 29:b.restart_behavior=c::TransactionRestartBehavior::bounded_statement_restart;break;
  case 30:b.origin_local_transaction_id=0;break;case 31:b.creating_local_transaction_id=2;break;
  case 32:b.creating_transaction_uuid.value=f::Id(8);break;case 33:b.cluster_uuid=b.body_uuid;break;
  case 34:b.cluster_uuid=b.startup_isolation_profile_uuid;break;case 35:b.evidence_policy_uuid=b.origin_transaction_uuid.value;break;
  case 36:b.lock_policy_uuid=b.default_isolation_profile_uuid;break;case 37:b.cluster_eligibility=c::TransactionClusterEligibility(0);break;
 }Refuse(b);}
 auto over=base;over.allowed_isolation_profile_count=65;Refuse(over,false);
 auto domain=base;domain.origin_transaction_uuid.kind=p::UuidKind::object;Refuse(domain,false);
 domain=base;domain.creating_transaction_uuid.kind=p::UuidKind::object;Refuse(domain,false);
 const auto raw=Golden(maximum);std::array<p::byte,c::kCatalogTransactionPolicyMaxBytes> out;out.fill(0xab);const auto before=out;
 for(std::size_t n=0;n<raw.size();++n){Check(!NoHeap([&]{return c::DecodeCatalogTransactionPolicy(std::string_view(raw).substr(0,n));}).ok(),"every truncated wire refuses");
  const auto encoded=NoHeap([&]{return c::EncodeCatalogTransactionPolicyInto(maximum,std::span(out).first(n));});
  Check(!encoded.ok()&&encoded.bytes.empty()&&out==before,"short output never writes a successful prefix");}
 for(const unsigned offset:{0u,4u,6u,8u,12u,16u,20u,22u,24u,26u,27u,28u}){auto invalid=raw;invalid[offset]^=0x7f;
  Check(!NoHeap([&]{return c::DecodeCatalogTransactionPolicy(invalid);}).ok(),"invalid framing/schema/type/tag refused");}
 auto trailing=raw;trailing+='x';Check(!c::DecodeCatalogTransactionPolicy(trailing).ok(),"trailing bytes refused");
 auto overlap=base;Check(NoHeap([&]{return c::EncodeCatalogTransactionPolicyInto(overlap,{reinterpret_cast<p::byte*>(&overlap),sizeof(overlap)});}).error==c::CatalogValueError::invalid_backing&&Golden(overlap)==Golden(base),"encoding cannot overwrite its actual input object");
}
void Bindings(){auto b=Value();auto m=Metadata(b);
 Check(c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(m)).has_value(),"common catalog dispatcher accepts exact transaction metadata");
 for(unsigned n=0;n<5;++n){auto bad=m;if(n==0)bad.record.header.object_uuid.value=f::Id(8);if(n==1)bad.record.header.row_uuid.value=b.lock_policy_uuid;
  if(n==2)bad.definition_version=2;if(n==3)bad.creator_local_transaction_id=2;if(n==4)bad.object_subtype=scratchbird::core::catalog::CatalogObjectSubtype::schema_application;
  Check(!c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(bad)),"common dispatcher cannot bypass family header/creator/generation/subtype checks");}
 Check(!c::CatalogTransactionPolicyMatchesDatabase(c::BorrowCatalogMetadataVersion(m),f::Id(9)),"database binding uses actual body identity");
 c::CatalogPolicyDescriptor d;d.policy_uuid=f::Id(500);d.policy_version_uuid=f::Id(501);d.database_uuid=b.database_uuid;
 d.body_uuid=b.body_uuid;d.body_generation=d.generation=1;d.policy_class=c::CatalogPolicyClass::transaction;
 d.creating_transaction_number=d.origin_local_transaction_id=1;d.creating_transaction_uuid=d.origin_transaction_uuid=b.origin_transaction_uuid;
 d.scope=c::CatalogPolicyScope::database;d.scope_uuid=b.database_uuid;d.authority_owner_uuid=f::Id(50);d.precedence_rank=1;d.enforcement_points=1;
 Check(c::CatalogTransactionPolicyMatchesDescriptor(b,d),"actual transaction descriptor selects exact declaration");
 ++d.body_generation;Check(!c::CatalogTransactionPolicyMatchesDescriptor(b,d),"descriptor cannot substitute a different body generation");--d.body_generation;
 d.policy_class=c::CatalogPolicyClass::authorization;Check(!c::CatalogTransactionPolicyMatchesDescriptor(b,d),"other policy classes cannot select transaction behavior");
 Check(c::CatalogMetadataPreservesFamilyOrigin(m,m),"exact same-generation body replay");
 auto changed=b;++changed.maximum_active_transactions;Check(!c::CatalogMetadataPreservesFamilyOrigin(m,Metadata(changed)),"same-generation behavior is immutable");
 auto newer=changed;newer.generation=2;newer.creating_transaction_uuid.value=f::Id(8);newer.creating_local_transaction_id=2;
 Check(c::CatalogMetadataPreservesFamilyOrigin(m,Metadata(newer)),"new semantic generation retains original provenance");
 newer.origin_transaction_uuid=newer.creating_transaction_uuid;newer.origin_local_transaction_id=2;
 Check(!c::CatalogMetadataPreservesFamilyOrigin(m,Metadata(newer)),"new generation cannot replace construction origin");
 b.cluster_uuid=f::Id(7);std::vector<p::Uuid> reserved;
 db::ReserveNativeCreationCatalogReferences({Metadata(b),{}},[&](const auto& id){reserved.push_back(id);});
 for(const auto& id:{b.body_uuid,b.database_uuid,b.default_isolation_profile_uuid,b.startup_isolation_profile_uuid,b.lock_policy_uuid,b.evidence_policy_uuid,*b.cluster_uuid,b.origin_transaction_uuid.value,b.creating_transaction_uuid.value})
  Check(std::find(reserved.begin(),reserved.end(),id)!=reserved.end(),"native issuer excludes every transaction policy reference");
}
struct Item {
  c::CatalogTransactionPolicyBody descriptor;
  c::CatalogMetadataVersion definition,vector_metadata,entry_metadata;
  c::CatalogNameVector vector;c::CatalogNameEntry entry;
  std::array<c::CatalogCreationNameEntryInput,1> entries;
  explicit Item(unsigned n):descriptor(Value(n)),definition(Metadata(descriptor,n)){
    vector_metadata=entry_metadata=definition;vector_metadata.record.payload.clear();entry_metadata.record.payload.clear();
    vector_metadata.record.header.kind=entry_metadata.record.header.kind=c::CatalogRecordKind::localized_name;
    vector_metadata.record.header.object_uuid=definition.name_vector_uuid;vector_metadata.record.header.row_uuid.value=f::Id(1001+3*n);
    vector_metadata.record.header.parent_uuid=definition.record.header.object_uuid;vector_metadata.object_subtype=scratchbird::core::catalog::CatalogObjectSubtype::name_vector;
    entry_metadata.record.header.object_uuid=definition.default_name_uuid;entry_metadata.record.header.row_uuid.value=f::Id(1002+3*n);
    entry_metadata.record.header.parent_uuid=definition.name_vector_uuid;entry_metadata.object_subtype=scratchbird::core::catalog::CatalogObjectSubtype::name_entry;entry_metadata.resource_epoch=1;
    vector.name_vector_uuid=entry.name_vector_uuid=definition.name_vector_uuid;vector.object_uuid=entry.object_uuid=definition.record.header.object_uuid;
    vector.default_name_entry_uuid=entry.name_entry_uuid=definition.default_name_uuid;vector.object_kind=entry.object_kind=c::CatalogRecordKind::policy;
    vector.owning_schema_uuid=definition.owning_schema_uuid;vector.default_language_uuid=c::CatalogLanguageIdentity(c::kCatalogLanguage_en_US);
    vector.name_collision_policy_uuid={p::UuidKind::object,f::Id(55)};vector.catalog_generation_id=1;
    vector.security_policy_uuid=definition.security_policy_uuid;vector.lifecycle_state=c::CatalogNameLifecycle::active;
    entry.scope_uuid=definition.record.header.parent_uuid;entry.parent_schema_uuid=definition.owning_schema_uuid;entry.language_uuid=c::CatalogLanguageIdentity(c::kCatalogLanguage_en_US);
    entry.dialect_profile_uuid={p::UuidKind::object,f::Id(56)};entry.identifier_profile_uuid={p::UuidKind::object,f::Id(57)};
    entry.raw_name_text=entry.display_name="explicit_policy_"+std::to_string(n);entry.normalized_lookup_key.assign(entry.raw_name_text.begin(),entry.raw_name_text.end());
    entry.created_transaction_uuid=descriptor.creating_transaction_uuid;entry.security_policy_uuid=definition.security_policy_uuid;
    entry.resource_epoch=entry.name_resolution_epoch=entry.catalog_generation_id=1;entry.default_for_language=entry.default_for_object=true;
    entry.lifecycle_state=c::CatalogNameLifecycle::active;
  }
  c::CatalogNamedCreationInput Input(){entries[0]={&entry_metadata,&entry};return {&definition,&vector_metadata,&vector,entries};}
};
void Physical(){scratchbird::tests::OwnedTempDirectory artifacts;
 for(const auto& profile:d::kCanonicalFilespacePageProfiles){Item item(0);std::array inputs{item.Input()};
  const auto made=c::AssembleLocalNamedCreationRecords(f::Id(1),item.descriptor.creating_transaction_uuid,c::kCatalogLanguage_en_US,inputs,{10,1000000});
  Check(made.ok()&&made.records->definitions.size()==1&&made.records->names.size()==2,"typed body and complete supplied names assemble");
  db::NativeCreationCatalogSeed seed;seed[4]=made.records->definitions;seed[1]=made.records->names;
  db::NativeFilespaceInitializationRequest request;request.bootstrap={f::Id(1),f::Id(2),profile.uuid,d::kNativeBootstrapIntegrityProfile,{},profile.page_size_bytes,1,0,1,7};
  request.operation_uuid=f::Id(3);request.writer_uuid=f::Id(4);request.policy_snapshot_uuid=f::Id(10);
  request.creator.transaction_uuid=item.descriptor.creating_transaction_uuid;request.creator.local_id=mga::MakeLocalTransactionId(1);request.creator.scope=mga::TransactionScope::local_node;
  request.total_pages=64;request.creation_utc_millis=1789357072000ULL;const auto budget=128*p::u64{profile.page_size_bytes};u::StandaloneUuidV7Issuer issuer({f::Id(1),f::Id(10)},{{},0,1000});
  const auto path=(artifacts.path()/std::to_string(profile.page_size_bytes)).string();d::FileDevice device;
  Check(device.Open(path,d::FileOpenMode::create_new,d::DatabaseFileOwnership::native_data_file).ok(),"real native transaction policy file");auto foreign=item.descriptor;foreign.database_uuid=f::Id(9);seed[4][0].metadata.record.payload=Golden(foreign);
  Check(!db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,request,seed,budget,issuer).ok()&&device.Size().size_bytes==0,"foreign body refuses before physical effects");
  seed[4][0].metadata.record.payload=Golden(item.descriptor);const auto created=db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,request,seed,budget,issuer);
  Check(created.ok(),"real selected native transaction policy persists");Check(device.Close().ok()&&device.Open(path,d::FileOpenMode::open_existing).ok(),"independent reopen");
  const std::vector<d::NativeFilespaceDevice> devices{{f::Id(2),profile.uuid,&device}};
  const auto rows=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(f::Id(1),devices,db::NativeCatalogSource::PageZero(created.receipt->filespace_uuid),2,5,{created.receipt->relations[4].object_uuid,{}},budget);
  Check(rows.ok()&&rows.rows.size()==1&&rows.rows[0].metadata.record.payload==Golden(item.descriptor)&&
    c::CatalogTransactionPolicyMatchesDatabase(c::BorrowCatalogMetadataVersion(rows.rows[0].metadata),f::Id(1)),"exact isolation, limits, behavior and origin fields survive native reopen");
  const auto zero=d::ReadFilespacePageZeroFromOpenDevice(device);Check(zero.ok()&&zero.record->bootstrap.lifecycle_state==1,"body persistence is not node activation: page zero stays as the create wrote it, an online filespace (owner 2026-11-07); serving is the server's act at open");
  Check(device.Close().ok(),"owned evidence body file closed");
 }}
}
int main(){try{Values();Bindings();Physical();std::cout<<checks<<" transaction policy checks passed (declarations only)\n";return 0;}
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<e.what()<<'\n';return 1;}}
