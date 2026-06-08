// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/storage_health_manager.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents::implemented_agents;
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
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1890000000000ull + seed);
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

agents::StorageHealthManagerPolicy Policy(const FixtureIds& ids) {
  auto policy = agents::DefaultStorageHealthManagerPolicy();
  policy.database_uuid = ids.database_uuid;
  policy.filespace_uuid = ids.filespace_uuid;
  policy.policy_uuid = ids.policy_uuid;
  policy.present = true;
  policy.valid = true;
  policy.quarantine_recommendation_allowed = true;
  policy.operator_review_route_allowed = true;
  policy.storage_cost_recommendation_allowed = true;
  policy.health_summary_allowed = true;
  policy.checksum_failure_quarantine_threshold = 1;
  policy.unknown_page_quarantine_threshold = 1;
  policy.fsync_p99_cost_update_threshold_microseconds = 1000;
  return policy;
}

agents::StorageHealthManagerMetricSnapshot Snapshot(const FixtureIds& ids) {
  agents::StorageHealthManagerMetricSnapshot snapshot;
  snapshot.database_uuid = ids.database_uuid;
  snapshot.filespace_uuid = ids.filespace_uuid;
  snapshot.policy_uuid = ids.policy_uuid;
  snapshot.filespace_health = agents::StorageHealthSeverity::critical;
  snapshot.device_error_count = 1;
  snapshot.checksum_failure_count = 1;
  snapshot.unknown_page_count = 1;
  snapshot.page_allocation_failure_count = 1;
  snapshot.fsync_latency_p99_microseconds = 5000;
  return snapshot;
}

agents::StorageHealthManagerActionRequest Request(
    const FixtureIds& ids,
    agents::StorageHealthManagerActionKind action,
    agents::StorageHealthEvidenceKind evidence_kind,
    platform::u64 seed = 1) {
  agents::StorageHealthManagerActionRequest request;
  request.action = action;
  request.evidence_kind = evidence_kind;
  request.request_uuid = MakeUuid(platform::UuidKind::object, 1000 + seed);
  request.evidence_uuid = MakeUuid(platform::UuidKind::object, 2000 + seed);
  request.database_uuid = ids.database_uuid;
  request.filespace_uuid = ids.filespace_uuid;
  request.policy_uuid = ids.policy_uuid;
  request.metric_evidence_uuid = MakeUuid(platform::UuidKind::object, 3000 + seed);
  request.explicit_evidence = true;
  request.metric_evidence_fresh = true;
  request.metric_evidence_trusted = true;
  return request;
}

void ExpectNoMutation(const agents::StorageHealthManagerActionResult& result,
                      const std::string& label) {
  Require(!result.physical_filespace_mutation_attempted,
          label + " attempted physical filespace mutation");
  Require(!result.page_ledger_mutation_attempted,
          label + " attempted page ledger mutation");
  Require(!result.index_mutation_attempted,
          label + " attempted index mutation");
  Require(!result.policy_override_attempted,
          label + " attempted policy override");
  Require(!result.evidence.physical_filespace_mutation_attempted,
          label + " evidence recorded physical mutation");
  Require(!result.evidence.page_ledger_mutation_attempted,
          label + " evidence recorded page mutation");
  Require(!result.evidence.index_mutation_attempted,
          label + " evidence recorded index mutation");
  Require(!result.evidence.policy_override_attempted,
          label + " evidence recorded policy override");
}

void ExpectRefused(const agents::StorageHealthManagerActionResult& result,
                   const std::string& diagnostic_code,
                   const std::string& label) {
  Require(!result.ok(), label + " unexpectedly succeeded");
  Require(result.fail_closed && result.refused, label + " did not fail closed");
  Require(result.diagnostic.diagnostic_code == diagnostic_code,
          label + " diagnostic mismatch: " + result.diagnostic.diagnostic_code);
  Require(result.evidence.diagnostic_code == diagnostic_code,
          label + " evidence diagnostic mismatch");
  ExpectNoMutation(result, label);
}

void ExpectAcceptedEvidence(const agents::StorageHealthManagerActionResult& result,
                            const std::string& diagnostic_code,
                            const std::string& label) {
  Require(result.ok(), label + " failed: " + result.diagnostic.diagnostic_code);
  Require(result.diagnostic.diagnostic_code == diagnostic_code,
          label + " diagnostic mismatch: " + result.diagnostic.diagnostic_code);
  Require(result.evidence.diagnostic_code == diagnostic_code,
          label + " evidence diagnostic mismatch");
  Require(result.evidence.durable_state_changed, label + " evidence missing");
  ExpectNoMutation(result, label);
}

void TestAllowedActionSurface() {
  const auto ids = MakeIds(1);
  auto policy = Policy(ids);
  policy.critical_automatic_quarantine_policy = true;
  const auto snapshot = Snapshot(ids);

  const auto quarantine = agents::EvaluateStorageHealthManagerAction(
      Request(ids,
              agents::StorageHealthManagerActionKind::request_filespace_quarantine,
              agents::StorageHealthEvidenceKind::checksum_failure,
              10),
      snapshot,
      policy);
  ExpectAcceptedEvidence(quarantine,
                         "STORAGE_HEALTH_MANAGER.QUARANTINE_ROUTE_RECOMMENDATION",
                         "allowed quarantine");
  Require(quarantine.route_recommended, "quarantine did not route recommendation");
  Require(quarantine.evidence.route_target == "filespace_capacity_manager",
          "quarantine route target mismatch");

  const auto cost = agents::EvaluateStorageHealthManagerAction(
      Request(ids,
              agents::StorageHealthManagerActionKind::update_storage_cost,
              agents::StorageHealthEvidenceKind::latency_histogram,
              11),
      snapshot,
      policy);
  ExpectAcceptedEvidence(cost,
                         "STORAGE_HEALTH_MANAGER.COST_RECOMMENDATION",
                         "allowed cost update");
  Require(cost.cost_update_recommended, "cost update was not recommendation");
  Require(cost.evidence.route_target == "optimizer_cost_registry",
          "cost route target mismatch");

  const auto summary = agents::EvaluateStorageHealthManagerAction(
      Request(ids,
              agents::StorageHealthManagerActionKind::emit_storage_health_summary,
              agents::StorageHealthEvidenceKind::health_summary,
              12),
      snapshot,
      policy);
  ExpectAcceptedEvidence(summary,
                         "STORAGE_HEALTH_MANAGER.HEALTH_SUMMARY_EMITTED",
                         "allowed health summary");
  Require(summary.summary_emitted, "summary was not emitted");
  Require(summary.evidence.route_target == "storage_health_evidence",
          "summary route target mismatch");
}

void TestDefaultRecommendOnlyBehavior() {
  const auto ids = MakeIds(2);
  const auto policy = Policy(ids);
  const auto snapshot = Snapshot(ids);

  const auto quarantine = agents::EvaluateStorageHealthManagerAction(
      Request(ids,
              agents::StorageHealthManagerActionKind::request_filespace_quarantine,
              agents::StorageHealthEvidenceKind::unknown_page,
              20),
      snapshot,
      policy);
  ExpectAcceptedEvidence(quarantine,
                         "STORAGE_HEALTH_MANAGER.QUARANTINE_OPERATOR_REVIEW",
                         "default quarantine");
  Require(quarantine.operator_review_required, "default quarantine skipped review");
  Require(quarantine.evidence.reason.find("recommendation only") != std::string::npos,
          "default quarantine evidence did not say recommendation only");

  const auto cost = agents::EvaluateStorageHealthManagerAction(
      Request(ids,
              agents::StorageHealthManagerActionKind::update_storage_cost,
              agents::StorageHealthEvidenceKind::latency_histogram,
              21),
      snapshot,
      policy);
  ExpectAcceptedEvidence(cost,
                         "STORAGE_HEALTH_MANAGER.COST_RECOMMENDATION",
                         "default cost recommendation");
  Require(cost.evidence.reason.find("recommendation evidence only") !=
              std::string::npos,
          "cost evidence did not say recommendation only");
}

void TestPolicyScopeAndEvidenceFailClosed() {
  const auto ids = MakeIds(3);
  auto policy = Policy(ids);
  const auto snapshot = Snapshot(ids);

  auto missing_policy = policy;
  missing_policy.present = false;
  ExpectRefused(agents::EvaluateStorageHealthManagerAction(
                    Request(ids,
                            agents::StorageHealthManagerActionKind::emit_storage_health_summary,
                            agents::StorageHealthEvidenceKind::health_summary,
                            30),
                    snapshot,
                    missing_policy),
                "STORAGE_HEALTH_MANAGER.POLICY_REQUIRED",
                "missing policy");

  auto missing_scope_request = Request(
      ids,
      agents::StorageHealthManagerActionKind::emit_storage_health_summary,
      agents::StorageHealthEvidenceKind::health_summary,
      31);
  missing_scope_request.filespace_uuid = platform::TypedUuid{};
  ExpectRefused(agents::EvaluateStorageHealthManagerAction(
                    missing_scope_request, snapshot, policy),
                "STORAGE_HEALTH_MANAGER.SCOPE_REQUIRED",
                "missing scope");

  auto mismatch_snapshot = snapshot;
  mismatch_snapshot.filespace_uuid = MakeUuid(platform::UuidKind::filespace, 900);
  ExpectRefused(agents::EvaluateStorageHealthManagerAction(
                    Request(ids,
                            agents::StorageHealthManagerActionKind::emit_storage_health_summary,
                            agents::StorageHealthEvidenceKind::health_summary,
                            32),
                    mismatch_snapshot,
                    policy),
                "STORAGE_HEALTH_MANAGER.SCOPE_MISMATCH",
                "scope mismatch");

  auto missing_evidence = Request(
      ids,
      agents::StorageHealthManagerActionKind::emit_storage_health_summary,
      agents::StorageHealthEvidenceKind::health_summary,
      33);
  missing_evidence.evidence_uuid = platform::TypedUuid{};
  ExpectRefused(agents::EvaluateStorageHealthManagerAction(
                    missing_evidence, snapshot, policy),
                "STORAGE_HEALTH_MANAGER.EVIDENCE_REQUIRED",
                "missing evidence");
}

void TestMetricGatesFailClosed() {
  const auto ids = MakeIds(4);
  const auto policy = Policy(ids);

  auto missing = Snapshot(ids);
  missing.filespace_health_present = false;
  ExpectRefused(agents::EvaluateStorageHealthManagerAction(
                    Request(ids,
                            agents::StorageHealthManagerActionKind::emit_storage_health_summary,
                            agents::StorageHealthEvidenceKind::health_summary,
                            40),
                    missing,
                    policy),
                "STORAGE_HEALTH_MANAGER.METRIC_MISSING",
                "missing filespace health");

  auto stale = Snapshot(ids);
  stale.checksum_fresh = false;
  ExpectRefused(agents::EvaluateStorageHealthManagerAction(
                    Request(ids,
                            agents::StorageHealthManagerActionKind::request_filespace_quarantine,
                            agents::StorageHealthEvidenceKind::checksum_failure,
                            41),
                    stale,
                    policy),
                "STORAGE_HEALTH_MANAGER.METRIC_STALE",
                "stale checksum");

  auto untrusted = Snapshot(ids);
  untrusted.unknown_page_trusted = false;
  ExpectRefused(agents::EvaluateStorageHealthManagerAction(
                    Request(ids,
                            agents::StorageHealthManagerActionKind::request_filespace_quarantine,
                            agents::StorageHealthEvidenceKind::unknown_page,
                            42),
                    untrusted,
                    policy),
                "STORAGE_HEALTH_MANAGER.METRIC_UNTRUSTED",
                "untrusted unknown-page");

  auto latency_missing = Snapshot(ids);
  latency_missing.storage_latency_present = false;
  ExpectRefused(agents::EvaluateStorageHealthManagerAction(
                    Request(ids,
                            agents::StorageHealthManagerActionKind::update_storage_cost,
                            agents::StorageHealthEvidenceKind::latency_histogram,
                            43),
                    latency_missing,
                    policy),
                "STORAGE_HEALTH_MANAGER.METRIC_MISSING",
                "missing storage latency");

  auto page_stale = Snapshot(ids);
  page_stale.page_metric_fresh = false;
  ExpectRefused(agents::EvaluateStorageHealthManagerAction(
                    Request(ids,
                            agents::StorageHealthManagerActionKind::emit_storage_health_summary,
                            agents::StorageHealthEvidenceKind::health_summary,
                            44),
                    page_stale,
                    policy),
                "STORAGE_HEALTH_MANAGER.METRIC_STALE",
                "stale page metric");
}

void TestQuarantineRoutingModesAndEvidenceKinds() {
  const auto ids = MakeIds(5);
  auto policy = Policy(ids);
  auto snapshot = Snapshot(ids);

  policy.critical_automatic_quarantine_policy = true;
  const auto automatic_route = agents::EvaluateStorageHealthManagerAction(
      Request(ids,
              agents::StorageHealthManagerActionKind::request_filespace_quarantine,
              agents::StorageHealthEvidenceKind::unknown_page,
              50),
      snapshot,
      policy);
  ExpectAcceptedEvidence(automatic_route,
                         "STORAGE_HEALTH_MANAGER.QUARANTINE_ROUTE_RECOMMENDATION",
                         "automatic quarantine route");
  Require(automatic_route.evidence.route_target == "filespace_capacity_manager",
          "automatic quarantine did not route to owning agent");
  Require(automatic_route.evidence.reason.find("no physical lifecycle mutation") !=
              std::string::npos,
          "automatic quarantine evidence did not exclude mutation");

  const auto device_route = agents::EvaluateStorageHealthManagerAction(
      Request(ids,
              agents::StorageHealthManagerActionKind::request_filespace_quarantine,
              agents::StorageHealthEvidenceKind::device_error,
              501),
      snapshot,
      policy);
  ExpectAcceptedEvidence(device_route,
                         "STORAGE_HEALTH_MANAGER.QUARANTINE_ROUTE_RECOMMENDATION",
                         "device-error quarantine route");
  Require(device_route.evidence.route_target == "filespace_capacity_manager",
          "device-error quarantine did not route to owning agent");

  auto review_request = Request(
      ids,
      agents::StorageHealthManagerActionKind::request_filespace_quarantine,
      agents::StorageHealthEvidenceKind::checksum_failure,
      51);
  review_request.operator_review_requested = true;
  const auto review = agents::EvaluateStorageHealthManagerAction(
      review_request, snapshot, policy);
  ExpectAcceptedEvidence(review,
                         "STORAGE_HEALTH_MANAGER.QUARANTINE_OPERATOR_REVIEW",
                         "operator review quarantine");
  Require(review.evidence.route_target == "operator_review",
          "operator review route target mismatch");

  snapshot.checksum_failure_count = 0;
  snapshot.unknown_page_count = 0;
  snapshot.device_error_count = 0;
  ExpectRefused(agents::EvaluateStorageHealthManagerAction(
                    Request(ids,
                            agents::StorageHealthManagerActionKind::request_filespace_quarantine,
                            agents::StorageHealthEvidenceKind::device_error,
                            52),
                    snapshot,
                    policy),
                "STORAGE_HEALTH_MANAGER.EVIDENCE_REQUIRED",
                "quarantine missing checksum or unknown-page evidence");
}

void TestOptimizerCostAndSummaryEvidence() {
  const auto ids = MakeIds(6);
  const auto policy = Policy(ids);
  const auto snapshot = Snapshot(ids);

  const auto cost = agents::EvaluateStorageHealthManagerAction(
      Request(ids,
              agents::StorageHealthManagerActionKind::update_storage_cost,
              agents::StorageHealthEvidenceKind::latency_histogram,
              60),
      snapshot,
      policy);
  ExpectAcceptedEvidence(cost,
                         "STORAGE_HEALTH_MANAGER.COST_RECOMMENDATION",
                         "fresh latency cost recommendation");
  Require(cost.evidence.reason.find("optimizer storage cost update request") !=
              std::string::npos,
          "cost evidence wording mismatch");

  const auto summary = agents::EvaluateStorageHealthManagerAction(
      Request(ids,
              agents::StorageHealthManagerActionKind::emit_storage_health_summary,
              agents::StorageHealthEvidenceKind::health_summary,
              61),
      snapshot,
      policy);
  ExpectAcceptedEvidence(summary,
                         "STORAGE_HEALTH_MANAGER.HEALTH_SUMMARY_EMITTED",
                         "health summary");
  Require(summary.evidence.reason.find("storage health summary emitted") !=
              std::string::npos,
          "summary evidence wording mismatch");
}

void TestForbiddenLifecyclePageIndexAndPolicyActions() {
  const auto ids = MakeIds(7);
  const auto policy = Policy(ids);
  const auto snapshot = Snapshot(ids);
  const std::vector<agents::StorageHealthManagerActionKind> forbidden = {
      agents::StorageHealthManagerActionKind::forbidden_request_filespace_expand,
      agents::StorageHealthManagerActionKind::forbidden_request_filespace_move,
      agents::StorageHealthManagerActionKind::forbidden_request_filespace_shrink,
      agents::StorageHealthManagerActionKind::forbidden_request_filespace_truncate,
      agents::StorageHealthManagerActionKind::forbidden_request_filespace_detach,
      agents::StorageHealthManagerActionKind::forbidden_request_filespace_delete,
      agents::StorageHealthManagerActionKind::forbidden_promote_filespace,
      agents::StorageHealthManagerActionKind::forbidden_demote_filespace,
      agents::StorageHealthManagerActionKind::forbidden_allocate_pages,
      agents::StorageHealthManagerActionKind::forbidden_relocate_pages,
      agents::StorageHealthManagerActionKind::forbidden_rebuild_indexes,
      agents::StorageHealthManagerActionKind::forbidden_override_filespace_policy,
      agents::StorageHealthManagerActionKind::forbidden_override_page_policy};

  platform::u64 seed = 70;
  for (const auto action : forbidden) {
    ExpectRefused(agents::EvaluateStorageHealthManagerAction(
                      Request(ids,
                              action,
                              agents::StorageHealthEvidenceKind::health_summary,
                              seed++),
                      snapshot,
                      policy),
                  "STORAGE_HEALTH_MANAGER.FORBIDDEN_ACTION",
                  agents::StorageHealthManagerActionKindName(action));
  }
}

}  // namespace

int main() {
  TestAllowedActionSurface();
  TestDefaultRecommendOnlyBehavior();
  TestPolicyScopeAndEvidenceFailClosed();
  TestMetricGatesFailClosed();
  TestQuarantineRoutingModesAndEvidenceKinds();
  TestOptimizerCostAndSummaryEvidence();
  TestForbiddenLifecyclePageIndexAndPolicyActions();
  return EXIT_SUCCESS;
}
