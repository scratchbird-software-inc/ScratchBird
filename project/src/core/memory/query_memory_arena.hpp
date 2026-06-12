// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-QUERY-MEMORY-ARENA-ANCHOR
#include "memory.hpp"
#include "hierarchical_memory_budget_ledger.hpp"
#include "temp_workspace_lifecycle.hpp"

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::core::memory {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u64;

enum class QueryMemoryFamily {
  unknown,
  relational,
  search,
  vector,
  graph,
  document,
  time_series,
  dml,
  candidate_set
};

struct QueryMemoryContext {
  std::string query_id;
  std::string statement_id;
  std::string session_id;
  std::string transaction_id;
  std::string database_id;
  std::string engine_id;
  std::string operation_id;
  bool engine_mga_authoritative = true;
  bool parser_or_reference_finality_or_visibility_authority = false;
  bool client_finality_or_visibility_authority = false;
  bool provider_finality_or_visibility_authority = false;
  bool wal_recovery_or_finality_authority = false;
};

struct QueryMemoryArenaLimits {
  u64 hard_limit_bytes = 0;
  u64 soft_limit_bytes = 0;
  u64 family_limit_bytes = 0;
  u64 query_limit_bytes = 0;
  u64 spill_limit_bytes = 0;
  bool allow_spill = false;
  bool require_hierarchical_reservation = true;
};

// MMCH_UNIFIED_MEMORY_SPILL_BUDGET
enum class UnifiedMemorySpillBudgetKind {
  heap,
  spill
};

struct UnifiedMemorySpillBudgetRequest {
  std::string operation_id;
  std::string owner_scope;
  UnifiedMemorySpillBudgetKind kind = UnifiedMemorySpillBudgetKind::heap;
  u64 bytes = 0;
};

struct UnifiedMemorySpillBudgetReservation {
  std::string reservation_id;
  std::string operation_id;
  std::string owner_scope;
  UnifiedMemorySpillBudgetKind kind = UnifiedMemorySpillBudgetKind::heap;
  u64 bytes = 0;
};

struct UnifiedMemorySpillBudgetSnapshot {
  std::string ledger_id;
  u64 limit_bytes = 0;
  u64 heap_bytes = 0;
  u64 spill_bytes = 0;
  u64 total_bytes = 0;
  u64 active_reservation_count = 0;
  u64 peak_total_bytes = 0;
  u64 denial_count = 0;
};

struct UnifiedMemorySpillBudgetResult {
  Status status;
  bool fail_closed = false;
  bool reservation_created = false;
  bool released = false;
  bool not_found = false;
  std::optional<UnifiedMemorySpillBudgetReservation> reservation;
  UnifiedMemorySpillBudgetSnapshot snapshot;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct QueryMemoryGrantRequest {
  QueryMemoryFamily family = QueryMemoryFamily::unknown;
  u64 bytes = 0;
  bool spillable = false;
  std::string purpose;
};

struct QueryMemoryGrant {
  std::string grant_id;
  QueryMemoryFamily family = QueryMemoryFamily::unknown;
  u64 bytes = 0;
  u64 spill_reserved_bytes = 0;
  bool spilled = false;
  std::string spill_allocation_id;
  std::string spill_operation_id;
  std::string unified_budget_reservation_id;
};

struct QueryMemoryArenaCounters {
  u64 current_bytes = 0;
  u64 peak_bytes = 0;
  u64 denied_count = 0;
  u64 spilled_count = 0;
  u64 spilled_bytes = 0;
  u64 cancelled_count = 0;
  u64 grant_count = 0;
  u64 release_count = 0;
  u64 active_grant_count = 0;
  u64 leak_count = 0;
  std::map<QueryMemoryFamily, u64> current_family_bytes;
  std::map<QueryMemoryFamily, u64> peak_family_bytes;
};

struct QueryMemoryArenaResult {
  Status status;
  bool fail_closed = false;
  std::optional<QueryMemoryGrant> grant;
  QueryMemoryArenaCounters counters;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct QueryMemoryArenaReleaseResult {
  Status status;
  bool fail_closed = false;
  QueryMemoryArenaCounters counters;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

class QueryMemoryArena {
 public:
  QueryMemoryArena(QueryMemoryContext context,
                   QueryMemoryArenaLimits limits,
                   BoundedAllocator* allocator,
                   TempWorkspaceLifecycleManager* temp_workspace = nullptr,
                   class UnifiedMemorySpillBudgetLedger* unified_budget = nullptr,
                   HierarchicalMemoryBudgetLedger* reservation_ledger = nullptr);
  QueryMemoryArena(const QueryMemoryArena&) = delete;
  QueryMemoryArena& operator=(const QueryMemoryArena&) = delete;
  ~QueryMemoryArena();

  QueryMemoryArenaResult Grant(QueryMemoryGrantRequest request);
  QueryMemoryArenaReleaseResult Release(const std::string& grant_id);
  QueryMemoryArenaReleaseResult Cancel(std::string reason);
  QueryMemoryArenaReleaseResult Reset();

  QueryMemoryArenaCounters Snapshot() const;
  const QueryMemoryContext& context() const { return context_; }
  const QueryMemoryArenaLimits& limits() const { return limits_; }

 private:
  struct ActiveGrant {
    QueryMemoryGrant grant;
    void* pointer = nullptr;
    usize alignment = 0;
    MemoryTag tag;
    bool arena_owned = false;
    HierarchicalMemoryReservationToken reservation_token;
  };

  struct HierarchicalReservationResult {
    Status status;
    bool fail_closed = false;
    std::optional<HierarchicalMemoryReservationToken> token;
    DiagnosticRecord diagnostic;
    std::vector<std::string> evidence;

    bool ok() const {
      return status.ok() && !fail_closed;
    }
  };

  QueryMemoryArenaResult Refuse(const QueryMemoryGrantRequest& request,
                                std::string diagnostic_code,
                                std::string message_key,
                                std::string reason,
                                scratchbird::core::platform::StatusCode code =
                                    scratchbird::core::platform::StatusCode::memory_invalid_request);
  QueryMemoryArenaReleaseResult RefuseRelease(std::string diagnostic_code,
                                              std::string message_key,
                                              std::string reason,
                                              scratchbird::core::platform::StatusCode code =
                                                  scratchbird::core::platform::StatusCode::memory_invalid_request) const;
  bool ContextMissing() const;
  bool UnsafeAuthority() const;
  bool SupportedFamily(QueryMemoryFamily family) const;
  bool AddWouldExceed(u64 current, u64 add, u64 limit) const;
  u64 FamilyCurrent(QueryMemoryFamily family) const;
  QueryMemoryArenaResult SpillInsteadOfGrant(QueryMemoryGrantRequest request, std::string reason);
  TempWorkspaceCleanupResult ReleaseSpill(const ActiveGrant& grant);
  HierarchicalReservationResult ReserveHierarchicalBudget(QueryMemoryGrantRequest request,
                                                          const char* memory_class);
  bool CommitHierarchicalBudget(const HierarchicalMemoryReservationToken& token,
                                QueryMemoryArenaReleaseResult* rollback_result);
  void ReleaseHierarchicalBudget(const ActiveGrant& grant,
                                 std::vector<std::string>* evidence);
  UnifiedMemorySpillBudgetResult ReserveUnifiedBudget(QueryMemoryGrantRequest request,
                                                      UnifiedMemorySpillBudgetKind kind);
  void ReleaseUnifiedBudget(const ActiveGrant& grant,
                            std::vector<std::string>* evidence);
  bool HasActiveHeapGrantLocked() const;
  DeallocationResult ResetHeapArenaLocked(std::vector<std::string>* evidence);
  void AppendBaseEvidence(std::vector<std::string>* evidence, QueryMemoryFamily family) const;
  DiagnosticRecord MakeArenaDiagnostic(Status status,
                                       std::string diagnostic_code,
                                       std::string message_key,
                                       std::string reason,
                                       QueryMemoryFamily family,
                                       u64 requested_bytes) const;

  QueryMemoryContext context_;
  QueryMemoryArenaLimits limits_;
  BoundedAllocator* allocator_ = nullptr;
  TempWorkspaceLifecycleManager* temp_workspace_ = nullptr;
  class UnifiedMemorySpillBudgetLedger* unified_budget_ = nullptr;
  HierarchicalMemoryBudgetLedger* reservation_ledger_ = nullptr;
  mutable std::mutex mutex_;
  QueryMemoryArenaCounters counters_;
  std::map<std::string, ActiveGrant> active_;
  std::optional<ArenaAllocator> heap_arena_;
  u64 next_grant_ = 1;
  bool released_ = false;
};

class UnifiedMemorySpillBudgetLedger {
 public:
  UnifiedMemorySpillBudgetLedger(std::string ledger_id, u64 limit_bytes);
  UnifiedMemorySpillBudgetLedger(const UnifiedMemorySpillBudgetLedger&) = delete;
  UnifiedMemorySpillBudgetLedger& operator=(const UnifiedMemorySpillBudgetLedger&) = delete;

  UnifiedMemorySpillBudgetResult Reserve(UnifiedMemorySpillBudgetRequest request);
  UnifiedMemorySpillBudgetResult Release(const std::string& reservation_id);
  UnifiedMemorySpillBudgetResult ReleaseOwnerReservations(const std::string& owner_scope);
  UnifiedMemorySpillBudgetSnapshot Snapshot() const;

 private:
  struct ActiveReservation {
    UnifiedMemorySpillBudgetReservation reservation;
  };

  UnifiedMemorySpillBudgetSnapshot SnapshotLocked() const;
  DiagnosticRecord MakeDiagnostic(Status status,
                                  std::string diagnostic_code,
                                  std::string message_key,
                                  std::string reason,
                                  const UnifiedMemorySpillBudgetRequest& request) const;

  std::string ledger_id_;
  u64 limit_bytes_ = 0;
  mutable std::mutex mutex_;
  std::map<std::string, ActiveReservation> active_;
  u64 heap_bytes_ = 0;
  u64 spill_bytes_ = 0;
  u64 peak_total_bytes_ = 0;
  u64 denial_count_ = 0;
  u64 next_reservation_ = 1;
};

const char* QueryMemoryFamilyName(QueryMemoryFamily family);
bool QueryMemoryFamilySupported(QueryMemoryFamily family);
const char* UnifiedMemorySpillBudgetKindName(UnifiedMemorySpillBudgetKind kind);

}  // namespace scratchbird::core::memory
