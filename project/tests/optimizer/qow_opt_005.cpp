// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_contract.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

constexpr std::string_view kCatalogEpoch =
    "019f0000-0000-7100-8000-000000005001";
constexpr std::string_view kStatisticsSnapshot =
    "019f0000-0000-7100-8000-000000005002";
constexpr std::string_view kRelation =
    "019f0000-0000-7100-8000-000000005003";

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-OPT-005-V1: " << detail << '\n';
  return condition;
}

plan::CanonicalLogicalRelationalGraph Graph() {
  plan::CanonicalLogicalRelationalGraph graph;
  graph.bound_sblr_tree_uuid =
      "019f0000-0000-7100-8000-000000005004";
  graph.catalog_epoch_uuid = std::string(kCatalogEpoch);
  graph.security_context_uuid =
      "019f0000-0000-7100-8000-000000005005";
  graph.local_transaction_id = 501;
  graph.statement_snapshot_id = 499;
  graph.root_logical_node_id = 3;
  graph.result_descriptor_ids = {1};

  plan::CanonicalLogicalRelationalNode values;
  values.logical_node_id = 1;
  values.node_kind = plan::CanonicalLogicalRelationalNodeKind::kValues;
  values.output_descriptor_ids = {1};
  values.bound_expression_ids = {1};
  values.origin_relational_node_ids = {1};
  values.semantic_variant_id = "values.literal-table.v1";

  plan::CanonicalLogicalRelationalNode scan;
  scan.logical_node_id = 2;
  scan.node_kind = plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
  scan.output_descriptor_ids = {1};
  scan.required_object_uuids = {std::string(kRelation)};
  scan.origin_relational_node_ids = {2};
  scan.semantic_variant_id = "relation.source.v1";

  plan::CanonicalLogicalRelationalNode join;
  join.logical_node_id = 3;
  join.node_kind = plan::CanonicalLogicalRelationalNodeKind::kJoin;
  join.input_logical_node_ids = {1, 2};
  join.output_descriptor_ids = {1};
  join.bound_expression_ids = {1};
  join.required_object_uuids = {std::string(kRelation)};
  join.origin_relational_node_ids = {3};
  join.semantic_variant_id = "join.inner.v1";
  graph.nodes = {values, scan, join};
  return graph;
}

opt::CanonicalOptimizerNodeEstimate Estimate(
    const std::uint32_t node_id,
    const opt::CanonicalOptimizerStatisticState state) {
  opt::CanonicalOptimizerNodeEstimate estimate;
  estimate.logical_node_id = node_id;
  estimate.state = state;
  estimate.source = opt::CanonicalOptimizerStatisticSource::kUnavailable;
  estimate.catalog_epoch_uuid = std::string(kCatalogEpoch);
  estimate.statistics_snapshot_uuid = std::string(kStatisticsSnapshot);
  estimate.statistics_generation = 9;
  estimate.admitted_at_monotonic_ns = 1'000'000;
  estimate.confidence = opt::CostConfidence::kUnknown;
  return estimate;
}

opt::CanonicalOptimizerStatisticsSnapshot Snapshot() {
  opt::CanonicalOptimizerStatisticsSnapshot snapshot;
  snapshot.statistics_snapshot_uuid = std::string(kStatisticsSnapshot);
  snapshot.catalog_epoch_uuid = std::string(kCatalogEpoch);
  snapshot.statistics_generation = 9;
  snapshot.admitted_at_monotonic_ns = 1'000'000;
  snapshot.captured_before_data_access = true;
  auto values = Estimate(
      1, opt::CanonicalOptimizerStatisticState::kNotApplicable);
  auto scan = Estimate(2, opt::CanonicalOptimizerStatisticState::kKnown);
  scan.object_uuid = std::string(kRelation);
  scan.source = opt::CanonicalOptimizerStatisticSource::kCatalogSample;
  scan.collected_at_monotonic_ns = 900'000;
  scan.maximum_age_ns = 200'000;
  scan.confidence = opt::CostConfidence::kMedium;
  scan.row_count_present = true;
  scan.row_count = 0;
  scan.page_count_present = true;
  scan.page_count = 1;
  auto join = Estimate(3, opt::CanonicalOptimizerStatisticState::kUnknown);
  snapshot.node_estimates = {values, scan, join};
  return snapshot;
}

bool ValidateQualifiedAndUnknownStatistics() {
  const auto result = opt::AdmitCanonicalOptimizerStatisticsBeforeAccess(
      Graph(), Snapshot());
  if (!result.accepted && !result.issues.empty()) {
    std::cerr << "QOW-TEST-OPT-005-V1: initial_refusal="
              << result.issues.front().diagnostic_id << ':'
              << result.issues.front().field_id << '\n';
  }
  bool passed = true;
  passed &= Require(result.accepted &&
                        result.degraded_for_unknown_statistics &&
                        !result.benchmark_clean_ready &&
                        !result.data_access_allowed &&
                        result.known_estimate_count == 1 &&
                        result.unknown_estimate_count == 1 &&
                        result.not_applicable_estimate_count == 1,
                    "qualified known/unknown statistics were not preserved");

  auto all_known = Snapshot();
  auto& join = all_known.node_estimates[2];
  join.state = opt::CanonicalOptimizerStatisticState::kKnown;
  join.object_uuid = std::string(kRelation);
  join.source = opt::CanonicalOptimizerStatisticSource::kCatalogExact;
  join.collected_at_monotonic_ns = 950'000;
  join.maximum_age_ns = 100'000;
  join.confidence = opt::CostConfidence::kHigh;
  join.row_count_present = true;
  join.row_count = 0;
  const auto clean = opt::AdmitCanonicalOptimizerStatisticsBeforeAccess(
      Graph(), all_known);
  passed &= Require(clean.accepted && clean.benchmark_clean_ready &&
                        !clean.degraded_for_unknown_statistics &&
                        !clean.data_access_allowed,
                    "fully catalog-qualified snapshot was not clean-ready");
  return passed;
}

bool ValidateNoDefaultOrActualSubstitution() {
  bool passed = true;
  auto missing = Snapshot();
  missing.node_estimates.pop_back();
  const auto missing_result =
      opt::AdmitCanonicalOptimizerStatisticsBeforeAccess(Graph(), missing);
  passed &= Require(!missing_result.accepted &&
                        missing_result.issues.front().diagnostic_id ==
                            "QOW-DIAG-OPTIMIZER-STATISTICS-COVERAGE-V1",
                    "missing estimate acquired a default");

  auto substituted_unknown = Snapshot();
  substituted_unknown.node_estimates[2].row_count_present = true;
  substituted_unknown.node_estimates[2].row_count = 1000;
  const auto substituted_result =
      opt::AdmitCanonicalOptimizerStatisticsBeforeAccess(
          Graph(), substituted_unknown);
  passed &= Require(!substituted_result.accepted &&
                        substituted_result.issues.front().diagnostic_id ==
                            "QOW-DIAG-OPTIMIZER-STATISTICS-UNKNOWN-V1",
                    "unknown statistic accepted a numeric substitute");

  auto actual = Snapshot();
  actual.runtime_actuals_present = true;
  actual.node_estimates[1].derived_from_runtime_actuals = true;
  const auto actual_result =
      opt::AdmitCanonicalOptimizerStatisticsBeforeAccess(Graph(), actual);
  passed &= Require(!actual_result.accepted &&
                        actual_result.issues.front().diagnostic_id ==
                            "QOW-DIAG-OPTIMIZER-STATISTICS-PHASE-V1",
                    "runtime actuals entered pre-access estimates");

  auto stale = Snapshot();
  stale.node_estimates[1].collected_at_monotonic_ns = 100;
  const auto stale_result =
      opt::AdmitCanonicalOptimizerStatisticsBeforeAccess(Graph(), stale);
  passed &= Require(!stale_result.accepted &&
                        stale_result.issues.front().diagnostic_id ==
                            "QOW-DIAG-OPTIMIZER-STATISTICS-PROVENANCE-V1",
                    "stale catalog statistic was admitted as current");
  return passed;
}

}  // namespace

// QOW-TEST-OPT-005-V1
int main() {
  bool passed = true;
  passed &= ValidateQualifiedAndUnknownStatistics();
  passed &= ValidateNoDefaultOrActualSubstitution();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
