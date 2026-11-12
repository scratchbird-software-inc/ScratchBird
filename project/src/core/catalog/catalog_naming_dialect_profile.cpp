// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_naming_dialect_profile.hpp"
#include "uuid.hpp"
#include <array>
#include <limits>

namespace scratchbird::core::catalog {
namespace {
using T=CatalogValueType;using E=CatalogValueError;
constexpr auto Fields(){
  std::array<CatalogValueFieldSchema,10> out{};
  for(u16 i=0;i<out.size();++i)out[i]={u16(i+1),T::unsigned_integer,true,8};
  for(const u16 tag:{1,4})out[tag-1]={tag,T::engine_identity,true,16,UuidKind::object};
  out[1]={2,T::engine_identity,true,16,UuidKind::database};
  for(const u16 tag:{7,9})out[tag-1]={tag,T::engine_identity,true,16,UuidKind::transaction};
  return out;
}
constexpr auto fields=Fields();
constexpr CatalogValueSchemaView schema{kCatalogNamingDialectProfileSchemaId,1,fields};
bool Identity(const Uuid& id){return uuid::IsEngineIdentityUuid(id);}
bool Typed(const TypedUuid& id,UuidKind kind){return id.kind==kind&&Identity(id.value);}
auto Identities(const CatalogNamingDialectProfile& r){return std::array{r.profile_uuid,r.database_uuid,
    r.identifier_profile_uuid,r.origin_transaction_uuid.value,r.creating_transaction_uuid.value};}
bool Valid(const CatalogNamingDialectProfile& r){
  if(!r.generation||!r.identifier_profile_generation||!Typed(r.origin_transaction_uuid,UuidKind::transaction)||
      !Typed(r.creating_transaction_uuid,UuidKind::transaction)||!r.origin_local_transaction_id||
      r.creating_local_transaction_id<r.origin_local_transaction_id||
      ((r.origin_transaction_uuid.value==r.creating_transaction_uuid.value)!=(r.origin_local_transaction_id==r.creating_local_transaction_id))||
      (r.generation==1&&r.creating_local_transaction_id!=r.origin_local_transaction_id))return false;
  const auto ids=Identities(r);
  for(std::size_t i=0;i<ids.size();++i){if(!Identity(ids[i]))return false;
    for(std::size_t j=0;j<i;++j)if(ids[i]==ids[j]&&!(i==4&&j==3))return false;
  }
  return true;
}
struct Backing {
  std::array<CatalogValueFieldView,10> views{};
  std::array<std::array<byte,8>,10> numbers{};
  std::size_t count=0;
  void Id(u16 tag,const Uuid& id,UuidKind kind){views[count++]={tag,T::engine_identity,kind,id.bytes};}
  void Number(u16 tag,u64 n){auto& value=numbers[tag-1];platform::StoreLittle64(value.data(),n);
    views[count++]={tag,T::unsigned_integer,UuidKind::unknown,value};}
  explicit Backing(const CatalogNamingDialectProfile& r){
    Id(1,r.profile_uuid,UuidKind::object);Id(2,r.database_uuid,UuidKind::database);Number(3,r.generation);
    Id(4,r.identifier_profile_uuid,UuidKind::object);Number(5,r.identifier_profile_generation);Number(6,r.allowed_dialect_mode_flags);
    Id(7,r.origin_transaction_uuid.value,UuidKind::transaction);Number(8,r.origin_local_transaction_id);
    Id(9,r.creating_transaction_uuid.value,UuidKind::transaction);Number(10,r.creating_local_transaction_id);
  }
};
bool Header(const CatalogTypedRecordView& m,const CatalogNamingDialectProfile& r){
  if(m.header.kind!=CatalogRecordKind::config_profile||m.header.object_uuid.kind!=UuidKind::object||
      m.header.object_uuid.value!=r.profile_uuid||!Typed(m.header.row_uuid,UuidKind::row))return false;
  for(const auto& id:Identities(r))if(m.header.row_uuid.value==id)return false;
  return true;
}
bool Metadata(const CatalogMetadataVersionView& m,const CatalogNamingDialectProfile& r){
  return Header(m.record,r)&&m.object_subtype==CatalogObjectSubtype::naming_dialect_profile&&m.definition_version==r.generation&&
      m.authority_scope==CatalogAuthorityScope::local&&Typed(m.owning_schema_uuid,UuidKind::schema)&&
      m.record.header.parent_uuid.kind==UuidKind::object&&m.record.header.parent_uuid.value==m.owning_schema_uuid.value&&
      Typed(m.owner_uuid,UuidKind::principal)&&Typed(m.audit_uuid,UuidKind::object)&&Typed(m.security_policy_uuid,UuidKind::object)&&
      Typed(m.default_name_uuid,UuidKind::object)&&Typed(m.name_vector_uuid,UuidKind::object)&&
      Typed(m.creator_transaction_uuid,UuidKind::transaction)&&m.creator_transaction_uuid.value==r.creating_transaction_uuid.value&&
      m.creator_local_transaction_id==r.creating_local_transaction_id;
}
bool Family(const CatalogMetadataVersionView& m){return m.object_subtype==CatalogObjectSubtype::naming_dialect_profile||IsCatalogNamingDialectProfilePayload(m.record.payload);}
}
std::optional<std::size_t> CatalogNamingDialectProfileEncodedBytes(const CatalogNamingDialectProfile& r) noexcept {
  if(!Valid(r))return {};const Backing b(r);
  return ValidateCatalogValueFields(schema,b.views)==E::none?std::optional{kCatalogNamingDialectProfileBytes}:std::nullopt;
}
CatalogValueEncodeIntoResult EncodeCatalogNamingDialectProfileInto(const CatalogNamingDialectProfile& r,std::span<byte> output) noexcept {
  if(!Valid(r))return {E::invalid_value,{}};
  const auto dest=reinterpret_cast<std::uintptr_t>(output.data()),source=reinterpret_cast<std::uintptr_t>(&r);
  if((output.size()&&!output.data())||output.size()>std::numeric_limits<std::uintptr_t>::max()-dest||
      (dest<source+sizeof(r)&&source<dest+output.size()))return {E::invalid_backing,{}};
  const Backing b(r);return EncodeCatalogValueBlockInto(schema,b.views,output);
}
CatalogNamingDialectProfileResult DecodeCatalogNamingDialectProfile(std::string_view bytes) noexcept {
  std::array<CatalogValueFieldView,10> backing;
  const auto parsed=DecodeCatalogValueBlockInto(schema,{reinterpret_cast<const byte*>(bytes.data()),bytes.size()},backing);
  if(!parsed.ok())return {parsed.error,{}};CatalogNamingDialectProfile r;
  for(const auto& f:parsed.fields)switch(f.id){
    case 1:r.profile_uuid=f.identity()->value;break;case 2:r.database_uuid=f.identity()->value;break;
    case 3:r.generation=*f.unsigned_value();break;case 4:r.identifier_profile_uuid=f.identity()->value;break;
    case 5:r.identifier_profile_generation=*f.unsigned_value();break;case 6:r.allowed_dialect_mode_flags=*f.unsigned_value();break;
    case 7:r.origin_transaction_uuid=*f.identity();break;case 8:r.origin_local_transaction_id=*f.unsigned_value();break;
    case 9:r.creating_transaction_uuid=*f.identity();break;case 10:r.creating_local_transaction_id=*f.unsigned_value();break;
  }
  if(!Valid(r))return {E::invalid_value,{}};return {E::none,r};
}
bool IsCatalogNamingDialectProfilePayload(std::string_view bytes) noexcept {
  return bytes.size()>=kCatalogValueBlockHeaderBytes&&bytes.substr(0,4)=="SBCV"&&
      platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data())+16)==kCatalogNamingDialectProfileSchemaId;
}
bool CatalogNamingDialectProfileMatchesHeader(const CatalogTypedRecordView& m) noexcept {
  const auto r=DecodeCatalogNamingDialectProfile(m.payload);return r.ok()&&Header(m,*r.record);
}
bool CatalogNamingDialectProfileMatchesMetadata(const CatalogMetadataVersionView& m) noexcept {
  const auto r=DecodeCatalogNamingDialectProfile(m.record.payload);return r.ok()&&Metadata(m,*r.record);
}
bool CatalogNamingDialectProfileMatchesDatabase(const CatalogMetadataVersionView& m,const Uuid& db) noexcept {
  if(!Family(m))return true;const auto r=DecodeCatalogNamingDialectProfile(m.record.payload);
  return Identity(db)&&r.ok()&&r.record->database_uuid==db&&Metadata(m,*r.record);
}
bool CatalogNamingDialectProfilePreservesOrigin(const CatalogMetadataVersionView& a,const CatalogMetadataVersionView& b) noexcept {
  if(!Family(a)&&!Family(b))return true;const auto x=DecodeCatalogNamingDialectProfile(a.record.payload),y=DecodeCatalogNamingDialectProfile(b.record.payload);
  return x.ok()&&y.ok()&&Metadata(a,*x.record)&&Metadata(b,*y.record)&&x.record->profile_uuid==y.record->profile_uuid&&
      x.record->database_uuid==y.record->database_uuid&&x.record->origin_transaction_uuid.value==y.record->origin_transaction_uuid.value&&
      x.record->origin_local_transaction_id==y.record->origin_local_transaction_id;
}
} // namespace scratchbird::core::catalog
