// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "access_path.hpp"
#include "logical_plan.hpp"
#include "optimizer_cost_full.hpp"
#include "selectivity_model.hpp"
#include "statistics_catalog.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool Near(double actual, double expected, double epsilon, const std::string& message) {
  return Require(std::fabs(actual - expected) <= epsilon, message);
}

opt::SelectivityEstimate Selectivity(const std::string& kind,
                                     std::uint64_t input_rows,
                                     std::uint64_t distinct_values = 0,
                                     opt::CostConfidence confidence = opt::CostConfidence::kHigh) {
  opt::PredicateSelectivityInput input;
  input.predicate_kind = kind;
  input.input_rows = input_rows;
  input.distinct_values = distinct_values;
  input.input_confidence = confidence;
  return opt::EstimatePredicateSelectivity(input);
}

bool RowEstimateSelectivityModelCoversRequiredPredicates() {
  const auto row_uuid = Selectivity("row_uuid_eq", 10000);
  const auto unique = Selectivity("unique_eq", 10000);
  if (!Require(row_uuid.exact_rows_known, "row UUID selectivity did not carry exact-row proof") ||
      !Require(opt::EstimateRowsAfterSelectivity(10000, row_uuid) == 1,
               "row UUID equality did not estimate exactly one row") ||
      !Require(opt::EstimateRowsAfterSelectivity(10000, unique) == 1,
               "unique equality did not estimate exactly one row") ||
      !Require(row_uuid.confidence == opt::CostConfidence::kExact,
               "row UUID equality confidence was not exact")) {
    return false;
  }

  const auto equality = Selectivity("scalar_eq", 10000, 2500);
  opt::PredicateSelectivityInput mcv_input;
  mcv_input.predicate_kind = "scalar_eq";
  mcv_input.input_rows = 10000;
  mcv_input.has_mcv_frequency = true;
  mcv_input.mcv_frequency = 0.07;
  mcv_input.input_confidence = opt::CostConfidence::kHigh;
  const auto mcv = opt::EstimatePredicateSelectivity(mcv_input);

  opt::PredicateSelectivityInput range_input;
  range_input.predicate_kind = "scalar_range";
  range_input.input_rows = 10000;
  range_input.has_histogram = true;
  range_input.range_fraction = 0.18;
  range_input.input_confidence = opt::CostConfidence::kHigh;
  const auto range = opt::EstimatePredicateSelectivity(range_input);

  opt::PredicateSelectivityInput in_input;
  in_input.predicate_kind = "in_list";
  in_input.input_rows = 10000;
  in_input.distinct_values = 100;
  in_input.in_list_count = 3;
  in_input.input_confidence = opt::CostConfidence::kHigh;
  const auto in_list = opt::EstimatePredicateSelectivity(in_input);

  opt::PredicateSelectivityInput like_input;
  like_input.predicate_kind = "like";
  like_input.input_rows = 10000;
  like_input.like_has_fixed_prefix = true;
  like_input.like_prefix_fraction = 0.02;
  like_input.input_confidence = opt::CostConfidence::kMedium;
  const auto like = opt::EstimatePredicateSelectivity(like_input);

  opt::PredicateSelectivityInput and_input;
  and_input.predicate_kind = "and";
  and_input.children = {Selectivity("scalar_eq", 10000, 10), range};
  const auto conjunction = opt::EstimatePredicateSelectivity(and_input);

  opt::PredicateSelectivityInput or_input;
  or_input.predicate_kind = "or";
  or_input.children = {Selectivity("scalar_eq", 10000, 10), range};
  const auto disjunction = opt::EstimatePredicateSelectivity(or_input);

  opt::PredicateSelectivityInput not_input;
  not_input.predicate_kind = "not";
  not_input.children = {Selectivity("scalar_eq", 10000, 10)};
  const auto negation = opt::EstimatePredicateSelectivity(not_input);

  opt::PredicateSelectivityInput join_input;
  join_input.predicate_kind = "join_eq";
  join_input.left_rows = 1000;
  join_input.right_rows = 2000;
  join_input.left_distinct_values = 1000;
  join_input.right_distinct_values = 1500;
  join_input.input_confidence = opt::CostConfidence::kHigh;
  const auto join = opt::EstimatePredicateSelectivity(join_input);

  return Near(equality.selectivity, 0.0004, 0.000001, "NDV equality selectivity mismatch") &&
         Require(opt::EstimateRowsAfterSelectivity(10000, equality) == 4,
                 "NDV equality row estimate mismatch") &&
         Require(opt::EstimateRowsAfterSelectivity(10000, mcv) == 700,
                 "MCV equality row estimate mismatch") &&
         Require(opt::EstimateRowsAfterSelectivity(10000, range) == 1800,
                 "histogram range row estimate mismatch") &&
         Require(opt::EstimateRowsAfterSelectivity(10000, in_list) == 300,
                 "IN-list row estimate mismatch") &&
         Require(opt::EstimateRowsAfterSelectivity(10000, like) == 200,
                 "LIKE-prefix row estimate mismatch") &&
         Near(conjunction.selectivity, 0.018, 0.000001, "AND selectivity mismatch") &&
         Near(disjunction.selectivity, 0.262, 0.000001, "OR selectivity mismatch") &&
         Near(negation.selectivity, 0.9, 0.000001, "NOT selectivity mismatch") &&
         Near(join.selectivity, 1.0 / 1500.0, 0.000001, "join NDV selectivity mismatch") &&
         Require(opt::EstimateJoinRowsAfterSelectivity(1000, 2000, join) == 1334,
                 "join row estimate mismatch") &&
         Require(join.confidence == opt::CostConfidence::kHigh,
                 "join selectivity confidence was not propagated");
}

opt::OptimizerCostEnvironment CostEnvironment() {
  opt::OptimizerCostEnvironment environment;
  environment.cost_profile_id = "batch_c_foundation";
  environment.cpu_tuple_cost = 0.01;
  environment.cpu_index_tuple_cost = 0.004;
  environment.cpu_operator_cost = 0.002;
  environment.visibility_tuple_cost = 0.003;
  environment.sequential_page_cost = 1.0;
  environment.random_page_cost = 4.0;
  environment.memory_kib_cost = 0.001;
  environment.spill_page_cost = 2.0;
  environment.memory_budget_bytes = 64 * 1024;
  return environment;
}

opt::CostFormulaInput BaseCostInput() {
  opt::CostFormulaInput input;
  input.access_kind = plan::PhysicalAccessKind::kScalarBtreeLookup;
  input.estimated_rows = 1000;
  input.row_width_bytes = 128;
  input.sequential_pages = 32;
  input.random_pages = 100;
  input.index_probe_count = 10;
  input.index_tuple_visits = 1000;
  input.visibility_recheck_rows = 1000;
  input.required_memory_bytes = 32 * 1024;
  input.cache_hit_ratio = 0.25;
  input.input_confidence = opt::CostConfidence::kHigh;
  return input;
}

bool LegacyGradeCostVectorModelsRequiredEffects() {
  const auto environment = CostEnvironment();
  auto base_input = BaseCostInput();
  const auto base = opt::EstimateCostVector(environment, base_input);

  auto cached_prefetch_input = base_input;
  cached_prefetch_input.cache_hit_ratio = 0.90;
  cached_prefetch_input.prefetch_pages = 32;
  const auto cached_prefetch = opt::EstimateCostVector(environment, cached_prefetch_input);

  auto correlated_input = base_input;
  correlated_input.heap_correlation = 0.95;
  const auto correlated = opt::EstimateCostVector(environment, correlated_input);

  auto noisy_input = base_input;
  noisy_input.false_positive_rows = 500;
  noisy_input.duplicate_rows = 250;
  const auto noisy = opt::EstimateCostVector(environment, noisy_input);

  auto spill_input = base_input;
  spill_input.required_memory_bytes = 256 * 1024;
  spill_input.spill_capable = true;
  const auto spill = opt::EstimateCostVector(environment, spill_input);

  auto rejected_input = spill_input;
  rejected_input.spill_capable = false;
  const auto rejected = opt::EstimateCostVector(environment, rejected_input);

  auto sort_input = base_input;
  sort_input.access_kind = plan::PhysicalAccessKind::kSort;
  sort_input.sort_input_rows = 10000;
  sort_input.sort_key_count = 2;
  sort_input.ordering_satisfied = false;
  const auto sort_cost = opt::EstimateCostVector(environment, sort_input);

  auto sort_avoided_input = sort_input;
  sort_avoided_input.ordering_satisfied = true;
  const auto sort_avoided = opt::EstimateCostVector(environment, sort_avoided_input);

  auto unknown_input = base_input;
  unknown_input.input_confidence = opt::CostConfidence::kUnknown;
  const auto unknown = opt::EstimateCostVector(environment, unknown_input);

  return Require(base.selectable, "base cost was unexpectedly rejected") &&
         Require(cached_prefetch.io_cost < base.io_cost,
                 "cache and prefetch did not reduce IO cost") &&
         Require(correlated.io_cost < base.io_cost,
                 "heap correlation did not reduce random IO cost") &&
         Require(noisy.row_cost > base.row_cost,
                 "false positives and duplicates did not raise row CPU cost") &&
         Require(spill.io_cost > base.io_cost && spill.uncertainty_cost > base.uncertainty_cost,
                 "spill did not raise IO and uncertainty cost") &&
         Require(!rejected.selectable &&
                 rejected.confidence == opt::CostConfidence::kRejected &&
                 rejected.rejection_reason == "memory_budget_exceeded_without_spill",
                 "non-spillable memory overage was not rejected") &&
         Require(sort_cost.total_cost > sort_avoided.total_cost,
                 "sort avoidance did not lower total cost") &&
         Require(unknown.uncertainty_cost > base.uncertainty_cost,
                 "unknown confidence did not increase uncertainty");
}

bool AccessPathEnumerationUsesSelectivityRows() {
  auto logical = plan::BuildQueryShapePlan({plan::QueryShapeKind::kPointLookup});
  logical.nodes.front().required_object_uuids.push_back("rel.batch_c");

  opt::OptimizerStatisticsCatalog catalog;
  const auto add = [&](const std::string& name, double value) {
    catalog.Add(opt::MakeStatistic(name, "relation", "rel.batch_c", value,
                                   opt::StatisticSource::kCatalogExact, 9, 0,
                                   opt::CostConfidence::kHigh));
  };
  add("row_count", 10000.0);
  add("visible_row_count", 10000.0);
  add("page_count", 400.0);
  add("average_row_bytes", 128.0);
  add("memory_grant_available_bytes", 1048576.0);
  add("filespace_available_pages", 5000.0);
  add("page_family_read_latency_microseconds", 750.0);
  add("index_depth", 3.0);
  add("index_leaf_pages", 64.0);
  add("index_fragmentation_ratio", 0.01);
  add("index_distinct_keys", 500.0);

  const auto candidates = opt::GenerateLocalAccessPathCandidates(logical.nodes.front(), catalog);
  const auto btree = std::find_if(candidates.begin(), candidates.end(), [](const opt::PlanCandidate& candidate) {
    return candidate.access_kind == plan::PhysicalAccessKind::kScalarBtreeLookup;
  });
  return Require(btree != candidates.end(), "btree candidate missing") &&
         Require(btree->estimated_rows == 20,
                 "btree candidate did not use NDV selectivity row estimate") &&
         Require(std::find(btree->statistics_diagnostics.begin(),
                           btree->statistics_diagnostics.end(),
                           "SB_OPTIMIZER_SELECTIVITY.NDV_EQ") != btree->statistics_diagnostics.end(),
                 "btree candidate did not expose selectivity diagnostic");
}

}  // namespace

int main() {
  if (!RowEstimateSelectivityModelCoversRequiredPredicates()) return 1;
  if (!LegacyGradeCostVectorModelsRequiredEffects()) return 1;
  if (!AccessPathEnumerationUsesSelectivityRows()) return 1;
  return 0;
}
