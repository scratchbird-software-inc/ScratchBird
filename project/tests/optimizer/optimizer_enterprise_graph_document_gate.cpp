// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_graph_document_costing_enterprise.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "OEIC-063 gate failure: " << message << '\n';
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
  identity.stats_epoch = 6301;
  identity.catalog_epoch = 6300;
  identity.transaction_visibility_epoch = 6302;
  identity.freshness = opt::OptimizerStatsFreshnessState::kFresh;
  identity.source = opt::StatisticSource::kCatalogExact;
  identity.confidence = opt::CostConfidence::kHigh;
  return identity;
}

opt::IndexStats Index(const std::string& family) {
  opt::IndexStats index;
  index.identity = Identity("idx." + family + ".063");
  index.index_uuid = "idx." + family + ".063";
  index.relation_uuid = "rel.mixed.063";
  index.index_family = family;
  index.height = 3;
  index.leaf_pages = 2048;
  index.distinct_keys = 250'000;
  index.clustering_factor = 0.6;
  index.visibility_coverage = 1.0;
  index.predicate_coverage = 0.15;
  index.candidate_set_producer = true;
  index.exact_recheck_required = true;
  index.mga_recheck_required = true;
  index.security_recheck_required = true;
  index.route_benchmark_clean = true;
  index.false_positive_ratio = 0.08;
  return index;
}

opt::TableCardinalityStats Table() {
  opt::TableCardinalityStats table;
  table.identity = Identity("rel.mixed.063");
  table.row_count = 500'000;
  table.visible_row_count = 490'000;
  table.page_count = 12'000;
  table.average_row_bytes = 768;
  return table;
}

opt::EnterpriseIndexCostAuthority Authority() {
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
  return authority;
}

opt::EnterpriseGraphDocumentMetric Metric(
    opt::EnterpriseGraphDocumentProfile profile) {
  opt::EnterpriseGraphDocumentMetric metric;
  metric.metric_snapshot_id = profile == opt::EnterpriseGraphDocumentProfile::kDocumentPath
                                  ? "metrics:document:063"
                                  : "metrics:graph:063";
  metric.route_label = "embedded.local.graph_document.063";
  metric.provider_id = profile == opt::EnterpriseGraphDocumentProfile::kDocumentPath
                           ? "document_provider.local"
                           : "graph_provider.local";
  metric.index_generation = "generation:063";
  metric.result_contract_hash = "sha256:graph-document-result-063";
  metric.evidence_digest = "sha256:graph-document-evidence-063";
  metric.generation = 6310;
  metric.route_epoch = 6311;
  metric.stats_epoch = 6312;
  metric.candidate_rows = 1'000;
  metric.exact_recheck_rows = 1'200;
  metric.document_shape_count = 16;
  metric.document_array_expansion_rows = 400;
  metric.document_wildcard_fanout = 20;
  metric.graph_frontier_width = 128;
  metric.graph_adjacency_degree = 12;
  metric.graph_label_selectivity_ppm = 120'000;
  metric.graph_property_selectivity_ppm = 80'000;
  metric.graph_visited_bitmap_density_ppm = 15'000;
  metric.document_path_selectivity = 0.05;
  metric.document_shape_selectivity = 0.20;
  metric.false_positive_ratio = 0.08;
  metric.fresh = true;
  metric.trusted = true;
  metric.exact_recheck_available = true;
  metric.path_wildcard_proof_present = true;
  metric.array_expansion_proof_present = true;
  metric.graph_frontier_proof_present = true;
  metric.graph_adjacency_proof_present = true;
  return metric;
}

opt::EnterpriseGraphDocumentCostingRequest Request(
    opt::EnterpriseGraphDocumentProfile profile) {
  opt::EnterpriseGraphDocumentCostingRequest request;
  request.profile = profile;
  request.index = Index(profile == opt::EnterpriseGraphDocumentProfile::kDocumentPath
                            ? "document_path"
                            : "graph");
  request.table = Table();
  request.environment.cost_profile_id = "enterprise-graph-document-063";
  request.environment.memory_budget_bytes = 64 * 1024 * 1024;
  request.authority = Authority();
  request.metric = Metric(profile);
  return request;
}

bool DocumentConsumesShapeWildcardAndArrayMetrics() {
  const auto result = opt::EstimateEnterpriseGraphDocumentCost(
      Request(opt::EnterpriseGraphDocumentProfile::kDocumentPath));
  if (!Require(result.accepted, "document-path costing was refused")) return false;
  return Require(result.selectable, "document-path costing was not selectable") &&
         Require(result.exact_recheck_rows == 1'200,
                 "document exact recheck rows were not consumed") &&
         Require(ContainsPrefix(result.evidence, "document_array_expansion_rows="),
                 "array expansion evidence missing") &&
         Require(ContainsPrefix(result.evidence, "document_wildcard_fanout="),
                 "wildcard fanout evidence missing");
}

bool GraphConsumesFrontierAdjacencyAndVisitedMetrics() {
  const auto result = opt::EstimateEnterpriseGraphDocumentCost(
      Request(opt::EnterpriseGraphDocumentProfile::kGraphSeed));
  if (!Require(result.accepted, "graph costing was refused")) return false;
  return Require(result.selectable, "graph costing was not selectable") &&
         Require(ContainsPrefix(result.evidence, "graph_frontier_width="),
                 "frontier evidence missing") &&
         Require(ContainsPrefix(result.evidence, "graph_adjacency_degree="),
                 "adjacency evidence missing") &&
         Require(ContainsPrefix(result.evidence, "graph_visited_bitmap_density_ppm="),
                 "visited bitmap evidence missing");
}

bool MissingDocumentWildcardProofFailsClosed() {
  auto request = Request(opt::EnterpriseGraphDocumentProfile::kDocumentPath);
  request.metric.path_wildcard_proof_present = false;
  const auto result = opt::EstimateEnterpriseGraphDocumentCost(request);
  return Require(!result.accepted, "document wildcard without proof was accepted") &&
         Require(result.diagnostic_code == "SB_OPT_GRAPH_DOCUMENT_METRIC_REQUIRED",
                 "document proof diagnostic changed");
}

bool MissingGraphFrontierProofFailsClosed() {
  auto request = Request(opt::EnterpriseGraphDocumentProfile::kGraphSeed);
  request.metric.graph_frontier_proof_present = false;
  const auto result = opt::EstimateEnterpriseGraphDocumentCost(request);
  return Require(!result.accepted, "graph frontier without proof was accepted") &&
         Require(result.diagnostic_code == "SB_OPT_GRAPH_DOCUMENT_METRIC_REQUIRED",
                 "graph proof diagnostic changed");
}

bool UnsafeMetricAuthorityFailsClosed() {
  auto request = Request(opt::EnterpriseGraphDocumentProfile::kGraphSeed);
  request.metric.cluster_route_or_metric_projection = true;
  const auto result = opt::EstimateEnterpriseGraphDocumentCost(request);
  return Require(!result.accepted, "cluster graph metric was accepted") &&
         Require(result.diagnostic_code == "SB_OPT_GRAPH_DOCUMENT_METRIC_REQUIRED",
                 "unsafe graph metric diagnostic changed");
}

}  // namespace

int main() {
  // SEARCH_KEY: OEIC_GRAPH_DOCUMENT_ROUTE_COSTING
  if (!DocumentConsumesShapeWildcardAndArrayMetrics()) return EXIT_FAILURE;
  if (!GraphConsumesFrontierAdjacencyAndVisitedMetrics()) return EXIT_FAILURE;
  if (!MissingDocumentWildcardProofFailsClosed()) return EXIT_FAILURE;
  if (!MissingGraphFrontierProofFailsClosed()) return EXIT_FAILURE;
  if (!UnsafeMetricAuthorityFailsClosed()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
