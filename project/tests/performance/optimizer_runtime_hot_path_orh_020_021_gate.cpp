// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_statistics_full.hpp"
#include "selectivity_model.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ORH-020/021 gate failure: " << message << '\n';
    return false;
  }
  return true;
}

bool Near(double actual, double expected) {
  return std::abs(actual - expected) < 0.000001;
}

bool Has(const std::vector<std::string>& evidence, const std::string& value) {
  return std::find(evidence.begin(), evidence.end(), value) != evidence.end();
}

opt::OptimizerStatsIdentity Identity(
    const std::string& relation_uuid,
    const std::string& statistic_uuid,
    opt::CostConfidence confidence = opt::CostConfidence::kHigh,
    std::uint64_t stats_epoch = 20,
    opt::OptimizerStatsFreshnessState freshness =
        opt::OptimizerStatsFreshnessState::kFresh) {
  opt::OptimizerStatsIdentity identity;
  identity.object_uuid = relation_uuid;
  identity.statistic_uuid = statistic_uuid;
  identity.stats_epoch = stats_epoch;
  identity.catalog_epoch = 7;
  identity.transaction_visibility_epoch = stats_epoch;
  identity.freshness = freshness;
  identity.source = opt::StatisticSource::kCatalogSample;
  identity.confidence = confidence;
  return identity;
}

opt::SelectivityEstimate Child(double selectivity) {
  opt::SelectivityEstimate estimate;
  estimate.selectivity = selectivity;
  estimate.confidence = opt::CostConfidence::kHigh;
  estimate.diagnostic_code = "orh-child";
  estimate.conservative = false;
  return estimate;
}

opt::ExtendedStatsSelectivityRequest DocumentRequest(
    const std::string& relation_uuid) {
  opt::ExtendedStatsSelectivityRequest request;
  request.relation_uuid = relation_uuid;
  request.document_path_digests = {"path.tenant", "path.status"};
  request.value_encodings = {"tenant-7", "active"};
  request.children = {Child(0.10), Child(0.20)};
  request.minimum_confidence = opt::CostConfidence::kMedium;
  return request;
}

opt::ExtendedStatsSelectivityRequest ColumnRequest(
    const std::string& relation_uuid) {
  opt::ExtendedStatsSelectivityRequest request;
  request.relation_uuid = relation_uuid;
  request.column_uuids = {"col.tenant", "col.status", "col.region"};
  request.value_encodings = {"tenant-7", "active", "north"};
  request.children = {Child(0.10), Child(0.20), Child(0.50)};
  request.minimum_confidence = opt::CostConfidence::kMedium;
  return request;
}

opt::ExtendedOptimizerStatistic BaseDocumentStat(
    const std::string& relation_uuid,
    const std::string& statistic_uuid,
    opt::ExtendedOptimizerStatisticKind kind) {
  opt::ExtendedOptimizerStatistic stats;
  stats.identity = Identity(relation_uuid, statistic_uuid);
  stats.kind = kind;
  stats.relation_uuid = relation_uuid;
  stats.document_path_digests = {"path.tenant", "path.status"};
  return stats;
}

opt::ExtendedOptimizerStatistic BaseColumnStat(
    const std::string& relation_uuid,
    const std::string& statistic_uuid,
    opt::ExtendedOptimizerStatisticKind kind,
    std::vector<std::string> columns) {
  opt::ExtendedOptimizerStatistic stats;
  stats.identity = Identity(relation_uuid, statistic_uuid);
  stats.kind = kind;
  stats.relation_uuid = relation_uuid;
  stats.column_uuids = std::move(columns);
  return stats;
}

std::vector<opt::ExtendedOptimizerStatistic> RankedFamilyStats(
    const std::string& relation_uuid) {
  auto document_bridge = BaseDocumentStat(
      relation_uuid,
      "rank.document",
      opt::ExtendedOptimizerStatisticKind::kDocumentPathBridge);
  document_bridge.sampled_dependency_selectivity = 0.040;

  auto ndv = BaseDocumentStat(
      relation_uuid,
      "rank.ndv",
      opt::ExtendedOptimizerStatisticKind::kMultiColumnNdv);
  ndv.multi_column_distinct_count = 80;

  auto correlation = BaseDocumentStat(
      relation_uuid,
      "rank.correlation",
      opt::ExtendedOptimizerStatisticKind::kCrossColumnCorrelation);
  correlation.correlation_coefficient = 0.88;

  auto functional = BaseDocumentStat(
      relation_uuid,
      "rank.functional",
      opt::ExtendedOptimizerStatisticKind::kFunctionalDependency);
  functional.functional_dependency_strength = 0.90;

  auto sampled = BaseDocumentStat(
      relation_uuid,
      "rank.sampled",
      opt::ExtendedOptimizerStatisticKind::kSampledDependency);
  sampled.sampled_dependency_selectivity = 0.050;

  auto histogram = BaseDocumentStat(
      relation_uuid,
      "rank.histogram",
      opt::ExtendedOptimizerStatisticKind::kMultiColumnHistogram);
  histogram.histogram_selectivity = 0.060;

  auto joint = BaseDocumentStat(
      relation_uuid,
      "rank.joint",
      opt::ExtendedOptimizerStatisticKind::kJointMcv);
  joint.multi_column_distinct_count = 40;
  joint.joint_mcv.push_back({{"tenant-7", "active"}, 0.070});

  return {document_bridge, ndv, correlation, functional, sampled, histogram, joint};
}

bool RankedFamilyPrecedenceIsDeterministic() {
  const auto request = DocumentRequest("rel.orh020.rank");
  const auto stats = RankedFamilyStats("rel.orh020.rank");

  const auto all =
      opt::EstimateCorrelatedConjunctionSelectivity(request, stats);
  if (!Require(all.used_kind == opt::ExtendedOptimizerStatisticKind::kJointMcv,
               "joint MCV exact match did not rank first") ||
      !Require(Near(all.estimate.selectivity, 0.070),
               "joint MCV exact frequency was not selected")) {
    return false;
  }

  auto without_joint = stats;
  without_joint.pop_back();
  const auto histogram =
      opt::EstimateCorrelatedConjunctionSelectivity(request, without_joint);
  if (!Require(histogram.used_kind ==
                   opt::ExtendedOptimizerStatisticKind::kMultiColumnHistogram,
               "multi-column histogram did not outrank sampled dependency") ||
      !Require(Near(histogram.estimate.selectivity, 0.060),
               "multi-column histogram selectivity mismatch")) {
    return false;
  }

  without_joint.pop_back();
  const auto sampled =
      opt::EstimateCorrelatedConjunctionSelectivity(request, without_joint);
  if (!Require(sampled.used_kind ==
                   opt::ExtendedOptimizerStatisticKind::kSampledDependency,
               "sampled dependency did not outrank functional dependency") ||
      !Require(Near(sampled.estimate.selectivity, 0.050),
               "sampled dependency selectivity mismatch")) {
    return false;
  }

  without_joint.pop_back();
  const auto functional =
      opt::EstimateCorrelatedConjunctionSelectivity(request, without_joint);
  if (!Require(functional.used_kind ==
                   opt::ExtendedOptimizerStatisticKind::kFunctionalDependency,
               "functional dependency did not outrank correlation") ||
      !Require(functional.estimate.selectivity > 0.18,
               "functional dependency was not applied")) {
    return false;
  }

  without_joint.pop_back();
  const auto correlation =
      opt::EstimateCorrelatedConjunctionSelectivity(request, without_joint);
  if (!Require(correlation.used_kind ==
                   opt::ExtendedOptimizerStatisticKind::kCrossColumnCorrelation,
               "cross-column correlation did not outrank NDV") ||
      !Require(correlation.estimate.selectivity > 0.17,
               "cross-column correlation was not applied")) {
    return false;
  }

  without_joint.pop_back();
  const auto ndv =
      opt::EstimateCorrelatedConjunctionSelectivity(request, without_joint);
  if (!Require(ndv.used_kind == opt::ExtendedOptimizerStatisticKind::kMultiColumnNdv,
               "multi-column NDV did not outrank document-path bridge") ||
      !Require(Near(ndv.estimate.selectivity, 0.0125),
               "multi-column NDV selectivity mismatch")) {
    return false;
  }

  without_joint.pop_back();
  const auto bridge =
      opt::EstimateCorrelatedConjunctionSelectivity(request, without_joint);
  return Require(bridge.used_kind ==
                     opt::ExtendedOptimizerStatisticKind::kDocumentPathBridge,
                 "document-path bridge was not selected as final ranked family") &&
         Require(Near(bridge.estimate.selectivity, 0.040),
                 "document-path bridge selectivity mismatch");
}

bool TieBreakersUseConfidenceEpochSpecificityAndObservedError() {
  const auto request = ColumnRequest("rel.orh020.tie");

  auto medium_new = BaseColumnStat(
      request.relation_uuid,
      "tie.medium_new",
      opt::ExtendedOptimizerStatisticKind::kMultiColumnHistogram,
      request.column_uuids);
  medium_new.identity.confidence = opt::CostConfidence::kMedium;
  medium_new.identity.stats_epoch = 90;
  medium_new.histogram_selectivity = 0.010;

  auto high_old = BaseColumnStat(
      request.relation_uuid,
      "tie.high_old",
      opt::ExtendedOptimizerStatisticKind::kMultiColumnHistogram,
      request.column_uuids);
  high_old.identity.stats_epoch = 10;
  high_old.histogram_selectivity = 0.020;

  auto high_new_bad_error = BaseColumnStat(
      request.relation_uuid,
      "tie.high_new_bad_error",
      opt::ExtendedOptimizerStatisticKind::kMultiColumnHistogram,
      request.column_uuids);
  high_new_bad_error.identity.stats_epoch = 20;
  high_new_bad_error.histogram_selectivity = 0.030;
  high_new_bad_error.observed_selectivity_error = 0.40;

  auto high_new_good_error = BaseColumnStat(
      request.relation_uuid,
      "tie.high_new_good_error",
      opt::ExtendedOptimizerStatisticKind::kMultiColumnHistogram,
      request.column_uuids);
  high_new_good_error.identity.stats_epoch = 20;
  high_new_good_error.histogram_selectivity = 0.040;
  high_new_good_error.observed_selectivity_error = 0.05;

  const auto result = opt::EstimateCorrelatedConjunctionSelectivity(
      request,
      {medium_new, high_old, high_new_bad_error, high_new_good_error});
  if (!Require(Near(result.estimate.selectivity, 0.040),
               "confidence/epoch/observed-error tie-break selected wrong stat") ||
      !Require(Has(result.evidence,
                   "extended_stats_selected_uuid=tie.high_new_good_error"),
               "selected stat UUID evidence missing for observed-error tie-break")) {
    return false;
  }

  auto subset = BaseColumnStat(
      request.relation_uuid,
      "tie.subset",
      opt::ExtendedOptimizerStatisticKind::kMultiColumnHistogram,
      {"col.tenant", "col.status"});
  subset.histogram_selectivity = 0.050;
  subset.observed_selectivity_error = 0.10;

  auto full = BaseColumnStat(
      request.relation_uuid,
      "tie.full",
      opt::ExtendedOptimizerStatisticKind::kMultiColumnHistogram,
      request.column_uuids);
  full.histogram_selectivity = 0.080;
  full.observed_selectivity_error = 0.10;

  const auto specificity = opt::EstimateCorrelatedConjunctionSelectivity(
      request,
      {subset, full});
  return Require(Near(specificity.estimate.selectivity, 0.080),
                 "predicate-shape specificity did not prefer full-shape stat") &&
         Require(Has(specificity.evidence, "extended_stats_selected_uuid=tie.full"),
                 "full-shape specificity evidence missing");
}

bool PartialShapeCompositionCombinesSubsetsAndRemainingScalars() {
  opt::ExtendedStatsSelectivityRequest request;
  request.relation_uuid = "rel.orh021.partial";
  request.column_uuids = {"col.tenant", "col.status", "col.region", "col.channel",
                          "col.priority"};
  request.value_encodings = {"tenant-7", "active", "north", "web", "high"};
  request.children = {Child(0.10), Child(0.20), Child(0.50), Child(0.25),
                      Child(0.40)};
  request.minimum_confidence = opt::CostConfidence::kMedium;

  auto tenant_status = BaseColumnStat(
      request.relation_uuid,
      "partial.tenant_status",
      opt::ExtendedOptimizerStatisticKind::kMultiColumnHistogram,
      {"col.tenant", "col.status"});
  tenant_status.histogram_selectivity = 0.060;

  auto region_channel = BaseColumnStat(
      request.relation_uuid,
      "partial.region_channel",
      opt::ExtendedOptimizerStatisticKind::kSampledDependency,
      {"col.region", "col.channel"});
  region_channel.sampled_dependency_selectivity = 0.250;

  const auto result = opt::EstimateCorrelatedConjunctionSelectivity(
      request,
      {region_channel, tenant_status});
  return Require(result.used_extended_stats,
                 "partial-shape extended stats were not used") &&
         Require(Near(result.estimate.selectivity, 0.006),
                 "partial-shape composition did not combine subsets and scalar") &&
         Require(Has(result.evidence, "extended_stats_selected_count=2"),
                 "partial-shape selected subset count evidence missing") &&
         Require(Has(result.evidence, "extended_stats_remaining_scalar_predicates=1"),
                 "partial-shape remaining scalar evidence missing") &&
         Require(Has(result.evidence,
                     "extended_stats_partial_shape_composition=true"),
                 "partial-shape composition evidence missing");
}

bool StaleAuthorityStatsRemainFailClosedDuringPartialFallback() {
  const auto request = ColumnRequest("rel.orh021.fail_closed");

  auto stale_full = BaseColumnStat(
      request.relation_uuid,
      "fail_closed.stale_full",
      opt::ExtendedOptimizerStatisticKind::kJointMcv,
      request.column_uuids);
  stale_full.identity.freshness = opt::OptimizerStatsFreshnessState::kStale;
  stale_full.joint_mcv.push_back({{"tenant-7", "active", "north"}, 0.700});

  auto authority_full = BaseColumnStat(
      request.relation_uuid,
      "fail_closed.authority_full",
      opt::ExtendedOptimizerStatisticKind::kMultiColumnHistogram,
      request.column_uuids);
  authority_full.histogram_selectivity = 0.800;
  authority_full.finality_authority = true;

  auto fresh_subset = BaseColumnStat(
      request.relation_uuid,
      "fail_closed.fresh_subset",
      opt::ExtendedOptimizerStatisticKind::kMultiColumnHistogram,
      {"col.tenant", "col.status"});
  fresh_subset.histogram_selectivity = 0.060;

  const auto result = opt::EstimateCorrelatedConjunctionSelectivity(
      request,
      {stale_full, authority_full, fresh_subset});
  return Require(result.used_extended_stats,
                 "fresh partial stat was not used after stale/authority refusals") &&
         Require(result.used_kind ==
                     opt::ExtendedOptimizerStatisticKind::kMultiColumnHistogram,
                 "stale or authority-bearing full stat was selected") &&
         Require(Near(result.estimate.selectivity, 0.030),
                 "fresh partial stat was not composed with remaining scalar") &&
         Require(result.mga_visibility_recheck_required,
                 "partial extended stat disabled MGA visibility recheck") &&
         Require(result.security_recheck_required,
                 "partial extended stat disabled security recheck") &&
         Require(!result.finality_authority,
                 "partial extended stat became finality authority");
}

}  // namespace

int main() {
  if (!RankedFamilyPrecedenceIsDeterministic()) return 1;
  if (!TieBreakersUseConfidenceEpochSpecificityAndObservedError()) return 1;
  if (!PartialShapeCompositionCombinesSubsetsAndRemainingScalars()) return 1;
  if (!StaleAuthorityStatsRemainFailClosedDuringPartialFallback()) return 1;
  return 0;
}
