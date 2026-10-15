// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_construction_audit.hpp"
#include "uuid.hpp"
#include <array>
#include <limits>

namespace scratchbird::storage::database {
namespace c=core::catalog;
using namespace core::platform;
using E=NativeConstructionAuditError;
std::optional<u64> NativeConstructionAuditTimeMicros(WallClockTime time) noexcept {
  if(time.unix_seconds<0||time.nanoseconds>=1000000000U)return {};
  const u64 seconds=static_cast<u64>(time.unix_seconds),fraction=time.nanoseconds/1000;
  if(seconds>(std::numeric_limits<u64>::max()-fraction)/1000000)return {};
  const auto result=seconds*1000000+fraction;if(!result)return {};return result;
}
NativeConstructionAuditResult AcquireNativeConstructionIntent(
    const NativeFilespaceInitializationRequest& r,const Uuid& engine,core::uuid::StandaloneUuidV7Issuer& issuer) noexcept {
  const auto& b=r.bootstrap;
  if(b.flags&disk::FilespaceBootstrapFlag::cluster_authority_required)return {E::cluster_requires_authority};
  if(disk::ValidateFilespaceBootstrap(b)!=disk::FilespaceBootstrapError::none||b.filespace_role!=1||b.lifecycle_state!=7||
      r.creator.transaction_uuid.kind!=UuidKind::transaction||r.creator.local_id.value!=1||
      r.creator.scope!=transaction::mga::TransactionScope::local_node||!r.total_pages||
      issuer.binding().database_uuid!=b.database_uuid||issuer.binding().policy_snapshot_uuid!=r.policy_snapshot_uuid)
    return {E::invalid_request};
  const std::array<Uuid,9> reserved{b.database_uuid,b.filespace_uuid,b.page_size_profile_uuid,b.checksum_profile_uuid,
      r.operation_uuid,r.writer_uuid,r.policy_snapshot_uuid,r.creator.transaction_uuid.value,engine};
  // writer and engine are not interchangeable. Their relation is established
  // by the actual enclosing runtime owner, not by replacing one with the other.
  for(std::size_t i=0;i<reserved.size();++i){if(!core::uuid::IsEngineIdentityUuid(reserved[i]))return {E::invalid_request};
    for(std::size_t j=0;j<i;++j)if(reserved[i]==reserved[j]&&!(i==8&&j==5))return {E::invalid_request};}
  const auto event=issuer.Issue(UuidKind::object);
  if(!event.ok()){
    const bool clock=event.error==core::uuid::StandaloneUuidV7Error::clock_failure||
        event.error==core::uuid::StandaloneUuidV7Error::timestamp_out_of_range||
        event.error==core::uuid::StandaloneUuidV7Error::time_regression;
    return {clock?E::time_source_failure:E::identity_failure,event.error};
  }
  const auto time=NativeConstructionAuditTimeMicros(event.observation->wall_clock);
  if(!time)return {E::time_source_failure};
  const auto row=issuer.Issue(UuidKind::row);
  if(!row.ok()){
    const bool clock=row.error==core::uuid::StandaloneUuidV7Error::clock_failure||
        row.error==core::uuid::StandaloneUuidV7Error::timestamp_out_of_range||
        row.error==core::uuid::StandaloneUuidV7Error::time_regression;
    return {clock?E::time_source_failure:E::identity_failure,row.error};
  }
  for(const auto& id:reserved)if(event.value->value==id||row.value->value==id)return {E::identity_failure};
  if(event.value->value==row.value->value)return {E::identity_failure};
  c::CatalogConstructionIntent value;
  value.event_uuid=event.value->value;value.database_uuid=b.database_uuid;value.engine_uuid=engine;
  value.operation_uuid=r.operation_uuid;value.filespace_uuid=b.filespace_uuid;
  value.origin_transaction_uuid=r.creator.transaction_uuid;value.origin_local_transaction_id=1;
  value.event_time_utc_us=*time;value.catalog_generation=1;
  value.event=c::ConstructionAuditEvent::construction_intent;value.event_class=c::ConstructionAuditClass::bootstrap_construction;
  value.policy_decision=c::ConstructionAuditDecision::not_evaluated;value.redaction=c::ConstructionAuditRedaction::safe_message_only;
  return {E::none,core::uuid::StandaloneUuidV7Error::none,value,*row.value,event.observation};
}
bool NativeConstructionIntentMatchesRequest(const c::CatalogMetadataVersionView& m,const NativeFilespaceInitializationRequest& r){
  if(m.object_subtype!="node_construction_intent"&&!c::IsCatalogConstructionIntentPayload(m.record.payload))return true;
  const auto value=c::DecodeCatalogConstructionIntent(m.record.payload);
  return value.ok()&&c::CatalogConstructionIntentMatchesDatabase(m,r.bootstrap.database_uuid)&&
      value.record->filespace_uuid==r.bootstrap.filespace_uuid&&value.record->operation_uuid==r.operation_uuid&&
      value.record->origin_transaction_uuid.value==r.creator.transaction_uuid.value&&
      value.record->origin_local_transaction_id==r.creator.local_id.value;
}
} // namespace scratchbird::storage::database
