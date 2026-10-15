// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_identifier_profile.hpp"
#include "uuid.hpp"
#include <array>
#include <limits>

namespace scratchbird::core::catalog {
namespace {
using E=CatalogValueError;using T=CatalogValueType;
constexpr u32 schema_id=65617;
constexpr CatalogValueFieldSchema fields[]{
  {1,T::engine_identity,true,16,UuidKind::object},{2,T::unsigned_integer,true,8},
  {3,T::engine_identity,true,16,UuidKind::database},{4,T::utf8_text,true,256},
  {5,T::unsigned_integer,true,8},{6,T::unsigned_integer,true,8},{7,T::unsigned_integer,true,8},
  {8,T::boolean,true,1},{9,T::unsigned_integer,true,8},{10,T::unsigned_integer,true,8},
  {11,T::unsigned_integer,true,8},{12,T::unsigned_integer,true,8},{13,T::unsigned_integer,true,8},
  {14,T::unsigned_integer,true,8},
  {15,T::engine_identity,false,16,UuidKind::object},{16,T::unsigned_integer,false,8},
  {17,T::engine_identity,false,16,UuidKind::object},{18,T::unsigned_integer,false,8},
  {19,T::engine_identity,false,16,UuidKind::object},{20,T::unsigned_integer,false,8},
  {21,T::engine_identity,true,16,UuidKind::transaction},{22,T::unsigned_integer,true,8}};
constexpr CatalogValueSchemaView schema{schema_id,1,fields};
bool Id(const Uuid& id){return uuid::IsEngineIdentityUuid(id);}
template<class Enum>bool Range(Enum value,u64 low,u64 high){const auto n=static_cast<u64>(value);return n>=low&&n<=high;}
bool Valid(const CatalogIdentifierResolutionProfile& r){
  if(!Id(r.profile_uuid)||!Id(r.database_uuid)||!r.generation||
      r.origin_transaction_uuid.kind!=UuidKind::transaction||!Id(r.origin_transaction_uuid.value)||
      !r.origin_local_transaction_id||r.profile_uuid==r.database_uuid||
      r.profile_uuid==r.origin_transaction_uuid.value||r.database_uuid==r.origin_transaction_uuid.value||
      r.profile_name.empty()||r.profile_name.size()>256||!r.profile_name.data()||r.profile_name.find('\0')!=r.profile_name.npos||
      r.quote_styles>15||!Range(r.escape_rule,1,4)||!Range(r.unquoted_fold_mode,0,4)||
      !Range(r.comparison_mode,1,4)||!Range(r.storage_display_mode,1,3)||
      !Range(r.reserved_word_behavior,1,3)||!Range(r.length_unit,1,2)||
      ((r.quote_styles==0)!=(r.escape_rule==IdentifierEscapeRule::not_applicable))||
      ((r.comparison_mode==IdentifierComparisonMode::profile_platform)!=r.platform.has_value()))return false;
  if((r.unquoted_fold_mode==IdentifierFoldMode::resource_upper||r.unquoted_fold_mode==IdentifierFoldMode::resource_lower)&&!r.resources)return false;
  if(r.comparison_mode==IdentifierComparisonMode::case_insensitive&&!r.resources)return false;
  // A folded lane must actually declare a fold operation. Case-insensitive
  // and platform comparisons have independent resource/profile semantics.
  if(r.comparison_mode==IdentifierComparisonMode::folded&&r.unquoted_fold_mode==IdentifierFoldMode::none)return false;
  std::array<Uuid,6> ids{r.profile_uuid,r.database_uuid,r.origin_transaction_uuid.value};std::size_t count=3;
  if(r.resources){const auto& b=*r.resources;
    if(!Id(b.charset_uuid)||!Id(b.collation_uuid)||!b.charset_generation||!b.collation_generation)return false;
    ids[count++]=b.charset_uuid;ids[count++]=b.collation_uuid;
  }
  if(r.platform){if(!Id(r.platform->profile_uuid)||!r.platform->generation)return false;ids[count++]=r.platform->profile_uuid;}
  for(std::size_t i=0;i<count;++i)for(std::size_t j=0;j<i;++j)if(ids[i]==ids[j])return false;
  return true;
}
struct FieldBacking {
  std::array<CatalogValueFieldView,22> views{};
  std::array<std::array<byte,8>,22> numbers{};
  std::size_t size=0;
  void Identity(u16 tag,const Uuid& id,UuidKind kind=UuidKind::object){views[size++]={tag,T::engine_identity,kind,id.bytes};}
  void Number(u16 tag,u64 value,T type=T::unsigned_integer){auto& bytes=numbers[size];const unsigned width=type==T::boolean?1:8;
    for(unsigned n=0;n<width;++n)bytes[n]=static_cast<byte>(value>>(n*8));
    views[size++]={tag,type,UuidKind::unknown,std::span<const byte>(bytes).first(width)};
  }
  void Bind(const CatalogIdentifierResolutionProfile& r){
    Identity(1,r.profile_uuid);Number(2,r.generation);Identity(3,r.database_uuid,UuidKind::database);
    views[size++]={4,T::utf8_text,UuidKind::unknown,{reinterpret_cast<const byte*>(r.profile_name.data()),r.profile_name.size()}};
    Number(5,r.quote_styles);Number(6,static_cast<u64>(r.escape_rule));Number(7,static_cast<u64>(r.unquoted_fold_mode));
    Number(8,r.quoted_preserves_case,T::boolean);Number(9,static_cast<u64>(r.comparison_mode));
    Number(10,static_cast<u64>(r.storage_display_mode));Number(11,static_cast<u64>(r.reserved_word_behavior));
    Number(12,r.maximum_identifier_length);Number(13,static_cast<u64>(r.length_unit));Number(14,r.dialect_mode_flags);
    if(r.resources){Identity(15,r.resources->charset_uuid);Number(16,r.resources->charset_generation);
      Identity(17,r.resources->collation_uuid);Number(18,r.resources->collation_generation);}
    if(r.platform){Identity(19,r.platform->profile_uuid);Number(20,r.platform->generation);}
    Identity(21,r.origin_transaction_uuid.value,UuidKind::transaction);Number(22,r.origin_local_transaction_id);
  }
  std::span<const CatalogValueFieldView> View()const{return std::span<const CatalogValueFieldView>(views).first(size);}
};
bool Header(const CatalogTypedRecordView& m,const CatalogIdentifierResolutionProfile& r){
  return m.header.kind==CatalogRecordKind::config_profile&&m.header.object_uuid.kind==UuidKind::object&&m.header.object_uuid.value==r.profile_uuid;
}
bool Metadata(const CatalogMetadataVersionView& m,const CatalogIdentifierResolutionProfile& r){
  return Header(m.record,r)&&m.object_subtype=="identifier_resolution_profile"&&m.authority_scope==CatalogAuthorityScope::local&&
      m.definition_version==r.generation&&m.owning_schema_uuid.kind==UuidKind::schema&&Id(m.owning_schema_uuid.value)&&
      m.record.header.parent_uuid.kind==UuidKind::object&&m.record.header.parent_uuid.value==m.owning_schema_uuid.value&&
      m.owner_uuid.kind==UuidKind::principal&&Id(m.owner_uuid.value)&&m.audit_uuid.kind==UuidKind::object&&Id(m.audit_uuid.value)&&
      m.security_policy_uuid.kind==UuidKind::object&&Id(m.security_policy_uuid.value)&&
      m.default_name_uuid.kind==UuidKind::object&&Id(m.default_name_uuid.value)&&
      m.name_vector_uuid.kind==UuidKind::object&&Id(m.name_vector_uuid.value)&&
      m.creator_transaction_uuid.kind==UuidKind::transaction&&Id(m.creator_transaction_uuid.value)&&
      r.origin_local_transaction_id<=m.creator_local_transaction_id&&
      (m.definition_version!=1||(r.origin_transaction_uuid.value==m.creator_transaction_uuid.value&&r.origin_local_transaction_id==m.creator_local_transaction_id));
}
bool Family(const CatalogMetadataVersionView& m){return m.object_subtype=="identifier_resolution_profile"||IsCatalogIdentifierProfilePayload(m.record.payload);}
}
std::optional<std::size_t> CatalogIdentifierProfileEncodedBytes(const CatalogIdentifierResolutionProfile& r){
  if(!Valid(r))return {};FieldBacking backing;backing.Bind(r);
  if(ValidateCatalogValueFields(schema,backing.View())!=E::none)return {};
  std::size_t bytes=kCatalogValueBlockHeaderBytes;for(const auto& field:backing.View())bytes+=8+field.bytes.size();return bytes;
}
CatalogValueEncodeIntoResult EncodeCatalogIdentifierProfileInto(const CatalogIdentifierResolutionProfile& r,std::span<byte> out){
  if(!Valid(r))return {E::invalid_value,{}};
  const auto dest=reinterpret_cast<std::uintptr_t>(out.data()),input=reinterpret_cast<std::uintptr_t>(&r);
  if((out.size()&&!out.data())||out.size()>std::numeric_limits<std::uintptr_t>::max()-dest||
      (dest<input+sizeof(r)&&input<dest+out.size()))return {E::invalid_backing,{}};
  FieldBacking backing;backing.Bind(r);return EncodeCatalogValueBlockInto(schema,backing.View(),out);
}
CatalogValueEncodeResult EncodeCatalogIdentifierProfile(const CatalogIdentifierResolutionProfile& r){
  std::array<byte,665> backing;const auto encoded=EncodeCatalogIdentifierProfileInto(r,backing);
  if(!encoded.ok())return {encoded.error,{}};
  return {E::none,{encoded.bytes.begin(),encoded.bytes.end()}};
}
CatalogIdentifierProfileResult DecodeCatalogIdentifierProfile(std::string_view bytes){
  std::array<CatalogValueFieldView,22> backing;
  const auto decoded=DecodeCatalogValueBlockInto(schema,{reinterpret_cast<const byte*>(bytes.data()),bytes.size()},backing);
  if(!decoded.ok())return {decoded.error,{}};
  CatalogIdentifierResolutionProfile r;CatalogIdentifierResourceBinding resources;CatalogIdentifierPlatformBinding platform;
  unsigned resource_fields=0,platform_fields=0;
  for(const auto& f:decoded.fields)switch(f.id){
    case 1:r.profile_uuid=f.identity()->value;break;case 2:r.generation=*f.unsigned_value();break;
    case 3:r.database_uuid=f.identity()->value;break;
    case 4:r.profile_name={reinterpret_cast<const char*>(f.bytes.data()),f.bytes.size()};break;
    case 5:r.quote_styles=*f.unsigned_value();break;case 6:r.escape_rule=static_cast<IdentifierEscapeRule>(*f.unsigned_value());break;
    case 7:r.unquoted_fold_mode=static_cast<IdentifierFoldMode>(*f.unsigned_value());break;
    case 8:r.quoted_preserves_case=f.bytes[0]!=0;break;
    case 9:r.comparison_mode=static_cast<IdentifierComparisonMode>(*f.unsigned_value());break;
    case 10:r.storage_display_mode=static_cast<IdentifierDisplayMode>(*f.unsigned_value());break;
    case 11:r.reserved_word_behavior=static_cast<IdentifierReservedWordMode>(*f.unsigned_value());break;
    case 12:r.maximum_identifier_length=*f.unsigned_value();break;case 13:r.length_unit=static_cast<IdentifierLengthUnit>(*f.unsigned_value());break;
    case 14:r.dialect_mode_flags=*f.unsigned_value();break;
    case 15:resources.charset_uuid=f.identity()->value;++resource_fields;break;
    case 16:resources.charset_generation=*f.unsigned_value();++resource_fields;break;
    case 17:resources.collation_uuid=f.identity()->value;++resource_fields;break;
    case 18:resources.collation_generation=*f.unsigned_value();++resource_fields;break;
    case 19:platform.profile_uuid=f.identity()->value;++platform_fields;break;
    case 20:platform.generation=*f.unsigned_value();++platform_fields;break;
    case 21:r.origin_transaction_uuid=*f.identity();break;case 22:r.origin_local_transaction_id=*f.unsigned_value();break;
  }
  if((resource_fields&&resource_fields!=4)||(platform_fields&&platform_fields!=2))return {E::invalid_value,{}};
  if(resource_fields)r.resources=resources;if(platform_fields)r.platform=platform;
  if(!Valid(r))return {E::invalid_value,{}};return {E::none,r};
}
bool IsCatalogIdentifierProfilePayload(std::string_view bytes){return bytes.size()>=kCatalogValueBlockHeaderBytes&&bytes.substr(0,4)=="SBCV"&&
    platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data())+16)==schema_id;}
bool CatalogIdentifierProfileMatchesHeader(const CatalogTypedRecordView& m){const auto r=DecodeCatalogIdentifierProfile(m.payload);return r.ok()&&Header(m,*r.record);}
bool CatalogIdentifierProfileMatchesMetadata(const CatalogMetadataVersionView& m){const auto r=DecodeCatalogIdentifierProfile(m.record.payload);return r.ok()&&Metadata(m,*r.record);}
bool CatalogIdentifierProfileMatchesDatabase(const CatalogMetadataVersionView& m,const Uuid& db){if(!Family(m))return true;
  const auto r=DecodeCatalogIdentifierProfile(m.record.payload);return Id(db)&&r.ok()&&r.record->database_uuid==db&&Metadata(m,*r.record);}
bool CatalogIdentifierProfilePreservesOrigin(const CatalogMetadataVersionView& a,const CatalogMetadataVersionView& b){
  if(!Family(a)&&!Family(b))return true;const auto x=DecodeCatalogIdentifierProfile(a.record.payload),y=DecodeCatalogIdentifierProfile(b.record.payload);
  return x.ok()&&y.ok()&&Metadata(a,*x.record)&&Metadata(b,*y.record)&&x.record->profile_uuid==y.record->profile_uuid&&
      x.record->database_uuid==y.record->database_uuid&&x.record->origin_transaction_uuid.value==y.record->origin_transaction_uuid.value&&
      x.record->origin_local_transaction_id==y.record->origin_local_transaction_id;
}
} // namespace scratchbird::core::catalog
