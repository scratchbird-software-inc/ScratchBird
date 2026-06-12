// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "runtime_consumption_evidence.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace opt = scratchbird::engine::optimizer;

constexpr std::string_view kBlockedDiagnostic =
    "ORH_LARGE_SCALE_BENCHMARK_TIER_UNAVAILABLE";

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << "ORH-288 gate failure: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasDiagnostic(const std::vector<std::string>& diagnostics,
                   std::string_view needle) {
  return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& item) {
    return item.find(needle) != std::string::npos;
  });
}

std::string JoinDiagnostics(const std::vector<std::string>& diagnostics) {
  std::ostringstream out;
  for (std::size_t i = 0; i < diagnostics.size(); ++i) {
    if (i != 0) {
      out << ';';
    }
    out << diagnostics[i];
  }
  return out.str();
}

std::string StableHash(const std::vector<std::string>& rows) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const auto& row : rows) {
    for (const unsigned char ch : row) {
      hash ^= ch;
      hash *= 1099511628211ULL;
    }
    hash ^= 0xffU;
    hash *= 1099511628211ULL;
  }
  std::ostringstream out;
  out << "fnv64:" << std::hex << hash;
  return out.str();
}

double Percentile(std::vector<double> samples, double percentile) {
  std::sort(samples.begin(), samples.end());
  const auto rank = static_cast<std::size_t>(
      std::ceil((percentile / 100.0) * static_cast<double>(samples.size())));
  const auto index = rank == 0 ? 0 : rank - 1;
  return samples[std::min(index, samples.size() - 1)];
}

enum class ScaleTier {
  small,
  medium,
  gb_scale,
};

enum class WorkloadFamily {
  ingest,
  skewed_join,
  high_cardinality_aggregate,
  update_heavy,
};

const char* ScaleTierName(ScaleTier tier) {
  switch (tier) {
    case ScaleTier::small:
      return "small";
    case ScaleTier::medium:
      return "medium";
    case ScaleTier::gb_scale:
      return "gb_scale";
  }
  return "unknown";
}

const char* WorkloadName(WorkloadFamily workload) {
  switch (workload) {
    case WorkloadFamily::ingest:
      return "ingest";
    case WorkloadFamily::skewed_join:
      return "skewed_join";
    case WorkloadFamily::high_cardinality_aggregate:
      return "high_cardinality_aggregate";
    case WorkloadFamily::update_heavy:
      return "update_heavy";
  }
  return "unknown";
}

std::vector<WorkloadFamily> RequiredWorkloads() {
  return {WorkloadFamily::ingest,
          WorkloadFamily::skewed_join,
          WorkloadFamily::high_cardinality_aggregate,
          WorkloadFamily::update_heavy};
}

std::vector<ScaleTier> RequiredTiers() {
  return {ScaleTier::small, ScaleTier::medium, ScaleTier::gb_scale};
}

struct LargeScaleLaneCapture {
  std::string lane_id;
  std::string route_label;
  ScaleTier scale_tier = ScaleTier::small;
  WorkloadFamily workload = WorkloadFamily::ingest;
  std::string cache_phase;
  std::string data_generation_seed;
  std::string data_generation_fingerprint;
  std::string skew_profile = "deterministic_zipf_1_2";
  std::string result_hash;
  std::vector<std::string> rows;
  std::vector<double> samples_us;
  double p50_us = 0.0;
  double p95_us = 0.0;
  double p99_us = 0.0;
  std::string output_suppression_policy = "timed_path_output_suppressed";
  std::string materialization_policy = "hash_result_materialized_outside_timing";
  std::string ab_profile_id = "orh287_all_optimizations";
  std::uint64_t artifact_generation = 288;
  std::uint64_t expected_artifact_generation = 288;
  bool stale_artifact = false;
  bool reproducible_data_generation = true;
  bool executed = true;
  bool exact_blocker = false;
  std::string exact_blocker_diagnostic;
  bool benchmark_clean_claim = false;
  bool reference_equivalent_controls_present = true;
  bool best_method_equivalence_present = true;
  bool mga_visibility_evidence_present = true;
  bool security_recheck_evidence_present = true;
  opt::RuntimeOptimizedPathEvidence runtime_evidence;
};

struct TierValidation {
  bool ok = false;
  bool benchmark_clean = false;
  bool completed_blocked = false;
  std::string diagnostic_code;
  std::vector<std::string> diagnostics;
  std::vector<std::string> evidence;
};

std::string WorkloadTierKey(WorkloadFamily workload, ScaleTier tier) {
  return std::string(WorkloadName(workload)) + ":" + ScaleTierName(tier);
}

std::vector<std::string> RowsFor(WorkloadFamily workload, ScaleTier tier) {
  const std::string prefix = WorkloadTierKey(workload, tier);
  return {prefix + ":row:001", prefix + ":row:002", prefix + ":row:003"};
}

std::vector<double> SamplesFor(ScaleTier tier, std::string_view phase) {
  const double base = tier == ScaleTier::small ? 90.0 : 240.0;
  const double cold = phase == "cold" ? 24.0 : 0.0;
  return {base + cold,
          base + cold + 1.0,
          base + cold + 2.0,
          base + cold + 3.0,
          base + cold + 4.0,
          base + cold + 5.0,
          base + cold + 6.0};
}

opt::RuntimeOptimizedPathEvidence RuntimeEvidence(
    const LargeScaleLaneCapture& lane) {
  opt::RuntimeOptimizedPathEvidence evidence;
  evidence.selected_path = lane.ab_profile_id;
  evidence.route_kind = lane.route_label;
  evidence.runtime_consumed = lane.executed;
  evidence.live_execution = lane.executed;
  evidence.contract_only = false;
  evidence.consumed_module =
      std::string("large_scale_tier.") + WorkloadName(lane.workload);
  evidence.transaction_snapshot_class =
      "engine_mga_snapshot_visible_through_local_transaction_id";
  evidence.catalog_epoch = 288;
  evidence.security_epoch = 288;
  evidence.redaction_epoch = 288;
  evidence.provider_generation = 288;
  evidence.result_contract_hash = lane.result_hash;
  evidence.fallback_reason =
      lane.executed ? "" : "large_scale_lane_not_executed";
  evidence.diagnostic_code =
      lane.executed ? "ORH_288.RUNTIME_CONSUMED"
                    : std::string(kBlockedDiagnostic);
  return evidence;
}

LargeScaleLaneCapture ExecutedLane(WorkloadFamily workload,
                                   ScaleTier tier,
                                   std::string phase) {
  LargeScaleLaneCapture lane;
  lane.scale_tier = tier;
  lane.workload = workload;
  lane.cache_phase = std::move(phase);
  lane.lane_id = WorkloadTierKey(workload, tier) + ":" + lane.cache_phase;
  lane.route_label = std::string("embedded:") + WorkloadName(workload);
  lane.data_generation_seed =
      "seed:288:" + WorkloadTierKey(workload, tier);
  lane.data_generation_fingerprint =
      "fingerprint:" + WorkloadTierKey(workload, tier);
  lane.rows = RowsFor(workload, tier);
  lane.result_hash = StableHash(lane.rows);
  lane.samples_us = SamplesFor(tier, lane.cache_phase);
  lane.p50_us = Percentile(lane.samples_us, 50.0);
  lane.p95_us = Percentile(lane.samples_us, 95.0);
  lane.p99_us = Percentile(lane.samples_us, 99.0);
  lane.runtime_evidence = RuntimeEvidence(lane);
  return lane;
}

LargeScaleLaneCapture BlockedGbLane(WorkloadFamily workload) {
  LargeScaleLaneCapture lane;
  lane.scale_tier = ScaleTier::gb_scale;
  lane.workload = workload;
  lane.cache_phase = "blocked";
  lane.lane_id = WorkloadTierKey(workload, ScaleTier::gb_scale) + ":blocked";
  lane.route_label = std::string("embedded:") + WorkloadName(workload);
  lane.data_generation_seed =
      "seed:288:" + WorkloadTierKey(workload, ScaleTier::gb_scale);
  lane.data_generation_fingerprint =
      "fingerprint:" + WorkloadTierKey(workload, ScaleTier::gb_scale);
  lane.rows = RowsFor(workload, ScaleTier::gb_scale);
  lane.result_hash = StableHash(lane.rows);
  lane.executed = false;
  lane.exact_blocker = true;
  lane.exact_blocker_diagnostic = std::string(kBlockedDiagnostic) +
                                  ".GB_SCALE_EXECUTION_NOT_RUN_IN_SMOKE_GATE";
  lane.runtime_evidence = RuntimeEvidence(lane);
  return lane;
}

opt::BenchmarkMethodologyRunEvidence MethodologyEvidence(
    const LargeScaleLaneCapture& lane) {
  opt::BenchmarkMethodologyRunEvidence run;
  run.run_id = lane.lane_id;
  run.route_label = lane.route_label;
  run.cache_phase = lane.cache_phase;
  run.scale_tier = ScaleTierName(lane.scale_tier);
  run.skew_profile = lane.skew_profile;
  run.repetition_count = lane.samples_us.size();
  run.sample_duration_us = lane.samples_us;
  run.p50_us = lane.p50_us;
  run.p95_us = lane.p95_us;
  run.p99_us = lane.p99_us;
  run.optimization_toggles = {
      "orh287_ab_profile=" + lane.ab_profile_id,
      "large_scale_tier=" + std::string(ScaleTierName(lane.scale_tier)),
  };
  run.profiler_source_labels = {
      "engine_internal_counter",
      "large_scale_tier_smoke_clock",
  };
  run.latest_scratchbird_baseline_id = "scratchbird-main:orh288";
  run.latest_scratchbird_baseline_p50_us = lane.p50_us + 30.0;
  run.reference_equivalent_baseline_id =
      "firebird-equivalent:" + std::string(WorkloadName(lane.workload));
  run.reference_equivalent_engine = "firebird";
  run.reference_equivalent_baseline_p50_us = lane.p50_us + 70.0;
  run.methodology_only = true;
  run.performance_proof = false;
  run.benchmark_clean_claim = false;
  run.diagnostic_code = "ORH_288.METHODOLOGY_CAPTURED";
  return run;
}

opt::BenchmarkMethodEvidence MethodEvidence(const LargeScaleLaneCapture& lane,
                                            std::string engine) {
  opt::BenchmarkMethodEvidence method;
  method.engine = std::move(engine);
  method.logical_task = WorkloadName(lane.workload);
  method.workload_family = WorkloadName(lane.workload);
  method.method = method.engine == "scratchbird"
                      ? "native_runtime_route"
                      : "reference_equivalent_best_method";
  method.best_normal_method = true;
  method.native_bulk_or_best_engine_path = true;
  method.prepared_or_warmed = true;
  method.output_suppressed = true;
  method.result_materialization_policy = lane.materialization_policy;
  method.transaction_policy = "engine_mga_transaction_inventory_authority";
  method.data_generator_id = lane.data_generation_fingerprint;
  method.scale_profile = ScaleTierName(lane.scale_tier);
  method.skew_profile = lane.skew_profile;
  method.resource_budget_profile = "orh288_smoke_budget";
  method.constraint_policy = "constraints_checked_equivalently";
  method.reference_reference_only = method.engine != "scratchbird";
  method.uses_reference_storage_or_finality_for_scratchbird = false;
  method.diagnostic_code = "ORH_288.BEST_METHOD_CONTROL";
  return method;
}

std::vector<LargeScaleLaneCapture> ValidInfrastructureLanes() {
  std::vector<LargeScaleLaneCapture> lanes;
  for (const auto workload : RequiredWorkloads()) {
    for (const auto tier : {ScaleTier::small, ScaleTier::medium}) {
      lanes.push_back(ExecutedLane(workload, tier, "cold"));
      lanes.push_back(ExecutedLane(workload, tier, "warm"));
    }
    lanes.push_back(BlockedGbLane(workload));
  }
  return lanes;
}

void AddDiagnostic(TierValidation* validation, std::string diagnostic) {
  validation->diagnostics.push_back(std::move(diagnostic));
}

void ValidateExecutedLane(const LargeScaleLaneCapture& lane,
                          TierValidation* validation) {
  if (lane.lane_id.empty() || lane.route_label.empty() ||
      lane.data_generation_seed.empty() ||
      lane.data_generation_fingerprint.empty()) {
    AddDiagnostic(validation,
                  lane.lane_id + ":ORH_288.LANE_IDENTITY_MISSING");
  }
  if (lane.stale_artifact ||
      lane.artifact_generation != lane.expected_artifact_generation) {
    AddDiagnostic(validation,
                  lane.lane_id + ":ORH_288.STALE_BENCHMARK_ARTIFACT");
  }
  if (!lane.reproducible_data_generation) {
    AddDiagnostic(validation,
                  lane.lane_id + ":ORH_288.DATA_GENERATION_NOT_REPRODUCIBLE");
  }
  if (lane.result_hash.empty()) {
    AddDiagnostic(validation,
                  lane.lane_id + ":ORH_288.RESULT_HASH_MISSING");
  }
  if (lane.output_suppression_policy.empty() ||
      lane.materialization_policy.empty()) {
    AddDiagnostic(validation,
                  lane.lane_id + ":ORH_288.OUTPUT_POLICY_MISSING");
  }
  if (lane.p50_us <= 0.0 || lane.p95_us <= 0.0 || lane.p99_us <= 0.0 ||
      lane.samples_us.empty()) {
    AddDiagnostic(validation,
                  lane.lane_id + ":ORH_288.TIMING_FIELDS_MISSING");
  }
  if (!lane.mga_visibility_evidence_present ||
      !lane.security_recheck_evidence_present ||
      lane.runtime_evidence.security_epoch == 0 ||
      lane.runtime_evidence.redaction_epoch == 0 ||
      lane.runtime_evidence.transaction_snapshot_class.empty()) {
    AddDiagnostic(validation,
                  lane.lane_id + ":ORH_288.MGA_SECURITY_EVIDENCE_MISSING");
  }
  const auto runtime =
      opt::ValidateRuntimeOptimizedPathEvidence(lane.runtime_evidence);
  if (!runtime.ok ||
      runtime.state != opt::RuntimeConsumptionState::kRuntimeConsumed) {
    AddDiagnostic(validation,
                  lane.lane_id + ":ORH_288.RUNTIME_CONSUMPTION_MISSING");
  }
  if (lane.runtime_evidence.contract_only) {
    AddDiagnostic(validation,
                  lane.lane_id + ":ORH_288.CONTRACT_ONLY_EVIDENCE");
  }
  if (!lane.reference_equivalent_controls_present ||
      !lane.best_method_equivalence_present) {
    AddDiagnostic(validation,
                  lane.lane_id + ":ORH_288.REFERENCE_EQUIVALENT_CONTROLS_MISSING");
  } else {
    const auto methods =
        std::vector<opt::BenchmarkMethodEvidence>{
            MethodEvidence(lane, "scratchbird"),
            MethodEvidence(lane, "firebird"),
            MethodEvidence(lane, "mysql"),
            MethodEvidence(lane, "postgresql")};
    const auto equivalence = opt::ValidateBestMethodBenchmarkEquivalence(
        methods, {"scratchbird", "firebird", "mysql", "postgresql"});
    if (!equivalence.ok) {
      AddDiagnostic(validation,
                    lane.lane_id + ":ORH_288.BEST_METHOD_EQUIVALENCE_FAILED");
    }
  }
  if (lane.benchmark_clean_claim) {
    AddDiagnostic(validation,
                  lane.lane_id + ":ORH_288.BENCHMARK_CLEAN_OVERCLAIM");
  }
}

TierValidation ValidateLargeScaleTierHarness(
    const std::vector<LargeScaleLaneCapture>& lanes) {
  TierValidation validation;
  std::map<std::string, std::vector<const LargeScaleLaneCapture*>> groups;
  for (const auto& lane : lanes) {
    groups[WorkloadTierKey(lane.workload, lane.scale_tier)].push_back(&lane);
  }

  for (const auto workload : RequiredWorkloads()) {
    for (const auto tier : RequiredTiers()) {
      const auto key = WorkloadTierKey(workload, tier);
      auto found = groups.find(key);
      if (found == groups.end()) {
        AddDiagnostic(&validation, key + ":ORH_288.REQUIRED_TIER_MISSING");
        continue;
      }
      const auto& group = found->second;
      if (tier == ScaleTier::gb_scale) {
        const bool has_blocker = std::any_of(
            group.begin(), group.end(), [](const auto* lane) {
              return !lane->executed && lane->exact_blocker &&
                     lane->exact_blocker_diagnostic.find(kBlockedDiagnostic) !=
                         std::string::npos &&
                     !lane->benchmark_clean_claim;
            });
        if (!has_blocker) {
          AddDiagnostic(&validation,
                        key + ":ORH_288.GB_SCALE_BLOCKER_REQUIRED");
        } else {
          validation.completed_blocked = true;
          validation.evidence.push_back(
              key + ":exact_blocker=" + std::string(kBlockedDiagnostic));
        }
        continue;
      }

      bool saw_cold = false;
      bool saw_warm = false;
      std::set<std::string> result_hashes;
      std::vector<opt::BenchmarkMethodologyRunEvidence> methodology;
      for (const auto* lane : group) {
        if (lane->cache_phase == "cold") {
          saw_cold = true;
        } else if (lane->cache_phase == "warm") {
          saw_warm = true;
        }
        result_hashes.insert(lane->result_hash);
        methodology.push_back(MethodologyEvidence(*lane));
        ValidateExecutedLane(*lane, &validation);
      }
      if (!saw_cold || !saw_warm) {
        AddDiagnostic(&validation,
                      key + ":ORH_288.COLD_WARM_PHASE_INCOMPLETE");
      }
      if (result_hashes.size() != 1) {
        AddDiagnostic(&validation,
                      key + ":ORH_288.RESULT_HASH_PHASE_MISMATCH");
      }
      const auto methodology_result =
          opt::ValidateBenchmarkMethodologyEvidence(methodology);
      if (!methodology_result.ok) {
        AddDiagnostic(&validation,
                      key + ":ORH_288.METHODOLOGY_VALIDATION_FAILED");
      }
      validation.evidence.push_back(
          key + ":result_hash=" +
          (result_hashes.empty() ? "missing" : *result_hashes.begin()));
    }
  }

  validation.ok = validation.diagnostics.empty();
  validation.benchmark_clean = validation.ok && !validation.completed_blocked;
  validation.diagnostic_code =
      validation.ok
          ? (validation.completed_blocked
                 ? "ORH_288.COMPLETED_BLOCKED_GB_SCALE_EXECUTION_REQUIRED"
                 : "ORH_288.OK")
          : "ORH_288.FAIL_CLOSED";
  return validation;
}

void ValidInfrastructureIsCompletedBlocked() {
  const auto validation = ValidateLargeScaleTierHarness(ValidInfrastructureLanes());
  Require(validation.ok, "valid tier harness failed closed");
  Require(validation.completed_blocked,
          "GB-scale blocker was not preserved as completed-blocked");
  Require(!validation.benchmark_clean,
          "completed-blocked infrastructure claimed benchmark-clean");
  Require(validation.diagnostic_code ==
              "ORH_288.COMPLETED_BLOCKED_GB_SCALE_EXECUTION_REQUIRED",
          "completed-blocked diagnostic drifted");
  Require(HasDiagnostic(validation.evidence, "exact_blocker="),
          "GB-scale exact blocker evidence missing");
}

void NegativeCasesFailClosed() {
  auto lanes = ValidInfrastructureLanes();
  lanes[0].stale_artifact = true;
  lanes[2].reference_equivalent_controls_present = false;
  lanes[2].result_hash.clear();
  lanes[3].runtime_evidence.contract_only = true;
  lanes[3].runtime_evidence.runtime_consumed = false;
  lanes[3].runtime_evidence.live_execution = false;
  lanes[2].mga_visibility_evidence_present = false;
  lanes[5].reproducible_data_generation = false;
  lanes[6].benchmark_clean_claim = true;
  lanes.erase(std::remove_if(lanes.begin(),
                             lanes.end(),
                             [](const auto& lane) {
                               return lane.workload == WorkloadFamily::ingest &&
                                      lane.scale_tier == ScaleTier::small &&
                                      lane.cache_phase == "warm";
                             }),
              lanes.end());

  const auto validation = ValidateLargeScaleTierHarness(lanes);
  Require(!validation.ok, "negative tier evidence accepted");
  Require(HasDiagnostic(validation.diagnostics,
                        "ORH_288.STALE_BENCHMARK_ARTIFACT"),
          "stale artifact diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics,
                        "ORH_288.REFERENCE_EQUIVALENT_CONTROLS_MISSING"),
          "reference controls diagnostic missing: " +
              JoinDiagnostics(validation.diagnostics));
  Require(HasDiagnostic(validation.diagnostics,
                        "ORH_288.RESULT_HASH_MISSING"),
          "result hash diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics,
                        "ORH_288.CONTRACT_ONLY_EVIDENCE"),
          "contract-only diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics,
                        "ORH_288.RUNTIME_CONSUMPTION_MISSING"),
          "runtime-consumption diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics,
                        "ORH_288.MGA_SECURITY_EVIDENCE_MISSING"),
          "MGA/security diagnostic missing: " +
              JoinDiagnostics(validation.diagnostics));
  Require(HasDiagnostic(validation.diagnostics,
                        "ORH_288.DATA_GENERATION_NOT_REPRODUCIBLE"),
          "non-reproducible data diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics,
                        "ORH_288.BENCHMARK_CLEAN_OVERCLAIM"),
          "benchmark-clean overclaim diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics,
                        "ORH_288.COLD_WARM_PHASE_INCOMPLETE"),
          "cold/warm diagnostic missing");
}

void GbScaleCannotPassWithoutExecutionOrBlocker() {
  auto lanes = ValidInfrastructureLanes();
  for (auto& lane : lanes) {
    if (lane.scale_tier == ScaleTier::gb_scale &&
        lane.workload == WorkloadFamily::skewed_join) {
      lane.exact_blocker = false;
      lane.exact_blocker_diagnostic.clear();
    }
  }
  const auto validation = ValidateLargeScaleTierHarness(lanes);
  Require(!validation.ok, "GB-scale lane without execution/blocker accepted");
  Require(HasDiagnostic(validation.diagnostics,
                        "ORH_288.GB_SCALE_BLOCKER_REQUIRED"),
          "GB-scale blocker diagnostic missing");
}

}  // namespace

int main() {
  ValidInfrastructureIsCompletedBlocked();
  NegativeCasesFailClosed();
  GbScaleCannotPassWithoutExecutionOrBlocker();
  std::cout << "optimizer_runtime_hot_path_orh_288_gate=passed\n";
  return EXIT_SUCCESS;
}
