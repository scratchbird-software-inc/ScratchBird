// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "engine/statement_cancellation.hpp"
#include "core/time/time.hpp"

namespace scratchbird::engine::internal_api {
struct StatementMonotonicClockBindingV1 {
  StatementCancellationBindingV1 owner;
  wire::TypedResultUuid clock{};
  std::uint64_t clock_generation = 0;
  bool operator==(const StatementMonotonicClockBindingV1&) const = default;
};
enum class StatementMonotonicClockReadErrorV1 : std::uint8_t {
  none, owner_unavailable, process_mismatch, source_unavailable,
  invalid_sample, out_of_range
};
struct StatementMonotonicClockSampleV1 {
  StatementMonotonicClockReadErrorV1 error = StatementMonotonicClockReadErrorV1::owner_unavailable;
  std::uint64_t nanoseconds = 0;
  bool ok() const noexcept { return error == StatementMonotonicClockReadErrorV1::none; }
};

// The actual receipt issuer alone creates this owner. It retains the same
// physically charged native control source as cancellation, not a raw receipt,
// session, callback or serialized assertion. Clock identity is independent of
// receipt/service identity. Not security, token, MGA or BLOB-host admission.
class RetainedStatementMonotonicClockV1 final {
 public:
  ~RetainedStatementMonotonicClockV1() = default;
  RetainedStatementMonotonicClockV1(const RetainedStatementMonotonicClockV1&) = delete;
  RetainedStatementMonotonicClockV1& operator=(const RetainedStatementMonotonicClockV1&) = delete;
  RetainedStatementMonotonicClockV1(RetainedStatementMonotonicClockV1&&) noexcept = default;
  RetainedStatementMonotonicClockV1& operator=(RetainedStatementMonotonicClockV1&&) noexcept = default;
  const StatementMonotonicClockBindingV1* binding() const noexcept;
  // No heap, locks, receipt lookup, UUID issuance or diagnostics. Equal ticks
  // and UINT64_MAX are valid. No clamping, fabricated progress or wall fallback.
  // Caller owns per-operation regression detection and deadline semantics.
  StatementMonotonicClockSampleV1 Sample() const noexcept;
 private:
  explicit RetainedStatementMonotonicClockV1(
      std::shared_ptr<StatementCancellationChannelStateV1>) noexcept;
  std::shared_ptr<StatementCancellationChannelStateV1> state_;
  friend sb_engine_status_t RetainStatementMonotonicClockV1(
      server_engine_bridge::StatementContextReceiptHandle,
      const wire::TypedResultUuid&, const wire::TypedResultUuid&,
      std::optional<RetainedStatementMonotonicClockV1>*) noexcept;
};
sb_engine_status_t RetainStatementMonotonicClockV1(
    server_engine_bridge::StatementContextReceiptHandle,
    const wire::TypedResultUuid& expected_session,
    const wire::TypedResultUuid& expected_receipt,
    std::optional<RetainedStatementMonotonicClockV1>* output) noexcept;
} // namespace scratchbird::engine::internal_api
