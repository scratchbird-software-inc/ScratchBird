// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "unified_memory_spill_budget.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

namespace scratchbird::core::memory {
namespace {
using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::memory}; }
Status ErrorStatus(StatusCode code) { return {code, Severity::error, Subsystem::memory}; }
std::string BoolText(bool value) { return value ? "true" : "false"; }
static_assert(std::is_nothrow_move_constructible_v<UnifiedMemorySpillBudgetResult>);
}  // namespace

const char* UnifiedMemorySpillBudgetKindName(UnifiedMemorySpillBudgetKind kind) {
  switch (kind) {
    case UnifiedMemorySpillBudgetKind::heap:
      return "heap";
    case UnifiedMemorySpillBudgetKind::spill:
      return "spill";
  }
  return "heap";
}

struct UnifiedMemorySpillBudgetLease::State {
  std::mutex mutex;
  bool live = true;
};

UnifiedMemorySpillBudgetLease::UseGuard::UseGuard(std::shared_ptr<State> state)
    : state_(std::move(state)),
      lock_(state_ ? std::unique_lock<std::mutex>(state_->mutex) : std::unique_lock<std::mutex>()) {}

bool UnifiedMemorySpillBudgetLease::UseGuard::live() const {
  return state_ && lock_.owns_lock() && state_->live;
}

UnifiedMemorySpillBudgetLease::UnifiedMemorySpillBudgetLease(UnifiedMemorySpillBudgetLease&& other) noexcept {
  std::lock_guard lock(other.mutex_);
  ledger_ = std::exchange(other.ledger_, nullptr);
  reservation_id_ = std::exchange(other.reservation_id_, {});
  state_ = std::move(other.state_);
}

UnifiedMemorySpillBudgetLease& UnifiedMemorySpillBudgetLease::operator=(
    UnifiedMemorySpillBudgetLease&& other) noexcept {
  if (this != &other) {
    (void)Reset();
    std::scoped_lock lock(mutex_, other.mutex_);
    ledger_ = std::exchange(other.ledger_, nullptr);
    reservation_id_ = std::exchange(other.reservation_id_, {});
    state_ = std::move(other.state_);
  }
  return *this;
}

UnifiedMemorySpillBudgetLease::~UnifiedMemorySpillBudgetLease() { (void)Reset(); }

bool UnifiedMemorySpillBudgetLease::valid() const {
  std::lock_guard lock(mutex_);
  return ledger_ && state_ && !reservation_id_.is_nil();
}

UnifiedMemorySpillBudgetLease::UseGuard UnifiedMemorySpillBudgetLease::Use() const {
  std::shared_ptr<State> state;
  { std::lock_guard lock(mutex_); state = state_; }
  return UseGuard(std::move(state));
}

bool UnifiedMemorySpillBudgetLease::live() const { return Use().live(); }

Status UnifiedMemorySpillBudgetLease::Reset() {
  std::lock_guard lock(mutex_);
  if (!ledger_) return OkStatus();
  const auto status = ledger_->ReleaseRetainedNoAlloc(reservation_id_, state_.get());
  if (status.ok() || status.code == StatusCode::memory_unknown_pointer) {
    ledger_ = nullptr;
    reservation_id_ = {};
    state_.reset();
  }
  return status;
}

UnifiedMemorySpillBudgetRetainResult UnifiedMemorySpillBudgetLedger::Retain(
    const QueryMemoryUuid& reservation_id) try {
  UnifiedMemorySpillBudgetRetainResult result;
  result.status = ErrorStatus(StatusCode::memory_unknown_pointer);
  std::lock_guard lock(mutex_);
  auto found = active_.find(reservation_id);
  if (found == active_.end()) return result;
  if (found->second.retained_owner) {
    result.status = ErrorStatus(StatusCode::memory_invalid_request);
    return result;
  }
  auto state = std::make_shared<UnifiedMemorySpillBudgetLease::State>();
  result.lease.ledger_ = this;
  result.lease.reservation_id_ = reservation_id;
  result.lease.state_ = state;
  found->second.retained_owner = std::move(state);
  result.status = OkStatus();
  return result;
} catch (const std::bad_alloc&) {
  UnifiedMemorySpillBudgetRetainResult result;
  result.status = ErrorStatus(StatusCode::memory_allocation_failed);
  return result;
}

UnifiedMemorySpillBudgetLedger::UnifiedMemorySpillBudgetLedger(
    QueryMemoryUuid ledger_id,
    u64 limit_bytes)
    : ledger_id_(std::move(ledger_id)), limit_bytes_(limit_bytes) {}

UnifiedMemorySpillBudgetSnapshot
UnifiedMemorySpillBudgetLedger::SnapshotLocked() const {
  UnifiedMemorySpillBudgetSnapshot snapshot;
  snapshot.ledger_id = ledger_id_;
  snapshot.limit_bytes = limit_bytes_;
  snapshot.heap_bytes = heap_bytes_;
  snapshot.spill_bytes = spill_bytes_;
  snapshot.total_bytes = heap_bytes_ + spill_bytes_;
  snapshot.active_reservation_count = active_.size();
  snapshot.peak_total_bytes = peak_total_bytes_;
  snapshot.denial_count = denial_count_;
  for (const auto& [id, entry] : active_) {
    if (!entry.retained_owner) continue;
    snapshot.retained_bytes += entry.reservation.bytes;
    ++snapshot.retained_reservation_count;
  }
  return snapshot;
}

UnifiedMemorySpillBudgetSnapshot
UnifiedMemorySpillBudgetLedger::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return SnapshotLocked();
}

UnifiedMemorySpillBudgetResult UnifiedMemorySpillBudgetLedger::Reserve(
    UnifiedMemorySpillBudgetRequest request) try {
  UnifiedMemorySpillBudgetResult result;
  result.status = OkStatus();
  result.evidence.push_back("MMCH_UNIFIED_MEMORY_SPILL_BUDGET");
  result.evidence.push_back("unified_memory_spill.kind=" +
                            std::string(UnifiedMemorySpillBudgetKindName(request.kind)));
  result.evidence.push_back("unified_memory_spill.requested_bytes=" +
                            std::to_string(request.bytes));
  result.evidence.push_back(
      "unified_memory_spill.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority");

  std::lock_guard<std::mutex> lock(mutex_);
  if (limit_bytes_ == 0 ||
      !scratchbird::core::uuid::IsEngineIdentityUuid(ledger_id_) ||
      !scratchbird::core::uuid::IsEngineIdentityUuid(request.operation_id) ||
      !scratchbird::core::uuid::IsEngineIdentityUuid(request.owner_scope) ||
      (request.kind != UnifiedMemorySpillBudgetKind::heap &&
       request.kind != UnifiedMemorySpillBudgetKind::spill) || request.bytes == 0) {
    ++denial_count_;
    result.status = ErrorStatus(StatusCode::memory_invalid_request);
    result.fail_closed = true;
    result.snapshot = SnapshotLocked();
    result.diagnostic = MakeDiagnostic(
        result.status,
        "SB_UNIFIED_MEMORY_SPILL_BUDGET.REQUEST_INVALID",
        "unified_memory_spill.request_invalid",
        "limit operation owner and positive bytes are required",
        request);
    result.evidence.push_back("unified_memory_spill.reservation_created=false");
    result.evidence.push_back("unified_memory_spill.fail_closed=true");
    return result;
  }
  const u64 current = heap_bytes_ + spill_bytes_;
  if (request.bytes > limit_bytes_ || current > limit_bytes_ - request.bytes) {
    ++denial_count_;
    result.status = ErrorStatus(StatusCode::memory_limit_exceeded);
    result.fail_closed = true;
    result.snapshot = SnapshotLocked();
    result.diagnostic = MakeDiagnostic(
        result.status,
        "SB_UNIFIED_MEMORY_SPILL_BUDGET.LIMIT_EXCEEDED",
        "unified_memory_spill.limit_exceeded",
        "combined heap and spill budget exceeded",
        request);
    result.evidence.push_back("unified_memory_spill.reservation_created=false");
    result.evidence.push_back("unified_memory_spill.fail_closed=true");
    result.evidence.push_back("unified_memory_spill.current_total_bytes=" +
                              std::to_string(current));
    return result;
  }

  UnifiedMemorySpillBudgetReservation reservation;
  const auto identity = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  if (!identity || active_.contains(*identity)) {
    result.status = ErrorStatus(StatusCode::memory_allocation_failed);
    result.fail_closed = true;
    result.snapshot = SnapshotLocked();
    result.diagnostic = MakeDiagnostic(result.status,
        "SB_UNIFIED_MEMORY_SPILL_BUDGET.REQUEST_INVALID",
        "unified_memory_spill.request_invalid", "reservation identity issuance failed", request);
    return result;
  }
  reservation.reservation_id = *identity;
  reservation.operation_id = std::move(request.operation_id);
  reservation.owner_scope = std::move(request.owner_scope);
  reservation.kind = request.kind;
  reservation.bytes = request.bytes;
  result.reservation = reservation;
  result.reservation_created = true;
  result.snapshot = SnapshotLocked();
  if (reservation.kind == UnifiedMemorySpillBudgetKind::heap)
    result.snapshot.heap_bytes += reservation.bytes;
  else
    result.snapshot.spill_bytes += reservation.bytes;
  result.snapshot.total_bytes += reservation.bytes;
  ++result.snapshot.active_reservation_count;
  result.snapshot.peak_total_bytes = std::max(peak_total_bytes_, result.snapshot.total_bytes);
  result.evidence.push_back("unified_memory_spill.reservation_created=true");
  result.evidence.push_back("unified_memory_spill.total_bytes=" +
                            std::to_string(result.snapshot.total_bytes));
  // All fallible result construction precedes publication. A failed map
  // allocation has the standard strong guarantee and leaves all counters intact.
  active_.emplace(reservation.reservation_id, ActiveReservation{reservation});
  heap_bytes_ = result.snapshot.heap_bytes;
  spill_bytes_ = result.snapshot.spill_bytes;
  peak_total_bytes_ = result.snapshot.peak_total_bytes;
  return result;
} catch (const std::bad_alloc&) {
  UnifiedMemorySpillBudgetResult result;
  result.status = ErrorStatus(StatusCode::memory_allocation_failed);
  result.fail_closed = true;
  result.snapshot = Snapshot();
  return result;
}


UnifiedMemorySpillBudgetResult UnifiedMemorySpillBudgetLedger::Release(
    const QueryMemoryUuid& reservation_id) try {
  UnifiedMemorySpillBudgetResult result;
  result.status = OkStatus();
  result.evidence.push_back("MMCH_UNIFIED_MEMORY_SPILL_BUDGET");
  result.evidence.push_back(
      "unified_memory_spill.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority");

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = active_.find(reservation_id);
  if (it == active_.end()) {
    result.status = ErrorStatus(StatusCode::memory_unknown_pointer);
    result.fail_closed = true;
    result.not_found = true;
    result.snapshot = SnapshotLocked();
    UnifiedMemorySpillBudgetRequest request;
    request.operation_id = reservation_id;
    result.diagnostic = MakeDiagnostic(result.status,
                                       "SB_UNIFIED_MEMORY_SPILL_BUDGET.NOT_FOUND",
                                       "unified_memory_spill.not_found",
                                       "reservation was not active",
                                       request);
    result.evidence.push_back("unified_memory_spill.released=false");
    return result;
  }
  result.reservation = it->second.reservation;
  result.snapshot = SnapshotLocked();
  if (it->second.retained_owner) {
    result.status = ErrorStatus(StatusCode::memory_invalid_request);
    result.fail_closed = true;
    result.retained = true;
    result.retained_bytes = it->second.reservation.bytes;
    result.evidence.push_back("unified_memory_spill.release_retained=true");
    std::lock_guard owner_lock(it->second.retained_owner->mutex);
    it->second.retained_owner->live = false;
    return result;
  }
  if (it->second.reservation.kind == UnifiedMemorySpillBudgetKind::heap)
    result.snapshot.heap_bytes -= it->second.reservation.bytes;
  else
    result.snapshot.spill_bytes -= it->second.reservation.bytes;
  result.snapshot.total_bytes -= it->second.reservation.bytes;
  --result.snapshot.active_reservation_count;
  result.released = true;
  result.evidence.push_back("unified_memory_spill.released=true");
  result.evidence.push_back("unified_memory_spill.total_bytes=" +
                            std::to_string(result.snapshot.total_bytes));
  active_.erase(it);
  heap_bytes_ = result.snapshot.heap_bytes;
  spill_bytes_ = result.snapshot.spill_bytes;
  return result;
} catch (const std::bad_alloc&) {
  UnifiedMemorySpillBudgetResult result;
  result.status = ErrorStatus(StatusCode::memory_allocation_failed);
  result.fail_closed = true;
  result.snapshot = Snapshot();
  return result;
}


Status UnifiedMemorySpillBudgetLedger::ReleaseNoAlloc(const QueryMemoryUuid& reservation_id) {
  return ReleaseRetainedNoAlloc(reservation_id, nullptr);
}

Status UnifiedMemorySpillBudgetLedger::ReleaseRetainedNoAlloc(
    const QueryMemoryUuid& reservation_id, const UnifiedMemorySpillBudgetLease::State* owner) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = active_.find(reservation_id);
  if (found == active_.end()) return ErrorStatus(StatusCode::memory_unknown_pointer);
  std::unique_lock<std::mutex> owner_lock;
  if (found->second.retained_owner) {
    owner_lock = std::unique_lock<std::mutex>(found->second.retained_owner->mutex);
    found->second.retained_owner->live = false;
    if (owner != found->second.retained_owner.get())
      return ErrorStatus(StatusCode::memory_invalid_request);
  } else if (owner != nullptr) {
    return ErrorStatus(StatusCode::memory_invalid_request);
  }
  const auto& reservation = found->second.reservation;
  if (reservation.kind == UnifiedMemorySpillBudgetKind::heap) heap_bytes_ -= reservation.bytes;
  else spill_bytes_ -= reservation.bytes;
  active_.erase(found);
  return OkStatus();
}

UnifiedMemorySpillBudgetResult
UnifiedMemorySpillBudgetLedger::ReleaseOwnerReservations(
    const QueryMemoryUuid& owner_scope) try {
  UnifiedMemorySpillBudgetResult result;
  result.status = OkStatus();
  result.evidence.push_back("MMCH_UNIFIED_MEMORY_SPILL_BUDGET");
  result.evidence.push_back(
      "unified_memory_spill.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority");

  std::lock_guard<std::mutex> lock(mutex_);
  if (!scratchbird::core::uuid::IsEngineIdentityUuid(owner_scope)) {
    result.status = ErrorStatus(StatusCode::memory_invalid_request);
    result.fail_closed = true;
    result.snapshot = SnapshotLocked();
    return result;
  }
  u64 released_count = 0;
  result.snapshot = SnapshotLocked();
  for (const auto& [identity, entry] : active_) {
    const auto& reservation = entry.reservation;
    if (reservation.owner_scope != owner_scope) continue;
    if (entry.retained_owner) {
      result.retained = true;
      result.retained_bytes += reservation.bytes;
      continue;
    }
    if (reservation.kind == UnifiedMemorySpillBudgetKind::heap)
      result.snapshot.heap_bytes -= reservation.bytes;
    else
      result.snapshot.spill_bytes -= reservation.bytes;
    result.snapshot.total_bytes -= reservation.bytes;
    --result.snapshot.active_reservation_count;
    ++released_count;
  }
  result.released = released_count != 0;
  if (result.retained) {
    result.status = ErrorStatus(StatusCode::memory_invalid_request);
    result.fail_closed = true;
  }
  result.evidence.push_back("unified_memory_spill.owner_released_count=" +
                            std::to_string(released_count));
  for (auto it = active_.begin(); it != active_.end();) {
    if (it->second.reservation.owner_scope != owner_scope) { ++it; continue; }
    if (it->second.retained_owner) {
      std::lock_guard owner_lock(it->second.retained_owner->mutex);
      it->second.retained_owner->live = false;
      ++it;
    } else {
      it = active_.erase(it);
    }
  }
  heap_bytes_ = result.snapshot.heap_bytes;
  spill_bytes_ = result.snapshot.spill_bytes;
  return result;
} catch (const std::bad_alloc&) {
  UnifiedMemorySpillBudgetResult result;
  result.status = ErrorStatus(StatusCode::memory_allocation_failed);
  result.fail_closed = true;
  result.snapshot = Snapshot();
  return result;
}


DiagnosticRecord UnifiedMemorySpillBudgetLedger::MakeDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string reason,
    const UnifiedMemorySpillBudgetRequest& request) const {
  std::vector<DiagnosticArgument> arguments;
  arguments.push_back({"reason", std::move(reason)});
  arguments.push_back({"ledger_id_present", BoolText(!ledger_id_.is_nil())});
  arguments.push_back({"operation_id_present", BoolText(!request.operation_id.is_nil())});
  arguments.push_back({"owner_scope_present", BoolText(!request.owner_scope.is_nil())});
  arguments.push_back({"kind", UnifiedMemorySpillBudgetKindName(request.kind)});
  arguments.push_back({"requested_bytes", std::to_string(request.bytes)});
  arguments.push_back({"limit_bytes", std::to_string(limit_bytes_)});
  arguments.push_back({"heap_bytes", std::to_string(heap_bytes_)});
  arguments.push_back({"spill_bytes", std::to_string(spill_bytes_)});
  arguments.push_back({"authority_scope",
                       "evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority"});
  return scratchbird::core::platform::MakeDiagnostic(
      status.code,
      status.severity,
      status.subsystem,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(arguments),
      {},
      "core.memory.unified_memory_spill_budget",
      "Lower heap grants, spill less, or cancel the query.");
}

}  // namespace scratchbird::core::memory
