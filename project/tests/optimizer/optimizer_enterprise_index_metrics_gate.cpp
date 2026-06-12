// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_optimizer_runtime_metrics.hpp"
#include "metric_registry.hpp"
#include "optimizer_metric_manifest.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace idx = scratchbird::core::index;
namespace metrics = scratchbird::core::metrics;
namespace opt = scratchbird::engine::optimizer;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "optimizer_enterprise_index_metrics_gate: " << message
              << '\n';
    std::exit(1);
  }
}

idx::IndexOptimizerRuntimeMetricAuthority GoodAuthority() {
  idx::IndexOptimizerRuntimeMetricAuthority authority;
  authority.index_descriptor_authoritative = true;
  authority.index_generation_authoritative = true;
  authority.family_provider_authoritative = true;
  authority.engine_scope_bound = true;
  authority.exact_recheck_preserved = true;
  authority.exact_rerank_preserved = true;
  authority.maintenance_runtime_authoritative = true;
  authority.candidate_set_runtime_authoritative = true;
  authority.search_runtime_authoritative = true;
  authority.vector_runtime_authoritative = true;
  authority.graph_runtime_authoritative = true;
  authority.document_path_runtime_authoritative = true;
  return authority;
}

idx::IndexOptimizerRuntimeMetricSample GoodSample() {
  idx::IndexOptimizerRuntimeMetricSample sample;
  sample.scope_uuid = "database-scope-1";
  sample.route_label = "embedded";
  sample.plan_node_id = "plan-node-7";
  sample.index_uuid = "index-uuid-1";
  sample.index_family = "mixed-noncluster-index-family";
  sample.index_generation = "index-generation-42";
  sample.evidence_digest = "index-evidence-digest-1";
  sample.source_generation = 42;
  sample.index_selectivity = 0.17;
  sample.index_false_positive_ratio = 0.03;
  sample.index_recheck_count = 11;
  sample.index_backlog_entries = 5;
  sample.btree_depth = 3;
  sample.btree_leaf_pages = 64;
  sample.hash_collision_depth = 2;
  sample.hash_overflow_depth = 1;
  sample.bitmap_density = 0.44;
  sample.bloom_observed_fpr = 0.02;
  sample.zone_prune_selectivity = 0.73;
  sample.text_posting_length = 118;
  sample.text_blockmax_skips = 19;
  sample.vector_recall_observed = 0.96;
  sample.vector_rerank_count = 40;
  sample.vector_tombstone_ratio = 0.08;
  sample.graph_frontier_width = 25;
  sample.graph_adjacency_degree = 7;
  sample.document_path_selectivity = 0.21;
  sample.authority = GoodAuthority();
  return sample;
}

bool HasMetricValue(const std::vector<metrics::MetricValue>& snapshot,
                    const std::string& family) {
  for (const auto& value : snapshot) {
    if (value.family == family) {
      return true;
    }
  }
  return false;
}

void RequireManifestLive(const std::string& metric_family) {
  for (const auto& entry : opt::OptimizerEnterpriseMetricManifest()) {
    if (entry.metric_family == metric_family) {
      Require(entry.producer_state ==
                  opt::OptimizerMetricProducerState::live_maintained,
              "manifest metric is not live-maintained: " + metric_family);
      Require(entry.enterprise_route_consumable,
              "manifest metric is not enterprise-route consumable: " +
                  metric_family);
      return;
    }
  }
  Require(false, "manifest metric missing: " + metric_family);
}

void TestIndexMetricPublication() {
  // SEARCH_KEY: OEIC_INDEX_FAMILY_OPTIMIZER_METRICS
  Require(opt::EnsureOptimizerEnterpriseMetricDescriptors().ok,
          "optimizer descriptors failed");
  Require(idx::EnsureIndexOptimizerRuntimeMetricDescriptors().ok,
          "index optimizer metric descriptors failed");

  const std::vector<std::string> manifest_families = {
      "index_selectivity",
      "index_false_positive_ratio",
      "index_recheck_count",
      "index_backlog_entries",
      "btree_depth",
      "btree_leaf_pages",
      "hash_collision_depth",
      "hash_overflow_depth",
      "bitmap_density",
      "bloom_observed_fpr",
      "zone_prune_selectivity",
      "text_posting_length",
      "text_blockmax_skips",
      "vector_recall_observed",
      "vector_rerank_count",
      "vector_tombstone_ratio",
      "graph_frontier_width",
      "graph_adjacency_degree",
      "document_path_selectivity"};
  for (const auto& family : manifest_families) {
    RequireManifestLive(family);
  }

  auto result = idx::PublishIndexOptimizerRuntimeMetrics(GoodSample());
  if (!result.ok) {
    std::cerr << result.diagnostic_code << ": " << result.detail << '\n';
    for (const auto& metric_result : result.metric_results) {
      if (!metric_result.ok) {
        std::cerr << "metric: " << metric_result.diagnostic_code << ": "
                  << metric_result.detail << '\n';
      }
    }
  }
  Require(result.ok, "valid index optimizer metric sample was refused");
  Require(result.diagnostic_code == "SB_OPTIMIZER_INDEX_METRICS.OK",
          "unexpected index metric diagnostic");

  const auto snapshot = metrics::DefaultMetricRegistry().SnapshotCurrent(false);
  const std::vector<std::string> registry_families = {
      "sb_optimizer_index_selectivity",
      "sb_optimizer_index_false_positive_ratio",
      "sb_optimizer_index_recheck_count",
      "sb_optimizer_index_backlog_entries",
      "sb_optimizer_btree_depth",
      "sb_optimizer_btree_leaf_pages",
      "sb_optimizer_hash_collision_depth",
      "sb_optimizer_hash_overflow_depth",
      "sb_optimizer_bitmap_density",
      "sb_optimizer_bloom_observed_fpr",
      "sb_optimizer_zone_prune_selectivity",
      "sb_optimizer_text_posting_length",
      "sb_optimizer_text_blockmax_skips",
      "sb_optimizer_vector_recall_observed",
      "sb_optimizer_vector_rerank_count",
      "sb_optimizer_vector_tombstone_ratio",
      "sb_optimizer_graph_frontier_width",
      "sb_optimizer_graph_adjacency_degree",
      "sb_optimizer_document_path_selectivity"};
  for (const auto& family : registry_families) {
    Require(HasMetricValue(snapshot, family),
            "optimizer index metric missing: " + family);
  }
}

void TestIndexMetricRefusals() {
  auto sample = GoodSample();
  sample.authority.parser_or_reference_authority = true;
  auto refused = idx::PublishIndexOptimizerRuntimeMetrics(sample);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_OPTIMIZER_INDEX_METRICS.UNSAFE_AUTHORITY",
          "parser/reference index authority was not refused");

  sample = GoodSample();
  sample.authority.exact_recheck_preserved = false;
  refused = idx::PublishIndexOptimizerRuntimeMetrics(sample);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_OPTIMIZER_INDEX_METRICS.INDEX_AUTHORITY_REQUIRED",
          "missing exact recheck proof was not refused");

  sample = GoodSample();
  sample.vector_recall_observed = 0.91;
  sample.authority.exact_rerank_preserved = false;
  refused = idx::PublishIndexOptimizerRuntimeMetrics(sample);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_OPTIMIZER_INDEX_METRICS.VECTOR_AUTHORITY_REQUIRED",
          "missing exact rerank proof was not refused");

  sample = GoodSample();
  sample.bloom_observed_fpr = 1.25;
  refused = idx::PublishIndexOptimizerRuntimeMetrics(sample);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_OPTIMIZER_INDEX_METRICS.RATIO_INVALID",
          "invalid ratio was not refused");
}

}  // namespace

int main() {
  TestIndexMetricPublication();
  TestIndexMetricRefusals();
  std::cout << "optimizer enterprise index metrics gate passed\n";
  return 0;
}
