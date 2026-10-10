// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "crypto_memory_adapter.hpp"
#include "reservation_backed_memory_resource.hpp"
#include <exception>
#include <new>
#include <system_error>
#include <type_traits>
#include <utility>

namespace scratchbird::core::runtime {
enum class RuntimeCryptoPoolError {
  none, invalid_grant, invalid_binding, already_owned, allocation_failed,
  memory_failure, adapter_failure, synchronization_failure
};
struct RuntimeCryptoPoolResult {
  RuntimeCryptoPoolError error = RuntimeCryptoPoolError::none;
  hash::CryptoMemoryError adapter_error = hash::CryptoMemoryError::none;
  platform::Status memory_status{};
  bool ok() const noexcept { return error == RuntimeCryptoPoolError::none; }
};

// Runtime custody only, never a grant issuer or a process-shutdown receipt.
// Lifecycle calls are externally serialized. Join all pool users before Close;
// the issuing manager/ledger must outlive this nonmovable owner. A busy/error
// close retains custody for retry. Destruction cannot force-free provider data.
class RuntimeCryptoPoolOwner {
 public:
  RuntimeCryptoPoolOwner() = default;
  RuntimeCryptoPoolOwner(const RuntimeCryptoPoolOwner&) = delete;
  RuntimeCryptoPoolOwner& operator=(const RuntimeCryptoPoolOwner&) = delete;
  ~RuntimeCryptoPoolOwner() { if (grant_) std::terminate(); }

  // Validation failure leaves the caller's grant unchanged. Once validated,
  // custody transfers even if allocation/Open fails: this object then owns
  // the explicit Close retry. No fixed capacity or replacement identity.
  RuntimeCryptoPoolResult Adopt(const hash::CryptoMemoryBinding& binding,
      std::unique_ptr<memory::ReservationBackedMemoryResource>& grant,
      platform::usize bytes) noexcept {
    if (grant_) return {RuntimeCryptoPoolError::already_owned};
    for (const auto& id : {binding.database, binding.operation, binding.owner, binding.context})
      if (!memory::MemorySystemUuidValid(id.bytes)) return {RuntimeCryptoPoolError::invalid_binding};
    try {
      if (!grant || !grant->active() || !bytes) return {RuntimeCryptoPoolError::invalid_grant};
      const auto& request = grant->request();
      if (request.binary_operation_uuid != binding.operation.bytes ||
          request.binary_ownership[memory::MemoryBinaryScopeKind::database] != binding.database.bytes ||
          request.binary_ownership[memory::MemoryBinaryScopeKind::owner] != binding.owner.bytes ||
          request.binary_ownership[memory::MemoryBinaryScopeKind::context] != binding.context.bytes ||
          !request.operation_id.empty() || !request.owner_id.empty())
        return {RuntimeCryptoPoolError::invalid_binding};
      const auto snapshot = grant->Snapshot();
      if (bytes > snapshot.reserved_bytes || snapshot.allocation_count != 0)
        return {RuntimeCryptoPoolError::invalid_grant};
      grant_ = std::move(grant);
      binding_ = binding;
      auto allocation = grant_->Allocate({bytes, alignof(std::max_align_t), "runtime crypto backing"});
      if (!allocation.ok()) return {RuntimeCryptoPoolError::memory_failure,
                                   hash::CryptoMemoryError::none, allocation.status};
      // Fixed ownership fields are installed before any subsequent fallible call.
      backing_ = allocation.pointer; bytes_ = allocation.bytes; alignment_ = allocation.alignment;
      const auto error = pool_.Open(binding_, backing_, bytes_);
      open_ = error == hash::CryptoMemoryError::none;
      return Adapter(error);
    } catch (const std::bad_alloc&) { return {RuntimeCryptoPoolError::allocation_failed}; }
      catch (const std::system_error&) { return {RuntimeCryptoPoolError::synchronization_failure}; }
  }

  RuntimeCryptoPoolResult InstallProcessAdapter() noexcept {
    const auto checked = Check(binding_);
    if (!checked.ok()) return checked;
    return Adapter(hash::InstallCryptoMemoryAdapter(pool_));
  }
  RuntimeCryptoPoolResult Prepare(hash::PreparedSha256& session,
      const hash::CryptoMemoryBinding& expected) noexcept {
    const auto checked = Check(expected);
    if (!checked.ok()) return checked;
    return Adapter(session.Prepare(pool_, expected));
  }
  // Execute a bounded provider operation under this owner's actual grant.
  // The callback handles its own algorithm result; successful scope admission
  // is not cryptographic success or result publication. No pool pointer or
  // scope escapes this call. Callback exceptions propagate after unpinning.
  // As with Prepare, callers serialize owner lifecycle calls against entry.
  // Provider allocations retained by the callback still prevent Close even
  // after the scope ends; the owner must drain them before releasing custody.
  template <typename Work>
    requires std::is_same_v<std::invoke_result_t<Work>, void>
  RuntimeCryptoPoolResult WithMemoryScope(
      const hash::CryptoMemoryBinding& expected, Work&& work) {
    const auto checked = Check(expected);
    if (!checked.ok()) return checked;
    hash::CryptoMemoryScope scope(pool_, expected);
    if (!scope.ok()) return Adapter(scope.error());
    std::forward<Work>(work)();
    return {};
  }
  RuntimeCryptoPoolResult Check(const hash::CryptoMemoryBinding& expected) const noexcept {
    if (expected.database != binding_.database || expected.operation != binding_.operation ||
        expected.owner != binding_.owner || expected.context != binding_.context)
      return {RuntimeCryptoPoolError::invalid_binding};
    try {
      if (!grant_ || !open_ || !grant_->active()) return {RuntimeCryptoPoolError::invalid_grant};
      const auto snapshot = pool_.Snapshot();
      if (snapshot.observation_error != hash::CryptoMemoryError::none) return Adapter(snapshot.observation_error);
      if (snapshot.revoked) return Adapter(hash::CryptoMemoryError::revoked);
      return {};
    } catch (const std::system_error&) { return {RuntimeCryptoPoolError::synchronization_failure}; }
  }
  RuntimeCryptoPoolResult Revoke() noexcept {
    if (!grant_ || !open_) return {RuntimeCryptoPoolError::invalid_grant};
    return Adapter(pool_.Revoke());
  }
  RuntimeCryptoPoolResult Close() noexcept {
    if (!grant_) return {};
    try {
      if (open_) {
        const auto error = pool_.Close();
        if (error != hash::CryptoMemoryError::none) return Adapter(error);
        open_ = false;
      }
      if (backing_) {
        const auto status = grant_->DeallocateNoAlloc(backing_, bytes_, alignment_);
        if (!status.ok()) return {RuntimeCryptoPoolError::memory_failure, hash::CryptoMemoryError::none, status};
        backing_ = nullptr; bytes_ = alignment_ = 0;
      }
      const auto status = grant_->ReleaseNoAlloc();
      if (!status.ok()) return {RuntimeCryptoPoolError::memory_failure, hash::CryptoMemoryError::none, status};
      grant_.reset(); binding_ = {};
      return {};
    } catch (const std::system_error&) { return {RuntimeCryptoPoolError::synchronization_failure}; }
  }
  bool has_custody() const noexcept { return bool(grant_); }
  platform::usize retained_backing_bytes() const noexcept { return bytes_; }
  hash::CryptoMemorySnapshot Snapshot() const noexcept { return pool_.Snapshot(); }

 private:
  static RuntimeCryptoPoolResult Adapter(hash::CryptoMemoryError error) noexcept {
    return {error == hash::CryptoMemoryError::none ? RuntimeCryptoPoolError::none :
            RuntimeCryptoPoolError::adapter_failure, error};
  }
  std::unique_ptr<memory::ReservationBackedMemoryResource> grant_;
  hash::CryptoMemoryBinding binding_{};
  hash::CryptoMemoryPool pool_;
  void* backing_ = nullptr;
  platform::usize bytes_ = 0, alignment_ = 0;
  bool open_ = false;
};
} // namespace scratchbird::core::runtime
