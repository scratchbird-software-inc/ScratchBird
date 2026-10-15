// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_policy_creation.hpp"
#include "database_lifecycle.hpp"
#include "native_creation_workspace.hpp"
#include "physical_mga_cow_store.hpp"
#include "bootstrap_policy_fixture.hpp"
#include "memory_configuration_fixture.hpp"
#include "catalog_security_record_codec.hpp"
#include "../support/owned_temp_directory.hpp"
#include "../support/policy_pack_fixture.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <new>
#include <stdexcept>

thread_local long allocation_budget=-1;
thread_local bool allocation_failed=false;
void* operator new(std::size_t size) {
  if(allocation_budget>=0&&allocation_budget--==0){allocation_failed=true;throw std::bad_alloc();}
  if(auto* out=std::malloc(size?size:1))return out;throw std::bad_alloc();
}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}

namespace c=scratchbird::core::catalog;namespace p=scratchbird::core::platform;
namespace db=scratchbird::storage::database;namespace d=scratchbird::storage::disk;
namespace f=scratchbird::tests::bootstrap_policy;namespace m=scratchbird::tests::memory_configuration;
namespace u=scratchbird::core::uuid;namespace mga=scratchbird::transaction::mga;
namespace {
unsigned checks=0;
void Check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
const p::TypedUuid tx{p::UuidKind::transaction,f::Id(5)};
p::TypedUuid Typed(p::UuidKind kind,unsigned n){return {kind,f::Id(n)};}
p::Uuid SourceId(unsigned suffix) {
  auto id=u::ParseTypedUuid(p::UuidKind::object,"018f7a10-1280-7000-8000-000000000001");
  Check(id.ok(),"independent source identity fixture");
  id.value.value.bytes[14]=suffix>>8;id.value.value.bytes[15]=suffix;return id.value.value;
}
struct Item {
  c::CatalogMetadataVersion definition,vector_metadata,entry_metadata;
  c::CatalogNameVector vector;c::CatalogNameEntry entry;
  std::array<c::CatalogCreationNameEntryInput,1> entries;
};
struct Package {
  std::vector<Item> items;std::vector<c::CatalogNamedCreationInput> inputs;
  std::array<db::NativeInitialMembershipIdentity,5> memberships;
  explicit Package(bool security=false):items(security?54:28) {
    for(unsigned i=0;i<memberships.size();++i)memberships[i]={f::Id(13000+i),SourceId(0x201+i),SourceId(0x100+i)};
    for(unsigned n=0;n<items.size();++n) {
      auto& x=items[n];const auto object=n==0?SourceId(1):n<=12?SourceId(0x10+n-1):n<=26?SourceId(0x400+n-13):n==27?f::Id(12000):
          n<=32?SourceId(0x100+n-28):n<=38?SourceId(0x200+n-33):n<=43?f::Id(13000+n-39):SourceId(0x300+n-44);
      const auto subtype=n==0?"bootstrap_policy_pack":n<=12?"bootstrap_security_provider":n<=26?"bootstrap_policy_profile":n==27?"memory_configuration":
          n<=32?"role_account":n<=38?"group_account":"grant_record";
      auto& meta=x.definition;meta=f::Metadata(0,tx,f::Id(60),{});
      meta.record.header.object_uuid={p::UuidKind::object,object};meta.record.header.row_uuid=Typed(p::UuidKind::row,200000+3*n);
      meta.object_subtype=subtype;if(n==27)meta.record.header.kind=c::CatalogRecordKind::config_profile;
      if(n>=28)meta.record.header.kind=n<=32?c::CatalogRecordKind::role_account:n<=38?c::CatalogRecordKind::group_account:c::CatalogRecordKind::grant_record;
      meta.default_name_uuid=Typed(p::UuidKind::object,300000+2*n);meta.name_vector_uuid=Typed(p::UuidKind::object,300001+2*n);
      x.vector_metadata=meta;x.vector_metadata.record.header.kind=c::CatalogRecordKind::localized_name;
      x.vector_metadata.record.header.object_uuid=meta.name_vector_uuid;x.vector_metadata.record.header.row_uuid=Typed(p::UuidKind::row,200001+3*n);
      x.vector_metadata.record.header.parent_uuid=meta.record.header.object_uuid;x.vector_metadata.object_subtype="name_vector";
      auto& v=x.vector;v.name_vector_uuid=meta.name_vector_uuid;v.object_uuid=meta.record.header.object_uuid;
      v.object_class=c::CatalogRecordKindName(meta.record.header.kind);v.owning_schema_uuid=meta.owning_schema_uuid;
      v.default_language_tag="en-US";v.default_name_entry_uuid=meta.default_name_uuid;
      v.name_collision_policy_uuid=Typed(p::UuidKind::object,55);v.catalog_generation_id=1;
      v.security_policy_uuid=meta.security_policy_uuid;v.lifecycle_state=c::CatalogNameLifecycle::active;
      x.entry_metadata=meta;x.entry_metadata.record.header.kind=c::CatalogRecordKind::localized_name;
      x.entry_metadata.record.header.object_uuid=meta.default_name_uuid;x.entry_metadata.record.header.row_uuid=Typed(p::UuidKind::row,200002+3*n);
      x.entry_metadata.record.header.parent_uuid=meta.name_vector_uuid;x.entry_metadata.object_subtype="name_entry";x.entry_metadata.resource_epoch=1;
      auto& e=x.entry;e.name_entry_uuid=meta.default_name_uuid;e.name_vector_uuid=meta.name_vector_uuid;
      e.object_uuid=meta.record.header.object_uuid;e.object_class=v.object_class;e.scope_uuid=meta.record.header.parent_uuid;
      e.parent_schema_uuid=meta.owning_schema_uuid;e.language_tag="en-US";e.dialect_profile_uuid=Typed(p::UuidKind::object,56);
      e.identifier_profile_uuid=Typed(p::UuidKind::object,57);e.raw_name_text=e.display_name="supplied_alias_"+std::to_string(n);
      e.normalized_lookup_key.assign(e.raw_name_text.begin(),e.raw_name_text.end());e.catalog_generation_id=1;
      e.created_transaction_uuid=tx;e.security_policy_uuid=meta.security_policy_uuid;
      e.resource_epoch=e.name_resolution_epoch=1;e.default_for_language=e.default_for_object=true;e.lifecycle_state=c::CatalogNameLifecycle::active;
    }
    Rebind();
  }
  void Rebind(){inputs.clear();for(auto& x:items){x.entries[0]={&x.entry_metadata,&x.entry};inputs.push_back({&x.definition,&x.vector_metadata,&x.vector,x.entries});}}
  auto Request()const{return db::NativeInitialPolicyRequest{SB_DEFAULT_POLICY_PACK_ROOT,f::Id(1),tx,f::Id(12000),"en-US",inputs,{1000,4*1024*1024}};}
  auto SecurityRequest()const{return db::NativeInitialPolicySecurityRequest{Request(),memberships};}
};
std::pair<p::Uuid,std::string> SecurityGolden(unsigned n) {
  static constexpr std::array<const char*,5> roles{"ROLE_APP_RUNTIME","ROLE_DBA","ROLE_SECURITY_ADMIN","ROLE_AUDIT_READER","ROLE_OPERATOR"};
  static constexpr std::array<const char*,5> role_descriptions{
      "Application runtime role for connect and event delivery.","Database administration role for local database objects.",
      "Security administration role for identities, grants, and policies.","Audit and lineage inspection role.","Operational management and observability role."};
  static constexpr std::array<const char*,6> groups{"PUBLIC","APP","DBA","SEC","AUD","OPS"};
  static constexpr std::array<const char*,6> group_descriptions{
      "Baseline public group with no implicit engine authority.","Application runtime users.","Local database administrators.",
      "Security administrators.","Audit readers.","Operators."};
  static constexpr std::array<unsigned,10> subjects{0,1,1,1,1,2,2,2,3,4};
  static constexpr std::array<const char*,10> rights{"CONNECT","SELECT","INSERT","UPDATE","DELETE",
      "SEC_IDENTITY_ADMIN","SEC_MEMBERSHIP_ADMIN","SEC_GRANT_ADMIN","AUDIT_READ","OBS_MANAGEMENT_INSPECT"};
  const unsigned kind=n<5?62:n<11?61:63;
  const auto id=n<5?SourceId(0x100+n):n<11?SourceId(0x200+n-5):n<16?f::Id(13000+n-11):SourceId(0x300+n-16);
  f::Golden g(65536+kind);f::Put(g.bytes,20,2,2);
  g.Identity(kind==62?3:kind==61?2:4,id);
  if(n>=11&&n<16){g.Identity(5,SourceId(0x201+n-11));g.Identity(6,SourceId(0x100+n-11));}
  if(n>=16)g.Identity(7,SourceId(0x100+subjects[n-16]));
  g.Identity(9,SourceId(1));
  if(n>=11&&n<16)g.Identity(10,id);
  g.Identity(11,tx.value);
  g.Integer(32,1);g.Integer(33,1);g.Integer(34,1);g.Boolean(35,true);g.Field(36,3,"uuid");g.Boolean(37,true);
  if(n<5){g.Field(44,3,roles[n]);g.Field(46,3,role_descriptions[n]);g.Field(47,3,std::string("group:")+groups[n+1]);}
  else if(n<11){g.Field(46,3,group_descriptions[n-5]);g.Field(48,3,groups[n-5]);}
  else if(n<16){g.Field(54,3,"group_membership");g.Field(55,3,"group");g.Field(56,3,"role");g.Field(58,3,"allow");}
  else {g.Field(54,3,"privilege");g.Field(57,3,"role");g.Field(58,3,"allow");g.Field(59,3,rights[n-16]);}
  return {id,g.Finish()};
}
void Verify(const c::CatalogNamedCreationSet& out,bool security=false) {
  Check(out.definitions.size()==(security?54:28)&&out.names.size()==(security?108:56)&&out.encoded_bytes>0,"all admitted policy/security definitions and every supplied name returned");
  unsigned packs=0,providers=0,profiles=0,memories=0,bindings=0;
  std::array<bool,26> security_seen{};
  for(const auto& record:out.definitions) {
    const auto& meta=record.metadata;
    Check(c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(meta)).has_value(),"complete native common metadata binding");
    if(c::IsCatalogSecurityRecordKind(meta.record.header.kind)) {
      bool matched=false;
      for(unsigned i=0;i<security_seen.size();++i){const auto expected=SecurityGolden(i);
        if(meta.record.header.object_uuid.value!=expected.first)continue;
        Check(!security_seen[i]&&meta.record.payload==expected.second,"complete exact native imported security bytes and immutable origin");
        security_seen[i]=true;matched=true;
      }
      Check(matched&&meta.creator_transaction_uuid.value==tx.value&&meta.creator_local_transaction_id==1,
          "no invented security identity or provenance");
      continue;
    }
    const auto pack=c::DecodeCatalogBootstrapPolicyPack(meta.record.payload);
    const auto provider=c::DecodeCatalogBootstrapSecurityProvider(meta.record.payload);
    const auto profile=c::DecodeCatalogBootstrapPolicyProfile(meta.record.payload);
    const auto memory=c::DecodeCatalogMemoryConfiguration(meta.record.payload);
    if(pack.ok()){++packs;Check(pack.record->policy_pack_uuid==SourceId(1)&&pack.record->declared_counts==std::array<p::u64,9>{12,1,11,5,6,5,10,14,58},"actual admitted pack counts and identity");}
    else if(provider.ok()){++providers;Check(provider.record->provider_uuid==SourceId(0x10+static_cast<unsigned>(provider.record->family)-1)&&provider.record->policy_pack_uuid==SourceId(1),"every real imported provider keeps exact binary source identity and family");}
    else if(profile.ok()){++profiles;if(profile.record->area=="memory_resource_governance") {
      ++bindings;Check(profile.record->resource_definition_uuid==f::Id(12000)&&profile.record->resource_definition_generation==1,"real materialized resource binding, no inferred identity/generation");}}
    else if(memory.ok()){
      ++memories;auto expected=m::Values(tx);expected.policy_pack_uuid=SourceId(1);expected.profile_uuid=SourceId(0x403);
      Check(m::Golden(expected)==meta.record.payload,"all actual memory settings and exact references match independent bytes");
    }else Check(false,"no legacy policy body or unsupported family escaped native producer");
    Check(meta.creator_transaction_uuid.kind==tx.kind&&meta.creator_transaction_uuid.value==tx.value&&meta.creator_local_transaction_id==1,
        "supplied actual creation origin retained without manufactured transaction");
  }
  Check(packs==1&&providers==12&&profiles==14&&memories==1&&bindings==1,"complete source family coverage");
  Check(std::count(security_seen.begin(),security_seen.end(),true)==(security?26:0),"complete security coverage, no bootstrap or service principal invented");
}
void SecurityLogical() {
  Package package(true);auto request=package.SecurityRequest();
  const auto made=db::AssembleNativeInitialPolicySecurityRecords(request);
  Check(made.ok(),"complete same-source policy and security cohort");Verify(*made.records,true);
  for(unsigned scenario=0;scenario<24;++scenario) {
    Package bad(true);auto changed=bad.SecurityRequest();std::vector<db::NativeInitialMembershipIdentity> extra;
    if(scenario==0)changed.group_memberships=changed.group_memberships.first(4);
    if(scenario==1){extra.assign(bad.memberships.begin(),bad.memberships.end());extra.push_back(extra[0]);changed.group_memberships=extra;}
    if(scenario==2)bad.memberships[1]=bad.memberships[0];
    if(scenario==3)bad.memberships[0].member_uuid=SourceId(0x999);
    if(scenario==4)bad.memberships[0].parent_uuid=SourceId(0x201);
    if(scenario==5)bad.memberships[0].membership_uuid={};
    if(scenario==6)bad.memberships[0].membership_uuid.bytes[6]=0x40;
    if(scenario==7)bad.memberships[1].membership_uuid=bad.memberships[0].membership_uuid;
    if(scenario==8)bad.memberships[0].membership_uuid=SourceId(0x100);
    if(scenario==9)bad.memberships[0].membership_uuid=SourceId(0x10);
    if(scenario==10)bad.memberships[0].membership_uuid=changed.policy.database_uuid;
    if(scenario==11)bad.items[28].definition.creator_transaction_uuid.value=f::Id(999);
    if(scenario==12)bad.items[28].definition.record.payload="unadmitted_shadow";
    if(scenario==13)bad.items[28].definition.record.header.kind=c::CatalogRecordKind::group_account;
    if(scenario==14){bad.items[28].vector.object_class="policy";bad.items[28].entry.object_class="policy";}
    if(scenario==15)bad.items[28].definition.record.header.object_uuid.value=SourceId(0x999);
    if(scenario==16)bad.items[28].definition.security_policy_uuid={};
    if(scenario==17)bad.items[28].definition.record.header.row_uuid.value=bad.items[29].definition.record.header.row_uuid.value;
    if(scenario==18)changed.policy.definitions=changed.policy.definitions.first(53);
    if(scenario==19)bad.inputs[28]=bad.inputs[29];
    if(scenario==20)changed.policy.limits.maximum_records=161;
    if(scenario==21)changed.policy.limits.maximum_encoded_bytes=made.records->encoded_bytes-1;
    if(scenario==22)bad.items[28].entries[0].entry=nullptr;
    if(scenario==23)bad.memberships[0].member_uuid=bad.memberships[0].parent_uuid;
    const auto refused=db::AssembleNativeInitialPolicySecurityRecords(changed);
    Check(!refused.ok()&&!refused.records,"invalid/omitted/repeated source relationship, identity, common/name and bounds refuse atomically");
    if(scenario==2||scenario==7||scenario==8||scenario==9)
      Check(refused.error==db::NativeInitialPolicyError::duplicate_identity,"membership collision has exact duplicate classification");
  }
  request.policy.limits={162,made.records->encoded_bytes};
  Check(db::AssembleNativeInitialPolicySecurityRecords(request).ok(),"exact combined cohort count and byte bounds");
  for(long budget:{0,1,5,31,97,257}) {
    allocation_failed=false;allocation_budget=budget;
    const auto refused=db::AssembleNativeInitialPolicySecurityRecords(request);allocation_budget=-1;
    Check(allocation_failed&&!refused.ok()&&!refused.records&&refused.error==db::NativeInitialPolicyError::resource_exhausted,
        "actual combined intake/scratch/owning allocation fault has no partial cohort");
  }
  std::reverse(package.memberships.begin(),package.memberships.end());
  const auto reordered=db::AssembleNativeInitialPolicySecurityRecords(request);
  Check(reordered.ok(),"membership bindings are binary keyed, not caller-order authority");Verify(*reordered.records,true);
}
void Logical() {
  Package package;auto request=package.Request();const auto made=db::AssembleNativeInitialPolicyRecords(request);Check(made.ok(),"actual native policy creation-plan assembly");Verify(*made.records);
  for(unsigned scenario=0;scenario<23;++scenario) {
    Package bad;auto changed=bad.Request();std::string malformed_root;
    if(scenario==0)changed.database_uuid={};if(scenario==1)changed.construction_transaction.kind=p::UuidKind::object;
    if(scenario==2)changed.memory_configuration_uuid=SourceId(1);if(scenario==3)changed.policy_seed_pack_root="/not-an-existing-scratchbird-policy-pack";
    if(scenario==4)changed.definitions=changed.definitions.first(27);
    if(scenario==5)bad.items[0].definition.record.payload="must_not_be_reinterpreted";
    if(scenario==6)bad.items[0].definition.record.header.object_uuid.value=f::Id(999);
    if(scenario==7)bad.items[0].definition.object_subtype="policy";
    if(scenario==8)bad.items[0].definition.owner_uuid={};if(scenario==9)bad.items[0].entry.default_for_object=false;
    if(scenario==10)bad.items[0].definition.record.header.row_uuid.value=bad.items[1].definition.record.header.row_uuid.value;
    if(scenario==11)bad.items[0].entry_metadata.record.header.row_uuid.value=changed.database_uuid;
    if(scenario==12)changed.limits.maximum_records=83;
    if(scenario==13)changed.limits.maximum_encoded_bytes=made.records->encoded_bytes-1;
    if(scenario==14)bad.items[27].definition.definition_version=2;
    if(scenario==15)changed.memory_configuration_uuid=SourceId(0x100);
    if(scenario==16)changed.database_uuid=SourceId(0x200);
    if(scenario==17)changed.construction_transaction.value=SourceId(0x300);
    if(scenario==18){malformed_root=SB_DEFAULT_POLICY_PACK_ROOT;malformed_root.append("\0ignored",8);changed.policy_seed_pack_root=malformed_root;}
    if(scenario==19)bad.inputs[0].vector_metadata=nullptr;
    if(scenario==20)bad.items[0].entries[0].entry=nullptr;
    if(scenario==21)changed.policy_seed_pack_root={static_cast<const char*>(nullptr),1};
    if(scenario==22)changed.database_default_language={static_cast<const char*>(nullptr),1};
    const auto refused=db::AssembleNativeInitialPolicyRecords(changed);
    Check(!refused.ok()&&!refused.records,"invalid source, identity, metadata, names and bounds yield no partial native package");
    if(scenario==3)Check(refused.error==db::NativeInitialPolicyError::source_invalid&&refused.source_diagnostic.has_value(),"actual source refusal retains diagnostic");
    if(scenario==10||scenario==11||(scenario>=15&&scenario<=17))Check(refused.error==db::NativeInitialPolicyError::duplicate_identity,"raw identity collision differs from source/name refusal");
    if(scenario==18)Check(refused.error==db::NativeInitialPolicyError::invalid_request&&!refused.source_diagnostic,
        "embedded NUL path refuses before any selected source is opened");
    if(scenario>=21)Check(refused.error==db::NativeInitialPolicyError::invalid_request&&!refused.source_diagnostic,
        "nonempty null source/language backing refuses before any dereference or source intake");
  }
  request.limits={84,made.records->encoded_bytes};const auto exact=db::AssembleNativeInitialPolicyRecords(request);
  Check(exact.ok()&&exact.records->encoded_bytes==made.records->encoded_bytes,"exact record and byte limit accepted without truncation");
  for(long budget: {0,1,5,31,97,257}) {
    allocation_failed=false;allocation_budget=budget;
    const auto refused=db::AssembleNativeInitialPolicyRecords(request);allocation_budget=-1;
    Check(allocation_failed&&!refused.ok()&&!refused.records,"actual intake/scratch/output allocation refusal cannot escape a partial native package");
  }
  package.items[0].entry.display_name="changed after assembly";
  Check(std::get<c::CatalogNameEntry>(*made.records->names[1].name).display_name=="supplied_alias_0","owning output does not borrow mutable caller or released import buffers");
}
void CustomSource(bool security=false) {
  scratchbird::tests::OwnedTempDirectory artifacts;
  const auto root=artifacts.path()/"selected-pack";
  std::filesystem::copy(SB_DEFAULT_POLICY_PACK_ROOT,root,std::filesystem::copy_options::recursive);
  const auto manifest=root/"POLICY_PACK_MANIFEST.json";std::string content;
  {std::ifstream input(manifest,std::ios::binary);content.assign(std::istreambuf_iterator<char>(input),{});Check(bool(input),"actual copied manifest readable");}
  const auto replace=[&](std::string_view from,std::string_view to) {
    const auto at=content.find(from);Check(at!=content.npos,"actual custom manifest field must exist");content.replace(at,from.size(),to);
  };
  replace("\"policy_pack_id\": \"default-local-password\"","\"policy_pack_id\": \"selected-custom-label\"");
  replace("\"policy_pack_version\": \"1.0.0\"","\"policy_pack_version\": \"1.0.9\"");
  {std::ofstream output(manifest,std::ios::binary|std::ios::trunc);output<<content;Check(bool(output),"write actual selected manifest");}
  Package package(security);auto request=package.Request();const auto path=root.string();request.policy_seed_pack_root=path;
  const auto made=security?db::AssembleNativeInitialPolicySecurityRecords({request,package.memberships}):db::AssembleNativeInitialPolicyRecords(request);Check(made.ok(),"actual custom manifest selection assembles native records without fixed-label fallback");
  const auto pack=c::DecodeCatalogBootstrapPolicyPack(made.records->definitions[0].metadata.record.payload);
  Check(pack.ok()&&pack.record->pack_id=="selected-custom-label"&&pack.record->pack_version=="1.0.9"&&pack.record->policy_pack_uuid==SourceId(1),
      "manifest labels remain data, exact binary pack identity is retained");
  Check(std::filesystem::remove_all(root)>0,"remove exact owned mutable source after successful assembly");
  Verify(*made.records,security);
}
void SecuritySourceMutations() {
  for(unsigned scenario=0;scenario<2;++scenario){
    scratchbird::tests::OwnedTempDirectory artifacts;
    const auto root=artifacts.path()/"selected-security-pack";
    std::filesystem::copy(SB_DEFAULT_POLICY_PACK_ROOT,root,std::filesystem::copy_options::recursive);
    scratchbird::tests::RewriteAndResealPolicyPack(root,[&](const auto& path,auto& text,const auto& replace){
      if(scenario==0){
        if(path=="policies/roles.json")replace(text,"Application runtime role for connect and event delivery.","Custom admitted role description.");
        if(path=="policies/groups.json")replace(text,"Baseline public group with no implicit engine authority.","Custom admitted group description.");
        if(path=="policies/grants.json"){
          replace(text,"\"right\": \"CONNECT\"","\"right\": \"AUDIT_READ\"");
          replace(text,"\"effect\": \"allow\"","\"effect\": \"deny\"");
          replace(text,"\"target_uuid\": \"\"","\"target_uuid\": \"018f7a10-1280-7000-8000-000000000106\"");
        }
      }else if(path=="policies/groups.json"){
        // Duplicate the first complete source relationship with a valid new
        // content hash: this must reach semantic admission, not hash refusal.
        const auto list=text.find("\"memberships\"");const auto first=text.find('{',list);const auto last=text.find('}',first);
        Check(list!=text.npos&&first!=text.npos&&last!=text.npos,"actual source membership to duplicate");
        const auto entry=text.substr(first,last-first+1);text.insert(last+1,","+entry);
      }
    });
    Package package(true);auto request=package.SecurityRequest();const auto source=root.string();request.policy.policy_seed_pack_root=source;
    const auto made=db::AssembleNativeInitialPolicySecurityRecords(request);
    if(scenario==0){
      Check(made.ok(),"actual changed source values, no hardcoded default fallback");
      unsigned changed=0,targets=0;
      for(const auto& record:made.records->definitions){
        if(!c::IsCatalogSecurityRecordKind(record.metadata.record.header.kind))continue;
        const auto decoded=c::DecodeCatalogSecurityRecordView(record.metadata.record.header.kind,record.metadata.record.payload);
        Check(decoded.ok(),"custom security payload native decode");
        const auto id=record.metadata.record.header.object_uuid.value;
        if(id==SourceId(0x100)){Check(decoded.record->Attribute("description")=="Custom admitted role description.","actual selected role value");++changed;}
        if(id==SourceId(0x200)){Check(decoded.record->Attribute("description")=="Custom admitted group description.","actual selected group value");++changed;}
        if(id==SourceId(0x300)){Check(decoded.record->Attribute("right")=="AUDIT_READ","actual selected grant value");++changed;}
        if(decoded.record->Attribute("grant_class")=="privilege"){
          Check(decoded.record->Identity("target_uuid")==SourceId(0x106)&&decoded.record->Attribute("effect")=="deny",
              "actual nonempty target and explicit deny retained, not text/unscoped/default-allow fallback");++targets;
        }
      }
      Check(changed==3&&targets==10,"every changed value and actual targeted grant retained");
    }else{
      Check(!made.ok()&&!made.records&&made.error==db::NativeInitialPolicyError::source_invalid&&made.source_diagnostic&&
          made.source_diagnostic->diagnostic_code=="SB-POLICY-PACK-GROUPS-INVALID",
          "resealed duplicate source pair refuses semantically before native output");
      db::DatabaseCreateConfig create;create.path=(artifacts.path()/"must-not-publish.sbdb").string();
      create.database_uuid=Typed(p::UuidKind::database,1);create.filespace_uuid=Typed(p::UuidKind::filespace,2);
      create.page_size=16384;create.creation_unix_epoch_millis=1789357072000ULL;
      create.require_resource_seed_pack=false;create.allow_minimal_resource_bootstrap=true;
      create.allow_uncredentialed_bootstrap=true;
      create.policy_seed_pack_root=source;
      const auto refused=db::CreateDatabaseFile(create);
      Check(!refused.ok()&&refused.diagnostic.diagnostic_code=="SB-POLICY-PACK-GROUPS-INVALID"&&
          !std::filesystem::exists(create.path),"same production importer refuses duplicate membership before physical publication");
    }
  }
}
void Physical(bool security=false) {
  scratchbird::tests::OwnedTempDirectory artifacts;
  Package package(security);const auto made=security?db::AssembleNativeInitialPolicySecurityRecords(package.SecurityRequest()):db::AssembleNativeInitialPolicyRecords(package.Request());Check(made.ok(),"actual source producer before physical construction");
  for(const auto& profile:d::kCanonicalFilespacePageProfiles) {
    db::NativeCreationCatalogSeed seed;seed[0]=made.records->definitions;seed[1]=made.records->names;
    db::NativeCreationCatalogIndexes indexes;
    for(unsigned role=0;role<2;++role) {
      scratchbird::storage::page::NativeBtreeDependencies deps;deps.index_uuid=f::Id(8010+role);
      deps.descriptor_generation=deps.storage_generation=1;deps.key_profile_uuid=f::Id(8020+role);deps.visibility_profile_uuid=f::Id(8030+role);
      deps.dependency_map_uuid=f::Id(8040+role);deps.dependency_map_sha256.fill(1+role);indexes[role]=db::NativeCreationCatalogIndexBinding{f::Id(8000+role),deps};
    }
    db::NativeFilespaceInitializationRequest request;
    request.bootstrap={f::Id(1),f::Id(2),profile.uuid,d::kNativeBootstrapIntegrityProfile,{},profile.page_size_bytes,1,0,1,7};
    request.operation_uuid=f::Id(3);request.writer_uuid=f::Id(4);request.policy_snapshot_uuid=f::Id(10);
    request.creator.transaction_uuid=tx;request.creator.local_id=mga::MakeLocalTransactionId(1);request.creator.scope=mga::TransactionScope::local_node;
    request.total_pages=256;request.creation_utc_millis=1789357072000ULL;
    u::StandaloneUuidV7Issuer issuer({f::Id(1),f::Id(10)},{{},0,1000});const auto path=(artifacts.path()/std::to_string(profile.page_size_bytes)).string();d::FileDevice device;
    Check(device.Open(path,d::FileOpenMode::create_new).ok(),"actual private native construction file");const auto budget=512*p::u64{profile.page_size_bytes};
    const auto created=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(device,request,seed,indexes,budget,issuer);
    Check(created.ok(),"actual imported native policy/name cohort persists through indexed construction");
    Check(device.Close().ok()&&device.Open(path,d::FileOpenMode::open_existing).ok(),"independent native policy reopen");
    const std::vector<d::NativeFilespaceDevice> devices{{f::Id(2),profile.uuid,&device}};
    const auto rows=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(f::Id(1),devices,created.receipt->checkpoint,2,1,
        {indexes[0]->relation_uuid,indexes[0]->dependencies},budget);
    Check(rows.ok()&&rows.rows.size()==package.items.size(),"all actual native imported definitions visible after committed construction and reopen");
    for(const auto& row:rows.rows) {
      const auto original=std::find_if(seed[0].begin(),seed[0].end(),[&](const auto& r){return r.metadata.record.header.object_uuid.value==row.metadata.record.header.object_uuid.value;});
      Check(original!=seed[0].end()&&row.metadata.record.payload==original->metadata.record.payload,"exact native imported bytes and raw identity survive persistence");
    }
    const auto names=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(f::Id(1),devices,created.receipt->checkpoint,2,2,
        {indexes[1]->relation_uuid,indexes[1]->dependencies},budget);
    Check(names.ok()&&names.rows.size()==2*package.items.size(),"every supplied localized name persists separately from object identity");
    const auto zero=d::ReadFilespacePageZeroFromOpenDevice(device);Check(zero.ok()&&zero.record->bootstrap.lifecycle_state==7,"construction does not falsely activate node or authorize service");
    Check(device.Close().ok(),"actual native policy construction device released");
  }
}
}
int main()try{Logical();SecurityLogical();CustomSource();CustomSource(true);SecuritySourceMutations();Physical();Physical(true);std::cout<<"PASS native policy creation "<<checks<<" checks\n";return 0;}
catch(const std::exception& e){allocation_budget=-1;std::cerr<<e.what()<<'\n';return 1;}
