// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "config.hpp"
#include "diagnostics.hpp"
#include "../core/runtime/runtime_crypto_pool_owner.hpp"
#include <optional>
#include <thread>

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
  bool SameOwner() const noexcept;
  const std::uint64_t process_id_;
  const std::thread::id thread_id_;
  std::optional<BootstrapDiagnosticScope> diagnostics_;
  core::memory::HierarchicalMemoryBudgetLedger ledger_;
  core::runtime::RuntimeCryptoPoolOwner pool_;
  bool attempted_=false,installed_=false,provider_cleaned_=false;
};
} // namespace scratchbird::server
