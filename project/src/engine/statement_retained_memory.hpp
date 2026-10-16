// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "engine/statement_context_receipt_retention.hpp"
#include "core/memory/reservation_backed_memory_resource.hpp"

namespace scratchbird::engine::internal_api {

// Observed projections, never transferable statement/security/MGA authority.
struct RetainedStatementMemoryBindingV1 {
  wire::TypedResultUuid database{}, session{}, transaction{}, statement{}, receipt{},
      snapshot{}, security_context{}, resource_admission{};
  std::uint64_t local_transaction_id = 0, snapshot_generation = 0,
      security_generation = 0;
  // The persisted datatype cohort is separate from the statement catalog
  // epoch. These are observations of the actual private receipt, not values
  // supplied by a caller's mutable EngineRequestContext.
  wire::TypedResultUuid datatype_catalog_snapshot{};
  std::uint64_t datatype_catalog_generation = 0, datatype_registry_generation = 0;
};
enum class RetainedStatementMemoryObservationV1 : std::uint8_t {
  live = 0, receipt_revoked, snapshot_revoked, released, unavailable
};
class RetainedStatementMemoryV1 final {
 public:
  ~RetainedStatementMemoryV1();
  RetainedStatementMemoryV1(const RetainedStatementMemoryV1&) = delete;
  RetainedStatementMemoryV1& operator=(const RetainedStatementMemoryV1&) = delete;
  // Containers/payload users must quiesce before destroying this owner. Memory
  // remains owned after public receipt retirement and after forced revocation;
  // revocation refuses NEW allocation, but cleanup can still deallocate.
  // Payload/telemetry operations must run outside synchronous receipt/scratch
  // lock scopes. In that scope the PMR and Snapshot refuse by bad_alloc,
  // Observe returns unavailable, and Release returns CONFLICT before any lock.
  std::pmr::memory_resource* memory() noexcept;
  const RetainedStatementMemoryBindingV1& binding() const noexcept;
  core::memory::ReservationBackedMemoryResourceSnapshot Snapshot() const;
  core::memory::ReservationBackedPmrMemoryResourceSnapshot AllocationSnapshot() const;
  RetainedStatementMemoryObservationV1 Observe() const noexcept;
  // Refuses without effects while any payload allocation remains. On success
  // releases physical capacity BEFORE the actual statement/MGA owners. The PMR
  // proxy remains callable for refusal; it cannot reactivate a closed grant.
  sb_engine_status_t Release() noexcept;
 private:
  struct State;
  explicit RetainedStatementMemoryV1(std::unique_ptr<State>) noexcept;
  static void DrainDeferred() noexcept;
  std::unique_ptr<State> state_;
  friend class RetainedStatementMemoryCleanupScopeV1;
  friend sb_engine_status_t RetainStatementMemoryForResultV1(
      server_engine_bridge::StatementContextReceiptHandle,
      const wire::TypedResultUuid&, const wire::TypedResultUuid&,
      std::uint64_t, std::unique_ptr<RetainedStatementMemoryV1>*,
      core::platform::DiagnosticRecord*) noexcept;
};

// Actual private receipt lookup/retention plus a real physical reservation.
// Neither caller-implemented virtual handles nor context booleans are inputs.
// The byte count is a bound/request, not a resource grant. Zero refuses.
// Cancellation/protected-content authorization remain separate owning services.
// Call outside a synchronous dispatch/scratch lock scope: that scope must not
// recursively lend its receipt lock or scratch allocation to a retained owner.
sb_engine_status_t RetainStatementMemoryForResultV1(
    server_engine_bridge::StatementContextReceiptHandle,
    const wire::TypedResultUuid& expected_session_uuid,
    const wire::TypedResultUuid& expected_receipt_uuid,
    std::uint64_t requested_bytes,
    std::unique_ptr<RetainedStatementMemoryV1>* out,
    core::platform::DiagnosticRecord* diagnostic = nullptr) noexcept;

} // namespace scratchbird::engine::internal_api
