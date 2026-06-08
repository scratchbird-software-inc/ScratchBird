// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction_memory_governance.hpp"

#include "transaction_memory_hooks.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::transaction::mga {
namespace {

using scratchbird::core::memory::MemoryCategory;
using scratchbird::core::memory::MemoryLifetime;
using scratchbird::core::memory::MemoryTag;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

constexpr const char* kAuthorityBoundary =
    "mga_memory.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_donor_wal_or_benchmark_authority";

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::transaction_mga};
}

Status ErrorStatus() {
  return {StatusCode::memory_invalid_request, Severity::error, Subsystem::transaction_mga};
}

MemoryCategory CategoryFor(MgaTransactionMemoryUseKind use_kind) {
  switch (use_kind) {
    case MgaTransactionMemoryUseKind::transaction_snapshot:
    case MgaTransactionMemoryUseKind::long_reader_snapshot:
      return MemoryCategory::transaction_snapshot;
    case MgaTransactionMemoryUseKind::version_chain_cleanup:
      return MemoryCategory::version_chain;
    case MgaTransactionMemoryUseKind::rollback_cleanup:
    case MgaTransactionMemoryUseKind::abort_cleanup:
    case MgaTransactionMemoryUseKind::savepoint_cleanup:
    case MgaTransactionMemoryUseKind::cleanup_sweep:
      return MemoryCategory::cleanup;
    case MgaTransactionMemoryUseKind::recovery_scan:
      return MemoryCategory::transaction_local;
  }
  return MemoryCategory::transaction_local;
}

MemoryLifetime LifetimeFor(MgaTransactionMemoryUseKind use_kind) {
  switch (use_kind) {
    case MgaTransactionMemoryUseKind::transaction_snapshot:
    case MgaTransactionMemoryUseKind::long_reader_snapshot:
      return MemoryLifetime::transaction;
    case MgaTransactionMemoryUseKind::version_chain_cleanup:
    case MgaTransactionMemoryUseKind::cleanup_sweep:
      return MemoryLifetime::deferred_epoch;
    case MgaTransactionMemoryUseKind::rollback_cleanup:
    case MgaTransactionMemoryUseKind::abort_cleanup:
    case MgaTransactionMemoryUseKind::savepoint_cleanup:
      return MemoryLifetime::transaction;
    case MgaTransactionMemoryUseKind::recovery_scan:
      return MemoryLifetime::temporary;
  }
  return MemoryLifetime::temporary;
}

bool AddWouldOverflow(u64 left, u64 right) {
  return right > 0 && left > static_cast<u64>(~0ull) - right;
}

}  // namespace

const char* MgaTransactionMemoryUseKindName(MgaTransactionMemoryUseKind use_kind) {
  switch (use_kind) {
    case MgaTransactionMemoryUseKind::transaction_snapshot:
      return "transaction_snapshot";
    case MgaTransactionMemoryUseKind::long_reader_snapshot:
      return "long_reader_snapshot";
    case MgaTransactionMemoryUseKind::version_chain_cleanup:
      return "version_chain_cleanup";
    case MgaTransactionMemoryUseKind::rollback_cleanup:
      return "rollback_cleanup";
    case MgaTransactionMemoryUseKind::abort_cleanup:
      return "abort_cleanup";
    case MgaTransactionMemoryUseKind::savepoint_cleanup:
      return "savepoint_cleanup";
    case MgaTransactionMemoryUseKind::cleanup_sweep:
      return "cleanup_sweep";
    case MgaTransactionMemoryUseKind::recovery_scan:
      return "recovery_scan";
  }
  return "unknown";
}

MgaTransactionMemoryGovernor::MgaTransactionMemoryGovernor(MemoryManager* memory_manager,
                                                           u64 hard_limit_bytes)
    : memory_manager_(memory_manager), hard_limit_bytes_(hard_limit_bytes) {}

MgaTransactionMemoryResult MgaTransactionMemoryGovernor::FailClosed(
    const MgaTransactionMemoryRequest& request,
    std::string diagnostic_code,
    std::string message_key,
    std::string reason) {
  ++snapshot_.refused_count;
  MgaTransactionMemoryResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.diagnostic = MakeDiagnostic(StatusCode::memory_invalid_request,
                                     Severity::error,
                                     Subsystem::transaction_mga,
                                     std::move(diagnostic_code),
                                     std::move(message_key),
                                     {{"use_kind", MgaTransactionMemoryUseKindName(request.use_kind)},
                                      {"transaction_id", request.transaction_id},
                                      {"scope_id", request.scope_id},
                                      {"reason", reason}},
                                     {},
                                     "transaction.mga.memory_governance",
                                     "fail closed without changing transaction finality or visibility");
  result.evidence.push_back("MMCH_MGA_TRANSACTION_MEMORY_GOVERNANCE");
  result.evidence.push_back(kAuthorityBoundary);
  result.evidence.push_back("mga_memory.fail_closed=true");
  result.evidence.push_back("mga_memory.reason=" + std::move(reason));
  return result;
}

MgaTransactionMemoryResult MgaTransactionMemoryGovernor::Acquire(
    MgaTransactionMemoryRequest request) {
  if (memory_manager_ == nullptr) {
    return FailClosed(request, "mga_memory_missing_allocator",
                      "transaction.mga.memory.missing_allocator",
                      "memory_manager_required");
  }
  if (request.bytes == 0) {
    return FailClosed(request, "mga_memory_zero_size",
                      "transaction.mga.memory.zero_size",
                      "bytes_required");
  }
  if (request.transaction_id.empty() || request.scope_id.empty()) {
    return FailClosed(request, "mga_memory_missing_scope",
                      "transaction.mga.memory.missing_scope",
                      "transaction_and_scope_required");
  }
  if (!request.authority.engine_mga_transaction_inventory_authoritative) {
    return FailClosed(request, "mga_memory_missing_inventory_authority",
                      "transaction.mga.memory.missing_inventory_authority",
                      "mga_inventory_authority_required");
  }
  if (request.authority.parser_or_client_authority || request.authority.donor_authority ||
      request.authority.wal_or_redo_authority ||
      request.authority.memory_pressure_finality_authority) {
    return FailClosed(request, "mga_memory_unsafe_authority",
                      "transaction.mga.memory.unsafe_authority",
                      "parser_client_donor_wal_or_memory_finality_authority_claimed");
  }
  if (AddWouldOverflow(snapshot_.current_bytes, request.bytes) ||
      (hard_limit_bytes_ != 0 &&
       snapshot_.current_bytes + request.bytes > hard_limit_bytes_)) {
    return FailClosed(request, "mga_memory_budget_exceeded",
                      "transaction.mga.memory.budget_exceeded",
                      "transaction_memory_budget_exceeded");
  }

  MemoryTag tag = MakeMgaMemoryTag(CategoryFor(request.use_kind),
                                   MgaTransactionMemoryUseKindName(request.use_kind),
                                   LifetimeFor(request.use_kind));
  tag.owner = "transaction.mga";
  tag.context_id = request.scope_id;
  tag.transaction_id = request.transaction_id;
  tag.purpose = request.purpose.empty() ? MgaTransactionMemoryUseKindName(request.use_kind)
                                        : request.purpose;

  auto scoped = memory_manager_->AllocateScoped(static_cast<std::size_t>(request.bytes),
                                                alignof(std::max_align_t),
                                                tag);
  if (!scoped.ok()) {
    ++snapshot_.refused_count;
    MgaTransactionMemoryResult result;
    result.status = scoped.status;
    result.fail_closed = true;
    result.diagnostic = scoped.diagnostic;
    result.evidence.push_back("MMCH_MGA_TRANSACTION_MEMORY_GOVERNANCE");
    result.evidence.push_back(kAuthorityBoundary);
    result.evidence.push_back("mga_memory.fail_closed=true");
    result.evidence.push_back("mga_memory.reason=allocator_refused");
    return result;
  }

  MgaTransactionMemoryReservation reservation;
  reservation.reservation_id = "mga-memory-" + std::to_string(next_reservation_sequence_++);
  reservation.use_kind = request.use_kind;
  reservation.transaction_id = request.transaction_id;
  reservation.scope_id = request.scope_id;
  reservation.bytes = request.bytes;

  ActiveReservation active;
  active.reservation = reservation;
  active.allocation = std::move(scoped.allocation);
  active_[reservation.reservation_id] = std::move(active);
  AddAccounting(reservation);

  MgaTransactionMemoryResult result;
  result.status = OkStatus();
  result.reservation = reservation;
  result.evidence.push_back("MMCH_MGA_TRANSACTION_MEMORY_GOVERNANCE");
  result.evidence.push_back(kAuthorityBoundary);
  result.evidence.push_back("mga_memory.granted=true");
  result.evidence.push_back("mga_memory.use_kind=" +
                            std::string(MgaTransactionMemoryUseKindName(request.use_kind)));
  result.evidence.push_back("mga_memory.reservation_id=" + reservation.reservation_id);
  return result;
}

void MgaTransactionMemoryGovernor::AddAccounting(
    const MgaTransactionMemoryReservation& reservation) {
  snapshot_.current_bytes += reservation.bytes;
  snapshot_.peak_bytes = std::max(snapshot_.peak_bytes, snapshot_.current_bytes);
  snapshot_.active_reservations = active_.size();
  snapshot_.bytes_by_transaction[reservation.transaction_id] += reservation.bytes;
  snapshot_.bytes_by_scope[reservation.scope_id] += reservation.bytes;
  snapshot_.bytes_by_use[reservation.use_kind] += reservation.bytes;
}

void MgaTransactionMemoryGovernor::RemoveAccounting(
    const MgaTransactionMemoryReservation& reservation) {
  snapshot_.current_bytes = reservation.bytes >= snapshot_.current_bytes
                                ? 0
                                : snapshot_.current_bytes - reservation.bytes;
  auto remove = [](auto* map, const auto& key, u64 bytes) {
    auto it = map->find(key);
    if (it == map->end()) {
      return;
    }
    it->second = bytes >= it->second ? 0 : it->second - bytes;
    if (it->second == 0) {
      map->erase(it);
    }
  };
  remove(&snapshot_.bytes_by_transaction, reservation.transaction_id, reservation.bytes);
  remove(&snapshot_.bytes_by_scope, reservation.scope_id, reservation.bytes);
  remove(&snapshot_.bytes_by_use, reservation.use_kind, reservation.bytes);
  snapshot_.active_reservations = active_.size();
}

MgaTransactionMemoryResult MgaTransactionMemoryGovernor::Release(
    const std::string& reservation_id) {
  auto it = active_.find(reservation_id);
  if (it == active_.end()) {
    MgaTransactionMemoryRequest request;
    request.scope_id = reservation_id;
    return FailClosed(request, "mga_memory_unknown_reservation",
                      "transaction.mga.memory.unknown_reservation",
                      "reservation_not_found");
  }
  const auto reservation = it->second.reservation;
  (void)it->second.allocation.Reset();
  active_.erase(it);
  RemoveAccounting(reservation);

  MgaTransactionMemoryResult result;
  result.status = OkStatus();
  result.reservation = reservation;
  result.evidence.push_back("MMCH_MGA_TRANSACTION_MEMORY_GOVERNANCE");
  result.evidence.push_back(kAuthorityBoundary);
  result.evidence.push_back("mga_memory.released=true");
  result.evidence.push_back("mga_memory.reservation_id=" + reservation.reservation_id);
  return result;
}

MgaTransactionMemoryResult MgaTransactionMemoryGovernor::ReleaseWhere(
    const std::string& scope_id,
    const std::string& transaction_id,
    const char* operation) {
  std::vector<std::string> ids;
  for (const auto& entry : active_) {
    const auto& reservation = entry.second.reservation;
    if ((!scope_id.empty() && reservation.scope_id == scope_id) ||
        (!transaction_id.empty() && reservation.transaction_id == transaction_id)) {
      ids.push_back(entry.first);
    }
  }
  u64 released_bytes = 0;
  u64 released_count = 0;
  for (const auto& id : ids) {
    auto it = active_.find(id);
    if (it == active_.end()) {
      continue;
    }
    const auto reservation = it->second.reservation;
    released_bytes += reservation.bytes;
    (void)it->second.allocation.Reset();
    active_.erase(it);
    RemoveAccounting(reservation);
    ++released_count;
  }
  MgaTransactionMemoryResult result;
  result.status = OkStatus();
  result.evidence.push_back("MMCH_MGA_TRANSACTION_MEMORY_GOVERNANCE");
  result.evidence.push_back(kAuthorityBoundary);
  result.evidence.push_back(std::string("mga_memory.") + operation + "=true");
  result.evidence.push_back("mga_memory.released_count=" + std::to_string(released_count));
  result.evidence.push_back("mga_memory.released_bytes=" + std::to_string(released_bytes));
  return result;
}

MgaTransactionMemoryResult MgaTransactionMemoryGovernor::ReleaseScope(
    const std::string& scope_id) {
  return ReleaseWhere(scope_id, {}, "scope_release");
}

MgaTransactionMemoryResult MgaTransactionMemoryGovernor::CancelTransaction(
    const std::string& transaction_id) {
  return ReleaseWhere({}, transaction_id, "transaction_cancel");
}

MgaTransactionMemorySnapshot MgaTransactionMemoryGovernor::Snapshot() const {
  MgaTransactionMemorySnapshot snapshot = snapshot_;
  snapshot.active_reservations = active_.size();
  return snapshot;
}

}  // namespace scratchbird::transaction::mga
