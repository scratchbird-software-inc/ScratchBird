// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "process_crypto_owner.hpp"
#include "uuid.hpp"
#include <openssl/crypto.h>
#include <limits>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace scratchbird::server {
namespace {
std::uint64_t ProcessId() noexcept {
#ifdef _WIN32
  return ::GetCurrentProcessId();
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}
}
ServerProcessCryptoOwner::ServerProcessCryptoOwner() noexcept
    :process_id_(ProcessId()),thread_id_(std::this_thread::get_id()),diagnostics_(std::in_place) {}
bool ServerProcessCryptoOwner::SameOwner() const noexcept {
  // Check process before touching any possibly inherited synchronization.
  return process_id_==ProcessId()&&thread_id_==std::this_thread::get_id();
}
ServerProcessCryptoOwner::~ServerProcessCryptoOwner() {
  if(!CloseAfterUserDrain().ok())std::terminate();
}
ProcessCryptoStartResult ServerProcessCryptoOwner::Start(
    const ServerBootstrapConfig& config,
    const core::memory::MemoryPolicyConfigResolveResult& policy) noexcept {
  using E=ProcessCryptoStartError;
  if(!SameOwner())return {E::wrong_owner};
  if(attempted_)return {E::already_started};
  attempted_=true;
  if(!policy.ok()||!policy.effective_hard_limit_bytes||!config.memory_openssl_budget_bytes||
      config.memory_openssl_budget_bytes>std::numeric_limits<core::platform::usize>::max())
    return {E::invalid_policy};
  const auto identities=core::uuid::IssueCryptoBootstrapIdentitiesV7();
  if(!identities)return {E::identity_failure};
  try {
    namespace m=core::memory;
    m::HierarchicalMemoryBudget root;
    root.scope={m::HierarchicalMemoryScopeKind::process,{},identities->process.bytes};
    root.hard_limit_bytes=policy.effective_hard_limit_bytes;
    root.provenance.source=m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    root.provenance.source_label=config.memory_policy_provenance;
    if(!ledger_.SetBudget(root).ok())return {E::reservation_failure};
    m::ReservationBackedMemoryResourceRequest request;
    request.memory_manager=&m::DefaultMemoryManager();request.reservation_ledger=&ledger_;
    request.scope_chain={root.scope};request.provenance=root.provenance;
    request.requested_bytes=config.memory_openssl_budget_bytes;
    request.category=m::MemoryCategory::core_runtime;
    request.consumer_kind=m::ReservationBackedMemoryConsumerKind::background_maintenance;
    request.route_label="server_process_crypto_bootstrap";
    request.purpose="process provider backing";request.memory_class="crypto_provider";
    request.binary_operation_uuid=identities->operation.bytes;
    request.binary_ownership[m::MemoryBinaryScopeKind::process]=identities->process.bytes;
    request.binary_ownership[m::MemoryBinaryScopeKind::owner]=identities->owner.bytes;
    request.binary_ownership[m::MemoryBinaryScopeKind::context]=identities->context.bytes;
    auto acquired=m::AcquireReservationBackedMemoryResource(request);
    if(!acquired.ok())return {E::reservation_failure};
    const core::hash::CryptoMemoryBinding binding{{},identities->operation,
        identities->owner,identities->context,identities->process};
    auto result=pool_.Adopt(binding,acquired.resource,
        static_cast<core::platform::usize>(config.memory_openssl_budget_bytes));
    if(!result.ok())return {E::pool_failure,result};
    result=pool_.InstallProcessAdapter();
    if(!result.ok())return {E::pool_failure,result};
    installed_=true;
    // No bootstrap UUID fallback is left selected after provider readiness.
    diagnostics_.reset();
    if(core::uuid::SealCryptoBootstrapDiagnosticIdentities()!=
        core::uuid::CryptoBootstrapDiagnosticError::none)return {E::diagnostic_seal_failure};
    return {E::none};
  } catch(const std::bad_alloc&) {return {E::reservation_failure};}
    catch(const std::system_error&) {return {E::reservation_failure};}
}
core::runtime::RuntimeCryptoPoolResult ServerProcessCryptoOwner::CloseAfterUserDrain() noexcept {
  namespace r=core::runtime;namespace h=core::hash;
  if(!SameOwner())return {r::RuntimeCryptoPoolError::invalid_binding};
  if(installed_) {
    const auto state=pool_.Snapshot();
    if(state.observation_error!=h::CryptoMemoryError::none)
      return {r::RuntimeCryptoPoolError::adapter_failure,state.observation_error};
    if(state.scopes)return {r::RuntimeCryptoPoolError::adapter_failure,h::CryptoMemoryError::busy};
    if(!provider_cleaned_) {
      OPENSSL_thread_stop();OPENSSL_cleanup();provider_cleaned_=true;
    }
    const auto stopped=h::StopCryptoMemoryAdapter();
    if(stopped!=h::CryptoMemoryError::none)return {r::RuntimeCryptoPoolError::adapter_failure,stopped};
    installed_=false;
  }
  return pool_.Close();
}
} // namespace scratchbird::server
