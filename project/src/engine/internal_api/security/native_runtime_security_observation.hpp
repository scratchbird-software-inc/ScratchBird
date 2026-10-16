// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "api_types.hpp"
#include "management/runtime_authority_binding.hpp"
#include "catalog_security_record_codec.hpp"
#include <limits>

namespace scratchbird::engine::internal_api {
enum class NativeRuntimeSecurityReadError {
  none, invalid_request, backing_unavailable, binding_source_failure,
  binding_absent, binding_ambiguous, binding_invalid, binding_not_active,
  binding_generation_mismatch, external_authority_required, security_source_failure,
  security_record_invalid, origin_not_committed, duplicate_identity,
  principal_absent, principal_wrong_family, principal_not_active,
  principal_not_service, principal_generation_invalid, resource_exhausted
};

// Exact observed records, NOT effective privileges or authentication. Attribute
// views borrow the governed catalog backing. Retain the actual acquiring-thread
// source lease AND this workspace, without reuse, for every access. No name or
// verifier is copied into an ordinary owning string. Retired records, denies,
// memberships and other principals remain visible to the later security owner.
// This observes the four native user/group/role/grant families, not policy
// execution, external authority, private-event reconciliation or whole-node
// activation. Other catalog families cannot silently become privilege rows.
struct NativeRuntimeSecurityRecordObservation {
  core::catalog::CatalogSecurityRecordView record;
  EngineUuid object_uuid, row_uuid, version_uuid, previous_version_uuid;
  EngineUuid origin_transaction_uuid, creator_transaction_uuid, retired_transaction_uuid;
  core::platform::u64 origin_local_transaction_id=0, creator_local_transaction_id=0;
  core::platform::u64 retired_local_transaction_id=0, definition_generation=0;
  core::platform::u64 catalog_generation=0, security_epoch=0;
  core::catalog::CatalogObjectLifecycle lifecycle=core::catalog::CatalogObjectLifecycle::creating;
  core::catalog::CatalogObjectStatus status=core::catalog::CatalogObjectStatus::proposed;
  bool deleted=false;
};
struct NativeRuntimeServicePrincipalObservation {
  EngineUuid principal_uuid, version_uuid;
  core::platform::u64 definition_generation=0, security_context_generation=0;
  core::platform::u64 security_generation=0, policy_generation=0;
};
struct NativeRuntimeSecurityReadRequest {
  EngineUuid database_uuid, runtime_binding_uuid;
  core::platform::u64 runtime_binding_generation=0, maximum_retained_image_bytes=0;
  // Expected physical bindings only. The actual selected root, navigation and
  // every base leaf must match. Neither arbitrary relation roles nor a supplied
  // dependency tuple can establish selected catalog membership or permission.
  storage::database::NativeCatalogRelationBinding configuration_relation, security_relation;
};
struct NativeRuntimeSecurityObservationResult {
  NativeRuntimeSecurityReadError error=NativeRuntimeSecurityReadError::invalid_request;
  RuntimeAuthorityBindingReadError binding_error=RuntimeAuthorityBindingReadError::invalid_request;
  storage::database::NativePinnedCatalogReadError source_error=storage::database::NativePinnedCatalogReadError::invalid_reader;
  storage::database::NativeCatalogRelationLeaseError relation_error=storage::database::NativeCatalogRelationLeaseError::invalid_request;
  core::catalog::CatalogValueError record_error=core::catalog::CatalogValueError::none;
  transaction::mga::MgaObservationStatus visibility_diagnostic;
  core::platform::u64 physical_bytes_read=0;
  std::optional<storage::disk::BoundedIoResult> io_failure;
  EngineUuid io_failure_filespace_uuid;
  std::optional<core::catalog::CatalogRuntimeAuthorityBinding> binding;
  EngineUuid binding_version_uuid;
  std::optional<NativeRuntimeServicePrincipalObservation> principal;
  std::span<const NativeRuntimeSecurityRecordObservation> records;
  bool ok() const noexcept {
    return error==NativeRuntimeSecurityReadError::none&&binding&&principal&&!records.empty();
  }
};
class NativeRuntimeSecurityPreparedMemory;
struct NativeRuntimeSecurityPreparedResult;
inline NativeRuntimeSecurityObservationResult ReadCommittedNativeRuntimeSecurityOnLease(
    const storage::database::NativeSelectedCheckpointMemoryLease&,
    const NativeRuntimeSecurityReadRequest&,NativeRuntimeSecurityPreparedMemory&) noexcept;

// Preparation allocates only from the real binary-bound retained storage grant,
// before any device/source guard. No grant, epoch or security authority is issued.
class NativeRuntimeSecurityPreparedMemory {
 public:
  NativeRuntimeSecurityPreparedMemory() noexcept=default;
  NativeRuntimeSecurityPreparedMemory(const NativeRuntimeSecurityPreparedMemory&)=delete;
  NativeRuntimeSecurityPreparedMemory& operator=(const NativeRuntimeSecurityPreparedMemory&)=delete;
  NativeRuntimeSecurityPreparedMemory(NativeRuntimeSecurityPreparedMemory&&) noexcept=default;
  NativeRuntimeSecurityPreparedMemory& operator=(NativeRuntimeSecurityPreparedMemory&&) noexcept=default;
  explicit operator bool() const noexcept {return bool(arena_)&&bool(visibility_)&&!records_.empty();}
 private:
  storage::database::NativeStorageArena arena_;
  storage::database::NativeCatalogVisibilityPreparedMemory visibility_;
  std::span<NativeRuntimeSecurityRecordObservation> records_;
  friend NativeRuntimeSecurityPreparedResult PrepareNativeRuntimeSecurityRead(
      const storage::database::NativeCatalogRelationMemoryLimits&,
      storage::database::NativeStorageMemory&,
      const storage::database::NativeStorageMemoryBinding&) noexcept;
  friend NativeRuntimeSecurityObservationResult ReadCommittedNativeRuntimeSecurityOnLease(
      const storage::database::NativeSelectedCheckpointMemoryLease&,
      const NativeRuntimeSecurityReadRequest&,NativeRuntimeSecurityPreparedMemory&) noexcept;
};
inline core::platform::usize NativeRuntimeSecurityWorkspaceBytes(
    const storage::database::NativeCatalogRelationMemoryLimits& limits) noexcept {
  const auto base=storage::database::NativeCatalogVisibilityWorkspaceBytes(limits);
  constexpr auto maximum=std::numeric_limits<core::platform::usize>::max();
  constexpr auto padding=alignof(NativeRuntimeSecurityRecordObservation)-1;
  if(!base||!limits.maximum_rows||limits.maximum_rows>(maximum-padding)/sizeof(NativeRuntimeSecurityRecordObservation))return 0;
  const auto extra=limits.maximum_rows*sizeof(NativeRuntimeSecurityRecordObservation)+padding;
  return base>maximum-extra?0:base+extra;
}
struct NativeRuntimeSecurityPreparedResult {
  NativeRuntimeSecurityReadError error=NativeRuntimeSecurityReadError::invalid_request;
  storage::database::NativeStorageMemoryError memory_error=storage::database::NativeStorageMemoryError::none;
  core::platform::Status allocation_status;
  core::platform::DiagnosticRecord allocation_diagnostic;
  NativeRuntimeSecurityPreparedMemory workspace;
  bool ok() const noexcept {return error==NativeRuntimeSecurityReadError::none&&bool(workspace);}
};
inline NativeRuntimeSecurityPreparedResult PrepareNativeRuntimeSecurityRead(
    const storage::database::NativeCatalogRelationMemoryLimits& limits,
    storage::database::NativeStorageMemory& memory,
    const storage::database::NativeStorageMemoryBinding& binding) noexcept {
  NativeRuntimeSecurityPreparedResult out;
  try {
    const auto bytes=NativeRuntimeSecurityWorkspaceBytes(limits);if(!bytes)return out;
    NativeRuntimeSecurityPreparedMemory w;
    auto allocated=memory.CreateArena(binding,bytes-storage::database::NativeCatalogVisibilityWorkspaceBytes(limits),
        alignof(NativeRuntimeSecurityRecordObservation));
    if(!allocated.ok()){
      out.error=NativeRuntimeSecurityReadError::backing_unavailable;out.memory_error=allocated.error;
      out.allocation_status=allocated.backing.status;out.allocation_diagnostic=std::move(allocated.backing.diagnostic);return out;
    }
    w.arena_=std::move(allocated.arena);
    auto block=w.arena_.Allocate(limits.maximum_rows*sizeof(NativeRuntimeSecurityRecordObservation),alignof(NativeRuntimeSecurityRecordObservation));
    if(!block.ok()){out.error=NativeRuntimeSecurityReadError::resource_exhausted;return out;}
    auto* slots=static_cast<NativeRuntimeSecurityRecordObservation*>(block.pointer);
    static_assert(std::is_trivially_destructible_v<NativeRuntimeSecurityRecordObservation>);
    std::uninitialized_value_construct_n(slots,limits.maximum_rows);w.records_={slots,limits.maximum_rows};
    auto visibility=storage::database::PrepareNativeCatalogVisibilityRead(limits,memory,binding);
    if(!visibility.ok()){
      out.error=NativeRuntimeSecurityReadError::backing_unavailable;out.memory_error=visibility.memory_error;
      out.allocation_status=visibility.allocation_status;out.allocation_diagnostic=std::move(visibility.allocation_diagnostic);return out;
    }
    w.visibility_=std::move(visibility.workspace);out.workspace=std::move(w);out.error=NativeRuntimeSecurityReadError::none;
  }catch(const std::bad_alloc&){out.error=NativeRuntimeSecurityReadError::resource_exhausted;}
   catch(const std::length_error&){out.error=NativeRuntimeSecurityReadError::resource_exhausted;}
   catch(const std::system_error&){out.error=NativeRuntimeSecurityReadError::backing_unavailable;}
  return out;
}

inline NativeRuntimeSecurityObservationResult ReadCommittedNativeRuntimeSecurityOnLease(
    const storage::database::NativeSelectedCheckpointMemoryLease& source,
    const NativeRuntimeSecurityReadRequest& request,NativeRuntimeSecurityPreparedMemory& w) noexcept {
  namespace db=storage::database;namespace c=core::catalog;namespace mga=transaction::mga;
  using E=NativeRuntimeSecurityReadError;NativeRuntimeSecurityObservationResult out;
  const auto fail=[&](E error){out.error=error;out.binding.reset();out.binding_version_uuid={};out.principal.reset();out.records={};};
  try {
    if(!core::uuid::IsEngineIdentityUuid(request.database_uuid)||
        !core::uuid::IsEngineIdentityUuid(request.runtime_binding_uuid)||!request.runtime_binding_generation||
        !request.maximum_retained_image_bytes||!source.selection().checkpoint_inventory.checkpoint||
        source.selection().checkpoint_inventory.checkpoint->header.database_uuid!=request.database_uuid)return out;
    if(!w||w.arena_.binding().database_uuid!=request.database_uuid){fail(E::backing_unavailable);return out;}
    // Layout authority: kind2 role5 configuration; kind2 role4 security. The
    // lower reader derives both heads from the ACTUAL privately selected root
    // and verifies all navigation/base-leaf membership and creator outcomes.
    const auto io_failure=[&](const auto& rows){
      if(!rows.source.roots.io.ok()){out.io_failure=rows.source.roots.io;out.io_failure_filespace_uuid=rows.source.roots.io_filespace_uuid;}
      else if(!rows.source.tree.io.ok()){out.io_failure=rows.source.tree.io;out.io_failure_filespace_uuid=rows.source.tree.io_filespace_uuid;}
      else if(!rows.source.leaf_failure.io.ok()){out.io_failure=rows.source.leaf_failure.io;out.io_failure_filespace_uuid=rows.source.leaf_failure.io_filespace_uuid;}
    };
    const auto configuration=db::NativeCatalogVisibilityLeaseReader::ReadCommitted(source,2,5,request.configuration_relation,
        request.maximum_retained_image_bytes,w.visibility_);
    out.physical_bytes_read=configuration.source.physical_bytes_read;
    out.source_error=configuration.error;out.relation_error=configuration.source.error;
    out.visibility_diagnostic=configuration.visibility_diagnostic;
    if(!configuration.ok()){
      out.binding_error=RuntimeAuthorityBindingReadError::source_failure;io_failure(configuration);
      fail(E::binding_source_failure);return out;
    }
    const db::NativeCatalogVisibleRow* selected=nullptr;
    for(const auto& row:configuration.rows){
      if(row.metadata.record.header.object_uuid.value!=request.runtime_binding_uuid)continue;
      if(selected){out.binding_error=RuntimeAuthorityBindingReadError::ambiguous;fail(E::binding_ambiguous);return out;}
      selected=&row;
    }
    if(!selected){out.binding_error=RuntimeAuthorityBindingReadError::absent;fail(E::binding_absent);return out;}
    if(selected->provisional||selected->metadata.record.header.deleted||
        selected->effective_lifecycle!=c::CatalogObjectLifecycle::active||selected->effective_status!=c::CatalogObjectStatus::active){
      out.binding_error=RuntimeAuthorityBindingReadError::not_committed_active;fail(E::binding_not_active);return out;
    }
    if(!c::CatalogRuntimeAuthorityBindingMatchesMetadata(selected->metadata)){
      out.binding_error=RuntimeAuthorityBindingReadError::invalid_definition;fail(E::binding_invalid);return out;
    }
    const auto decoded_binding=c::DecodeCatalogRuntimeAuthorityBinding(selected->metadata.record.payload);
    if(!decoded_binding.ok()||decoded_binding.record->database_uuid!=request.database_uuid){
      out.binding_error=RuntimeAuthorityBindingReadError::invalid_definition;fail(E::binding_invalid);return out;
    }
    if(decoded_binding.record->generation!=request.runtime_binding_generation){
      out.binding_error=RuntimeAuthorityBindingReadError::generation_mismatch;fail(E::binding_generation_mismatch);return out;
    }
    // Both are fixed-size owning values, copied BEFORE the shared visibility
    // backing is reused for security. No configuration view escapes that reuse.
    const auto binding=*decoded_binding.record;const auto version=selected->version_uuid;
    out.binding_error=RuntimeAuthorityBindingReadError::none;
    if(binding.authority_mode!=c::RuntimeAuthorityMode::database_local){fail(E::external_authority_required);return out;}
    const auto& inventory=source.selection().checkpoint_inventory.inventory.entries;
    const auto committed_origin=[&](core::platform::u64 local,const EngineUuid& identity){
      const auto entry=std::lower_bound(inventory.begin(),inventory.end(),local,
          [](const auto& e,core::platform::u64 n){return e.identity.local_id.value<n;});
      return local&&entry!=inventory.end()&&entry->identity.local_id.value==local&&
          entry->identity.transaction_uuid.value==identity&&entry->identity.scope==mga::TransactionScope::local_node&&
          mga::HasCommittedInventoryOutcome(*entry);
    };
    if(!committed_origin(binding.origin_local_transaction_id,binding.origin_transaction_uuid.value)){
      fail(E::origin_not_committed);return out;
    }
    const auto rows=db::NativeCatalogVisibilityLeaseReader::ReadCommitted(source,2,4,request.security_relation,
        request.maximum_retained_image_bytes,w.visibility_);
    out.physical_bytes_read+=rows.source.physical_bytes_read;out.source_error=rows.error;
    out.relation_error=rows.source.error;out.visibility_diagnostic=rows.visibility_diagnostic;
    if(!rows.ok()){
      io_failure(rows);
      fail(E::security_source_failure);return out;
    }
    // Validate retained invisible/unindexed security versions too. They cannot
    // become a structurally corrupt bypass merely because MGA excludes them.
    for(const auto& base:rows.source.bases)for(const auto& indexed:base.page.metadata){
      const auto& m=indexed.metadata;if(!c::IsCatalogSecurityRecordKind(m.record.header.kind))continue;
      const auto decoded=c::DecodeCatalogSecurityRecordView(m.record.header.kind,m.record.payload);
      if(!decoded.ok()||!c::CatalogSecurityPayloadMatchesMetadata(m)){
        out.record_error=decoded.error;fail(E::security_record_invalid);return out;
      }
      const auto origin=decoded.record->Unsigned("creator_tx");
      const auto found=std::lower_bound(inventory.begin(),inventory.end(),origin.value_or(0),
          [](const auto& e,core::platform::u64 n){return e.identity.local_id.value<n;});
      if(!origin||found==inventory.end()||found->identity.local_id.value!=*origin||
          found->identity.transaction_uuid.value!=decoded.record->Identity("origin_transaction_uuid")||
          found->identity.scope!=mga::TransactionScope::local_node){fail(E::origin_not_committed);return out;}
    }
    core::platform::usize count=0;std::optional<NativeRuntimeServicePrincipalObservation> principal;
    bool found_principal=false;
    for(const auto& row:rows.rows){
      const auto& m=row.metadata;const bool wanted=m.record.header.object_uuid.value==binding.service_principal_uuid;
      if(wanted){if(found_principal){fail(E::duplicate_identity);return out;}found_principal=true;
        if(m.record.header.kind!=c::CatalogRecordKind::user_account){fail(E::principal_wrong_family);return out;}}
      if(!c::IsCatalogSecurityRecordKind(m.record.header.kind))continue;
      if(row.provisional){fail(E::security_record_invalid);return out;}
      const auto decoded=c::DecodeCatalogSecurityRecordView(m.record.header.kind,m.record.payload);
      if(!decoded.ok()||!c::CatalogSecurityPayloadMatchesMetadata(m)){
        out.record_error=decoded.error;fail(E::security_record_invalid);return out;
      }
      if(!committed_origin(*decoded.record->Unsigned("creator_tx"),decoded.record->Identity("origin_transaction_uuid"))){
        fail(E::origin_not_committed);return out;
      }
      if(count==w.records_.size()){fail(E::resource_exhausted);return out;}
      auto& record=w.records_[count];record={};record.record=*decoded.record;
      record.object_uuid=m.record.header.object_uuid.value;record.row_uuid=m.record.header.row_uuid.value;
      record.version_uuid=row.version_uuid;record.previous_version_uuid=row.previous_version_uuid;
      record.origin_transaction_uuid=decoded.record->Identity("origin_transaction_uuid");
      record.origin_local_transaction_id=*decoded.record->Unsigned("creator_tx");
      record.creator_transaction_uuid=m.creator_transaction_uuid.value;record.creator_local_transaction_id=m.creator_local_transaction_id;
      record.retired_transaction_uuid=m.retired_transaction_uuid.value;
      if(!record.retired_transaction_uuid.is_nil()){
        // The common metadata codec binds retirement to this exact version's
        // creator, including its actual local number. Reuse the sorted native
        // inventory lookup instead of scanning the whole inventory per record.
        if(record.retired_transaction_uuid!=record.creator_transaction_uuid||
            !committed_origin(record.creator_local_transaction_id,record.retired_transaction_uuid)){
          fail(E::origin_not_committed);return out;}
        record.retired_local_transaction_id=record.creator_local_transaction_id;
      }
      record.definition_generation=m.definition_version;record.catalog_generation=m.catalog_generation;record.security_epoch=m.security_epoch;
      record.lifecycle=row.effective_lifecycle;record.status=row.effective_status;record.deleted=m.record.header.deleted;
      ++count;
      if(!wanted)continue;
      if(record.deleted||record.lifecycle!=c::CatalogObjectLifecycle::active||record.status!=c::CatalogObjectStatus::active||
          decoded.record->Boolean("active")!=true||decoded.record->Boolean("created_disabled")==true){fail(E::principal_not_active);return out;}
      if(decoded.record->Attribute("kind")!="service"||decoded.record->Attribute("principal_kind")!="service"||
          decoded.record->Boolean("bootstrap_principal")!=false){fail(E::principal_not_service);return out;}
      const auto context=decoded.record->Unsigned("security_context_generation"),security=decoded.record->Unsigned("security_generation"),
          policy=decoded.record->Unsigned("policy_generation");
      if(decoded.record->Unsigned("security_context_authority_version")!=1||!context||!*context||!security||!*security||!policy||!*policy){
        fail(E::principal_generation_invalid);return out;}
      principal=NativeRuntimeServicePrincipalObservation{record.object_uuid,record.version_uuid,record.definition_generation,*context,*security,*policy};
    }
    if(!principal){fail(E::principal_absent);return out;}
    const auto records=w.records_.first(count);
    std::sort(records.begin(),records.end(),[](const auto& a,const auto& b){return a.object_uuid<b.object_uuid;});
    for(core::platform::usize n=1;n<count;++n)if(records[n-1].object_uuid==records[n].object_uuid){fail(E::duplicate_identity);return out;}
    out.binding=binding;out.binding_version_uuid=version;out.principal=principal;out.records=w.records_.first(count);out.error=E::none;
  }catch(const std::bad_alloc&){fail(E::resource_exhausted);}
   catch(const std::length_error&){fail(E::resource_exhausted);}
   catch(...){fail(E::security_source_failure);}
  return out;
}
} // namespace scratchbird::engine::internal_api
