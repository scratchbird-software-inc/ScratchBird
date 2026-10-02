// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"
#include "filespace_bootstrap.hpp"
#include "uuid.hpp"
#include <array>
#include <algorithm>
#include <limits>
#include <optional>
#include <string_view>

namespace scratchbird::core::catalog {
enum class StorageActionApproval : u64 { none=0, operator_approval=1, sysarch=2, break_glass=3 };
enum class StorageActionPressure : u64 { unknown=1, degraded=2, critical=3, failed=4 };
// STORAGE-NATIVE-ACTION-POLICY-001. Definition only, not a selected policy or grant.
struct CatalogStorageActionPolicy {
  Uuid policy_uuid, database_uuid, filespace_uuid, storage_profile_uuid, page_size_profile_uuid;
  u64 generation=0;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id=0;
  bool enabled=false, growth_allowed=false, preallocation_allowed=false;
  StorageActionApproval approval=StorageActionApproval::operator_approval;
  u64 minimum_free_pages=4, target_free_pages=8, growth_increment_pages=8;
  u64 maximum_total_pages=0, maximum_pages_per_action=0, maximum_work_bytes=0;
  u64 maximum_retained_image_bytes=0, cooldown_microseconds=60000000, maximum_runtime_microseconds=0;
  StorageActionPressure refusal_pressure=StorageActionPressure::degraded;
};
struct CatalogStorageActionPolicyResult {
  CatalogValueError error=CatalogValueError::invalid_value;
  std::optional<CatalogStorageActionPolicy> record;
  bool ok() const {return error==CatalogValueError::none&&record.has_value();}
};
const CatalogValueSchema& CatalogStorageActionPolicySchema();
CatalogValueEncodeResult EncodeCatalogStorageActionPolicy(const CatalogStorageActionPolicy&);
CatalogStorageActionPolicyResult DecodeCatalogStorageActionPolicy(std::string_view);
bool IsCatalogStorageActionPolicyPayload(std::string_view);
bool CatalogStorageActionPolicyMatchesHeader(const CatalogTypedRecord&);
bool CatalogStorageActionPolicyMatchesMetadata(const CatalogMetadataVersion&);
bool CatalogStorageActionPolicyPreservesOrigin(const CatalogMetadataVersion&,const CatalogMetadataVersion&);
} // namespace scratchbird::core::catalog

namespace scratchbird::core::catalog {
namespace storage_action_policy_detail {
inline bool Identity(const TypedUuid& id,UuidKind kind){return id.kind==kind&&uuid::IsEngineIdentityUuid(id.value);}
inline bool Valid(const CatalogStorageActionPolicy& r){
  for(const auto* id:{&r.policy_uuid,&r.database_uuid,&r.filespace_uuid,&r.storage_profile_uuid,&r.page_size_profile_uuid})
    if(!uuid::IsEngineIdentityUuid(*id))return false;
  const auto& profiles=storage::disk::kCanonicalFilespacePageProfiles;
  const auto profile=std::find_if(profiles.begin(),profiles.end(),[&](const auto& p){return p.uuid==r.page_size_profile_uuid;});
  return profile!=profiles.end()&&r.generation&&Identity(r.origin_transaction_uuid,UuidKind::transaction)&&r.origin_local_transaction_id&&
    static_cast<u64>(r.approval)<=3&&static_cast<u64>(r.refusal_pressure)>=1&&static_cast<u64>(r.refusal_pressure)<=4&&
    r.minimum_free_pages<=r.target_free_pages&&r.target_free_pages<=0x7fffffff&&
    r.growth_increment_pages&&r.growth_increment_pages<=0x7fffffff&&r.maximum_total_pages&&
    r.maximum_total_pages<=std::numeric_limits<u64>::max()/profile->page_size_bytes&&
    r.target_free_pages<=r.maximum_total_pages&&r.growth_increment_pages<=r.maximum_pages_per_action&&
    r.maximum_pages_per_action<=r.maximum_total_pages&&r.maximum_pages_per_action<=r.maximum_work_bytes/profile->page_size_bytes&&
    r.maximum_retained_image_bytes>=768&&r.cooldown_microseconds<=86400000000ULL&&
    r.maximum_runtime_microseconds&&r.maximum_runtime_microseconds<=86400000000ULL;
}
inline bool Family(const CatalogMetadataVersion& r){return r.object_subtype=="storage_action"||IsCatalogStorageActionPolicyPayload(r.record.payload);}
}
inline constexpr CatalogValueSchemaView CatalogStorageActionPolicySchemaView() {
  using T = CatalogValueType;
  static constexpr CatalogValueFieldSchema fields[]{
    {1,T::engine_identity,true,16,UuidKind::object},{2,T::unsigned_integer,true,8},
    {3,T::engine_identity,true,16,UuidKind::database},{4,T::engine_identity,true,16,UuidKind::filespace},
    {5,T::engine_identity,true,16,UuidKind::object},{6,T::engine_identity,true,16,UuidKind::object},
    {7,T::engine_identity,true,16,UuidKind::transaction},{8,T::unsigned_integer,true,8},
    {9,T::boolean,true,1},{10,T::boolean,true,1},{11,T::boolean,true,1},
    {12,T::unsigned_integer,true,8},{13,T::unsigned_integer,true,8},{14,T::unsigned_integer,true,8},
    {15,T::unsigned_integer,true,8},{16,T::unsigned_integer,true,8},{17,T::unsigned_integer,true,8},
    {18,T::unsigned_integer,true,8},{19,T::unsigned_integer,true,8},{20,T::unsigned_integer,true,8},
    {21,T::unsigned_integer,true,8},{22,T::unsigned_integer,true,8}};
  return {65548, 1, fields};
}
inline const CatalogValueSchema& CatalogStorageActionPolicySchema() {
  static const CatalogValueSchema schema = [] {
    const auto view = CatalogStorageActionPolicySchemaView();
    return CatalogValueSchema{view.id, view.version, {view.fields.begin(), view.fields.end()}};
  }();
  return schema;
}
inline CatalogValueEncodeResult EncodeCatalogStorageActionPolicy(const CatalogStorageActionPolicy& r){
  if(!storage_action_policy_detail::Valid(r))return {CatalogValueError::invalid_value,{}};
  return EncodeCatalogValueBlock(CatalogStorageActionPolicySchema(),{
    {1,TypedUuid{UuidKind::object,r.policy_uuid}},{2,r.generation},
    {3,TypedUuid{UuidKind::database,r.database_uuid}},{4,TypedUuid{UuidKind::filespace,r.filespace_uuid}},
    {5,TypedUuid{UuidKind::object,r.storage_profile_uuid}},{6,TypedUuid{UuidKind::object,r.page_size_profile_uuid}},
    {7,r.origin_transaction_uuid},{8,r.origin_local_transaction_id},{9,r.enabled},{10,r.growth_allowed},{11,r.preallocation_allowed},
    {12,static_cast<u64>(r.approval)},{13,r.minimum_free_pages},{14,r.target_free_pages},{15,r.growth_increment_pages},
    {16,r.maximum_total_pages},{17,r.maximum_pages_per_action},{18,r.maximum_work_bytes},{19,r.maximum_retained_image_bytes},
    {20,r.cooldown_microseconds},{21,r.maximum_runtime_microseconds},{22,static_cast<u64>(r.refusal_pressure)}});
}
inline CatalogStorageActionPolicyResult DecodeCatalogStorageActionPolicy(std::string_view bytes){
  if(bytes.size()>kCatalogValueBlockMaxBytes)return {CatalogValueError::size_limit,{}};
  std::array<CatalogValueFieldView, 22> fields;
  const auto decoded = DecodeCatalogValueBlockInto(CatalogStorageActionPolicySchemaView(),
      {reinterpret_cast<const byte*>(bytes.data()), bytes.size()}, fields);
  if(!decoded.ok())return {decoded.error,{}};
  const auto& f=decoded.fields;CatalogStorageActionPolicy r;
  r.policy_uuid=f[0].identity()->value;r.generation=*f[1].unsigned_value();
  r.database_uuid=f[2].identity()->value;r.filespace_uuid=f[3].identity()->value;
  r.storage_profile_uuid=f[4].identity()->value;r.page_size_profile_uuid=f[5].identity()->value;
  r.origin_transaction_uuid=*f[6].identity();r.origin_local_transaction_id=*f[7].unsigned_value();
  r.enabled=(f[8].bytes[0] != 0);r.growth_allowed=(f[9].bytes[0] != 0);r.preallocation_allowed=(f[10].bytes[0] != 0);
  r.approval=static_cast<StorageActionApproval>(*f[11].unsigned_value());
  r.minimum_free_pages=*f[12].unsigned_value();r.target_free_pages=*f[13].unsigned_value();
  r.growth_increment_pages=*f[14].unsigned_value();r.maximum_total_pages=*f[15].unsigned_value();
  r.maximum_pages_per_action=*f[16].unsigned_value();r.maximum_work_bytes=*f[17].unsigned_value();
  r.maximum_retained_image_bytes=*f[18].unsigned_value();r.cooldown_microseconds=*f[19].unsigned_value();
  r.maximum_runtime_microseconds=*f[20].unsigned_value();r.refusal_pressure=static_cast<StorageActionPressure>(*f[21].unsigned_value());
  if(!storage_action_policy_detail::Valid(r))return {CatalogValueError::invalid_value,{}};
  return {CatalogValueError::none,std::move(r)};
}
inline bool IsCatalogStorageActionPolicyPayload(std::string_view b){
  return b.size()>=kCatalogValueBlockHeaderBytes&&b.substr(0,4)=="SBCV"&&
    platform::LoadLittle32(reinterpret_cast<const byte*>(b.data())+16)==65548;
}
inline bool CatalogStorageActionPolicyMatchesHeader(const CatalogTypedRecordView& r) {
  if(r.header.kind!=CatalogRecordKind::policy||!storage_action_policy_detail::Identity(r.header.object_uuid,UuidKind::object))return false;
  const auto decoded=DecodeCatalogStorageActionPolicy(r.payload);
  return decoded.ok()&&decoded.record->policy_uuid==r.header.object_uuid.value;
}
inline bool CatalogStorageActionPolicyMatchesHeader(const CatalogTypedRecord& r) {
  return CatalogStorageActionPolicyMatchesHeader(BorrowCatalogTypedRecord(r));
}
inline bool CatalogStorageActionPolicyMatchesMetadata(const CatalogMetadataVersion& m){
  if(!CatalogStorageActionPolicyMatchesHeader(m.record)||m.object_subtype!="storage_action"||
      m.authority_scope!=CatalogAuthorityScope::local||!storage_action_policy_detail::Identity(m.owning_schema_uuid,UuidKind::schema)||
      !storage_action_policy_detail::Identity(m.record.header.parent_uuid,UuidKind::object)||m.owning_schema_uuid.value!=m.record.header.parent_uuid.value||
      !storage_action_policy_detail::Identity(m.default_name_uuid,UuidKind::object)||!storage_action_policy_detail::Identity(m.name_vector_uuid,UuidKind::object)||
      !storage_action_policy_detail::Identity(m.creator_transaction_uuid,UuidKind::transaction))return false;
  const auto decoded=DecodeCatalogStorageActionPolicy(m.record.payload);const auto& r=*decoded.record;
  // Local transaction numbers identify creators; they do not prove commit
  // order. Live visibility and writable version admission belong to MGA.
  return r.generation==m.definition_version&&
    (m.definition_version!=1||(r.origin_transaction_uuid.value==m.creator_transaction_uuid.value&&
      r.origin_local_transaction_id==m.creator_local_transaction_id));
}
inline bool CatalogStorageActionPolicyPreservesOrigin(const CatalogMetadataVersion& a,const CatalogMetadataVersion& b){
  if(!storage_action_policy_detail::Family(a)&&!storage_action_policy_detail::Family(b))return true;
  if(!CatalogStorageActionPolicyMatchesMetadata(a)||!CatalogStorageActionPolicyMatchesMetadata(b))return false;
  const auto x=DecodeCatalogStorageActionPolicy(a.record.payload),y=DecodeCatalogStorageActionPolicy(b.record.payload);
  return x.record->policy_uuid==y.record->policy_uuid&&x.record->database_uuid==y.record->database_uuid&&
    x.record->filespace_uuid==y.record->filespace_uuid&&x.record->storage_profile_uuid==y.record->storage_profile_uuid&&
    x.record->page_size_profile_uuid==y.record->page_size_profile_uuid&&
    x.record->origin_transaction_uuid.value==y.record->origin_transaction_uuid.value&&
    x.record->origin_local_transaction_id==y.record->origin_local_transaction_id;
}

// STORAGE-NATIVE-ACTION-ATTACHMENT-001. Persisted selection, not approval.
struct CatalogStorageActionAttachment {
  Uuid attachment_uuid, database_uuid, filespace_uuid, storage_profile_uuid, page_size_profile_uuid, policy_uuid;
  u64 generation=0;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id=0;
};
struct CatalogStorageActionAttachmentResult {
  CatalogValueError error=CatalogValueError::invalid_value;
  std::optional<CatalogStorageActionAttachment> record;
  bool ok() const {return error==CatalogValueError::none&&record.has_value();}
};
inline constexpr CatalogValueSchemaView CatalogStorageActionAttachmentSchemaView() {
  using T = CatalogValueType;
  static constexpr CatalogValueFieldSchema fields[]{
    {1,T::engine_identity,true,16,UuidKind::object},{2,T::unsigned_integer,true,8},
    {3,T::engine_identity,true,16,UuidKind::database},{4,T::engine_identity,true,16,UuidKind::filespace},
    {5,T::engine_identity,true,16,UuidKind::object},{6,T::engine_identity,true,16,UuidKind::object},
    {7,T::engine_identity,true,16,UuidKind::object},{8,T::engine_identity,true,16,UuidKind::transaction},
    {9,T::unsigned_integer,true,8}};
  return {65549, 1, fields};
}
inline const CatalogValueSchema& CatalogStorageActionAttachmentSchema() {
  static const CatalogValueSchema schema = [] {
    const auto view = CatalogStorageActionAttachmentSchemaView();
    return CatalogValueSchema{view.id, view.version, {view.fields.begin(), view.fields.end()}};
  }();
  return schema;
}
inline bool ValidCatalogStorageActionAttachment(const CatalogStorageActionAttachment& r){
  for(const auto* id:{&r.attachment_uuid,&r.database_uuid,&r.filespace_uuid,&r.storage_profile_uuid,&r.page_size_profile_uuid,&r.policy_uuid})
    if(!uuid::IsEngineIdentityUuid(*id))return false;
  const auto& profiles=storage::disk::kCanonicalFilespacePageProfiles;
  return r.generation&&r.origin_local_transaction_id&&storage_action_policy_detail::Identity(r.origin_transaction_uuid,UuidKind::transaction)&&
    std::any_of(profiles.begin(),profiles.end(),[&](const auto& p){return p.uuid==r.page_size_profile_uuid;});
}
inline CatalogValueEncodeResult EncodeCatalogStorageActionAttachment(const CatalogStorageActionAttachment& r){
  if(!ValidCatalogStorageActionAttachment(r))return {CatalogValueError::invalid_value,{}};
  return EncodeCatalogValueBlock(CatalogStorageActionAttachmentSchema(),{
    {1,TypedUuid{UuidKind::object,r.attachment_uuid}},{2,r.generation},{3,TypedUuid{UuidKind::database,r.database_uuid}},
    {4,TypedUuid{UuidKind::filespace,r.filespace_uuid}},{5,TypedUuid{UuidKind::object,r.storage_profile_uuid}},
    {6,TypedUuid{UuidKind::object,r.page_size_profile_uuid}},{7,TypedUuid{UuidKind::object,r.policy_uuid}},
    {8,r.origin_transaction_uuid},{9,r.origin_local_transaction_id}});
}
inline CatalogStorageActionAttachmentResult DecodeCatalogStorageActionAttachment(std::string_view bytes){
  if(bytes.size()>kCatalogValueBlockMaxBytes)return {CatalogValueError::size_limit,{}};
  std::array<CatalogValueFieldView, 9> fields;
  const auto decoded = DecodeCatalogValueBlockInto(CatalogStorageActionAttachmentSchemaView(),
      {reinterpret_cast<const byte*>(bytes.data()), bytes.size()}, fields);
  if(!decoded.ok())return {decoded.error,{}};
  const auto& f=decoded.fields;CatalogStorageActionAttachment r;
  r.attachment_uuid=f[0].identity()->value;r.generation=*f[1].unsigned_value();
  r.database_uuid=f[2].identity()->value;r.filespace_uuid=f[3].identity()->value;
  r.storage_profile_uuid=f[4].identity()->value;r.page_size_profile_uuid=f[5].identity()->value;
  r.policy_uuid=f[6].identity()->value;r.origin_transaction_uuid=*f[7].identity();
  r.origin_local_transaction_id=*f[8].unsigned_value();
  if(!ValidCatalogStorageActionAttachment(r))return {CatalogValueError::invalid_value,{}};
  return {CatalogValueError::none,std::move(r)};
}
inline bool IsCatalogStorageActionAttachmentPayload(std::string_view b){
  return b.size()>=kCatalogValueBlockHeaderBytes&&b.substr(0,4)=="SBCV"&&
    platform::LoadLittle32(reinterpret_cast<const byte*>(b.data())+16)==65549;
}
inline bool CatalogStorageActionAttachmentMatchesHeader(const CatalogTypedRecordView& r) {
  if(r.header.kind!=CatalogRecordKind::config_profile||!storage_action_policy_detail::Identity(r.header.object_uuid,UuidKind::object))return false;
  const auto decoded=DecodeCatalogStorageActionAttachment(r.payload);
  return decoded.ok()&&decoded.record->attachment_uuid==r.header.object_uuid.value;
}
inline bool CatalogStorageActionAttachmentMatchesHeader(const CatalogTypedRecord& r) {
  return CatalogStorageActionAttachmentMatchesHeader(BorrowCatalogTypedRecord(r));
}
inline bool CatalogStorageActionAttachmentMatchesMetadata(const CatalogMetadataVersion& m){
  if(!CatalogStorageActionAttachmentMatchesHeader(m.record)||m.object_subtype!="storage_action_attachment"||
      m.authority_scope!=CatalogAuthorityScope::local||!storage_action_policy_detail::Identity(m.owning_schema_uuid,UuidKind::schema)||
      !storage_action_policy_detail::Identity(m.record.header.parent_uuid,UuidKind::object)||m.owning_schema_uuid.value!=m.record.header.parent_uuid.value||
      !storage_action_policy_detail::Identity(m.default_name_uuid,UuidKind::object)||!storage_action_policy_detail::Identity(m.name_vector_uuid,UuidKind::object)||
      !storage_action_policy_detail::Identity(m.creator_transaction_uuid,UuidKind::transaction))return false;
  const auto decoded=DecodeCatalogStorageActionAttachment(m.record.payload);const auto& r=*decoded.record;
  return r.generation==m.definition_version&&
    (m.definition_version!=1||(r.origin_transaction_uuid.value==m.creator_transaction_uuid.value&&r.origin_local_transaction_id==m.creator_local_transaction_id));
}
inline bool CatalogStorageActionAttachmentPreservesOrigin(const CatalogMetadataVersion& a,const CatalogMetadataVersion& b){
  const auto family=[](const auto& m){return m.object_subtype=="storage_action_attachment"||IsCatalogStorageActionAttachmentPayload(m.record.payload);};
  if(!family(a)&&!family(b))return true;
  if(!CatalogStorageActionAttachmentMatchesMetadata(a)||!CatalogStorageActionAttachmentMatchesMetadata(b))return false;
  const auto x=DecodeCatalogStorageActionAttachment(a.record.payload),y=DecodeCatalogStorageActionAttachment(b.record.payload);
  return x.record->attachment_uuid==y.record->attachment_uuid&&x.record->database_uuid==y.record->database_uuid&&
    x.record->filespace_uuid==y.record->filespace_uuid&&x.record->storage_profile_uuid==y.record->storage_profile_uuid&&
    x.record->page_size_profile_uuid==y.record->page_size_profile_uuid&&
    x.record->origin_transaction_uuid.value==y.record->origin_transaction_uuid.value&&x.record->origin_local_transaction_id==y.record->origin_local_transaction_id;
}
} // namespace scratchbird::core::catalog
