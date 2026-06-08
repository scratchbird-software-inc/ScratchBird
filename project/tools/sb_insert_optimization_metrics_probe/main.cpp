// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_registry.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    return false;
  }
  return true;
}

struct ExpectedMetric {
  const char* family;
  scratchbird::core::metrics::MetricType type;
  scratchbird::core::metrics::MetricUnit unit;
  const char* namespace_path;
  const char* producer_owner;
};

}  // namespace

int main() {
  using namespace scratchbird::core::metrics;

  const std::vector<ExpectedMetric> expected = {
      {"sb_dml_insert_batch_started_total", MetricType::counter, MetricUnit::count, "sys.metrics.dml.insert", "engine_insert"},
      {"sb_dml_insert_batch_fallback_total", MetricType::counter, MetricUnit::count, "sys.metrics.dml.insert", "engine_insert"},
      {"sb_dml_insert_batch_fallback_reason_total", MetricType::counter, MetricUnit::count, "sys.metrics.dml.insert", "engine_insert"},
      {"sb_dml_insert_rows_inserted_total", MetricType::counter, MetricUnit::count, "sys.metrics.dml.insert", "engine_insert"},
      {"sb_dml_insert_rows_per_batch", MetricType::histogram, MetricUnit::count, "sys.metrics.dml.insert", "engine_insert"},
      {"sb_dml_insert_row_template_cache_hit_total", MetricType::counter, MetricUnit::count, "sys.metrics.dml.insert", "engine_insert"},
      {"sb_dml_insert_identity_range_reserved_total", MetricType::counter, MetricUnit::count, "sys.metrics.dml.insert", "engine_insert"},
      {"sb_page_insert_page_reuse_total", MetricType::counter, MetricUnit::count, "sys.metrics.page.insert", "engine_insert"},
      {"sb_page_insert_page_full_retry_total", MetricType::counter, MetricUnit::count, "sys.metrics.page.insert", "engine_insert"},
      {"sb_page_insert_page_split_total", MetricType::counter, MetricUnit::count, "sys.metrics.page.insert", "engine_insert"},
      {"sb_page_allocation_reserve_low_total", MetricType::counter, MetricUnit::count, "sys.metrics.page.allocation", "engine_insert"},
      {"sb_index_insert_synchronous_total", MetricType::counter, MetricUnit::count, "sys.metrics.index.insert", "engine_insert"},
      {"sb_index_insert_delta_ledger_total", MetricType::counter, MetricUnit::count, "sys.metrics.index.insert", "engine_insert"},
      {"sb_index_insert_delta_merge_total", MetricType::counter, MetricUnit::count, "sys.metrics.index.insert", "engine_insert"},
      {"sb_index_insert_unique_preflight_hit_total", MetricType::counter, MetricUnit::count, "sys.metrics.index.insert", "engine_insert"},
      {"sb_index_insert_sorted_run_total", MetricType::counter, MetricUnit::count, "sys.metrics.index.insert", "engine_insert"},
      {"sb_filespace_insert_growth_request_total", MetricType::counter, MetricUnit::count, "sys.metrics.filespace.insert", "engine_insert"},
      {"sb_filespace_insert_growth_wait_microseconds", MetricType::histogram, MetricUnit::microseconds, "sys.metrics.filespace.insert", "engine_insert"},
      {"sb_dml_insert_cancel_total", MetricType::counter, MetricUnit::count, "sys.metrics.dml.insert", "engine_insert"},
      {"sb_dml_insert_trace_event_total", MetricType::counter, MetricUnit::count, "sys.metrics.dml.insert", "engine_insert"},
  };

  bool ok = true;
  auto& registry = DefaultMetricRegistry();
  for (const auto& metric : expected) {
    const auto* descriptor = registry.FindDescriptorOrAlias(metric.family);
    ok &= Require(descriptor != nullptr, std::string("missing insert metric descriptor ") + metric.family);
    if (descriptor == nullptr) {
      continue;
    }
    ok &= Require(descriptor->type == metric.type, std::string("wrong metric type ") + metric.family);
    ok &= Require(descriptor->unit == metric.unit, std::string("wrong metric unit ") + metric.family);
    ok &= Require(descriptor->namespace_path == metric.namespace_path, std::string("wrong namespace ") + metric.family);
    ok &= Require(descriptor->producer_owner == metric.producer_owner, std::string("wrong producer ") + metric.family);
    ok &= Require(!descriptor->cluster_only, std::string("insert metric must be local-visible ") + metric.family);
    ok &= Require(!descriptor->help.empty(), std::string("missing help ") + metric.family);
  }

  ok &= Require(registry.IncrementCounter("sb_dml_insert_batch_started_total", {}, 1.0, "engine_insert").ok,
                "insert batch counter accepts engine_insert producer");
  ok &= Require(registry.IncrementCounter("sb_dml_insert_rows_inserted_total", {}, 3.0, "engine_insert").ok,
                "insert row counter accepts engine_insert producer");
  ok &= Require(registry.ObserveHistogram("sb_dml_insert_rows_per_batch", {}, 3.0, "engine_insert").ok,
                "insert rows-per-batch histogram accepts observation");
  ok &= Require(registry.ObserveHistogram("sb_filespace_insert_growth_wait_microseconds", {}, 11.0, "engine_insert").ok,
                "insert filespace wait histogram accepts observation");
  ok &= Require(!registry.SetGauge("sb_dml_insert_rows_inserted_total", {}, 1.0, "engine_insert").ok,
                "insert counter rejects gauge update");

  return ok ? 0 : 1;
}
