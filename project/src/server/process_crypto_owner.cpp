// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "process_crypto_owner.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <limits>
#include <tuple>
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
    process_scope_=root.scope;provenance_=root.provenance;
    process_limit_bytes_=root.hard_limit_bytes;
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
    process_crypto_token_=acquired.resource->reservation_token();
    const core::hash::CryptoMemoryBinding binding{{},identities->operation,
        identities->owner,identities->context,identities->process};
    auto result=pool_.Adopt(binding,acquired.resource,
        static_cast<core::platform::usize>(config.memory_openssl_budget_bytes),
        m::kDefaultNativeCryptoProtectedMemoryPolicy);
    if(!result.ok())return {E::pool_failure,std::move(result)};
    result=pool_.InstallProcessAdapter();
    if(!result.ok())return {E::pool_failure,std::move(result)};
    installed_=true;
    // No bootstrap UUID fallback is left selected after provider readiness.
    diagnostics_.reset();
    if(core::uuid::SealCryptoBootstrapDiagnosticIdentities()!=
        core::uuid::CryptoBootstrapDiagnosticError::none)return {E::diagnostic_seal_failure};
    return {E::none};
  } catch(const std::bad_alloc&) {return {E::reservation_failure};}
    catch(const std::system_error&) {return {E::reservation_failure};}
}
NativeBootstrapMemoryResult ServerProcessCryptoOwner::AcquireNativeBootstrapResource(
    NativeBootstrapMemoryRequest input) noexcept {
  namespace m=core::memory;using E=NativeBootstrapMemoryError;
  if(!SameOwner())return {E::wrong_owner};
  if(acquiring_native_)return {E::busy};
  if(!installed_||provider_cleaned_)return {E::not_ready};
  if(!input.bytes||input.bytes>std::numeric_limits<core::platform::usize>::max()||
      (input.kind!=NativeBootstrapMemoryKind::storage&&input.kind!=NativeBootstrapMemoryKind::database_crypto))
    return {E::invalid_request};
  for(const auto& id:{input.database,input.operation,input.owner,input.context})
    if(!m::MemorySystemUuidValid(id.bytes))return {E::invalid_request};
  // This reservation flag, not a held mutex, spans all governor callbacks.
  // Reentrant acquisition/terminal cleanup cannot retire the issuing ledger.
  acquiring_native_=true;
  struct Release {bool& flag;~Release(){flag=false;}} release{acquiring_native_};
  try {
    m::HierarchicalMemoryBudget database;
    database.scope={m::HierarchicalMemoryScopeKind::database,{},input.database.bytes};
    const auto selected=native_memory_envelopes_.find(input.database);
    database.hard_limit_bytes=selected==native_memory_envelopes_.end()?process_limit_bytes_:
        selected->second.envelope.effective_hard_limit_bytes;
    if(selected!=native_memory_envelopes_.end()) {
      database.soft_limit_bytes=selected->second.envelope.enforced_soft_limit_bytes;
      const auto category=input.kind==NativeBootstrapMemoryKind::storage?
          m::MemoryCategory::page_buffer:m::MemoryCategory::core_runtime;
      const auto limit=input.kind==NativeBootstrapMemoryKind::storage?
          selected->second.envelope.page_buffer_limit_bytes:selected->second.envelope.crypto_limit_bytes;
      // This private ledger has one admission owner. acquiring_native_ fences
      // every new reservation across this observation and Reserve; releases
      // may only reduce the charge. Retained revoked payloads still count.
      const auto used=ledger_.ReservationBytesInScope(
          m::HierarchicalMemoryScopeKind::database,input.database.bytes,category);
      if(limit&&(used>limit||input.bytes>limit-used)) {
        NativeBootstrapMemoryResult refused;refused.error=E::child_budget_refused;
        using namespace core::platform;
        refused.admission.status={StatusCode::memory_limit_exceeded,Severity::error,Subsystem::memory};
        refused.admission.diagnostic=MakeDiagnostic(StatusCode::memory_limit_exceeded,Severity::error,
            Subsystem::memory,"SB-MEMORY-BUDGET-HARD-LIMIT-REFUSED","memory.budget.hard_limit.refused",
            {{"memory_class",input.kind==NativeBootstrapMemoryKind::storage?"native_storage":"crypto_provider"},
             {"existing_bytes",std::to_string(used)},{"requested_bytes",std::to_string(input.bytes)},
             {"hard_limit_bytes",std::to_string(limit)}},{},"server.native_memory_policy",{});
        return refused;
      }
    }
    database.provenance=provenance_;
    NativeBootstrapMemoryResult result;
    auto budget=ledger_.SetBudget(database);
    if(!budget.ok()) {
      result.error=E::reservation_failure;result.admission.status=budget.status;
      result.admission.diagnostic=std::move(budget.diagnostic);return result;
    }
    m::ReservationBackedMemoryResourceRequest request;
    request.memory_manager=&m::DefaultMemoryManager();request.reservation_ledger=&ledger_;
    request.scope_chain={process_scope_,database.scope};request.provenance=provenance_;
    request.requested_bytes=input.bytes;
    request.category=input.kind==NativeBootstrapMemoryKind::storage?m::MemoryCategory::page_buffer:m::MemoryCategory::core_runtime;
    request.consumer_kind=m::ReservationBackedMemoryConsumerKind::background_maintenance;
    request.route_label="server_native_bootstrap";
    request.purpose="native database bootstrap backing";
    request.memory_class=input.kind==NativeBootstrapMemoryKind::storage?"native_storage":"crypto_provider";
    request.binary_operation_uuid=input.operation.bytes;
    request.binary_ownership[m::MemoryBinaryScopeKind::database]=input.database.bytes;
    request.binary_ownership[m::MemoryBinaryScopeKind::owner]=input.owner.bytes;
    request.binary_ownership[m::MemoryBinaryScopeKind::context]=input.context.bytes;
    // Database crypto uses an explicit database scope. Its physical charge
    // still includes process_scope_; it is not the process-default provider.
    result.admission=m::AcquireReservationBackedMemoryResource(std::move(request));
    result.error=result.admission.ok()?E::none:E::reservation_failure;
    return result;
  }catch(const std::bad_alloc&){return {E::allocation_failure};}
   catch(const std::system_error&){return {E::synchronization_failure};}
}
NativeDatabaseMemoryEnvelopeResult ServerProcessCryptoOwner::ConstrainNativeDatabaseMemory(
    core::platform::Uuid database, const core::catalog::CatalogMemoryConfiguration& config,
    core::platform::Uuid version) noexcept {
  namespace m=core::memory;using E=NativeDatabaseMemoryEnvelopeError;
  if(!SameOwner())return {E::wrong_owner};
  if(acquiring_native_)return {E::busy};
  if(!installed_||provider_cleaned_)return {E::not_ready};
  if(!m::MemorySystemUuidValid(database.bytes)||!m::MemorySystemUuidValid(config.configuration_uuid.bytes)||
      !m::MemorySystemUuidValid(version.bytes)||!config.generation||
      core::catalog::ValidateCatalogMemoryConfigurationSettings(config)!=core::catalog::CatalogValueError::none)
    return {E::invalid_request};
  acquiring_native_=true;
  struct Release {bool& flag;~Release(){flag=false;}} release{acquiring_native_};
  NativeDatabaseMemoryEnvelopeResult result;
  result.database=database;result.configuration=config.configuration_uuid;result.version=version;
  result.configuration_generation=config.generation;
  result.configured_hard_limit_bytes=config.hard_limit_bytes;
  result.configured_soft_limit_bytes=config.soft_limit_bytes;
  result.enforced_soft_limit_bytes=config.reject_over_soft_limit?config.soft_limit_bytes:0;
  result.page_buffer_limit_bytes=config.page_buffer_pool_limit_bytes;
  result.crypto_limit_bytes=config.openssl_budget_bytes;
  try {
    const auto previous=native_memory_envelopes_.find(database);
    if(previous!=native_memory_envelopes_.end()){
      const auto& active=previous->second.envelope;
      if(active.configuration!=result.configuration||active.version!=version||
          active.configuration_generation!=config.generation||
          active.configured_hard_limit_bytes!=config.hard_limit_bytes||
          active.configured_soft_limit_bytes!=result.configured_soft_limit_bytes||
          active.enforced_soft_limit_bytes!=result.enforced_soft_limit_bytes||
          active.page_buffer_limit_bytes!=result.page_buffer_limit_bytes||active.crypto_limit_bytes!=result.crypto_limit_bytes)
        return {E::binding_mismatch};
      return active;
    }
    // Resolve the actual catalog settings against the real platform. These
    // definition generations are resolver metadata, never an engine epoch or
    // an activation receipt. No override, degraded allowance or source path
    // can be supplied by this retained-source-only boundary.
    m::MemoryPolicyConfig policy;
    policy.policy_name=config.policy_name;policy.provenance=config.provenance;
    policy.hard_limit_bytes=config.hard_limit_bytes;policy.soft_limit_bytes=config.soft_limit_bytes;
    policy.per_context_limit_bytes=config.per_context_limit_bytes;
    policy.page_buffer_pool_limit_bytes=config.page_buffer_pool_limit_bytes;
    policy.failure_mode=config.failure_mode==core::catalog::MemoryConfigurationFailureMode::fatal_status?
        m::AllocationFailureMode::fatal_status:m::AllocationFailureMode::return_error;
    policy.track_allocations=config.track_allocations;
    policy.zero_memory_on_allocate=config.zero_memory_on_allocate;
    policy.zero_memory_on_release=config.zero_memory_on_release;
    policy.reject_over_soft_limit=config.reject_over_soft_limit;
    policy.enable_platform_memory_probe=config.enable_platform_memory_probe;
    policy.require_platform_memory_ceiling=config.require_platform_memory_ceiling;
    policy.source_epoch=config.policy_pack_generation;
    policy.reload_generation=policy.policy_generation=config.generation;
    auto resolved=m::ResolveMemoryPolicyConfig(policy);
    result.effective_hard_limit_bytes=std::min(resolved.effective_hard_limit_bytes,process_limit_bytes_);
    if(!resolved.ok()) {
      result.error=E::policy_refused;result.diagnostic=std::move(resolved.diagnostics.front());return result;
    }
    // The process ceiling is an additional parent constraint. Never silently
    // trim configured child limits or the architect's minimum startup floor.
    if(config.soft_limit_bytes>result.effective_hard_limit_bytes||
        config.per_context_limit_bytes>result.effective_hard_limit_bytes||
        config.page_buffer_pool_limit_bytes>result.effective_hard_limit_bytes||
        config.openssl_budget_bytes>result.effective_hard_limit_bytes) {
      result.error=E::budget_refused;return result;
    }
    if(config.min_startup_available_bytes>result.effective_hard_limit_bytes) {
      result.error=E::startup_floor_unavailable;
      using namespace core::platform;
      result.diagnostic=MakeDiagnostic(StatusCode::memory_invalid_request,Severity::error,Subsystem::memory,
          "CONFIG.MEMORY_POLICY_MIN_STARTUP_UNAVAILABLE","config.memory_policy_min_startup_unavailable",
          {{"effective_hard_limit_bytes",std::to_string(result.effective_hard_limit_bytes)},
           {"min_startup_available_bytes",std::to_string(config.min_startup_available_bytes)}},
          {},"server.native_memory_policy",{});
      return result;
    }
    for(const auto& [category,limit,error]:{
        std::tuple{m::MemoryCategory::core_runtime,config.openssl_budget_bytes,E::crypto_budget_refused},
        std::tuple{m::MemoryCategory::page_buffer,config.page_buffer_pool_limit_bytes,E::page_budget_refused}}) {
      const auto used=ledger_.ReservationBytesInScope(m::HierarchicalMemoryScopeKind::database,database.bytes,category);
      if(limit&&used>limit) {
        result.error=error;using namespace core::platform;
        result.diagnostic=MakeDiagnostic(StatusCode::memory_limit_exceeded,Severity::error,Subsystem::memory,
            "SB-MEMORY-BUDGET-LIVE-SHRINK-REFUSED","memory.budget.live_shrink.refused",
            {{"memory_class",category==m::MemoryCategory::page_buffer?"native_storage":"crypto_provider"},
             {"existing_bytes",std::to_string(used)},{"requested_hard_limit_bytes",std::to_string(limit)},
             {"existing_work_rule","reject_change"}},{},"server.native_memory_policy",{});
        return result;
      }
    }
    result.error=E::none;
    m::HierarchicalMemoryBudget budget;
    budget.scope={m::HierarchicalMemoryScopeKind::database,{},database.bytes};
    budget.hard_limit_bytes=result.effective_hard_limit_bytes;
    budget.soft_limit_bytes=result.enforced_soft_limit_bytes;budget.provenance=provenance_;
    // Prepare the owning map node before the atomic ledger change. No failed
    // installation leaves a selected constraint or changes existing credit.
    const auto [position,inserted]=native_memory_envelopes_.emplace(database,NativeMemoryState{result});
    struct Undo {
      decltype(native_memory_envelopes_)& entries;decltype(position) item;bool keep=false;
      ~Undo(){if(!keep)entries.erase(item);}
    } undo{native_memory_envelopes_,position};
    auto changed=ledger_.SetBudget(std::move(budget));
    if(!changed.ok()) {
      result.error=E::budget_refused;result.diagnostic=std::move(changed.diagnostic);return result;
    }
    undo.keep=true;
    return result;
  }catch(const std::bad_alloc&){return {E::allocation_failure};}
   catch(const std::system_error&){return {E::synchronization_failure};}
}
NativeDatabaseAllocationBehaviorResult ServerProcessCryptoOwner::InstallNativeDatabaseAllocationBehavior(
    core::platform::Uuid database, const core::catalog::CatalogMemoryConfiguration& config,
    core::platform::Uuid version) noexcept {
  namespace m=core::memory;using E=NativeDatabaseMemoryEnvelopeError;
  if(!SameOwner())return {E::wrong_owner};
  if(acquiring_native_)return {E::busy};
  if(!installed_||provider_cleaned_)return {E::not_ready};
  acquiring_native_=true;
  struct Leave {bool& flag;~Leave(){flag=false;}} leave{acquiring_native_};
  try {
    const auto selected=native_memory_envelopes_.find(database);
    if(selected==native_memory_envelopes_.end())return {E::not_ready};
    const auto& envelope=selected->second.envelope;
    if(envelope.configuration!=config.configuration_uuid||envelope.version!=version||
        envelope.configuration_generation!=config.generation)return {E::binding_mismatch};
    m::DatabaseAllocationBehavior behavior;
    behavior.database_uuid=database.bytes;
    behavior.binding={config.configuration_uuid.bytes,config.generation};
    behavior.per_context_limit_bytes=config.per_context_limit_bytes;
    behavior.failure_mode=config.failure_mode==core::catalog::MemoryConfigurationFailureMode::fatal_status?
        m::AllocationFailureMode::fatal_status:m::AllocationFailureMode::return_error;
    behavior.zero_memory_on_allocate=config.zero_memory_on_allocate;
    behavior.zero_memory_on_release=config.zero_memory_on_release;
    auto installed=m::DefaultMemoryManager().allocator()->InstallDatabaseAllocationBehavior(behavior);
    if(!installed.ok())return {E::budget_refused,installed};
    selected->second.allocation_behavior_installed=true;
    return {E::none,installed};
  }catch(const std::bad_alloc&){return {E::allocation_failure};}
   catch(const std::system_error&){return {E::synchronization_failure};}
}
bool ServerProcessCryptoOwner::HasNativeDatabaseAllocationBehavior(
    core::platform::Uuid database, const core::catalog::CatalogMemoryConfiguration& config,
    core::platform::Uuid version) const noexcept {
  if(!SameOwner()||acquiring_native_||!installed_||provider_cleaned_)return false;
  const auto found=native_memory_envelopes_.find(database);
  if(found==native_memory_envelopes_.end()||!found->second.allocation_behavior_installed)return false;
  const auto& envelope=found->second.envelope;
  return envelope.configuration==config.configuration_uuid&&envelope.version==version&&
      envelope.configuration_generation==config.generation;
}
NativeDatabaseMemoryEnvelopeError ServerProcessCryptoOwner::RetireNativeDatabaseMemory(
    core::platform::Uuid database) noexcept {
  using E=NativeDatabaseMemoryEnvelopeError;
  if(!SameOwner())return E::wrong_owner;
  if(acquiring_native_)return E::busy;
  acquiring_native_=true;
  struct Release {bool& flag;~Release(){flag=false;}} release{acquiring_native_};
  try {
    // Includes revoked resources still retained by payload owners. Keep the
    // selected limit and original host custody until the last loan releases.
    if(ledger_.HasReservationsInScope(core::memory::HierarchicalMemoryScopeKind::database,database.bytes))
      return E::busy;
    const auto selected=native_memory_envelopes_.find(database);
    if(selected!=native_memory_envelopes_.end()&&selected->second.allocation_behavior_installed) {
      const auto& envelope=selected->second.envelope;
      const auto retired=core::memory::DefaultMemoryManager().allocator()->RetireDatabaseAllocationBehavior(
          database.bytes,{envelope.configuration.bytes,envelope.configuration_generation});
      if(retired!=core::memory::DatabaseAllocationBehaviorStatus::applied)return E::busy;
    }
    native_memory_envelopes_.erase(database);
    return E::none;
  }catch(const std::system_error&){return E::synchronization_failure;}
}
core::runtime::RuntimeCryptoPoolResult ServerProcessCryptoOwner::PrepareNativeLocalPasswordAlgorithms() noexcept {
  namespace r=core::runtime;namespace h=core::hash;
  if(!SameOwner())return {r::RuntimeCryptoPoolError::invalid_binding};
  if(!installed_||provider_cleaned_)return {r::RuntimeCryptoPoolError::invalid_grant};
  if(preparing_native_password_)return {r::RuntimeCryptoPoolError::adapter_failure,h::CryptoMemoryError::busy};
  preparing_native_password_=true;
  struct Release {bool& flag;~Release(){flag=false;}} release{preparing_native_password_};
  try {
    bool loaded=native_password_algorithms_prepared_;
    const auto scoped=WithProviderScope([&] {
      if(loaded)return;
      // Exercise the same primitive with public, noncredential input. This
      // prepares its process-global KDF/MAC/digest caches in process backing
      // on supported OpenSSL versions, including implementations without the
      // OpenSSL 3 fetch APIs. One round is resource preparation only: no catalog
      // verifier, credential source, authenticated result or secret is involved.
      constexpr char public_input[]="ScratchBird provider resource preparation";
      const unsigned char public_salt[16]{};
      unsigned char public_output[32]{};
      loaded=PKCS5_PBKDF2_HMAC(public_input,sizeof(public_input)-1,public_salt,sizeof(public_salt),
          1,EVP_sha256(),sizeof(public_output),public_output)==1;
      OPENSSL_cleanse(public_output,sizeof(public_output));
    });
    if(!scoped.ok())return scoped;
    if(!loaded)return {r::RuntimeCryptoPoolError::adapter_failure,h::CryptoMemoryError::provider_failure};
    native_password_algorithms_prepared_=true;return {};
  }catch(const std::bad_alloc&){return {r::RuntimeCryptoPoolError::allocation_failed};}
   catch(const std::system_error&){return {r::RuntimeCryptoPoolError::synchronization_failure};}
}
core::runtime::RuntimeCryptoPoolResult ServerProcessCryptoOwner::CloseAfterUserDrain() noexcept {
  namespace r=core::runtime;namespace h=core::hash;
  if(!SameOwner())return {r::RuntimeCryptoPoolError::invalid_binding};
  if(acquiring_native_||preparing_native_password_||!native_memory_envelopes_.empty())
    return {r::RuntimeCryptoPoolError::adapter_failure,h::CryptoMemoryError::busy};
  try {
    if(ledger_.HasReservationsOtherThan(pool_.has_custody()?process_crypto_token_:
        core::memory::HierarchicalMemoryReservationToken{}))
      return {r::RuntimeCryptoPoolError::adapter_failure,h::CryptoMemoryError::busy};
  }catch(const std::system_error&){return {r::RuntimeCryptoPoolError::synchronization_failure};}
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
