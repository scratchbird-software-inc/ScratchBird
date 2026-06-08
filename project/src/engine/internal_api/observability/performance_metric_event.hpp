// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: CDP_PERF_METRICS
// SEARCH_KEY: ENTERPRISE_EVENT_SCHEMA
// Engine-owned DML performance metric event schema. This is observability-only:
// it records route/counter evidence and does not own parser, transaction, or
// storage mutation authority.

enum class InstrumentationOverheadMode {
  kBenchmarkClean,
  kDiagnosticLight,
  kDiagnosticFull,
  kSupportBundle
};

struct InstrumentationOverheadPolicy {
  InstrumentationOverheadMode mode = InstrumentationOverheadMode::kBenchmarkClean;
  bool benchmark_clean_eligible = true;
  bool route_phase_timing_enabled = false;
  bool append_page_index_timing_enabled = false;
  bool agent_cpu_thread_counters_enabled = false;
  bool support_bundle_summary_enabled = false;
  bool hot_path_string_formatting_enabled = false;
};

struct PerformanceMetricSchemaField {
  std::string name;
  std::string type;
  bool required = true;
  std::string condition;
};

struct PerformanceMetricPhaseTimings {
  std::optional<EngineApiU64> parse_us;
  std::optional<EngineApiU64> bind_us;
  std::optional<EngineApiU64> lower_us;
  std::optional<EngineApiU64> plan_us;
  std::optional<EngineApiU64> execute_us;
  std::string measurement_source;
  std::string measurement_quality;
};

struct PerformanceMetricStorageTimings {
  std::optional<EngineApiU64> append_us;
  std::optional<EngineApiU64> page_us;
  std::optional<EngineApiU64> index_us;
  std::string measurement_source;
  std::string measurement_quality;
};

struct PerformanceMetricAgentCounters {
  std::optional<EngineApiU64> agent_thread_count;
  std::optional<EngineApiU64> agent_cpu_user_us;
  std::optional<EngineApiU64> agent_cpu_system_us;
  std::optional<EngineApiU64> agent_wait_us;
  std::optional<EngineApiU64> agent_io_read_bytes;
  std::optional<EngineApiU64> agent_io_write_bytes;
  std::string measurement_source;
  std::string measurement_quality;
};

struct PerformanceMetricCacheFlags {
  std::optional<bool> plan_cache_hit;
  std::optional<bool> metadata_cache_hit;
  std::optional<bool> page_cache_hit;
  std::optional<bool> index_cache_hit;
  std::string measurement_source;
  std::string measurement_quality;
};

// SEARCH_KEY: ODFR_HOT_PATH_ATTRIBUTION_SCHEMA
struct PerformanceMetricHotPathAttribution {
  std::optional<EngineApiU64> cpu_sample_count;
  std::optional<EngineApiU64> cpu_sample_attributed_count;
  std::string cpu_sample_attribution;
  std::optional<EngineApiU64> allocator_allocation_count;
  std::optional<EngineApiU64> allocator_allocation_bytes;
  std::optional<EngineApiU64> lock_latch_wait_count;
  std::optional<EngineApiU64> lock_latch_wait_us;
  std::optional<EngineApiU64> syscall_count;
  std::optional<EngineApiU64> file_open_count;
  std::optional<EngineApiU64> file_flush_count;
  std::optional<EngineApiU64> file_fsync_count;
  std::optional<EngineApiU64> page_fault_count;
  std::optional<EngineApiU64> context_switch_count;
  std::optional<EngineApiU64> evidence_rendering_us;
  std::optional<EngineApiU64> result_formatting_us;
  std::optional<EngineApiU64> regression_budget_us;
  std::optional<EngineApiU64> regression_budget_margin_us;
  std::optional<bool> regression_budget_validated;
  std::string cpu_sample_measurement_source;
  std::string cpu_sample_measurement_quality;
  std::string allocator_counter_measurement_source;
  std::string allocator_counter_measurement_quality;
  std::string lock_latch_wait_measurement_source;
  std::string lock_latch_wait_measurement_quality;
  std::string syscall_count_measurement_source;
  std::string syscall_count_measurement_quality;
  std::string file_io_count_measurement_source;
  std::string file_io_count_measurement_quality;
  std::string page_fault_count_measurement_source;
  std::string page_fault_count_measurement_quality;
  std::string context_switch_count_measurement_source;
  std::string context_switch_count_measurement_quality;
  std::string evidence_rendering_measurement_source;
  std::string evidence_rendering_measurement_quality;
  std::string result_formatting_measurement_source;
  std::string result_formatting_measurement_quality;
  std::string regression_budget_measurement_source;
  std::string regression_budget_measurement_quality;
  // SEARCH_KEY: ORH_WHOLE_ROUTE_PROFILER_ATTRIBUTION
  // Whole-route profiler costs are attribution evidence only. They do not own
  // parser, storage, or transaction authority.
  std::optional<EngineApiU64> parser_lowering_us;
  std::optional<EngineApiU64> sbps_listener_us;
  std::optional<EngineApiU64> sblr_dispatch_us;
  std::optional<EngineApiU64> internal_api_us;
  std::optional<EngineApiU64> executor_us;
  std::optional<EngineApiU64> storage_us;
  std::optional<EngineApiU64> index_layer_us;
  std::optional<EngineApiU64> transaction_us;
  std::optional<EngineApiU64> result_rendering_us;
  std::optional<EngineApiU64> evidence_construction_us;
  std::optional<EngineApiU64> allocation_us;
  std::optional<EngineApiU64> syscall_us;
  std::optional<EngineApiU64> wait_us;
  std::string parser_lowering_measurement_source;
  std::string sbps_listener_measurement_source;
  std::string sblr_dispatch_measurement_source;
  std::string internal_api_measurement_source;
  std::string executor_measurement_source;
  std::string storage_measurement_source;
  std::string index_layer_measurement_source;
  std::string transaction_measurement_source;
  std::string result_rendering_measurement_source;
  std::string evidence_construction_measurement_source;
  std::string allocation_measurement_source;
  std::string syscall_measurement_source;
  std::string wait_measurement_source;
  std::string parser_lowering_measurement_quality;
  std::string sbps_listener_measurement_quality;
  std::string sblr_dispatch_measurement_quality;
  std::string internal_api_measurement_quality;
  std::string executor_measurement_quality;
  std::string storage_measurement_quality;
  std::string index_layer_measurement_quality;
  std::string transaction_measurement_quality;
  std::string result_rendering_measurement_quality;
  std::string evidence_construction_measurement_quality;
  std::string allocation_measurement_quality;
  std::string syscall_measurement_quality;
  std::string wait_measurement_quality;
};

struct PerformanceMetricEvent {
  std::string route;
  std::string operation;
  PerformanceMetricPhaseTimings phase_timings;
  PerformanceMetricStorageTimings storage_timings;
  PerformanceMetricAgentCounters agent_counters;
  PerformanceMetricCacheFlags cache_flags;
  PerformanceMetricHotPathAttribution hot_path_attribution;
  std::optional<EngineApiU64> statistics_epoch;
  std::string resource_governor_state;
  std::optional<bool> message_vector_present;
  std::string message_vector_hash;
  std::string result_hash;
  InstrumentationOverheadMode overhead_mode = InstrumentationOverheadMode::kBenchmarkClean;
};

struct PerformanceMetricValidationResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string detail;
  std::vector<std::string> missing_fields;
};

EngineApiU64 PerformanceMetricEventSchemaVersion();
const std::vector<PerformanceMetricSchemaField>& PerformanceMetricEventSchema();

InstrumentationOverheadMode DefaultInstrumentationOverheadMode();
const char* InstrumentationOverheadModeName(InstrumentationOverheadMode mode);
std::vector<std::string> InstrumentationOverheadModeNames();
bool TryParseInstrumentationOverheadMode(std::string_view name,
                                         InstrumentationOverheadMode* mode);
bool InstrumentationOverheadModeIsBenchmarkClean(InstrumentationOverheadMode mode);
InstrumentationOverheadPolicy InstrumentationOverheadPolicyForMode(
    InstrumentationOverheadMode mode);

PerformanceMetricValidationResult ValidatePerformanceMetricEvent(
    const PerformanceMetricEvent& event);
std::string SerializePerformanceMetricEventJson(const PerformanceMetricEvent& event);

}  // namespace scratchbird::engine::internal_api
