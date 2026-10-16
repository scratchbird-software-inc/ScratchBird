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
// Verified logical component bytes and exact expected occurrence metadata,
// NOT a live value/locator/row permission. Installing these as a live value
// still requires the owning native runtime receiving operation.
class StatementBlobDecodedComponentV3 final {
 public:
  StatementBlobDecodedComponentV3() noexcept = default;
  ~StatementBlobDecodedComponentV3() noexcept;
  StatementBlobDecodedComponentV3(const StatementBlobDecodedComponentV3&) = delete;
  StatementBlobDecodedComponentV3& operator=(const StatementBlobDecodedComponentV3&) = delete;
  StatementBlobDecodedComponentV3(StatementBlobDecodedComponentV3&&) = delete;
  StatementBlobDecodedComponentV3& operator=(StatementBlobDecodedComponentV3&&) = delete;
  bool bound() const noexcept { return host_!=nullptr; }
  std::span<const core::platform::byte> bytes() const noexcept { return {data_,size_}; }
  const ExecutionTypeDescriptor* descriptor() const noexcept { return bound() ? &descriptor_ : nullptr; }
  const core::datatypes::BlobValidatedProfileHandleV3* profile() const noexcept { return bound() ? &profile_ : nullptr; }
  std::optional<core::datatypes::BlobValueStateV3> state() const noexcept {
    return bound() ? std::optional{state_} : std::nullopt;
  }
  // Same outside-receipt/scratch-lock and quiesced-user cleanup contract.
  void Reset() noexcept;
 private:
  core::datatypes::BlobLifetimeReceiverHostV3Generation1* host_=nullptr;
  core::platform::byte* allocation_=nullptr;
  core::platform::byte* data_=nullptr;
  std::size_t size_=0,allocated_=0;
  ExecutionTypeDescriptor descriptor_;
  core::datatypes::BlobValidatedProfileHandleV3 profile_{};
  core::datatypes::BlobValueStateV3 state_=core::datatypes::BlobValueStateV3::sql_null;
  friend class core::datatypes::BlobLifetimeReceiverHostV3Generation1;
};
enum class StatementBlobDecodeFailureV3 : std::uint8_t {
  none,source_unbound,output_in_use,descriptor_invalid,operation_not_admitted,
  admission_closed,resource_preflight,allocation_failed,lifetime,component_failed,
  extent_invalid,internal_failure
};
struct StatementBlobDecodeResultV3 {
  StatementBlobDecodeFailureV3 failure=StatementBlobDecodeFailureV3::source_unbound;
  bool lifetime_invoked=false;
  core::datatypes::BlobLifetimeRuntimeResultV3 lifetime{};
  core::datatypes::BlobComponentPassResultV3 component{};
  core::datatypes::BlobLifetimeResourceV3 resource{};
  std::uint64_t required=0,available=0;
  // Protected bytes actually delivered to codec visitors, not an assertion
  // that a later refused reader copied nothing. Shared reader work remains
  // authoritative in the input's actual WorkSnapshot, including discarded work.
  std::uint64_t header_bytes_visited=0,content_bytes_visited=0,staged_bytes=0,sink_attempts=0;
  bool ok() const noexcept { return failure==StatementBlobDecodeFailureV3::none && lifetime_invoked && lifetime.ok(); }
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
// Datatype-owned native typed serialization, not SBPS/SBWP/SBLR lane adoption.
// Owns both the real transferred runtime value and canonical SBBLOB03 bytes.
class StatementBlobSerializedComponentV3 final {
 public:
  StatementBlobSerializedComponentV3() noexcept = default;
  StatementBlobSerializedComponentV3(const StatementBlobSerializedComponentV3&) = delete;
  StatementBlobSerializedComponentV3& operator=(const StatementBlobSerializedComponentV3&) = delete;
  StatementBlobSerializedComponentV3(StatementBlobSerializedComponentV3&&) = delete;
  StatementBlobSerializedComponentV3& operator=(StatementBlobSerializedComponentV3&&) = delete;
  bool bound() const noexcept { return frame_.bound() && value_.bound(); }
  std::span<const core::platform::byte> bytes() const noexcept { return bound() ? frame_.bytes() : std::span<const core::platform::byte>{}; }
  const ExecutionTypeDescriptor* descriptor() const noexcept { return bound() ? value_.descriptor() : nullptr; }
  const core::datatypes::BlobValidatedProfileHandleV3* profile() const noexcept { return bound() ? value_.profile() : nullptr; }
  std::optional<core::datatypes::BlobValueStateV3> state() const noexcept { return bound() ? value_.state() : std::nullopt; }
  core::datatypes::BlobLifetimeRuntimeResultV3 Release() noexcept {
    const auto result=value_.Release(); frame_.Reset(); return result;
  }
 private:
  // Reverse destruction drains the actual runtime lease before its remaining
  // artifact pin, with no borrowed source wrapper or metadata pointer.
  StatementBlobComponentV3 frame_;
  StatementBlobValueV3 value_;
  friend class core::datatypes::BlobLifetimeReceiverHostV3Generation1;
};
} // namespace scratchbird::engine::internal_api
