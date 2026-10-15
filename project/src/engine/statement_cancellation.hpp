// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "engine/statement_context_receipt_retention.hpp"

namespace scratchbird::engine::internal_api {
struct StatementCancellationChannelStateV1;
struct StatementCancellationBindingV1 {
  wire::TypedResultUuid database{}, session{}, transaction{}, statement{}, receipt{};
  bool operator==(const StatementCancellationBindingV1&) const = default;
};
enum class StatementCancellationObservationV1 : std::uint8_t {
  not_requested = 0, requested, source_unavailable
};

// Owns ONLY a cooperative cancellation signal. Not statement/MGA/security,
// content, memory, finality or proof that an operation actually stopped.
// Only a real live opaque receipt can issue it. Already-issued owners may
// outlive public retirement/session deletion; they retain no raw session or
// borrowed callback. Source destruction requires quiesced users as usual.
class RetainedStatementCancellationV1 final {
 public:
  ~RetainedStatementCancellationV1() = default;
  RetainedStatementCancellationV1(const RetainedStatementCancellationV1&) = delete;
  RetainedStatementCancellationV1& operator=(const RetainedStatementCancellationV1&) = delete;
  RetainedStatementCancellationV1(RetainedStatementCancellationV1&&) noexcept = default;
  RetainedStatementCancellationV1& operator=(RetainedStatementCancellationV1&&) noexcept = default;
  // Null for a moved-from/disarmed owner; no fabricated zero identity view.
  const StatementCancellationBindingV1* binding() const noexcept;
  // Atomic, nonallocating, no callbacks/receipt locks. True means the request
  // changed the signal, not cancellation completion or rollback/no effects.
  bool Request() noexcept;
  // An unsampled legacy lexical probe is NOT false cancellation. Never calls
  // that probe. A real positive signal remains observable after retirement.
  StatementCancellationObservationV1 Observe() const noexcept;
 private:
  explicit RetainedStatementCancellationV1(
      std::shared_ptr<StatementCancellationChannelStateV1>) noexcept;
  std::shared_ptr<StatementCancellationChannelStateV1> state_;
  friend sb_engine_status_t RetainStatementCancellationV1(
      server_engine_bridge::StatementContextReceiptHandle,
      const wire::TypedResultUuid&, const wire::TypedResultUuid&,
      std::optional<RetainedStatementCancellationV1>*) noexcept;
};
sb_engine_status_t RetainStatementCancellationV1(
    server_engine_bridge::StatementContextReceiptHandle,
    const wire::TypedResultUuid& expected_session,
    const wire::TypedResultUuid& expected_receipt,
    std::optional<RetainedStatementCancellationV1>* output) noexcept;
} // namespace scratchbird::engine::internal_api

namespace scratchbird::server_engine_bridge {
// Exact authenticated private-server receipt tuple. No caller cancellation
// predicate/boolean is authority. Does not wait for the executing receipt lock
// or mutate MGA finality. A successful request is not completion/no-effects.
// Public retirement closes this lookup; existing retained signal owners remain
// independently usable. Refuses synchronous dispatch/scratch reentry before
// touching the registry; a retained signal's atomic Request remains usable.
sb_engine_status_t RequestStatementContextCancellationV1(
    StatementContextReceiptHandle,
    const wire::TypedResultUuid& expected_session,
    const wire::TypedResultUuid& expected_receipt) noexcept;
} // namespace scratchbird::server_engine_bridge
