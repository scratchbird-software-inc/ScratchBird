// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_visibility_lease_reader.hpp"
#include "catalog_context_namespace.hpp"

namespace scratchbird::storage::database {
struct NativeContextNamespaceBinding {
  Uuid database_uuid, namespace_uuid;
  u64 definition_generation=0;
};
enum class NativeContextNamespaceReadError {
  none, invalid_request, node_mismatch, source_failure, missing, ambiguous,
  family_mismatch, generation_mismatch
};
struct NativeContextNamespaceReadResult {
  NativeContextNamespaceReadError error=NativeContextNamespaceReadError::invalid_request;
  NativeCatalogVisibilityLeaseResult source;
  std::optional<core::catalog::CatalogContextNamespace> definition;
  const NativeCatalogVisibleRow* selected=nullptr;
  bool ok() const noexcept {
    return error==NativeContextNamespaceReadError::none&&source.ok()&&definition&&selected;
  }
};
// Read-only typed observation through a privately constructed actual source
// lease, never supplied rows or a caller checkpoint. Prepare workspace BEFORE
// acquiring the source guards. Source, pin and workspace outlive borrowed use;
// another read into that workspace invalidates ALL previous borrowed results.
// Success is NOT authorization, activity or durable publication. In particular,
// selected->provisional/effective_lifecycle/effective_status/deleted remain
// mandatory inputs to the owning engine's access/CRUD admission. Retired rows
// are observable for reconciliation; this API never resurrects them or grants
// CONTEXT_READ/WRITE. No session keys or values live in this record.
class NativeContextNamespaceLeaseReader {
 public:
  static NativeContextNamespaceReadResult ReadCommitted(
      const NativeSelectedCheckpointMemoryLease& source,u16 selector,u16 role,
      const NativeCatalogRelationBinding& relation,const NativeContextNamespaceBinding& binding,
      u64 budget,NativeCatalogVisibilityPreparedMemory& workspace) noexcept {
    return Read(source,selector,role,relation,nullptr,nullptr,binding,budget,workspace);
  }
  static NativeContextNamespaceReadResult ReadPinned(
      const NativeSelectedCheckpointMemoryLease& source,u16 selector,u16 role,
      const NativeCatalogRelationBinding& relation,
      const transaction::mga::TransactionIdentity& reader,const transaction::mga::PublishedSnapshotPin& pin,
      const NativeContextNamespaceBinding& binding,u64 budget,
      NativeCatalogVisibilityPreparedMemory& workspace) noexcept {
    return Read(source,selector,role,relation,&reader,&pin,binding,budget,workspace);
  }
 private:
  static NativeContextNamespaceReadResult Read(
      const NativeSelectedCheckpointMemoryLease& source,u16 selector,u16 role,
      const NativeCatalogRelationBinding& relation,const transaction::mga::TransactionIdentity* reader,
      const transaction::mga::PublishedSnapshotPin* pin,const NativeContextNamespaceBinding& binding,
      u64 budget,NativeCatalogVisibilityPreparedMemory& workspace) noexcept {
    using E=NativeContextNamespaceReadError;namespace c=core::catalog;
    NativeContextNamespaceReadResult out;
    if(!core::uuid::IsEngineIdentityUuid(binding.database_uuid)||
       !core::uuid::IsEngineIdentityUuid(binding.namespace_uuid)||!binding.definition_generation||!budget)return out;
    if(source.selection().checkpoint_inventory.checkpoint->header.database_uuid!=binding.database_uuid){
      out.error=E::node_mismatch;return out;
    }
    out.source=pin?NativeCatalogVisibilityLeaseReader::ReadPinned(source,selector,role,relation,*reader,*pin,budget,workspace):
      NativeCatalogVisibilityLeaseReader::ReadCommitted(source,selector,role,relation,budget,workspace);
    if(!out.source.ok()){out.error=E::source_failure;return out;}
    const NativeCatalogVisibleRow* found=nullptr;
    for(const auto& row:out.source.rows)if(row.metadata.record.header.object_uuid.value==binding.namespace_uuid){
      if(found){out.error=E::ambiguous;return out;}found=&row;
    }
    if(!found){out.error=E::missing;return out;}
    const auto& m=found->metadata;
    if(!c::CatalogContextNamespaceMatchesMetadata(m)){out.error=E::family_mismatch;return out;}
    const auto definition=c::DecodeCatalogContextNamespace(m.record.payload);
    if(definition.record->database_uuid!=binding.database_uuid){out.error=E::node_mismatch;return out;}
    if(definition.record->generation!=binding.definition_generation){out.error=E::generation_mismatch;return out;}
    out.definition=definition.record;out.selected=found;out.error=E::none;return out;
  }
};
} // namespace scratchbird::storage::database
