// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "engine/statement_retained_memory.hpp"
#include "core/datatypes/datatype_blob.hpp"

namespace scratchbird::engine::internal_api {

// Engine-private receiving of bytes already supplied as a statement operand.
// This is not a row locator, SELECT permission or a persisted-source reader.
// The owning native operation still admits its own type occurrence, function,
// policy and provider. This constructor does not admit a parser/wire envelope.
// Only the real opaque statement receipt can issue its lifetime owner. All
// work/destruction occurs outside synchronous receipt/scratch lock scopes.
enum class StatementBlobInputCarrierV3 : std::uint8_t {
  materialized = 1, logical_reader = 2
};
struct StatementBlobInputBoundsV3 {
  std::uint8_t operation = 0;
  StatementBlobInputCarrierV3 carrier = StatementBlobInputCarrierV3::materialized;
  core::datatypes::BlobValueStateV3 state = core::datatypes::BlobValueStateV3::value;
  // Requests/bounds, never authority or permission to enlarge runtime grants.
  std::uint64_t resident_bytes = 0, valid_for_ns = 0;
  std::uint32_t retain_tickets = 0, access_tickets = 0;
  std::array<std::uint64_t,11> maximum_work{};
  std::uint64_t scratch_bytes = 0, window_bytes = 0, relative_timeout_ns = 0;
};
struct StatementBlobInputRetainV3 {
  sb_engine_status_t status = SB_ENGINE_STATUS_INVALID_HANDLE;
  bool runtime_invoked = false;
  core::datatypes::BlobLifetimeRuntimeResultV3 runtime{};
  bool ok() const noexcept { return status == SB_ENGINE_STATUS_OK && runtime_invoked && runtime.ok(); }
};
// Private receiving facts. No public BLOB diagnostic is fabricated before its
// lifetime factory was invoked. Native memory diagnostics remain in the
// separate optional owning diagnostic output. Work counts survive failure.
enum class StatementBlobInputAdmissionFailureV3 : std::uint8_t {
  none, invalid_request, output_in_use, memory_admission, datatype_cohort,
  cancellation_admission, clock_admission, owner_binding, clock_unavailable,
  clock_regression, cancellation_unavailable, cancellation_requested,
  deadline_overflow, deadline_expired, source_unavailable, identity_source,
  execution_bounds, allocation_failed, internal_failure
};
struct StatementBlobInputAdmissionV3 {
  sb_engine_status_t status = SB_ENGINE_STATUS_INVALID_ARGUMENT;
  StatementBlobInputAdmissionFailureV3 failure = StatementBlobInputAdmissionFailureV3::invalid_request;
  std::uint64_t copied_bytes = 0, identities_issued = 0;
  bool ok() const noexcept { return status == SB_ENGINE_STATUS_OK && failure == StatementBlobInputAdmissionFailureV3::none; }
};

class StatementBlobInputV3 final {
 public:
  StatementBlobInputV3() noexcept = default;
  ~StatementBlobInputV3() noexcept;
  StatementBlobInputV3(const StatementBlobInputV3&) = delete;
  StatementBlobInputV3& operator=(const StatementBlobInputV3&) = delete;
  // Concurrent Retain/profile calls require a stable wrapper. Quiesce calls
  // into this wrapper before Reset/destruction; neither races its host pointer.
  // Independently retained leases may outlive the wrapper. Reset closes new
  // admission while their actual pins retain backing and cleanup duty.
  void Reset() noexcept;
  bool bound() const noexcept { return host_ != nullptr; }
  const core::datatypes::BlobValidatedProfileHandleV3* profile() const noexcept;
  StatementBlobInputRetainV3 Retain(core::datatypes::BlobRetainedLifetimeLeaseV3&) noexcept;
 private:
  core::datatypes::BlobLifetimeReceiverHostV3Generation1* host_ = nullptr;
  friend StatementBlobInputAdmissionV3 ReceiveStatementBlobInputV3(
      server_engine_bridge::StatementContextReceiptHandle,
      const wire::TypedResultUuid&, const wire::TypedResultUuid&,
      std::span<const core::platform::byte>, const StatementBlobInputBoundsV3&,
      StatementBlobInputV3*, core::platform::DiagnosticRecord*) noexcept;
};

// Copies into actually admitted private physical memory. Raw input must remain
// immutable for this synchronous copy only. No borrowed input or byte pointer
// is returned. Failure preserves the destination and publishes no partial owner.
StatementBlobInputAdmissionV3 ReceiveStatementBlobInputV3(
    server_engine_bridge::StatementContextReceiptHandle,
    const wire::TypedResultUuid& expected_session,
    const wire::TypedResultUuid& expected_receipt,
    std::span<const core::platform::byte> input,
    const StatementBlobInputBoundsV3&, StatementBlobInputV3*,
    core::platform::DiagnosticRecord* diagnostic = nullptr) noexcept;
} // namespace scratchbird::engine::internal_api
