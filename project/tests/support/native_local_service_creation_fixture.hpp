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
// Complete production-assembled 64-definition/128-name initial enrollment.
// These inputs grant no provider activation, runtime session or node readiness.
// All borrowed request views require this immovable owner and verifier backing.
struct NativeLocalServiceCreationItem {
  c::CatalogMetadataVersion definition,vector_metadata,entry_metadata;
  c::CatalogNameVector vector;c::CatalogNameEntry entry;
  std::array<c::CatalogCreationNameEntryInput,1> entries;
};
class NativeLocalServiceCreationFixture {
 public:
  std::vector<NativeLocalServiceCreationItem> items;std::vector<c::CatalogNamedCreationInput> inputs;
  std::array<db::NativeInitialMembershipIdentity,5> memberships;
  NativeLocalServiceCreationFixture(p::Uuid database,p::TypedUuid construction,std::string_view verifier)
      :items(64),database_(database),construction_(construction),verifier_(verifier) {
    for(unsigned i=0;i<memberships.size();++i)memberships[i]={f::Id(13000+i),SourceId(0x201+i),SourceId(0x100+i)};
    for(unsigned n=0;n<items.size();++n) {
      auto& x=items[n];const auto object=n==0?SourceId(1):n<=12?SourceId(0x10+n-1):n<=26?SourceId(0x400+n-13):n==27?f::Id(12000):
          n<=32?SourceId(0x100+n-28):n<=38?SourceId(0x200+n-33):n<=43?f::Id(13000+n-39):n<=53?SourceId(0x300+n-44):
          n==54?SourceId(0x105):n<=56?f::Id(14000+n-55):n<=58?f::Id(15000+n-57):f::Id(15100+n-59);
      const auto subtype=n==0?"bootstrap_policy_pack":n<=12?"bootstrap_security_provider":n<=26?"bootstrap_policy_profile":n==27?"memory_configuration":
          n<=32?"role_account":n<=38?"group_account":n<=53?"grant_record":n==54?"role_account":n==55||n==57?"user_account":n==58?"agent_runtime_authority":"grant_record";
      auto& meta=x.definition;meta=f::Metadata(0,construction_,f::Id(60),{});
      meta.owner_uuid=Typed(p::UuidKind::principal,14000);
      meta.record.header.object_uuid={p::UuidKind::object,object};meta.record.header.row_uuid=Typed(p::UuidKind::row,200000+3*n);
      meta.object_subtype=subtype;if(n==27)meta.record.header.kind=c::CatalogRecordKind::config_profile;
      if(n>=28)meta.record.header.kind=n<=32||n==54?c::CatalogRecordKind::role_account:n<=38?c::CatalogRecordKind::group_account:n==55||n==57?c::CatalogRecordKind::user_account:n==58?c::CatalogRecordKind::config_profile:c::CatalogRecordKind::grant_record;
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
    Rebind();
  }
  void Rebind(){inputs.clear();for(auto& x:items){x.entries[0]={&x.entry_metadata,&x.entry};inputs.push_back({&x.definition,&x.vector_metadata,&x.vector,x.entries});}}
  NativeLocalServiceCreationFixture(const NativeLocalServiceCreationFixture&)=delete;
  NativeLocalServiceCreationFixture& operator=(const NativeLocalServiceCreationFixture&)=delete;
  auto Request(std::string_view source)const{return db::NativeInitialPolicyRequest{source,database_,construction_,f::Id(12000),"en-US",inputs,{1000,4*1024*1024}};}
  auto BootstrapRequest(std::string_view source)const{return db::NativeInitialPolicySecurityRequest{Request(source),memberships,
      db::NativeInitialBootstrapIdentity{f::Id(14000),f::Id(14001),"BootstrapAdmin",verifier_}};}
  auto ServiceRequest(std::string_view source)const{auto request=BootstrapRequest(source);request.local_service=db::NativeInitialLocalServiceIdentity{
      f::Id(15000),f::Id(15001),f::Id(15002),SourceId(0x10),SourceId(0x402),
      {f::Id(15100),f::Id(15101),f::Id(15102),f::Id(15103),f::Id(15104)},"NodeService",verifier_};return request;}
 private:
  p::Uuid database_;
  p::TypedUuid construction_;
  std::string_view verifier_;
};
} // namespace scratchbird::tests::native_local_service
