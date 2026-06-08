// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CEIC-025: multi-tenant memory fairness and scheduling.
#include "hierarchical_memory_budget_ledger.hpp"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace scratchbird::core::memory {

// CEIC-025_MULTI_TENANT_MEMORY_FAIRNESS
enum class MemoryFairnessWorkClass {
  foreground,
  background
};

enum class MemoryFairnessDecisionAction {
  grant,
  spill,
  throttle,
  cancel,
  deny
};

struct MemoryFairnessScopePolicy {
  HierarchicalMemoryScopeRef scope;
  u64 guarantee_bytes = 0;
  u64 soft_max_bytes = 0;
  u64 hard_max_bytes = 0;
  u64 burst_bytes = 0;
  u64 burst_window_ms = 0;
  u64 priority_weight = 1;
  u64 starvation_prevention_ms = 0;
  u64 foreground_protection_bytes = 0;
  bool background_scope = false;
  HierarchicalMemoryBudgetProvenance provenance;
};

struct MemoryFairnessRequest {
  std::vector<HierarchicalMemoryScopeRef> scope_chain;
  MemoryCategory category = MemoryCategory::unknown;
  std::string memory_class = "unclassified";
  std::string owner_id;
  u64 requested_bytes = 0;
  u64 now_ms = 0;
  u64 wait_started_at_ms = 0;
  u64 prior_refusal_count = 0;
  u64 lease_expires_at_ms = 0;
  MemoryFairnessWorkClass work_class = MemoryFairnessWorkClass::foreground;
  bool spillable = false;
  bool throttleable = true;
  bool cancelable = false;
  int priority = 0;
  u64 weight = 1;
  HierarchicalMemoryBudgetProvenance provenance;
};

struct MemoryFairnessGrantToken {
  u64 grant_id = 0;
  u64 bytes = 0;
  HierarchicalMemoryReservationToken reservation;

  bool valid() const {
    return grant_id != 0 && bytes != 0 && reservation.valid();
  }
};

struct MemoryFairnessMetricRow {
  std::string metric_name;
  std::string scope_kind;
  std::string scope_id;
  u64 value = 0;
  std::string unit = "count";
};

struct MemoryFairnessSupportBundleRow {
  std::string key;
  std::string value;
  std::string redaction_class = "public";
  bool redacted = false;
};

struct MemoryFairnessDecision {
  Status status;
  DiagnosticRecord diagnostic;
  MemoryFairnessDecisionAction action = MemoryFairnessDecisionAction::deny;
  MemoryFairnessGrantToken grant;
  std::string dominant_scope_key;
  bool hard_max_exceeded = false;
  bool soft_max_exceeded = false;
  bool burst_used = false;
  bool burst_window_expired = false;
  bool foreground_protection_applied = false;
  bool starvation_prevention_applied = false;
  bool support_bundle_ready = false;
  std::vector<std::string> evidence;
  std::vector<MemoryFairnessMetricRow> metrics;
  std::vector<MemoryFairnessSupportBundleRow> support_bundle_rows;

  bool ok() const {
    return status.ok() && action == MemoryFairnessDecisionAction::grant &&
           grant.valid();
  }
};

struct MemoryFairnessScopeSnapshot {
  HierarchicalMemoryScopeKind kind = HierarchicalMemoryScopeKind::process;
  std::string scope_id;
  u64 guarantee_bytes = 0;
  u64 soft_max_bytes = 0;
  u64 hard_max_bytes = 0;
  u64 burst_bytes = 0;
  u64 burst_window_ms = 0;
  u64 burst_window_expires_at_ms = 0;
  u64 active_bytes = 0;
  u64 peak_bytes = 0;
  u64 active_grant_count = 0;
  u64 priority_weight_total = 0;
  u64 grant_count = 0;
  u64 spill_count = 0;
  u64 throttle_count = 0;
  u64 cancel_count = 0;
  u64 deny_count = 0;
  u64 burst_grant_count = 0;
  u64 burst_refusal_count = 0;
  u64 starvation_prevention_count = 0;
  u64 foreground_protection_count = 0;
  bool background_scope = false;
};

struct MemoryFairnessSnapshot {
  u64 active_bytes = 0;
  u64 peak_bytes = 0;
  u64 decision_count = 0;
  u64 grant_count = 0;
  u64 release_count = 0;
  u64 spill_count = 0;
  u64 throttle_count = 0;
  u64 cancel_count = 0;
  u64 deny_count = 0;
  u64 burst_grant_count = 0;
  u64 burst_refusal_count = 0;
  u64 starvation_prevention_count = 0;
  u64 foreground_protection_count = 0;
  std::vector<MemoryFairnessScopeSnapshot> scopes;
  std::vector<MemoryFairnessMetricRow> metrics;
  std::vector<MemoryFairnessSupportBundleRow> support_bundle_rows;
};

const char* MemoryFairnessWorkClassName(MemoryFairnessWorkClass work_class);
const char* MemoryFairnessDecisionActionName(MemoryFairnessDecisionAction action);

class MultiTenantMemoryFairnessScheduler {
 public:
  explicit MultiTenantMemoryFairnessScheduler(
      HierarchicalMemoryBudgetLedger* ledger);
  MultiTenantMemoryFairnessScheduler(
      const MultiTenantMemoryFairnessScheduler&) = delete;
  MultiTenantMemoryFairnessScheduler& operator=(
      const MultiTenantMemoryFairnessScheduler&) = delete;

  HierarchicalMemoryBudgetOperationResult SetScopePolicy(
      MemoryFairnessScopePolicy policy);
  MemoryFairnessDecision Admit(MemoryFairnessRequest request);
  HierarchicalMemoryBudgetOperationResult Release(
      MemoryFairnessGrantToken grant);
  MemoryFairnessSnapshot Snapshot() const;

 private:
  struct ScopeState {
    MemoryFairnessScopePolicy policy;
    u64 active_bytes = 0;
    u64 peak_bytes = 0;
    u64 active_grant_count = 0;
    u64 priority_weight_total = 0;
    u64 burst_window_expires_at_ms = 0;
    u64 grant_count = 0;
    u64 spill_count = 0;
    u64 throttle_count = 0;
    u64 cancel_count = 0;
    u64 deny_count = 0;
    u64 burst_grant_count = 0;
    u64 burst_refusal_count = 0;
    u64 starvation_prevention_count = 0;
    u64 foreground_protection_count = 0;
  };

  struct GrantRecord {
    MemoryFairnessGrantToken grant;
    std::vector<HierarchicalMemoryScopeRef> scope_chain;
    u64 priority_weight = 1;
    bool burst_used = false;
  };

  MemoryFairnessDecision FailDecision(
      const MemoryFairnessRequest& request,
      MemoryFairnessDecisionAction action,
      scratchbird::core::platform::StatusCode code,
      Severity severity,
      std::string diagnostic_code,
      std::string message_key,
      std::string reason,
      std::string dominant_scope_key,
      bool hard_max_exceeded,
      bool soft_max_exceeded,
      bool burst_window_expired,
      bool foreground_protection_applied,
      bool starvation_prevention_applied);

  MemoryFairnessDecision GrantDecision(
      const MemoryFairnessRequest& request,
      MemoryFairnessGrantToken grant,
      bool burst_used,
      bool starvation_prevention_applied);

  ScopeState& MutableScopeStateLocked(const HierarchicalMemoryScopeRef& scope);
  const ScopeState* FindScopeStateLocked(
      const HierarchicalMemoryScopeRef& scope) const;
  void RefreshBurstWindowLocked(ScopeState* state, u64 now_ms);
  bool ScopeCanUseBurstLocked(ScopeState* state,
                              u64 projected_bytes,
                              u64 now_ms,
                              bool* expired) const;
  u64 RequestPriorityWeight(const MemoryFairnessRequest& request) const;
  u64 RootHardLimitLocked(const MemoryFairnessRequest& request) const;
  u64 ProtectedForegroundHeadroomLocked(
      const MemoryFairnessRequest& request,
      u64 request_priority_weight) const;
  bool RequestContainsScopeLocked(const std::string& scope_key,
                                  const MemoryFairnessRequest& request) const;
  MemoryFairnessDecisionAction ReliefActionForRequest(
      const MemoryFairnessRequest& request) const;
  void CountDecisionLocked(const MemoryFairnessDecision& decision);
  void AttachEvidenceRows(MemoryFairnessDecision* decision,
                          const MemoryFairnessRequest& request,
                          const std::string& reason) const;

  HierarchicalMemoryBudgetLedger* ledger_ = nullptr;
  mutable std::mutex mutex_;
  std::map<std::string, ScopeState> scopes_;
  std::map<u64, GrantRecord> grants_;
  u64 next_grant_id_ = 1;
  u64 active_bytes_ = 0;
  u64 peak_bytes_ = 0;
  u64 decision_count_ = 0;
  u64 grant_count_ = 0;
  u64 release_count_ = 0;
  u64 spill_count_ = 0;
  u64 throttle_count_ = 0;
  u64 cancel_count_ = 0;
  u64 deny_count_ = 0;
  u64 burst_grant_count_ = 0;
  u64 burst_refusal_count_ = 0;
  u64 starvation_prevention_count_ = 0;
  u64 foreground_protection_count_ = 0;
};

}  // namespace scratchbird::core::memory
