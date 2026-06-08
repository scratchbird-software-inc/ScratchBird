// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/page_allocation_manager.hpp"
#include "page_allocation_lifecycle.hpp"
#include "page_filespace_handoff.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <functional>
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
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1850000000000ull + seed);
  return generated.ok() ? generated.value : platform::TypedUuid{};
}

struct FixtureIds {
  platform::TypedUuid database_uuid;
  platform::TypedUuid filespace_uuid;
  platform::TypedUuid policy_uuid;
  platform::TypedUuid transaction_uuid;
  platform::TypedUuid capacity_evidence_uuid;
};

FixtureIds MakeIds(platform::u64 seed) {
  return {MakeUuid(platform::UuidKind::database, 100 + seed),
          MakeUuid(platform::UuidKind::filespace, 200 + seed),
          MakeUuid(platform::UuidKind::object, 300 + seed),
          MakeUuid(platform::UuidKind::transaction, 400 + seed),
          MakeUuid(platform::UuidKind::object, 500 + seed)};
}

agents::PageAllocationManagerPolicy Policy(const FixtureIds& ids) {
  auto policy = agents::DefaultPageAllocationManagerPolicy();
  policy.database_uuid = ids.database_uuid;
  policy.filespace_uuid = ids.filespace_uuid;
  policy.policy_uuid = ids.policy_uuid;
  policy.minimum_free_pages = 4;
  policy.target_free_pages = 8;
  policy.low_water_notify_ratio = 0.50;
  policy.capacity_request_allowed = false;
  policy.capacity_request_policy_explicit = false;
  policy.live_preallocation_allowed = true;
  policy.live_preallocation_policy_explicit = true;
  policy.capacity_evidence_required = true;
  return policy;
}

agents::PageAllocationManagerMetricSnapshot Snapshot(const FixtureIds& ids) {
  agents::PageAllocationManagerMetricSnapshot snapshot;
  snapshot.database_uuid = ids.database_uuid;
  snapshot.filespace_uuid = ids.filespace_uuid;
  snapshot.policy_uuid = ids.policy_uuid;
  snapshot.page_family = "data";
  snapshot.free_pages = 12;
  snapshot.released_pages = 2;
  snapshot.reserved_pages = 2;
  snapshot.preallocated_pages = 1;
  snapshot.allocated_pages = 128;
  snapshot.target_free_deficit_pages = 0;
  snapshot.preallocation_target_pages = 7;
  snapshot.preallocation_deficit_pages = 6;
  snapshot.metrics_present = true;
  snapshot.metrics_fresh = true;
  snapshot.metrics_trusted = true;
  snapshot.scope_compatible = true;
  snapshot.allocation_failure_signal = false;
  snapshot.allocation_failures_total = 0;
  return snapshot;
}

agents::PageAllocationManagerActionContext ActionContext(const FixtureIds& ids) {
  agents::PageAllocationManagerActionContext context;
  context.present = true;
  context.engine_authoritative = true;
  context.transaction_uuid = ids.transaction_uuid;
  context.local_transaction_id = 71;
  context.page_generation = 9;
  context.durability_fence_satisfied = true;
  context.capacity_evidence_present = true;
  context.capacity_evidence_fresh = true;
  context.capacity_evidence_scope_compatible = true;
  context.capacity_evidence_uuid = ids.capacity_evidence_uuid;
  context.capacity_evidence_free_pages = 16;
  context.cluster_route_requested = false;
  return context;
}

page::PageAllocationLedger Ledger(const FixtureIds& ids,
                                  platform::u64 start_page = 500,
                                  platform::u64 page_count = 16) {
  page::PageAllocationLedger ledger;
  ledger.database_uuid = ids.database_uuid;
  ledger.filespace_uuid = ids.filespace_uuid;
  ledger.free_extents.push_back({start_page, page_count});
  return ledger;
}

platform::u64 FreeExtentPages(const page::PageAllocationLedger& ledger) {
  platform::u64 total = 0;
  for (const auto& extent : ledger.free_extents) {
    total += extent.page_count;
  }
  return total;
}

struct LedgerSnapshot {
  std::size_t allocations = 0;
  std::size_t evidence = 0;
  std::size_t free_extent_count = 0;
  platform::u64 free_pages = 0;
  platform::u64 next_allocation_seed = 0;
};

LedgerSnapshot Capture(const page::PageAllocationLedger& ledger) {
  return {ledger.allocations.size(),
          ledger.evidence.size(),
          ledger.free_extents.size(),
          FreeExtentPages(ledger),
          ledger.next_allocation_seed};
}

void RequireUnchanged(const page::PageAllocationLedger& ledger,
                      const LedgerSnapshot& before,
                      const std::string& label) {
  const auto after = Capture(ledger);
  Require(after.allocations == before.allocations, label + " changed allocations");
  Require(after.evidence == before.evidence, label + " changed evidence");
  Require(after.free_extent_count == before.free_extent_count, label + " changed extents");
  Require(after.free_pages == before.free_pages, label + " changed free pages");
  Require(after.next_allocation_seed == before.next_allocation_seed,
          label + " changed allocation identity seed");
}

void ExpectFailNoMutation(const agents::PageAllocationManagerTickResult& result,
                          const page::PageAllocationLedger& ledger,
                          const LedgerSnapshot& before,
                          const page::PageFilespaceAgentRequestQueue& queue,
                          const std::string& diagnostic_code,
                          const std::string& label) {
  Require(!result.ok(), label + " unexpectedly succeeded");
  Require(result.fail_closed, label + " did not fail closed");
  Require(result.diagnostic.diagnostic_code == diagnostic_code,
          label + " diagnostic mismatch: " + result.diagnostic.diagnostic_code);
  Require(!result.ledger_state_changed, label + " reported ledger mutation");
  Require(!result.accepted_evidence, label + " accepted evidence on refusal");
  Require(result.preallocated_pages == 0, label + " reported preallocated pages");
  Require(queue.records.empty(), label + " enqueued a filespace request");
  RequireUnchanged(ledger, before, label);
}

void RunFailureCase(
    const std::string& label,
    const std::string& diagnostic_code,
    const std::function<void(agents::PageAllocationManagerMetricSnapshot&,
                             agents::PageAllocationManagerPolicy&,
                             agents::PageAllocationManagerActionContext&,
                             page::PageAllocationLedger&)>& mutate) {
  const auto ids = MakeIds(1000 + label.size());
  auto snapshot = Snapshot(ids);
  auto policy = Policy(ids);
  auto context = ActionContext(ids);
  auto ledger = Ledger(ids);
  page::PageFilespaceAgentRequestQueue queue;
  mutate(snapshot, policy, context, ledger);
  const auto before = Capture(ledger);

  const auto result = agents::EvaluatePageAllocationManagerTick(
      &queue, &ledger, snapshot, policy, context);
  ExpectFailNoMutation(result, ledger, before, queue, diagnostic_code, label);
  Require(!result.direct_action_attempted, label + " should have failed before storage action");
}

void TestDefaultPolicySuppressesWithoutLedgerMutation() {
  const auto ids = MakeIds(1);
  auto policy = Policy(ids);
  policy.live_preallocation_allowed = false;
  policy.live_preallocation_policy_explicit = false;
  auto ledger = Ledger(ids);
  page::PageFilespaceAgentRequestQueue queue;
  const auto before = Capture(ledger);

  const auto result = agents::EvaluatePageAllocationManagerTick(
      &queue, &ledger, Snapshot(ids), policy, ActionContext(ids));

  Require(result.ok(), "default policy suppression failed");
  Require(result.decision == agents::PageAllocationManagerDecisionKind::preallocation_suppressed,
          "default policy did not suppress live preallocation");
  Require(result.diagnostic.diagnostic_code == "PAGE_AGENT.PREALLOCATION_SUPPRESSED",
          "default suppression diagnostic mismatch");
  Require(result.preallocation_suppressed && result.preallocation_recommended,
          "default suppression flags mismatch");
  Require(!result.direct_action_attempted, "default suppression attempted direct action");
  Require(!result.ledger_state_changed, "default suppression reported ledger mutation");
  Require(queue.records.empty(), "default suppression enqueued a filespace request");
  RequireUnchanged(ledger, before, "default suppression");
}

void TestExplicitPolicyPreallocatesWithEvidence() {
  const auto ids = MakeIds(2);
  auto ledger = Ledger(ids);
  page::PageFilespaceAgentRequestQueue queue;
  const auto before = Capture(ledger);

  const auto result = agents::EvaluatePageAllocationManagerTick(
      &queue, &ledger, Snapshot(ids), Policy(ids), ActionContext(ids));

  Require(result.ok(), "direct preallocation failed: " + result.diagnostic.diagnostic_code);
  Require(result.decision == agents::PageAllocationManagerDecisionKind::preallocation_completed,
          "direct preallocation decision mismatch");
  Require(result.direct_action_attempted, "direct preallocation did not attempt storage action");
  Require(result.ledger_state_changed, "direct preallocation did not report ledger mutation");
  Require(result.accepted_evidence, "direct preallocation evidence was not accepted");
  Require(result.requested_pages == 6 && result.preallocated_pages == 6,
          "direct preallocation page counts mismatch");
  Require(result.preallocation_uuid.valid(), "direct preallocation UUID missing");
  Require(result.preallocation_evidence.diagnostic_code ==
              "SB-STORAGE-PAGE-PREALLOCATION-PREALLOCATED",
          "direct preallocation evidence diagnostic mismatch");
  Require(result.preallocation_evidence.new_state ==
              page::PageAllocationLifecycleState::preallocated,
          "direct preallocation did not use preallocated state");
  Require(result.preallocation_evidence.durable_state_changed &&
              result.preallocation_evidence.durability_fence_satisfied &&
              result.preallocation_evidence.capacity_evidence_accepted,
          "direct preallocation evidence durability fields mismatch");
  Require(queue.records.empty(), "direct preallocation enqueued a filespace request");
  Require(ledger.allocations.size() == before.allocations + 1,
          "direct preallocation allocation count mismatch");
  Require(ledger.evidence.size() == before.evidence + 1,
          "direct preallocation evidence count mismatch");
  Require(FreeExtentPages(ledger) == before.free_pages - 6,
          "direct preallocation did not consume free extent pages");
  Require(ledger.allocations.back().state == page::PageAllocationLifecycleState::preallocated,
          "ledger allocation state is not preallocated");

  const auto recovery = page::ClassifyPageAllocationLedgerForRecovery(ledger);
  Require(recovery.ok(), "preallocated recovery classification failed");
  bool retained_preallocated = false;
  bool unsafe_reuse = false;
  for (const auto& classification : recovery.classifications) {
    if (classification.observed_state == page::PageAllocationLifecycleState::preallocated) {
      retained_preallocated =
          classification.action == page::PageAllocationRecoveryAction::retain &&
          !classification.fail_closed;
      unsafe_reuse = classification.action ==
                    page::PageAllocationRecoveryAction::release_to_free_map;
    }
  }
  Require(retained_preallocated, "preallocated recovery did not retain the pool");
  Require(!unsafe_reuse, "preallocated recovery became unsafe reuse");
}

void TestStoragePreallocationRefusalDoesNotPartiallyMutate() {
  const auto ids = MakeIds(3);
  auto ledger = Ledger(ids);
  ledger.free_extents.clear();
  ledger.free_extents.push_back({800, 3});
  ledger.free_extents.push_back({900, 3});
  page::PageFilespaceAgentRequestQueue queue;
  const auto before = Capture(ledger);

  const auto result = agents::EvaluatePageAllocationManagerTick(
      &queue, &ledger, Snapshot(ids), Policy(ids), ActionContext(ids));

  ExpectFailNoMutation(result,
                       ledger,
                       before,
                       queue,
                       "SB-STORAGE-PAGE-PREALLOCATION-INSUFFICIENT-FREE-SPACE",
                       "fragmented free extent storage refusal");
  Require(result.direct_action_attempted,
          "fragmented free extent refusal did not reach storage lifecycle");
}

void TestMissingLedgerFailsClosedWithoutQueueMutation() {
  const auto ids = MakeIds(4);
  page::PageFilespaceAgentRequestQueue queue;
  const auto result = agents::EvaluatePageAllocationManagerTick(
      &queue, nullptr, Snapshot(ids), Policy(ids), ActionContext(ids));

  Require(!result.ok(), "missing ledger unexpectedly succeeded");
  Require(result.fail_closed, "missing ledger did not fail closed");
  Require(result.diagnostic.diagnostic_code == "PAGE_AGENT.ACTION_CONTEXT_REQUIRED",
          "missing ledger diagnostic mismatch: " + result.diagnostic.diagnostic_code);
  Require(!result.direct_action_attempted, "missing ledger attempted direct action");
  Require(!result.ledger_state_changed, "missing ledger reported ledger mutation");
  Require(queue.records.empty(), "missing ledger enqueued a filespace request");
}

void TestFailClosedInputsDoNotMutateLedgerOrQueue() {
  RunFailureCase(
      "insufficient free extents",
      "PAGE_AGENT.FILESPACE_CAPACITY_REQUIRED",
      [](auto&, auto&, auto&, auto& ledger) {
        ledger.free_extents.clear();
        ledger.free_extents.push_back({700, 2});
      });

  RunFailureCase(
      "missing action context",
      "PAGE_AGENT.ACTION_CONTEXT_REQUIRED",
      [](auto&, auto&, auto& context, auto&) {
        context.present = false;
      });

  RunFailureCase(
      "missing capacity evidence",
      "PAGE_AGENT.CAPACITY_EVIDENCE_REQUIRED",
      [](auto&, auto&, auto& context, auto&) {
        context.capacity_evidence_present = false;
        context.capacity_evidence_uuid = platform::TypedUuid{};
      });

  RunFailureCase(
      "invalid durability fence",
      "PAGE_AGENT.DURABILITY_FENCE_REQUIRED",
      [](auto&, auto&, auto& context, auto&) {
        context.durability_fence_satisfied = false;
      });

  RunFailureCase(
      "invalid transaction identity",
      "PAGE_AGENT.TRANSACTION_ID_INVALID",
      [](auto&, auto&, auto& context, auto&) {
        context.local_transaction_id = 0;
      });

  RunFailureCase(
      "policy disabled",
      "PAGE_AGENT.PREALLOCATION_POLICY_DISABLED",
      [](auto&, auto& policy, auto&, auto&) {
        policy.live_preallocation_allowed = false;
        policy.live_preallocation_policy_explicit = true;
      });

  RunFailureCase(
      "stale metrics",
      "PAGE_AGENT.METRIC_STALE",
      [](auto& snapshot, auto&, auto&, auto&) {
        snapshot.metrics_fresh = false;
      });

  RunFailureCase(
      "allocation failure signal",
      "PAGE_AGENT.ALLOCATION_FAILURE",
      [](auto& snapshot, auto&, auto&, auto&) {
        snapshot.allocation_failure_signal = true;
      });
}

}  // namespace

int main() {
  TestDefaultPolicySuppressesWithoutLedgerMutation();
  TestExplicitPolicyPreallocatesWithEvidence();
  TestStoragePreallocationRefusalDoesNotPartiallyMutate();
  TestMissingLedgerFailsClosedWithoutQueueMutation();
  TestFailClosedInputsDoNotMutateLedgerOrQueue();
  return EXIT_SUCCESS;
}
