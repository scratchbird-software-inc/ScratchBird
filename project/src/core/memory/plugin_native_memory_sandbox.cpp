// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-027: plugin/UDR and native-library memory sandbox.
#include "plugin_native_memory_sandbox.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace scratchbird::core::memory {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

constexpr const char* kEvidenceAnchor =
    "CEIC-027_PLUGIN_NATIVE_MEMORY_SANDBOX";
constexpr const char* kAuthorityScope =
    "plugin_native_memory_sandbox.authority_scope=memory_evidence_only_not_transaction_finality_visibility_authorization_security_recovery_parser_donor_wal_benchmark_optimizer_plan_index_finality_cluster_or_agent_action_authority";

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::memory};
}

Status ErrorStatus(StatusCode code = StatusCode::memory_invalid_request,
                   Severity severity = Severity::error) {
  return {code, severity, Subsystem::memory};
}

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

bool Blank(const std::string& value) {
  return value.find_first_not_of(" \t\r\n") == std::string::npos;
}

void UpdatePeak(u64 current, u64* peak) {
  if (current > *peak) {
    *peak = current;
  }
}

std::string ScopeKey(const HierarchicalMemoryScopeRef& scope) {
  return std::string(HierarchicalMemoryScopeKindName(scope.kind)) + ":" +
         scope.scope_id;
}

DiagnosticRecord MakeSandboxDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::vector<DiagnosticArgument> arguments = {}) {
  arguments.push_back({"authority_scope", kAuthorityScope});
  return MakeDiagnostic(
      status.code,
      status.severity,
      status.subsystem,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(arguments),
      {},
      "core.memory.plugin_native_memory_sandbox",
      "Reserve plugin/UDR and native-library memory through CEIC-011, CEIC-016, and CEIC-026 before native code is admitted.");
}

bool RuntimeProvenanceSource(HierarchicalMemoryBudgetProvenanceSource source) {
  switch (source) {
    case HierarchicalMemoryBudgetProvenanceSource::runtime_policy:
    case HierarchicalMemoryBudgetProvenanceSource::server_runtime_api:
    case HierarchicalMemoryBudgetProvenanceSource::agent_runtime:
      return true;
    case HierarchicalMemoryBudgetProvenanceSource::unknown:
    case HierarchicalMemoryBudgetProvenanceSource::execution_plan_evidence:
    case HierarchicalMemoryBudgetProvenanceSource::test_fixture:
    case HierarchicalMemoryBudgetProvenanceSource::synthetic_evidence:
      break;
  }
  return false;
}

bool SafeProvenance(const HierarchicalMemoryBudgetProvenance& provenance,
                    std::string* reason) {
  if (!RuntimeProvenanceSource(provenance.source)) {
    *reason = "runtime_policy_server_api_or_agent_runtime_source_required";
    return false;
  }
  if (Blank(provenance.source_label)) {
    *reason = "non_empty_runtime_source_label_required";
    return false;
  }
  if (!provenance.engine_mga_authoritative || !provenance.memory_evidence_only) {
    *reason = "engine_mga_and_memory_evidence_only_provenance_required";
    return false;
  }
  if (provenance.parser_authority || provenance.donor_authority ||
      provenance.transaction_finality_authority ||
      provenance.visibility_authority || provenance.recovery_authority ||
      provenance.authorization_authority || provenance.benchmark_authority ||
      provenance.support_bundle_authority || provenance.cluster_authority ||
      provenance.debug_or_relaxed_path) {
    *reason = "unsafe_authority_or_relaxed_provenance_refused";
    return false;
  }
  return true;
}

bool SafeAuthority(const ForeignMemoryAuthority& authority, std::string* reason) {
  if (!authority.engine_mga_authoritative || !authority.memory_evidence_only) {
    *reason = "engine_mga_and_memory_evidence_only_authority_required";
    return false;
  }
  if (!authority.security_or_policy_checked) {
    *reason = "security_or_policy_check_required";
    return false;
  }
  if (!authority.evidence_fresh || Blank(authority.evidence_label) ||
      Blank(authority.authority_generation)) {
    *reason = "fresh_non_empty_authority_evidence_required";
    return false;
  }
  if (authority.transaction_finality_authority || authority.visibility_authority ||
      authority.recovery_authority || authority.parser_authority ||
      authority.donor_authority || authority.wal_authority ||
      authority.benchmark_authority || authority.support_bundle_authority ||
      authority.optimizer_plan_authority || authority.index_finality_authority ||
      authority.agent_action_authority || authority.authorization_authority ||
      authority.cluster_authority || authority.debug_or_relaxed_path) {
    *reason = "unsafe_authority_claim_refused";
    return false;
  }
  return true;
}

bool ValidateScopeChain(const std::vector<HierarchicalMemoryScopeRef>& chain,
                        std::string* reason) {
  if (chain.empty()) {
    *reason = "scope_chain_required";
    return false;
  }
  std::set<std::string> seen;
  for (const auto& scope : chain) {
    if (Blank(scope.scope_id)) {
      *reason = "scope_id_required";
      return false;
    }
    if (!seen.insert(ScopeKey(scope)).second) {
      *reason = "duplicate_scope_refused";
      return false;
    }
  }
  return true;
}

bool PluginClassProofPresent(const MemoryBudgetLeaseSnapshot& snapshot,
                             std::string* reason) {
  for (const auto& cls : snapshot.classes) {
    if (cls.kind == MemoryClassKind::plugin_udr) {
      if (!cls.plugin_udr_class) {
        *reason = "plugin_udr_class_flag_missing";
        return false;
      }
      if (cls.category != MemoryCategory::udr_reserved) {
        *reason = "plugin_udr_class_category_mismatch";
        return false;
      }
      if (cls.max_lease_ms == 0) {
        *reason = "plugin_udr_class_unbounded_lease";
        return false;
      }
      if (cls.high_pressure_action !=
          MemoryClassPressureAction::sandbox_plugin_udr) {
        *reason = "plugin_udr_class_pressure_action_missing";
        return false;
      }
      return true;
    }
  }
  *reason = "plugin_udr_class_policy_missing";
  return false;
}

bool EvidenceContains(const std::vector<std::string>& evidence,
                      const std::string& token) {
  for (const auto& entry : evidence) {
    if (entry.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

u64 EffectiveEstimatedBytes(const PluginNativeMemorySandboxRequest& request,
                            bool* conservative_estimate) {
  u64 bytes = request.estimated_bytes;
  if (request.observed_bytes > bytes) {
    bytes = request.observed_bytes;
  }
  if (!request.provider_available && request.allow_conservative_estimate &&
      request.conservative_estimated_bytes > bytes) {
    bytes = request.conservative_estimated_bytes;
    *conservative_estimate = true;
  }
  return bytes;
}

ForeignMemoryReservationRequest ForeignRequestFromSandbox(
    const PluginNativeMemorySandboxRequest& request,
    u64 estimated_bytes,
    bool conservative_estimate,
    HierarchicalMemoryBudgetLedger* reservation_ledger) {
  ForeignMemoryReservationRequest foreign;
  foreign.source = request.source;
  foreign.reservation_ledger = reservation_ledger;
  foreign.scope_chain = request.scope_chain;
  foreign.category = DefaultForeignMemoryCategory(request.source);
  foreign.memory_class =
      std::string("ceic_027.plugin_native.") +
      ForeignMemorySourceName(request.source);
  foreign.estimated_bytes = estimated_bytes;
  foreign.observed_bytes = request.observed_bytes;
  foreign.owner_id = request.owner_id;
  foreign.owning_scope = request.sandbox_id;
  foreign.operation_id = request.operation_id;
  foreign.native_callsite = request.native_callsite;
  foreign.confidence =
      conservative_estimate ? ForeignMemoryConfidence::conservative
                            : request.confidence;
  foreign.expected_release_event = request.expected_release_event;
  foreign.over_limit_action = request.over_limit_action;
  foreign.linkage_mode = request.linkage_mode;
  foreign.production_like = true;
  foreign.live_route_claim = request.live_route_claim;
  foreign.untracked_high_risk_native_call = false;
  foreign.conservative_reservation = conservative_estimate;
  foreign.provenance = request.provenance;
  foreign.authority = request.authority;
  foreign.authority.provider_available = request.provider_available;
  foreign.evidence = {
      kEvidenceAnchor,
      "plugin_native_memory_sandbox.foreign_handle=CEIC-016",
      "plugin_native_memory_sandbox.class_lease=CEIC-026_plugin_udr",
      "plugin_native_memory_sandbox.plugin_allocator_abi=" +
          request.plugin_allocator_abi,
      "plugin_native_memory_sandbox.plugin_memory_context_id=" +
          request.plugin_memory_context_id,
      "plugin_native_memory_sandbox.invocation_budget_bytes=" +
          std::to_string(request.invocation_budget_bytes),
      "plugin_native_memory_sandbox.result_buffer_owner=engine",
      std::string("plugin_native_memory_sandbox.provider_available=") +
          BoolText(request.provider_available),
      std::string("plugin_native_memory_sandbox.live_provider_proof=") +
          BoolText(request.live_provider_proof),
      std::string("plugin_native_memory_sandbox.conservative_estimate=") +
          BoolText(conservative_estimate)};
  for (const auto& entry : request.evidence) {
    foreign.evidence.push_back("plugin_native_memory_sandbox.request_evidence=" +
                               entry);
  }
  return foreign;
}

}  // namespace

const std::vector<ForeignMemorySource>& PluginNativeMemorySandboxModeledSources() {
  static const std::vector<ForeignMemorySource> sources = {
      ForeignMemorySource::plugin_udr,
      ForeignMemorySource::llvm,
      ForeignMemorySource::icu,
      ForeignMemorySource::crypto,
      ForeignMemorySource::regex,
      ForeignMemorySource::json,
      ForeignMemorySource::compression,
      ForeignMemorySource::mmap,
      ForeignMemorySource::thread_stack,
      ForeignMemorySource::driver_native,
      ForeignMemorySource::gpu_optional};
  return sources;
}

bool PluginNativeMemorySandboxCoversSource(ForeignMemorySource source) {
  const auto& sources = PluginNativeMemorySandboxModeledSources();
  return std::find(sources.begin(), sources.end(), source) != sources.end();
}

PluginNativeMemorySandboxReservation::~PluginNativeMemorySandboxReservation() {
  if (active()) {
    (void)Release(ForeignMemoryReleaseEvent::scope_exit,
                  {"plugin_native_memory_sandbox.release.scope_exit=true"});
  }
}

PluginNativeMemorySandboxReservation::PluginNativeMemorySandboxReservation(
    ManagerOnlyTag,
    PluginNativeMemorySandboxManager* owner,
    PluginNativeMemorySandboxToken token,
    std::shared_ptr<HandleState> state)
    : owner_(owner), token_(token), state_(std::move(state)) {}

bool PluginNativeMemorySandboxReservation::active() const {
  return owner_ != nullptr && state_ != nullptr && token_.valid() &&
         !state_->released.load();
}

const PluginNativeMemorySandboxToken&
PluginNativeMemorySandboxReservation::token() const {
  return token_;
}

PluginNativeMemorySandboxReleaseResult
PluginNativeMemorySandboxReservation::Release(
    ForeignMemoryReleaseEvent event,
    std::vector<std::string> release_evidence) {
  if (owner_ == nullptr || state_ == nullptr || !token_.valid()) {
    PluginNativeMemorySandboxReleaseResult result;
    result.status = ErrorStatus(StatusCode::memory_unknown_pointer);
    result.fail_closed = true;
    result.diagnostic = MakeSandboxDiagnostic(
        result.status,
        "SB-CEIC-027-PLUGIN-NATIVE-RELEASE-HANDLE-INVALID",
        "memory.ceic_027.plugin_native.release_handle_invalid",
        {{"reason", "reservation_handle_invalid"}});
    result.evidence.push_back(kEvidenceAnchor);
    result.evidence.push_back(kAuthorityScope);
    return result;
  }
  if (state_->released.exchange(true)) {
    PluginNativeMemorySandboxReleaseResult result;
    result.status = OkStatus();
    result.released = true;
    result.evidence.push_back(kEvidenceAnchor);
    result.evidence.push_back(kAuthorityScope);
    result.evidence.push_back(
        "plugin_native_memory_sandbox.release.already_released=true");
    return result;
  }
  auto result = owner_->Release(token_, event, std::move(release_evidence));
  if (!result.ok()) {
    state_->released.store(false);
  }
  return result;
}

PluginNativeMemorySandboxReleaseResult
PluginNativeMemorySandboxReservation::Release(
    std::vector<std::string> release_evidence) {
  return Release(ForeignMemoryReleaseEvent::explicit_release,
                 std::move(release_evidence));
}

PluginNativeMemorySandboxActiveSnapshot
PluginNativeMemorySandboxReservation::Snapshot() const {
  if (owner_ == nullptr || !token_.valid()) {
    PluginNativeMemorySandboxActiveSnapshot snapshot;
    snapshot.token = token_;
    snapshot.evidence.push_back(kEvidenceAnchor);
    snapshot.evidence.push_back(kAuthorityScope);
    snapshot.evidence.push_back(
        "plugin_native_memory_sandbox.snapshot.invalid_handle=true");
    return snapshot;
  }
  const auto snapshot = owner_->Snapshot();
  for (const auto& active : snapshot.active_reservations) {
    if (active.token.sandbox_reservation_id == token_.sandbox_reservation_id) {
      return active;
    }
  }
  PluginNativeMemorySandboxActiveSnapshot missing;
  missing.token = token_;
  missing.evidence.push_back(kEvidenceAnchor);
  missing.evidence.push_back(kAuthorityScope);
  missing.evidence.push_back(
      "plugin_native_memory_sandbox.snapshot.reservation_missing=true");
  return missing;
}

PluginNativeMemorySandboxManager::PluginNativeMemorySandboxManager(
    HierarchicalMemoryBudgetLedger* reservation_ledger,
    ForeignMemoryReservationLedger* foreign_ledger,
    MemoryClassPolicyLeaseManager* class_lease_manager)
    : reservation_ledger_(reservation_ledger),
      foreign_ledger_(foreign_ledger),
      class_lease_manager_(class_lease_manager) {}

PluginNativeMemorySandboxAcquireResult
PluginNativeMemorySandboxManager::Acquire(
    PluginNativeMemorySandboxRequest request) {
  if (request.authority.evidence_label.empty()) {
    request.authority.evidence_label = "ceic_027_plugin_native_sandbox";
  }
  if (request.authority.authority_generation.empty()) {
    request.authority.authority_generation = "ceic-027-runtime";
  }
  request.authority.provider_available = request.provider_available;

  bool conservative_estimate = false;
  const u64 estimated_bytes =
      EffectiveEstimatedBytes(request, &conservative_estimate);

  std::string reason;
  if (reservation_ledger_ == nullptr || foreign_ledger_ == nullptr ||
      class_lease_manager_ == nullptr) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++missing_class_lease_refusal_count_;
    }
    return RefuseAcquire(request,
                         "SB-CEIC-027-PLUGIN-NATIVE-MANAGER-MISSING",
                         "memory.ceic_027.plugin_native.manager_missing",
                         "ceic_011_016_026_managers_required");
  }
  if (!PluginNativeMemorySandboxCoversSource(request.source)) {
    return RefuseAcquire(request,
                         "SB-CEIC-027-PLUGIN-NATIVE-SOURCE-REFUSED",
                         "memory.ceic_027.plugin_native.source_refused",
                         "modeled_plugin_native_source_required");
  }
  if (!ValidateScopeChain(request.scope_chain, &reason) ||
      Blank(request.owner_id) || Blank(request.sandbox_id) ||
      Blank(request.plugin_id) || Blank(request.plugin_allocator_abi) ||
      Blank(request.plugin_memory_context_id) ||
      Blank(request.udr_entrypoint) ||
      Blank(request.operation_id) || Blank(request.native_callsite)) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++missing_identity_refusal_count_;
    }
    if (reason.empty()) {
      reason =
          "owner_sandbox_plugin_allocator_abi_memory_context_udr_operation_and_callsite_required";
    }
    return RefuseAcquire(request,
                         "SB-CEIC-027-PLUGIN-NATIVE-IDENTITY-REFUSED",
                         "memory.ceic_027.plugin_native.identity_refused",
                         reason);
  }
  if (estimated_bytes == 0 || request.confidence == ForeignMemoryConfidence::unknown) {
    return RefuseAcquire(request,
                         "SB-CEIC-027-PLUGIN-NATIVE-ESTIMATE-REFUSED",
                         "memory.ceic_027.plugin_native.estimate_refused",
                         "non_zero_estimate_and_known_confidence_required");
  }
  if (request.invocation_budget_bytes == 0 ||
      estimated_bytes > request.invocation_budget_bytes) {
    return RefuseAcquire(
        request,
        "SB-CEIC-027-PLUGIN-NATIVE-INVOCATION-BUDGET-REFUSED",
        "memory.ceic_027.plugin_native.invocation_budget_refused",
        "plugin_invocation_budget_must_cover_estimated_native_bytes",
        StatusCode::memory_limit_exceeded,
        Severity::error);
  }
  if (!request.result_buffer_owned_by_engine) {
    return RefuseAcquire(
        request,
        "SB-CEIC-027-PLUGIN-NATIVE-RESULT-BUFFER-REFUSED",
        "memory.ceic_027.plugin_native.result_buffer_refused",
        "result_buffer_must_remain_engine_owned");
  }
  if (!request.support_bundle_view_enabled) {
    return RefuseAcquire(
        request,
        "SB-CEIC-027-PLUGIN-NATIVE-SUPPORT-VIEW-REFUSED",
        "memory.ceic_027.plugin_native.support_view_refused",
        "plugin_support_bundle_view_required");
  }
  if (!request.plugin_unload_cleanup_supported) {
    return RefuseAcquire(
        request,
        "SB-CEIC-027-PLUGIN-NATIVE-UNLOAD-CLEANUP-REFUSED",
        "memory.ceic_027.plugin_native.unload_cleanup_refused",
        "plugin_unload_cleanup_hook_required");
  }
  if ((request.pressure_state == MemoryPressureState::high_pressure ||
       request.pressure_state == MemoryPressureState::emergency_pressure) &&
      !request.plugin_cancellation_on_pressure) {
    return RefuseAcquire(
        request,
        "SB-CEIC-027-PLUGIN-NATIVE-PRESSURE-CANCEL-REFUSED",
        "memory.ceic_027.plugin_native.pressure_cancel_refused",
        "plugin_cancellation_on_memory_pressure_required");
  }
  if (request.deadline_ms == 0 || request.deadline_ms <= request.now_ms) {
    return RefuseAcquire(request,
                         "SB-CEIC-027-PLUGIN-NATIVE-DEADLINE-REFUSED",
                         "memory.ceic_027.plugin_native.deadline_refused",
                         "future_class_lease_deadline_required");
  }
  if (request.route_kind == MemoryBudgetLeaseRouteKind::cluster) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++cluster_refusal_count_;
    }
    return RefuseAcquire(
        request,
        "SB-CEIC-027-PLUGIN-NATIVE-CLUSTER-REFUSED",
        "memory.ceic_027.plugin_native.cluster_refused",
        "cluster_reserved_external_cluster_routes_refused",
        StatusCode::memory_invalid_request,
        Severity::error);
  }
  if (!request.production_raw_external_allocation_gate ||
      (request.raw_external_allocation &&
       !request.raw_plugin_allocation_explicitly_allowed)) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++raw_external_allocation_refusal_count_;
    }
    return RefuseAcquire(
        request,
        "SB-CEIC-027-PLUGIN-NATIVE-RAW-EXTERNAL-ALLOCATION-REFUSED",
        "memory.ceic_027.plugin_native.raw_external_allocation_refused",
        "production_gate_rejects_raw_plugin_allocation_without_explicit_allowance");
  }
  if (request.raw_external_allocation &&
      request.raw_plugin_allocation_explicitly_allowed) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++raw_plugin_allocation_allowed_count_;
  }
  if (request.untracked_native_allocation) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++untracked_native_allocation_refusal_count_;
    }
    return RefuseAcquire(
        request,
        "SB-CEIC-027-PLUGIN-NATIVE-UNTRACKED-ALLOCATION-REFUSED",
        "memory.ceic_027.plugin_native.untracked_allocation_refused",
        "untracked_native_allocation_refused");
  }
  if (!request.plugin_udr_sandboxed) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++missing_identity_refusal_count_;
    }
    return RefuseAcquire(
        request,
        "SB-CEIC-027-PLUGIN-NATIVE-SANDBOX-IDENTITY-REFUSED",
        "memory.ceic_027.plugin_native.sandbox_identity_refused",
        "sandbox_identity_and_plugin_udr_sandbox_flag_required");
  }
  if (!SafeProvenance(request.provenance, &reason) ||
      !SafeAuthority(request.authority, &reason)) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++unsafe_provenance_refusal_count_;
    }
    return RefuseAcquire(request,
                         "SB-CEIC-027-PLUGIN-NATIVE-PROVENANCE-REFUSED",
                         "memory.ceic_027.plugin_native.provenance_refused",
                         reason);
  }
  if (!request.provider_available) {
    if (request.live_route_claim || request.live_provider_proof ||
        !request.allow_conservative_estimate ||
        request.source == ForeignMemorySource::gpu_optional) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++absent_provider_fail_closed_count_;
      }
      return RefuseAcquire(
          request,
          "SB-CEIC-027-PLUGIN-NATIVE-PROVIDER-ABSENT-REFUSED",
          "memory.ceic_027.plugin_native.provider_absent_refused",
          "absent_provider_requires_fail_closed_or_non_live_conservative_estimate");
    }
  }
  if (request.live_route_claim && !request.live_provider_proof) {
    return RefuseAcquire(
        request,
        "SB-CEIC-027-PLUGIN-NATIVE-LIVE-PROOF-REFUSED",
        "memory.ceic_027.plugin_native.live_proof_refused",
        "live_route_claim_requires_live_provider_proof");
  }
  if (request.require_plugin_udr_class_proof) {
    const auto class_snapshot = class_lease_manager_->Snapshot();
    if (!PluginClassProofPresent(class_snapshot, &reason)) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stale_class_lease_refusal_count_;
      }
      return RefuseAcquire(
          request,
          "SB-CEIC-027-PLUGIN-NATIVE-CLASS-PROOF-REFUSED",
          "memory.ceic_027.plugin_native.class_proof_refused",
          reason);
    }
  } else {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++missing_class_lease_refusal_count_;
    }
    return RefuseAcquire(
        request,
        "SB-CEIC-027-PLUGIN-NATIVE-CLASS-PROOF-MISSING",
        "memory.ceic_027.plugin_native.class_proof_missing",
        "plugin_udr_class_lease_proof_required");
  }

  MemoryBudgetLeaseRequest lease_request;
  lease_request.scope_chain = request.scope_chain;
  lease_request.class_kind = MemoryClassKind::plugin_udr;
  lease_request.route_kind = request.route_kind;
  lease_request.owner_id = request.owner_id;
  lease_request.requested_bytes = estimated_bytes;
  lease_request.now_ms = request.now_ms;
  lease_request.deadline_ms = request.deadline_ms;
  lease_request.pressure_state = request.pressure_state;
  lease_request.plugin_udr_sandboxed = true;
  lease_request.priority = 5;
  lease_request.weight = 1;
  lease_request.provenance = request.provenance;
  auto class_lease = class_lease_manager_->AcquireLease(std::move(lease_request));
  if (!class_lease.ok() ||
      class_lease.class_kind != MemoryClassKind::plugin_udr ||
      !EvidenceContains(class_lease.evidence,
                        "memory_class_policy_lease.plugin_udr.sandboxed=true")) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++missing_class_lease_refusal_count_;
    }
    auto refused = RefuseAcquire(
        request,
        "SB-CEIC-027-PLUGIN-NATIVE-LEASE-REFUSED",
        "memory.ceic_027.plugin_native.lease_refused",
        class_lease.diagnostic.diagnostic_code.empty()
            ? "plugin_udr_class_lease_refused_or_missing_sandbox_evidence"
            : class_lease.diagnostic.diagnostic_code,
        class_lease.status.code,
        class_lease.status.severity);
    refused.evidence.insert(refused.evidence.end(),
                            class_lease.evidence.begin(),
                            class_lease.evidence.end());
    return refused;
  }

  auto foreign_request = ForeignRequestFromSandbox(
      request, estimated_bytes, conservative_estimate, reservation_ledger_);
  auto foreign =
      AcquireForeignMemoryReservation(foreign_ledger_, std::move(foreign_request));
  if (!foreign.ok()) {
    (void)class_lease_manager_->CancelLease(class_lease.lease);
    auto refused = RefuseAcquire(
        request,
        "SB-CEIC-027-PLUGIN-NATIVE-FOREIGN-RESERVATION-REFUSED",
        "memory.ceic_027.plugin_native.foreign_reservation_refused",
        foreign.diagnostic.diagnostic_code.empty()
            ? "ceic_016_foreign_reservation_refused"
            : foreign.diagnostic.diagnostic_code,
        foreign.status.code,
        foreign.status.severity);
    refused.evidence.insert(refused.evidence.end(),
                            foreign.evidence.begin(),
                            foreign.evidence.end());
    return refused;
  }

  PluginNativeMemorySandboxToken token;
  auto state =
      std::make_shared<PluginNativeMemorySandboxReservation::HandleState>();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    token.sandbox_reservation_id = next_reservation_id_++;
    token.class_lease = class_lease.lease;
    token.foreign_reservation = foreign.reservation->token();

    ReservationRecord record;
    record.request = request;
    record.request.estimated_bytes = estimated_bytes;
    record.request.confidence =
        conservative_estimate ? ForeignMemoryConfidence::conservative
                              : request.confidence;
    record.token = token;
    record.pressure_action = class_lease.pressure_action;
    record.conservative_estimate = conservative_estimate;
    record.foreign_reservation = std::move(foreign.reservation);
    record.state = state;
    ApplyReservationLocked(record);
    records_[token.sandbox_reservation_id] = std::move(record);
    if (conservative_estimate) {
      ++conservative_estimate_count_;
    }
  }

  PluginNativeMemorySandboxAcquireResult result;
  result.status = OkStatus();
  result.reservation = std::make_unique<PluginNativeMemorySandboxReservation>(
      PluginNativeMemorySandboxReservation::ManagerOnlyTag{},
      this,
      token,
      state);
  AttachBaseEvidence(&result.evidence, request, conservative_estimate);
  result.evidence.push_back("plugin_native_memory_sandbox.reservation_created=true");
  result.evidence.push_back("plugin_native_memory_sandbox.ceic_011_reserved=true");
  result.evidence.push_back("plugin_native_memory_sandbox.ceic_016_handle=true");
  result.evidence.push_back("plugin_native_memory_sandbox.ceic_026_plugin_udr_lease=true");
  result.evidence.push_back(
      "plugin_native_memory_sandbox.class_pressure_action=" +
      std::string(MemoryClassPressureActionName(class_lease.pressure_action)));
  result.evidence.push_back("plugin_native_memory_sandbox.sandbox_reservation_id=" +
                            std::to_string(token.sandbox_reservation_id));
  result.evidence.insert(result.evidence.end(),
                         class_lease.evidence.begin(),
                         class_lease.evidence.end());
  result.evidence.insert(result.evidence.end(),
                         foreign.evidence.begin(),
                         foreign.evidence.end());
  AttachMetricAndSupportRows(&result.metrics,
                             &result.support_bundle_rows,
                             request,
                             "granted",
                             class_lease.pressure_action);
  result.diagnostic = MakeSandboxDiagnostic(
      result.status,
      "SB-CEIC-027-PLUGIN-NATIVE-SANDBOX-GRANTED",
      "memory.ceic_027.plugin_native.sandbox_granted",
      {{"source", ForeignMemorySourceName(request.source)},
       {"sandbox_id", request.sandbox_id},
       {"estimated_bytes", std::to_string(estimated_bytes)},
       {"pressure_action",
        MemoryClassPressureActionName(class_lease.pressure_action)}});
  return result;
}

PluginNativeMemorySandboxReleaseResult
PluginNativeMemorySandboxManager::Release(
    PluginNativeMemorySandboxToken token,
    ForeignMemoryReleaseEvent event,
    std::vector<std::string> release_evidence) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = records_.find(token.sandbox_reservation_id);
  if (it == records_.end() ||
      it->second.token.class_lease.lease_id != token.class_lease.lease_id ||
      it->second.token.foreign_reservation.reservation_id !=
          token.foreign_reservation.reservation_id) {
    return RefuseRelease(token,
                         "SB-CEIC-027-PLUGIN-NATIVE-RELEASE-UNKNOWN",
                         "memory.ceic_027.plugin_native.release_unknown",
                         it == records_.end() ? "reservation_not_found"
                                              : "reservation_token_mismatch",
                         StatusCode::memory_unknown_pointer);
  }
  ReservationRecord& record = it->second;
  if (record.request.require_release_evidence && release_evidence.empty()) {
    ++missing_release_evidence_refusal_count_;
    return RefuseRelease(token,
                         "SB-CEIC-027-PLUGIN-NATIVE-RELEASE-EVIDENCE-REFUSED",
                         "memory.ceic_027.plugin_native.release_evidence_refused",
                         "non_empty_release_evidence_required");
  }

  auto snapshot = SnapshotForRecordLocked(record);
  const PluginNativeMemorySandboxRequest request_copy = record.request;
  const MemoryClassPressureAction pressure_action = record.pressure_action;
  const bool conservative_estimate = record.conservative_estimate;
  auto foreign_release = record.foreign_reservation->Release(event);
  if (!foreign_release.ok()) {
    return RefuseRelease(
        token,
        "SB-CEIC-027-PLUGIN-NATIVE-FOREIGN-RELEASE-REFUSED",
        "memory.ceic_027.plugin_native.foreign_release_refused",
        foreign_release.diagnostic.diagnostic_code.empty()
            ? "ceic_016_foreign_release_refused"
            : foreign_release.diagnostic.diagnostic_code,
        foreign_release.status.code);
  }
  auto lease_release = class_lease_manager_->CancelLease(record.token.class_lease);
  if (!lease_release.ok()) {
    return RefuseRelease(
        token,
        "SB-CEIC-027-PLUGIN-NATIVE-LEASE-RELEASE-REFUSED",
        "memory.ceic_027.plugin_native.lease_release_refused",
        lease_release.diagnostic.diagnostic_code.empty()
            ? "ceic_026_class_lease_release_refused"
            : lease_release.diagnostic.diagnostic_code,
        lease_release.status.code);
  }

  if (record.state != nullptr) {
    record.state->released.store(true);
  }
  const bool owner_cleanup =
      event == ForeignMemoryReleaseEvent::owner_cleanup;
  const bool plugin_unload =
      event == ForeignMemoryReleaseEvent::adapter_shutdown;
  ApplyReleaseLocked(record, event);
  records_.erase(it);

  PluginNativeMemorySandboxReleaseResult result;
  result.status = OkStatus();
  result.released = true;
  result.snapshot = std::move(snapshot);
  AttachBaseEvidence(&result.evidence, request_copy, conservative_estimate);
  result.evidence.push_back("plugin_native_memory_sandbox.release_event=" +
                            std::string(ForeignMemoryReleaseEventName(event)));
  result.evidence.push_back("plugin_native_memory_sandbox.release_evidence_present=true");
  for (const auto& entry : release_evidence) {
    result.evidence.push_back("plugin_native_memory_sandbox.release_evidence=" +
                              entry);
  }
  result.evidence.insert(result.evidence.end(),
                         foreign_release.evidence.begin(),
                         foreign_release.evidence.end());
  result.evidence.insert(result.evidence.end(),
                         lease_release.evidence.begin(),
                         lease_release.evidence.end());
  AttachMetricAndSupportRows(&result.metrics,
                             &result.support_bundle_rows,
                             request_copy,
                             owner_cleanup
                                 ? "owner_cleanup"
                                 : (plugin_unload ? "plugin_unload_cleanup"
                                                  : "released"),
                             pressure_action);
  result.diagnostic = MakeSandboxDiagnostic(
      result.status,
      "SB-CEIC-027-PLUGIN-NATIVE-SANDBOX-RELEASED",
      "memory.ceic_027.plugin_native.sandbox_released",
      {{"source", ForeignMemorySourceName(request_copy.source)},
       {"sandbox_id", request_copy.sandbox_id},
       {"release_event", ForeignMemoryReleaseEventName(event)}});
  return result;
}

PluginNativeMemorySandboxCleanupResult
PluginNativeMemorySandboxManager::CleanupOwner(std::string owner_id) {
  PluginNativeMemorySandboxCleanupResult cleanup;
  cleanup.status = OkStatus();
  cleanup.evidence.push_back(kEvidenceAnchor);
  cleanup.evidence.push_back(kAuthorityScope);
  cleanup.evidence.push_back("plugin_native_memory_sandbox.owner_cleanup.owner_id=" +
                             owner_id);
  if (Blank(owner_id)) {
    cleanup.status = ErrorStatus();
    cleanup.fail_closed = true;
    cleanup.diagnostic = MakeSandboxDiagnostic(
        cleanup.status,
        "SB-CEIC-027-PLUGIN-NATIVE-OWNER-CLEANUP-INVALID",
        "memory.ceic_027.plugin_native.owner_cleanup_invalid",
        {{"reason", "owner_id_required"}});
    return cleanup;
  }

  std::vector<PluginNativeMemorySandboxToken> tokens;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : records_) {
      if (entry.second.request.owner_id == owner_id) {
        tokens.push_back(entry.second.token);
      }
    }
  }
  std::sort(tokens.begin(),
            tokens.end(),
            [](const PluginNativeMemorySandboxToken& lhs,
               const PluginNativeMemorySandboxToken& rhs) {
              return lhs.sandbox_reservation_id < rhs.sandbox_reservation_id;
            });

  for (const auto& token : tokens) {
    auto released =
        Release(token,
                ForeignMemoryReleaseEvent::owner_cleanup,
                {"plugin_native_memory_sandbox.release.owner_cleanup=true",
                 "plugin_native_memory_sandbox.release.owner_id=" + owner_id});
    if (!released.ok()) {
      cleanup.status = released.status;
      cleanup.fail_closed = true;
      cleanup.diagnostic = released.diagnostic;
      break;
    }
    ++cleanup.cleaned_reservation_count;
    cleanup.cleaned_estimated_bytes += released.snapshot.estimated_bytes;
  }
  cleanup.evidence.push_back("plugin_native_memory_sandbox.owner_cleanup.cleaned_count=" +
                             std::to_string(cleanup.cleaned_reservation_count));
  cleanup.evidence.push_back(
      "plugin_native_memory_sandbox.owner_cleanup.cleaned_estimated_bytes=" +
      std::to_string(cleanup.cleaned_estimated_bytes));
  cleanup.metrics.push_back(
      {"sb_plugin_native_memory_sandbox_owner_cleanup_total", "owner",
       owner_id, cleanup.cleaned_reservation_count, "count"});
  cleanup.support_bundle_rows.push_back(
      {"plugin_native_memory_sandbox.owner_cleanup.cleaned_count",
       std::to_string(cleanup.cleaned_reservation_count), "public", false});
  cleanup.support_bundle_rows.push_back(
      {"plugin_native_memory_sandbox.authority_scope", kAuthorityScope,
       "public", false});
  if (cleanup.ok()) {
    cleanup.diagnostic = MakeSandboxDiagnostic(
        cleanup.status,
        "SB-CEIC-027-PLUGIN-NATIVE-OWNER-CLEANUP",
        "memory.ceic_027.plugin_native.owner_cleanup",
        {{"owner_id", owner_id},
         {"cleaned_count", std::to_string(cleanup.cleaned_reservation_count)}});
  }
  return cleanup;
}

PluginNativeMemorySandboxCleanupResult
PluginNativeMemorySandboxManager::CleanupPluginUnload(std::string plugin_id) {
  PluginNativeMemorySandboxCleanupResult cleanup;
  cleanup.status = OkStatus();
  cleanup.evidence.push_back(kEvidenceAnchor);
  cleanup.evidence.push_back(kAuthorityScope);
  cleanup.evidence.push_back(
      "plugin_native_memory_sandbox.plugin_unload.plugin_id=" + plugin_id);
  if (Blank(plugin_id)) {
    cleanup.status = ErrorStatus();
    cleanup.fail_closed = true;
    cleanup.diagnostic = MakeSandboxDiagnostic(
        cleanup.status,
        "SB-CEIC-027-PLUGIN-NATIVE-PLUGIN-UNLOAD-INVALID",
        "memory.ceic_027.plugin_native.plugin_unload_invalid",
        {{"reason", "plugin_id_required"}});
    return cleanup;
  }

  std::vector<PluginNativeMemorySandboxToken> tokens;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : records_) {
      if (entry.second.request.plugin_id == plugin_id) {
        tokens.push_back(entry.second.token);
      }
    }
  }
  std::sort(tokens.begin(),
            tokens.end(),
            [](const PluginNativeMemorySandboxToken& lhs,
               const PluginNativeMemorySandboxToken& rhs) {
              return lhs.sandbox_reservation_id < rhs.sandbox_reservation_id;
            });

  for (const auto& token : tokens) {
    auto released =
        Release(token,
                ForeignMemoryReleaseEvent::adapter_shutdown,
                {"plugin_native_memory_sandbox.release.plugin_unload=true",
                 "plugin_native_memory_sandbox.release.plugin_id=" +
                     plugin_id});
    if (!released.ok()) {
      cleanup.status = released.status;
      cleanup.fail_closed = true;
      cleanup.diagnostic = released.diagnostic;
      break;
    }
    ++cleanup.cleaned_reservation_count;
    cleanup.cleaned_estimated_bytes += released.snapshot.estimated_bytes;
  }
  cleanup.evidence.push_back(
      "plugin_native_memory_sandbox.plugin_unload.cleaned_count=" +
      std::to_string(cleanup.cleaned_reservation_count));
  cleanup.evidence.push_back(
      "plugin_native_memory_sandbox.plugin_unload.cleaned_estimated_bytes=" +
      std::to_string(cleanup.cleaned_estimated_bytes));
  cleanup.metrics.push_back(
      {"sb_plugin_native_memory_sandbox_unload_cleanup_total", "plugin",
       plugin_id, cleanup.cleaned_reservation_count, "count"});
  cleanup.support_bundle_rows.push_back(
      {"plugin_native_memory_sandbox.plugin_unload.cleaned_count",
       std::to_string(cleanup.cleaned_reservation_count), "public", false});
  cleanup.support_bundle_rows.push_back(
      {"plugin_native_memory_sandbox.authority_scope", kAuthorityScope,
       "public", false});
  if (cleanup.ok()) {
    cleanup.diagnostic = MakeSandboxDiagnostic(
        cleanup.status,
        "SB-CEIC-027-PLUGIN-NATIVE-PLUGIN-UNLOAD-CLEANUP",
        "memory.ceic_027.plugin_native.plugin_unload_cleanup",
        {{"plugin_id", plugin_id},
         {"cleaned_count", std::to_string(cleanup.cleaned_reservation_count)}});
  }
  return cleanup;
}

PluginNativeMemorySandboxSnapshot
PluginNativeMemorySandboxManager::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  PluginNativeMemorySandboxSnapshot snapshot;
  snapshot.active_reservation_count = static_cast<u64>(records_.size());
  snapshot.current_estimated_bytes = current_estimated_bytes_;
  snapshot.peak_estimated_bytes = peak_estimated_bytes_;
  snapshot.reservation_count = reservation_count_;
  snapshot.release_count = release_count_;
  snapshot.owner_cleanup_count = owner_cleanup_count_;
  snapshot.plugin_unload_cleanup_count = plugin_unload_cleanup_count_;
  snapshot.fail_closed_refusal_count = fail_closed_refusal_count_;
  snapshot.unsafe_provenance_refusal_count = unsafe_provenance_refusal_count_;
  snapshot.raw_external_allocation_refusal_count =
      raw_external_allocation_refusal_count_;
  snapshot.untracked_native_allocation_refusal_count =
      untracked_native_allocation_refusal_count_;
  snapshot.missing_identity_refusal_count = missing_identity_refusal_count_;
  snapshot.missing_class_lease_refusal_count =
      missing_class_lease_refusal_count_;
  snapshot.stale_class_lease_refusal_count = stale_class_lease_refusal_count_;
  snapshot.missing_release_evidence_refusal_count =
      missing_release_evidence_refusal_count_;
  snapshot.cluster_refusal_count = cluster_refusal_count_;
  snapshot.absent_provider_fail_closed_count = absent_provider_fail_closed_count_;
  snapshot.conservative_estimate_count = conservative_estimate_count_;
  snapshot.raw_plugin_allocation_allowed_count =
      raw_plugin_allocation_allowed_count_;

  for (const auto& entry : source_accounting_) {
    PluginNativeMemorySandboxSourceSnapshot source;
    source.source = entry.first;
    source.active_reservation_count = entry.second.active_reservation_count;
    source.current_estimated_bytes = entry.second.current_estimated_bytes;
    source.peak_estimated_bytes = entry.second.peak_estimated_bytes;
    source.reservation_count = entry.second.reservation_count;
    source.release_count = entry.second.release_count;
    source.owner_cleanup_count = entry.second.owner_cleanup_count;
    source.refusal_count = entry.second.refusal_count;
    snapshot.sources.push_back(std::move(source));
  }
  for (const auto& entry : records_) {
    snapshot.active_reservations.push_back(SnapshotForRecordLocked(entry.second));
  }

  snapshot.metrics.push_back(
      {"sb_plugin_native_memory_sandbox_active_bytes", "global", "ceic-027",
       snapshot.current_estimated_bytes, "bytes"});
  snapshot.metrics.push_back(
      {"sb_plugin_native_memory_sandbox_active_total", "global", "ceic-027",
       snapshot.active_reservation_count, "count"});
  snapshot.metrics.push_back(
      {"sb_plugin_native_memory_sandbox_refusal_total", "global", "ceic-027",
       snapshot.fail_closed_refusal_count, "count"});
  snapshot.metrics.push_back(
      {"sb_plugin_native_memory_sandbox_conservative_estimate_total", "global",
       "ceic-027", snapshot.conservative_estimate_count, "count"});
  snapshot.support_bundle_rows.push_back(
      {"plugin_native_memory_sandbox.authority_scope", kAuthorityScope,
       "public", false});
  snapshot.support_bundle_rows.push_back(
      {"plugin_native_memory_sandbox.integrated_support_bundle_closure",
       "not_claimed_ceic_091_pending", "public", false});
  snapshot.support_bundle_rows.push_back(
      {"plugin_native_memory_sandbox.production_raw_external_allocation_gate",
       "raw_external_allocation_refused", "public", false});
  snapshot.support_bundle_rows.push_back(
      {"plugin_native_memory_sandbox.plugin_support_bundle_view",
       "allocator_abi_memory_context_invocation_budget_result_buffer_pressure_unload",
       "public", false});
  snapshot.support_bundle_rows.push_back(
      {"plugin_native_memory_sandbox.cluster_boundary",
       "cluster_reserved_external_cluster_routes_refused", "public", false});
  return snapshot;
}

PluginNativeMemorySandboxAcquireResult
PluginNativeMemorySandboxManager::RefuseAcquire(
    const PluginNativeMemorySandboxRequest& request,
    std::string diagnostic_code,
    std::string message_key,
    std::string reason,
    StatusCode code,
    Severity severity) {
  PluginNativeMemorySandboxAcquireResult result;
  result.status = ErrorStatus(code, severity);
  result.fail_closed = true;
  result.diagnostic = MakeSandboxDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      {{"reason", reason},
       {"source", ForeignMemorySourceName(request.source)},
       {"owner_id", request.owner_id},
       {"sandbox_id", request.sandbox_id},
       {"operation_id", request.operation_id}});
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++fail_closed_refusal_count_;
    ++source_accounting_[request.source].refusal_count;
  }
  bool conservative = false;
  (void)EffectiveEstimatedBytes(request, &conservative);
  AttachBaseEvidence(&result.evidence, request, conservative);
  result.evidence.push_back("plugin_native_memory_sandbox.fail_closed=true");
  result.evidence.push_back("plugin_native_memory_sandbox.refused=" +
                            result.diagnostic.diagnostic_code);
  AttachMetricAndSupportRows(&result.metrics,
                             &result.support_bundle_rows,
                             request,
                             reason,
                             MemoryClassPressureAction::deny);
  return result;
}

PluginNativeMemorySandboxReleaseResult
PluginNativeMemorySandboxManager::RefuseRelease(
    PluginNativeMemorySandboxToken token,
    std::string diagnostic_code,
    std::string message_key,
    std::string reason,
    StatusCode code) {
  PluginNativeMemorySandboxReleaseResult result;
  result.status = ErrorStatus(code);
  result.fail_closed = true;
  result.diagnostic = MakeSandboxDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      {{"reason", reason},
       {"sandbox_reservation_id", std::to_string(token.sandbox_reservation_id)},
       {"class_lease_id", std::to_string(token.class_lease.lease_id)},
       {"foreign_reservation_id",
        std::to_string(token.foreign_reservation.reservation_id)}});
  result.evidence.push_back(kEvidenceAnchor);
  result.evidence.push_back(kAuthorityScope);
  result.evidence.push_back("plugin_native_memory_sandbox.release.fail_closed=true");
  result.evidence.push_back("plugin_native_memory_sandbox.release.refused=" +
                            result.diagnostic.diagnostic_code);
  result.support_bundle_rows.push_back(
      {"plugin_native_memory_sandbox.release.refused",
       result.diagnostic.diagnostic_code, "public", false});
  result.metrics.push_back(
      {"sb_plugin_native_memory_sandbox_release_refusal_total", "global",
       "ceic-027", 1, "count"});
  return result;
}

PluginNativeMemorySandboxActiveSnapshot
PluginNativeMemorySandboxManager::SnapshotForRecordLocked(
    const ReservationRecord& record) const {
  PluginNativeMemorySandboxActiveSnapshot snapshot;
  snapshot.token = record.token;
  snapshot.source = record.request.source;
  snapshot.owner_id = record.request.owner_id;
  snapshot.sandbox_id = record.request.sandbox_id;
  snapshot.plugin_id = record.request.plugin_id;
  snapshot.plugin_allocator_abi = record.request.plugin_allocator_abi;
  snapshot.plugin_memory_context_id = record.request.plugin_memory_context_id;
  snapshot.udr_entrypoint = record.request.udr_entrypoint;
  snapshot.operation_id = record.request.operation_id;
  snapshot.native_callsite = record.request.native_callsite;
  snapshot.estimated_bytes = record.request.estimated_bytes;
  snapshot.observed_bytes = record.request.observed_bytes;
  snapshot.invocation_budget_bytes = record.request.invocation_budget_bytes;
  snapshot.result_buffer_bytes = record.request.result_buffer_bytes;
  snapshot.confidence = record.request.confidence;
  snapshot.pressure_action = record.pressure_action;
  snapshot.provider_available = record.request.provider_available;
  snapshot.live_provider_proof = record.request.live_provider_proof;
  snapshot.conservative_estimate = record.conservative_estimate;
  snapshot.result_buffer_owned_by_engine =
      record.request.result_buffer_owned_by_engine;
  snapshot.plugin_cancellation_on_pressure =
      record.request.plugin_cancellation_on_pressure;
  snapshot.support_bundle_view_enabled = record.request.support_bundle_view_enabled;
  AttachBaseEvidence(&snapshot.evidence,
                     record.request,
                     record.conservative_estimate);
  snapshot.evidence.push_back("plugin_native_memory_sandbox.handle_active=true");
  return snapshot;
}

void PluginNativeMemorySandboxManager::ApplyReservationLocked(
    const ReservationRecord& record) {
  const u64 estimated = record.request.estimated_bytes;
  current_estimated_bytes_ += estimated;
  UpdatePeak(current_estimated_bytes_, &peak_estimated_bytes_);
  ++reservation_count_;

  auto& source = source_accounting_[record.request.source];
  ++source.active_reservation_count;
  source.current_estimated_bytes += estimated;
  UpdatePeak(source.current_estimated_bytes, &source.peak_estimated_bytes);
  ++source.reservation_count;
}

void PluginNativeMemorySandboxManager::ApplyReleaseLocked(
    const ReservationRecord& record,
    ForeignMemoryReleaseEvent event) {
  const u64 estimated = record.request.estimated_bytes;
  const bool owner_cleanup =
      event == ForeignMemoryReleaseEvent::owner_cleanup;
  const bool plugin_unload =
      event == ForeignMemoryReleaseEvent::adapter_shutdown;
  current_estimated_bytes_ =
      current_estimated_bytes_ >= estimated ? current_estimated_bytes_ - estimated
                                            : 0;
  ++release_count_;
  if (owner_cleanup) {
    ++owner_cleanup_count_;
  }
  if (plugin_unload) {
    ++plugin_unload_cleanup_count_;
  }

  auto& source = source_accounting_[record.request.source];
  if (source.active_reservation_count != 0) {
    --source.active_reservation_count;
  }
  source.current_estimated_bytes =
      source.current_estimated_bytes >= estimated
          ? source.current_estimated_bytes - estimated
          : 0;
  ++source.release_count;
  if (owner_cleanup) {
    ++source.owner_cleanup_count;
  }
}

void PluginNativeMemorySandboxManager::AttachBaseEvidence(
    std::vector<std::string>* evidence,
    const PluginNativeMemorySandboxRequest& request,
    bool conservative_estimate) const {
  evidence->push_back(kEvidenceAnchor);
  evidence->push_back(kAuthorityScope);
  evidence->push_back("plugin_native_memory_sandbox.source=" +
                      std::string(ForeignMemorySourceName(request.source)));
  evidence->push_back("plugin_native_memory_sandbox.owner_id=" +
                      request.owner_id);
  evidence->push_back("plugin_native_memory_sandbox.sandbox_id=" +
                      request.sandbox_id);
  evidence->push_back("plugin_native_memory_sandbox.plugin_id=" +
                      request.plugin_id);
  evidence->push_back("plugin_native_memory_sandbox.plugin_allocator_abi=" +
                      request.plugin_allocator_abi);
  evidence->push_back("plugin_native_memory_sandbox.plugin_memory_context_id=" +
                      request.plugin_memory_context_id);
  evidence->push_back("plugin_native_memory_sandbox.udr_entrypoint=" +
                      request.udr_entrypoint);
  evidence->push_back("plugin_native_memory_sandbox.operation_id=" +
                      request.operation_id);
  evidence->push_back("plugin_native_memory_sandbox.native_callsite=" +
                      request.native_callsite);
  evidence->push_back("plugin_native_memory_sandbox.provider_available=" +
                      BoolText(request.provider_available));
  evidence->push_back("plugin_native_memory_sandbox.live_provider_proof=" +
                      BoolText(request.live_provider_proof));
  evidence->push_back("plugin_native_memory_sandbox.live_route_claim=" +
                      BoolText(request.live_route_claim));
  evidence->push_back("plugin_native_memory_sandbox.invocation_budget_bytes=" +
                      std::to_string(request.invocation_budget_bytes));
  evidence->push_back("plugin_native_memory_sandbox.result_buffer_bytes=" +
                      std::to_string(request.result_buffer_bytes));
  evidence->push_back("plugin_native_memory_sandbox.result_buffer_owner=engine");
  evidence->push_back(
      "plugin_native_memory_sandbox.result_buffer_owned_by_engine=" +
      BoolText(request.result_buffer_owned_by_engine));
  evidence->push_back("plugin_native_memory_sandbox.conservative_estimate=" +
                      BoolText(conservative_estimate));
  evidence->push_back("plugin_native_memory_sandbox.raw_external_allocation=" +
                      BoolText(request.raw_external_allocation));
  evidence->push_back(
      "plugin_native_memory_sandbox.raw_plugin_allocation_explicitly_allowed=" +
      BoolText(request.raw_plugin_allocation_explicitly_allowed));
  evidence->push_back(
      "plugin_native_memory_sandbox.production_raw_external_allocation_gate=" +
      BoolText(request.production_raw_external_allocation_gate));
  evidence->push_back("plugin_native_memory_sandbox.untracked_native_allocation=" +
                      BoolText(request.untracked_native_allocation));
  evidence->push_back("plugin_native_memory_sandbox.plugin_udr_sandboxed=" +
                      BoolText(request.plugin_udr_sandboxed));
  evidence->push_back(
      "plugin_native_memory_sandbox.plugin_cancellation_on_pressure=" +
      BoolText(request.plugin_cancellation_on_pressure));
  evidence->push_back(
      "plugin_native_memory_sandbox.plugin_unload_cleanup_supported=" +
      BoolText(request.plugin_unload_cleanup_supported));
  evidence->push_back(
      "plugin_native_memory_sandbox.support_bundle_view_enabled=" +
      BoolText(request.support_bundle_view_enabled));
  evidence->push_back("plugin_native_memory_sandbox.pressure_state=" +
                      std::string(MemoryPressureStateName(
                          request.pressure_state)));
  evidence->push_back("plugin_native_memory_sandbox.ceic_011_reservation_required=true");
  evidence->push_back("plugin_native_memory_sandbox.ceic_016_foreign_handle_required=true");
  evidence->push_back("plugin_native_memory_sandbox.ceic_026_plugin_udr_class_lease_required=true");
  evidence->push_back("plugin_native_memory_sandbox.no_authority.transaction_finality=true");
  evidence->push_back("plugin_native_memory_sandbox.no_authority.visibility=true");
  evidence->push_back("plugin_native_memory_sandbox.no_authority.authorization_security=true");
  evidence->push_back("plugin_native_memory_sandbox.no_authority.recovery=true");
  evidence->push_back("plugin_native_memory_sandbox.no_authority.parser_donor_wal=true");
  evidence->push_back("plugin_native_memory_sandbox.no_authority.benchmark_optimizer_index_agent=true");
  evidence->push_back("plugin_native_memory_sandbox.cluster_boundary=external_provider_only_fail_closed");
  for (const auto& entry : request.evidence) {
    evidence->push_back("plugin_native_memory_sandbox.evidence=" + entry);
  }
}

void PluginNativeMemorySandboxManager::AttachMetricAndSupportRows(
    std::vector<PluginNativeMemorySandboxMetricRow>* metrics,
    std::vector<PluginNativeMemorySandboxSupportBundleRow>* support_rows,
    const PluginNativeMemorySandboxRequest& request,
    std::string reason,
    MemoryClassPressureAction action) const {
  metrics->push_back(
      {"sb_plugin_native_memory_sandbox_decision_total",
       ForeignMemorySourceName(request.source), request.sandbox_id, 1,
       "count"});
  metrics->push_back(
      {"sb_plugin_native_memory_sandbox_pressure_action_total",
       MemoryClassPressureActionName(action), request.sandbox_id, 1, "count"});
  support_rows->push_back(
      {"plugin_native_memory_sandbox.reason", reason, "public", false});
  support_rows->push_back(
      {"plugin_native_memory_sandbox.source",
       ForeignMemorySourceName(request.source), "public", false});
  support_rows->push_back(
      {"plugin_native_memory_sandbox.sandbox_id", request.sandbox_id, "public",
       false});
  support_rows->push_back(
      {"plugin_native_memory_sandbox.pressure_action",
       MemoryClassPressureActionName(action), "public", false});
  support_rows->push_back(
      {"plugin_native_memory_sandbox.authority_scope", kAuthorityScope,
       "public", false});
  support_rows->push_back(
      {"plugin_native_memory_sandbox.integrated_support_bundle_closure",
       "not_claimed_ceic_091_pending", "public", false});
}

PluginNativeMemorySandboxAcquireResult AcquirePluginNativeMemorySandbox(
    PluginNativeMemorySandboxManager* manager,
    PluginNativeMemorySandboxRequest request) {
  if (manager == nullptr) {
    PluginNativeMemorySandboxAcquireResult result;
    result.status = ErrorStatus();
    result.fail_closed = true;
    result.diagnostic = MakeSandboxDiagnostic(
        result.status,
        "SB-CEIC-027-PLUGIN-NATIVE-MANAGER-REQUIRED",
        "memory.ceic_027.plugin_native.manager_required",
        {{"reason", "plugin_native_memory_sandbox_manager_required"}});
    result.evidence.push_back(kEvidenceAnchor);
    result.evidence.push_back(kAuthorityScope);
    return result;
  }
  return manager->Acquire(std::move(request));
}

}  // namespace scratchbird::core::memory
