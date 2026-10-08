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
#include "specialized_workload_metrics.hpp"
#include "metric_bound_definition.hpp"
#include "../support/metric_projection_fixture.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
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
  sample.scope_uuid = scratchbird::tests::FixtureUuid(1270, 1);
  sample.database_uuid = scratchbird::tests::FixtureUuid(1270, 2);
  sample.node_uuid = scratchbird::tests::FixtureUuid(1270, 3);
  sample.route_label = "embedded";
  sample.plan_node_id = "plan-node-7";
  sample.index_uuid = scratchbird::tests::FixtureUuid(1270, 4);
  sample.index_family = "mixed-noncluster-index-family";
  sample.index_generation = 42;
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

void AdmitIndexSeries(scratchbird::tests::MetricProjectionFixture& fixture,
                      const idx::IndexOptimizerRuntimeMetricSample& sample) {
  for (const auto& definition : idx::IndexOptimizerRuntimeMetricDescriptorDefinitions())
    fixture.AdmitDefinition(definition, {{"scope_uuid", sample.scope_uuid}, {"index_uuid", sample.index_uuid},
        {"index_generation", std::to_string(sample.index_generation)}, {"route_label", sample.route_label},
        {"plan_node_id", sample.plan_node_id}, {"metric_family", definition.family.substr(std::string("sb_optimizer_").size())},
        {"source_generation", std::to_string(sample.source_generation)}, {"evidence_digest", sample.evidence_digest}});
}

void TestIndexMetricPublication(scratchbird::tests::MetricProjectionFixture& fixture) {
  // SEARCH_KEY: OEIC_INDEX_FAMILY_OPTIMIZER_METRICS
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
  if (result.ok) { fixture.ExpectProduced(19); fixture.Seal(); }
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

void TestNativeValues(scratchbird::tests::MetricProjectionFixture& fixture) {
  using Sample = idx::IndexOptimizerRuntimeMetricSample;
  const std::vector<std::pair<std::string, std::optional<std::uint64_t> Sample::*>> counts{
      {"index_backlog_entries", &Sample::index_backlog_entries},
      {"btree_depth", &Sample::btree_depth}, {"btree_leaf_pages", &Sample::btree_leaf_pages},
      {"hash_collision_depth", &Sample::hash_collision_depth},
      {"hash_overflow_depth", &Sample::hash_overflow_depth},
      {"text_posting_length", &Sample::text_posting_length},
      {"vector_rerank_count", &Sample::vector_rerank_count},
      {"graph_frontier_width", &Sample::graph_frontier_width},
      {"graph_adjacency_degree", &Sample::graph_adjacency_degree}};
  for (const auto count : {std::uint64_t{0}, (std::uint64_t{1} << 53) + 1,
                           std::numeric_limits<std::uint64_t>::max()}) {
    auto sample = GoodSample();
    for (const auto& [name, member] : counts) sample.*member = count;
    sample.index_recheck_count = 0;
    sample.text_blockmax_skips = 0;
    const auto result = idx::PublishIndexOptimizerRuntimeMetrics(sample);
    Require(result.ok && result.index_uuid == sample.index_uuid &&
                result.index_generation == sample.index_generation, "native index receipt invalid");
    fixture.ExpectProduced(19); fixture.Seal();
    const auto snapshot = metrics::DefaultMetricRegistry().SnapshotCurrent(false);
    for (const auto& [name, member] : counts) {
      bool found = false;
      for (const auto& value : snapshot) if (value.family == "sb_optimizer_" + name) {
        Require(std::holds_alternative<std::uint64_t>(value.value) &&
                    std::get<std::uint64_t>(value.value) == count, "index count lost precision: " + name);
        found = true;
      }
      Require(found, "missing native index count: " + name);
    }
  }
  auto sample = GoodSample();
  const auto delta = (std::uint64_t{1} << 53) + 1;
  sample.index_recheck_count = delta; sample.text_blockmax_skips = delta;
  Require(idx::PublishIndexOptimizerRuntimeMetrics(sample).ok, "native counter delta refused");
  fixture.ExpectProduced(19); fixture.Seal();
  for (const auto& value : metrics::DefaultMetricRegistry().SnapshotCurrent(false)) {
    if (value.family == "sb_optimizer_index_recheck_count" || value.family == "sb_optimizer_text_blockmax_skips")
      Require(std::holds_alternative<std::uint64_t>(value.value) &&
                  std::get<std::uint64_t>(value.value) == delta +
                      (value.family == "sb_optimizer_index_recheck_count" ? 11 : 19), "counter rounded or zero skipped");
  }
  const auto refuse = [&](const Sample& bad) {
    const auto result = idx::PublishIndexOptimizerRuntimeMetrics(bad);
    Require(!result.ok && result.metric_results.empty() && result.index_uuid == bad.index_uuid &&
                result.index_generation == bad.index_generation, "invalid input did not refuse before effects");
    fixture.VerifyReadOnly();
  };
  for (auto member : {&Sample::index_selectivity, &Sample::index_false_positive_ratio,
                      &Sample::bitmap_density, &Sample::bloom_observed_fpr, &Sample::zone_prune_selectivity,
                      &Sample::vector_recall_observed, &Sample::vector_tombstone_ratio, &Sample::document_path_selectivity}) {
    for (const auto invalid : {std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity(), -0.01, 1.01}) {
      auto bad = GoodSample(); bad.*member = invalid; refuse(bad);
    }
  }
  for (auto member : {&Sample::scope_uuid, &Sample::index_uuid, &Sample::database_uuid, &Sample::node_uuid}) {
    auto bad = GoodSample(); bad.*member = {}; refuse(bad);
  }
  auto bad = GoodSample(); bad.database_uuid = scratchbird::tests::FixtureUuid(1270, 99); refuse(bad);
  bad = GoodSample(); bad.node_uuid = scratchbird::tests::FixtureUuid(1270, 99); refuse(bad);
  bad = GoodSample(); bad.index_generation = 0; refuse(bad);
  bad = GoodSample(); bad.source_generation = 0; refuse(bad);

  auto& registry = metrics::DefaultMetricRegistry();
  const auto descriptor = registry.FindDescriptor("sb_optimizer_btree_depth");
  Require(descriptor != nullptr, "index definition missing");
  metrics::MetricLabelSet labels{{"scope_uuid", GoodSample().scope_uuid}, {"index_uuid", GoodSample().index_uuid},
      {"index_generation", "42"}, {"route_label", "embedded"}, {"plan_node_id", "plan-node-7"},
      {"metric_family", "btree_depth"}, {"source_generation", "42"}, {"evidence_digest", "index-evidence-digest-1"}};
  Require(!registry.SetGauge(descriptor->family, labels, 1.0, "index_runtime").ok, "floating index count accepted");
  labels[1].value = "019d0000-0000-7000-8000-000000000001";
  Require(!registry.ValidateLabels(*descriptor, labels).ok, "text index UUID accepted");
  fixture.VerifyReadOnly();
}

void TestSeriesIsolation(scratchbird::tests::MetricProjectionFixture& fixture) {
  for (bool new_index : {true, false}) {
    auto sample = GoodSample();
    if (new_index) sample.index_uuid = scratchbird::tests::FixtureUuid(1270, 5);
    else sample.index_generation = 43;
    sample.btree_leaf_pages = new_index ? 999 : 123;
    AdmitIndexSeries(fixture, sample);
    Require(idx::PublishIndexOptimizerRuntimeMetrics(sample).ok, "separate index series refused");
    fixture.ExpectProduced(19); fixture.Seal();
  }
  std::size_t matched = 0;
  for (const auto& value : metrics::DefaultMetricRegistry().SnapshotCurrent(false)) {
    if (value.family != "sb_optimizer_btree_leaf_pages") continue;
    metrics::MetricUuid index;
    std::string generation;
    for (const auto& label : value.labels) {
      if (label.key == "index_uuid") index = std::get<metrics::MetricUuid>(label.value);
      if (label.key == "index_generation") generation = std::get<std::string>(label.value);
    }
    const auto expected = index == scratchbird::tests::FixtureUuid(1270, 5) ? 999 : generation == "43" ? 123 : 64;
    Require(std::holds_alternative<std::uint64_t>(value.value) &&
                std::get<std::uint64_t>(value.value) == static_cast<std::uint64_t>(expected), "index identity/generation conflated");
    ++matched;
  }
  Require(matched == 3, "index generations collapsed to one series");
  auto sample = GoodSample(); sample.btree_leaf_pages.reset();
  Require(idx::PublishIndexOptimizerRuntimeMetrics(sample).ok, "optional index count refused");
  fixture.ExpectProduced(18); fixture.Seal();
  sample.scope_uuid = scratchbird::tests::FixtureUuid(1270, 98);
  const auto refused = idx::PublishIndexOptimizerRuntimeMetrics(sample);
  Require(!refused.ok && refused.diagnostic_code != "SB_OPTIMIZER_INDEX_METRICS.OK" &&
              refused.metric_results.size() == 18, "unadmitted series reported successful publication");
  for (const auto& result : refused.metric_results) Require(!result.ok, "unexpected admitted series");
  fixture.VerifyReadOnly();
}

}  // namespace

int main() {
  metrics::MetricRegistry empty;
  Require(!idx::EnsureIndexOptimizerRuntimeMetricDescriptors(&empty).ok &&
              !empty.FindDescriptor("sb_optimizer_btree_depth"), "descriptor verification fabricated admission");
  const auto sample = GoodSample();
  scratchbird::tests::MetricProjectionFixture fixture(sample.database_uuid, sample.node_uuid, 1271);
  AdmitIndexSeries(fixture, sample); fixture.Seal();
  for (const auto& definition : opt::SpecializedWorkloadMetricDescriptorDefinitions())
    if (metrics::DefaultMetricRegistry().FindDescriptor(definition.family))
      Require(metrics::ValidateBoundMetricDefinition(metrics::DefaultMetricRegistry(), definition).ok,
              "shared specialized/index definition mismatch: " + definition.family);
  TestIndexMetricPublication(fixture);
  TestIndexMetricRefusals();
  fixture.VerifyReadOnly();
  TestNativeValues(fixture);
  TestSeriesIsolation(fixture);
  fixture.VerifyAndDrain();
  std::cout << "optimizer enterprise index metrics gate passed\n";
  return 0;
}
