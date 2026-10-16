// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "engine/statement_blob_input.hpp"

namespace scratchbird::engine::internal_api {
class StatementBlobDecodedComponentV3;
struct StatementBlobDecodeResultV3;
// Encoded artifacts have an unsigned encoded extent, not SQL state or logical
// BLOB length. Copying/authenticating an artifact never validates its format.
struct StatementBlobEncodedInputBoundsV3 {
  std::uint8_t operation = 0; // Actual encoded operations9..13 only.
  std::uint64_t resident_bytes = 0, valid_for_ns = 0;
  std::uint32_t retain_tickets = 0, access_tickets = 0;
  std::array<std::uint64_t,11> maximum_work{};
  std::uint64_t scratch_bytes = 0, window_bytes = 0, relative_timeout_ns = 0;
};
class StatementBlobEncodedInputV3 final {
 public:
  StatementBlobEncodedInputV3() noexcept = default;
  ~StatementBlobEncodedInputV3() noexcept;
  StatementBlobEncodedInputV3(const StatementBlobEncodedInputV3&) = delete;
  StatementBlobEncodedInputV3& operator=(const StatementBlobEncodedInputV3&) = delete;
  StatementBlobEncodedInputV3(StatementBlobEncodedInputV3&&) = delete;
  StatementBlobEncodedInputV3& operator=(StatementBlobEncodedInputV3&&) = delete;
  // Stable-wrapper concurrency and outside-receipt/scratch-lock rules match
  // the native logical input. Independent leases retain their actual backing.
  void Reset() noexcept;
  bool bound() const noexcept { return host_ != nullptr; }
  const core::datatypes::BlobValidatedProfileHandleV3* profile() const noexcept;
  std::optional<std::uint64_t> extent() const noexcept;
  std::optional<std::array<core::datatypes::BlobLifetimeCounterV3,12>> WorkSnapshot() noexcept;
  StatementBlobInputRetainV3 Retain(core::datatypes::BlobRetainedLifetimeLeaseV3&) noexcept;
  StatementBlobDecodeResultV3 DecodeComponent(
      ExecutionTypeDescriptor&& target,StatementBlobDecodedComponentV3&) noexcept;
 private:
  core::datatypes::BlobLifetimeReceiverHostV3Generation1* host_ = nullptr;
  friend StatementBlobInputAdmissionV3 ReceiveStatementBlobEncodedInputV3(
      server_engine_bridge::StatementContextReceiptHandle,
      const wire::TypedResultUuid&, const wire::TypedResultUuid&,
      std::span<const core::platform::byte>, const StatementBlobEncodedInputBoundsV3&,
      StatementBlobEncodedInputV3*, core::platform::DiagnosticRecord*) noexcept;
};
// Known statement-supplied encoded bytes, not a locator/file/row permission or
// an assertion that a component/backup is valid. No arbitrary reader callback.
StatementBlobInputAdmissionV3 ReceiveStatementBlobEncodedInputV3(
    server_engine_bridge::StatementContextReceiptHandle,
    const wire::TypedResultUuid& expected_session,
    const wire::TypedResultUuid& expected_receipt,
    std::span<const core::platform::byte>, const StatementBlobEncodedInputBoundsV3&,
    StatementBlobEncodedInputV3*, core::platform::DiagnosticRecord* diagnostic = nullptr) noexcept;
} // namespace scratchbird::engine::internal_api
