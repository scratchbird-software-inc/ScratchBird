// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_join_controls_enterprise.hpp"

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
    std::cerr << "OEIC join controls gate failure: " << message << '\n';
    return false;
  }
  return true;
}

bool Contains(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool EvidenceContains(const std::vector<std::string>& values, const std::string& prefix) {
  return std::any_of(values.begin(), values.end(), [&](const std::string& value) {
    return value.find(prefix) != std::string::npos;
  });
}

plan::OptimizerPolicyMetadata Policy(std::string join_policy) {
  plan::OptimizerPolicyMetadata policy;
  policy.optimizer_policy_metadata_present = true;
  policy.policy_source_kind = "sblr_api";
  policy.policy_epoch = 9041;
  policy.normalized_controls.plan_profile_id = "plan_profile:enterprise";
  policy.normalized_controls.join_search_policy_id = std::move(join_policy);
  policy.normalized_controls.memory_policy_id = "memory_policy:governed";
  policy.normalized_controls.spill_policy_id = "spill_policy:governed";
  policy.normalized_controls.parallelism_policy_id = "parallelism:local";
  policy.normalized_controls.safe_control_ids = {
      "join.frontier_width.16",
      "join.transition_budget.2048",
      "join.preserve_property_frontier"};
  policy.safe_control_ids = {"join.exhaustive_limit.8", "join.bounded_limit.16"};
  return policy;
}

opt::EnterpriseJoinControlRequest Request(std::string join_policy) {
  opt::EnterpriseJoinControlRequest request;
  request.policy_metadata = Policy(std::move(join_policy));
  request.relation_count = 3;
  request.runtime_memory_budget_bytes = 32 * 1024 * 1024;
  request.max_transition_budget = 4096;
  return request;
}

opt::JoinRelationNode Relation(std::string uuid, std::uint64_t rows) {
  opt::JoinRelationNode relation;
  relation.relation_uuid = std::move(uuid);
  relation.estimated_rows = rows;
  relation.covering_path_available = true;
  relation.native_visibility_preserved = true;
  relation.exact_output_preserved = true;
  relation.parallel_eligible = true;
  return relation;
}

opt::JoinPredicateEdge Edge(std::string left, std::string right) {
  opt::JoinPredicateEdge edge;
  edge.left_relation_uuid = std::move(left);
  edge.right_relation_uuid = std::move(right);
  edge.predicate_kind = "eq";
  edge.semantic_kind = opt::JoinSemanticKind::kInner;
  edge.equality = true;
  edge.selectivity = 0.01;
  return edge;
}

opt::JoinGraph Graph() {
  return opt::BuildJoinGraph(
      {Relation("rel.a", 1000), Relation("rel.b", 100), Relation("rel.c", 10)},
      {Edge("rel.a", "rel.b"), Edge("rel.b", "rel.c")},
      false,
      false);
}

bool StrategyControlsAndTelemetryPass() {
  // SEARCH_KEY: OEIC_JOIN_STRATEGY_CONTROL_ENTERPRISE
  const std::vector<std::pair<std::string, opt::JoinSearchStrategy>> strategies = {
      {"join_search:auto", opt::JoinSearchStrategy::kAuto},
      {"join_search:exhaustive_dp", opt::JoinSearchStrategy::kExhaustiveDp},
      {"join_search:bounded_dp", opt::JoinSearchStrategy::kBoundedDp},
      {"join_search:hypergraph_greedy", opt::JoinSearchStrategy::kHypergraphGreedy},
      {"join_search:heuristic_greedy", opt::JoinSearchStrategy::kHeuristicGreedy},
      {"join_search:input_order", opt::JoinSearchStrategy::kInputOrder},
  };

  for (const auto& [policy_id, expected] : strategies) {
    const auto controls = opt::BuildEnterpriseJoinSearchPolicy(Request(policy_id));
    if (!Require(controls.ok, "safe join controls were refused for " + policy_id)) return false;
    if (!Require(controls.join_policy.strategy == expected,
                 "join strategy did not map from policy " + policy_id)) return false;
    if (!Require(controls.join_policy.frontier_width == 16,
                 "frontier width control was not applied")) return false;
    if (!Require(controls.join_policy.transition_budget == 2048,
                 "transition budget control was not applied")) return false;
    if (!Require(EvidenceContains(controls.evidence, "join_control.parser_authority=false"),
                 "parser non-authority evidence missing")) return false;
    if (!Require(EvidenceContains(controls.evidence, "join_control.cluster_authority=false"),
                 "cluster non-authority evidence missing")) return false;

    const auto plan_result = opt::EnumerateJoinOrderWithPolicy(Graph(), controls.join_policy);
    std::vector<std::string> diagnostics;
    if (!Require(opt::ValidateEnterpriseJoinStrategyTelemetry(controls, plan_result, &diagnostics),
                 "join telemetry validation failed for " + policy_id)) return false;
    if (!Require(Contains(diagnostics, "SB_OPT_JOIN_TELEMETRY_VALIDATED"),
                 "telemetry validation diagnostic missing")) return false;
  }
  return true;
}

bool UnsafeControlSurfacesFailClosed() {
  auto raw_sql = Request("join_search:bounded_dp");
  raw_sql.policy_metadata.raw_sql_text_present = true;
  const auto raw_sql_result = opt::BuildEnterpriseJoinSearchPolicy(raw_sql);

  auto unsafe_token = Request("join_search:bounded_dp");
  unsafe_token.policy_metadata.normalized_controls.safe_control_ids.push_back("SELECT.*.FROM.customer");
  const auto unsafe_token_result = opt::BuildEnterpriseJoinSearchPolicy(unsafe_token);

  auto cluster = Request("join_search:bounded_dp");
  cluster.authority.cluster_authority = true;
  const auto cluster_result = opt::BuildEnterpriseJoinSearchPolicy(cluster);

  auto reference = Request("join_search:bounded_dp");
  reference.policy_metadata.reference_or_legacy_policy_authority_claimed = true;
  const auto reference_result = opt::BuildEnterpriseJoinSearchPolicy(reference);

  auto unknown = Request("join_search:random_walk");
  const auto unknown_result = opt::BuildEnterpriseJoinSearchPolicy(unknown);

  return Require(!raw_sql_result.ok &&
                     raw_sql_result.refusal_diagnostic == "SB_OPT_JOIN_CONTROL_POLICY_AUTHORITY_INVALID",
                 "raw SQL policy authority was not refused") &&
         Require(!unsafe_token_result.ok &&
                     unsafe_token_result.refusal_diagnostic == "SB_OPT_JOIN_CONTROL_UNSAFE_POLICY_TOKEN",
                 "unsafe control token was not refused") &&
         Require(!cluster_result.ok &&
                     cluster_result.refusal_diagnostic == "SB_OPT_JOIN_CONTROL_UNSAFE_AUTHORITY",
                 "cluster authority was not refused") &&
         Require(!reference_result.ok &&
                     reference_result.refusal_diagnostic == "SB_OPT_JOIN_CONTROL_POLICY_AUTHORITY_INVALID",
                 "reference/legacy policy authority was not refused") &&
         Require(!unknown_result.ok &&
                     unknown_result.refusal_diagnostic == "SB_OPT_JOIN_CONTROL_UNKNOWN_STRATEGY",
                 "unknown join strategy was not refused");
}

bool ProductionScopeRequiresRealBudget() {
  auto missing_budget = Request("join_search:bounded_dp");
  missing_budget.runtime_memory_budget_bytes = 0;
  const auto result = opt::BuildEnterpriseJoinSearchPolicy(missing_budget);
  return Require(!result.ok &&
                     result.refusal_diagnostic == "SB_OPT_JOIN_CONTROL_MEMORY_BUDGET_REQUIRED",
                 "production join controls accepted missing memory budget");
}

}  // namespace

int main() {
  if (!StrategyControlsAndTelemetryPass()) return EXIT_FAILURE;
  if (!UnsafeControlSurfacesFailClosed()) return EXIT_FAILURE;
  if (!ProductionScopeRequiresRealBudget()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
