// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_registry.hpp"
#include "optimizer_metric_manifest.hpp"
#include "specialized_workload_metrics.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace metrics = scratchbird::core::metrics;
namespace opt = scratchbird::engine::optimizer;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "optimizer_enterprise_specialized_metrics_gate: " << message
              << '\n';
    std::exit(1);
  }
}

opt::SpecializedWorkloadMetricAuthority GoodAuthority() {
  opt::SpecializedWorkloadMetricAuthority authority;
  authority.provider_contract_authoritative = true;
  authority.route_runtime_authoritative = true;
  authority.descriptor_visibility_proof_present = true;
  authority.index_generation_proof_present = true;
  authority.engine_scope_bound = true;
  authority.exact_recheck_preserved = true;
  authority.mga_recheck_preserved = true;
  authority.security_recheck_preserved = true;
  authority.candidate_set_runtime_authoritative = true;
  authority.document_runtime_authoritative = true;
  authority.search_runtime_authoritative = true;
  authority.vector_runtime_authoritative = true;
  authority.graph_runtime_authoritative = true;
  authority.time_series_runtime_authoritative = true;
  return authority;
}

opt::SpecializedWorkloadMetricSample GoodSample() {
  opt::SpecializedWorkloadMetricSample sample;
  sample.scope_uuid = "database-scope-1";
  sample.route_label = "embedded";
  sample.plan_node_id = "plan-node-specialized-1";
  sample.workload_family = "document_search_vector_graph_time_series";
  sample.provider_id = "local-specialized-provider";
  sample.index_generation = "index-generation-91";
  sample.result_contract_hash = "result-contract-specialized-1";
  sample.evidence_digest = "specialized-evidence-digest-1";
  sample.source_generation = 91;
  sample.candidate_set_cardinality = 1024;
  sample.candidate_set_density = 0.25;
  sample.candidate_set_recheck_ratio = 0.75;
  sample.specialized_exact_recheck_rows = 768;
  sample.specialized_false_positive_ratio = 0.04;
  sample.document_path_selectivity = 0.12;
  sample.text_posting_length = 4096;
  sample.text_blockmax_skips = 23;
  sample.vector_recall_observed = 0.97;
  sample.vector_rerank_count = 128;
  sample.graph_frontier_width = 44;
  sample.graph_adjacency_degree = 9;
  sample.time_series_bucket_count = 16;
  sample.time_series_rollup_selectivity = 0.31;
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
      Require(entry.benchmark_clean_consumable,
              "manifest metric is not benchmark-clean consumable: " +
                  metric_family);
      return;
    }
  }
  Require(false, "manifest metric missing: " + metric_family);
}

void TestSpecializedMetricPublication() {
  // SEARCH_KEY: OEIC_SPECIALIZED_WORKLOAD_OPTIMIZER_METRICS
  Require(opt::EnsureOptimizerEnterpriseMetricDescriptors().ok,
          "optimizer descriptors failed");
  Require(opt::EnsureSpecializedWorkloadMetricDescriptors().ok,
          "specialized workload descriptors failed");

  const std::vector<std::string> manifest_families = {
      "candidate_set_cardinality",
      "candidate_set_density",
      "candidate_set_recheck_ratio",
      "specialized_exact_recheck_rows",
      "specialized_false_positive_ratio",
      "document_path_selectivity",
      "text_posting_length",
      "text_blockmax_skips",
      "vector_recall_observed",
      "vector_rerank_count",
      "graph_frontier_width",
      "graph_adjacency_degree",
      "time_series_bucket_count",
      "time_series_rollup_selectivity"};
  for (const auto& family : manifest_families) {
    RequireManifestLive(family);
  }

  auto result = opt::PublishSpecializedWorkloadMetrics(GoodSample());
  if (!result.ok) {
    std::cerr << result.diagnostic_code << ": " << result.detail << '\n';
    for (const auto& metric_result : result.metric_results) {
      if (!metric_result.ok) {
        std::cerr << "metric: " << metric_result.diagnostic_code << ": "
                  << metric_result.detail << '\n';
      }
    }
  }
  Require(result.ok, "valid specialized workload metric sample was refused");
  Require(result.diagnostic_code == "SB_OPTIMIZER_SPECIALIZED_METRICS.OK",
          "unexpected specialized metric diagnostic");

  const auto snapshot = metrics::DefaultMetricRegistry().SnapshotCurrent(false);
  const std::vector<std::string> registry_families = {
      "sb_optimizer_candidate_set_cardinality",
      "sb_optimizer_candidate_set_density",
      "sb_optimizer_candidate_set_recheck_ratio",
      "sb_optimizer_specialized_exact_recheck_rows",
      "sb_optimizer_specialized_false_positive_ratio",
      "sb_optimizer_document_path_selectivity",
      "sb_optimizer_text_posting_length",
      "sb_optimizer_text_blockmax_skips",
      "sb_optimizer_vector_recall_observed",
      "sb_optimizer_vector_rerank_count",
      "sb_optimizer_graph_frontier_width",
      "sb_optimizer_graph_adjacency_degree",
      "sb_optimizer_time_series_bucket_count",
      "sb_optimizer_time_series_rollup_selectivity"};
  for (const auto& family : registry_families) {
    Require(HasMetricValue(snapshot, family),
            "specialized optimizer metric missing: " + family);
  }
}

void TestSpecializedMetricRefusals() {
  auto sample = GoodSample();
  sample.authority.parser_or_reference_authority = true;
  auto refused = opt::PublishSpecializedWorkloadMetrics(sample);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_OPTIMIZER_SPECIALIZED_METRICS.UNSAFE_AUTHORITY",
          "parser/reference specialized authority was not refused");

  sample = GoodSample();
  sample.authority.exact_recheck_preserved = false;
  refused = opt::PublishSpecializedWorkloadMetrics(sample);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_OPTIMIZER_SPECIALIZED_METRICS.ROUTE_AUTHORITY_REQUIRED",
          "missing exact recheck proof was not refused");

  sample = GoodSample();
  sample.authority.candidate_set_runtime_authoritative = false;
  refused = opt::PublishSpecializedWorkloadMetrics(sample);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_OPTIMIZER_SPECIALIZED_METRICS.CANDIDATE_SET_AUTHORITY_REQUIRED",
          "missing candidate-set authority was not refused");

  sample = GoodSample();
  sample.time_series_rollup_selectivity = 1.5;
  refused = opt::PublishSpecializedWorkloadMetrics(sample);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_OPTIMIZER_SPECIALIZED_METRICS.RATIO_INVALID",
          "invalid specialized ratio was not refused");
}

}  // namespace

int main() {
  TestSpecializedMetricPublication();
  TestSpecializedMetricRefusals();
  std::cout << "optimizer enterprise specialized metrics gate passed\n";
  return 0;
}
