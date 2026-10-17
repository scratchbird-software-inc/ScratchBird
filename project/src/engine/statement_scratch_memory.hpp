// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "scratchbird/engine/engine.h"
#include "engine/internal_api/api_types.hpp"
#include "core/memory/reservation_backed_memory_resource.hpp"

namespace scratchbird::engine::internal_api {

// Private thread-local observation seam for lock-order conformance. It runs
// before acquisition, grants no ownership and cannot replace admission.
using StatementMemoryBeforeLifetimeLockHookV1=void(*)(void*) noexcept;
void SetThreadStatementMemoryBeforeLifetimeLockHookForTestingV1(
    StatementMemoryBeforeLifetimeLockHookV1,void*) noexcept;


// Issued only inside a synchronous live-receipt call. The owning bridge keeps
// session/receipt guards and an actual MGA pin throughout the callback.
// PMR users must die before the callback returns; this is not a result owner.
class StatementScratchMemoryV1 {
 public:
  virtual ~StatementScratchMemoryV1() = default;
  StatementScratchMemoryV1(const StatementScratchMemoryV1&) = delete;
  StatementScratchMemoryV1& operator=(const StatementScratchMemoryV1&) = delete;
  virtual std::pmr::memory_resource* memory() noexcept = 0;
  // Same actual live receipt, MGA pin and capacity as memory(), but every
  // backing allocation has allocator-issued protected custody and zeroizing
  // release. Borrowers/containers must be destroyed before this call returns.
  // Native default requires actual OS locking AND dump exclusion; failure of
  // either refuses the allocation, never ordinary/best-effort fallback.
  virtual std::pmr::memory_resource* protected_memory() noexcept = 0;
  virtual const EngineRequestContext& context() const noexcept = 0;
  virtual std::uint64_t limit_bytes() const noexcept = 0;
  virtual bool cancelled() noexcept = 0;
  // Read-only lifecycle telemetry, never authorization to release a grant.
  virtual std::uint64_t teardown_requests() const noexcept = 0;
  virtual core::memory::ReservationBackedMemoryResourceSnapshot Snapshot() const = 0;
  static bool Poll(void* owner) noexcept {
    return owner == nullptr || static_cast<StatementScratchMemoryV1*>(owner)->cancelled();
  }
 private:
  StatementScratchMemoryV1() = default;
  friend class NativeStatementScratchMemoryV1;
};
class StatementScratchMemoryAdmissionV1 {
 public:
  virtual ~StatementScratchMemoryAdmissionV1() = default;
  // Scheduling observation only; cannot mint admission or transfer locks.
  virtual bool OwnsCurrentOperationThread(const EngineRequestContext&) const noexcept = 0;
  virtual sb_engine_status_t Run(const EngineRequestContext&,
      const std::function<void(StatementScratchMemoryV1&)>&, core::platform::DiagnosticRecord*) noexcept = 0;
  // Synchronous operation scope, never a transferable result owner. Staging
  // containers may remain live through native DML; nested leaf scratch shares
  // the same actual grant after exact issuer/context validation. Recursive
  // operation scopes refuse. requested_bytes is only a caller ceiling.
  virtual sb_engine_status_t RunOperation(const EngineRequestContext&,
      std::uint64_t requested_bytes,
      const std::function<void(StatementScratchMemoryV1&)>&,
      core::platform::DiagnosticRecord*) noexcept = 0;
 private:
  StatementScratchMemoryAdmissionV1() = default;
  friend class NativeStatementScratchAdmissionV1;
};

// Context projections alone cannot mint this capability. Limits come from the
// configured physical memory manager, not parser flags or optimizer estimates.
inline sb_engine_status_t RunWithStatementScratchMemoryV1(const EngineRequestContext& context,
    const std::function<void(StatementScratchMemoryV1&)>& callback,
    core::platform::DiagnosticRecord* diagnostic = nullptr) noexcept {
  const auto issuer = context.statement_scratch_memory_admission.lock();
  return issuer ? issuer->Run(context, callback, diagnostic) : SB_ENGINE_STATUS_INVALID_HANDLE;
}

inline sb_engine_status_t RunWithStatementOperationMemoryV1(const EngineRequestContext& context,
    std::uint64_t requested_bytes,
    const std::function<void(StatementScratchMemoryV1&)>& callback,
    core::platform::DiagnosticRecord* diagnostic = nullptr) noexcept {
  const auto issuer = context.statement_scratch_memory_admission.lock();
  return issuer ? issuer->RunOperation(context, requested_bytes, callback, diagnostic)
                : SB_ENGINE_STATUS_INVALID_HANDLE;
}

// Ask the actual private issuer rather than introducing a dependency from the
// internal ingestion library back to the public ABI implementation.
inline bool StatementOperationOwnsCurrentThreadV1(const EngineRequestContext& context) noexcept {
  const auto issuer = context.statement_scratch_memory_admission.lock();
  return issuer && issuer->OwnsCurrentOperationThread(context);
}

}  // namespace scratchbird::engine::internal_api
