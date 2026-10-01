// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_runtime_authority_binding.hpp"
#include "physical_mga_cow_store.hpp"
#include <new>
#include <stdexcept>

namespace scratchbird::engine::internal_api {
enum class RuntimeAuthorityBindingReadError {
  none, invalid_request, source_failure, absent, ambiguous, invalid_definition,
  not_committed_active, generation_mismatch, resource_exhausted
};
struct RuntimeAuthorityBindingReadResult {
  RuntimeAuthorityBindingReadError error = RuntimeAuthorityBindingReadError::invalid_request;
  std::optional<core::catalog::CatalogRuntimeAuthorityBinding> binding;
  core::platform::Uuid native_version_uuid, snapshot_uuid;
  storage::database::NativePinnedCatalogReadError source_error = storage::database::NativePinnedCatalogReadError::none;
  core::platform::DiagnosticRecord diagnostic;
  bool ok() const noexcept { return error == RuntimeAuthorityBindingReadError::none && binding.has_value(); }
};

// Read-only mounted catalog source. The caller supplies already-owned devices,
// the admitted catalog relation/checkpoint and a real snapshot pin. This does
// not authenticate, select a provider, authorize mutation or pin configuration
// activation; keep that separate owning fence through startup admission/BEGIN.
inline RuntimeAuthorityBindingReadResult ReadRuntimeAuthorityBindingFromOpenDevices(
    const core::platform::Uuid& database_uuid,
    const std::vector<storage::disk::NativeFilespaceDevice>& devices,
    const storage::disk::FilespaceRootReference& checkpoint,
    core::platform::u16 catalog_selector, core::platform::u16 relation_role,
    const storage::database::NativeCatalogRelationBinding& relation,
    const transaction::mga::TransactionIdentity& reader,
    const transaction::mga::PublishedSnapshotPin& snapshot,
    const core::platform::Uuid& binding_uuid, core::platform::u64 expected_generation,
    core::platform::u64 maximum_retained_image_bytes) noexcept {
  using E = RuntimeAuthorityBindingReadError;
  RuntimeAuthorityBindingReadResult result;
  try {
    if (!core::uuid::IsEngineIdentityUuid(database_uuid) ||
        !core::uuid::IsEngineIdentityUuid(binding_uuid) || !expected_generation ||
        !maximum_retained_image_bytes) return result;
    const auto source = storage::database::ReadNativePinnedCatalogVersionsFromOpenDevices(
        database_uuid, devices, checkpoint, catalog_selector, relation_role, relation,
        reader, snapshot, maximum_retained_image_bytes);
    if (!source.ok()) {
      result.error=E::source_failure; result.source_error=source.error;
      result.diagnostic=source.diagnostic; return result;
    }
    const storage::database::NativeCatalogVersionRow* selected=nullptr;
    for (const auto& row : source.rows) {
      if (row.metadata.record.header.object_uuid.value != binding_uuid) continue;
      if (selected) { result.error=E::ambiguous; return result; }
      selected=&row;
    }
    if (!selected) { result.error=E::absent; return result; }
    if (selected->provisional || selected->metadata.record.header.deleted ||
        selected->effective_lifecycle != core::catalog::CatalogObjectLifecycle::active ||
        selected->effective_status != core::catalog::CatalogObjectStatus::active) {
      result.error=E::not_committed_active; return result;
    }
    if (!core::catalog::CatalogRuntimeAuthorityBindingMatchesMetadata(selected->metadata)) {
      result.error=E::invalid_definition; return result;
    }
    const auto decoded=core::catalog::DecodeCatalogRuntimeAuthorityBinding(selected->metadata.record.payload);
    if (!decoded.ok() || decoded.record->database_uuid != database_uuid) {
      result.error=E::invalid_definition; return result;
    }
    if (decoded.record->generation != expected_generation) {
      result.error=E::generation_mismatch; return result;
    }
    result.binding=decoded.record; result.native_version_uuid=selected->version_uuid;
    result.snapshot_uuid=source.snapshot_uuid; result.error=E::none; return result;
  } catch (const std::bad_alloc&) { result.error=E::resource_exhausted; }
    catch (const std::length_error&) { result.error=E::resource_exhausted; }
  result.binding.reset(); result.native_version_uuid={}; result.snapshot_uuid={};
  return result;
}
} // namespace scratchbird::engine::internal_api
