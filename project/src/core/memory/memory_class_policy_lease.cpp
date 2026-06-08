// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory_class_policy_lease.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace scratchbird::core::memory {
namespace {

using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::StatusCode;

constexpr const char* kClassLeaseAuthorityScope =
    "memory_class_policy_lease.authority_scope=evidence_only_not_transaction_finality_visibility_authorization_security_recovery_parser_donor_wal_benchmark_optimizer_plan_index_finality_cluster_or_agent_action_authority";

Status LeaseStatus(StatusCode code, Severity severity) {
  return {code, severity, Subsystem::memory};
}

Status OkStatus() {
  return LeaseStatus(StatusCode::ok, Severity::info);
}

std::string ScopeKey(const HierarchicalMemoryScopeRef& scope) {
  return std::string(HierarchicalMemoryScopeKindName(scope.kind)) + ":" +
         scope.scope_id;
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
  if (provenance.source_label.empty()) {
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

HierarchicalMemoryBudgetProvenance BuiltInPolicyProvenance() {
  HierarchicalMemoryBudgetProvenance provenance;
  provenance.source = HierarchicalMemoryBudgetProvenanceSource::runtime_policy;
  provenance.source_label = "ceic-026-built-in-memory-class-policy";
  provenance.engine_mga_authoritative = true;
  provenance.memory_evidence_only = true;
  return provenance;
}

bool ValidateScopeChain(const std::vector<HierarchicalMemoryScopeRef>& chain,
                        std::string* reason,
                        std::string* duplicate_scope_key) {
  if (chain.empty()) {
    *reason = "scope_chain_empty";
    return false;
  }
  std::set<std::string> seen;
  for (const auto& scope : chain) {
    if (scope.scope_id.empty()) {
      *reason = "scope_id_empty";
      return false;
    }
    const auto key = ScopeKey(scope);
    if (!seen.insert(key).second) {
      *reason = "duplicate_scope";
      *duplicate_scope_key = key;
      return false;
    }
  }
  return true;
}

bool IsClusterMemoryCategory(MemoryCategory category) {
  return category == MemoryCategory::cluster_control_reserved ||
         category == MemoryCategory::cluster_decision_reserved;
}

MemoryClassPressureAction SelectPressureAction(
    const MemoryClassPolicy& policy,
    MemoryPressureState state) {
  switch (state) {
    case MemoryPressureState::normal:
      return policy.normal_action;
    case MemoryPressureState::soft_pressure:
      return policy.soft_pressure_action;
    case MemoryPressureState::high_pressure:
      return policy.high_pressure_action;
    case MemoryPressureState::emergency_pressure:
      return policy.emergency_pressure_action;
    case MemoryPressureState::recovery:
      return policy.recovery_action;
  }
  return MemoryClassPressureAction::deny;
}

DiagnosticRecord MakeLeaseDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::vector<DiagnosticArgument> arguments = {}) {
  arguments.push_back({"authority_scope", kClassLeaseAuthorityScope});
  return MakeDiagnostic(
      status.code,
      status.severity,
      status.subsystem,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(arguments),
      {},
      "core.memory.memory_class_policy_lease",
      "Treat memory class lease rows as governed memory evidence only; do not use them as transaction, visibility, security, recovery, parser, donor, optimizer, index, cluster, or agent authority.");
}

bool ProtectedRequestSafe(const MemoryBudgetLeaseRequest& request) {
  return request.protected_material_routed_through_protected_buffer &&
         request.protected_material_redacted &&
         request.protected_zero_on_release &&
         !request.plaintext_material_observed;
}

bool ProtectedRecoverySafe(const MemoryBudgetLeaseRecoveryInput& input) {
  return input.protected_material_routed_through_protected_buffer &&
         input.protected_material_redacted &&
         input.protected_zero_on_release &&
         !input.plaintext_material_observed;
}

bool LeaseMatches(const MemoryBudgetLeaseToken& expected,
                  const MemoryBudgetLeaseToken& actual) {
  return expected.lease_id == actual.lease_id &&
         expected.bytes == actual.bytes &&
         expected.creation_sequence == actual.creation_sequence &&
         expected.reservation.token_id == actual.reservation.token_id &&
         expected.reservation.bytes == actual.reservation.bytes;
}

}  // namespace

const char* MemoryClassKindName(MemoryClassKind kind) {
  switch (kind) {
    case MemoryClassKind::critical_engine:
      return "critical_engine";
    case MemoryClassKind::query_scratch:
      return "query_scratch";
    case MemoryClassKind::clean_page_cache:
      return "clean_page_cache";
    case MemoryClassKind::dirty_page_cache:
      return "dirty_page_cache";
    case MemoryClassKind::protected_material:
      return "protected_material";
    case MemoryClassKind::result_driver_buffer:
      return "result_driver_buffer";
    case MemoryClassKind::background_maintenance:
      return "background_maintenance";
    case MemoryClassKind::plugin_udr:
      return "plugin_udr";
    case MemoryClassKind::parser_handoff:
      return "parser_handoff";
    case MemoryClassKind::diagnostics_support:
      return "diagnostics_support";
  }
  return "unknown";
}

const char* MemoryBudgetLeaseRouteKindName(MemoryBudgetLeaseRouteKind route_kind) {
  switch (route_kind) {
    case MemoryBudgetLeaseRouteKind::local:
      return "local";
    case MemoryBudgetLeaseRouteKind::cluster:
      return "cluster";
  }
  return "unknown";
}

const char* MemoryClassPressureActionName(MemoryClassPressureAction action) {
  switch (action) {
    case MemoryClassPressureAction::grant:
      return "grant";
    case MemoryClassPressureAction::protect_critical:
      return "protect_critical";
    case MemoryClassPressureAction::prefer_spill:
      return "prefer_spill";
    case MemoryClassPressureAction::shrink_clean_page_cache:
      return "shrink_clean_page_cache";
    case MemoryClassPressureAction::flush_dirty_page_cache:
      return "flush_dirty_page_cache";
    case MemoryClassPressureAction::refuse_protected_material:
      return "refuse_protected_material";
    case MemoryClassPressureAction::throttle:
      return "throttle";
    case MemoryClassPressureAction::cancel:
      return "cancel";
    case MemoryClassPressureAction::suspend_background:
      return "suspend_background";
    case MemoryClassPressureAction::sandbox_plugin_udr:
      return "sandbox_plugin_udr";
    case MemoryClassPressureAction::throttle_parser_handoff:
      return "throttle_parser_handoff";
    case MemoryClassPressureAction::degrade_diagnostics:
      return "degrade_diagnostics";
    case MemoryClassPressureAction::external_provider_required:
      return "external_provider_required";
    case MemoryClassPressureAction::deny:
      return "deny";
  }
  return "unknown";
}

const char* MemoryBudgetLeaseCleanupReasonName(
    MemoryBudgetLeaseCleanupReason reason) {
  switch (reason) {
    case MemoryBudgetLeaseCleanupReason::cancel:
      return "cancel";
    case MemoryBudgetLeaseCleanupReason::expired:
      return "expired";
    case MemoryBudgetLeaseCleanupReason::owner_disconnect:
      return "owner_disconnect";
  }
  return "unknown";
}

const char* MemoryBudgetLeaseRecoveryClassificationName(
    MemoryBudgetLeaseRecoveryClassification classification) {
  switch (classification) {
    case MemoryBudgetLeaseRecoveryClassification::evidence_only_rebuilt:
      return "evidence_only_rebuilt";
    case MemoryBudgetLeaseRecoveryClassification::expired_cleanup_required:
      return "expired_cleanup_required";
    case MemoryBudgetLeaseRecoveryClassification::protected_material_quarantine:
      return "protected_material_quarantine";
    case MemoryBudgetLeaseRecoveryClassification::external_cluster_provider_required:
      return "external_cluster_provider_required";
    case MemoryBudgetLeaseRecoveryClassification::unsafe_provenance_refused:
      return "unsafe_provenance_refused";
  }
  return "unknown";
}

MemoryClassPolicy DefaultMemoryClassPolicy(MemoryClassKind kind) {
  MemoryClassPolicy policy;
  policy.kind = kind;
  policy.class_name = MemoryClassKindName(kind);
  policy.provenance = BuiltInPolicyProvenance();

  switch (kind) {
    case MemoryClassKind::critical_engine:
      policy.category = MemoryCategory::core_runtime;
      policy.max_lease_ms = 30 * 1000;
      policy.max_renewals = 1;
      policy.spillable = false;
      policy.throttleable = false;
      policy.cancelable = false;
      policy.protected_from_cancellation = true;
      policy.critical_engine_class = true;
      policy.soft_pressure_action = MemoryClassPressureAction::protect_critical;
      policy.high_pressure_action = MemoryClassPressureAction::protect_critical;
      policy.emergency_pressure_action =
          MemoryClassPressureAction::protect_critical;
      policy.recovery_action = MemoryClassPressureAction::protect_critical;
      break;
    case MemoryClassKind::query_scratch:
      policy.category = MemoryCategory::executor_query_reserved;
      policy.max_lease_ms = 120 * 1000;
      policy.max_renewals = 2;
      policy.spillable = true;
      policy.cancelable = true;
      policy.soft_pressure_action = MemoryClassPressureAction::prefer_spill;
      policy.high_pressure_action = MemoryClassPressureAction::prefer_spill;
      policy.emergency_pressure_action = MemoryClassPressureAction::cancel;
      break;
    case MemoryClassKind::clean_page_cache:
      policy.category = MemoryCategory::page_buffer;
      policy.max_lease_ms = 300 * 1000;
      policy.max_renewals = 4;
      policy.spillable = false;
      policy.cancelable = false;
      policy.soft_pressure_action =
          MemoryClassPressureAction::shrink_clean_page_cache;
      policy.high_pressure_action =
          MemoryClassPressureAction::shrink_clean_page_cache;
      policy.emergency_pressure_action =
          MemoryClassPressureAction::shrink_clean_page_cache;
      break;
    case MemoryClassKind::dirty_page_cache:
      policy.category = MemoryCategory::page_buffer;
      policy.max_lease_ms = 120 * 1000;
      policy.max_renewals = 3;
      policy.spillable = false;
      policy.cancelable = false;
      policy.soft_pressure_action =
          MemoryClassPressureAction::flush_dirty_page_cache;
      policy.high_pressure_action =
          MemoryClassPressureAction::flush_dirty_page_cache;
      policy.emergency_pressure_action =
          MemoryClassPressureAction::flush_dirty_page_cache;
      break;
    case MemoryClassKind::protected_material:
      policy.category = MemoryCategory::core_runtime;
      policy.max_lease_ms = 30 * 1000;
      policy.max_renewals = 1;
      policy.spillable = false;
      policy.throttleable = false;
      policy.cancelable = false;
      policy.requires_protected_material_route = true;
      policy.requires_zero_on_release = true;
      policy.excludes_protected_material_from_support = true;
      policy.soft_pressure_action =
          MemoryClassPressureAction::refuse_protected_material;
      policy.high_pressure_action =
          MemoryClassPressureAction::refuse_protected_material;
      policy.emergency_pressure_action =
          MemoryClassPressureAction::refuse_protected_material;
      policy.recovery_action =
          MemoryClassPressureAction::refuse_protected_material;
      break;
    case MemoryClassKind::result_driver_buffer:
      policy.category = MemoryCategory::datatype_payload;
      policy.max_lease_ms = 180 * 1000;
      policy.max_renewals = 2;
      policy.spillable = true;
      policy.cancelable = true;
      policy.soft_pressure_action = MemoryClassPressureAction::throttle;
      policy.high_pressure_action = MemoryClassPressureAction::prefer_spill;
      policy.emergency_pressure_action = MemoryClassPressureAction::cancel;
      break;
    case MemoryClassKind::background_maintenance:
      policy.category = MemoryCategory::cleanup;
      policy.max_lease_ms = 240 * 1000;
      policy.max_renewals = 2;
      policy.spillable = true;
      policy.cancelable = true;
      policy.soft_pressure_action =
          MemoryClassPressureAction::suspend_background;
      policy.high_pressure_action =
          MemoryClassPressureAction::suspend_background;
      policy.emergency_pressure_action = MemoryClassPressureAction::cancel;
      break;
    case MemoryClassKind::plugin_udr:
      policy.category = MemoryCategory::udr_reserved;
      policy.max_lease_ms = 60 * 1000;
      policy.max_renewals = 1;
      policy.spillable = false;
      policy.cancelable = true;
      policy.plugin_udr_class = true;
      policy.soft_pressure_action = MemoryClassPressureAction::sandbox_plugin_udr;
      policy.high_pressure_action = MemoryClassPressureAction::sandbox_plugin_udr;
      policy.emergency_pressure_action = MemoryClassPressureAction::cancel;
      break;
    case MemoryClassKind::parser_handoff:
      policy.category = MemoryCategory::parser_handoff_reserved;
      policy.max_lease_ms = 30 * 1000;
      policy.max_renewals = 1;
      policy.parser_handoff_class = true;
      policy.spillable = false;
      policy.cancelable = false;
      policy.soft_pressure_action =
          MemoryClassPressureAction::throttle_parser_handoff;
      policy.high_pressure_action =
          MemoryClassPressureAction::throttle_parser_handoff;
      policy.emergency_pressure_action =
          MemoryClassPressureAction::throttle_parser_handoff;
      break;
    case MemoryClassKind::diagnostics_support:
      policy.category = MemoryCategory::diagnostics;
      policy.max_lease_ms = 60 * 1000;
      policy.max_renewals = 1;
      policy.diagnostics_support_class = true;
      policy.spillable = true;
      policy.cancelable = true;
      policy.soft_pressure_action =
          MemoryClassPressureAction::degrade_diagnostics;
      policy.high_pressure_action =
          MemoryClassPressureAction::degrade_diagnostics;
      policy.emergency_pressure_action =
          MemoryClassPressureAction::degrade_diagnostics;
      break;
  }
  return policy;
}

MemoryClassPolicyLeaseManager::MemoryClassPolicyLeaseManager(
    HierarchicalMemoryBudgetLedger* ledger)
    : ledger_(ledger) {
  const MemoryClassKind kinds[] = {
      MemoryClassKind::critical_engine,
      MemoryClassKind::query_scratch,
      MemoryClassKind::clean_page_cache,
      MemoryClassKind::dirty_page_cache,
      MemoryClassKind::protected_material,
      MemoryClassKind::result_driver_buffer,
      MemoryClassKind::background_maintenance,
      MemoryClassKind::plugin_udr,
      MemoryClassKind::parser_handoff,
      MemoryClassKind::diagnostics_support};
  for (MemoryClassKind kind : kinds) {
    ClassState state;
    state.policy = DefaultMemoryClassPolicy(kind);
    classes_[kind] = std::move(state);
  }
}

HierarchicalMemoryBudgetOperationResult
MemoryClassPolicyLeaseManager::SetClassPolicy(MemoryClassPolicy policy) {
  HierarchicalMemoryBudgetOperationResult result;
  std::string provenance_reason;
  if (!SafeProvenance(policy.provenance, &provenance_reason)) {
    result.status =
        LeaseStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeLeaseDiagnostic(
        result.status,
        "SB-MEMORY-CLASS-POLICY-PROVENANCE-REFUSED",
        "memory.class_policy.provenance_refused",
        {{"class", MemoryClassKindName(policy.kind)},
         {"reason", provenance_reason}});
    return result;
  }
  if (policy.class_name.empty()) {
    result.status =
        LeaseStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeLeaseDiagnostic(
        result.status,
        "SB-MEMORY-CLASS-POLICY-INVALID",
        "memory.class_policy.invalid",
        {{"class", MemoryClassKindName(policy.kind)},
         {"reason", "class_name_empty"}});
    return result;
  }
  if (policy.max_lease_ms == 0) {
    result.status =
        LeaseStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeLeaseDiagnostic(
        result.status,
        "SB-MEMORY-CLASS-POLICY-UNBOUNDED-LEASE-REFUSED",
        "memory.class_policy.unbounded_lease_refused",
        {{"class", policy.class_name},
         {"reason", "max_lease_ms_required"}});
    return result;
  }
  if (IsClusterMemoryCategory(policy.category)) {
    result.status =
        LeaseStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeLeaseDiagnostic(
        result.status,
        "SB-MEMORY-CLASS-POLICY-CLUSTER-REFUSED",
        "memory.class_policy.cluster_refused",
        {{"class", policy.class_name},
         {"reason", "cluster_memory_external_provider_only"}});
    return result;
  }
  if (!policy.renewable) {
    policy.max_renewals = 0;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto& state = classes_[policy.kind];
  state.policy = std::move(policy);
  result.status = OkStatus();
  return result;
}

MemoryBudgetLeaseDecision MemoryClassPolicyLeaseManager::AcquireLease(
    MemoryBudgetLeaseRequest request) {
  MemoryClassPolicy policy;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    policy = PolicyForKindLocked(request.class_kind);
  }
  const MemoryClassPressureAction action =
      SelectPressureAction(policy, request.pressure_state);

  std::string invalid_reason;
  std::string duplicate_scope_key;
  if (request.requested_bytes == 0 ||
      request.owner_id.empty() ||
      request.deadline_ms == 0 ||
      request.deadline_ms <= request.now_ms ||
      !ValidateScopeChain(request.scope_chain, &invalid_reason,
                          &duplicate_scope_key)) {
    std::string reason = invalid_reason;
    if (request.requested_bytes == 0) {
      reason = "requested_bytes_zero";
    } else if (request.owner_id.empty()) {
      reason = "owner_id_empty";
    } else if (request.deadline_ms == 0) {
      reason = "deadline_missing";
    } else if (request.deadline_ms <= request.now_ms) {
      reason = "deadline_not_future";
    }
    return FailDecision(request,
                        policy,
                        MemoryClassPressureAction::deny,
                        StatusCode::memory_invalid_request,
                        Severity::error,
                        "SB-MEMORY-CLASS-LEASE-REQUEST-INVALID",
                        "memory.class_lease.request_invalid",
                        reason + ":" + duplicate_scope_key,
                        false);
  }
  if (ledger_ == nullptr) {
    return FailDecision(request,
                        policy,
                        MemoryClassPressureAction::deny,
                        StatusCode::memory_invalid_request,
                        Severity::error,
                        "SB-MEMORY-CLASS-LEASE-LEDGER-MISSING",
                        "memory.class_lease.ledger_missing",
                        "ceic_011_ledger_required",
                        false);
  }
  std::string provenance_reason;
  if (!SafeProvenance(request.provenance, &provenance_reason)) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++unsafe_provenance_refusal_count_;
    }
    return FailDecision(request,
                        policy,
                        MemoryClassPressureAction::deny,
                        StatusCode::memory_invalid_request,
                        Severity::error,
                        "SB-MEMORY-CLASS-LEASE-PROVENANCE-REFUSED",
                        "memory.class_lease.provenance_refused",
                        provenance_reason,
                        false);
  }
  if (request.route_kind == MemoryBudgetLeaseRouteKind::cluster ||
      IsClusterMemoryCategory(policy.category)) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++cluster_refusal_count_;
    }
    return FailDecision(
        request,
        policy,
        MemoryClassPressureAction::external_provider_required,
        StatusCode::memory_invalid_request,
        Severity::error,
        "SB-MEMORY-CLASS-LEASE-CLUSTER-REFUSED",
        "memory.class_lease.cluster_refused",
        "cluster_memory_external_provider_only",
        true);
  }
  if (policy.max_lease_ms != 0 &&
      request.deadline_ms - request.now_ms > policy.max_lease_ms) {
    return FailDecision(request,
                        policy,
                        action,
                        StatusCode::memory_limit_exceeded,
                        Severity::error,
                        "SB-MEMORY-CLASS-LEASE-DEADLINE-REFUSED",
                        "memory.class_lease.deadline_refused",
                        "deadline_exceeds_policy_max",
                        false);
  }
  if (policy.requires_protected_material_route &&
      !ProtectedRequestSafe(request)) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++protected_material_refusal_count_;
    }
    return FailDecision(
        request,
        policy,
        MemoryClassPressureAction::refuse_protected_material,
        StatusCode::memory_invalid_request,
        Severity::error,
        "SB-MEMORY-CLASS-LEASE-PROTECTED-MATERIAL-REFUSED",
        "memory.class_lease.protected_material_refused",
        "protected_buffer_redaction_zeroization_required",
        false);
  }
  if (request.weight == 0) {
    request.weight = 1;
  }

  HierarchicalMemoryReservationRequest reservation_request;
  reservation_request.scope_chain = request.scope_chain;
  reservation_request.category = policy.category;
  reservation_request.memory_class = policy.class_name;
  reservation_request.requested_bytes = request.requested_bytes;
  reservation_request.owner_id = request.owner_id;
  reservation_request.spillable = policy.spillable;
  reservation_request.cancelable = policy.cancelable;
  reservation_request.priority = request.priority;
  reservation_request.weight = request.weight;
  reservation_request.lease_expires_at_ms = 0;
  reservation_request.provenance = request.provenance;
  auto reservation = ledger_->Reserve(std::move(reservation_request));
  if (!reservation.ok()) {
    return FailDecision(request,
                        policy,
                        action,
                        reservation.status.code,
                        reservation.status.severity,
                        reservation.diagnostic.diagnostic_code,
                        reservation.diagnostic.message_key,
                        "ceic_011_reservation_refused",
                        false);
  }

  auto commit = ledger_->Commit(reservation.token);
  if (!commit.ok()) {
    (void)ledger_->Release(reservation.token);
    return FailDecision(request,
                        policy,
                        MemoryClassPressureAction::deny,
                        commit.status.code,
                        commit.status.severity,
                        commit.diagnostic.diagnostic_code,
                        commit.diagnostic.message_key,
                        "ceic_011_commit_refused",
                        false);
  }

  MemoryBudgetLeaseToken lease;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    lease.lease_id = next_lease_id_++;
    lease.bytes = request.requested_bytes;
    lease.creation_sequence = next_creation_sequence_++;
    lease.reservation = reservation.token;

    LeaseRecord record;
    record.lease = lease;
    record.scope_chain = request.scope_chain;
    record.class_kind = policy.kind;
    record.class_name = policy.class_name;
    record.category = policy.category;
    record.owner_id = request.owner_id;
    record.created_at_ms = request.now_ms;
    record.deadline_ms = request.deadline_ms;
    record.renewal_count = 0;
    record.max_renewals = policy.max_renewals;
    record.pressure_action = action;
    record.protected_material = policy.requires_protected_material_route;
    leases_[lease.lease_id] = std::move(record);

    auto& state = classes_[policy.kind];
    state.active_bytes += request.requested_bytes;
    ++state.active_lease_count;
    ++state.created_lease_count;
    active_bytes_ += request.requested_bytes;
    ++created_lease_count_;
  }

  return GrantDecision(request, policy, lease, action);
}

MemoryBudgetLeaseRenewalResult MemoryClassPolicyLeaseManager::RenewLease(
    MemoryBudgetLeaseRenewalRequest request) {
  MemoryBudgetLeaseRenewalResult result;
  std::string provenance_reason;
  if (!SafeProvenance(request.provenance, &provenance_reason)) {
    result.status =
        LeaseStatus(StatusCode::memory_invalid_request, Severity::error);
    result.fail_closed = true;
    result.diagnostic = MakeLeaseDiagnostic(
        result.status,
        "SB-MEMORY-CLASS-LEASE-RENEW-PROVENANCE-REFUSED",
        "memory.class_lease.renew_provenance_refused",
        {{"reason", provenance_reason},
         {"lease_id", std::to_string(request.lease.lease_id)}});
    result.evidence.push_back("CEIC-026_MEMORY_CLASS_POLICY_LEASES");
    result.evidence.push_back(kClassLeaseAuthorityScope);
    return result;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = leases_.find(request.lease.lease_id);
  if (it == leases_.end() || !LeaseMatches(it->second.lease, request.lease)) {
    ++renewal_refusal_count_;
    result.status =
        LeaseStatus(StatusCode::memory_unknown_pointer, Severity::error);
    result.fail_closed = true;
    result.diagnostic = MakeLeaseDiagnostic(
        result.status,
        "SB-MEMORY-CLASS-LEASE-RENEW-UNKNOWN",
        "memory.class_lease.renew_unknown",
        {{"reason", it == leases_.end() ? "lease_not_found"
                                        : "lease_token_mismatch"},
         {"lease_id", std::to_string(request.lease.lease_id)}});
    return result;
  }

  LeaseRecord& record = it->second;
  const auto policy = PolicyForKindLocked(record.class_kind);
  if (request.now_ms >= record.deadline_ms) {
    ++renewal_refusal_count_;
    result.status =
        LeaseStatus(StatusCode::memory_limit_exceeded, Severity::error);
    result.fail_closed = true;
    result.lease = record.lease;
    result.deadline_ms = record.deadline_ms;
    result.renewal_count = record.renewal_count;
    result.diagnostic = MakeLeaseDiagnostic(
        result.status,
        "SB-MEMORY-CLASS-LEASE-RENEW-EXPIRED",
        "memory.class_lease.renew_expired",
        {{"reason", "lease_expired"},
         {"lease_id", std::to_string(record.lease.lease_id)},
         {"deadline_ms", std::to_string(record.deadline_ms)},
         {"now_ms", std::to_string(request.now_ms)}});
    AttachRenewalEvidence(&result, record, "lease_expired");
    return result;
  }
  if (!policy.renewable || record.renewal_count >= record.max_renewals) {
    ++renewal_refusal_count_;
    result.status =
        LeaseStatus(StatusCode::memory_limit_exceeded, Severity::error);
    result.fail_closed = true;
    result.lease = record.lease;
    result.deadline_ms = record.deadline_ms;
    result.renewal_count = record.renewal_count;
    result.diagnostic = MakeLeaseDiagnostic(
        result.status,
        "SB-MEMORY-CLASS-LEASE-RENEW-MAX-REFUSED",
        "memory.class_lease.renew_max_refused",
        {{"reason", "max_renewals_reached"},
         {"lease_id", std::to_string(record.lease.lease_id)},
         {"renewal_count", std::to_string(record.renewal_count)},
         {"max_renewals", std::to_string(record.max_renewals)}});
    AttachRenewalEvidence(&result, record, "max_renewals_reached");
    return result;
  }

  const u64 new_deadline =
      request.new_deadline_ms != 0
          ? request.new_deadline_ms
          : record.deadline_ms + request.extend_by_ms;
  if (new_deadline <= record.deadline_ms ||
      (policy.max_lease_ms != 0 && new_deadline - request.now_ms > policy.max_lease_ms)) {
    ++renewal_refusal_count_;
    result.status =
        LeaseStatus(StatusCode::memory_invalid_request, Severity::error);
    result.fail_closed = true;
    result.lease = record.lease;
    result.deadline_ms = record.deadline_ms;
    result.renewal_count = record.renewal_count;
    result.diagnostic = MakeLeaseDiagnostic(
        result.status,
        "SB-MEMORY-CLASS-LEASE-RENEW-DEADLINE-REFUSED",
        "memory.class_lease.renew_deadline_refused",
        {{"reason", "deadline_not_extended_or_exceeds_policy"},
         {"lease_id", std::to_string(record.lease.lease_id)},
         {"requested_deadline_ms", std::to_string(new_deadline)}});
    AttachRenewalEvidence(&result, record, "deadline_refused");
    return result;
  }

  record.deadline_ms = new_deadline;
  ++record.renewal_count;
  ++renewal_count_;
  auto& state = classes_[record.class_kind];
  ++state.renewal_count;

  result.status = OkStatus();
  result.lease = record.lease;
  result.deadline_ms = record.deadline_ms;
  result.renewal_count = record.renewal_count;
  result.diagnostic = MakeLeaseDiagnostic(
      result.status,
      "SB-MEMORY-CLASS-LEASE-RENEWED",
      "memory.class_lease.renewed",
      {{"lease_id", std::to_string(record.lease.lease_id)},
       {"deadline_ms", std::to_string(record.deadline_ms)},
       {"renewal_count", std::to_string(record.renewal_count)}});
  AttachRenewalEvidence(&result, record, "renewed");
  return result;
}

MemoryBudgetLeaseCleanupResult MemoryClassPolicyLeaseManager::CancelLease(
    MemoryBudgetLeaseToken lease) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = leases_.find(lease.lease_id);
  MemoryBudgetLeaseCleanupResult result;
  result.reason = MemoryBudgetLeaseCleanupReason::cancel;
  if (it == leases_.end() || !LeaseMatches(it->second.lease, lease)) {
    result.status =
        LeaseStatus(StatusCode::memory_unknown_pointer, Severity::error);
    result.fail_closed = true;
    result.diagnostic = MakeLeaseDiagnostic(
        result.status,
        "SB-MEMORY-CLASS-LEASE-CANCEL-UNKNOWN",
        "memory.class_lease.cancel_unknown",
        {{"reason", it == leases_.end() ? "lease_not_found"
                                        : "lease_token_mismatch"},
         {"lease_id", std::to_string(lease.lease_id)}});
    AttachCleanupEvidence(&result, MemoryBudgetLeaseCleanupReason::cancel,
                          "unknown");
    return result;
  }
  return CleanupLeaseLocked(lease.lease_id, MemoryBudgetLeaseCleanupReason::cancel);
}

MemoryBudgetLeaseCleanupResult
MemoryClassPolicyLeaseManager::CleanupExpiredLeases(u64 now_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  MemoryBudgetLeaseCleanupResult cleanup;
  cleanup.status = OkStatus();
  cleanup.reason = MemoryBudgetLeaseCleanupReason::expired;
  std::vector<u64> expired;
  for (const auto& entry : leases_) {
    if (entry.second.deadline_ms <= now_ms) {
      expired.push_back(entry.first);
    }
  }
  for (u64 lease_id : expired) {
    auto result = CleanupLeaseLocked(lease_id,
                                     MemoryBudgetLeaseCleanupReason::expired);
    if (result.ok()) {
      cleanup.cleaned_lease_count += result.cleaned_lease_count;
      cleanup.cleaned_bytes += result.cleaned_bytes;
    } else {
      cleanup.status = result.status;
      cleanup.diagnostic = result.diagnostic;
      cleanup.fail_closed = true;
      break;
    }
  }
  AttachCleanupEvidence(&cleanup, MemoryBudgetLeaseCleanupReason::expired,
                        "expired_leases");
  return cleanup;
}

MemoryBudgetLeaseCleanupResult MemoryClassPolicyLeaseManager::CleanupOwner(
    std::string owner_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  MemoryBudgetLeaseCleanupResult cleanup;
  cleanup.reason = MemoryBudgetLeaseCleanupReason::owner_disconnect;
  if (owner_id.empty()) {
    cleanup.status =
        LeaseStatus(StatusCode::memory_invalid_request, Severity::error);
    cleanup.fail_closed = true;
    cleanup.diagnostic = MakeLeaseDiagnostic(
        cleanup.status,
        "SB-MEMORY-CLASS-LEASE-OWNER-CLEANUP-INVALID",
        "memory.class_lease.owner_cleanup_invalid",
        {{"reason", "owner_id_empty"}});
    AttachCleanupEvidence(&cleanup,
                          MemoryBudgetLeaseCleanupReason::owner_disconnect,
                          "owner_id_empty");
    return cleanup;
  }
  cleanup.status = OkStatus();
  std::vector<u64> owned;
  for (const auto& entry : leases_) {
    if (entry.second.owner_id == owner_id) {
      owned.push_back(entry.first);
    }
  }
  for (u64 lease_id : owned) {
    auto result = CleanupLeaseLocked(
        lease_id, MemoryBudgetLeaseCleanupReason::owner_disconnect);
    if (result.ok()) {
      cleanup.cleaned_lease_count += result.cleaned_lease_count;
      cleanup.cleaned_bytes += result.cleaned_bytes;
    } else {
      cleanup.status = result.status;
      cleanup.diagnostic = result.diagnostic;
      cleanup.fail_closed = true;
      break;
    }
  }
  AttachCleanupEvidence(&cleanup,
                        MemoryBudgetLeaseCleanupReason::owner_disconnect,
                        owner_id);
  return cleanup;
}

MemoryBudgetLeaseRecoveryResult
MemoryClassPolicyLeaseManager::ClassifyRecovery(
    MemoryBudgetLeaseRecoveryInput input) {
  MemoryClassPolicy policy;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    policy = PolicyForKindLocked(input.class_kind);
    ++recovery_classification_count_;
    ++classes_[input.class_kind].recovery_classification_count;
  }

  MemoryBudgetLeaseRecoveryResult result;
  result.status = OkStatus();
  result.pressure_action = SelectPressureAction(policy, MemoryPressureState::recovery);

  std::string provenance_reason;
  if (!SafeProvenance(input.provenance, &provenance_reason)) {
    result.status =
        LeaseStatus(StatusCode::memory_invalid_request, Severity::error);
    result.fail_closed = true;
    result.classification =
        MemoryBudgetLeaseRecoveryClassification::unsafe_provenance_refused;
    result.pressure_action = MemoryClassPressureAction::deny;
    result.diagnostic = MakeLeaseDiagnostic(
        result.status,
        "SB-MEMORY-CLASS-LEASE-RECOVERY-PROVENANCE-REFUSED",
        "memory.class_lease.recovery_provenance_refused",
        {{"reason", provenance_reason},
         {"lease_id", std::to_string(input.lease_id)}});
    AttachRecoveryEvidence(&result, input, policy, provenance_reason);
    return result;
  }
  if (input.route_kind == MemoryBudgetLeaseRouteKind::cluster ||
      IsClusterMemoryCategory(policy.category)) {
    result.status =
        LeaseStatus(StatusCode::memory_invalid_request, Severity::error);
    result.fail_closed = true;
    result.classification =
        MemoryBudgetLeaseRecoveryClassification::external_cluster_provider_required;
    result.pressure_action =
        MemoryClassPressureAction::external_provider_required;
    result.diagnostic = MakeLeaseDiagnostic(
        result.status,
        "SB-MEMORY-CLASS-LEASE-RECOVERY-CLUSTER-REFUSED",
        "memory.class_lease.recovery_cluster_refused",
        {{"reason", "cluster_memory_external_provider_only"},
         {"lease_id", std::to_string(input.lease_id)}});
    AttachRecoveryEvidence(&result, input, policy,
                           "cluster_memory_external_provider_only");
    return result;
  }
  if (policy.requires_protected_material_route &&
      !ProtectedRecoverySafe(input)) {
    result.classification =
        MemoryBudgetLeaseRecoveryClassification::protected_material_quarantine;
    result.pressure_action =
        MemoryClassPressureAction::refuse_protected_material;
    result.diagnostic = MakeLeaseDiagnostic(
        result.status,
        "SB-MEMORY-CLASS-LEASE-RECOVERY-PROTECTED-QUARANTINE",
        "memory.class_lease.recovery_protected_quarantine",
        {{"reason", "protected_material_route_incomplete"},
         {"lease_id", std::to_string(input.lease_id)}});
    AttachRecoveryEvidence(&result, input, policy,
                           "protected_material_route_incomplete");
    return result;
  }
  if (input.deadline_ms != 0 && input.deadline_ms <= input.now_ms) {
    result.classification =
        MemoryBudgetLeaseRecoveryClassification::expired_cleanup_required;
    result.diagnostic = MakeLeaseDiagnostic(
        result.status,
        "SB-MEMORY-CLASS-LEASE-RECOVERY-EXPIRED",
        "memory.class_lease.recovery_expired",
        {{"reason", "expired_cleanup_required"},
         {"lease_id", std::to_string(input.lease_id)}});
    AttachRecoveryEvidence(&result, input, policy,
                           "expired_cleanup_required");
    return result;
  }

  result.classification =
      MemoryBudgetLeaseRecoveryClassification::evidence_only_rebuilt;
  result.diagnostic = MakeLeaseDiagnostic(
      result.status,
      "SB-MEMORY-CLASS-LEASE-RECOVERY-CLASSIFIED",
      "memory.class_lease.recovery_classified",
      {{"reason", "evidence_only_rebuilt"},
       {"lease_id", std::to_string(input.lease_id)}});
  AttachRecoveryEvidence(&result, input, policy, "evidence_only_rebuilt");
  return result;
}

MemoryBudgetLeaseSnapshot MemoryClassPolicyLeaseManager::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  MemoryBudgetLeaseSnapshot snapshot;
  snapshot.active_bytes = active_bytes_;
  snapshot.active_lease_count = static_cast<u64>(leases_.size());
  snapshot.created_lease_count = created_lease_count_;
  snapshot.renewal_count = renewal_count_;
  snapshot.renewal_refusal_count = renewal_refusal_count_;
  snapshot.cancel_cleanup_count = cancel_cleanup_count_;
  snapshot.expiry_cleanup_count = expiry_cleanup_count_;
  snapshot.owner_cleanup_count = owner_cleanup_count_;
  snapshot.recovery_classification_count = recovery_classification_count_;
  snapshot.unsafe_provenance_refusal_count = unsafe_provenance_refusal_count_;
  snapshot.cluster_refusal_count = cluster_refusal_count_;
  snapshot.protected_material_refusal_count = protected_material_refusal_count_;

  for (const auto& entry : leases_) {
    const auto& record = entry.second;
    MemoryBudgetLeaseRecordSnapshot lease;
    lease.lease = record.lease;
    lease.class_kind = record.class_kind;
    lease.class_name = record.class_name;
    lease.owner_id = record.owner_id;
    lease.created_at_ms = record.created_at_ms;
    lease.deadline_ms = record.deadline_ms;
    lease.renewal_count = record.renewal_count;
    lease.max_renewals = record.max_renewals;
    lease.pressure_action = record.pressure_action;
    snapshot.leases.push_back(std::move(lease));
  }
  for (const auto& entry : classes_) {
    const auto& state = entry.second;
    const auto& policy = state.policy;
    MemoryClassPolicySnapshot cls;
    cls.kind = policy.kind;
    cls.class_name = policy.class_name;
    cls.category = policy.category;
    cls.active_bytes = state.active_bytes;
    cls.active_lease_count = state.active_lease_count;
    cls.created_lease_count = state.created_lease_count;
    cls.renewal_count = state.renewal_count;
    cls.cancel_cleanup_count = state.cancel_cleanup_count;
    cls.expiry_cleanup_count = state.expiry_cleanup_count;
    cls.owner_cleanup_count = state.owner_cleanup_count;
    cls.recovery_classification_count = state.recovery_classification_count;
    cls.max_lease_ms = policy.max_lease_ms;
    cls.max_renewals = policy.max_renewals;
    cls.requires_protected_material_route =
        policy.requires_protected_material_route;
    cls.plugin_udr_class = policy.plugin_udr_class;
    cls.parser_handoff_class = policy.parser_handoff_class;
    cls.diagnostics_support_class = policy.diagnostics_support_class;
    cls.critical_engine_class = policy.critical_engine_class;
    cls.soft_pressure_action = policy.soft_pressure_action;
    cls.high_pressure_action = policy.high_pressure_action;
    cls.emergency_pressure_action = policy.emergency_pressure_action;
    snapshot.classes.push_back(std::move(cls));
  }
  snapshot.metrics.push_back(
      {"sb_memory_budget_lease_active_bytes", "global", "ceic-026",
       snapshot.active_bytes, "bytes"});
  snapshot.metrics.push_back(
      {"sb_memory_budget_lease_active_total", "global", "ceic-026",
       snapshot.active_lease_count, "count"});
  snapshot.metrics.push_back(
      {"sb_memory_budget_lease_renewals_total", "global", "ceic-026",
       snapshot.renewal_count, "count"});
  snapshot.metrics.push_back(
      {"sb_memory_class_pressure_actions_total", "global", "ceic-026",
       snapshot.created_lease_count, "count"});
  snapshot.support_bundle_rows.push_back(
      {"memory_class_policy_lease.authority_scope",
       kClassLeaseAuthorityScope, "public", false});
  snapshot.support_bundle_rows.push_back(
      {"memory_class_policy_lease.integrated_support_bundle_closure",
       "not_claimed_ceic_091_pending", "public", false});
  snapshot.support_bundle_rows.push_back(
      {"memory_class_policy_lease.cluster_boundary",
       "external_provider_only_fail_closed", "public", false});
  snapshot.support_bundle_rows.push_back(
      {"memory_class_policy_lease.protected_material",
       "redacted_reference_only_zero_on_release_required", "protected_material",
       true});
  return snapshot;
}

MemoryClassPolicy MemoryClassPolicyLeaseManager::PolicyForKindLocked(
    MemoryClassKind kind) const {
  auto it = classes_.find(kind);
  if (it != classes_.end()) {
    return it->second.policy;
  }
  return DefaultMemoryClassPolicy(kind);
}

MemoryBudgetLeaseDecision MemoryClassPolicyLeaseManager::FailDecision(
    const MemoryBudgetLeaseRequest& request,
    MemoryClassPolicy policy,
    MemoryClassPressureAction action,
    StatusCode code,
    Severity severity,
    std::string diagnostic_code,
    std::string message_key,
    std::string reason,
    bool cluster_external_provider_required) {
  MemoryBudgetLeaseDecision decision;
  decision.status = LeaseStatus(code, severity);
  decision.class_kind = policy.kind;
  decision.class_name = policy.class_name;
  decision.pressure_action = action;
  decision.fail_closed = true;
  decision.protected_material_excluded =
      policy.excludes_protected_material_from_support;
  decision.cluster_external_provider_required =
      cluster_external_provider_required;
  decision.diagnostic = MakeLeaseDiagnostic(
      decision.status,
      std::move(diagnostic_code),
      std::move(message_key),
      {{"reason", reason},
       {"class", policy.class_name},
       {"pressure_action", MemoryClassPressureActionName(action)},
       {"requested_bytes", std::to_string(request.requested_bytes)},
       {"route_kind", MemoryBudgetLeaseRouteKindName(request.route_kind)}});
  AttachDecisionEvidence(&decision, request, policy, reason);
  return decision;
}

MemoryBudgetLeaseDecision MemoryClassPolicyLeaseManager::GrantDecision(
    const MemoryBudgetLeaseRequest& request,
    const MemoryClassPolicy& policy,
    MemoryBudgetLeaseToken lease,
    MemoryClassPressureAction action) {
  MemoryBudgetLeaseDecision decision;
  decision.status = OkStatus();
  decision.class_kind = policy.kind;
  decision.class_name = policy.class_name;
  decision.pressure_action = action;
  decision.lease = lease;
  decision.support_bundle_ready = true;
  decision.protected_material_excluded =
      policy.excludes_protected_material_from_support;
  decision.diagnostic = MakeLeaseDiagnostic(
      decision.status,
      "SB-MEMORY-CLASS-LEASE-GRANTED",
      "memory.class_lease.granted",
      {{"class", policy.class_name},
       {"lease_id", std::to_string(lease.lease_id)},
       {"creation_sequence", std::to_string(lease.creation_sequence)},
       {"pressure_action", MemoryClassPressureActionName(action)},
       {"requested_bytes", std::to_string(request.requested_bytes)}});
  AttachDecisionEvidence(&decision, request, policy, "granted");
  return decision;
}

MemoryBudgetLeaseCleanupResult
MemoryClassPolicyLeaseManager::CleanupLeaseLocked(
    u64 lease_id,
    MemoryBudgetLeaseCleanupReason reason) {
  MemoryBudgetLeaseCleanupResult result;
  result.reason = reason;
  auto it = leases_.find(lease_id);
  if (it == leases_.end()) {
    result.status =
        LeaseStatus(StatusCode::memory_unknown_pointer, Severity::error);
    result.fail_closed = true;
    result.diagnostic = MakeLeaseDiagnostic(
        result.status,
        "SB-MEMORY-CLASS-LEASE-CLEANUP-UNKNOWN",
        "memory.class_lease.cleanup_unknown",
        {{"reason", "lease_not_found"},
         {"lease_id", std::to_string(lease_id)}});
    AttachCleanupEvidence(&result, reason, "unknown");
    return result;
  }

  const LeaseRecord record = it->second;
  HierarchicalMemoryBudgetOperationResult ledger_result;
  if (reason == MemoryBudgetLeaseCleanupReason::cancel) {
    ledger_result = ledger_->Cancel(record.lease.reservation);
  } else {
    ledger_result = ledger_->Release(record.lease.reservation);
  }
  if (!ledger_result.ok()) {
    result.status = ledger_result.status;
    result.diagnostic = ledger_result.diagnostic;
    result.fail_closed = true;
    AttachCleanupEvidence(&result, reason, record.owner_id);
    return result;
  }

  auto& state = classes_[record.class_kind];
  state.active_bytes =
      state.active_bytes >= record.lease.bytes
          ? state.active_bytes - record.lease.bytes
          : 0;
  if (state.active_lease_count != 0) {
    --state.active_lease_count;
  }
  active_bytes_ = active_bytes_ >= record.lease.bytes
                      ? active_bytes_ - record.lease.bytes
                      : 0;
  switch (reason) {
    case MemoryBudgetLeaseCleanupReason::cancel:
      ++state.cancel_cleanup_count;
      ++cancel_cleanup_count_;
      break;
    case MemoryBudgetLeaseCleanupReason::expired:
      ++state.expiry_cleanup_count;
      ++expiry_cleanup_count_;
      break;
    case MemoryBudgetLeaseCleanupReason::owner_disconnect:
      ++state.owner_cleanup_count;
      ++owner_cleanup_count_;
      break;
  }
  leases_.erase(it);

  result.status = OkStatus();
  result.cleaned_lease_count = 1;
  result.cleaned_bytes = record.lease.bytes;
  result.diagnostic = MakeLeaseDiagnostic(
      result.status,
      "SB-MEMORY-CLASS-LEASE-CLEANED",
      "memory.class_lease.cleaned",
      {{"reason", MemoryBudgetLeaseCleanupReasonName(reason)},
       {"lease_id", std::to_string(record.lease.lease_id)},
       {"class", record.class_name},
       {"bytes", std::to_string(record.lease.bytes)}});
  AttachCleanupEvidence(&result, reason, record.owner_id);
  return result;
}

void MemoryClassPolicyLeaseManager::AttachDecisionEvidence(
    MemoryBudgetLeaseDecision* decision,
    const MemoryBudgetLeaseRequest& request,
    const MemoryClassPolicy& policy,
    std::string reason) const {
  decision->support_bundle_ready = true;
  decision->evidence.push_back("CEIC-026_MEMORY_CLASS_POLICY_LEASES");
  decision->evidence.push_back(kClassLeaseAuthorityScope);
  decision->evidence.push_back("memory_class_policy_lease.reason=" + reason);
  decision->evidence.push_back("memory_class_policy_lease.class=" +
                               policy.class_name);
  decision->evidence.push_back(
      std::string("memory_class_policy_lease.category=") +
      MemoryCategoryName(policy.category));
  decision->evidence.push_back(
      std::string("memory_class_policy_lease.pressure_state=") +
      MemoryPressureStateName(request.pressure_state));
  decision->evidence.push_back(
      std::string("memory_class_policy_lease.pressure_action=") +
      MemoryClassPressureActionName(decision->pressure_action));
  decision->evidence.push_back(
      std::string("memory_class_policy_lease.route_kind=") +
      MemoryBudgetLeaseRouteKindName(request.route_kind));
  decision->evidence.push_back(
      "memory_class_policy_lease.no_authority.transaction_finality=true");
  decision->evidence.push_back(
      "memory_class_policy_lease.no_authority.visibility=true");
  decision->evidence.push_back(
      "memory_class_policy_lease.no_authority.authorization_security=true");
  decision->evidence.push_back(
      "memory_class_policy_lease.no_authority.recovery=true");
  decision->evidence.push_back(
      "memory_class_policy_lease.no_authority.parser_donor_wal=true");
  decision->evidence.push_back(
      "memory_class_policy_lease.no_authority.benchmark_optimizer_index_agent=true");
  decision->evidence.push_back(
      "memory_class_policy_lease.cluster_boundary=external_provider_only_fail_closed");
  if (policy.requires_protected_material_route) {
    decision->evidence.push_back(
        std::string("memory_class_policy_lease.protected_material.route=") +
        (request.protected_material_routed_through_protected_buffer ? "protected_buffer"
                                                                    : "missing"));
    decision->evidence.push_back(
        std::string("memory_class_policy_lease.protected_material.redacted=") +
        (request.protected_material_redacted ? "true" : "false"));
    decision->evidence.push_back(
        std::string("memory_class_policy_lease.protected_material.zero_on_release=") +
        (request.protected_zero_on_release ? "true" : "false"));
  }
  if (policy.plugin_udr_class) {
    decision->evidence.push_back(
        std::string("memory_class_policy_lease.plugin_udr.sandboxed=") +
        (request.plugin_udr_sandboxed ? "true" : "false"));
    decision->evidence.push_back(
        "memory_class_policy_lease.plugin_udr.native_sandbox_closure=implemented_CEIC_027");
  }
  for (const auto& scope_ref : request.scope_chain) {
    decision->evidence.push_back("memory_class_policy_lease.scope=" +
                                 ScopeKey(scope_ref));
  }
  decision->metrics.push_back(
      {"sb_memory_budget_lease_decision_total", "global", "ceic-026", 1,
       "count"});
  decision->metrics.push_back(
      {"sb_memory_class_pressure_action_total", policy.class_name,
       MemoryClassPressureActionName(decision->pressure_action), 1, "count"});
  decision->support_bundle_rows.push_back(
      {"memory_class_policy_lease.class", policy.class_name, "public", false});
  decision->support_bundle_rows.push_back(
      {"memory_class_policy_lease.pressure_action",
       MemoryClassPressureActionName(decision->pressure_action), "public",
       false});
  decision->support_bundle_rows.push_back(
      {"memory_class_policy_lease.authority_scope", kClassLeaseAuthorityScope,
       "public", false});
  decision->support_bundle_rows.push_back(
      {"memory_class_policy_lease.integrated_support_bundle_closure",
       "not_claimed_ceic_091_pending", "public", false});
  if (policy.requires_protected_material_route) {
    decision->support_bundle_rows.push_back(
        {"memory_class_policy_lease.protected_material",
         "protected_reference_only_redacted_zero_on_release", "protected_material",
         true});
  }
}

void MemoryClassPolicyLeaseManager::AttachCleanupEvidence(
    MemoryBudgetLeaseCleanupResult* result,
    MemoryBudgetLeaseCleanupReason reason,
    std::string scope_id) const {
  result->evidence.push_back("CEIC-026_MEMORY_CLASS_POLICY_LEASES");
  result->evidence.push_back(kClassLeaseAuthorityScope);
  result->evidence.push_back(
      std::string("memory_class_policy_lease.cleanup.reason=") +
      MemoryBudgetLeaseCleanupReasonName(reason));
  result->evidence.push_back("memory_class_policy_lease.cleanup.scope=" +
                             scope_id);
  result->evidence.push_back(
      "memory_class_policy_lease.cleanup.no_transaction_finality_or_recovery_authority=true");
  result->metrics.push_back(
      {"sb_memory_budget_lease_cleanup_total",
       MemoryBudgetLeaseCleanupReasonName(reason), scope_id,
       result->cleaned_lease_count, "count"});
  result->support_bundle_rows.push_back(
      {"memory_class_policy_lease.cleanup.reason",
       MemoryBudgetLeaseCleanupReasonName(reason), "public", false});
  result->support_bundle_rows.push_back(
      {"memory_class_policy_lease.authority_scope", kClassLeaseAuthorityScope,
       "public", false});
}

void MemoryClassPolicyLeaseManager::AttachRenewalEvidence(
    MemoryBudgetLeaseRenewalResult* result,
    const LeaseRecord& record,
    std::string reason) const {
  result->evidence.push_back("CEIC-026_MEMORY_CLASS_POLICY_LEASES");
  result->evidence.push_back(kClassLeaseAuthorityScope);
  result->evidence.push_back("memory_class_policy_lease.renew.reason=" +
                             reason);
  result->evidence.push_back("memory_class_policy_lease.renew.class=" +
                             record.class_name);
  result->evidence.push_back("memory_class_policy_lease.renew.lease_id=" +
                             std::to_string(record.lease.lease_id));
  result->evidence.push_back("memory_class_policy_lease.renew.deadline_ms=" +
                             std::to_string(record.deadline_ms));
  result->evidence.push_back("memory_class_policy_lease.renew.count=" +
                             std::to_string(record.renewal_count));
  result->evidence.push_back("memory_class_policy_lease.renew.max=" +
                             std::to_string(record.max_renewals));
  result->evidence.push_back(
      "memory_class_policy_lease.renew.no_authority.transaction_visibility_recovery=true");
  result->metrics.push_back(
      {"sb_memory_budget_lease_renewal_total", record.class_name,
       record.owner_id, result->ok() ? 1ull : 0ull, "count"});
  result->support_bundle_rows.push_back(
      {"memory_class_policy_lease.renew.reason", reason, "public", false});
  result->support_bundle_rows.push_back(
      {"memory_class_policy_lease.authority_scope", kClassLeaseAuthorityScope,
       "public", false});
}

void MemoryClassPolicyLeaseManager::AttachRecoveryEvidence(
    MemoryBudgetLeaseRecoveryResult* result,
    const MemoryBudgetLeaseRecoveryInput& input,
    const MemoryClassPolicy& policy,
    std::string reason) const {
  result->evidence.push_back("CEIC-026_MEMORY_CLASS_POLICY_LEASES");
  result->evidence.push_back(kClassLeaseAuthorityScope);
  result->evidence.push_back(
      "memory_class_policy_lease.recovery.evidence_only=true");
  result->evidence.push_back(
      "memory_class_policy_lease.recovery.not_transaction_recovery_authority=true");
  result->evidence.push_back("memory_class_policy_lease.recovery.reason=" +
                             reason);
  result->evidence.push_back(
      std::string("memory_class_policy_lease.recovery.classification=") +
      MemoryBudgetLeaseRecoveryClassificationName(result->classification));
  result->evidence.push_back("memory_class_policy_lease.recovery.class=" +
                             policy.class_name);
  result->evidence.push_back("memory_class_policy_lease.recovery.lease_id=" +
                             std::to_string(input.lease_id));
  result->evidence.push_back(
      std::string("memory_class_policy_lease.recovery.pressure_action=") +
      MemoryClassPressureActionName(result->pressure_action));
  result->metrics.push_back(
      {"sb_memory_budget_lease_recovery_classification_total",
       policy.class_name,
       MemoryBudgetLeaseRecoveryClassificationName(result->classification), 1,
       "count"});
  result->support_bundle_rows.push_back(
      {"memory_class_policy_lease.recovery.classification",
       MemoryBudgetLeaseRecoveryClassificationName(result->classification),
       "public", false});
  result->support_bundle_rows.push_back(
      {"memory_class_policy_lease.authority_scope", kClassLeaseAuthorityScope,
       "public", false});
}

}  // namespace scratchbird::core::memory
