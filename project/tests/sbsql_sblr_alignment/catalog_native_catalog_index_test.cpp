// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// SB_CLARIFY_NATIVE_CATALOG_INDEX_PROFILE_ROWS (owner decision 2026-11-04,
// option a): the key profile, visibility profile and dependency map rows behind
// a native catalog relation's index binding. Codec, common-dispatcher binding,
// issuer reservation and real native persistence; the open-time comparison of
// stored index tuples against these rows is the engine's, tested separately.
#include "catalog_native_catalog_index.hpp"
#include "catalog_named_creation.hpp"
#include "native_creation_workspace.hpp"
#include "native_catalog_index_rows.hpp"
#include "hash_digest.hpp"
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
using Key=c::CatalogNativeCatalogIndexKeyProfile;using Visibility=c::CatalogNativeCatalogIndexVisibilityProfile;
using Map=c::CatalogNativeCatalogIndexDependencyMap;
unsigned checks=0;
void Check(bool ok,const char* why,std::source_location loc=std::source_location::current()){++checks;
 if(!ok){allocation_budget=-1;throw std::runtime_error(std::string(why)+" line="+std::to_string(loc.line()));}}
template<class Call>auto NoHeap(Call&& call){allocation_budget=0;allocation_count=0;auto result=call();const auto calls=allocation_count;allocation_budget=-1;
 Check(!calls,"bounded catalog index codecs never allocate");return result;}
const p::TypedUuid tx{p::UuidKind::transaction,f::Id(5)};
Key KeyValue(){Key r;r.profile_uuid=c::kNativeCatalogIndexKeyProfileUuid;r.database_uuid=f::Id(1);r.generation=1;
 r.key_form=c::NativeCatalogIndexKeyForm::raw16_canonical_row_uuid;r.key_order=c::NativeCatalogIndexKeyOrder::unsigned_canonical_bytes;
 r.tie_break=c::NativeCatalogIndexTieBreak::native_row_then_version_uuid;
 r.origin_transaction_uuid=r.creating_transaction_uuid=tx;r.origin_local_transaction_id=r.creating_local_transaction_id=1;return r;}
Visibility VisibilityValue(){Visibility r;r.profile_uuid=c::kNativeCatalogIndexVisibilityProfileUuid;r.database_uuid=f::Id(1);r.generation=1;
 r.visibility=c::NativeCatalogIndexVisibility::native_mga_inventory_creator;
 r.origin_transaction_uuid=r.creating_transaction_uuid=tx;r.origin_local_transaction_id=r.creating_local_transaction_id=1;return r;}
Map MapValue(unsigned n=0){Map r;r.map_uuid=f::Id(300+n);r.database_uuid=f::Id(1);r.generation=1;r.relation_role=2;
 r.relation_uuid=c::kNativeNameCatalogRelationUuid;r.index_uuid=f::Id(310+n);r.index_family_uuid=c::kNativeBtreeIndexFamilyUuid;
 r.key_profile_uuid=c::kNativeCatalogIndexKeyProfileUuid;r.visibility_profile_uuid=c::kNativeCatalogIndexVisibilityProfileUuid;
 r.descriptor_generation=r.storage_generation=1;
 r.origin_transaction_uuid=r.creating_transaction_uuid=tx;r.origin_local_transaction_id=r.creating_local_transaction_id=1;return r;}
// Independent SBCV construction (bootstrap_policy_fixture.hpp), not the production schema.
std::string Golden(const Key& r){f::Golden g(65629);g.Identity(1,r.profile_uuid);g.Identity(2,r.database_uuid);g.Integer(3,r.generation);
 g.Integer(4,p::u64(r.key_form));g.Integer(5,p::u64(r.key_order));g.Integer(6,p::u64(r.tie_break));
 g.Identity(7,r.origin_transaction_uuid.value);g.Integer(8,r.origin_local_transaction_id);
 g.Identity(9,r.creating_transaction_uuid.value);g.Integer(10,r.creating_local_transaction_id);return g.Finish();}
std::string Golden(const Visibility& r){f::Golden g(65630);g.Identity(1,r.profile_uuid);g.Identity(2,r.database_uuid);g.Integer(3,r.generation);
 g.Integer(4,p::u64(r.visibility));g.Identity(5,r.origin_transaction_uuid.value);g.Integer(6,r.origin_local_transaction_id);
 g.Identity(7,r.creating_transaction_uuid.value);g.Integer(8,r.creating_local_transaction_id);return g.Finish();}
std::string Golden(const Map& r){f::Golden g(65631);g.Identity(1,r.map_uuid);g.Identity(2,r.database_uuid);g.Integer(3,r.generation);
 g.Integer(4,r.relation_role);g.Identity(5,r.relation_uuid);g.Identity(6,r.index_uuid);g.Identity(7,r.index_family_uuid);
 g.Identity(8,r.key_profile_uuid);g.Identity(9,r.visibility_profile_uuid);g.Integer(10,r.descriptor_generation);
 g.Integer(11,r.storage_generation);g.Identity(12,r.origin_transaction_uuid.value);g.Integer(13,r.origin_local_transaction_id);
 g.Identity(14,r.creating_transaction_uuid.value);g.Integer(15,r.creating_local_transaction_id);return g.Finish();}
struct KeyCodec{static constexpr std::size_t bytes=c::kCatalogNativeCatalogIndexKeyProfileBytes;
 static auto Size(const Key& r){return c::CatalogNativeCatalogIndexKeyProfileEncodedBytes(r);}
 static auto Into(const Key& r,std::span<p::byte> o){return c::EncodeCatalogNativeCatalogIndexKeyProfileInto(r,o);}
 static auto Decode(std::string_view b){return c::DecodeCatalogNativeCatalogIndexKeyProfile(b);}};
struct VisibilityCodec{static constexpr std::size_t bytes=c::kCatalogNativeCatalogIndexVisibilityProfileBytes;
 static auto Size(const Visibility& r){return c::CatalogNativeCatalogIndexVisibilityProfileEncodedBytes(r);}
 static auto Into(const Visibility& r,std::span<p::byte> o){return c::EncodeCatalogNativeCatalogIndexVisibilityProfileInto(r,o);}
 static auto Decode(std::string_view b){return c::DecodeCatalogNativeCatalogIndexVisibilityProfile(b);}};
struct MapCodec{static constexpr std::size_t bytes=c::kCatalogNativeCatalogIndexDependencyMapBytes;
 static auto Size(const Map& r){return c::CatalogNativeCatalogIndexDependencyMapEncodedBytes(r);}
 static auto Into(const Map& r,std::span<p::byte> o){return c::EncodeCatalogNativeCatalogIndexDependencyMapInto(r,o);}
 static auto Decode(std::string_view b){return c::DecodeCatalogNativeCatalogIndexDependencyMap(b);}};
template<class Codec,class R> void Roundtrip(const R& value){const auto expected=Golden(value);
 std::array<p::byte,Codec::bytes+2> output;output.fill(0xa7);
 const auto size=NoHeap([&]{return Codec::Size(value);});Check(size&&*size==expected.size()&&*size==Codec::bytes,"exact registered byte count");
 const auto encoded=NoHeap([&]{return Codec::Into(value,std::span(output).subspan(1));});
 Check(encoded.ok()&&std::string_view(reinterpret_cast<const char*>(encoded.bytes.data()),encoded.bytes.size())==expected,"exact independent SBCV fields");
 Check(output.front()==0xa7&&std::all_of(output.begin()+expected.size()+1,output.end(),[](auto b){return b==0xa7;}),"unaligned destination preserves guards");
 const auto decoded=NoHeap([&]{return Codec::Decode(expected);});Check(decoded.ok()&&Golden(*decoded.record)==expected,"all fields retained");
 Check(c::IsCatalogNativeCatalogIndexPayload(expected),"family recognised by schema");}
template<class Codec,class R> void Refuse(const R& value,bool wire=true){std::array<p::byte,Codec::bytes> output;output.fill(0xa7);const auto before=output;
 const auto encoded=NoHeap([&]{return Codec::Into(value,output);});
 Check(!encoded.ok()&&encoded.bytes.empty()&&output==before&&!NoHeap([&]{return Codec::Size(value);}),"invalid record leaves output unchanged");
 if(wire){const auto raw=Golden(value);Check(!NoHeap([&]{return Codec::Decode(raw);}).ok(),"invalid record bytes refused");}}
template<class Codec,class R> void Framing(const R& value){const auto raw=Golden(value);
 std::array<p::byte,Codec::bytes> out;out.fill(0xab);const auto before=out;
 for(std::size_t n=0;n<raw.size();++n){Check(!NoHeap([&]{return Codec::Decode(std::string_view(raw).substr(0,n));}).ok(),"every truncated wire refuses");
   const auto encoded=NoHeap([&]{return Codec::Into(value,std::span(out).first(n));});
   Check(!encoded.ok()&&encoded.bytes.empty()&&out==before,"short output never writes a successful prefix");}
 for(const unsigned offset:{0u,4u,6u,8u,12u,16u,20u,24u,26u,28u}){auto invalid=raw;invalid[offset]^=0x7f;
   Check(!NoHeap([&]{return Codec::Decode(invalid);}).ok(),"invalid framing/schema/type/tag refused");}
 auto trailing=raw;trailing+='x';Check(!Codec::Decode(trailing).ok(),"trailing bytes refused");
 auto overlap=value;Check(NoHeap([&]{return Codec::Into(overlap,{reinterpret_cast<p::byte*>(&overlap),sizeof(overlap)});}).error==
   c::CatalogValueError::invalid_backing&&Golden(overlap)==Golden(value),"encoding cannot overwrite its input object");}
void Values(){
 Roundtrip<KeyCodec>(KeyValue());Roundtrip<VisibilityCodec>(VisibilityValue());Roundtrip<MapCodec>(MapValue());
 for(p::u64 role=1;role<=6;++role){auto m=MapValue();m.relation_role=role;Roundtrip<MapCodec>(m);}
 // The three schemas are distinct: no record decodes as another family.
 Check(!c::DecodeCatalogNativeCatalogIndexDependencyMap(Golden(KeyValue())).ok()&&
   !c::DecodeCatalogNativeCatalogIndexKeyProfile(Golden(VisibilityValue())).ok()&&
   !c::DecodeCatalogNativeCatalogIndexVisibilityProfile(Golden(MapValue())).ok(),"families are not interchangeable");
 Framing<KeyCodec>(KeyValue());Framing<VisibilityCodec>(VisibilityValue());Framing<MapCodec>(MapValue());
 // Provenance, shared by all three families.
 for(unsigned n=0;n<6;++n){auto r=KeyValue();switch(n){
   case 0:r.generation=0;break;case 1:r.origin_local_transaction_id=0;break;case 2:r.creating_local_transaction_id=2;break;
   case 3:r.creating_transaction_uuid.value=f::Id(8);break;case 4:r.origin_transaction_uuid.kind=p::UuidKind::object;break;
   case 5:r.generation=2;r.origin_local_transaction_id=2;r.creating_transaction_uuid.value=f::Id(8);r.creating_local_transaction_id=1;break;}
   Refuse<KeyCodec>(r,n!=4);}
 {auto r=KeyValue();r.generation=2;r.creating_transaction_uuid.value=f::Id(8);r.creating_local_transaction_id=2;Roundtrip<KeyCodec>(r);}
 for(unsigned n=0;n<8;++n){auto r=KeyValue();switch(n){
   case 0:r.profile_uuid={};break;case 1:r.database_uuid.bytes[6]=0x40;break;case 2:r.key_form=c::NativeCatalogIndexKeyForm(0);break;
   case 3:r.key_form=c::NativeCatalogIndexKeyForm(4);break;case 4:r.key_order=c::NativeCatalogIndexKeyOrder(2);break;  // forms 2 and 3 are the name trees (SB_NATIVE_NAME_TREES)
   case 5:r.tie_break=c::NativeCatalogIndexTieBreak(0);break;case 6:r.database_uuid=r.profile_uuid;break;
   case 7:r.origin_transaction_uuid.value=r.creating_transaction_uuid.value=r.database_uuid;break;}
   Refuse<KeyCodec>(r);}
 for(unsigned n=0;n<4;++n){auto r=VisibilityValue();switch(n){
   case 0:r.visibility=c::NativeCatalogIndexVisibility(0);break;case 1:r.visibility=c::NativeCatalogIndexVisibility(2);break;
   case 2:r.profile_uuid={};break;case 3:r.profile_uuid=r.database_uuid;break;}
   Refuse<VisibilityCodec>(r);}
 for(unsigned n=0;n<14;++n){auto r=MapValue();switch(n){
   case 0:r.relation_role=0;break;case 1:r.relation_role=7;break;case 2:r.descriptor_generation=0;break;case 3:r.storage_generation=0;break;
   case 4:r.index_family_uuid=f::Id(320);break;case 5:r.index_uuid={};break;case 6:r.relation_uuid.bytes[8]=0;break;
   case 7:r.key_profile_uuid=r.visibility_profile_uuid;break;case 8:r.index_uuid=r.relation_uuid;break;
   case 9:r.map_uuid=r.index_uuid;break;case 10:r.visibility_profile_uuid=r.database_uuid;break;
   case 11:r.key_profile_uuid={};break;case 12:r.generation=0;break;case 13:r.creating_local_transaction_id=2;break;}
   Refuse<MapCodec>(r);}
}
template<class R> c::CatalogMetadataVersion Metadata(const R& r,const p::Uuid& object,std::string_view subtype,unsigned n){
 auto m=f::Metadata(0,r.creating_transaction_uuid,f::Id(60),Golden(r));m.record.header.kind=c::CatalogRecordKind::config_profile;
 m.record.header.object_uuid={p::UuidKind::object,object};m.record.header.row_uuid={p::UuidKind::row,f::Id(1000+3*n)};
 m.default_name_uuid={p::UuidKind::object,f::Id(2000+2*n)};m.name_vector_uuid={p::UuidKind::object,f::Id(2001+2*n)};
 m.object_subtype=std::string(subtype);m.creator_local_transaction_id=r.creating_local_transaction_id;m.definition_version=r.generation;return m;}
c::CatalogMetadataVersion KeyMetadata(const Key& r){return Metadata(r,r.profile_uuid,"native_catalog_index_key_profile",0);}
c::CatalogMetadataVersion VisibilityMetadata(const Visibility& r){return Metadata(r,r.profile_uuid,"native_catalog_index_visibility_profile",1);}
c::CatalogMetadataVersion MapMetadata(const Map& r){return Metadata(r,r.map_uuid,"native_catalog_index_dependency_map",2);}
void Bindings(){
 for(const auto& m:{KeyMetadata(KeyValue()),VisibilityMetadata(VisibilityValue()),MapMetadata(MapValue())}){
   Check(c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(m)).has_value(),"common dispatcher accepts exact metadata");
   Check(c::CatalogNativeCatalogIndexMatchesDatabase(c::BorrowCatalogMetadataVersion(m),f::Id(1)),"actual database binding");
   Check(!c::CatalogNativeCatalogIndexMatchesDatabase(c::BorrowCatalogMetadataVersion(m),f::Id(9)),"foreign database refused");
   Check(c::CatalogMetadataPreservesFamilyOrigin(m,m),"exact same-generation replay");
   for(unsigned n=0;n<7;++n){auto bad=m;switch(n){
     case 0:bad.record.header.object_uuid.value=f::Id(8);break;case 1:bad.record.header.row_uuid.value=f::Id(1);break;
     case 2:bad.definition_version=2;break;case 3:bad.creator_local_transaction_id=2;break;case 4:bad.object_subtype="generic_profile";break;
     case 5:bad.record.header.kind=c::CatalogRecordKind::policy;break;case 6:bad.authority_scope=c::CatalogAuthorityScope::cluster;break;}
     Check(!c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(bad)),"header/creator/generation/subtype/kind/scope bound");}
 }
 // A subtype of one family cannot carry another family's payload.
 auto swapped=KeyMetadata(KeyValue());swapped.object_subtype="native_catalog_index_dependency_map";
 Check(!c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(swapped)),"subtype and payload family agree");
 Check(c::CatalogNativeCatalogIndexMatchesDatabase(c::BorrowCatalogMetadataVersion(f::Metadata(0,tx,f::Id(60),"")),f::Id(9)),
   "records outside the family pass the family's database check");
 const auto m=MapMetadata(MapValue());
 auto changed=MapValue();changed.storage_generation=2;
 Check(!c::CatalogMetadataPreservesFamilyOrigin(m,MapMetadata(changed)),"same-generation content immutable");
 changed.generation=2;changed.creating_transaction_uuid.value=f::Id(8);changed.creating_local_transaction_id=2;
 Check(c::CatalogMetadataPreservesFamilyOrigin(m,MapMetadata(changed)),"new generation retains original provenance");
 changed.origin_transaction_uuid=changed.creating_transaction_uuid;changed.origin_local_transaction_id=2;
 Check(!c::CatalogMetadataPreservesFamilyOrigin(m,MapMetadata(changed)),"new generation cannot replace origin");
 Check(!c::CatalogMetadataPreservesFamilyOrigin(KeyMetadata(KeyValue()),m),"a key profile cannot become a dependency map");
 const auto map=MapValue();std::vector<p::Uuid> reserved;
 db::ReserveNativeCreationCatalogReferences({m,{}},[&](const auto& id){reserved.push_back(id);});
 for(const auto& id:{map.map_uuid,map.database_uuid,map.relation_uuid,map.index_uuid,map.index_family_uuid,map.key_profile_uuid,
     map.visibility_profile_uuid,map.origin_transaction_uuid.value})
   Check(std::find(reserved.begin(),reserved.end(),id)!=reserved.end(),"native issuer excludes every map reference");
}
// Real native persistence: the three rows in the configuration relation of a
// populated creation workspace, read back after an independent reopen.
struct Item {
  c::CatalogMetadataVersion definition,vector_metadata,entry_metadata;
  c::CatalogNameVector vector;c::CatalogNameEntry entry;
  std::array<c::CatalogCreationNameEntryInput,1> entries;
  explicit Item(c::CatalogMetadataVersion d,unsigned n):definition(std::move(d)){
    vector_metadata=entry_metadata=definition;vector_metadata.record.payload.clear();entry_metadata.record.payload.clear();
    vector_metadata.record.header.kind=entry_metadata.record.header.kind=c::CatalogRecordKind::localized_name;
    vector_metadata.record.header.object_uuid=definition.name_vector_uuid;vector_metadata.record.header.row_uuid.value=f::Id(1001+3*n);
    vector_metadata.record.header.parent_uuid=definition.record.header.object_uuid;vector_metadata.object_subtype="name_vector";
    entry_metadata.record.header.object_uuid=definition.default_name_uuid;entry_metadata.record.header.row_uuid.value=f::Id(1002+3*n);
    entry_metadata.record.header.parent_uuid=definition.name_vector_uuid;entry_metadata.object_subtype="name_entry";entry_metadata.resource_epoch=1;
    vector.name_vector_uuid=entry.name_vector_uuid=definition.name_vector_uuid;vector.object_uuid=entry.object_uuid=definition.record.header.object_uuid;
    vector.default_name_entry_uuid=entry.name_entry_uuid=definition.default_name_uuid;vector.object_class=entry.object_class="config_profile";
    vector.owning_schema_uuid=definition.owning_schema_uuid;vector.default_language_tag="en-US";
    vector.name_collision_policy_uuid={p::UuidKind::object,f::Id(55)};vector.catalog_generation_id=1;
    vector.security_policy_uuid=definition.security_policy_uuid;vector.lifecycle_state=c::CatalogNameLifecycle::active;
    entry.scope_uuid=definition.record.header.parent_uuid;entry.parent_schema_uuid=definition.owning_schema_uuid;entry.language_tag="en-US";
    entry.dialect_profile_uuid={p::UuidKind::object,f::Id(56)};entry.identifier_profile_uuid={p::UuidKind::object,f::Id(57)};
    entry.raw_name_text=entry.display_name="catalog_index_row_"+std::to_string(n);entry.normalized_lookup_key.assign(entry.raw_name_text.begin(),entry.raw_name_text.end());
    entry.created_transaction_uuid=tx;entry.security_policy_uuid=definition.security_policy_uuid;
    entry.resource_epoch=entry.name_resolution_epoch=entry.catalog_generation_id=1;entry.default_for_language=entry.default_for_object=true;
    entry.lifecycle_state=c::CatalogNameLifecycle::active;
  }
  c::CatalogNamedCreationInput Input(){entries[0]={&entry_metadata,&entry};return {&definition,&vector_metadata,&vector,entries};}
};
void Physical(){scratchbird::tests::OwnedTempDirectory artifacts;
 for(const auto& profile:d::kCanonicalFilespacePageProfiles){
  Item key(KeyMetadata(KeyValue()),0),visibility(VisibilityMetadata(VisibilityValue()),1),map(MapMetadata(MapValue()),2);
  std::array inputs{key.Input(),visibility.Input(),map.Input()};
  const auto made=c::AssembleLocalNamedCreationRecords(f::Id(1),tx,"en-US",inputs,{10,1000000});
  Check(made.ok()&&made.records->definitions.size()==3&&made.records->names.size()==6,"typed rows and complete supplied names assemble");
  db::NativeCreationCatalogSeed seed;seed[4]=made.records->definitions;seed[1]=made.records->names;
  db::NativeFilespaceInitializationRequest request;request.bootstrap={f::Id(1),f::Id(2),profile.uuid,d::kNativeBootstrapIntegrityProfile,{},profile.page_size_bytes,1,0,1,7};
  request.operation_uuid=f::Id(3);request.writer_uuid=f::Id(4);request.policy_snapshot_uuid=f::Id(10);
  request.creator.transaction_uuid=tx;request.creator.local_id=mga::MakeLocalTransactionId(1);request.creator.scope=mga::TransactionScope::local_node;
  request.total_pages=64;request.creation_utc_millis=1789357072000ULL;const auto budget=128*p::u64{profile.page_size_bytes};
  u::StandaloneUuidV7Issuer issuer({f::Id(1),f::Id(10)},{{},0,1000});
  const auto path=(artifacts.path()/std::to_string(profile.page_size_bytes)).string();d::FileDevice device;
  Check(device.Open(path,d::FileOpenMode::create_new).ok(),"real native catalog index profile file");
  auto foreign=MapValue();foreign.database_uuid=f::Id(9);const auto kept=seed[4][2].metadata.record.payload;
  seed[4][2].metadata.record.payload=Golden(foreign);
  Check(!db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,request,seed,budget,issuer).ok()&&device.Size().size_bytes==0,
    "foreign dependency map refuses before physical effects");
  seed[4][2].metadata.record.payload=kept;
  const auto created=db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,request,seed,budget,issuer);
  Check(created.ok(),"real native catalog index rows persist");Check(device.Close().ok()&&device.Open(path,d::FileOpenMode::open_existing).ok(),"independent reopen");
  const std::vector<d::NativeFilespaceDevice> devices{{f::Id(2),profile.uuid,&device}};
  const auto rows=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(f::Id(1),devices,db::NativeCatalogSource::PageZero(created.receipt->filespace_uuid),2,5,{created.receipt->relations[4].object_uuid,{}},budget);
  Check(rows.ok()&&rows.rows.size()==3,"three rows read back");
  std::array<std::string,3> expected{Golden(KeyValue()),Golden(VisibilityValue()),Golden(MapValue())};
  for(const auto& row:rows.rows){
    Check(std::find(expected.begin(),expected.end(),row.metadata.record.payload)!=expected.end()&&
      c::CatalogNativeCatalogIndexMatchesDatabase(c::BorrowCatalogMetadataVersion(row.metadata),f::Id(1)),"exact rows survive native reopen");
  }
  Check(device.Close().ok(),"owned file closed");
 }}
}
// The shared builder: rows and bindings that cannot disagree.
void Builder(){
 db::NativeCatalogIndexRowsRequest request{f::Id(1),tx,1,{}};
 for(unsigned i:{1u,3u,4u,5u})request.roles[i]=db::NativeCatalogIndexRoleIdentities{i==1?p::Uuid{}:f::Id(400+i),f::Id(410+i),f::Id(420+i)};
 const auto built=db::BuildNativeCatalogIndexRows(request);
 Check(built.ok()&&built.rows.size()==6,"two profiles and one map per indexed role");
 Check(built.rows[0].object_uuid==c::kNativeCatalogIndexKeyProfileUuid&&built.rows[0].payload==Golden(KeyValue())&&
   built.rows[1].object_uuid==c::kNativeCatalogIndexVisibilityProfileUuid&&built.rows[1].payload==Golden(VisibilityValue()),
   "constant profiles with the exact registered payloads");
 unsigned at=2;
 for(unsigned i=0;i<6;++i){
   if(!request.roles[i]){Check(!built.indexes[i],"an unindexed role gets no binding");continue;}
   Check(built.indexes[i].has_value(),"every indexed role gets a binding");
   const auto& binding=*built.indexes[i];const auto& row=built.rows[at++];
   const auto map=c::DecodeCatalogNativeCatalogIndexDependencyMap(row.payload);
   Check(map.ok()&&row.object_uuid==map.record->map_uuid&&row.object_subtype=="native_catalog_index_dependency_map","map row decodes");
   const auto& m=*map.record;const auto& d=binding.dependencies;
   Check(m.relation_role==i+1&&m.relation_uuid==binding.relation_uuid&&m.index_uuid==d.index_uuid&&
     m.key_profile_uuid==d.key_profile_uuid&&m.visibility_profile_uuid==d.visibility_profile_uuid&&m.map_uuid==d.dependency_map_uuid&&
     m.descriptor_generation==d.descriptor_generation&&m.storage_generation==d.storage_generation&&
     m.index_family_uuid==c::kNativeBtreeIndexFamilyUuid,"binding and map name the same identities and generations");
   const std::vector<p::byte> bytes(reinterpret_cast<const p::byte*>(row.payload.data()),reinterpret_cast<const p::byte*>(row.payload.data())+row.payload.size());
   const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);
   Check(digest.ok()&&digest.digest==d.dependency_map_sha256,"binding digest is SHA-256 over the encoded map");
   Check(i==1?binding.relation_uuid==c::kNativeNameCatalogRelationUuid:binding.relation_uuid==f::Id(400+i),
     "the names relation takes the shared constant; others keep the caller's identity");
 }
 Check(at==built.rows.size(),"no extra rows");
 // The same request twice yields the same bytes and digests.
 const auto again=db::BuildNativeCatalogIndexRows(request);
 Check(again.ok()&&again.rows.size()==built.rows.size()&&std::equal(again.rows.begin(),again.rows.end(),built.rows.begin(),
   [](const auto& a,const auto& b){return a.object_uuid==b.object_uuid&&a.payload==b.payload;})&&
   std::equal(again.indexes.begin(),again.indexes.end(),built.indexes.begin(),[](const auto& a,const auto& b){
     return a.has_value()==b.has_value()&&(!a||(a->relation_uuid==b->relation_uuid&&a->dependencies==b->dependencies));}),"deterministic");
 const auto none=db::BuildNativeCatalogIndexRows({f::Id(1),tx,1,{}});
 Check(none.ok()&&none.rows.empty()&&std::all_of(none.indexes.begin(),none.indexes.end(),[](const auto& b){return !b;}),"no indexed role, no rows");
 for(unsigned n=0;n<10;++n){auto bad=request;auto expected=db::NativeCatalogIndexRowsError::invalid_request;switch(n){
   case 0:bad.roles[1]->relation_uuid=f::Id(401);break;
   case 1:bad.roles[3]->relation_uuid={};break;
   case 2:bad.roles[3]->index_uuid.bytes[6]=0x40;break;
   case 3:bad.roles[4]->map_uuid={};break;
   case 4:bad.database_uuid={};break;
   case 5:bad.construction_transaction.kind=p::UuidKind::object;break;
   case 6:bad.construction_local_transaction_id=0;break;
   case 7:bad.roles[4]->index_uuid=bad.roles[3]->index_uuid;expected=db::NativeCatalogIndexRowsError::duplicate_identity;break;
   case 8:bad.roles[5]->relation_uuid=c::kNativeNameCatalogRelationUuid;expected=db::NativeCatalogIndexRowsError::duplicate_identity;break;
   case 9:bad.roles[3]->map_uuid=c::kNativeCatalogIndexKeyProfileUuid;expected=db::NativeCatalogIndexRowsError::duplicate_identity;break;}
   const auto refused=db::BuildNativeCatalogIndexRows(bad);
   Check(refused.error==expected&&refused.rows.empty()&&std::all_of(refused.indexes.begin(),refused.indexes.end(),[](const auto& b){return !b;}),
     "invalid or colliding identities refused with no partial output");}
}
int main(){try{Values();Bindings();Builder();Physical();std::cout<<checks<<" native catalog index profile checks passed (declarations only)\n";return 0;}
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<e.what()<<'\n';return 1;}}
