// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_transaction_policy.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <limits>

namespace scratchbird::core::catalog {
namespace {
using T=CatalogValueType;using E=CatalogValueError;
constexpr auto Fields(){
  std::array<CatalogValueFieldSchema,30> out{};
  for(u16 i=0;i<out.size();++i)out[i]={u16(i+1),T::unsigned_integer,true,8};
  for(const u16 id:{1,4,5,13,24})out[id-1]={id,T::engine_identity,true,16,UuidKind::object};
  out[1]={2,T::engine_identity,true,16,UuidKind::database};
  out[5]={6,T::engine_identity_list,true,1024,UuidKind::object};
  for(const u16 id:{26,28})out[id-1]={id,T::engine_identity,true,16,UuidKind::transaction};
  out[29]={30,T::engine_identity,false,16,UuidKind::cluster};return out;
}
constexpr auto fields=Fields();
constexpr CatalogValueSchemaView schema{kCatalogTransactionPolicySchemaId,1,fields};
bool Identity(const Uuid& id){return uuid::IsEngineIdentityUuid(id);}
bool Transaction(const TypedUuid& id){return id.kind==UuidKind::transaction&&Identity(id.value);}
auto Controls(const CatalogTransactionPolicyBody& r){
  return std::array{r.body_uuid,r.database_uuid,r.lock_policy_uuid,r.evidence_policy_uuid,
      r.origin_transaction_uuid.value,r.creating_transaction_uuid.value,r.cluster_uuid.value_or(Uuid{})};
}
// SB_DRIFT_TRANSACTION_POLICY_ZERO_IS_NO_LIMIT (owner 2026-11-04): zero means
// no limit for the transaction lifetime, snapshot age and active-transaction
// bounds, and is their default. Every attached user is always in a snapshot
// transaction, so a mandatory finite bound would cap users and end live work.
bool Valid(const CatalogTransactionPolicyBody& r) noexcept {
  if(!r.generation||!Transaction(r.origin_transaction_uuid)||!Transaction(r.creating_transaction_uuid)||
      !r.origin_local_transaction_id||r.creating_local_transaction_id<r.origin_local_transaction_id||
      ((r.origin_transaction_uuid.value==r.creating_transaction_uuid.value)!=
          (r.origin_local_transaction_id==r.creating_local_transaction_id))||
      (r.generation==1&&r.creating_local_transaction_id!=r.origin_local_transaction_id)||
      !r.allowed_isolation_profile_count||r.allowed_isolation_profile_count>r.allowed_isolation_profiles.size()||
      !r.read_mode_mask||(r.read_mode_mask&~u64{3})||r.deadlock_priority>255||
      u64(r.savepoint_mode)<1||u64(r.savepoint_mode)>2||
      (r.savepoint_mode==TransactionSavepointMode::disabled)!=(r.maximum_savepoints==0)||
      u64(r.autocommit_mode)<1||u64(r.autocommit_mode)>2||
      u64(r.cluster_eligibility)<1||u64(r.cluster_eligibility)>2||
      r.retention_behavior!=TransactionRetentionBehavior::pin_required_horizons_until_retirement||
      (r.reload_trigger_mask&~u64{15})||(r.reload_trigger_mask&7)!=7||
      r.limbo_behavior!=TransactionLimboBehavior::block_and_require_recovery||
      r.commit_ordering!=TransactionCommitOrdering::engine_commit_sequence||
      u64(r.restart_behavior)<1||u64(r.restart_behavior)>2||
      (r.restart_behavior==TransactionRestartBehavior::no_automatic_restart)!=(r.maximum_statement_restarts==0)||
      r.flags!=kTransactionPolicyMandatoryFlags)return false;
  const auto control=Controls(r);const auto count=r.cluster_uuid?control.size():control.size()-1;
  for(std::size_t i=0;i<count;++i){if(!Identity(control[i]))return false;
    for(std::size_t j=0;j<i;++j)if(control[i]==control[j]&&!(i==5&&j==4))return false;
  }
  bool default_found=false,startup_found=false;
  for(std::size_t i=0;i<r.allowed_isolation_profile_count;++i){const auto& id=r.allowed_isolation_profiles[i];
    if(!Identity(id)||(i&&!(r.allowed_isolation_profiles[i-1]<id)))return false;
    for(std::size_t j=0;j<count;++j)if(id==control[j])return false;
    default_found|=id==r.default_isolation_profile_uuid;startup_found|=id==r.startup_isolation_profile_uuid;
  }
  return default_found&&startup_found;
}
struct Backing {
  std::array<CatalogValueFieldView,30> views{};
  std::array<std::array<byte,8>,30> numbers{};
  std::array<byte,1024> profiles{};
  std::size_t count=0;
  void Id(u16 tag,const Uuid& id,UuidKind kind){views[count++]={tag,T::engine_identity,kind,id.bytes};}
  void Number(u16 tag,u64 n){auto& bytes=numbers[tag-1];platform::StoreLittle64(bytes.data(),n);
    views[count++]={tag,T::unsigned_integer,UuidKind::unknown,bytes};}
  explicit Backing(const CatalogTransactionPolicyBody& r){
    Id(1,r.body_uuid,UuidKind::object);Id(2,r.database_uuid,UuidKind::database);Number(3,r.generation);
    Id(4,r.default_isolation_profile_uuid,UuidKind::object);Id(5,r.startup_isolation_profile_uuid,UuidKind::object);
    for(std::size_t i=0;i<r.allowed_isolation_profile_count;++i)
      std::copy(r.allowed_isolation_profiles[i].bytes.begin(),r.allowed_isolation_profiles[i].bytes.end(),profiles.begin()+16*i);
    views[count++]={6,T::engine_identity_list,UuidKind::object,std::span<const byte>{profiles}.first(16*r.allowed_isolation_profile_count)};
    Number(7,r.read_mode_mask);Number(8,r.maximum_transaction_microseconds);Number(9,r.maximum_snapshot_microseconds);
    Number(10,r.maximum_active_transactions);Number(11,r.maximum_savepoints);Number(12,r.maximum_prepared_transactions);
    Id(13,r.lock_policy_uuid,UuidKind::object);Number(14,r.deadlock_priority);Number(15,u64(r.savepoint_mode));
    Number(16,u64(r.autocommit_mode));Number(17,u64(r.cluster_eligibility));Number(18,u64(r.retention_behavior));
    Number(19,r.reload_trigger_mask);Number(20,u64(r.limbo_behavior));Number(21,u64(r.commit_ordering));
    Number(22,u64(r.restart_behavior));Number(23,r.maximum_statement_restarts);Id(24,r.evidence_policy_uuid,UuidKind::object);
    Number(25,r.flags);Id(26,r.origin_transaction_uuid.value,UuidKind::transaction);Number(27,r.origin_local_transaction_id);
    Id(28,r.creating_transaction_uuid.value,UuidKind::transaction);Number(29,r.creating_local_transaction_id);
    if(r.cluster_uuid)Id(30,*r.cluster_uuid,UuidKind::cluster);
  }
  auto View()const noexcept{return std::span<const CatalogValueFieldView>{views}.first(count);}
};
bool Header(const CatalogTypedRecordView& m,const CatalogTransactionPolicyBody& r){
  if(m.header.kind!=CatalogRecordKind::policy||m.header.object_uuid.kind!=UuidKind::object||m.header.object_uuid.value!=r.body_uuid||
      m.header.row_uuid.kind!=UuidKind::row||!Identity(m.header.row_uuid.value))return false;
  for(const auto& id:Controls(r))if(m.header.row_uuid.value==id)return false;
  for(std::size_t i=0;i<r.allowed_isolation_profile_count;++i)if(m.header.row_uuid.value==r.allowed_isolation_profiles[i])return false;
  return true;
}
bool Metadata(const CatalogMetadataVersionView& m,const CatalogTransactionPolicyBody& r){
  return Header(m.record,r)&&m.object_subtype==CatalogObjectSubtype::transaction_policy_body&&m.definition_version==r.generation&&
      m.creator_transaction_uuid.kind==UuidKind::transaction&&m.creator_transaction_uuid.value==r.creating_transaction_uuid.value&&
      m.creator_local_transaction_id==r.creating_local_transaction_id&&
      (m.authority_scope==CatalogAuthorityScope::cluster)==r.cluster_uuid.has_value();
}
bool Family(const CatalogMetadataVersionView& m){return m.object_subtype==CatalogObjectSubtype::transaction_policy_body||IsCatalogTransactionPolicyPayload(m.record.payload);}
}
std::optional<std::size_t> CatalogTransactionPolicyEncodedBytes(const CatalogTransactionPolicyBody& r) noexcept {
  if(!Valid(r))return {};const Backing b(r);if(ValidateCatalogValueFields(schema,b.View())!=E::none)return {};
  std::size_t bytes=kCatalogValueBlockHeaderBytes;for(const auto& field:b.View())bytes+=8+field.bytes.size();return bytes;
}
CatalogValueEncodeIntoResult EncodeCatalogTransactionPolicyInto(const CatalogTransactionPolicyBody& r,std::span<byte> output) noexcept {
  if(!Valid(r))return {E::invalid_value,{}};
  const auto dest=reinterpret_cast<std::uintptr_t>(output.data()),source=reinterpret_cast<std::uintptr_t>(&r);
  if((output.size()&&!output.data())||output.size()>std::numeric_limits<std::uintptr_t>::max()-dest||
      (dest<source+sizeof(r)&&source<dest+output.size()))return {E::invalid_backing,{}};
  const Backing b(r);return EncodeCatalogValueBlockInto(schema,b.View(),output);
}
CatalogTransactionPolicyResult DecodeCatalogTransactionPolicy(std::string_view bytes) noexcept {
  std::array<CatalogValueFieldView,30> backing;
  const auto parsed=DecodeCatalogValueBlockInto(schema,{reinterpret_cast<const byte*>(bytes.data()),bytes.size()},backing);
  if(!parsed.ok())return {parsed.error,{}};CatalogTransactionPolicyBody r;
  for(const auto& f:parsed.fields)switch(f.id){
    case 1:r.body_uuid=f.identity()->value;break;case 2:r.database_uuid=f.identity()->value;break;case 3:r.generation=*f.unsigned_value();break;
    case 4:r.default_isolation_profile_uuid=f.identity()->value;break;case 5:r.startup_isolation_profile_uuid=f.identity()->value;break;
    case 6:r.allowed_isolation_profile_count=f.bytes.size()/16;
      for(std::size_t i=0;i<r.allowed_isolation_profile_count;++i)std::copy_n(f.bytes.begin()+16*i,16,r.allowed_isolation_profiles[i].bytes.begin());break;
    case 7:r.read_mode_mask=*f.unsigned_value();break;case 8:r.maximum_transaction_microseconds=*f.unsigned_value();break;
    case 9:r.maximum_snapshot_microseconds=*f.unsigned_value();break;case 10:r.maximum_active_transactions=*f.unsigned_value();break;
    case 11:r.maximum_savepoints=*f.unsigned_value();break;case 12:r.maximum_prepared_transactions=*f.unsigned_value();break;
    case 13:r.lock_policy_uuid=f.identity()->value;break;case 14:r.deadlock_priority=*f.unsigned_value();break;
    case 15:r.savepoint_mode=TransactionSavepointMode(*f.unsigned_value());break;
    case 16:r.autocommit_mode=TransactionAutocommitMode(*f.unsigned_value());break;
    case 17:r.cluster_eligibility=TransactionClusterEligibility(*f.unsigned_value());break;
    case 18:r.retention_behavior=TransactionRetentionBehavior(*f.unsigned_value());break;case 19:r.reload_trigger_mask=*f.unsigned_value();break;
    case 20:r.limbo_behavior=TransactionLimboBehavior(*f.unsigned_value());break;case 21:r.commit_ordering=TransactionCommitOrdering(*f.unsigned_value());break;
    case 22:r.restart_behavior=TransactionRestartBehavior(*f.unsigned_value());break;case 23:r.maximum_statement_restarts=*f.unsigned_value();break;
    case 24:r.evidence_policy_uuid=f.identity()->value;break;case 25:r.flags=*f.unsigned_value();break;
    case 26:r.origin_transaction_uuid=*f.identity();break;case 27:r.origin_local_transaction_id=*f.unsigned_value();break;
    case 28:r.creating_transaction_uuid=*f.identity();break;case 29:r.creating_local_transaction_id=*f.unsigned_value();break;
    case 30:r.cluster_uuid=f.identity()->value;break;
  }
  if(!Valid(r))return {E::invalid_value,{}};return {E::none,r};
}
bool IsCatalogTransactionPolicyPayload(std::string_view bytes) noexcept {
  return bytes.size()>=kCatalogValueBlockHeaderBytes&&bytes.substr(0,4)=="SBCV"&&
      platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data())+16)==kCatalogTransactionPolicySchemaId;
}
bool CatalogTransactionPolicyMatchesHeader(const CatalogTypedRecordView& m) noexcept {const auto r=DecodeCatalogTransactionPolicy(m.payload);return r.ok()&&Header(m,*r.record);}
bool CatalogTransactionPolicyMatchesMetadata(const CatalogMetadataVersionView& m) noexcept {const auto r=DecodeCatalogTransactionPolicy(m.record.payload);return r.ok()&&Metadata(m,*r.record);}
bool CatalogTransactionPolicyMatchesDatabase(const CatalogMetadataVersionView& m,const Uuid& db) noexcept {
  if(!Family(m))return true;const auto r=DecodeCatalogTransactionPolicy(m.record.payload);return Identity(db)&&r.ok()&&Metadata(m,*r.record)&&r.record->database_uuid==db;
}
bool CatalogTransactionPolicyPreservesOrigin(const CatalogMetadataVersionView& a,const CatalogMetadataVersionView& b) noexcept {
  if(!Family(a)&&!Family(b))return true;const auto x=DecodeCatalogTransactionPolicy(a.record.payload),y=DecodeCatalogTransactionPolicy(b.record.payload);
  if(!x.ok()||!y.ok()||!Metadata(a,*x.record)||!Metadata(b,*y.record))return false;const auto& p=*x.record;const auto& q=*y.record;
  return p.body_uuid==q.body_uuid&&p.database_uuid==q.database_uuid&&p.origin_transaction_uuid.value==q.origin_transaction_uuid.value&&
      p.origin_local_transaction_id==q.origin_local_transaction_id&&((p.generation==q.generation&&a.record.payload==b.record.payload)||q.generation>p.generation);
}
bool CatalogTransactionPolicyMatchesDescriptor(const CatalogTransactionPolicyBody& body,const CatalogPolicyDescriptor& descriptor) noexcept {
  return Valid(body)&&CatalogPolicyDescriptorEncodedBytes(descriptor).has_value()&&descriptor.policy_class==CatalogPolicyClass::transaction&&
      descriptor.database_uuid==body.database_uuid&&descriptor.body_uuid==body.body_uuid&&descriptor.body_generation==body.generation&&
      descriptor.cluster_uuid==body.cluster_uuid;
}
} // namespace scratchbird::core::catalog
