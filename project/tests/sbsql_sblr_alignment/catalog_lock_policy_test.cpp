// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_lock_policy.hpp"
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
using Body=c::CatalogLockPolicyBody;
unsigned checks=0;
void Check(bool ok,const char* why,std::source_location loc=std::source_location::current()){++checks;
 if(!ok){allocation_budget=-1;throw std::runtime_error(std::string(why)+" line="+std::to_string(loc.line()));}}
template<class Call>auto NoHeap(Call&& call){allocation_budget=0;allocation_count=0;auto result=call();const auto calls=allocation_count;allocation_budget=-1;
 Check(!calls,"bounded lock policy codecs never allocate");return result;}
Body Value(unsigned n=0){Body b;b.body_uuid=f::Id(100+n);b.database_uuid=f::Id(1);b.generation=1;
 b.fairness_threshold_microseconds=1000000;b.maximum_locks=256;b.maximum_lock_memory_bytes=65536;
 b.maximum_graph_nodes=128;b.maximum_graph_edges=256;b.maximum_deadlock_work=4096;
 b.deadlock_detection_interval_microseconds=100000;b.evidence_policy_uuid=f::Id(401);
 b.origin_transaction_uuid=b.creating_transaction_uuid={p::UuidKind::transaction,f::Id(5)};
 b.origin_local_transaction_id=b.creating_local_transaction_id=1;return b;}
std::string Golden(const Body& r){f::Golden g(65626);
 g.Identity(1,r.body_uuid);g.Identity(2,r.database_uuid);g.Integer(3,r.generation);
 g.Integer(4,p::u64(r.wait_behavior));g.Integer(5,r.wait_timeout_microseconds);g.Integer(6,p::u64(r.fairness));
 g.Integer(7,r.fairness_threshold_microseconds);g.Integer(8,r.maximum_locks);g.Integer(9,r.maximum_waiters);
 g.Integer(10,r.maximum_lock_memory_bytes);g.Integer(11,r.maximum_graph_nodes);g.Integer(12,r.maximum_graph_edges);
 g.Integer(13,r.maximum_deadlock_work);g.Integer(14,r.deadlock_detection_interval_microseconds);
 g.Integer(15,p::u64(r.victim_policy));g.Integer(16,p::u64(r.victim_outcome));g.Integer(17,p::u64(r.timeout_outcome));
 g.Integer(18,r.conversion_priority?1:0);g.Integer(19,r.escalation_lock_threshold);g.Integer(20,r.escalation_memory_threshold);
 g.Integer(21,p::u64(r.predicate_guard));g.Identity(22,r.evidence_policy_uuid);
 if(r.donor_mapping_uuid)g.Identity(23,*r.donor_mapping_uuid);if(r.maintenance_window_policy_uuid)g.Identity(24,*r.maintenance_window_policy_uuid);
 g.Identity(25,r.origin_transaction_uuid.value);g.Integer(26,r.origin_local_transaction_id);
 g.Identity(27,r.creating_transaction_uuid.value);g.Integer(28,r.creating_local_transaction_id);
 if(r.cluster_uuid)g.Identity(29,*r.cluster_uuid);g.Integer(30,r.flags);return g.Finish();}
c::CatalogMetadataVersion Metadata(const Body& r,unsigned n=0){auto m=f::Metadata(0,r.creating_transaction_uuid,f::Id(60),Golden(r));
 m.record.header.object_uuid={p::UuidKind::object,r.body_uuid};m.record.header.row_uuid={p::UuidKind::row,f::Id(1000+3*n)};
 m.default_name_uuid={p::UuidKind::object,f::Id(2000+2*n)};m.name_vector_uuid={p::UuidKind::object,f::Id(2001+2*n)};
 m.object_subtype="lock_policy_body";m.creator_local_transaction_id=r.creating_local_transaction_id;m.definition_version=r.generation;
 if(r.cluster_uuid)m.authority_scope=c::CatalogAuthorityScope::cluster;return m;}
void Roundtrip(const Body& value){const auto expected=Golden(value);std::array<p::byte,c::kCatalogLockPolicyMaxBytes+2> output;output.fill(0xa7);
 const auto size=NoHeap([&]{return c::CatalogLockPolicyEncodedBytes(value);});Check(size&&*size==expected.size()&&*size<=c::kCatalogLockPolicyMaxBytes,"independent complete byte count");
 const auto encoded=NoHeap([&]{return c::EncodeCatalogLockPolicyInto(value,std::span(output).subspan(1));});
 Check(encoded.ok()&&std::string_view(reinterpret_cast<const char*>(encoded.bytes.data()),encoded.bytes.size())==expected,"exact independent SBCV transaction fields");
 Check(output.front()==0xa7&&std::all_of(output.begin()+expected.size()+1,output.end(),[](auto b){return b==0xa7;}),"unaligned destination preserves guards");
 const auto decoded=NoHeap([&]{return c::DecodeCatalogLockPolicy(expected);});Check(decoded.ok()&&Golden(*decoded.record)==expected,"all lock policy fields retained");}
void Refuse(const Body& value,bool wire=true){std::array<p::byte,c::kCatalogLockPolicyMaxBytes> output;output.fill(0xa7);const auto before=output;
 const auto encoded=NoHeap([&]{return c::EncodeCatalogLockPolicyInto(value,output);});
 Check(!encoded.ok()&&encoded.bytes.empty()&&output==before&&!NoHeap([&]{return c::CatalogLockPolicyEncodedBytes(value);}),"invalid body leaves output unchanged");
 if(wire){const auto raw=Golden(value);const auto decoded=NoHeap([&]{return c::DecodeCatalogLockPolicy(raw);});Check(!decoded.ok()&&!decoded.record,"invalid lock policy bytes refused");}}

Body Wait(unsigned mode){auto b=Value();b.wait_behavior=c::LockWaitBehavior(mode);
 if(mode!=1)b.maximum_waiters=128;if(mode>=3)b.wait_timeout_microseconds=1000000;
 if(mode==8)b.donor_mapping_uuid=f::Id(402);if(mode==9)b.maintenance_window_policy_uuid=f::Id(403);return b;}
void Values(){const auto base=Value();Roundtrip(base);Check(Golden(base).size()==496,"standalone exact lock body size");
 for(unsigned mode=1;mode<=9;++mode)Roundtrip(Wait(mode));
 auto maximum=Wait(9);maximum.donor_mapping_uuid=f::Id(402);maximum.cluster_uuid=f::Id(7);
 maximum.fairness=c::LockQueueFairness::priority_with_starvation_prevention;maximum.conversion_priority=true;
 maximum.maximum_locks=maximum.maximum_waiters=maximum.maximum_lock_memory_bytes=maximum.maximum_graph_nodes=
 maximum.maximum_graph_edges=maximum.maximum_deadlock_work=maximum.fairness_threshold_microseconds=
 maximum.deadlock_detection_interval_microseconds=maximum.wait_timeout_microseconds=UINT64_MAX;
 maximum.escalation_lock_threshold=maximum.escalation_memory_threshold=UINT64_MAX;Roundtrip(maximum);
 Check(Golden(maximum).size()==568,"all optional lock identities match registry maximum");
 for(unsigned n=1;n<=7;++n){auto b=maximum;b.victim_outcome=c::LockVictimOutcome(n);Roundtrip(b);}
 for(unsigned n=1;n<=5;++n){auto b=maximum;b.timeout_outcome=c::LockTimeoutOutcome(n);Roundtrip(b);}
 for(unsigned bit=0;bit<64;++bit){auto b=base;b.flags^=p::u64{1}<<bit;Refuse(b);}
 for(unsigned field=0;field<34;++field){auto b=base;switch(field){
   case 0:b.body_uuid={};break;case 1:b.database_uuid.bytes[6]=0x40;break;case 2:b.generation=0;break;
   case 3:b.wait_behavior=c::LockWaitBehavior(0);break;case 4:b.wait_behavior=c::LockWaitBehavior(10);break;
   case 5:b.wait_timeout_microseconds=1;break;case 6:b.maximum_waiters=1;break;
   case 7:b.fairness=c::LockQueueFairness(0);break;case 8:b.fairness=c::LockQueueFairness(3);break;
   case 9:b.fairness_threshold_microseconds=0;break;
   // SB_DRIFT_LOCK_POLICY_ZERO_IS_NO_LIMIT (owner 2026-11-04): zero was refused
   // here; it now means no limit and must round-trip.
   case 10:b.maximum_locks=0;Roundtrip(b);continue;
   case 11:b.maximum_lock_memory_bytes=0;Roundtrip(b);continue;case 12:b.maximum_graph_nodes=0;Roundtrip(b);continue;
   case 13:b.maximum_graph_edges=0;Roundtrip(b);continue;
   case 14:b.maximum_deadlock_work=0;Roundtrip(b);continue;case 15:b.deadlock_detection_interval_microseconds=0;break;
   case 16:b.victim_policy=c::LockVictimPolicy(2);break;case 17:b.victim_outcome=c::LockVictimOutcome(0);break;
   case 18:b.victim_outcome=c::LockVictimOutcome(8);break;case 19:b.timeout_outcome=c::LockTimeoutOutcome(0);break;
   case 20:b.timeout_outcome=c::LockTimeoutOutcome(6);break;case 21:b.conversion_priority=true;break;
   case 22:b.escalation_lock_threshold=b.maximum_locks+1;break;case 23:b.escalation_memory_threshold=b.maximum_lock_memory_bytes+1;break;
   case 24:b.predicate_guard=c::LockPredicateGuard(0);break;case 25:b.predicate_guard=c::LockPredicateGuard(3);break;
   case 26:b.evidence_policy_uuid={};break;case 27:b.donor_mapping_uuid=p::Uuid{};break;
   case 28:b.maintenance_window_policy_uuid=p::Uuid{};break;case 29:b.cluster_uuid=p::Uuid{};break;
   case 30:b.origin_local_transaction_id=0;break;case 31:b.creating_local_transaction_id=2;break;
   case 32:b.creating_transaction_uuid.value=f::Id(8);break;case 33:b.evidence_policy_uuid=b.body_uuid;break;
 }Refuse(b);}
 // With no lock-count or lock-memory limit an escalation threshold has no cap.
 {auto b=base;b.maximum_locks=b.maximum_lock_memory_bytes=0;
  b.escalation_lock_threshold=b.escalation_memory_threshold=UINT64_MAX;Roundtrip(b);}
 for(unsigned mode=2;mode<=9;++mode){auto b=Wait(mode);b.maximum_waiters=0;Refuse(b);
   b=Wait(mode);b.wait_timeout_microseconds=(mode==2?1:0);Refuse(b);}
 for(unsigned n=0;n<5;++n){auto b=maximum;
   if(n==0){b.wait_behavior=c::LockWaitBehavior::donor_compatible_wait;b.donor_mapping_uuid.reset();}
   if(n==1){b.victim_outcome=c::LockVictimOutcome::donor_compatible_error;b.donor_mapping_uuid.reset();}
   if(n==2){b.timeout_outcome=c::LockTimeoutOutcome::donor_compatible_error;b.donor_mapping_uuid.reset();}
   if(n==3)b.maintenance_window_policy_uuid.reset();
   if(n==4){b.victim_outcome=c::LockVictimOutcome::cluster_recovery_required;b.cluster_uuid.reset();}Refuse(b);}
 auto b=base;b.donor_mapping_uuid=b.origin_transaction_uuid.value;Refuse(b);
 b=maximum;b.maintenance_window_policy_uuid=b.donor_mapping_uuid;Refuse(b);
 b=base;b.origin_transaction_uuid.kind=p::UuidKind::object;Refuse(b,false);
 b=base;b.creating_transaction_uuid.kind=p::UuidKind::object;Refuse(b,false);
 const auto raw=Golden(maximum);std::array<p::byte,c::kCatalogLockPolicyMaxBytes> out;out.fill(0xab);const auto before=out;
 for(std::size_t n=0;n<raw.size();++n){Check(!NoHeap([&]{return c::DecodeCatalogLockPolicy(std::string_view(raw).substr(0,n));}).ok(),"every truncated wire refuses");
   const auto encoded=NoHeap([&]{return c::EncodeCatalogLockPolicyInto(maximum,std::span(out).first(n));});
   Check(!encoded.ok()&&encoded.bytes.empty()&&out==before,"short output never writes a successful prefix");}
 for(const unsigned offset:{0u,4u,6u,8u,12u,16u,20u,22u,24u,26u,27u,28u}){auto invalid=raw;invalid[offset]^=0x7f;
   Check(!NoHeap([&]{return c::DecodeCatalogLockPolicy(invalid);}).ok(),"invalid framing/schema/type/tag refused");}
 auto malformed=raw;bool found=false;
 for(std::size_t pos=24;pos<malformed.size();){const auto* bytes=reinterpret_cast<const p::byte*>(malformed.data()+pos);
   const auto tag=p::LoadLittle16(bytes);const auto length=p::LoadLittle32(bytes+4);
   if(tag==18){malformed[pos+8]=2;found=true;break;}pos+=8+length;}
 Check(found&&!c::DecodeCatalogLockPolicy(malformed).ok(),"conversion priority refuses noncanonical boolean");
 auto trailing=raw;trailing+='x';Check(!c::DecodeCatalogLockPolicy(trailing).ok(),"trailing bytes refused");
 auto overlap=base;Check(NoHeap([&]{return c::EncodeCatalogLockPolicyInto(overlap,{reinterpret_cast<p::byte*>(&overlap),sizeof(overlap)});}).error==c::CatalogValueError::invalid_backing&&Golden(overlap)==Golden(base),"encoding cannot overwrite its actual input object");
}
void Bindings(){auto b=Value();auto m=Metadata(b);
 Check(c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(m)).has_value(),"common dispatcher accepts exact lock metadata");
 for(unsigned n=0;n<5;++n){auto bad=m;if(n==0)bad.record.header.object_uuid.value=f::Id(8);if(n==1)bad.record.header.row_uuid.value=b.evidence_policy_uuid;
   if(n==2)bad.definition_version=2;if(n==3)bad.creator_local_transaction_id=2;if(n==4)bad.object_subtype="generic_policy";
   Check(!c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(bad)),"common family cannot bypass header/creator/generation/subtype checks");}
 Check(!c::CatalogLockPolicyMatchesDatabase(c::BorrowCatalogMetadataVersion(m),f::Id(9)),"actual database binding required");
 c::CatalogPolicyDescriptor d;d.policy_uuid=f::Id(500);d.policy_version_uuid=f::Id(501);d.database_uuid=b.database_uuid;
 d.body_uuid=b.body_uuid;d.body_generation=d.generation=1;d.policy_class=c::CatalogPolicyClass::lock_admission;
 d.creating_transaction_number=d.origin_local_transaction_id=1;d.creating_transaction_uuid=d.origin_transaction_uuid=b.origin_transaction_uuid;
 d.scope=c::CatalogPolicyScope::database;d.scope_uuid=b.database_uuid;d.authority_owner_uuid=f::Id(50);d.precedence_rank=1;d.enforcement_points=1;
 Check(c::CatalogLockPolicyMatchesDescriptor(b,d),"actual lock descriptor selects exact declaration");
 ++d.body_generation;Check(!c::CatalogLockPolicyMatchesDescriptor(b,d),"wrong body generation refuses");--d.body_generation;
 d.policy_class=c::CatalogPolicyClass::transaction;Check(!c::CatalogLockPolicyMatchesDescriptor(b,d),"wrong policy class refuses");
 Check(c::CatalogMetadataPreservesFamilyOrigin(m,m),"exact same-generation replay");
 auto changed=b;++changed.maximum_locks;Check(!c::CatalogMetadataPreservesFamilyOrigin(m,Metadata(changed)),"same-generation behavior immutable");
 changed.generation=2;changed.creating_transaction_uuid.value=f::Id(8);changed.creating_local_transaction_id=2;
 Check(c::CatalogMetadataPreservesFamilyOrigin(m,Metadata(changed)),"new generation retains original provenance");
 changed.origin_transaction_uuid=changed.creating_transaction_uuid;changed.origin_local_transaction_id=2;
 Check(!c::CatalogMetadataPreservesFamilyOrigin(m,Metadata(changed)),"new generation cannot replace origin");
 b=Wait(9);b.cluster_uuid=f::Id(7);b.donor_mapping_uuid=f::Id(402);std::vector<p::Uuid> reserved;
 db::ReserveNativeCreationCatalogReferences({Metadata(b),{}},[&](const auto& id){reserved.push_back(id);});
 for(const auto& id:{b.body_uuid,b.database_uuid,b.evidence_policy_uuid,*b.maintenance_window_policy_uuid,*b.donor_mapping_uuid,*b.cluster_uuid,b.origin_transaction_uuid.value,b.creating_transaction_uuid.value})
   Check(std::find(reserved.begin(),reserved.end(),id)!=reserved.end(),"native issuer excludes every policy reference");
}
struct Item {
  c::CatalogLockPolicyBody descriptor;
  c::CatalogMetadataVersion definition,vector_metadata,entry_metadata;
  c::CatalogNameVector vector;c::CatalogNameEntry entry;
  std::array<c::CatalogCreationNameEntryInput,1> entries;
  explicit Item(unsigned n):descriptor(Value(n)),definition(Metadata(descriptor,n)){
    vector_metadata=entry_metadata=definition;vector_metadata.record.payload.clear();entry_metadata.record.payload.clear();
    vector_metadata.record.header.kind=entry_metadata.record.header.kind=c::CatalogRecordKind::localized_name;
    vector_metadata.record.header.object_uuid=definition.name_vector_uuid;vector_metadata.record.header.row_uuid.value=f::Id(1001+3*n);
    vector_metadata.record.header.parent_uuid=definition.record.header.object_uuid;vector_metadata.object_subtype="name_vector";
    entry_metadata.record.header.object_uuid=definition.default_name_uuid;entry_metadata.record.header.row_uuid.value=f::Id(1002+3*n);
    entry_metadata.record.header.parent_uuid=definition.name_vector_uuid;entry_metadata.object_subtype="name_entry";entry_metadata.resource_epoch=1;
    vector.name_vector_uuid=entry.name_vector_uuid=definition.name_vector_uuid;vector.object_uuid=entry.object_uuid=definition.record.header.object_uuid;
    vector.default_name_entry_uuid=entry.name_entry_uuid=definition.default_name_uuid;vector.object_kind=entry.object_kind=c::CatalogRecordKind::policy;
    vector.owning_schema_uuid=definition.owning_schema_uuid;vector.default_language_tag="en-US";
    vector.name_collision_policy_uuid={p::UuidKind::object,f::Id(55)};vector.catalog_generation_id=1;
    vector.security_policy_uuid=definition.security_policy_uuid;vector.lifecycle_state=c::CatalogNameLifecycle::active;
    entry.scope_uuid=definition.record.header.parent_uuid;entry.parent_schema_uuid=definition.owning_schema_uuid;entry.language_tag="en-US";
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
  const auto made=c::AssembleLocalNamedCreationRecords(f::Id(1),item.descriptor.creating_transaction_uuid,"en-US",inputs,{10,1000000});
  Check(made.ok()&&made.records->definitions.size()==1&&made.records->names.size()==2,"typed body and complete supplied names assemble");
  db::NativeCreationCatalogSeed seed;seed[4]=made.records->definitions;seed[1]=made.records->names;
  db::NativeFilespaceInitializationRequest request;request.bootstrap={f::Id(1),f::Id(2),profile.uuid,d::kNativeBootstrapIntegrityProfile,{},profile.page_size_bytes,1,0,1,7};
  request.operation_uuid=f::Id(3);request.writer_uuid=f::Id(4);request.policy_snapshot_uuid=f::Id(10);
  request.creator.transaction_uuid=item.descriptor.creating_transaction_uuid;request.creator.local_id=mga::MakeLocalTransactionId(1);request.creator.scope=mga::TransactionScope::local_node;
  request.total_pages=64;request.creation_utc_millis=1789357072000ULL;const auto budget=128*p::u64{profile.page_size_bytes};u::StandaloneUuidV7Issuer issuer({f::Id(1),f::Id(10)},{{},0,1000});
  const auto path=(artifacts.path()/std::to_string(profile.page_size_bytes)).string();d::FileDevice device;
  Check(device.Open(path,d::FileOpenMode::create_new).ok(),"real native lock policy file");auto foreign=item.descriptor;foreign.database_uuid=f::Id(9);seed[4][0].metadata.record.payload=Golden(foreign);
  Check(!db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,request,seed,budget,issuer).ok()&&device.Size().size_bytes==0,"foreign body refuses before physical effects");
  seed[4][0].metadata.record.payload=Golden(item.descriptor);const auto created=db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,request,seed,budget,issuer);
  Check(created.ok(),"real selected native lock policy persists");Check(device.Close().ok()&&device.Open(path,d::FileOpenMode::open_existing).ok(),"independent reopen");
  const std::vector<d::NativeFilespaceDevice> devices{{f::Id(2),profile.uuid,&device}};
  const auto rows=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(f::Id(1),devices,db::NativeCatalogSource::PageZero(created.receipt->filespace_uuid),2,5,{created.receipt->relations[4].object_uuid,{}},budget);
  Check(rows.ok()&&rows.rows.size()==1&&rows.rows[0].metadata.record.payload==Golden(item.descriptor)&&
    c::CatalogLockPolicyMatchesDatabase(c::BorrowCatalogMetadataVersion(rows.rows[0].metadata),f::Id(1)),"exact isolation, limits, behavior and origin fields survive native reopen");
  const auto zero=d::ReadFilespacePageZeroFromOpenDevice(device);Check(zero.ok()&&zero.record->bootstrap.lifecycle_state==1,"body persistence is not node activation: page zero stays as the create wrote it, an online filespace (owner 2026-11-07); serving is the server's act at open");
  Check(device.Close().ok(),"owned evidence body file closed");
 }}
}
int main(){try{Values();Bindings();Physical();std::cout<<checks<<" lock policy checks passed (declarations only)\n";return 0;}
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<e.what()<<'\n';return 1;}}
