// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_bootstrap_policy.hpp"
#include "uuid.hpp"
#include <algorithm>

namespace scratchbird::core::catalog {
namespace {
using E=CatalogValueError;using T=CatalogValueType;
constexpr CatalogValueFieldSchema pack_fields[]{
  {1,T::engine_identity,true,16,UuidKind::object},{2,T::unsigned_integer,true,8},
  {3,T::utf8_text,true,256},{4,T::utf8_text,true,256},{5,T::opaque_bytes,true,32},
  {6,T::utf8_text,true,256},{7,T::boolean,true,1},{8,T::boolean,true,1},{9,T::boolean,true,1},
  {10,T::engine_identity,true,16,UuidKind::transaction},{11,T::unsigned_integer,true,8},
  {12,T::unsigned_integer,true,8},{13,T::unsigned_integer,true,8},{14,T::unsigned_integer,true,8},
  {15,T::unsigned_integer,true,8},{16,T::unsigned_integer,true,8},{17,T::unsigned_integer,true,8},
  {18,T::unsigned_integer,true,8},{19,T::unsigned_integer,true,8},{20,T::unsigned_integer,true,8},
  {21,T::unsigned_integer,true,8}};
constexpr CatalogValueFieldSchema provider_fields[]{
  {1,T::engine_identity,true,16,UuidKind::object},{2,T::unsigned_integer,true,8},
  {3,T::engine_identity,true,16,UuidKind::object},{4,T::unsigned_integer,true,8},
  {5,T::unsigned_integer,true,8},{6,T::boolean,true,1},{7,T::unsigned_integer,true,8},
  {8,T::boolean,true,1},{9,T::unsigned_integer,false,8},
  {10,T::engine_identity,true,16,UuidKind::transaction},{11,T::unsigned_integer,true,8}};
constexpr CatalogValueFieldSchema profile_fields[]{
  {1,T::engine_identity,true,16,UuidKind::object},{2,T::unsigned_integer,true,8},
  {3,T::engine_identity,true,16,UuidKind::object},{4,T::unsigned_integer,true,8},
  {5,T::utf8_text,true,256},{6,T::utf8_text,true,256},
  {7,T::engine_identity,false,16,UuidKind::object},
  {8,T::engine_identity,true,16,UuidKind::transaction},{9,T::unsigned_integer,true,8}};
constexpr CatalogValueSchemaView pack_schema{65588,1,pack_fields};
constexpr CatalogValueSchemaView provider_schema{65589,1,provider_fields};
constexpr CatalogValueSchemaView profile_schema{65590,1,profile_fields};
bool Id(const Uuid& id){return uuid::IsEngineIdentityUuid(id);}
bool Typed(const TypedUuid& id,UuidKind kind){return id.kind==kind&&Id(id.value);}
bool Text(std::string_view v){return !v.empty()&&v.size()<=256&&v.find('\0')==v.npos;}
bool Origin(const TypedUuid& tx,u64 local){return Typed(tx,UuidKind::transaction)&&local;}
bool Valid(const CatalogBootstrapPolicyPack& r) {
  if(!Id(r.policy_pack_uuid)||!r.generation||!r.imported_schema_version||!Origin(r.origin_transaction_uuid,r.origin_local_transaction_id)||
      r.policy_pack_uuid==r.origin_transaction_uuid.value||!Text(r.pack_id)||!Text(r.pack_version)||
      !Text(r.declared_signature_status)||r.post_create_filesystem_authority)return false;
  const auto& c=r.declared_counts;
  return c[1]<=c[0]&&c[2]<=c[0]-c[1]&&
      (!r.local_password_only||(c[1]==1&&c[2]==c[0]-1));
}
bool Valid(const CatalogBootstrapSecurityProvider& r) {
  const auto f=static_cast<u64>(r.family),a=static_cast<u64>(r.authority);
  if(!Id(r.provider_uuid)||!Id(r.policy_pack_uuid)||r.provider_uuid==r.policy_pack_uuid||
      !r.generation||!r.policy_pack_generation||!Origin(r.origin_transaction_uuid,r.origin_local_transaction_id)||
      r.provider_uuid==r.origin_transaction_uuid.value||r.policy_pack_uuid==r.origin_transaction_uuid.value||
      f<1||f>12||a<1||a>2||
      (r.verifier_policy&&*r.verifier_policy!=BootstrapVerifierPolicy::local_password_verifier_required))return false;
  if(r.family==BootstrapProviderFamily::local_password)
    return !r.external_provider&&r.authority==BootstrapProviderAuthority::durable_catalog&&
        r.verifier_policy==BootstrapVerifierPolicy::local_password_verifier_required;
  return r.external_provider&&r.authority==BootstrapProviderAuthority::external_provider_admission&&
      !r.verifier_policy;
}
bool Valid(const CatalogBootstrapPolicyProfile& r) {
  return Id(r.profile_uuid)&&Id(r.policy_pack_uuid)&&r.profile_uuid!=r.policy_pack_uuid&&
      r.generation&&r.policy_pack_generation&&Origin(r.origin_transaction_uuid,r.origin_local_transaction_id)&&
      r.profile_uuid!=r.origin_transaction_uuid.value&&r.policy_pack_uuid!=r.origin_transaction_uuid.value&&
      Text(r.area)&&Text(r.mode)&&(!r.resource_definition_uuid||
        (Id(*r.resource_definition_uuid)&&*r.resource_definition_uuid!=r.profile_uuid&&
         *r.resource_definition_uuid!=r.policy_pack_uuid&&*r.resource_definition_uuid!=r.origin_transaction_uuid.value));
}
// Fixed field/scalar backing. Values are already canonical native bytes;
// shared SBCV validation rejects framing/value/size/overlap before output.
struct Fields {
  std::array<CatalogValueFieldView,21> fields{};
  std::array<std::array<byte,8>,21> scalars{};
  std::size_t count=0;
  void Id(u16 tag,const Uuid& id,UuidKind kind=UuidKind::object) {
    fields[count++]={tag,T::engine_identity,kind,id.bytes};
  }
  void Number(u16 tag,u64 value,T type=T::unsigned_integer) {
    auto& bytes=scalars[count];const auto size=type==T::boolean?1:8;
    for(int i=0;i<size;++i)bytes[i]=static_cast<byte>(value>>(8*i));
    fields[count++]={tag,type,UuidKind::unknown,std::span<const byte>(bytes).first(size)};
  }
  void Text(u16 tag,std::string_view value) {
    fields[count++]={tag,T::utf8_text,UuidKind::unknown,
        {reinterpret_cast<const byte*>(value.data()),value.size()}};
  }
  void Bytes(u16 tag,std::span<const byte> value){fields[count++]={tag,T::opaque_bytes,UuidKind::unknown,value};}
  CatalogValueEncodeIntoResult Encode(CatalogValueSchemaView schema,std::span<byte> backing) const {
    return EncodeCatalogValueBlockInto(schema,std::span<const CatalogValueFieldView>(fields).first(count),backing);
  }
};
std::string_view Text(const CatalogValueFieldView& f){return {reinterpret_cast<const char*>(f.bytes.data()),f.bytes.size()};}
u32 Schema(std::string_view bytes) {
  if(bytes.size()<kCatalogValueBlockHeaderBytes||bytes.substr(0,4)!="SBCV")return 0;
  return platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data())+16);
}
struct Family {
  Uuid object,pack;u64 generation=0;
  TypedUuid origin;u64 local=0;std::string_view subtype;
};
std::optional<Family> DecodeFamily(std::string_view bytes) {
  switch(Schema(bytes)) {
    case 65588:{const auto r=DecodeCatalogBootstrapPolicyPack(bytes);if(r.ok())return Family{
      r.record->policy_pack_uuid,r.record->policy_pack_uuid,r.record->generation,
      r.record->origin_transaction_uuid,r.record->origin_local_transaction_id,"bootstrap_policy_pack"};break;}
    case 65589:{const auto r=DecodeCatalogBootstrapSecurityProvider(bytes);if(r.ok())return Family{
      r.record->provider_uuid,r.record->policy_pack_uuid,r.record->generation,
      r.record->origin_transaction_uuid,r.record->origin_local_transaction_id,"bootstrap_security_provider"};break;}
    case 65590:{const auto r=DecodeCatalogBootstrapPolicyProfile(bytes);if(r.ok())return Family{
      r.record->profile_uuid,r.record->policy_pack_uuid,r.record->generation,
      r.record->origin_transaction_uuid,r.record->origin_local_transaction_id,"bootstrap_policy_profile"};break;}
  }
  return {};
}
} // namespace
CatalogValueEncodeIntoResult EncodeCatalogBootstrapPolicyPackInto(const CatalogBootstrapPolicyPack& r,std::span<byte> backing) {
  if(!Valid(r))return {E::invalid_value,{}};
  Fields f;f.Id(1,r.policy_pack_uuid);f.Number(2,r.generation);f.Text(3,r.pack_id);f.Text(4,r.pack_version);
  f.Bytes(5,r.content_sha256);f.Text(6,r.declared_signature_status);f.Number(7,r.create_time_only,T::boolean);
  f.Number(8,r.post_create_filesystem_authority,T::boolean);f.Number(9,r.local_password_only,T::boolean);
  f.Id(10,r.origin_transaction_uuid.value,UuidKind::transaction);f.Number(11,r.origin_local_transaction_id);
  for(unsigned i=0;i<r.declared_counts.size();++i)f.Number(12+i,r.declared_counts[i]);
  f.Number(21,r.imported_schema_version);
  return f.Encode(pack_schema,backing);
}
CatalogValueEncodeIntoResult EncodeCatalogBootstrapSecurityProviderInto(const CatalogBootstrapSecurityProvider& r,std::span<byte> backing) {
  if(!Valid(r))return {E::invalid_value,{}};
  Fields f;f.Id(1,r.provider_uuid);f.Number(2,r.generation);f.Id(3,r.policy_pack_uuid);
  f.Number(4,r.policy_pack_generation);f.Number(5,static_cast<u64>(r.family));f.Number(6,r.enabled_by_default,T::boolean);
  f.Number(7,static_cast<u64>(r.authority));f.Number(8,r.external_provider,T::boolean);
  if(r.verifier_policy)f.Number(9,static_cast<u64>(*r.verifier_policy));
  f.Id(10,r.origin_transaction_uuid.value,UuidKind::transaction);f.Number(11,r.origin_local_transaction_id);
  return f.Encode(provider_schema,backing);
}
CatalogValueEncodeIntoResult EncodeCatalogBootstrapPolicyProfileInto(const CatalogBootstrapPolicyProfile& r,std::span<byte> backing) {
  if(!Valid(r))return {E::invalid_value,{}};
  Fields f;f.Id(1,r.profile_uuid);f.Number(2,r.generation);f.Id(3,r.policy_pack_uuid);f.Number(4,r.policy_pack_generation);
  f.Text(5,r.area);f.Text(6,r.mode);if(r.resource_definition_uuid)f.Id(7,*r.resource_definition_uuid);
  f.Id(8,r.origin_transaction_uuid.value,UuidKind::transaction);f.Number(9,r.origin_local_transaction_id);
  return f.Encode(profile_schema,backing);
}
CatalogBootstrapPolicyResult<CatalogBootstrapPolicyPack> DecodeCatalogBootstrapPolicyPack(std::string_view bytes) {
  std::array<CatalogValueFieldView,21> backing;
  const auto fields=DecodeCatalogValueBlockInto(pack_schema,
      {reinterpret_cast<const byte*>(bytes.data()),bytes.size()},backing);
  if(!fields.ok())return {fields.error,{}};
  CatalogBootstrapPolicyPack r;
  for(const auto& f:fields.fields) switch(f.id) {
    case 1:r.policy_pack_uuid=f.identity()->value;break;case 2:r.generation=*f.unsigned_value();break;
    case 3:r.pack_id=Text(f);break;case 4:r.pack_version=Text(f);break;
    case 5:if(f.bytes.size()!=32)return {E::invalid_value,{}};
      std::copy(f.bytes.begin(),f.bytes.end(),r.content_sha256.begin());break;
    case 6:r.declared_signature_status=Text(f);break;
    case 7:r.create_time_only=f.bytes[0];break;case 8:r.post_create_filesystem_authority=f.bytes[0];break;
    case 9:r.local_password_only=f.bytes[0];break;case 10:r.origin_transaction_uuid=*f.identity();break;
    case 11:r.origin_local_transaction_id=*f.unsigned_value();break;
    case 21:r.imported_schema_version=*f.unsigned_value();break;
    default:r.declared_counts[f.id-12]=*f.unsigned_value();break;
  }
  if(!Valid(r))return {E::invalid_value,{}};
  return {E::none,r};
}
CatalogBootstrapPolicyResult<CatalogBootstrapSecurityProvider> DecodeCatalogBootstrapSecurityProvider(std::string_view bytes) {
  std::array<CatalogValueFieldView,11> backing;
  const auto fields=DecodeCatalogValueBlockInto(provider_schema,
      {reinterpret_cast<const byte*>(bytes.data()),bytes.size()},backing);
  if(!fields.ok())return {fields.error,{}};
  CatalogBootstrapSecurityProvider r;
  for(const auto& f:fields.fields) switch(f.id) {
    case 1:r.provider_uuid=f.identity()->value;break;case 2:r.generation=*f.unsigned_value();break;
    case 3:r.policy_pack_uuid=f.identity()->value;break;case 4:r.policy_pack_generation=*f.unsigned_value();break;
    case 5:r.family=static_cast<BootstrapProviderFamily>(*f.unsigned_value());break;
    case 6:r.enabled_by_default=f.bytes[0];break;
    case 7:r.authority=static_cast<BootstrapProviderAuthority>(*f.unsigned_value());break;
    case 8:r.external_provider=f.bytes[0];break;
    case 9:r.verifier_policy=static_cast<BootstrapVerifierPolicy>(*f.unsigned_value());break;
    case 10:r.origin_transaction_uuid=*f.identity();break;case 11:r.origin_local_transaction_id=*f.unsigned_value();break;
  }
  if(!Valid(r))return {E::invalid_value,{}};
  return {E::none,r};
}
CatalogBootstrapPolicyResult<CatalogBootstrapPolicyProfile> DecodeCatalogBootstrapPolicyProfile(std::string_view bytes) {
  std::array<CatalogValueFieldView,9> backing;
  const auto fields=DecodeCatalogValueBlockInto(profile_schema,
      {reinterpret_cast<const byte*>(bytes.data()),bytes.size()},backing);
  if(!fields.ok())return {fields.error,{}};
  CatalogBootstrapPolicyProfile r;
  for(const auto& f:fields.fields) switch(f.id) {
    case 1:r.profile_uuid=f.identity()->value;break;case 2:r.generation=*f.unsigned_value();break;
    case 3:r.policy_pack_uuid=f.identity()->value;break;case 4:r.policy_pack_generation=*f.unsigned_value();break;
    case 5:r.area=Text(f);break;case 6:r.mode=Text(f);break;
    case 7:r.resource_definition_uuid=f.identity()->value;break;
    case 8:r.origin_transaction_uuid=*f.identity();break;case 9:r.origin_local_transaction_id=*f.unsigned_value();break;
  }
  if(!Valid(r))return {E::invalid_value,{}};
  return {E::none,r};
}
bool IsCatalogBootstrapPolicyPayload(std::string_view bytes) {
  const auto id=Schema(bytes);return id>=65588&&id<=65590;
}
bool CatalogBootstrapPolicyMatchesHeader(const CatalogTypedRecordView& r) {
  if(r.header.kind!=CatalogRecordKind::policy||!Typed(r.header.object_uuid,UuidKind::object))return false;
  const auto family=DecodeFamily(r.payload);return family&&family->object==r.header.object_uuid.value;
}
bool CatalogBootstrapPolicyMatchesMetadata(const CatalogMetadataVersionView& m) {
  if(m.record.header.kind!=CatalogRecordKind::policy||!Typed(m.record.header.object_uuid,UuidKind::object)||
      m.authority_scope!=CatalogAuthorityScope::local||
      !Typed(m.owner_uuid,UuidKind::principal)||!Typed(m.audit_uuid,UuidKind::object)||
      !Typed(m.security_policy_uuid,UuidKind::object)||!Typed(m.default_name_uuid,UuidKind::object)||
      !Typed(m.name_vector_uuid,UuidKind::object)||!Typed(m.owning_schema_uuid,UuidKind::schema)||
      !Typed(m.creator_transaction_uuid,UuidKind::transaction)||!m.creator_local_transaction_id||
      !Typed(m.record.header.parent_uuid,UuidKind::object)||
      m.record.header.parent_uuid.value!=m.owning_schema_uuid.value)return false;
  const auto f=DecodeFamily(m.record.payload);
  return f&&f->object==m.record.header.object_uuid.value&&f->subtype==m.object_subtype&&
      f->generation==m.definition_version&&f->local<=m.creator_local_transaction_id&&
      (m.definition_version!=1||(f->origin.kind==m.creator_transaction_uuid.kind&&
        f->origin.value==m.creator_transaction_uuid.value&&f->local==m.creator_local_transaction_id));
}
bool CatalogBootstrapPolicyPreservesOrigin(const CatalogMetadataVersionView& a,const CatalogMetadataVersionView& b) {
  const auto family=[](const auto& m){return IsCatalogBootstrapPolicyPayload(m.record.payload)||
    m.object_subtype=="bootstrap_policy_pack"||m.object_subtype=="bootstrap_security_provider"||m.object_subtype=="bootstrap_policy_profile";};
  if(!family(a)&&!family(b))return true;
  if(!CatalogBootstrapPolicyMatchesMetadata(a)||!CatalogBootstrapPolicyMatchesMetadata(b))return false;
  const auto x=DecodeFamily(a.record.payload),y=DecodeFamily(b.record.payload);
  return x->subtype==y->subtype&&x->object==y->object&&x->pack==y->pack&&
      x->origin.kind==y->origin.kind&&x->origin.value==y->origin.value&&x->local==y->local;
}
} // namespace scratchbird::core::catalog
