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
#include "../support/metric_projection_fixture.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
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
  sample.scope_uuid = scratchbird::tests::FixtureUuid(1272, 1);
  sample.database_uuid = scratchbird::tests::FixtureUuid(1272, 2);
  sample.node_uuid = scratchbird::tests::FixtureUuid(1272, 3);
  sample.route_label = "embedded";
  sample.plan_node_id = "plan-node-specialized-1";
  sample.workload_family = "document_search_vector_graph_time_series";
  sample.provider_id = "local-specialized-provider";
  sample.index_generation = 91;
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

metrics::MetricLabelSet Labels(const opt::SpecializedWorkloadMetricSample& sample,
                               const std::string& family) {
  return {{"scope_uuid", sample.scope_uuid}, {"index_generation", std::to_string(sample.index_generation)},
      {"route_label", sample.route_label}, {"plan_node_id", sample.plan_node_id},
      {"metric_family", family.substr(std::string("sb_optimizer_").size())},
      {"source_generation", std::to_string(sample.source_generation)}, {"evidence_digest", sample.evidence_digest}};
}

void Admit(scratchbird::tests::MetricProjectionFixture& fixture, const opt::SpecializedWorkloadMetricSample& sample) {
  for (const auto& definition : opt::SpecializedWorkloadMetricDescriptorDefinitions())
    fixture.AdmitDefinition(definition, Labels(sample, definition.family));
}

void TestSpecializedMetricPublication(scratchbird::tests::MetricProjectionFixture& fixture) {
  // SEARCH_KEY: OEIC_SPECIALIZED_WORKLOAD_OPTIMIZER_METRICS
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
  if (result.ok) { fixture.ExpectProduced(14); fixture.Seal(); }
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

void TestNativeValues(scratchbird::tests::MetricProjectionFixture& fixture) {
  using Sample = opt::SpecializedWorkloadMetricSample;
  const std::vector<std::pair<std::string, std::optional<std::uint64_t> Sample::*>> counts{
      {"candidate_set_cardinality", &Sample::candidate_set_cardinality},
      {"specialized_exact_recheck_rows", &Sample::specialized_exact_recheck_rows},
      {"text_posting_length", &Sample::text_posting_length}, {"vector_rerank_count", &Sample::vector_rerank_count},
      {"graph_frontier_width", &Sample::graph_frontier_width}, {"graph_adjacency_degree", &Sample::graph_adjacency_degree},
      {"time_series_bucket_count", &Sample::time_series_bucket_count}};
  for (auto count : {std::uint64_t{0}, (std::uint64_t{1} << 53) + 1, std::numeric_limits<std::uint64_t>::max()}) {
    auto sample = GoodSample();
    for (const auto& [name, member] : counts) sample.*member = count;
    sample.text_blockmax_skips = 0;
    const auto result = opt::PublishSpecializedWorkloadMetrics(sample);
    Require(result.ok && result.scope_uuid == sample.scope_uuid && result.index_generation == sample.index_generation,
            "native specialized publication/receipt failed");
    fixture.ExpectProduced(14); fixture.Seal();
    for (const auto& [name, member] : counts) {
      bool found = false;
      for (const auto& value : metrics::DefaultMetricRegistry().SnapshotCurrent(false)) if (value.family == "sb_optimizer_" + name) {
        Require(std::holds_alternative<std::uint64_t>(value.value) && std::get<std::uint64_t>(value.value) == count,
                "native specialized count lost precision: " + name);
        found = true;
      }
      Require(found, "missing specialized count: " + name);
    }
  }
  auto sample = GoodSample();
  const auto delta = (std::uint64_t{1} << 53) + 1;
  sample.text_blockmax_skips = delta;
  Require(opt::PublishSpecializedWorkloadMetrics(sample).ok, "native specialized counter refused");
  fixture.ExpectProduced(14); fixture.Seal();
  for (const auto& value : metrics::DefaultMetricRegistry().SnapshotCurrent(false))
    if (value.family == "sb_optimizer_text_blockmax_skips")
      Require(std::holds_alternative<std::uint64_t>(value.value) && std::get<std::uint64_t>(value.value) == delta + 23,
              "specialized counter rounded or zero skipped");
  const auto refuse = [&](const Sample& bad) {
    const auto result = opt::PublishSpecializedWorkloadMetrics(bad);
    Require(!result.ok && result.metric_results.empty() && result.scope_uuid == bad.scope_uuid &&
                result.index_generation == bad.index_generation, "invalid sample was not refused before effects");
    fixture.VerifyReadOnly();
  };
  for (auto member : {&Sample::candidate_set_density, &Sample::candidate_set_recheck_ratio,
                      &Sample::specialized_false_positive_ratio, &Sample::document_path_selectivity,
                      &Sample::vector_recall_observed, &Sample::time_series_rollup_selectivity})
    for (auto invalid : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -0.01, 1.01}) {
      auto bad = GoodSample(); bad.*member = invalid; refuse(bad);
    }
  for (auto member : {&Sample::scope_uuid, &Sample::database_uuid, &Sample::node_uuid}) {
    auto bad = GoodSample(); bad.*member = {}; refuse(bad);
  }
  auto bad = GoodSample(); bad.database_uuid = scratchbird::tests::FixtureUuid(1272, 99); refuse(bad);
  bad = GoodSample(); bad.node_uuid = scratchbird::tests::FixtureUuid(1272, 99); refuse(bad);
  bad = GoodSample(); bad.index_generation = 0; refuse(bad);
  bad = GoodSample(); bad.source_generation = 0; refuse(bad);

  auto& registry = metrics::DefaultMetricRegistry();
  const std::string family = "sb_optimizer_candidate_set_cardinality";
  auto labels = Labels(GoodSample(), family);
  Require(!registry.SetGauge(family, labels, 1.0, "candidate_set_runtime").ok, "floating specialized count accepted");
  labels[0].value = "019d0000-0000-7000-8000-000000000001";
  Require(!registry.ValidateLabels(*registry.FindDescriptor(family), labels).ok, "text specialized UUID accepted");
  fixture.VerifyReadOnly();

  sample = GoodSample(); sample.index_generation = 92; sample.candidate_set_cardinality = 42;
  Admit(fixture, sample);
  Require(opt::PublishSpecializedWorkloadMetrics(sample).ok, "separate specialized generation refused");
  fixture.ExpectProduced(14); fixture.Seal();
  std::size_t matched = 0;
  for (const auto& value : registry.SnapshotCurrent(false)) if (value.family == family) {
    std::string generation;
    for (const auto& label : value.labels) if (label.key == "index_generation") generation = std::get<std::string>(label.value);
    Require(std::get<std::uint64_t>(value.value) == (generation == "92" ? 42 : 1024), "specialized generations conflated");
    ++matched;
  }
  Require(matched == 2, "missing specialized generation series");
  sample.scope_uuid = scratchbird::tests::FixtureUuid(1272, 98);
  const auto result = opt::PublishSpecializedWorkloadMetrics(sample);
  Require(!result.ok && result.metric_results.size() == 14 && result.diagnostic_code != "SB_OPTIMIZER_SPECIALIZED_METRICS.OK",
          "unadmitted specialized series reported success");
  for (const auto& item : result.metric_results) Require(!item.ok, "unexpected specialized series admitted");
  fixture.VerifyReadOnly();
}

}  // namespace

int main() {
  metrics::MetricRegistry empty;
  Require(!opt::EnsureSpecializedWorkloadMetricDescriptors(&empty).ok &&
              !empty.FindDescriptor("sb_optimizer_candidate_set_cardinality"), "verification fabricated metric admission");
  const auto sample = GoodSample();
  scratchbird::tests::MetricProjectionFixture fixture(sample.database_uuid, sample.node_uuid, 1273);
  Admit(fixture, sample); fixture.Seal();
  TestSpecializedMetricPublication(fixture);
  TestSpecializedMetricRefusals();
  fixture.VerifyReadOnly();
  TestNativeValues(fixture);
  fixture.VerifyAndDrain();
  std::cout << "optimizer enterprise specialized metrics gate passed\n";
  return 0;
}
