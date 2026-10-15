// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_construction_intent.hpp"
#include "uuid.hpp"
#include <array>
#include <limits>

namespace scratchbird::core::catalog {
namespace {
using E=CatalogValueError;using T=CatalogValueType;
constexpr u32 schema_id=65618;
constexpr CatalogValueFieldSchema fields[]{
  {1,T::engine_identity,true,16,UuidKind::object},
  {2,T::engine_identity,true,16,UuidKind::database},
  {3,T::engine_identity,true,16,UuidKind::object},
  {4,T::engine_identity,true,16,UuidKind::object},
  {5,T::engine_identity,true,16,UuidKind::filespace},
  {6,T::engine_identity,true,16,UuidKind::transaction},
  {7,T::unsigned_integer,true,8},{8,T::unsigned_integer,true,8},
  {9,T::unsigned_integer,true,8},{10,T::unsigned_integer,true,8},
  {11,T::unsigned_integer,true,8},{12,T::unsigned_integer,true,8},
  {13,T::unsigned_integer,true,8},
  {14,T::engine_identity,false,16,UuidKind::principal}};
constexpr CatalogValueSchemaView schema{schema_id,1,fields};
bool Id(const Uuid& id){return uuid::IsEngineIdentityUuid(id);}
bool Typed(const TypedUuid& id,UuidKind kind){return id.kind==kind&&Id(id.value);}
bool Valid(const CatalogConstructionIntent& r){
  if(!Typed(r.origin_transaction_uuid,UuidKind::transaction)||r.origin_local_transaction_id!=1||
      !r.event_time_utc_us||r.catalog_generation!=1||
      r.event!=ConstructionAuditEvent::construction_intent||r.event_class!=ConstructionAuditClass::bootstrap_construction||
      r.policy_decision!=ConstructionAuditDecision::not_evaluated||r.redaction!=ConstructionAuditRedaction::safe_message_only||
      (r.actor_uuid&&!Typed(*r.actor_uuid,UuidKind::principal)))return false;
  const std::array<Uuid,7> ids{r.event_uuid,r.database_uuid,r.engine_uuid,r.operation_uuid,
      r.filespace_uuid,r.origin_transaction_uuid.value,r.actor_uuid?r.actor_uuid->value:Uuid{}};
  const auto count=r.actor_uuid?7U:6U;
  for(unsigned i=0;i<count;++i){if(!Id(ids[i]))return false;
    for(unsigned j=0;j<i;++j)if(ids[i]==ids[j])return false;}
  return true;
}
struct Backing {
  std::array<CatalogValueFieldView,14> views{};
  std::array<std::array<byte,8>,7> numbers{};
  std::size_t size=0;
  void Identity(u16 tag,const Uuid& id,UuidKind kind){views[size++]={tag,T::engine_identity,kind,id.bytes};}
  void Number(u16 tag,u64 value){auto& b=numbers[tag-7];platform::StoreLittle64(b.data(),value);
    views[size++]={tag,T::unsigned_integer,UuidKind::unknown,b};}
  explicit Backing(const CatalogConstructionIntent& r){
    Identity(1,r.event_uuid,UuidKind::object);Identity(2,r.database_uuid,UuidKind::database);
    Identity(3,r.engine_uuid,UuidKind::object);Identity(4,r.operation_uuid,UuidKind::object);
    Identity(5,r.filespace_uuid,UuidKind::filespace);Identity(6,r.origin_transaction_uuid.value,UuidKind::transaction);
    Number(7,r.origin_local_transaction_id);Number(8,r.event_time_utc_us);Number(9,r.catalog_generation);
    Number(10,static_cast<u64>(r.event));Number(11,static_cast<u64>(r.event_class));
    Number(12,static_cast<u64>(r.policy_decision));Number(13,static_cast<u64>(r.redaction));
    if(r.actor_uuid)Identity(14,r.actor_uuid->value,UuidKind::principal);
  }
  auto View()const{return std::span<const CatalogValueFieldView>(views).first(size);}
};
bool Header(const CatalogTypedRecordView& m,const CatalogConstructionIntent& r){
  return m.header.kind==CatalogRecordKind::audit_evidence&&Typed(m.header.object_uuid,UuidKind::object)&&
      m.header.object_uuid.value==r.event_uuid&&Typed(m.header.row_uuid,UuidKind::row)&&
      m.header.row_uuid.value!=r.event_uuid;
}
bool Metadata(const CatalogMetadataVersionView& m,const CatalogConstructionIntent& r){
  if(!Header(m.record,r)||m.object_subtype!="node_construction_intent"||m.authority_scope!=CatalogAuthorityScope::local||
      !Typed(m.owner_uuid,UuidKind::principal)||!Typed(m.owning_schema_uuid,UuidKind::schema)||
      !Typed(m.record.header.parent_uuid,UuidKind::object)||m.record.header.parent_uuid.value!=m.owning_schema_uuid.value||
      !Typed(m.audit_uuid,UuidKind::object)||m.audit_uuid.value!=r.event_uuid||
      m.default_name_uuid.valid()||!m.default_name_uuid.value.is_nil()||m.name_vector_uuid.valid()||!m.name_vector_uuid.value.is_nil()||
      !Typed(m.creator_transaction_uuid,UuidKind::transaction)||m.creator_local_transaction_id<r.origin_local_transaction_id||
      !m.definition_version||
      (m.definition_version==1&&(m.creator_transaction_uuid.value!=r.origin_transaction_uuid.value||m.creator_local_transaction_id!=1)))return false;
  // A row/schema identity cannot masquerade as an operation, runtime or origin.
  for(const auto& id:{r.event_uuid,r.database_uuid,r.engine_uuid,r.operation_uuid,r.filespace_uuid,r.origin_transaction_uuid.value})
    if(m.record.header.row_uuid.value==id||m.owning_schema_uuid.value==id)return false;
  if(r.actor_uuid&&(m.record.header.row_uuid.value==r.actor_uuid->value||m.owning_schema_uuid.value==r.actor_uuid->value))return false;
  return true;
}
bool Family(const CatalogMetadataVersionView& m){return m.object_subtype=="node_construction_intent"||IsCatalogConstructionIntentPayload(m.record.payload);}
}
std::optional<std::size_t> CatalogConstructionIntentEncodedBytes(const CatalogConstructionIntent& r){
  if(!Valid(r))return {};const Backing b(r);
  if(ValidateCatalogValueFields(schema,b.View())!=E::none)return {};
  return r.actor_uuid?304:280;
}
CatalogValueEncodeIntoResult EncodeCatalogConstructionIntentInto(const CatalogConstructionIntent& r,std::span<byte> out){
  if(!Valid(r))return {E::invalid_value,{}};
  const auto dest=reinterpret_cast<std::uintptr_t>(out.data()),input=reinterpret_cast<std::uintptr_t>(&r);
  if((out.size()&&!out.data())||out.size()>std::numeric_limits<std::uintptr_t>::max()-dest||
      (dest<input+sizeof(r)&&input<dest+out.size()))return {E::invalid_backing,{}};
  const Backing backing(r);return EncodeCatalogValueBlockInto(schema,backing.View(),out);
}
CatalogConstructionIntentResult DecodeCatalogConstructionIntent(std::string_view bytes){
  std::array<CatalogValueFieldView,14> backing;
  const auto d=DecodeCatalogValueBlockInto(schema,{reinterpret_cast<const byte*>(bytes.data()),bytes.size()},backing);
  if(!d.ok())return {d.error,{}};
  CatalogConstructionIntent r;
  for(const auto& f:d.fields)switch(f.id){
    case 1:r.event_uuid=f.identity()->value;break;case 2:r.database_uuid=f.identity()->value;break;
    case 3:r.engine_uuid=f.identity()->value;break;case 4:r.operation_uuid=f.identity()->value;break;
    case 5:r.filespace_uuid=f.identity()->value;break;case 6:r.origin_transaction_uuid=*f.identity();break;
    case 7:r.origin_local_transaction_id=*f.unsigned_value();break;case 8:r.event_time_utc_us=*f.unsigned_value();break;
    case 9:r.catalog_generation=*f.unsigned_value();break;case 10:r.event=static_cast<ConstructionAuditEvent>(*f.unsigned_value());break;
    case 11:r.event_class=static_cast<ConstructionAuditClass>(*f.unsigned_value());break;
    case 12:r.policy_decision=static_cast<ConstructionAuditDecision>(*f.unsigned_value());break;
    case 13:r.redaction=static_cast<ConstructionAuditRedaction>(*f.unsigned_value());break;
    case 14:r.actor_uuid=*f.identity();break;
  }
  if(!Valid(r))return {E::invalid_value,{}};return {E::none,r};
}
bool IsCatalogConstructionIntentPayload(std::string_view b){return b.size()>=kCatalogValueBlockHeaderBytes&&b.substr(0,4)=="SBCV"&&
    platform::LoadLittle32(reinterpret_cast<const byte*>(b.data())+16)==schema_id;}
bool CatalogConstructionIntentMatchesHeader(const CatalogTypedRecordView& m){const auto r=DecodeCatalogConstructionIntent(m.payload);return r.ok()&&Header(m,*r.record);}
bool CatalogConstructionIntentMatchesMetadata(const CatalogMetadataVersionView& m){const auto r=DecodeCatalogConstructionIntent(m.record.payload);return r.ok()&&Metadata(m,*r.record);}
bool CatalogConstructionIntentMatchesDatabase(const CatalogMetadataVersionView& m,const Uuid& db){if(!Family(m))return true;
  const auto r=DecodeCatalogConstructionIntent(m.record.payload);return Id(db)&&r.ok()&&r.record->database_uuid==db&&Metadata(m,*r.record);}
bool CatalogConstructionIntentPreservesOrigin(const CatalogMetadataVersionView& a,const CatalogMetadataVersionView& b){
  if(!Family(a)&&!Family(b))return true;return a.record.payload==b.record.payload&&
      CatalogConstructionIntentMatchesMetadata(a)&&CatalogConstructionIntentMatchesMetadata(b);
}
} // namespace scratchbird::core::catalog
