// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "engine/statement_blob_input.hpp"

namespace scratchbird::engine::internal_api {
enum class StatementBlobRowFailureV3 : std::uint8_t {
  none, source_unbound, output_in_use, operation_not_admitted, admission_closed,
  extent_invalid, resource_preflight, allocation_failed, lifetime, internal_failure
};
struct StatementBlobRowResultV3 {
  StatementBlobRowFailureV3 failure = StatementBlobRowFailureV3::source_unbound;
  bool lifetime_invoked = false;
  core::datatypes::BlobLifetimeRuntimeResultV3 lifetime{};
  core::datatypes::BlobLifetimeResourceV3 resource{};
  std::uint64_t required = 0, available = 0, bytes_visited = 0, sink_attempts = 0;
  bool ok() const noexcept {
    return failure == StatementBlobRowFailureV3::none && lifetime_invoked && lifetime.ok();
  }
};

// Private immutable row-input staging, not a row permission, stored locator,
// generic V1/V2 value or publication receipt. Owns exact logical bytes plus
// the actual transferred typed lease. The destination operation must freshly
// admit its column, exact origin statement, resources and MGA effects. A
// matching origin projection alone is never that admission.
class StatementBlobRowValueV3 final {
 public:
  StatementBlobRowValueV3() noexcept = default;
  ~StatementBlobRowValueV3() noexcept;
  StatementBlobRowValueV3(const StatementBlobRowValueV3&) = delete;
  StatementBlobRowValueV3& operator=(const StatementBlobRowValueV3&) = delete;
  StatementBlobRowValueV3(StatementBlobRowValueV3&&) = delete;
  StatementBlobRowValueV3& operator=(StatementBlobRowValueV3&&) = delete;
  bool bound() const noexcept { return host_ && value_.bound(); }
  std::span<const core::platform::byte> bytes() const noexcept {
    return bound() ? std::span<const core::platform::byte>{data_, size_} : std::span<const core::platform::byte>{};
  }
  const ExecutionTypeDescriptor* descriptor() const noexcept { return bound() ? value_.descriptor() : nullptr; }
  const core::datatypes::BlobValidatedProfileHandleV3* profile() const noexcept { return bound() ? value_.profile() : nullptr; }
  std::optional<core::datatypes::BlobValueStateV3> state() const noexcept { return bound() ? value_.state() : std::nullopt; }
  // A borrowed observation of the actual retained owner's immutable binding,
  // never a copied authority or independent owner. The row's existing custody
  // pin retains it; it cannot outlive this wrapper's synchronous use.
  const RetainedStatementMemoryBindingV1* origin() const noexcept {
    return bound() ? origin_ : nullptr;
  }
  core::datatypes::BlobLifetimeRuntimeResultV3 Probe() noexcept { return value_.Probe(); }
  // Available only when the actual original native operation admitted HASH.
  StatementBlobHashResultV3 HashLogical() noexcept;
  // Quiesce callers, release outside synchronous receipt/scratch lock scopes.
  // One wrapper lifecycle; release never rearms it for another input.
  core::datatypes::BlobLifetimeRuntimeResultV3 Release() noexcept;
 private:
  StatementBlobValueV3 value_;
  core::datatypes::BlobLifetimeReceiverHostV3Generation1* host_ = nullptr;
  const RetainedStatementMemoryBindingV1* origin_ = nullptr;
  core::platform::byte* allocation_ = nullptr;
  core::platform::byte* data_ = nullptr;
  std::size_t size_ = 0, allocated_ = 0;
  friend class core::datatypes::BlobLifetimeReceiverHostV3Generation1;
};
} // namespace scratchbird::engine::internal_api
