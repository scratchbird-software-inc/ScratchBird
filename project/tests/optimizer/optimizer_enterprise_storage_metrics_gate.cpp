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

#include <cstdlib>
#include <iostream>
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
  sample.scope_uuid = "database-scope-1";
  sample.database_uuid = "database-uuid-1";
  sample.filespace_uuid = "filespace-uuid-1";
  sample.node_uuid = "local-node";
  sample.route_label = "embedded";
  sample.page_family = "data";
  sample.page_class = "heap";
  sample.device_profile = "ssd-local";
  sample.evidence_digest = "storage-digest-1";
  sample.source_generation = 9;
  sample.page_count = 100;
  sample.resident_pages = 40;
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

void TestStorageMetricPublication() {
  // SEARCH_KEY: OEIC_STORAGE_IO_OPTIMIZER_METRICS
  Require(opt::EnsureOptimizerEnterpriseMetricDescriptors().ok,
          "optimizer descriptors failed");
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
  Require(result.diagnostic_code == "SB_OPTIMIZER_STORAGE_METRICS.OK",
          "unexpected storage metric diagnostic");

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

void TestStorageMetricRefusals() {
  auto sample = GoodSample();
  sample.authority.parser_or_donor_authority = true;
  auto refused = page::PublishOptimizerStorageMetrics(sample);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_OPTIMIZER_STORAGE_METRICS.UNSAFE_AUTHORITY",
          "parser/donor storage authority was not refused");

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
  TestStorageMetricPublication();
  TestStorageMetricRefusals();
  std::cout << "optimizer enterprise storage metrics gate passed\n";
  return 0;
}
