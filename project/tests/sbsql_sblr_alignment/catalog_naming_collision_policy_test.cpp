// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_naming_collision_policy.hpp"
#include "bootstrap_policy_fixture.hpp"
#include "native_creation_workspace.hpp"
#include "native_creation_catalog_input.hpp"
#include "physical_mga_cow_store.hpp"
#include "../support/owned_temp_directory.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>

thread_local bool heap_denied=false;
void* operator new(std::size_t size){if(heap_denied)throw std::bad_alloc();if(auto* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
namespace c=scratchbird::core::catalog;namespace p=scratchbird::core::platform;
namespace f=scratchbird::tests::bootstrap_policy;namespace db=scratchbird::storage::database;
namespace d=scratchbird::storage::disk;namespace u=scratchbird::core::uuid;namespace mga=scratchbird::transaction::mga;
namespace {
unsigned checks=0;
void Check(bool ok,const char* why){++checks;if(!ok){heap_denied=false;throw std::runtime_error(why);}}
c::CatalogNamingCollisionPolicyBody Profile(){
  c::CatalogNamingCollisionPolicyBody r;r.body_uuid=f::Id(100);r.database_uuid=f::Id(1);r.generation=1;
  r.class_count=r.group_count=1;r.classes[0]={c::CatalogRecordKind::table_descriptor,f::Id(110)};
  r.groups[0]={f::Id(110),c::CatalogCollisionLanguageLane::exact_tag,c::CatalogCollisionKeyLane::separate};
  r.origin_transaction_uuid=r.creating_transaction_uuid={p::UuidKind::transaction,f::Id(5)};
  r.origin_local_transaction_id=r.creating_local_transaction_id=1;return r;
}
std::string Golden(const c::CatalogNamingCollisionPolicyBody& r){
  f::Golden g(65627);g.Identity(1,r.body_uuid);g.Identity(2,r.database_uuid);g.Integer(3,r.generation);
  std::string classes,groups;
  for(std::size_t i=0;i<std::min(r.class_count,r.classes.size());++i){const auto at=classes.size();classes.resize(at+2);
    f::Put(classes,at,static_cast<p::u16>(r.classes[i].kind),2);classes+=f::Bytes(r.classes[i].group_uuid);}
  for(std::size_t i=0;i<std::min(r.group_count,r.groups.size());++i){groups+=f::Bytes(r.groups[i].group_uuid);
    groups.push_back(static_cast<char>(r.groups[i].language_lane));groups.push_back(static_cast<char>(r.groups[i].key_lane));groups.append(6,0);}
  g.Field(4,4,classes);g.Field(5,4,groups);g.Identity(6,r.origin_transaction_uuid.value);g.Integer(7,r.origin_local_transaction_id);
  g.Identity(8,r.creating_transaction_uuid.value);g.Integer(9,r.creating_local_transaction_id);return g.Finish();
}
c::CatalogMetadataVersion Metadata(const c::CatalogNamingCollisionPolicyBody& r){
  auto m=f::Metadata(0,r.creating_transaction_uuid,f::Id(60),Golden(r));
  m.record.header.object_uuid={p::UuidKind::object,r.body_uuid};m.record.header.row_uuid={p::UuidKind::row,f::Id(101)};
  m.object_subtype=scratchbird::core::catalog::CatalogObjectSubtype::naming_collision_policy_body;m.definition_version=r.generation;m.creator_local_transaction_id=r.creating_local_transaction_id;return m;
}
void Roundtrip(const c::CatalogNamingCollisionPolicyBody& r){
  const auto golden=Golden(r);std::array<p::byte,3000> out;out.fill(0x5a);
  heap_denied=true;const auto size=c::CatalogNamingCollisionPolicyEncodedBytes(r);
  const auto encoded=c::EncodeCatalogNamingCollisionPolicyInto(r,out);const auto decoded=c::DecodeCatalogNamingCollisionPolicy(golden);heap_denied=false;
  Check(size&&*size==184+18*r.class_count+24*r.group_count&&*size==golden.size()&&encoded.ok()&&decoded.ok()&&
      std::ranges::equal(encoded.bytes,std::span(reinterpret_cast<const p::byte*>(golden.data()),golden.size()))&&Golden(*decoded.record)==golden,
      "independent complete native collision policy byte oracle without heap");
  Check(std::all_of(out.begin()+golden.size(),out.end(),[](auto b){return b==0x5a;}),"unused output bytes unchanged");
  for(std::size_t n=0;n<golden.size();++n){out.fill(0x5a);heap_denied=true;
    const auto small=c::EncodeCatalogNamingCollisionPolicyInto(r,std::span(out).first(n));
    const auto truncated=c::DecodeCatalogNamingCollisionPolicy(std::string_view(golden).substr(0,n));heap_denied=false;
    Check(!small.ok()&&small.bytes.empty()&&!truncated.ok()&&!truncated.record&&
        std::all_of(out.begin(),out.end(),[](auto b){return b==0x5a;}),"every short input/output refuses atomically without allocation");}
  auto trailing=golden;trailing.push_back(0);Check(!c::DecodeCatalogNamingCollisionPolicy(trailing).ok(),"exact extent, no trailing bytes");
}
void Values(){
  auto r=Profile();Roundtrip(r);
  for(unsigned language=1;language<=3;++language)for(unsigned lane=1;lane<=2;++lane){
    r.groups[0].language_lane=static_cast<c::CatalogCollisionLanguageLane>(language);r.groups[0].key_lane=static_cast<c::CatalogCollisionKeyLane>(lane);Roundtrip(r);}
  r=Profile();r.class_count=0;
  for(const auto& kind:c::BuiltinCatalogRecordDescriptorViews())r.classes[r.class_count++].kind=kind.kind;
  std::sort(r.classes.begin(),r.classes.begin()+r.class_count,[](const auto& a,const auto& b){return p::u16(a.kind)<p::u16(b.kind);});
  r.group_count=r.class_count;
  for(std::size_t i=0;i<r.class_count;++i){r.classes[i].group_uuid=f::Id(1000+i);
    r.groups[i]={f::Id(1000+i),static_cast<c::CatalogCollisionLanguageLane>(1+i%3),static_cast<c::CatalogCollisionKeyLane>(1+i%2)};}
  r.generation=r.creating_local_transaction_id=UINT64_MAX;r.creating_transaction_uuid.value=f::Id(6);Roundtrip(r);
  const auto all=r;
  for(unsigned n=0;n<23;++n){r=all;
    if(n==0)r.body_uuid={};if(n==1)r.database_uuid.bytes[6]=0x40;if(n==2)r.generation=0;
    if(n==3)r.class_count=0;if(n==4)r.group_count=0;if(n==5)r.class_count=65;if(n==6)r.group_count=65;
    if(n==7)r.classes[0].kind=c::CatalogRecordKind::unknown;if(n==8)r.classes[1].kind=r.classes[0].kind;
    if(n==9)std::swap(r.classes[0],r.classes[1]);if(n==10)r.classes[0].group_uuid=f::Id(2000);
    if(n==11)r.groups[0].group_uuid={};if(n==12)r.groups[1].group_uuid=r.groups[0].group_uuid;
    if(n==13)r.groups[0].language_lane=static_cast<c::CatalogCollisionLanguageLane>(0);
    if(n==14)r.groups[0].key_lane=static_cast<c::CatalogCollisionKeyLane>(3);
    if(n==15)r.classes[0].group_uuid=r.classes[1].group_uuid;
    if(n==16)r.origin_local_transaction_id=0;if(n==17)r.creating_local_transaction_id=1;
    if(n==18)r.creating_transaction_uuid.value=r.origin_transaction_uuid.value;
    if(n==19)r.generation=1;if(n==20)r.body_uuid=r.database_uuid;
    if(n==21)r.origin_transaction_uuid.kind=p::UuidKind::object;
    if(n==22){r=Profile();r.groups[0].group_uuid=r.body_uuid;r.classes[0].group_uuid=r.body_uuid;}
    std::array<p::byte,3000> out;out.fill(0x5a);const auto wire=Golden(r);heap_denied=true;
    const auto size=c::CatalogNamingCollisionPolicyEncodedBytes(r);const auto encoded=c::EncodeCatalogNamingCollisionPolicyInto(r,out);
    const auto decoded=c::DecodeCatalogNamingCollisionPolicy(wire);heap_denied=false;
    Check(!size&&!encoded.ok()&&encoded.bytes.empty()&&std::all_of(out.begin(),out.end(),[](auto b){return b==0x5a;}),"invalid supplied policy refuses before effects");
    if(n!=5&&n!=6&&n!=21)Check(!decoded.ok()&&!decoded.record,"independent invalid body cannot decode a partial policy");
  }
  const auto wire=Golden(Profile());
  for(unsigned at:{0u,4u,6u,8u,12u,16u,20u,22u,24u,26u,27u,28u,140u,141u,142u,143u,144u,145u}){
    auto bad=wire;bad[at]^=1;heap_denied=true;const auto result=c::DecodeCatalogNamingCollisionPolicy(bad);heap_denied=false;
    Check(!result.ok()&&!result.record,"header/field corruption and every reserved group byte refuse");}
  r=Profile();std::array<p::byte,sizeof(r)> before,after;std::memcpy(before.data(),&r,sizeof(r));
  for(std::size_t at=0;at<sizeof(r);++at){heap_denied=true;
    const auto result=c::EncodeCatalogNamingCollisionPolicyInto(r,{reinterpret_cast<p::byte*>(&r)+at,1});heap_denied=false;
    std::memcpy(after.data(),&r,sizeof(r));Check(result.error==c::CatalogValueError::invalid_backing&&before==after,"entire fixed body, including unused slots, protected from output overlap");}
  std::vector<p::Uuid> reserved;db::ReserveNativeCreationCatalogReferences({Metadata(r),{}},[&](const auto& id){reserved.push_back(id);});
  Check(std::find(reserved.begin(),reserved.end(),r.groups[0].group_uuid)!=reserved.end(),"embedded namespace identity reserved against generated physical identities");
}
void Bindings(){
  const auto body=Profile();const auto m=Metadata(body);const auto view=c::BorrowCatalogMetadataVersion(m);
  Check(c::CatalogNamingCollisionPolicyMatchesMetadata(view)&&c::CatalogMetadataVersionEncodedBytes(view)&&
      c::CatalogNamingCollisionPolicyMatchesDatabase(view,f::Id(1))&&!c::CatalogNamingCollisionPolicyMatchesDatabase(view,f::Id(2)),"common metadata and independent database binding");
  for(unsigned n=0;n<15;++n){auto bad=m;
    if(n==0)bad.record.header.kind=c::CatalogRecordKind::config_profile;if(n==1)bad.record.header.object_uuid.value=f::Id(102);
    if(n==2)bad.object_subtype=scratchbird::core::catalog::CatalogObjectSubtype::schema_application;if(n==3)bad.definition_version=2;if(n==4)bad.authority_scope=c::CatalogAuthorityScope::cluster;
    if(n==5)bad.record.header.parent_uuid.value=f::Id(61);if(n==6)bad.owner_uuid={};if(n==7)bad.audit_uuid={};
    if(n==8)bad.creator_local_transaction_id=2;if(n==9)bad.creator_transaction_uuid.value=f::Id(6);
    if(n==10)bad.security_policy_uuid={};if(n==11)bad.default_name_uuid={};if(n==12)bad.name_vector_uuid={};
    if(n==13)bad.record.header.row_uuid.value=body.groups[0].group_uuid;if(n==14)bad.record.payload="not native";
    Check(!c::CatalogNamingCollisionPolicyMatchesMetadata(c::BorrowCatalogMetadataVersion(bad))&&
        !c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(bad)),"common dispatcher rejects body/header/subtype/creator disguise");}
  auto next=body;next.generation=2;next.creating_transaction_uuid.value=f::Id(6);next.creating_local_transaction_id=2;
  Check(c::CatalogMetadataPreservesFamilyOrigin(m,Metadata(next)),"successor retains actual origin");
  for(unsigned n=0;n<4;++n){auto bad=next;if(n==0)bad.body_uuid=f::Id(102);if(n==1)bad.database_uuid=f::Id(2);
    if(n==2)bad.origin_transaction_uuid.value=f::Id(7);if(n==3)bad.origin_local_transaction_id=2;
    Check(!c::CatalogMetadataPreservesFamilyOrigin(m,Metadata(bad)),"successor cannot change original identity or creator");}
}
void Physical(){
  scratchbird::tests::OwnedTempDirectory artifacts;const auto value=Profile();const auto meta=Metadata(value);
  for(const auto& profile:d::kCanonicalFilespacePageProfiles){
    const auto path=(artifacts.path()/std::to_string(profile.page_size_bytes)).string();d::FileDevice device;
    Check(device.Open(path,d::FileOpenMode::create_new).ok(),"private real native construction file");
    db::NativeCreationCatalogSeed seed;seed[0].push_back({meta,{}});db::NativeCreationCatalogIndexes indexes;
    scratchbird::storage::page::NativeBtreeDependencies deps;deps.index_uuid=f::Id(8010);deps.descriptor_generation=deps.storage_generation=1;
    deps.key_profile_uuid=f::Id(8020);deps.visibility_profile_uuid=f::Id(8030);deps.dependency_map_uuid=f::Id(8040);deps.dependency_map_sha256.fill(1);
    indexes[0]=db::NativeCreationCatalogIndexBinding{f::Id(8000),deps};db::NativeFilespaceInitializationRequest request;
    request.bootstrap={f::Id(1),f::Id(2),profile.uuid,d::kNativeBootstrapIntegrityProfile,{},profile.page_size_bytes,1,0,1,7};
    request.operation_uuid=f::Id(3);request.writer_uuid=f::Id(4);request.policy_snapshot_uuid=f::Id(10);
    request.creator.transaction_uuid=value.origin_transaction_uuid;request.creator.local_id=mga::MakeLocalTransactionId(1);request.creator.scope=mga::TransactionScope::local_node;
    request.total_pages=64;request.creation_utc_millis=1789357072000ULL;const auto budget=128*p::u64{profile.page_size_bytes};
    u::StandaloneUuidV7Issuer issuer({f::Id(1),f::Id(10)},{{},0,1000});
    auto wrong=value;wrong.database_uuid=f::Id(9);seed[0][0].metadata=Metadata(wrong);
    const auto refused=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(device,request,seed,indexes,budget,issuer);
    const auto extent=device.Size();Check(!refused.ok()&&!refused.receipt&&extent.ok()&&extent.size_bytes==0,"foreign database profile refuses before writes");
    auto alias=value;alias.groups[0].group_uuid=alias.classes[0].group_uuid=f::Id(2);seed[0][0].metadata=Metadata(alias);
    const auto aliased=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(device,request,seed,indexes,budget,issuer);
    Check(!aliased.ok()&&device.Size().size_bytes==0,"embedded namespace cannot own an existing filespace identity");
    seed[0][0].metadata=meta;
    auto duplicate=value;duplicate.body_uuid=f::Id(111);auto duplicate_metadata=Metadata(duplicate);duplicate_metadata.record.header.row_uuid.value=f::Id(112);
    seed[0].push_back({duplicate_metadata,{}});
    const auto shared=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(device,request,seed,indexes,budget,issuer);
    Check(!shared.ok()&&device.Size().size_bytes==0,"different native bodies cannot own the same embedded namespace identity");
    seed[0].pop_back();
    const auto made=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(device,request,seed,indexes,budget,issuer);
    Check(made.ok(),"typed naming collision policy persists through native indexed construction");
    Check(device.Close().ok()&&device.Open(path,d::FileOpenMode::open_existing).ok(),"independent real reopen");
    const std::vector<d::NativeFilespaceDevice> devices{{f::Id(2),profile.uuid,&device}};
    const auto rows=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(f::Id(1),devices,db::NativeCatalogSource::PageZero(made.receipt->filespace_uuid),2,1,{indexes[0]->relation_uuid,deps},budget);
    Check(rows.ok()&&rows.rows.size()==1&&rows.rows[0].metadata.record.payload==Golden(value)&&
        c::CatalogNamingCollisionPolicyMatchesDatabase(c::BorrowCatalogMetadataVersion(rows.rows[0].metadata),f::Id(1)),"complete stored profile survives native committed read");
    const auto zero=d::ReadFilespacePageZeroFromOpenDevice(device);Check(zero.ok()&&zero.record->bootstrap.lifecycle_state==1,"stored naming profile cannot activate a parser or admit serving: page zero stays as the create wrote it, an online filespace (owner 2026-11-07); serving is the server's act at open");
    Check(device.Close().ok(),"real construction device released");
  }
}
}
int main()try{Values();Bindings();Physical();std::cout<<"PASS naming collision policy "<<checks<<" checks\n";return 0;}
catch(const std::exception& e){heap_denied=false;std::cerr<<e.what()<<'\n';return 1;}
