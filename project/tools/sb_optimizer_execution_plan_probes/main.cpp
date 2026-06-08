// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "access_path.hpp"
#include "cluster_candidate.hpp"
#include "optimizer_contract.hpp"
#include "optimizer_feedback.hpp"
#include "observability/explain_api.hpp"
#include "rule_planner.hpp"
#include "selectivity_model.hpp"
#include "statistics_catalog.hpp"

// SEARCH_KEY: SB_OPTIMIZER_PROBES

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace planner = scratchbird::engine::planner;
namespace sblr = scratchbird::engine::sblr;
namespace api = scratchbird::engine::internal_api;

namespace {

struct CheckState {
  bool ok = true;
  std::vector<std::string> failures;
};

void Require(CheckState* state, bool condition, std::string message) {
  if (!condition) {
    state->ok = false;
    state->failures.push_back(std::move(message));
  }
}

std::string JsonEscape(std::string_view input) {
  std::string out;
  for (char ch : input) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      default: out += ch; break;
    }
  }
  return out;
}

bool HasSelectedAccess(const opt::OptimizedPlan& plan, planner::PhysicalAccessKind access_kind) {
  for (const auto& candidate : plan.candidates) {
    if (candidate.selected && candidate.plan_candidate.access_kind == access_kind) return true;
  }
  return false;
}

bool HasRejectedReason(const std::vector<opt::PlanCandidate>& candidates, const std::string& reason) {
  for (const auto& candidate : candidates) {
    if (!candidate.cost.selectable && candidate.cost.rejection_reason == reason) return true;
  }
  return false;
}

planner::LogicalPlan BuildSelectPlan(std::string predicate_kind) {
  planner::RulePlannerInput input;
  input.envelope = sblr::MakeSblrEnvelope("dml.select_rows", "dml.select_rows", "TRACE-OPT-EXECUTION_PLAN");
  input.api_request.predicate.predicate_kind = std::move(predicate_kind);
  return planner::BuildDeterministicLogicalPlan(input);
}

void RunAllChecks(CheckState* state) {
  opt::OptimizerStatisticsCatalog statistics = opt::DefaultLocalStatisticsCatalog();
  const auto stats_statuses = statistics.ValidateAll(1000);
  for (const auto& status : stats_statuses) {
    Require(state, status.ok, "default statistics contract validates: " + status.detail);
  }
  auto stale = opt::MakeStatistic("row_count", "relation", "stale", 10.0, opt::StatisticSource::kCatalogSample, 1, 10000000, opt::CostConfidence::kLow);
  Require(state, !opt::ValidateStatistic(stale, 1000).ok, "stale statistic rejected");

  opt::PredicateSelectivityInput unknown_selectivity;
  unknown_selectivity.predicate_kind = "opaque_predicate";
  const auto unknown_estimate = opt::EstimatePredicateSelectivity(unknown_selectivity);
  Require(state, unknown_estimate.conservative && unknown_estimate.diagnostic_code == "SB_OPTIMIZER_SELECTIVITY.UNKNOWN_CONSERVATIVE", "unknown selectivity conservative");

  opt::PredicateSelectivityInput point_selectivity;
  point_selectivity.predicate_kind = "unique_eq";
  const auto point_estimate = opt::EstimatePredicateSelectivity(point_selectivity);
  Require(state, !point_estimate.conservative && point_estimate.selectivity < 0.01, "unique selectivity low and non-conservative");

  const auto logical = BuildSelectPlan("scalar_eq");
  Require(state, logical.ok && !logical.nodes.empty(), "rule planner builds scalar select plan");
  const auto optimized = opt::OptimizeLogicalPlan(logical);
  Require(state, optimized.ok, "optimizer returns ok for local select");
  Require(state, HasSelectedAccess(optimized, planner::PhysicalAccessKind::kScalarBtreeLookup), "optimizer selects scalar btree lookup");
  Require(state, opt::SerializeOptimizedPlanToJson(optimized).find("statistics_version") != std::string::npos, "optimized plan serializes statistics evidence");

  const auto access_candidates = opt::GenerateLocalAccessPathCandidates(logical.nodes.front(), statistics);
  Require(state, !access_candidates.empty(), "access path candidates generated");
  Require(state, HasRejectedReason(access_candidates, "index_family_not_supported_placeholder"), "unsupported bitmap summary rejected explicitly");
  const auto best = opt::ChooseBestSelectableCandidate(access_candidates);
  Require(state, best.has_value() && best->cost.selectable, "best selectable access candidate exists");

  auto lookup_cost = opt::EstimateNodeCost(planner::MakeLogicalPlanNode(planner::LogicalPlanNodeKind::kDmlRead, planner::PhysicalAccessKind::kRowUuidLookup, "dml.select_rows", "row_lookup"));
  auto scan_cost = opt::EstimateNodeCost(planner::MakeLogicalPlanNode(planner::LogicalPlanNodeKind::kDmlRead, planner::PhysicalAccessKind::kTableScan, "dml.select_rows", "table_scan"));
  Require(state, opt::IsBetterCost(lookup_cost, scan_cost), "lookup cost better than table scan");
  Require(state, opt::SerializeCostVectorToJson(scan_cost).find("confidence") != std::string::npos, "cost vector serializes confidence");

  opt::JoinPlanningInput join;
  join.left_cardinality = 100;
  join.right_cardinality = 10;
  join.reorder_safe = true;
  join.equi_join = true;
  join.memory_budget_bytes = 1048576;
  const auto join_decision = opt::PlanLocalJoin(join);
  Require(state, join_decision.ok, "join planner returns decision");
  Require(state, join_decision.selected_method == planner::PhysicalAccessKind::kJoinHash, "join planner can select hash join when facts allow");
  join.hash_join_executor_available = false;
  const auto join_rejected = opt::PlanLocalJoin(join);
  Require(state, HasRejectedReason(join_rejected.candidates, "executor_hash_join_unavailable"), "unsupported hash join rejected explicitly");

  const auto sort_cost = opt::EstimateNodeCost(planner::MakeLogicalPlanNode(planner::LogicalPlanNodeKind::kDmlRead, planner::PhysicalAccessKind::kSort, "query.sort", "sort"));
  const auto topn_cost = opt::EstimateNodeCost(planner::MakeLogicalPlanNode(planner::LogicalPlanNodeKind::kDmlRead, planner::PhysicalAccessKind::kTopN, "query.top_n", "top_n"));
  Require(state, opt::IsBetterCost(topn_cost, sort_cost), "top-N cost better than full sort");

  opt::ClusterCandidateFacts missing_cluster_facts;
  const auto cluster_candidate = opt::BuildClusterFragmentCandidate(missing_cluster_facts);
  Require(state, !opt::ClusterCandidateMayWin(cluster_candidate), "cluster candidate cannot win without authority");
  Require(state, cluster_candidate.cost.rejection_reason == "cluster_authority_unavailable", "cluster rejection reason explicit");
  opt::ClusterCandidateFacts remote_missing;
  remote_missing.cluster_authority_available = true;
  remote_missing.route_generation_available = true;
  const auto remote_candidate = opt::BuildRemoteNodePushdownCandidate(remote_missing);
  Require(state, !opt::ClusterCandidateMayWin(remote_candidate), "remote candidate cannot win without remote stats");
  Require(state, remote_candidate.cost.rejection_reason == "remote_stats_unavailable", "remote stats rejection explicit");

  opt::OptimizerRuntimeFeedback feedback;
  feedback.operator_family = "scan";
  feedback.plan_shape = "local_index_lookup";
  feedback.estimated_rows = 10;
  feedback.actual_rows = 100;
  const auto feedback_status = opt::EvaluateOptimizerRuntimeFeedback(feedback);
  Require(state, feedback_status.ok && feedback_status.estimate_error_ratio == 10.0, "runtime feedback computes estimate error as metric input");

  api::EngineExplainOptimizerEvidenceRequest explain_request;
  explain_request.operation_id = "observability.explain_optimizer_evidence";
  explain_request.context.security_context_present = true;
  explain_request.candidate_evidence_rows.push_back(opt::SerializePlanCandidateToJson(*best));
  const auto explain_result = api::EngineExplainOptimizerEvidence(explain_request);
  Require(state, explain_result.ok && !explain_result.result_shape.rows.empty(), "optimizer explain evidence API returns rows");

  const std::string serialized_plan = opt::SerializeOptimizedPlanToJson(optimized);
  Require(state, serialized_plan.find("wal") == std::string::npos && serialized_plan.find("WAL") == std::string::npos, "optimizer evidence has no WAL authority terms");
}

}  // namespace

int main(int argc, char** argv) {
  CheckState state;
  RunAllChecks(&state);
  const std::string probe_name = argc > 0 && argv[0] != nullptr ? std::filesystem::path(argv[0]).filename().string() : "sb_optimizer_execution_plan_probe";
  std::cout << "{\n  \"probe\": \"" << JsonEscape(probe_name) << "\",\n";
  std::cout << "  \"ok\": " << (state.ok ? "true" : "false") << ",\n";
  std::cout << "  \"failure_count\": " << state.failures.size() << ",\n";
  std::cout << "  \"failures\": [";
  for (std::size_t i = 0; i < state.failures.size(); ++i) {
    if (i != 0) std::cout << ", ";
    std::cout << "\"" << JsonEscape(state.failures[i]) << "\"";
  }
  std::cout << "]\n}\n";
  return state.ok ? 0 : 1;
}
