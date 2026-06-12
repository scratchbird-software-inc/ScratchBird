// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "memory.hpp"
#include "runtime_platform.hpp"

#include <map>
#include <string>
#include <vector>

namespace scratchbird::transaction::mga {

using scratchbird::core::memory::MemoryManager;
using scratchbird::core::memory::ScopedAllocation;
using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u64;

// MMCH_MGA_TRANSACTION_MEMORY_GOVERNANCE
enum class MgaTransactionMemoryUseKind {
  transaction_snapshot,
  long_reader_snapshot,
  version_chain_cleanup,
  rollback_cleanup,
  abort_cleanup,
  savepoint_cleanup,
  cleanup_sweep,
  recovery_scan
};

struct MgaTransactionMemoryAuthority {
  bool engine_mga_transaction_inventory_authoritative = true;
  bool parser_or_client_authority = false;
  bool reference_authority = false;
  bool wal_or_redo_authority = false;
  bool memory_pressure_finality_authority = false;
};

struct MgaTransactionMemoryRequest {
  MgaTransactionMemoryUseKind use_kind = MgaTransactionMemoryUseKind::transaction_snapshot;
  std::string transaction_id;
  std::string scope_id;
  std::string purpose;
  u64 bytes = 0;
  MgaTransactionMemoryAuthority authority;
};

struct MgaTransactionMemoryReservation {
  std::string reservation_id;
  MgaTransactionMemoryUseKind use_kind = MgaTransactionMemoryUseKind::transaction_snapshot;
  std::string transaction_id;
  std::string scope_id;
  u64 bytes = 0;
};

struct MgaTransactionMemoryResult {
  Status status;
  bool fail_closed = false;
  MgaTransactionMemoryReservation reservation;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok() && !fail_closed;
  }
};

struct MgaTransactionMemorySnapshot {
  u64 current_bytes = 0;
  u64 peak_bytes = 0;
  u64 active_reservations = 0;
  u64 refused_count = 0;
  std::map<std::string, u64> bytes_by_transaction;
  std::map<std::string, u64> bytes_by_scope;
  std::map<MgaTransactionMemoryUseKind, u64> bytes_by_use;
};

class MgaTransactionMemoryGovernor {
 public:
  MgaTransactionMemoryGovernor(MemoryManager* memory_manager, u64 hard_limit_bytes);
  MgaTransactionMemoryGovernor(const MgaTransactionMemoryGovernor&) = delete;
  MgaTransactionMemoryGovernor& operator=(const MgaTransactionMemoryGovernor&) = delete;

  MgaTransactionMemoryResult Acquire(MgaTransactionMemoryRequest request);
  MgaTransactionMemoryResult Release(const std::string& reservation_id);
  MgaTransactionMemoryResult ReleaseScope(const std::string& scope_id);
  MgaTransactionMemoryResult CancelTransaction(const std::string& transaction_id);
  MgaTransactionMemorySnapshot Snapshot() const;

 private:
  struct ActiveReservation {
    MgaTransactionMemoryReservation reservation;
    ScopedAllocation allocation;
  };

  MgaTransactionMemoryResult FailClosed(const MgaTransactionMemoryRequest& request,
                                        std::string diagnostic_code,
                                        std::string message_key,
                                        std::string reason);
  void AddAccounting(const MgaTransactionMemoryReservation& reservation);
  void RemoveAccounting(const MgaTransactionMemoryReservation& reservation);
  MgaTransactionMemoryResult ReleaseWhere(const std::string& scope_id,
                                          const std::string& transaction_id,
                                          const char* operation);

  MemoryManager* memory_manager_ = nullptr;
  u64 hard_limit_bytes_ = 0;
  u64 next_reservation_sequence_ = 1;
  MgaTransactionMemorySnapshot snapshot_;
  std::map<std::string, ActiveReservation> active_;
};

const char* MgaTransactionMemoryUseKindName(MgaTransactionMemoryUseKind use_kind);

}  // namespace scratchbird::transaction::mga
