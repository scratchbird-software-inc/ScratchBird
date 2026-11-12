// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_naming_dialect_profile.hpp"
#include "bootstrap_policy_fixture.hpp"
#include "native_creation_workspace.hpp"
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
c::CatalogNamingDialectProfile Profile(){
  c::CatalogNamingDialectProfile r;r.profile_uuid=f::Id(100);r.database_uuid=f::Id(1);r.identifier_profile_uuid=f::Id(110);
  r.generation=r.identifier_profile_generation=1;
  r.origin_transaction_uuid=r.creating_transaction_uuid={p::UuidKind::transaction,f::Id(5)};
  r.origin_local_transaction_id=r.creating_local_transaction_id=1;return r;
}
std::string Golden(const c::CatalogNamingDialectProfile& r){
  f::Golden g(65628);g.Identity(1,r.profile_uuid);g.Identity(2,r.database_uuid);g.Integer(3,r.generation);
  g.Identity(4,r.identifier_profile_uuid);g.Integer(5,r.identifier_profile_generation);g.Integer(6,r.allowed_dialect_mode_flags);
  g.Identity(7,r.origin_transaction_uuid.value);g.Integer(8,r.origin_local_transaction_id);
  g.Identity(9,r.creating_transaction_uuid.value);g.Integer(10,r.creating_local_transaction_id);return g.Finish();
}
c::CatalogMetadataVersion Metadata(const c::CatalogNamingDialectProfile& r){
  auto m=f::Metadata(0,r.creating_transaction_uuid,f::Id(60),Golden(r));m.record.header.kind=c::CatalogRecordKind::config_profile;
  m.record.header.object_uuid={p::UuidKind::object,r.profile_uuid};m.record.header.row_uuid={p::UuidKind::row,f::Id(101)};
  m.object_subtype=scratchbird::core::catalog::CatalogObjectSubtype::naming_dialect_profile;m.definition_version=r.generation;m.creator_local_transaction_id=r.creating_local_transaction_id;return m;
}
void Roundtrip(const c::CatalogNamingDialectProfile& r){
  const auto expected=Golden(r);std::array<p::byte,256> output;output.fill(0x5a);
  heap_denied=true;const auto size=c::CatalogNamingDialectProfileEncodedBytes(r);
  const auto encoded=c::EncodeCatalogNamingDialectProfileInto(r,output);const auto decoded=c::DecodeCatalogNamingDialectProfile(expected);heap_denied=false;
  Check(size&&*size==224&&expected.size()==224&&encoded.ok()&&decoded.ok()&&encoded.bytes.size()==expected.size()&&
      std::equal(encoded.bytes.begin(),encoded.bytes.end(),reinterpret_cast<const p::byte*>(expected.data()))&&Golden(*decoded.record)==expected,
      "heap-free first-use independent complete SBCV byte oracle");
  Check(std::all_of(output.begin()+224,output.end(),[](auto b){return b==0x5a;}),"unused output retained");
  for(std::size_t n=0;n<224;++n){output.fill(0x5a);heap_denied=true;
    const auto small=c::EncodeCatalogNamingDialectProfileInto(r,std::span(output).first(n));
    const auto short_input=c::DecodeCatalogNamingDialectProfile(std::string_view(expected).substr(0,n));heap_denied=false;
    Check(!small.ok()&&small.bytes.empty()&&!short_input.ok()&&!short_input.record&&
        std::all_of(output.begin(),output.end(),[](auto b){return b==0x5a;}),"every truncated input/output refuses before effects");}
  auto trailing=expected;trailing.push_back(0);Check(!c::DecodeCatalogNamingDialectProfile(trailing).ok(),"exact frame extent required");
}
void Values(){
  auto original=Profile();Roundtrip(original);
  auto maximum=original;maximum.generation=maximum.identifier_profile_generation=maximum.allowed_dialect_mode_flags=UINT64_MAX;
  maximum.creating_transaction_uuid.value=f::Id(6);maximum.creating_local_transaction_id=UINT64_MAX;Roundtrip(maximum);
  for(unsigned n=0;n<17;++n){auto r=original;
    if(n==0)r.profile_uuid={};if(n==1)r.database_uuid.bytes[6]=0x40;if(n==2)r.identifier_profile_uuid={};
    if(n==3)r.generation=0;if(n==4)r.identifier_profile_generation=0;if(n==5)r.origin_local_transaction_id=0;
    if(n==6)r.creating_local_transaction_id=0;if(n==7)r.origin_transaction_uuid.kind=p::UuidKind::object;
    if(n==8)r.creating_transaction_uuid.kind=p::UuidKind::object;if(n==9)r.profile_uuid=r.database_uuid;
    if(n==10)r.identifier_profile_uuid=r.profile_uuid;if(n==11)r.identifier_profile_uuid=r.database_uuid;
    if(n==12)r.origin_transaction_uuid.value=r.identifier_profile_uuid;if(n==13)r.creating_transaction_uuid.value=r.database_uuid;
    if(n==14)r.creating_transaction_uuid.value=f::Id(6);if(n==15)r.creating_local_transaction_id=2;
    if(n==16){r.creating_transaction_uuid.value=f::Id(6);r.creating_local_transaction_id=2;}
    std::array<p::byte,256> out;out.fill(0x5a);const auto bytes=Golden(r);heap_denied=true;
    const auto size=c::CatalogNamingDialectProfileEncodedBytes(r);
    const auto encoded=c::EncodeCatalogNamingDialectProfileInto(r,out);const auto decoded=c::DecodeCatalogNamingDialectProfile(bytes);heap_denied=false;
    Check(!size&&!encoded.ok()&&encoded.bytes.empty()&&std::all_of(out.begin(),out.end(),[](auto b){return b==0x5a;}),"invalid identity/generation/creator refuses atomically");
    if(n!=7&&n!=8)Check(!decoded.ok()&&!decoded.record,"independent invalid body refuses decoding");
  }
  const auto frame=Golden(original);
  for(const auto at:{0u,4u,6u,8u,12u,16u,20u,22u,24u,26u,27u,28u}){
    auto bytes=frame;bytes[at]^=1;heap_denied=true;const auto decoded=c::DecodeCatalogNamingDialectProfile(bytes);heap_denied=false;
    Check(!decoded.ok()&&!decoded.record,"malformed header and typed fields refuse without allocation");}
  std::array<p::byte,sizeof(original)> before,after;std::memcpy(before.data(),&original,sizeof(original));
  for(std::size_t at=0;at<sizeof(original);++at){heap_denied=true;
    const auto result=c::EncodeCatalogNamingDialectProfileInto(original,{reinterpret_cast<p::byte*>(&original)+at,1});heap_denied=false;
    std::memcpy(after.data(),&original,sizeof(original));Check(result.error==c::CatalogValueError::invalid_backing&&before==after,"whole input object overlap refused before writing");}
}
void Bindings(){
  const auto profile=Profile();const auto m=Metadata(profile);const auto view=c::BorrowCatalogMetadataVersion(m);
  Check(c::CatalogNamingDialectProfileMatchesMetadata(view)&&c::CatalogMetadataVersionEncodedBytes(view)&&
      c::CatalogNamingDialectProfileMatchesDatabase(view,f::Id(1))&&!c::CatalogNamingDialectProfileMatchesDatabase(view,f::Id(2)),"common header/metadata/independent database binding");
  for(unsigned n=0;n<18;++n){auto bad=m;
    if(n==0)bad.record.header.kind=c::CatalogRecordKind::policy;if(n==1)bad.record.header.object_uuid.value=f::Id(102);
    if(n==2)bad.object_subtype=scratchbird::core::catalog::CatalogObjectSubtype::schema_application;if(n==3)bad.authority_scope=c::CatalogAuthorityScope::cluster;
    if(n==4)bad.definition_version=2;if(n==5)bad.owning_schema_uuid.kind=p::UuidKind::object;
    if(n==6)bad.record.header.parent_uuid.value=f::Id(61);if(n==7)bad.owner_uuid={};if(n==8)bad.audit_uuid={};
    if(n==9)bad.creator_transaction_uuid.value=f::Id(6);if(n==10)bad.creator_local_transaction_id=2;
    if(n==11)f::Put(bad.record.payload,16,65617,4);if(n==12)bad.record.payload="not a native family";
    if(n==13)bad.record.header.object_uuid.kind=p::UuidKind::schema;
    if(n==14)bad.security_policy_uuid={};if(n==15)bad.default_name_uuid={};if(n==16)bad.name_vector_uuid={};
    if(n==17)bad.record.header.row_uuid.value=profile.identifier_profile_uuid;
    const auto b=c::BorrowCatalogMetadataVersion(bad);Check(!c::CatalogNamingDialectProfileMatchesMetadata(b)&&!c::CatalogMetadataVersionEncodedBytes(b),"common dispatcher refuses disguised or mismatched profile");}
  auto next=profile;next.generation=2;next.creating_transaction_uuid.value=f::Id(6);next.creating_local_transaction_id=2;
  const auto successor=Metadata(next);Check(c::CatalogMetadataPreservesFamilyOrigin(m,successor),"successor retains original construction identity");
  for(unsigned n=0;n<4;++n){auto changed=next;if(n==0)changed.profile_uuid=f::Id(102);if(n==1)changed.database_uuid=f::Id(2);
    if(n==2)changed.origin_transaction_uuid.value=f::Id(7);if(n==3)changed.origin_local_transaction_id=2;
    Check(!c::CatalogMetadataPreservesFamilyOrigin(m,Metadata(changed)),"successor cannot change durable origin/database/profile");}
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
    seed[0][0].metadata=meta;
    const auto made=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(device,request,seed,indexes,budget,issuer);
    Check(made.ok(),"typed naming dialect persists through native indexed construction");
    Check(device.Close().ok()&&device.Open(path,d::FileOpenMode::open_existing).ok(),"independent real reopen");
    const std::vector<d::NativeFilespaceDevice> devices{{f::Id(2),profile.uuid,&device}};
    const auto rows=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(f::Id(1),devices,db::NativeCatalogSource::PageZero(made.receipt->filespace_uuid),2,1,{indexes[0]->relation_uuid,deps},budget);
    Check(rows.ok()&&rows.rows.size()==1&&rows.rows[0].metadata.record.payload==Golden(value)&&
        c::CatalogNamingDialectProfileMatchesDatabase(c::BorrowCatalogMetadataVersion(rows.rows[0].metadata),f::Id(1)),"complete stored profile survives native committed read");
    const auto zero=d::ReadFilespacePageZeroFromOpenDevice(device);Check(zero.ok()&&zero.record->bootstrap.lifecycle_state==1,"stored naming profile cannot activate a parser or admit serving: page zero stays as the create wrote it, an online filespace (owner 2026-11-07); serving is the server's act at open");
    Check(device.Close().ok(),"real construction device released");
  }
}
}
int main()try{Values();Bindings();Physical();std::cout<<"PASS naming dialect profile "<<checks<<" checks\n";return 0;}
catch(const std::exception& e){heap_denied=false;std::cerr<<e.what()<<'\n';return 1;}
