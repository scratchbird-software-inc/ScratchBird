// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_metric_runtime.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace metrics = scratchbird::core::metrics;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

agents::AgentRuntimeContext Context() {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.private_features_available = true;
  context.standalone_edition = true;
  context.cluster_authority_available = false;
  context.database_uuid = "019f0501-0000-7000-8000-000000000001";
  context.principal_uuid = "019f0501-0000-7000-8000-000000000002";
  context.rights = {
      "OBS_AGENT_STATE_READ",
      "OBS_AGENT_CONTROL",
      "OBS_AGENT_EVIDENCE_READ"};
  context.wall_now_microseconds = 1700000000000000ull;
  context.monotonic_now_microseconds = 5000000;
  return context;
}

agents::AgentTypeDescriptor PageDescriptor() {
  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "page_allocation_manager descriptor missing");
  return *descriptor;
}

agents::AgentPolicy PagePolicy(const agents::AgentTypeDescriptor& descriptor) {
  auto policy = agents::BaselinePolicyForAgent(descriptor);
  policy.policy_generation = 51;
  return policy;
}

agents::AgentObservedMetricSnapshot SnapshotFor(
    const agents::AgentMetricDependency& dependency,
    const agents::AgentRuntimeContext& context,
    const std::string& source_id,
    agents::u64 sequence) {
  agents::AgentObservedMetricSnapshot snapshot;
  snapshot.metric_family = dependency.metric_family;
  snapshot.namespace_path = dependency.namespace_prefix;
  snapshot.source_id = source_id;
  snapshot.generation = sequence;
  snapshot.source_sequence = sequence;
  snapshot.previous_source_sequence = sequence - 1;
  const auto freshness = dependency.max_freshness_microseconds == 0
      ? 1000
      : dependency.max_freshness_microseconds / 2;
  snapshot.observed_wall_microseconds =
      context.wall_now_microseconds - freshness;
  snapshot.scope_uuid = context.database_uuid;
  snapshot.digest = "digest:" + dependency.metric_family + ":7";
  snapshot.value_digest = snapshot.digest;
  snapshot.schema_digest = "schema:" + dependency.metric_family + ":7";
  snapshot.source_quality = dependency.required_source_quality ==
      agents::AgentMetricSourceQuality::unknown
          ? agents::AgentMetricSourceQuality::trusted
          : dependency.required_source_quality;
  snapshot.trusted = true;
  snapshot.attestation_verified = true;
  snapshot.redacted = true;
  snapshot.trust_provenance = "engine_observed";
  snapshot.provenance_record = "provenance:" + dependency.metric_family +
                               ":" + source_id;
  snapshot.attestation_key_id = "metric-key:" + source_id;
  snapshot.attestation_digest = "attestation:" + dependency.metric_family +
                                ":" + source_id;
  snapshot.evidence_uuid = "evidence:" + dependency.metric_family + ":" +
                           source_id;
  snapshot.snapshot_id = "snapshot:" + dependency.metric_family + ":" +
                         source_id;
  snapshot.authority_claims = {"metric_evidence"};
  return snapshot;
}

std::vector<agents::AgentObservedMetricSnapshot> RequiredSnapshots(
    const agents::AgentTypeDescriptor& descriptor,
    const agents::AgentRuntimeContext& context) {
  std::vector<agents::AgentObservedMetricSnapshot> snapshots;
  agents::u64 sequence = 700;
  for (const auto& dependency : descriptor.metric_dependencies) {
    if (!dependency.required || dependency.cluster_only) { continue; }
    snapshots.push_back(SnapshotFor(dependency, context, "metric-source-a",
                                    sequence++));
    snapshots.push_back(SnapshotFor(dependency, context, "metric-source-b",
                                    sequence++));
  }
  return snapshots;
}

void RequireMetricRefusal(
    const agents::AgentMetricSnapshotEvaluation& evaluation,
    const std::string& code) {
  Require(!evaluation.accepted, "metric evaluation unexpectedly accepted");
  Require(evaluation.failed_closed, "metric evaluation did not fail closed");
  Require(evaluation.status.diagnostic_code == code,
          "metric status mismatch: " + evaluation.status.diagnostic_code);
  Require(!evaluation.input_digest.empty(), "metric input digest missing");
  Require(!evaluation.diagnostics.empty(), "metric diagnostic missing");
  Require(evaluation.diagnostics.front().diagnostic_code == code,
          "metric diagnostic mismatch: " +
              evaluation.diagnostics.front().diagnostic_code);
}

void TestStrictMetricSnapshots() {
  const auto context = Context();
  const auto descriptor = PageDescriptor();
  auto snapshots = RequiredSnapshots(descriptor, context);
  Require(!snapshots.empty(), "page descriptor required snapshots missing");

  const auto accepted = agents::EvaluateAgentObservedMetricSnapshots(
      descriptor, context, snapshots);
  Require(accepted.accepted, "fresh trusted snapshots refused");
  Require(!accepted.failed_closed, "accepted snapshot marked failed closed");
  Require(!accepted.input_digest.empty(), "accepted input digest missing");
  Require(accepted.status.diagnostic_code == "SB_AGENT_METRIC_SNAPSHOT.ACCEPTED",
          "accepted diagnostic mismatch: " + accepted.status.diagnostic_code);

  auto missing = snapshots;
  const auto missing_family = missing.front().metric_family;
  for (auto it = missing.begin(); it != missing.end();) {
    if (it->metric_family == missing_family) {
      it = missing.erase(it);
    } else {
      ++it;
    }
  }
  RequireMetricRefusal(
      agents::EvaluateAgentObservedMetricSnapshots(descriptor, context, missing),
      "SB_AGENT_METRIC_SNAPSHOT.MISSING");

  auto stale = snapshots;
  const auto freshness = descriptor.metric_dependencies.front().max_freshness_microseconds;
  stale.front().observed_wall_microseconds =
      context.wall_now_microseconds - freshness - 1;
  RequireMetricRefusal(
      agents::EvaluateAgentObservedMetricSnapshots(descriptor, context, stale),
      "SB_AGENT_METRIC_SNAPSHOT.STALE");

  auto untrusted = snapshots;
  untrusted.front().trusted = false;
  untrusted.front().trust_provenance = "fixture_untrusted";
  RequireMetricRefusal(
      agents::EvaluateAgentObservedMetricSnapshots(descriptor, context, untrusted),
      "SB_AGENT_METRIC_SNAPSHOT.UNTRUSTED");

  auto scope_mismatch = snapshots;
  scope_mismatch.front().scope_uuid =
      "019f0501-ffff-7000-8000-000000000099";
  RequireMetricRefusal(
      agents::EvaluateAgentObservedMetricSnapshots(descriptor, context,
                                                   scope_mismatch),
      "SB_AGENT_METRIC_SNAPSHOT.SCOPE_MISMATCH");
}

void TestRelaxedRegistryOnlyIsProbeOnly() {
  const auto context = Context();
  const auto descriptor = PageDescriptor();

  RequireMetricRefusal(
      agents::EvaluateAgentObservedMetricSnapshots(descriptor, context, {}),
      "SB_AGENT_METRIC_SNAPSHOT.MISSING");

  agents::AgentMetricSnapshotEvaluationOptions options;
  options.mode = agents::AgentMetricRuntimeMode::test_probe_relaxed_registry_only;
  options.registry = &metrics::DefaultMetricRegistry();
  const auto relaxed = agents::EvaluateAgentObservedMetricSnapshots(
      descriptor, context, {}, options);
  Require(relaxed.accepted, "explicit test/probe registry-only path refused");
  Require(relaxed.relaxed_registry_only,
          "relaxed registry-only flag not recorded");
  Require(relaxed.status.diagnostic_code ==
              "SB_AGENT_METRIC_SNAPSHOT.RELAXED_TEST_PROBE_ONLY",
          "relaxed registry diagnostic mismatch: " +
              relaxed.status.diagnostic_code);
}

agents::AgentResourceReservationRequest ReservationRequest(
    std::string key,
    std::string owner) {
  agents::AgentResourceReservationRequest request;
  request.reservation_key = std::move(key);
  request.owner_scope = std::move(owner);
  request.memory_bytes = 128;
  request.worker_slots = 1;
  request.overhead_microseconds = 10;
  return request;
}

void RequireReservationRefusal(
    const agents::AgentResourceReservationResult& result,
    const std::string& code) {
  Require(!result.ok, "resource reservation unexpectedly passed for " + code);
  Require(result.failed_closed, "resource reservation did not fail closed");
  Require(result.diagnostic_code == code,
          "resource reservation diagnostic mismatch: " + result.diagnostic_code);
  Require(!result.evidence_uuid.empty(), "resource reservation evidence missing");
  Require(!result.reservation_created,
          "refused resource reservation created a token");
}

void TestReservationAcquireReleaseAndBudgets() {
  const auto context = Context();
  const auto descriptor = PageDescriptor();
  const auto policy = PagePolicy(descriptor);
  agents::AgentResourceReservationLimits unbounded_limits;
  agents::AgentResourceReservationLedger unbounded_ledger(
      "arhc-051-unbounded-ledger", unbounded_limits);
  RequireReservationRefusal(
      unbounded_ledger.Acquire(descriptor, policy, context,
                               ReservationRequest("reservation-unbounded",
                                                  "owner-unbounded")),
      "SB_AGENT_RESOURCE_RESERVATION.UNBOUNDED_LIMIT_REFUSED");

  agents::AgentResourceReservationLimits limits;
  limits.max_memory_bytes = 512;
  limits.max_worker_slots = 3;
  limits.foreground_reserved_worker_slots = 1;
  limits.max_overhead_microseconds = 100;

  agents::AgentResourceReservationLedger ledger("arhc-051-ledger", limits);
  auto request = ReservationRequest("reservation-one", "owner-one");
  const auto acquired = ledger.Acquire(descriptor, policy, context, request);
  Require(acquired.ok, "reservation acquire refused");
  Require(acquired.reservation_created, "reservation token not created");
  Require(acquired.snapshot.active_reservation_count == 1,
          "active reservation count mismatch");
  Require(acquired.snapshot.active_memory_bytes == request.memory_bytes,
          "active memory mismatch");

  RequireReservationRefusal(
      ledger.Acquire(descriptor, policy, context, request),
      "SB_AGENT_RESOURCE_RESERVATION.DUPLICATE");

  const auto released = ledger.Release(acquired.reservation.token_id);
  Require(released.ok, "reservation release failed");
  Require(released.released, "reservation was not marked released");
  Require(released.snapshot.active_reservation_count == 0,
          "release leaked active reservation");

  const auto second_release = ledger.Release(acquired.reservation.token_id);
  Require(second_release.ok, "idempotent release failed");
  Require(second_release.idempotent, "second release not marked idempotent");

  auto memory = ReservationRequest("reservation-memory", "owner-memory");
  memory.memory_bytes = 1024;
  RequireReservationRefusal(
      ledger.Acquire(descriptor, policy, context, memory),
      "SB_AGENT_RESOURCE_RESERVATION.MEMORY_OVER_BUDGET");

  auto overhead = ReservationRequest("reservation-overhead", "owner-overhead");
  overhead.overhead_microseconds = 101;
  RequireReservationRefusal(
      ledger.Acquire(descriptor, policy, context, overhead),
      "SB_AGENT_RESOURCE_RESERVATION.OVERHEAD_OVER_BUDGET");

  auto empty_request = ReservationRequest("reservation-empty", "owner-empty");
  empty_request.memory_bytes = 0;
  RequireReservationRefusal(
      ledger.Acquire(descriptor, policy, context, empty_request),
      "SB_AGENT_RESOURCE_RESERVATION.EMPTY_REQUEST_REFUSED");
}

void TestForegroundWorkerProtectionAndCancellationCleanup() {
  const auto context = Context();
  const auto descriptor = PageDescriptor();
  const auto policy = PagePolicy(descriptor);

  agents::AgentResourceReservationLimits protected_limits;
  protected_limits.max_memory_bytes = 512;
  protected_limits.max_worker_slots = 1;
  protected_limits.foreground_reserved_worker_slots = 1;
  protected_limits.max_overhead_microseconds = 100;
  agents::AgentResourceReservationLedger protected_ledger(
      "arhc-051-protected-ledger", protected_limits);
  RequireReservationRefusal(
      protected_ledger.Acquire(descriptor, policy, context,
                               ReservationRequest("reservation-worker",
                                                  "owner-worker")),
      "SB_AGENT_RESOURCE_RESERVATION.FOREGROUND_WORKER_PROTECTION");

  agents::AgentResourceReservationLimits limits;
  limits.max_memory_bytes = 512;
  limits.max_worker_slots = 3;
  limits.foreground_reserved_worker_slots = 1;
  limits.max_overhead_microseconds = 100;
  agents::AgentResourceReservationLedger ledger("arhc-051-cleanup-ledger",
                                                limits);
  auto active = ledger.Acquire(descriptor, policy, context,
                               ReservationRequest("reservation-cleanup",
                                                  "owner-cleanup"));
  Require(active.ok, "cleanup reservation acquire failed");
  const auto cancelled = ledger.CancelOwnerReservations("owner-cleanup");
  Require(cancelled.ok, "owner cancellation cleanup failed");
  Require(cancelled.released, "owner cancellation did not release reservation");
  Require(cancelled.snapshot.active_reservation_count == 0,
          "owner cancellation leaked active reservation");
  const auto repeat = ledger.Release(active.reservation.token_id);
  Require(repeat.ok && repeat.idempotent,
          "post-cancel idempotent release failed");

  auto foreground = ReservationRequest("reservation-foreground",
                                       "owner-foreground");
  foreground.foreground_database_work_active = true;
  RequireReservationRefusal(
      ledger.Acquire(descriptor, policy, context, foreground),
      "SB_AGENT_RESOURCE_RESERVATION.FOREGROUND_PROTECTION");
}

}  // namespace

int main() {
  TestStrictMetricSnapshots();
  TestRelaxedRegistryOnlyIsProbeOnly();
  TestReservationAcquireReleaseAndBudgets();
  TestForegroundWorkerProtectionAndCancellationCleanup();
  return EXIT_SUCCESS;
}
