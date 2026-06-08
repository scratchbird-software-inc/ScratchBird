// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/filespace_capacity_manager.hpp"
#include "page_allocation_lifecycle.hpp"
#include "page_filespace_handoff.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

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
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1870000000000ull + seed);
  return generated.ok() ? generated.value : platform::TypedUuid{};
}

bool SameUuid(const platform::TypedUuid& left, const platform::TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
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

agents::FilespaceCapacityManagerPolicy ActivePolicy(const FixtureIds& ids) {
  auto policy = agents::DefaultFilespaceCapacityManagerPolicy();
  policy.database_uuid = ids.database_uuid;
  policy.filespace_uuid = ids.filespace_uuid;
  policy.policy_uuid = ids.policy_uuid;
  policy.minimum_free_pages = 4;
  policy.target_free_pages = 8;
  policy.max_capacity_window_pages = 8;
  policy.capacity_window_allowed = true;
  policy.capacity_processing_policy_explicit = true;
  policy.expand_allowed = true;
  policy.expand_request_policy_explicit = true;
  return policy;
}

agents::FilespaceCapacityManagerPolicy DefaultRecommendOnlyPolicy(const FixtureIds& ids) {
  auto policy = agents::DefaultFilespaceCapacityManagerPolicy();
  policy.database_uuid = ids.database_uuid;
  policy.filespace_uuid = ids.filespace_uuid;
  policy.policy_uuid = ids.policy_uuid;
  return policy;
}

agents::FilespaceCapacityManagerMetricSnapshot HealthySnapshot(const FixtureIds& ids) {
  agents::FilespaceCapacityManagerMetricSnapshot snapshot;
  snapshot.database_uuid = ids.database_uuid;
  snapshot.filespace_uuid = ids.filespace_uuid;
  snapshot.policy_uuid = ids.policy_uuid;
  snapshot.total_pages = 256;
  snapshot.used_pages = 192;
  snapshot.free_pages = 4;
  snapshot.reserved_pages = 2;
  snapshot.available_capacity_window_pages = 8;
  snapshot.metrics_present = true;
  snapshot.metrics_fresh = true;
  snapshot.metrics_trusted = true;
  snapshot.scope_compatible = true;
  snapshot.health_state = agents::FilespaceCapacityHealthState::healthy;
  snapshot.role_state = agents::FilespaceCapacityRoleState::active_primary;
  return snapshot;
}

page::PageAllocationLedger PageLedger(const FixtureIds& ids) {
  page::PageAllocationLedger ledger;
  ledger.database_uuid = ids.database_uuid;
  ledger.filespace_uuid = ids.filespace_uuid;
  ledger.free_extents.push_back({100, 12});
  return ledger;
}

void RequirePageLedgerUnchanged(const page::PageAllocationLedger& ledger,
                                std::size_t free_extent_count,
                                std::size_t allocation_count,
                                std::size_t evidence_count) {
  Require(ledger.free_extents.size() == free_extent_count,
          "filespace capacity manager mutated free extents");
  Require(ledger.allocations.size() == allocation_count,
          "filespace capacity manager mutated page allocations");
  Require(ledger.evidence.size() == evidence_count,
          "filespace capacity manager wrote page allocation evidence");
}

platform::TypedUuid MakeEvidence(platform::u64 seed) {
  return MakeUuid(platform::UuidKind::object, 10000 + seed);
}

page::PageFilespaceAgentRequest MakeRequest(
    const FixtureIds& ids,
    page::PageFilespaceAgentRequestKind kind,
    platform::u64 requested_pages,
    std::string responding_agent = "filespace_capacity_manager") {
  page::PageFilespaceAgentRequest request;
  request.request_uuid = MakeUuid(platform::UuidKind::object, 20000 + requested_pages +
                                  static_cast<platform::u64>(kind));
  request.database_uuid = ids.database_uuid;
  request.filespace_uuid = ids.filespace_uuid;
  request.policy_uuid = ids.policy_uuid;
  request.kind = kind;
  request.state = page::PageFilespaceAgentRequestState::created;
  request.requesting_agent = "page_allocation_manager";
  request.responding_agent = std::move(responding_agent);
  request.page_family = "data";
  request.requested_pages = requested_pages;
  request.released_free_pages = 2;
  request.target_reserve_pages = 8;
  request.threshold_pages = 4;
  request.free_pages = 2;
  request.preallocated_pages = 0;
  request.allocated_pages = 128;
  request.reserved_pages = 2;
  request.reason = "test capacity request";
  return request;
}

void AppendRequest(page::PageFilespaceAgentRequestQueue* queue,
                   page::PageFilespaceAgentRequest request,
                   page::PageFilespaceAgentRequestState state) {
  page::PageFilespaceAgentQueueRecord record;
  record.sequence = queue->next_sequence++;
  record.request = request;
  record.request.state = state;
  record.allowed = true;
  record.filespace_agent_action_required =
      record.request.responding_agent == "filespace_capacity_manager";
  record.page_agent_action_required =
      record.request.responding_agent == "page_allocation_manager";
  record.target_free_pages = 8;
  record.low_water_pages = 4;
  record.diagnostic_code = "ok";
  record.evidence_state = "request_waiting_for_owner";
  record.evidence_id = MakeEvidence(record.sequence);
  record.explicit_evidence = true;

  auto add_transition =
      [&](page::PageFilespaceAgentRequestState previous,
          page::PageFilespaceAgentRequestState next,
          const std::string& reason) {
        page::PageFilespaceAgentTransitionRecord transition;
        transition.sequence = queue->next_sequence++;
        transition.previous_state = previous;
        transition.new_state = next;
        transition.evidence_id = MakeEvidence(transition.sequence);
        transition.diagnostic_code = "ok";
        transition.reason = reason;
        transition.explicit_evidence = true;
        record.transitions.push_back(transition);
      };

  if (state == page::PageFilespaceAgentRequestState::waiting_filespace_agent ||
      state == page::PageFilespaceAgentRequestState::waiting_page_agent) {
    add_transition(page::PageFilespaceAgentRequestState::created,
                   state,
                   "queued for owner");
  } else if (state == page::PageFilespaceAgentRequestState::approved) {
    add_transition(page::PageFilespaceAgentRequestState::created,
                   page::PageFilespaceAgentRequestState::waiting_filespace_agent,
                   "queued for filespace agent");
    add_transition(page::PageFilespaceAgentRequestState::waiting_filespace_agent,
                   page::PageFilespaceAgentRequestState::approved,
                   "already approved");
    record.evidence_state = "capacity_window_open";
  }

  queue->records.push_back(record);
}

page::PageFilespaceAgentRequestQueue QueueWithRequest(
    const FixtureIds& ids,
    page::PageFilespaceAgentRequestKind kind,
    platform::u64 requested_pages,
    page::PageFilespaceAgentRequestState state =
        page::PageFilespaceAgentRequestState::waiting_filespace_agent,
    std::string responding_agent = "filespace_capacity_manager") {
  page::PageFilespaceAgentRequestQueue queue;
  AppendRequest(&queue, MakeRequest(ids, kind, requested_pages, std::move(responding_agent)), state);
  return queue;
}

void RequireQueueState(const page::PageFilespaceAgentRequestQueue& queue,
                       page::PageFilespaceAgentRequestState state,
                       const std::string& diagnostic_code,
                       const std::string& evidence_state) {
  Require(queue.records.size() == 1, "queue record count mismatch");
  const auto& record = queue.records[0];
  Require(record.request.state == state, "queue state mismatch");
  Require(record.explicit_evidence && record.evidence_id.valid(), "queue evidence missing");
  Require(record.diagnostic_code == diagnostic_code,
          "queue diagnostic mismatch: expected " + diagnostic_code +
              " got " + record.diagnostic_code);
  Require(record.evidence_state == evidence_state,
          "queue evidence state mismatch: expected " + evidence_state +
              " got " + record.evidence_state);
  Require(!record.transitions.empty(), "queue transition evidence missing");
  Require(record.transitions.back().new_state == state, "final transition state mismatch");
  Require(record.transitions.back().diagnostic_code == diagnostic_code,
          "transition diagnostic mismatch");
  Require(record.transitions.back().explicit_evidence &&
              record.transitions.back().evidence_id.valid(),
          "transition explicit evidence missing");
}

void ExpectRefusal(const agents::FilespaceCapacityManagerTickResult& result,
                   const page::PageFilespaceAgentRequestQueue& queue,
                   const std::string& diagnostic_code) {
  Require(!result.ok(), diagnostic_code + " unexpectedly succeeded");
  Require(result.fail_closed && result.refused, diagnostic_code + " did not fail closed");
  Require(result.decision == agents::FilespaceCapacityManagerDecisionKind::capacity_window_refused,
          diagnostic_code + " did not refuse a capacity window");
  Require(result.diagnostic.diagnostic_code == diagnostic_code,
          "result diagnostic mismatch: expected " + diagnostic_code +
              " got " + result.diagnostic.diagnostic_code);
  Require(result.evidence.diagnostic_code == diagnostic_code,
          diagnostic_code + " evidence diagnostic mismatch");
  Require(result.evidence.durable_state_changed,
          diagnostic_code + " did not write durable refusal evidence");
  Require(!result.physical_filespace_mutation_attempted &&
              !result.page_ledger_mutation_attempted,
          diagnostic_code + " attempted forbidden mutation");
  RequireQueueState(queue,
                    page::PageFilespaceAgentRequestState::refused,
                    diagnostic_code,
                    "capacity_window_refused");
}

void TestHealthyMetricsNoQueuedRequestNoAction() {
  const auto ids = MakeIds(1);
  page::PageFilespaceAgentRequestQueue queue;
  const auto result = agents::EvaluateFilespaceCapacityManagerTick(
      &queue, HealthySnapshot(ids), ActivePolicy(ids));

  Require(result.ok(), "healthy no-request tick failed");
  Require(result.decision == agents::FilespaceCapacityManagerDecisionKind::no_action,
          "healthy no-request did not produce no-action");
  Require(result.diagnostic.diagnostic_code == "FILESPACE_AGENT.NO_ACTION",
          "healthy no-action diagnostic mismatch");
  Require(queue.records.empty(), "healthy no-action mutated queue");
  Require(!result.physical_filespace_mutation_attempted &&
              !result.page_ledger_mutation_attempted,
          "healthy no-action attempted forbidden mutation");
}

void TestAcceptedExtendFilespaceRequestApprovesCapacityWindow() {
  const auto ids = MakeIds(10);
  auto queue = QueueWithRequest(ids,
                                page::PageFilespaceAgentRequestKind::extend_filespace,
                                6);
  auto ledger = PageLedger(ids);
  const std::size_t free_extents = ledger.free_extents.size();
  const std::size_t allocations = ledger.allocations.size();
  const std::size_t evidence = ledger.evidence.size();

  const auto result = agents::EvaluateFilespaceCapacityManagerTick(
      &queue, HealthySnapshot(ids), ActivePolicy(ids));

  Require(result.ok(), "accepted capacity request failed: " +
                           result.diagnostic.diagnostic_code);
  Require(result.approved, "capacity window was not approved");
  Require(result.decision ==
              agents::FilespaceCapacityManagerDecisionKind::capacity_window_approved,
          "approved decision mismatch");
  Require(result.granted_pages == 6 && result.requested_pages == 6,
          "granted/requested pages mismatch");
  Require(result.capacity_window_pages == 8,
          "capacity window pages mismatch");
  Require(result.evidence.granted_pages == 6 &&
              result.evidence.capacity_window_pages == 8 &&
              result.evidence.durable_state_changed &&
              result.evidence.evidence_uuid.valid(),
          "approval evidence mismatch");
  Require(SameUuid(result.evidence.request_uuid, queue.records[0].request.request_uuid),
          "approval evidence request UUID mismatch");
  Require(!result.evidence.physical_filespace_mutation_attempted &&
              !result.evidence.page_ledger_mutation_attempted,
          "approval evidence recorded forbidden mutation");
  RequireQueueState(queue,
                    page::PageFilespaceAgentRequestState::approved,
                    "ok",
                    "capacity_window_open");
  RequirePageLedgerUnchanged(ledger, free_extents, allocations, evidence);

  const auto restored =
      page::RestorePageFilespaceAgentRequestQueue(page::SerializePageFilespaceAgentRequestQueue(queue));
  Require(restored.ok(), "approved queue restore failed: " +
                             restored.diagnostic.diagnostic_code);
  Require(restored.queue.records[0].request.state ==
              page::PageFilespaceAgentRequestState::approved,
          "restored approved queue state mismatch");
  Require(restored.queue.records[0].transitions.back().diagnostic_code == "ok",
          "restored approval transition diagnostic mismatch");
}

void TestMultipleWaitingRequestsProcessedInOneTick() {
  const auto ids = MakeIds(20);
  page::PageFilespaceAgentRequestQueue queue;
  AppendRequest(&queue,
                MakeRequest(ids, page::PageFilespaceAgentRequestKind::extend_filespace, 3),
                page::PageFilespaceAgentRequestState::waiting_filespace_agent);
  AppendRequest(&queue,
                MakeRequest(ids, page::PageFilespaceAgentRequestKind::extend_filespace, 4),
                page::PageFilespaceAgentRequestState::waiting_filespace_agent);

  const auto result = agents::EvaluateFilespaceCapacityManagerTick(
      &queue, HealthySnapshot(ids), ActivePolicy(ids));

  Require(result.ok(), "multi-request tick failed: " +
                           result.diagnostic.diagnostic_code);
  Require(result.processed_records == 2 && result.approved,
          "multi-request tick did not process both waiting records");
  Require(result.requested_pages == 7 && result.granted_pages == 7,
          "multi-request granted/requested total mismatch");
  Require(queue.records.size() == 2, "multi-request queue size changed");
  for (const auto& record : queue.records) {
    Require(record.request.state == page::PageFilespaceAgentRequestState::approved,
            "multi-request record was not approved");
    Require(record.diagnostic_code == "ok" &&
                record.evidence_state == "capacity_window_open",
            "multi-request record evidence mismatch");
  }
}

void TestMultipleWaitingRequestsCannotOvercommitCapacityWindow() {
  const auto ids = MakeIds(21);
  page::PageFilespaceAgentRequestQueue queue;
  AppendRequest(&queue,
                MakeRequest(ids, page::PageFilespaceAgentRequestKind::extend_filespace, 6),
                page::PageFilespaceAgentRequestState::waiting_filespace_agent);
  AppendRequest(&queue,
                MakeRequest(ids, page::PageFilespaceAgentRequestKind::extend_filespace, 4),
                page::PageFilespaceAgentRequestState::waiting_filespace_agent);

  const auto result = agents::EvaluateFilespaceCapacityManagerTick(
      &queue, HealthySnapshot(ids), ActivePolicy(ids));

  Require(!result.ok(), "overcommit tick unexpectedly succeeded");
  Require(result.processed_records == 2, "overcommit tick did not inspect both records");
  Require(result.requested_pages == 10 && result.granted_pages == 6,
          "overcommit tick did not consume capacity cumulatively");
  Require(result.capacity_window_pages == 8,
          "overcommit tick lost the original capacity window");
  Require(result.diagnostic.diagnostic_code ==
              "FILESPACE_AGENT.REQUESTED_PAGES_OVER_LIMIT",
          "overcommit tick diagnostic mismatch: " +
              result.diagnostic.diagnostic_code);
  Require(queue.records.size() == 2, "overcommit queue size changed");
  Require(queue.records[0].request.state ==
              page::PageFilespaceAgentRequestState::approved,
          "first overcommit record was not approved");
  Require(queue.records[0].diagnostic_code == "ok" &&
              queue.records[0].evidence_state == "capacity_window_open",
          "first overcommit record evidence mismatch");
  Require(queue.records[1].request.state ==
              page::PageFilespaceAgentRequestState::refused,
          "second overcommit record was not refused");
  Require(queue.records[1].diagnostic_code ==
              "FILESPACE_AGENT.REQUESTED_PAGES_OVER_LIMIT",
          "second overcommit record diagnostic mismatch");
  Require(queue.records[1].evidence_state == "capacity_window_refused",
          "second overcommit record evidence state mismatch");
}

void TestMetricPolicySafetyAndScopeRefusals() {
  struct Case {
    std::string diagnostic_code;
    agents::FilespaceCapacityManagerMetricSnapshot snapshot;
    agents::FilespaceCapacityManagerPolicy policy;
    agents::FilespaceCapacityManagerSafetyState safety;
  };

  const auto base_ids = MakeIds(30);
  std::vector<Case> cases;

  {
    auto snapshot = HealthySnapshot(base_ids);
    snapshot.metrics_present = false;
    cases.push_back({"FILESPACE_AGENT.METRIC_MISSING",
                     snapshot,
                     ActivePolicy(base_ids),
                     agents::FilespaceCapacityManagerSafetyState{}});
  }
  {
    auto snapshot = HealthySnapshot(base_ids);
    snapshot.metrics_fresh = false;
    cases.push_back({"FILESPACE_AGENT.METRIC_STALE",
                     snapshot,
                     ActivePolicy(base_ids),
                     agents::FilespaceCapacityManagerSafetyState{}});
  }
  {
    auto snapshot = HealthySnapshot(base_ids);
    snapshot.metrics_trusted = false;
    cases.push_back({"FILESPACE_AGENT.METRIC_UNTRUSTED",
                     snapshot,
                     ActivePolicy(base_ids),
                     agents::FilespaceCapacityManagerSafetyState{}});
  }
  {
    auto policy = ActivePolicy(base_ids);
    policy.target_free_pages = 2;
    policy.minimum_free_pages = 4;
    cases.push_back({"FILESPACE_AGENT.POLICY_INVALID",
                     HealthySnapshot(base_ids),
                     policy,
                     agents::FilespaceCapacityManagerSafetyState{}});
  }
  {
    auto policy = ActivePolicy(base_ids);
    policy.filespace_uuid = MakeUuid(platform::UuidKind::filespace, 9999);
    cases.push_back({"FILESPACE_AGENT.POLICY_SCOPE_INCOMPATIBLE",
                     HealthySnapshot(base_ids),
                     policy,
                     agents::FilespaceCapacityManagerSafetyState{}});
  }
  {
    auto snapshot = HealthySnapshot(base_ids);
    snapshot.health_state = agents::FilespaceCapacityHealthState::critical;
    cases.push_back({"FILESPACE_AGENT.HEALTH_DENIED",
                     snapshot,
                     ActivePolicy(base_ids),
                     agents::FilespaceCapacityManagerSafetyState{}});
  }
  {
    auto snapshot = HealthySnapshot(base_ids);
    snapshot.role_state = agents::FilespaceCapacityRoleState::forbidden;
    cases.push_back({"FILESPACE_AGENT.ROLE_DENIED",
                     snapshot,
                     ActivePolicy(base_ids),
                     agents::FilespaceCapacityManagerSafetyState{}});
  }
  {
    auto safety = agents::FilespaceCapacityManagerSafetyState{};
    safety.startup_complete = false;
    cases.push_back({"FILESPACE_AGENT.STARTUP_UNSAFE",
                     HealthySnapshot(base_ids),
                     ActivePolicy(base_ids),
                     safety});
  }
  {
    auto safety = agents::FilespaceCapacityManagerSafetyState{};
    safety.recovery_complete = false;
    cases.push_back({"FILESPACE_AGENT.RECOVERY_UNSAFE",
                     HealthySnapshot(base_ids),
                     ActivePolicy(base_ids),
                     safety});
  }
  {
    auto safety = agents::FilespaceCapacityManagerSafetyState{};
    safety.maintenance_mode = true;
    safety.maintenance_allows_capacity_windows = false;
    cases.push_back({"FILESPACE_AGENT.MAINTENANCE_UNSAFE",
                     HealthySnapshot(base_ids),
                     ActivePolicy(base_ids),
                     safety});
  }
  cases.push_back({"FILESPACE_AGENT.ACTION_RECOMMEND_ONLY",
                   HealthySnapshot(base_ids),
                   DefaultRecommendOnlyPolicy(base_ids),
                   agents::FilespaceCapacityManagerSafetyState{}});

  platform::u64 seed = 0;
  for (const auto& test_case : cases) {
    const auto ids = MakeIds(300 + seed++);
    auto queue = QueueWithRequest(ids,
                                  page::PageFilespaceAgentRequestKind::extend_filespace,
                                  6);
    auto snapshot = test_case.snapshot;
    auto policy = test_case.policy;
    snapshot.database_uuid = ids.database_uuid;
    snapshot.filespace_uuid = ids.filespace_uuid;
    snapshot.policy_uuid = ids.policy_uuid;
    if (test_case.diagnostic_code != "FILESPACE_AGENT.POLICY_SCOPE_INCOMPATIBLE") {
      policy.database_uuid = ids.database_uuid;
      policy.filespace_uuid = ids.filespace_uuid;
      policy.policy_uuid = ids.policy_uuid;
    }
    const auto result = agents::EvaluateFilespaceCapacityManagerTick(
        &queue, snapshot, policy, test_case.safety);
    ExpectRefusal(result, queue, test_case.diagnostic_code);
  }
}

void TestMissingQueueFailsClosed() {
  const auto ids = MakeIds(60);
  const auto result = agents::EvaluateFilespaceCapacityManagerTick(
      nullptr, HealthySnapshot(ids), ActivePolicy(ids));
  Require(!result.ok(), "missing queue unexpectedly succeeded");
  Require(result.fail_closed, "missing queue did not fail closed");
  Require(result.diagnostic.diagnostic_code == "FILESPACE_AGENT.QUEUE_MISSING",
          "missing queue diagnostic mismatch");
  Require(!result.queue_mutated && !result.physical_filespace_mutation_attempted &&
              !result.page_ledger_mutation_attempted,
          "missing queue attempted mutation");
}

void TestNonWaitingAndWrongRespondingAgentNoMutation() {
  {
    const auto ids = MakeIds(70);
    auto queue = QueueWithRequest(ids,
                                  page::PageFilespaceAgentRequestKind::extend_filespace,
                                  6,
                                  page::PageFilespaceAgentRequestState::approved);
    const auto before = page::SerializePageFilespaceAgentRequestQueue(queue);
    const auto result = agents::EvaluateFilespaceCapacityManagerTick(
        &queue, HealthySnapshot(ids), ActivePolicy(ids));
    Require(result.ok(), "non-waiting request failed");
    Require(result.decision == agents::FilespaceCapacityManagerDecisionKind::no_action,
            "non-waiting request was processed");
    Require(page::SerializePageFilespaceAgentRequestQueue(queue) == before,
            "non-waiting request mutated queue");
  }
  {
    const auto ids = MakeIds(71);
    auto queue = QueueWithRequest(ids,
                                  page::PageFilespaceAgentRequestKind::extend_filespace,
                                  6,
                                  page::PageFilespaceAgentRequestState::waiting_page_agent,
                                  "page_allocation_manager");
    const auto before = page::SerializePageFilespaceAgentRequestQueue(queue);
    const auto result = agents::EvaluateFilespaceCapacityManagerTick(
        &queue, HealthySnapshot(ids), ActivePolicy(ids));
    Require(result.ok(), "wrong responding agent failed");
    Require(result.decision == agents::FilespaceCapacityManagerDecisionKind::no_action,
            "wrong responding agent was processed");
    Require(page::SerializePageFilespaceAgentRequestQueue(queue) == before,
            "wrong responding agent mutated queue");
  }
}

void TestBoundaryAndRequestedPageRefusals() {
  const std::vector<page::PageFilespaceAgentRequestKind> page_owned = {
      page::PageFilespaceAgentRequestKind::reserve_pages,
      page::PageFilespaceAgentRequestKind::relocate_pages,
      page::PageFilespaceAgentRequestKind::release_pages};
  platform::u64 seed = 0;
  for (const auto kind : page_owned) {
    const auto ids = MakeIds(80 + seed++);
    auto queue = QueueWithRequest(ids, kind, 3);
    const auto result = agents::EvaluateFilespaceCapacityManagerTick(
        &queue, HealthySnapshot(ids), ActivePolicy(ids));
    ExpectRefusal(result, queue, "FILESPACE_AGENT.PAGE_AUTHORITY_REQUIRED");
  }
  {
    const auto ids = MakeIds(90);
    auto queue = QueueWithRequest(ids,
                                  page::PageFilespaceAgentRequestKind::extend_filespace,
                                  0);
    const auto result = agents::EvaluateFilespaceCapacityManagerTick(
        &queue, HealthySnapshot(ids), ActivePolicy(ids));
    ExpectRefusal(result, queue, "FILESPACE_AGENT.REQUESTED_PAGES_REQUIRED");
  }
  {
    const auto ids = MakeIds(91);
    auto queue = QueueWithRequest(ids,
                                  page::PageFilespaceAgentRequestKind::extend_filespace,
                                  12);
    const auto result = agents::EvaluateFilespaceCapacityManagerTick(
        &queue, HealthySnapshot(ids), ActivePolicy(ids));
    ExpectRefusal(result, queue, "FILESPACE_AGENT.REQUESTED_PAGES_OVER_LIMIT");
  }
  {
    const auto ids = MakeIds(92);
    auto queue = QueueWithRequest(ids,
                                  page::PageFilespaceAgentRequestKind::truncate_filespace,
                                  4);
    const auto result = agents::EvaluateFilespaceCapacityManagerTick(
        &queue, HealthySnapshot(ids), ActivePolicy(ids));
    ExpectRefusal(result, queue, "FILESPACE_AGENT.ACTION_RECOMMEND_ONLY");

    const auto restored =
        page::RestorePageFilespaceAgentRequestQueue(page::SerializePageFilespaceAgentRequestQueue(queue));
    Require(restored.ok(), "refused queue restore failed: " +
                               restored.diagnostic.diagnostic_code);
    Require(restored.queue.records[0].request.state ==
                page::PageFilespaceAgentRequestState::refused,
            "restored refusal queue state mismatch");
    Require(restored.queue.records[0].transitions.back().diagnostic_code ==
                "FILESPACE_AGENT.ACTION_RECOMMEND_ONLY",
            "restored refusal transition diagnostic mismatch");
  }
}

}  // namespace

int main() {
  TestHealthyMetricsNoQueuedRequestNoAction();
  TestAcceptedExtendFilespaceRequestApprovesCapacityWindow();
  TestMultipleWaitingRequestsProcessedInOneTick();
  TestMultipleWaitingRequestsCannotOvercommitCapacityWindow();
  TestMetricPolicySafetyAndScopeRefusals();
  TestMissingQueueFailsClosed();
  TestNonWaitingAndWrongRespondingAgentNoMutation();
  TestBoundaryAndRequestedPageRefusals();
  return EXIT_SUCCESS;
}
