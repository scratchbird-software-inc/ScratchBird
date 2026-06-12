// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CEIC-020: result cursor, plan-cache, and prepared-statement memory governance.
#include "hierarchical_memory_budget_ledger.hpp"

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::core::memory {

enum class ResultCursorPlanMemorySurface {
  streaming_result,
  cursor,
  result_frame,
  plan_cache_entry,
  prepared_statement,
  descriptor_snapshot
};

enum class ResultCursorPlanMemoryReleaseReason {
  close,
  cancel,
  timeout,
  disconnect,
  rollback,
  epoch_invalidation,
  eviction,
  shrink,
  pressure_forced_close,
  expired_lease,
  explicit_release
};

struct ResultCursorPlanMemoryAuthority {
  bool engine_mga_authoritative = true;
  bool transaction_inventory_authoritative = true;
  bool security_or_policy_checked = true;
  bool memory_evidence_only = true;
  bool parser_authority = false;
  bool reference_authority = false;
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool recovery_authority = false;
  bool authorization_authority = false;
  bool wal_authority = false;
  bool benchmark_authority = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool agent_action_authority = false;
  bool cluster_authority = false;
  bool debug_or_relaxed_path = false;
};

struct ResultCursorPlanMemoryEpochs {
  u64 catalog_epoch = 0;
  u64 security_epoch = 0;
  u64 redaction_epoch = 0;
  u64 policy_epoch = 0;
  u64 resource_epoch = 0;
  u64 descriptor_epoch = 0;
  u64 memory_policy_epoch = 0;
};

struct ResultCursorPlanMemoryScope {
  std::string database_id;
  std::string tenant_id;
  std::string user_id;
  std::string role_id;
  std::string session_id;
  std::string connection_id;
  std::string transaction_id;
  std::string statement_id;
  std::string query_id;
  std::string cursor_id;
  std::string plan_cache_key;
  std::string prepared_statement_id;
  std::string descriptor_snapshot_id;
};

struct ResultCursorPlanMemoryPolicy {
  u64 max_result_frame_bytes = 64u * 1024u;
  u64 max_outstanding_frames_per_connection = 8;
  u64 max_outstanding_frames_per_session = 8;
  u64 max_outstanding_frames_per_query = 4;
  u64 max_outstanding_frames_per_cursor = 2;
  u64 max_cursor_bytes_per_connection = 8u * 1024u * 1024u;
  u64 max_cursor_bytes_per_session = 8u * 1024u * 1024u;
  u64 max_cursor_bytes_per_query = 4u * 1024u * 1024u;
  u64 max_plan_cache_bytes_per_database = 32u * 1024u * 1024u;
  u64 max_plan_cache_bytes_per_tenant = 16u * 1024u * 1024u;
  u64 max_plan_cache_bytes_per_user = 8u * 1024u * 1024u;
  u64 max_plan_cache_bytes_per_session = 4u * 1024u * 1024u;
  u64 max_prepared_statement_bytes_per_database = 32u * 1024u * 1024u;
  u64 max_prepared_statement_bytes_per_tenant = 16u * 1024u * 1024u;
  u64 max_prepared_statement_bytes_per_user = 8u * 1024u * 1024u;
  u64 max_prepared_statement_bytes_per_session = 4u * 1024u * 1024u;
  u64 max_descriptor_snapshot_bytes_per_database = 16u * 1024u * 1024u;
  u64 max_descriptor_snapshot_bytes_per_session = 2u * 1024u * 1024u;
  bool require_epoch_evidence = true;
  bool require_ledger_reservation = true;
  bool cluster_surfaces_external_only = true;
};

struct ResultCursorPlanMemoryLeaseRequest {
  ResultCursorPlanMemorySurface surface =
      ResultCursorPlanMemorySurface::streaming_result;
  HierarchicalMemoryBudgetLedger* ledger = nullptr;
  ResultCursorPlanMemoryPolicy policy;
  ResultCursorPlanMemoryScope scope;
  ResultCursorPlanMemoryEpochs epochs;
  ResultCursorPlanMemoryAuthority authority;
  HierarchicalMemoryBudgetProvenance provenance;
  MemoryCategory category = MemoryCategory::executor_query_reserved;
  std::string memory_class = "ceic_020.result_cursor_plan";
  std::string owner_id;
  std::string route_label;
  u64 requested_bytes = 0;
  u64 lease_expires_at_ms = 0;
  bool spillable = false;
  bool cancelable = true;
  int priority = 0;
  u64 weight = 1;
  bool cluster_route_requested = false;
};

struct ResultCursorPlanMemoryLeaseRecord {
  std::string lease_id;
  ResultCursorPlanMemorySurface surface =
      ResultCursorPlanMemorySurface::streaming_result;
  ResultCursorPlanMemoryScope scope;
  ResultCursorPlanMemoryEpochs epochs;
  HierarchicalMemoryBudgetLedger* ledger = nullptr;
  HierarchicalMemoryReservationToken token;
  u64 reserved_bytes = 0;
  u64 acquired_sequence = 0;
  std::string memory_class;
  std::string owner_id;
  std::string route_label;
  u64 lease_expires_at_ms = 0;
  bool active = false;
  bool frame_lease = false;
};

struct ResultCursorPlanMemoryDecision {
  scratchbird::core::platform::Status status;
  scratchbird::core::platform::DiagnosticRecord diagnostic;
  bool fail_closed = true;
  bool accepted = false;
  bool backpressure_required = false;
  bool forced_close_required = false;
  std::string lease_id;
  ResultCursorPlanMemoryReleaseReason release_reason =
      ResultCursorPlanMemoryReleaseReason::explicit_release;
  u64 released_lease_count = 0;
  u64 released_bytes = 0;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok() && accepted && !fail_closed;
  }
};

struct ResultCursorPlanMemorySnapshot {
  u64 active_lease_count = 0;
  u64 active_bytes = 0;
  u64 result_frame_count = 0;
  u64 result_frame_bytes = 0;
  u64 cursor_count = 0;
  u64 cursor_bytes = 0;
  u64 plan_cache_entry_count = 0;
  u64 plan_cache_bytes = 0;
  u64 prepared_statement_count = 0;
  u64 prepared_statement_bytes = 0;
  u64 descriptor_snapshot_count = 0;
  u64 descriptor_snapshot_bytes = 0;
  u64 backpressure_count = 0;
  u64 forced_close_count = 0;
  u64 release_count = 0;
  u64 epoch_invalidation_count = 0;
  std::vector<ResultCursorPlanMemoryLeaseRecord> active_leases;
};

const char* ResultCursorPlanMemorySurfaceName(
    ResultCursorPlanMemorySurface surface);
const char* ResultCursorPlanMemoryReleaseReasonName(
    ResultCursorPlanMemoryReleaseReason reason);

class ResultCursorPlanMemoryGovernor {
 public:
  ResultCursorPlanMemoryGovernor() = default;
  ResultCursorPlanMemoryGovernor(const ResultCursorPlanMemoryGovernor&) = delete;
  ResultCursorPlanMemoryGovernor& operator=(
      const ResultCursorPlanMemoryGovernor&) = delete;

  ResultCursorPlanMemoryDecision Acquire(
      ResultCursorPlanMemoryLeaseRequest request);
  ResultCursorPlanMemoryDecision Release(
      const std::string& lease_id,
      ResultCursorPlanMemoryReleaseReason reason);
  ResultCursorPlanMemoryDecision ReleaseByCursor(
      const std::string& cursor_id,
      ResultCursorPlanMemoryReleaseReason reason);
  ResultCursorPlanMemoryDecision ReleaseResultFramesByCursor(
      const std::string& cursor_id,
      ResultCursorPlanMemoryReleaseReason reason);
  ResultCursorPlanMemoryDecision ReleaseBySession(
      const std::string& session_id,
      ResultCursorPlanMemoryReleaseReason reason);
  ResultCursorPlanMemoryDecision ReleaseByConnection(
      const std::string& connection_id,
      ResultCursorPlanMemoryReleaseReason reason);
  ResultCursorPlanMemoryDecision ReleaseByQuery(
      const std::string& query_id,
      ResultCursorPlanMemoryReleaseReason reason);
  ResultCursorPlanMemoryDecision ReleaseByTransaction(
      const std::string& transaction_id,
      ResultCursorPlanMemoryReleaseReason reason);
  ResultCursorPlanMemoryDecision InvalidateByEpoch(
      ResultCursorPlanMemoryEpochs current_epochs);
  ResultCursorPlanMemoryDecision ShrinkPlanCache(
      const std::string& database_id,
      u64 target_bytes);
  ResultCursorPlanMemoryDecision ForceCloseCursorUnderPressure(
      const std::string& cursor_id);
  ResultCursorPlanMemoryDecision CleanupExpiredLeases(u64 now_ms);
  ResultCursorPlanMemorySnapshot Snapshot() const;

 private:
  struct Counter {
    u64 bytes = 0;
    u64 count = 0;
    u64 frames = 0;
  };

  void AddCountersLocked(const ResultCursorPlanMemoryLeaseRecord& record);
  void RemoveCountersLocked(const ResultCursorPlanMemoryLeaseRecord& record);
  Counter CounterForLocked(ResultCursorPlanMemorySurface surface,
                           const std::string& dimension,
                           const std::string& value) const;
  std::vector<HierarchicalMemoryScopeRef> BuildScopeChain(
      const ResultCursorPlanMemoryLeaseRequest& request) const;

  mutable std::mutex mutex_;
  std::map<std::string, ResultCursorPlanMemoryLeaseRecord> leases_;
  std::map<std::string, Counter> counters_;
  u64 next_sequence_ = 1;
  u64 backpressure_count_ = 0;
  u64 forced_close_count_ = 0;
  u64 release_count_ = 0;
  u64 epoch_invalidation_count_ = 0;
};

}  // namespace scratchbird::core::memory
