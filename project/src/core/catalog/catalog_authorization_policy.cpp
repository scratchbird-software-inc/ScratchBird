// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_authorization_policy.hpp"
#include "uuid.hpp"
#include <array>
#include <limits>

namespace scratchbird::core::catalog {
namespace {
using T=CatalogValueType;using E=CatalogValueError;
constexpr CatalogValueFieldSchema fields[]{
  {1,T::engine_identity,true,16,UuidKind::object},
  {2,T::engine_identity,true,16,UuidKind::database},
  {3,T::unsigned_integer,true,8},
  {4,T::engine_identity,true,16,UuidKind::schema},
  {5,T::engine_identity,true,16,UuidKind::schema},
  {6,T::engine_identity,true,16,UuidKind::schema},
  {7,T::unsigned_integer,true,8},
  {8,T::engine_identity,false,16,UuidKind::schema},
  {9,T::engine_identity,true,16,UuidKind::schema},
  {10,T::engine_identity,true,16,UuidKind::schema},
  {11,T::engine_identity,true,16,UuidKind::schema},
  {12,T::unsigned_integer,true,8},
  {13,T::unsigned_integer,true,8},
  {14,T::unsigned_integer,true,8},
  {15,T::unsigned_integer,true,8},
  {16,T::unsigned_integer,true,8},
  {17,T::unsigned_integer,true,8},
  {18,T::unsigned_integer,true,8},
  {19,T::unsigned_integer,true,8},
  {20,T::unsigned_integer,true,8},
  {21,T::engine_identity,true,16,UuidKind::transaction},
  {22,T::unsigned_integer,true,8},
  {23,T::engine_identity,true,16,UuidKind::transaction},
  {24,T::unsigned_integer,true,8},
  {25,T::engine_identity,false,16,UuidKind::cluster},
};
constexpr CatalogValueSchemaView schema{kCatalogAuthorizationPolicySchemaId,1,fields};
bool Identity(const Uuid& id){return uuid::IsEngineIdentityUuid(id);}
bool Transaction(const TypedUuid& id){return id.kind==UuidKind::transaction&&Identity(id.value);}
bool Region(const void* data,std::size_t bytes){return (!bytes||data)&&bytes<=std::numeric_limits<std::uintptr_t>::max()-reinterpret_cast<std::uintptr_t>(data);}
bool Valid(const CatalogAuthorizationPolicyBody& r){
  if(!Identity(r.body_uuid)||!Identity(r.database_uuid)||!Transaction(r.origin_transaction_uuid)||!Transaction(r.creating_transaction_uuid)||
      (r.cluster_uuid&&!Identity(*r.cluster_uuid))||!r.generation||!r.origin_local_transaction_id||
      r.creating_local_transaction_id<r.origin_local_transaction_id||
      ((r.origin_transaction_uuid.value==r.creating_transaction_uuid.value)!=(r.origin_local_transaction_id==r.creating_local_transaction_id))||
      (r.generation==1&&r.creating_local_transaction_id!=r.origin_local_transaction_id)||
      u64(r.external_mapping)<1||u64(r.external_mapping)>2||u64(r.stale_behavior)<1||u64(r.stale_behavior)>2||
      u64(r.failure_behavior)<1||u64(r.failure_behavior)>2||
      (r.external_mapping==AuthorizationExternalMapping::internal_snapshot)!=r.external_mapping_schema_uuid.has_value()||
      (r.reload_trigger_mask&~u64{63})||(r.reload_trigger_mask&12)!=12||
      (r.external_mapping==AuthorizationExternalMapping::internal_snapshot&&!(r.reload_trigger_mask&16))||
      (r.flags&~u64{127})||(r.flags&63)!=63||!r.maximum_records||!r.maximum_membership_depth||
      r.maximum_membership_depth>r.maximum_records||!r.maximum_work||
      (r.stale_behavior==AuthorizationStaleBehavior::deny&&r.maximum_stale_microseconds)||
      (r.stale_behavior==AuthorizationStaleBehavior::bounded_cached_snapshot&&
        (r.external_mapping!=AuthorizationExternalMapping::internal_snapshot||!r.cache_lifetime_microseconds||!r.maximum_stale_microseconds)))return false;
  const std::array control{r.body_uuid,r.database_uuid,r.origin_transaction_uuid.value,r.creating_transaction_uuid.value,r.cluster_uuid.value_or(Uuid{})};
  const auto count=r.cluster_uuid?5u:4u;
  for(unsigned i=0;i<count;++i)for(unsigned j=0;j<i;++j)if(control[i]==control[j]&&!(i==3&&j==2))return false;
  const std::array sources{r.user_schema_uuid,r.role_schema_uuid,r.group_schema_uuid,r.grant_schema_uuid,
      r.row_policy_schema_uuid,r.column_policy_schema_uuid,r.external_mapping_schema_uuid.value_or(Uuid{})};
  const auto source_count=r.external_mapping_schema_uuid?7u:6u;
  for(unsigned i=0;i<source_count;++i){if(!Identity(sources[i]))return false;
    for(unsigned j=0;j<count;++j)if(sources[i]==control[j])return false;}
  return true;
}
struct Backing {
  std::array<CatalogValueFieldView,25> views{};
  std::array<std::array<byte,8>,25> numbers{};
  std::size_t count=0;
  void Id(u16 tag,const Uuid& id,UuidKind kind){views[count++]={tag,T::engine_identity,kind,id.bytes};}
  void Number(u16 tag,u64 n){auto& bytes=numbers[tag-1];platform::StoreLittle64(bytes.data(),n);views[count++]={tag,T::unsigned_integer,UuidKind::unknown,bytes};}
  explicit Backing(const CatalogAuthorizationPolicyBody& r){
    Id(1,r.body_uuid,UuidKind::object);
    Id(2,r.database_uuid,UuidKind::database);
    Number(3,r.generation);
    Id(4,r.user_schema_uuid,UuidKind::schema);
    Id(5,r.role_schema_uuid,UuidKind::schema);
    Id(6,r.group_schema_uuid,UuidKind::schema);
    Number(7,u64(r.external_mapping));
    if(r.external_mapping_schema_uuid)Id(8,*r.external_mapping_schema_uuid,UuidKind::schema);
    Id(9,r.grant_schema_uuid,UuidKind::schema);
    Id(10,r.row_policy_schema_uuid,UuidKind::schema);
    Id(11,r.column_policy_schema_uuid,UuidKind::schema);
    Number(12,r.cache_lifetime_microseconds);
    Number(13,r.reload_trigger_mask);
    Number(14,u64(r.stale_behavior));
    Number(15,r.maximum_stale_microseconds);
    Number(16,u64(r.failure_behavior));
    Number(17,r.maximum_records);
    Number(18,r.maximum_membership_depth);
    Number(19,r.maximum_work);
    Number(20,r.flags);
    Id(21,r.origin_transaction_uuid.value,UuidKind::transaction);
    Number(22,r.origin_local_transaction_id);
    Id(23,r.creating_transaction_uuid.value,UuidKind::transaction);
    Number(24,r.creating_local_transaction_id);
    if(r.cluster_uuid)Id(25,*r.cluster_uuid,UuidKind::cluster);
  }
  auto View()const{return std::span<const CatalogValueFieldView>(views).first(count);}
};
bool Header(const CatalogTypedRecordView& m,const CatalogAuthorizationPolicyBody& r){
  for(const auto& id:{r.body_uuid,r.database_uuid,r.user_schema_uuid,r.role_schema_uuid,r.group_schema_uuid,r.external_mapping_schema_uuid.value_or(Uuid{}),r.grant_schema_uuid,r.row_policy_schema_uuid,r.column_policy_schema_uuid,r.origin_transaction_uuid.value,r.creating_transaction_uuid.value,r.cluster_uuid.value_or(Uuid{})})if(m.header.row_uuid.value==id)return false;
  return m.header.kind==CatalogRecordKind::policy&&m.header.object_uuid.kind==UuidKind::object&&m.header.object_uuid.value==r.body_uuid&&
      m.header.row_uuid.kind==UuidKind::row&&Identity(m.header.row_uuid.value);
}
bool Metadata(const CatalogMetadataVersionView& m,const CatalogAuthorizationPolicyBody& r){
  return Header(m.record,r)&&m.object_subtype=="authorization_policy_body"&&m.definition_version==r.generation&&
      m.creator_transaction_uuid.kind==UuidKind::transaction&&m.creator_transaction_uuid.value==r.creating_transaction_uuid.value&&
      m.creator_local_transaction_id==r.creating_local_transaction_id&&
      (m.authority_scope==CatalogAuthorityScope::cluster)==r.cluster_uuid.has_value();
}
bool Family(const CatalogMetadataVersionView& m){return m.object_subtype=="authorization_policy_body"||IsCatalogAuthorizationPolicyPayload(m.record.payload);}
}
std::optional<std::size_t> CatalogAuthorizationPolicyEncodedBytes(const CatalogAuthorizationPolicyBody& r) noexcept {
  if(!Valid(r))return {};const Backing b(r);if(ValidateCatalogValueFields(schema,b.View())!=E::none)return {};
  std::size_t bytes=kCatalogValueBlockHeaderBytes;for(const auto& f:b.View())bytes+=8+f.bytes.size();return bytes;
}
CatalogValueEncodeIntoResult EncodeCatalogAuthorizationPolicyInto(const CatalogAuthorizationPolicyBody& r,std::span<byte> output) noexcept {
  if(!Valid(r))return {E::invalid_value,{}};
  if(!Region(output.data(),output.size()))return {E::invalid_backing,{}};
  const auto at=reinterpret_cast<std::uintptr_t>(output.data()),source=reinterpret_cast<std::uintptr_t>(&r);
  if(output.size()&&at<source+sizeof(r)&&source<at+output.size())return {E::invalid_backing,{}};
  const Backing b(r);return EncodeCatalogValueBlockInto(schema,b.View(),output);
}
CatalogAuthorizationPolicyResult DecodeCatalogAuthorizationPolicy(std::string_view bytes) noexcept {
  std::array<CatalogValueFieldView,25> backing;
  const auto parsed=DecodeCatalogValueBlockInto(schema,{reinterpret_cast<const byte*>(bytes.data()),bytes.size()},backing);
  if(!parsed.ok())return {parsed.error,{}};CatalogAuthorizationPolicyBody r;
  for(const auto& f:parsed.fields)switch(f.id){
    case 1:r.body_uuid=f.identity()->value;break;
    case 2:r.database_uuid=f.identity()->value;break;
    case 3:r.generation=*f.unsigned_value();break;
    case 4:r.user_schema_uuid=f.identity()->value;break;
    case 5:r.role_schema_uuid=f.identity()->value;break;
    case 6:r.group_schema_uuid=f.identity()->value;break;
    case 7:r.external_mapping=AuthorizationExternalMapping(*f.unsigned_value());break;
    case 8:r.external_mapping_schema_uuid=f.identity()->value;break;
    case 9:r.grant_schema_uuid=f.identity()->value;break;
    case 10:r.row_policy_schema_uuid=f.identity()->value;break;
    case 11:r.column_policy_schema_uuid=f.identity()->value;break;
    case 12:r.cache_lifetime_microseconds=*f.unsigned_value();break;
    case 13:r.reload_trigger_mask=*f.unsigned_value();break;
    case 14:r.stale_behavior=AuthorizationStaleBehavior(*f.unsigned_value());break;
    case 15:r.maximum_stale_microseconds=*f.unsigned_value();break;
    case 16:r.failure_behavior=AuthorizationFailureBehavior(*f.unsigned_value());break;
    case 17:r.maximum_records=*f.unsigned_value();break;
    case 18:r.maximum_membership_depth=*f.unsigned_value();break;
    case 19:r.maximum_work=*f.unsigned_value();break;
    case 20:r.flags=*f.unsigned_value();break;
    case 21:r.origin_transaction_uuid=*f.identity();break;
    case 22:r.origin_local_transaction_id=*f.unsigned_value();break;
    case 23:r.creating_transaction_uuid=*f.identity();break;
    case 24:r.creating_local_transaction_id=*f.unsigned_value();break;
    case 25:r.cluster_uuid=f.identity()->value;break;
  }
  if(!Valid(r))return {E::invalid_value,{}};return {E::none,r};
}
bool IsCatalogAuthorizationPolicyPayload(std::string_view bytes) noexcept {return bytes.size()>=kCatalogValueBlockHeaderBytes&&bytes.substr(0,4)=="SBCV"&&
    platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data())+16)==kCatalogAuthorizationPolicySchemaId;}
bool CatalogAuthorizationPolicyMatchesHeader(const CatalogTypedRecordView& m) noexcept {const auto r=DecodeCatalogAuthorizationPolicy(m.payload);return r.ok()&&Header(m,*r.record);}
bool CatalogAuthorizationPolicyMatchesMetadata(const CatalogMetadataVersionView& m) noexcept {const auto r=DecodeCatalogAuthorizationPolicy(m.record.payload);return r.ok()&&Metadata(m,*r.record);}
bool CatalogAuthorizationPolicyMatchesDatabase(const CatalogMetadataVersionView& m,const Uuid& db) noexcept {
  if(!Family(m))return true;const auto r=DecodeCatalogAuthorizationPolicy(m.record.payload);return Identity(db)&&r.ok()&&Metadata(m,*r.record)&&r.record->database_uuid==db;
}
bool CatalogAuthorizationPolicyPreservesOrigin(const CatalogMetadataVersionView& a,const CatalogMetadataVersionView& b) noexcept {
  if(!Family(a)&&!Family(b))return true;const auto x=DecodeCatalogAuthorizationPolicy(a.record.payload),y=DecodeCatalogAuthorizationPolicy(b.record.payload);
  if(!x.ok()||!y.ok()||!Metadata(a,*x.record)||!Metadata(b,*y.record))return false;const auto& p=*x.record;const auto& q=*y.record;
  return p.body_uuid==q.body_uuid&&p.database_uuid==q.database_uuid&&p.origin_transaction_uuid.value==q.origin_transaction_uuid.value&&
      p.origin_local_transaction_id==q.origin_local_transaction_id&&((p.generation==q.generation&&a.record.payload==b.record.payload)||q.generation>p.generation);
}
bool CatalogAuthorizationPolicyMatchesDescriptor(const CatalogAuthorizationPolicyBody& body,const CatalogPolicyDescriptor& descriptor) noexcept {
  return Valid(body)&&CatalogPolicyDescriptorEncodedBytes(descriptor).has_value()&&descriptor.policy_class==CatalogPolicyClass::authorization&&
      descriptor.database_uuid==body.database_uuid&&descriptor.body_uuid==body.body_uuid&&descriptor.body_generation==body.generation&&
      descriptor.cluster_uuid==body.cluster_uuid;
}
} // namespace scratchbird::core::catalog
