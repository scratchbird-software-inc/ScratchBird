// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "core/agents/resource_governance_admission.hpp"
#include "server_engine_bridge/statement_context.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
namespace agents = scratchbird::core::agents;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "IA-01 package resource governance: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

agents::ResourceGovernanceQuotaDescriptor Descriptor() {
  agents::ResourceGovernanceQuotaDescriptor descriptor;
  descriptor.descriptor_id =
      "engine.runtime.sblr_package.query_memory_arena.v1";
  descriptor.family = agents::ResourceGovernanceFamily::kQueryMemoryArena;
  descriptor.source =
      agents::ResourceGovernanceDescriptorSource::kRuntimePolicy;
  descriptor.source_path_or_label =
      "engine.builtin.runtime_policy.sblr_package.v1";
  descriptor.descriptor_generation = 7;
  descriptor.expected_generation = 7;
  descriptor.over_limit_action = agents::ResourceGovernanceAction::kFailClosed;
  descriptor.benchmark_clean = true;
  descriptor.runtime_dependency_present = true;
  descriptor.limits.memory_bytes = 256 * 1024 * 1024;
  descriptor.limits.device_memory_bytes = 1;
  descriptor.limits.pinned_memory_bytes = 1;
  descriptor.limits.io_bytes = 64 * 1024 * 1024;
  descriptor.limits.io_ops = 1'000'000;
  descriptor.limits.worker_threads = 64;
  descriptor.limits.backlog_items = 4096;
  descriptor.limits.candidate_rows = 1'000'000;
  descriptor.limits.cache_entries = 1'000'000;
  descriptor.limits.batch_rows = 1'000'000;
  descriptor.limits.fragments = 262144;
  descriptor.limits.lanes = 64;
  descriptor.limits.time_budget_microseconds = 300'000'000;
  return descriptor;
}

agents::ResourceGovernanceReservationAcquireRequest Request(
    std::string_view owner) {
  agents::ResourceGovernanceReservationAcquireRequest request;
  request.admission.operation_id = "engine.op.package_begin_end";
  request.admission.expected_family =
      agents::ResourceGovernanceFamily::kQueryMemoryArena;
  request.admission.descriptor = Descriptor();
  request.admission.requested.memory_bytes = 64 * 1024 * 1024 + 4096;
  request.admission.requested.io_bytes = 4096;
  request.admission.requested.io_ops = 1;
  request.admission.requested.worker_threads = 1;
  request.admission.requested.backlog_items = 1;
  request.admission.requested.candidate_rows = 1;
  request.admission.requested.cache_entries = 1;
  request.admission.requested.batch_rows = 1;
  request.admission.requested.fragments = 3;
  request.admission.requested.lanes = 1;
  request.admission.requested.time_budget_microseconds = 1;
  request.owner_scope = owner;
  return request;
}
}  // namespace

int main() {
  using scratchbird::server_engine_bridge::
      ClassifyStatementPackageReservationId;
  using scratchbird::server_engine_bridge::
      StatementPackageReservationIdAdmission;
  Require(ClassifyStatementPackageReservationId(0, false) ==
              StatementPackageReservationIdAdmission::kZeroExhausted,
          "wrapped reservation identifier zero was not refused");
  Require(ClassifyStatementPackageReservationId(7, true) ==
              StatementPackageReservationIdAdmission::kCollision,
          "colliding reservation identifier was not refused");
  Require(ClassifyStatementPackageReservationId(7, false) ==
              StatementPackageReservationIdAdmission::kAdmitted,
          "fresh nonzero reservation identifier was not admitted");
  agents::ResourceGovernanceReservationLedger ledger("ia01.package.session");
  const auto cancelled = ledger.Acquire(Request("receipt.cancel"));
  Require(cancelled.ok && cancelled.reservation_created &&
              cancelled.snapshot.active_reservation_count == 1,
          "session-owned predecode reservation was not created");
  const auto cancel_release = ledger.Release(
      cancelled.reservation.token_id,
      agents::ResourceGovernanceReservationReleaseReason::kCancel);
  Require(cancel_release.ok && cancel_release.released &&
              cancel_release.reason ==
                  agents::ResourceGovernanceReservationReleaseReason::kCancel &&
              cancel_release.snapshot.active_reservation_count == 0 &&
              cancel_release.snapshot.active.memory_bytes == 0,
          "cancellation did not release the reservation exactly once");
  Require(ledger.Release(cancelled.reservation.token_id).not_found,
          "released cancellation token remained live");

  const auto timed = ledger.Acquire(Request("receipt.timeout"));
  Require(timed.ok && timed.reservation_created,
          "timeout reservation was not created");
  const auto timeout_release = ledger.Release(
      timed.reservation.token_id,
      agents::ResourceGovernanceReservationReleaseReason::kTimeout);
  Require(timeout_release.ok && timeout_release.released &&
              timeout_release.reason ==
                  agents::ResourceGovernanceReservationReleaseReason::kTimeout &&
              timeout_release.snapshot.active_reservation_count == 0,
          "timeout did not use the timeout release reason");

  auto over_limit = Request("receipt.overflow");
  over_limit.admission.requested.memory_bytes =
      std::numeric_limits<std::int64_t>::max();
  const auto refused = ledger.Acquire(std::move(over_limit));
  Require(!refused.ok && refused.fail_closed &&
              !refused.reservation_created &&
              refused.snapshot.active_reservation_count == 0,
          "overflow-sized request did not fail closed without reservation");
  return EXIT_SUCCESS;
}
