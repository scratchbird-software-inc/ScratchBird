// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "uuid.hpp"
#include "time.hpp"
#include <mutex>

namespace scratchbird::core::uuid {
struct StandaloneUuidV7Binding {
  Uuid database_uuid;
  Uuid policy_snapshot_uuid;
  bool operator==(const StandaloneUuidV7Binding& other) const noexcept {
    return database_uuid == other.database_uuid && policy_snapshot_uuid == other.policy_snapshot_uuid;
  }
};
struct StandaloneUuidV7Policy {
  time::LocalTimeAuthorityPolicy clock;
  u64 max_uuid_regression_ms = 0;
  u64 same_ms_wait_timeout_ms = 0; // Missing policy is invalid, never a default authority.
};
enum class StandaloneUuidV7Error {
  none, invalid_binding, invalid_policy, wrong_process, invalid_kind,
  clock_failure, timestamp_out_of_range, time_regression,
  randomness_unavailable, sequence_exhausted, resource_exhausted
};
struct StandaloneUuidV7Issue {
  StandaloneUuidV7Error error = StandaloneUuidV7Error::invalid_binding;
  std::optional<TypedUuid> value;
  std::optional<time::ClockSnapshot> observation;
  time::LocalClockObservationDecision clock_decision = time::LocalClockObservationDecision::accepted;
  bool ok() const noexcept { return error == StandaloneUuidV7Error::none && value && observation; }
};

// Trusted owning-kernel policy input, NOT proof of selected catalog authority,
// authentication, a cluster-time provider or permission to publish an identity.
// Retain one instance for its owning allocation lifetime, not one per identity.
// Inherited instances refuse in a forked child before touching their mutex.
class StandaloneUuidV7Issuer {
 public:
  StandaloneUuidV7Issuer(StandaloneUuidV7Binding, StandaloneUuidV7Policy);
  StandaloneUuidV7Issuer(const StandaloneUuidV7Issuer&) = delete;
  StandaloneUuidV7Issuer& operator=(const StandaloneUuidV7Issuer&) = delete;
  const StandaloneUuidV7Binding& binding() const noexcept { return binding_; }
  StandaloneUuidV7Issue Issue(UuidKind) noexcept;

 private:
  const StandaloneUuidV7Binding binding_;
  const StandaloneUuidV7Policy policy_;
  const u64 process_;
  const StandaloneUuidV7Error admission_;
  std::mutex mutex_;
  time::LocalTimeAuthorityState clock_;
  std::optional<u64> last_millis_;
  Uuid last_value_;
};
} // namespace scratchbird::core::uuid
