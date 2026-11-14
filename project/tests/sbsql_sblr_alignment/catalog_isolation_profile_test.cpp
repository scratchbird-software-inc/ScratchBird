// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_isolation_profile.hpp"
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
using Body=c::CatalogIsolationProfile;
unsigned checks=0;
void Check(bool ok,const char* why,std::source_location loc=std::source_location::current()){++checks;
 if(!ok){allocation_budget=-1;throw std::runtime_error(std::string(why)+" line="+std::to_string(loc.line()));}}
template<class Call>auto NoHeap(Call&& call){allocation_budget=0;allocation_count=0;auto result=call();const auto calls=allocation_count;allocation_budget=-1;
 Check(!calls,"bounded isolation profile codecs never allocate");return result;}
Body Value(unsigned n=0){Body b;b.profile_uuid=f::Id(100+n);b.database_uuid=f::Id(1);b.generation=1;
 b.profile_kind=c::IsolationProfileKind::maintenance_operation_isolation;
 b.snapshot_construction_point=c::IsolationSnapshotPoint::operation_start;b.snapshot_refresh_mask=1;
 b.write_conflict_detection_mask=3;b.wait_policy_uuid=b.deadlock_policy_uuid=f::Id(400);
 b.metadata_visibility=c::IsolationMetadataVisibility::transaction_epoch;
 b.origin_transaction_uuid=b.creating_transaction_uuid={p::UuidKind::transaction,f::Id(5)};
 b.origin_local_transaction_id=b.creating_local_transaction_id=1;return b;}
std::string Golden(const Body& r){f::Golden g(65625);
 g.Identity(1,r.profile_uuid);g.Identity(2,r.database_uuid);g.Integer(3,r.generation);
 g.Integer(4,p::u64(r.profile_kind));g.Integer(5,p::u64(r.snapshot_construction_point));g.Integer(6,r.snapshot_refresh_mask);
 g.Integer(7,p::u64(r.own_write_visibility));g.Integer(8,p::u64(r.active_other_visibility));g.Integer(9,p::u64(r.limbo_visibility));
 g.Integer(10,r.write_conflict_detection_mask);g.Identity(11,r.wait_policy_uuid);g.Identity(12,r.deadlock_policy_uuid);
 g.Integer(13,p::u64(r.predicate_validation));g.Integer(14,p::u64(r.unique_validation));
 g.Integer(15,p::u64(r.metadata_visibility));g.Integer(16,p::u64(r.cluster_participant_mode));
 if(r.donor_mapping_uuid)g.Identity(17,*r.donor_mapping_uuid);
 g.Identity(18,r.origin_transaction_uuid.value);g.Integer(19,r.origin_local_transaction_id);
 g.Identity(20,r.creating_transaction_uuid.value);g.Integer(21,r.creating_local_transaction_id);
 if(r.cluster_uuid)g.Identity(22,*r.cluster_uuid);return g.Finish();}
c::CatalogMetadataVersion Metadata(const Body& r,unsigned n=0){auto m=f::Metadata(0,r.creating_transaction_uuid,f::Id(60),Golden(r));
 m.record.header.object_uuid={p::UuidKind::object,r.profile_uuid};m.record.header.row_uuid={p::UuidKind::row,f::Id(1000+3*n)};
 m.default_name_uuid={p::UuidKind::object,f::Id(2000+2*n)};m.name_vector_uuid={p::UuidKind::object,f::Id(2001+2*n)};
 m.object_subtype=scratchbird::core::catalog::CatalogObjectSubtype::isolation_profile;m.creator_local_transaction_id=r.creating_local_transaction_id;m.definition_version=r.generation;
 if(r.cluster_uuid)m.authority_scope=c::CatalogAuthorityScope::cluster;return m;}
void Roundtrip(const Body& value){const auto expected=Golden(value);std::array<p::byte,c::kCatalogIsolationProfileMaxBytes+2> output;output.fill(0xa7);
 const auto size=NoHeap([&]{return c::CatalogIsolationProfileEncodedBytes(value);});Check(size&&*size==expected.size()&&*size<=c::kCatalogIsolationProfileMaxBytes,"independent complete byte count");
 const auto encoded=NoHeap([&]{return c::EncodeCatalogIsolationProfileInto(value,std::span(output).subspan(1));});
 Check(encoded.ok()&&std::string_view(reinterpret_cast<const char*>(encoded.bytes.data()),encoded.bytes.size())==expected,"exact independent SBCV transaction fields");
 Check(output.front()==0xa7&&std::all_of(output.begin()+expected.size()+1,output.end(),[](auto b){return b==0xa7;}),"unaligned destination preserves guards");
 const auto decoded=NoHeap([&]{return c::DecodeCatalogIsolationProfile(expected);});Check(decoded.ok()&&Golden(*decoded.record)==expected,"all isolation profile fields retained");}
void Refuse(const Body& value,bool wire=true){std::array<p::byte,c::kCatalogIsolationProfileMaxBytes> output;output.fill(0xa7);const auto before=output;
 const auto encoded=NoHeap([&]{return c::EncodeCatalogIsolationProfileInto(value,output);});
 Check(!encoded.ok()&&encoded.bytes.empty()&&output==before&&!NoHeap([&]{return c::CatalogIsolationProfileEncodedBytes(value);}),"invalid body leaves output unchanged");
 if(wire){const auto raw=Golden(value);const auto decoded=NoHeap([&]{return c::DecodeCatalogIsolationProfile(raw);});Check(!decoded.ok()&&!decoded.record,"invalid isolation profile bytes refused");}}

Body Profile(unsigned kind){auto b=Value();b.profile_kind=c::IsolationProfileKind(kind);
 if(kind==1)b.donor_mapping_uuid=f::Id(401);
 if(kind==2||kind==3){b.snapshot_construction_point=c::IsolationSnapshotPoint::statement_start;b.snapshot_refresh_mask=2;b.metadata_visibility=c::IsolationMetadataVisibility::statement_epoch;}
 if(kind>=4&&kind<=7){b.snapshot_construction_point=c::IsolationSnapshotPoint::transaction_start;}
 if(kind==6||kind==7){b.predicate_validation=c::IsolationPredicateValidation::serializable_required;b.write_conflict_detection_mask|=16;}
 if(kind==8){b.snapshot_construction_point=c::IsolationSnapshotPoint::historical_boundary;b.metadata_visibility=c::IsolationMetadataVisibility::historical_epoch;}
 if(kind==11){b.cluster_uuid=f::Id(7);b.cluster_participant_mode=c::IsolationClusterMode::cluster_bound;
   b.snapshot_construction_point=c::IsolationSnapshotPoint::cluster_participant_start;b.metadata_visibility=c::IsolationMetadataVisibility::cluster_epoch;}
 return b;
}
void Values(){const auto base=Value();Roundtrip(base);Check(Golden(base).size()==392,"standalone complete profile byte count");
 for(unsigned kind=1;kind<=11;++kind)Roundtrip(Profile(kind));
 auto maximum=Profile(11);maximum.donor_mapping_uuid=f::Id(401);Roundtrip(maximum);
 Check(Golden(maximum).size()==440,"all optional identities match registered maximum");
 for(unsigned mask=0;mask<256;++mask){auto b=base;b.snapshot_refresh_mask=mask;
   if(mask&&mask<=127&&(!(mask&1)||mask==1)&&!(mask&64))Roundtrip(b);else Refuse(b);
 }
 for(unsigned mask=0;mask<64;++mask){auto b=base;b.write_conflict_detection_mask=mask;if(mask&&mask<=31)Roundtrip(b);else Refuse(b);}
 for(unsigned kind=1;kind<=11;++kind){auto b=Profile(kind);b.active_other_visibility=c::IsolationActiveOthers::diagnostic_only;
   if(kind==1||kind==9)Roundtrip(b);else Refuse(b);
   b.active_other_visibility=c::IsolationActiveOthers::compatibility_view_only;
   if(kind==1||kind==9)Roundtrip(b);else Refuse(b);
 }
 for(unsigned field=0;field<34;++field){auto b=base;switch(field){
   case 0:b.profile_uuid={};break;case 1:b.database_uuid.bytes[6]=0x40;break;case 2:b.generation=0;break;
   case 3:b.profile_kind=c::IsolationProfileKind(0);break;case 4:b.profile_kind=c::IsolationProfileKind(12);break;
   case 5:b.snapshot_construction_point=c::IsolationSnapshotPoint(0);break;case 6:b.snapshot_construction_point=c::IsolationSnapshotPoint(8);break;
   case 7:b.own_write_visibility=c::IsolationOwnWrites(0);break;case 8:b.own_write_visibility=c::IsolationOwnWrites(4);break;
   case 9:b.active_other_visibility=c::IsolationActiveOthers(0);break;case 10:b.active_other_visibility=c::IsolationActiveOthers(4);break;
   case 11:b.limbo_visibility=c::IsolationLimbo(0);break;case 12:b.limbo_visibility=c::IsolationLimbo(5);break;
   case 13:b.wait_policy_uuid={};break;case 14:b.deadlock_policy_uuid={};break;
   case 15:b.predicate_validation=c::IsolationPredicateValidation(0);break;case 16:b.predicate_validation=c::IsolationPredicateValidation(5);break;
   case 17:b.unique_validation=c::IsolationUniqueValidation(0);break;case 18:b.unique_validation=c::IsolationUniqueValidation(5);break;
   case 19:b.metadata_visibility=c::IsolationMetadataVisibility(0);break;case 20:b.metadata_visibility=c::IsolationMetadataVisibility(6);break;
   case 21:b.cluster_participant_mode=c::IsolationClusterMode(0);break;case 22:b.cluster_participant_mode=c::IsolationClusterMode(5);break;
   case 23:b.donor_mapping_uuid=p::Uuid{};break;case 24:b.cluster_uuid=p::Uuid{};break;
   case 25:b.origin_local_transaction_id=0;break;case 26:b.creating_local_transaction_id=2;break;
   case 27:b.creating_transaction_uuid.value=f::Id(8);break;case 28:b.wait_policy_uuid=b.profile_uuid;break;
   case 29:b.deadlock_policy_uuid=b.database_uuid;break;case 30:b.donor_mapping_uuid=b.wait_policy_uuid;break;
   case 31:b.cluster_uuid=b.origin_transaction_uuid.value;break;case 32:b.unique_validation=c::IsolationUniqueValidation::donor_profile;break;
   case 33:b.unique_validation=c::IsolationUniqueValidation::cluster_authority;break;
 }Refuse(b);}
 for(unsigned kind=2;kind<=8;++kind){auto b=Profile(kind);b.own_write_visibility=c::IsolationOwnWrites::hidden;if(kind<=7)Refuse(b);
   b=Profile(kind);b.snapshot_construction_point=c::IsolationSnapshotPoint::fetch;Refuse(b);
   b=Profile(kind);b.snapshot_refresh_mask=4;Refuse(b);
   b=Profile(kind);b.metadata_visibility=c::IsolationMetadataVisibility::historical_epoch;if(kind!=8)Refuse(b);
 }
 for(unsigned kind:{6u,7u}){auto b=Profile(kind);b.predicate_validation=c::IsolationPredicateValidation::none;Refuse(b);
   b=Profile(kind);b.write_conflict_detection_mask&=~p::u64{16};Refuse(b);}
 auto b=Profile(7);b.predicate_validation=c::IsolationPredicateValidation::declared_sets;Refuse(b);
 b=Profile(1);b.donor_mapping_uuid.reset();Refuse(b);
 b=Profile(3);b.snapshot_refresh_mask=10;b.metadata_visibility=c::IsolationMetadataVisibility::cursor_epoch;Roundtrip(b);
 for(unsigned n=0;n<4;++n){b=Profile(11);if(n==0)b.cluster_uuid.reset();if(n==1)b.cluster_participant_mode=c::IsolationClusterMode::local_until_cluster_write;
   if(n==2)b.snapshot_construction_point=c::IsolationSnapshotPoint::operation_start;if(n==3)b.metadata_visibility=c::IsolationMetadataVisibility::transaction_epoch;Refuse(b);}
 b=base;b.origin_transaction_uuid.kind=p::UuidKind::object;Refuse(b,false);
 b=base;b.creating_transaction_uuid.kind=p::UuidKind::object;Refuse(b,false);
 const auto raw=Golden(maximum);std::array<p::byte,c::kCatalogIsolationProfileMaxBytes> out;out.fill(0xab);const auto before=out;
 for(std::size_t n=0;n<raw.size();++n){Check(!NoHeap([&]{return c::DecodeCatalogIsolationProfile(std::string_view(raw).substr(0,n));}).ok(),"every truncated wire refuses");
   const auto encoded=NoHeap([&]{return c::EncodeCatalogIsolationProfileInto(maximum,std::span(out).first(n));});
   Check(!encoded.ok()&&encoded.bytes.empty()&&out==before,"short output never writes a successful prefix");}
 for(const unsigned offset:{0u,4u,6u,8u,12u,16u,20u,22u,24u,26u,27u,28u}){auto invalid=raw;invalid[offset]^=0x7f;
   Check(!NoHeap([&]{return c::DecodeCatalogIsolationProfile(invalid);}).ok(),"invalid framing/schema/type/tag refused");}
 auto trailing=raw;trailing+='x';Check(!c::DecodeCatalogIsolationProfile(trailing).ok(),"trailing bytes refused");
 auto overlap=base;Check(NoHeap([&]{return c::EncodeCatalogIsolationProfileInto(overlap,{reinterpret_cast<p::byte*>(&overlap),sizeof(overlap)});}).error==c::CatalogValueError::invalid_backing&&Golden(overlap)==Golden(base),"encoding cannot overwrite its actual input object");
}
void Bindings(){auto b=Value();auto m=Metadata(b);
 Check(c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(m)).has_value(),"common dispatcher accepts exact isolation metadata");
 for(unsigned n=0;n<5;++n){auto bad=m;if(n==0)bad.record.header.object_uuid.value=f::Id(8);if(n==1)bad.record.header.row_uuid.value=b.wait_policy_uuid;
   if(n==2)bad.definition_version=2;if(n==3)bad.creator_local_transaction_id=2;if(n==4)bad.object_subtype=scratchbird::core::catalog::CatalogObjectSubtype::schema_application;
   Check(!c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(bad)),"common family cannot bypass header/creator/generation/subtype checks");}
 Check(!c::CatalogIsolationProfileMatchesDatabase(c::BorrowCatalogMetadataVersion(m),f::Id(9)),"actual database binding required");
 Check(c::CatalogMetadataPreservesFamilyOrigin(m,m),"exact same-generation replay");
 auto changed=b;changed.write_conflict_detection_mask=7;Check(!c::CatalogMetadataPreservesFamilyOrigin(m,Metadata(changed)),"same-generation behavior immutable");
 changed.generation=2;changed.creating_transaction_uuid.value=f::Id(8);changed.creating_local_transaction_id=2;
 Check(c::CatalogMetadataPreservesFamilyOrigin(m,Metadata(changed)),"new generation retains original provenance");
 changed.origin_transaction_uuid=changed.creating_transaction_uuid;changed.origin_local_transaction_id=2;
 Check(!c::CatalogMetadataPreservesFamilyOrigin(m,Metadata(changed)),"new generation cannot replace origin");
 b=Profile(11);b.donor_mapping_uuid=f::Id(401);std::vector<p::Uuid> reserved;
 db::ReserveNativeCreationCatalogReferences({Metadata(b),{}},[&](const auto& id){reserved.push_back(id);});
 for(const auto& id:{b.profile_uuid,b.database_uuid,b.wait_policy_uuid,b.deadlock_policy_uuid,*b.donor_mapping_uuid,*b.cluster_uuid,b.origin_transaction_uuid.value,b.creating_transaction_uuid.value})
   Check(std::find(reserved.begin(),reserved.end(),id)!=reserved.end(),"native issuer excludes every profile reference");
}
struct Item {
  c::CatalogIsolationProfile descriptor;
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
  Check(device.Open(path,d::FileOpenMode::create_new,d::DatabaseFileOwnership::native_data_file).ok(),"real native isolation profile file");auto foreign=item.descriptor;foreign.database_uuid=f::Id(9);seed[4][0].metadata.record.payload=Golden(foreign);
  Check(!db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,request,seed,budget,issuer).ok()&&device.Size().size_bytes==0,"foreign body refuses before physical effects");
  seed[4][0].metadata.record.payload=Golden(item.descriptor);const auto created=db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,request,seed,budget,issuer);
  Check(created.ok(),"real selected native isolation profile persists");Check(device.Close().ok()&&device.Open(path,d::FileOpenMode::open_existing,d::DatabaseFileOwnership::native_data_file).ok(),"independent reopen");
  const std::vector<d::NativeFilespaceDevice> devices{{f::Id(2),profile.uuid,&device}};
  const auto rows=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(f::Id(1),devices,db::NativeCatalogSource::PageZero(created.receipt->filespace_uuid),2,5,{created.receipt->relations[4].object_uuid,{}},budget);
  Check(rows.ok()&&rows.rows.size()==1&&rows.rows[0].metadata.record.payload==Golden(item.descriptor)&&
    c::CatalogIsolationProfileMatchesDatabase(c::BorrowCatalogMetadataVersion(rows.rows[0].metadata),f::Id(1)),"exact isolation, limits, behavior and origin fields survive native reopen");
  const auto zero=d::ReadFilespacePageZeroFromOpenDevice(device);Check(zero.ok()&&zero.record->bootstrap.lifecycle_state==1,"body persistence is not node activation: page zero stays as the create wrote it, an online filespace (owner 2026-11-07); serving is the server's act at open");
  Check(device.Close().ok(),"owned evidence body file closed");
 }}
}
int main(){try{Values();Bindings();Physical();std::cout<<checks<<" isolation profile checks passed (declarations only)\n";return 0;}
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<e.what()<<'\n';return 1;}}
