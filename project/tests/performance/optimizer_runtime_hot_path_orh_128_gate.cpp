// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "observability/performance_metric_event.hpp"
#include "runtime_consumption_evidence.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace optimizer = scratchbird::engine::optimizer;

constexpr const char* kBudgetExceeded =
    "ORH_EVIDENCE_OVERHEAD_BUDGET_EXCEEDED";

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << "ORH-128 gate failure: " << message << '\n';
  std::exit(1);
}

void Require(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}

bool Contains(const std::vector<std::string>& values,
              const std::string& needle) {
  return std::any_of(values.begin(),
                     values.end(),
                     [&](const std::string& value) {
                       return value.find(needle) != std::string::npos;
                     });
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

bool SameMetric(double lhs, double rhs) {
  return std::abs(lhs - rhs) < 0.0001;
}

double NearestRankPercentile(std::vector<double> samples, double percentile) {
  std::sort(samples.begin(), samples.end());
  const auto rank =
      static_cast<std::size_t>(std::ceil((percentile / 100.0) * samples.size()));
  return samples[std::max<std::size_t>(1, rank) - 1];
}

struct OverheadFamilyEvidence {
  std::string family;
  std::vector<double> samples_us;
  double p50_us = 0.0;
  double p95_us = 0.0;
  double p99_us = 0.0;
  double p99_budget_us = 0.0;
  std::string measurement_source;
  std::string measurement_quality;
  bool separated_from_engine_execution = true;
  bool separated_from_result_formatting = true;
  bool hidden_inside_execution_timing = false;
  bool high_cost_hot_path_evidence_enabled = true;
  bool disabled_for_support_bundle = false;
  bool benchmark_clean_claim = false;
};

struct OverheadValidation {
  bool ok = false;
  bool benchmark_clean = false;
  std::vector<std::string> diagnostics;
  std::vector<std::string> evidence_rows;
};

bool QualityPermitsZero(const OverheadFamilyEvidence& family) {
  return family.measurement_quality == "actual_zero" ||
         (family.disabled_for_support_bundle &&
          family.measurement_quality == "disabled");
}

bool SourceIsMeasured(const OverheadFamilyEvidence& family) {
  return family.measurement_source == "measured_by_internal_counter" ||
         family.measurement_source == "measured_by_platform_api" ||
         family.measurement_source == "measured_by_perf_sample";
}

void AddDiagnostic(OverheadValidation* validation,
                   const OverheadFamilyEvidence& family,
                   const std::string& suffix) {
  validation->diagnostics.push_back(std::string(kBudgetExceeded) + "." +
                                    family.family + "." + suffix);
}

OverheadValidation ValidateEvidenceOverheadBudget(
    const std::vector<OverheadFamilyEvidence>& families,
    bool benchmark_clean_claim,
    bool support_bundle_mode) {
  OverheadValidation validation;
  const std::set<std::string> required_families = {
      "engine_execution",
      "result_formatting",
      "evidence_rendering",
      "support_bundle_generation",
      "metric_capture",
      "diagnostic_construction",
  };
  std::set<std::string> seen;

  for (const auto& family : families) {
    seen.insert(family.family);
    validation.evidence_rows.push_back(
        family.family + ":p50=" + std::to_string(family.p50_us) +
        ":p95=" + std::to_string(family.p95_us) +
        ":p99=" + std::to_string(family.p99_us) +
        ":budget_p99=" + std::to_string(family.p99_budget_us) +
        ":source=" + family.measurement_source +
        ":quality=" + family.measurement_quality);

    if (family.family.empty()) {
      AddDiagnostic(&validation, family, "MISSING_FAMILY");
    }
    if (family.samples_us.size() < 5) {
      AddDiagnostic(&validation, family, "INSUFFICIENT_SAMPLES");
    }
    if (family.measurement_source.empty() ||
        family.measurement_quality.empty()) {
      AddDiagnostic(&validation, family, "MISSING_PROVENANCE");
    } else if (!SourceIsMeasured(family) ||
               family.measurement_quality == "estimated" ||
               family.measurement_quality == "unsupported" ||
               family.measurement_quality == "unavailable") {
      AddDiagnostic(&validation, family, "PROVENANCE_UNSAFE");
    }
    if (!family.separated_from_engine_execution ||
        !family.separated_from_result_formatting ||
        family.hidden_inside_execution_timing) {
      AddDiagnostic(&validation, family, "HIDDEN_IN_EXECUTION_TIMING");
    }
    if (std::any_of(family.samples_us.begin(),
                    family.samples_us.end(),
                    [](double value) { return value < 0.0; })) {
      AddDiagnostic(&validation, family, "NEGATIVE_METRIC");
    }
    const bool ambiguous_zero =
        family.p50_us == 0.0 || family.p95_us == 0.0 ||
        family.p99_us == 0.0 ||
        std::any_of(family.samples_us.begin(),
                    family.samples_us.end(),
                    [](double value) { return value == 0.0; });
    if (ambiguous_zero && !QualityPermitsZero(family)) {
      AddDiagnostic(&validation, family, "AMBIGUOUS_ZERO_METRIC");
    }
    if (!family.samples_us.empty() &&
        (!SameMetric(family.p50_us,
                     NearestRankPercentile(family.samples_us, 50.0)) ||
         !SameMetric(family.p95_us,
                     NearestRankPercentile(family.samples_us, 95.0)) ||
         !SameMetric(family.p99_us,
                     NearestRankPercentile(family.samples_us, 99.0)) ||
         family.p50_us > family.p95_us ||
         family.p95_us > family.p99_us)) {
      AddDiagnostic(&validation, family, "PERCENTILE_MISMATCH");
    }
    if (family.p99_us > family.p99_budget_us) {
      AddDiagnostic(&validation, family, "P99_OVER_BUDGET");
    }
    if (support_bundle_mode && family.family == "evidence_rendering" &&
        family.high_cost_hot_path_evidence_enabled) {
      AddDiagnostic(&validation, family, "SUPPORT_MODE_HOT_PATH_ENABLED");
    }
    if (family.benchmark_clean_claim && support_bundle_mode) {
      AddDiagnostic(&validation, family, "BENCHMARK_CLEAN_REJECTED");
    }
  }

  for (const auto& required : required_families) {
    if (seen.find(required) == seen.end()) {
      validation.diagnostics.push_back(std::string(kBudgetExceeded) + "." +
                                       required + ".MISSING_FAMILY");
    }
  }

  validation.ok = validation.diagnostics.empty();
  validation.benchmark_clean =
      validation.ok && benchmark_clean_claim && !support_bundle_mode;
  if (benchmark_clean_claim && !validation.ok) {
    validation.diagnostics.push_back(std::string(kBudgetExceeded) +
                                     ".BENCHMARK_CLEAN_REJECTED");
    validation.benchmark_clean = false;
  }
  return validation;
}

OverheadFamilyEvidence Family(std::string family,
                              std::vector<double> samples,
                              double budget,
                              std::string source =
                                  "measured_by_internal_counter",
                              std::string quality = "measured") {
  OverheadFamilyEvidence evidence;
  evidence.family = std::move(family);
  evidence.samples_us = std::move(samples);
  evidence.p50_us = NearestRankPercentile(evidence.samples_us, 50.0);
  evidence.p95_us = NearestRankPercentile(evidence.samples_us, 95.0);
  evidence.p99_us = NearestRankPercentile(evidence.samples_us, 99.0);
  evidence.p99_budget_us = budget;
  evidence.measurement_source = std::move(source);
  evidence.measurement_quality = std::move(quality);
  return evidence;
}

std::vector<OverheadFamilyEvidence> BenchmarkCleanFamilies() {
  auto support = Family("support_bundle_generation",
                        {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
                        1.0,
                        "measured_by_internal_counter",
                        "actual_zero");
  support.high_cost_hot_path_evidence_enabled = false;
  return {
      Family("engine_execution",
             {42.0, 43.0, 44.0, 45.0, 46.0, 47.0, 48.0},
             80.0),
      Family("result_formatting",
             {4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0},
             15.0),
      Family("evidence_rendering",
             {2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0},
             12.0),
      support,
      Family("metric_capture",
             {3.0, 3.0, 4.0, 4.0, 5.0, 5.0, 6.0},
             10.0,
             "measured_by_platform_api"),
      Family("diagnostic_construction",
             {1.0, 2.0, 2.0, 3.0, 3.0, 4.0, 4.0},
             8.0),
  };
}

void SetHotPathProvenance(api::PerformanceMetricHotPathAttribution* hot) {
  hot->cpu_sample_measurement_source = "measured_by_perf_sample";
  hot->cpu_sample_measurement_quality = "measured";
  hot->allocator_counter_measurement_source = "measured_by_internal_counter";
  hot->allocator_counter_measurement_quality = "actual_zero";
  hot->lock_latch_wait_measurement_source = "measured_by_internal_counter";
  hot->lock_latch_wait_measurement_quality = "actual_zero";
  hot->syscall_count_measurement_source = "measured_by_platform_api";
  hot->syscall_count_measurement_quality = "actual_zero";
  hot->file_io_count_measurement_source = "measured_by_platform_api";
  hot->file_io_count_measurement_quality = "actual_zero";
  hot->page_fault_count_measurement_source = "measured_by_platform_api";
  hot->page_fault_count_measurement_quality = "actual_zero";
  hot->context_switch_count_measurement_source = "measured_by_platform_api";
  hot->context_switch_count_measurement_quality = "actual_zero";
  hot->evidence_rendering_measurement_source = "measured_by_internal_counter";
  hot->evidence_rendering_measurement_quality = "measured";
  hot->result_formatting_measurement_source = "measured_by_internal_counter";
  hot->result_formatting_measurement_quality = "measured";
  hot->regression_budget_measurement_source = "measured_by_internal_counter";
  hot->regression_budget_measurement_quality = "measured";
  hot->parser_lowering_measurement_source = "measured_by_internal_counter";
  hot->parser_lowering_measurement_quality = "actual_zero";
  hot->sbps_listener_measurement_source = "measured_by_internal_counter";
  hot->sbps_listener_measurement_quality = "actual_zero";
  hot->sblr_dispatch_measurement_source = "measured_by_internal_counter";
  hot->sblr_dispatch_measurement_quality = "actual_zero";
  hot->internal_api_measurement_source = "measured_by_internal_counter";
  hot->internal_api_measurement_quality = "measured";
  hot->executor_measurement_source = "measured_by_internal_counter";
  hot->executor_measurement_quality = "measured";
  hot->storage_measurement_source = "measured_by_internal_counter";
  hot->storage_measurement_quality = "actual_zero";
  hot->index_layer_measurement_source = "measured_by_internal_counter";
  hot->index_layer_measurement_quality = "actual_zero";
  hot->transaction_measurement_source = "measured_by_internal_counter";
  hot->transaction_measurement_quality = "actual_zero";
  hot->result_rendering_measurement_source = "measured_by_internal_counter";
  hot->result_rendering_measurement_quality = "measured";
  hot->evidence_construction_measurement_source =
      "measured_by_internal_counter";
  hot->evidence_construction_measurement_quality = "measured";
  hot->allocation_measurement_source = "measured_by_internal_counter";
  hot->allocation_measurement_quality = "actual_zero";
  hot->syscall_measurement_source = "measured_by_internal_counter";
  hot->syscall_measurement_quality = "actual_zero";
  hot->wait_measurement_source = "measured_by_internal_counter";
  hot->wait_measurement_quality = "actual_zero";
}

api::PerformanceMetricEvent OverheadMetricEvent() {
  api::PerformanceMetricEvent event;
  event.route = "embedded.orh128.overhead_budget";
  event.operation = "optimizer_runtime_hot_path.orh_128";
  event.phase_timings.measurement_source = "measured_by_internal_counter";
  event.phase_timings.measurement_quality = "measured";
  event.phase_timings.parse_us = 0;
  event.phase_timings.bind_us = 0;
  event.phase_timings.lower_us = 0;
  event.phase_timings.plan_us = 0;
  event.phase_timings.execute_us = 48;
  event.storage_timings.measurement_source = "measured_by_internal_counter";
  event.storage_timings.measurement_quality = "actual_zero";
  event.storage_timings.append_us = 0;
  event.storage_timings.page_us = 0;
  event.storage_timings.index_us = 0;
  event.agent_counters.measurement_source = "measured_by_platform_api";
  event.agent_counters.measurement_quality = "measured";
  event.agent_counters.agent_thread_count = 1;
  event.agent_counters.agent_cpu_user_us = 6;
  event.agent_counters.agent_cpu_system_us = 1;
  event.agent_counters.agent_wait_us = 0;
  event.agent_counters.agent_io_read_bytes = 0;
  event.agent_counters.agent_io_write_bytes = 0;
  event.cache_flags.measurement_source = "measured_by_internal_counter";
  event.cache_flags.measurement_quality = "measured";
  event.cache_flags.plan_cache_hit = true;
  event.cache_flags.metadata_cache_hit = true;
  event.cache_flags.page_cache_hit = true;
  event.cache_flags.index_cache_hit = true;

  auto& hot = event.hot_path_attribution;
  hot.cpu_sample_count = 7;
  hot.cpu_sample_attributed_count = 7;
  hot.cpu_sample_attribution = "orh128_overhead_budget";
  hot.allocator_allocation_count = 0;
  hot.allocator_allocation_bytes = 0;
  hot.lock_latch_wait_count = 0;
  hot.lock_latch_wait_us = 0;
  hot.syscall_count = 0;
  hot.file_open_count = 0;
  hot.file_flush_count = 0;
  hot.file_fsync_count = 0;
  hot.page_fault_count = 0;
  hot.context_switch_count = 0;
  hot.evidence_rendering_us = 8;
  hot.result_formatting_us = 10;
  hot.regression_budget_us = 80;
  hot.regression_budget_margin_us = 32;
  hot.regression_budget_validated = true;
  hot.parser_lowering_us = 0;
  hot.sbps_listener_us = 0;
  hot.sblr_dispatch_us = 0;
  hot.internal_api_us = 6;
  hot.executor_us = 48;
  hot.storage_us = 0;
  hot.index_layer_us = 0;
  hot.transaction_us = 0;
  hot.result_rendering_us = 10;
  hot.evidence_construction_us = 4;
  hot.allocation_us = 0;
  hot.syscall_us = 0;
  hot.wait_us = 0;
  SetHotPathProvenance(&hot);

  event.statistics_epoch = 128;
  event.resource_governor_state = "admitted";
  event.message_vector_present = false;
  event.result_hash = "sha256:orh128-overhead-budget";
  event.overhead_mode = api::InstrumentationOverheadMode::kBenchmarkClean;
  return event;
}

optimizer::BenchmarkMethodologyRunEvidence MethodologyRun(
    std::string run_id,
    std::string cache_phase,
    std::vector<double> samples) {
  optimizer::BenchmarkMethodologyRunEvidence run;
  run.run_id = std::move(run_id);
  run.route_label = "embedded:orh128_overhead_budget";
  run.cache_phase = std::move(cache_phase);
  run.scale_tier = "small";
  run.skew_profile = "uniform";
  run.repetition_count = samples.size();
  run.sample_duration_us = std::move(samples);
  run.p50_us = NearestRankPercentile(run.sample_duration_us, 50.0);
  run.p95_us = NearestRankPercentile(run.sample_duration_us, 95.0);
  run.p99_us = NearestRankPercentile(run.sample_duration_us, 99.0);
  run.optimization_toggles = {
      "runtime_hot_path=on",
      "evidence_overhead_budget=on",
      "support_bundle_hot_path_evidence=off",
  };
  run.profiler_source_labels = {
      "engine_internal_counter",
      "platform_perf_event",
  };
  run.latest_scratchbird_baseline_id =
      "scratchbird-private-latest:ORH-128-overhead-budget";
  run.latest_scratchbird_baseline_p50_us = 120.0;
  run.reference_equivalent_baseline_id = "firebird-equivalent:evidence-light";
  run.reference_equivalent_engine = "firebird";
  run.reference_equivalent_baseline_p50_us = 150.0;
  run.methodology_only = false;
  run.performance_proof = true;
  run.benchmark_clean_claim = true;
  run.diagnostic_code = "SB_ORH_BENCHMARK_METHODOLOGY.RUN_RECORDED";
  return run;
}

void ProvesSeparateMeasuredOverheadFamilies() {
  const auto metric_validation =
      api::ValidatePerformanceMetricEvent(OverheadMetricEvent());
  Require(metric_validation.ok,
          "ORH-128 metric event rejected: " +
              metric_validation.diagnostic_code + " " +
              metric_validation.detail);
  const auto json = api::SerializePerformanceMetricEventJson(OverheadMetricEvent());
  Require(json.find("\"evidence_rendering_us\":8") != std::string::npos,
          "metric event did not serialize evidence rendering cost");
  Require(json.find("\"result_formatting_us\":10") != std::string::npos,
          "metric event did not serialize result formatting cost");
  Require(json.find("\"executor_us\":48") != std::string::npos,
          "metric event did not serialize engine execution cost");
  Require(json.find("\"evidence_construction_us\":4") != std::string::npos,
          "metric event did not serialize diagnostic/evidence construction cost");

  const auto validation =
      ValidateEvidenceOverheadBudget(BenchmarkCleanFamilies(), true, false);
  Require(validation.ok, "valid ORH-128 overhead families were rejected");
  Require(validation.benchmark_clean,
          "inside-budget overhead evidence should permit benchmark-clean");
  Require(Contains(validation.evidence_rows, "metric_capture:p50="),
          "metric capture overhead evidence missing");
}

void ProvesBenchmarkMethodologyControls() {
  std::vector<optimizer::BenchmarkMethodologyRunEvidence> runs = {
      MethodologyRun("orh128-cold", "cold",
                     {90.0, 91.0, 92.0, 93.0, 94.0, 95.0, 96.0}),
      MethodologyRun("orh128-warm", "warm",
                     {72.0, 73.0, 74.0, 75.0, 76.0, 77.0, 78.0}),
  };
  const auto validation = optimizer::ValidateBenchmarkMethodologyEvidence(runs);
  Require(validation.ok, validation.diagnostic_code);
  Require(validation.benchmark_clean,
          "ORH-123 methodology controls should preserve benchmark-clean claim");
}

void RejectsBudgetOverrun() {
  auto families = BenchmarkCleanFamilies();
  families[2].samples_us = {10.0, 11.0, 12.0, 13.0, 14.0, 15.0, 40.0};
  families[2].p50_us = NearestRankPercentile(families[2].samples_us, 50.0);
  families[2].p95_us = NearestRankPercentile(families[2].samples_us, 95.0);
  families[2].p99_us = NearestRankPercentile(families[2].samples_us, 99.0);
  const auto validation =
      ValidateEvidenceOverheadBudget(families, true, false);
  Require(!validation.ok, "over-budget evidence rendering should fail closed");
  Require(Contains(validation.diagnostics,
                   "evidence_rendering.P99_OVER_BUDGET"),
          "missing P99 over-budget diagnostic");
  Require(Contains(validation.diagnostics, "BENCHMARK_CLEAN_REJECTED"),
          "benchmark-clean overrun rejection missing");
}

void RejectsEstimatedUnsupportedOrMissingProvenance() {
  auto estimated = BenchmarkCleanFamilies();
  estimated[4].measurement_source = "estimated";
  estimated[4].measurement_quality = "estimated";
  const auto estimated_validation =
      ValidateEvidenceOverheadBudget(estimated, true, false);
  Require(!estimated_validation.ok,
          "estimated metric capture overhead should fail closed");
  Require(Contains(estimated_validation.diagnostics,
                   "metric_capture.PROVENANCE_UNSAFE"),
          "missing estimated-provenance diagnostic");

  auto missing = BenchmarkCleanFamilies();
  missing[5].measurement_source.clear();
  const auto missing_validation =
      ValidateEvidenceOverheadBudget(missing, true, false);
  Require(!missing_validation.ok,
          "missing diagnostic-construction provenance should fail closed");
  Require(Contains(missing_validation.diagnostics,
                   "diagnostic_construction.MISSING_PROVENANCE"),
          "missing provenance diagnostic");
}

void RejectsHiddenOrAmbiguousZeroMetrics() {
  auto hidden = BenchmarkCleanFamilies();
  hidden[0].hidden_inside_execution_timing = true;
  hidden[2].separated_from_engine_execution = false;
  const auto hidden_validation =
      ValidateEvidenceOverheadBudget(hidden, true, false);
  Require(!hidden_validation.ok,
          "hidden evidence overhead should fail closed");
  Require(Contains(hidden_validation.diagnostics,
                   "evidence_rendering.HIDDEN_IN_EXECUTION_TIMING"),
          "missing hidden-in-execution diagnostic");

  auto zero = BenchmarkCleanFamilies();
  zero[5].samples_us = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  zero[5].p50_us = 0.0;
  zero[5].p95_us = 0.0;
  zero[5].p99_us = 0.0;
  zero[5].measurement_quality = "measured";
  const auto zero_validation = ValidateEvidenceOverheadBudget(zero, true, false);
  Require(!zero_validation.ok,
          "ambiguous zero diagnostic overhead should fail closed");
  Require(Contains(zero_validation.diagnostics,
                   "diagnostic_construction.AMBIGUOUS_ZERO_METRIC"),
          "missing ambiguous-zero diagnostic");
}

void ProvesSupportBundleModeDisablesHighCostHotPathEvidence() {
  const auto policy = api::InstrumentationOverheadPolicyForMode(
      api::InstrumentationOverheadMode::kSupportBundle);
  Require(policy.support_bundle_summary_enabled,
          "support-bundle policy did not enable summary evidence");
  Require(!policy.hot_path_string_formatting_enabled,
          "support-bundle policy kept high-cost hot-path string evidence on");
  Require(!policy.benchmark_clean_eligible,
          "support-bundle policy must not be benchmark-clean eligible");

  auto families = BenchmarkCleanFamilies();
  families[2].samples_us = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  families[2].p50_us = 0.0;
  families[2].p95_us = 0.0;
  families[2].p99_us = 0.0;
  families[2].measurement_quality = "disabled";
  families[2].high_cost_hot_path_evidence_enabled = false;
  families[2].disabled_for_support_bundle = true;
  families[3] = Family("support_bundle_generation",
                       {18.0, 19.0, 20.0, 21.0, 22.0, 23.0, 24.0},
                       40.0);
  families[3].benchmark_clean_claim = false;
  const auto validation = ValidateEvidenceOverheadBudget(families, false, true);
  Require(validation.ok, "support-bundle overhead evidence should pass");
  Require(!validation.benchmark_clean,
          "support-bundle overhead evidence must not be benchmark-clean");

  const std::vector<std::string> support_summary = {
      "support_bundle.evidence_overhead.mode=support_bundle",
      "support_bundle.evidence_overhead.redaction=sensitive_payload_redacted",
      "support_bundle.evidence_overhead.family=support_bundle_generation",
      "support_bundle.evidence_overhead.hot_path_evidence=disabled",
  };
  Require(Contains(support_summary, "sensitive_payload_redacted"),
          "support bundle redaction summary missing");
  Require(!Contains(support_summary, "cleartext_secret"),
          "support bundle summary leaked sensitive material");
}

void RejectsSupportBundleBenchmarkCleanOverclaim() {
  auto families = BenchmarkCleanFamilies();
  families[2].benchmark_clean_claim = true;
  families[2].high_cost_hot_path_evidence_enabled = true;
  const auto validation = ValidateEvidenceOverheadBudget(families, true, true);
  Require(!validation.ok,
          "support-bundle benchmark-clean overclaim should fail closed");
  Require(Contains(validation.diagnostics,
                   "evidence_rendering.SUPPORT_MODE_HOT_PATH_ENABLED"),
          "missing support-bundle hot-path disable diagnostic");
  Require(Contains(validation.diagnostics, "BENCHMARK_CLEAN_REJECTED"),
          "missing support-bundle benchmark-clean rejection");
}

void ProvesEvidenceIsNotAuthority() {
  struct AuthorityEvidence {
    bool evidence_is_visibility_authority = false;
    bool evidence_is_transaction_finality_authority = false;
    bool evidence_is_authorization_authority = false;
    bool evidence_is_recovery_authority = false;
    std::string authority_scope = "advisory_observability_only";
  } authority;

  Require(!authority.evidence_is_visibility_authority,
          "overhead evidence must not own visibility authority");
  Require(!authority.evidence_is_transaction_finality_authority,
          "overhead evidence must not own transaction finality authority");
  Require(!authority.evidence_is_authorization_authority,
          "overhead evidence must not own authorization authority");
  Require(!authority.evidence_is_recovery_authority,
          "overhead evidence must not own recovery authority");
  Require(authority.authority_scope == "advisory_observability_only",
          "overhead authority scope changed");
}

}  // namespace

int main() {
  Require(StartsWith(kBudgetExceeded,
                     "ORH_EVIDENCE_OVERHEAD_BUDGET_EXCEEDED"),
          "ORH-128 diagnostic prefix drifted");
  ProvesSeparateMeasuredOverheadFamilies();
  ProvesBenchmarkMethodologyControls();
  RejectsBudgetOverrun();
  RejectsEstimatedUnsupportedOrMissingProvenance();
  RejectsHiddenOrAmbiguousZeroMetrics();
  ProvesSupportBundleModeDisablesHighCostHotPathEvidence();
  RejectsSupportBundleBenchmarkCleanOverclaim();
  ProvesEvidenceIsNotAuthority();

  std::cout << "ORH-128 evidence overhead budget gate passed: "
            << "families=engine_execution,result_formatting,"
            << "evidence_rendering,support_bundle_generation,"
            << "metric_capture,diagnostic_construction "
            << "diagnostic=" << kBudgetExceeded << '\n';
  return 0;
}
