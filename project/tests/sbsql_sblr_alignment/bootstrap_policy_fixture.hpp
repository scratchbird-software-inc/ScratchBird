// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_bootstrap_policy.hpp"
#include <string>

namespace scratchbird::tests::bootstrap_policy {
namespace c=core::catalog;namespace p=core::platform;
inline p::Uuid Id(unsigned n){p::Uuid id;id.bytes[0]=1;id.bytes[6]=0x70;id.bytes[8]=0x80;
  id.bytes[12]=n>>24;id.bytes[13]=n>>16;id.bytes[14]=n>>8;id.bytes[15]=n;return id;}
inline void Put(std::string& s,std::size_t at,p::u64 value,unsigned n) {
  for(unsigned i=0;i<n;++i)s[at+i]=static_cast<char>(value>>(8*i));
}
inline std::string Number(p::u64 n){std::string out(8,0);Put(out,0,n,8);return out;}
inline std::string Bytes(const p::Uuid& id){return {reinterpret_cast<const char*>(id.bytes.data()),16};}
// Independent Core SBCV/TLV construction, no production schema/codec helpers.
struct Golden {
  std::string bytes=std::string(24,0);unsigned count=0;
  explicit Golden(unsigned schema){bytes.replace(0,4,"SBCV");Put(bytes,4,1,2);Put(bytes,6,24,2);
    Put(bytes,16,schema,4);Put(bytes,20,1,2);}
  void Field(unsigned tag,unsigned type,std::string_view value){const auto at=bytes.size();bytes.resize(at+8,0);
    Put(bytes,at,tag,2);Put(bytes,at+2,type,1);Put(bytes,at+4,value.size(),4);bytes.append(value);++count;}
  void Integer(unsigned tag,p::u64 n){Field(tag,1,Number(n));}
  void Boolean(unsigned tag,bool v){const char byte=v;Field(tag,2,{&byte,1});}
  void Identity(unsigned tag,const p::Uuid& id){Field(tag,5,Bytes(id));}
  std::string Finish(){Put(bytes,8,bytes.size(),4);Put(bytes,12,count,4);return bytes;}
};
inline std::string EncodeGolden(const c::CatalogBootstrapPolicyPack& r) {
  Golden g(65588);g.Identity(1,r.policy_pack_uuid);g.Integer(2,r.generation);g.Field(3,3,r.pack_id);g.Field(4,3,r.pack_version);
  g.Field(5,4,{reinterpret_cast<const char*>(r.content_sha256.data()),32});g.Field(6,3,r.declared_signature_status);
  g.Boolean(7,r.create_time_only);g.Boolean(8,r.post_create_filesystem_authority);g.Boolean(9,r.local_password_only);
  g.Identity(10,r.origin_transaction_uuid.value);g.Integer(11,r.origin_local_transaction_id);
  for(unsigned i=0;i<9;++i)g.Integer(12+i,r.declared_counts[i]);g.Integer(21,r.imported_schema_version);return g.Finish();
}
inline std::string EncodeGolden(const c::CatalogBootstrapSecurityProvider& r) {
  Golden g(65589);g.Identity(1,r.provider_uuid);g.Integer(2,r.generation);g.Identity(3,r.policy_pack_uuid);
  g.Integer(4,r.policy_pack_generation);g.Integer(5,static_cast<p::u64>(r.family));g.Boolean(6,r.enabled_by_default);
  g.Integer(7,static_cast<p::u64>(r.authority));g.Boolean(8,r.external_provider);
  if(r.verifier_policy)g.Integer(9,static_cast<p::u64>(*r.verifier_policy));
  g.Identity(10,r.origin_transaction_uuid.value);g.Integer(11,r.origin_local_transaction_id);return g.Finish();
}
inline std::string EncodeGolden(const c::CatalogBootstrapPolicyProfile& r) {
  Golden g(65590);g.Identity(1,r.profile_uuid);g.Integer(2,r.generation);g.Identity(3,r.policy_pack_uuid);
  g.Integer(4,r.policy_pack_generation);g.Field(5,3,r.area);g.Field(6,3,r.mode);
  if(r.resource_definition_uuid)g.Identity(7,*r.resource_definition_uuid);
  g.Identity(8,r.origin_transaction_uuid.value);g.Integer(9,r.origin_local_transaction_id);
  if(r.resource_definition_generation)g.Integer(10,*r.resource_definition_generation);return g.Finish();
}
struct Fixture {
  c::CatalogBootstrapPolicyPack pack;
  c::CatalogBootstrapSecurityProvider provider;
  c::CatalogBootstrapPolicyProfile profile;
  explicit Fixture(p::TypedUuid tx) {
    pack.policy_pack_uuid=Id(9000);pack.generation=1;pack.imported_schema_version=1;
    pack.pack_id="selected-pack";pack.pack_version="1.0.0";pack.declared_signature_status="signature-ready-unsigned";
    for(unsigned i=0;i<32;++i)pack.content_sha256[i]=i;
    pack.create_time_only=pack.local_password_only=true;pack.origin_transaction_uuid=tx;
    pack.origin_local_transaction_id=1;pack.declared_counts={12,1,11,6,3,3,24,14,14};
    provider.provider_uuid=Id(9001);provider.policy_pack_uuid=pack.policy_pack_uuid;
    provider.generation=provider.policy_pack_generation=1;
    provider.family=c::BootstrapProviderFamily::local_password;provider.enabled_by_default=true;
    provider.authority=c::BootstrapProviderAuthority::durable_catalog;
    provider.verifier_policy=c::BootstrapVerifierPolicy::local_password_verifier_required;
    provider.origin_transaction_uuid=tx;provider.origin_local_transaction_id=1;
    profile.profile_uuid=Id(9002);profile.policy_pack_uuid=pack.policy_pack_uuid;
    profile.generation=profile.policy_pack_generation=1;profile.area="security_provider_selection";
    profile.mode="local_password_only";profile.origin_transaction_uuid=tx;profile.origin_local_transaction_id=1;
  }
  std::array<std::string,3> Payloads() const{return {EncodeGolden(pack),EncodeGolden(provider),EncodeGolden(profile)};}
};
inline c::CatalogMetadataVersion Metadata(unsigned index,const p::TypedUuid& tx,const p::Uuid& schema,std::string payload) {
  c::CatalogMetadataVersion m;m.record.header.kind=c::CatalogRecordKind::policy;
  m.record.header.object_uuid={p::UuidKind::object,Id(9000+index)};
  m.record.header.row_uuid={p::UuidKind::row,Id(109000+index)};
  m.record.header.parent_uuid={p::UuidKind::object,schema};m.record.payload=std::move(payload);
  m.owner_uuid={p::UuidKind::principal,Id(50)};m.audit_uuid={p::UuidKind::object,Id(51)};
  m.security_policy_uuid={p::UuidKind::object,Id(52)};m.owning_schema_uuid={p::UuidKind::schema,schema};
  m.default_name_uuid={p::UuidKind::object,Id(9100+2*index)};m.name_vector_uuid={p::UuidKind::object,Id(9101+2*index)};
  m.creator_transaction_uuid=tx;m.creator_local_transaction_id=1;
  m.definition_version=m.catalog_generation=m.schema_epoch=m.security_epoch=m.dependency_generation=m.invalidation_generation=1;
  m.status=c::CatalogObjectStatus::active;m.lifecycle=c::CatalogObjectLifecycle::active;
  m.trace_search_key="BOOTSTRAP-POLICY-TEST";m.retention_class="catalog_history";
  constexpr const char* subtypes[]{"bootstrap_policy_pack","bootstrap_security_provider","bootstrap_policy_profile"};
  m.object_subtype=subtypes[index];return m;
}
} // namespace scratchbird::tests::bootstrap_policy
