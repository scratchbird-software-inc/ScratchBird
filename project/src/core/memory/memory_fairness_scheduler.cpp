// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory_fairness_scheduler.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace scratchbird::core::memory {
namespace {

using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::StatusCode;

constexpr const char* kFairnessAuthorityScope =
    "evidence_only_not_transaction_finality_visibility_authorization_security_recovery_parser_donor_wal_benchmark_optimizer_plan_index_finality_cluster_or_agent_action_authority";

Status FairnessStatus(StatusCode code, Severity severity) {
  return {code, severity, Subsystem::memory};
}

Status OkStatus() {
  return FairnessStatus(StatusCode::ok, Severity::info);
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

DiagnosticRecord MakeFairnessDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::vector<DiagnosticArgument> arguments = {}) {
  arguments.push_back({"authority_scope", kFairnessAuthorityScope});
  return MakeDiagnostic(
      status.code,
      status.severity,
      status.subsystem,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(arguments),
      {},
      "core.memory.multi_tenant_memory_fairness_scheduler",
      "Use CEIC-025 scheduling decisions only as memory admission evidence; do not treat memory evidence as transaction, security, recovery, parser, donor, optimizer, index, cluster, or agent authority.");
}

}  // namespace

const char* MemoryFairnessWorkClassName(MemoryFairnessWorkClass work_class) {
  switch (work_class) {
    case MemoryFairnessWorkClass::foreground:
      return "foreground";
    case MemoryFairnessWorkClass::background:
      return "background";
  }
  return "unknown";
}

const char* MemoryFairnessDecisionActionName(
    MemoryFairnessDecisionAction action) {
  switch (action) {
    case MemoryFairnessDecisionAction::grant:
      return "grant";
    case MemoryFairnessDecisionAction::spill:
      return "spill";
    case MemoryFairnessDecisionAction::throttle:
      return "throttle";
    case MemoryFairnessDecisionAction::cancel:
      return "cancel";
    case MemoryFairnessDecisionAction::deny:
      return "deny";
  }
  return "unknown";
}

MultiTenantMemoryFairnessScheduler::MultiTenantMemoryFairnessScheduler(
    HierarchicalMemoryBudgetLedger* ledger)
    : ledger_(ledger) {}

HierarchicalMemoryBudgetOperationResult
MultiTenantMemoryFairnessScheduler::SetScopePolicy(
    MemoryFairnessScopePolicy policy) {
  HierarchicalMemoryBudgetOperationResult result;
  std::string provenance_reason;
  if (!SafeProvenance(policy.provenance, &provenance_reason)) {
    result.status =
        FairnessStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeFairnessDiagnostic(
        result.status,
        "SB-MEMORY-FAIRNESS-POLICY-PROVENANCE-REFUSED",
        "memory.fairness.policy.provenance_refused",
        {{"scope_kind", HierarchicalMemoryScopeKindName(policy.scope.kind)},
         {"scope_id", policy.scope.scope_id},
         {"reason", provenance_reason}});
    return result;
  }
  if (ledger_ == nullptr) {
    result.status =
        FairnessStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeFairnessDiagnostic(
        result.status,
        "SB-MEMORY-FAIRNESS-LEDGER-MISSING",
        "memory.fairness.ledger_missing",
        {{"reason", "ceic_011_ledger_required"}});
    return result;
  }
  if (policy.scope.scope_id.empty()) {
    result.status =
        FairnessStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeFairnessDiagnostic(
        result.status,
        "SB-MEMORY-FAIRNESS-POLICY-SCOPE-INVALID",
        "memory.fairness.policy.scope_invalid",
        {{"scope_kind", HierarchicalMemoryScopeKindName(policy.scope.kind)},
         {"reason", "scope_id_empty"}});
    return result;
  }
  if (policy.hard_max_bytes != 0 &&
      policy.soft_max_bytes > policy.hard_max_bytes) {
    result.status =
        FairnessStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeFairnessDiagnostic(
        result.status,
        "SB-MEMORY-FAIRNESS-POLICY-LIMITS-INVALID",
        "memory.fairness.policy.limits_invalid",
        {{"scope_kind", HierarchicalMemoryScopeKindName(policy.scope.kind)},
         {"scope_id", policy.scope.scope_id},
         {"hard_max_bytes", std::to_string(policy.hard_max_bytes)},
         {"soft_max_bytes", std::to_string(policy.soft_max_bytes)}});
    return result;
  }
  if (policy.hard_max_bytes != 0 &&
      policy.guarantee_bytes > policy.hard_max_bytes) {
    result.status =
        FairnessStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeFairnessDiagnostic(
        result.status,
        "SB-MEMORY-FAIRNESS-POLICY-GUARANTEE-INVALID",
        "memory.fairness.policy.guarantee_invalid",
        {{"scope_kind", HierarchicalMemoryScopeKindName(policy.scope.kind)},
         {"scope_id", policy.scope.scope_id},
         {"hard_max_bytes", std::to_string(policy.hard_max_bytes)},
         {"guarantee_bytes", std::to_string(policy.guarantee_bytes)}});
    return result;
  }
  if (policy.priority_weight == 0) {
    policy.priority_weight = 1;
  }

  HierarchicalMemoryBudget budget;
  budget.scope = policy.scope;
  budget.hard_limit_bytes = policy.hard_max_bytes;
  budget.soft_limit_bytes = 0;
  budget.provenance = policy.provenance;
  auto budget_result = ledger_->SetBudget(std::move(budget));
  if (!budget_result.ok()) {
    return budget_result;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto& state = scopes_[ScopeKey(policy.scope)];
  state.policy = std::move(policy);
  result.status = OkStatus();
  return result;
}

MemoryFairnessDecision MultiTenantMemoryFairnessScheduler::Admit(
    MemoryFairnessRequest request) {
  std::string invalid_reason;
  std::string duplicate_scope_key;
  if (request.requested_bytes == 0 ||
      !ValidateScopeChain(request.scope_chain, &invalid_reason,
                          &duplicate_scope_key)) {
    return FailDecision(
        request,
        MemoryFairnessDecisionAction::deny,
        StatusCode::memory_invalid_request,
        Severity::error,
        "SB-MEMORY-FAIRNESS-REQUEST-INVALID",
        "memory.fairness.request.invalid",
        request.requested_bytes == 0 ? "requested_bytes_zero" : invalid_reason,
        duplicate_scope_key,
        false,
        false,
        false,
        false,
        false);
  }
  if (ledger_ == nullptr) {
    return FailDecision(request,
                        MemoryFairnessDecisionAction::deny,
                        StatusCode::memory_invalid_request,
                        Severity::error,
                        "SB-MEMORY-FAIRNESS-LEDGER-MISSING",
                        "memory.fairness.ledger_missing",
                        "ceic_011_ledger_required",
                        {},
                        false,
                        false,
                        false,
                        false,
                        false);
  }
  std::string provenance_reason;
  if (!SafeProvenance(request.provenance, &provenance_reason)) {
    return FailDecision(
        request,
        MemoryFairnessDecisionAction::deny,
        StatusCode::memory_invalid_request,
        Severity::error,
        "SB-MEMORY-FAIRNESS-REQUEST-PROVENANCE-REFUSED",
        "memory.fairness.request.provenance_refused",
        provenance_reason,
        {},
        false,
        false,
        false,
        false,
        false);
  }
  if (IsClusterMemoryCategory(request.category)) {
    return FailDecision(
        request,
        MemoryFairnessDecisionAction::deny,
        StatusCode::memory_invalid_request,
        Severity::error,
        "SB-MEMORY-FAIRNESS-CLUSTER-REFUSED",
        "memory.fairness.cluster.refused",
        "cluster_production_memory_scheduling_blocked",
        {},
        false,
        false,
        false,
        false,
        false);
  }
  if (request.memory_class.empty()) {
    request.memory_class = "unclassified";
  }
  if (request.weight == 0) {
    request.weight = 1;
  }

  bool grant_uses_burst = false;
  bool starvation_prevention_applied = false;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    const u64 request_priority_weight = RequestPriorityWeight(request);
    for (const auto& scope_ref : request.scope_chain) {
      ScopeState& state = MutableScopeStateLocked(scope_ref);
      RefreshBurstWindowLocked(&state, request.now_ms);
      const u64 projected = state.active_bytes + request.requested_bytes;
      const auto& policy = state.policy;
      const std::string key = ScopeKey(scope_ref);
      const bool request_within_guarantee =
          policy.guarantee_bytes != 0 &&
          projected <= policy.guarantee_bytes &&
          request.work_class == MemoryFairnessWorkClass::foreground;
      const bool waited =
          policy.starvation_prevention_ms != 0 &&
          request.wait_started_at_ms != 0 &&
          request.now_ms >= request.wait_started_at_ms &&
          request.now_ms - request.wait_started_at_ms >=
              policy.starvation_prevention_ms;
      if (request_within_guarantee &&
          (waited || request.prior_refusal_count != 0)) {
        starvation_prevention_applied = true;
      }

      if (policy.hard_max_bytes != 0 && projected > policy.hard_max_bytes) {
        lock.unlock();
        return FailDecision(request,
                            ReliefActionForRequest(request),
                            StatusCode::memory_limit_exceeded,
                            Severity::error,
                            "SB-MEMORY-FAIRNESS-HARD-MAX-REFUSED",
                            "memory.fairness.hard_max.refused",
                            "hard_max_exceeded",
                            key,
                            true,
                            false,
                            false,
                            false,
                            starvation_prevention_applied);
      }
      if (policy.soft_max_bytes != 0 && projected > policy.soft_max_bytes &&
          !request_within_guarantee) {
        bool burst_expired = false;
        if (ScopeCanUseBurstLocked(&state, projected, request.now_ms,
                                   &burst_expired)) {
          grant_uses_burst = true;
          continue;
        }
        if (burst_expired) {
          ++state.burst_refusal_count;
          ++burst_refusal_count_;
        }
        lock.unlock();
        return FailDecision(request,
                            ReliefActionForRequest(request),
                            StatusCode::memory_limit_exceeded,
                            Severity::warning,
                            "SB-MEMORY-FAIRNESS-SOFT-MAX-RELIEF",
                            "memory.fairness.soft_max.relief",
                            "soft_max_exceeded",
                            key,
                            false,
                            true,
                            burst_expired,
                            false,
                            starvation_prevention_applied);
      }
    }

    const u64 root_hard = RootHardLimitLocked(request);
    const u64 protected_headroom =
        ProtectedForegroundHeadroomLocked(request, request_priority_weight);
    if (root_hard != 0 &&
        active_bytes_ + request.requested_bytes + protected_headroom >
            root_hard) {
      lock.unlock();
      return FailDecision(
          request,
          ReliefActionForRequest(request),
          StatusCode::memory_limit_exceeded,
          Severity::warning,
          "SB-MEMORY-FAIRNESS-FOREGROUND-PROTECTION",
          "memory.fairness.foreground_protection",
          "foreground_guarantee_headroom_protected",
          {},
          false,
          false,
          false,
          true,
          starvation_prevention_applied);
    }
  }

  HierarchicalMemoryReservationRequest reservation_request;
  reservation_request.scope_chain = request.scope_chain;
  reservation_request.category = request.category;
  reservation_request.memory_class = request.memory_class;
  reservation_request.requested_bytes = request.requested_bytes;
  reservation_request.owner_id = request.owner_id;
  reservation_request.spillable = request.spillable;
  reservation_request.cancelable = request.cancelable;
  reservation_request.priority = request.priority;
  reservation_request.weight = request.weight;
  reservation_request.lease_expires_at_ms = request.lease_expires_at_ms;
  reservation_request.provenance = request.provenance;
  auto reservation = ledger_->Reserve(std::move(reservation_request));
  if (!reservation.ok()) {
    MemoryFairnessDecision decision;
    decision.status = reservation.status;
    decision.diagnostic = reservation.diagnostic;
    switch (reservation.recommendation) {
      case HierarchicalMemoryReservationRecommendation::spill:
        decision.action = MemoryFairnessDecisionAction::spill;
        break;
      case HierarchicalMemoryReservationRecommendation::cancel:
        decision.action = MemoryFairnessDecisionAction::cancel;
        break;
      case HierarchicalMemoryReservationRecommendation::degrade:
      case HierarchicalMemoryReservationRecommendation::deny:
      case HierarchicalMemoryReservationRecommendation::granted:
        decision.action = ReliefActionForRequest(request);
        break;
    }
    decision.dominant_scope_key = "ceic_011_ledger";
    decision.hard_max_exceeded =
        reservation.recommendation ==
        HierarchicalMemoryReservationRecommendation::deny;
    decision.soft_max_exceeded =
        reservation.recommendation !=
        HierarchicalMemoryReservationRecommendation::deny;
    decision.starvation_prevention_applied = starvation_prevention_applied;
    AttachEvidenceRows(&decision, request, "ceic_011_ledger_refused");
    std::lock_guard<std::mutex> lock(mutex_);
    CountDecisionLocked(decision);
    return decision;
  }

  auto commit = ledger_->Commit(reservation.token);
  if (!commit.ok()) {
    (void)ledger_->Release(reservation.token);
    MemoryFairnessDecision decision;
    decision.status = commit.status;
    decision.diagnostic = commit.diagnostic;
    decision.action = MemoryFairnessDecisionAction::deny;
    decision.dominant_scope_key = "ceic_011_ledger_commit";
    decision.starvation_prevention_applied = starvation_prevention_applied;
    AttachEvidenceRows(&decision, request, "ceic_011_commit_refused");
    std::lock_guard<std::mutex> lock(mutex_);
    CountDecisionLocked(decision);
    return decision;
  }

  MemoryFairnessGrantToken grant;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    grant.grant_id = next_grant_id_++;
    grant.bytes = request.requested_bytes;
    grant.reservation = reservation.token;
    const u64 priority_weight = RequestPriorityWeight(request);
    for (const auto& scope_ref : request.scope_chain) {
      ScopeState& state = MutableScopeStateLocked(scope_ref);
      const u64 previous = state.active_bytes;
      state.active_bytes += request.requested_bytes;
      state.peak_bytes = std::max(state.peak_bytes, state.active_bytes);
      ++state.active_grant_count;
      state.priority_weight_total += priority_weight;
      ++state.grant_count;
      if (grant_uses_burst && state.policy.soft_max_bytes != 0 &&
          previous + request.requested_bytes > state.policy.soft_max_bytes) {
        if (state.burst_window_expires_at_ms == 0 &&
            state.policy.burst_window_ms != 0) {
          state.burst_window_expires_at_ms =
              request.now_ms + state.policy.burst_window_ms;
        }
        ++state.burst_grant_count;
      }
      if (starvation_prevention_applied) {
        ++state.starvation_prevention_count;
      }
    }
    active_bytes_ += request.requested_bytes;
    peak_bytes_ = std::max(peak_bytes_, active_bytes_);
    if (grant_uses_burst) {
      ++burst_grant_count_;
    }
    if (starvation_prevention_applied) {
      ++starvation_prevention_count_;
    }
    GrantRecord record;
    record.grant = grant;
    record.scope_chain = request.scope_chain;
    record.priority_weight = priority_weight;
    record.burst_used = grant_uses_burst;
    grants_[grant.grant_id] = std::move(record);
  }
  return GrantDecision(request, grant, grant_uses_burst,
                       starvation_prevention_applied);
}

HierarchicalMemoryBudgetOperationResult
MultiTenantMemoryFairnessScheduler::Release(MemoryFairnessGrantToken grant) {
  HierarchicalMemoryBudgetOperationResult result;
  if (!grant.valid()) {
    result.status =
        FairnessStatus(StatusCode::memory_unknown_pointer, Severity::error);
    result.diagnostic = MakeFairnessDiagnostic(
        result.status,
        "SB-MEMORY-FAIRNESS-RELEASE-UNKNOWN-GRANT",
        "memory.fairness.release.unknown_grant",
        {{"reason", "grant_invalid"},
         {"grant_id", std::to_string(grant.grant_id)}});
    return result;
  }
  if (ledger_ == nullptr) {
    result.status =
        FairnessStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeFairnessDiagnostic(
        result.status,
        "SB-MEMORY-FAIRNESS-LEDGER-MISSING",
        "memory.fairness.ledger_missing",
        {{"reason", "ceic_011_ledger_required_for_release"},
         {"grant_id", std::to_string(grant.grant_id)}});
    return result;
  }

  GrantRecord record;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = grants_.find(grant.grant_id);
    if (it == grants_.end() || it->second.grant.bytes != grant.bytes) {
      result.status =
          FairnessStatus(StatusCode::memory_unknown_pointer, Severity::error);
      result.diagnostic = MakeFairnessDiagnostic(
          result.status,
          "SB-MEMORY-FAIRNESS-RELEASE-UNKNOWN-GRANT",
          "memory.fairness.release.unknown_grant",
          {{"reason", it == grants_.end() ? "grant_not_found"
                                          : "grant_bytes_mismatch"},
           {"grant_id", std::to_string(grant.grant_id)}});
      return result;
    }
    record = it->second;
  }

  auto ledger_release = ledger_->Release(record.grant.reservation);
  if (!ledger_release.ok()) {
    return ledger_release;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& scope_ref : record.scope_chain) {
      ScopeState& state = MutableScopeStateLocked(scope_ref);
      state.active_bytes = state.active_bytes >= record.grant.bytes
                               ? state.active_bytes - record.grant.bytes
                               : 0;
      if (state.active_grant_count != 0) {
        --state.active_grant_count;
      }
      state.priority_weight_total =
          state.priority_weight_total >= record.priority_weight
              ? state.priority_weight_total - record.priority_weight
              : 0;
    }
    active_bytes_ = active_bytes_ >= record.grant.bytes
                        ? active_bytes_ - record.grant.bytes
                        : 0;
    grants_.erase(record.grant.grant_id);
    ++release_count_;
  }

  result.status = OkStatus();
  return result;
}

MemoryFairnessSnapshot MultiTenantMemoryFairnessScheduler::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  MemoryFairnessSnapshot snapshot;
  snapshot.active_bytes = active_bytes_;
  snapshot.peak_bytes = peak_bytes_;
  snapshot.decision_count = decision_count_;
  snapshot.grant_count = grant_count_;
  snapshot.release_count = release_count_;
  snapshot.spill_count = spill_count_;
  snapshot.throttle_count = throttle_count_;
  snapshot.cancel_count = cancel_count_;
  snapshot.deny_count = deny_count_;
  snapshot.burst_grant_count = burst_grant_count_;
  snapshot.burst_refusal_count = burst_refusal_count_;
  snapshot.starvation_prevention_count = starvation_prevention_count_;
  snapshot.foreground_protection_count = foreground_protection_count_;
  for (const auto& entry : scopes_) {
    const auto& state = entry.second;
    MemoryFairnessScopeSnapshot scope;
    scope.kind = state.policy.scope.kind;
    scope.scope_id = state.policy.scope.scope_id;
    scope.guarantee_bytes = state.policy.guarantee_bytes;
    scope.soft_max_bytes = state.policy.soft_max_bytes;
    scope.hard_max_bytes = state.policy.hard_max_bytes;
    scope.burst_bytes = state.policy.burst_bytes;
    scope.burst_window_ms = state.policy.burst_window_ms;
    scope.burst_window_expires_at_ms = state.burst_window_expires_at_ms;
    scope.active_bytes = state.active_bytes;
    scope.peak_bytes = state.peak_bytes;
    scope.active_grant_count = state.active_grant_count;
    scope.priority_weight_total = state.priority_weight_total;
    scope.grant_count = state.grant_count;
    scope.spill_count = state.spill_count;
    scope.throttle_count = state.throttle_count;
    scope.cancel_count = state.cancel_count;
    scope.deny_count = state.deny_count;
    scope.burst_grant_count = state.burst_grant_count;
    scope.burst_refusal_count = state.burst_refusal_count;
    scope.starvation_prevention_count = state.starvation_prevention_count;
    scope.foreground_protection_count = state.foreground_protection_count;
    scope.background_scope = state.policy.background_scope;
    snapshot.scopes.push_back(std::move(scope));
  }
  snapshot.metrics.push_back(
      {"sb_memory_tenant_fairness_active_bytes", "global", "ceic-025",
       snapshot.active_bytes, "bytes"});
  snapshot.metrics.push_back(
      {"sb_memory_tenant_fairness_decisions_total", "global", "ceic-025",
       snapshot.decision_count, "count"});
  snapshot.metrics.push_back(
      {"sb_memory_tenant_fairness_foreground_protection_total", "global",
       "ceic-025", snapshot.foreground_protection_count, "count"});
  snapshot.metrics.push_back(
      {"sb_memory_tenant_fairness_starvation_prevention_total", "global",
       "ceic-025", snapshot.starvation_prevention_count, "count"});
  snapshot.support_bundle_rows.push_back(
      {"memory_fairness.authority_scope", kFairnessAuthorityScope, "public",
       false});
  snapshot.support_bundle_rows.push_back(
      {"memory_fairness.integrated_support_bundle_closure",
       "not_claimed_ceic_091_pending", "public", false});
  return snapshot;
}

MemoryFairnessDecision MultiTenantMemoryFairnessScheduler::FailDecision(
    const MemoryFairnessRequest& request,
    MemoryFairnessDecisionAction action,
    StatusCode code,
    Severity severity,
    std::string diagnostic_code,
    std::string message_key,
    std::string reason,
    std::string dominant_scope_key,
    bool hard_max_exceeded,
    bool soft_max_exceeded,
    bool burst_window_expired,
    bool foreground_protection_applied,
    bool starvation_prevention_applied) {
  MemoryFairnessDecision decision;
  decision.status = FairnessStatus(code, severity);
  decision.action = action;
  decision.dominant_scope_key = std::move(dominant_scope_key);
  decision.hard_max_exceeded = hard_max_exceeded;
  decision.soft_max_exceeded = soft_max_exceeded;
  decision.burst_window_expired = burst_window_expired;
  decision.foreground_protection_applied = foreground_protection_applied;
  decision.starvation_prevention_applied = starvation_prevention_applied;
  decision.diagnostic = MakeFairnessDiagnostic(
      decision.status,
      std::move(diagnostic_code),
      std::move(message_key),
      {{"reason", reason},
       {"action", MemoryFairnessDecisionActionName(action)},
       {"dominant_scope", decision.dominant_scope_key},
       {"requested_bytes", std::to_string(request.requested_bytes)},
       {"work_class", MemoryFairnessWorkClassName(request.work_class)}});
  AttachEvidenceRows(&decision, request, reason);
  std::lock_guard<std::mutex> lock(mutex_);
  CountDecisionLocked(decision);
  return decision;
}

MemoryFairnessDecision MultiTenantMemoryFairnessScheduler::GrantDecision(
    const MemoryFairnessRequest& request,
    MemoryFairnessGrantToken grant,
    bool burst_used,
    bool starvation_prevention_applied) {
  MemoryFairnessDecision decision;
  decision.status = OkStatus();
  decision.action = MemoryFairnessDecisionAction::grant;
  decision.grant = grant;
  decision.burst_used = burst_used;
  decision.starvation_prevention_applied = starvation_prevention_applied;
  decision.support_bundle_ready = true;
  decision.diagnostic = MakeFairnessDiagnostic(
      decision.status,
      "SB-MEMORY-FAIRNESS-GRANTED",
      "memory.fairness.granted",
      {{"action", "grant"},
       {"requested_bytes", std::to_string(request.requested_bytes)},
       {"grant_id", std::to_string(grant.grant_id)},
       {"work_class", MemoryFairnessWorkClassName(request.work_class)}});
  AttachEvidenceRows(&decision, request, "granted");
  std::lock_guard<std::mutex> lock(mutex_);
  CountDecisionLocked(decision);
  return decision;
}

MultiTenantMemoryFairnessScheduler::ScopeState&
MultiTenantMemoryFairnessScheduler::MutableScopeStateLocked(
    const HierarchicalMemoryScopeRef& scope) {
  auto& state = scopes_[ScopeKey(scope)];
  if (state.policy.scope.scope_id.empty()) {
    state.policy.scope = scope;
    state.policy.priority_weight = 1;
  }
  return state;
}

const MultiTenantMemoryFairnessScheduler::ScopeState*
MultiTenantMemoryFairnessScheduler::FindScopeStateLocked(
    const HierarchicalMemoryScopeRef& scope) const {
  const auto it = scopes_.find(ScopeKey(scope));
  if (it == scopes_.end()) {
    return nullptr;
  }
  return &it->second;
}

void MultiTenantMemoryFairnessScheduler::RefreshBurstWindowLocked(
    ScopeState* state,
    u64 now_ms) {
  if (state->burst_window_expires_at_ms != 0 &&
      now_ms >= state->burst_window_expires_at_ms) {
    state->burst_window_expires_at_ms = 0;
  }
}

bool MultiTenantMemoryFairnessScheduler::ScopeCanUseBurstLocked(
    ScopeState* state,
    u64 projected_bytes,
    u64 now_ms,
    bool* expired) const {
  *expired = false;
  const auto& policy = state->policy;
  if (policy.burst_bytes == 0 || policy.burst_window_ms == 0 ||
      policy.soft_max_bytes == 0) {
    return false;
  }
  if (state->burst_window_expires_at_ms == 0 &&
      state->active_bytes > policy.soft_max_bytes) {
    *expired = true;
    return false;
  }
  if (projected_bytes > policy.soft_max_bytes + policy.burst_bytes) {
    return false;
  }
  if (state->burst_window_expires_at_ms == 0) {
    return true;
  }
  return now_ms < state->burst_window_expires_at_ms;
}

u64 MultiTenantMemoryFairnessScheduler::RequestPriorityWeight(
    const MemoryFairnessRequest& request) const {
  return static_cast<u64>(std::max(request.priority, 0)) +
         std::max<u64>(request.weight, 1);
}

u64 MultiTenantMemoryFairnessScheduler::RootHardLimitLocked(
    const MemoryFairnessRequest& request) const {
  u64 hard = 0;
  for (const auto& scope_ref : request.scope_chain) {
    if (scope_ref.kind != HierarchicalMemoryScopeKind::process &&
        scope_ref.kind != HierarchicalMemoryScopeKind::database) {
      continue;
    }
    const auto* state = FindScopeStateLocked(scope_ref);
    if (state == nullptr || state->policy.hard_max_bytes == 0) {
      continue;
    }
    hard = hard == 0 ? state->policy.hard_max_bytes
                     : std::min(hard, state->policy.hard_max_bytes);
  }
  return hard;
}

u64 MultiTenantMemoryFairnessScheduler::ProtectedForegroundHeadroomLocked(
    const MemoryFairnessRequest& request,
    u64 request_priority_weight) const {
  u64 protected_headroom = 0;
  for (const auto& scope_ref : request.scope_chain) {
    if (scope_ref.kind != HierarchicalMemoryScopeKind::process &&
        scope_ref.kind != HierarchicalMemoryScopeKind::database) {
      continue;
    }
    const auto* root = FindScopeStateLocked(scope_ref);
    if (root != nullptr) {
      if (request.work_class == MemoryFairnessWorkClass::background) {
        protected_headroom = std::max(protected_headroom,
                                      root->policy.foreground_protection_bytes);
      }
    }
  }
  for (const auto& entry : scopes_) {
    const auto& state = entry.second;
    if (state.policy.guarantee_bytes == 0 || state.policy.background_scope) {
      continue;
    }
    const bool protects_against_request =
        request.work_class == MemoryFairnessWorkClass::background ||
        state.policy.priority_weight > request_priority_weight;
    if (!protects_against_request ||
        RequestContainsScopeLocked(entry.first, request)) {
      continue;
    }
    if (state.active_bytes < state.policy.guarantee_bytes) {
      protected_headroom += state.policy.guarantee_bytes - state.active_bytes;
    }
  }
  return protected_headroom;
}

bool MultiTenantMemoryFairnessScheduler::RequestContainsScopeLocked(
    const std::string& scope_key,
    const MemoryFairnessRequest& request) const {
  for (const auto& scope_ref : request.scope_chain) {
    if (ScopeKey(scope_ref) == scope_key) {
      return true;
    }
  }
  return false;
}

MemoryFairnessDecisionAction
MultiTenantMemoryFairnessScheduler::ReliefActionForRequest(
    const MemoryFairnessRequest& request) const {
  if (request.spillable) {
    return MemoryFairnessDecisionAction::spill;
  }
  if (request.throttleable) {
    return MemoryFairnessDecisionAction::throttle;
  }
  if (request.cancelable) {
    return MemoryFairnessDecisionAction::cancel;
  }
  return MemoryFairnessDecisionAction::deny;
}

void MultiTenantMemoryFairnessScheduler::CountDecisionLocked(
    const MemoryFairnessDecision& decision) {
  ++decision_count_;
  switch (decision.action) {
    case MemoryFairnessDecisionAction::grant:
      ++grant_count_;
      break;
    case MemoryFairnessDecisionAction::spill:
      ++spill_count_;
      break;
    case MemoryFairnessDecisionAction::throttle:
      ++throttle_count_;
      break;
    case MemoryFairnessDecisionAction::cancel:
      ++cancel_count_;
      break;
    case MemoryFairnessDecisionAction::deny:
      ++deny_count_;
      break;
  }
  if (decision.foreground_protection_applied) {
    ++foreground_protection_count_;
  }
  for (const auto& row : decision.evidence) {
    const std::string prefix = "memory_fairness.affected_scope=";
    if (row.find(prefix) != 0) {
      continue;
    }
    const auto key = row.substr(prefix.size());
    auto it = scopes_.find(key);
    if (it == scopes_.end()) {
      continue;
    }
    switch (decision.action) {
      case MemoryFairnessDecisionAction::grant:
        break;
      case MemoryFairnessDecisionAction::spill:
        ++it->second.spill_count;
        break;
      case MemoryFairnessDecisionAction::throttle:
        ++it->second.throttle_count;
        break;
      case MemoryFairnessDecisionAction::cancel:
        ++it->second.cancel_count;
        break;
      case MemoryFairnessDecisionAction::deny:
        ++it->second.deny_count;
        break;
    }
    if (decision.foreground_protection_applied) {
      ++it->second.foreground_protection_count;
    }
  }
}

void MultiTenantMemoryFairnessScheduler::AttachEvidenceRows(
    MemoryFairnessDecision* decision,
    const MemoryFairnessRequest& request,
    const std::string& reason) const {
  decision->support_bundle_ready = true;
  decision->evidence.push_back("CEIC-025_MULTI_TENANT_MEMORY_FAIRNESS");
  decision->evidence.push_back(std::string("memory_fairness.action=") +
                               MemoryFairnessDecisionActionName(
                                   decision->action));
  decision->evidence.push_back("memory_fairness.reason=" + reason);
  decision->evidence.push_back(
      std::string("memory_fairness.work_class=") +
      MemoryFairnessWorkClassName(request.work_class));
  decision->evidence.push_back("memory_fairness.requested_bytes=" +
                               std::to_string(request.requested_bytes));
  decision->evidence.push_back(
      std::string("memory_fairness.starvation_prevention.applied=") +
      (decision->starvation_prevention_applied ? "true" : "false"));
  decision->evidence.push_back(
      std::string("memory_fairness.foreground_protection.applied=") +
      (decision->foreground_protection_applied ? "true" : "false"));
  decision->evidence.push_back(
      std::string("memory_fairness.burst.used=") +
      (decision->burst_used ? "true" : "false"));
  decision->evidence.push_back(
      std::string("memory_fairness.burst.window_expired=") +
      (decision->burst_window_expired ? "true" : "false"));
  decision->evidence.push_back(
      std::string("memory_fairness.authority_scope=") +
      kFairnessAuthorityScope);
  decision->evidence.push_back(
      "memory_fairness.integrated_support_bundle_closure=not_claimed_ceic_091_pending");
  decision->evidence.push_back(
      "memory_fairness.cluster_production_behavior=blocked_not_implemented");
  for (const auto& scope_ref : request.scope_chain) {
    decision->evidence.push_back("memory_fairness.affected_scope=" +
                                 ScopeKey(scope_ref));
  }
  decision->metrics.push_back(
      {"sb_memory_tenant_fairness_decision_total", "global", "ceic-025", 1,
       "count"});
  decision->metrics.push_back(
      {"sb_memory_tenant_fairness_requested_bytes", "request",
       request.owner_id, request.requested_bytes, "bytes"});
  decision->support_bundle_rows.push_back(
      {"memory_fairness.action",
       MemoryFairnessDecisionActionName(decision->action),
       "public",
       false});
  decision->support_bundle_rows.push_back(
      {"memory_fairness.reason", reason, "public", false});
  decision->support_bundle_rows.push_back(
      {"memory_fairness.authority_scope", kFairnessAuthorityScope, "public",
       false});
  decision->support_bundle_rows.push_back(
      {"memory_fairness.integrated_support_bundle_closure",
       "not_claimed_ceic_091_pending", "public", false});
}

}  // namespace scratchbird::core::memory
