// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_statistics_full.hpp"
#include "selectivity_model.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool Near(double actual, double expected) {
  return std::abs(actual - expected) < 0.000001;
}

opt::OptimizerStatsIdentity Identity(const std::string& relation_uuid,
                                     const std::string& statistic_uuid,
                                     opt::CostConfidence confidence = opt::CostConfidence::kHigh,
                                     opt::OptimizerStatsFreshnessState freshness =
                                         opt::OptimizerStatsFreshnessState::kFresh) {
  opt::OptimizerStatsIdentity identity;
  identity.object_uuid = relation_uuid;
  identity.statistic_uuid = statistic_uuid;
  identity.stats_epoch = 11;
  identity.catalog_epoch = 7;
  identity.transaction_visibility_epoch = 11;
  identity.freshness = freshness;
  identity.source = opt::StatisticSource::kCatalogSample;
  identity.confidence = confidence;
  return identity;
}

opt::SelectivityEstimate Child(double selectivity,
                               opt::CostConfidence confidence = opt::CostConfidence::kHigh) {
  opt::SelectivityEstimate estimate;
  estimate.selectivity = selectivity;
  estimate.confidence = confidence;
  estimate.diagnostic_code = "child";
  estimate.conservative = false;
  return estimate;
}

opt::ExtendedOptimizerStatistic BaseExtended(const std::string& relation_uuid,
                                             opt::ExtendedOptimizerStatisticKind kind) {
  opt::ExtendedOptimizerStatistic stats;
  stats.identity = Identity(relation_uuid, relation_uuid + ":extended");
  stats.kind = kind;
  stats.relation_uuid = relation_uuid;
  stats.column_uuids = {"col.region", "col.postal"};
  return stats;
}

opt::ExtendedStatsSelectivityRequest BaseRequest(const std::string& relation_uuid) {
  opt::ExtendedStatsSelectivityRequest request;
  request.relation_uuid = relation_uuid;
  request.column_uuids = {"col.postal", "col.region"};
  request.value_encodings = {"CA", "94105"};
  request.children = {Child(0.10), Child(0.10)};
  request.minimum_confidence = opt::CostConfidence::kMedium;
  return request;
}

bool JointMcvAndNdvImproveCorrelatedEquality() {
  auto joint = BaseExtended("rel.customer", opt::ExtendedOptimizerStatisticKind::kJointMcv);
  joint.multi_column_distinct_count = 20;
  joint.joint_mcv.push_back({{"CA", "94105"}, 0.075});

  opt::OptimizerStatisticsStore store;
  store.UpsertExtendedStatistic(joint);
  const auto stats = store.FindExtendedStatisticsForRelation("rel.customer");
  const auto result = opt::EstimateCorrelatedConjunctionSelectivity(
      BaseRequest("rel.customer"),
      stats);

  return Require(result.used_extended_stats, "joint MCV extended stats were not used") &&
         Require(result.diagnostic_code == "SB_OPTIMIZER_EXTENDED_STATS.USED",
                 "joint MCV diagnostic mismatch") &&
         Require(result.used_kind == opt::ExtendedOptimizerStatisticKind::kJointMcv,
                 "joint MCV kind was not reported") &&
         Require(result.estimate.selectivity > 0.01,
                 "joint MCV did not improve over independent selectivity") &&
         Require(Near(result.estimate.selectivity, 0.075),
                 "joint MCV frequency was not selected");
}

bool DependencyAndCorrelationPreventOverReduction() {
  auto dependency = BaseExtended("rel.address",
                                 opt::ExtendedOptimizerStatisticKind::kFunctionalDependency);
  dependency.functional_dependency_strength = 0.95;
  const auto dep_result = opt::EstimateCorrelatedConjunctionSelectivity(
      BaseRequest("rel.address"),
      {dependency});

  auto correlation = BaseExtended("rel.metric",
                                  opt::ExtendedOptimizerStatisticKind::kCrossColumnCorrelation);
  correlation.correlation_coefficient = 0.90;
  const auto corr_result = opt::EstimateCorrelatedConjunctionSelectivity(
      BaseRequest("rel.metric"),
      {correlation});

  return Require(dep_result.used_extended_stats, "functional dependency was not used") &&
         Require(dep_result.estimate.selectivity > 0.09,
                 "functional dependency over-reduced correlated predicates") &&
         Require(corr_result.used_extended_stats, "cross-column correlation was not used") &&
         Require(corr_result.estimate.selectivity > 0.08,
                 "cross-column correlation over-reduced correlated predicates");
}

bool StaleMissingAndShapeMismatchFallbacksAreExact() {
  auto stale = BaseExtended("rel.stale", opt::ExtendedOptimizerStatisticKind::kMultiColumnNdv);
  stale.identity.freshness = opt::OptimizerStatsFreshnessState::kStale;
  stale.multi_column_distinct_count = 10;
  const auto stale_result = opt::EstimateCorrelatedConjunctionSelectivity(
      BaseRequest("rel.stale"),
      {stale});

  const auto missing_result = opt::EstimateCorrelatedConjunctionSelectivity(
      BaseRequest("rel.missing"),
      {});

  auto shape = BaseExtended("rel.shape", opt::ExtendedOptimizerStatisticKind::kMultiColumnNdv);
  shape.column_uuids = {"col.other", "col.shape"};
  const auto shape_result = opt::EstimateCorrelatedConjunctionSelectivity(
      BaseRequest("rel.shape"),
      {shape});

  return Require(!stale_result.used_extended_stats, "stale extended stats were used") &&
         Require(stale_result.diagnostic_code == "SB_OPTIMIZER_EXTENDED_STATS.FALLBACK_STALE",
                 "stale fallback diagnostic mismatch") &&
         Require(Near(stale_result.estimate.selectivity, 0.01),
                 "stale fallback did not use independent scalar product") &&
         Require(!missing_result.used_extended_stats, "missing extended stats were used") &&
         Require(missing_result.diagnostic_code == "SB_OPTIMIZER_EXTENDED_STATS.FALLBACK_MISSING",
                 "missing fallback diagnostic mismatch") &&
         Require(Near(missing_result.estimate.selectivity, 0.01),
                 "missing fallback did not use independent scalar product") &&
         Require(!shape_result.used_extended_stats, "shape-mismatched extended stats were used") &&
         Require(shape_result.diagnostic_code ==
                     "SB_OPTIMIZER_EXTENDED_STATS.FALLBACK_SHAPE_MISMATCH",
                 "shape mismatch fallback diagnostic mismatch");
}

bool FkPkShortcutRemainsAdvisory() {
  auto fkpk = BaseExtended("rel.order_items",
                           opt::ExtendedOptimizerStatisticKind::kFkPkJoinCardinality);
  fkpk.fk_pk_shortcut = true;
  fkpk.fk_pk_estimated_rows = 1200;

  auto request = BaseRequest("rel.order_items");
  request.join_cardinality_request = true;
  const auto result = opt::EstimateCorrelatedConjunctionSelectivity(request, {fkpk});

  return Require(result.used_extended_stats, "FK/PK shortcut was not used") &&
         Require(result.estimate.exact_rows_known, "FK/PK shortcut did not estimate rows") &&
         Require(result.estimate.exact_rows == 1200, "FK/PK shortcut row estimate mismatch") &&
         Require(result.mga_visibility_recheck_required,
                 "FK/PK shortcut disabled MGA visibility recheck") &&
         Require(result.security_recheck_required,
                 "FK/PK shortcut disabled security recheck") &&
         Require(!result.finality_authority, "FK/PK shortcut became finality authority");
}

bool DocumentPathBridgeUsesSameModel() {
  opt::ExtendedOptimizerStatistic bridge;
  bridge.identity = Identity("rel.document", "rel.document:path_bridge");
  bridge.kind = opt::ExtendedOptimizerStatisticKind::kDocumentPathBridge;
  bridge.relation_uuid = "rel.document";
  bridge.document_path_digests = {"sha256:tenant_path", "sha256:state_path"};
  bridge.sampled_dependency_selectivity = 0.12;

  opt::ExtendedStatsSelectivityRequest request;
  request.relation_uuid = "rel.document";
  request.document_path_digests = {"sha256:state_path", "sha256:tenant_path"};
  request.value_encodings = {"tenant-7", "active"};
  request.children = {Child(0.10), Child(0.10)};
  request.minimum_confidence = opt::CostConfidence::kMedium;
  const auto used = opt::EstimateCorrelatedConjunctionSelectivity(request, {bridge});

  auto low_confidence = bridge;
  low_confidence.identity.confidence = opt::CostConfidence::kLow;
  const auto fallback = opt::EstimateCorrelatedConjunctionSelectivity(request, {low_confidence});

  return Require(used.used_extended_stats, "document path bridge was not used") &&
         Require(used.used_kind == opt::ExtendedOptimizerStatisticKind::kDocumentPathBridge,
                 "document path bridge kind mismatch") &&
         Require(Near(used.estimate.selectivity, 0.12),
                 "document path bridge did not use sampled dependency selectivity") &&
         Require(!fallback.used_extended_stats,
                 "low-confidence document path bridge was used") &&
         Require(fallback.diagnostic_code ==
                     "SB_OPTIMIZER_EXTENDED_STATS.FALLBACK_LOW_CONFIDENCE",
                 "document path low-confidence fallback diagnostic mismatch") &&
         Require(Near(fallback.estimate.selectivity, 0.01),
                 "document path fallback did not use independent scalar product");
}

bool HistogramAndSampledDependencyAreSelectedDirectly() {
  auto histogram = BaseExtended("rel.histogram",
                                opt::ExtendedOptimizerStatisticKind::kMultiColumnHistogram);
  histogram.histogram_selectivity = 0.025;
  const auto histogram_result = opt::EstimateCorrelatedConjunctionSelectivity(
      BaseRequest("rel.histogram"),
      {histogram});

  auto sampled = BaseExtended("rel.sampled",
                              opt::ExtendedOptimizerStatisticKind::kSampledDependency);
  sampled.sampled_dependency_selectivity = 0.035;
  const auto sampled_result = opt::EstimateCorrelatedConjunctionSelectivity(
      BaseRequest("rel.sampled"),
      {sampled});

  return Require(histogram_result.used_extended_stats,
                 "multi-column histogram stats were not used") &&
         Require(histogram_result.diagnostic_code == "SB_OPTIMIZER_EXTENDED_STATS.USED",
                 "multi-column histogram diagnostic mismatch") &&
         Require(histogram_result.used_kind ==
                     opt::ExtendedOptimizerStatisticKind::kMultiColumnHistogram,
                 "multi-column histogram kind mismatch") &&
         Require(Near(histogram_result.estimate.selectivity, 0.025),
                 "multi-column histogram selectivity mismatch") &&
         Require(histogram_result.evidence.size() > 1 &&
                     histogram_result.evidence[1] ==
                         "extended_stats_kind=multi_column_histogram",
                 "multi-column histogram evidence missing kind") &&
         Require(sampled_result.used_extended_stats,
                 "sampled dependency stats were not used") &&
         Require(sampled_result.diagnostic_code == "SB_OPTIMIZER_EXTENDED_STATS.USED",
                 "sampled dependency diagnostic mismatch") &&
         Require(sampled_result.used_kind ==
                     opt::ExtendedOptimizerStatisticKind::kSampledDependency,
                 "sampled dependency kind mismatch") &&
         Require(Near(sampled_result.estimate.selectivity, 0.035),
                 "sampled dependency selectivity mismatch") &&
         Require(sampled_result.evidence.size() > 1 &&
                     sampled_result.evidence[1] ==
                         "extended_stats_kind=sampled_dependency",
                 "sampled dependency evidence missing kind");
}

bool SnapshotValidationCoversExtendedStats() {
  opt::OptimizerStatisticsStore store;
  auto histogram = BaseExtended("rel.histogram",
                                opt::ExtendedOptimizerStatisticKind::kMultiColumnHistogram);
  histogram.histogram_selectivity = 0.025;
  auto sampled = BaseExtended("rel.sampled",
                              opt::ExtendedOptimizerStatisticKind::kSampledDependency);
  sampled.sampled_dependency_selectivity = 0.035;
  store.UpsertExtendedStatistic(histogram);
  store.UpsertExtendedStatistic(sampled);

  const auto snapshot = store.Snapshot("odfr010-snapshot");
  const auto statuses = opt::ValidateOptimizerStatsSnapshot(snapshot);
  return Require(snapshot.extended_stats.size() == 2,
                 "snapshot did not retain extended statistics") &&
         Require(statuses.size() == 1 && statuses.front().ok,
                 "extended statistics snapshot validation failed");
}

bool SnapshotValidationRejectsBadJointMcvFrequency() {
  opt::OptimizerStatisticsStore store;
  auto joint = BaseExtended("rel.bad_mcv", opt::ExtendedOptimizerStatisticKind::kJointMcv);
  joint.joint_mcv.push_back({{"CA", "94105"}, 1.25});
  store.UpsertExtendedStatistic(joint);
  const auto statuses = opt::ValidateOptimizerStatsSnapshot(store.Snapshot("bad-mcv"));
  for (const auto& status : statuses) {
    if (status.diagnostic_code == "SB_OPT_EXTENDED_STATS_JOINT_MCV_FREQUENCY_INVALID") {
      return true;
    }
  }
  return Require(false, "invalid joint MCV frequency was not rejected");
}

}  // namespace

int main() {
  if (!JointMcvAndNdvImproveCorrelatedEquality()) return 1;
  if (!DependencyAndCorrelationPreventOverReduction()) return 1;
  if (!StaleMissingAndShapeMismatchFallbacksAreExact()) return 1;
  if (!FkPkShortcutRemainsAdvisory()) return 1;
  if (!DocumentPathBridgeUsesSameModel()) return 1;
  if (!HistogramAndSampledDependencyAreSelectedDirectly()) return 1;
  if (!SnapshotValidationCoversExtendedStats()) return 1;
  if (!SnapshotValidationRejectsBadJointMcvFrequency()) return 1;
  return 0;
}
