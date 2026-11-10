// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_native_catalog_index.hpp"
#include "uuid.hpp"
#include <array>
#include <limits>

namespace scratchbird::core::catalog {
namespace {
using T=CatalogValueType;using E=CatalogValueError;
using Key=CatalogNativeCatalogIndexKeyProfile;using Visibility=CatalogNativeCatalogIndexVisibilityProfile;
using Map=CatalogNativeCatalogIndexDependencyMap;
constexpr CatalogValueFieldSchema Id(u16 id,UuidKind kind){return {id,T::engine_identity,true,16,kind};}
constexpr CatalogValueFieldSchema Number(u16 id){return {id,T::unsigned_integer,true,8};}
constexpr auto O=UuidKind::object;constexpr auto D=UuidKind::database;constexpr auto X=UuidKind::transaction;
constexpr std::array key_fields{Id(1,O),Id(2,D),Number(3),Number(4),Number(5),Number(6),Id(7,X),Number(8),Id(9,X),Number(10)};
constexpr std::array visibility_fields{Id(1,O),Id(2,D),Number(3),Number(4),Id(5,X),Number(6),Id(7,X),Number(8)};
constexpr std::array map_fields{Id(1,O),Id(2,D),Number(3),Number(4),Id(5,O),Id(6,O),Id(7,O),Id(8,O),Id(9,O),
    Number(10),Number(11),Id(12,X),Number(13),Id(14,X),Number(15)};
constexpr CatalogValueSchemaView key_schema{kCatalogNativeCatalogIndexKeyProfileSchemaId,1,key_fields};
constexpr CatalogValueSchemaView visibility_schema{kCatalogNativeCatalogIndexVisibilityProfileSchemaId,1,visibility_fields};
constexpr CatalogValueSchemaView map_schema{kCatalogNativeCatalogIndexDependencyMapSchemaId,1,map_fields};
constexpr std::string_view kKeySubtype="native_catalog_index_key_profile";
constexpr std::string_view kVisibilitySubtype="native_catalog_index_visibility_profile";
constexpr std::string_view kMapSubtype="native_catalog_index_dependency_map";

bool Identity(const Uuid& id){return uuid::IsEngineIdentityUuid(id);}
bool Transaction(const TypedUuid& id){return id.kind==UuidKind::transaction&&Identity(id.value);}
template<class R> bool Provenance(const R& r) noexcept {
  return r.generation&&Transaction(r.origin_transaction_uuid)&&Transaction(r.creating_transaction_uuid)&&
      r.origin_local_transaction_id&&r.creating_local_transaction_id>=r.origin_local_transaction_id&&
      ((r.origin_transaction_uuid.value==r.creating_transaction_uuid.value)==
          (r.origin_local_transaction_id==r.creating_local_transaction_id))&&
      (r.generation!=1||r.creating_local_transaction_id==r.origin_local_transaction_id);
}
// Every control identity is a real engine identity and pairwise distinct; the
// last two are the origin and creating transactions, which may be equal.
template<std::size_t N> bool Distinct(const std::array<Uuid,N>& control) noexcept {
  for(std::size_t i=0;i<N;++i){
    if(!Identity(control[i]))return false;
    for(std::size_t j=0;j<i;++j)if(control[i]==control[j]&&!(i==N-1&&j==N-2))return false;
  }
  return true;
}
auto Controls(const Key& r){return std::array{r.profile_uuid,r.database_uuid,r.origin_transaction_uuid.value,r.creating_transaction_uuid.value};}
auto Controls(const Visibility& r){return std::array{r.profile_uuid,r.database_uuid,r.origin_transaction_uuid.value,r.creating_transaction_uuid.value};}
auto Controls(const Map& r){return std::array{r.map_uuid,r.database_uuid,r.relation_uuid,r.index_uuid,r.index_family_uuid,
    r.key_profile_uuid,r.visibility_profile_uuid,r.origin_transaction_uuid.value,r.creating_transaction_uuid.value};}
bool Valid(const Key& r) noexcept {
  return Provenance(r)&&Distinct(Controls(r))&&
      (r.key_form==NativeCatalogIndexKeyForm::raw16_canonical_row_uuid||
       r.key_form==NativeCatalogIndexKeyForm::name_authoritative_v1||r.key_form==NativeCatalogIndexKeyForm::name_uuid_v1)&&
      r.key_order==NativeCatalogIndexKeyOrder::unsigned_canonical_bytes&&
      r.tie_break==NativeCatalogIndexTieBreak::native_row_then_version_uuid;
}
bool Valid(const Visibility& r) noexcept {
  return Provenance(r)&&Distinct(Controls(r))&&r.visibility==NativeCatalogIndexVisibility::native_mga_inventory_creator;
}
// Catalog relation indexes are native B-trees only.
bool Valid(const Map& r) noexcept {
  return Provenance(r)&&Distinct(Controls(r))&&r.relation_role>=1&&r.relation_role<=6&&
      r.index_family_uuid==kNativeBtreeIndexFamilyUuid&&r.descriptor_generation&&r.storage_generation;
}
template<std::size_t N> struct Backing {
  std::array<CatalogValueFieldView,N> views{};
  std::array<std::array<byte,8>,N> numbers{};
  std::size_t count=0;
  void Id(u16 tag,const Uuid& id,UuidKind kind){views[count++]={tag,T::engine_identity,kind,id.bytes};}
  void Number(u16 tag,u64 n){auto& bytes=numbers[tag-1];platform::StoreLittle64(bytes.data(),n);
    views[count++]={tag,T::unsigned_integer,UuidKind::unknown,bytes};}
  auto View()const noexcept{return std::span<const CatalogValueFieldView>{views}.first(count);}
};
Backing<10> Fields(const Key& r){Backing<10> b;
  b.Id(1,r.profile_uuid,O);b.Id(2,r.database_uuid,D);b.Number(3,r.generation);b.Number(4,u64(r.key_form));
  b.Number(5,u64(r.key_order));b.Number(6,u64(r.tie_break));b.Id(7,r.origin_transaction_uuid.value,X);
  b.Number(8,r.origin_local_transaction_id);b.Id(9,r.creating_transaction_uuid.value,X);b.Number(10,r.creating_local_transaction_id);
  return b;}
Backing<8> Fields(const Visibility& r){Backing<8> b;
  b.Id(1,r.profile_uuid,O);b.Id(2,r.database_uuid,D);b.Number(3,r.generation);b.Number(4,u64(r.visibility));
  b.Id(5,r.origin_transaction_uuid.value,X);b.Number(6,r.origin_local_transaction_id);
  b.Id(7,r.creating_transaction_uuid.value,X);b.Number(8,r.creating_local_transaction_id);
  return b;}
Backing<15> Fields(const Map& r){Backing<15> b;
  b.Id(1,r.map_uuid,O);b.Id(2,r.database_uuid,D);b.Number(3,r.generation);b.Number(4,r.relation_role);
  b.Id(5,r.relation_uuid,O);b.Id(6,r.index_uuid,O);b.Id(7,r.index_family_uuid,O);b.Id(8,r.key_profile_uuid,O);
  b.Id(9,r.visibility_profile_uuid,O);b.Number(10,r.descriptor_generation);b.Number(11,r.storage_generation);
  b.Id(12,r.origin_transaction_uuid.value,X);b.Number(13,r.origin_local_transaction_id);
  b.Id(14,r.creating_transaction_uuid.value,X);b.Number(15,r.creating_local_transaction_id);
  return b;}
CatalogValueSchemaView Schema(const Key&){return key_schema;}
CatalogValueSchemaView Schema(const Visibility&){return visibility_schema;}
CatalogValueSchemaView Schema(const Map&){return map_schema;}
template<class R> std::optional<std::size_t> Size(const R& r) noexcept {
  if(!Valid(r))return {};const auto b=Fields(r);if(ValidateCatalogValueFields(Schema(r),b.View())!=E::none)return {};
  std::size_t bytes=kCatalogValueBlockHeaderBytes;for(const auto& field:b.View())bytes+=8+field.bytes.size();return bytes;
}
template<class R> CatalogValueEncodeIntoResult Into(const R& r,std::span<byte> output) noexcept {
  if(!Valid(r))return {E::invalid_value,{}};
  const auto dest=reinterpret_cast<std::uintptr_t>(output.data()),source=reinterpret_cast<std::uintptr_t>(&r);
  if((output.size()&&!output.data())||output.size()>std::numeric_limits<std::uintptr_t>::max()-dest||
      (dest<source+sizeof(r)&&source<dest+output.size()))return {E::invalid_backing,{}};
  const auto b=Fields(r);return EncodeCatalogValueBlockInto(Schema(r),b.View(),output);
}
template<class R,std::size_t Bytes> CatalogValueEncodeResult Owning(const R& r) {
  std::array<byte,Bytes> backing;const auto encoded=Into(r,backing);
  if(!encoded.ok())return {encoded.error,{}};
  return {E::none,{encoded.bytes.begin(),encoded.bytes.end()}};
}
template<std::size_t N> CatalogValueDecodeViewResult Parse(CatalogValueSchemaView schema,std::string_view bytes,
    std::array<CatalogValueFieldView,N>& backing) noexcept {
  return DecodeCatalogValueBlockInto(schema,{reinterpret_cast<const byte*>(bytes.data()),bytes.size()},backing);
}
u32 SchemaOf(std::string_view bytes) noexcept {
  if(bytes.size()<kCatalogValueBlockHeaderBytes||bytes.substr(0,4)!="SBCV")return 0;
  return platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data())+16);
}
// The record's own identity, its creator and its subtype.
struct Binding {
  Uuid object;std::string_view subtype;u64 generation=0;TypedUuid creating;u64 creating_local=0;
};
Binding Bind(const Key& r){return {r.profile_uuid,kKeySubtype,r.generation,r.creating_transaction_uuid,r.creating_local_transaction_id};}
Binding Bind(const Visibility& r){return {r.profile_uuid,kVisibilitySubtype,r.generation,r.creating_transaction_uuid,r.creating_local_transaction_id};}
Binding Bind(const Map& r){return {r.map_uuid,kMapSubtype,r.generation,r.creating_transaction_uuid,r.creating_local_transaction_id};}
template<class R> bool Header(const CatalogTypedRecordView& m,const R& r) {
  if(m.header.kind!=CatalogRecordKind::config_profile||m.header.object_uuid.kind!=UuidKind::object||
      m.header.object_uuid.value!=Bind(r).object||m.header.row_uuid.kind!=UuidKind::row||!Identity(m.header.row_uuid.value))return false;
  for(const auto& id:Controls(r))if(m.header.row_uuid.value==id)return false;
  return true;
}
template<class R> bool Metadata(const CatalogMetadataVersionView& m,const R& r) {
  const auto b=Bind(r);
  return Header(m.record,r)&&m.object_subtype==b.subtype&&m.definition_version==b.generation&&
      m.creator_transaction_uuid.kind==UuidKind::transaction&&m.creator_transaction_uuid.value==b.creating.value&&
      m.creator_local_transaction_id==b.creating_local&&m.authority_scope==CatalogAuthorityScope::local;
}
enum class Family { none,key,visibility,map };
Family FamilyOf(const CatalogMetadataVersionView& m) {
  if(m.object_subtype==kKeySubtype)return Family::key;
  if(m.object_subtype==kVisibilitySubtype)return Family::visibility;
  if(m.object_subtype==kMapSubtype)return Family::map;
  switch(SchemaOf(m.record.payload)){
    case kCatalogNativeCatalogIndexKeyProfileSchemaId:return Family::key;
    case kCatalogNativeCatalogIndexVisibilityProfileSchemaId:return Family::visibility;
    case kCatalogNativeCatalogIndexDependencyMapSchemaId:return Family::map;
  }
  return Family::none;
}
// Decodes the payload as the given family and applies `check` to the record.
template<class Check> bool WithRecord(Family family,std::string_view payload,Check&& check) {
  switch(family){
    case Family::key:{const auto r=DecodeCatalogNativeCatalogIndexKeyProfile(payload);return r.ok()&&check(*r.record);}
    case Family::visibility:{const auto r=DecodeCatalogNativeCatalogIndexVisibilityProfile(payload);return r.ok()&&check(*r.record);}
    case Family::map:{const auto r=DecodeCatalogNativeCatalogIndexDependencyMap(payload);return r.ok()&&check(*r.record);}
    case Family::none:break;
  }
  return false;
}
Uuid DatabaseOf(const Key& r){return r.database_uuid;}
Uuid DatabaseOf(const Visibility& r){return r.database_uuid;}
Uuid DatabaseOf(const Map& r){return r.database_uuid;}
} // namespace

std::optional<std::size_t> CatalogNativeCatalogIndexKeyProfileEncodedBytes(const Key& r) noexcept {return Size(r);}
CatalogValueEncodeIntoResult EncodeCatalogNativeCatalogIndexKeyProfileInto(const Key& r,std::span<byte> out) noexcept {return Into(r,out);}
CatalogValueEncodeResult EncodeCatalogNativeCatalogIndexKeyProfile(const Key& r) {
  return Owning<Key,kCatalogNativeCatalogIndexKeyProfileBytes>(r);
}
CatalogNativeCatalogIndexResult<Key> DecodeCatalogNativeCatalogIndexKeyProfile(std::string_view bytes) noexcept {
  std::array<CatalogValueFieldView,10> backing;const auto parsed=Parse(key_schema,bytes,backing);
  if(!parsed.ok())return {parsed.error,{}};Key r;
  for(const auto& f:parsed.fields)switch(f.id){
    case 1:r.profile_uuid=f.identity()->value;break;case 2:r.database_uuid=f.identity()->value;break;
    case 3:r.generation=*f.unsigned_value();break;case 4:r.key_form=NativeCatalogIndexKeyForm(*f.unsigned_value());break;
    case 5:r.key_order=NativeCatalogIndexKeyOrder(*f.unsigned_value());break;
    case 6:r.tie_break=NativeCatalogIndexTieBreak(*f.unsigned_value());break;
    case 7:r.origin_transaction_uuid=*f.identity();break;case 8:r.origin_local_transaction_id=*f.unsigned_value();break;
    case 9:r.creating_transaction_uuid=*f.identity();break;case 10:r.creating_local_transaction_id=*f.unsigned_value();break;
  }
  if(!Valid(r))return {E::invalid_value,{}};return {E::none,r};
}
std::optional<std::size_t> CatalogNativeCatalogIndexVisibilityProfileEncodedBytes(const Visibility& r) noexcept {return Size(r);}
CatalogValueEncodeIntoResult EncodeCatalogNativeCatalogIndexVisibilityProfileInto(const Visibility& r,std::span<byte> out) noexcept {return Into(r,out);}
CatalogValueEncodeResult EncodeCatalogNativeCatalogIndexVisibilityProfile(const Visibility& r) {
  return Owning<Visibility,kCatalogNativeCatalogIndexVisibilityProfileBytes>(r);
}
CatalogNativeCatalogIndexResult<Visibility> DecodeCatalogNativeCatalogIndexVisibilityProfile(std::string_view bytes) noexcept {
  std::array<CatalogValueFieldView,8> backing;const auto parsed=Parse(visibility_schema,bytes,backing);
  if(!parsed.ok())return {parsed.error,{}};Visibility r;
  for(const auto& f:parsed.fields)switch(f.id){
    case 1:r.profile_uuid=f.identity()->value;break;case 2:r.database_uuid=f.identity()->value;break;
    case 3:r.generation=*f.unsigned_value();break;case 4:r.visibility=NativeCatalogIndexVisibility(*f.unsigned_value());break;
    case 5:r.origin_transaction_uuid=*f.identity();break;case 6:r.origin_local_transaction_id=*f.unsigned_value();break;
    case 7:r.creating_transaction_uuid=*f.identity();break;case 8:r.creating_local_transaction_id=*f.unsigned_value();break;
  }
  if(!Valid(r))return {E::invalid_value,{}};return {E::none,r};
}
std::optional<std::size_t> CatalogNativeCatalogIndexDependencyMapEncodedBytes(const Map& r) noexcept {return Size(r);}
CatalogValueEncodeIntoResult EncodeCatalogNativeCatalogIndexDependencyMapInto(const Map& r,std::span<byte> out) noexcept {return Into(r,out);}
CatalogValueEncodeResult EncodeCatalogNativeCatalogIndexDependencyMap(const Map& r) {
  return Owning<Map,kCatalogNativeCatalogIndexDependencyMapBytes>(r);
}
CatalogNativeCatalogIndexResult<Map> DecodeCatalogNativeCatalogIndexDependencyMap(std::string_view bytes) noexcept {
  std::array<CatalogValueFieldView,15> backing;const auto parsed=Parse(map_schema,bytes,backing);
  if(!parsed.ok())return {parsed.error,{}};Map r;
  for(const auto& f:parsed.fields)switch(f.id){
    case 1:r.map_uuid=f.identity()->value;break;case 2:r.database_uuid=f.identity()->value;break;
    case 3:r.generation=*f.unsigned_value();break;case 4:r.relation_role=*f.unsigned_value();break;
    case 5:r.relation_uuid=f.identity()->value;break;case 6:r.index_uuid=f.identity()->value;break;
    case 7:r.index_family_uuid=f.identity()->value;break;case 8:r.key_profile_uuid=f.identity()->value;break;
    case 9:r.visibility_profile_uuid=f.identity()->value;break;case 10:r.descriptor_generation=*f.unsigned_value();break;
    case 11:r.storage_generation=*f.unsigned_value();break;case 12:r.origin_transaction_uuid=*f.identity();break;
    case 13:r.origin_local_transaction_id=*f.unsigned_value();break;case 14:r.creating_transaction_uuid=*f.identity();break;
    case 15:r.creating_local_transaction_id=*f.unsigned_value();break;
  }
  if(!Valid(r))return {E::invalid_value,{}};return {E::none,r};
}
bool IsCatalogNativeCatalogIndexPayload(std::string_view bytes) noexcept {
  const auto id=SchemaOf(bytes);
  return id==kCatalogNativeCatalogIndexKeyProfileSchemaId||id==kCatalogNativeCatalogIndexVisibilityProfileSchemaId||
      id==kCatalogNativeCatalogIndexDependencyMapSchemaId;
}
bool CatalogNativeCatalogIndexMatchesHeader(const CatalogTypedRecordView& m) noexcept {
  Family family=Family::none;
  switch(SchemaOf(m.payload)){
    case kCatalogNativeCatalogIndexKeyProfileSchemaId:family=Family::key;break;
    case kCatalogNativeCatalogIndexVisibilityProfileSchemaId:family=Family::visibility;break;
    case kCatalogNativeCatalogIndexDependencyMapSchemaId:family=Family::map;break;
  }
  return WithRecord(family,m.payload,[&](const auto& r){return Header(m,r);});
}
bool CatalogNativeCatalogIndexMatchesMetadata(const CatalogMetadataVersionView& m) noexcept {
  return WithRecord(FamilyOf(m),m.record.payload,[&](const auto& r){return Metadata(m,r);});
}
bool CatalogNativeCatalogIndexMatchesDatabase(const CatalogMetadataVersionView& m,const Uuid& db) noexcept {
  const auto family=FamilyOf(m);if(family==Family::none)return true;
  return Identity(db)&&WithRecord(family,m.record.payload,[&](const auto& r){return Metadata(m,r)&&DatabaseOf(r)==db;});
}
bool CatalogNativeCatalogIndexPreservesOrigin(const CatalogMetadataVersionView& a,const CatalogMetadataVersionView& b) noexcept {
  const auto x=FamilyOf(a),y=FamilyOf(b);
  if(x==Family::none&&y==Family::none)return true;
  if(x!=y)return false;
  return WithRecord(x,a.record.payload,[&](const auto& p){
    using R=std::decay_t<decltype(p)>;
    return WithRecord(y,b.record.payload,[&](const auto& q_any){
      if constexpr(!std::is_same_v<std::decay_t<decltype(q_any)>,R>)return false;
      else {
        const auto& q=q_any;
        return Metadata(a,p)&&Metadata(b,q)&&Bind(p).object==Bind(q).object&&DatabaseOf(p)==DatabaseOf(q)&&
            p.origin_transaction_uuid.value==q.origin_transaction_uuid.value&&
            p.origin_local_transaction_id==q.origin_local_transaction_id&&
            ((p.generation==q.generation&&a.record.payload==b.record.payload)||q.generation>p.generation);
      }
    });
  });
}
} // namespace scratchbird::core::catalog
