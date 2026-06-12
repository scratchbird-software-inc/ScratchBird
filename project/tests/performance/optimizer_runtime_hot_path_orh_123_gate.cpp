// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "runtime_consumption_evidence.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace optimizer = scratchbird::engine::optimizer;

namespace {

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << "ORH-GATE-123 failure: " << message << '\n';
  std::exit(1);
}

void Expect(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}

bool HasDiagnostic(
    const optimizer::BenchmarkMethodologyValidation& validation,
    const std::string& diagnostic) {
  for (const auto& item : validation.diagnostics) {
    if (item.find(diagnostic) != std::string::npos) return true;
  }
  return false;
}

optimizer::BenchmarkMethodologyRunEvidence MakeMethodologyRun(
    std::string run_id,
    std::string route_label,
    std::string cache_phase,
    std::string scale_tier,
    std::string skew_profile,
    std::vector<double> samples,
    double p50_us,
    double p95_us,
    double p99_us) {
  optimizer::BenchmarkMethodologyRunEvidence run;
  run.run_id = std::move(run_id);
  run.route_label = std::move(route_label);
  run.cache_phase = std::move(cache_phase);
  run.scale_tier = std::move(scale_tier);
  run.skew_profile = std::move(skew_profile);
  run.repetition_count = samples.size();
  run.sample_duration_us = std::move(samples);
  run.p50_us = p50_us;
  run.p95_us = p95_us;
  run.p99_us = p99_us;
  run.optimization_toggles = {
      "runtime_hot_path=on",
      "nosql_document_path_index=on",
      "result_text_rendering=off",
  };
  run.profiler_source_labels = {
      "engine_internal_counter",
      "platform_perf_event",
  };
  run.latest_scratchbird_baseline_id =
      "scratchbird-private-latest:ORH-123-methodology";
  run.latest_scratchbird_baseline_p50_us = 210.0;
  run.reference_equivalent_baseline_id = "firebird-equivalent:legacy-route";
  run.reference_equivalent_engine = "firebird";
  run.reference_equivalent_baseline_p50_us = 240.0;
  run.methodology_only = true;
  run.performance_proof = false;
  run.benchmark_clean_claim = false;
  run.diagnostic_code = "SB_ORH_BENCHMARK_METHODOLOGY.RUN_RECORDED";
  return run;
}

std::vector<optimizer::BenchmarkMethodologyRunEvidence> MakeValidMethodologySet() {
  return {
      MakeMethodologyRun("orh123-cold-uniform",
                         "embedded:nosql_document_path_lookup",
                         "cold",
                         "small",
                         "uniform",
                         {100.0, 110.0, 120.0, 130.0, 140.0, 150.0, 160.0},
                         130.0,
                         160.0,
                         160.0),
      MakeMethodologyRun("orh123-warm-skewed",
                         "embedded:nosql_document_path_lookup",
                         "warm",
                         "small",
                         "zipfian_1_2",
                         {80.0, 82.0, 84.0, 86.0, 88.0, 90.0, 92.0},
                         86.0,
                         92.0,
                         92.0),
  };
}

void ValidMethodologyEvidenceRecordsNoiseControls() {
  const auto validation =
      optimizer::ValidateBenchmarkMethodologyEvidence(MakeValidMethodologySet());
  Expect(validation.ok, validation.diagnostic_code);
  Expect(validation.diagnostic_code == "SB_ORH_BENCHMARK_METHODOLOGY.OK",
         "valid methodology evidence should produce OK diagnostic");
  Expect(!validation.benchmark_clean,
         "methodology-only evidence must not become benchmark-clean");
}

void PerformanceProofEvidenceCanCarryBenchmarkCleanClaim() {
  auto runs = MakeValidMethodologySet();
  for (auto& run : runs) {
    run.methodology_only = false;
    run.performance_proof = true;
    run.benchmark_clean_claim = true;
  }
  const auto validation =
      optimizer::ValidateBenchmarkMethodologyEvidence(runs);
  Expect(validation.ok, validation.diagnostic_code);
  Expect(validation.benchmark_clean,
         "performance-proof evidence with full controls should remain benchmark-clean");
}

void FailsClosedForInsufficientSamples() {
  auto runs = MakeValidMethodologySet();
  runs.front().sample_duration_us = {100.0, 110.0};
  runs.front().repetition_count = 1;
  runs.front().p50_us = 110.0;
  runs.front().p95_us = 110.0;
  runs.front().p99_us = 110.0;
  const auto validation =
      optimizer::ValidateBenchmarkMethodologyEvidence(runs);
  Expect(!validation.ok, "insufficient samples should fail closed");
  Expect(HasDiagnostic(validation,
                       "SB_ORH_BENCHMARK_METHODOLOGY.SAMPLES_INSUFFICIENT"),
         "missing insufficient-sample diagnostic");
}

void FailsClosedForSampleCountMismatch() {
  auto runs = MakeValidMethodologySet();
  runs.front().repetition_count = runs.front().sample_duration_us.size() + 1;
  const auto validation =
      optimizer::ValidateBenchmarkMethodologyEvidence(runs);
  Expect(!validation.ok, "sample count mismatch should fail closed");
  Expect(HasDiagnostic(validation,
                       "SB_ORH_BENCHMARK_METHODOLOGY.SAMPLE_COUNT_MISMATCH"),
         "missing sample-count mismatch diagnostic");
}

void FailsClosedForPercentileMismatch() {
  auto runs = MakeValidMethodologySet();
  runs.front().p95_us = 130.0;
  const auto validation =
      optimizer::ValidateBenchmarkMethodologyEvidence(runs);
  Expect(!validation.ok, "percentile mismatch should fail closed");
  Expect(HasDiagnostic(validation,
                       "SB_ORH_BENCHMARK_METHODOLOGY.PERCENTILE_MISMATCH"),
         "missing percentile mismatch diagnostic");
}

void FailsClosedForMissingProfilerProvenance() {
  auto runs = MakeValidMethodologySet();
  runs.front().profiler_source_labels.clear();
  const auto validation =
      optimizer::ValidateBenchmarkMethodologyEvidence(runs);
  Expect(!validation.ok, "missing profiler source should fail closed");
  Expect(HasDiagnostic(validation,
                       "SB_ORH_BENCHMARK_METHODOLOGY.PROFILER_SOURCE_MISSING"),
         "missing profiler provenance diagnostic");
}

void FailsClosedForMissingOptimizationToggles() {
  auto runs = MakeValidMethodologySet();
  runs.front().optimization_toggles.clear();
  const auto validation =
      optimizer::ValidateBenchmarkMethodologyEvidence(runs);
  Expect(!validation.ok, "missing optimization toggles should fail closed");
  Expect(HasDiagnostic(
             validation,
             "SB_ORH_BENCHMARK_METHODOLOGY.OPTIMIZATION_TOGGLES_MISSING"),
         "missing optimization-toggle diagnostic");
}

void FailsClosedForAmbiguousZeroMetrics() {
  auto runs = MakeValidMethodologySet();
  runs.front().sample_duration_us.front() = 0.0;
  const auto validation =
      optimizer::ValidateBenchmarkMethodologyEvidence(runs);
  Expect(!validation.ok, "ambiguous zero metrics should fail closed");
  Expect(HasDiagnostic(validation,
                       "SB_ORH_BENCHMARK_METHODOLOGY.AMBIGUOUS_ZERO_METRIC"),
         "missing ambiguous-zero diagnostic");
}

void FailsClosedForMissingRouteLabels() {
  auto runs = MakeValidMethodologySet();
  runs.front().route_label.clear();
  const auto validation =
      optimizer::ValidateBenchmarkMethodologyEvidence(runs);
  Expect(!validation.ok, "missing route label should fail closed");
  Expect(HasDiagnostic(validation,
                       "SB_ORH_BENCHMARK_METHODOLOGY.MISSING_ROUTE_LABEL"),
         "missing route-label diagnostic");
}

void FailsClosedForMissingBaselineComparisons() {
  auto runs = MakeValidMethodologySet();
  runs.front().latest_scratchbird_baseline_id.clear();
  runs.front().latest_scratchbird_baseline_p50_us = 0.0;
  runs.front().reference_equivalent_baseline_id.clear();
  runs.front().reference_equivalent_engine.clear();
  runs.front().reference_equivalent_baseline_p50_us = 0.0;
  const auto validation =
      optimizer::ValidateBenchmarkMethodologyEvidence(runs);
  Expect(!validation.ok, "missing baseline comparisons should fail closed");
  Expect(HasDiagnostic(
             validation,
             "SB_ORH_BENCHMARK_METHODOLOGY.LATEST_SCRATCHBIRD_BASELINE_MISSING"),
         "missing latest ScratchBird baseline diagnostic");
  Expect(HasDiagnostic(
             validation,
             "SB_ORH_BENCHMARK_METHODOLOGY.REFERENCE_EQUIVALENT_BASELINE_MISSING"),
         "missing reference-equivalent baseline diagnostic");
}

void FailsClosedForUnsupportedReferenceEquivalentEngine() {
  auto runs = MakeValidMethodologySet();
  runs.front().reference_equivalent_engine = "unknown_engine";
  const auto validation =
      optimizer::ValidateBenchmarkMethodologyEvidence(runs);
  Expect(!validation.ok, "unsupported reference-equivalent engine should fail closed");
  Expect(HasDiagnostic(
             validation,
             "SB_ORH_BENCHMARK_METHODOLOGY.REFERENCE_EQUIVALENT_ENGINE_UNSUPPORTED"),
         "missing unsupported reference engine diagnostic");
}

void FailsClosedForBenchmarkCleanOverclaim() {
  auto runs = MakeValidMethodologySet();
  runs.front().benchmark_clean_claim = true;
  const auto validation =
      optimizer::ValidateBenchmarkMethodologyEvidence(runs);
  Expect(!validation.ok, "methodology-only benchmark-clean claim should fail");
  Expect(HasDiagnostic(validation,
                       "SB_ORH_BENCHMARK_METHODOLOGY.BENCHMARK_CLEAN_OVERCLAIM"),
         "missing benchmark-clean overclaim diagnostic");
}

void FailsClosedWhenColdWarmCoverageIsIncomplete() {
  auto runs = MakeValidMethodologySet();
  runs.back().cache_phase = "cold";
  const auto validation =
      optimizer::ValidateBenchmarkMethodologyEvidence(runs);
  Expect(!validation.ok, "missing warm phase should fail closed");
  Expect(HasDiagnostic(validation,
                       "SB_ORH_BENCHMARK_METHODOLOGY.COLD_WARM_PHASE_INCOMPLETE"),
         "missing cold/warm coverage diagnostic");
}

}  // namespace

int main() {
  ValidMethodologyEvidenceRecordsNoiseControls();
  PerformanceProofEvidenceCanCarryBenchmarkCleanClaim();
  FailsClosedForInsufficientSamples();
  FailsClosedForSampleCountMismatch();
  FailsClosedForPercentileMismatch();
  FailsClosedForMissingProfilerProvenance();
  FailsClosedForMissingOptimizationToggles();
  FailsClosedForAmbiguousZeroMetrics();
  FailsClosedForMissingRouteLabels();
  FailsClosedForMissingBaselineComparisons();
  FailsClosedForUnsupportedReferenceEquivalentEngine();
  FailsClosedForBenchmarkCleanOverclaim();
  FailsClosedWhenColdWarmCoverageIsIncomplete();
  return 0;
}
