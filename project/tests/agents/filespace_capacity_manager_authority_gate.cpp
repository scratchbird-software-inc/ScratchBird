// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/filespace_capacity_manager.hpp"
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
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1880000000000ull + seed);
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

agents::FilespaceCapacityManagerPolicy Policy(const FixtureIds& ids) {
  auto policy = agents::DefaultFilespaceCapacityManagerPolicy();
  policy.database_uuid = ids.database_uuid;
  policy.filespace_uuid = ids.filespace_uuid;
  policy.policy_uuid = ids.policy_uuid;
  policy.minimum_free_pages = 4;
  policy.target_free_pages = 8;
  policy.max_capacity_window_pages = 8;
  return policy;
}

agents::FilespaceCapacityManagerPolicy FullPolicy(const FixtureIds& ids) {
  auto policy = Policy(ids);
  policy.capacity_window_allowed = true;
  policy.capacity_processing_policy_explicit = true;
  policy.expand_allowed = true;
  policy.expand_request_policy_explicit = true;
  policy.move_allowed = true;
  policy.move_request_policy_explicit = true;
  policy.shrink_allowed = true;
  policy.shrink_request_policy_explicit = true;
  policy.truncate_allowed = true;
  policy.truncate_request_policy_explicit = true;
  policy.quarantine_allowed = true;
  policy.quarantine_request_policy_explicit = true;
  policy.shadow_promotion_allowed = true;
  policy.shadow_promotion_policy_explicit = true;
  return policy;
}

agents::FilespaceCapacityManagerMetricSnapshot Snapshot(const FixtureIds& ids) {
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

agents::FilespaceCapacityManagerSafetyState SafeState() {
  agents::FilespaceCapacityManagerSafetyState safety;
  safety.startup_complete = true;
  safety.recovery_complete = true;
  safety.maintenance_mode = false;
  safety.maintenance_allows_capacity_windows = true;
  safety.engine_authoritative = true;
  return safety;
}

agents::FilespaceCapacityManagerActionRequest Request(
    const FixtureIds& ids,
    agents::FilespaceCapacityManagerActionKind action,
    platform::u64 seed = 1) {
  agents::FilespaceCapacityManagerActionRequest request;
  request.action = action;
  request.request_uuid = MakeUuid(platform::UuidKind::object, 1000 + seed);
  request.evidence_uuid = MakeUuid(platform::UuidKind::object, 2000 + seed);
  request.database_uuid = ids.database_uuid;
  request.filespace_uuid = ids.filespace_uuid;
  request.source_filespace_uuid = ids.filespace_uuid;
  request.target_filespace_uuid = MakeUuid(platform::UuidKind::filespace, 3000 + seed);
  request.policy_uuid = ids.policy_uuid;
  request.capacity_proof_uuid = MakeUuid(platform::UuidKind::object, 4000 + seed);
  request.device_proof_uuid = MakeUuid(platform::UuidKind::object, 5000 + seed);
  request.object_list_proof_uuid = MakeUuid(platform::UuidKind::object, 6000 + seed);
  request.page_relocation_request_uuid = MakeUuid(platform::UuidKind::object, 7000 + seed);
  request.shrink_ready_evidence_uuid = MakeUuid(platform::UuidKind::object, 8000 + seed);
  request.device_health_evidence_uuid = MakeUuid(platform::UuidKind::object, 9000 + seed);
  request.checksum_evidence_uuid = MakeUuid(platform::UuidKind::object, 10000 + seed);
  request.unknown_page_evidence_uuid = MakeUuid(platform::UuidKind::object, 11000 + seed);
  request.primary_degradation_proof_uuid = MakeUuid(platform::UuidKind::object, 12000 + seed);
  request.candidate_readiness_proof_uuid = MakeUuid(platform::UuidKind::object, 13000 + seed);
  request.target_bytes = 64 * 1024;
  request.safe_tail_bytes = 32 * 1024;
  request.blocker_count = 0;
  request.live_action_requested = true;
  request.explicit_evidence = true;
  request.catalog_persistence_migration_requirement_present = true;
  request.has_obs_agent_action_approve = true;
  request.has_filespace_lifecycle_control = true;
  request.has_lifecycle_truncate_control = true;
  request.has_obs_agent_recommendation_read = true;
  return request;
}

void ExpectRefused(const agents::FilespaceCapacityManagerActionResult& result,
                   const std::string& diagnostic_code,
                   const std::string& label) {
  Require(!result.ok(), label + " unexpectedly succeeded");
  Require(result.fail_closed && result.refused, label + " did not fail closed");
  Require(result.diagnostic.diagnostic_code == diagnostic_code,
          label + " diagnostic mismatch: " + result.diagnostic.diagnostic_code);
  Require(result.evidence.diagnostic_code == diagnostic_code,
          label + " evidence diagnostic mismatch");
  Require(result.evidence.durable_state_changed, label + " did not write evidence");
  Require(!result.physical_filespace_mutation_attempted &&
              !result.page_ledger_mutation_attempted,
          label + " attempted forbidden mutation");
}

void ExpectAuthorized(const agents::FilespaceCapacityManagerActionResult& result,
                      const std::string& label) {
  Require(result.ok(), label + " failed: " + result.diagnostic.diagnostic_code);
  Require(result.authorized, label + " was not authorized");
  Require(result.decision ==
              agents::FilespaceCapacityManagerDecisionKind::action_authorized,
          label + " decision mismatch");
  Require(result.evidence.durable_state_changed, label + " evidence missing");
  Require(!result.physical_filespace_mutation_attempted &&
              !result.page_ledger_mutation_attempted,
          label + " attempted forbidden mutation");
}

page::PageFilespaceAgentRequestQueue QueueWithExtendRequest(const FixtureIds& ids) {
  page::PageFilespaceAgentRequestQueue queue;
  page::PageFilespaceAgentQueueRecord record;
  record.sequence = queue.next_sequence++;
  record.request.request_uuid = MakeUuid(platform::UuidKind::object, 50000);
  record.request.database_uuid = ids.database_uuid;
  record.request.filespace_uuid = ids.filespace_uuid;
  record.request.policy_uuid = ids.policy_uuid;
  record.request.kind = page::PageFilespaceAgentRequestKind::extend_filespace;
  record.request.state = page::PageFilespaceAgentRequestState::waiting_filespace_agent;
  record.request.requesting_agent = "page_allocation_manager";
  record.request.responding_agent = "filespace_capacity_manager";
  record.request.page_family = "data";
  record.request.requested_pages = 4;
  record.allowed = true;
  record.filespace_agent_action_required = true;
  record.target_free_pages = 8;
  record.low_water_pages = 4;
  record.diagnostic_code = "ok";
  record.evidence_state = "request_waiting_for_owner";
  record.evidence_id = MakeUuid(platform::UuidKind::object, 50001);
  record.explicit_evidence = true;
  queue.records.push_back(record);
  return queue;
}

void TestAllSixActionRowsAreEvaluated() {
  const auto ids = MakeIds(1);
  const auto snapshot = Snapshot(ids);
  const auto policy = FullPolicy(ids);
  const auto safety = SafeState();

  const std::vector<agents::FilespaceCapacityManagerActionKind> accepted = {
      agents::FilespaceCapacityManagerActionKind::request_filespace_expand,
      agents::FilespaceCapacityManagerActionKind::request_filespace_move,
      agents::FilespaceCapacityManagerActionKind::request_filespace_shrink,
      agents::FilespaceCapacityManagerActionKind::request_filespace_truncate,
      agents::FilespaceCapacityManagerActionKind::request_filespace_quarantine};
  platform::u64 seed = 10;
  for (const auto action : accepted) {
    const auto result = agents::EvaluateFilespaceCapacityManagerAction(
        Request(ids, action, seed++), snapshot, policy, safety);
    ExpectAuthorized(result, agents::FilespaceCapacityManagerActionKindName(action));
  }

  const auto promotion = agents::EvaluateFilespaceCapacityManagerAction(
      Request(ids,
              agents::FilespaceCapacityManagerActionKind::recommend_primary_shadow_promotion,
              99),
      snapshot,
      policy,
      safety);
  Require(promotion.ok(), "promotion recommendation failed");
  Require(promotion.recommended, "promotion was not a recommendation");
  Require(!promotion.physical_filespace_mutation_attempted &&
              !promotion.page_ledger_mutation_attempted,
          "promotion attempted forbidden mutation");
}

void TestDefaultRecommendOnlyRefusalsAndSuppression() {
  const auto ids = MakeIds(2);
  const auto snapshot = Snapshot(ids);
  const auto policy = Policy(ids);
  const auto safety = SafeState();

  const auto expand = agents::EvaluateFilespaceCapacityManagerAction(
      Request(ids, agents::FilespaceCapacityManagerActionKind::request_filespace_expand),
      snapshot,
      policy,
      safety);
  ExpectRefused(expand, "FILESPACE_AGENT.ACTION_RECOMMEND_ONLY", "default expand");

  const auto promotion = agents::EvaluateFilespaceCapacityManagerAction(
      Request(ids,
              agents::FilespaceCapacityManagerActionKind::recommend_primary_shadow_promotion),
      snapshot,
      policy,
      safety);
  Require(promotion.ok(), "default promotion should suppress without failing");
  Require(promotion.suppressed, "default promotion was not suppressed");
  Require(promotion.diagnostic.diagnostic_code == "FILESPACE_AGENT.ACTION_RECOMMEND_ONLY",
          "default promotion diagnostic mismatch");
}

void TestExplicitExpandAuthorityAndFailureModes() {
  const auto ids = MakeIds(3);
  const auto policy = FullPolicy(ids);
  const auto safety = SafeState();

  const auto accepted = agents::EvaluateFilespaceCapacityManagerAction(
      Request(ids, agents::FilespaceCapacityManagerActionKind::request_filespace_expand),
      Snapshot(ids),
      policy,
      safety);
  ExpectAuthorized(accepted, "explicit expand");

  auto missing_request_uuid = Request(
      ids, agents::FilespaceCapacityManagerActionKind::request_filespace_expand, 30);
  missing_request_uuid.request_uuid = platform::TypedUuid{};
  ExpectRefused(agents::EvaluateFilespaceCapacityManagerAction(
                    missing_request_uuid, Snapshot(ids), policy, safety),
                "FILESPACE_AGENT.EVIDENCE_REQUIRED",
                "expand missing request uuid");

  auto missing_permission = Request(
      ids, agents::FilespaceCapacityManagerActionKind::request_filespace_expand, 31);
  missing_permission.has_filespace_lifecycle_control = false;
  ExpectRefused(agents::EvaluateFilespaceCapacityManagerAction(
                    missing_permission, Snapshot(ids), policy, safety),
                "FILESPACE_AGENT.PERMISSION_DENIED",
                "expand missing permission");

  auto missing_proof = Request(
      ids, agents::FilespaceCapacityManagerActionKind::request_filespace_expand, 32);
  missing_proof.device_proof_uuid = platform::TypedUuid{};
  ExpectRefused(agents::EvaluateFilespaceCapacityManagerAction(
                    missing_proof, Snapshot(ids), policy, safety),
                "FILESPACE_AGENT.EVIDENCE_REQUIRED",
                "expand missing proof");

  auto stale = Snapshot(ids);
  stale.metrics_fresh = false;
  ExpectRefused(agents::EvaluateFilespaceCapacityManagerAction(
                    Request(ids,
                            agents::FilespaceCapacityManagerActionKind::request_filespace_expand,
                            33),
                    stale,
                    policy,
                    safety),
                "FILESPACE_AGENT.METRIC_STALE",
                "expand stale metrics");
}

void TestShrinkAndTruncateRequirePageProofAndNoBlockers() {
  const auto ids = MakeIds(4);
  const auto policy = FullPolicy(ids);
  const auto snapshot = Snapshot(ids);
  const auto safety = SafeState();

  auto shrink_missing = Request(
      ids, agents::FilespaceCapacityManagerActionKind::request_filespace_shrink, 41);
  shrink_missing.page_relocation_request_uuid = platform::TypedUuid{};
  ExpectRefused(agents::EvaluateFilespaceCapacityManagerAction(
                    shrink_missing, snapshot, policy, safety),
                "FILESPACE_AGENT.EVIDENCE_REQUIRED",
                "shrink missing page proof");

  auto shrink_blocked = Request(
      ids, agents::FilespaceCapacityManagerActionKind::request_filespace_shrink, 42);
  shrink_blocked.blocker_count = 1;
  ExpectRefused(agents::EvaluateFilespaceCapacityManagerAction(
                    shrink_blocked, snapshot, policy, safety),
                "FILESPACE_AGENT.SHRINK_BLOCKED",
                "shrink blocked");

  ExpectAuthorized(agents::EvaluateFilespaceCapacityManagerAction(
                     Request(ids,
                             agents::FilespaceCapacityManagerActionKind::request_filespace_shrink,
                             43),
                     snapshot,
                     policy,
                     safety),
                 "shrink accepted");

  auto truncate_missing = Request(
      ids, agents::FilespaceCapacityManagerActionKind::request_filespace_truncate, 44);
  truncate_missing.shrink_ready_evidence_uuid = platform::TypedUuid{};
  ExpectRefused(agents::EvaluateFilespaceCapacityManagerAction(
                    truncate_missing, snapshot, policy, safety),
                "FILESPACE_AGENT.EVIDENCE_REQUIRED",
                "truncate missing shrink-ready proof");

  auto truncate_blocked = Request(
      ids, agents::FilespaceCapacityManagerActionKind::request_filespace_truncate, 45);
  truncate_blocked.blocker_count = 1;
  ExpectRefused(agents::EvaluateFilespaceCapacityManagerAction(
                    truncate_blocked, snapshot, policy, safety),
                "FILESPACE_AGENT.SHRINK_BLOCKED",
                "truncate blocked");

  ExpectAuthorized(agents::EvaluateFilespaceCapacityManagerAction(
                     Request(ids,
                             agents::FilespaceCapacityManagerActionKind::request_filespace_truncate,
                             46),
                     snapshot,
                     policy,
                     safety),
                 "truncate accepted");
}

void TestQuarantineAndPromotionSpecialGates() {
  const auto ids = MakeIds(5);
  const auto policy = FullPolicy(ids);
  const auto snapshot = Snapshot(ids);
  const auto safety = SafeState();

  auto quarantine_missing = Request(
      ids, agents::FilespaceCapacityManagerActionKind::request_filespace_quarantine, 51);
  quarantine_missing.device_health_evidence_uuid = platform::TypedUuid{};
  quarantine_missing.checksum_evidence_uuid = platform::TypedUuid{};
  quarantine_missing.unknown_page_evidence_uuid = platform::TypedUuid{};
  ExpectRefused(agents::EvaluateFilespaceCapacityManagerAction(
                    quarantine_missing, snapshot, policy, safety),
                "FILESPACE_AGENT.EVIDENCE_REQUIRED",
                "quarantine missing proof");

  quarantine_missing.operator_review_requested = true;
  const auto review = agents::EvaluateFilespaceCapacityManagerAction(
      quarantine_missing, snapshot, policy, safety);
  Require(review.ok() && review.approval_required,
          "quarantine operator review did not become approval-required");

  auto quarantine = Request(
      ids, agents::FilespaceCapacityManagerActionKind::request_filespace_quarantine, 52);
  quarantine.device_health_evidence_uuid = platform::TypedUuid{};
  quarantine.unknown_page_evidence_uuid = platform::TypedUuid{};
  ExpectAuthorized(agents::EvaluateFilespaceCapacityManagerAction(
                     quarantine, snapshot, policy, safety),
                 "quarantine checksum proof");

  auto quarantine_absent_stale = Request(
      ids, agents::FilespaceCapacityManagerActionKind::request_filespace_quarantine, 521);
  quarantine_absent_stale.device_health_evidence_uuid = platform::TypedUuid{};
  quarantine_absent_stale.unknown_page_evidence_uuid = platform::TypedUuid{};
  quarantine_absent_stale.device_health_evidence_fresh = false;
  quarantine_absent_stale.unknown_page_evidence_fresh = false;
  ExpectAuthorized(agents::EvaluateFilespaceCapacityManagerAction(
                       quarantine_absent_stale, snapshot, policy, safety),
                   "quarantine ignored absent stale proofs");

  auto promotion_no_read = Request(
      ids,
      agents::FilespaceCapacityManagerActionKind::recommend_primary_shadow_promotion,
      53);
  promotion_no_read.has_obs_agent_recommendation_read = false;
  ExpectRefused(agents::EvaluateFilespaceCapacityManagerAction(
                    promotion_no_read, snapshot, policy, safety),
                "FILESPACE_AGENT.PERMISSION_DENIED",
                "promotion missing read permission");

  auto no_shadow_policy = Policy(ids);
  auto promotion = Request(
      ids,
      agents::FilespaceCapacityManagerActionKind::recommend_primary_shadow_promotion,
      54);
  const auto suppressed = agents::EvaluateFilespaceCapacityManagerAction(
      promotion, snapshot, no_shadow_policy, safety);
  Require(suppressed.ok() && suppressed.suppressed,
          "promotion without shadow policy was not suppressed");

  auto missing_proof = Request(
      ids,
      agents::FilespaceCapacityManagerActionKind::recommend_primary_shadow_promotion,
      55);
  missing_proof.candidate_readiness_proof_uuid = platform::TypedUuid{};
  const auto proof_suppressed = agents::EvaluateFilespaceCapacityManagerAction(
      missing_proof, snapshot, policy, safety);
  Require(proof_suppressed.ok() && proof_suppressed.suppressed &&
              proof_suppressed.diagnostic.diagnostic_code ==
                  "FILESPACE_AGENT.EVIDENCE_REQUIRED",
          "promotion missing proof was not suppressed");
}

void TestPageBoundaryForbiddenActions() {
  const auto ids = MakeIds(6);
  const auto policy = FullPolicy(ids);
  const auto snapshot = Snapshot(ids);
  const auto safety = SafeState();
  const std::vector<agents::FilespaceCapacityManagerActionKind> forbidden = {
      agents::FilespaceCapacityManagerActionKind::forbidden_allocate_page,
      agents::FilespaceCapacityManagerActionKind::forbidden_relocate_page,
      agents::FilespaceCapacityManagerActionKind::forbidden_compact_page_family,
      agents::FilespaceCapacityManagerActionKind::forbidden_rebuild_index,
      agents::FilespaceCapacityManagerActionKind::forbidden_advance_mga_cleanup};
  platform::u64 seed = 60;
  for (const auto action : forbidden) {
    ExpectRefused(agents::EvaluateFilespaceCapacityManagerAction(
                      Request(ids, action, seed++), snapshot, policy, safety),
                  "FILESPACE_AGENT.PAGE_AUTHORITY_REQUIRED",
                  agents::FilespaceCapacityManagerActionKindName(action));
  }
}

void TestCapacityWindowQueueRequiresExplicitExpandAuthority() {
  const auto ids = MakeIds(7);
  auto policy = Policy(ids);
  policy.capacity_window_allowed = true;
  policy.capacity_processing_policy_explicit = true;
  auto queue = QueueWithExtendRequest(ids);

  auto result = agents::EvaluateFilespaceCapacityManagerTick(
      &queue, Snapshot(ids), policy, SafeState());
  Require(!result.ok(), "queue path accepted without expand authority");
  Require(result.diagnostic.diagnostic_code == "FILESPACE_AGENT.ACTION_RECOMMEND_ONLY",
          "queue missing expand authority diagnostic mismatch: " +
              result.diagnostic.diagnostic_code);
  Require(queue.records[0].request.state == page::PageFilespaceAgentRequestState::refused,
          "queue missing expand authority did not refuse record");

  policy.expand_allowed = true;
  policy.expand_request_policy_explicit = true;
  auto stale_proof = Snapshot(ids);
  stale_proof.expand_device_proof_present = false;
  queue = QueueWithExtendRequest(ids);
  result = agents::EvaluateFilespaceCapacityManagerTick(
      &queue, stale_proof, policy, SafeState());
  Require(!result.ok(), "queue path accepted without expand proof");
  Require(result.diagnostic.diagnostic_code == "FILESPACE_AGENT.EVIDENCE_REQUIRED",
          "queue missing expand proof diagnostic mismatch: " +
              result.diagnostic.diagnostic_code);

  auto stale_expand_proof = Snapshot(ids);
  stale_expand_proof.expand_device_proof_fresh = false;
  queue = QueueWithExtendRequest(ids);
  result = agents::EvaluateFilespaceCapacityManagerTick(
      &queue, stale_expand_proof, policy, SafeState());
  Require(!result.ok(), "queue path accepted stale expand proof");
  Require(result.diagnostic.diagnostic_code == "FILESPACE_AGENT.METRIC_STALE",
          "queue stale expand proof diagnostic mismatch: " +
              result.diagnostic.diagnostic_code);
}

}  // namespace

int main() {
  TestAllSixActionRowsAreEvaluated();
  TestDefaultRecommendOnlyRefusalsAndSuppression();
  TestExplicitExpandAuthorityAndFailureModes();
  TestShrinkAndTruncateRequirePageProofAndNoBlockers();
  TestQuarantineAndPromotionSpecialGates();
  TestPageBoundaryForbiddenActions();
  TestCapacityWindowQueueRequiresExplicitExpandAuthority();
  return EXIT_SUCCESS;
}
