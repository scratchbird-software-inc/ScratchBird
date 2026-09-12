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

using ResultCursorPlanMemoryUuid = scratchbird::core::platform::Uuid;
static_assert(sizeof(ResultCursorPlanMemoryUuid) == 16);

struct ResultCursorPlanMemoryScope {
  ResultCursorPlanMemoryUuid process_id;
  ResultCursorPlanMemoryUuid plan_cache_entry_id;
  ResultCursorPlanMemoryUuid database_id;
  ResultCursorPlanMemoryUuid tenant_id;
  ResultCursorPlanMemoryUuid user_id;
  ResultCursorPlanMemoryUuid role_id;
  ResultCursorPlanMemoryUuid session_id;
  ResultCursorPlanMemoryUuid connection_id;
  ResultCursorPlanMemoryUuid transaction_id;
  ResultCursorPlanMemoryUuid statement_id;
  ResultCursorPlanMemoryUuid query_id;
  ResultCursorPlanMemoryUuid cursor_id;
  // Content metadata only; ownership and limits use plan_cache_entry_id.
  std::string plan_cache_key;
  ResultCursorPlanMemoryUuid prepared_statement_id;
  ResultCursorPlanMemoryUuid descriptor_snapshot_id;
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
  // V1 requires both. Requests attempting to disable either are refused;
  // these policy fields cannot authorize unversioned or unbacked leases.
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
  ResultCursorPlanMemoryUuid owner_id;
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
  ResultCursorPlanMemoryUuid lease_id;
  ResultCursorPlanMemorySurface surface =
      ResultCursorPlanMemorySurface::streaming_result;
  ResultCursorPlanMemoryScope scope;
  ResultCursorPlanMemoryEpochs epochs;
  HierarchicalMemoryBudgetLedger* ledger = nullptr;
  HierarchicalMemoryReservationToken token;
  u64 reserved_bytes = 0;
  u64 acquired_sequence = 0;
  std::string memory_class;
  ResultCursorPlanMemoryUuid owner_id;
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
  ResultCursorPlanMemoryUuid lease_id;
  // Appended only after the corresponding real lease is successfully retired.
  std::vector<ResultCursorPlanMemoryUuid> released_lease_ids;
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
  u64 quota_counter_count = 0;
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
  // Every supplied ledger must outlive this governor and its live leases.
  ~ResultCursorPlanMemoryGovernor();
  ResultCursorPlanMemoryGovernor(const ResultCursorPlanMemoryGovernor&) = delete;
  ResultCursorPlanMemoryGovernor& operator=(
      const ResultCursorPlanMemoryGovernor&) = delete;

  ResultCursorPlanMemoryDecision Acquire(
      ResultCursorPlanMemoryLeaseRequest request);
  ResultCursorPlanMemoryDecision Release(
      const ResultCursorPlanMemoryUuid& lease_id,
      ResultCursorPlanMemoryReleaseReason reason);
  // Actual owner teardown without diagnostic/evidence allocation. A stale
  // handle is a typed refusal, not proof that this invocation released it.
  Status ReleaseNoAlloc(const ResultCursorPlanMemoryUuid& lease_id);
  ResultCursorPlanMemoryDecision ReleaseByCursor(
      const ResultCursorPlanMemoryUuid& cursor_id,
      ResultCursorPlanMemoryReleaseReason reason);
  ResultCursorPlanMemoryDecision ReleaseResultFramesByCursor(
      const ResultCursorPlanMemoryUuid& cursor_id,
      ResultCursorPlanMemoryReleaseReason reason);
  ResultCursorPlanMemoryDecision ReleaseBySession(
      const ResultCursorPlanMemoryUuid& session_id,
      ResultCursorPlanMemoryReleaseReason reason);
  ResultCursorPlanMemoryDecision ReleaseByConnection(
      const ResultCursorPlanMemoryUuid& connection_id,
      ResultCursorPlanMemoryReleaseReason reason);
  ResultCursorPlanMemoryDecision ReleaseByQuery(
      const ResultCursorPlanMemoryUuid& query_id,
      ResultCursorPlanMemoryReleaseReason reason);
  ResultCursorPlanMemoryDecision ReleaseByTransaction(
      const ResultCursorPlanMemoryUuid& transaction_id,
      ResultCursorPlanMemoryReleaseReason reason);
  ResultCursorPlanMemoryDecision InvalidateByEpoch(
      ResultCursorPlanMemoryEpochs current_epochs);
  ResultCursorPlanMemoryDecision ShrinkPlanCache(
      const ResultCursorPlanMemoryUuid& database_id,
      u64 target_bytes);
  ResultCursorPlanMemoryDecision ForceCloseCursorUnderPressure(
      const ResultCursorPlanMemoryUuid& cursor_id);
  ResultCursorPlanMemoryDecision CleanupExpiredLeases(u64 now_ms);
  ResultCursorPlanMemorySnapshot Snapshot() const;

 private:
  struct Counter {
    u64 bytes = 0;
    u64 count = 0;
    u64 frames = 0;
  };

  struct CounterKey {
    ResultCursorPlanMemorySurface surface;
    std::string dimension;  // Internal fixed dimension tag, never an owner.
    ResultCursorPlanMemoryUuid owner;
    bool operator<(const CounterKey& other) const {
      if (surface != other.surface) return surface < other.surface;
      if (dimension != other.dimension) return dimension < other.dimension;
      return owner < other.owner;
    }
  };
  struct OwnedLease : ResultCursorPlanMemoryLeaseRecord {
    std::vector<CounterKey> counter_keys;
  };
  using LeaseMap = std::map<ResultCursorPlanMemoryUuid, OwnedLease>;
  bool PrepareCountersLocked(OwnedLease& record);
  void AddCountersLocked(const OwnedLease& record);
  void RemoveCountersLocked(const OwnedLease& record);
  ResultCursorPlanMemoryDecision DrainLocked(
      const std::vector<LeaseMap::iterator>& selected,
      ResultCursorPlanMemoryDecision aggregate);
  Counter CounterForLocked(ResultCursorPlanMemorySurface surface,
                           const std::string& dimension,
                           const ResultCursorPlanMemoryUuid& value) const;
  std::vector<HierarchicalMemoryScopeRef> BuildScopeChain(
      const ResultCursorPlanMemoryLeaseRequest& request) const;

  mutable std::mutex mutex_;
  LeaseMap leases_;
  std::map<CounterKey, Counter> counters_;
  u64 next_sequence_ = 1;
  u64 active_bytes_ = 0;
  u64 backpressure_count_ = 0;
  u64 forced_close_count_ = 0;
  u64 release_count_ = 0;
  u64 epoch_invalidation_count_ = 0;
};

}  // namespace scratchbird::core::memory
