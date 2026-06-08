// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: SB_RESOURCE_GOVERNANCE_ADMISSION_ODF_106
// Engine-owned resource governance for accelerator, cache, bulk, and parallel
// admission paths. The route accepts only runtime policy descriptors, requires
// bounded quotas for every resource dimension, and never delegates MGA,
// security, finality, visibility, recovery, parser, or donor authority.

#include "agent_runtime.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace scratchbird::core::agents {

enum class ResourceGovernanceFamily : std::uint32_t {
  kUnknown = 0,
  kQueryMemoryArena,
  kAdaptiveTuningKnob,
  kAsyncPageIo,
  kParallelPhysicalPipeline,
  kPreparedNativeSpecialization,
  kScoringKernelAccelerator,
  kAcceleratorProviderCache,
  kBulkCopyLane,
  kOptimizedCompression,
  kOptimizedCache,
  kOptimizedVectorMaintenance,
  kOptimizedNoSqlProvider,
  kStreamingCursor,
  kBackgroundJob,
};

enum class ResourceGovernanceAction : std::uint32_t {
  kAdmit = 0,
  kSlowdownDegrade,
  kExactScalarFallback,
  kCancel,
  kFailClosed,
};

enum class ResourceGovernanceDescriptorSource : std::uint32_t {
  kUnknown = 0,
  kRuntimePolicy,
  kServerRuntimeApi,
  kAgentRuntime,
  kExecution_PlanEvidence,
};

struct ResourceGovernanceQuotaVector {
  std::int64_t memory_bytes = 0;
  std::int64_t device_memory_bytes = 0;
  std::int64_t pinned_memory_bytes = 0;
  std::int64_t io_bytes = 0;
  std::int64_t io_ops = 0;
  std::int64_t worker_threads = 0;
  std::int64_t backlog_items = 0;
  std::int64_t candidate_rows = 0;
  std::int64_t cache_entries = 0;
  std::int64_t batch_rows = 0;
  std::int64_t fragments = 0;
  std::int64_t lanes = 0;
  std::int64_t time_budget_microseconds = 0;
};

struct ResourceGovernanceQuotaDescriptor {
  std::string descriptor_id;
  ResourceGovernanceFamily family = ResourceGovernanceFamily::kUnknown;
  ResourceGovernanceDescriptorSource source =
      ResourceGovernanceDescriptorSource::kUnknown;
  std::string source_path_or_label;
  std::uint64_t descriptor_generation = 0;
  std::uint64_t expected_generation = 0;
  ResourceGovernanceQuotaVector limits;
  ResourceGovernanceAction over_limit_action =
      ResourceGovernanceAction::kFailClosed;
  bool benchmark_clean = false;
  bool runtime_dependency_present = false;
  bool engine_mga_authoritative = true;
  bool security_authoritative = true;
  bool parser_or_donor_authority = false;
  bool provider_transaction_finality_authority = false;
  bool provider_visibility_authority = false;
  bool provider_recovery_authority = false;
  bool wal_recovery_authority = false;
  bool corrupt = false;
};

struct ResourceGovernanceAdmissionRequest {
  std::string operation_id;
  ResourceGovernanceQuotaDescriptor descriptor;
  ResourceGovernanceFamily expected_family = ResourceGovernanceFamily::kUnknown;
  ResourceGovernanceQuotaVector requested;
  bool cancellation_requested = false;
  bool stale_runtime_epoch = false;
  bool require_exact_scalar_fallback_available = false;
  bool exact_scalar_fallback_available = false;
};

struct ResourceGovernanceAdmissionResult {
  bool ok = false;
  bool fail_closed = false;
  bool reservation_created = false;
  ResourceGovernanceAction action = ResourceGovernanceAction::kFailClosed;
  AgentRuntimeStatus status;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  std::string exceeded_quota;
  std::vector<std::string> evidence;
};

// MMCH_RESOURCE_RESERVATION_LIFECYCLE
enum class ResourceGovernanceReservationReleaseReason : std::uint32_t {
  kRelease = 0,
  kCancel,
  kTimeout,
  kDisconnect,
  kShutdown,
};

struct ResourceGovernanceReservationToken {
  std::string token_id;
  std::string operation_id;
  std::string descriptor_id;
  ResourceGovernanceFamily family = ResourceGovernanceFamily::kUnknown;
  ResourceGovernanceQuotaVector reserved;
  std::string owner_scope;
  std::uint64_t created_sequence = 0;
  std::uint64_t lease_deadline_tick = 0;
};

struct ResourceGovernanceReservationAcquireRequest {
  ResourceGovernanceAdmissionRequest admission;
  std::string owner_scope;
  std::uint64_t lease_deadline_tick = 0;
};

struct ResourceGovernanceReservationSnapshot {
  std::string ledger_id;
  std::uint64_t active_reservation_count = 0;
  std::uint64_t created_reservation_count = 0;
  std::uint64_t released_reservation_count = 0;
  ResourceGovernanceQuotaVector active;
};

struct ResourceGovernanceReservationAcquireResult {
  AgentRuntimeStatus status;
  ResourceGovernanceAdmissionResult admission;
  ResourceGovernanceReservationToken reservation;
  ResourceGovernanceReservationSnapshot snapshot;
  bool ok = false;
  bool fail_closed = false;
  bool reservation_created = false;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  std::string exceeded_quota;
  std::vector<std::string> evidence;
};

struct ResourceGovernanceReservationReleaseResult {
  AgentRuntimeStatus status;
  ResourceGovernanceReservationToken reservation;
  ResourceGovernanceReservationSnapshot snapshot;
  ResourceGovernanceReservationReleaseReason reason =
      ResourceGovernanceReservationReleaseReason::kRelease;
  bool ok = false;
  bool released = false;
  bool not_found = false;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  std::vector<std::string> evidence;
};

struct ResourceGovernanceReservationCleanupResult {
  AgentRuntimeStatus status;
  ResourceGovernanceReservationSnapshot snapshot;
  ResourceGovernanceReservationReleaseReason reason =
      ResourceGovernanceReservationReleaseReason::kRelease;
  bool ok = false;
  std::uint64_t released_count = 0;
  std::string owner_scope;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

const char* ResourceGovernanceFamilyName(ResourceGovernanceFamily family);
const char* ResourceGovernanceActionName(ResourceGovernanceAction action);
const char* ResourceGovernanceDescriptorSourceName(
    ResourceGovernanceDescriptorSource source);
const char* ResourceGovernanceReservationReleaseReasonName(
    ResourceGovernanceReservationReleaseReason reason);

ResourceGovernanceAdmissionResult AdmitResourceGovernance(
    const ResourceGovernanceAdmissionRequest& request);

std::string SerializeResourceGovernanceEvidence(
    const ResourceGovernanceAdmissionResult& result);

class ResourceGovernanceReservationLedger {
 public:
  explicit ResourceGovernanceReservationLedger(std::string ledger_id);

  ResourceGovernanceReservationAcquireResult Acquire(
      ResourceGovernanceReservationAcquireRequest request);
  ResourceGovernanceReservationReleaseResult Release(
      const std::string& token_id,
      ResourceGovernanceReservationReleaseReason reason =
          ResourceGovernanceReservationReleaseReason::kRelease);
  ResourceGovernanceReservationCleanupResult ReleaseOwnerReservations(
      const std::string& owner_scope,
      ResourceGovernanceReservationReleaseReason reason =
          ResourceGovernanceReservationReleaseReason::kDisconnect);
  ResourceGovernanceReservationCleanupResult ExpireReservations(
      std::uint64_t now_tick);
  ResourceGovernanceReservationSnapshot Snapshot() const;

 private:
  struct ActiveReservation {
    ResourceGovernanceReservationToken token;
  };

  ResourceGovernanceReservationSnapshot SnapshotLocked() const;

  std::string ledger_id_;
  mutable std::mutex mutex_;
  std::map<std::string, ActiveReservation> active_;
  ResourceGovernanceQuotaVector active_usage_;
  std::uint64_t next_sequence_ = 0;
  std::uint64_t released_count_ = 0;
};

// MMCH_HIERARCHICAL_MEMORY_BUDGETS
enum class HierarchicalMemoryBudgetScopeKind : std::uint32_t {
  kUnknown = 0,
  kDatabase,
  kTenant,
  kUser,
  kRole,
  kSession,
  kTransaction,
  kStatement,
  kQuery,
  kOperator,
  kBackground,
};

struct HierarchicalMemoryBudgetScope {
  std::string scope_id;
  std::string parent_scope_id;
  HierarchicalMemoryBudgetScopeKind kind =
      HierarchicalMemoryBudgetScopeKind::kUnknown;
  std::uint64_t limit_bytes = 0;
  bool active = true;
};

struct HierarchicalMemoryBudgetScopeSnapshot {
  std::string scope_id;
  std::string parent_scope_id;
  HierarchicalMemoryBudgetScopeKind kind =
      HierarchicalMemoryBudgetScopeKind::kUnknown;
  std::uint64_t limit_bytes = 0;
  std::uint64_t current_bytes = 0;
  std::uint64_t peak_bytes = 0;
  std::uint64_t active_reservation_count = 0;
};

struct HierarchicalMemoryBudgetReservationToken {
  std::string token_id;
  std::string operation_id;
  std::string owner_scope;
  std::string leaf_scope_id;
  std::uint64_t bytes = 0;
  std::uint64_t created_sequence = 0;
  std::vector<std::string> debited_scope_chain;
};

struct HierarchicalMemoryBudgetReserveRequest {
  std::string operation_id;
  std::string owner_scope;
  std::string leaf_scope_id;
  std::uint64_t bytes = 0;
};

struct HierarchicalMemoryBudgetReserveResult {
  AgentRuntimeStatus status;
  bool ok = false;
  bool reservation_created = false;
  bool fail_closed = false;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  std::string refused_scope_id;
  HierarchicalMemoryBudgetReservationToken reservation;
  std::vector<HierarchicalMemoryBudgetScopeSnapshot> snapshots;
  std::vector<std::string> evidence;
};

struct HierarchicalMemoryBudgetReleaseResult {
  AgentRuntimeStatus status;
  bool ok = false;
  bool released = false;
  bool not_found = false;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  HierarchicalMemoryBudgetReservationToken reservation;
  std::vector<HierarchicalMemoryBudgetScopeSnapshot> snapshots;
  std::vector<std::string> evidence;
};

const char* HierarchicalMemoryBudgetScopeKindName(
    HierarchicalMemoryBudgetScopeKind kind);

class HierarchicalMemoryBudgetLedger {
 public:
  explicit HierarchicalMemoryBudgetLedger(std::string ledger_id);

  AgentRuntimeStatus RegisterScope(HierarchicalMemoryBudgetScope scope);
  HierarchicalMemoryBudgetReserveResult Reserve(
      HierarchicalMemoryBudgetReserveRequest request);
  HierarchicalMemoryBudgetReleaseResult Release(const std::string& token_id);
  HierarchicalMemoryBudgetReleaseResult ReleaseOwnerReservations(
      const std::string& owner_scope);
  std::vector<HierarchicalMemoryBudgetScopeSnapshot> Snapshot() const;

 private:
  struct ScopeState {
    HierarchicalMemoryBudgetScope scope;
    std::uint64_t current_bytes = 0;
    std::uint64_t peak_bytes = 0;
    std::uint64_t active_reservation_count = 0;
  };

  struct ActiveHierarchicalReservation {
    HierarchicalMemoryBudgetReservationToken token;
  };

  std::vector<std::string> ScopeChainLocked(const std::string& leaf_scope_id,
                                            std::string* diagnostic) const;
  std::vector<HierarchicalMemoryBudgetScopeSnapshot> SnapshotLocked() const;

  std::string ledger_id_;
  mutable std::mutex mutex_;
  std::map<std::string, ScopeState> scopes_;
  std::map<std::string, ActiveHierarchicalReservation> active_;
  std::uint64_t next_sequence_ = 0;
};

}  // namespace scratchbird::core::agents
