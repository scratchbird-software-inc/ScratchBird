// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_memory_coupling.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

namespace agents = scratchbird::core::agents;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    Fail(message);
  }
}

void RegisterMemoryScopes(agents::HierarchicalMemoryBudgetLedger* ledger) {
  agents::HierarchicalMemoryBudgetScope database;
  database.scope_id = "database:019f0084";
  database.kind = agents::HierarchicalMemoryBudgetScopeKind::kDatabase;
  database.limit_bytes = 4096;
  Require(ledger->RegisterScope(database).ok, "database scope registration failed");

  agents::HierarchicalMemoryBudgetScope background;
  background.scope_id = "background:memory-governor";
  background.parent_scope_id = database.scope_id;
  background.kind = agents::HierarchicalMemoryBudgetScopeKind::kBackground;
  background.limit_bytes = 2048;
  Require(ledger->RegisterScope(background).ok,
          "background scope registration failed");
}

agents::AgentMemoryReservationRequest ReservationRequest() {
  agents::AgentMemoryReservationRequest request;
  request.agent_type_id = "memory_governor";
  request.action_id = "shrink_cache";
  request.operation_id = "mmch084-shrink-cache";
  request.owner_scope = "agent:memory_governor";
  request.leaf_memory_scope_id = "background:memory-governor";
  request.memory_bytes = 1024;
  request.live_action = true;
  request.allocation_or_memory_action = true;
  return request;
}

void TestMmch084BudgetIntegration() {
  agents::HierarchicalMemoryBudgetLedger memory_ledger("mmch084-memory");
  RegisterMemoryScopes(&memory_ledger);
  agents::ResourceGovernanceReservationLedger resource_ledger("mmch084-resource");

  const auto admitted = agents::AcquireAgentMemoryReservations(
      ReservationRequest(), &memory_ledger, &resource_ledger);
  Require(admitted.ok, "MMCH-084 bounded reservation was refused");
  Require(admitted.hierarchical_reservation_created &&
              admitted.resource_reservation_created,
          "MMCH-084 did not create both reservation tokens");
  Require(admitted.status.diagnostic_code ==
              "SB_AGENT_MEMORY_BUDGET.RESERVATION_CREATED",
          "MMCH-084 admitted diagnostic drifted");

  auto unbounded = ReservationRequest();
  unbounded.memory_bytes = 0;
  const auto refused = agents::AcquireAgentMemoryReservations(
      unbounded, &memory_ledger, &resource_ledger);
  Require(!refused.ok && refused.fail_closed,
          "MMCH-084 unbounded path did not fail closed");
  Require(refused.status.diagnostic_code ==
              "SB_AGENT_MEMORY_BUDGET.UNBOUNDED_MEMORY_REFUSED",
          "MMCH-084 unbounded diagnostic mismatch");
}

agents::AgentMemoryMetricSnapshot Snapshot() {
  agents::AgentMemoryMetricSnapshot snapshot;
  snapshot.metric_family = "sb_memory_allocated_bytes";
  snapshot.namespace_path = "sys.metrics.memory";
  snapshot.generation = 85;
  snapshot.sampled_at_microseconds = 1000;
  snapshot.scope_uuid = "019f0085-0000-7000-8000-000000000001";
  snapshot.digest = "sha256:trusted-mmch085";
  snapshot.max_freshness_microseconds = 1000;
  snapshot.source_quality = agents::AgentMetricSourceQuality::trusted;
  snapshot.trusted = true;
  return snapshot;
}

void TestMmch085MetricSnapshotAuthority() {
  const auto accepted =
      agents::ValidateAgentMemoryMetricSnapshot(Snapshot(), 1500, false);
  Require(accepted.ok, "MMCH-085 trusted fresh snapshot refused");

  auto stale = Snapshot();
  const auto stale_decision =
      agents::ValidateAgentMemoryMetricSnapshot(stale, 3001, false);
  Require(!stale_decision.ok && stale_decision.fail_closed,
          "MMCH-085 stale snapshot did not fail closed");
  Require(stale_decision.status.diagnostic_code ==
              "SB_AGENT_MEMORY_METRIC.STALE_REFUSED",
          "MMCH-085 stale diagnostic mismatch");

  auto relaxed = Snapshot();
  relaxed.registry_only_relaxed_path = true;
  relaxed.test_only_relaxed_path = true;
  const auto prod_relaxed =
      agents::ValidateAgentMemoryMetricSnapshot(relaxed, 1500, false);
  Require(!prod_relaxed.ok && prod_relaxed.fail_closed,
          "MMCH-085 production relaxed metric path was accepted");
  const auto test_relaxed =
      agents::ValidateAgentMemoryMetricSnapshot(relaxed, 1500, true);
  Require(test_relaxed.ok && test_relaxed.accepted_registry_relaxed_test_path,
          "MMCH-085 test-only relaxed metric path was not bounded to tests");
}

agents::AgentMemoryPressureActionBoundaryRequest BoundaryRequest() {
  agents::AgentMemoryPressureActionBoundaryRequest request;
  request.agent_type_id = "memory_governor";
  request.action_id = "shrink_cache";
  request.action_kind =
      agents::AgentMemoryPressureActionKind::page_cache_shrink;
  request.action_boundary_approved = true;
  request.durable_evidence_uuid = "019f0086-0000-7000-8000-000000000001";
  request.outcome_verification_uuid = "019f0086-0000-7000-8000-000000000002";
  request.outcome_verified = true;
  return request;
}

void TestMmch086ActionBoundary() {
  const auto accepted =
      agents::EvaluateAgentMemoryPressureActionBoundary(BoundaryRequest());
  Require(accepted.ok && accepted.action_allowed,
          "MMCH-086 approved action boundary refused");

  auto missing_evidence = BoundaryRequest();
  missing_evidence.durable_evidence_uuid.clear();
  const auto refused =
      agents::EvaluateAgentMemoryPressureActionBoundary(missing_evidence);
  Require(!refused.ok && refused.fail_closed,
          "MMCH-086 missing durable evidence did not fail closed");
  Require(refused.status.diagnostic_code ==
              "SB_AGENT_MEMORY_ACTION.DURABLE_EVIDENCE_REQUIRED",
          "MMCH-086 durable evidence diagnostic mismatch");

  auto missing_outcome = BoundaryRequest();
  missing_outcome.outcome_verified = false;
  const auto outcome =
      agents::EvaluateAgentMemoryPressureActionBoundary(missing_outcome);
  Require(!outcome.ok && outcome.status.diagnostic_code ==
                             "SB_AGENT_MEMORY_ACTION.OUTCOME_VERIFICATION_REQUIRED",
          "MMCH-086 outcome verification diagnostic mismatch");
}

void TestMmch087EvidenceRedactionOverhead() {
  agents::AgentMemoryEvidenceBundleRequest request;
  request.agent_type_id = "support_bundle_triage_agent";
  request.support_bundle_uuid = "019f0087-0000-7000-8000-000000000001";
  request.overhead_budget_bytes = 1024;
  request.fields.push_back({"safe_metric", "resident_bytes=1024", false});
  request.fields.push_back({"protected_buffer", "secret-page-material", true});

  const auto bundle = agents::BuildAgentMemoryEvidenceBundle(request);
  Require(bundle.ok, "MMCH-087 evidence bundle refused");
  Require(bundle.overhead_budget_evidence_present,
          "MMCH-087 overhead budget evidence missing");
  bool redacted = false;
  bool tamper = false;
  for (const auto& row : bundle.rows) {
    if (row.key == "protected_buffer") {
      redacted = row.redacted && row.value == "redacted";
    }
    tamper = tamper || !row.tamper_evidence_digest.empty();
  }
  Require(redacted, "MMCH-087 protected material was not redacted");
  Require(tamper, "MMCH-087 tamper evidence digest missing");

  request.overhead_budget_bytes = 1;
  const auto over_budget = agents::BuildAgentMemoryEvidenceBundle(request);
  Require(!over_budget.ok && over_budget.fail_closed,
          "MMCH-087 over-budget evidence did not fail closed");
  Require(over_budget.status.diagnostic_code ==
              "SB_AGENT_MEMORY_EVIDENCE.OVERHEAD_BUDGET_EXCEEDED",
          "MMCH-087 overhead diagnostic mismatch");
}

void TestMmch088CrashRestartRoute() {
  agents::AgentMemoryCrashRestartRouteRequest request;
  request.agent_type_id = "memory_governor";
  request.route_id = "memory-pressure-cleanup";
  request.persisted_route_generation = 88;
  request.recovered_route_generation = 88;
  request.crash_recovery_mode = true;
  request.durable_evidence_available = true;
  request.outcome_verification_available = true;

  const auto recovered = agents::EvaluateAgentMemoryCrashRestartRoute(request);
  Require(recovered.ok && recovered.recovered,
          "MMCH-088 deterministic recovery route refused");

  request.memory_claims_recovery_authority = true;
  const auto refused = agents::EvaluateAgentMemoryCrashRestartRoute(request);
  Require(!refused.ok && refused.refused_deterministically,
          "MMCH-088 memory recovery-authority claim did not refuse");
  Require(refused.status.diagnostic_code ==
              "SB_AGENT_MEMORY_CRASH.RECOVERY_AUTHORITY_REFUSED",
          "MMCH-088 recovery authority diagnostic mismatch");
}

void TestMmch089ProductionSeparation() {
  agents::AgentMemoryProductionSeparationInput production;
  production.production_build = true;
  production.fixture_agent_state = true;
  const auto refused =
      agents::ValidateAgentMemoryProductionSeparation(production);
  Require(!refused.ok && refused.fail_closed,
          "MMCH-089 fixture agent state accepted in production");
  Require(refused.status.diagnostic_code ==
              "SB_AGENT_MEMORY_PRODUCTION.TEST_PATH_REFUSED",
          "MMCH-089 production separation diagnostic mismatch");

  agents::AgentMemoryProductionSeparationInput test;
  test.production_build = false;
  test.fixture_agent_state = true;
  test.relaxed_metric_path = true;
  test.probe_only_memory_action = true;
  const auto accepted =
      agents::ValidateAgentMemoryProductionSeparation(test);
  Require(accepted.ok, "MMCH-089 non-production fixture path refused");
}

}  // namespace

int main() {
  TestMmch084BudgetIntegration();
  TestMmch085MetricSnapshotAuthority();
  TestMmch086ActionBoundary();
  TestMmch087EvidenceRedactionOverhead();
  TestMmch088CrashRestartRoute();
  TestMmch089ProductionSeparation();
  std::cout
      << "MMCH_AGENT_MEMORY_BUDGET_INTEGRATION "
      << "MMCH_AGENT_MEMORY_METRIC_SNAPSHOT_AUTHORITY "
      << "MMCH_AGENT_MEMORY_PRESSURE_ACTION_BOUNDARY "
      << "MMCH_AGENT_MEMORY_EVIDENCE_REDACTION_OVERHEAD "
      << "MMCH_AGENT_MEMORY_CRASH_RESTART_ROUTE "
      << "MMCH_AGENT_MEMORY_PRODUCTION_TEST_SEPARATION ok\n";
  return EXIT_SUCCESS;
}
