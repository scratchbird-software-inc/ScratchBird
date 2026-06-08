// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "observability/performance_metric_event.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace api = scratchbird::engine::internal_api;

// SEARCH_KEY: ODFR_HOT_PATH_ATTRIBUTION_GATE

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool Contains(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool ContainsText(const std::string& text, const std::string& expected) {
  return text.find(expected) != std::string::npos;
}

void SetBenchmarkCleanMeasurementProvenance(
    api::PerformanceMetricHotPathAttribution* hot) {
  hot->cpu_sample_measurement_source = "measured_by_perf_sample";
  hot->cpu_sample_measurement_quality = "measured";
  hot->allocator_counter_measurement_source = "measured_by_internal_counter";
  hot->allocator_counter_measurement_quality = "measured";
  hot->lock_latch_wait_measurement_source = "measured_by_internal_counter";
  hot->lock_latch_wait_measurement_quality = "actual_zero";
  hot->syscall_count_measurement_source = "measured_by_platform_api";
  hot->syscall_count_measurement_quality = "measured";
  hot->file_io_count_measurement_source = "measured_by_platform_api";
  hot->file_io_count_measurement_quality = "actual_zero";
  hot->page_fault_count_measurement_source = "measured_by_platform_api";
  hot->page_fault_count_measurement_quality = "actual_zero";
  hot->context_switch_count_measurement_source = "measured_by_platform_api";
  hot->context_switch_count_measurement_quality = "measured";
  hot->evidence_rendering_measurement_source = "measured_by_internal_counter";
  hot->evidence_rendering_measurement_quality = "measured";
  hot->result_formatting_measurement_source = "measured_by_internal_counter";
  hot->result_formatting_measurement_quality = "measured";
  hot->regression_budget_measurement_source = "estimated";
  hot->regression_budget_measurement_quality = "estimated";
  hot->parser_lowering_measurement_source = "measured_by_internal_counter";
  hot->parser_lowering_measurement_quality = "measured";
  hot->sbps_listener_measurement_source = "measured_by_internal_counter";
  hot->sbps_listener_measurement_quality = "measured";
  hot->sblr_dispatch_measurement_source = "measured_by_internal_counter";
  hot->sblr_dispatch_measurement_quality = "measured";
  hot->internal_api_measurement_source = "measured_by_internal_counter";
  hot->internal_api_measurement_quality = "measured";
  hot->executor_measurement_source = "measured_by_internal_counter";
  hot->executor_measurement_quality = "measured";
  hot->storage_measurement_source = "measured_by_internal_counter";
  hot->storage_measurement_quality = "measured";
  hot->index_layer_measurement_source = "measured_by_internal_counter";
  hot->index_layer_measurement_quality = "measured";
  hot->transaction_measurement_source = "measured_by_internal_counter";
  hot->transaction_measurement_quality = "measured";
  hot->result_rendering_measurement_source = "measured_by_internal_counter";
  hot->result_rendering_measurement_quality = "measured";
  hot->evidence_construction_measurement_source = "measured_by_internal_counter";
  hot->evidence_construction_measurement_quality = "measured";
  hot->allocation_measurement_source = "estimated";
  hot->allocation_measurement_quality = "estimated";
  hot->syscall_measurement_source = "estimated";
  hot->syscall_measurement_quality = "estimated";
  hot->wait_measurement_source = "measured_by_internal_counter";
  hot->wait_measurement_quality = "actual_zero";
}

void SetMetricFamilyProvenance(api::PerformanceMetricEvent* event) {
  event->phase_timings.measurement_source = "measured_by_internal_counter";
  event->phase_timings.measurement_quality = "measured";
  event->storage_timings.measurement_source = "measured_by_internal_counter";
  event->storage_timings.measurement_quality = "measured";
  event->agent_counters.measurement_source = "measured_by_platform_api";
  event->agent_counters.measurement_quality = "measured";
  event->cache_flags.measurement_source = "measured_by_internal_counter";
  event->cache_flags.measurement_quality = "measured";
}

api::PerformanceMetricEvent CompleteBenchmarkCleanEvent() {
  api::PerformanceMetricEvent event;
  event.route = "embedded.sblr.select";
  event.operation = "optimizer_deficiency.odfr_001";
  SetMetricFamilyProvenance(&event);
  event.phase_timings.parse_us = 0;
  event.phase_timings.bind_us = 2;
  event.phase_timings.lower_us = 3;
  event.phase_timings.plan_us = 5;
  event.phase_timings.execute_us = 13;
  event.storage_timings.append_us = 0;
  event.storage_timings.page_us = 7;
  event.storage_timings.index_us = 11;
  event.agent_counters.agent_thread_count = 1;
  event.agent_counters.agent_cpu_user_us = 17;
  event.agent_counters.agent_cpu_system_us = 19;
  event.agent_counters.agent_wait_us = 0;
  event.agent_counters.agent_io_read_bytes = 4096;
  event.agent_counters.agent_io_write_bytes = 0;
  event.cache_flags.plan_cache_hit = true;
  event.cache_flags.metadata_cache_hit = true;
  event.cache_flags.page_cache_hit = true;
  event.cache_flags.index_cache_hit = false;
  event.hot_path_attribution.cpu_sample_count = 101;
  event.hot_path_attribution.cpu_sample_attributed_count = 101;
  event.hot_path_attribution.cpu_sample_attribution = "symbolized_stack_bucket";
  event.hot_path_attribution.allocator_allocation_count = 4;
  event.hot_path_attribution.allocator_allocation_bytes = 512;
  event.hot_path_attribution.lock_latch_wait_count = 0;
  event.hot_path_attribution.lock_latch_wait_us = 0;
  event.hot_path_attribution.syscall_count = 8;
  event.hot_path_attribution.file_open_count = 0;
  event.hot_path_attribution.file_flush_count = 0;
  event.hot_path_attribution.file_fsync_count = 0;
  event.hot_path_attribution.page_fault_count = 0;
  event.hot_path_attribution.context_switch_count = 1;
  event.hot_path_attribution.evidence_rendering_us = 3;
  event.hot_path_attribution.result_formatting_us = 5;
  event.hot_path_attribution.regression_budget_us = 250;
  event.hot_path_attribution.regression_budget_margin_us = 242;
  event.hot_path_attribution.regression_budget_validated = true;
  event.hot_path_attribution.parser_lowering_us = 3;
  event.hot_path_attribution.sbps_listener_us = 2;
  event.hot_path_attribution.sblr_dispatch_us = 4;
  event.hot_path_attribution.internal_api_us = 3;
  event.hot_path_attribution.executor_us = 13;
  event.hot_path_attribution.storage_us = 7;
  event.hot_path_attribution.index_layer_us = 11;
  event.hot_path_attribution.transaction_us = 1;
  event.hot_path_attribution.result_rendering_us = 5;
  event.hot_path_attribution.evidence_construction_us = 3;
  event.hot_path_attribution.allocation_us = 1;
  event.hot_path_attribution.syscall_us = 2;
  event.hot_path_attribution.wait_us = 0;
  SetBenchmarkCleanMeasurementProvenance(&event.hot_path_attribution);
  event.statistics_epoch = 42;
  event.resource_governor_state = "admitted";
  event.message_vector_present = false;
  event.result_hash = "odfr001-result-hash";
  event.overhead_mode = api::InstrumentationOverheadMode::kBenchmarkClean;
  return event;
}

bool SchemaContainsRequiredAttributionFields() {
  const auto& schema = api::PerformanceMetricEventSchema();
  const std::vector<std::string> fields = {
      "cpu_sample_count",
      "cpu_sample_attributed_count",
      "cpu_sample_attribution",
      "cpu_sample_measurement_source",
      "cpu_sample_measurement_quality",
      "allocator_allocation_count",
      "allocator_allocation_bytes",
      "allocator_counter_measurement_source",
      "allocator_counter_measurement_quality",
      "lock_latch_wait_count",
      "lock_latch_wait_us",
      "lock_latch_wait_measurement_source",
      "lock_latch_wait_measurement_quality",
      "syscall_count",
      "syscall_count_measurement_source",
      "syscall_count_measurement_quality",
      "file_open_count",
      "file_flush_count",
      "file_fsync_count",
      "file_io_count_measurement_source",
      "file_io_count_measurement_quality",
      "page_fault_count",
      "page_fault_count_measurement_source",
      "page_fault_count_measurement_quality",
      "context_switch_count",
      "context_switch_count_measurement_source",
      "context_switch_count_measurement_quality",
      "evidence_rendering_us",
      "evidence_rendering_measurement_source",
      "evidence_rendering_measurement_quality",
      "result_formatting_us",
      "result_formatting_measurement_source",
      "result_formatting_measurement_quality",
      "regression_budget_us",
      "regression_budget_margin_us",
      "regression_budget_validated",
      "regression_budget_measurement_source",
      "regression_budget_measurement_quality",
      "parser_lowering_us",
      "sbps_listener_us",
      "sblr_dispatch_us",
      "internal_api_us",
      "executor_us",
      "storage_us",
      "index_layer_us",
      "transaction_us",
      "result_rendering_us",
      "evidence_construction_us",
      "allocation_us",
      "syscall_us",
      "wait_us",
      "parser_lowering_measurement_source",
      "parser_lowering_measurement_quality",
      "sbps_listener_measurement_source",
      "sbps_listener_measurement_quality",
      "sblr_dispatch_measurement_source",
      "sblr_dispatch_measurement_quality",
      "internal_api_measurement_source",
      "internal_api_measurement_quality",
      "executor_measurement_source",
      "executor_measurement_quality",
      "storage_measurement_source",
      "storage_measurement_quality",
      "index_layer_measurement_source",
      "index_layer_measurement_quality",
      "transaction_measurement_source",
      "transaction_measurement_quality",
      "result_rendering_measurement_source",
      "result_rendering_measurement_quality",
      "evidence_construction_measurement_source",
      "evidence_construction_measurement_quality",
      "allocation_measurement_source",
      "allocation_measurement_quality",
      "syscall_measurement_source",
      "syscall_measurement_quality",
      "wait_measurement_source",
      "wait_measurement_quality"};

  for (const auto& field : fields) {
    const auto found = std::find_if(schema.begin(), schema.end(), [&](const auto& entry) {
      return entry.name == field && entry.required;
    });
    if (!Require(found != schema.end(), "schema missing required field: " + field)) {
      return false;
    }
  }
  return true;
}

bool ValidationRejectsMissingAttribution() {
  auto event = CompleteBenchmarkCleanEvent();
  event.hot_path_attribution.syscall_count.reset();
  event.hot_path_attribution.regression_budget_validated.reset();
  event.hot_path_attribution.sblr_dispatch_us.reset();
  event.hot_path_attribution.sblr_dispatch_measurement_source.clear();
  event.hot_path_attribution.sblr_dispatch_measurement_quality.clear();
  const auto result = api::ValidatePerformanceMetricEvent(event);
  return Require(!result.ok, "missing attribution event was accepted") &&
         Require(result.diagnostic_code == "CDP.PERFORMANCE_METRIC_EVENT.MISSING_REQUIRED_FIELD",
                 "missing attribution diagnostic mismatch") &&
         Require(Contains(result.missing_fields, "syscall_count"),
                 "missing syscall_count not reported") &&
         Require(Contains(result.missing_fields, "regression_budget_validated"),
                 "missing regression_budget_validated not reported") &&
         Require(Contains(result.missing_fields, "sblr_dispatch_us"),
                 "missing sblr_dispatch_us not reported") &&
         Require(Contains(result.missing_fields,
                          "sblr_dispatch_measurement_source"),
                 "missing sblr_dispatch_measurement_source not reported") &&
         Require(Contains(result.missing_fields,
                          "sblr_dispatch_measurement_quality"),
                 "missing sblr_dispatch_measurement_quality not reported");
}

bool SerializationRendersAttributionAndBudget() {
  const auto event = CompleteBenchmarkCleanEvent();
  const auto result = api::ValidatePerformanceMetricEvent(event);
  if (!Require(result.ok, "complete benchmark-clean attribution event rejected")) {
    return false;
  }
  const auto json = api::SerializePerformanceMetricEventJson(event);
  return Require(ContainsText(json, "\"schema_id\":\"scratchbird.performance_metric_event.v3\""),
                 "schema v3 id missing") &&
         Require(ContainsText(json, "\"instrumentation_overhead_mode\":\"benchmark_clean\""),
                 "benchmark-clean mode missing") &&
         Require(ContainsText(json, "\"cpu_sample_attribution\":\"symbolized_stack_bucket\""),
                 "CPU attribution missing") &&
         Require(ContainsText(json, "\"cpu_sample_measurement_source\":\"measured_by_perf_sample\""),
                 "CPU sample measurement source missing") &&
         Require(ContainsText(json, "\"allocator_allocation_count\":4"),
                 "allocator count missing") &&
         Require(ContainsText(json,
                              "\"allocator_counter_measurement_quality\":\"measured\""),
                 "allocator measurement quality missing") &&
         Require(ContainsText(json, "\"allocator_allocation_bytes\":512"),
                 "allocator bytes missing") &&
         Require(ContainsText(json, "\"lock_latch_wait_count\":0"),
                 "lock/latch count missing") &&
         Require(ContainsText(json, "\"syscall_count\":8"),
                 "syscall count missing") &&
         Require(ContainsText(json, "\"file_open_count\":0"),
                 "file open count missing") &&
         Require(ContainsText(json, "\"file_flush_count\":0"),
                 "file flush count missing") &&
         Require(ContainsText(json, "\"file_fsync_count\":0"),
                 "file fsync count missing") &&
         Require(ContainsText(json, "\"file_io_count_measurement_quality\":\"actual_zero\""),
                 "file IO actual-zero quality missing") &&
         Require(ContainsText(json, "\"page_fault_count\":0"),
                 "page fault count missing") &&
         Require(ContainsText(json, "\"context_switch_count\":1"),
                 "context switch count missing") &&
         Require(ContainsText(json, "\"evidence_rendering_us\":3"),
                 "evidence rendering cost missing") &&
         Require(ContainsText(json, "\"result_formatting_us\":5"),
                 "result formatting cost missing") &&
         Require(ContainsText(json, "\"regression_budget_validated\":true"),
                 "regression budget validation missing") &&
         Require(ContainsText(json,
                              "\"regression_budget_measurement_source\":\"estimated\""),
                 "regression budget source missing") &&
         Require(ContainsText(json, "\"sblr_dispatch_us\":4"),
                 "SBLR dispatch attribution missing") &&
         Require(ContainsText(json, "\"wait_measurement_source\":\"measured_by_internal_counter\""),
                 "wait measurement source missing") &&
         Require(ContainsText(json, "\"wait_measurement_quality\":\"actual_zero\""),
                 "wait measurement quality missing");
}

}  // namespace

int main() {
  if (!SchemaContainsRequiredAttributionFields()) return 1;
  if (!ValidationRejectsMissingAttribution()) return 1;
  if (!SerializationRendersAttributionAndBudget()) return 1;
  return 0;
}
