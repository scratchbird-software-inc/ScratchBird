// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CEIC-027: plugin/UDR and native-library memory sandbox.
#include "foreign_memory_reservation.hpp"
#include "memory_class_policy_lease.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace scratchbird::core::memory {

using scratchbird::core::platform::StatusCode;

// CEIC-027_PLUGIN_NATIVE_MEMORY_SANDBOX
struct PluginNativeMemorySandboxMetricRow {
  std::string metric_name;
  std::string scope_kind;
  std::string scope_id;
  u64 value = 0;
  std::string unit = "count";
};

struct PluginNativeMemorySandboxSupportBundleRow {
  std::string key;
  std::string value;
  std::string redaction_class = "public";
  bool redacted = false;
};

struct PluginNativeMemorySandboxRequest {
  ForeignMemorySource source = ForeignMemorySource::plugin_udr;
  std::vector<HierarchicalMemoryScopeRef> scope_chain;
  MemoryBudgetLeaseRouteKind route_kind = MemoryBudgetLeaseRouteKind::local;
  std::string owner_id;
  std::string sandbox_id;
  std::string plugin_id;
  std::string plugin_allocator_abi = "scratchbird.plugin_allocator.v1";
  std::string plugin_memory_context_id;
  std::string udr_entrypoint;
  std::string library_path;
  std::string operation_id;
  std::string native_callsite;
  u64 estimated_bytes = 0;
  u64 observed_bytes = 0;
  u64 conservative_estimated_bytes = 0;
  u64 invocation_budget_bytes = 0;
  u64 result_buffer_bytes = 0;
  ForeignMemoryConfidence confidence = ForeignMemoryConfidence::estimated;
  ForeignMemoryReleaseEvent expected_release_event =
      ForeignMemoryReleaseEvent::explicit_release;
  ForeignMemoryOverLimitAction over_limit_action =
      ForeignMemoryOverLimitAction::deny;
  ForeignMemoryLinkageMode linkage_mode =
      ForeignMemoryLinkageMode::not_applicable;
  MemoryPressureState pressure_state = MemoryPressureState::normal;
  u64 now_ms = 0;
  u64 deadline_ms = 0;
  bool provider_available = true;
  bool live_provider_proof = false;
  bool live_route_claim = false;
  bool allow_conservative_estimate = false;
  bool production_raw_external_allocation_gate = true;
  bool raw_external_allocation = false;
  bool raw_plugin_allocation_explicitly_allowed = false;
  bool untracked_native_allocation = false;
  bool plugin_udr_sandboxed = true;
  bool result_buffer_owned_by_engine = true;
  bool plugin_cancellation_on_pressure = true;
  bool plugin_unload_cleanup_supported = true;
  bool support_bundle_view_enabled = true;
  bool require_plugin_udr_class_proof = true;
  bool require_release_evidence = true;
  HierarchicalMemoryBudgetProvenance provenance;
  ForeignMemoryAuthority authority;
  std::vector<std::string> evidence;
};

struct PluginNativeMemorySandboxToken {
  u64 sandbox_reservation_id = 0;
  MemoryBudgetLeaseToken class_lease;
  ForeignMemoryReservationToken foreign_reservation;

  bool valid() const {
    return sandbox_reservation_id != 0 && class_lease.valid() &&
           foreign_reservation.valid();
  }
};

struct PluginNativeMemorySandboxActiveSnapshot {
  PluginNativeMemorySandboxToken token;
  ForeignMemorySource source = ForeignMemorySource::unknown;
  std::string owner_id;
  std::string sandbox_id;
  std::string plugin_id;
  std::string plugin_allocator_abi;
  std::string plugin_memory_context_id;
  std::string udr_entrypoint;
  std::string operation_id;
  std::string native_callsite;
  u64 estimated_bytes = 0;
  u64 observed_bytes = 0;
  u64 invocation_budget_bytes = 0;
  u64 result_buffer_bytes = 0;
  ForeignMemoryConfidence confidence = ForeignMemoryConfidence::unknown;
  MemoryClassPressureAction pressure_action = MemoryClassPressureAction::deny;
  bool provider_available = false;
  bool live_provider_proof = false;
  bool conservative_estimate = false;
  bool result_buffer_owned_by_engine = false;
  bool plugin_cancellation_on_pressure = false;
  bool support_bundle_view_enabled = false;
  std::vector<std::string> evidence;
};

struct PluginNativeMemorySandboxSourceSnapshot {
  ForeignMemorySource source = ForeignMemorySource::unknown;
  u64 active_reservation_count = 0;
  u64 current_estimated_bytes = 0;
  u64 peak_estimated_bytes = 0;
  u64 reservation_count = 0;
  u64 release_count = 0;
  u64 owner_cleanup_count = 0;
  u64 refusal_count = 0;
};

struct PluginNativeMemorySandboxSnapshot {
  u64 active_reservation_count = 0;
  u64 current_estimated_bytes = 0;
  u64 peak_estimated_bytes = 0;
  u64 reservation_count = 0;
  u64 release_count = 0;
  u64 owner_cleanup_count = 0;
  u64 plugin_unload_cleanup_count = 0;
  u64 fail_closed_refusal_count = 0;
  u64 unsafe_provenance_refusal_count = 0;
  u64 raw_external_allocation_refusal_count = 0;
  u64 untracked_native_allocation_refusal_count = 0;
  u64 missing_identity_refusal_count = 0;
  u64 missing_class_lease_refusal_count = 0;
  u64 stale_class_lease_refusal_count = 0;
  u64 missing_release_evidence_refusal_count = 0;
  u64 cluster_refusal_count = 0;
  u64 absent_provider_fail_closed_count = 0;
  u64 conservative_estimate_count = 0;
  u64 raw_plugin_allocation_allowed_count = 0;
  std::vector<PluginNativeMemorySandboxSourceSnapshot> sources;
  std::vector<PluginNativeMemorySandboxActiveSnapshot> active_reservations;
  std::vector<PluginNativeMemorySandboxMetricRow> metrics;
  std::vector<PluginNativeMemorySandboxSupportBundleRow> support_bundle_rows;
};

struct PluginNativeMemorySandboxAcquireResult {
  Status status;
  bool fail_closed = false;
  std::unique_ptr<class PluginNativeMemorySandboxReservation> reservation;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  std::vector<PluginNativeMemorySandboxMetricRow> metrics;
  std::vector<PluginNativeMemorySandboxSupportBundleRow> support_bundle_rows;

  bool ok() const {
    return status.ok() && !fail_closed && reservation != nullptr;
  }
};

struct PluginNativeMemorySandboxReleaseResult {
  Status status;
  bool fail_closed = false;
  bool released = false;
  DiagnosticRecord diagnostic;
  PluginNativeMemorySandboxActiveSnapshot snapshot;
  std::vector<std::string> evidence;
  std::vector<PluginNativeMemorySandboxMetricRow> metrics;
  std::vector<PluginNativeMemorySandboxSupportBundleRow> support_bundle_rows;

  bool ok() const {
    return status.ok() && !fail_closed;
  }
};

struct PluginNativeMemorySandboxCleanupResult {
  Status status;
  bool fail_closed = false;
  DiagnosticRecord diagnostic;
  u64 cleaned_reservation_count = 0;
  u64 cleaned_estimated_bytes = 0;
  std::vector<std::string> evidence;
  std::vector<PluginNativeMemorySandboxMetricRow> metrics;
  std::vector<PluginNativeMemorySandboxSupportBundleRow> support_bundle_rows;

  bool ok() const {
    return status.ok() && !fail_closed;
  }
};

class PluginNativeMemorySandboxManager;

class PluginNativeMemorySandboxReservation {
 public:
  class ManagerOnlyTag {
   private:
    ManagerOnlyTag() = default;
    friend class PluginNativeMemorySandboxManager;
  };

  struct HandleState {
    std::atomic<bool> released{false};
  };

  PluginNativeMemorySandboxReservation(
      const PluginNativeMemorySandboxReservation&) = delete;
  PluginNativeMemorySandboxReservation& operator=(
      const PluginNativeMemorySandboxReservation&) = delete;
  PluginNativeMemorySandboxReservation(
      ManagerOnlyTag,
      PluginNativeMemorySandboxManager* owner,
      PluginNativeMemorySandboxToken token,
      std::shared_ptr<HandleState> state);
  ~PluginNativeMemorySandboxReservation();

  PluginNativeMemorySandboxReleaseResult Release(
      ForeignMemoryReleaseEvent event,
      std::vector<std::string> release_evidence);
  PluginNativeMemorySandboxReleaseResult Release(
      std::vector<std::string> release_evidence);
  PluginNativeMemorySandboxActiveSnapshot Snapshot() const;
  bool active() const;
  const PluginNativeMemorySandboxToken& token() const;

 private:
  PluginNativeMemorySandboxManager* owner_ = nullptr;
  PluginNativeMemorySandboxToken token_;
  std::shared_ptr<HandleState> state_;

  friend class PluginNativeMemorySandboxManager;
};

class PluginNativeMemorySandboxManager {
 public:
  PluginNativeMemorySandboxManager(
      HierarchicalMemoryBudgetLedger* reservation_ledger,
      ForeignMemoryReservationLedger* foreign_ledger,
      MemoryClassPolicyLeaseManager* class_lease_manager);
  PluginNativeMemorySandboxManager(const PluginNativeMemorySandboxManager&) =
      delete;
  PluginNativeMemorySandboxManager& operator=(
      const PluginNativeMemorySandboxManager&) = delete;

  PluginNativeMemorySandboxAcquireResult Acquire(
      PluginNativeMemorySandboxRequest request);
  PluginNativeMemorySandboxReleaseResult Release(
      PluginNativeMemorySandboxToken token,
      ForeignMemoryReleaseEvent event,
      std::vector<std::string> release_evidence);
  PluginNativeMemorySandboxCleanupResult CleanupOwner(std::string owner_id);
  PluginNativeMemorySandboxCleanupResult CleanupPluginUnload(
      std::string plugin_id);
  PluginNativeMemorySandboxSnapshot Snapshot() const;

 private:
  struct BucketAccounting {
    u64 active_reservation_count = 0;
    u64 current_estimated_bytes = 0;
    u64 peak_estimated_bytes = 0;
    u64 reservation_count = 0;
    u64 release_count = 0;
    u64 owner_cleanup_count = 0;
    u64 refusal_count = 0;
  };

  struct ReservationRecord {
    PluginNativeMemorySandboxRequest request;
    PluginNativeMemorySandboxToken token;
    MemoryClassPressureAction pressure_action = MemoryClassPressureAction::deny;
    bool conservative_estimate = false;
    std::unique_ptr<ForeignMemoryReservation> foreign_reservation;
    std::shared_ptr<PluginNativeMemorySandboxReservation::HandleState> state;
  };

  PluginNativeMemorySandboxAcquireResult RefuseAcquire(
      const PluginNativeMemorySandboxRequest& request,
      std::string diagnostic_code,
      std::string message_key,
      std::string reason,
      StatusCode code = StatusCode::memory_invalid_request,
      Severity severity = Severity::error);
  PluginNativeMemorySandboxReleaseResult RefuseRelease(
      PluginNativeMemorySandboxToken token,
      std::string diagnostic_code,
      std::string message_key,
      std::string reason,
      StatusCode code = StatusCode::memory_invalid_request);
  PluginNativeMemorySandboxActiveSnapshot SnapshotForRecordLocked(
      const ReservationRecord& record) const;
  void ApplyReservationLocked(const ReservationRecord& record);
  void ApplyReleaseLocked(const ReservationRecord& record,
                          ForeignMemoryReleaseEvent event);
  void AttachBaseEvidence(std::vector<std::string>* evidence,
                          const PluginNativeMemorySandboxRequest& request,
                          bool conservative_estimate) const;
  void AttachMetricAndSupportRows(
      std::vector<PluginNativeMemorySandboxMetricRow>* metrics,
      std::vector<PluginNativeMemorySandboxSupportBundleRow>* support_rows,
      const PluginNativeMemorySandboxRequest& request,
      std::string reason,
      MemoryClassPressureAction action) const;

  HierarchicalMemoryBudgetLedger* reservation_ledger_ = nullptr;
  ForeignMemoryReservationLedger* foreign_ledger_ = nullptr;
  MemoryClassPolicyLeaseManager* class_lease_manager_ = nullptr;
  mutable std::mutex mutex_;
  std::map<u64, ReservationRecord> records_;
  std::map<ForeignMemorySource, BucketAccounting> source_accounting_;
  u64 next_reservation_id_ = 1;
  u64 current_estimated_bytes_ = 0;
  u64 peak_estimated_bytes_ = 0;
  u64 reservation_count_ = 0;
  u64 release_count_ = 0;
  u64 owner_cleanup_count_ = 0;
  u64 plugin_unload_cleanup_count_ = 0;
  u64 fail_closed_refusal_count_ = 0;
  u64 unsafe_provenance_refusal_count_ = 0;
  u64 raw_external_allocation_refusal_count_ = 0;
  u64 untracked_native_allocation_refusal_count_ = 0;
  u64 missing_identity_refusal_count_ = 0;
  u64 missing_class_lease_refusal_count_ = 0;
  u64 stale_class_lease_refusal_count_ = 0;
  u64 missing_release_evidence_refusal_count_ = 0;
  u64 cluster_refusal_count_ = 0;
  u64 absent_provider_fail_closed_count_ = 0;
  u64 conservative_estimate_count_ = 0;
  u64 raw_plugin_allocation_allowed_count_ = 0;
};

const std::vector<ForeignMemorySource>& PluginNativeMemorySandboxModeledSources();
bool PluginNativeMemorySandboxCoversSource(ForeignMemorySource source);

PluginNativeMemorySandboxAcquireResult AcquirePluginNativeMemorySandbox(
    PluginNativeMemorySandboxManager* manager,
    PluginNativeMemorySandboxRequest request);

}  // namespace scratchbird::core::memory
