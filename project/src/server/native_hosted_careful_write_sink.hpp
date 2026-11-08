// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_hosted_database_custody.hpp"
#include "../storage/disk/careful_write.hpp"

namespace scratchbird::server {

// SEARCH_KEY: SB_TRANSACTION_INVENTORY_WRITER
// The careful-write sink of a hosted database: every read, write and sync goes
// through the custody's retained writer operation (Operation::ReadAtBounded,
// WriteAtBounded, Sync) on one filespace, so the owned writer's own refusals
// apply: a write while a read lease is live, a second I/O at once, a write to
// a read-only member, a write past the end of the file. A refusal before any
// I/O comes back as a failed receipt (native_attempted false). The custody's
// operation does not report whether the device forces ordered writes, so this
// sink reports false and a careful-write plan syncs between dependent writes.
// The operation must outlive the sink, and only its own thread may use it.
class NativeHostedCarefulWriteSink final : public storage::disk::CarefulWriteSink {
 public:
  NativeHostedCarefulWriteSink(NativeHostedDatabaseCustody::Operation& operation, const core::platform::Uuid& filespace,
                               const core::platform::Uuid& profile) noexcept
      : operation_(operation), filespace_(filespace), profile_(profile) {}
  NativeHostedCarefulWriteSink(const NativeHostedCarefulWriteSink&) = delete;
  NativeHostedCarefulWriteSink& operator=(const NativeHostedCarefulWriteSink&) = delete;
  storage::disk::BoundedIoResult WriteAt(core::platform::u64 offset,
                                         std::span<const core::platform::byte> bytes) override {
    return Receipt(operation_.WriteAtBounded(filespace_, profile_, offset, bytes),
                   storage::disk::BoundedIoOperation::write, offset, bytes.size());
  }
  storage::disk::BoundedIoResult ReadAt(core::platform::u64 offset, std::span<core::platform::byte> bytes) override {
    return Receipt(operation_.ReadAtBounded(filespace_, profile_, offset, bytes),
                   storage::disk::BoundedIoOperation::read, offset, bytes.size());
  }
  storage::disk::BoundedIoResult Sync() override {
    return Receipt(operation_.Sync(filespace_, profile_), storage::disk::BoundedIoOperation::sync, 0, 0);
  }
  bool forced_ordered() const noexcept override { return false; }
  // The owned writer's refusal of the last call, when it refused before any I/O.
  storage::database::NativeWriterError last_writer_error() const noexcept { return last_; }

 private:
  storage::disk::BoundedIoResult Receipt(const storage::database::NativeWriterIoResult& r,
                                         storage::disk::BoundedIoOperation kind, core::platform::u64 offset,
                                         std::size_t bytes) noexcept {
    last_ = r.error;
    if (r.io && (r.ok() || !r.io->ok())) return *r.io;
    // Refused before any I/O, or refused although the transfer reported success.
    storage::disk::BoundedIoResult refused = r.io.value_or(storage::disk::BoundedIoResult{});
    refused.operation = kind;
    refused.offset = offset;
    refused.requested_bytes = bytes;
    if (!r.io) refused.bytes_transferred = 0;
    refused.error = storage::disk::BoundedIoError::native_failure;
    return refused;
  }
  NativeHostedDatabaseCustody::Operation& operation_;
  core::platform::Uuid filespace_, profile_;
  storage::database::NativeWriterError last_ = storage::database::NativeWriterError::none;
};

}  // namespace scratchbird::server
