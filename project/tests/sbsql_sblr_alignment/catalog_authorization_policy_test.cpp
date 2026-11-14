// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_authorization_policy.hpp"
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
using Body=c::CatalogAuthorizationPolicyBody;
unsigned checks=0;
void Check(bool ok,const char* why,std::source_location loc=std::source_location::current()){++checks;
 if(!ok){allocation_budget=-1;throw std::runtime_error(std::string(why)+" line="+std::to_string(loc.line()));}}
template<class Call>auto NoHeap(Call&& call){allocation_budget=0;allocation_count=0;auto result=call();const auto calls=allocation_count;allocation_budget=-1;
 Check(!calls,"bounded authorization policy codecs never allocate");return result;}
Body Value(unsigned n=0){Body b;b.body_uuid=f::Id(100+n);b.database_uuid=f::Id(1);b.generation=1;
 b.user_schema_uuid=f::Id(60);b.role_schema_uuid=f::Id(61);b.group_schema_uuid=f::Id(62);
 b.grant_schema_uuid=f::Id(63);b.row_policy_schema_uuid=f::Id(64);b.column_policy_schema_uuid=f::Id(65);
 b.maximum_records=4096;b.maximum_membership_depth=64;b.maximum_work=1048576;
 b.origin_transaction_uuid=b.creating_transaction_uuid={p::UuidKind::transaction,f::Id(5)};
 b.origin_local_transaction_id=b.creating_local_transaction_id=1;return b;}
std::string Golden(const Body& r){f::Golden g(65623);
 g.Identity(1,r.body_uuid);g.Identity(2,r.database_uuid);g.Integer(3,r.generation);
 g.Identity(4,r.user_schema_uuid);g.Identity(5,r.role_schema_uuid);g.Identity(6,r.group_schema_uuid);
 g.Integer(7,p::u64(r.external_mapping));if(r.external_mapping_schema_uuid)g.Identity(8,*r.external_mapping_schema_uuid);
 g.Identity(9,r.grant_schema_uuid);g.Identity(10,r.row_policy_schema_uuid);g.Identity(11,r.column_policy_schema_uuid);
 g.Integer(12,r.cache_lifetime_microseconds);g.Integer(13,r.reload_trigger_mask);g.Integer(14,p::u64(r.stale_behavior));
 g.Integer(15,r.maximum_stale_microseconds);g.Integer(16,p::u64(r.failure_behavior));g.Integer(17,r.maximum_records);
 g.Integer(18,r.maximum_membership_depth);g.Integer(19,r.maximum_work);g.Integer(20,r.flags);
 g.Identity(21,r.origin_transaction_uuid.value);g.Integer(22,r.origin_local_transaction_id);
 g.Identity(23,r.creating_transaction_uuid.value);g.Integer(24,r.creating_local_transaction_id);
 if(r.cluster_uuid)g.Identity(25,*r.cluster_uuid);return g.Finish();}
c::CatalogMetadataVersion Metadata(const Body& r,unsigned n=0){auto m=f::Metadata(0,r.creating_transaction_uuid,f::Id(60),Golden(r));
 m.record.header.object_uuid={p::UuidKind::object,r.body_uuid};m.record.header.row_uuid={p::UuidKind::row,f::Id(1000+3*n)};
 m.default_name_uuid={p::UuidKind::object,f::Id(2000+2*n)};m.name_vector_uuid={p::UuidKind::object,f::Id(2001+2*n)};
 m.object_subtype=scratchbird::core::catalog::CatalogObjectSubtype::authorization_policy_body;m.creator_local_transaction_id=r.creating_local_transaction_id;m.definition_version=r.generation;
 if(r.cluster_uuid)m.authority_scope=c::CatalogAuthorityScope::cluster;return m;}
void Roundtrip(const Body& value){const auto expected=Golden(value);std::array<p::byte,c::kCatalogAuthorizationPolicyMaxBytes+2> output;output.fill(0xa7);
 const auto size=NoHeap([&]{return c::CatalogAuthorizationPolicyEncodedBytes(value);});Check(size&&*size==expected.size()&&*size<=c::kCatalogAuthorizationPolicyMaxBytes,"independent complete byte count");
 const auto encoded=NoHeap([&]{return c::EncodeCatalogAuthorizationPolicyInto(value,std::span(output).subspan(1));});
 Check(encoded.ok()&&std::string_view(reinterpret_cast<const char*>(encoded.bytes.data()),encoded.bytes.size())==expected,"exact independent SBCV fields");
 Check(output.front()==0xa7&&std::all_of(output.begin()+expected.size()+1,output.end(),[](auto b){return b==0xa7;}),"unaligned destination preserves guards");
 const auto decoded=NoHeap([&]{return c::DecodeCatalogAuthorizationPolicy(expected);});Check(decoded.ok()&&Golden(*decoded.record)==expected,"all decoded authorization fields retained");}
void Refuse(const Body& value,bool wire=true){std::array<p::byte,c::kCatalogAuthorizationPolicyMaxBytes> output;output.fill(0xa7);const auto before=output;
 const auto encoded=NoHeap([&]{return c::EncodeCatalogAuthorizationPolicyInto(value,output);});
 Check(!encoded.ok()&&encoded.bytes.empty()&&output==before&&!NoHeap([&]{return c::CatalogAuthorizationPolicyEncodedBytes(value);}),"invalid body leaves output unchanged");
 if(wire){const auto raw=Golden(value);const auto decoded=NoHeap([&]{return c::DecodeCatalogAuthorizationPolicy(raw);});Check(!decoded.ok()&&!decoded.record,"wire invalid body refused");}}
void Values(){const auto base=Value();Roundtrip(base);Check(Golden(base).size()==472,"exact standalone no-external body size");
 auto external=base;external.external_mapping=c::AuthorizationExternalMapping::internal_snapshot;
 external.external_mapping_schema_uuid=f::Id(66);external.cluster_uuid=f::Id(7);Roundtrip(external);
 Check(Golden(external).size()==520,"all optional native authorization fields have exact maximum size");
 external.stale_behavior=c::AuthorizationStaleBehavior::bounded_cached_snapshot;
 external.cache_lifetime_microseconds=60000000;external.maximum_stale_microseconds=1000000;Roundtrip(external);
 for(unsigned mask=0;mask<128;++mask){auto b=base;b.reload_trigger_mask=mask;
  if(mask<64&&(mask&12)==12)Roundtrip(b);else Refuse(b);}
 for(unsigned flags=0;flags<256;++flags){auto b=base;b.flags=flags;
  if(flags==63||flags==127)Roundtrip(b);else Refuse(b);}
 for(unsigned field=0;field<32;++field){auto b=base;switch(field){
  case 0:b.body_uuid={};break;case 1:b.database_uuid.bytes[6]=0x40;break;
  case 2:b.user_schema_uuid=b.body_uuid;break;case 3:b.generation=0;break;
  case 4:b.role_schema_uuid={};break;case 5:b.group_schema_uuid=b.database_uuid;break;
  case 6:b.grant_schema_uuid=b.origin_transaction_uuid.value;break;case 7:b.row_policy_schema_uuid={};break;
  case 8:b.column_policy_schema_uuid={};break;case 9:b.external_mapping=c::AuthorizationExternalMapping(0);break;
  case 10:b.external_mapping=c::AuthorizationExternalMapping(3);break;
  case 11:b.external_mapping=c::AuthorizationExternalMapping::internal_snapshot;break;
  case 12:b.external_mapping_schema_uuid=f::Id(66);break;
  case 13:b.stale_behavior=c::AuthorizationStaleBehavior(3);break;
  case 14:b.stale_behavior=c::AuthorizationStaleBehavior::bounded_cached_snapshot;break;
  case 15:b.maximum_stale_microseconds=1;break;case 16:b.failure_behavior=c::AuthorizationFailureBehavior(0);break;
  case 17:b.failure_behavior=c::AuthorizationFailureBehavior(3);break;case 18:b.maximum_records=0;break;
  case 19:b.maximum_membership_depth=0;break;case 20:b.maximum_membership_depth=b.maximum_records+1;break;
  case 21:b.maximum_work=0;break;case 22:b.origin_local_transaction_id=0;break;
  case 23:b.creating_local_transaction_id=2;break;case 24:b.creating_transaction_uuid.value=f::Id(8);break;
  case 25:b.cluster_uuid=b.database_uuid;break;case 26:b.cluster_uuid=b.body_uuid;break;
  case 27:b.cluster_uuid=b.user_schema_uuid;break;case 28:b.cluster_uuid=b.origin_transaction_uuid.value;break;
  case 29:b.external_mapping=external.external_mapping;b.external_mapping_schema_uuid=external.external_mapping_schema_uuid;b.reload_trigger_mask=12;break;
  case 30:b=external;b.cache_lifetime_microseconds=0;break;case 31:b=external;b.maximum_stale_microseconds=0;break;
 }Refuse(b);}
 auto domain=base;domain.origin_transaction_uuid.kind=p::UuidKind::object;Refuse(domain,false);
 domain=base;domain.creating_transaction_uuid.kind=p::UuidKind::object;Refuse(domain,false);
 auto shared=base;shared.role_schema_uuid=shared.group_schema_uuid=shared.grant_schema_uuid=shared.row_policy_schema_uuid=shared.column_policy_schema_uuid=shared.user_schema_uuid;Roundtrip(shared);
 auto maximum=external;maximum.generation=maximum.creating_local_transaction_id=maximum.maximum_records=maximum.maximum_membership_depth=maximum.maximum_work=UINT64_MAX;
 maximum.creating_transaction_uuid.value=f::Id(9);maximum.cache_lifetime_microseconds=maximum.maximum_stale_microseconds=UINT64_MAX;Roundtrip(maximum);
 auto disconnect=base;disconnect.failure_behavior=c::AuthorizationFailureBehavior::disconnect;Roundtrip(disconnect);
 const auto raw=Golden(external);std::array<p::byte,600> out;out.fill(0xab);const auto before=out;
 for(std::size_t n=0;n<raw.size();++n){Check(!NoHeap([&]{return c::DecodeCatalogAuthorizationPolicy(std::string_view(raw).substr(0,n));}).ok(),"all truncations refused");
  Check(!NoHeap([&]{return c::EncodeCatalogAuthorizationPolicyInto(external,std::span(out).first(n));}).ok()&&out==before,"all short destinations unchanged");}
 auto bad=raw;bad.push_back(0);Check(!c::DecodeCatalogAuthorizationPolicy(bad).ok(),"trailing bytes refused");
 for(unsigned at:{0u,4u,6u,8u,12u,16u,20u,24u,26u,28u}){bad=raw;bad[at]^=0x7f;Check(!c::DecodeCatalogAuthorizationPolicy(bad).ok(),"framing/schema/field corruption refused");}
 auto alias=external;const auto original=Golden(alias);
 Check(!NoHeap([&]{return c::EncodeCatalogAuthorizationPolicyInto(alias,{reinterpret_cast<p::byte*>(&alias),sizeof(alias)});}).ok()&&Golden(alias)==original,
   "whole input and output overlap refused before writes");
}
void Bindings(){auto b=Value();auto m=Metadata(b);auto view=c::BorrowCatalogMetadataVersion(m);
 Check(c::CatalogAuthorizationPolicyMatchesHeader(view.record)&&c::CatalogAuthorizationPolicyMatchesMetadata(view)&&!c::ValidateCatalogMetadataVersionView(view),"common dispatcher admits exact complete typed authorization body");
 Check(c::CatalogAuthorizationPolicyMatchesDatabase(view,f::Id(1))&&!c::CatalogAuthorizationPolicyMatchesDatabase(view,f::Id(2)),"actual database identity binding");
 for(unsigned field=0;field<6;++field){auto invalid=m;switch(field){case 0:invalid.object_subtype=scratchbird::core::catalog::CatalogObjectSubtype::policy_descriptor;break;
  case 1:invalid.record.header.kind=c::CatalogRecordKind::config_profile;break;case 2:invalid.definition_version++;break;
  case 3:invalid.creator_local_transaction_id++;break;case 4:invalid.creator_transaction_uuid.value=f::Id(9);break;
  case 5:invalid.authority_scope=c::CatalogAuthorityScope::cluster;break;}
  Check(c::ValidateCatalogMetadataVersionView(c::BorrowCatalogMetadataVersion(invalid)).has_value(),"dispatch rejects body/header/generation/creator/authority mismatch");}
 c::CatalogPolicyDescriptor d;d.policy_uuid=f::Id(700);d.policy_version_uuid=f::Id(701);d.database_uuid=b.database_uuid;
 d.body_uuid=b.body_uuid;d.body_generation=d.generation=1;d.policy_class=c::CatalogPolicyClass::authorization;
 d.creating_transaction_number=d.origin_local_transaction_id=1;d.origin_transaction_uuid=d.creating_transaction_uuid=b.creating_transaction_uuid;
 d.scope=c::CatalogPolicyScope::database;d.scope_uuid=b.database_uuid;d.authority_owner_uuid=f::Id(50);d.precedence_rank=1;d.enforcement_points=1;
 Check(NoHeap([&]{return c::CatalogAuthorizationPolicyMatchesDescriptor(b,d);}),"exact authorization descriptor selects complete typed body");
 for(unsigned field=0;field<5;++field){auto invalid=d;switch(field){case 0:invalid.body_uuid=f::Id(702);break;case 1:invalid.body_generation++;break;
  case 2:invalid.database_uuid=invalid.scope_uuid=f::Id(703);break;case 3:invalid.policy_class=c::CatalogPolicyClass::authentication;break;case 4:invalid.cluster_uuid=f::Id(8);break;}
  Check(!c::CatalogAuthorizationPolicyMatchesDescriptor(b,invalid),"wrong body tuple/class/database/cluster cannot bind");}
 auto newer=b;newer.generation=2;newer.creating_transaction_uuid.value=f::Id(7);newer.creating_local_transaction_id=2;
 Check(c::CatalogMetadataPreservesFamilyOrigin(m,Metadata(newer)),"higher semantic generation retains actual origin");
 auto same=b;same.maximum_work++;Check(!c::CatalogMetadataPreservesFamilyOrigin(m,Metadata(same)),"same-generation body bytes immutable");
 newer.origin_transaction_uuid=newer.creating_transaction_uuid;newer.origin_local_transaction_id=2;
 Check(!c::CatalogMetadataPreservesFamilyOrigin(m,Metadata(newer)),"new generation cannot replace original creator");
 b.external_mapping=c::AuthorizationExternalMapping::internal_snapshot;b.external_mapping_schema_uuid=f::Id(66);b.cluster_uuid=f::Id(8);
 std::vector<p::Uuid> reserved;db::ReserveNativeCreationCatalogReferences({Metadata(b),{}},[&](const auto& id){reserved.push_back(id);});
 for(const auto& id:{b.body_uuid,b.database_uuid,b.user_schema_uuid,b.role_schema_uuid,b.group_schema_uuid,b.grant_schema_uuid,b.row_policy_schema_uuid,b.column_policy_schema_uuid,*b.external_mapping_schema_uuid,*b.cluster_uuid,b.origin_transaction_uuid.value,b.creating_transaction_uuid.value})
  Check(std::find(reserved.begin(),reserved.end(),id)!=reserved.end(),"native issuer excludes every actual authorization reference");
}
struct Item {
  c::CatalogAuthorizationPolicyBody descriptor;
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
  Check(device.Open(path,d::FileOpenMode::create_new,d::DatabaseFileOwnership::native_data_file).ok(),"real native authorization policy file");auto foreign=item.descriptor;foreign.database_uuid=f::Id(9);seed[4][0].metadata.record.payload=Golden(foreign);
  Check(!db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,request,seed,budget,issuer).ok()&&device.Size().size_bytes==0,"foreign body refuses before physical effects");
  seed[4][0].metadata.record.payload=Golden(item.descriptor);const auto created=db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,request,seed,budget,issuer);
  Check(created.ok(),"real selected native authorization policy persists");Check(device.Close().ok()&&device.Open(path,d::FileOpenMode::open_existing,d::DatabaseFileOwnership::native_data_file).ok(),"independent reopen");
  const std::vector<d::NativeFilespaceDevice> devices{{f::Id(2),profile.uuid,&device}};
  const auto rows=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(f::Id(1),devices,db::NativeCatalogSource::PageZero(created.receipt->filespace_uuid),2,5,{created.receipt->relations[4].object_uuid,{}},budget);
  Check(rows.ok()&&rows.rows.size()==1&&rows.rows[0].metadata.record.payload==Golden(item.descriptor)&&
    c::CatalogAuthorizationPolicyMatchesDatabase(c::BorrowCatalogMetadataVersion(rows.rows[0].metadata),f::Id(1)),"exact mapping, cache, failure and origin fields survive native reopen");
  const auto zero=d::ReadFilespacePageZeroFromOpenDevice(device);Check(zero.ok()&&zero.record->bootstrap.lifecycle_state==1,"body persistence is not node activation: page zero stays as the create wrote it, an online filespace (owner 2026-11-07); serving is the server's act at open");
  Check(device.Close().ok(),"owned evidence body file closed");
 }}
}
int main(){try{Values();Bindings();Physical();std::cout<<checks<<" authorization policy checks passed (declarations only)\n";return 0;}
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<e.what()<<'\n';return 1;}}
