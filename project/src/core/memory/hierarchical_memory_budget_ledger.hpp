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
  plugin
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
};

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

  bool ok() const {
    return status.ok();
  }
};

struct HierarchicalMemoryScopeSnapshot {
  HierarchicalMemoryScopeKind kind = HierarchicalMemoryScopeKind::process;
  std::string scope_id;
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
  std::vector<HierarchicalMemoryScopeSnapshot> scopes;
  std::vector<HierarchicalMemoryClassSnapshot> classes;
};

const char* HierarchicalMemoryScopeKindName(HierarchicalMemoryScopeKind kind);
const char* HierarchicalMemoryReservationRecommendationName(
    HierarchicalMemoryReservationRecommendation recommendation);
const char* HierarchicalMemoryBudgetProvenanceSourceName(
    HierarchicalMemoryBudgetProvenanceSource source);

class HierarchicalMemoryBudgetLedger {
 public:
  explicit HierarchicalMemoryBudgetLedger(usize scope_shard_count = 64,
                                          usize token_shard_count = 64);
  HierarchicalMemoryBudgetLedger(const HierarchicalMemoryBudgetLedger&) = delete;
  HierarchicalMemoryBudgetLedger& operator=(const HierarchicalMemoryBudgetLedger&) = delete;
  ~HierarchicalMemoryBudgetLedger();

  usize scope_shard_count() const;
  usize token_shard_count() const;

  HierarchicalMemoryBudgetOperationResult SetBudget(HierarchicalMemoryBudget budget);
  HierarchicalMemoryReservationResult Reserve(HierarchicalMemoryReservationRequest request);
  HierarchicalMemoryBudgetOperationResult Commit(HierarchicalMemoryReservationToken token);
  HierarchicalMemoryBudgetOperationResult Release(HierarchicalMemoryReservationToken token);
  HierarchicalMemoryBudgetOperationResult Cancel(HierarchicalMemoryReservationToken token);
  HierarchicalMemoryCleanupResult CleanupOwner(std::string owner_id);
  HierarchicalMemoryCleanupResult CleanupExpiredLeases(u64 now_ms);
  HierarchicalMemoryBudgetSnapshot Snapshot() const;

  struct ScopeAccounting {
    HierarchicalMemoryScopeKind kind = HierarchicalMemoryScopeKind::process;
    std::string scope_id;
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
  enum class CleanupReason {
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
    HierarchicalMemoryReservationState state = HierarchicalMemoryReservationState::reserved;
    ShardedMemoryAccountingToken accounting_token;
    int priority = 0;
    u64 weight = 1;
    u64 lease_expires_at_ms = 0;
  };

  struct ScopeShard {
    mutable std::mutex mutex;
    std::map<std::string, ScopeAccounting> scopes;
    std::map<std::string, ClassAccounting> classes;
  };

  struct TokenShard {
    mutable std::mutex mutex;
    std::unordered_map<u64, ReservationRecord> tokens;
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
  std::atomic<u64> global_reserved_bytes_{0};
  std::atomic<u64> global_current_bytes_{0};
  std::atomic<u64> global_peak_bytes_{0};
  std::atomic<u64> global_reservation_count_{0};
  std::atomic<u64> global_commit_count_{0};
  std::atomic<u64> global_release_count_{0};
  std::atomic<u64> global_cancel_cleanup_count_{0};
  std::atomic<u64> global_owner_cleanup_count_{0};
  std::atomic<u64> global_lease_expiry_cleanup_count_{0};
  std::atomic<u64> global_active_reservation_count_{0};
  std::atomic<u64> global_active_allocation_count_{0};
  std::atomic<u64> hard_limit_refusal_count_{0};
  std::atomic<u64> soft_limit_recommendation_count_{0};
  std::atomic<u64> failed_commit_count_{0};
  std::atomic<u64> failed_release_count_{0};
};

}  // namespace scratchbird::core::memory
