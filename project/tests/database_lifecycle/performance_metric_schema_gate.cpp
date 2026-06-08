// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "observability/performance_metric_event.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool ContainsName(const std::vector<std::string>& names, std::string_view name) {
  for (const auto& item : names) {
    if (item == name) {
      return true;
    }
  }
  return false;
}

const api::PerformanceMetricSchemaField* SchemaField(std::string_view name) {
  for (const auto& field : api::PerformanceMetricEventSchema()) {
    if (field.name == name) {
      return &field;
    }
  }
  return nullptr;
}

void RequireSchemaField(std::string_view name,
                        std::string_view type,
                        bool required) {
  const auto* field = SchemaField(name);
  Require(field != nullptr, std::string("schema field missing: ") + std::string(name));
  Require(field->type == type, std::string("schema field type mismatch: ") + std::string(name));
  Require(field->required == required,
          std::string("schema field required flag mismatch: ") + std::string(name));
}

bool MissingField(const api::PerformanceMetricValidationResult& result,
                  std::string_view field_name) {
  for (const auto& missing : result.missing_fields) {
    if (missing == field_name) {
      return true;
    }
  }
  return false;
}

void SetMeasurementProvenance(api::PerformanceMetricHotPathAttribution* hot) {
  hot->cpu_sample_measurement_source = "measured_by_perf_sample";
  hot->cpu_sample_measurement_quality = "measured";
  hot->allocator_counter_measurement_source = "measured_by_internal_counter";
  hot->allocator_counter_measurement_quality = "measured";
  hot->lock_latch_wait_measurement_source = "measured_by_internal_counter";
  hot->lock_latch_wait_measurement_quality = "measured";
  hot->syscall_count_measurement_source = "measured_by_platform_api";
  hot->syscall_count_measurement_quality = "measured";
  hot->file_io_count_measurement_source = "measured_by_platform_api";
  hot->file_io_count_measurement_quality = "measured";
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
  hot->transaction_measurement_source = "estimated";
  hot->transaction_measurement_quality = "estimated";
  hot->result_rendering_measurement_source = "measured_by_internal_counter";
  hot->result_rendering_measurement_quality = "measured";
  hot->evidence_construction_measurement_source = "measured_by_internal_counter";
  hot->evidence_construction_measurement_quality = "measured";
  hot->allocation_measurement_source = "estimated";
  hot->allocation_measurement_quality = "estimated";
  hot->syscall_measurement_source = "estimated";
  hot->syscall_measurement_quality = "estimated";
  hot->wait_measurement_source = "measured_by_internal_counter";
  hot->wait_measurement_quality = "measured";
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

api::PerformanceMetricEvent CompleteEvent() {
  api::PerformanceMetricEvent event;
  event.route = "embedded_ipc";
  event.operation = "copy_insert_batch";
  SetMetricFamilyProvenance(&event);
  event.phase_timings.parse_us = 11;
  event.phase_timings.bind_us = 12;
  event.phase_timings.lower_us = 13;
  event.phase_timings.plan_us = 14;
  event.phase_timings.execute_us = 15;
  event.storage_timings.append_us = 21;
  event.storage_timings.page_us = 22;
  event.storage_timings.index_us = 23;
  event.agent_counters.agent_thread_count = 2;
  event.agent_counters.agent_cpu_user_us = 31;
  event.agent_counters.agent_cpu_system_us = 32;
  event.agent_counters.agent_wait_us = 33;
  event.agent_counters.agent_io_read_bytes = 4096;
  event.agent_counters.agent_io_write_bytes = 8192;
  event.cache_flags.plan_cache_hit = true;
  event.cache_flags.metadata_cache_hit = false;
  event.cache_flags.page_cache_hit = true;
  event.cache_flags.index_cache_hit = false;
  event.hot_path_attribution.cpu_sample_count = 101;
  event.hot_path_attribution.cpu_sample_attributed_count = 100;
  event.hot_path_attribution.cpu_sample_attribution = "symbolized_stack_bucket";
  event.hot_path_attribution.allocator_allocation_count = 7;
  event.hot_path_attribution.allocator_allocation_bytes = 2048;
  event.hot_path_attribution.lock_latch_wait_count = 3;
  event.hot_path_attribution.lock_latch_wait_us = 34;
  event.hot_path_attribution.syscall_count = 9;
  event.hot_path_attribution.file_open_count = 1;
  event.hot_path_attribution.file_flush_count = 2;
  event.hot_path_attribution.file_fsync_count = 1;
  event.hot_path_attribution.page_fault_count = 0;
  event.hot_path_attribution.context_switch_count = 4;
  event.hot_path_attribution.evidence_rendering_us = 5;
  event.hot_path_attribution.result_formatting_us = 6;
  event.hot_path_attribution.regression_budget_us = 250;
  event.hot_path_attribution.regression_budget_margin_us = 239;
  event.hot_path_attribution.regression_budget_validated = true;
  event.hot_path_attribution.parser_lowering_us = 24;
  event.hot_path_attribution.sbps_listener_us = 10;
  event.hot_path_attribution.sblr_dispatch_us = 8;
  event.hot_path_attribution.internal_api_us = 9;
  event.hot_path_attribution.executor_us = 15;
  event.hot_path_attribution.storage_us = 45;
  event.hot_path_attribution.index_layer_us = 23;
  event.hot_path_attribution.transaction_us = 7;
  event.hot_path_attribution.result_rendering_us = 6;
  event.hot_path_attribution.evidence_construction_us = 5;
  event.hot_path_attribution.allocation_us = 2;
  event.hot_path_attribution.syscall_us = 3;
  event.hot_path_attribution.wait_us = 34;
  SetMeasurementProvenance(&event.hot_path_attribution);
  event.statistics_epoch = 42;
  event.resource_governor_state = "foreground_unthrottled";
  event.message_vector_present = false;
  event.result_hash = "sha256:result-fixture";
  return event;
}

void TestSchemaFields() {
  Require(api::PerformanceMetricEventSchemaVersion() == 3,
          "performance metric schema version mismatch");

  RequireSchemaField("route", "string", true);
  RequireSchemaField("operation", "string", true);
  RequireSchemaField("instrumentation_overhead_mode", "enum", true);
  RequireSchemaField("parse_us", "uint64", true);
  RequireSchemaField("bind_us", "uint64", true);
  RequireSchemaField("lower_us", "uint64", true);
  RequireSchemaField("plan_us", "uint64", true);
  RequireSchemaField("execute_us", "uint64", true);
  RequireSchemaField("phase_timings_measurement_source", "enum", true);
  RequireSchemaField("phase_timings_measurement_quality", "enum", true);
  RequireSchemaField("append_us", "uint64", true);
  RequireSchemaField("page_us", "uint64", true);
  RequireSchemaField("index_us", "uint64", true);
  RequireSchemaField("storage_timings_measurement_source", "enum", true);
  RequireSchemaField("storage_timings_measurement_quality", "enum", true);
  RequireSchemaField("agent_thread_count", "uint64", true);
  RequireSchemaField("agent_cpu_user_us", "uint64", true);
  RequireSchemaField("agent_cpu_system_us", "uint64", true);
  RequireSchemaField("agent_wait_us", "uint64", true);
  RequireSchemaField("agent_io_read_bytes", "uint64", true);
  RequireSchemaField("agent_io_write_bytes", "uint64", true);
  RequireSchemaField("agent_counters_measurement_source", "enum", true);
  RequireSchemaField("agent_counters_measurement_quality", "enum", true);
  RequireSchemaField("plan_cache_hit", "bool", true);
  RequireSchemaField("metadata_cache_hit", "bool", true);
  RequireSchemaField("page_cache_hit", "bool", true);
  RequireSchemaField("index_cache_hit", "bool", true);
  RequireSchemaField("cache_flags_measurement_source", "enum", true);
  RequireSchemaField("cache_flags_measurement_quality", "enum", true);
  RequireSchemaField("cpu_sample_count", "uint64", true);
  RequireSchemaField("cpu_sample_attributed_count", "uint64", true);
  RequireSchemaField("cpu_sample_attribution", "string", true);
  for (const std::string field :
       {"cpu_sample",
        "allocator_counter",
        "lock_latch_wait",
        "syscall_count",
        "file_io_count",
        "page_fault_count",
        "context_switch_count",
        "evidence_rendering",
        "result_formatting",
        "regression_budget"}) {
    RequireSchemaField(field + "_measurement_source", "enum", true);
    RequireSchemaField(field + "_measurement_quality", "enum", true);
  }
  RequireSchemaField("allocator_allocation_count", "uint64", true);
  RequireSchemaField("allocator_allocation_bytes", "uint64", true);
  RequireSchemaField("lock_latch_wait_count", "uint64", true);
  RequireSchemaField("lock_latch_wait_us", "uint64", true);
  RequireSchemaField("syscall_count", "uint64", true);
  RequireSchemaField("file_open_count", "uint64", true);
  RequireSchemaField("file_flush_count", "uint64", true);
  RequireSchemaField("file_fsync_count", "uint64", true);
  RequireSchemaField("page_fault_count", "uint64", true);
  RequireSchemaField("context_switch_count", "uint64", true);
  RequireSchemaField("evidence_rendering_us", "uint64", true);
  RequireSchemaField("result_formatting_us", "uint64", true);
  RequireSchemaField("regression_budget_us", "uint64", true);
  RequireSchemaField("regression_budget_margin_us", "uint64", true);
  RequireSchemaField("regression_budget_validated", "bool", true);
  RequireSchemaField("parser_lowering_us", "uint64", true);
  RequireSchemaField("sbps_listener_us", "uint64", true);
  RequireSchemaField("sblr_dispatch_us", "uint64", true);
  RequireSchemaField("internal_api_us", "uint64", true);
  RequireSchemaField("executor_us", "uint64", true);
  RequireSchemaField("storage_us", "uint64", true);
  RequireSchemaField("index_layer_us", "uint64", true);
  RequireSchemaField("transaction_us", "uint64", true);
  RequireSchemaField("result_rendering_us", "uint64", true);
  RequireSchemaField("evidence_construction_us", "uint64", true);
  RequireSchemaField("allocation_us", "uint64", true);
  RequireSchemaField("syscall_us", "uint64", true);
  RequireSchemaField("wait_us", "uint64", true);
  for (const std::string field :
       {"parser_lowering",
        "sbps_listener",
        "sblr_dispatch",
        "internal_api",
        "executor",
        "storage",
        "index_layer",
        "transaction",
        "result_rendering",
        "evidence_construction",
        "allocation",
        "syscall",
        "wait"}) {
    RequireSchemaField(field + "_measurement_source", "enum", true);
    RequireSchemaField(field + "_measurement_quality", "enum", true);
  }
  RequireSchemaField("statistics_epoch", "uint64", true);
  RequireSchemaField("resource_governor_state", "string", true);
  RequireSchemaField("message_vector_present", "bool", true);
  RequireSchemaField("message_vector_hash", "string", false);
  RequireSchemaField("result_hash", "string", true);
}

void TestValidation() {
  const auto valid = api::ValidatePerformanceMetricEvent(CompleteEvent());
  Require(valid.ok, "complete performance metric event failed validation");

  auto missing_route = CompleteEvent();
  missing_route.route.clear();
  const auto route_validation = api::ValidatePerformanceMetricEvent(missing_route);
  Require(!route_validation.ok, "missing route was accepted");
  Require(MissingField(route_validation, "route"), "missing route was not reported");

  auto missing_parse = CompleteEvent();
  missing_parse.phase_timings.parse_us.reset();
  const auto parse_validation = api::ValidatePerformanceMetricEvent(missing_parse);
  Require(!parse_validation.ok, "missing parse timing was accepted");
  Require(MissingField(parse_validation, "parse_us"), "missing parse timing was not reported");

  auto missing_cache_flag = CompleteEvent();
  missing_cache_flag.cache_flags.plan_cache_hit.reset();
  const auto cache_validation = api::ValidatePerformanceMetricEvent(missing_cache_flag);
  Require(!cache_validation.ok, "missing cache hit flag was accepted");
  Require(MissingField(cache_validation, "plan_cache_hit"),
          "missing cache hit flag was not reported");

  auto missing_hot_path_attribution = CompleteEvent();
  missing_hot_path_attribution.hot_path_attribution.cpu_sample_count.reset();
  const auto hot_path_validation =
      api::ValidatePerformanceMetricEvent(missing_hot_path_attribution);
  Require(!hot_path_validation.ok, "missing hot-path attribution was accepted");
  Require(MissingField(hot_path_validation, "cpu_sample_count"),
          "missing hot-path attribution was not reported");

  auto diagnostic_without_vector_hash = CompleteEvent();
  diagnostic_without_vector_hash.message_vector_present = true;
  diagnostic_without_vector_hash.message_vector_hash.clear();
  const auto vector_validation =
      api::ValidatePerformanceMetricEvent(diagnostic_without_vector_hash);
  Require(!vector_validation.ok, "diagnostic event without message vector hash was accepted");
  Require(MissingField(vector_validation, "message_vector_hash"),
          "missing message vector hash was not reported");

  auto diagnostic_with_vector_hash = CompleteEvent();
  diagnostic_with_vector_hash.message_vector_present = true;
  diagnostic_with_vector_hash.message_vector_hash = "sha256:message-vector-fixture";
  Require(api::ValidatePerformanceMetricEvent(diagnostic_with_vector_hash).ok,
          "diagnostic event with message vector hash was refused");
}

void TestSerialization() {
  const auto event = CompleteEvent();
  const auto json = api::SerializePerformanceMetricEventJson(event);
  Require(Contains(json, "\"schema_version\":3"), "serialized event missing schema version");
  Require(Contains(json, "\"schema_id\":\"scratchbird.performance_metric_event.v3\""),
          "serialized event missing schema id");
  Require(Contains(json, "\"route\":\"embedded_ipc\""), "serialized event missing route");
  Require(Contains(json, "\"operation\":\"copy_insert_batch\""),
          "serialized event missing operation");
  Require(Contains(json, "\"instrumentation_overhead_mode\":\"benchmark_clean\""),
          "serialized event missing default benchmark-clean mode");
  Require(Contains(json, "\"parse_us\":11"), "serialized event missing parse timing");
  Require(Contains(json, "\"append_us\":21"), "serialized event missing append timing");
  Require(Contains(json,
                   "\"phase_timings_measurement_source\":\"measured_by_internal_counter\""),
          "serialized event missing phase timing measurement source");
  Require(Contains(json, "\"storage_timings_measurement_quality\":\"measured\""),
          "serialized event missing storage timing measurement quality");
  Require(Contains(json, "\"agent_thread_count\":2"),
          "serialized event missing agent thread counter");
  Require(Contains(json,
                   "\"agent_counters_measurement_source\":\"measured_by_platform_api\""),
          "serialized event missing agent counter measurement source");
  Require(Contains(json, "\"plan_cache_hit\":true"), "serialized event missing cache flag");
  Require(Contains(json, "\"cache_flags_measurement_quality\":\"measured\""),
          "serialized event missing cache flag measurement quality");
  Require(Contains(json, "\"cpu_sample_attribution\":\"symbolized_stack_bucket\""),
          "serialized event missing CPU sample attribution");
  Require(Contains(json, "\"cpu_sample_measurement_source\":\"measured_by_perf_sample\""),
          "serialized event missing CPU sample measurement source");
  Require(Contains(json,
                   "\"page_fault_count_measurement_quality\":\"actual_zero\""),
          "serialized event missing page-fault measurement quality");
  Require(Contains(json, "\"allocator_allocation_count\":7"),
          "serialized event missing allocator count");
  Require(Contains(json,
                   "\"allocator_counter_measurement_source\":\"measured_by_internal_counter\""),
          "serialized event missing allocator counter measurement source");
  Require(Contains(json, "\"allocator_allocation_bytes\":2048"),
          "serialized event missing allocator bytes");
  Require(Contains(json, "\"lock_latch_wait_count\":3"),
          "serialized event missing lock/latch wait count");
  Require(Contains(json, "\"syscall_count\":9"), "serialized event missing syscall count");
  Require(Contains(json, "\"file_open_count\":1"),
          "serialized event missing file open count");
  Require(Contains(json, "\"file_flush_count\":2"),
          "serialized event missing file flush count");
  Require(Contains(json, "\"file_fsync_count\":1"),
          "serialized event missing file fsync count");
  Require(Contains(json,
                   "\"file_io_count_measurement_source\":\"measured_by_platform_api\""),
          "serialized event missing file IO measurement source");
  Require(Contains(json, "\"page_fault_count\":0"),
          "serialized event missing page fault count");
  Require(Contains(json, "\"context_switch_count\":4"),
          "serialized event missing context switch count");
  Require(Contains(json, "\"evidence_rendering_us\":5"),
          "serialized event missing evidence rendering cost");
  Require(Contains(json, "\"result_formatting_us\":6"),
          "serialized event missing result formatting cost");
  Require(Contains(json, "\"regression_budget_validated\":true"),
          "serialized event missing regression budget validation");
  Require(Contains(json,
                   "\"regression_budget_measurement_quality\":\"estimated\""),
          "serialized event missing regression budget measurement quality");
  Require(Contains(json, "\"index_layer_us\":23"),
          "serialized event missing index-layer attribution");
  Require(Contains(json, "\"sblr_dispatch_us\":8"),
          "serialized event missing SBLR dispatch attribution");
  Require(Contains(json, "\"transaction_measurement_source\":\"estimated\""),
          "serialized event missing transaction measurement source");
  Require(Contains(json, "\"transaction_measurement_quality\":\"estimated\""),
          "serialized event missing transaction measurement quality");
  Require(Contains(json, "\"wait_measurement_source\":\"measured_by_internal_counter\""),
          "serialized event missing wait measurement source");
  Require(Contains(json, "\"wait_measurement_quality\":\"measured\""),
          "serialized event missing wait measurement quality");
  Require(Contains(json, "\"statistics_epoch\":42"),
          "serialized event missing statistics epoch");
  Require(Contains(json, "\"resource_governor_state\":\"foreground_unthrottled\""),
          "serialized event missing resource governor state");
  Require(Contains(json, "\"message_vector_hash\":null"),
          "serialized event should keep absent message vector hash explicit");
  Require(Contains(json, "\"result_hash\":\"sha256:result-fixture\""),
          "serialized event missing result hash");
}

void TestInstrumentationOverheadModes() {
  const auto names = api::InstrumentationOverheadModeNames();
  Require(names.size() == 4, "instrumentation overhead mode count mismatch");
  Require(ContainsName(names, "benchmark_clean"), "benchmark_clean mode missing");
  Require(ContainsName(names, "diagnostic_light"), "diagnostic_light mode missing");
  Require(ContainsName(names, "diagnostic_full"), "diagnostic_full mode missing");
  Require(ContainsName(names, "support_bundle"), "support_bundle mode missing");

  Require(api::DefaultInstrumentationOverheadMode() ==
              api::InstrumentationOverheadMode::kBenchmarkClean,
          "default instrumentation overhead mode is not benchmark_clean");
  Require(std::string(api::InstrumentationOverheadModeName(
              api::DefaultInstrumentationOverheadMode())) == "benchmark_clean",
          "default instrumentation overhead mode name mismatch");

  api::InstrumentationOverheadMode parsed = api::InstrumentationOverheadMode::kDiagnosticFull;
  Require(api::TryParseInstrumentationOverheadMode("diagnostic_light", &parsed),
          "diagnostic_light mode did not parse");
  Require(parsed == api::InstrumentationOverheadMode::kDiagnosticLight,
          "diagnostic_light parsed to wrong mode");
  Require(!api::TryParseInstrumentationOverheadMode("unknown_mode", &parsed),
          "unknown overhead mode parsed successfully");

  const auto clean =
      api::InstrumentationOverheadPolicyForMode(api::InstrumentationOverheadMode::kBenchmarkClean);
  Require(api::InstrumentationOverheadModeIsBenchmarkClean(clean.mode),
          "benchmark_clean was not treated as benchmark clean");
  Require(clean.benchmark_clean_eligible, "benchmark_clean policy is not benchmark eligible");
  Require(!clean.agent_cpu_thread_counters_enabled,
          "benchmark_clean enabled diagnostic agent counters");
  Require(!clean.hot_path_string_formatting_enabled,
          "benchmark_clean enabled hot-path string formatting");

  const auto full =
      api::InstrumentationOverheadPolicyForMode(api::InstrumentationOverheadMode::kDiagnosticFull);
  Require(!api::InstrumentationOverheadModeIsBenchmarkClean(full.mode),
          "diagnostic_full was treated as benchmark clean");
  Require(!full.benchmark_clean_eligible, "diagnostic_full policy is benchmark eligible");
  Require(full.route_phase_timing_enabled, "diagnostic_full missing route phase timing");
  Require(full.append_page_index_timing_enabled,
          "diagnostic_full missing append/page/index timing");
  Require(full.agent_cpu_thread_counters_enabled,
          "diagnostic_full missing agent CPU/thread counters");

  const auto support =
      api::InstrumentationOverheadPolicyForMode(api::InstrumentationOverheadMode::kSupportBundle);
  Require(support.support_bundle_summary_enabled,
          "support_bundle mode did not enable support bundle summary");
  Require(!support.hot_path_string_formatting_enabled,
          "support_bundle mode enabled hot-path string formatting");
}

}  // namespace

int main() {
  TestSchemaFields();
  TestValidation();
  TestSerialization();
  TestInstrumentationOverheadModes();
  return EXIT_SUCCESS;
}
