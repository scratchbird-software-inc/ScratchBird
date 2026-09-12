// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory_fairness_scheduler.hpp"

#include <algorithm>
#include <limits>
#include <string_view>
#include <new>
#include <type_traits>
#include <set>
#include <utility>

namespace scratchbird::core::memory {
namespace {

using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::StatusCode;

constexpr const char* kFairnessAuthorityScope =
    "evidence_only_not_transaction_finality_visibility_authorization_security_recovery_parser_reference_wal_benchmark_optimizer_plan_index_finality_cluster_or_agent_action_authority";

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
  if (provenance.parser_authority || provenance.reference_authority ||
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
    if (scope.scope_id.empty() || static_cast<unsigned>(scope.kind) >
        static_cast<unsigned>(HierarchicalMemoryScopeKind::descriptor_snapshot)) {
      *reason = "scope_id_empty_or_kind_invalid";
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

// A pending real reservation is owned even while later preparation fails.
struct PendingReservation {
  HierarchicalMemoryBudgetLedger* ledger;
  HierarchicalMemoryReservationToken token;
  ~PendingReservation() { if (token.valid()) (void)ledger->ReleaseNoAlloc(token); }
};
void Increment(u64& value) noexcept {
  if (value != std::numeric_limits<u64>::max()) ++value;
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
      "Use CEIC-025 scheduling decisions only as memory admission evidence; do not treat memory evidence as transaction, security, recovery, parser, reference, optimizer, index, cluster, or agent authority.");
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
    MemoryFairnessScopePolicy policy) try {
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

  // Serialize policy publication with scheduler readers and prepare any map
  // node before changing the actual parent budget. The parent update uses
  // reject_change for still-owned grants; a refusal leaves both policies alone.
  const auto key = ScopeKey(policy.scope);
  std::lock_guard<std::mutex> lock(mutex_);
  const auto existing = scopes_.find(key);
  decltype(scopes_) prepared;
  if (existing == scopes_.end()) {
    ScopeState state;
    state.policy = std::move(policy);
    prepared.emplace(key, std::move(state));
  }
  auto node = prepared.empty() ? decltype(scopes_)::node_type{} : prepared.extract(prepared.begin());
  auto budget_result = ledger_->SetBudget(std::move(budget));
  if (!budget_result.ok()) {
    return budget_result;
  }
  static_assert(std::is_nothrow_move_assignable_v<MemoryFairnessScopePolicy>);
  static_assert(std::is_nothrow_move_constructible_v<HierarchicalMemoryBudgetOperationResult>);
  if (existing != scopes_.end()) {
    existing->second.policy = std::move(policy);
  } else {
    // Same-allocator node transfer and string comparison do not allocate.
    scopes_.insert(std::move(node));
  }
  result.status = OkStatus();
  return result;
} catch (const std::bad_alloc&) {
  HierarchicalMemoryBudgetOperationResult result;
  result.status = FairnessStatus(StatusCode::memory_allocation_failed, Severity::error);
  result.diagnostic.status = result.status;
  return result;
}

MemoryFairnessDecision MultiTenantMemoryFairnessScheduler::Admit(
    MemoryFairnessRequest request) try {
  // One lock orders policy checks, parent ownership and local publication.
  // There is no reclaim wait or callback while holding this admission lock.
  std::lock_guard lock(mutex_);
  std::string reason, duplicate;
  if (request.requested_bytes == 0 ||
      !ValidateScopeChain(request.scope_chain, &reason, &duplicate) ||
      static_cast<unsigned>(request.work_class) > static_cast<unsigned>(MemoryFairnessWorkClass::background) ||
      static_cast<unsigned>(request.category) > static_cast<unsigned>(MemoryCategory::test_probe)) {
    return FailDecision(request, MemoryFairnessDecisionAction::deny,
        StatusCode::memory_invalid_request, Severity::error,
        "SB-MEMORY-FAIRNESS-REQUEST-INVALID", "memory.fairness.request.invalid",
        "invalid_bytes_scope_or_profile", duplicate, false, false, false, false, false);
  }
  if (!ledger_ || !SafeProvenance(request.provenance, &reason)) {
    return FailDecision(request, MemoryFairnessDecisionAction::deny,
        StatusCode::memory_invalid_request, Severity::error,
        !ledger_ ? "SB-MEMORY-FAIRNESS-LEDGER-MISSING" : "SB-MEMORY-FAIRNESS-REQUEST-PROVENANCE-REFUSED",
        "memory.fairness.admission.authority_refused",
        !ledger_ ? "ceic_011_ledger_required" : reason, {}, false, false, false, false, false);
  }
  if (request.memory_class.empty()) request.memory_class = "unclassified";
  if (request.weight == 0) request.weight = 1;
  constexpr auto max = std::numeric_limits<u64>::max();
  const auto priority = static_cast<u64>(std::max(request.priority, 0));
  const auto overflow = [&] {
    return FailDecision(request, MemoryFairnessDecisionAction::deny,
        StatusCode::memory_limit_exceeded, Severity::error,
        "SB-MEMORY-FAIRNESS-CAPACITY-EXHAUSTED", "memory.fairness.capacity.exhausted",
        "arithmetic_or_grant_identity_exhausted", {}, true, false, false, false, false);
  };
  if (next_grant_id_ == 0 || request.weight > max - priority ||
      request.requested_bytes > max - active_bytes_) return overflow();
  const auto priority_weight = RequestPriorityWeight(request);

  // New scope nodes are private until the complete grant can be published.
  // Node transfer preserves pointers held by existing and newly staged grants.
  decltype(scopes_) prepared_scopes;
  std::vector<ScopeState*> affected;
  std::vector<bool> uses_burst;
  affected.reserve(request.scope_chain.size());
  uses_burst.reserve(request.scope_chain.size());
  bool burst_used = false, starvation = false;
  for (const auto& ref : request.scope_chain) {
    const auto key = ScopeKey(ref);
    const auto found = scopes_.find(key);
    ScopeState* state = nullptr;
    if (found != scopes_.end()) state = &found->second;
    else {
      ScopeState prepared;
      prepared.policy.scope = ref;
      state = &prepared_scopes.emplace(key, std::move(prepared)).first->second;
    }
    affected.push_back(state);
    uses_burst.push_back(false);
    if (request.requested_bytes > max - state->active_bytes ||
        priority_weight > max - state->priority_weight_total) return overflow();
    const auto projected = state->active_bytes + request.requested_bytes;
    const auto& policy = state->policy;
    const bool guaranteed = policy.guarantee_bytes != 0 &&
        projected <= policy.guarantee_bytes && request.work_class == MemoryFairnessWorkClass::foreground;
    const bool waited = policy.starvation_prevention_ms != 0 &&
        request.wait_started_at_ms != 0 && request.now_ms >= request.wait_started_at_ms &&
        request.now_ms - request.wait_started_at_ms >= policy.starvation_prevention_ms;
    starvation = starvation || (guaranteed && (waited || request.prior_refusal_count != 0));
    if (policy.hard_max_bytes && projected > policy.hard_max_bytes)
      return FailDecision(request, ReliefActionForRequest(request),
          StatusCode::memory_limit_exceeded, Severity::error,
          "SB-MEMORY-FAIRNESS-HARD-MAX-REFUSED", "memory.fairness.hard_max.refused",
          "hard_max_exceeded", key, true, false, false, false, starvation);
    if (policy.soft_max_bytes && projected > policy.soft_max_bytes && !guaranteed) {
      bool expired = false;
      if (!ScopeCanUseBurstLocked(state, projected, request.now_ms, &expired))
        return FailDecision(request, ReliefActionForRequest(request),
            StatusCode::memory_limit_exceeded, Severity::warning,
            "SB-MEMORY-FAIRNESS-SOFT-MAX-RELIEF", "memory.fairness.soft_max.relief",
            "soft_max_exceeded", key, false, true, expired, false, starvation);
      if ((state->burst_window_expires_at_ms == 0 || request.now_ms >= state->burst_window_expires_at_ms) &&
          policy.burst_window_ms > max - request.now_ms) return overflow();
      uses_burst.back() = true;
      burst_used = true;
    }
  }
  const auto root_hard = RootHardLimitLocked(request);
  const auto headroom = ProtectedForegroundHeadroomLocked(request, priority_weight);
  if (!headroom) return overflow();
  const auto projected_total = active_bytes_ + request.requested_bytes;
  if (root_hard && (*headroom > root_hard || projected_total > root_hard - *headroom))
    return FailDecision(request, ReliefActionForRequest(request),
        StatusCode::memory_limit_exceeded, Severity::warning,
        "SB-MEMORY-FAIRNESS-FOREGROUND-PROTECTION", "memory.fairness.foreground_protection",
        "foreground_guarantee_headroom_protected", {}, false, false, false, true, starvation);

  MemoryFairnessGrantToken grant;
  grant.grant_id = next_grant_id_;
  grant.bytes = request.requested_bytes;
  // All rich response/evidence and local owning metadata precede the real
  // reservation. Neither telemetry nor optional strings can orphan a grant.
  auto decision = GrantDecision(request, grant, burst_used, starvation);
  GrantRecord record;
  record.grant = grant;
  record.scopes = affected;
  record.priority_weight = priority_weight;
  record.burst_used = burst_used;
  decltype(grants_) prepared_grants;
  prepared_grants.emplace(grant.grant_id, std::move(record));
  auto node = prepared_grants.extract(prepared_grants.begin());
  HierarchicalMemoryReservationRequest parent;
  parent.scope_chain = request.scope_chain;
  parent.category = request.category;
  parent.memory_class = request.memory_class;
  parent.requested_bytes = request.requested_bytes;
  parent.owner_id = request.owner_id;
  parent.spillable = request.spillable;
  parent.cancelable = request.cancelable;
  parent.priority = request.priority;
  parent.weight = request.weight;
  parent.lease_expires_at_ms = request.lease_expires_at_ms;
  parent.provenance = request.provenance;
  const auto parent_failure = [&](Status status, DiagnosticRecord diagnostic, const char* why,
      HierarchicalMemoryReservationRecommendation recommendation = HierarchicalMemoryReservationRecommendation::deny) {
    MemoryFairnessDecision refused;
    refused.status = status;
    refused.diagnostic = std::move(diagnostic);
    refused.diagnostic.status = status;
    refused.action = MemoryFairnessDecisionAction::deny;
    if (status.code == StatusCode::memory_limit_exceeded) {
      switch (recommendation) {
        case HierarchicalMemoryReservationRecommendation::spill: refused.action = MemoryFairnessDecisionAction::spill; break;
        case HierarchicalMemoryReservationRecommendation::cancel: refused.action = MemoryFairnessDecisionAction::cancel; break;
        default: refused.action = ReliefActionForRequest(request); break;
      }
      refused.hard_max_exceeded = recommendation == HierarchicalMemoryReservationRecommendation::deny;
      refused.soft_max_exceeded = !refused.hard_max_exceeded;
    }
    refused.starvation_prevention_applied = starvation;
    refused.dominant_scope_key = "ceic_011_ledger";
    AttachEvidenceRows(&refused, request, why);
    CountDecisionLocked(refused, request.scope_chain);
    return refused;
  };
  PendingReservation pending{ledger_, {}};
  auto reserved = ledger_->Reserve(std::move(parent));
  if (!reserved.ok()) return parent_failure(reserved.status, std::move(reserved.diagnostic),
      "ceic_011_ledger_refused", reserved.recommendation);
  pending.token = reserved.token;
  auto committed = ledger_->Commit(reserved.token);
  if (!committed.ok()) return parent_failure(committed.status, std::move(committed.diagnostic), "ceic_011_commit_refused");
  auto retained = ledger_->Retain(reserved.token);
  if (!retained.ok()) return parent_failure(retained.status, {}, "ceic_011_retain_refused");
  node.mapped().lease = std::move(retained.lease);
  pending.token = {};
  auto use = node.mapped().lease.Use();
  if (!use.live()) return parent_failure(
      FairnessStatus(StatusCode::memory_invalid_request, Severity::error), {}, "ceic_011_parent_revoked");

  // No allocation or ledger call follows this publication boundary. The
  // retained use guard prevents parent revocation from racing final adoption.
  grant.reservation = reserved.token;
  node.mapped().grant = grant;
  decision.grant = grant;
  scopes_.merge(prepared_scopes);
  grants_.insert(std::move(node));
  for (usize i = 0; i < affected.size(); ++i) {
    auto& state = *affected[i];
    RefreshBurstWindowLocked(&state, request.now_ms);
    state.active_bytes += request.requested_bytes;
    state.peak_bytes = std::max(state.peak_bytes, state.active_bytes);
    ++state.active_grant_count;
    state.priority_weight_total += priority_weight;
    Increment(state.grant_count);
    if (uses_burst[i]) {
      if (!state.burst_window_expires_at_ms)
        state.burst_window_expires_at_ms = request.now_ms + state.policy.burst_window_ms;
      Increment(state.burst_grant_count);
    }
    if (starvation) Increment(state.starvation_prevention_count);
  }
  active_bytes_ = projected_total;
  peak_bytes_ = std::max(peak_bytes_, active_bytes_);
  if (burst_used) Increment(burst_grant_count_);
  if (starvation) Increment(starvation_prevention_count_);
  next_grant_id_ = next_grant_id_ == max ? 0 : next_grant_id_ + 1;
  CountDecisionLocked(decision, request.scope_chain);
  static_assert(std::is_nothrow_move_constructible_v<MemoryFairnessDecision>);
  return decision;
} catch (const std::bad_alloc&) {
  MemoryFairnessDecision result;
  result.status = FairnessStatus(StatusCode::memory_allocation_failed, Severity::error);
  result.diagnostic.status = result.status;
  return result;
}

Status MultiTenantMemoryFairnessScheduler::ReleaseNoAlloc(MemoryFairnessGrantToken grant) {
  std::lock_guard lock(mutex_);
  const auto unknown = FairnessStatus(StatusCode::memory_unknown_pointer, Severity::error);
  if (!ledger_) return FairnessStatus(StatusCode::memory_invalid_request, Severity::error);
  if (!grant.valid()) return unknown;
  auto it = grants_.find(grant.grant_id);
  if (it == grants_.end()) return unknown;
  auto& record = it->second;
  if (record.grant.bytes != grant.bytes ||
      record.grant.reservation.token_id != grant.reservation.token_id ||
      record.grant.reservation.bytes != grant.reservation.bytes) return unknown;
  const auto status = record.lease.Reset();
  if (!status.ok()) return status;
  for (auto* state : record.scopes) {
    state->active_bytes -= record.grant.bytes;
    --state->active_grant_count;
    state->priority_weight_total -= record.priority_weight;
  }
  active_bytes_ -= record.grant.bytes;
  grants_.erase(it);
  Increment(release_count_);
  return OkStatus();
}

HierarchicalMemoryBudgetOperationResult
MultiTenantMemoryFairnessScheduler::Release(MemoryFairnessGrantToken grant) try {
  HierarchicalMemoryBudgetOperationResult result;
  result.status = ReleaseNoAlloc(grant);
  if (!result.ok())
    result.diagnostic = MakeFairnessDiagnostic(result.status,
        "SB-MEMORY-FAIRNESS-RELEASE-UNKNOWN-GRANT", "memory.fairness.release.unknown_grant",
        {{"reason", ledger_ ? "grant_identity_invalid_or_parent_release_failed" : "ceic_011_ledger_required_for_release"}});
  return result;
} catch (const std::bad_alloc&) {
  HierarchicalMemoryBudgetOperationResult result;
  result.status = FairnessStatus(StatusCode::memory_allocation_failed, Severity::error);
  result.diagnostic.status = result.status;
  return result;
}

MultiTenantMemoryFairnessScheduler::~MultiTenantMemoryFairnessScheduler() {
  // Callers quiesce grant users first. Destruction of each retained lease
  // releases the actual parent charge without allocating metadata.
  std::lock_guard lock(mutex_);
  grants_.clear();
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
  CountDecisionLocked(decision, request.scope_chain);
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
  return decision;
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
  if ((state->burst_window_expires_at_ms == 0 || now_ms >= state->burst_window_expires_at_ms) &&
      state->active_bytes > policy.soft_max_bytes) {
    *expired = true;
    return false;
  }
  if (projected_bytes > policy.soft_max_bytes &&
      projected_bytes - policy.soft_max_bytes > policy.burst_bytes) {
    return false;
  }
  if (state->burst_window_expires_at_ms == 0 || now_ms >= state->burst_window_expires_at_ms) {
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

std::optional<u64> MultiTenantMemoryFairnessScheduler::ProtectedForegroundHeadroomLocked(
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
      const auto available = state.policy.guarantee_bytes - state.active_bytes;
      if (available > std::numeric_limits<u64>::max() - protected_headroom) return std::nullopt;
      protected_headroom += available;
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
    const MemoryFairnessDecision& decision,
    const std::vector<HierarchicalMemoryScopeRef>& affected) noexcept {
  // Optional evidence is not accounting authority. Match preexisting typed
  // scope fields directly, without allocating keys or parsing strings.
  Increment(decision_count_);
  switch (decision.action) {
    case MemoryFairnessDecisionAction::grant: Increment(grant_count_); break;
    case MemoryFairnessDecisionAction::spill: Increment(spill_count_); break;
    case MemoryFairnessDecisionAction::throttle: Increment(throttle_count_); break;
    case MemoryFairnessDecisionAction::cancel: Increment(cancel_count_); break;
    case MemoryFairnessDecisionAction::deny: Increment(deny_count_); break;
  }
  if (decision.foreground_protection_applied) Increment(foreground_protection_count_);
  if (decision.burst_window_expired) Increment(burst_refusal_count_);
  for (auto& [key, state] : scopes_) {
    bool matches = false;
    for (const auto& ref : affected)
      if (ref.kind == state.policy.scope.kind && ref.scope_id == state.policy.scope.scope_id) {
        matches = true; break;
      }
    if (!matches) continue;
    switch (decision.action) {
      case MemoryFairnessDecisionAction::grant: break;
      case MemoryFairnessDecisionAction::spill: Increment(state.spill_count); break;
      case MemoryFairnessDecisionAction::throttle: Increment(state.throttle_count); break;
      case MemoryFairnessDecisionAction::cancel: Increment(state.cancel_count); break;
      case MemoryFairnessDecisionAction::deny: Increment(state.deny_count); break;
    }
    if (decision.foreground_protection_applied) Increment(state.foreground_protection_count);
    if (decision.burst_window_expired && key == decision.dominant_scope_key)
      Increment(state.burst_refusal_count);
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
      "memory_fairness.cluster_category=local_accounting_only_not_cluster_authority");
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
