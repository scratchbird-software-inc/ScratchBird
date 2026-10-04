// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cstdint>

namespace scratchbird::core::concurrency {
// Process-local scheduler states, NOT an on-disk/wire encoding or agent state.
enum class RuntimeTaskState : std::uint8_t {
  created, policy_checking, admitted, queued, waiting_resource, waiting_lock,
  waiting_horizon, waiting_cluster, running, yielding, throttled, paused,
  cancelling, cancelled, completed, failed, retry_scheduled, review_required,
  recovery_required
};

// A required owning guard, never evidence that the guard passed. In particular,
// queue/worker grants, current authority, safe-boundary and durable/evidence
// acknowledgements cannot be supplied by this finite topology classifier.
enum class RuntimeTaskTransitionGuard : std::uint8_t {
  invalid, descriptor_identity, policy_admission, policy_review, admission_failure,
  initial_queue_admission, admission_wait, queued_dependency_wait, wait_requeue,
  dispatch_claim, yield_request, yield_requeue, yielded_pause, retain_claim_throttle,
  retained_claim_resume, running_pause, paused_requeue, cancel_before_execution,
  cancel_execution, cancellation_acknowledgement, completion_acknowledgement,
  settled_failure, retry_admission, retry_requeue, review_classification,
  recovery_classification
};

constexpr bool RuntimeTaskStateKnown(RuntimeTaskState state) noexcept {
  return static_cast<std::uint8_t>(state) <=
      static_cast<std::uint8_t>(RuntimeTaskState::recovery_required);
}

constexpr bool RuntimeTaskStateSettled(RuntimeTaskState state) noexcept {
  return state == RuntimeTaskState::completed || state == RuntimeTaskState::cancelled ||
      state == RuntimeTaskState::failed;
}

// SEARCH_KEY: RUNTIME_TASK_TRANSITION_TOPOLOGY
// DR-MGA-SCHED-001 / SB-MGA-SCHED-QUEUE-BEFORE-EXECUTION. No mutation occurs here.
// Callers must serialize actual admission/dispatch/cancellation, validate every
// owning guard and preserve resources/effects before publishing a transition.
// Repeated requests are observations, not self-transitions. Invalid pairs require
// the owning diagnostic path to emit MGA.SCHED.TASK_STATE_INVALID.
constexpr RuntimeTaskTransitionGuard ClassifyRuntimeTaskTransition(
    RuntimeTaskState from, RuntimeTaskState to) noexcept {
  using S = RuntimeTaskState;
  using G = RuntimeTaskTransitionGuard;
  if (!RuntimeTaskStateKnown(from) || !RuntimeTaskStateKnown(to) || from == to)
    return G::invalid;
  if (from == S::failed)
    return to == S::retry_scheduled ? G::retry_admission : G::invalid;
  if (RuntimeTaskStateSettled(from)) return G::invalid;
  if (to == S::review_required)
    return from == S::policy_checking ? G::policy_review : G::review_classification;
  if (to == S::recovery_required) return G::recovery_classification;
  if (from == S::review_required || from == S::recovery_required)
    return G::invalid; // A wake is not an owning review/recovery disposition.
  if (to == S::failed) {
    if (from == S::policy_checking) return G::admission_failure;
    return from == S::cancelling ? G::invalid : G::settled_failure;
  }
  if (to == S::cancelling) {
    switch (from) {
      case S::created: case S::policy_checking: case S::admitted: case S::queued:
      case S::waiting_resource: case S::waiting_lock: case S::waiting_horizon:
      case S::waiting_cluster: case S::retry_scheduled: return G::cancel_before_execution;
      case S::running: case S::yielding: case S::throttled: case S::paused:
        return G::cancel_execution;
      default: return G::invalid;
    }
  }
  switch (from) {
    case S::created:
      return to == S::policy_checking ? G::descriptor_identity : G::invalid;
    case S::policy_checking:
      return to == S::admitted ? G::policy_admission : G::invalid;
    case S::admitted: case S::queued:
      if (from == S::admitted && to == S::queued) return G::initial_queue_admission;
      if (from == S::queued && to == S::running) return G::dispatch_claim;
      if (to == S::waiting_resource || to == S::waiting_lock ||
          to == S::waiting_horizon || to == S::waiting_cluster)
        return from == S::admitted ? G::admission_wait : G::queued_dependency_wait;
      return G::invalid;
    case S::waiting_resource: case S::waiting_lock:
    case S::waiting_horizon: case S::waiting_cluster:
      return to == S::queued ? G::wait_requeue : G::invalid;
    case S::running:
      if (to == S::yielding) return G::yield_request;
      if (to == S::throttled) return G::retain_claim_throttle;
      if (to == S::paused) return G::running_pause;
      return to == S::completed ? G::completion_acknowledgement : G::invalid;
    case S::yielding:
      if (to == S::queued) return G::yield_requeue;
      return to == S::paused ? G::yielded_pause : G::invalid;
    case S::throttled:
      if (to == S::running) return G::retained_claim_resume;
      return to == S::yielding ? G::yield_request : G::invalid;
    case S::paused: return to == S::queued ? G::paused_requeue : G::invalid;
    case S::cancelling:
      return to == S::cancelled ? G::cancellation_acknowledgement : G::invalid;
    case S::retry_scheduled: return to == S::queued ? G::retry_requeue : G::invalid;
    default: return G::invalid;
  }
}
} // namespace scratchbird::core::concurrency
