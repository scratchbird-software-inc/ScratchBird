// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "crypto_memory_adapter.hpp"
#include "reservation_backed_memory_resource.hpp"
#include <exception>
#include <new>
#include <optional>
#include <system_error>
#include <type_traits>
#include <utility>

namespace scratchbird::core::runtime {
enum class RuntimeCryptoPoolError {
  none, invalid_grant, invalid_binding, already_owned, allocation_failed,
  memory_failure, adapter_failure, synchronization_failure, invalid_protection_policy
};
struct RuntimeCryptoPoolResult {
  RuntimeCryptoPoolError error = RuntimeCryptoPoolError::none;
  hash::CryptoMemoryError adapter_error = hash::CryptoMemoryError::none;
  platform::Status memory_status{};
  // Original redacted native allocator cause, moved without a second admission
  // or allocation. A generic crypto error must not replace protection details.
  platform::DiagnosticRecord memory_diagnostic;
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
  // the explicit Close retry. No fixed capacity or replacement identity. The
  // owning runtime must explicitly select its admitted platform policy; there
  // is no implicit best-effort/strict profile or inferred OS-lock guarantee.
  RuntimeCryptoPoolResult Adopt(const hash::CryptoMemoryBinding& binding,
      std::unique_ptr<memory::ReservationBackedMemoryResource>& grant,
      platform::usize bytes, memory::ProtectedMemoryPlatformPolicy protection) noexcept {
    if (grant_) return {RuntimeCryptoPoolError::already_owned};
    if (!hash::ValidCryptoMemoryBinding(binding)) return {RuntimeCryptoPoolError::invalid_binding};
    if (protection != memory::ProtectedMemoryPlatformPolicy::best_effort &&
        protection != memory::ProtectedMemoryPlatformPolicy::require_platform_lock &&
        protection != memory::ProtectedMemoryPlatformPolicy::require_no_dump &&
        protection != memory::ProtectedMemoryPlatformPolicy::require_lock_and_no_dump)
      return {RuntimeCryptoPoolError::invalid_protection_policy};
    try {
      if (!grant || !grant->active() || !bytes) return {RuntimeCryptoPoolError::invalid_grant};
      const auto& request = grant->request();
      if (request.binary_operation_uuid != binding.operation.bytes ||
          request.binary_ownership[memory::MemoryBinaryScopeKind::database] != binding.database.bytes ||
          request.binary_ownership[memory::MemoryBinaryScopeKind::process] != binding.process.bytes ||
          request.binary_ownership[memory::MemoryBinaryScopeKind::owner] != binding.owner.bytes ||
          request.binary_ownership[memory::MemoryBinaryScopeKind::context] != binding.context.bytes ||
          !request.operation_id.empty() || !request.owner_id.empty())
        return {RuntimeCryptoPoolError::invalid_binding};
      // Shared admission checks supplied scope tags; this narrower process
      // owner additionally requires a singleton process-only charged chain.
      if (binding.process != platform::Uuid{} &&
          (request.scope_chain.size() != 1 ||
           request.scope_chain.front().kind != memory::HierarchicalMemoryScopeKind::process ||
           !request.scope_chain.front().scope_id.empty() ||
           request.scope_chain.front().binary_scope_uuid != binding.process.bytes))
        return {RuntimeCryptoPoolError::invalid_binding};
      const auto snapshot = grant->Snapshot();
      if (bytes > snapshot.reserved_bytes || snapshot.allocation_count != 0)
        return {RuntimeCryptoPoolError::invalid_grant};
      grant_ = std::move(grant);
      binding_ = binding;
      protection_ = protection;
      // The SAME retained capacity and immutable binary owner supply actual
      // protected custody. The selected policy is not a promise that this OS
      // locked/excluded dumps; the allocator enforces required profiles and
      // reports actual stronger alignment and originating policy binding.
      auto allocation = grant_->AllocateProtected(
          {bytes, alignof(std::max_align_t), "runtime crypto backing"}, protection);
      if (!allocation.ok()) return {RuntimeCryptoPoolError::memory_failure,
                                   hash::CryptoMemoryError::none, allocation.status,
                                   std::move(allocation.diagnostic)};
      // Fixed ownership fields are installed before any subsequent fallible call.
      backing_ = allocation.pointer; bytes_ = allocation.bytes; alignment_ = allocation.alignment;
      policy_binding_ = allocation.policy_binding;
      const auto error = pool_.Open(binding_, backing_, bytes_);
      open_ = error == hash::CryptoMemoryError::none;
      return Adapter(error);
    } catch (const std::bad_alloc&) { return {RuntimeCryptoPoolError::allocation_failed}; }
      catch (const std::system_error&) { return {RuntimeCryptoPoolError::synchronization_failure}; }
  }

  RuntimeCryptoPoolResult InstallProcessAdapter() noexcept {
    if (binding_.process == platform::Uuid{}) return {RuntimeCryptoPoolError::invalid_binding};
    const auto checked = Check(binding_);
    if (!checked.ok()) return checked;
    return Adapter(hash::InstallCryptoMemoryAdapter(pool_));
  }
  RuntimeCryptoPoolResult CheckProcessAdapter(
      const hash::CryptoMemoryBinding& expected) const noexcept {
    if (expected.process == platform::Uuid{}) return {RuntimeCryptoPoolError::invalid_binding};
    const auto checked = Check(expected);
    if (!checked.ok()) return checked;
    return Adapter(hash::CheckCryptoMemoryAdapter(pool_, expected));
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
    if (expected != binding_)
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
        policy_binding_.reset();
      }
      const auto status = grant_->ReleaseNoAlloc();
      if (!status.ok()) return {RuntimeCryptoPoolError::memory_failure, hash::CryptoMemoryError::none, status};
      grant_.reset(); binding_ = {}; protection_.reset();
      return {};
    } catch (const std::system_error&) { return {RuntimeCryptoPoolError::synchronization_failure}; }
  }
  bool has_custody() const noexcept { return bool(grant_); }
  platform::usize retained_backing_bytes() const noexcept { return bytes_; }
  platform::usize backing_alignment() const noexcept { return alignment_; }
  const std::optional<memory::MemoryPolicyBinding>& backing_policy_binding() const noexcept {
    return policy_binding_;
  }
  const std::optional<memory::ProtectedMemoryPlatformPolicy>& protection_policy() const noexcept { return protection_; }
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
  std::optional<memory::MemoryPolicyBinding> policy_binding_;
  std::optional<memory::ProtectedMemoryPlatformPolicy> protection_;
  bool open_ = false;
};
} // namespace scratchbird::core::runtime
