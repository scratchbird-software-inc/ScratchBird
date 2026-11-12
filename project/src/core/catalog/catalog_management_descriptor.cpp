// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_management_descriptor.hpp"
#include "uuid.hpp"
#include <limits>

namespace scratchbird::core::catalog {
namespace {
using E=CatalogValueError;using T=CatalogValueType;
constexpr CatalogValueFieldSchema profile_fields[]{
  {1,T::engine_identity,true,16,UuidKind::object},{2,T::engine_identity,true,16,UuidKind::database},
  {3,T::unsigned_integer,true,8},{4,T::unsigned_integer,true,8},
  {5,T::engine_identity,true,16,UuidKind::transaction},{6,T::unsigned_integer,true,8},
  {7,T::engine_identity,true,16,UuidKind::transaction},{8,T::unsigned_integer,true,8}};
constexpr CatalogValueFieldSchema descriptor_fields[]{
  profile_fields[0],profile_fields[1],profile_fields[2],profile_fields[3],
  {5,T::engine_identity,true,16,UuidKind::object},{6,T::engine_identity,true,16,UuidKind::object},
  {7,T::engine_identity,true,16,UuidKind::object},{8,T::engine_identity,true,16,UuidKind::object},
  {9,T::engine_identity,true,16,UuidKind::object},{10,T::engine_identity,true,16,UuidKind::object},
  {11,T::engine_identity,true,16,UuidKind::object},{12,T::engine_identity,true,16,UuidKind::object},
  {13,T::engine_identity,true,16,UuidKind::object},{14,T::engine_identity,true,16,UuidKind::object},
  {15,T::unsigned_integer,true,8},{16,T::engine_identity,true,16,UuidKind::transaction},
  {17,T::unsigned_integer,true,8},{18,T::engine_identity,true,16,UuidKind::transaction},
  {19,T::unsigned_integer,true,8}};
constexpr CatalogValueSchemaView profile_schema{kCatalogManagementProfileSchemaId,1,profile_fields};
constexpr CatalogValueSchemaView descriptor_schema{kCatalogManagementDescriptorSchemaId,1,descriptor_fields};
bool Id(const Uuid& id){return uuid::IsEngineIdentityUuid(id);}
bool Typed(const TypedUuid& id,UuidKind kind){return id.kind==kind&&Id(id.value);}
template<class R>bool Origin(const R& r,const Uuid& object){
  if(!Id(object)||!Id(r.database_uuid)||!r.generation||
      !Typed(r.origin_transaction_uuid,UuidKind::transaction)||!Typed(r.creating_transaction_uuid,UuidKind::transaction)||
      !r.origin_local_transaction_id||r.creating_local_transaction_id<r.origin_local_transaction_id)return false;
  if((r.origin_transaction_uuid.value==r.creating_transaction_uuid.value)!=
      (r.origin_local_transaction_id==r.creating_local_transaction_id))return false;
  if(r.generation==1&&r.origin_local_transaction_id!=r.creating_local_transaction_id)return false;
  return object!=r.database_uuid&&object!=r.origin_transaction_uuid.value&&object!=r.creating_transaction_uuid.value&&
    r.database_uuid!=r.origin_transaction_uuid.value&&r.database_uuid!=r.creating_transaction_uuid.value;
}
bool Valid(const CatalogManagementProfile& r){
  return Origin(r,r.profile_uuid)&&u64(r.code)>=1&&u64(r.code)<=10;
}
bool Valid(const CatalogManagementOperationDescriptor& r){
  if(!Origin(r,r.descriptor_uuid)||r.code!=CatalogManagementOperationCode::verify_start||
      (r.flags&~u64{31})||(r.flags&3)!=3)return false;
  for(std::size_t i=0;i<r.profiles.size();++i){const auto& id=r.profiles[i];
    if(!Id(id)||id==r.descriptor_uuid||id==r.database_uuid||id==r.origin_transaction_uuid.value||id==r.creating_transaction_uuid.value)return false;
    for(std::size_t j=0;j<i;++j)if(id==r.profiles[j])return false;
  }
  return true;
}
struct Backing {
  std::array<CatalogValueFieldView,19> fields{};
  std::array<std::array<byte,8>,19> numbers{};
  std::size_t count=0;
  void Identity(u16 tag,const Uuid& id,UuidKind kind=UuidKind::object){fields[count++]={tag,T::engine_identity,kind,id.bytes};}
  void Number(u16 tag,u64 value){auto& bytes=numbers[tag-1];platform::StoreLittle64(bytes.data(),value);fields[count++]={tag,T::unsigned_integer,UuidKind::unknown,bytes};}
  template<class R>void Tail(u16 tag,const R& r){
    Identity(tag,r.origin_transaction_uuid.value,UuidKind::transaction);Number(tag+1,r.origin_local_transaction_id);
    Identity(tag+2,r.creating_transaction_uuid.value,UuidKind::transaction);Number(tag+3,r.creating_local_transaction_id);
  }
  explicit Backing(const CatalogManagementProfile& r){
    Identity(1,r.profile_uuid);Identity(2,r.database_uuid,UuidKind::database);Number(3,r.generation);Number(4,u64(r.code));Tail(5,r);
  }
  explicit Backing(const CatalogManagementOperationDescriptor& r){
    Identity(1,r.descriptor_uuid);Identity(2,r.database_uuid,UuidKind::database);Number(3,r.generation);Number(4,u64(r.code));
    for(u16 i=0;i<r.profiles.size();++i)Identity(5+i,r.profiles[i]);Number(15,r.flags);Tail(16,r);
  }
  std::span<const CatalogValueFieldView> View()const{return std::span{fields}.first(count);}
};
template<class R>std::optional<std::size_t> Size(const R& r,CatalogValueSchemaView schema){
  if(!Valid(r))return {};const Backing b(r);if(ValidateCatalogValueFields(schema,b.View())!=E::none)return {};
  std::size_t bytes=kCatalogValueBlockHeaderBytes;for(const auto& field:b.View())bytes+=8+field.bytes.size();return bytes;
}
template<class R>CatalogValueEncodeIntoResult Encode(const R& r,CatalogValueSchemaView schema,std::span<byte> out){
  if(!Valid(r))return {E::invalid_value,{}};
  const auto dest=reinterpret_cast<std::uintptr_t>(out.data()),input=reinterpret_cast<std::uintptr_t>(&r);
  if((out.size()&&!out.data())||out.size()>std::numeric_limits<std::uintptr_t>::max()-dest||
      (dest<input+sizeof(r)&&input<dest+out.size()))return {E::invalid_backing,{}};
  const Backing b(r);return EncodeCatalogValueBlockInto(schema,b.View(),out);
}
template<class R>void DecodeOrigin(R& r,u16 tag,std::span<const CatalogValueFieldView> fields){
  r.origin_transaction_uuid=*fields[tag-1].identity();r.origin_local_transaction_id=*fields[tag].unsigned_value();
  r.creating_transaction_uuid=*fields[tag+1].identity();r.creating_local_transaction_id=*fields[tag+2].unsigned_value();
}
bool Payload(std::string_view b,u32 schema){return b.size()>=kCatalogValueBlockHeaderBytes&&b.substr(0,4)=="SBCV"&&
    platform::LoadLittle32(reinterpret_cast<const byte*>(b.data())+16)==schema;}
template<class R>bool Header(const CatalogTypedRecordView& m,const R& r,const Uuid& object){
  return m.header.kind==CatalogRecordKind::management_operation&&Typed(m.header.object_uuid,UuidKind::object)&&
      m.header.object_uuid.value==object&&Typed(m.header.row_uuid,UuidKind::row)&&
      m.header.row_uuid.value!=object&&m.header.row_uuid.value!=r.database_uuid&&
      m.header.row_uuid.value!=r.origin_transaction_uuid.value&&m.header.row_uuid.value!=r.creating_transaction_uuid.value;
}
bool Owner(const TypedUuid& owner){
  switch(owner.kind){case UuidKind::principal:case UuidKind::object:case UuidKind::schema:
    case UuidKind::database:case UuidKind::cluster:return Id(owner.value);default:return false;}
}
template<class R>bool Metadata(const CatalogMetadataVersionView& m,const R& r,const Uuid& object,scratchbird::core::catalog::CatalogObjectSubtype subtype){
  return Header(m.record,r,object)&&
      m.object_subtype==subtype&&m.definition_version==r.generation&&m.authority_scope==CatalogAuthorityScope::local&&
      Typed(m.owning_schema_uuid,UuidKind::schema)&&Typed(m.record.header.parent_uuid,UuidKind::object)&&
      m.record.header.parent_uuid.value==m.owning_schema_uuid.value&&
      Owner(m.owner_uuid)&&Typed(m.audit_uuid,UuidKind::object)&&
      Typed(m.default_name_uuid,UuidKind::object)&&Typed(m.name_vector_uuid,UuidKind::object)&&
      Typed(m.creator_transaction_uuid,UuidKind::transaction)&&m.creator_transaction_uuid.value==r.creating_transaction_uuid.value&&
      m.creator_local_transaction_id==r.creating_local_transaction_id;
}
bool ProfileFamily(const CatalogMetadataVersionView& m){return m.object_subtype==CatalogObjectSubtype::management_profile||IsCatalogManagementProfilePayload(m.record.payload);}
bool DescriptorFamily(const CatalogMetadataVersionView& m){return m.object_subtype==CatalogObjectSubtype::management_descriptor||IsCatalogManagementDescriptorPayload(m.record.payload);}
template<class R>bool Preserves(const R& a,const R& b,std::string_view before,std::string_view after){
  return a.database_uuid==b.database_uuid&&a.code==b.code&&a.origin_transaction_uuid.value==b.origin_transaction_uuid.value&&
      a.origin_local_transaction_id==b.origin_local_transaction_id&&
      (b.generation>a.generation||(b.generation==a.generation&&before==after));
}
}
std::optional<std::size_t> CatalogManagementProfileEncodedBytes(const CatalogManagementProfile& r){return Size(r,profile_schema);}
std::optional<std::size_t> CatalogManagementDescriptorEncodedBytes(const CatalogManagementOperationDescriptor& r){return Size(r,descriptor_schema);}
CatalogValueEncodeIntoResult EncodeCatalogManagementProfileInto(const CatalogManagementProfile& r,std::span<byte> out){return Encode(r,profile_schema,out);}
CatalogValueEncodeIntoResult EncodeCatalogManagementDescriptorInto(const CatalogManagementOperationDescriptor& r,std::span<byte> out){return Encode(r,descriptor_schema,out);}
CatalogManagementProfileResult DecodeCatalogManagementProfile(std::string_view bytes){
  std::array<CatalogValueFieldView,8> backing;
  const auto d=DecodeCatalogValueBlockInto(profile_schema,{reinterpret_cast<const byte*>(bytes.data()),bytes.size()},backing);
  if(!d.ok())return {d.error,{}};
  CatalogManagementProfile r;r.profile_uuid=backing[0].identity()->value;r.database_uuid=backing[1].identity()->value;
  r.generation=*backing[2].unsigned_value();r.code=CatalogManagementProfileCode(*backing[3].unsigned_value());DecodeOrigin(r,5,d.fields);
  return Valid(r)?CatalogManagementProfileResult{E::none,r}:CatalogManagementProfileResult{E::invalid_value,{}};
}
CatalogManagementDescriptorResult DecodeCatalogManagementDescriptor(std::string_view bytes){
  std::array<CatalogValueFieldView,19> backing;
  const auto d=DecodeCatalogValueBlockInto(descriptor_schema,{reinterpret_cast<const byte*>(bytes.data()),bytes.size()},backing);
  if(!d.ok())return {d.error,{}};
  CatalogManagementOperationDescriptor r;r.descriptor_uuid=backing[0].identity()->value;r.database_uuid=backing[1].identity()->value;
  r.generation=*backing[2].unsigned_value();r.code=CatalogManagementOperationCode(*backing[3].unsigned_value());
  for(std::size_t i=0;i<r.profiles.size();++i)r.profiles[i]=backing[4+i].identity()->value;
  r.flags=*backing[14].unsigned_value();DecodeOrigin(r,16,d.fields);
  return Valid(r)?CatalogManagementDescriptorResult{E::none,r}:CatalogManagementDescriptorResult{E::invalid_value,{}};
}
bool IsCatalogManagementProfilePayload(std::string_view bytes){return Payload(bytes,kCatalogManagementProfileSchemaId);}
bool IsCatalogManagementDescriptorPayload(std::string_view bytes){return Payload(bytes,kCatalogManagementDescriptorSchemaId);}
bool CatalogManagementProfileMatchesHeader(const CatalogTypedRecordView& m){
  const auto d=DecodeCatalogManagementProfile(m.payload);return d.ok()&&Header(m,*d.record,d.record->profile_uuid);
}
bool CatalogManagementDescriptorMatchesHeader(const CatalogTypedRecordView& m){
  const auto d=DecodeCatalogManagementDescriptor(m.payload);return d.ok()&&Header(m,*d.record,d.record->descriptor_uuid);
}
bool CatalogManagementProfileMatchesMetadata(const CatalogMetadataVersionView& m){
  const auto d=DecodeCatalogManagementProfile(m.record.payload);return d.ok()&&Metadata(m,*d.record,d.record->profile_uuid,CatalogObjectSubtype::management_profile);
}
bool CatalogManagementDescriptorMatchesMetadata(const CatalogMetadataVersionView& m){
  const auto d=DecodeCatalogManagementDescriptor(m.record.payload);return d.ok()&&Metadata(m,*d.record,d.record->descriptor_uuid,CatalogObjectSubtype::management_descriptor);
}
bool CatalogManagementDefinitionMatchesDatabase(const CatalogMetadataVersionView& m,const Uuid& database){
  if(ProfileFamily(m)){const auto r=DecodeCatalogManagementProfile(m.record.payload);
    return Id(database)&&r.ok()&&r.record->database_uuid==database&&CatalogManagementProfileMatchesMetadata(m);}
  if(DescriptorFamily(m)){const auto r=DecodeCatalogManagementDescriptor(m.record.payload);
    return Id(database)&&r.ok()&&r.record->database_uuid==database&&CatalogManagementDescriptorMatchesMetadata(m);}
  return true;
}
bool CatalogManagementDefinitionPreservesOrigin(const CatalogMetadataVersionView& a,const CatalogMetadataVersionView& b){
  if(ProfileFamily(a)||ProfileFamily(b)){
    const auto x=DecodeCatalogManagementProfile(a.record.payload),y=DecodeCatalogManagementProfile(b.record.payload);
    return x.ok()&&y.ok()&&CatalogManagementProfileMatchesMetadata(a)&&CatalogManagementProfileMatchesMetadata(b)&&
      x.record->profile_uuid==y.record->profile_uuid&&Preserves(*x.record,*y.record,a.record.payload,b.record.payload);
  }
  if(DescriptorFamily(a)||DescriptorFamily(b)){
    const auto x=DecodeCatalogManagementDescriptor(a.record.payload),y=DecodeCatalogManagementDescriptor(b.record.payload);
    return x.ok()&&y.ok()&&CatalogManagementDescriptorMatchesMetadata(a)&&CatalogManagementDescriptorMatchesMetadata(b)&&
      x.record->descriptor_uuid==y.record->descriptor_uuid&&Preserves(*x.record,*y.record,a.record.payload,b.record.payload);
  }
  return true;
}
} // namespace scratchbird::core::catalog
