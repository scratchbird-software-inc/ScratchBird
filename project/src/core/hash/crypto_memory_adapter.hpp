// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "hash_digest_parts.hpp"
#include <atomic>
#include <cstddef>

namespace scratchbird::core::hash {

// This layer consumes backing, never manufactures a memory grant. The issuing
// owner retains the actual grant, pool object and bytes until Close succeeds.
struct CryptoMemoryBinding {
  platform::Uuid database, operation, owner, context;
};
enum class CryptoMemoryError : unsigned char {
  none, invalid_binding, invalid_backing, not_installed, already_installed,
  custom_allocator, late_installation, closed, revoked, busy, provider_failure
};
struct CryptoMemorySnapshot {
  CryptoMemoryBinding binding{};
  std::size_t backing_bytes=0, live_bytes=0, occupied_bytes=0, live_blocks=0, scopes=0;
  u64 allocations=0, refusals=0;
  bool open=false, revoked=false;
  CryptoMemoryError observation_error=CryptoMemoryError::busy;
};
struct CryptoMemoryAdapterSnapshot {
  CryptoMemoryBinding process_binding{};
  std::size_t live_blocks=0, active_scopes=0;
  bool installed=false, stopped=false;
  CryptoMemoryError observation_error=CryptoMemoryError::busy;
};
struct CryptoMemoryInternals;
class CryptoMemoryPool {
 public:
  CryptoMemoryPool() noexcept = default;
  CryptoMemoryPool(const CryptoMemoryPool&)=delete;
  CryptoMemoryPool& operator=(const CryptoMemoryPool&)=delete;
  // Destruction/backing release requires a successful Close. No implicit
  // crypto cleanup or forced release of outstanding provider allocations.
  CryptoMemoryError Open(const CryptoMemoryBinding&,void*,std::size_t) noexcept;
  CryptoMemoryError Close() noexcept;
  CryptoMemoryError Revoke() noexcept;
  CryptoMemorySnapshot Snapshot() const noexcept;
 private:
  struct Block;
  CryptoMemorySnapshot state_{};
  Block* first_=nullptr;
  CryptoMemoryPool* next_=nullptr;
  friend struct CryptoMemoryInternals;
  friend class CryptoMemoryScope;
  friend class PreparedSha256;
  friend CryptoMemoryError InstallCryptoMemoryAdapter(CryptoMemoryPool&) noexcept;
};

// Explicit bootstrap only; call before any OpenSSL allocation or crypto thread.
// No provider/configuration change, heap fallback or automatic runtime wiring.
CryptoMemoryError InstallCryptoMemoryAdapter(CryptoMemoryPool&) noexcept;
// Call AFTER worker joins, thread cleanup and OPENSSL_cleanup. Busy retains
// every pointer/charge. Hooks remain installed in a terminal refusing state.
CryptoMemoryError StopCryptoMemoryAdapter() noexcept;
CryptoMemoryAdapterSnapshot SnapshotCryptoMemoryAdapter() noexcept;

class CryptoMemoryScope {
 public:
  CryptoMemoryScope(CryptoMemoryPool&,const CryptoMemoryBinding&,
                    bool reject_allocations=false) noexcept;
  CryptoMemoryScope(const CryptoMemoryScope&)=delete;
  CryptoMemoryScope& operator=(const CryptoMemoryScope&)=delete;
  ~CryptoMemoryScope();
  bool ok() const noexcept {return error_==CryptoMemoryError::none;}
  CryptoMemoryError error() const noexcept {return error_;}
 private:
  CryptoMemoryScope() noexcept; // Atomically pin the process pool for Prepare.
  CryptoMemoryScope* previous_=nullptr;
  CryptoMemoryPool* pool_=nullptr;
  CryptoMemoryError error_=CryptoMemoryError::not_installed;
  bool reject_=true, counted_=false;
  friend struct CryptoMemoryInternals;
  friend class PreparedSha256;
};

// Prepare/Close outside all subsystem guards. Compute reuses the retained EVP
// provider/context using only the retained pool, with no backing refill.
// EVP may replace its private state during reset. Provider failure poisons the
// context; explicit Prepare is required before retry. A session is single-user.
class PreparedSha256 {
 public:
  PreparedSha256() noexcept = default;
  PreparedSha256(const PreparedSha256&)=delete;
  PreparedSha256& operator=(const PreparedSha256&)=delete;
  ~PreparedSha256();
  CryptoMemoryError Prepare(CryptoMemoryPool&,const CryptoMemoryBinding&) noexcept;
  CryptoMemoryError Close() noexcept;
  Sha256PartsResult Compute(const HashDigestSegment*,std::size_t) noexcept;
 private:
  void Clear() noexcept;
  std::atomic_flag busy_=ATOMIC_FLAG_INIT;
  CryptoMemoryPool* pool_=nullptr;
  CryptoMemoryBinding binding_{};
  void* context_=nullptr;
  bool ready_=false;
};

// Only the current thread's native hash calls are redirected. There is no
// fallback to fresh provider allocation when an installed session fails.
class PreparedSha256Scope {
 public:
  explicit PreparedSha256Scope(PreparedSha256&) noexcept;
  PreparedSha256Scope(const PreparedSha256Scope&)=delete;
  PreparedSha256Scope& operator=(const PreparedSha256Scope&)=delete;
  ~PreparedSha256Scope();
 private:
  PreparedSha256* previous_;
};
PreparedSha256* CurrentPreparedSha256() noexcept;
} // namespace scratchbird::core::hash
