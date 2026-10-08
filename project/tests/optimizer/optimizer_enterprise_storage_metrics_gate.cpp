#include "../support/binary_uuid_fixture.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_registry.hpp"
#include "optimizer_metric_manifest.hpp"
#include "optimizer_storage_metrics.hpp"
#include "../support/metric_projection_fixture.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace metrics = scratchbird::core::metrics;
namespace opt = scratchbird::engine::optimizer;
namespace page = scratchbird::storage::page;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "optimizer_enterprise_storage_metrics_gate: " << message
              << '\n';
    std::exit(1);
  }
}

page::OptimizerStorageMetricAuthority GoodAuthority() {
  page::OptimizerStorageMetricAuthority authority;
  authority.storage_page_manager_authoritative = true;
  authority.filespace_identity_authoritative = true;
  authority.engine_scope_bound = true;
  return authority;
}

page::OptimizerStorageMetricSample GoodSample() {
  page::OptimizerStorageMetricSample sample;
  sample.scope_uuid = scratchbird::tests::FixtureUuid(1266, 1);
  sample.database_uuid = scratchbird::tests::FixtureUuid(1266, 2);
  sample.filespace_uuid = scratchbird::tests::FixtureUuid(1266, 3);
  sample.node_uuid = scratchbird::tests::FixtureUuid(1266, 4);
  sample.route_label = "embedded";
  sample.page_family = "data";
  sample.page_class = "heap";
  sample.device_profile = "ssd-local";
  sample.evidence_digest = "storage-digest-1";
  sample.source_generation = 9;
  sample.page_count = 100;
  sample.resident_pages = 40;
  sample.resident_bytes = 40ull * 32768ull;
  sample.pinned_pages = 3;
  sample.dirty_pages = 4;
  sample.writeback_pages = 2;
  sample.cache_hits = 32;
  sample.cache_misses = 8;
  sample.prefetch_considered = 12;
  sample.prefetch_scheduled = 10;
  sample.prefetch_used = 8;
  sample.prefetch_wasted = 2;
  sample.sequential_read_latency_microseconds = 100;
  sample.random_read_latency_microseconds = 700;
  sample.filespace_total_bytes = 1024ull * 1024ull * 1024ull;
  sample.filespace_used_bytes = 512ull * 1024ull * 1024ull;
  sample.filespace_free_bytes = 512ull * 1024ull * 1024ull;
  sample.filespace_reserved_bytes = 64ull * 1024ull * 1024ull;
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

void AdmitStorageSeries(scratchbird::tests::MetricProjectionFixture& fixture) {
  const auto sample = GoodSample();
  for (const auto& definition : page::OptimizerStorageMetricDescriptorDefinitions()) {
    const auto family = definition.family.substr(std::string("sb_optimizer_").size());
    metrics::MetricLabelSet labels{{"scope_uuid", sample.scope_uuid}, {"route_label", sample.route_label},
        {"metric_family", family}, {"source_generation", "9"}, {"evidence_digest", sample.evidence_digest}};
    if (family == "page_cache_hit_miss") {
      auto hit = labels; hit.push_back({"result", "hit"});
      fixture.AdmitDefinition(definition, std::move(hit));
      labels.push_back({"result", "miss"});
    }
    fixture.AdmitDefinition(definition, std::move(labels));
  }
  for (const auto* family : {"sb_page_cache_resident_pages", "sb_page_cache_resident_bytes",
                            "sb_page_cache_pinned_pages", "sb_page_cache_dirty_pages"})
    fixture.Admit(family, {{"component", "storage.page_cache"}, {"database_uuid", sample.database_uuid},
        {"filespace_uuid", sample.filespace_uuid}, {"page_family", sample.page_family}});
  for (const auto* family : {"sb_filespace_total_bytes", "sb_filespace_used_bytes", "sb_filespace_free_bytes",
                            "sb_filespace_reserved_bytes", "sb_filespace_device_read_latency_microseconds"}) {
    metrics::MetricLabelSet labels{{"component", "storage.filespace"}, {"database_uuid", sample.database_uuid},
        {"filespace_uuid", sample.filespace_uuid}, {"node_uuid", sample.node_uuid},
        {"filespace_role", "primary"}, {"device_class", sample.device_profile}};
    if (std::string_view(family) == "sb_filespace_reserved_bytes") labels.push_back({"reason_class", "optimizer_costing"});
    fixture.Admit(family, std::move(labels));
  }
}

void TestStorageMetricPublication(scratchbird::tests::MetricProjectionFixture& fixture) {
  // SEARCH_KEY: OEIC_STORAGE_IO_OPTIMIZER_METRICS
  Require(page::EnsureOptimizerStorageMetricDescriptors().ok,
          "storage metric descriptors failed");

  auto result = page::PublishOptimizerStorageMetrics(GoodSample());
  if (!result.ok) {
    std::cerr << result.diagnostic_code << ": " << result.detail << '\n';
    for (const auto& metric_result : result.metric_results) {
      if (!metric_result.ok) {
        std::cerr << "metric: " << metric_result.diagnostic_code << ": "
                  << metric_result.detail << '\n';
      }
    }
  }
  Require(result.ok, "valid storage optimizer metric sample was refused");
  fixture.ExpectProduced(19);
  fixture.Seal();
  Require(result.diagnostic_code == "SB_OPTIMIZER_STORAGE_METRICS.OK",
          "unexpected storage metric diagnostic");
  Require(result.filespace_uuid == GoodSample().filespace_uuid,
          "success receipt lost its binary filespace identity");

  const auto snapshot = metrics::DefaultMetricRegistry().SnapshotCurrent(false);
  Require(HasMetricValue(snapshot, "sb_optimizer_page_cache_hit_miss"),
          "page cache hit/miss optimizer metric missing");
  Require(HasMetricValue(snapshot, "sb_optimizer_prefetch_usefulness"),
          "prefetch usefulness optimizer metric missing");
  Require(HasMetricValue(snapshot, "sb_optimizer_page_count"),
          "page count optimizer metric missing");
  Require(HasMetricValue(snapshot, "sb_optimizer_page_cache_dirty_pressure"),
          "dirty pressure optimizer metric missing");
  Require(HasMetricValue(snapshot, "sb_optimizer_page_cache_pin_pressure"),
          "pin pressure optimizer metric missing");
  Require(HasMetricValue(snapshot, "sb_optimizer_writeback_pressure"),
          "writeback pressure optimizer metric missing");
  Require(HasMetricValue(snapshot, "sb_optimizer_sequential_page_cost"),
          "sequential page cost optimizer metric missing");
  Require(HasMetricValue(snapshot, "sb_optimizer_random_page_cost"),
          "random page cost optimizer metric missing");
  Require(HasMetricValue(snapshot, "sb_optimizer_filespace_pressure"),
          "filespace pressure optimizer metric missing");
  Require(HasMetricValue(snapshot, "sb_page_cache_resident_pages"),
          "core page-cache resident metric missing");
  Require(HasMetricValue(snapshot, "sb_filespace_free_bytes"),
          "core filespace free metric missing");
}

void TestNativeBoundsAndAdmission(scratchbird::tests::MetricProjectionFixture& fixture) {
  for (const auto count : {std::uint64_t{0}, (std::uint64_t{1} << 53) + 1,
                           std::numeric_limits<std::uint64_t>::max()}) {
    auto sample = GoodSample();
    sample.page_count = count;
    sample.resident_pages = sample.pinned_pages = sample.dirty_pages = sample.writeback_pages = 0;
    sample.resident_bytes = 0;
    // Cumulative counter bounds are tested separately from the gauge so each
    // accepted delta remains representable after prior observations.
    sample.cache_hits = sample.cache_misses = 0;
    Require(page::PublishOptimizerStorageMetrics(sample).ok, "native count boundary refused");
    fixture.ExpectProduced(19); fixture.Seal();
    bool found = false;
    for (const auto& value : metrics::DefaultMetricRegistry().SnapshotCurrent(false)) {
      if (value.family == "sb_optimizer_page_count") {
        found = true;
        Require(std::holds_alternative<std::uint64_t>(value.value) &&
                std::get<std::uint64_t>(value.value) == count, "native UINT64 count lost precision");
      }
    }
    Require(found, "count observation absent");
  }
  auto exact_counter = GoodSample();
  exact_counter.cache_hits = (std::uint64_t{1} << 53) + 1;
  Require(page::PublishOptimizerStorageMetrics(exact_counter).ok, "exact large counter delta refused");
  fixture.ExpectProduced(19); fixture.Seal();
  for (const auto& value : metrics::DefaultMetricRegistry().SnapshotCurrent(false)) {
    if (value.family != "sb_optimizer_page_cache_hit_miss") continue;
    for (const auto& label : value.labels) if (label.key == "result" && std::get<std::string>(label.value) == "hit")
      Require(std::get<std::uint64_t>(value.value) == (std::uint64_t{1} << 53) + 33,
              "counter delta passed through FLOAT64");
  }
  for (unsigned mutation = 0; mutation != 7; ++mutation) {
    auto invalid = GoodSample();
    if (mutation == 0) { invalid.filespace_total_bytes = 100; invalid.filespace_used_bytes = std::numeric_limits<std::uint64_t>::max(); invalid.filespace_free_bytes = 101; }
    if (mutation == 1) { invalid.filespace_total_bytes = std::numeric_limits<std::uint64_t>::max(); invalid.filespace_used_bytes = invalid.filespace_total_bytes; invalid.filespace_free_bytes = 0; invalid.filespace_reserved_bytes = 1; }
    if (mutation == 2) invalid.node_uuid = {};
    if (mutation == 3) invalid.database_uuid = scratchbird::tests::FixtureUuid(1266, 99);
    if (mutation == 4) invalid.resident_bytes.reset();
    if (mutation == 5) invalid.sequential_read_latency_microseconds = (std::uint64_t{1} << 53) + 1;
    if (mutation == 6) invalid.random_read_latency_microseconds = std::numeric_limits<std::uint64_t>::max();
    const auto refused = page::PublishOptimizerStorageMetrics(invalid);
    Require(!refused.ok && refused.metric_results.empty(), "invalid sample published partial observations");
    fixture.VerifyReadOnly();
  }
  const auto* descriptor = metrics::DefaultMetricRegistry().FindDescriptor("sb_optimizer_page_count");
  metrics::MetricLabelSet labels{{"scope_uuid", "019d0000-0000-7000-8000-000000000001"},
      {"route_label", "embedded"}, {"metric_family", "page_count"}, {"source_generation", "9"}, {"evidence_digest", "storage-digest-1"}};
  Require(!metrics::DefaultMetricRegistry().ValidateLabels(*descriptor, labels).ok, "text scope UUID accepted");
  labels[0].value = GoodSample().scope_uuid;
  Require(!metrics::DefaultMetricRegistry().SetGauge(descriptor->family, labels, 1.0, "storage_page").ok,
          "FLOAT64 substituted for native page count");
  fixture.VerifyReadOnly();
  auto exact_duration = GoodSample();
  exact_duration.sequential_read_latency_microseconds = std::uint64_t{1} << 63;
  Require(page::PublishOptimizerStorageMetrics(exact_duration).ok, "exact large FLOAT64 duration refused");
  fixture.ExpectProduced(19); fixture.Seal();
  metrics::MetricRegistry mismatched;
  for (const auto& definition : page::OptimizerStorageMetricDescriptorDefinitions()) {
    auto registered = *metrics::DefaultMetricRegistry().FindDescriptor(definition.family);
    if (definition.family == "sb_optimizer_page_count") registered.value_type = metrics::MetricScalarType::float64;
    Require(mismatched.RegisterDescriptor(std::move(registered)).ok, "malformed test registration");
  }
  Require(!page::EnsureOptimizerStorageMetricDescriptors(&mismatched).ok,
          "same-named descriptor with incompatible native value contract accepted");
  fixture.VerifyReadOnly();

  // Unadmitted optimizer series do not suppress independent, admitted core
  // observations. A failure receipt must retain those actual partial effects.
  auto unknown_scope = GoodSample();
  unknown_scope.scope_uuid = scratchbird::tests::FixtureUuid(1266, 100);
  const auto partial = page::PublishOptimizerStorageMetrics(unknown_scope);
  Require(!partial.ok && partial.filespace_uuid == unknown_scope.filespace_uuid &&
          partial.metric_results.size() == 14, "partial publication failure lost its receipt");
  for (unsigned i = 0; i != 14; ++i)
    Require(partial.metric_results[i].ok == (i >= 10), "partial publication effects misreported");
  fixture.ExpectProduced(9); fixture.Seal();
}

void TestStorageMetricRefusals() {
  auto sample = GoodSample();
  sample.authority.parser_or_reference_authority = true;
  auto refused = page::PublishOptimizerStorageMetrics(sample);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_OPTIMIZER_STORAGE_METRICS.UNSAFE_AUTHORITY",
          "parser/reference storage authority was not refused");

  sample = GoodSample();
  sample.authority.filespace_identity_authoritative = false;
  refused = page::PublishOptimizerStorageMetrics(sample);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_OPTIMIZER_STORAGE_METRICS.STORAGE_AUTHORITY_REQUIRED",
          "missing filespace authority was not refused");

  sample = GoodSample();
  sample.dirty_pages = sample.resident_pages + 1;
  refused = page::PublishOptimizerStorageMetrics(sample);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_OPTIMIZER_STORAGE_METRICS.COUNTERS_INCONSISTENT",
          "inconsistent page-cache counters were not refused");
}

}  // namespace

int main() {
  metrics::MetricRegistry empty;
  Require(!page::EnsureOptimizerStorageMetricDescriptors(&empty).ok && empty.Descriptors().empty(),
          "definition lookup invented catalog metric identities");
  const auto sample = GoodSample();
  scratchbird::tests::MetricProjectionFixture fixture(sample.database_uuid, sample.node_uuid, 1267);
  AdmitStorageSeries(fixture);
  fixture.Seal(); fixture.VerifyReadOnly();
  TestStorageMetricPublication(fixture);
  TestStorageMetricRefusals();
  fixture.VerifyReadOnly();
  TestNativeBoundsAndAdmission(fixture);
  fixture.VerifyAndDrain();
  std::cout << "optimizer enterprise storage metrics gate passed\n";
  return 0;
}
