// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/page_allocation_manager.hpp"
#include "page_filespace_handoff.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

namespace agents = scratchbird::core::agents::implemented_agents;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

platform::TypedUuid MakeUuid(platform::UuidKind kind, platform::u64 seed) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1840000000000ull + seed);
  return generated.ok() ? generated.value : platform::TypedUuid{};
}

struct FixtureIds {
  platform::TypedUuid database_uuid;
  platform::TypedUuid filespace_uuid;
  platform::TypedUuid policy_uuid;
};

FixtureIds MakeIds(platform::u64 seed) {
  return {MakeUuid(platform::UuidKind::database, 100 + seed),
          MakeUuid(platform::UuidKind::filespace, 200 + seed),
          MakeUuid(platform::UuidKind::object, 300 + seed)};
}

agents::PageAllocationManagerPolicy Policy(const FixtureIds& ids) {
  agents::PageAllocationManagerPolicy policy =
      agents::DefaultPageAllocationManagerPolicy();
  policy.database_uuid = ids.database_uuid;
  policy.filespace_uuid = ids.filespace_uuid;
  policy.policy_uuid = ids.policy_uuid;
  policy.minimum_free_pages = 4;
  policy.target_free_pages = 8;
  policy.low_water_notify_ratio = 0.50;
  policy.capacity_request_allowed = false;
  policy.capacity_request_policy_explicit = false;
  policy.live_preallocation_allowed = false;
  return policy;
}

agents::PageAllocationManagerMetricSnapshot HealthySnapshot(const FixtureIds& ids) {
  agents::PageAllocationManagerMetricSnapshot snapshot;
  snapshot.database_uuid = ids.database_uuid;
  snapshot.filespace_uuid = ids.filespace_uuid;
  snapshot.policy_uuid = ids.policy_uuid;
  snapshot.page_family = "data";
  snapshot.free_pages = 6;
  snapshot.released_pages = 2;
  snapshot.reserved_pages = 2;
  snapshot.preallocated_pages = 4;
  snapshot.allocated_pages = 128;
  snapshot.target_free_deficit_pages = 0;
  snapshot.preallocation_target_pages = 4;
  snapshot.preallocation_deficit_pages = 0;
  snapshot.metrics_present = true;
  snapshot.metrics_fresh = true;
  snapshot.metrics_trusted = true;
  snapshot.scope_compatible = true;
  snapshot.allocation_failure_signal = false;
  snapshot.allocation_failures_total = 0;
  return snapshot;
}

bool SameUuid(const platform::TypedUuid& left, const platform::TypedUuid& right) {
  return left.kind == right.kind &&
         uuid::UuidToString(left.value) == uuid::UuidToString(right.value);
}

void ExpectNoQueueMutation(const agents::PageAllocationManagerTickResult& result,
                           const page::PageFilespaceAgentRequestQueue& queue,
                           const std::string& diagnostic_code) {
  Require(!result.ok(), diagnostic_code + " unexpectedly succeeded");
  Require(result.fail_closed, diagnostic_code + " did not fail closed");
  Require(result.diagnostic.diagnostic_code == diagnostic_code,
          "diagnostic mismatch: expected " + diagnostic_code +
              " got " + result.diagnostic.diagnostic_code);
  Require(queue.records.empty(), diagnostic_code + " mutated the durable queue");
  Require(!result.direct_action_attempted,
          diagnostic_code + " attempted a direct page/filespace action");
}

void TestHealthyMetricsNoAction() {
  const auto ids = MakeIds(1);
  page::PageFilespaceAgentRequestQueue queue;
  const auto result = agents::EvaluatePageAllocationManagerTick(
      &queue, HealthySnapshot(ids), Policy(ids));

  Require(result.ok(), "healthy metrics did not succeed");
  Require(result.decision == agents::PageAllocationManagerDecisionKind::no_action,
          "healthy metrics did not produce no-action decision");
  Require(result.diagnostic.diagnostic_code == "PAGE_AGENT.NO_ACTION",
          "healthy diagnostic mismatch");
  Require(result.released_free_pages == 8 && result.low_water_pages == 4,
          "healthy threshold accounting mismatch");
  Require(queue.records.empty(), "healthy metrics mutated queue");
  Require(!result.direct_action_attempted, "healthy path attempted direct action");
}

void TestLowReserveQueuesDurableCapacityRequestAndRecovery() {
  const auto ids = MakeIds(10);
  auto policy = Policy(ids);
  policy.capacity_request_allowed = true;
  policy.capacity_request_policy_explicit = true;

  auto snapshot = HealthySnapshot(ids);
  snapshot.free_pages = 1;
  snapshot.released_pages = 1;
  snapshot.target_free_deficit_pages = 6;

  page::PageFilespaceAgentRequestQueue queue;
  const auto result = agents::EvaluatePageAllocationManagerTick(&queue, snapshot, policy);

  Require(result.ok(), "low-reserve capacity request failed: " +
                           result.diagnostic.diagnostic_code);
  Require(result.decision ==
              agents::PageAllocationManagerDecisionKind::capacity_request_queued,
          "low reserve did not queue capacity request");
  Require(result.capacity_request_enqueued && result.accepted_evidence,
          "low reserve did not return accepted durable evidence");
  Require(result.diagnostic.diagnostic_code == "PAGE_AGENT.FILESPACE_CAPACITY_REQUIRED",
          "low reserve manager diagnostic mismatch");
  Require(queue.records.size() == 1, "low reserve did not enqueue one request");

  const auto& record = queue.records[0];
  Require(record.request.kind == page::PageFilespaceAgentRequestKind::extend_filespace,
          "low reserve request kind mismatch");
  Require(record.request.state ==
              page::PageFilespaceAgentRequestState::waiting_filespace_agent,
          "low reserve request state mismatch");
  Require(record.request.requesting_agent == "page_allocation_manager" &&
              record.request.responding_agent == "filespace_capacity_manager",
          "low reserve request owner routing mismatch");
  Require(record.request.requested_pages == 6 &&
              record.request.released_free_pages == 2 &&
              record.request.target_reserve_pages == 8 &&
              record.request.threshold_pages == 4,
          "low reserve request counts mismatch");
  Require(record.explicit_evidence && record.evidence_id.valid(),
          "low reserve queue evidence missing");
  Require(result.handoff.evidence.evidence_id.valid() &&
              result.handoff.evidence.durable_state_changed &&
              SameUuid(result.handoff.evidence.request_uuid, record.request.request_uuid),
          "low reserve handoff evidence mismatch");

  const auto restored =
      page::RestorePageFilespaceAgentRequestQueue(page::SerializePageFilespaceAgentRequestQueue(queue));
  Require(restored.ok(), "low reserve queue restore failed: " +
                             restored.diagnostic.diagnostic_code);
  const auto classified =
      page::ClassifyPageFilespaceAgentRequestQueueForRecovery(restored.queue);
  Require(classified.ok(), "low reserve recovery classification failed: " +
                               classified.diagnostic.diagnostic_code);
  Require(classified.classifications.size() == 1,
          "low reserve recovery classification count mismatch");
  Require(classified.classifications[0].action ==
              page::PageFilespaceAgentRequestRecoveryAction::resume_revalidate,
          "queued low reserve request was not resume/revalidate");
}

void TestAboveThresholdNoQueue() {
  const auto ids = MakeIds(20);
  auto policy = Policy(ids);
  policy.capacity_request_allowed = true;
  policy.capacity_request_policy_explicit = true;

  auto snapshot = HealthySnapshot(ids);
  snapshot.free_pages = 3;
  snapshot.released_pages = 2;
  snapshot.target_free_deficit_pages = 0;

  page::PageFilespaceAgentRequestQueue queue;
  const auto result = agents::EvaluatePageAllocationManagerTick(&queue, snapshot, policy);
  Require(result.ok(), "above-threshold evaluation failed");
  Require(result.decision == agents::PageAllocationManagerDecisionKind::no_action,
          "above-threshold metrics queued or recommended work");
  Require(queue.records.empty(), "above-threshold metrics mutated queue");
}

void TestTargetFreeDeficitQueuesDurableCapacityRequest() {
  const auto ids = MakeIds(25);
  auto policy = Policy(ids);
  policy.capacity_request_allowed = true;
  policy.capacity_request_policy_explicit = true;

  auto snapshot = HealthySnapshot(ids);
  snapshot.free_pages = 5;
  snapshot.released_pages = 0;
  snapshot.target_free_deficit_pages = 3;

  page::PageFilespaceAgentRequestQueue queue;
  const auto result = agents::EvaluatePageAllocationManagerTick(&queue, snapshot, policy);
  Require(result.ok(), "target-free deficit capacity request failed: " +
                           result.diagnostic.diagnostic_code);
  Require(result.decision ==
              agents::PageAllocationManagerDecisionKind::capacity_request_queued,
          "target-free deficit did not queue capacity request");
  Require(result.capacity_request_enqueued && result.accepted_evidence,
          "target-free deficit did not return accepted durable evidence");
  Require(queue.records.size() == 1, "target-free deficit did not enqueue one request");
  const auto& record = queue.records[0];
  Require(record.request.kind == page::PageFilespaceAgentRequestKind::extend_filespace,
          "target-free deficit request kind mismatch");
  Require(record.request.requested_pages == 3 &&
              record.request.released_free_pages == 5 &&
              record.request.threshold_pages == 4,
          "target-free deficit request counts mismatch");
  Require(record.request.responding_agent == "filespace_capacity_manager",
          "target-free deficit did not target filespace capacity manager");
  Require(record.explicit_evidence && record.evidence_id.valid(),
          "target-free deficit queue evidence missing");
}

void TestMetricDependencyFailuresFailClosed() {
  const auto ids = MakeIds(30);
  auto policy = Policy(ids);
  policy.capacity_request_allowed = true;
  policy.capacity_request_policy_explicit = true;

  {
    page::PageFilespaceAgentRequestQueue queue;
    auto snapshot = HealthySnapshot(ids);
    snapshot.metrics_present = false;
    ExpectNoQueueMutation(
        agents::EvaluatePageAllocationManagerTick(&queue, snapshot, policy),
        queue,
        "PAGE_AGENT.METRIC_MISSING");
  }
  {
    page::PageFilespaceAgentRequestQueue queue;
    auto snapshot = HealthySnapshot(ids);
    snapshot.metrics_fresh = false;
    ExpectNoQueueMutation(
        agents::EvaluatePageAllocationManagerTick(&queue, snapshot, policy),
        queue,
        "PAGE_AGENT.METRIC_STALE");
  }
  {
    page::PageFilespaceAgentRequestQueue queue;
    auto snapshot = HealthySnapshot(ids);
    snapshot.metrics_trusted = false;
    ExpectNoQueueMutation(
        agents::EvaluatePageAllocationManagerTick(&queue, snapshot, policy),
        queue,
        "PAGE_AGENT.METRIC_UNTRUSTED");
  }
}

void TestInvalidPolicyScopeAndPageFamilyRefusals() {
  {
    const auto ids = MakeIds(40);
    auto policy = Policy(ids);
    policy.minimum_free_pages = 8;
    policy.target_free_pages = 4;
    page::PageFilespaceAgentRequestQueue queue;
    ExpectNoQueueMutation(
        agents::EvaluatePageAllocationManagerTick(&queue, HealthySnapshot(ids), policy),
        queue,
        "PAGE_AGENT.POLICY_INVALID");
  }
  {
    const auto ids = MakeIds(41);
    auto policy = Policy(ids);
    policy.filespace_uuid = MakeUuid(platform::UuidKind::filespace, 9999);
    page::PageFilespaceAgentRequestQueue queue;
    ExpectNoQueueMutation(
        agents::EvaluatePageAllocationManagerTick(&queue, HealthySnapshot(ids), policy),
        queue,
        "PAGE_AGENT.POLICY_SCOPE_INCOMPATIBLE");
  }
  {
    const auto ids = MakeIds(42);
    auto snapshot = HealthySnapshot(ids);
    snapshot.page_family = "not_a_page_family";
    page::PageFilespaceAgentRequestQueue queue;
    ExpectNoQueueMutation(
        agents::EvaluatePageAllocationManagerTick(&queue, snapshot, Policy(ids)),
        queue,
        "PAGE_AGENT.UNKNOWN_PAGE_FAMILY");
  }
}

void TestAllocationFailureAndPolicyDeniedCapacityRefusals() {
  {
    const auto ids = MakeIds(50);
    auto snapshot = HealthySnapshot(ids);
    snapshot.allocation_failure_signal = true;
    page::PageFilespaceAgentRequestQueue queue;
    ExpectNoQueueMutation(
        agents::EvaluatePageAllocationManagerTick(&queue, snapshot, Policy(ids)),
        queue,
        "PAGE_AGENT.ALLOCATION_FAILURE");
  }
  {
    const auto ids = MakeIds(51);
    auto snapshot = HealthySnapshot(ids);
    snapshot.free_pages = 1;
    snapshot.released_pages = 1;
    snapshot.target_free_deficit_pages = 6;
    page::PageFilespaceAgentRequestQueue queue;
    ExpectNoQueueMutation(
        agents::EvaluatePageAllocationManagerTick(&queue, snapshot, Policy(ids)),
        queue,
        "PAGE_AGENT.PERMISSION_DENIED");
  }
}

void TestPreallocationDeficitSuppressedUntilBoundedActionSlice() {
  const auto ids = MakeIds(60);
  auto snapshot = HealthySnapshot(ids);
  snapshot.free_pages = 8;
  snapshot.released_pages = 2;
  snapshot.preallocated_pages = 1;
  snapshot.preallocation_target_pages = 8;
  snapshot.preallocation_deficit_pages = 7;

  page::PageFilespaceAgentRequestQueue queue;
  const auto result = agents::EvaluatePageAllocationManagerTick(
      &queue, snapshot, Policy(ids));

  Require(result.ok(), "preallocation deficit suppression failed");
  Require(result.decision ==
              agents::PageAllocationManagerDecisionKind::preallocation_suppressed,
          "preallocation deficit was not suppressed");
  Require(result.diagnostic.diagnostic_code == "PAGE_AGENT.PREALLOCATION_SUPPRESSED",
          "preallocation suppression diagnostic mismatch");
  Require(result.preallocation_recommended && result.preallocation_suppressed,
          "preallocation suppression flags mismatch");
  Require(result.preallocation_deficit_pages == 7,
          "preallocation deficit count mismatch");
  Require(queue.records.empty(), "preallocation suppression mutated queue");
  Require(!result.direct_action_attempted,
          "preallocation suppression attempted direct action");
}

}  // namespace

int main() {
  TestHealthyMetricsNoAction();
  TestLowReserveQueuesDurableCapacityRequestAndRecovery();
  TestAboveThresholdNoQueue();
  TestTargetFreeDeficitQueuesDurableCapacityRequest();
  TestMetricDependencyFailuresFailClosed();
  TestInvalidPolicyScopeAndPageFamilyRefusals();
  TestAllocationFailureAndPolicyDeniedCapacityRefusals();
  TestPreallocationDeficitSuppressedUntilBoundedActionSlice();
  return EXIT_SUCCESS;
}
