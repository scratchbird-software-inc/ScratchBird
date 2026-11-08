// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// SEARCH_KEY: SB_CAREFUL_WRITE_PRECEDENCE
// Process-loss sinks for careful-write tests (fork + _exit). They model
// process death only: the kernel page cache survives, so a test using them
// proves issue order and format invariants for every write prefix, not power
// loss or device reordering.
#include "careful_write.hpp"

#include <unistd.h>

#include <algorithm>
#include <span>

namespace scratchbird::tests {

// Passes every call to the inner sink, but _exits with `exit_code` after
// `limit` writes: before the first write when the limit is 0, otherwise right
// after the limit-th.
class CrashAfterWritesSink final : public scratchbird::storage::disk::CarefulWriteSink {
 public:
  using BoundedIoResult = scratchbird::storage::disk::BoundedIoResult;
  using byte = scratchbird::core::platform::byte;
  using u32 = scratchbird::core::platform::u32;
  using u64 = scratchbird::core::platform::u64;
  CrashAfterWritesSink(scratchbird::storage::disk::CarefulWriteSink& inner, u32 limit, int exit_code)
      : inner_(inner), limit_(limit), exit_code_(exit_code) {}
  BoundedIoResult WriteAt(u64 offset, std::span<const byte> bytes) override {
    if (writes_ == limit_) _exit(exit_code_);
    const auto r = inner_.WriteAt(offset, bytes);
    if (r.ok() && ++writes_ == limit_) _exit(exit_code_);
    return r;
  }
  BoundedIoResult ReadAt(u64 offset, std::span<byte> bytes) override { return inner_.ReadAt(offset, bytes); }
  BoundedIoResult Sync() override { return inner_.Sync(); }
  bool forced_ordered() const noexcept override { return inner_.forced_ordered(); }
  u32 writes() const noexcept { return writes_; }

 private:
  scratchbird::storage::disk::CarefulWriteSink& inner_;
  u32 limit_;
  int exit_code_;
  u32 writes_ = 0;
};

// Like CrashAfterWritesSink, but the write numbered `torn` (1-based) reaches
// the inner sink only as its first half before the process exits: a write
// torn at the half-way boundary.
class TornWriteSink final : public scratchbird::storage::disk::CarefulWriteSink {
 public:
  using BoundedIoResult = scratchbird::storage::disk::BoundedIoResult;
  using byte = scratchbird::core::platform::byte;
  using u32 = scratchbird::core::platform::u32;
  using u64 = scratchbird::core::platform::u64;
  TornWriteSink(scratchbird::storage::disk::CarefulWriteSink& inner, u32 torn, int exit_code)
      : inner_(inner), torn_(torn), exit_code_(exit_code) {}
  BoundedIoResult WriteAt(u64 offset, std::span<const byte> bytes) override {
    if (++writes_ == torn_) {
      (void)inner_.WriteAt(offset, bytes.first(bytes.size() / 2));
      _exit(exit_code_);
    }
    return inner_.WriteAt(offset, bytes);
  }
  BoundedIoResult ReadAt(u64 offset, std::span<byte> bytes) override { return inner_.ReadAt(offset, bytes); }
  BoundedIoResult Sync() override { return inner_.Sync(); }
  bool forced_ordered() const noexcept override { return inner_.forced_ordered(); }

 private:
  scratchbird::storage::disk::CarefulWriteSink& inner_;
  u32 torn_;
  int exit_code_;
  u32 writes_ = 0;
};

}  // namespace scratchbird::tests
