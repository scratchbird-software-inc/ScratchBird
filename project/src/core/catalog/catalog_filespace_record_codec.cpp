// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_filespace_record_codec.hpp"
#include "uuid.hpp"
#include <array>
#include <cstdint>
#include <limits>
#include <utility>

namespace scratchbird::core::catalog {
namespace {
using R=CatalogFilespaceRecord;
using T=CatalogValueType;
constexpr std::array<std::pair<u16,u64 R::*>,11> kCounters{{
    {3,&R::filespace_role},{10,&R::state},{11,&R::physical_filespace_id},
    {12,&R::lifecycle_generation},{13,&R::filespace_manifest_generation},
    {14,&R::catalog_manifest_format_version},{15,&R::resource_seed_manifest_format_version},
    {16,&R::registered_txn},{17,&R::last_lifecycle_transaction},{18,&R::uuid_source},
    {37,&R::creator_transaction_number}}};
constexpr std::array<std::pair<u16,bool R::*>,24> kFlags{{
    {4,&R::first_filespace},{5,&R::startup_authority},{6,&R::catalog_persistence_owner},
    {7,&R::filespace_manifest_owner},{8,&R::recovery_evidence_owner},{9,&R::read_only},
    {19,&R::header_database_uuid_match_required},{20,&R::header_filespace_uuid_match_required},
    {21,&R::startup_state_coupled},{22,&R::page_header_coupled},{23,&R::open_validate_header},
    {24,&R::attach_admission_validate_header},{25,&R::transaction_admission_validate_filespace},
    {26,&R::maintenance_validate_header},{27,&R::verify_repair_validate_header},
    {28,&R::shutdown_validate_header},{29,&R::recovery_validate_header},
    {30,&R::drop_requires_database_lifecycle},{31,&R::quarantine_on_ambiguous},
    {32,&R::state_change_evidence_before_success},{33,&R::mga_visibility_required},
    {34,&R::path_is_locator_not_identity},{35,&R::duplicate_identity_refusal},
    {36,&R::stale_identity_refusal}}};
constexpr auto kFields=[] {
  std::array<CatalogValueFieldSchema,37> out{};
  for(unsigned i=0;i<out.size();++i)out[i]={static_cast<u16>(i+1),T::boolean,true,1};
  out[0]={1,T::engine_identity,true,16,UuidKind::filespace};
  out[1]={2,T::engine_identity,true,16,UuidKind::database};
  for(const auto& [tag,member]:kCounters)out[tag-1]={tag,T::unsigned_integer,true,8};
  return out;
}();
constexpr CatalogValueSchemaView kSchema{65538,1,kFields};
bool Valid(const R& r) {
  constexpr auto max=std::numeric_limits<u32>::max();
  return r.filespace_uuid.kind==UuidKind::filespace&&uuid::IsEngineIdentityUuid(r.filespace_uuid.value)&&
      r.database_uuid.kind==UuidKind::database&&uuid::IsEngineIdentityUuid(r.database_uuid.value)&&
      r.filespace_role>=1&&r.filespace_role<=16&&r.state>=1&&r.state<=16&&r.uuid_source==1&&
      r.physical_filespace_id<=max&&r.lifecycle_generation>0&&r.filespace_manifest_generation>0&&
      r.catalog_manifest_format_version>0&&r.catalog_manifest_format_version<=max&&
      r.resource_seed_manifest_format_version>0&&r.resource_seed_manifest_format_version<=max&&
      r.registered_txn>0&&r.last_lifecycle_transaction>0&&r.creator_transaction_number>0;
}
}
const CatalogValueSchema& CatalogFilespaceRecordSchema() {
  static const CatalogValueSchema schema{65538,1,{kFields.begin(),kFields.end()}};
  return schema;
}
std::optional<std::size_t> CatalogFilespaceRecordEncodedBytes(const R& r) {
  if(!Valid(r))return {};
  return 464;
}
CatalogValueEncodeIntoResult EncodeCatalogFilespaceRecordInto(const R& r,std::span<byte> backing) {
  if(!Valid(r))return {CatalogValueError::invalid_value,{}};
  const auto out=reinterpret_cast<std::uintptr_t>(backing.data());
  const auto input=reinterpret_cast<std::uintptr_t>(&r);
  if((backing.size()&&!backing.data())||backing.size()>std::numeric_limits<std::uintptr_t>::max()-out||
      (out<input+sizeof(r)&&input<out+backing.size()))return {CatalogValueError::invalid_backing,{}};
  std::array<std::array<byte,8>,37> scalars{};
  std::array<CatalogValueFieldView,37> fields{};
  fields[0]={1,T::engine_identity,r.filespace_uuid.kind,r.filespace_uuid.value.bytes};
  fields[1]={2,T::engine_identity,r.database_uuid.kind,r.database_uuid.value.bytes};
  for(const auto& [tag,member]:kCounters) {
    for(unsigned i=0;i<8;++i)scalars[tag-1][i]=static_cast<byte>((r.*member)>>(8*i));
    fields[tag-1]={tag,T::unsigned_integer,UuidKind::unknown,scalars[tag-1]};
  }
  for(const auto& [tag,member]:kFlags) {
    scalars[tag-1][0]=r.*member;
    fields[tag-1]={tag,T::boolean,UuidKind::unknown,std::span(scalars[tag-1]).first<1>()};
  }
  return EncodeCatalogValueBlockInto(kSchema,fields,backing);
}
CatalogValueEncodeResult EncodeCatalogFilespaceRecord(const R& r) {
  std::array<byte,464> backing;
  const auto encoded=EncodeCatalogFilespaceRecordInto(r,backing);
  if(!encoded.ok())return {encoded.error,{}};
  return {CatalogValueError::none,{encoded.bytes.begin(),encoded.bytes.end()}};
}
CatalogFilespaceRecordDecodeResult DecodeCatalogFilespaceRecord(std::string_view bytes) {
  if(bytes.size()!=464)return {CatalogValueError::invalid_framing,{}};
  std::array<CatalogValueFieldView,37> fields;
  const auto decoded=DecodeCatalogValueBlockInto(kSchema,
      {reinterpret_cast<const byte*>(bytes.data()),bytes.size()},fields);
  if(!decoded.ok())return {decoded.error,{}};
  R r;r.filespace_uuid=*fields[0].identity();r.database_uuid=*fields[1].identity();
  for(const auto& [tag,member]:kCounters)r.*member=*fields[tag-1].unsigned_value();
  for(const auto& [tag,member]:kFlags)r.*member=fields[tag-1].bytes[0]!=0;
  if(!Valid(r))return {CatalogValueError::invalid_value,{}};
  return {CatalogValueError::none,r};
}
} // namespace scratchbird::core::catalog
