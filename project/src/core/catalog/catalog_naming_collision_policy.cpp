// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_naming_collision_policy.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <limits>

namespace scratchbird::core::catalog {
namespace {
using T=CatalogValueType;using E=CatalogValueError;
constexpr CatalogValueFieldSchema fields[]{
  {1,T::engine_identity,true,16,UuidKind::object},{2,T::engine_identity,true,16,UuidKind::database},
  {3,T::unsigned_integer,true,8},{4,T::opaque_bytes,true,1152},{5,T::opaque_bytes,true,1536},
  {6,T::engine_identity,true,16,UuidKind::transaction},{7,T::unsigned_integer,true,8},
  {8,T::engine_identity,true,16,UuidKind::transaction},{9,T::unsigned_integer,true,8}};
constexpr CatalogValueSchemaView schema{kCatalogNamingCollisionPolicySchemaId,1,fields};
bool Id(const Uuid& id){return uuid::IsEngineIdentityUuid(id);}
bool Typed(const TypedUuid& id,UuidKind kind){return id.kind==kind&&Id(id.value);}
auto Identities(const CatalogNamingCollisionPolicyBody& r){return std::array{r.body_uuid,r.database_uuid,
    r.origin_transaction_uuid.value,r.creating_transaction_uuid.value};}
bool Valid(const CatalogNamingCollisionPolicyBody& r){
  if(!r.generation||!r.class_count||r.class_count>r.classes.size()||!r.group_count||r.group_count>r.groups.size()||
      !Typed(r.origin_transaction_uuid,UuidKind::transaction)||!Typed(r.creating_transaction_uuid,UuidKind::transaction)||
      !r.origin_local_transaction_id||r.creating_local_transaction_id<r.origin_local_transaction_id||
      ((r.origin_transaction_uuid.value==r.creating_transaction_uuid.value)!=(r.origin_local_transaction_id==r.creating_local_transaction_id))||
      (r.generation==1&&r.creating_local_transaction_id!=r.origin_local_transaction_id))return false;
  const auto ids=Identities(r);
  for(std::size_t i=0;i<ids.size();++i){if(!Id(ids[i]))return false;
    for(std::size_t j=0;j<i;++j)if(ids[i]==ids[j]&&!(i==3&&j==2))return false;}
  for(std::size_t i=0;i<r.group_count;++i){const auto& group=r.groups[i];
    if(!Id(group.group_uuid)||u8(group.language_lane)<1||u8(group.language_lane)>3||u8(group.key_lane)<1||u8(group.key_lane)>2||
        (i&&!(r.groups[i-1].group_uuid<group.group_uuid)))return false;
    for(const auto& id:ids)if(group.group_uuid==id)return false;
  }
  std::array<bool,kCatalogNamingCollisionMaximumBindings> used{};
  for(std::size_t i=0;i<r.class_count;++i){const auto& binding=r.classes[i];
    if(!FindBuiltinCatalogRecordDescriptor(binding.kind)||
        (i&&u16(r.classes[i-1].kind)>=u16(binding.kind)))return false;
    const auto begin=r.groups.begin(),end=begin+r.group_count;
    const auto found=std::lower_bound(begin,end,binding.group_uuid,[](const auto& group,const auto& id){return group.group_uuid<id;});
    if(found==end||found->group_uuid!=binding.group_uuid)return false;used[found-begin]=true;
  }
  return std::all_of(used.begin(),used.begin()+r.group_count,[](bool value){return value;});
}
struct Backing {
  std::array<CatalogValueFieldView,9> views{};
  std::array<std::array<byte,8>,9> numbers{};
  std::array<byte,1152> classes{};std::array<byte,1536> groups{};
  std::size_t count=0;
  void Identity(u16 tag,const Uuid& id,UuidKind kind){views[count++]={tag,T::engine_identity,kind,id.bytes};}
  void Number(u16 tag,u64 n){auto& value=numbers[tag-1];platform::StoreLittle64(value.data(),n);
    views[count++]={tag,T::unsigned_integer,UuidKind::unknown,value};}
  explicit Backing(const CatalogNamingCollisionPolicyBody& r){
    Identity(1,r.body_uuid,UuidKind::object);Identity(2,r.database_uuid,UuidKind::database);Number(3,r.generation);
    for(std::size_t i=0;i<r.class_count;++i){auto* at=classes.data()+18*i;const auto& rule=r.classes[i];
      at[0]=u16(rule.kind);at[1]=u16(rule.kind)>>8;std::copy(rule.group_uuid.bytes.begin(),rule.group_uuid.bytes.end(),at+2);}
    for(std::size_t i=0;i<r.group_count;++i){auto* at=groups.data()+24*i;const auto& group=r.groups[i];
      std::copy(group.group_uuid.bytes.begin(),group.group_uuid.bytes.end(),at);at[16]=u8(group.language_lane);at[17]=u8(group.key_lane);}
    views[count++]={4,T::opaque_bytes,UuidKind::unknown,std::span<const byte>(classes).first(18*r.class_count)};
    views[count++]={5,T::opaque_bytes,UuidKind::unknown,std::span<const byte>(groups).first(24*r.group_count)};
    Identity(6,r.origin_transaction_uuid.value,UuidKind::transaction);Number(7,r.origin_local_transaction_id);
    Identity(8,r.creating_transaction_uuid.value,UuidKind::transaction);Number(9,r.creating_local_transaction_id);
  }
};
bool Header(const CatalogTypedRecordView& m,const CatalogNamingCollisionPolicyBody& r){
  if(m.header.kind!=CatalogRecordKind::policy||m.header.object_uuid.kind!=UuidKind::object||
      m.header.object_uuid.value!=r.body_uuid||!Typed(m.header.row_uuid,UuidKind::row))return false;
  for(const auto& id:Identities(r))if(m.header.row_uuid.value==id)return false;
  for(std::size_t i=0;i<r.group_count;++i)if(m.header.row_uuid.value==r.groups[i].group_uuid)return false;
  return true;
}
bool Metadata(const CatalogMetadataVersionView& m,const CatalogNamingCollisionPolicyBody& r){
  return Header(m.record,r)&&m.object_subtype==CatalogObjectSubtype::naming_collision_policy_body&&m.definition_version==r.generation&&
      m.authority_scope==CatalogAuthorityScope::local&&Typed(m.owning_schema_uuid,UuidKind::schema)&&
      m.record.header.parent_uuid.kind==UuidKind::object&&m.record.header.parent_uuid.value==m.owning_schema_uuid.value&&
      Typed(m.owner_uuid,UuidKind::principal)&&Typed(m.audit_uuid,UuidKind::object)&&Typed(m.security_policy_uuid,UuidKind::object)&&
      Typed(m.default_name_uuid,UuidKind::object)&&Typed(m.name_vector_uuid,UuidKind::object)&&
      Typed(m.creator_transaction_uuid,UuidKind::transaction)&&m.creator_transaction_uuid.value==r.creating_transaction_uuid.value&&
      m.creator_local_transaction_id==r.creating_local_transaction_id;
}
bool Family(const CatalogMetadataVersionView& m){return m.object_subtype==CatalogObjectSubtype::naming_collision_policy_body||IsCatalogNamingCollisionPolicyPayload(m.record.payload);}
}
std::optional<std::size_t> CatalogNamingCollisionPolicyEncodedBytes(const CatalogNamingCollisionPolicyBody& r) noexcept {
  if(!Valid(r))return {};const Backing backing(r);
  if(ValidateCatalogValueFields(schema,backing.views)!=E::none)return {};
  return 184+18*r.class_count+24*r.group_count;
}
CatalogValueEncodeIntoResult EncodeCatalogNamingCollisionPolicyInto(const CatalogNamingCollisionPolicyBody& r,std::span<byte> output) noexcept {
  if(!Valid(r))return {E::invalid_value,{}};
  const auto dest=reinterpret_cast<std::uintptr_t>(output.data()),source=reinterpret_cast<std::uintptr_t>(&r);
  if((output.size()&&!output.data())||output.size()>UINTPTR_MAX-dest||
      (dest<source+sizeof(r)&&source<dest+output.size()))return {E::invalid_backing,{}};
  const Backing backing(r);return EncodeCatalogValueBlockInto(schema,backing.views,output);
}
CatalogNamingCollisionPolicyResult DecodeCatalogNamingCollisionPolicy(std::string_view bytes) noexcept {
  std::array<CatalogValueFieldView,9> backing;
  const auto parsed=DecodeCatalogValueBlockInto(schema,{reinterpret_cast<const byte*>(bytes.data()),bytes.size()},backing);
  if(!parsed.ok())return {parsed.error,{}};CatalogNamingCollisionPolicyBody r;
  for(const auto& f:parsed.fields)switch(f.id){
    case 1:r.body_uuid=f.identity()->value;break;case 2:r.database_uuid=f.identity()->value;break;
    case 3:r.generation=*f.unsigned_value();break;
    case 4:
      if(f.bytes.empty()||f.bytes.size()%18)return {E::invalid_value,{}};r.class_count=f.bytes.size()/18;
      for(std::size_t i=0;i<r.class_count;++i){auto& binding=r.classes[i];const auto* at=f.bytes.data()+18*i;
        binding.kind=static_cast<CatalogRecordKind>(u16(at[0])|(u16(at[1])<<8));std::copy_n(at+2,16,binding.group_uuid.bytes.begin());}
      break;
    case 5:
      if(f.bytes.empty()||f.bytes.size()%24)return {E::invalid_value,{}};r.group_count=f.bytes.size()/24;
      for(std::size_t i=0;i<r.group_count;++i){auto& group=r.groups[i];const auto* at=f.bytes.data()+24*i;
        std::copy_n(at,16,group.group_uuid.bytes.begin());group.language_lane=static_cast<CatalogCollisionLanguageLane>(at[16]);
        group.key_lane=static_cast<CatalogCollisionKeyLane>(at[17]);
        for(unsigned n=18;n<24;++n)if(at[n])return {E::invalid_value,{}};}
      break;
    case 6:r.origin_transaction_uuid=*f.identity();break;case 7:r.origin_local_transaction_id=*f.unsigned_value();break;
    case 8:r.creating_transaction_uuid=*f.identity();break;case 9:r.creating_local_transaction_id=*f.unsigned_value();break;
  }
  if(!Valid(r))return {E::invalid_value,{}};return {E::none,r};
}
bool IsCatalogNamingCollisionPolicyPayload(std::string_view bytes) noexcept {
  return bytes.size()>=kCatalogValueBlockHeaderBytes&&bytes.substr(0,4)=="SBCV"&&
      platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data())+16)==kCatalogNamingCollisionPolicySchemaId;
}
bool CatalogNamingCollisionPolicyMatchesHeader(const CatalogTypedRecordView& m) noexcept {
  const auto r=DecodeCatalogNamingCollisionPolicy(m.payload);return r.ok()&&Header(m,*r.record);
}
bool CatalogNamingCollisionPolicyMatchesMetadata(const CatalogMetadataVersionView& m) noexcept {
  const auto r=DecodeCatalogNamingCollisionPolicy(m.record.payload);return r.ok()&&Metadata(m,*r.record);
}
bool CatalogNamingCollisionPolicyMatchesDatabase(const CatalogMetadataVersionView& m,const Uuid& db) noexcept {
  if(!Family(m))return true;const auto r=DecodeCatalogNamingCollisionPolicy(m.record.payload);
  return Id(db)&&r.ok()&&r.record->database_uuid==db&&Metadata(m,*r.record);
}
bool CatalogNamingCollisionPolicyPreservesOrigin(const CatalogMetadataVersionView& a,const CatalogMetadataVersionView& b) noexcept {
  if(!Family(a)&&!Family(b))return true;const auto x=DecodeCatalogNamingCollisionPolicy(a.record.payload),y=DecodeCatalogNamingCollisionPolicy(b.record.payload);
  return x.ok()&&y.ok()&&Metadata(a,*x.record)&&Metadata(b,*y.record)&&x.record->body_uuid==y.record->body_uuid&&
      x.record->database_uuid==y.record->database_uuid&&x.record->origin_transaction_uuid.value==y.record->origin_transaction_uuid.value&&
      x.record->origin_local_transaction_id==y.record->origin_local_transaction_id;
}
} // namespace scratchbird::core::catalog
