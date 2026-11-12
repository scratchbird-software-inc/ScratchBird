// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_policy_creation.hpp"
#include <map>
#include <set>
#include "native_initial_policy_catalogs.hpp"
#include "database_lifecycle.hpp"
#include "bootstrap_security_validation.hpp"
#include "native_creation_workspace.hpp"
#include "native_catalog_relation_lease_reader.hpp"
#include "physical_mga_cow_store.hpp"
#include "bootstrap_policy_fixture.hpp"
#include "memory_configuration_fixture.hpp"
#include "catalog_security_record_codec.hpp"
#include "catalog_native_security_record.hpp"
#include "catalog_runtime_authority_binding.hpp"
#include "default_pack_agent_roles.inc"
#include "default_pack_agent_principals.inc"
#include "../support/owned_temp_directory.hpp"
#include "../support/policy_pack_fixture.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>

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
constexpr std::string_view fingerprint="local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=89abcdef0123456789abcdef01234567:"
    "verifier=abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
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
  // SB_NATIVE_CREATE_AGENT_PRINCIPALS: with a service, L5's SYSARCH membership
  // and every pack agent type's principal, grants and membership, issued by the
  // production helper from this fixed range and appended after the items below.
  static constexpr unsigned kL5Membership=15003,kAgentIdentityBase=70000;
  std::vector<db::NativeInitialAgentIdentity> agents;
  bool bootstrap_owned=false;
  // Default pack: 5 standard + 44 agent roles, 11 standard + 198 agent grants
  // (the 11th, 2026-10-30, grants CONNECT to the PUBLIC group), 199 agent
  // grants (the 199th grants OBS_AGENT_CONTROL_INTERNAL to transaction_pressure).
  static constexpr unsigned kRoles=49,kGrants=210,kSecurity=28+kRoles+6+5+kGrants;
  explicit Package(bool security=false,bool bootstrap=false,bool service=false)
      :items(service?kSecurity+10:bootstrap?kSecurity+3:security?kSecurity:28),bootstrap_owned(bootstrap) {
    for(unsigned i=0;i<memberships.size();++i)memberships[i]={f::Id(13000+i),SourceId(0x201+i),SourceId(0x100+i)};
    const unsigned base=static_cast<unsigned>(items.size());
    for(unsigned n=0;n<base;++n) {
      const unsigned roles_end=28+kRoles,groups_end=roles_end+6,members_end=groups_end+5,grants_end=members_end+kGrants;
      const unsigned t=n-grants_end;  // tail position, valid once n>=grants_end
      const auto object=n==0?SourceId(1):n<=12?SourceId(0x10+n-1):n<=26?SourceId(0x400+n-13):n==27?f::Id(12000):
          n<roles_end?(n-28<5?SourceId(0x100+n-28):SourceId(0x600+n-28-5)):
          n<groups_end?SourceId(0x200+n-roles_end):n<members_end?f::Id(13000+n-groups_end):
          n<grants_end?(n-members_end<11?SourceId(0x300+n-members_end):SourceId(0x1000+n-members_end-11)):
          t==0?SourceId(0x105):t<=2?f::Id(14000+t-1):t<=4?f::Id(15000+t-3):f::Id(15100+t-5);
      const bool role=n>=28&&(n<roles_end||t==0);
      const auto subtype=n==0?scratchbird::core::catalog::CatalogObjectSubtype::bootstrap_policy_pack:n<=12?scratchbird::core::catalog::CatalogObjectSubtype::bootstrap_security_provider:n<=26?scratchbird::core::catalog::CatalogObjectSubtype::bootstrap_policy_profile:n==27?scratchbird::core::catalog::CatalogObjectSubtype::memory_configuration:
          role?scratchbird::core::catalog::CatalogObjectSubtype::role_account:n<groups_end?scratchbird::core::catalog::CatalogObjectSubtype::group_account:n<grants_end?scratchbird::core::catalog::CatalogObjectSubtype::grant_record:t==1||t==3?scratchbird::core::catalog::CatalogObjectSubtype::user_account:t==4?scratchbird::core::catalog::CatalogObjectSubtype::agent_runtime_authority:scratchbird::core::catalog::CatalogObjectSubtype::grant_record;
      std::optional<c::CatalogRecordKind> kind;
      if(n==27)kind=c::CatalogRecordKind::config_profile;
      if(n>=28)kind=role?c::CatalogRecordKind::role_account:n<groups_end?c::CatalogRecordKind::group_account:n>=grants_end&&(t==1||t==3)?c::CatalogRecordKind::user_account:n>=grants_end&&t==4?c::CatalogRecordKind::config_profile:c::CatalogRecordKind::grant_record;
      Fill(n,object,kind,subtype,n>=grants_end&&t==4);
    }
    if(service) {
      unsigned issued=0;
      auto agent_ids=db::IssueNativeInitialAgentIdentities(SB_DEFAULT_POLICY_PACK_ROOT,
          [&]()->std::optional<p::Uuid>{return f::Id(kAgentIdentityBase+issued++);});
      Check(agent_ids.ok(),"production helper issues every pack agent identity");
      agents=std::move(agent_ids.agents);
      unsigned n=base;
      Append(n,f::Id(kL5Membership),c::CatalogRecordKind::grant_record);
      for(const auto& agent:agents) {
        Append(n,agent.principal_uuid,c::CatalogRecordKind::user_account);
        for(const auto& grant:agent.grant_uuids)Append(n,grant,c::CatalogRecordKind::grant_record);
        Append(n,agent.membership_uuid,c::CatalogRecordKind::grant_record);
      }
    }
    Rebind();
  }
  void Fill(unsigned n,const p::Uuid& object,std::optional<c::CatalogRecordKind> kind,scratchbird::core::catalog::CatalogObjectSubtype subtype,bool service_policy) {
      auto& x=items[n];
      auto& meta=x.definition;meta=f::Metadata(0,tx,f::Id(60),{});
      if(bootstrap_owned)meta.owner_uuid=Typed(p::UuidKind::principal,14000);
      meta.record.header.object_uuid={p::UuidKind::object,object};meta.record.header.row_uuid=Typed(p::UuidKind::row,200000+3*n);
      meta.object_subtype=subtype;if(kind)meta.record.header.kind=*kind;
      if(service_policy)meta.security_policy_uuid={p::UuidKind::object,SourceId(0x402)};
      meta.default_name_uuid=Typed(p::UuidKind::object,300000+2*n);meta.name_vector_uuid=Typed(p::UuidKind::object,300001+2*n);
      x.vector_metadata=meta;x.vector_metadata.record.header.kind=c::CatalogRecordKind::localized_name;
      x.vector_metadata.record.header.object_uuid=meta.name_vector_uuid;x.vector_metadata.record.header.row_uuid=Typed(p::UuidKind::row,200001+3*n);
      x.vector_metadata.record.header.parent_uuid=meta.record.header.object_uuid;x.vector_metadata.object_subtype=scratchbird::core::catalog::CatalogObjectSubtype::name_vector;
      auto& v=x.vector;v.name_vector_uuid=meta.name_vector_uuid;v.object_uuid=meta.record.header.object_uuid;
      v.object_kind=meta.record.header.kind;v.owning_schema_uuid=meta.owning_schema_uuid;
      v.default_language_uuid=c::CatalogLanguageIdentity(c::kCatalogLanguage_en_US);v.default_name_entry_uuid=meta.default_name_uuid;
      v.name_collision_policy_uuid=Typed(p::UuidKind::object,55);v.catalog_generation_id=1;
      v.security_policy_uuid=meta.security_policy_uuid;v.lifecycle_state=c::CatalogNameLifecycle::active;
      x.entry_metadata=meta;x.entry_metadata.record.header.kind=c::CatalogRecordKind::localized_name;
      x.entry_metadata.record.header.object_uuid=meta.default_name_uuid;x.entry_metadata.record.header.row_uuid=Typed(p::UuidKind::row,200002+3*n);
      x.entry_metadata.record.header.parent_uuid=meta.name_vector_uuid;x.entry_metadata.object_subtype=scratchbird::core::catalog::CatalogObjectSubtype::name_entry;x.entry_metadata.resource_epoch=1;
      auto& e=x.entry;e.name_entry_uuid=meta.default_name_uuid;e.name_vector_uuid=meta.name_vector_uuid;
      e.object_uuid=meta.record.header.object_uuid;e.object_kind=v.object_kind;e.scope_uuid=meta.record.header.parent_uuid;
      e.parent_schema_uuid=meta.owning_schema_uuid;e.language_uuid=c::CatalogLanguageIdentity(c::kCatalogLanguage_en_US);e.dialect_profile_uuid=Typed(p::UuidKind::object,56);
      e.identifier_profile_uuid=Typed(p::UuidKind::object,57);e.raw_name_text=e.display_name="supplied_alias_"+std::to_string(n);
      e.normalized_lookup_key.assign(e.raw_name_text.begin(),e.raw_name_text.end());e.catalog_generation_id=1;
      e.created_transaction_uuid=tx;e.security_policy_uuid=meta.security_policy_uuid;
      e.resource_epoch=e.name_resolution_epoch=1;e.default_for_language=e.default_for_object=true;e.lifecycle_state=c::CatalogNameLifecycle::active;
  }
  void Append(unsigned& n,const p::Uuid& object,c::CatalogRecordKind kind) {
    items.emplace_back();Fill(n++,object,kind,c::CatalogSecuritySubtype(kind),false);
  }
  void Rebind(){inputs.clear();for(auto& x:items){x.entries[0]={&x.entry_metadata,&x.entry};inputs.push_back({&x.definition,&x.vector_metadata,&x.vector,x.entries});}}
  auto Request()const{return db::NativeInitialPolicyRequest{SB_DEFAULT_POLICY_PACK_ROOT,f::Id(1),tx,f::Id(12000),c::kCatalogLanguage_en_US,inputs,{3000,16*1024*1024}};}
  auto SecurityRequest()const{return db::NativeInitialPolicySecurityRequest{Request(),memberships};}
  auto BootstrapRequest()const{return db::NativeInitialPolicySecurityRequest{Request(),memberships,
      db::NativeInitialBootstrapIdentity{f::Id(14000),f::Id(14001),"BootstrapAdmin",fingerprint}};}
  auto ServiceRequest()const{auto request=BootstrapRequest();request.local_service=db::NativeInitialLocalServiceIdentity{
      f::Id(15000),f::Id(15001),f::Id(15002),SourceId(0x10),SourceId(0x402),
      {f::Id(15100),f::Id(15101),f::Id(15102),f::Id(15103),f::Id(15104)},"NodeService",fingerprint,f::Id(kL5Membership)};
    request.agents=agents;return request;}
};
// ---- 3a-2: independent hand-built SBCV goldens of the typed native security
// schema (65635 principal, 65636 role, 65637 group, 65638 grant; version 1).
// Field numbers and enumeration codes are written out here, not taken from
// the production schema or encoder. A right's code is its coding-time
// constant (catalog_security_constants.hpp), the definition of that code.
p::u64 RightCode(std::string_view tag){const auto r=c::CatalogSecurityRightForTag(tag);Check(r.has_value(),"every golden right has a constant");return static_cast<p::u64>(*r);}
p::u64 AuthorityCode(std::string_view s){
  static constexpr std::array<std::string_view,6> names{"engine_owned_sysarch","l5_database","direct_bounded_action",
      "observe_only","recommend_only","request_action"};
  for(unsigned i=0;i<names.size();++i)if(names[i]==s)return i+1;
  Check(false,"golden authority class listed");return 0;
}
std::string HexBytes(std::string_view hex){std::string out;
  for(std::size_t i=0;i+1<hex.size();i+=2)out.push_back(static_cast<char>(std::stoi(std::string(hex.substr(i,2)),nullptr,16)));return out;}
// The fixture fingerprint's parts, parsed independently of the engine.
struct BootstrapCredential {
  p::u64 iterations=0;std::string salt,verifier;
  explicit BootstrapCredential(std::string_view text){
    const auto at=[&](std::string_view key){const auto k=text.find(key);Check(k!=text.npos,"fixture fingerprint field");return k+key.size();};
    const auto i=at("iterations="),sa=at(":salt="),v=at(":verifier=");
    iterations=std::stoull(std::string(text.substr(i,text.find(':',i)-i)));
    salt=HexBytes(text.substr(sa,32));verifier=HexBytes(text.substr(v,64));
  }
  bool Matches(const c::NativeSecurityCredential& credential)const{
    return credential.scheme==c::CatalogSecurityCredentialScheme::pbkdf2_sha256_v1&&credential.iterations==iterations&&
        std::string(reinterpret_cast<const char*>(credential.salt.data()),16)==salt&&
        std::string(reinterpret_cast<const char*>(credential.verifier.data()),32)==verifier;
  }
};
// Fields 1..6: primary, origin transaction, origin local 1, creator_tx 1,
// policy generation 1, optional pack.
void TypedCommon(f::Golden& g,const p::Uuid& id,const std::optional<p::Uuid>& pack){
  g.Identity(1,id);g.Identity(2,tx.value);g.Integer(3,1);g.Integer(4,1);g.Integer(5,1);if(pack)g.Identity(6,*pack);}
// principal_kind 1 user / 2 service; the bootstrap/service credential is the
// fixture fingerprint's parts (600000, salt, verifier); agents have none.
std::string TypedPrincipal(const p::Uuid& id,p::u64 kind,std::optional<p::u64> authority,bool active,bool create_time_only,
    bool bootstrap,bool credential=true){
  f::Golden g(65635);TypedCommon(g,id,std::nullopt);g.Integer(7,kind);if(authority)g.Integer(8,*authority);
  g.Boolean(9,active);g.Boolean(10,create_time_only);g.Boolean(11,bootstrap);g.Integer(12,1);g.Integer(13,1);g.Integer(14,1);
  if(credential){g.Integer(15,1);g.Integer(16,600000);g.Field(17,4,HexBytes("89abcdef0123456789abcdef01234567"));
    g.Field(18,4,HexBytes("abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"));}
  return g.Finish();
}
std::string TypedRole(const p::Uuid& id,const std::optional<p::Uuid>& pack,std::optional<p::u64> authority,bool engine_owned,
    bool immutable,bool active,bool loaded,bool create_time_only,p::u64 context_version,p::u64 context_generation){
  f::Golden g(65636);TypedCommon(g,id,pack);if(authority)g.Integer(7,*authority);g.Boolean(8,engine_owned);g.Boolean(9,immutable);
  g.Boolean(10,active);g.Boolean(11,loaded);g.Boolean(12,create_time_only);g.Integer(13,context_version);g.Integer(14,context_generation);
  return g.Finish();
}
std::string TypedGroup(const p::Uuid& id,const p::Uuid& pack){
  f::Golden g(65637);TypedCommon(g,id,pack);g.Boolean(7,true);g.Boolean(8,true);return g.Finish();}
// grant_class 1 privilege / 2 role_membership / 3 group_membership; subject,
// member and parent kinds 1 principal / 2 role / 3 group; effect 1 allow.
struct GrantGolden{p::Uuid id;std::optional<p::Uuid> pack,subject,member,parent,target,principal,role;
  p::u64 cls=0,sk=0,mk=0,pk=0,effect=0,right=0,security_generation=0;};
std::string TypedGrant(const GrantGolden& r){
  f::Golden g(65638);TypedCommon(g,r.id,r.pack);g.Integer(7,r.cls);
  if(r.subject)g.Identity(8,*r.subject);if(r.member)g.Identity(9,*r.member);if(r.parent)g.Identity(10,*r.parent);
  if(r.target)g.Identity(11,*r.target);if(r.principal)g.Identity(13,*r.principal);if(r.role)g.Identity(14,*r.role);
  if(r.sk)g.Integer(15,r.sk);if(r.mk)g.Integer(16,r.mk);if(r.pk)g.Integer(17,r.pk);if(r.effect)g.Integer(18,r.effect);
  if(r.right)g.Integer(19,r.right);g.Integer(20,r.security_generation);return g.Finish();
}
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
  constexpr unsigned kAgentRoles=std::size(kDefaultPackAgentRoles);
  // 5 roles, 6 groups, 5 memberships, 11 standard grants (16..26), then agents.
  const bool agent_role=n>=27&&n<27+kAgentRoles,agent_grant=n>=27+kAgentRoles;
  const auto* ar=agent_role?&kDefaultPackAgentRoles[n-27]:nullptr;
  const auto* ag=agent_grant?&kDefaultPackAgentGrants[n-27-kAgentRoles]:nullptr;
  const bool public_connect=n==26;  // CONNECT to the PUBLIC group
  const unsigned kind=n<5||agent_role?62:n<11?61:63;
  const auto id=agent_role?SourceId(ar->suffix):agent_grant?SourceId(ag->suffix):
      n<5?SourceId(0x100+n):n<11?SourceId(0x200+n-5):n<16?f::Id(13000+n-11):SourceId(0x300+n-16);
  // The pack's codes and descriptions above no longer appear in the typed
  // records (3a-2); kept as the pack's documented content.
  (void)roles;(void)role_descriptions;(void)groups;(void)group_descriptions;
  // 3a-2 (SB_CATALOG_NATIVE_SECURITY_RECORDS): the typed schema, hand-built.
  // Imported records: origin local 1, creator_tx 1, policy generation 1, the
  // pack, engine-owned and loaded at create. Codes, descriptions, default
  // assignments and identity_authority are no longer record fields (names go
  // to the names table; description/default_assignment dropped, owner Q65).
  if(kind==62)return {id,TypedRole(id,SourceId(1),std::nullopt,true,false,true,true,false,0,0)};
  if(kind==61)return {id,TypedGroup(id,SourceId(1))};
  GrantGolden r;r.id=id;r.pack=SourceId(1);
  if(n>=11&&n<16){r.cls=3;r.member=SourceId(0x201+n-11);r.parent=SourceId(0x100+n-11);r.mk=3;r.pk=2;r.effect=1;}
  else{r.cls=1;r.effect=1;
    if(agent_grant){r.subject=SourceId(ag->subject);r.sk=2;r.right=RightCode(ag->right);}
    else if(public_connect){r.subject=SourceId(0x200);r.sk=3;r.right=RightCode("CONNECT");}
    else{r.subject=SourceId(0x100+subjects[n-16]);r.sk=2;r.right=RightCode(rights[n-16]);}}
  return {id,TypedGrant(r)};
}
std::pair<p::Uuid,std::string> BootstrapGolden(unsigned n) {
  // 3a-2 typed schema. SYSARCH (canonical UUID, no pack), the bootstrap
  // principal (user, its binary credential) and its SYSARCH membership.
  if(n==0)return {SourceId(0x105),TypedRole(SourceId(0x105),std::nullopt,1,true,true,true,false,true,1,1)};
  if(n==1)return {f::Id(14000),TypedPrincipal(f::Id(14000),1,std::nullopt,true,true,true)};
  GrantGolden r;r.id=f::Id(14001);r.cls=2;r.member=f::Id(14000);r.parent=SourceId(0x105);r.principal=f::Id(14000);
  r.role=SourceId(0x105);r.mk=1;r.pk=2;r.security_generation=1;
  return {r.id,TypedGrant(r)};
}
std::pair<p::Uuid,std::string> ServiceGolden(unsigned n) {
  if(n==1){
    f::Golden g(65587);g.Identity(1,f::Id(15001));g.Integer(2,1);g.Identity(3,f::Id(1));
    g.Identity(4,f::Id(15000));g.Identity(5,f::Id(1));g.Identity(6,SourceId(0x10));g.Identity(7,SourceId(0x402));
    g.Identity(8,f::Id(15002));g.Integer(9,1);g.Integer(10,1);g.Integer(11,1);g.Integer(12,1);g.Integer(13,1);
    g.Identity(14,tx.value);g.Integer(15,1);return {f::Id(15001),g.Finish()};
  }
  // 3a-2 typed schema. The L5 principal (service, authority class l5_database,
  // its binary credential) and its five direct grants.
  if(n==0)return {f::Id(15000),TypedPrincipal(f::Id(15000),2,2,true,false,false)};
  static constexpr std::array<const char*,5> rights{"CONNECT","CATALOG_MUTATE","OBS_AGENT_STATE_READ","OBS_AGENT_EVIDENCE_READ","OBS_AGENT_CONTROL"};
  GrantGolden r;r.id=f::Id(15100+n-2);r.cls=1;r.subject=f::Id(15000);r.sk=1;r.effect=1;r.right=RightCode(rights[n-2]);r.security_generation=1;
  return {r.id,TypedGrant(r)};
}
// SB_NATIVE_CREATE_AGENT_PRINCIPALS (2026-11-03): L5's SYSARCH membership (D5)
// and every pack agent type's principal, grants and task-group membership,
// encoded independently like the goldens above, keyed by object identity. The
// rights, roles and authority classes come from the generated pack table.
// 3a-2: typed schema; a grant record has no create_time_only field.
std::map<p::Uuid,std::string> AgentGoldens(const std::vector<db::NativeInitialAgentIdentity>& agents) {
  Check(agents.size()==std::size(kDefaultPackAgentPrincipals),"one issued identity set per pack agent type");
  std::map<p::Uuid,std::string> out;
  const auto membership=[&](const p::Uuid& id,const p::Uuid& principal,const p::Uuid& role){
    GrantGolden r;r.id=id;r.cls=2;r.member=principal;r.parent=role;r.principal=principal;r.role=role;r.mk=1;r.pk=2;r.security_generation=1;
    Check(out.emplace(id,TypedGrant(r)).second,"distinct golden membership identity");};
  membership(f::Id(Package::kL5Membership),f::Id(15000),SourceId(0x105));
  for(std::size_t i=0;i<agents.size();++i){
    const auto& expected=kDefaultPackAgentPrincipals[i];const auto& agent=agents[i];
    Check(agent.agent_type_id==expected.type&&agent.grant_uuids.size()==expected.right_count,"issued identities follow pack order");
    const auto authority=*expected.authority_class?std::optional<p::u64>(AuthorityCode(expected.authority_class)):std::nullopt;
    Check(out.emplace(agent.principal_uuid,TypedPrincipal(agent.principal_uuid,2,authority,true,false,false,false)).second,
        "distinct golden agent principal");
    for(unsigned r=0;r<expected.right_count;++r){
      GrantGolden g;g.id=agent.grant_uuids[r];g.cls=1;g.subject=agent.principal_uuid;g.sk=1;g.effect=1;
      g.right=RightCode(kDefaultPackAgentRights[expected.first_right+r]);g.security_generation=1;
      Check(out.emplace(agent.grant_uuids[r],TypedGrant(g)).second,"distinct golden agent grant");}
    membership(agent.membership_uuid,agent.principal_uuid,SourceId(expected.role_suffix));
  }
  return out;
}
void Verify(const c::CatalogNamedCreationSet& out,bool security=false,bool bootstrap=false,bool service=false,
            const std::vector<db::NativeInitialAgentIdentity>* agents=nullptr) {
  Check(!service||agents,"a service package verifies its agent records");
  const auto agent_goldens=service?AgentGoldens(*agents):std::map<p::Uuid,std::string>{};
  std::set<p::Uuid> agent_seen;
  const unsigned count=service?Package::kSecurity+10+static_cast<unsigned>(agent_goldens.size()):
      bootstrap?Package::kSecurity+3:security?Package::kSecurity:28;
  Check(out.definitions.size()==count&&out.names.size()==2*count&&out.encoded_bytes>0,"all admitted policy/security definitions and every supplied name returned");
  unsigned packs=0,providers=0,profiles=0,memories=0,bindings=0;
  constexpr unsigned kSecurityRecords=27+std::size(kDefaultPackAgentRoles)+std::size(kDefaultPackAgentGrants);  // 5+6+5+11 standard
  std::array<bool,kSecurityRecords> security_seen{};
  std::array<bool,3> bootstrap_seen{};
  std::array<bool,7> service_seen{};
  for(const auto& record:out.definitions) {
    const auto& meta=record.metadata;
    Check(c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(meta)).has_value(),"complete native common metadata binding");
    if(service){bool matched=false;
      for(unsigned i=0;i<service_seen.size();++i){const auto expected=ServiceGolden(i);
        if(meta.record.header.object_uuid.value!=expected.first)continue;
        Check(!service_seen[i]&&meta.record.payload==expected.second,"independent complete native service principal, direct grants and runtime binding bytes");
        Check(meta.owner_uuid.kind==p::UuidKind::principal&&meta.owner_uuid.value==f::Id(14000)&&
            meta.creator_transaction_uuid.kind==tx.kind&&meta.creator_transaction_uuid.value==tx.value&&meta.creator_local_transaction_id==1,
            "service records retain explicit bootstrap ownership and actual construction origin");
        service_seen[i]=true;matched=true;
      }
      if(matched)continue;
      const auto agent=agent_goldens.find(meta.record.header.object_uuid.value);
      if(agent!=agent_goldens.end()){
        Check(agent_seen.insert(agent->first).second&&meta.record.payload==agent->second,
            "independent complete native L5 membership and agent principal, grant and membership bytes");
        Check(meta.owner_uuid.kind==p::UuidKind::principal&&meta.owner_uuid.value==f::Id(14000)&&
            meta.creator_transaction_uuid.value==tx.value&&meta.creator_local_transaction_id==1,
            "agent records retain explicit bootstrap ownership and actual construction origin");
        continue;
      }
    }
    if(c::IsCatalogSecurityRecordKind(meta.record.header.kind)) {
      bool matched=false;
      if(bootstrap)for(unsigned i=0;i<bootstrap_seen.size();++i){const auto expected=BootstrapGolden(i);
        if(meta.record.header.object_uuid.value!=expected.first)continue;
        Check(!bootstrap_seen[i]&&meta.record.payload==expected.second,"complete independently encoded bootstrap values and identities");
        bootstrap_seen[i]=true;matched=true;
      }
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
    if(pack.ok()){++packs;Check(pack.record->policy_pack_uuid==SourceId(1)&&pack.record->declared_counts==std::array<p::u64,9>{12,1,11,49,6,5,210,14,58},"actual admitted pack counts and identity");}
    else if(provider.ok()){++providers;Check(provider.record->provider_uuid==SourceId(0x10+static_cast<unsigned>(provider.record->family)-1)&&provider.record->policy_pack_uuid==SourceId(1),"every real imported provider keeps exact binary source identity and family");}
    else if(profile.ok()){++profiles;if(profile.record->area==c::BootstrapPolicyArea::memory_resource_governance) {/*3b*/
      ++bindings;Check(profile.record->resource_definition_uuid==f::Id(12000)&&profile.record->resource_definition_generation==1,"real materialized resource binding, no inferred identity/generation");}}
    else if(memory.ok()){
      ++memories;auto expected=m::Values(tx);expected.policy_pack_uuid=SourceId(1);expected.profile_uuid=SourceId(0x403);
      Check(m::Golden(expected)==meta.record.payload,"all actual memory settings and exact references match independent bytes");
    }else Check(false,"no legacy policy body or unsupported family escaped native producer");
    Check(meta.creator_transaction_uuid.kind==tx.kind&&meta.creator_transaction_uuid.value==tx.value&&meta.creator_local_transaction_id==1,
        "supplied actual creation origin retained without manufactured transaction");
  }
  Check(packs==1&&providers==12&&profiles==14&&memories==1&&bindings==1,"complete source family coverage");
  Check(std::count(security_seen.begin(),security_seen.end(),true)==(security?kSecurityRecords:0),"complete security coverage, no bootstrap or service principal invented");
  Check(std::count(bootstrap_seen.begin(),bootstrap_seen.end(),true)==(bootstrap?3:0),"bootstrap supplied explicitly or wholly absent");
  Check(std::count(service_seen.begin(),service_seen.end(),true)==(service?7:0),"service enrollment is complete or wholly absent, never inferred");
  Check(agent_seen.size()==agent_goldens.size(),"L5 membership and every agent principal, grant and membership are present");
}
void BootstrapLogical() {
  Package package(true,true);auto request=package.BootstrapRequest();
  std::string name="BootstrapAdmin",credential_input(fingerprint);
  request.bootstrap->principal_name=name;request.bootstrap->credential_fingerprint=credential_input;
  const auto made=db::AssembleNativeInitialPolicySecurityRecords(request);Check(made.ok(),"explicit native bootstrap enrollment");
  name.assign("changed");credential_input.assign("destroyed caller credential input");Verify(*made.records,true,true);
  request.bootstrap->principal_name="BootstrapAdmin";request.bootstrap->credential_fingerprint=fingerprint;
  // 3a-2: native records are typed; the legacy semantic validator
  // (ValidateBootstrapSecurityRecords) reads only the legacy attribute bag, a
  // drift path left unchanged, so the same identities, credential and context
  // generation are checked on the typed records directly. The principal's name
  // ("BootstrapAdmin") is a names-table entry, which this caller supplies, so
  // it is not a record field (the generated-name path is exercised by
  // native_generated_definition_creation_test).
  {
    std::optional<c::NativeSecurityPrincipal> principal;std::optional<c::NativeSecurityGrant> membership;std::optional<c::NativeSecurityRole> sysarch;
    for(const auto& record:made.records->definitions){
      const auto& meta=record.metadata;if(!c::IsCatalogSecurityRecordKind(meta.record.header.kind))continue;
      const auto typed=c::DecodeNativeSecurityRecord(meta.record.header.kind,meta.record.payload);
      Check(typed.ok()&&c::NativeSecurityRecordMatchesMetadata(c::BorrowCatalogMetadataVersion(meta)),"every native security record is the typed record");
      if(meta.record.header.object_uuid.value==f::Id(14000))principal=*typed.record->principal();
      if(meta.record.header.object_uuid.value==f::Id(14001))membership=*typed.record->grant();
      if(meta.record.header.object_uuid.value==SourceId(0x105))sysarch=*typed.record->role();
    }
    BootstrapCredential expected(fingerprint);
    Check(principal&&principal->principal_kind==c::CatalogSecurityPrincipalKind::user&&principal->bootstrap_principal&&principal->active&&
        principal->security_context_generation==1&&principal->credential&&expected.Matches(*principal->credential)&&
        membership&&membership->member_uuid==f::Id(14000)&&membership->parent_uuid==SourceId(0x105)&&
        membership->grant_class==c::CatalogSecurityGrantClass::role_membership&&
        sysarch&&sysarch->authority_class==c::CatalogSecurityAuthorityClass::engine_owned_sysarch&&sysarch->engine_owned&&sysarch->immutable,
        "typed bootstrap records preserve identities and the parsed credential, not commitment or authentication");
  }
  for(unsigned scenario=0;scenario<24;++scenario){Package bad(true,true);auto changed=bad.BootstrapRequest();std::string credential;
    auto& b=*changed.bootstrap;
    if(scenario==0)b.principal_uuid={};if(scenario==1)b.membership_uuid.bytes[6]=0x40;
    if(scenario==2)b.membership_uuid=b.principal_uuid;if(scenario==3)b.principal_uuid=SourceId(0x105);
    if(scenario==4)b.principal_uuid=changed.policy.database_uuid;if(scenario==5)b.principal_uuid=SourceId(0x100);
    if(scenario==6)b.membership_uuid=SourceId(0x300);
    if(scenario==7)b.principal_name="bad name";if(scenario==8)b.principal_name="";
    if(scenario==9)b.principal_name={static_cast<const char*>(nullptr),1};
    if(scenario==10)b.credential_fingerprint="";if(scenario==11)b.credential_fingerprint="sha256:not-a-verifier";
    if(scenario==12){credential=fingerprint;const auto at=credential.find("89abcdef0123456789abcdef01234567");credential.replace(at,32,std::string(32,'0'));b.credential_fingerprint=credential;}
    if(scenario==13){credential=fingerprint;credential.replace(credential.find("600000"),6,"599999");b.credential_fingerprint=credential;}
    if(scenario==14){credential=fingerprint;credential.replace(credential.find("verifier=")+9,64,std::string(64,'0'));b.credential_fingerprint=credential;}
    if(scenario==15)changed.policy.definitions=changed.policy.definitions.first(Package::kSecurity+2);
    if(scenario==16)bad.items[Package::kSecurity+1].definition.record.header.kind=c::CatalogRecordKind::role_account;
    if(scenario==17)changed.policy.limits.maximum_records=3*(Package::kSecurity+3)-1;
    if(scenario==18)changed.policy.limits.maximum_encoded_bytes=made.records->encoded_bytes-1;
    if(scenario==19)changed.bootstrap.reset();
    if(scenario==20)b.credential_fingerprint={static_cast<const char*>(nullptr),1};
    if(scenario==21){credential=fingerprint;credential.replace(credential.find("600000"),6,"10000001");b.credential_fingerprint=credential;}
    if(scenario==22){credential=fingerprint;credential.replace(credential.find("600000"),6,"0600000");b.credential_fingerprint=credential;}
    if(scenario==23){credential=fingerprint;credential.push_back('\0');b.credential_fingerprint=credential;}
    const auto refused=db::AssembleNativeInitialPolicySecurityRecords(changed);
    Check(!refused.ok()&&!refused.records,"invalid credential, identity, bootstrap coverage and budget refuse atomically");
  }
  request.policy.limits={3*(Package::kSecurity+3),made.records->encoded_bytes};Check(db::AssembleNativeInitialPolicySecurityRecords(request).ok(),"exact enrolled bootstrap limits");
  std::string maximum(fingerprint);maximum.replace(maximum.find("600000"),6,"10000000");
  // 3a-2: the typed credential is fixed-width, so the two extra digits no
  // longer grow the record; the +2 byte allowance is now unused slack.
  auto bounded=request;bounded.policy.limits.maximum_encoded_bytes+=2;bounded.bootstrap->credential_fingerprint=maximum;
  const auto admitted_maximum=db::AssembleNativeInitialPolicySecurityRecords(bounded);
  Check(admitted_maximum.ok(),"exact maximum verifier iteration bound accepted");
  for(const auto& record:admitted_maximum.records->definitions)if(record.metadata.record.header.kind==c::CatalogRecordKind::user_account){
    // 3a-2: the parsed parts are retained exactly (iterations 10000000).
    const auto native=c::DecodeNativeSecurityRecord(record.metadata.record.header.kind,record.metadata.record.payload);
    Check(native.ok()&&native.record->principal()->credential&&BootstrapCredential(maximum).Matches(*native.record->principal()->credential),
        "maximum verifier value is retained exactly, not normalized/truncated");}
  for(long budget:{0,1,5,31,97,257}){allocation_failed=false;allocation_budget=budget;
    const auto refused=db::AssembleNativeInitialPolicySecurityRecords(request);allocation_budget=-1;
    Check(allocation_failed&&!refused.ok()&&!refused.records,"actual bootstrap allocation fault cannot publish partial credentials or metadata");}
}
void ServiceLogical() {
  Package package(true,true,true);auto request=package.ServiceRequest();
  std::string name="NodeService",credential(fingerprint);
  request.local_service->principal_name=name;request.local_service->credential_fingerprint=credential;
  const auto made=db::AssembleNativeInitialPolicySecurityRecords(request);
  Check(made.ok(),"explicit local service provisioning from actual admitted provider and security policy");
  name.assign("destroyed caller name");credential.assign("destroyed caller verifier");Verify(*made.records,true,true,true,&package.agents);
  request.local_service->principal_name="NodeService";request.local_service->credential_fingerprint=fingerprint;
  const auto binding=std::find_if(made.records->definitions.begin(),made.records->definitions.end(),[](const auto& r){return r.metadata.record.header.object_uuid.value==f::Id(15001);});
  Check(binding!=made.records->definitions.end()&&c::CatalogRuntimeAuthorityBindingMatchesMetadata(binding->metadata),
      "runtime selection uses the qualified native binding metadata contract");
  const auto decoded=c::DecodeCatalogRuntimeAuthorityBinding(binding->metadata.record.payload);
  Check(decoded.ok()&&decoded.record->credential_reference_uuid==f::Id(15002)&&decoded.record->service_principal_uuid==f::Id(15000)&&
      decoded.record->database_uuid==f::Id(1)&&decoded.record->security_authority_uuid==f::Id(1),"exact node-bound service and separately custodied credential reference");
  for(unsigned scenario=0;scenario<43;++scenario){
    Package bad(true,true,true);auto changed=bad.ServiceRequest();auto& s=*changed.local_service;
    std::string invalid;
    if(scenario==0)changed.bootstrap.reset();
    if(scenario==1)s.principal_uuid={};if(scenario==2)s.runtime_binding_uuid.bytes[6]=0x40;
    if(scenario==3)s.credential_reference_uuid={};if(scenario==4)s.grant_uuids[4]={};
    if(scenario==5)s.principal_uuid=f::Id(14000);if(scenario==6)s.runtime_binding_uuid=f::Id(14001);
    if(scenario==7)s.credential_reference_uuid=SourceId(0x105);if(scenario==8)s.grant_uuids[0]=s.principal_uuid;
    if(scenario==9)s.grant_uuids[4]=s.grant_uuids[0];if(scenario==10)s.runtime_binding_uuid=s.principal_uuid;
    if(scenario==11)s.credential_reference_uuid=changed.policy.database_uuid;
    if(scenario==12)s.credential_reference_uuid=changed.policy.construction_transaction.value;
    if(scenario==13)s.credential_reference_uuid=changed.policy.memory_configuration_uuid;
    if(scenario==14)s.provider_uuid=SourceId(0x11);if(scenario==15)s.provider_uuid=f::Id(16000);
    if(scenario==16)s.policy_uuid=SourceId(0x400);if(scenario==17)s.policy_uuid=f::Id(16001);
    if(scenario==18)s.principal_name="bootstrapadmin";if(scenario==19)s.principal_name="rOoT";
    if(scenario==20)s.principal_name="";if(scenario==21)s.principal_name="not a name";
    if(scenario==22)s.principal_name={static_cast<const char*>(nullptr),1};
    if(scenario==23)s.credential_fingerprint="";if(scenario==24)s.credential_fingerprint="not-a-verifier";
    if(scenario==25)s.credential_fingerprint={static_cast<const char*>(nullptr),1};
    if(scenario==26){invalid=fingerprint;invalid.push_back('\0');s.credential_fingerprint=invalid;}
    if(scenario==27){invalid=fingerprint;invalid.replace(invalid.find("600000"),6,"0600000");s.credential_fingerprint=invalid;}
    if(scenario==28)changed.policy.definitions=changed.policy.definitions.first(Package::kSecurity+9);
    if(scenario==29)changed.local_service.reset();
    // One record short of the complete service package, agents included.
    if(scenario==30)changed.policy.limits.maximum_records=3*static_cast<unsigned>(bad.items.size())-1;
    if(scenario==31)changed.policy.limits.maximum_encoded_bytes=made.records->encoded_bytes-1;
    if(scenario==32)bad.items[Package::kSecurity+3].definition.owner_uuid=Typed(p::UuidKind::principal,16002);
    if(scenario==33)bad.items[Package::kSecurity+4].definition.security_policy_uuid=Typed(p::UuidKind::object,55);
    if(scenario==34)bad.items[Package::kSecurity+4].definition.object_subtype=scratchbird::core::catalog::CatalogObjectSubtype::memory_configuration;
    if(scenario==35)bad.items[Package::kSecurity+9].definition.owner_uuid=Typed(p::UuidKind::principal,16002);
    if(scenario==36)s.credential_reference_uuid=bad.items[Package::kSecurity+9].definition.record.header.row_uuid.value;
    if(scenario==37)s.credential_reference_uuid=bad.items[Package::kSecurity+9].entry.name_entry_uuid.value;
    if(scenario==38)s.credential_reference_uuid=SourceId(0x300);
    if(scenario==39)s.grant_uuids[4]=SourceId(0x300);
    if(scenario==40)bad.items[Package::kSecurity+9].entry.default_for_object=false;
    if(scenario==41)bad.items[Package::kSecurity+4].definition.security_epoch=2;
    if(scenario==42)bad.items[Package::kSecurity+4].definition.visibility=c::CatalogVisibilityClass::public_metadata;
    const auto refused=db::AssembleNativeInitialPolicySecurityRecords(changed);
    if(refused.ok()||refused.records)std::cerr<<"service refusal scenario "<<scenario<<" unexpectedly admitted\n";
    Check(!refused.ok()&&!refused.records,"bad service identity, selection, ownership, alias, credentials or limits refuses the whole cohort");
  }
  // Exactly the complete service cohort, agent records included (2026-11-03).
  request.policy.limits={3*static_cast<unsigned>(package.items.size()),made.records->encoded_bytes};
  Check(db::AssembleNativeInitialPolicySecurityRecords(request).ok(),"exact service cohort record/byte admission");
  for(long budget:{0,1,5,31,97,257,1024}){allocation_failed=false;allocation_budget=budget;
    const auto refused=db::AssembleNativeInitialPolicySecurityRecords(request);allocation_budget=-1;
    Check(allocation_failed&&!refused.ok()&&!refused.records,"service construction allocation failure cannot publish a partial identity/grant/binding cohort");}
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
    if(scenario==14){bad.items[28].vector.object_kind=bad.items[28].entry.object_kind=c::CatalogRecordKind::policy;}
    if(scenario==15)bad.items[28].definition.record.header.object_uuid.value=SourceId(0x999);
    if(scenario==16)bad.items[28].definition.security_policy_uuid={};
    if(scenario==17)bad.items[28].definition.record.header.row_uuid.value=bad.items[29].definition.record.header.row_uuid.value;
    if(scenario==18)changed.policy.definitions=changed.policy.definitions.first(Package::kSecurity-1);
    if(scenario==19)bad.inputs[28]=bad.inputs[29];
    if(scenario==20)changed.policy.limits.maximum_records=3*Package::kSecurity-1;
    if(scenario==21)changed.policy.limits.maximum_encoded_bytes=made.records->encoded_bytes-1;
    if(scenario==22)bad.items[28].entries[0].entry=nullptr;
    if(scenario==23)bad.memberships[0].member_uuid=bad.memberships[0].parent_uuid;
    const auto refused=db::AssembleNativeInitialPolicySecurityRecords(changed);
    Check(!refused.ok()&&!refused.records,"invalid/omitted/repeated source relationship, identity, common/name and bounds refuse atomically");
    if(scenario==2||scenario==7||scenario==8||scenario==9)
      Check(refused.error==db::NativeInitialPolicyError::duplicate_identity,"membership collision has exact duplicate classification");
  }
  request.policy.limits={3*Package::kSecurity,made.records->encoded_bytes};
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
    if(scenario==7)bad.items[0].definition.object_subtype=scratchbird::core::catalog::CatalogObjectSubtype::schema_application;
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
    if(scenario==22)changed.database_default_language=SourceId(0x998);  // an engine UUID that is no language constant
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
  // 3b: the manifest's text id and version are not record data any more (the id
  // is the pack's reference name in the names table; the version is not carried).
  // A pack differing only in those labels writes a record byte-identical to the
  // default pack's, with the exact binary pack identity.
  Package plain(security);const auto plain_request=plain.Request();
  const auto baseline=security?db::AssembleNativeInitialPolicySecurityRecords({plain_request,plain.memberships}):db::AssembleNativeInitialPolicyRecords(plain_request);
  Check(pack.ok()&&pack.record->policy_pack_uuid==SourceId(1)&&baseline.ok()&&
      made.records->definitions[0].metadata.record.payload==baseline.records->definitions[0].metadata.record.payload,
      "3b: manifest labels are not record data; exact binary pack identity is retained");
  Check(std::filesystem::remove_all(root)>0,"remove exact owned mutable source after successful assembly");
  Verify(*made.records,security);
}
void ServiceSourceMutations() {
  for(unsigned scenario=0;scenario<6;++scenario){
    scratchbird::tests::OwnedTempDirectory artifacts;const auto root=artifacts.path()/"selected-service-pack";
    std::filesystem::copy(SB_DEFAULT_POLICY_PACK_ROOT,root,std::filesystem::copy_options::recursive);
    scratchbird::tests::RewriteAndResealPolicyPack(root,[&](const auto& path,auto& text,const auto& replace){
      if(scenario==0){
        replace(text,"018f7a10-1280-7000-8000-000000000010","018f7a10-1280-7000-8000-000000000050");
        replace(text,"018f7a10-1280-7000-8000-000000000402","018f7a10-1280-7000-8000-000000000450");
      }
      if(path=="policies/security_providers.json"){
        if(scenario==1)replace(text,"\"enabled_by_default\": true","\"enabled_by_default\": false");
        if(scenario==2)replace(text,"\"external_provider\": false","\"external_provider\": true");
        if(scenario==3)replace(text,"\"authority\": \"durable_catalog_row\"","\"authority\": \"unsupported_by_default\"");
        if(scenario==4)replace(text,"local_password_verifier_required","not_a_verifier_policy");
      }
      if(scenario==5&&path=="policies/policy_profiles.json")replace(text,"deny_by_default","allow_by_default");
    });
    Package package(true,true,true);auto request=package.ServiceRequest();const auto source=root.string();request.policy.policy_seed_pack_root=source;
    if(scenario==0){
      request.local_service->provider_uuid=SourceId(0x50);request.local_service->policy_uuid=SourceId(0x450);
      for(const auto n:{1u,15u}){
        const auto id=SourceId(n==1?0x50:0x450);auto& x=package.items[n];
        x.definition.record.header.object_uuid.value=id;x.vector_metadata.record.header.parent_uuid.value=id;
        x.vector.object_uuid.value=id;x.entry.object_uuid.value=id;
      }
      auto& binding=package.items[Package::kSecurity+4];
      binding.definition.security_policy_uuid.value=binding.vector_metadata.security_policy_uuid.value=
          binding.vector.security_policy_uuid.value=binding.entry_metadata.security_policy_uuid.value=binding.entry.security_policy_uuid.value=SourceId(0x450);
    }
    const auto made=db::AssembleNativeInitialPolicySecurityRecords(request);
    if(scenario==0){
      Check(made.ok(),"valid alternate source provider/policy identities, not hardcoded defaults");
      Check(std::filesystem::remove_all(root)>0,"release exact private service source before reading owning result");
      unsigned providers=0,policies=0,bindings=0;
      for(const auto& record:made.records->definitions){
        const auto& meta=record.metadata;
        if(meta.record.header.object_uuid.value==SourceId(0x50)){
          const auto value=c::DecodeCatalogBootstrapSecurityProvider(meta.record.payload);
          Check(value.ok()&&value.record->provider_uuid==SourceId(0x50)&&value.record->family==c::BootstrapProviderFamily::local_password,
              "actual alternate provider definition survives source removal");++providers;
        }
        if(meta.record.header.object_uuid.value==SourceId(0x450)){
          const auto value=c::DecodeCatalogBootstrapPolicyProfile(meta.record.payload);
          Check(value.ok()&&value.record->profile_uuid==SourceId(0x450)&&value.record->area==c::BootstrapPolicyArea::default_security_posture&&value.record->mode==c::BootstrapPolicyMode::deny_by_default,/*3b*/
              "actual alternate posture source survives source removal");++policies;
        }
        if(meta.record.header.object_uuid.value==f::Id(15001)){
          const auto value=c::DecodeCatalogRuntimeAuthorityBinding(meta.record.payload);
          Check(value.ok()&&c::CatalogRuntimeAuthorityBindingMatchesMetadata(meta)&&value.record->provider_uuid==SourceId(0x50)&&
              value.record->policy_uuid==SourceId(0x450),"runtime selection binds actual alternate source, not a default substitution");++bindings;
        }
      }
      Check(providers==1&&policies==1&&bindings==1,"all alternate selected definitions exist exactly once");
    }else Check(!made.ok()&&!made.records,"correctly resealed unsafe provider/posture refuses before any service output");
  }
}
void SecuritySourceMutations() {
  for(unsigned scenario=0;scenario<4;++scenario){
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
      }else if(scenario==1&&path=="policies/groups.json"){
        // Duplicate the first complete source relationship with a valid new
        // content hash: this must reach semantic admission, not hash refusal.
        const auto list=text.find("\"memberships\"");const auto first=text.find('{',list);const auto last=text.find('}',first);
        Check(list!=text.npos&&first!=text.npos&&last!=text.npos,"actual source membership to duplicate");
        const auto entry=text.substr(first,last-first+1);text.insert(last+1,","+entry);
      }
      if(scenario==2&&path=="policies/roles.json")replace(text,"ROLE_APP_RUNTIME","ROLE_SYSARCH");
      if(scenario==3)replace(text,"018f7a10-1280-7000-8000-000000000100","018f7a10-1280-7000-8000-000000000105");
    });
    Package package(true,scenario>=2);auto request=scenario>=2?package.BootstrapRequest():package.SecurityRequest();const auto source=root.string();request.policy.policy_seed_pack_root=source;
    const auto made=db::AssembleNativeInitialPolicySecurityRecords(request);
    if(scenario==0){
      Check(made.ok(),"actual changed source values, no hardcoded default fallback");
      unsigned changed=0,targets=0;
      for(const auto& record:made.records->definitions){
        if(!c::IsCatalogSecurityRecordKind(record.metadata.record.header.kind))continue;
        // 3a-2: typed records. Role and group descriptions are not record
        // fields (dropped, owner Q65; the comments table is step 4), so of the
        // three changed source values only the grant's right is observable;
        // the role and group are checked present and typed.
        const auto decoded=c::DecodeNativeSecurityRecord(record.metadata.record.header.kind,record.metadata.record.payload);
        Check(decoded.ok(),"custom security payload native decode");
        const auto id=record.metadata.record.header.object_uuid.value;
        if(id==SourceId(0x100)){Check(decoded.record->role()!=nullptr,"actual selected role present");++changed;}
        if(id==SourceId(0x200)){Check(decoded.record->group()!=nullptr,"actual selected group present");++changed;}
        if(id==SourceId(0x300)){Check(decoded.record->grant()&&decoded.record->grant()->right==c::CatalogSecurityRight::AUDIT_READ,"actual selected grant value");++changed;}
        if(const auto* g=decoded.record->grant();g&&g->grant_class==c::CatalogSecurityGrantClass::privilege){
          Check(g->target_uuid==SourceId(0x106)&&g->effect==c::CatalogSecurityEffect::deny,
              "actual nonempty target and explicit deny retained, not text/unscoped/default-allow fallback");++targets;
        }
      }
      Check(changed==3&&targets==11+std::size(kDefaultPackAgentGrants),"every changed value and actual targeted grant retained");  // 11 standard grants
    }else if(scenario==1){
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
    }else{
      Check(!made.ok()&&!made.records&&made.error==db::NativeInitialPolicyError::source_invalid&&made.source_diagnostic&&
          made.source_diagnostic->diagnostic_code=="SB-DB-BOOTSTRAP-SECURITY-SYSARCH-DUPLICATE",
          "validly resealed imported role cannot replace reserved bootstrap identity or classification");
    }
  }
}
auto NameBytes(const c::CatalogNamePayload& name) {
  return std::visit([](const auto& value){
    if constexpr(std::is_same_v<std::decay_t<decltype(value)>,c::CatalogNameVector>)
      return c::EncodeCatalogNameVector(value);
    else return c::EncodeCatalogNameEntry(value);
  },name);
}
void PlacementLogical() {
  using E=db::NativeInitialPolicyCatalogError;
  for(unsigned cohort=0;cohort<4;++cohort){
    const bool security=cohort>=1,bootstrap=cohort>=2,service=cohort>=3;
    Package package(security,bootstrap,service);
    const auto request=service?package.ServiceRequest():bootstrap?package.BootstrapRequest():package.SecurityRequest();
    const auto assemble=[&]{return security?db::AssembleNativeInitialPolicySecurityCatalogs(request):
        db::AssembleNativeInitialPolicyCatalogs(package.Request());};
    const auto made=assemble();Check(made.ok()&&made.source_error==db::NativeInitialPolicyError::none,
        "production initial policy placement consumes the complete actual admitted source");
    const auto& catalogs=made.plan->catalogs;
    // With a service: plus L5's SYSARCH membership and every agent type's
    // principal, grants and membership, all security records (2026-11-03).
    const unsigned agent_records=service?static_cast<unsigned>(package.items.size())-(Package::kSecurity+10):0;
    const unsigned definitions=service?Package::kSecurity+10+agent_records:bootstrap?Package::kSecurity+3:security?Package::kSecurity:28;
    const unsigned security_count=security?(Package::kSecurity-28)+(bootstrap?3:0)+(service?6:0)+agent_records:0;
    Check(catalogs[0].empty()&&catalogs[2].empty()&&catalogs[5].empty()&&
        catalogs[1].size()==2*definitions&&catalogs[3].size()==security_count&&catalogs[4].size()==28+(service?1:0),
        "exact names/security/configuration roles, no default object-head placement");
    c::CatalogNamedCreationSet joined;joined.names=catalogs[1];joined.encoded_bytes=made.plan->encoded_bytes;
    for(unsigned slot:{3u,4u})for(const auto& row:catalogs[slot]){
      Check(!row.name&&((slot==3)==c::IsCatalogSecurityRecordKind(row.metadata.record.header.kind)),
          "security and configuration family classification is not supplied placement authority");
      joined.definitions.push_back(row);
    }
    Verify(joined,security,bootstrap,service,&package.agents);
    for(const auto& row:catalogs[1])Check(row.name&&row.metadata.record.header.kind==c::CatalogRecordKind::localized_name,
        "all supplied localized names and vectors reside only in name registry");
    package.items[0].entry.display_name="caller data changed after production placement";
    Check(std::get<c::CatalogNameEntry>(*catalogs[1][1].name).display_name=="supplied_alias_0",
        "production placement owns localized data after caller mutation");
    package.items[0].entry.display_name="supplied_alias_0";
    for(unsigned scenario=0;scenario<8;++scenario){
      Package bad(security,bootstrap,service);
      auto changed=service?bad.ServiceRequest():bootstrap?bad.BootstrapRequest():bad.SecurityRequest();
      if(scenario==0)changed.policy.database_uuid={};
      if(scenario==1)changed.policy.policy_seed_pack_root="/not-an-existing-scratchbird-policy-pack";
      if(scenario==2)changed.policy.limits.maximum_records=3*definitions-1;
      if(scenario==3)changed.policy.limits.maximum_encoded_bytes=made.plan->encoded_bytes-1;
      if(scenario==4)bad.items[0].definition.object_subtype=static_cast<scratchbird::core::catalog::CatalogObjectSubtype>(0xfffe);  // an unlisted subtype
      if(scenario==5)bad.items[0].entry.default_for_object=false;
      if(scenario==6)bad.items[0].definition.record.header.row_uuid=bad.items[1].definition.record.header.row_uuid;
      if(scenario==7)changed.policy.definitions=changed.policy.definitions.first(definitions-1);
      const auto direct=security?db::AssembleNativeInitialPolicySecurityRecords(changed):db::AssembleNativeInitialPolicyRecords(changed.policy);
      const auto refused=security?db::AssembleNativeInitialPolicySecurityCatalogs(changed):db::AssembleNativeInitialPolicyCatalogs(changed.policy);
      Check(!direct.ok()&&!refused.ok()&&!refused.plan&&refused.error==E::source_failure&&
          refused.source_error==direct.error&&refused.name_error==direct.name_error&&
          refused.source_diagnostic.has_value()==direct.source_diagnostic.has_value(),
          "placement cannot bypass source coverage, canonical family/name, identity or exact limits and preserves typed refusal");
      if(direct.source_diagnostic)Check(refused.source_diagnostic->diagnostic_code==direct.source_diagnostic->diagnostic_code&&
          refused.source_diagnostic->message_key==direct.source_diagnostic->message_key,
          "owning source diagnostic survives failed placement without replacement or partial catalogs");
    }
    // Warm the same real source, then distinguish source assembly allocations
    // from the one/two actual destination reserves. No mutable prebuilt package
    // is injected to bypass the public complete-source boundary.
    allocation_budget=std::numeric_limits<long>::max();
    const auto measured=security?db::AssembleNativeInitialPolicySecurityRecords(request):db::AssembleNativeInitialPolicyRecords(package.Request());
    const auto source_allocations=std::numeric_limits<long>::max()-allocation_budget;allocation_budget=-1;
    Check(measured.ok()&&source_allocations>0,"measure actual complete-source allocation positions before placement");
    const long destination_reserves=security?2:1;
    for(const long budget:{0L,1L,5L,31L,97L,257L,source_allocations-1,source_allocations,source_allocations+destination_reserves-1}){
      allocation_failed=false;allocation_budget=budget;const auto refused=assemble();allocation_budget=-1;
      Check(allocation_failed&&!refused.ok()&&!refused.plan,
          "source or destination allocation fault never exposes moved/partial native role cohorts");
      if(budget>=source_allocations)Check(refused.source_error==db::NativeInitialPolicyError::none&&refused.error==E::resource_exhausted,
          "failure during actual placement reserves preserves successful source classification and fails atomically");
    }
    allocation_budget=source_allocations+destination_reserves;const auto exact=assemble();allocation_budget=-1;
    Check(exact.ok()&&exact.plan->encoded_bytes==made.plan->encoded_bytes,
        "exact measured source plus destination-reserve allocation bound succeeds without payload recopy");
  }
}
void Physical(bool security=false,bool bootstrap=false,bool service=false) {
  scratchbird::tests::OwnedTempDirectory artifacts;
  Package package(security,bootstrap,service);const auto made=security?db::AssembleNativeInitialPolicySecurityCatalogs(service?package.ServiceRequest():bootstrap?package.BootstrapRequest():package.SecurityRequest()):db::AssembleNativeInitialPolicyCatalogs(package.Request());Check(made.ok(),"actual production source and role placement before physical construction");
  for(const auto& profile:d::kCanonicalFilespacePageProfiles) {
    const auto& seed=made.plan->catalogs;
    db::NativeCreationCatalogIndexes indexes;
    for(unsigned role:{1u,3u,4u}) {
      if(seed[role].empty())continue;
      scratchbird::storage::page::NativeBtreeDependencies deps;deps.index_uuid=f::Id(8010+role);
      deps.descriptor_generation=deps.storage_generation=1;deps.key_profile_uuid=f::Id(8020+role);deps.visibility_profile_uuid=f::Id(8030+role);
      deps.dependency_map_uuid=f::Id(8040+role);deps.dependency_map_sha256.fill(1+role);indexes[role]=db::NativeCreationCatalogIndexBinding{f::Id(8000+role),deps};
    }
    db::NativeFilespaceInitializationRequest request;
    request.bootstrap={f::Id(1),f::Id(2),profile.uuid,d::kNativeBootstrapIntegrityProfile,{},profile.page_size_bytes,1,0,1,7};
    request.operation_uuid=f::Id(3);request.writer_uuid=f::Id(4);request.policy_snapshot_uuid=f::Id(10);
    request.creator.transaction_uuid=tx;request.creator.local_id=mga::MakeLocalTransactionId(1);request.creator.scope=mga::TransactionScope::local_node;
    // 256 pages and a 512-page image budget until 2026-11-03; the service
    // cohort with every agent type's principal needs more at 8 KiB pages.
    request.total_pages=1024;request.creation_utc_millis=1789357072000ULL;
    u::StandaloneUuidV7Issuer issuer({f::Id(1),f::Id(10)},{{},0,1000});const auto path=(artifacts.path()/std::to_string(profile.page_size_bytes)).string();d::FileDevice device;
    Check(device.Open(path,d::FileOpenMode::create_new).ok(),"actual private native construction file");const auto budget=p::u64{db::NativeHostedSourceBackingBytes(profile.page_size_bytes)};
    const auto created=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(device,request,seed,indexes,budget,issuer);
    Check(created.ok(),"actual imported native policy/name cohort persists through indexed construction");
    Check(device.Close().ok()&&device.Open(path,d::FileOpenMode::open_existing).ok(),"independent native policy reopen");
    const std::vector<d::NativeFilespaceDevice> devices{{f::Id(2),profile.uuid,&device}};
    std::vector<c::CatalogTypedRecord> security_records;
    std::size_t definition_count=0,name_count=0;
    for(unsigned role=0;role<seed.size();++role){
      const auto rows=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(f::Id(1),devices,db::NativeCatalogSource::PageZero(created.receipt->filespace_uuid),2,role+1,
          {created.receipt->relation_uuids[role],created.receipt->index_dependencies[role]},budget);
      Check(rows.ok()&&rows.rows.size()==seed[role].size(),"every native relation preserves its exact production-placed cohort after independent reopen");
      for(const auto& row:rows.rows){
        const auto original=std::find_if(seed[role].begin(),seed[role].end(),[&](const auto& r){return r.metadata.record.header.object_uuid.value==row.metadata.record.header.object_uuid.value;});
        Check(original!=seed[role].end()&&!row.provisional,"exact raw identity remains committed under its actual native role");
        auto expected=original->metadata;
        if(role==1){
          ++name_count;Check(original->name&&row.name_payload,"actual name envelope survives native reopen");
          const auto before=NameBytes(*original->name),after=NameBytes(*row.name_payload);
          Check(before.ok()&&after.ok()&&before.bytes==after.bytes,"all localized vector/entry values survive exact native persistence without identity replacement");
          expected.record.payload=row.metadata.record.payload;
        }else{
          ++definition_count;Check(row.metadata.record.payload==original->metadata.record.payload,
              "exact native imported definition bytes survive in the correct security/configuration relation");
          if(role==3)security_records.push_back(row.metadata.record);
        }
        const auto before=c::EncodeCatalogMetadataVersion(expected),after=c::EncodeCatalogMetadataVersion(row.metadata);
        Check(before.ok()&&after.ok()&&before.bytes==after.bytes,"complete owner/origin/name/governance/common metadata survives native reopen unchanged");
      }
    }
    Check(definition_count==package.items.size()&&name_count==2*package.items.size(),
        "all original definitions and every supplied name are retained exactly once across declared roles");
    if(bootstrap){
      // 3a-2: typed records (the legacy validator reads only the legacy bag).
      std::optional<c::NativeSecurityPrincipal> principal;std::optional<c::NativeSecurityGrant> membership;
      for(const auto& record:security_records){
        const auto typed=c::DecodeNativeSecurityRecord(record.header.kind,record.payload);
        Check(typed.ok(),"reopened native security record is typed");
        if(record.header.object_uuid.value==f::Id(14000))principal=*typed.record->principal();
        if(record.header.object_uuid.value==f::Id(14001))membership=*typed.record->grant();
      }
      Check(principal&&principal->bootstrap_principal&&principal->credential&&BootstrapCredential(fingerprint).Matches(*principal->credential)&&
          membership&&membership->member_uuid==f::Id(14000)&&membership->parent_uuid==SourceId(0x105),
          "actual reopened native records preserve bootstrap semantics without claiming authentication or activation");
    }
    const auto zero=d::ReadFilespacePageZeroFromOpenDevice(device);Check(zero.ok()&&zero.record->bootstrap.lifecycle_state==1,"construction does not falsely activate node or authorize service: page zero stays as the create wrote it, an online filespace (owner 2026-11-07); serving is the server's act at open");
    Check(device.Close().ok(),"actual native policy construction device released");
  }
}
}
int main()try{Logical();SecurityLogical();BootstrapLogical();ServiceLogical();PlacementLogical();CustomSource();CustomSource(true);SecuritySourceMutations();ServiceSourceMutations();Physical();Physical(true);Physical(true,true);Physical(true,true,true);std::cout<<"PASS native policy creation "<<checks<<" checks\n";return 0;}
catch(const std::exception& e){allocation_budget=-1;std::cerr<<e.what()<<'\n';return 1;}
