// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "observability/performance_metric_event.hpp"

#include <sstream>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

bool IsValidMode(InstrumentationOverheadMode mode) {
  switch (mode) {
    case InstrumentationOverheadMode::kBenchmarkClean:
    case InstrumentationOverheadMode::kDiagnosticLight:
    case InstrumentationOverheadMode::kDiagnosticFull:
    case InstrumentationOverheadMode::kSupportBundle:
      return true;
  }
  return false;
}

void AddMissing(std::vector<std::string>* missing, std::string field) {
  if (missing != nullptr) {
    missing->push_back(std::move(field));
  }
}

void RequireString(const std::string& value,
                   const std::string& field,
                   std::vector<std::string>* missing) {
  if (value.empty()) {
    AddMissing(missing, field);
  }
}

bool IsValidMeasurementSource(const std::string& value) {
  return value == "measured_by_internal_counter" ||
         value == "measured_by_perf_sample" ||
         value == "measured_by_platform_api" ||
         value == "estimated" || value == "not_available_zeroed" ||
         value == "disabled" || value == "unsupported";
}

bool IsValidMeasurementQuality(const std::string& value) {
  return value == "measured" || value == "estimated" ||
         value == "actual_zero" || value == "not_available_zeroed" ||
         value == "disabled" || value == "unsupported";
}

void RequireSource(const std::string& value,
                   const std::string& field,
                   std::vector<std::string>* missing) {
  if (!IsValidMeasurementSource(value)) {
    AddMissing(missing, field);
  }
}

void RequireQuality(const std::string& value,
                    const std::string& field,
                    std::vector<std::string>* missing) {
  if (!IsValidMeasurementQuality(value)) {
    AddMissing(missing, field);
  }
}

template <typename TValue>
void RequirePresent(const std::optional<TValue>& value,
                    const std::string& field,
                    std::vector<std::string>* missing) {
  if (!value.has_value()) {
    AddMissing(missing, field);
  }
}

std::string JsonEscape(std::string_view value) {
  std::ostringstream out;
  for (const char ch : value) {
    switch (ch) {
      case '"':
        out << "\\\"";
        break;
      case '\\':
        out << "\\\\";
        break;
      case '\b':
        out << "\\b";
        break;
      case '\f':
        out << "\\f";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          out << "\\u00";
          static constexpr char kHex[] = "0123456789abcdef";
          out << kHex[(static_cast<unsigned char>(ch) >> 4) & 0x0f];
          out << kHex[static_cast<unsigned char>(ch) & 0x0f];
        } else {
          out << ch;
        }
        break;
    }
  }
  return out.str();
}

void AddJsonString(std::ostringstream* out,
                   bool* first,
                   std::string_view key,
                   std::string_view value) {
  if (!*first) {
    *out << ',';
  }
  *first = false;
  *out << '"' << key << "\":\"" << JsonEscape(value) << '"';
}

void AddJsonNullableString(std::ostringstream* out,
                           bool* first,
                           std::string_view key,
                           std::string_view value) {
  if (!*first) {
    *out << ',';
  }
  *first = false;
  *out << '"' << key << "\":";
  if (value.empty()) {
    *out << "null";
  } else {
    *out << '"' << JsonEscape(value) << '"';
  }
}

void AddJsonU64(std::ostringstream* out,
                bool* first,
                std::string_view key,
                std::optional<EngineApiU64> value) {
  if (!*first) {
    *out << ',';
  }
  *first = false;
  *out << '"' << key << "\":";
  if (value.has_value()) {
    *out << *value;
  } else {
    *out << "null";
  }
}

void AddJsonBool(std::ostringstream* out,
                 bool* first,
                 std::string_view key,
                 std::optional<bool> value) {
  if (!*first) {
    *out << ',';
  }
  *first = false;
  *out << '"' << key << "\":";
  if (value.has_value()) {
    *out << (*value ? "true" : "false");
  } else {
    *out << "null";
  }
}

}  // namespace

EngineApiU64 PerformanceMetricEventSchemaVersion() {
  return 3;
}

const std::vector<PerformanceMetricSchemaField>& PerformanceMetricEventSchema() {
  static const std::vector<PerformanceMetricSchemaField> kSchema = {
      {"route", "string", true, ""},
      {"operation", "string", true, ""},
      {"instrumentation_overhead_mode", "enum", true, ""},
      {"parse_us", "uint64", true, ""},
      {"bind_us", "uint64", true, ""},
      {"lower_us", "uint64", true, ""},
      {"plan_us", "uint64", true, ""},
      {"execute_us", "uint64", true, ""},
      {"phase_timings_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"phase_timings_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"append_us", "uint64", true, ""},
      {"page_us", "uint64", true, ""},
      {"index_us", "uint64", true, ""},
      {"storage_timings_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"storage_timings_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"agent_thread_count", "uint64", true, ""},
      {"agent_cpu_user_us", "uint64", true, ""},
      {"agent_cpu_system_us", "uint64", true, ""},
      {"agent_wait_us", "uint64", true, ""},
      {"agent_io_read_bytes", "uint64", true, ""},
      {"agent_io_write_bytes", "uint64", true, ""},
      {"agent_counters_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"agent_counters_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"plan_cache_hit", "bool", true, ""},
      {"metadata_cache_hit", "bool", true, ""},
      {"page_cache_hit", "bool", true, ""},
      {"index_cache_hit", "bool", true, ""},
      {"cache_flags_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"cache_flags_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"cpu_sample_count", "uint64", true, ""},
      {"cpu_sample_attributed_count", "uint64", true, ""},
      {"cpu_sample_attribution", "string", true, ""},
      {"cpu_sample_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"cpu_sample_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"allocator_allocation_count", "uint64", true, ""},
      {"allocator_allocation_bytes", "uint64", true, ""},
      {"allocator_counter_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"allocator_counter_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"lock_latch_wait_count", "uint64", true, ""},
      {"lock_latch_wait_us", "uint64", true, ""},
      {"lock_latch_wait_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"lock_latch_wait_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"syscall_count", "uint64", true, ""},
      {"syscall_count_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"syscall_count_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"file_open_count", "uint64", true, ""},
      {"file_flush_count", "uint64", true, ""},
      {"file_fsync_count", "uint64", true, ""},
      {"file_io_count_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"file_io_count_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"page_fault_count", "uint64", true, ""},
      {"page_fault_count_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"page_fault_count_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"context_switch_count", "uint64", true, ""},
      {"context_switch_count_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"context_switch_count_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"evidence_rendering_us", "uint64", true, ""},
      {"evidence_rendering_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"evidence_rendering_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"result_formatting_us", "uint64", true, ""},
      {"result_formatting_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"result_formatting_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"regression_budget_us", "uint64", true, ""},
      {"regression_budget_margin_us", "uint64", true, ""},
      {"regression_budget_validated", "bool", true, ""},
      {"regression_budget_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"regression_budget_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"parser_lowering_us", "uint64", true, ""},
      {"sbps_listener_us", "uint64", true, ""},
      {"sblr_dispatch_us", "uint64", true, ""},
      {"internal_api_us", "uint64", true, ""},
      {"executor_us", "uint64", true, ""},
      {"storage_us", "uint64", true, ""},
      {"index_layer_us", "uint64", true, ""},
      {"transaction_us", "uint64", true, ""},
      {"result_rendering_us", "uint64", true, ""},
      {"evidence_construction_us", "uint64", true, ""},
      {"allocation_us", "uint64", true, ""},
      {"syscall_us", "uint64", true, ""},
      {"wait_us", "uint64", true, ""},
      {"parser_lowering_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"sbps_listener_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"sblr_dispatch_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"internal_api_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"executor_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"storage_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"index_layer_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"transaction_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"result_rendering_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"evidence_construction_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"allocation_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"syscall_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"wait_measurement_source", "enum", true, "measured_by_internal_counter|measured_by_perf_sample|measured_by_platform_api|estimated|not_available_zeroed|disabled|unsupported"},
      {"parser_lowering_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"sbps_listener_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"sblr_dispatch_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"internal_api_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"executor_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"storage_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"index_layer_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"transaction_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"result_rendering_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"evidence_construction_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"allocation_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"syscall_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"wait_measurement_quality", "enum", true, "measured|estimated|actual_zero|not_available_zeroed|disabled|unsupported"},
      {"statistics_epoch", "uint64", true, ""},
      {"resource_governor_state", "string", true, ""},
      {"message_vector_present", "bool", true, ""},
      {"message_vector_hash", "string", false, "required when message_vector_present is true"},
      {"result_hash", "string", true, ""}};
  return kSchema;
}

InstrumentationOverheadMode DefaultInstrumentationOverheadMode() {
  return InstrumentationOverheadMode::kBenchmarkClean;
}

const char* InstrumentationOverheadModeName(InstrumentationOverheadMode mode) {
  switch (mode) {
    case InstrumentationOverheadMode::kBenchmarkClean:
      return "benchmark_clean";
    case InstrumentationOverheadMode::kDiagnosticLight:
      return "diagnostic_light";
    case InstrumentationOverheadMode::kDiagnosticFull:
      return "diagnostic_full";
    case InstrumentationOverheadMode::kSupportBundle:
      return "support_bundle";
  }
  return "unknown";
}

std::vector<std::string> InstrumentationOverheadModeNames() {
  return {"benchmark_clean", "diagnostic_light", "diagnostic_full", "support_bundle"};
}

bool TryParseInstrumentationOverheadMode(std::string_view name,
                                         InstrumentationOverheadMode* mode) {
  if (mode == nullptr) {
    return false;
  }
  if (name == "benchmark_clean") {
    *mode = InstrumentationOverheadMode::kBenchmarkClean;
    return true;
  }
  if (name == "diagnostic_light") {
    *mode = InstrumentationOverheadMode::kDiagnosticLight;
    return true;
  }
  if (name == "diagnostic_full") {
    *mode = InstrumentationOverheadMode::kDiagnosticFull;
    return true;
  }
  if (name == "support_bundle") {
    *mode = InstrumentationOverheadMode::kSupportBundle;
    return true;
  }
  return false;
}

bool InstrumentationOverheadModeIsBenchmarkClean(InstrumentationOverheadMode mode) {
  return mode == InstrumentationOverheadMode::kBenchmarkClean;
}

InstrumentationOverheadPolicy InstrumentationOverheadPolicyForMode(
    InstrumentationOverheadMode mode) {
  InstrumentationOverheadPolicy policy;
  policy.mode = mode;
  switch (mode) {
    case InstrumentationOverheadMode::kBenchmarkClean:
      policy.benchmark_clean_eligible = true;
      policy.route_phase_timing_enabled = false;
      policy.append_page_index_timing_enabled = false;
      policy.agent_cpu_thread_counters_enabled = false;
      policy.support_bundle_summary_enabled = false;
      policy.hot_path_string_formatting_enabled = false;
      return policy;
    case InstrumentationOverheadMode::kDiagnosticLight:
      policy.benchmark_clean_eligible = false;
      policy.route_phase_timing_enabled = true;
      policy.append_page_index_timing_enabled = false;
      policy.agent_cpu_thread_counters_enabled = false;
      policy.support_bundle_summary_enabled = false;
      policy.hot_path_string_formatting_enabled = false;
      return policy;
    case InstrumentationOverheadMode::kDiagnosticFull:
      policy.benchmark_clean_eligible = false;
      policy.route_phase_timing_enabled = true;
      policy.append_page_index_timing_enabled = true;
      policy.agent_cpu_thread_counters_enabled = true;
      policy.support_bundle_summary_enabled = false;
      policy.hot_path_string_formatting_enabled = true;
      return policy;
    case InstrumentationOverheadMode::kSupportBundle:
      policy.benchmark_clean_eligible = false;
      policy.route_phase_timing_enabled = false;
      policy.append_page_index_timing_enabled = false;
      policy.agent_cpu_thread_counters_enabled = false;
      policy.support_bundle_summary_enabled = true;
      policy.hot_path_string_formatting_enabled = false;
      return policy;
  }
  policy.benchmark_clean_eligible = false;
  return policy;
}

PerformanceMetricValidationResult ValidatePerformanceMetricEvent(
    const PerformanceMetricEvent& event) {
  PerformanceMetricValidationResult result;
  result.diagnostic_code = "CDP.PERFORMANCE_METRIC_EVENT.OK";

  RequireString(event.route, "route", &result.missing_fields);
  RequireString(event.operation, "operation", &result.missing_fields);
  if (!IsValidMode(event.overhead_mode)) {
    AddMissing(&result.missing_fields, "instrumentation_overhead_mode");
  }
  RequirePresent(event.phase_timings.parse_us, "parse_us", &result.missing_fields);
  RequirePresent(event.phase_timings.bind_us, "bind_us", &result.missing_fields);
  RequirePresent(event.phase_timings.lower_us, "lower_us", &result.missing_fields);
  RequirePresent(event.phase_timings.plan_us, "plan_us", &result.missing_fields);
  RequirePresent(event.phase_timings.execute_us, "execute_us", &result.missing_fields);
  RequireSource(event.phase_timings.measurement_source,
                "phase_timings_measurement_source",
                &result.missing_fields);
  RequireQuality(event.phase_timings.measurement_quality,
                 "phase_timings_measurement_quality",
                 &result.missing_fields);
  RequirePresent(event.storage_timings.append_us, "append_us", &result.missing_fields);
  RequirePresent(event.storage_timings.page_us, "page_us", &result.missing_fields);
  RequirePresent(event.storage_timings.index_us, "index_us", &result.missing_fields);
  RequireSource(event.storage_timings.measurement_source,
                "storage_timings_measurement_source",
                &result.missing_fields);
  RequireQuality(event.storage_timings.measurement_quality,
                 "storage_timings_measurement_quality",
                 &result.missing_fields);
  RequirePresent(event.agent_counters.agent_thread_count,
                 "agent_thread_count",
                 &result.missing_fields);
  RequirePresent(event.agent_counters.agent_cpu_user_us,
                 "agent_cpu_user_us",
                 &result.missing_fields);
  RequirePresent(event.agent_counters.agent_cpu_system_us,
                 "agent_cpu_system_us",
                 &result.missing_fields);
  RequirePresent(event.agent_counters.agent_wait_us,
                 "agent_wait_us",
                 &result.missing_fields);
  RequirePresent(event.agent_counters.agent_io_read_bytes,
                 "agent_io_read_bytes",
                 &result.missing_fields);
  RequirePresent(event.agent_counters.agent_io_write_bytes,
                 "agent_io_write_bytes",
                 &result.missing_fields);
  RequireSource(event.agent_counters.measurement_source,
                "agent_counters_measurement_source",
                &result.missing_fields);
  RequireQuality(event.agent_counters.measurement_quality,
                 "agent_counters_measurement_quality",
                 &result.missing_fields);
  RequirePresent(event.cache_flags.plan_cache_hit, "plan_cache_hit", &result.missing_fields);
  RequirePresent(event.cache_flags.metadata_cache_hit,
                 "metadata_cache_hit",
                 &result.missing_fields);
  RequirePresent(event.cache_flags.page_cache_hit, "page_cache_hit", &result.missing_fields);
  RequirePresent(event.cache_flags.index_cache_hit, "index_cache_hit", &result.missing_fields);
  RequireSource(event.cache_flags.measurement_source,
                "cache_flags_measurement_source",
                &result.missing_fields);
  RequireQuality(event.cache_flags.measurement_quality,
                 "cache_flags_measurement_quality",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.cpu_sample_count,
                 "cpu_sample_count",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.cpu_sample_attributed_count,
                 "cpu_sample_attributed_count",
                 &result.missing_fields);
  RequireString(event.hot_path_attribution.cpu_sample_attribution,
                "cpu_sample_attribution",
                &result.missing_fields);
  RequireSource(event.hot_path_attribution.cpu_sample_measurement_source,
                "cpu_sample_measurement_source",
                &result.missing_fields);
  RequireQuality(event.hot_path_attribution.cpu_sample_measurement_quality,
                 "cpu_sample_measurement_quality",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.allocator_allocation_count,
                 "allocator_allocation_count",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.allocator_allocation_bytes,
                 "allocator_allocation_bytes",
                 &result.missing_fields);
  RequireSource(event.hot_path_attribution.allocator_counter_measurement_source,
                "allocator_counter_measurement_source",
                &result.missing_fields);
  RequireQuality(event.hot_path_attribution.allocator_counter_measurement_quality,
                 "allocator_counter_measurement_quality",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.lock_latch_wait_count,
                 "lock_latch_wait_count",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.lock_latch_wait_us,
                 "lock_latch_wait_us",
                 &result.missing_fields);
  RequireSource(event.hot_path_attribution.lock_latch_wait_measurement_source,
                "lock_latch_wait_measurement_source",
                &result.missing_fields);
  RequireQuality(event.hot_path_attribution.lock_latch_wait_measurement_quality,
                 "lock_latch_wait_measurement_quality",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.syscall_count,
                 "syscall_count",
                 &result.missing_fields);
  RequireSource(event.hot_path_attribution.syscall_count_measurement_source,
                "syscall_count_measurement_source",
                &result.missing_fields);
  RequireQuality(event.hot_path_attribution.syscall_count_measurement_quality,
                 "syscall_count_measurement_quality",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.file_open_count,
                 "file_open_count",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.file_flush_count,
                 "file_flush_count",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.file_fsync_count,
                 "file_fsync_count",
                 &result.missing_fields);
  RequireSource(event.hot_path_attribution.file_io_count_measurement_source,
                "file_io_count_measurement_source",
                &result.missing_fields);
  RequireQuality(event.hot_path_attribution.file_io_count_measurement_quality,
                 "file_io_count_measurement_quality",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.page_fault_count,
                 "page_fault_count",
                 &result.missing_fields);
  RequireSource(event.hot_path_attribution.page_fault_count_measurement_source,
                "page_fault_count_measurement_source",
                &result.missing_fields);
  RequireQuality(event.hot_path_attribution.page_fault_count_measurement_quality,
                 "page_fault_count_measurement_quality",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.context_switch_count,
                 "context_switch_count",
                 &result.missing_fields);
  RequireSource(event.hot_path_attribution.context_switch_count_measurement_source,
                "context_switch_count_measurement_source",
                &result.missing_fields);
  RequireQuality(event.hot_path_attribution.context_switch_count_measurement_quality,
                 "context_switch_count_measurement_quality",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.evidence_rendering_us,
                 "evidence_rendering_us",
                 &result.missing_fields);
  RequireSource(event.hot_path_attribution.evidence_rendering_measurement_source,
                "evidence_rendering_measurement_source",
                &result.missing_fields);
  RequireQuality(event.hot_path_attribution.evidence_rendering_measurement_quality,
                 "evidence_rendering_measurement_quality",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.result_formatting_us,
                 "result_formatting_us",
                 &result.missing_fields);
  RequireSource(event.hot_path_attribution.result_formatting_measurement_source,
                "result_formatting_measurement_source",
                &result.missing_fields);
  RequireQuality(event.hot_path_attribution.result_formatting_measurement_quality,
                 "result_formatting_measurement_quality",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.regression_budget_us,
                 "regression_budget_us",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.regression_budget_margin_us,
                 "regression_budget_margin_us",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.regression_budget_validated,
                 "regression_budget_validated",
                 &result.missing_fields);
  RequireSource(event.hot_path_attribution.regression_budget_measurement_source,
                "regression_budget_measurement_source",
                &result.missing_fields);
  RequireQuality(event.hot_path_attribution.regression_budget_measurement_quality,
                 "regression_budget_measurement_quality",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.parser_lowering_us,
                 "parser_lowering_us",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.sbps_listener_us,
                 "sbps_listener_us",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.sblr_dispatch_us,
                 "sblr_dispatch_us",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.internal_api_us,
                 "internal_api_us",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.executor_us,
                 "executor_us",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.storage_us,
                 "storage_us",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.index_layer_us,
                 "index_layer_us",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.transaction_us,
                 "transaction_us",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.result_rendering_us,
                 "result_rendering_us",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.evidence_construction_us,
                 "evidence_construction_us",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.allocation_us,
                 "allocation_us",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.syscall_us,
                 "syscall_us",
                 &result.missing_fields);
  RequirePresent(event.hot_path_attribution.wait_us,
                 "wait_us",
                 &result.missing_fields);
#define REQUIRE_MEASUREMENT_PROVENANCE(field)                                      \
  RequireSource(event.hot_path_attribution.field##_measurement_source,             \
                #field "_measurement_source",                                    \
                &result.missing_fields);                                           \
  RequireQuality(event.hot_path_attribution.field##_measurement_quality,           \
                 #field "_measurement_quality",                                  \
                 &result.missing_fields)
  REQUIRE_MEASUREMENT_PROVENANCE(parser_lowering);
  REQUIRE_MEASUREMENT_PROVENANCE(sbps_listener);
  REQUIRE_MEASUREMENT_PROVENANCE(sblr_dispatch);
  REQUIRE_MEASUREMENT_PROVENANCE(internal_api);
  REQUIRE_MEASUREMENT_PROVENANCE(executor);
  REQUIRE_MEASUREMENT_PROVENANCE(storage);
  REQUIRE_MEASUREMENT_PROVENANCE(index_layer);
  REQUIRE_MEASUREMENT_PROVENANCE(transaction);
  REQUIRE_MEASUREMENT_PROVENANCE(result_rendering);
  REQUIRE_MEASUREMENT_PROVENANCE(evidence_construction);
  REQUIRE_MEASUREMENT_PROVENANCE(allocation);
  REQUIRE_MEASUREMENT_PROVENANCE(syscall);
  REQUIRE_MEASUREMENT_PROVENANCE(wait);
#undef REQUIRE_MEASUREMENT_PROVENANCE
  RequirePresent(event.statistics_epoch, "statistics_epoch", &result.missing_fields);
  RequireString(event.resource_governor_state, "resource_governor_state", &result.missing_fields);
  RequirePresent(event.message_vector_present, "message_vector_present", &result.missing_fields);
  if (event.message_vector_present.value_or(false)) {
    RequireString(event.message_vector_hash, "message_vector_hash", &result.missing_fields);
  }
  RequireString(event.result_hash, "result_hash", &result.missing_fields);

  result.ok = result.missing_fields.empty();
  if (!result.ok) {
    result.diagnostic_code = "CDP.PERFORMANCE_METRIC_EVENT.MISSING_REQUIRED_FIELD";
    std::ostringstream detail;
    detail << "missing required performance metric event fields:";
    for (const auto& field : result.missing_fields) {
      detail << ' ' << field;
    }
    result.detail = detail.str();
  }
  return result;
}

std::string SerializePerformanceMetricEventJson(const PerformanceMetricEvent& event) {
  std::ostringstream out;
  bool first = true;
  out << '{';
  AddJsonU64(&out, &first, "schema_version", PerformanceMetricEventSchemaVersion());
  AddJsonString(&out, &first, "schema_id", "scratchbird.performance_metric_event.v3");
  AddJsonString(&out, &first, "route", event.route);
  AddJsonString(&out, &first, "operation", event.operation);
  AddJsonString(&out,
                &first,
                "instrumentation_overhead_mode",
                InstrumentationOverheadModeName(event.overhead_mode));
  AddJsonU64(&out, &first, "parse_us", event.phase_timings.parse_us);
  AddJsonU64(&out, &first, "bind_us", event.phase_timings.bind_us);
  AddJsonU64(&out, &first, "lower_us", event.phase_timings.lower_us);
  AddJsonU64(&out, &first, "plan_us", event.phase_timings.plan_us);
  AddJsonU64(&out, &first, "execute_us", event.phase_timings.execute_us);
  AddJsonString(&out,
                &first,
                "phase_timings_measurement_source",
                event.phase_timings.measurement_source);
  AddJsonString(&out,
                &first,
                "phase_timings_measurement_quality",
                event.phase_timings.measurement_quality);
  AddJsonU64(&out, &first, "append_us", event.storage_timings.append_us);
  AddJsonU64(&out, &first, "page_us", event.storage_timings.page_us);
  AddJsonU64(&out, &first, "index_us", event.storage_timings.index_us);
  AddJsonString(&out,
                &first,
                "storage_timings_measurement_source",
                event.storage_timings.measurement_source);
  AddJsonString(&out,
                &first,
                "storage_timings_measurement_quality",
                event.storage_timings.measurement_quality);
  AddJsonU64(&out, &first, "agent_thread_count", event.agent_counters.agent_thread_count);
  AddJsonU64(&out, &first, "agent_cpu_user_us", event.agent_counters.agent_cpu_user_us);
  AddJsonU64(&out,
             &first,
             "agent_cpu_system_us",
             event.agent_counters.agent_cpu_system_us);
  AddJsonU64(&out, &first, "agent_wait_us", event.agent_counters.agent_wait_us);
  AddJsonU64(&out, &first, "agent_io_read_bytes", event.agent_counters.agent_io_read_bytes);
  AddJsonU64(&out,
             &first,
             "agent_io_write_bytes",
             event.agent_counters.agent_io_write_bytes);
  AddJsonString(&out,
                &first,
                "agent_counters_measurement_source",
                event.agent_counters.measurement_source);
  AddJsonString(&out,
                &first,
                "agent_counters_measurement_quality",
                event.agent_counters.measurement_quality);
  AddJsonBool(&out, &first, "plan_cache_hit", event.cache_flags.plan_cache_hit);
  AddJsonBool(&out, &first, "metadata_cache_hit", event.cache_flags.metadata_cache_hit);
  AddJsonBool(&out, &first, "page_cache_hit", event.cache_flags.page_cache_hit);
  AddJsonBool(&out, &first, "index_cache_hit", event.cache_flags.index_cache_hit);
  AddJsonString(&out,
                &first,
                "cache_flags_measurement_source",
                event.cache_flags.measurement_source);
  AddJsonString(&out,
                &first,
                "cache_flags_measurement_quality",
                event.cache_flags.measurement_quality);
  AddJsonU64(&out,
             &first,
             "cpu_sample_count",
             event.hot_path_attribution.cpu_sample_count);
  AddJsonU64(&out,
             &first,
             "cpu_sample_attributed_count",
             event.hot_path_attribution.cpu_sample_attributed_count);
  AddJsonString(&out,
                &first,
                "cpu_sample_attribution",
                event.hot_path_attribution.cpu_sample_attribution);
  AddJsonString(&out,
                &first,
                "cpu_sample_measurement_source",
                event.hot_path_attribution.cpu_sample_measurement_source);
  AddJsonString(&out,
                &first,
                "cpu_sample_measurement_quality",
                event.hot_path_attribution.cpu_sample_measurement_quality);
  AddJsonU64(&out,
             &first,
             "allocator_allocation_count",
             event.hot_path_attribution.allocator_allocation_count);
  AddJsonU64(&out,
             &first,
             "allocator_allocation_bytes",
             event.hot_path_attribution.allocator_allocation_bytes);
  AddJsonString(&out,
                &first,
                "allocator_counter_measurement_source",
                event.hot_path_attribution.allocator_counter_measurement_source);
  AddJsonString(&out,
                &first,
                "allocator_counter_measurement_quality",
                event.hot_path_attribution.allocator_counter_measurement_quality);
  AddJsonU64(&out,
             &first,
             "lock_latch_wait_count",
             event.hot_path_attribution.lock_latch_wait_count);
  AddJsonU64(&out,
             &first,
             "lock_latch_wait_us",
             event.hot_path_attribution.lock_latch_wait_us);
  AddJsonString(&out,
                &first,
                "lock_latch_wait_measurement_source",
                event.hot_path_attribution.lock_latch_wait_measurement_source);
  AddJsonString(&out,
                &first,
                "lock_latch_wait_measurement_quality",
                event.hot_path_attribution.lock_latch_wait_measurement_quality);
  AddJsonU64(&out, &first, "syscall_count", event.hot_path_attribution.syscall_count);
  AddJsonString(&out,
                &first,
                "syscall_count_measurement_source",
                event.hot_path_attribution.syscall_count_measurement_source);
  AddJsonString(&out,
                &first,
                "syscall_count_measurement_quality",
                event.hot_path_attribution.syscall_count_measurement_quality);
  AddJsonU64(&out, &first, "file_open_count", event.hot_path_attribution.file_open_count);
  AddJsonU64(&out, &first, "file_flush_count", event.hot_path_attribution.file_flush_count);
  AddJsonU64(&out, &first, "file_fsync_count", event.hot_path_attribution.file_fsync_count);
  AddJsonString(&out,
                &first,
                "file_io_count_measurement_source",
                event.hot_path_attribution.file_io_count_measurement_source);
  AddJsonString(&out,
                &first,
                "file_io_count_measurement_quality",
                event.hot_path_attribution.file_io_count_measurement_quality);
  AddJsonU64(&out, &first, "page_fault_count", event.hot_path_attribution.page_fault_count);
  AddJsonString(&out,
                &first,
                "page_fault_count_measurement_source",
                event.hot_path_attribution.page_fault_count_measurement_source);
  AddJsonString(&out,
                &first,
                "page_fault_count_measurement_quality",
                event.hot_path_attribution.page_fault_count_measurement_quality);
  AddJsonU64(&out,
             &first,
             "context_switch_count",
             event.hot_path_attribution.context_switch_count);
  AddJsonString(&out,
                &first,
                "context_switch_count_measurement_source",
                event.hot_path_attribution.context_switch_count_measurement_source);
  AddJsonString(&out,
                &first,
                "context_switch_count_measurement_quality",
                event.hot_path_attribution.context_switch_count_measurement_quality);
  AddJsonU64(&out,
             &first,
             "evidence_rendering_us",
             event.hot_path_attribution.evidence_rendering_us);
  AddJsonString(&out,
                &first,
                "evidence_rendering_measurement_source",
                event.hot_path_attribution.evidence_rendering_measurement_source);
  AddJsonString(&out,
                &first,
                "evidence_rendering_measurement_quality",
                event.hot_path_attribution.evidence_rendering_measurement_quality);
  AddJsonU64(&out,
             &first,
             "result_formatting_us",
             event.hot_path_attribution.result_formatting_us);
  AddJsonString(&out,
                &first,
                "result_formatting_measurement_source",
                event.hot_path_attribution.result_formatting_measurement_source);
  AddJsonString(&out,
                &first,
                "result_formatting_measurement_quality",
                event.hot_path_attribution.result_formatting_measurement_quality);
  AddJsonU64(&out,
             &first,
             "regression_budget_us",
             event.hot_path_attribution.regression_budget_us);
  AddJsonU64(&out,
             &first,
             "regression_budget_margin_us",
             event.hot_path_attribution.regression_budget_margin_us);
  AddJsonBool(&out,
              &first,
              "regression_budget_validated",
              event.hot_path_attribution.regression_budget_validated);
  AddJsonString(&out,
                &first,
                "regression_budget_measurement_source",
                event.hot_path_attribution.regression_budget_measurement_source);
  AddJsonString(&out,
                &first,
                "regression_budget_measurement_quality",
                event.hot_path_attribution.regression_budget_measurement_quality);
  AddJsonU64(&out, &first, "parser_lowering_us", event.hot_path_attribution.parser_lowering_us);
  AddJsonU64(&out, &first, "sbps_listener_us", event.hot_path_attribution.sbps_listener_us);
  AddJsonU64(&out, &first, "sblr_dispatch_us", event.hot_path_attribution.sblr_dispatch_us);
  AddJsonU64(&out, &first, "internal_api_us", event.hot_path_attribution.internal_api_us);
  AddJsonU64(&out, &first, "executor_us", event.hot_path_attribution.executor_us);
  AddJsonU64(&out, &first, "storage_us", event.hot_path_attribution.storage_us);
  AddJsonU64(&out, &first, "index_layer_us", event.hot_path_attribution.index_layer_us);
  AddJsonU64(&out, &first, "transaction_us", event.hot_path_attribution.transaction_us);
  AddJsonU64(&out, &first, "result_rendering_us", event.hot_path_attribution.result_rendering_us);
  AddJsonU64(&out, &first, "evidence_construction_us", event.hot_path_attribution.evidence_construction_us);
  AddJsonU64(&out, &first, "allocation_us", event.hot_path_attribution.allocation_us);
  AddJsonU64(&out, &first, "syscall_us", event.hot_path_attribution.syscall_us);
  AddJsonU64(&out, &first, "wait_us", event.hot_path_attribution.wait_us);
#define ADD_MEASUREMENT_PROVENANCE(field)                                          \
  AddJsonString(&out,                                                             \
                &first,                                                           \
                #field "_measurement_source",                                    \
                event.hot_path_attribution.field##_measurement_source);           \
  AddJsonString(&out,                                                             \
                &first,                                                           \
                #field "_measurement_quality",                                   \
                event.hot_path_attribution.field##_measurement_quality)
  ADD_MEASUREMENT_PROVENANCE(parser_lowering);
  ADD_MEASUREMENT_PROVENANCE(sbps_listener);
  ADD_MEASUREMENT_PROVENANCE(sblr_dispatch);
  ADD_MEASUREMENT_PROVENANCE(internal_api);
  ADD_MEASUREMENT_PROVENANCE(executor);
  ADD_MEASUREMENT_PROVENANCE(storage);
  ADD_MEASUREMENT_PROVENANCE(index_layer);
  ADD_MEASUREMENT_PROVENANCE(transaction);
  ADD_MEASUREMENT_PROVENANCE(result_rendering);
  ADD_MEASUREMENT_PROVENANCE(evidence_construction);
  ADD_MEASUREMENT_PROVENANCE(allocation);
  ADD_MEASUREMENT_PROVENANCE(syscall);
  ADD_MEASUREMENT_PROVENANCE(wait);
#undef ADD_MEASUREMENT_PROVENANCE
  AddJsonU64(&out, &first, "statistics_epoch", event.statistics_epoch);
  AddJsonString(&out, &first, "resource_governor_state", event.resource_governor_state);
  AddJsonBool(&out, &first, "message_vector_present", event.message_vector_present);
  AddJsonNullableString(&out, &first, "message_vector_hash", event.message_vector_hash);
  AddJsonString(&out, &first, "result_hash", event.result_hash);
  out << '}';
  return out.str();
}

}  // namespace scratchbird::engine::internal_api
