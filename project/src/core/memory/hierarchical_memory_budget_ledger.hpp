// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CEIC-011: reservation-first hierarchical memory budget governance.
#include "sharded_memory_accounting_ledger.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace scratchbird::core::memory {

enum class HierarchicalMemoryScopeKind {
  process,
  database,
  tenant,
  user,
  role,
  session,
  transaction,
  statement,
  query,
  operator_scope,
  page_cache,
  background,
  plugin,
  connection,
  cursor,
  plan_cache_entry,
  prepared_statement,
  descriptor_snapshot
};

enum class HierarchicalMemoryReservationState {
  reserved,
  active
};

enum class HierarchicalMemoryReservationRecommendation {
  granted,
  deny,
  spill,
  cancel,
  degrade
};

enum class HierarchicalMemoryBudgetProvenanceSource {
  unknown,
  runtime_policy,
  server_runtime_api,
  agent_runtime,
  execution_plan_evidence,
  test_fixture,
  synthetic_evidence
};

struct HierarchicalMemoryBudgetProvenance {
  HierarchicalMemoryBudgetProvenanceSource source =
      HierarchicalMemoryBudgetProvenanceSource::unknown;
  std::string source_label;
  bool engine_mga_authoritative = true;
  bool memory_evidence_only = true;
  bool parser_authority = false;
  bool reference_authority = false;
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool recovery_authority = false;
  bool authorization_authority = false;
  bool benchmark_authority = false;
  bool support_bundle_authority = false;
  bool cluster_authority = false;
  bool debug_or_relaxed_path = false;
};

struct HierarchicalMemoryScopeRef {
  HierarchicalMemoryScopeKind kind = HierarchicalMemoryScopeKind::process;
  std::string scope_id;
  // Exactly one identity alternative. Binary identities never become strings.
  MemoryBinaryUuid binary_scope_uuid{};
};

MemoryBinaryScopeKind HierarchicalMemoryBinaryScopeKind(HierarchicalMemoryScopeKind kind);

struct HierarchicalMemoryBudget {
  HierarchicalMemoryScopeRef scope;
  u64 hard_limit_bytes = 0;
  u64 soft_limit_bytes = 0;
  HierarchicalMemoryBudgetProvenance provenance;
};

struct HierarchicalMemoryReservationRequest {
  std::vector<HierarchicalMemoryScopeRef> scope_chain;
  MemoryCategory category = MemoryCategory::unknown;
  std::string memory_class = "unclassified";
  u64 requested_bytes = 0;
  std::string owner_id;
  bool spillable = false;
  bool cancelable = false;
  int priority = 0;
  u64 weight = 1;
  u64 lease_expires_at_ms = 0;
  HierarchicalMemoryBudgetProvenance provenance;
  MemoryBinaryUuid binary_owner_uuid{};
};

struct HierarchicalMemoryReservationToken {
  u64 token_id = 0;
  u64 bytes = 0;

  bool valid() const {
    return token_id != 0 && bytes != 0;
  }
};

struct HierarchicalMemoryBudgetOperationResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool retained = false;
  bool newly_revoked = false;
  u64 retained_bytes = 0;

  bool ok() const {
    return status.ok();
  }
};

struct HierarchicalMemoryReservationResult {
  Status status;
  DiagnosticRecord diagnostic;
  HierarchicalMemoryReservationRecommendation recommendation =
      HierarchicalMemoryReservationRecommendation::deny;
  HierarchicalMemoryReservationToken token;

  bool ok() const {
    return status.ok() && token.valid() &&
           recommendation == HierarchicalMemoryReservationRecommendation::granted;
  }
};

struct HierarchicalMemoryCleanupResult {
  Status status;
  DiagnosticRecord diagnostic;
  u64 cleaned_reservation_count = 0;
  u64 cleaned_bytes = 0;
  u64 revoked_reservation_count = 0;
  u64 retained_bytes = 0;

  bool ok() const {
    return status.ok();
  }
};

struct HierarchicalMemoryScopeSnapshot {
  HierarchicalMemoryScopeKind kind = HierarchicalMemoryScopeKind::process;
  std::string scope_id;
  MemoryBinaryUuid binary_scope_uuid{};
  u64 hard_limit_bytes = 0;
  u64 soft_limit_bytes = 0;
  u64 reserved_bytes = 0;
  u64 active_bytes = 0;
  u64 current_bytes = 0;
  u64 peak_bytes = 0;
  u64 reservation_count = 0;
  u64 commit_count = 0;
  u64 release_count = 0;
  u64 cancel_cleanup_count = 0;
  u64 owner_cleanup_count = 0;
  u64 lease_expiry_cleanup_count = 0;
  u64 active_reservation_count = 0;
  u64 active_allocation_count = 0;
  u64 priority_weight_total = 0;
};

struct HierarchicalMemoryClassSnapshot {
  MemoryCategory category = MemoryCategory::unknown;
  std::string memory_class;
  u64 reserved_bytes = 0;
  u64 active_bytes = 0;
  u64 current_bytes = 0;
  u64 peak_bytes = 0;
  u64 reservation_count = 0;
  u64 commit_count = 0;
  u64 release_count = 0;
};

struct HierarchicalMemoryBudgetSnapshot {
  u64 shard_count = 0;
  u64 token_shard_count = 0;
  u64 reserved_bytes = 0;
  u64 active_bytes = 0;
  u64 current_bytes = 0;
  u64 peak_bytes = 0;
  u64 reservation_count = 0;
  u64 commit_count = 0;
  u64 release_count = 0;
  u64 hard_limit_refusal_count = 0;
  u64 soft_limit_recommendation_count = 0;
  u64 failed_commit_count = 0;
  u64 failed_release_count = 0;
  u64 cancel_cleanup_count = 0;
  u64 owner_cleanup_count = 0;
  u64 lease_expiry_cleanup_count = 0;
  u64 active_reservation_count = 0;
  u64 active_allocation_count = 0;
  u64 pending_revocation_count = 0;
  u64 retained_revoked_bytes = 0;
  std::vector<HierarchicalMemoryScopeSnapshot> scopes;
  std::vector<HierarchicalMemoryClassSnapshot> classes;
};

const char* HierarchicalMemoryScopeKindName(HierarchicalMemoryScopeKind kind);
const char* HierarchicalMemoryReservationRecommendationName(
    HierarchicalMemoryReservationRecommendation recommendation);
const char* HierarchicalMemoryBudgetProvenanceSourceName(
    HierarchicalMemoryBudgetProvenanceSource source);

class HierarchicalMemoryBudgetLedger;

// A single owning context retains the real reservation. Revocation prevents
// new uses but never erases its charge or asynchronously frees live payloads.
// The ledger must outlive this lease; payloads must quiesce before Reset.
class HierarchicalMemoryReservationLease {
  struct State;
 public:
  class UseGuard {
   public:
    UseGuard() = default;
    UseGuard(UseGuard&&) noexcept = default;
    UseGuard& operator=(UseGuard&&) = delete;
    bool live() const;
   private:
    explicit UseGuard(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
    std::unique_lock<std::mutex> lock_;
    friend class HierarchicalMemoryReservationLease;
  };
  HierarchicalMemoryReservationLease() = default;
  HierarchicalMemoryReservationLease(const HierarchicalMemoryReservationLease&) = delete;
  HierarchicalMemoryReservationLease& operator=(const HierarchicalMemoryReservationLease&) = delete;
  HierarchicalMemoryReservationLease(HierarchicalMemoryReservationLease&& other) noexcept;
  HierarchicalMemoryReservationLease& operator=(HierarchicalMemoryReservationLease&& other) noexcept;
  ~HierarchicalMemoryReservationLease();
  bool valid() const;
  bool live() const;
  UseGuard Use() const;
  Status Reset();
 private:
  HierarchicalMemoryBudgetLedger* ledger_ = nullptr;
  mutable std::mutex mutex_;
  HierarchicalMemoryReservationToken token_;
  std::shared_ptr<State> state_;
  friend class HierarchicalMemoryBudgetLedger;
};

struct HierarchicalMemoryRetainResult {
  Status status;
  HierarchicalMemoryReservationLease lease;
  bool ok() const { return status.ok() && lease.valid(); }
};

class HierarchicalMemoryBudgetLedger {
 public:
  explicit HierarchicalMemoryBudgetLedger(usize scope_shard_count = 64,
                                          usize token_shard_count = 64);
  HierarchicalMemoryBudgetLedger(const HierarchicalMemoryBudgetLedger&) = delete;
  HierarchicalMemoryBudgetLedger& operator=(const HierarchicalMemoryBudgetLedger&) = delete;
  ~HierarchicalMemoryBudgetLedger();

  usize scope_shard_count() const;
  usize token_shard_count() const;

  // Immediate admission-policy update with explicit reject_change semantics:
  // a nonzero hard limit cannot fall below reserved + active owned bytes,
  // including revoked grants retained by payload owners. Existing tokens are
  // never canceled or replaced. Soft limits govern subsequent reservations.
  // Failure preserves the prior policy and scope/accounting publication.
  HierarchicalMemoryBudgetOperationResult SetBudget(HierarchicalMemoryBudget budget);
  HierarchicalMemoryReservationResult Reserve(HierarchicalMemoryReservationRequest request);
  HierarchicalMemoryBudgetOperationResult Commit(HierarchicalMemoryReservationToken token);
  HierarchicalMemoryBudgetOperationResult Release(HierarchicalMemoryReservationToken token);
  // Teardown/RAII path: invalid or competing-cleanup handles also need no allocation.
  Status ReleaseNoAlloc(HierarchicalMemoryReservationToken token);
  HierarchicalMemoryRetainResult Retain(HierarchicalMemoryReservationToken token);
  HierarchicalMemoryBudgetOperationResult Cancel(HierarchicalMemoryReservationToken token);
  HierarchicalMemoryCleanupResult CleanupOwner(std::string owner_id);
  HierarchicalMemoryCleanupResult CleanupOwner(const MemoryBinaryUuid& owner_uuid);
  HierarchicalMemoryCleanupResult CleanupExpiredLeases(u64 now_ms);
  HierarchicalMemoryBudgetSnapshot Snapshot() const;

  struct ScopeAccounting {
    HierarchicalMemoryScopeKind kind = HierarchicalMemoryScopeKind::process;
    std::string scope_id;
    MemoryBinaryUuid binary_scope_uuid{};
    u64 hard_limit_bytes = 0;
    u64 soft_limit_bytes = 0;
    u64 reserved_bytes = 0;
    u64 active_bytes = 0;
    u64 peak_bytes = 0;
    u64 reservation_count = 0;
    u64 commit_count = 0;
    u64 release_count = 0;
    u64 cancel_cleanup_count = 0;
    u64 owner_cleanup_count = 0;
    u64 lease_expiry_cleanup_count = 0;
    u64 active_reservation_count = 0;
    u64 active_allocation_count = 0;
    u64 priority_weight_total = 0;
  };

  struct ClassAccounting {
    MemoryCategory category = MemoryCategory::unknown;
    std::string memory_class;
    u64 reserved_bytes = 0;
    u64 active_bytes = 0;
    u64 peak_bytes = 0;
    u64 reservation_count = 0;
    u64 commit_count = 0;
    u64 release_count = 0;
  };

 private:
  HierarchicalMemoryCleanupResult CleanupOwnerImpl(
      std::string_view owner_id, const MemoryBinaryUuid& owner_uuid);
  HierarchicalMemoryBudgetOperationResult ReleaseImpl(
      HierarchicalMemoryReservationToken token, bool materialize_diagnostic,
      const HierarchicalMemoryReservationLease::State* retained_owner = nullptr);
  friend class HierarchicalMemoryReservationLease;
  enum class CleanupReason {
    release,
    cancel,
    owner,
    lease_expiry
  };

  struct ReservationRecord {
    HierarchicalMemoryReservationToken token;
    std::vector<HierarchicalMemoryScopeRef> scope_chain;
    MemoryCategory category = MemoryCategory::unknown;
    std::string memory_class;
    std::string owner_id;
    MemoryBinaryUuid binary_owner_uuid{};
    HierarchicalMemoryReservationState state = HierarchicalMemoryReservationState::reserved;
    ShardedMemoryAccountingToken accounting_token;
    int priority = 0;
    u64 weight = 1;
    u64 lease_expires_at_ms = 0;
    // Prepared before accounting publication; stable map-node pointers and
    // ordered shard indexes make completion independent of allocator health.
    std::vector<usize> scope_shard_indexes;
    std::vector<ScopeAccounting*> scopes;
    ClassAccounting* class_accounting = nullptr;
    std::shared_ptr<HierarchicalMemoryReservationLease::State> retained_owner;
    bool revocation_pending = false;
    CleanupReason pending_reason = CleanupReason::release;
  };

  struct ScopeShard {
    mutable std::mutex mutex;
    std::map<ShardedMemoryScopeKey, ScopeAccounting> scopes;
    std::map<std::string, ClassAccounting> classes;
  };

  struct TokenShard {
    mutable std::mutex mutex;
    std::unordered_map<u64, ReservationRecord> tokens;
  };

  class ScopeLocks {
   public:
    ScopeLocks(HierarchicalMemoryBudgetLedger& ledger, const std::vector<usize>& indexes);
    ScopeLocks(const ScopeLocks&) = delete;
    ScopeLocks& operator=(const ScopeLocks&) = delete;
    ~ScopeLocks();
    void Unlock() noexcept;
   private:
    HierarchicalMemoryBudgetLedger& ledger_;
    const std::vector<usize>& indexes_;
    usize locked_ = 0;
  };

  usize ScopeShardIndex(const HierarchicalMemoryScopeRef& scope) const;
  usize TokenShardIndex(u64 token_id) const;
  std::vector<usize> ScopeShardIndexesForChain(
      const std::vector<HierarchicalMemoryScopeRef>& chain) const;
  std::vector<std::unique_lock<std::mutex>> LockScopeShardsForChain(
      const std::vector<HierarchicalMemoryScopeRef>& chain);
  ScopeShard& ScopeShardForIndex(usize shard_index);
  const ScopeShard& ScopeShardForIndex(usize shard_index) const;
  TokenShard& TokenShardForIndex(usize shard_index);
  const TokenShard& TokenShardForIndex(usize shard_index) const;

  HierarchicalMemoryBudgetOperationResult CleanupLocked(TokenShard& token_shard,
                                                        u64 token_id,
                                                        CleanupReason reason);
  HierarchicalMemoryBudgetOperationResult TokenFailure(scratchbird::core::platform::StatusCode code,
                                                       std::string diagnostic_code,
                                                       std::string message_key,
                                                       const HierarchicalMemoryReservationToken& token,
                                                       std::vector<DiagnosticArgument> arguments);

  std::vector<std::unique_ptr<ScopeShard>> scope_shards_;
  std::vector<std::unique_ptr<TokenShard>> token_shards_;
  ShardedMemoryAccountingLedger accounting_;
  std::atomic<u64> next_token_id_{1};
  std::atomic<u64> global_reservation_count_{0};
  std::atomic<u64> global_commit_count_{0};
  std::atomic<u64> global_release_count_{0};
  std::atomic<u64> global_cancel_cleanup_count_{0};
  std::atomic<u64> global_owner_cleanup_count_{0};
  std::atomic<u64> global_lease_expiry_cleanup_count_{0};
  std::atomic<u64> global_active_reservation_count_{0};
  std::atomic<u64> global_active_allocation_count_{0};
  std::atomic<u64> pending_revocation_count_{0};
  std::atomic<u64> retained_revoked_bytes_{0};
  std::atomic<u64> hard_limit_refusal_count_{0};
  std::atomic<u64> soft_limit_recommendation_count_{0};
  std::atomic<u64> failed_commit_count_{0};
  std::atomic<u64> failed_release_count_{0};
};

}  // namespace scratchbird::core::memory
