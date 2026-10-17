// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "startup_credential_source.hpp"
#include "engine/internal_api/security/native_runtime_security_observation.hpp"
#include "engine/internal_api/security/local_password_credential.hpp"
#include "core/catalog/catalog_bootstrap_policy.hpp"
#include "core/hash/crypto_memory_adapter.hpp"
#include <stop_token>

namespace scratchbird::server {
enum class NativeRuntimePrincipalCredentialError {
  none, invalid_request, cancelled, backing_unavailable, security_source_failure,
  provider_source_failure, provider_absent, provider_ambiguous, provider_invalid,
  provider_inactive, provider_generation_mismatch, local_provider_required,
  credential_source_unavailable, credential_binding_mismatch,
  protected_workspace_required, crypto_scope_unavailable, credential_rejected,
  provider_failure, resource_exhausted
};

// Prepare before acquiring ANY source/device guards. This owns two independent
// governed catalog readers: provider observation must not invalidate the
// security/verifier views while they are being authenticated.
struct NativeRuntimePrincipalCredentialPreparedMemory {
  engine::internal_api::NativeRuntimeSecurityPreparedMemory security;
  storage::database::NativeCatalogVisibilityPreparedMemory provider;
  explicit operator bool() const noexcept { return bool(security) && bool(provider); }
};
inline core::platform::usize NativeRuntimePrincipalCredentialWorkspaceBytes(
    const storage::database::NativeCatalogRelationMemoryLimits& limits) noexcept {
  const auto a = engine::internal_api::NativeRuntimeSecurityWorkspaceBytes(limits);
  const auto b = storage::database::NativeCatalogVisibilityWorkspaceBytes(limits);
  return !a || !b || a > std::numeric_limits<core::platform::usize>::max() - b ? 0 : a + b;
}
struct NativeRuntimePrincipalCredentialPreparation {
  NativeRuntimePrincipalCredentialError error = NativeRuntimePrincipalCredentialError::invalid_request;
  storage::database::NativeStorageMemoryError memory_error = storage::database::NativeStorageMemoryError::none;
  core::platform::Status allocation_status;
  core::platform::DiagnosticRecord allocation_diagnostic;
  NativeRuntimePrincipalCredentialPreparedMemory workspace;
  bool ok() const noexcept { return error == NativeRuntimePrincipalCredentialError::none && bool(workspace); }
};
inline NativeRuntimePrincipalCredentialPreparation PrepareNativeRuntimePrincipalCredential(
    const storage::database::NativeCatalogRelationMemoryLimits& limits,
    storage::database::NativeStorageMemory& memory,
    const storage::database::NativeStorageMemoryBinding& binding) noexcept {
  NativeRuntimePrincipalCredentialPreparation out;
  if (!NativeRuntimePrincipalCredentialWorkspaceBytes(limits)) return out;
  auto security = engine::internal_api::PrepareNativeRuntimeSecurityRead(limits, memory, binding);
  if (!security.ok()) {
    out.error = NativeRuntimePrincipalCredentialError::backing_unavailable;
    out.memory_error = security.memory_error; out.allocation_status = security.allocation_status;
    out.allocation_diagnostic = std::move(security.allocation_diagnostic); return out;
  }
  auto provider = storage::database::PrepareNativeCatalogVisibilityRead(limits, memory, binding);
  if (!provider.ok()) {
    out.error = NativeRuntimePrincipalCredentialError::backing_unavailable;
    out.memory_error = provider.memory_error; out.allocation_status = provider.allocation_status;
    out.allocation_diagnostic = std::move(provider.allocation_diagnostic); return out;
  }
  out.workspace.security = std::move(security.workspace);
  out.workspace.provider = std::move(provider.workspace);
  out.error = NativeRuntimePrincipalCredentialError::none; return out;
}

struct NativeRuntimePrincipalCredentialRequest {
  engine::internal_api::NativeRuntimeSecurityReadRequest security;
  std::stop_token stop;
};
struct NativeRuntimePrincipalCredentialResult;
class NativeRuntimePrincipalCredentialLease;
struct NativeLocalServiceStartupSelectionResult;
inline NativeLocalServiceStartupSelectionResult SelectNativeLocalServiceStartupDeclarations(
    const NativeRuntimePrincipalCredentialLease&) noexcept;
inline NativeRuntimePrincipalCredentialResult VerifyNativeRuntimePrincipalCredential(
    const NativeRuntimePrincipalCredentialRequest&,
    storage::database::NativeSelectedCheckpointMemoryHandle&,
    NativeRuntimePrincipalCredentialPreparedMemory&, StartupCredentialSource::Lease&,
    const core::memory::ScopedProtectedBuffer&) noexcept;

// A privately issued comparison proof, NOT an authenticated session, effective
// authorization, provider/configuration activation, startup permit or BEGIN.
// Keeps the original actual source and protected credential-source fence alive.
// The node devices, granting managers and StartupCredentialSource outlive it.
// Read-only source custody cannot be upgraded to a write/publication lease.
// Quiesce all borrowed record users before destroying this acquiring-thread
// owner. No plaintext credential or derived key is returned or retained here.
class NativeRuntimePrincipalCredentialLease final {
 public:
  NativeRuntimePrincipalCredentialLease(const NativeRuntimePrincipalCredentialLease&) = delete;
  NativeRuntimePrincipalCredentialLease& operator=(const NativeRuntimePrincipalCredentialLease&) = delete;
  NativeRuntimePrincipalCredentialLease(NativeRuntimePrincipalCredentialLease&& other) noexcept
      : workspace_(TakeWorkspace(other)), source_(std::move(other.source_)),
        credential_(std::move(other.credential_)), observation_(other.observation_),
        provider_(other.provider_), provider_version_(other.provider_version_),
        credential_binding_(other.credential_binding_), configuration_(other.configuration_),
        thread_(std::this_thread::get_id()) {}
  NativeRuntimePrincipalCredentialLease& operator=(NativeRuntimePrincipalCredentialLease&&) = delete;
  ~NativeRuntimePrincipalCredentialLease() {
    CheckThread();
    // The credential owner erases/releases protected memory. Drop all native
    // device guards before that cleanup or either workspace's deallocation.
    source_.Reset(); credential_.Reset();
  }
  bool valid() const noexcept { CheckThread(); return bool(source_) && credential_.binding().has_value(); }
  const core::catalog::CatalogRuntimeAuthorityBinding& binding() const noexcept {
    CheckAccess(); return *observation_.binding;
  }
  const engine::internal_api::NativeRuntimeServicePrincipalObservation& principal() const noexcept {
    CheckAccess(); return *observation_.principal;
  }
  const core::platform::Uuid& binding_version_uuid() const noexcept {
    CheckAccess(); return observation_.binding_version_uuid;
  }
  const core::catalog::CatalogBootstrapSecurityProvider& provider() const noexcept {
    CheckAccess(); return provider_;
  }
  const core::platform::Uuid& provider_version_uuid() const noexcept { CheckAccess(); return provider_version_; }
  const StartupCredentialTransportBinding& credential_binding() const noexcept { CheckAccess(); return credential_binding_; }
  std::span<const engine::internal_api::NativeRuntimeSecurityRecordObservation> security_records() const noexcept {
    CheckAccess(); return observation_.records;
  }
 private:
  friend NativeRuntimePrincipalCredentialResult VerifyNativeRuntimePrincipalCredential(
      const NativeRuntimePrincipalCredentialRequest&, storage::database::NativeSelectedCheckpointMemoryHandle&,
      NativeRuntimePrincipalCredentialPreparedMemory&, StartupCredentialSource::Lease&,
      const core::memory::ScopedProtectedBuffer&) noexcept;
  friend NativeLocalServiceStartupSelectionResult SelectNativeLocalServiceStartupDeclarations(
      const NativeRuntimePrincipalCredentialLease&) noexcept;
  NativeRuntimePrincipalCredentialLease(NativeRuntimePrincipalCredentialPreparedMemory&& workspace,
      storage::database::NativeSelectedCheckpointMemoryHandle&& source, StartupCredentialSource::Lease&& credential,
      engine::internal_api::NativeRuntimeSecurityObservationResult observation,
      core::catalog::CatalogBootstrapSecurityProvider provider, core::platform::Uuid provider_version,
      StartupCredentialTransportBinding credential_binding,
      std::span<const storage::database::NativeCatalogVisibleRow> configuration) noexcept
      : workspace_(std::move(workspace)), source_(std::move(source)), credential_(std::move(credential)),
        observation_(std::move(observation)), provider_(provider), provider_version_(provider_version),
        credential_binding_(credential_binding), configuration_(configuration),
        thread_(std::this_thread::get_id()) {}
  void CheckThread() const noexcept { if (thread_ != std::this_thread::get_id()) std::terminate(); }
  void CheckAccess() const noexcept { if (!valid()) std::terminate(); }
  static NativeRuntimePrincipalCredentialPreparedMemory TakeWorkspace(NativeRuntimePrincipalCredentialLease& other) noexcept {
    other.CheckThread(); return std::move(other.workspace_);
  }
  // Reverse destruction releases credential and source guards BEFORE governed
  // catalog backing; no allocation/deallocation callback runs under them.
  NativeRuntimePrincipalCredentialPreparedMemory workspace_;
  storage::database::NativeSelectedCheckpointMemoryHandle source_;
  StartupCredentialSource::Lease credential_;
  engine::internal_api::NativeRuntimeSecurityObservationResult observation_;
  core::catalog::CatalogBootstrapSecurityProvider provider_;
  core::platform::Uuid provider_version_;
  StartupCredentialTransportBinding credential_binding_;
  // Exact immutable rows already read into the retained provider workspace.
  // Accessible only to the native declaration selector, not a mutable catalog
  // or an independently constructible configuration/activation authority.
  std::span<const storage::database::NativeCatalogVisibleRow> configuration_;
  std::thread::id thread_;
};

struct NativeRuntimePrincipalCredentialResult {
  NativeRuntimePrincipalCredentialError error = NativeRuntimePrincipalCredentialError::invalid_request;
  engine::internal_api::NativeRuntimeSecurityReadError security_error = engine::internal_api::NativeRuntimeSecurityReadError::none;
  storage::database::NativePinnedCatalogReadError source_error = storage::database::NativePinnedCatalogReadError::none;
  storage::database::NativeCatalogRelationLeaseError relation_error = storage::database::NativeCatalogRelationLeaseError::none;
  core::catalog::CatalogValueError record_error = core::catalog::CatalogValueError::none;
  transaction::mga::MgaObservationStatus visibility_diagnostic;
  core::hash::CryptoMemoryError crypto_error = core::hash::CryptoMemoryError::none;
  engine::internal_api::LocalPasswordCredentialError verifier_error = engine::internal_api::LocalPasswordCredentialError::none;
  core::platform::u64 physical_bytes_read = 0;
  std::optional<storage::disk::BoundedIoResult> io_failure;
  core::platform::Uuid io_failure_filespace_uuid;
  std::optional<NativeRuntimePrincipalCredentialLease> lease;
  bool ok() const noexcept { return error == NativeRuntimePrincipalCredentialError::none && lease && lease->valid(); }
};

// All owners/backing and the actual database CryptoMemoryScope must already be
// admitted before entry. The private credential lease proves protected source
// generation custody, not permission to release credentials; its provisioner
// retains that separate authority. No password, grant or snapshot supplied by
// a caller substitutes the internal native reads below. Failure never consumes
// the caller's source/workspace/credential owners or exposes a partial proof.
inline NativeRuntimePrincipalCredentialResult VerifyNativeRuntimePrincipalCredential(
    const NativeRuntimePrincipalCredentialRequest& request,
    storage::database::NativeSelectedCheckpointMemoryHandle& source,
    NativeRuntimePrincipalCredentialPreparedMemory& workspace,
    StartupCredentialSource::Lease& credential,
    const core::memory::ScopedProtectedBuffer& derived) noexcept {
  namespace api = engine::internal_api; namespace db = storage::database;
  namespace catalog = core::catalog; namespace mga = transaction::mga;
  using E = NativeRuntimePrincipalCredentialError;
  NativeRuntimePrincipalCredentialResult out;
  const auto fail = [&](E error) { out.error = error; return std::move(out); };
  if (request.stop.stop_requested()) return fail(E::cancelled);
  if (!source || !workspace || !core::uuid::IsEngineIdentityUuid(request.security.database_uuid)) return out;
  const auto custody = credential.binding();
  if (!custody) return fail(E::credential_source_unavailable);
  if (!derived.valid() || derived.size() != 32 ||
      !derived.evidence().platform_lock_succeeded || !derived.evidence().no_dump_succeeded ||
      derived.tag().binary_ownership[core::memory::MemoryBinaryScopeKind::database] != request.security.database_uuid.bytes)
    return fail(E::protected_workspace_required);
  struct EraseDerived {
    void* bytes;
    ~EraseDerived() { OPENSSL_cleanse(bytes,32); }
  } erase_derived{derived.data()};
  out.crypto_error = core::hash::CheckExplicitCryptoMemoryScope(request.security.database_uuid);
  if (out.crypto_error != core::hash::CryptoMemoryError::none) return fail(E::crypto_scope_unavailable);
  auto observed = api::ReadCommittedNativeRuntimeSecurityOnLease(*source, request.security, workspace.security);
  out.physical_bytes_read = observed.physical_bytes_read;
  out.security_error = observed.error; out.source_error = observed.source_error;
  out.relation_error = observed.relation_error; out.record_error = observed.record_error;
  out.visibility_diagnostic = observed.visibility_diagnostic;
  out.io_failure = observed.io_failure; out.io_failure_filespace_uuid = observed.io_failure_filespace_uuid;
  if (!observed.ok()) return fail(E::security_source_failure);
  const auto& binding = *observed.binding;
  if (!binding.credential_reference_uuid || custody->database != binding.database_uuid ||
      custody->principal != binding.service_principal_uuid || custody->provider != binding.provider_uuid ||
      custody->credential_reference != *binding.credential_reference_uuid || !custody->generation)
    return fail(E::credential_binding_mismatch);
  if (request.stop.stop_requested()) return fail(E::cancelled);
  const auto rows = db::NativeCatalogVisibilityLeaseReader::ReadCommitted(*source, 2, 5,
      request.security.configuration_relation, request.security.maximum_retained_image_bytes, workspace.provider);
  if (rows.source.physical_bytes_read > std::numeric_limits<core::platform::u64>::max() - out.physical_bytes_read)
    return fail(E::resource_exhausted);
  out.physical_bytes_read += rows.source.physical_bytes_read;
  if (!rows.ok()) {
    out.source_error = rows.error; out.relation_error = rows.source.error;
    out.visibility_diagnostic = rows.visibility_diagnostic;
    const auto io = [&](const auto& r) {
      if (!r.io.ok()) { out.io_failure = r.io; out.io_failure_filespace_uuid = r.io_filespace_uuid; }
    };
    io(rows.source.roots); io(rows.source.tree); io(rows.source.leaf_failure);
    return fail(E::provider_source_failure);
  }
  const db::NativeCatalogVisibleRow* selected = nullptr;
  for (const auto& row : rows.rows) {
    if (row.metadata.record.header.object_uuid.value != binding.provider_uuid) continue;
    if (selected) return fail(E::provider_ambiguous);
    selected = &row;
  }
  if (!selected) return fail(E::provider_absent);
  const auto& metadata = selected->metadata;
  if (!catalog::CatalogBootstrapPolicyMatchesMetadata(metadata) || metadata.object_subtype != "bootstrap_security_provider")
    return fail(E::provider_invalid);
  const auto provider = catalog::DecodeCatalogBootstrapSecurityProvider(metadata.record.payload);
  out.record_error = provider.error;
  if (!provider.ok() || provider.record->provider_uuid != binding.provider_uuid) return fail(E::provider_invalid);
  if (selected->provisional || metadata.record.header.deleted ||
      selected->effective_lifecycle != catalog::CatalogObjectLifecycle::active ||
      selected->effective_status != catalog::CatalogObjectStatus::active)
    return fail(E::provider_inactive);
  if (provider.record->generation != binding.provider_generation) return fail(E::provider_generation_mismatch);
  if (provider.record->family != catalog::BootstrapProviderFamily::local_password ||
      provider.record->authority != catalog::BootstrapProviderAuthority::durable_catalog ||
      provider.record->external_provider ||
      provider.record->verifier_policy != catalog::BootstrapVerifierPolicy::local_password_verifier_required)
    return fail(E::local_provider_required);
  // An imported default is not current provider activation. Preserve its exact
  // value in the comparison proof; actual admission decides activation later.
  const auto& inventory = source->selection().checkpoint_inventory.inventory.entries;
  const auto origin = std::lower_bound(inventory.begin(), inventory.end(), provider.record->origin_local_transaction_id,
      [](const auto& e, auto id) { return e.identity.local_id.value < id; });
  if (origin == inventory.end() || origin->identity.local_id.value != provider.record->origin_local_transaction_id ||
      origin->identity.transaction_uuid.value != provider.record->origin_transaction_uuid.value ||
      origin->identity.scope != mga::TransactionScope::local_node || !mga::HasCommittedInventoryOutcome(*origin))
    return fail(E::provider_invalid);
  std::optional<std::string_view> verifier;
  for (const auto& record : observed.records)
    if (record.object_uuid == binding.service_principal_uuid)
      verifier = record.record.Attribute("credential_fingerprint");
  if (!verifier) {
    out.verifier_error = api::LocalPasswordCredentialError::invalid_verifier;
    return fail(E::credential_rejected);
  }
  if (request.stop.stop_requested()) return fail(E::cancelled);
  const auto verified = api::VerifyLocalPasswordCredential(*verifier, credential.credential(),
      std::span<std::byte,32>{static_cast<std::byte*>(derived.data()),32});
  out.verifier_error = verified.error;
  if (!verified.verified()) return fail(verified.error == api::LocalPasswordCredentialError::provider_failure
      ? E::provider_failure : E::credential_rejected);
  if (request.stop.stop_requested()) return fail(E::cancelled);
  out.crypto_error = core::hash::CheckExplicitCryptoMemoryScope(request.security.database_uuid);
  if (out.crypto_error != core::hash::CryptoMemoryError::none) return fail(E::crypto_scope_unavailable);
  NativeRuntimePrincipalCredentialLease proof(std::move(workspace), std::move(source), std::move(credential),
      std::move(observed), *provider.record, selected->version_uuid, *custody, rows.rows);
  out.lease.emplace(std::move(proof)); out.error = E::none; return out;
}
} // namespace scratchbird::server
