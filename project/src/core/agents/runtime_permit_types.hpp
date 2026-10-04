// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "runtime_platform.hpp"
#include <cstdint>
#include <array>
#include <chrono>
#include <optional>
#include <stop_token>
#include <string_view>
#include <limits>

namespace scratchbird::core::agents {

enum class RuntimePermitProfile { worker_slot, queued_task };
enum class RuntimePermitCode {
  bound, granted, released, closed, invalid_binding, policy_unbound,
  exhausted, allocation_failed, identity_failed, synchronization_failed,
  no_grant, cancelled, timed_out, sequence_exhausted, wait_policy_unbound,
  waiter_exhausted
};

// The real grant path uses this same boundary. Exhaustion is permanent for
// this incarnation; a capacity wake cannot make issuance safe again.
constexpr bool RuntimePermitSequenceAvailable(std::uint64_t issued) noexcept {
  return issued != std::numeric_limits<std::uint64_t>::max();
}

struct RuntimePermitAuthority {
  platform::Uuid database;
  platform::Uuid incarnation;
  platform::Uuid policy;
  std::uint64_t policy_generation = 0;
  bool operator==(const RuntimePermitAuthority&) const = default;
};
// Trusted owning-runtime selection, not a policy authentication receipt.
// Pin exactly once on the node's existing governor; no per-request capacity.
struct RuntimePermitPolicy {
  RuntimePermitAuthority authority;
  std::uint32_t worker_capacity = 0;
  std::uint32_t queue_capacity = 0;
  // Selected bounds on whole blocking calls, including cancellation-callback
  // joining after terminal selection. Zero admits immediate attempts only.
  std::uint32_t worker_waiter_limit = 0;
  std::uint32_t queue_waiter_limit = 0;
};
struct RuntimePermitInstanceBinding {
  platform::Uuid semaphore;
  std::uint64_t generation = 0;
};
struct RuntimePermitRequest {
  RuntimePermitAuthority authority;
  RuntimePermitProfile profile = RuntimePermitProfile::queued_task;
  platform::Uuid task;
  platform::Uuid attempt;
  std::optional<platform::Uuid> worker;
  platform::Uuid semaphore;
  std::uint64_t semaphore_generation = 0;
  std::uint32_t quantity = 0;
  std::uint64_t lease_deadline_tick = 0;
  bool operator==(const RuntimePermitRequest&) const = default;
};
struct RuntimePermitView {
  platform::Uuid governor;
  platform::Uuid grant;
  RuntimePermitRequest binding;
  std::uint64_t issuance = 0;
};

// Acquisition controls, not the resource-use lease in RuntimePermitRequest.
// Checked under grant/close serialization before admission and again after
// fallible metadata preparation. AcquireRuntimePermit never waits. The separate
// bounded WaitAcquire API requires cancellation or a finite, reasoned wait.
// No controls means an immediate capacity attempt, not an uncancellable wait.
struct RuntimePermitAcquireControl {
  std::stop_token cancellation;
  std::optional<std::chrono::steady_clock::time_point> wait_deadline;
};

class ResourceGovernanceReservationLedger;
// One owning release capability. The ledger and its actual governed metadata
// provider must outlive this permit. Quiesce payload/execution and callbacks
// before Release or destruction. A copied View never owns a resource.
// Moves, Release and destruction of one capability are caller-serialized.
// view() is borrowed only until that capability is moved/released/destroyed.
class RuntimePermitGrant {
 public:
  RuntimePermitGrant() = default;
  ~RuntimePermitGrant();
  RuntimePermitGrant(const RuntimePermitGrant&) = delete;
  RuntimePermitGrant& operator=(const RuntimePermitGrant&) = delete;
  RuntimePermitGrant(RuntimePermitGrant&&) noexcept;
  RuntimePermitGrant& operator=(RuntimePermitGrant&&) noexcept;
  explicit operator bool() const noexcept { return ledger_ != nullptr; }
  // Process-local capability provenance, not a copied governor UUID or policy
  // authentication. The ledger must remain alive through the capability.
  bool IssuedBy(const ResourceGovernanceReservationLedger& ledger) const noexcept {
    return ledger_ == &ledger;
  }
  const RuntimePermitView* view() const noexcept { return ledger_ ? &view_ : nullptr; }
  RuntimePermitCode Release(const RuntimePermitRequest& expected) noexcept;
  // Administrative cancellation/expiry is an observation, never uncharging.
  bool QuiescenceRequested() const;
 private:
  ResourceGovernanceReservationLedger* ledger_ = nullptr;
  RuntimePermitView view_;
  void Reset() noexcept;
  friend class ResourceGovernanceReservationLedger;
};
struct RuntimePermitAcquireResult {
  RuntimePermitCode code = RuntimePermitCode::invalid_binding;
  RuntimePermitGrant permit;
  bool ok() const noexcept { return code == RuntimePermitCode::granted && bool(permit); }
};
struct RuntimePermitWaitResult {
  RuntimePermitAcquireResult admission;
  bool registered = false;
  std::uint64_t wait_duration_us = 0;
  std::array<char, 96> uninterruptible_reason{};
  std::uint8_t reason_size = 0;
  std::string_view reason() const noexcept {
    return {uninterruptible_reason.data(), reason_size};
  }
};
// Fixed-size evidence from the actual selected instance. This is neither a
// memory-retirement receipt nor a current policy/security admission receipt.
struct RuntimePermitInstanceSnapshot {
  RuntimePermitCode code = RuntimePermitCode::invalid_binding;
  std::uint32_t capacity = 0;
  std::uint32_t waiter_limit = 0;
  std::uint32_t holders = 0;
  std::uint32_t waiters = 0;
  std::uint32_t wait_calls = 0;
  bool closed = false;
};
struct RuntimePermitDrainResult {
  RuntimePermitCode code = RuntimePermitCode::invalid_binding;
  bool drained = false;
  std::uint64_t wait_duration_us = 0;
  std::array<char, 96> uninterruptible_reason{};
  std::uint8_t reason_size = 0;
  std::string_view reason() const noexcept {
    return {uninterruptible_reason.data(), reason_size};
  }
};
} // namespace scratchbird::core::agents
