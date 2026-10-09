// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/memory/memory.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>

namespace scratchbird::engine::internal_api::session {

using Identity = core::memory::MemoryBinaryUuid;
enum class ValueFamily : std::uint8_t { setting, qualifier, application_context };
enum class ValueScope : std::uint8_t { session, transaction };
struct ValueKey {
  ValueFamily family = ValueFamily::application_context;
  Identity identity{};
  bool operator==(const ValueKey&) const = default;
};

// An internal retention carrier, NOT a datatype or authorization admission API.
// Its caller must already have admitted the exact descriptor, native payload,
// namespace, scope, privileges, policy and audit. No text conversion takes place
// here. A binary value may contain any bytes, including older-version user UUIDs.
struct AdmittedValueView {
  Identity datatype_uuid{};
  std::uint64_t datatype_generation = 0;
  Identity security_label_uuid{};
  bool is_null = false;
  std::span<const std::uint8_t> bytes;
};

enum class StoreStatus : std::uint8_t {
  ok, not_found, invalid_argument, owner_mismatch, closed, stale_generation,
  generation_exhausted, transaction_missing, transaction_exists,
  savepoint_missing, savepoint_exists, quota_exceeded, memory_refused,
  output_too_small
};
struct StoreResult {
  StoreStatus status = StoreStatus::invalid_argument;
  std::uint64_t generation = 0;
  bool ok() const { return status == StoreStatus::ok; }
};
struct ValueReadResult : StoreResult {
  Identity datatype_uuid{};
  std::uint64_t datatype_generation = 0;
  Identity security_label_uuid{};
  bool is_null = false;
  ValueScope scope = ValueScope::session;
  std::size_t bytes_required = 0;
};
struct StoreSnapshot : StoreResult {
  std::size_t resident_bytes = 0;  // Includes the index and rollback history.
  std::size_t retained_versions = 0;
  std::size_t active_transactions = 0;
  std::size_t savepoints = 0;
};
struct StoreLimits {
  // Bounded retention limits, not replacements for namespace/configuration
  // admission limits. Old versions consume the same budget as current values.
  std::size_t resident_bytes = 32 * 1024 * 1024;
  std::size_t retained_versions = 4096;
  std::size_t active_transactions = 64;
  std::size_t savepoints = 256;
};

// Binary-bound, memory-manager-accounted ephemeral state. Session writes are
// independent of transaction rollback; an override expires at BOTH terminal
// outcomes, revealing the current session value. Savepoints roll back only
// their transaction's versions. No durable/MGA-finality claim is made here.
// All operations serialize on this store's mutex. Owner teardown must retain
// its session lifetime lease until calls complete before destroying the store.
class ScopedValueStore final {
 public:
  // Internal lifecycle participant. Retains the state lock across the owning
  // MGA decision; destruction alone preserves all overrides. Only a confirmed
  // native terminal publication may call ApplyKnownTerminal(). The enclosing
  // session lifetime lease must outlive this guard. Never transport this guard.
  class TerminalGuard {
   public:
    TerminalGuard(TerminalGuard&&) = default;
    TerminalGuard& operator=(TerminalGuard&&) = delete;
    TerminalGuard(const TerminalGuard&) = delete;
    StoreResult admission() const { return admission_; }
    void ApplyKnownTerminal() noexcept;
   private:
    friend class ScopedValueStore;
    TerminalGuard(ScopedValueStore&, const Identity&, const Identity&);
    ScopedValueStore* store_;
    std::unique_lock<std::mutex> lock_;
    Identity transaction_;
    StoreResult admission_;
    bool applied_ = false;
  };
  struct Deleter { void operator()(ScopedValueStore*) const noexcept; };
  using Owner = std::unique_ptr<ScopedValueStore, Deleter>;
  struct Creation { Owner owner; StoreStatus status = StoreStatus::invalid_argument; };
  static Creation Create(core::memory::BoundedAllocator&, const Identity& database,
                         const Identity& session, StoreLimits = {});
  ScopedValueStore(const ScopedValueStore&) = delete;
  ScopedValueStore& operator=(const ScopedValueStore&) = delete;
  bool BoundTo(const Identity& database, const Identity& session) const noexcept {
    return database_ == database && session_ == session;
  }

  StoreSnapshot Snapshot(const Identity& session) const;
  StoreResult BeginTransaction(const Identity& session, const Identity& transaction,
                               std::uint64_t expected_generation);
  // Called only after the owning MGA lifecycle has confirmed a terminal result.
  // An uncertain commit/rollback is not permission to expire an override.
  StoreResult EndTransaction(const Identity& session, const Identity& transaction,
                             std::uint64_t expected_generation);
  TerminalGuard PrepareTerminal(const Identity& session, const Identity& transaction);
  StoreResult Savepoint(const Identity& session, const Identity& transaction,
                        const Identity& savepoint, std::uint64_t expected_generation);
  StoreResult RollbackTo(const Identity& session, const Identity& transaction,
                         const Identity& savepoint, std::uint64_t expected_generation);
  StoreResult ReleaseSavepoint(const Identity& session, const Identity& transaction,
                               const Identity& savepoint, std::uint64_t expected_generation);

  StoreResult WriteAdmitted(const Identity& session, const ValueKey&, ValueScope,
                            const Identity& transaction, std::uint64_t expected_generation,
                            const AdmittedValueView&);
  // Removing a transaction override reveals the session value, but preserves
  // its prior override in rollback history while a savepoint can require it.
  StoreResult EraseAdmitted(const Identity& session, const ValueKey&, ValueScope,
                            const Identity& transaction, std::uint64_t expected_generation);
  // Copies into caller-owned, pre-admitted memory. No returned pointer can
  // outlive the lock or retain an unaccounted copy of a private value.
  ValueReadResult ReadAdmitted(const Identity& session, const ValueKey&,
                               const Identity& transaction, std::uint64_t expected_generation,
                               std::span<std::uint8_t> output) const;
  StoreResult Reset(const Identity& session, std::uint64_t expected_generation);
  StoreResult Close(const Identity& session);

 private:
  struct Version;
  struct Transaction;
  struct Mark;
  ScopedValueStore(core::memory::BoundedAllocator&, Identity, Identity, StoreLimits);
  ~ScopedValueStore();
  StoreStatus Check(const Identity&, std::uint64_t expected, bool mutation) const;
  StoreStatus CheckScope(ValueScope, const Identity&) const;
  StoreResult Result(StoreStatus status) const { return {status, generation_}; }
  Transaction* FindTransaction(const Identity&) const;
  Mark* FindMark(const Identity&, const Identity&) const;
  Version* FindVersion(const ValueKey&, const Identity&) const;
  static std::size_t Bucket(const ValueKey&);
  void* Allocate(std::size_t, const Identity& transaction);
  void Free(void*, std::size_t) noexcept;
  void Remove(Version*) noexcept;
  void Remove(Mark*) noexcept;
  void Compact(const Identity&) noexcept;
  void CompactKey(const ValueKey&, const Identity&) noexcept;
  void Clear() noexcept;
  void ExpireTransaction(Transaction*) noexcept;
  StoreResult Mutate(const Identity&, const ValueKey&, ValueScope, const Identity&,
                     std::uint64_t expected, const AdmittedValueView*, bool erase);

  core::memory::BoundedAllocator& allocator_;
  const Identity database_;
  const Identity session_;
  const StoreLimits limits_;
  mutable std::mutex mutex_;
  bool closed_ = false;
  std::uint64_t generation_ = 1;
  std::size_t resident_bytes_ = sizeof(ScopedValueStore);
  std::size_t version_count_ = 0;
  std::size_t transaction_count_ = 0;
  std::size_t mark_count_ = 0;
  std::array<Version*, 256> buckets_{};
  Version* versions_ = nullptr;
  Transaction* transactions_ = nullptr;
  Mark* marks_ = nullptr;
};

}  // namespace scratchbird::engine::internal_api::session
