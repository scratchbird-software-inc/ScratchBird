// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../sbsql_sblr_alignment/bootstrap_policy_fixture.hpp"
#include "native_initial_policy_catalogs.hpp"
#include "uuid.hpp"
#include <stdexcept>

namespace scratchbird::tests::native_local_service {
namespace c=core::catalog;namespace p=core::platform;
namespace db=storage::database;namespace f=bootstrap_policy;namespace u=core::uuid;
inline p::TypedUuid Typed(p::UuidKind kind,unsigned n){return {kind,f::Id(n)};}
inline p::Uuid SourceId(unsigned suffix) {
  auto id=u::ParseTypedUuid(p::UuidKind::object,"018f7a10-1280-7000-8000-000000000001");
  if(!id.ok())throw std::runtime_error("invalid native service fixture source identity");
  id.value.value.bytes[14]=suffix>>8;id.value.value.bytes[15]=suffix;return id.value.value;
}
// Complete production-assembled initial enrollment: one definition and two
// name rows per object of the default pack, plus bootstrap and service.
// These inputs grant no provider activation, runtime session or node readiness.
// All borrowed request views require this immovable owner and verifier backing.
struct NativeLocalServiceCreationItem {
  c::CatalogMetadataVersion definition,vector_metadata,entry_metadata;
  c::CatalogNameVector vector;c::CatalogNameEntry entry;
  std::array<c::CatalogCreationNameEntryInput,1> entries;
};
class NativeLocalServiceCreationFixture {
 public:
  // Default pack: 5 standard + 44 agent task-group roles; 11 standard + 199
  // agent role grants (appendix-agent-security-roles.md, 2026-10-29; 2026-10-30
  // adds CONNECT to PUBLIC and OBS_AGENT_CONTROL_INTERNAL to transaction_pressure).
  static constexpr unsigned kRoles=49,kGrants=210;
  static constexpr unsigned kDefinitions=28+kRoles+6+5+kGrants+10;
  // SB_NATIVE_CREATE_AGENT_PRINCIPALS: L5's SYSARCH membership and, per pack
  // agent type, a principal, its grants and its task-group membership, issued
  // by the production helper from this fixed range and appended after the
  // definitions above (the assembler matches definitions by object identity).
  static constexpr unsigned kL5Membership=15003,kAgentIdentityBase=70000;
  std::vector<NativeLocalServiceCreationItem> items;std::vector<c::CatalogNamedCreationInput> inputs;
  std::array<db::NativeInitialMembershipIdentity,5> memberships;
  std::vector<db::NativeInitialAgentIdentity> agents;
  NativeLocalServiceCreationFixture(p::Uuid database,p::TypedUuid construction,std::string_view verifier,
                                    std::string_view pack_root=SB_DEFAULT_POLICY_PACK_ROOT)
      :items(kDefinitions),database_(database),construction_(construction),verifier_(verifier) {
    for(unsigned i=0;i<memberships.size();++i)memberships[i]={f::Id(13000+i),SourceId(0x201+i),SourceId(0x100+i)};
    unsigned issued=0;
    auto agent_ids=db::IssueNativeInitialAgentIdentities(pack_root,[&]()->std::optional<p::Uuid>{return f::Id(kAgentIdentityBase+issued++);});
    if(!agent_ids.ok())throw std::runtime_error("native service fixture agent identities");
    agents=std::move(agent_ids.agents);
    for(unsigned n=0;n<kDefinitions;++n) {
      // Pack order: 5 standard roles (0x100..) then the agent task-group roles
      // (0x600..); 11 standard grants (0x300..0x30a; the 11th, 2026-10-30,
      // grants CONNECT to the PUBLIC group) then the agent role grants
      // (0x1000..). The tail is sysarch, bootstrap principal and membership,
      // service principal, runtime authority binding and five service grants.
      const unsigned roles_end=28+kRoles,groups_end=roles_end+6,members_end=groups_end+5,grants_end=members_end+kGrants;
      const unsigned t=n-grants_end;  // tail position, valid once n>=grants_end
      const auto object=n==0?SourceId(1):n<=12?SourceId(0x10+n-1):n<=26?SourceId(0x400+n-13):n==27?f::Id(12000):
          n<roles_end?(n-28<5?SourceId(0x100+n-28):SourceId(0x600+n-28-5)):
          n<groups_end?SourceId(0x200+n-roles_end):n<members_end?f::Id(13000+n-groups_end):
          n<grants_end?(n-members_end<11?SourceId(0x300+n-members_end):SourceId(0x1000+n-members_end-11)):
          t==0?SourceId(0x105):t<=2?f::Id(14000+t-1):t<=4?f::Id(15000+t-3):f::Id(15100+t-5);
      const bool role=n>=28&&(n<roles_end||t==0);
      const auto subtype=n==0?"bootstrap_policy_pack":n<=12?"bootstrap_security_provider":n<=26?"bootstrap_policy_profile":n==27?"memory_configuration":
          role?"role_account":n<groups_end?"group_account":n<grants_end?"grant_record":t==1||t==3?"user_account":t==4?"agent_runtime_authority":"grant_record";
      std::optional<c::CatalogRecordKind> kind;
      if(n==27)kind=c::CatalogRecordKind::config_profile;
      if(n>=28)kind=role?c::CatalogRecordKind::role_account:n<groups_end?c::CatalogRecordKind::group_account:n>=grants_end&&(t==1||t==3)?c::CatalogRecordKind::user_account:n>=grants_end&&t==4?c::CatalogRecordKind::config_profile:c::CatalogRecordKind::grant_record;
      Fill(n,object,kind,subtype);
    }
    unsigned n=kDefinitions;
    Append(n,f::Id(kL5Membership),c::CatalogRecordKind::grant_record);
    for(const auto& agent:agents) {
      Append(n,agent.principal_uuid,c::CatalogRecordKind::user_account);
      for(const auto& grant:agent.grant_uuids)Append(n,grant,c::CatalogRecordKind::grant_record);
      Append(n,agent.membership_uuid,c::CatalogRecordKind::grant_record);
    }
    Rebind();
  }
  // One definition with its name vector and default name entry. The row, name
  // and alias identities follow the item position, as they always have.
  void Fill(unsigned n,const p::Uuid& object,std::optional<c::CatalogRecordKind> kind,std::string_view subtype) {
      auto& x=items[n];
      auto& meta=x.definition;meta=f::Metadata(0,construction_,f::Id(60),{});
      meta.owner_uuid=Typed(p::UuidKind::principal,14000);
      meta.record.header.object_uuid={p::UuidKind::object,object};meta.record.header.row_uuid=Typed(p::UuidKind::row,200000+3*n);
      meta.object_subtype=std::string(subtype);if(kind)meta.record.header.kind=*kind;
      meta.security_policy_uuid={p::UuidKind::object,SourceId(0x402)};
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
      e.created_transaction_uuid=construction_;e.security_policy_uuid=meta.security_policy_uuid;
      e.resource_epoch=e.name_resolution_epoch=1;e.default_for_language=e.default_for_object=true;e.lifecycle_state=c::CatalogNameLifecycle::active;
  }
  void Append(unsigned& n,const p::Uuid& object,c::CatalogRecordKind kind) {
    items.emplace_back();Fill(n++,object,kind,c::CatalogRecordKindName(kind));
  }
  void Rebind(){inputs.clear();for(auto& x:items){x.entries[0]={&x.entry_metadata,&x.entry};inputs.push_back({&x.definition,&x.vector_metadata,&x.vector,x.entries});}}
  NativeLocalServiceCreationFixture(const NativeLocalServiceCreationFixture&)=delete;
  NativeLocalServiceCreationFixture& operator=(const NativeLocalServiceCreationFixture&)=delete;
  auto Request(std::string_view source)const{return db::NativeInitialPolicyRequest{source,database_,construction_,f::Id(12000),"en-US",inputs,{3000,16*1024*1024}};}
  auto BootstrapRequest(std::string_view source)const{return db::NativeInitialPolicySecurityRequest{Request(source),memberships,
      db::NativeInitialBootstrapIdentity{f::Id(14000),f::Id(14001),"BootstrapAdmin",verifier_}};}
  auto ServiceRequest(std::string_view source)const{auto request=BootstrapRequest(source);request.local_service=db::NativeInitialLocalServiceIdentity{
      f::Id(15000),f::Id(15001),f::Id(15002),SourceId(0x10),SourceId(0x402),
      {f::Id(15100),f::Id(15101),f::Id(15102),f::Id(15103),f::Id(15104)},"NodeService",verifier_,f::Id(kL5Membership)};
    request.agents=agents;return request;}
 private:
  p::Uuid database_;
  p::TypedUuid construction_;
  std::string_view verifier_;
};
} // namespace scratchbird::tests::native_local_service
