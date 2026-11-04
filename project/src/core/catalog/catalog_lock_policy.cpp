// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_lock_policy.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <limits>

namespace scratchbird::core::catalog {
namespace {
using T=CatalogValueType;using E=CatalogValueError;
constexpr auto Fields(){
  std::array<CatalogValueFieldSchema,30> out{};
  for(u16 i=0;i<out.size();++i)out[i]={u16(i+1),T::unsigned_integer,true,8};
  for(const u16 id:{1,22})out[id-1]={id,T::engine_identity,true,16,UuidKind::object};
  out[1]={2,T::engine_identity,true,16,UuidKind::database};
  for(const u16 id:{23,24})out[id-1]={id,T::engine_identity,false,16,UuidKind::object};
  for(const u16 id:{25,27})out[id-1]={id,T::engine_identity,true,16,UuidKind::transaction};
  out[28]={29,T::engine_identity,false,16,UuidKind::cluster};return out;
}
constexpr auto fields=Fields();
constexpr CatalogValueSchemaView schema{kCatalogLockPolicySchemaId,1,fields};
bool Identity(const Uuid& id){return uuid::IsEngineIdentityUuid(id);}
bool Transaction(const TypedUuid& id){return id.kind==UuidKind::transaction&&Identity(id.value);}
template<class V>bool Range(V value,u64 maximum){return u64(value)>=1&&u64(value)<=maximum;}
auto Controls(const CatalogLockPolicyBody& r){
  return std::array{r.body_uuid,r.database_uuid,r.evidence_policy_uuid,
      r.donor_mapping_uuid.value_or(Uuid{}),r.maintenance_window_policy_uuid.value_or(Uuid{}),
      r.origin_transaction_uuid.value,r.creating_transaction_uuid.value,r.cluster_uuid.value_or(Uuid{})};
}
// SB_DRIFT_LOCK_POLICY_ZERO_IS_NO_LIMIT (owner 2026-11-04): zero means no
// limit for the lock count, lock memory, wait-graph size and deadlock work,
// and is their default; an escalation threshold is capped only by a nonzero
// maximum. The fairness threshold and detection interval are not limits.
bool Valid(const CatalogLockPolicyBody& r) noexcept {
  if(!r.generation||!Transaction(r.origin_transaction_uuid)||!Transaction(r.creating_transaction_uuid)||
      !r.origin_local_transaction_id||r.creating_local_transaction_id<r.origin_local_transaction_id||
      ((r.origin_transaction_uuid.value==r.creating_transaction_uuid.value)!=
          (r.origin_local_transaction_id==r.creating_local_transaction_id))||
      (r.generation==1&&r.creating_local_transaction_id!=r.origin_local_transaction_id)||
      !Range(r.wait_behavior,9)||!Range(r.fairness,2)||!r.fairness_threshold_microseconds||
      !r.deadlock_detection_interval_microseconds||
      r.victim_policy!=LockVictimPolicy::core_deterministic_logical_lock_order||
      !Range(r.victim_outcome,7)||!Range(r.timeout_outcome,5)||!Range(r.predicate_guard,2)||
      (r.conversion_priority&&r.fairness!=LockQueueFairness::priority_with_starvation_prevention)||
      (r.maximum_locks&&r.escalation_lock_threshold>r.maximum_locks)||
      (r.maximum_lock_memory_bytes&&r.escalation_memory_threshold>r.maximum_lock_memory_bytes)||
      r.flags!=kLockPolicyMandatoryFlags)return false;
  const bool no_wait=r.wait_behavior==LockWaitBehavior::no_wait;
  const bool forever=r.wait_behavior==LockWaitBehavior::wait_forever;
  if((no_wait||forever)!=(r.wait_timeout_microseconds==0)||no_wait!=(r.maximum_waiters==0))return false;
  if((r.wait_behavior==LockWaitBehavior::donor_compatible_wait||r.victim_outcome==LockVictimOutcome::donor_compatible_error||
      r.timeout_outcome==LockTimeoutOutcome::donor_compatible_error)&&!r.donor_mapping_uuid)return false;
  if(r.wait_behavior==LockWaitBehavior::maintenance_window_wait&&!r.maintenance_window_policy_uuid)return false;
  if(r.victim_outcome==LockVictimOutcome::cluster_recovery_required&&!r.cluster_uuid)return false;
  const auto control=Controls(r);
  for(std::size_t i=0;i<control.size();++i){
    if((i==3&&!r.donor_mapping_uuid)||(i==4&&!r.maintenance_window_policy_uuid)||(i==7&&!r.cluster_uuid))continue;
    if(!Identity(control[i]))return false;
    for(std::size_t j=0;j<i;++j)if(control[i]==control[j]&&!(i==6&&j==5))return false;
  }
  return true;
}
struct Backing {
  std::array<CatalogValueFieldView,30> views{};
  std::array<std::array<byte,8>,30> numbers{};
  std::size_t count=0;
  void Id(u16 tag,const Uuid& id,UuidKind kind){views[count++]={tag,T::engine_identity,kind,id.bytes};}
  void Number(u16 tag,u64 n){auto& bytes=numbers[tag-1];platform::StoreLittle64(bytes.data(),n);
    views[count++]={tag,T::unsigned_integer,UuidKind::unknown,bytes};}
  explicit Backing(const CatalogLockPolicyBody& r){
    Id(1,r.body_uuid,UuidKind::object);
    Id(2,r.database_uuid,UuidKind::database);
    Number(3,u64(r.generation));
    Number(4,u64(r.wait_behavior));
    Number(5,u64(r.wait_timeout_microseconds));
    Number(6,u64(r.fairness));
    Number(7,u64(r.fairness_threshold_microseconds));
    Number(8,u64(r.maximum_locks));
    Number(9,u64(r.maximum_waiters));
    Number(10,u64(r.maximum_lock_memory_bytes));
    Number(11,u64(r.maximum_graph_nodes));
    Number(12,u64(r.maximum_graph_edges));
    Number(13,u64(r.maximum_deadlock_work));
    Number(14,u64(r.deadlock_detection_interval_microseconds));
    Number(15,u64(r.victim_policy));
    Number(16,u64(r.victim_outcome));
    Number(17,u64(r.timeout_outcome));
    Number(18,u64(r.conversion_priority));
    Number(19,u64(r.escalation_lock_threshold));
    Number(20,u64(r.escalation_memory_threshold));
    Number(21,u64(r.predicate_guard));
    Id(22,r.evidence_policy_uuid,UuidKind::object);
    if(r.donor_mapping_uuid)Id(23,*r.donor_mapping_uuid,UuidKind::object);
    if(r.maintenance_window_policy_uuid)Id(24,*r.maintenance_window_policy_uuid,UuidKind::object);
    Id(25,r.origin_transaction_uuid.value,UuidKind::transaction);
    Number(26,u64(r.origin_local_transaction_id));
    Id(27,r.creating_transaction_uuid.value,UuidKind::transaction);
    Number(28,u64(r.creating_local_transaction_id));
    if(r.cluster_uuid)Id(29,*r.cluster_uuid,UuidKind::cluster);
    Number(30,u64(r.flags));
  }
  auto View()const noexcept{return std::span<const CatalogValueFieldView>{views}.first(count);}
};
bool Header(const CatalogTypedRecordView& m,const CatalogLockPolicyBody& r){
  if(m.header.kind!=CatalogRecordKind::policy||m.header.object_uuid.kind!=UuidKind::object||m.header.object_uuid.value!=r.body_uuid||
      m.header.row_uuid.kind!=UuidKind::row||!Identity(m.header.row_uuid.value))return false;
  for(const auto& id:Controls(r))if(m.header.row_uuid.value==id)return false;
  return true;
}
bool Metadata(const CatalogMetadataVersionView& m,const CatalogLockPolicyBody& r){
  return Header(m.record,r)&&m.object_subtype=="lock_policy_body"&&m.definition_version==r.generation&&
      m.creator_transaction_uuid.kind==UuidKind::transaction&&m.creator_transaction_uuid.value==r.creating_transaction_uuid.value&&
      m.creator_local_transaction_id==r.creating_local_transaction_id&&
      (m.authority_scope==CatalogAuthorityScope::cluster)==r.cluster_uuid.has_value();
}
bool Family(const CatalogMetadataVersionView& m){return m.object_subtype=="lock_policy_body"||IsCatalogLockPolicyPayload(m.record.payload);}
}
std::optional<std::size_t> CatalogLockPolicyEncodedBytes(const CatalogLockPolicyBody& r) noexcept {
  if(!Valid(r))return {};const Backing b(r);if(ValidateCatalogValueFields(schema,b.View())!=E::none)return {};
  std::size_t bytes=kCatalogValueBlockHeaderBytes;for(const auto& field:b.View())bytes+=8+field.bytes.size();return bytes;
}
CatalogValueEncodeIntoResult EncodeCatalogLockPolicyInto(const CatalogLockPolicyBody& r,std::span<byte> output) noexcept {
  if(!Valid(r))return {E::invalid_value,{}};
  const auto dest=reinterpret_cast<std::uintptr_t>(output.data()),source=reinterpret_cast<std::uintptr_t>(&r);
  if((output.size()&&!output.data())||output.size()>std::numeric_limits<std::uintptr_t>::max()-dest||
      (dest<source+sizeof(r)&&source<dest+output.size()))return {E::invalid_backing,{}};
  const Backing b(r);return EncodeCatalogValueBlockInto(schema,b.View(),output);
}
CatalogLockPolicyResult DecodeCatalogLockPolicy(std::string_view bytes) noexcept {
  std::array<CatalogValueFieldView,30> backing;
  const auto parsed=DecodeCatalogValueBlockInto(schema,{reinterpret_cast<const byte*>(bytes.data()),bytes.size()},backing);
  if(!parsed.ok())return {parsed.error,{}};CatalogLockPolicyBody r;
  for(const auto& f:parsed.fields)switch(f.id){
    case 1:r.body_uuid=f.identity()->value;break;
    case 2:r.database_uuid=f.identity()->value;break;
    case 3:r.generation=*f.unsigned_value();break;
    case 4:r.wait_behavior=LockWaitBehavior(*f.unsigned_value());break;
    case 5:r.wait_timeout_microseconds=*f.unsigned_value();break;
    case 6:r.fairness=LockQueueFairness(*f.unsigned_value());break;
    case 7:r.fairness_threshold_microseconds=*f.unsigned_value();break;
    case 8:r.maximum_locks=*f.unsigned_value();break;
    case 9:r.maximum_waiters=*f.unsigned_value();break;
    case 10:r.maximum_lock_memory_bytes=*f.unsigned_value();break;
    case 11:r.maximum_graph_nodes=*f.unsigned_value();break;
    case 12:r.maximum_graph_edges=*f.unsigned_value();break;
    case 13:r.maximum_deadlock_work=*f.unsigned_value();break;
    case 14:r.deadlock_detection_interval_microseconds=*f.unsigned_value();break;
    case 15:r.victim_policy=LockVictimPolicy(*f.unsigned_value());break;
    case 16:r.victim_outcome=LockVictimOutcome(*f.unsigned_value());break;
    case 17:r.timeout_outcome=LockTimeoutOutcome(*f.unsigned_value());break;
    case 18:if(*f.unsigned_value()>1)return {E::invalid_value,{}};r.conversion_priority=*f.unsigned_value()!=0;break;
    case 19:r.escalation_lock_threshold=*f.unsigned_value();break;
    case 20:r.escalation_memory_threshold=*f.unsigned_value();break;
    case 21:r.predicate_guard=LockPredicateGuard(*f.unsigned_value());break;
    case 22:r.evidence_policy_uuid=f.identity()->value;break;
    case 23:r.donor_mapping_uuid=f.identity()->value;break;
    case 24:r.maintenance_window_policy_uuid=f.identity()->value;break;
    case 25:r.origin_transaction_uuid=*f.identity();break;
    case 26:r.origin_local_transaction_id=*f.unsigned_value();break;
    case 27:r.creating_transaction_uuid=*f.identity();break;
    case 28:r.creating_local_transaction_id=*f.unsigned_value();break;
    case 29:r.cluster_uuid=f.identity()->value;break;
    case 30:r.flags=*f.unsigned_value();break;
  }
  if(!Valid(r))return {E::invalid_value,{}};return {E::none,r};
}
bool IsCatalogLockPolicyPayload(std::string_view bytes) noexcept {
  return bytes.size()>=kCatalogValueBlockHeaderBytes&&bytes.substr(0,4)=="SBCV"&&
      platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data())+16)==kCatalogLockPolicySchemaId;
}
bool CatalogLockPolicyMatchesHeader(const CatalogTypedRecordView& m) noexcept {const auto r=DecodeCatalogLockPolicy(m.payload);return r.ok()&&Header(m,*r.record);}
bool CatalogLockPolicyMatchesMetadata(const CatalogMetadataVersionView& m) noexcept {const auto r=DecodeCatalogLockPolicy(m.record.payload);return r.ok()&&Metadata(m,*r.record);}
bool CatalogLockPolicyMatchesDatabase(const CatalogMetadataVersionView& m,const Uuid& db) noexcept {
  if(!Family(m))return true;const auto r=DecodeCatalogLockPolicy(m.record.payload);return Identity(db)&&r.ok()&&Metadata(m,*r.record)&&r.record->database_uuid==db;
}
bool CatalogLockPolicyPreservesOrigin(const CatalogMetadataVersionView& a,const CatalogMetadataVersionView& b) noexcept {
  if(!Family(a)&&!Family(b))return true;const auto x=DecodeCatalogLockPolicy(a.record.payload),y=DecodeCatalogLockPolicy(b.record.payload);
  if(!x.ok()||!y.ok()||!Metadata(a,*x.record)||!Metadata(b,*y.record))return false;const auto& p=*x.record;const auto& q=*y.record;
  return p.body_uuid==q.body_uuid&&p.database_uuid==q.database_uuid&&p.origin_transaction_uuid.value==q.origin_transaction_uuid.value&&
      p.origin_local_transaction_id==q.origin_local_transaction_id&&((p.generation==q.generation&&a.record.payload==b.record.payload)||q.generation>p.generation);
}
bool CatalogLockPolicyMatchesDescriptor(const CatalogLockPolicyBody& body,const CatalogPolicyDescriptor& descriptor) noexcept {
  return Valid(body)&&CatalogPolicyDescriptorEncodedBytes(descriptor).has_value()&&descriptor.policy_class==CatalogPolicyClass::lock_admission&&
      descriptor.database_uuid==body.database_uuid&&descriptor.body_uuid==body.body_uuid&&descriptor.body_generation==body.generation&&
      descriptor.cluster_uuid==body.cluster_uuid;
}
} // namespace scratchbird::core::catalog
