// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_memory_configuration.hpp"
#include "uuid.hpp"
#include <array>

namespace scratchbird::core::catalog {
namespace {
using E=CatalogValueError;using T=CatalogValueType;
constexpr CatalogValueFieldSchema fields[]{
  {1,T::engine_identity,true,16,UuidKind::object},{2,T::unsigned_integer,true,8},
  {3,T::engine_identity,true,16,UuidKind::object},{4,T::unsigned_integer,true,8},
  {5,T::engine_identity,true,16,UuidKind::object},{6,T::unsigned_integer,true,8},
  {7,T::utf8_text,true,256},{8,T::utf8_text,true,256},{9,T::utf8_text,true,256},
  {10,T::engine_identity,true,16,UuidKind::transaction},{11,T::unsigned_integer,true,8},
  {12,T::unsigned_integer,true,8},{13,T::unsigned_integer,true,8},{14,T::unsigned_integer,true,8},
  {15,T::unsigned_integer,true,8},{16,T::unsigned_integer,true,8},{17,T::unsigned_integer,true,8},
  {18,T::unsigned_integer,true,8},{19,T::boolean,true,1},{20,T::boolean,true,1},
  {21,T::boolean,true,1},{22,T::boolean,true,1},{23,T::boolean,true,1},{24,T::boolean,true,1},
  {25,T::boolean,true,1},{26,T::unsigned_integer,true,8},{27,T::boolean,true,1},
  {28,T::boolean,true,1},{29,T::boolean,true,1},{30,T::unsigned_integer,true,8},
  {31,T::boolean,true,1},{32,T::boolean,true,1},{33,T::boolean,true,1}};
constexpr CatalogValueSchemaView schema{65591,1,fields};
bool Id(const Uuid& value){return uuid::IsEngineIdentityUuid(value);}
bool Typed(const TypedUuid& value,UuidKind kind){return value.kind==kind&&Id(value.value);}
bool Text(std::string_view value){return !value.empty()&&value.size()<=256&&value.find('\0')==value.npos;}
bool ValidSettings(const CatalogMemoryConfiguration& r) {
  if(!r.generation||r.imported_schema_version!=1||
      !Text(r.policy_key)||!Text(r.policy_name)||!Text(r.provenance))return false;
  return r.hard_limit_bytes&&r.soft_limit_bytes<=r.hard_limit_bytes&&
      r.per_context_limit_bytes<=r.hard_limit_bytes&&r.page_buffer_pool_limit_bytes<=r.hard_limit_bytes&&
      r.openssl_budget_bytes&&r.openssl_budget_bytes<r.hard_limit_bytes&&
      r.min_startup_available_bytes<=r.hard_limit_bytes&&
      (r.failure_mode==MemoryConfigurationFailureMode::return_error||r.failure_mode==MemoryConfigurationFailureMode::fatal_status)&&
      (!r.require_platform_memory_ceiling||r.enable_platform_memory_probe)&&
      r.page_buffer_growth==MemoryConfigurationGrowth::adaptive_with_policy_limit&&
      r.write_flush_authority==MemoryConfigurationWriteAuthority::mga_inventory_and_page_writeback&&
      r.dirty_writeback_required&&!r.cache_finality_authority&&!r.cache_visibility_authority&&!r.wal_or_redo_authority;
}
bool Valid(const CatalogMemoryConfiguration& r) {
  if(!Id(r.configuration_uuid)||!Id(r.policy_pack_uuid)||!Id(r.profile_uuid)||
      !Typed(r.origin_transaction_uuid,UuidKind::transaction)||!r.origin_local_transaction_id||
      !r.policy_pack_generation||!ValidSettings(r))return false;
  const std::array ids{r.configuration_uuid,r.policy_pack_uuid,r.profile_uuid,r.origin_transaction_uuid.value};
  for(std::size_t i=0;i<ids.size();++i)for(std::size_t j=0;j<i;++j)if(ids[i]==ids[j])return false;
  return true;
}
struct Encoder {
  std::array<CatalogValueFieldView,33> values{};
  std::array<std::array<byte,8>,33> scalars{};
  std::size_t count=0;
  void Id(u16 tag,const Uuid& id,UuidKind kind=UuidKind::object){values[count++]={tag,T::engine_identity,kind,id.bytes};}
  void Number(u16 tag,u64 n,T type=T::unsigned_integer){
    auto& bytes=scalars[count];const auto size=type==T::boolean?1:8;
    for(int i=0;i<size;++i)bytes[i]=static_cast<byte>(n>>(8*i));
    values[count++]={tag,type,UuidKind::unknown,std::span<const byte>(bytes).first(size)};
  }
  void Text(u16 tag,std::string_view text){values[count++]={tag,T::utf8_text,UuidKind::unknown,
      {reinterpret_cast<const byte*>(text.data()),text.size()}};}
};
std::string_view Text(const CatalogValueFieldView& value){return {reinterpret_cast<const char*>(value.bytes.data()),value.bytes.size()};}
} // namespace
CatalogValueError ValidateCatalogMemoryConfigurationSettings(const CatalogMemoryConfiguration& r) {
  return ValidSettings(r)?E::none:E::invalid_value;
}
CatalogValueEncodeIntoResult EncodeCatalogMemoryConfigurationInto(const CatalogMemoryConfiguration& r,std::span<byte> backing) {
  if(!Valid(r))return {E::invalid_value,{}};
  Encoder f;f.Id(1,r.configuration_uuid);f.Number(2,r.generation);f.Id(3,r.policy_pack_uuid);
  f.Number(4,r.policy_pack_generation);f.Id(5,r.profile_uuid);f.Number(6,r.imported_schema_version);
  f.Text(7,r.policy_key);f.Text(8,r.policy_name);f.Text(9,r.provenance);
  f.Id(10,r.origin_transaction_uuid.value,UuidKind::transaction);f.Number(11,r.origin_local_transaction_id);
  f.Number(12,r.hard_limit_bytes);f.Number(13,r.soft_limit_bytes);f.Number(14,r.per_context_limit_bytes);
  f.Number(15,r.page_buffer_pool_limit_bytes);f.Number(16,r.openssl_budget_bytes);f.Number(17,r.min_startup_available_bytes);
  f.Number(18,static_cast<u64>(r.failure_mode));f.Number(19,r.track_allocations,T::boolean);
  f.Number(20,r.zero_memory_on_allocate,T::boolean);f.Number(21,r.zero_memory_on_release,T::boolean);
  f.Number(22,r.reject_over_soft_limit,T::boolean);f.Number(23,r.enable_platform_memory_probe,T::boolean);
  f.Number(24,r.require_platform_memory_ceiling,T::boolean);f.Number(25,r.adaptive_cache_enabled,T::boolean);
  f.Number(26,static_cast<u64>(r.page_buffer_growth));f.Number(27,r.index_read_optimization,T::boolean);
  f.Number(28,r.ordinary_disconnect_heap_trim,T::boolean);f.Number(29,r.dirty_writeback_required,T::boolean);
  f.Number(30,static_cast<u64>(r.write_flush_authority));f.Number(31,r.cache_finality_authority,T::boolean);
  f.Number(32,r.cache_visibility_authority,T::boolean);f.Number(33,r.wal_or_redo_authority,T::boolean);
  return EncodeCatalogValueBlockInto(schema,f.values,backing);
}
CatalogMemoryConfigurationResult DecodeCatalogMemoryConfiguration(std::string_view bytes) {
  std::array<CatalogValueFieldView,33> backing;
  const auto decoded=DecodeCatalogValueBlockInto(schema,
      {reinterpret_cast<const byte*>(bytes.data()),bytes.size()},backing);
  if(!decoded.ok())return {decoded.error,{}};
  CatalogMemoryConfiguration r;
  for(const auto& f:decoded.fields)switch(f.id) {
    case 1:r.configuration_uuid=f.identity()->value;break;case 2:r.generation=*f.unsigned_value();break;
    case 3:r.policy_pack_uuid=f.identity()->value;break;case 4:r.policy_pack_generation=*f.unsigned_value();break;
    case 5:r.profile_uuid=f.identity()->value;break;case 6:r.imported_schema_version=*f.unsigned_value();break;
    case 7:r.policy_key=Text(f);break;case 8:r.policy_name=Text(f);break;case 9:r.provenance=Text(f);break;
    case 10:r.origin_transaction_uuid=*f.identity();break;case 11:r.origin_local_transaction_id=*f.unsigned_value();break;
    case 12:r.hard_limit_bytes=*f.unsigned_value();break;case 13:r.soft_limit_bytes=*f.unsigned_value();break;
    case 14:r.per_context_limit_bytes=*f.unsigned_value();break;case 15:r.page_buffer_pool_limit_bytes=*f.unsigned_value();break;
    case 16:r.openssl_budget_bytes=*f.unsigned_value();break;case 17:r.min_startup_available_bytes=*f.unsigned_value();break;
    case 18:r.failure_mode=static_cast<MemoryConfigurationFailureMode>(*f.unsigned_value());break;
    case 19:r.track_allocations=f.bytes[0]!=0;break;case 20:r.zero_memory_on_allocate=f.bytes[0]!=0;break;
    case 21:r.zero_memory_on_release=f.bytes[0]!=0;break;case 22:r.reject_over_soft_limit=f.bytes[0]!=0;break;
    case 23:r.enable_platform_memory_probe=f.bytes[0]!=0;break;case 24:r.require_platform_memory_ceiling=f.bytes[0]!=0;break;
    case 25:r.adaptive_cache_enabled=f.bytes[0]!=0;break;
    case 26:r.page_buffer_growth=static_cast<MemoryConfigurationGrowth>(*f.unsigned_value());break;
    case 27:r.index_read_optimization=f.bytes[0]!=0;break;case 28:r.ordinary_disconnect_heap_trim=f.bytes[0]!=0;break;
    case 29:r.dirty_writeback_required=f.bytes[0]!=0;break;
    case 30:r.write_flush_authority=static_cast<MemoryConfigurationWriteAuthority>(*f.unsigned_value());break;
    case 31:r.cache_finality_authority=f.bytes[0]!=0;break;case 32:r.cache_visibility_authority=f.bytes[0]!=0;break;
    case 33:r.wal_or_redo_authority=f.bytes[0]!=0;break;
  }
  if(!Valid(r))return {E::invalid_value,{}};
  return {E::none,r};
}
bool IsCatalogMemoryConfigurationPayload(std::string_view bytes) {
  return bytes.size()>=kCatalogValueBlockHeaderBytes&&bytes.substr(0,4)=="SBCV"&&
      platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data())+16)==65591;
}
bool CatalogMemoryConfigurationMatchesHeader(const CatalogTypedRecordView& record) {
  if(record.header.kind!=CatalogRecordKind::config_profile||!Typed(record.header.object_uuid,UuidKind::object))return false;
  const auto decoded=DecodeCatalogMemoryConfiguration(record.payload);
  return decoded.ok()&&decoded.record->configuration_uuid==record.header.object_uuid.value;
}
bool CatalogMemoryConfigurationMatchesMetadata(const CatalogMetadataVersionView& m) {
  if(m.object_subtype!="memory_configuration"||m.record.header.kind!=CatalogRecordKind::config_profile||
      !Typed(m.record.header.object_uuid,UuidKind::object)||
      m.authority_scope!=CatalogAuthorityScope::local||!Typed(m.owner_uuid,UuidKind::principal)||
      !Typed(m.audit_uuid,UuidKind::object)||!Typed(m.security_policy_uuid,UuidKind::object)||
      !Typed(m.default_name_uuid,UuidKind::object)||!Typed(m.name_vector_uuid,UuidKind::object)||
      !Typed(m.owning_schema_uuid,UuidKind::schema)||!Typed(m.creator_transaction_uuid,UuidKind::transaction)||
      !m.creator_local_transaction_id||!Typed(m.record.header.parent_uuid,UuidKind::object)||
      m.record.header.parent_uuid.value!=m.owning_schema_uuid.value)return false;
  const auto decoded=DecodeCatalogMemoryConfiguration(m.record.payload);
  if(!decoded.ok())return false;
  const auto& r=*decoded.record;
  return r.configuration_uuid==m.record.header.object_uuid.value&&
      r.generation==m.definition_version&&r.origin_local_transaction_id<=m.creator_local_transaction_id&&
      (m.definition_version!=1||(r.origin_transaction_uuid.kind==m.creator_transaction_uuid.kind&&
       r.origin_transaction_uuid.value==m.creator_transaction_uuid.value&&r.origin_local_transaction_id==m.creator_local_transaction_id));
}
bool CatalogMemoryConfigurationPreservesOrigin(const CatalogMetadataVersionView& a,const CatalogMetadataVersionView& b) {
  const auto family=[](const auto& m){return m.object_subtype=="memory_configuration"||IsCatalogMemoryConfigurationPayload(m.record.payload);};
  if(!family(a)&&!family(b))return true;
  if(!CatalogMemoryConfigurationMatchesMetadata(a)||!CatalogMemoryConfigurationMatchesMetadata(b))return false;
  const auto x=DecodeCatalogMemoryConfiguration(a.record.payload),y=DecodeCatalogMemoryConfiguration(b.record.payload);
  return x.record->configuration_uuid==y.record->configuration_uuid&&x.record->policy_pack_uuid==y.record->policy_pack_uuid&&
      x.record->profile_uuid==y.record->profile_uuid&&x.record->origin_transaction_uuid.kind==y.record->origin_transaction_uuid.kind&&
      x.record->origin_transaction_uuid.value==y.record->origin_transaction_uuid.value&&
      x.record->origin_local_transaction_id==y.record->origin_local_transaction_id;
}
} // namespace scratchbird::core::catalog
