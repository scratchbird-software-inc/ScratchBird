// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "config.hpp"
#include "diagnostics.hpp"
#include "../core/runtime/runtime_crypto_pool_owner.hpp"
#include "../core/catalog/catalog_memory_configuration.hpp"
#include <optional>
#include <thread>
#include <map>

namespace scratchbird::server {
enum class ProcessCryptoStartError {
  none, already_started, wrong_owner, invalid_policy, identity_failure,
  reservation_failure, pool_failure, diagnostic_seal_failure
};
struct ProcessCryptoStartResult {
  ProcessCryptoStartError error=ProcessCryptoStartError::invalid_policy;
  core::runtime::RuntimeCryptoPoolResult pool{};
  bool ok() const noexcept {return error==ProcessCryptoStartError::none;}
};
enum class NativeBootstrapMemoryKind { storage, database_crypto };
struct NativeBootstrapMemoryRequest {
  core::platform::Uuid database, operation, owner, context;
  std::uint64_t bytes = 0;
  NativeBootstrapMemoryKind kind = NativeBootstrapMemoryKind::storage;
};
enum class NativeBootstrapMemoryError {
  none, wrong_owner, not_ready, busy, invalid_request, reservation_failure,
  allocation_failure, synchronization_failure, child_budget_refused
};
struct NativeBootstrapMemoryResult {
  NativeBootstrapMemoryError error = NativeBootstrapMemoryError::not_ready;
  core::memory::ReservationBackedMemoryResourceAcquireResult admission;
  bool ok() const noexcept { return error == NativeBootstrapMemoryError::none && admission.ok(); }
};
enum class NativeDatabaseMemoryEnvelopeError {
  none, wrong_owner, not_ready, busy, invalid_request, selected_guards_held,
  binding_mismatch, authorization_refused, budget_refused, allocation_failure, synchronization_failure,
  policy_refused, startup_floor_unavailable, page_budget_refused, crypto_budget_refused
};
// Observation of a real ledger constraint, not full memory-policy activation,
// provider admission, a service session, or an engine resource epoch.
struct NativeDatabaseMemoryEnvelopeResult {
  NativeDatabaseMemoryEnvelopeError error=NativeDatabaseMemoryEnvelopeError::invalid_request;
  core::platform::Uuid database, configuration, version;
  std::uint64_t configuration_generation=0, configured_hard_limit_bytes=0, effective_hard_limit_bytes=0;
  std::uint64_t configured_soft_limit_bytes=0, enforced_soft_limit_bytes=0;
  std::uint64_t page_buffer_limit_bytes=0, crypto_limit_bytes=0;
  core::platform::DiagnosticRecord diagnostic;
  bool ok() const noexcept {return error==NativeDatabaseMemoryEnvelopeError::none;}
};
struct NativeDatabaseAllocationBehaviorResult {
  NativeDatabaseMemoryEnvelopeError error=NativeDatabaseMemoryEnvelopeError::not_ready;
  core::memory::DatabaseAllocationBehaviorResult allocation;
  bool ok() const noexcept {return error==NativeDatabaseMemoryEnvelopeError::none&&allocation.ok();}
};
class NativeHostedDatabaseRuntime;
// Exclusive process-host lifetime. Construct before configuration diagnostics;
// Start only after installing the selected actual memory-manager policy, before
// ANY OpenSSL user. Destroy after every hosted worker/provider/engine owner.
// This is not a database grant or an embedded application's global cleanup API.
class ServerProcessCryptoOwner final {
 public:
  ServerProcessCryptoOwner() noexcept;
  ~ServerProcessCryptoOwner();
  ServerProcessCryptoOwner(const ServerProcessCryptoOwner&)=delete;
  ServerProcessCryptoOwner& operator=(const ServerProcessCryptoOwner&)=delete;
  ProcessCryptoStartResult Start(const ServerBootstrapConfig&,
      const core::memory::MemoryPolicyConfigResolveResult&) noexcept;
  // Physical bootstrap backing only. Charge the same actual process ledger
  // and selected manager, additionally scoped to the probed native database.
  // No principal, provider activation, transaction or publication is admitted.
  // The returned resource and every derived owner must retire before this
  // process owner; Close refuses while any such reservation remains live.
  NativeBootstrapMemoryResult AcquireNativeBootstrapResource(NativeBootstrapMemoryRequest) noexcept;
  // Own process-wide OpenSSL algorithm-cache entries in process backing before
  // a database scope first uses local-password primitives. This prepares only
  // library resources; it does not admit or activate a catalog provider.
  core::runtime::RuntimeCryptoPoolResult PrepareNativeLocalPasswordAlgorithms() noexcept;
  // Caller has joined all provider users and released their contexts. Refusal
  // retains backing; never force-free a busy pool merely to unwind.
  core::runtime::RuntimeCryptoPoolResult CloseAfterUserDrain() noexcept;
  core::hash::CryptoMemorySnapshot Snapshot() const noexcept {
    if(!SameOwner()) {
      core::hash::CryptoMemorySnapshot refused;
      refused.observation_error=core::hash::CryptoMemoryError::invalid_binding;
      return refused;
    }
    return pool_.Snapshot();
  }
  bool installed() const noexcept {return SameOwner()&&installed_;}
  // Serialized host work; callback cannot acquire a pool pointer or transfer
  // custody. The actual reservation is checked again by the private owner.
  template<class Work>
    requires std::is_same_v<std::invoke_result_t<Work>,void>
  core::runtime::RuntimeCryptoPoolResult WithProviderScope(Work&& work) {
    if(!SameOwner())return {core::runtime::RuntimeCryptoPoolError::invalid_binding};
    const auto state=pool_.Snapshot();
    if(state.observation_error!=core::hash::CryptoMemoryError::none)
      return {core::runtime::RuntimeCryptoPoolError::adapter_failure,state.observation_error};
    return pool_.WithMemoryScope(state.binding,std::forward<Work>(work));
  }
 private:
  friend class NativeHostedDatabaseRuntime;
  // Only the retained native host selects these values from its authenticated
  // checkpoint observation, after retiring all selected device guards.
  NativeDatabaseMemoryEnvelopeResult ConstrainNativeDatabaseMemory(
      core::platform::Uuid database, const core::catalog::CatalogMemoryConfiguration&,
      core::platform::Uuid version) noexcept;
  NativeDatabaseMemoryEnvelopeError RetireNativeDatabaseMemory(core::platform::Uuid database) noexcept;
  NativeDatabaseAllocationBehaviorResult InstallNativeDatabaseAllocationBehavior(
      core::platform::Uuid database, const core::catalog::CatalogMemoryConfiguration&,
      core::platform::Uuid version) noexcept;
  bool HasNativeDatabaseAllocationBehavior(core::platform::Uuid database,
      const core::catalog::CatalogMemoryConfiguration&, core::platform::Uuid version) const noexcept;
  bool SameOwner() const noexcept;
  const std::uint64_t process_id_;
  const std::thread::id thread_id_;
  std::optional<BootstrapDiagnosticScope> diagnostics_;
  core::memory::HierarchicalMemoryBudgetLedger ledger_;
  core::runtime::RuntimeCryptoPoolOwner pool_;
  core::memory::HierarchicalMemoryScopeRef process_scope_;
  core::memory::HierarchicalMemoryBudgetProvenance provenance_;
  core::memory::HierarchicalMemoryReservationToken process_crypto_token_;
  struct NativeMemoryState {
    NativeDatabaseMemoryEnvelopeResult envelope;
    bool allocation_behavior_installed=false;
  };
  std::map<core::platform::Uuid,NativeMemoryState> native_memory_envelopes_;
  std::uint64_t process_limit_bytes_ = 0;
  bool acquiring_native_ = false;
  bool preparing_native_password_ = false, native_password_algorithms_prepared_ = false;
  bool attempted_=false,installed_=false,provider_cleaned_=false;
};
} // namespace scratchbird::server
