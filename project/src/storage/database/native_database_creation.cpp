// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_database_creation.hpp"
#include "catalog_filespace_record_codec.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <stdexcept>

namespace scratchbird::storage::database {
namespace {
namespace c=core::catalog;
using namespace core::platform;
using E=NativeInitialDatabaseError;
NativeInitialDatabaseResult Fail(E e,c::CatalogNamedCreationError n=c::CatalogNamedCreationError::none) {
  return {e,n,{}};
}
E NameError(c::CatalogNamedCreationError e) {
  switch(e) {
    case c::CatalogNamedCreationError::none:return E::none;
    case c::CatalogNamedCreationError::invalid_request:return E::invalid_request;
    case c::CatalogNamedCreationError::invalid_metadata:return E::invalid_definition;
    case c::CatalogNamedCreationError::invalid_names:return E::invalid_names;
    case c::CatalogNamedCreationError::duplicate_identity:return E::duplicate_identity;
    case c::CatalogNamedCreationError::resource_exhausted:return E::resource_exhausted;
  }
  return E::invalid_request;
}
c::CatalogFilespaceRecord PrimaryDefinition(const NativeFilespaceInitializationRequest& physical) {
  c::CatalogFilespaceRecord r;
  r.filespace_uuid={UuidKind::filespace,physical.bootstrap.filespace_uuid};
  r.database_uuid={UuidKind::database,physical.bootstrap.database_uuid};
  r.filespace_role=physical.bootstrap.filespace_role;
  r.first_filespace=r.startup_authority=r.catalog_persistence_owner=
      r.filespace_manifest_owner=r.recovery_evidence_owner=true;
  r.state=physical.bootstrap.lifecycle_state;r.physical_filespace_id=0;
  r.lifecycle_generation=r.filespace_manifest_generation=r.catalog_manifest_format_version=
      r.resource_seed_manifest_format_version=1;
  r.registered_txn=r.last_lifecycle_transaction=r.creator_transaction_number=physical.creator.local_id.value;
  r.uuid_source=1;
  r.header_database_uuid_match_required=r.header_filespace_uuid_match_required=r.startup_state_coupled=
      r.page_header_coupled=r.open_validate_header=r.attach_admission_validate_header=
      r.transaction_admission_validate_filespace=r.maintenance_validate_header=r.verify_repair_validate_header=
      r.shutdown_validate_header=r.recovery_validate_header=r.drop_requires_database_lifecycle=
      r.quarantine_on_ambiguous=r.state_change_evidence_before_success=r.mga_visibility_required=
      r.path_is_locator_not_identity=r.duplicate_identity_refusal=r.stale_identity_refusal=true;
  return r;
}
}
static NativeInitialDatabaseResult AssembleNativeInitialDatabaseRecordsImpl(
    const NativeInitialDatabaseRequest& r,const c::CatalogNamedCreationInput* primary) noexcept {
  try {
    const auto& named=r.definition;
    if(!r.construction||!named.definition||!named.vector_metadata||!named.vector||
        named.entries.empty()||!named.entries.data()||r.database_default_language.empty()||
        !r.database_default_language.data()||r.database_default_language.size()>131040||
        !r.limits.maximum_records||!r.limits.maximum_encoded_bytes)return Fail(E::invalid_request);
    const auto& physical=*r.construction;const auto& b=physical.bootstrap;
    if(disk::ValidateFilespaceBootstrap(b)!=disk::FilespaceBootstrapError::none||
        b.filespace_role!=1||b.lifecycle_state!=7||
        physical.creator.transaction_uuid.kind!=UuidKind::transaction||
        physical.creator.local_id.value!=1||
        physical.creator.scope!=transaction::mga::TransactionScope::local_node||
        !physical.creation_utc_millis||physical.creation_utc_millis>0xffffffffffffULL||
        !physical.total_pages)return Fail(E::invalid_construction);
    const std::array<Uuid,8> enclosing{b.database_uuid,b.filespace_uuid,b.page_size_profile_uuid,
        b.checksum_profile_uuid,physical.operation_uuid,physical.writer_uuid,
        physical.creator.transaction_uuid.value,physical.policy_snapshot_uuid};
    for(std::size_t i=0;i<enclosing.size();++i) {
      if(!core::uuid::IsEngineIdentityUuid(enclosing[i]))return Fail(E::invalid_construction);
      for(std::size_t j=0;j<i;++j)if(enclosing[i]==enclosing[j])return Fail(E::invalid_construction);
    }
    const auto& metadata=*named.definition;
    if(metadata.record.header.kind!=c::CatalogRecordKind::database||
        metadata.object_subtype!="database"||!metadata.record.payload.empty()||
        !metadata.record.header.parent_uuid.value.is_nil()||!metadata.owning_schema_uuid.value.is_nil())
      return Fail(E::invalid_definition);
    c::CatalogDatabaseRecord definition;
    definition.database_uuid={UuidKind::database,b.database_uuid};
    // Native SBFP V1 has no minor revision. This catalog format is the native
    // six-relation construction contract, not the legacy SBDBV001 manifest.
    definition.database_header_format_major=b.durable_format_generation;
    definition.database_header_format_minor=0;
    definition.catalog_manifest_format_version=1;
    definition.page_size=b.page_size_bytes;
    definition.creation_unix_epoch_millis=physical.creation_utc_millis;
    definition.creator_transaction_number=physical.creator.local_id.value;
    definition.datatype_cohort=r.datatype_cohort;
    std::array<byte,232> backing;
    const auto encoded=c::EncodeCatalogDatabaseRecordInto(definition,backing);
    if(!encoded.ok())return Fail(E::invalid_definition);
    std::array<c::CatalogMetadataVersionView,2> views;
    views[0]=c::BorrowCatalogMetadataVersion(metadata);
    views[0].record.payload={reinterpret_cast<const char*>(encoded.bytes.data()),encoded.bytes.size()};
    std::array<byte,464> primary_backing;
    const std::array<const c::CatalogNamedCreationInput*,2> supplied{&named,primary};
    const unsigned families=primary?2:1;
    if(primary) {
      if(!primary->definition||!primary->vector_metadata||!primary->vector||primary->entries.empty()||
          !primary->entries.data())return Fail(E::invalid_request);
      const auto& m=*primary->definition;
      if(m.record.header.kind!=c::CatalogRecordKind::filespace||m.object_subtype!="filespace"||
          !m.record.payload.empty()||m.record.header.parent_uuid.kind!=UuidKind::object||
          m.record.header.parent_uuid.value!=metadata.record.header.object_uuid.value||
          !m.owning_schema_uuid.value.is_nil())return Fail(E::invalid_definition);
      const auto manifest=PrimaryDefinition(physical);
      const auto payload=c::EncodeCatalogFilespaceRecordInto(manifest,primary_backing);
      if(!payload.ok())return Fail(E::invalid_definition);
      views[1]=c::BorrowCatalogMetadataVersion(m);
      views[1].record.payload={reinterpret_cast<const char*>(payload.bytes.data()),payload.bytes.size()};
    }
    u64 records=0,bytes=0;
    for(unsigned family=0;family<families;++family) {
      const auto& input=*supplied[family];
      const auto checked=c::PreflightLocalNamedCreationRecord(views[family],input,
          physical.creator.transaction_uuid,r.database_default_language,
          {r.limits.maximum_records-records,r.limits.maximum_encoded_bytes-bytes});
      if(checked.error!=c::CatalogNamedCreationError::none)return Fail(NameError(checked.error),checked.error);
      if(input.vector->object_class!=(family?"filespace":"database"))return Fail(E::invalid_names);
      for(const auto& entry:input.entries)
        if(entry.entry->scope_uuid.kind!=UuidKind::object||
            entry.entry->scope_uuid.value!=metadata.record.header.object_uuid.value||entry.entry->parent_object_uuid||
            entry.entry->parent_schema_uuid)return Fail(E::invalid_names);
      records+=checked.records;bytes+=checked.encoded_bytes;
    }
    // Do not allow any definition/name object or row to masquerade as the
    // primary, operation, issuer/source profile or actual construction owner.
    // Per-package duplicate admission remains in the shared named assembler.
    const auto distinct=[&](const c::CatalogMetadataVersion& m) {
      for(const auto& id:{m.record.header.object_uuid.value,m.record.header.row_uuid.value})
        if(id==r.datatype_cohort.catalog_snapshot_uuid.value||
            std::find(enclosing.begin(),enclosing.end(),id)!=enclosing.end())return false;
      return true;
    };
    for(unsigned family=0;family<families;++family) {
      const auto& input=*supplied[family];
      if(!distinct(*input.definition)||!distinct(*input.vector_metadata))return Fail(E::duplicate_identity);
      for(const auto& entry:input.entries)if(!distinct(*entry.metadata))return Fail(E::duplicate_identity);
    }
    if(records>std::numeric_limits<std::size_t>::max()/2)return Fail(E::resource_exhausted);
    const auto count=static_cast<std::size_t>(2*records);
    std::array<Uuid,64> small;
    std::vector<Uuid> large;
    if(count>small.size())large.resize(count);
    auto* ids=large.empty()?small.data():large.data();std::size_t at=0;
    const auto append=[&](const c::CatalogMetadataVersion& m) {
      ids[at++]=m.record.header.object_uuid.value;ids[at++]=m.record.header.row_uuid.value;
    };
    for(unsigned family=0;family<families;++family) {
      const auto& input=*supplied[family];append(*input.definition);append(*input.vector_metadata);
      for(const auto& entry:input.entries)append(*entry.metadata);
    }
    std::sort(ids,ids+count);
    if(std::adjacent_find(ids,ids+count)!=ids+count)return Fail(E::duplicate_identity);
    std::array<c::CatalogMetadataVersion,2> owned;
    std::array<c::CatalogNamedCreationInput,2> inputs;
    for(unsigned family=0;family<families;++family) {
      const auto& input=*supplied[family];owned[family]=*input.definition;
      owned[family].record.payload.assign(views[family].record.payload);
      inputs[family]={&owned[family],input.vector_metadata,input.vector,input.entries};
    }
    auto made=c::AssembleLocalNamedCreationRecords(b.database_uuid,physical.creator.transaction_uuid,
        r.database_default_language,std::span<const c::CatalogNamedCreationInput>(inputs).first(families),r.limits);
    if(!made.ok())return Fail(NameError(made.error),made.error);
    return {E::none,c::CatalogNamedCreationError::none,std::move(made.records)};
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}
   catch(const std::length_error&){return Fail(E::resource_exhausted);}
}
NativeInitialDatabaseResult AssembleNativeInitialDatabaseRecords(const NativeInitialDatabaseRequest& r) noexcept {
  return AssembleNativeInitialDatabaseRecordsImpl(r,nullptr);
}
NativeInitialDatabaseResult AssembleNativeInitialDatabaseFilespaceRecords(
    const NativeInitialDatabaseFilespaceRequest& r) noexcept {
  return AssembleNativeInitialDatabaseRecordsImpl(r.database,&r.primary_filespace);
}
} // namespace scratchbird::storage::database
