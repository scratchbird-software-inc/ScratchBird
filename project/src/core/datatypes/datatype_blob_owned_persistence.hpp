// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "datatype_blob_component.hpp"
#include "reservation_backed_memory_resource.hpp"
#include <stop_token>

namespace scratchbird::core::datatypes {

// Standalone datatype bytes, NEVER live BLOB/runtime/transaction authority.
// Source and destination use separate actual grants. The caller must keep each
// grant's memory manager and ledger alive, and quiesce users before destruction.
// No writable byte view, caller reader/sink, or borrowed source pointer escapes.
class BlobOwnedPersistenceBytesV3 final {
 public:
  BlobOwnedPersistenceBytesV3() noexcept = default;
  ~BlobOwnedPersistenceBytesV3() noexcept;
  BlobOwnedPersistenceBytesV3(const BlobOwnedPersistenceBytesV3&) = delete;
  BlobOwnedPersistenceBytesV3& operator=(const BlobOwnedPersistenceBytesV3&) = delete;
  BlobOwnedPersistenceBytesV3(BlobOwnedPersistenceBytesV3&&) = delete;
  BlobOwnedPersistenceBytesV3& operator=(BlobOwnedPersistenceBytesV3&&) = delete;
  bool bound() const noexcept { return resource_ != nullptr; }
  std::span<const byte> bytes() const noexcept { return {data_, size_}; }
 private:
  std::unique_ptr<memory::ReservationBackedMemoryResource> resource_;
  byte* allocation_ = nullptr;
  byte* data_ = nullptr;
  std::size_t size_ = 0, allocated_ = 0;
  friend class BlobOwnedPersistenceCodecV3;
};

class BlobOwnedPersistenceDecodedV3 final {
 public:
  BlobValueStateV3 state() const noexcept { return state_; }
  const BlobOwnedPersistenceBytesV3& content() const noexcept { return content_; }
 private:
  BlobValueStateV3 state_ = BlobValueStateV3::value;
  BlobOwnedPersistenceBytesV3 content_;
  friend class BlobOwnedPersistenceCodecV3;
};

enum class BlobOwnedPersistenceFailureV3 : u8 {
  none = 0, invalid_argument, output_in_use, invalid_grant,
  allocation_failed, cancelled, component_failed, internal_failure
};
struct BlobOwnedPersistenceControlV3 {
  std::size_t chunk_bytes = 65536;
  std::stop_token cancellation;
};
struct BlobOwnedPersistenceResultV3 {
  BlobOwnedPersistenceFailureV3 failure = BlobOwnedPersistenceFailureV3::none;
  BlobComponentPassResultV3 component;
  platform::DiagnosticRecord memory_diagnostic;
  // Actual source/content work; encoded header staging is counted separately.
  u64 digest_bytes = 0, copied_bytes = 0, header_bytes_written = 0;
  bool ok() const noexcept { return failure == BlobOwnedPersistenceFailureV3::none; }
};

// Moves the actual grant ONLY on success, publishes output once, scrubs and
// rolls back every provisional allocation on failure. Private statuses do not
// manufacture a public diagnostic. Component failures keep exact existing
// field/digest vectors; memory failures keep the actual allocator diagnostic.
BlobOwnedPersistenceResultV3 CopyBlobPersistenceBytesV3(
    std::unique_ptr<memory::ReservationBackedMemoryResource>& grant,
    std::span<const byte> source, BlobOwnedPersistenceBytesV3& output,
    const BlobOwnedPersistenceControlV3& control = {}) noexcept;
BlobOwnedPersistenceResultV3 EncodeOwnedBlobComponentV3(
    const BlobValidatedProfileHandleV3&, BlobValueStateV3,
    const BlobOwnedPersistenceBytesV3& source,
    std::unique_ptr<memory::ReservationBackedMemoryResource>& grant,
    BlobOwnedPersistenceBytesV3& output,
    const BlobOwnedPersistenceControlV3& control = {}) noexcept;
BlobOwnedPersistenceResultV3 DecodeOwnedBlobComponentV3(
    const BlobValidatedProfileHandleV3&,
    const BlobOwnedPersistenceBytesV3& source, bool null_allowed,
    std::unique_ptr<memory::ReservationBackedMemoryResource>& grant,
    BlobOwnedPersistenceDecodedV3& output,
    const BlobOwnedPersistenceControlV3& control = {}) noexcept;

} // namespace scratchbird::core::datatypes
