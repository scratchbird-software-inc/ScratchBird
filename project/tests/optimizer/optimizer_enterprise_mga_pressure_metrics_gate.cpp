// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_registry.hpp"
#include "optimizer_metric_manifest.hpp"
#include "optimizer_mga_pressure_metrics.hpp"
#include "../support/metric_projection_fixture.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace metrics = scratchbird::core::metrics;
namespace opt = scratchbird::engine::optimizer;
namespace mga = scratchbird::transaction::mga;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "optimizer_enterprise_mga_pressure_metrics_gate: " << message
              << '\n';
    std::exit(1);
  }
}

mga::OptimizerMgaPressureAuthority GoodAuthority() {
  mga::OptimizerMgaPressureAuthority authority;
  authority.transaction_inventory_authoritative = true;
  authority.cleanup_horizon_authoritative = true;
  authority.row_version_runtime_authoritative = true;
  authority.engine_scope_bound = true;
  return authority;
}

mga::OptimizerMgaPressureSample GoodSample() {
  mga::OptimizerMgaPressureSample sample;
  sample.scope_uuid = scratchbird::tests::FixtureUuid(1268, 1);
  sample.database_uuid = scratchbird::tests::FixtureUuid(1268, 2);
  sample.node_uuid = scratchbird::tests::FixtureUuid(1268, 3);
  sample.route_label = "embedded";
  sample.relation_uuid = scratchbird::tests::FixtureUuid(1268, 4);
  sample.page_class = "data";
  sample.evidence_digest = "mga-digest-1";
  sample.source_generation = 11;
  sample.cleanup_debt_bytes = 16 * 1024;
  sample.retained_dead_bytes = 64 * 1024;
  sample.chain_depth_bucket = 4;
  sample.chain_scatter_bucket = 2;
  sample.same_page_update_ratio = 0.42;
  sample.commit_fence_backlog = 3;
  sample.authoritative_cleanup_horizon_local_transaction_id = 100;
  sample.retained_row_versions = 17;
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

void AdmitMgaSeries(scratchbird::tests::MetricProjectionFixture& fixture) {
  const auto sample = GoodSample();
  for (const auto& definition : mga::OptimizerMgaPressureMetricDescriptorDefinitions()) {
    fixture.AdmitDefinition(definition, {{"scope_uuid", sample.scope_uuid}, {"route_label", sample.route_label},
        {"metric_family", definition.family.substr(std::string("sb_optimizer_").size())},
        {"source_generation", "11"}, {"evidence_digest", sample.evidence_digest}});
  }
  for (const auto* family : {"sb_mga_cleanup_horizon_local_transaction_id", "sb_mga_cleanup_retained_row_versions"})
    fixture.Admit(family, {{"component", "transaction.mga.cleanup"}, {"authority", "local_inventory"}});
}

void TestMgaPressurePublication(scratchbird::tests::MetricProjectionFixture& fixture) {
  // SEARCH_KEY: OEIC_MGA_PRESSURE_OPTIMIZER_METRICS
  Require(mga::EnsureOptimizerMgaPressureMetricDescriptors().ok,
          "MGA pressure descriptors failed");

  auto result = mga::PublishOptimizerMgaPressureMetrics(GoodSample());
  if (!result.ok) {
    std::cerr << result.diagnostic_code << ": " << result.detail << '\n';
    for (const auto& metric_result : result.metric_results) {
      if (!metric_result.ok) {
        std::cerr << "metric: " << metric_result.diagnostic_code << ": "
                  << metric_result.detail << '\n';
      }
    }
  }
  Require(result.ok, "valid MGA pressure metric sample was refused");
  fixture.ExpectProduced(8); fixture.Seal();
  Require(result.relation_uuid == GoodSample().relation_uuid, "binary relation identity lost");
  Require(result.diagnostic_code == "SB_OPTIMIZER_MGA_PRESSURE.OK",
          "unexpected MGA pressure diagnostic");

  const auto snapshot = metrics::DefaultMetricRegistry().SnapshotCurrent(false);
  Require(HasMetricValue(snapshot, "sb_optimizer_mga_cleanup_debt"),
          "MGA cleanup debt metric missing");
  Require(HasMetricValue(snapshot, "sb_optimizer_mga_retained_dead_bytes"),
          "MGA retained dead bytes metric missing");
  Require(HasMetricValue(snapshot, "sb_optimizer_mga_chain_depth"),
          "MGA chain depth metric missing");
  Require(HasMetricValue(snapshot, "sb_optimizer_mga_chain_scatter"),
          "MGA chain scatter metric missing");
  Require(HasMetricValue(snapshot, "sb_optimizer_same_page_update_ratio"),
          "same-page update ratio metric missing");
  Require(HasMetricValue(snapshot, "sb_optimizer_commit_fence_pressure"),
          "commit fence pressure metric missing");
  Require(HasMetricValue(snapshot, "sb_mga_cleanup_horizon_local_transaction_id"),
          "core MGA cleanup horizon metric missing");
  bool retained = false;
  for (const auto& value : snapshot) if (value.family == "sb_mga_cleanup_retained_row_versions") {
    retained = true;
    Require(std::holds_alternative<std::uint64_t>(value.value) && std::get<std::uint64_t>(value.value) == 17,
            "retained row count fabricated from chain depth");
  }
  Require(retained, "independent retained-row measurement missing");
}

void TestNativeMgaBounds(scratchbird::tests::MetricProjectionFixture& fixture) {
  for (const auto count : {std::uint64_t{0}, (std::uint64_t{1} << 53) + 1,
                           std::numeric_limits<std::uint64_t>::max()}) {
    auto sample = GoodSample();
    sample.cleanup_debt_bytes = sample.retained_dead_bytes = sample.chain_depth_bucket =
        sample.chain_scatter_bucket = sample.commit_fence_backlog = count;
    sample.retained_row_versions = sample.authoritative_cleanup_horizon_local_transaction_id = count;
    Require(mga::PublishOptimizerMgaPressureMetrics(sample).ok, "native MGA boundary refused");
    fixture.ExpectProduced(8); fixture.Seal();
    unsigned exact = 0;
    for (const auto& value : metrics::DefaultMetricRegistry().SnapshotCurrent(false)) {
      if (value.family == "sb_optimizer_same_page_update_ratio") continue;
      Require(std::holds_alternative<std::uint64_t>(value.value) && std::get<std::uint64_t>(value.value) == count,
              "MGA UINT64 measurement lost precision");
      ++exact;
    }
    Require(exact == 7, "MGA exact native observations missing");
  }
  for (unsigned mutation = 0; mutation != 7; ++mutation) {
    auto sample = GoodSample();
    if (mutation == 0) sample.same_page_update_ratio = std::numeric_limits<double>::quiet_NaN();
    if (mutation == 1) sample.same_page_update_ratio = std::numeric_limits<double>::infinity();
    if (mutation == 2) sample.retained_row_versions.reset();
    if (mutation == 3) sample.authoritative_cleanup_horizon_local_transaction_id.reset();
    if (mutation == 4) sample.relation_uuid.bytes[6] = 0x40;
    if (mutation == 5) sample.database_uuid = scratchbird::tests::FixtureUuid(1268, 99);
    if (mutation == 6) sample.node_uuid = {};
    const auto refused = mga::PublishOptimizerMgaPressureMetrics(sample);
    Require(!refused.ok && refused.metric_results.empty() && refused.relation_uuid == sample.relation_uuid,
            "invalid MGA sample published effects or lost binary receipt");
    fixture.VerifyReadOnly();
  }
  const auto* descriptor = metrics::DefaultMetricRegistry().FindDescriptor("sb_optimizer_mga_cleanup_debt");
  metrics::MetricLabelSet labels{{"scope_uuid", "019d0000-0000-7000-8000-000000000001"},
      {"route_label", "embedded"}, {"metric_family", "mga_cleanup_debt"}, {"source_generation", "11"}, {"evidence_digest", "mga-digest-1"}};
  Require(!metrics::DefaultMetricRegistry().ValidateLabels(*descriptor, labels).ok, "text MGA UUID accepted");
  labels[0].value = GoodSample().scope_uuid;
  Require(!metrics::DefaultMetricRegistry().SetGauge(descriptor->family, labels, 1.0, "transaction_mga_cleanup").ok,
          "FLOAT64 substituted for native MGA bytes");
  fixture.VerifyReadOnly();
}

void TestMgaPressureRefusals() {
  auto sample = GoodSample();
  sample.authority.parser_or_reference_authority = true;
  auto refused = mga::PublishOptimizerMgaPressureMetrics(sample);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_OPTIMIZER_MGA_PRESSURE.UNSAFE_AUTHORITY",
          "parser/reference authority was not refused");

  sample = GoodSample();
  sample.authority.cleanup_horizon_authoritative = false;
  refused = mga::PublishOptimizerMgaPressureMetrics(sample);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_OPTIMIZER_MGA_PRESSURE.MGA_AUTHORITY_REQUIRED",
          "missing cleanup-horizon authority was not refused");

  sample = GoodSample();
  sample.same_page_update_ratio = 1.5;
  refused = mga::PublishOptimizerMgaPressureMetrics(sample);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_OPTIMIZER_MGA_PRESSURE.RATIO_INVALID",
          "invalid same-page update ratio was not refused");
}

}  // namespace

int main() {
  metrics::MetricRegistry empty;
  Require(!mga::EnsureOptimizerMgaPressureMetricDescriptors(&empty).ok && empty.Descriptors().empty(),
          "MGA definition lookup invented metric identities");
  const auto sample = GoodSample();
  scratchbird::tests::MetricProjectionFixture fixture(sample.database_uuid, sample.node_uuid, 1269);
  AdmitMgaSeries(fixture); fixture.Seal();
  TestMgaPressurePublication(fixture);
  TestMgaPressureRefusals();
  fixture.VerifyReadOnly();
  TestNativeMgaBounds(fixture);
  fixture.VerifyAndDrain();
  std::cout << "optimizer enterprise MGA pressure metrics gate passed\n";
  return 0;
}
