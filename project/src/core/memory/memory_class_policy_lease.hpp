// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CEIC-026: memory class policies and budget leases.
#include "hierarchical_memory_budget_ledger.hpp"
#include "memory_pressure_response.hpp"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace scratchbird::core::memory {

// CEIC-026_MEMORY_CLASS_POLICY_LEASES
enum class MemoryClassKind {
  critical_engine,
  query_scratch,
  clean_page_cache,
  dirty_page_cache,
  protected_material,
  result_driver_buffer,
  background_maintenance,
  plugin_udr,
  parser_handoff,
  diagnostics_support
};

enum class MemoryBudgetLeaseRouteKind {
  local,
  cluster
};

enum class MemoryClassPressureAction {
  grant,
  protect_critical,
  prefer_spill,
  shrink_clean_page_cache,
  flush_dirty_page_cache,
  refuse_protected_material,
  throttle,
  cancel,
  suspend_background,
  sandbox_plugin_udr,
  throttle_parser_handoff,
  degrade_diagnostics,
  external_provider_required,
  deny
};

enum class MemoryBudgetLeaseCleanupReason {
  cancel,
  expired,
  owner_disconnect
};

enum class MemoryBudgetLeaseRecoveryClassification {
  evidence_only_rebuilt,
  expired_cleanup_required,
  protected_material_quarantine,
  external_cluster_provider_required,
  unsafe_provenance_refused
};

struct MemoryClassPolicy {
  MemoryClassKind kind = MemoryClassKind::query_scratch;
  std::string class_name = "query_scratch";
  MemoryCategory category = MemoryCategory::executor_query_reserved;
  u64 max_lease_ms = 60 * 1000;
  u64 max_renewals = 2;
  bool renewable = true;
  bool spillable = false;
  bool throttleable = true;
  bool cancelable = false;
  bool protected_from_cancellation = false;
  bool requires_protected_material_route = false;
  bool requires_zero_on_release = false;
  bool excludes_protected_material_from_support = true;
  bool plugin_udr_class = false;
  bool parser_handoff_class = false;
  bool diagnostics_support_class = false;
  bool critical_engine_class = false;
  MemoryClassPressureAction normal_action = MemoryClassPressureAction::grant;
  MemoryClassPressureAction soft_pressure_action =
      MemoryClassPressureAction::throttle;
  MemoryClassPressureAction high_pressure_action =
      MemoryClassPressureAction::throttle;
  MemoryClassPressureAction emergency_pressure_action =
      MemoryClassPressureAction::cancel;
  MemoryClassPressureAction recovery_action =
      MemoryClassPressureAction::throttle;
  HierarchicalMemoryBudgetProvenance provenance;
};

struct MemoryBudgetLeaseRequest {
  std::vector<HierarchicalMemoryScopeRef> scope_chain;
  MemoryClassKind class_kind = MemoryClassKind::query_scratch;
  MemoryBudgetLeaseRouteKind route_kind = MemoryBudgetLeaseRouteKind::local;
  std::string owner_id;
  u64 requested_bytes = 0;
  u64 now_ms = 0;
  u64 deadline_ms = 0;
  MemoryPressureState pressure_state = MemoryPressureState::normal;
  bool protected_material_routed_through_protected_buffer = false;
  bool protected_material_redacted = true;
  bool protected_zero_on_release = true;
  bool plaintext_material_observed = false;
  bool plugin_udr_sandboxed = false;
  int priority = 0;
  u64 weight = 1;
  HierarchicalMemoryBudgetProvenance provenance;
};

struct MemoryBudgetLeaseToken {
  u64 lease_id = 0;
  u64 bytes = 0;
  u64 creation_sequence = 0;
  HierarchicalMemoryReservationToken reservation;

  bool valid() const {
    return lease_id != 0 && bytes != 0 && creation_sequence != 0 &&
           reservation.valid();
  }
};

struct MemoryBudgetLeaseRenewalRequest {
  MemoryBudgetLeaseToken lease;
  u64 now_ms = 0;
  u64 extend_by_ms = 0;
  u64 new_deadline_ms = 0;
  HierarchicalMemoryBudgetProvenance provenance;
};

struct MemoryBudgetLeaseMetricRow {
  std::string metric_name;
  std::string scope_kind;
  std::string scope_id;
  u64 value = 0;
  std::string unit = "count";
};

struct MemoryBudgetLeaseSupportBundleRow {
  std::string key;
  std::string value;
  std::string redaction_class = "public";
  bool redacted = false;
};

struct MemoryBudgetLeaseDecision {
  Status status;
  DiagnosticRecord diagnostic;
  MemoryClassKind class_kind = MemoryClassKind::query_scratch;
  std::string class_name;
  MemoryClassPressureAction pressure_action = MemoryClassPressureAction::deny;
  MemoryBudgetLeaseToken lease;
  bool fail_closed = false;
  bool support_bundle_ready = false;
  bool protected_material_excluded = true;
  bool cluster_external_provider_required = false;
  std::vector<std::string> evidence;
  std::vector<MemoryBudgetLeaseMetricRow> metrics;
  std::vector<MemoryBudgetLeaseSupportBundleRow> support_bundle_rows;

  bool ok() const {
    return status.ok() && lease.valid() &&
           pressure_action != MemoryClassPressureAction::deny &&
           pressure_action !=
               MemoryClassPressureAction::external_provider_required;
  }
};

struct MemoryBudgetLeaseRenewalResult {
  Status status;
  DiagnosticRecord diagnostic;
  MemoryBudgetLeaseToken lease;
  u64 deadline_ms = 0;
  u64 renewal_count = 0;
  bool fail_closed = false;
  std::vector<std::string> evidence;
  std::vector<MemoryBudgetLeaseMetricRow> metrics;
  std::vector<MemoryBudgetLeaseSupportBundleRow> support_bundle_rows;

  bool ok() const {
    return status.ok() && lease.valid();
  }
};

struct MemoryBudgetLeaseCleanupResult {
  Status status;
  DiagnosticRecord diagnostic;
  MemoryBudgetLeaseCleanupReason reason = MemoryBudgetLeaseCleanupReason::expired;
  u64 cleaned_lease_count = 0;
  u64 cleaned_bytes = 0;
  bool fail_closed = false;
  std::vector<std::string> evidence;
  std::vector<MemoryBudgetLeaseMetricRow> metrics;
  std::vector<MemoryBudgetLeaseSupportBundleRow> support_bundle_rows;

  bool ok() const {
    return status.ok();
  }
};

struct MemoryBudgetLeaseRecoveryInput {
  MemoryClassKind class_kind = MemoryClassKind::query_scratch;
  MemoryBudgetLeaseRouteKind route_kind = MemoryBudgetLeaseRouteKind::local;
  std::string owner_id;
  std::vector<HierarchicalMemoryScopeRef> scope_chain;
  u64 lease_id = 0;
  u64 bytes = 0;
  u64 creation_sequence = 0;
  u64 deadline_ms = 0;
  u64 now_ms = 0;
  u64 renewal_count = 0;
  u64 max_renewals = 0;
  bool protected_material_routed_through_protected_buffer = false;
  bool protected_material_redacted = true;
  bool protected_zero_on_release = true;
  bool plaintext_material_observed = false;
  HierarchicalMemoryBudgetProvenance provenance;
};

struct MemoryBudgetLeaseRecoveryResult {
  Status status;
  DiagnosticRecord diagnostic;
  MemoryBudgetLeaseRecoveryClassification classification =
      MemoryBudgetLeaseRecoveryClassification::evidence_only_rebuilt;
  MemoryClassPressureAction pressure_action = MemoryClassPressureAction::throttle;
  bool fail_closed = false;
  std::vector<std::string> evidence;
  std::vector<MemoryBudgetLeaseMetricRow> metrics;
  std::vector<MemoryBudgetLeaseSupportBundleRow> support_bundle_rows;

  bool ok() const {
    return status.ok();
  }
};

struct MemoryBudgetLeaseRecordSnapshot {
  MemoryBudgetLeaseToken lease;
  MemoryClassKind class_kind = MemoryClassKind::query_scratch;
  std::string class_name;
  std::string owner_id;
  u64 created_at_ms = 0;
  u64 deadline_ms = 0;
  u64 renewal_count = 0;
  u64 max_renewals = 0;
  MemoryClassPressureAction pressure_action = MemoryClassPressureAction::grant;
};

struct MemoryClassPolicySnapshot {
  MemoryClassKind kind = MemoryClassKind::query_scratch;
  std::string class_name;
  MemoryCategory category = MemoryCategory::unknown;
  u64 active_bytes = 0;
  u64 active_lease_count = 0;
  u64 created_lease_count = 0;
  u64 renewal_count = 0;
  u64 cancel_cleanup_count = 0;
  u64 expiry_cleanup_count = 0;
  u64 owner_cleanup_count = 0;
  u64 recovery_classification_count = 0;
  u64 max_lease_ms = 0;
  u64 max_renewals = 0;
  bool requires_protected_material_route = false;
  bool plugin_udr_class = false;
  bool parser_handoff_class = false;
  bool diagnostics_support_class = false;
  bool critical_engine_class = false;
  MemoryClassPressureAction soft_pressure_action =
      MemoryClassPressureAction::throttle;
  MemoryClassPressureAction high_pressure_action =
      MemoryClassPressureAction::throttle;
  MemoryClassPressureAction emergency_pressure_action =
      MemoryClassPressureAction::cancel;
};

struct MemoryBudgetLeaseSnapshot {
  u64 active_bytes = 0;
  u64 active_lease_count = 0;
  u64 created_lease_count = 0;
  u64 renewal_count = 0;
  u64 renewal_refusal_count = 0;
  u64 cancel_cleanup_count = 0;
  u64 expiry_cleanup_count = 0;
  u64 owner_cleanup_count = 0;
  u64 recovery_classification_count = 0;
  u64 unsafe_provenance_refusal_count = 0;
  u64 cluster_refusal_count = 0;
  u64 protected_material_refusal_count = 0;
  std::vector<MemoryBudgetLeaseRecordSnapshot> leases;
  std::vector<MemoryClassPolicySnapshot> classes;
  std::vector<MemoryBudgetLeaseMetricRow> metrics;
  std::vector<MemoryBudgetLeaseSupportBundleRow> support_bundle_rows;
};

const char* MemoryClassKindName(MemoryClassKind kind);
const char* MemoryBudgetLeaseRouteKindName(MemoryBudgetLeaseRouteKind route_kind);
const char* MemoryClassPressureActionName(MemoryClassPressureAction action);
const char* MemoryBudgetLeaseCleanupReasonName(MemoryBudgetLeaseCleanupReason reason);
const char* MemoryBudgetLeaseRecoveryClassificationName(
    MemoryBudgetLeaseRecoveryClassification classification);
MemoryClassPolicy DefaultMemoryClassPolicy(MemoryClassKind kind);

class MemoryClassPolicyLeaseManager {
 public:
  explicit MemoryClassPolicyLeaseManager(HierarchicalMemoryBudgetLedger* ledger);
  MemoryClassPolicyLeaseManager(const MemoryClassPolicyLeaseManager&) = delete;
  MemoryClassPolicyLeaseManager& operator=(const MemoryClassPolicyLeaseManager&) = delete;

  HierarchicalMemoryBudgetOperationResult SetClassPolicy(MemoryClassPolicy policy);
  MemoryBudgetLeaseDecision AcquireLease(MemoryBudgetLeaseRequest request);
  MemoryBudgetLeaseRenewalResult RenewLease(MemoryBudgetLeaseRenewalRequest request);
  MemoryBudgetLeaseCleanupResult CancelLease(MemoryBudgetLeaseToken lease);
  MemoryBudgetLeaseCleanupResult CleanupExpiredLeases(u64 now_ms);
  MemoryBudgetLeaseCleanupResult CleanupOwner(std::string owner_id);
  MemoryBudgetLeaseRecoveryResult ClassifyRecovery(
      MemoryBudgetLeaseRecoveryInput input);
  MemoryBudgetLeaseSnapshot Snapshot() const;

 private:
  struct ClassState {
    MemoryClassPolicy policy;
    u64 active_bytes = 0;
    u64 active_lease_count = 0;
    u64 created_lease_count = 0;
    u64 renewal_count = 0;
    u64 cancel_cleanup_count = 0;
    u64 expiry_cleanup_count = 0;
    u64 owner_cleanup_count = 0;
    u64 recovery_classification_count = 0;
  };

  struct LeaseRecord {
    MemoryBudgetLeaseToken lease;
    std::vector<HierarchicalMemoryScopeRef> scope_chain;
    MemoryClassKind class_kind = MemoryClassKind::query_scratch;
    std::string class_name;
    MemoryCategory category = MemoryCategory::unknown;
    std::string owner_id;
    u64 created_at_ms = 0;
    u64 deadline_ms = 0;
    u64 renewal_count = 0;
    u64 max_renewals = 0;
    MemoryClassPressureAction pressure_action =
        MemoryClassPressureAction::grant;
    bool protected_material = false;
  };

  MemoryClassPolicy PolicyForKindLocked(MemoryClassKind kind) const;
  MemoryBudgetLeaseDecision FailDecision(
      const MemoryBudgetLeaseRequest& request,
      MemoryClassPolicy policy,
      MemoryClassPressureAction action,
      scratchbird::core::platform::StatusCode code,
      Severity severity,
      std::string diagnostic_code,
      std::string message_key,
      std::string reason,
      bool cluster_external_provider_required);
  MemoryBudgetLeaseDecision GrantDecision(
      const MemoryBudgetLeaseRequest& request,
      const MemoryClassPolicy& policy,
      MemoryBudgetLeaseToken lease,
      MemoryClassPressureAction action);
  MemoryBudgetLeaseCleanupResult CleanupLeaseLocked(
      u64 lease_id,
      MemoryBudgetLeaseCleanupReason reason);
  void AttachDecisionEvidence(MemoryBudgetLeaseDecision* decision,
                              const MemoryBudgetLeaseRequest& request,
                              const MemoryClassPolicy& policy,
                              std::string reason) const;
  void AttachCleanupEvidence(MemoryBudgetLeaseCleanupResult* result,
                             MemoryBudgetLeaseCleanupReason reason,
                             std::string scope_id) const;
  void AttachRenewalEvidence(MemoryBudgetLeaseRenewalResult* result,
                             const LeaseRecord& record,
                             std::string reason) const;
  void AttachRecoveryEvidence(MemoryBudgetLeaseRecoveryResult* result,
                              const MemoryBudgetLeaseRecoveryInput& input,
                              const MemoryClassPolicy& policy,
                              std::string reason) const;

  HierarchicalMemoryBudgetLedger* ledger_ = nullptr;
  mutable std::mutex mutex_;
  std::map<MemoryClassKind, ClassState> classes_;
  std::map<u64, LeaseRecord> leases_;
  u64 next_lease_id_ = 1;
  u64 next_creation_sequence_ = 1;
  u64 active_bytes_ = 0;
  u64 created_lease_count_ = 0;
  u64 renewal_count_ = 0;
  u64 renewal_refusal_count_ = 0;
  u64 cancel_cleanup_count_ = 0;
  u64 expiry_cleanup_count_ = 0;
  u64 owner_cleanup_count_ = 0;
  u64 recovery_classification_count_ = 0;
  u64 unsafe_provenance_refusal_count_ = 0;
  u64 cluster_refusal_count_ = 0;
  u64 protected_material_refusal_count_ = 0;
};

}  // namespace scratchbird::core::memory
