// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_identifier_profile.hpp"
#include "bootstrap_policy_fixture.hpp"
#include "native_creation_workspace.hpp"
#include "physical_mga_cow_store.hpp"
#include "../support/owned_temp_directory.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>

thread_local bool heap_denied=false;
thread_local unsigned allocations=0;
void* operator new(std::size_t size){if(heap_denied)throw std::bad_alloc();++allocations;if(auto* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
namespace c=scratchbird::core::catalog;namespace p=scratchbird::core::platform;
namespace f=scratchbird::tests::bootstrap_policy;namespace db=scratchbird::storage::database;
namespace d=scratchbird::storage::disk;namespace u=scratchbird::core::uuid;namespace mga=scratchbird::transaction::mga;
// Freeze named wire values independently of the field roundtrip helper.
static_assert(static_cast<p::u64>(c::IdentifierEscapeRule::doubled_delimiter)==1&&
    static_cast<p::u64>(c::IdentifierEscapeRule::backslash)==2&&
    static_cast<p::u64>(c::IdentifierEscapeRule::doubled_or_backslash)==3&&
    static_cast<p::u64>(c::IdentifierEscapeRule::not_applicable)==4);
static_assert(static_cast<p::u64>(c::IdentifierFoldMode::none)==0&&
    static_cast<p::u64>(c::IdentifierFoldMode::upper_ascii)==1&&
    static_cast<p::u64>(c::IdentifierFoldMode::lower_ascii)==2&&
    static_cast<p::u64>(c::IdentifierFoldMode::resource_upper)==3&&
    static_cast<p::u64>(c::IdentifierFoldMode::resource_lower)==4);
static_assert(static_cast<p::u64>(c::IdentifierComparisonMode::exact)==1&&
    static_cast<p::u64>(c::IdentifierComparisonMode::folded)==2&&
    static_cast<p::u64>(c::IdentifierComparisonMode::case_insensitive)==3&&
    static_cast<p::u64>(c::IdentifierComparisonMode::profile_platform)==4);
static_assert(static_cast<p::u64>(c::IdentifierDisplayMode::preserve_user_text)==1&&
    static_cast<p::u64>(c::IdentifierDisplayMode::store_folded_display)==2&&
    static_cast<p::u64>(c::IdentifierDisplayMode::donor_projection)==3);
static_assert(static_cast<p::u64>(c::IdentifierReservedWordMode::reserved_requires_quote)==1&&
    static_cast<p::u64>(c::IdentifierReservedWordMode::contextual)==2&&
    static_cast<p::u64>(c::IdentifierReservedWordMode::parser_profile_specific)==3);
static_assert(static_cast<p::u64>(c::IdentifierLengthUnit::unicode_scalar_values)==1&&
    static_cast<p::u64>(c::IdentifierLengthUnit::encoded_octets)==2);
namespace {
unsigned checks=0;
void Check(bool ok,const char* why){++checks;if(!ok){heap_denied=false;throw std::runtime_error(why);}}
c::CatalogIdentifierResolutionProfile Profile(){
  c::CatalogIdentifierResolutionProfile r;r.profile_uuid=f::Id(100);r.database_uuid=f::Id(1);r.generation=1;
  r.profile_name="display_data_not_an_algorithm";r.quote_styles=1;r.escape_rule=c::IdentifierEscapeRule::doubled_delimiter;
  r.unquoted_fold_mode=c::IdentifierFoldMode::upper_ascii;r.quoted_preserves_case=true;r.comparison_mode=c::IdentifierComparisonMode::folded;
  r.storage_display_mode=c::IdentifierDisplayMode::preserve_user_text;r.reserved_word_behavior=c::IdentifierReservedWordMode::reserved_requires_quote;
  r.maximum_identifier_length=0;r.length_unit=c::IdentifierLengthUnit::unicode_scalar_values;
  r.origin_transaction_uuid={p::UuidKind::transaction,f::Id(5)};r.origin_local_transaction_id=1;return r;
}
std::string Golden(const c::CatalogIdentifierResolutionProfile& r,unsigned resource_mask=15,unsigned platform_mask=3){
  f::Golden g(65617);g.Identity(1,r.profile_uuid);g.Integer(2,r.generation);g.Identity(3,r.database_uuid);g.Field(4,3,r.profile_name);
  g.Integer(5,r.quote_styles);g.Integer(6,static_cast<p::u64>(r.escape_rule));g.Integer(7,static_cast<p::u64>(r.unquoted_fold_mode));
  g.Boolean(8,r.quoted_preserves_case);g.Integer(9,static_cast<p::u64>(r.comparison_mode));g.Integer(10,static_cast<p::u64>(r.storage_display_mode));
  g.Integer(11,static_cast<p::u64>(r.reserved_word_behavior));g.Integer(12,r.maximum_identifier_length);g.Integer(13,static_cast<p::u64>(r.length_unit));g.Integer(14,r.dialect_mode_flags);
  if(r.resources){if(resource_mask&1)g.Identity(15,r.resources->charset_uuid);if(resource_mask&2)g.Integer(16,r.resources->charset_generation);
    if(resource_mask&4)g.Identity(17,r.resources->collation_uuid);if(resource_mask&8)g.Integer(18,r.resources->collation_generation);}
  if(r.platform){if(platform_mask&1)g.Identity(19,r.platform->profile_uuid);if(platform_mask&2)g.Integer(20,r.platform->generation);}
  g.Identity(21,r.origin_transaction_uuid.value);g.Integer(22,r.origin_local_transaction_id);return g.Finish();
}
c::CatalogMetadataVersion Metadata(const c::CatalogIdentifierResolutionProfile& r){
  auto m=f::Metadata(0,r.origin_transaction_uuid,f::Id(60),Golden(r));m.record.header.kind=c::CatalogRecordKind::config_profile;
  m.record.header.object_uuid={p::UuidKind::object,r.profile_uuid};m.record.header.row_uuid={p::UuidKind::row,f::Id(101)};
  m.object_subtype="identifier_resolution_profile";m.definition_version=r.generation;return m;
}
void Roundtrip(const c::CatalogIdentifierResolutionProfile& r){
  const auto expected=Golden(r);std::array<p::byte,1024> backing;backing.fill(0x5a);
  heap_denied=true;const auto size=c::CatalogIdentifierProfileEncodedBytes(r);
  const auto encoded=c::EncodeCatalogIdentifierProfileInto(r,backing);const auto decoded=c::DecodeCatalogIdentifierProfile(expected);heap_denied=false;
  Check(size&&*size==expected.size()&&encoded.ok()&&decoded.ok()&&encoded.bytes.size()==expected.size()&&
      std::equal(encoded.bytes.begin(),encoded.bytes.end(),reinterpret_cast<const p::byte*>(expected.data()))&&Golden(*decoded.record)==expected,
      "heap-free first-use exact full independent native profile byte oracle");
  Check(std::all_of(backing.begin()+expected.size(),backing.end(),[](auto b){return b==0x5a;}),"unused output tail retained");
  const auto begin=reinterpret_cast<std::uintptr_t>(expected.data()),name=reinterpret_cast<std::uintptr_t>(decoded.record->profile_name.data());
  Check(name>=begin&&name+decoded.record->profile_name.size()<=begin+expected.size(),"decoded name borrows exact admitted source, no owning copy");
  allocations=0;const auto owned=c::EncodeCatalogIdentifierProfile(r);const auto allocated=allocations;
  Check(owned.ok()&&allocated==1&&owned.bytes==std::vector<p::byte>(encoded.bytes.begin(),encoded.bytes.end()),"owning encoding makes only its final payload allocation");
  for(std::size_t n=0;n<expected.size();++n){backing.fill(0x5a);heap_denied=true;
    const auto short_out=c::EncodeCatalogIdentifierProfileInto(r,std::span<p::byte>(backing).first(n));
    const auto short_in=c::DecodeCatalogIdentifierProfile(std::string_view(expected).substr(0,n));heap_denied=false;
    Check(!short_out.ok()&&short_out.bytes.empty()&&!short_in.ok()&&!short_in.record&&
        std::all_of(backing.begin(),backing.end(),[](auto b){return b==0x5a;}),"every short input/output refuses atomically without heap");}
  auto changed=expected;changed.push_back(0);heap_denied=true;const auto trailing=c::DecodeCatalogIdentifierProfile(changed);heap_denied=false;
  Check(!trailing.ok()&&!trailing.record,"exact profile extent, no trailing bytes");
}
void Refuse(const c::CatalogIdentifierResolutionProfile& r,bool check_wire=true){
  std::array<p::byte,1024> backing;backing.fill(0x5a);const auto expected=check_wire?Golden(r):std::string{};
  heap_denied=true;const auto size=c::CatalogIdentifierProfileEncodedBytes(r);
  const auto encoded=c::EncodeCatalogIdentifierProfileInto(r,backing);const auto decoded=check_wire?c::DecodeCatalogIdentifierProfile(expected):c::CatalogIdentifierProfileResult{};heap_denied=false;
  Check(!size&&!encoded.ok()&&encoded.bytes.empty()&&std::all_of(backing.begin(),backing.end(),[](auto b){return b==0x5a;}),"bad profile refuses before output or allocation");
  if(check_wire)Check(!decoded.ok()&&!decoded.record,"independently framed invalid profile refuses decode");
}
void Values(){
  const auto original=Profile();Roundtrip(original);
  auto unicode=original;unicode.profile_name="\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";Roundtrip(unicode);
  const auto framed=Golden(original);
  for(const auto at:{0u,4u,6u,8u,12u,16u,20u,22u,24u,26u,27u,28u}){
    auto malformed=framed;malformed[at]^=1;heap_denied=true;const auto decoded=c::DecodeCatalogIdentifierProfile(malformed);heap_denied=false;
    Check(!decoded.ok()&&!decoded.record,"bad framing/version/schema/field tag/type/extent refuses without heap");}
  for(unsigned mask=0;mask<16;++mask)for(unsigned escape=1;escape<=4;++escape){auto r=original;r.quote_styles=mask;r.escape_rule=static_cast<c::IdentifierEscapeRule>(escape);
    if((mask==0)==(escape==4))Roundtrip(r);else Refuse(r);}
  for(unsigned fold=0;fold<=4;++fold)for(unsigned compare=1;compare<=4;++compare){auto r=original;
    r.unquoted_fold_mode=static_cast<c::IdentifierFoldMode>(fold);r.comparison_mode=static_cast<c::IdentifierComparisonMode>(compare);
    if(fold>=3||compare==3)r.resources=c::CatalogIdentifierResourceBinding{f::Id(200),f::Id(201),2,3};
    if(compare==4)r.platform=c::CatalogIdentifierPlatformBinding{f::Id(202),4};
    if(fold==0&&compare==2)Refuse(r);else Roundtrip(r);}
  for(unsigned display=1;display<=3;++display)for(unsigned words=1;words<=3;++words)for(unsigned unit=1;unit<=2;++unit){auto r=original;
    r.storage_display_mode=static_cast<c::IdentifierDisplayMode>(display);r.reserved_word_behavior=static_cast<c::IdentifierReservedWordMode>(words);
    r.length_unit=static_cast<c::IdentifierLengthUnit>(unit);r.quoted_preserves_case=false;Roundtrip(r);}
  std::string maximum_name(256,'N');auto maximum=original;maximum.profile_name=maximum_name;
  maximum.generation=maximum.origin_local_transaction_id=maximum.maximum_identifier_length=maximum.dialect_mode_flags=std::numeric_limits<p::u64>::max();
  maximum.resources=c::CatalogIdentifierResourceBinding{f::Id(200),f::Id(201),UINT64_MAX,UINT64_MAX};
  maximum.comparison_mode=c::IdentifierComparisonMode::profile_platform;maximum.platform=c::CatalogIdentifierPlatformBinding{f::Id(202),UINT64_MAX};Roundtrip(maximum);
  for(unsigned mask=1;mask<15;++mask){const auto wire=Golden(maximum,mask);heap_denied=true;const auto r=c::DecodeCatalogIdentifierProfile(wire);heap_denied=false;Check(!r.ok()&&!r.record,"partial resource UUID/generation quartet refuses");}
  for(unsigned mask=1;mask<3;++mask){const auto wire=Golden(maximum,15,mask);heap_denied=true;const auto r=c::DecodeCatalogIdentifierProfile(wire);heap_denied=false;Check(!r.ok()&&!r.record,"partial platform UUID/generation pair refuses");}
  for(unsigned n=0;n<33;++n){auto r=maximum;std::string text;
    if(n==0)r.profile_uuid={};if(n==1)r.database_uuid.bytes[6]=0x40;if(n==2)r.generation=0;
    if(n==3)r.profile_uuid=r.database_uuid;if(n==4)r.origin_transaction_uuid.value=r.profile_uuid;
    if(n==5)r.origin_transaction_uuid.kind=p::UuidKind::object;if(n==6)r.origin_local_transaction_id=0;
    if(n==7)r.profile_name="";if(n==8)r.profile_name=std::string_view("bad\0name",8);
    if(n==9){text.assign(257,'X');r.profile_name=text;}if(n==10)r.profile_name=std::string_view("\xff",1);
    if(n==11)r.quote_styles=16;if(n==12)r.escape_rule=static_cast<c::IdentifierEscapeRule>(5);
    if(n==13)r.unquoted_fold_mode=static_cast<c::IdentifierFoldMode>(5);if(n==14)r.comparison_mode=static_cast<c::IdentifierComparisonMode>(5);
    if(n==15)r.storage_display_mode=static_cast<c::IdentifierDisplayMode>(0);if(n==16)r.reserved_word_behavior=static_cast<c::IdentifierReservedWordMode>(4);
    if(n==17)r.length_unit=static_cast<c::IdentifierLengthUnit>(3);if(n==18)r.resources->charset_uuid={};
    if(n==19)r.resources->collation_uuid=r.resources->charset_uuid;if(n==20)r.resources->charset_generation=0;
    if(n==21)r.resources->collation_generation=0;if(n==22)r.platform->generation=0;
    if(n==23)r.platform->profile_uuid=r.profile_uuid;if(n==24)r.platform.reset();
    if(n==25)r.comparison_mode=c::IdentifierComparisonMode::exact;
    if(n==26){r.resources.reset();r.unquoted_fold_mode=c::IdentifierFoldMode::resource_upper;}
    if(n==27)r.platform->profile_uuid=r.resources->collation_uuid;
    if(n==28)r.resources->charset_uuid=r.database_uuid;if(n==29)r.resources->collation_uuid=r.origin_transaction_uuid.value;
    if(n==30)r.origin_transaction_uuid.value=r.database_uuid;
    if(n==31)r.profile_name={static_cast<const char*>(nullptr),1};
    if(n==32){r.platform.reset();r.resources.reset();r.comparison_mode=c::IdentifierComparisonMode::case_insensitive;}
    Refuse(r,n!=5&&n!=31);
  }
  // Every byte of the input object and borrowed text is protected from aliasing
  // destination writes, including scalar/padding bytes not directly serialized.
  auto value=original;std::array<p::byte,sizeof(value)> before,after;std::memcpy(before.data(),&value,sizeof(value));
  for(std::size_t at=0;at<sizeof(value);++at){heap_denied=true;const auto r=c::EncodeCatalogIdentifierProfileInto(value,{reinterpret_cast<p::byte*>(&value)+at,1});heap_denied=false;
    std::memcpy(after.data(),&value,sizeof(value));Check(!r.ok()&&r.error==c::CatalogValueError::invalid_backing&&after==before,"whole-object input overlap is refused unchanged");}
  std::array<p::byte,1024> text_backing{};const auto text=std::string(original.profile_name);std::copy(text.begin(),text.end(),text_backing.begin());
  value.profile_name={reinterpret_cast<const char*>(text_backing.data()),text.size()};const auto text_before=text_backing;
  heap_denied=true;const auto alias=c::EncodeCatalogIdentifierProfileInto(value,text_backing);heap_denied=false;
  Check(!alias.ok()&&alias.error==c::CatalogValueError::invalid_backing&&text_backing==text_before,"borrowed name backing cannot be overwritten");
}
void Bindings(){
  const auto r=Profile();const auto m=Metadata(r);const auto view=c::BorrowCatalogMetadataVersion(m);
  Check(c::CatalogIdentifierProfileMatchesMetadata(view)&&c::CatalogMetadataVersionEncodedBytes(view)&&
      c::CatalogIdentifierProfileMatchesDatabase(view,f::Id(1))&&!c::CatalogIdentifierProfileMatchesDatabase(view,f::Id(2)),"full native common and owning database binding");
  for(unsigned n=0;n<17;++n){auto bad=m;
    if(n==0)bad.record.header.kind=c::CatalogRecordKind::policy;if(n==1)bad.record.header.object_uuid.value=f::Id(102);
    if(n==2)bad.object_subtype="other_profile";if(n==3)bad.authority_scope=c::CatalogAuthorityScope::cluster;
    if(n==4)bad.definition_version=2;if(n==5)bad.owning_schema_uuid.kind=p::UuidKind::object;
    if(n==6)bad.record.header.parent_uuid.value=f::Id(61);if(n==7)bad.owner_uuid={};if(n==8)bad.audit_uuid={};
    if(n==9)bad.creator_transaction_uuid.value=f::Id(6);if(n==10)bad.creator_local_transaction_id=2;
    if(n==11)f::Put(bad.record.payload,16,65618,4);if(n==12)bad.record.payload="not a native family";
    if(n==13)bad.record.header.object_uuid.kind=p::UuidKind::schema;
    if(n==14)bad.security_policy_uuid={};if(n==15)bad.default_name_uuid={};if(n==16)bad.name_vector_uuid={};
    const auto b=c::BorrowCatalogMetadataVersion(bad);Check(!c::CatalogIdentifierProfileMatchesMetadata(b)&&!c::CatalogMetadataVersionEncodedBytes(b),"common dispatcher cannot bypass profile kind/subtype/origin checks");}
  auto next=r;next.generation=2;auto successor=Metadata(next);successor.creator_transaction_uuid.value=f::Id(6);successor.creator_local_transaction_id=2;
  Check(c::CatalogMetadataPreservesFamilyOrigin(m,successor),"successor preserves original profile/database/construction identity");
  for(unsigned n=0;n<4;++n){auto changed=next;if(n==0)changed.profile_uuid=f::Id(102);if(n==1)changed.database_uuid=f::Id(2);
    if(n==2)changed.origin_transaction_uuid.value=f::Id(6);if(n==3)changed.origin_local_transaction_id=2;
    auto bad=Metadata(changed);bad.creator_transaction_uuid=successor.creator_transaction_uuid;bad.creator_local_transaction_id=2;
    Check(!c::CatalogMetadataPreservesFamilyOrigin(m,bad),"successor cannot rebind durable origin/database/profile");}
}
void Physical(){
  scratchbird::tests::OwnedTempDirectory artifacts;const auto value=Profile();const auto meta=Metadata(value);
  for(const auto& profile:d::kCanonicalFilespacePageProfiles){
    const auto path=(artifacts.path()/std::to_string(profile.page_size_bytes)).string();d::FileDevice device;
    Check(device.Open(path,d::FileOpenMode::create_new).ok(),"private real profile construction file");
    db::NativeCreationCatalogSeed seed;seed[0].push_back({meta,{}});db::NativeCreationCatalogIndexes indexes;
    scratchbird::storage::page::NativeBtreeDependencies deps;deps.index_uuid=f::Id(8010);deps.descriptor_generation=deps.storage_generation=1;
    deps.key_profile_uuid=f::Id(8020);deps.visibility_profile_uuid=f::Id(8030);deps.dependency_map_uuid=f::Id(8040);deps.dependency_map_sha256.fill(1);
    indexes[0]=db::NativeCreationCatalogIndexBinding{f::Id(8000),deps};
    db::NativeFilespaceInitializationRequest request;
    request.bootstrap={f::Id(1),f::Id(2),profile.uuid,d::kNativeBootstrapIntegrityProfile,{},profile.page_size_bytes,1,0,1,7};
    request.operation_uuid=f::Id(3);request.writer_uuid=f::Id(4);request.policy_snapshot_uuid=f::Id(10);
    request.creator.transaction_uuid=value.origin_transaction_uuid;request.creator.local_id=mga::MakeLocalTransactionId(1);request.creator.scope=mga::TransactionScope::local_node;
    request.total_pages=64;request.creation_utc_millis=1789357072000ULL;const auto budget=128*p::u64{profile.page_size_bytes};
    u::StandaloneUuidV7Issuer issuer({f::Id(1),f::Id(10)},{{},0,1000});
    auto wrong=value;wrong.database_uuid=f::Id(9);seed[0][0].metadata=Metadata(wrong);
    const auto refused=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(device,request,seed,indexes,budget,issuer);
    const auto extent=device.Size();Check(!refused.ok()&&!refused.receipt&&extent.ok()&&extent.size_bytes==0,"foreign database profile refuses before physical effects");
    seed[0][0].metadata=meta;
    auto resource=value;resource.profile_uuid=f::Id(110);resource.unquoted_fold_mode=c::IdentifierFoldMode::resource_upper;
    resource.comparison_mode=c::IdentifierComparisonMode::case_insensitive;resource.resources=c::CatalogIdentifierResourceBinding{f::Id(200),f::Id(201),2,3};
    auto platform=value;platform.profile_uuid=f::Id(120);platform.comparison_mode=c::IdentifierComparisonMode::profile_platform;
    platform.platform=c::CatalogIdentifierPlatformBinding{f::Id(202),4};platform.resources=resource.resources;
    platform.maximum_identifier_length=platform.dialect_mode_flags=UINT64_MAX;
    for(const auto& extra:{resource,platform}){
      auto m=Metadata(extra);const auto index=seed[0].size();m.record.header.row_uuid.value=f::Id(101+10*index);
      m.default_name_uuid.value=f::Id(9100+2*index);m.name_vector_uuid.value=f::Id(9101+2*index);seed[0].push_back({m,{}});
    }
    const auto made=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(device,request,seed,indexes,budget,issuer);
    Check(made.ok(),"actual typed profile persists through native indexed construction");
    Check(device.Close().ok()&&device.Open(path,d::FileOpenMode::open_existing).ok(),"independent actual native profile reopen");
    const std::vector<d::NativeFilespaceDevice> devices{{f::Id(2),profile.uuid,&device}};
    const auto rows=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(f::Id(1),devices,made.receipt->checkpoint,2,1,{indexes[0]->relation_uuid,deps},budget);
    Check(rows.ok()&&rows.rows.size()==3,"all actual profile definitions survive indexed committed read/reopen");
    for(const auto& expected:{value,resource,platform}){
      const auto row=std::find_if(rows.rows.begin(),rows.rows.end(),[&](const auto& row){return row.metadata.record.header.object_uuid.value==expected.profile_uuid;});
      Check(row!=rows.rows.end()&&row->metadata.record.payload==Golden(expected)&&
          c::CatalogIdentifierProfileMatchesDatabase(c::BorrowCatalogMetadataVersion(row->metadata),f::Id(1)),
          "complete scalar/optional resource/platform bytes and raw identities survive independent native reopen");
    }
    const auto zero=d::ReadFilespacePageZeroFromOpenDevice(device);Check(zero.ok()&&zero.record->bootstrap.lifecycle_state==7,"stored profile is not naming/source/security/serving admission");
    Check(device.Close().ok(),"actual private construction device released");
  }
}
}
int main()try{Values();Bindings();Physical();std::cout<<"PASS identifier profile "<<checks<<" checks\n";return 0;}
catch(const std::exception& e){heap_denied=false;std::cerr<<e.what()<<'\n';return 1;}
