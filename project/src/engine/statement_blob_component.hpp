// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "engine/statement_blob_input.hpp"
#include "core/datatypes/datatype_blob_component.hpp"

namespace scratchbird::engine::internal_api {
// Canonical artifact bytes, NOT a live value, typed wire packet, locator,
// durable publication or permission. The native pin keeps their exact physical
// owner until cleanup even if source wrappers/MGA admission have retired.
class StatementBlobComponentV3 final {
 public:
  StatementBlobComponentV3() noexcept = default;
  ~StatementBlobComponentV3() noexcept;
  StatementBlobComponentV3(const StatementBlobComponentV3&) = delete;
  StatementBlobComponentV3& operator=(const StatementBlobComponentV3&) = delete;
  StatementBlobComponentV3(StatementBlobComponentV3&&) = delete;
  StatementBlobComponentV3& operator=(StatementBlobComponentV3&&) = delete;
  bool bound() const noexcept { return host_ != nullptr; }
  std::span<const core::platform::byte> bytes() const noexcept { return {data_,size_}; }
  // Quiesce artifact users before reset/destruction. No ordinary admission is
  // needed to scrub/free an already-owned artifact. Like the input owner,
  // destruction must occur outside synchronous receipt/scratch lock scopes.
  void Reset() noexcept;
 private:
  core::datatypes::BlobLifetimeReceiverHostV3Generation1* host_ = nullptr;
  core::platform::byte* allocation_ = nullptr;
  core::platform::byte* data_ = nullptr;
  std::size_t size_ = 0, allocated_ = 0;
  friend class core::datatypes::BlobLifetimeReceiverHostV3Generation1;
};
enum class StatementBlobComponentFailureV3 : std::uint8_t {
  none, source_unbound, output_in_use, operation_not_admitted, extent_invalid,
  admission_closed, resource_preflight, allocation_failed, lifetime,
  component_failed, internal_failure
};
struct StatementBlobComponentResultV3 {
  StatementBlobComponentFailureV3 failure = StatementBlobComponentFailureV3::source_unbound;
  bool lifetime_invoked = false;
  core::datatypes::BlobLifetimeRuntimeResultV3 lifetime{};
  core::datatypes::BlobComponentPassResultV3 component{};
  core::datatypes::BlobLifetimeResourceV3 resource{};
  std::uint64_t required = 0, available = 0;
  // Actual protected bytes delivered in each pass and private sink work.
  // These survive all failures, including final source admission failure.
  std::uint64_t first_pass_bytes = 0, second_pass_bytes = 0,
      staged_bytes = 0, sink_attempts = 0;
  bool ok() const noexcept { return failure == StatementBlobComponentFailureV3::none && lifetime_invoked && lifetime.ok(); }
};
} // namespace scratchbird::engine::internal_api
