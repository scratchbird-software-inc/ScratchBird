// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "executor_foundation.hpp"
#include "executor_operator_metrics.hpp"
#include "metric_registry.hpp"
#include "optimizer_metric_manifest.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace exec = scratchbird::engine::executor;
namespace opt = scratchbird::engine::optimizer;
namespace metrics = scratchbird::core::metrics;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "optimizer_enterprise_operator_actuals_gate: " << message
              << '\n';
    std::exit(1);
  }
}

exec::ExecutorOperatorMetricAuthority GoodAuthority() {
  exec::ExecutorOperatorMetricAuthority authority;
  authority.engine_mga_snapshot_bound = true;
  authority.transaction_inventory_authoritative = true;
  authority.security_recheck_preserved = true;
  return authority;
}

exec::ExecutorOperatorActualsSample SampleFor(const std::string& family,
                                              const std::string& node_id,
                                              std::uint64_t estimated_rows,
                                              std::uint64_t actual_rows,
                                              std::uint64_t rows_examined,
                                              std::uint64_t rows_filtered) {
  exec::ExecutorOperatorActualsSample sample;
  sample.scope_uuid = "database-uuid-1";
  sample.route_label = "embedded";
  sample.plan_node_id = node_id;
  sample.operator_family = family;
  sample.plan_shape = "enterprise.actuals." + family;
  sample.evidence_digest = "digest-" + node_id;
  sample.source_generation = 7;
  sample.estimated_rows = estimated_rows;
  sample.actual_rows = actual_rows;
  sample.rows_examined = rows_examined;
  sample.rows_filtered = rows_filtered;
  sample.loop_count = 1;
  sample.estimated_pages = 2;
  sample.actual_pages = 3;
  sample.estimated_io_operations = 2;
  sample.actual_io_operations = 3;
  sample.estimated_visibility_recheck_rows = estimated_rows;
  sample.actual_visibility_recheck_rows = actual_rows;
  sample.estimated_spill_bytes = 0;
  sample.actual_spill_bytes = family == "sort" ? 4096 : 0;
  sample.spill_passes = family == "sort" ? 1 : 0;
  sample.memory_grant_bytes = 65536;
  sample.peak_memory_bytes = 32768 + actual_rows;
  sample.estimated_latency_microseconds = 100;
  sample.actual_latency_microseconds = 125 + actual_rows;
  sample.cpu_time_microseconds = 20 + actual_rows;
  sample.estimated_resource_units = estimated_rows + 1;
  sample.actual_resource_units = actual_rows + rows_examined + 1;
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

void PublishAndRequire(exec::ExecutorOperatorActualsSample sample) {
  auto result = exec::PublishExecutorOperatorActuals(sample);
  if (!result.ok) {
    std::cerr << result.diagnostic_code << ": " << result.detail << '\n';
    for (const auto& metric_result : result.metric_results) {
      if (!metric_result.ok) {
        std::cerr << "metric: " << metric_result.diagnostic_code << ": "
                  << metric_result.detail << '\n';
      }
    }
  }
  Require(result.ok, "valid operator actuals sample was refused");
  Require(result.diagnostic_code == "SB_EXECUTOR_OPERATOR_ACTUALS.OK",
          "unexpected operator actuals diagnostic");
  bool advisory = false;
  for (const auto& evidence : result.evidence) {
    if (evidence == "executor.operator_actuals.advisory_only=true") {
      advisory = true;
    }
  }
  Require(advisory, "operator actuals evidence did not record advisory-only status");
}

void TestRealBatchOperatorActuals() {
  // SEARCH_KEY: OEIC_EXECUTOR_OPERATOR_ACTUALS_METRICS
  Require(opt::EnsureOptimizerEnterpriseMetricDescriptors().ok,
          "optimizer metric descriptors failed");
  Require(exec::EnsureExecutorOperatorActualsMetricDescriptors().ok,
          "executor operator metric descriptors failed");

  auto input = exec::MakeBatch("orders",
                              {{{1, 10}}, {{2, 20}}, {{3, 30}}, {{4, 40}}});
  auto filtered = exec::FilterGreaterThan(input, 1, 15);
  PublishAndRequire(SampleFor("filter",
                              "filter-node",
                              input.rows.size(),
                              filtered.rows.size(),
                              input.rows.size(),
                              input.rows.size() - filtered.rows.size()));

  auto sorted = exec::SortByColumn(input, 1, false);
  PublishAndRequire(SampleFor("sort",
                              "sort-node",
                              input.rows.size(),
                              sorted.rows.size(),
                              input.rows.size(),
                              0));

  auto right = exec::MakeBatch("customers", {{{1, 100}}, {{3, 300}}});
  auto joined = exec::HashJoinEqual(input, right, 0, 0);
  PublishAndRequire(SampleFor("hash_join",
                              "join-node",
                              2,
                              joined.rows.size(),
                              input.rows.size() + right.rows.size(),
                              0));

  auto aggregate = exec::AggregateSumByKey(input, 0, 1);
  PublishAndRequire(SampleFor("aggregate",
                              "aggregate-node",
                              input.rows.size(),
                              aggregate.rows.size(),
                              input.rows.size(),
                              0));

  auto window = exec::AddRowNumberWindow(input, 1);
  PublishAndRequire(SampleFor("window",
                              "window-node",
                              input.rows.size(),
                              window.rows.size(),
                              input.rows.size(),
                              0));

  auto setop = exec::SetUnionDistinct(input, right);
  PublishAndRequire(SampleFor("set_operation",
                              "setop-node",
                              input.rows.size() + right.rows.size(),
                              setop.rows.size(),
                              input.rows.size() + right.rows.size(),
                              0));

  auto dml_result_rows = exec::MakeBatch("dml.write.result", {{{1}}, {{2}}});
  PublishAndRequire(SampleFor("dml_write",
                              "dml-node",
                              2,
                              dml_result_rows.rows.size(),
                              dml_result_rows.rows.size(),
                              0));

  auto result_frame = exec::SetUnionAll(filtered, aggregate);
  PublishAndRequire(SampleFor("result_frame",
                              "result-frame-node",
                              filtered.rows.size() + aggregate.rows.size(),
                              result_frame.rows.size(),
                              result_frame.rows.size(),
                              0));

  const auto snapshot = metrics::DefaultMetricRegistry().SnapshotCurrent(false);
  Require(HasMetricValue(snapshot, "sb_optimizer_operator_actual_rows"),
          "operator actual rows metric missing from current snapshot");
  Require(HasMetricValue(snapshot, "sb_optimizer_operator_rows_examined"),
          "operator rows examined metric missing from current snapshot");
  Require(HasMetricValue(snapshot, "sb_optimizer_operator_rows_filtered"),
          "operator rows filtered metric missing from current snapshot");
  Require(HasMetricValue(snapshot, "sb_optimizer_operator_loop_count"),
          "operator loop count metric missing from current snapshot");
  Require(HasMetricValue(snapshot, "sb_optimizer_operator_cpu_time"),
          "operator cpu time metric missing from current snapshot");
  Require(HasMetricValue(snapshot, "sb_optimizer_spill_passes"),
          "spill passes metric missing from current snapshot");
  Require(HasMetricValue(snapshot, "sb_optimizer_feedback_actual_rows"),
          "optimizer feedback actual rows metric missing from current snapshot");
  Require(HasMetricValue(snapshot, "sb_optimizer_feedback_memory_grant_bytes"),
          "optimizer feedback memory grant metric missing from current snapshot");
  Require(HasMetricValue(snapshot, "sb_optimizer_feedback_actual_spill_bytes"),
          "optimizer feedback spill metric missing from current snapshot");
}

void TestAuthorityRefusals() {
  auto sample = SampleFor("filter", "bad-node", 10, 5, 10, 5);
  sample.authority.parser_or_donor_authority = true;
  auto refused = exec::PublishExecutorOperatorActuals(sample);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_EXECUTOR_OPERATOR_ACTUALS.UNSAFE_AUTHORITY",
          "parser/donor authority was not refused");

  sample = SampleFor("filter", "stale-node", 10, 5, 10, 5);
  sample.freshness_microseconds = sample.max_freshness_microseconds + 1;
  refused = exec::PublishExecutorOperatorActuals(sample);
  Require(!refused.ok &&
              refused.diagnostic_code == "SB_EXECUTOR_OPERATOR_ACTUALS.STALE",
          "stale operator actuals were not refused");

  sample = SampleFor("filter", "mga-node", 10, 5, 10, 5);
  sample.authority.transaction_inventory_authoritative = false;
  refused = exec::PublishExecutorOperatorActuals(sample);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_EXECUTOR_OPERATOR_ACTUALS.MGA_SECURITY_EVIDENCE_REQUIRED",
          "missing MGA/security evidence was not refused");
}

}  // namespace

int main() {
  TestRealBatchOperatorActuals();
  TestAuthorityRefusals();
  std::cout << "optimizer enterprise operator actuals gate passed\n";
  return 0;
}
