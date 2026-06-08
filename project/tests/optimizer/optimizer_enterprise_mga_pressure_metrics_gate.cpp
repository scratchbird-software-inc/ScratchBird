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

#include <cstdlib>
#include <iostream>
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
  sample.scope_uuid = "database-scope-1";
  sample.route_label = "embedded";
  sample.relation_uuid = "relation-uuid-1";
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

void TestMgaPressurePublication() {
  // SEARCH_KEY: OEIC_MGA_PRESSURE_OPTIMIZER_METRICS
  Require(opt::EnsureOptimizerEnterpriseMetricDescriptors().ok,
          "optimizer descriptors failed");
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
}

void TestMgaPressureRefusals() {
  auto sample = GoodSample();
  sample.authority.parser_or_donor_authority = true;
  auto refused = mga::PublishOptimizerMgaPressureMetrics(sample);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_OPTIMIZER_MGA_PRESSURE.UNSAFE_AUTHORITY",
          "parser/donor authority was not refused");

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
  TestMgaPressurePublication();
  TestMgaPressureRefusals();
  std::cout << "optimizer enterprise MGA pressure metrics gate passed\n";
  return 0;
}
