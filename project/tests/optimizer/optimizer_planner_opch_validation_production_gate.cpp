// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_safety_gates.hpp"
#include "physical_plan.hpp"
#include "runtime_consumption_evidence.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "OPCH validation/production gate failure: " << message << '\n';
    return false;
  }
  return true;
}

bool Has(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

opt::PhysicalPlanNode Scan(std::string id) {
  opt::PhysicalPlanNode node;
  node.node_id = std::move(id);
  node.access_kind = plan::PhysicalAccessKind::kTableScan;
  node.executor_capability_id = opt::RequiredExecutorCapabilityForAccessKind(node.access_kind);
  node.descriptor_digest = "desc:scan";
  node.storage_backed = true;
  node.preserves_visibility = true;
  return node;
}

opt::PhysicalPlanNode Join() {
  opt::PhysicalPlanNode node;
  node.node_id = "join:opch092";
  node.access_kind = plan::PhysicalAccessKind::kJoinHash;
  node.executor_capability_id = opt::RequiredExecutorCapabilityForAccessKind(node.access_kind);
  node.descriptor_digest = "desc:join";
  node.storage_backed = false;
  node.preserves_visibility = true;
  node.materializes = true;
  node.memory_evidence_required = true;
  node.memory_evidence_present = true;
  node.memory_evidence_trusted = true;
  node.agent_evidence_required = true;
  node.agent_evidence_present = true;
  node.agent_evidence_trusted = true;
  node.children = {Scan("scan:left"), Scan("scan:right")};
  return node;
}

bool PhysicalPlanValidationCoversMemoryAndAgentEvidence() {
  // SEARCH_KEY: OPCH_PHYSICAL_PLAN_VALIDATION_EXPANSION
  const auto ok = opt::ValidatePhysicalPlanNode(Join());
  auto missing_memory = Join();
  missing_memory.memory_evidence_present = false;
  const auto memory_bad = opt::ValidatePhysicalPlanNode(missing_memory);
  auto missing_agent = Join();
  missing_agent.agent_evidence_trusted = false;
  const auto agent_bad = opt::ValidatePhysicalPlanNode(missing_agent);
  auto parser_authority = Join();
  parser_authority.parser_or_reference_evidence_authority = true;
  const auto parser_bad = opt::ValidatePhysicalPlanNode(parser_authority);

  return Require(ok.ok, "valid physical plan rejected") &&
         Require(Has(memory_bad.diagnostics,
                     "SB_OPT_PHYSICAL_PLAN_MEMORY_EVIDENCE_REQUIRED"),
                 "missing memory evidence accepted") &&
         Require(Has(agent_bad.diagnostics,
                     "SB_OPT_PHYSICAL_PLAN_AGENT_EVIDENCE_REQUIRED"),
                 "missing agent evidence accepted") &&
         Require(Has(parser_bad.diagnostics,
                     "SB_OPT_PHYSICAL_PLAN_PARSER_REFERENCE_AUTHORITY_FORBIDDEN"),
                 "parser/reference authority accepted by physical plan");
}

bool ProductionBuildGateBlocksTestOnlyOptimizerInputs() {
  // SEARCH_KEY: OPCH_OPTIMIZER_PRODUCTION_TEST_SEPARATION
  opt::OptimizerProductionBuildGateInput safe;
  const auto safe_result = opt::EvaluateOptimizerProductionBuildGate(safe);
  opt::OptimizerProductionBuildGateInput unsafe;
  unsafe.fixture_statistics_enabled = true;
  unsafe.legacy_only_defaults_enabled = true;
  unsafe.local_default_statistics_enabled = true;
  unsafe.policy_default_statistics_enabled = true;
  unsafe.reference_produced_evidence_enabled = true;
  unsafe.relaxed_benchmark_clean_paths_enabled = true;
  unsafe.relaxed_metrics_enabled = true;
  unsafe.placeholder_runtime_evidence_enabled = true;
  unsafe.synthetic_agent_recommendations_enabled = true;
  unsafe.synthetic_feedback_enabled = true;
  unsafe.parser_execution_shortcuts_enabled = true;
  unsafe.forced_collision_hooks_enabled = true;
  unsafe.cluster_stub_live_claims_enabled = true;
  unsafe.debug_only_paths_enabled = true;
  const auto unsafe_result = opt::EvaluateOptimizerProductionBuildGate(unsafe);

  return Require(safe_result.ok, "safe production build gate rejected") &&
         Require(!unsafe_result.ok, "unsafe production build flags accepted") &&
         Require(Has(unsafe_result.diagnostics,
                     "SB_OPT_PRODUCTION_GATE_FIXTURE_STATS_FORBIDDEN"),
                 "fixture stats production diagnostic missing") &&
         Require(Has(unsafe_result.diagnostics,
                     "SB_OPT_PRODUCTION_GATE_REFERENCE_EVIDENCE_FORBIDDEN"),
                 "reference evidence production diagnostic missing") &&
         Require(Has(unsafe_result.diagnostics,
                     "SB_OPT_PRODUCTION_GATE_PARSER_EXECUTION_FORBIDDEN"),
                 "parser shortcut production diagnostic missing") &&
         Require(Has(unsafe_result.diagnostics,
                     "SB_OPT_PRODUCTION_GATE_LOCAL_DEFAULT_STATS_FORBIDDEN"),
                 "local default production diagnostic missing") &&
         Require(Has(unsafe_result.diagnostics,
                     "SB_OPT_PRODUCTION_GATE_POLICY_DEFAULT_STATS_FORBIDDEN"),
                 "policy default production diagnostic missing") &&
         Require(Has(unsafe_result.diagnostics,
                     "SB_OPT_PRODUCTION_GATE_RELAXED_METRICS_FORBIDDEN"),
                 "relaxed metric production diagnostic missing") &&
         Require(Has(unsafe_result.diagnostics,
                     "SB_OPT_PRODUCTION_GATE_PLACEHOLDER_RUNTIME_EVIDENCE_FORBIDDEN"),
                 "placeholder runtime evidence diagnostic missing") &&
         Require(Has(unsafe_result.diagnostics,
                     "SB_OPT_PRODUCTION_GATE_CLUSTER_STUB_LIVE_CLAIMS_FORBIDDEN"),
                 "cluster stub production diagnostic missing") &&
         Require(Has(unsafe_result.diagnostics,
                     "SB_OPT_PRODUCTION_GATE_DEBUG_ONLY_PATHS_FORBIDDEN"),
                 "debug-only optimizer diagnostic missing");
}

opt::RuntimeOptimizedPathEvidence RuntimeEvidenceFromPlaceholderHelper() {
  return opt::MarkRuntimeEvidenceConsumed(
      opt::MakeSelectionOnlyRuntimeEvidence(
          "optimizer.selected.path",
          "embedded",
          "SB_OPCH_TEST.RUNTIME_CONSUMED",
          "selection helper placeholder awaiting executor consumption"),
      "engine.executor.embedded.optimized_path");
}

opt::RuntimeOptimizedPathEvidence RuntimeEvidenceWithRealRouteProof() {
  auto evidence = RuntimeEvidenceFromPlaceholderHelper();
  evidence.transaction_snapshot_class = "engine.mga.snapshot";
  evidence.catalog_epoch = 42;
  evidence.security_epoch = 43;
  evidence.redaction_epoch = 44;
  evidence.provider_generation = 45;
  evidence.result_contract_hash = "hash:opch-real-route-result-contract";
  return evidence;
}

bool ProductionRouteClaimsRejectPlaceholderRuntimeEvidence() {
  // SEARCH_KEY: OPCH_PLACEHOLDER_RUNTIME_EVIDENCE_PRODUCTION_GUARD
  const opt::RouteCompletionClaim claim{
      .route_kind = "embedded",
      .benchmark_clean = true,
      .live_route = true,
      .mark_complete = true,
  };
  const auto placeholder =
      opt::EvaluateRouteCompletionClaim(claim,
                                        {RuntimeEvidenceFromPlaceholderHelper()});
  const auto real =
      opt::EvaluateRouteCompletionClaim(claim,
                                        {RuntimeEvidenceWithRealRouteProof()});

  return Require(!placeholder.can_mark_complete,
                 "placeholder helper evidence closed benchmark-clean route") &&
         Require(placeholder.diagnostic_code ==
                     "SB_ORH_ROUTE_PLACEHOLDER_RUNTIME_EVIDENCE",
                 "placeholder runtime diagnostic missing") &&
         Require(real.can_mark_complete,
                 "real route proof was blocked from benchmark-clean closure") &&
         Require(real.diagnostic_code ==
                     "SB_ORH_ROUTE_CLAIM.LIVE_CONSUMPTION_OK",
                 "real route proof success diagnostic changed");
}

}  // namespace

int main() {
  if (!PhysicalPlanValidationCoversMemoryAndAgentEvidence()) return EXIT_FAILURE;
  if (!ProductionBuildGateBlocksTestOnlyOptimizerInputs()) return EXIT_FAILURE;
  if (!ProductionRouteClaimsRejectPlaceholderRuntimeEvidence()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
