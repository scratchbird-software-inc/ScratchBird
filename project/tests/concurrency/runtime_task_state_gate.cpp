// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "runtime_task_state.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string_view>

namespace c = scratchbird::core::concurrency;
using S = c::RuntimeTaskState;
using G = c::RuntimeTaskTransitionGuard;
using Row = std::array<G,19>;
using Matrix = std::array<Row,19>;

// Independent declarative oracle from the accepted transition table. Never use
// the production classifier to construct expected edges or expected guards.
constexpr Matrix Expected() {
  Matrix matrix{};
  const auto edge = [&](S from, S to, G guard) {
    auto& value = matrix[static_cast<unsigned>(from)][static_cast<unsigned>(to)];
    if (value != G::invalid) std::abort();
    value = guard;
  };
  edge(S::created,S::policy_checking,G::descriptor_identity);
  edge(S::policy_checking,S::admitted,G::policy_admission);
  edge(S::policy_checking,S::failed,G::admission_failure);
  edge(S::admitted,S::queued,G::initial_queue_admission);
  for (auto target : {S::waiting_resource,S::waiting_lock,S::waiting_horizon,S::waiting_cluster}) {
    edge(S::admitted,target,G::admission_wait);
    edge(S::queued,target,G::queued_dependency_wait);
    edge(target,S::queued,G::wait_requeue);
  }
  edge(S::queued,S::running,G::dispatch_claim);
  edge(S::running,S::yielding,G::yield_request);
  edge(S::yielding,S::queued,G::yield_requeue);
  edge(S::yielding,S::paused,G::yielded_pause);
  edge(S::running,S::throttled,G::retain_claim_throttle);
  edge(S::throttled,S::running,G::retained_claim_resume);
  edge(S::throttled,S::yielding,G::yield_request);
  edge(S::running,S::paused,G::running_pause);
  edge(S::paused,S::queued,G::paused_requeue);
  for (auto from : {S::created,S::policy_checking,S::admitted,S::queued,S::waiting_resource,
                   S::waiting_lock,S::waiting_horizon,S::waiting_cluster,S::retry_scheduled})
    edge(from,S::cancelling,G::cancel_before_execution);
  for (auto from : {S::running,S::yielding,S::throttled,S::paused})
    edge(from,S::cancelling,G::cancel_execution);
  edge(S::cancelling,S::cancelled,G::cancellation_acknowledgement);
  edge(S::running,S::completed,G::completion_acknowledgement);
  for (auto from : {S::created,S::admitted,S::queued,S::waiting_resource,S::waiting_lock,
                   S::waiting_horizon,S::waiting_cluster,S::running,S::yielding,S::throttled,
                   S::paused,S::retry_scheduled})
    edge(from,S::failed,G::settled_failure);
  edge(S::failed,S::retry_scheduled,G::retry_admission);
  edge(S::retry_scheduled,S::queued,G::retry_requeue);
  for (auto from : {S::created,S::policy_checking,S::admitted,S::queued,S::waiting_resource,
                   S::waiting_lock,S::waiting_horizon,S::waiting_cluster,S::running,S::yielding,
                   S::throttled,S::paused,S::cancelling,S::retry_scheduled,
                   S::review_required,S::recovery_required}) {
    if (from != S::review_required)
      edge(from,S::review_required,from == S::policy_checking ? G::policy_review : G::review_classification);
    if (from != S::recovery_required) edge(from,S::recovery_required,G::recovery_classification);
  }
  return matrix;
}
constexpr auto expected = Expected();
constexpr unsigned EdgeCount() {
  unsigned count = 0;
  for (const auto& row : expected) for (auto guard : row) count += guard != G::invalid;
  return count;
}
static_assert(EdgeCount() == 84);
static_assert(noexcept(c::ClassifyRuntimeTaskTransition(S::created,S::queued)));
static_assert(c::ClassifyRuntimeTaskTransition(S::admitted,S::running) == G::invalid);
static_assert(c::ClassifyRuntimeTaskTransition(S::queued,S::running) == G::dispatch_claim);
static_assert(c::ClassifyRuntimeTaskTransition(S::cancelling,S::completed) == G::invalid);

int main(int argc, char** argv) {
  const bool dump = argc == 2 && std::string_view(argv[1]) == "--edges";
  if (argc != 1 && !dump) return 2;
  constexpr std::array names{"created","policy_checking","admitted","queued",
      "waiting_resource","waiting_lock","waiting_horizon","waiting_cluster",
      "running","yielding","throttled","paused","cancelling","cancelled",
      "completed","failed","retry_scheduled","review_required","recovery_required"};
  unsigned checks = 0;
  for (unsigned from = 0; from != 256; ++from) {
    if (c::RuntimeTaskStateKnown(static_cast<S>(from)) != (from < 19)) return 1;
    const bool settled = from == 13 || from == 14 || from == 15;
    if (c::RuntimeTaskStateSettled(static_cast<S>(from)) != settled) return 1;
    for (unsigned to = 0; to != 256; ++to) {
      const auto want = from < 19 && to < 19 ? expected[from][to] : G::invalid;
      const auto actual = c::ClassifyRuntimeTaskTransition(static_cast<S>(from),static_cast<S>(to));
      if (dump && actual != G::invalid && from < 19 && to < 19)
        std::printf("EDGE %s %s\n",names[from],names[to]);
      if (actual != want) {
        std::fprintf(stderr,"transition mismatch from=%u to=%u expected=%u actual=%u\n",
            from,to,static_cast<unsigned>(want),static_cast<unsigned>(actual));
        return 1;
      }
      ++checks;
    }
  }
  std::printf("PASS task topology: %u pairs;84 guarded edges;277 rejected known pairs;unknown states refused\n",checks);
}
