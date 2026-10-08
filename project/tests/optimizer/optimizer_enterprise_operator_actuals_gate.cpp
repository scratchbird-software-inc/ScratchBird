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
#include "../support/metric_projection_fixture.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
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
  sample.scope_uuid = scratchbird::tests::FixtureUuid(1274, 1);
  sample.database_uuid = scratchbird::tests::FixtureUuid(1274, 2);
  sample.node_uuid = scratchbird::tests::FixtureUuid(1274, 3);
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

void Admit(scratchbird::tests::MetricProjectionFixture& fixture,
           const exec::ExecutorOperatorActualsSample& sample) {
  for (const auto& definition : exec::ExecutorOperatorActualsMetricDescriptorDefinitions())
    fixture.AdmitDefinition(definition, {{"scope_uuid", sample.scope_uuid}, {"route_label", sample.route_label},
        {"plan_node_id", sample.plan_node_id}, {"metric_family", definition.family.substr(std::string("sb_optimizer_").size())},
        {"source_generation", std::to_string(sample.source_generation)}, {"evidence_digest", sample.evidence_digest}});
  static const auto definitions = metrics::BuiltinMetricDescriptorDefinitions();
  for (const auto& definition : definitions) if (definition.family.starts_with("sb_optimizer_feedback_"))
    fixture.AdmitDefinition(definition, {{"component", "optimizer.feedback"}, {"operator_family", sample.operator_family},
        {"plan_shape", sample.plan_shape}});
}

void PublishAndRequire(scratchbird::tests::MetricProjectionFixture& fixture, exec::ExecutorOperatorActualsSample sample) {
  Admit(fixture, sample);
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
  Require(result.scope_uuid == sample.scope_uuid && result.metric_results.size() == 23, "incomplete native operator receipt");
  fixture.ExpectProduced(23); fixture.Seal();
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

void TestRealBatchOperatorActuals(scratchbird::tests::MetricProjectionFixture& fixture) {
  // SEARCH_KEY: OEIC_EXECUTOR_OPERATOR_ACTUALS_METRICS

  auto input = exec::MakeBatch("orders",
                              {{{1, 10}}, {{2, 20}}, {{3, 30}}, {{4, 40}}});
  auto filtered = exec::FilterGreaterThan(input, 1, 15);
  PublishAndRequire(fixture, SampleFor("filter",
                              "filter-node",
                              input.rows.size(),
                              filtered.rows.size(),
                              input.rows.size(),
                              input.rows.size() - filtered.rows.size()));

  auto sorted = exec::SortByColumn(input, 1, false);
  PublishAndRequire(fixture, SampleFor("sort",
                              "sort-node",
                              input.rows.size(),
                              sorted.rows.size(),
                              input.rows.size(),
                              0));

  auto right = exec::MakeBatch("customers", {{{1, 100}}, {{3, 300}}});
  auto joined = exec::HashJoinEqual(input, right, 0, 0);
  PublishAndRequire(fixture, SampleFor("hash_join",
                              "join-node",
                              2,
                              joined.rows.size(),
                              input.rows.size() + right.rows.size(),
                              0));

  auto aggregate = exec::AggregateSumByKey(input, 0, 1);
  PublishAndRequire(fixture, SampleFor("aggregate",
                              "aggregate-node",
                              input.rows.size(),
                              aggregate.rows.size(),
                              input.rows.size(),
                              0));

  auto window = exec::AddRowNumberWindow(input, 1);
  PublishAndRequire(fixture, SampleFor("window",
                              "window-node",
                              input.rows.size(),
                              window.rows.size(),
                              input.rows.size(),
                              0));

  auto setop = exec::SetUnionDistinct(input, right);
  PublishAndRequire(fixture, SampleFor("set_operation",
                              "setop-node",
                              input.rows.size() + right.rows.size(),
                              setop.rows.size(),
                              input.rows.size() + right.rows.size(),
                              0));

  auto dml_result_rows = exec::MakeBatch("dml.write.result", {{{1}}, {{2}}});
  PublishAndRequire(fixture, SampleFor("dml_write",
                              "dml-node",
                              2,
                              dml_result_rows.rows.size(),
                              dml_result_rows.rows.size(),
                              0));

  auto result_frame = exec::SetUnionAll(filtered, aggregate);
  PublishAndRequire(fixture, SampleFor("result_frame",
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
  sample.authority.parser_or_reference_authority = true;
  auto refused = exec::PublishExecutorOperatorActuals(sample);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_EXECUTOR_OPERATOR_ACTUALS.UNSAFE_AUTHORITY",
          "parser/reference authority was not refused");

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

void TestExactValuesAndEffects(scratchbird::tests::MetricProjectionFixture& fixture) {
  using Sample = exec::ExecutorOperatorActualsSample;
  auto sample = SampleFor("native_bounds", "bounds-node", 1, 1, 1, 0);
  Admit(fixture, sample); fixture.Seal();
  for (const auto count : {std::uint64_t{0}, (std::uint64_t{1} << 53) + 1, std::numeric_limits<std::uint64_t>::max()}) {
    for (auto member : {&Sample::estimated_rows, &Sample::actual_rows, &Sample::rows_examined, &Sample::rows_filtered,
                        &Sample::estimated_pages, &Sample::actual_pages, &Sample::estimated_io_operations,
                        &Sample::actual_io_operations, &Sample::estimated_visibility_recheck_rows,
                        &Sample::actual_visibility_recheck_rows, &Sample::estimated_spill_bytes, &Sample::actual_spill_bytes,
                        &Sample::spill_passes, &Sample::memory_grant_bytes, &Sample::peak_memory_bytes,
                        &Sample::estimated_resource_units, &Sample::actual_resource_units}) sample.*member = count;
    sample.loop_count = count == 0 ? 1 : count;
    const auto result = exec::PublishExecutorOperatorActuals(sample);
    Require(result.ok && result.metric_results.size() == 23 && result.scope_uuid == sample.scope_uuid,
            "exact UINT64 actuals rejected");
    fixture.ExpectProduced(23); fixture.Seal();
    std::size_t matched = 0;
    for (const auto& value : metrics::DefaultMetricRegistry().SnapshotCurrent(false)) {
      bool selected = false;
      for (const auto& label : value.labels)
        if ((label.key == "plan_node_id" && std::get<std::string>(label.value) == "bounds-node") ||
            (label.key == "operator_family" && std::get<std::string>(label.value) == "native_bounds")) selected = true;
      if (!selected) continue;
      if (value.family == "sb_optimizer_operator_cpu_time" || value.family.ends_with("latency_microseconds")) continue;
      Require(std::holds_alternative<std::uint64_t>(value.value) &&
                  std::get<std::uint64_t>(value.value) == (value.family == "sb_optimizer_operator_loop_count" ? sample.loop_count : count),
              "executor/feedback count rounded: " + value.family);
      ++matched;
    }
    Require(matched == 20, "native operator/feedback values missing");
  }
  const auto refuse = [&](const Sample& bad) {
    const auto result = exec::PublishExecutorOperatorActuals(bad);
    Require(!result.ok && result.metric_results.empty() && result.scope_uuid == bad.scope_uuid,
            "bad operator input was not refused before effects");
    fixture.VerifyReadOnly();
  };
  for (auto member : {&Sample::estimated_latency_microseconds, &Sample::actual_latency_microseconds, &Sample::cpu_time_microseconds})
    for (auto value : {(std::uint64_t{1} << 53) + 1, std::numeric_limits<std::uint64_t>::max()}) {
      auto bad = sample; bad.*member = value; refuse(bad);
    }
  auto bad = sample; bad.scope_uuid = {}; refuse(bad);
  bad = sample; bad.database_uuid = scratchbird::tests::FixtureUuid(1274, 99); refuse(bad);
  bad = sample; bad.node_uuid = scratchbird::tests::FixtureUuid(1274, 99); refuse(bad);
  bad = sample; bad.loop_count = 0; refuse(bad);
  bad = sample; bad.rows_examined = 0; bad.rows_filtered = 1; refuse(bad);
  for (auto value : {std::uint64_t{0}, std::uint64_t{1} << 63}) {
    sample.estimated_latency_microseconds = sample.actual_latency_microseconds = sample.cpu_time_microseconds = value;
    Require(exec::PublishExecutorOperatorActuals(sample).ok, "exact FLOAT64 duration refused");
    fixture.ExpectProduced(23); fixture.Seal();
  }
  sample.plan_node_id = "unadmitted-node";
  const auto result = exec::PublishExecutorOperatorActuals(sample);
  Require(!result.ok && result.metric_results.size() == 23 && result.diagnostic_code != "SB_EXECUTOR_OPERATOR_ACTUALS.OK",
          "partial executor publication claimed success");
  for (std::size_t i = 0; i < result.metric_results.size(); ++i)
    Require(result.metric_results[i].ok == (i >= 6), "partial publication lost accepted/refused outcome");
  fixture.ExpectProduced(17); fixture.Seal();
}

}  // namespace

int main() {
  metrics::MetricRegistry empty;
  Require(!exec::EnsureExecutorOperatorActualsMetricDescriptors(&empty).ok &&
              !empty.FindDescriptor("sb_optimizer_operator_actual_rows"), "operator verification fabricated admission");
  auto sample = SampleFor("filter", "filter-node", 4, 3, 4, 1);
  scratchbird::tests::MetricProjectionFixture fixture(sample.database_uuid, sample.node_uuid, 1275, 512);
  TestRealBatchOperatorActuals(fixture);
  TestAuthorityRefusals();
  fixture.VerifyReadOnly();
  TestExactValuesAndEffects(fixture);
  fixture.VerifyAndDrain();
  std::cout << "optimizer enterprise operator actuals gate passed\n";
  return 0;
}
