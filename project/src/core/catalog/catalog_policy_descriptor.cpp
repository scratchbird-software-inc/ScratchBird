// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_policy_descriptor.hpp"
#include "uuid.hpp"
#include <array>
#include <limits>

namespace scratchbird::core::catalog {
namespace {
using E=CatalogValueError;using T=CatalogValueType;
constexpr u32 schema_id=65619;
constexpr CatalogValueFieldSchema fields[]{
  {1,T::engine_identity,true,16,UuidKind::object},{2,T::engine_identity,true,16,UuidKind::object},
  {3,T::engine_identity,true,16,UuidKind::database},{4,T::unsigned_integer,true,8},
  {5,T::unsigned_integer,true,8},{6,T::unsigned_integer,true,8},
  {7,T::unsigned_integer,false,8},{8,T::unsigned_integer,false,8},{9,T::unsigned_integer,true,8},
  {10,T::engine_identity,true,16,UuidKind::object},{11,T::engine_identity,true,16,UuidKind::object},
  {12,T::unsigned_integer,true,8},{13,T::unsigned_integer,true,8},{14,T::unsigned_integer,true,8},
  {15,T::engine_identity,false,16,UuidKind::object},{16,T::engine_identity,false,16,UuidKind::object},
  {17,T::engine_identity,false,16,UuidKind::object},{18,T::unsigned_integer,true,8},
  {19,T::unsigned_integer,true,8},{20,T::unsigned_integer,true,8},{21,T::unsigned_integer,true,8},
  {22,T::engine_identity,false,16,UuidKind::cluster},{23,T::engine_identity,true,16,UuidKind::transaction},
  {24,T::unsigned_integer,true,8},{25,T::engine_identity,true,16,UuidKind::transaction},
  {26,T::engine_identity,true,16,UuidKind::object},{27,T::unsigned_integer,true,8}};
constexpr CatalogValueSchemaView schema{schema_id,kCatalogPolicyDescriptorSchemaVersion,fields};
bool Id(const Uuid& id){return uuid::IsEngineIdentityUuid(id);}
bool Typed(const TypedUuid& id,UuidKind kind){return id.kind==kind&&Id(id.value);}
bool AuthorityOwner(const TypedUuid& id){
  // MGA permits a subject/object owner, not only a user principal. The actual
  // owner's class and permission still require enclosing reference admission.
  switch(id.kind){
    case UuidKind::principal:case UuidKind::object:case UuidKind::schema:
    case UuidKind::database:case UuidKind::cluster:return Id(id.value);
    default:return false;
  }
}
template<class Enum>bool Range(Enum value,u64 maximum){const auto n=static_cast<u64>(value);return n>=1&&n<=maximum;}
bool ClusterScope(CatalogPolicyScope scope){return scope==CatalogPolicyScope::cluster_member||scope==CatalogPolicyScope::placement_unit;}
bool Valid(const CatalogPolicyDescriptor& r){
  if(!Id(r.policy_uuid)||!Id(r.policy_version_uuid)||!Id(r.database_uuid)||!Id(r.scope_uuid)||!Id(r.authority_owner_uuid)||
      !Id(r.body_uuid)||!r.body_generation||
      !Typed(r.origin_transaction_uuid,UuidKind::transaction)||!Typed(r.creating_transaction_uuid,UuidKind::transaction)||
      !r.generation||!r.creating_transaction_number||!r.origin_local_transaction_id||
      r.origin_local_transaction_id>r.creating_transaction_number||!Range(r.policy_class,23)||!Range(r.scope,21)||
      !r.precedence_rank||!Range(r.default_decision,5)||!Range(r.exception_behavior,5)||
      !Range(r.recovery_behavior,4)||!Range(r.activation_boundary,6)||
      !r.enforcement_points||(r.enforcement_points&~kCatalogPolicyEnforcementMask)||
      (r.cache_invalidation_mask&~kCatalogPolicyInvalidationMask)||
      (r.scope==CatalogPolicyScope::database&&r.scope_uuid!=r.database_uuid)||
      (ClusterScope(r.scope)&&!r.cluster_uuid)||
      (r.generation==1&&(r.origin_transaction_uuid.value!=r.creating_transaction_uuid.value||
          r.origin_local_transaction_id!=r.creating_transaction_number)))return false;
  for(const auto& id:{r.database_uuid,r.origin_transaction_uuid.value,r.creating_transaction_uuid.value})
    if(r.policy_uuid==id||r.policy_version_uuid==id)return false;
  if(r.policy_uuid==r.policy_version_uuid||r.database_uuid==r.origin_transaction_uuid.value||
      r.database_uuid==r.creating_transaction_uuid.value)return false;
  for(const auto& id:{r.policy_uuid,r.policy_version_uuid,r.database_uuid,
      r.origin_transaction_uuid.value,r.creating_transaction_uuid.value})
    if(r.body_uuid==id)return false;
  for(const auto* value:{&r.effective_transaction_number,&r.superseding_transaction_number})
    if(*value&&(!**value||**value<r.creating_transaction_number))return false;
  if(r.effective_transaction_number&&r.superseding_transaction_number&&
      *r.superseding_transaction_number<*r.effective_transaction_number)return false;
  for(const auto* id:{&r.evidence_profile_uuid,&r.disclosure_profile_uuid,&r.retention_profile_uuid,&r.cluster_uuid})
    if(*id&&!Id(**id))return false;
  return true;
}
struct Backing {
  std::array<CatalogValueFieldView,27> views{};
  std::array<std::array<byte,8>,27> numbers{};
  std::size_t size=0;
  void Identity(u16 tag,const Uuid& id,UuidKind kind=UuidKind::object){views[size++]={tag,T::engine_identity,kind,id.bytes};}
  void Number(u16 tag,u64 value){auto& b=numbers[tag-1];platform::StoreLittle64(b.data(),value);views[size++]={tag,T::unsigned_integer,UuidKind::unknown,b};}
  explicit Backing(const CatalogPolicyDescriptor& r){
    Identity(1,r.policy_uuid);Identity(2,r.policy_version_uuid);Identity(3,r.database_uuid,UuidKind::database);
    Number(4,r.generation);Number(5,static_cast<u64>(r.policy_class));Number(6,r.creating_transaction_number);
    if(r.effective_transaction_number)Number(7,*r.effective_transaction_number);
    if(r.superseding_transaction_number)Number(8,*r.superseding_transaction_number);
    Number(9,static_cast<u64>(r.scope));Identity(10,r.scope_uuid);Identity(11,r.authority_owner_uuid);Number(12,r.precedence_rank);
    Number(13,static_cast<u64>(r.default_decision));Number(14,static_cast<u64>(r.exception_behavior));
    if(r.evidence_profile_uuid)Identity(15,*r.evidence_profile_uuid);
    if(r.disclosure_profile_uuid)Identity(16,*r.disclosure_profile_uuid);
    if(r.retention_profile_uuid)Identity(17,*r.retention_profile_uuid);
    Number(18,static_cast<u64>(r.recovery_behavior));Number(19,static_cast<u64>(r.activation_boundary));
    Number(20,r.cache_invalidation_mask);Number(21,r.enforcement_points);
    if(r.cluster_uuid)Identity(22,*r.cluster_uuid,UuidKind::cluster);
    Identity(23,r.origin_transaction_uuid.value,UuidKind::transaction);Number(24,r.origin_local_transaction_id);
    Identity(25,r.creating_transaction_uuid.value,UuidKind::transaction);
    Identity(26,r.body_uuid);Number(27,r.body_generation);
  }
  auto View()const{return std::span<const CatalogValueFieldView>(views).first(size);}
};
bool Header(const CatalogTypedRecordView& m,const CatalogPolicyDescriptor& r){
  return m.header.kind==CatalogRecordKind::policy&&Typed(m.header.object_uuid,UuidKind::object)&&
      m.header.object_uuid.value==r.policy_uuid&&Typed(m.header.row_uuid,UuidKind::row)&&
      m.header.row_uuid.value!=r.policy_uuid&&m.header.row_uuid.value!=r.policy_version_uuid&&
      m.header.row_uuid.value!=r.body_uuid;
}
bool Metadata(const CatalogMetadataVersionView& m,const CatalogPolicyDescriptor& r){
  return Header(m.record,r)&&m.object_subtype=="policy_descriptor"&&m.definition_version==r.generation&&
      AuthorityOwner(m.owner_uuid)&&m.owner_uuid.value==r.authority_owner_uuid&&
      Typed(m.owning_schema_uuid,UuidKind::schema)&&Typed(m.record.header.parent_uuid,UuidKind::object)&&
      m.record.header.parent_uuid.value==m.owning_schema_uuid.value&&Typed(m.audit_uuid,UuidKind::object)&&
      Typed(m.default_name_uuid,UuidKind::object)&&Typed(m.name_vector_uuid,UuidKind::object)&&
      Typed(m.creator_transaction_uuid,UuidKind::transaction)&&m.creator_transaction_uuid.value==r.creating_transaction_uuid.value&&
      m.creator_local_transaction_id==r.creating_transaction_number&&
      (!(m.authority_scope==CatalogAuthorityScope::cluster)||r.cluster_uuid.has_value())&&
      (!ClusterScope(r.scope)||m.authority_scope==CatalogAuthorityScope::cluster);
}
bool Family(const CatalogMetadataVersionView& m){return m.object_subtype=="policy_descriptor"||IsCatalogPolicyDescriptorPayload(m.record.payload);}
}
std::optional<std::size_t> CatalogPolicyDescriptorEncodedBytes(const CatalogPolicyDescriptor& r){
  if(!Valid(r))return {};const Backing b(r);if(ValidateCatalogValueFields(schema,b.View())!=E::none)return {};
  std::size_t bytes=kCatalogValueBlockHeaderBytes;for(const auto& f:b.View())bytes+=8+f.bytes.size();return bytes;
}
CatalogValueEncodeIntoResult EncodeCatalogPolicyDescriptorInto(const CatalogPolicyDescriptor& r,std::span<byte> out){
  if(!Valid(r))return {E::invalid_value,{}};
  const auto dest=reinterpret_cast<std::uintptr_t>(out.data()),input=reinterpret_cast<std::uintptr_t>(&r);
  if((out.size()&&!out.data())||out.size()>std::numeric_limits<std::uintptr_t>::max()-dest||
      (dest<input+sizeof(r)&&input<dest+out.size()))return {E::invalid_backing,{}};
  const Backing b(r);return EncodeCatalogValueBlockInto(schema,b.View(),out);
}
CatalogPolicyDescriptorResult DecodeCatalogPolicyDescriptor(std::string_view bytes){
  std::array<CatalogValueFieldView,27> backing;
  const auto d=DecodeCatalogValueBlockInto(schema,{reinterpret_cast<const byte*>(bytes.data()),bytes.size()},backing);
  if(!d.ok())return {d.error,{}};CatalogPolicyDescriptor r;
  for(const auto& f:d.fields)switch(f.id){
    case 1:r.policy_uuid=f.identity()->value;break;case 2:r.policy_version_uuid=f.identity()->value;break;
    case 3:r.database_uuid=f.identity()->value;break;case 4:r.generation=*f.unsigned_value();break;
    case 5:r.policy_class=static_cast<CatalogPolicyClass>(*f.unsigned_value());break;
    case 6:r.creating_transaction_number=*f.unsigned_value();break;case 7:r.effective_transaction_number=*f.unsigned_value();break;
    case 8:r.superseding_transaction_number=*f.unsigned_value();break;case 9:r.scope=static_cast<CatalogPolicyScope>(*f.unsigned_value());break;
    case 10:r.scope_uuid=f.identity()->value;break;case 11:r.authority_owner_uuid=f.identity()->value;break;
    case 12:{const auto n=*f.unsigned_value();if(n>65535)return {E::invalid_value,{}};r.precedence_rank=static_cast<u16>(n);break;}
    case 13:r.default_decision=static_cast<CatalogPolicyDefault>(*f.unsigned_value());break;
    case 14:r.exception_behavior=static_cast<CatalogPolicyException>(*f.unsigned_value());break;
    case 15:r.evidence_profile_uuid=f.identity()->value;break;case 16:r.disclosure_profile_uuid=f.identity()->value;break;
    case 17:r.retention_profile_uuid=f.identity()->value;break;case 18:r.recovery_behavior=static_cast<CatalogPolicyRecovery>(*f.unsigned_value());break;
    case 19:r.activation_boundary=static_cast<CatalogPolicyActivation>(*f.unsigned_value());break;
    case 20:r.cache_invalidation_mask=*f.unsigned_value();break;case 21:r.enforcement_points=*f.unsigned_value();break;
    case 22:r.cluster_uuid=f.identity()->value;break;case 23:r.origin_transaction_uuid=*f.identity();break;
    case 24:r.origin_local_transaction_id=*f.unsigned_value();break;case 25:r.creating_transaction_uuid=*f.identity();break;
    case 26:r.body_uuid=f.identity()->value;break;case 27:r.body_generation=*f.unsigned_value();break;
  }
  if(!Valid(r))return {E::invalid_value,{}};return {E::none,r};
}
bool IsCatalogPolicyDescriptorPayload(std::string_view b){return b.size()>=kCatalogValueBlockHeaderBytes&&b.substr(0,4)=="SBCV"&&
    platform::LoadLittle32(reinterpret_cast<const byte*>(b.data())+16)==schema_id;}
bool CatalogPolicyDescriptorMatchesHeader(const CatalogTypedRecordView& m){const auto r=DecodeCatalogPolicyDescriptor(m.payload);return r.ok()&&Header(m,*r.record);}
bool CatalogPolicyDescriptorMatchesMetadata(const CatalogMetadataVersionView& m){const auto r=DecodeCatalogPolicyDescriptor(m.record.payload);return r.ok()&&Metadata(m,*r.record);}
bool CatalogPolicyDescriptorMatchesDatabase(const CatalogMetadataVersionView& m,const Uuid& db){if(!Family(m))return true;
  const auto r=DecodeCatalogPolicyDescriptor(m.record.payload);return Id(db)&&r.ok()&&r.record->database_uuid==db&&Metadata(m,*r.record);}
bool CatalogPolicyDescriptorPreservesOrigin(const CatalogMetadataVersionView& a,const CatalogMetadataVersionView& b){
  if(!Family(a)&&!Family(b))return true;const auto x=DecodeCatalogPolicyDescriptor(a.record.payload),y=DecodeCatalogPolicyDescriptor(b.record.payload);
  if(!x.ok()||!y.ok()||!Metadata(a,*x.record)||!Metadata(b,*y.record))return false;
  const auto& p=*x.record;const auto& q=*y.record;
  return p.policy_uuid==q.policy_uuid&&p.database_uuid==q.database_uuid&&p.policy_class==q.policy_class&&
      p.origin_transaction_uuid.value==q.origin_transaction_uuid.value&&p.origin_local_transaction_id==q.origin_local_transaction_id&&
      ((q.generation==p.generation&&a.record.payload==b.record.payload)||
       (q.generation>p.generation&&q.policy_version_uuid!=p.policy_version_uuid));
}
} // namespace scratchbird::core::catalog
