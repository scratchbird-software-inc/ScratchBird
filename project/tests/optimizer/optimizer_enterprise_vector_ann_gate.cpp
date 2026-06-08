// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_vector_ann_costing_enterprise.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "OEIC-061 gate failure: " << message << '\n';
    return false;
  }
  return true;
}

bool ContainsPrefix(const std::vector<std::string>& values, const std::string& prefix) {
  return std::any_of(values.begin(), values.end(), [&](const std::string& value) {
    return value.find(prefix) == 0;
  });
}

opt::OptimizerStatsIdentity Identity(const std::string& object_uuid) {
  opt::OptimizerStatsIdentity identity;
  identity.object_uuid = object_uuid;
  identity.statistic_uuid = "stats:" + object_uuid;
  identity.stats_epoch = 6101;
  identity.catalog_epoch = 6100;
  identity.transaction_visibility_epoch = 6102;
  identity.freshness = opt::OptimizerStatsFreshnessState::kFresh;
  identity.source = opt::StatisticSource::kCatalogExact;
  identity.confidence = opt::CostConfidence::kHigh;
  return identity;
}

opt::IndexStats VectorIndex(const std::string& family) {
  opt::IndexStats index;
  index.identity = Identity("idx.vector.061");
  index.index_uuid = "idx.vector.061";
  index.relation_uuid = "rel.embedding.061";
  index.index_family = family;
  index.height = 3;
  index.leaf_pages = 2048;
  index.distinct_keys = 1'000'000;
  index.clustering_factor = 0.7;
  index.visibility_coverage = 1.0;
  index.predicate_coverage = 0.2;
  index.candidate_set_producer = true;
  index.exact_recheck_required = true;
  index.mga_recheck_required = true;
  index.security_recheck_required = true;
  index.route_benchmark_clean = true;
  index.false_positive_ratio = 0.02;
  return index;
}

opt::TableCardinalityStats Table() {
  opt::TableCardinalityStats table;
  table.identity = Identity("rel.embedding.061");
  table.row_count = 1'000'000;
  table.visible_row_count = 980'000;
  table.page_count = 20'000;
  table.average_row_bytes = 512;
  return table;
}

opt::EnterpriseIndexCostAuthority Authority(bool exact_rerank = true) {
  opt::EnterpriseIndexCostAuthority authority;
  authority.optimizer_scope = true;
  authority.catalog_descriptor_authority = true;
  authority.index_stats_authority = true;
  authority.route_capability_authority = true;
  authority.runtime_metric_authority = true;
  authority.generated_index_readiness_manifest = true;
  authority.readiness_manifest_current = true;
  authority.route_runtime_proof = true;
  authority.operation_metric_producer_proof = true;
  authority.support_bundle_producer_proof = true;
  authority.crash_cleanup_corruption_proof = true;
  authority.storage_integration_proof = true;
  authority.exact_recheck_preserved = true;
  authority.mga_recheck_preserved = true;
  authority.security_recheck_preserved = true;
  authority.exact_rerank_proven = exact_rerank;
  return authority;
}

opt::EnterpriseVectorRuntimeMetric Metric() {
  opt::EnterpriseVectorRuntimeMetric metric;
  metric.metric_snapshot_id = "metrics:vector:061";
  metric.route_label = "embedded.local.vector.061";
  metric.provider_id = "vector_provider.local";
  metric.index_generation = "vector_generation:061";
  metric.result_contract_hash = "sha256:vector-result-061";
  metric.evidence_digest = "sha256:vector-evidence-061";
  metric.generation = 6110;
  metric.route_epoch = 6111;
  metric.stats_epoch = 6112;
  metric.index_generation_epoch = 6113;
  metric.vector_count = 1'000'000;
  metric.dimensions = 768;
  metric.top_k = 25;
  metric.candidate_rows = 500;
  metric.exact_rerank_rows = 500;
  metric.tombstone_rows = 5'000;
  metric.ef_search = 96;
  metric.nprobe = 16;
  metric.recall_observed = 0.98;
  metric.metadata_prefilter_selectivity = 0.25;
  metric.list_imbalance_ratio = 0.12;
  metric.quantization_error_ratio = 0.04;
  metric.fresh = true;
  metric.trusted = true;
  metric.exact_payload_available = true;
  metric.exact_rerank_available = true;
  metric.exact_fallback_available = true;
  metric.metadata_prefilter_available = true;
  return metric;
}

opt::EnterpriseVectorAnnCostingRequest Request(
    opt::EnterpriseVectorIndexProfile profile) {
  opt::EnterpriseVectorAnnCostingRequest request;
  request.profile = profile;
  request.index = VectorIndex(profile == opt::EnterpriseVectorIndexProfile::kExact
                                  ? "vector_exact"
                                  : profile == opt::EnterpriseVectorIndexProfile::kHnsw
                                        ? "vector_hnsw"
                                        : "vector_ivf");
  request.table = Table();
  request.environment.cost_profile_id = "enterprise-vector-061";
  request.environment.memory_budget_bytes = 64 * 1024 * 1024;
  request.authority = Authority(true);
  request.metric = Metric();
  return request;
}

bool HnswConsumesRecallAndRerankMetrics() {
  const auto result = opt::EstimateEnterpriseVectorAnnCost(
      Request(opt::EnterpriseVectorIndexProfile::kHnsw));
  if (!Require(result.accepted, "HNSW vector costing was refused")) return false;
  return Require(result.selectable, "HNSW vector costing was not selectable") &&
         Require(!result.exact_fallback_selected,
                 "strong HNSW metrics should not choose exact fallback") &&
         Require(result.exact_rerank_rows == 500,
                 "exact rerank rows were not consumed") &&
         Require(ContainsPrefix(result.evidence, "recall_observed="),
                 "recall evidence missing") &&
         Require(ContainsPrefix(result.evidence, "metadata_prefilter_selectivity="),
                 "metadata prefilter evidence missing");
}

bool PoorRecallSelectsExactFallback() {
  auto request = Request(opt::EnterpriseVectorIndexProfile::kIvfPq);
  request.metric.recall_observed = 0.72;
  const auto result = opt::EstimateEnterpriseVectorAnnCost(request);
  return Require(result.accepted, "poor recall with exact fallback was refused") &&
         Require(result.exact_fallback_selected,
                 "poor recall did not select exact fallback") &&
         Require(result.diagnostic_code ==
                     "SB_OPT_VECTOR_ANN_EXACT_FALLBACK_SELECTED",
                 "exact fallback diagnostic changed");
}

bool MissingExactRerankFailsClosed() {
  auto request = Request(opt::EnterpriseVectorIndexProfile::kHnsw);
  request.metric.exact_rerank_available = false;
  request.authority.exact_rerank_proven = false;
  const auto result = opt::EstimateEnterpriseVectorAnnCost(request);
  return Require(!result.accepted, "missing exact rerank was accepted") &&
         Require(result.diagnostic_code ==
                     "SB_OPT_VECTOR_ANN_EXACT_RERANK_REQUIRED",
                 "missing exact rerank diagnostic changed");
}

bool TombstoneAndImbalanceRecommendRebuild() {
  auto request = Request(opt::EnterpriseVectorIndexProfile::kIvfSq8);
  request.metric.tombstone_rows = 350'000;
  request.metric.list_imbalance_ratio = 0.85;
  const auto result = opt::EstimateEnterpriseVectorAnnCost(request);
  return Require(result.accepted, "imbalanced vector index was refused") &&
         Require(result.rebuild_recommended,
                 "tombstone/list imbalance did not recommend rebuild") &&
         Require(ContainsPrefix(result.evidence, "rebuild_recommended=true"),
                 "rebuild evidence missing");
}

bool UnsafeMetricAuthorityFailsClosed() {
  auto request = Request(opt::EnterpriseVectorIndexProfile::kHnsw);
  request.metric.cluster_route_or_metric_projection = true;
  const auto result = opt::EstimateEnterpriseVectorAnnCost(request);
  return Require(!result.accepted, "cluster vector metric was accepted") &&
         Require(result.diagnostic_code == "SB_OPT_VECTOR_ANN_METRIC_REQUIRED",
                 "unsafe metric diagnostic changed");
}

}  // namespace

int main() {
  // SEARCH_KEY: OEIC_VECTOR_ANN_RECALL_COSTING
  if (!HnswConsumesRecallAndRerankMetrics()) return EXIT_FAILURE;
  if (!PoorRecallSelectsExactFallback()) return EXIT_FAILURE;
  if (!MissingExactRerankFailsClosed()) return EXIT_FAILURE;
  if (!TombstoneAndImbalanceRecommendRebuild()) return EXIT_FAILURE;
  if (!UnsafeMetricAuthorityFailsClosed()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
