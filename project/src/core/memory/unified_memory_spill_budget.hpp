// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "runtime_platform.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::core::memory {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u64;
using QueryMemoryUuid = scratchbird::core::platform::Uuid;

// MMCH_UNIFIED_MEMORY_SPILL_BUDGET
enum class UnifiedMemorySpillBudgetKind {
  heap,
  spill
};

struct UnifiedMemorySpillBudgetRequest {
  QueryMemoryUuid operation_id{};
  QueryMemoryUuid owner_scope{};
  UnifiedMemorySpillBudgetKind kind = UnifiedMemorySpillBudgetKind::heap;
  u64 bytes = 0;
};

struct UnifiedMemorySpillBudgetReservation {
  QueryMemoryUuid reservation_id{};
  QueryMemoryUuid operation_id{};
  QueryMemoryUuid owner_scope{};
  UnifiedMemorySpillBudgetKind kind = UnifiedMemorySpillBudgetKind::heap;
  u64 bytes = 0;
};

struct UnifiedMemorySpillBudgetSnapshot {
  QueryMemoryUuid ledger_id{};
  u64 limit_bytes = 0;
  u64 heap_bytes = 0;
  u64 spill_bytes = 0;
  u64 total_bytes = 0;
  u64 active_reservation_count = 0;
  u64 peak_total_bytes = 0;
  u64 denial_count = 0;
  u64 retained_bytes = 0;
  u64 retained_reservation_count = 0;
};

struct UnifiedMemorySpillBudgetResult {
  Status status;
  bool fail_closed = false;
  bool reservation_created = false;
  bool released = false;
  bool not_found = false;
  bool retained = false;
  u64 retained_bytes = 0;
  std::optional<UnifiedMemorySpillBudgetReservation> reservation;
  UnifiedMemorySpillBudgetSnapshot snapshot;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

class UnifiedMemorySpillBudgetLedger;
class UnifiedMemorySpillBudgetLease {
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
    friend class UnifiedMemorySpillBudgetLease;
  };
  UnifiedMemorySpillBudgetLease() = default;
  UnifiedMemorySpillBudgetLease(const UnifiedMemorySpillBudgetLease&) = delete;
  UnifiedMemorySpillBudgetLease& operator=(const UnifiedMemorySpillBudgetLease&) = delete;
  UnifiedMemorySpillBudgetLease(UnifiedMemorySpillBudgetLease&& other) noexcept;
  UnifiedMemorySpillBudgetLease& operator=(UnifiedMemorySpillBudgetLease&& other) noexcept;
  ~UnifiedMemorySpillBudgetLease();
  bool valid() const;
  bool live() const;
  UseGuard Use() const;
  // The ledger outlives the lease. Drop use guards and retire owned backing
  // before Reset; public raw-ID release revokes use but cannot drop this charge.
  Status Reset();
 private:
  mutable std::mutex mutex_;
  UnifiedMemorySpillBudgetLedger* ledger_ = nullptr;
  QueryMemoryUuid reservation_id_{};
  std::shared_ptr<State> state_;
  friend class UnifiedMemorySpillBudgetLedger;
};

struct UnifiedMemorySpillBudgetRetainResult {
  Status status;
  UnifiedMemorySpillBudgetLease lease;
  bool ok() const { return status.ok() && lease.valid(); }
};

class UnifiedMemorySpillBudgetLedger {
 public:
  UnifiedMemorySpillBudgetLedger(QueryMemoryUuid ledger_id, u64 limit_bytes);
  UnifiedMemorySpillBudgetLedger(const UnifiedMemorySpillBudgetLedger&) = delete;
  UnifiedMemorySpillBudgetLedger& operator=(const UnifiedMemorySpillBudgetLedger&) = delete;

  UnifiedMemorySpillBudgetResult Reserve(UnifiedMemorySpillBudgetRequest request);
  UnifiedMemorySpillBudgetRetainResult Retain(const QueryMemoryUuid& reservation_id);
  UnifiedMemorySpillBudgetResult Release(const QueryMemoryUuid& reservation_id);
  // Exact-token rollback without allocating diagnostic/evidence strings.
  Status ReleaseNoAlloc(const QueryMemoryUuid& reservation_id);
  UnifiedMemorySpillBudgetResult ReleaseOwnerReservations(const QueryMemoryUuid& owner_scope);
  UnifiedMemorySpillBudgetSnapshot Snapshot() const;

 private:
  struct ActiveReservation {
    UnifiedMemorySpillBudgetReservation reservation;
    std::shared_ptr<UnifiedMemorySpillBudgetLease::State> retained_owner;
  };

  Status ReleaseRetainedNoAlloc(const QueryMemoryUuid& reservation_id,
                               const UnifiedMemorySpillBudgetLease::State* owner);
  friend class UnifiedMemorySpillBudgetLease;

  UnifiedMemorySpillBudgetSnapshot SnapshotLocked() const;
  DiagnosticRecord MakeDiagnostic(Status status,
                                  std::string diagnostic_code,
                                  std::string message_key,
                                  std::string reason,
                                  const UnifiedMemorySpillBudgetRequest& request) const;

  QueryMemoryUuid ledger_id_{};
  u64 limit_bytes_ = 0;
  mutable std::mutex mutex_;
  std::map<QueryMemoryUuid, ActiveReservation> active_;
  u64 heap_bytes_ = 0;
  u64 spill_bytes_ = 0;
  u64 peak_total_bytes_ = 0;
  u64 denial_count_ = 0;
};

const char* UnifiedMemorySpillBudgetKindName(UnifiedMemorySpillBudgetKind kind);

}  // namespace scratchbird::core::memory
