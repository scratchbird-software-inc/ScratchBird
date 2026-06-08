// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_route_capability.hpp"
#include "runtime_consumption_evidence.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace idx = scratchbird::core::index;
namespace opt = scratchbird::engine::optimizer;

constexpr std::string_view kOk = "ORH_AB_BISECT.OK";
constexpr std::string_view kResultMismatch =
    "ORH_AB_BISECT_RESULT_MISMATCH";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "ORH-287 gate failure: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
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

std::string StableHash(std::vector<std::string> rows) {
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

enum class ProfileKind {
  disabled_baseline,
  single_optimization,
  all_optimizations,
};

const char* ProfileKindName(ProfileKind kind) {
  switch (kind) {
    case ProfileKind::disabled_baseline:
      return "disabled_baseline";
    case ProfileKind::single_optimization:
      return "single_optimization";
    case ProfileKind::all_optimizations:
      return "all_optimizations";
  }
  return "unknown";
}

struct RouteFamily {
  std::string route_family_id;
  std::string route_label;
  idx::IndexRouteKind route = idx::IndexRouteKind::unknown;
  idx::IndexFamily family = idx::IndexFamily::unknown;
};

struct OptimizationProfile {
  std::string profile_id;
  ProfileKind kind = ProfileKind::disabled_baseline;
  std::string isolated_optimization_id;
  std::vector<std::string> toggles;
};

struct RunMetadata {
  std::string source_commit = "orh-287-ci-smoke";
  std::string dataset_lane_id;
  std::string dataset_fingerprint;
  std::string reproducible_seed;
  std::uint64_t artifact_generation = 287;
  std::uint64_t expected_artifact_generation = 287;
  bool stale_artifact = false;
  bool reproducible = true;
};

struct RouteRunCapture {
  RouteFamily route;
  OptimizationProfile profile;
  RunMetadata metadata;
  std::string run_id;
  std::vector<std::string> rows;
  std::string result_hash;
  std::string required_ordering = "stable_route_order";
  std::vector<double> samples_us;
  double p50_us = 0.0;
  double p95_us = 0.0;
  double p99_us = 0.0;
  std::string exact_fallback_reason = "none";
  opt::RuntimeOptimizedPathEvidence runtime_evidence;
  std::vector<std::string> profiler_source_labels = {
      "engine_internal_counter",
      "steady_clock_smoke_gate",
  };
  std::vector<std::string> diagnostics;
  bool benchmark_clean_claim = false;
  bool route_capability_consumed = false;
  bool result_contract_hash_matches_runtime = true;
  bool mga_visibility_evidence_present = true;
  bool security_recheck_evidence_present = true;
  bool donor_parser_client_authority = false;
};

struct ABValidation {
  bool ok = false;
  bool benchmark_clean = false;
  std::string diagnostic_code;
  std::vector<std::string> diagnostics;
  std::vector<std::string> bisection_evidence;
};

opt::RuntimeOptimizedPathEvidence RuntimeEvidence(
    const RouteFamily& route,
    const OptimizationProfile& profile,
    const std::string& result_hash) {
  opt::RuntimeOptimizedPathEvidence evidence;
  evidence.selected_path = profile.profile_id;
  evidence.route_kind = idx::IndexRouteKindName(route.route);
  evidence.transaction_snapshot_class =
      "engine_mga_snapshot_visible_through_local_transaction_id";
  evidence.catalog_epoch = 287;
  evidence.security_epoch = 287;
  evidence.redaction_epoch = 287;
  evidence.provider_generation = 287;
  evidence.result_contract_hash = result_hash;
  evidence.diagnostic_code = "ORH_AB_BISECT.RUNTIME_CAPTURED";
  evidence.runtime_consumed = true;
  evidence.live_execution = true;
  evidence.contract_only = false;
  evidence.consumed_module = route.route_family_id + ".runtime_capture";
  return evidence;
}

OptimizationProfile BaselineProfile() {
  return {"baseline-disabled",
          ProfileKind::disabled_baseline,
          "disabled",
          {"all_runtime_optimizations=off", "exact_fallback=on"}};
}

OptimizationProfile SingleProfile(std::string optimization_id) {
  return {"single-" + optimization_id,
          ProfileKind::single_optimization,
          std::move(optimization_id),
          {"all_runtime_optimizations=off",
           "single_optimization=on",
           "exact_fallback_recheck=on"}};
}

OptimizationProfile AllProfile() {
  return {"all-optimizations",
          ProfileKind::all_optimizations,
          "all",
          {"all_runtime_optimizations=on", "exact_fallback_recheck=on"}};
}

RouteRunCapture Capture(RouteFamily route,
                        OptimizationProfile profile,
                        std::string lane,
                        std::vector<std::string> rows,
                        std::vector<double> samples,
                        std::string fallback_reason) {
  RouteRunCapture capture;
  capture.route = std::move(route);
  capture.profile = std::move(profile);
  capture.metadata.dataset_lane_id = std::move(lane);
  capture.metadata.dataset_fingerprint =
      "dataset:" + capture.metadata.dataset_lane_id + ":orh287";
  capture.metadata.reproducible_seed =
      "seed:287:" + capture.route.route_family_id;
  capture.run_id = capture.metadata.dataset_lane_id + ":" +
                   capture.route.route_family_id + ":" +
                   capture.profile.profile_id;
  capture.rows = std::move(rows);
  capture.result_hash = StableHash(capture.rows);
  capture.samples_us = std::move(samples);
  capture.p50_us = Percentile(capture.samples_us, 50.0);
  capture.p95_us = Percentile(capture.samples_us, 95.0);
  capture.p99_us = Percentile(capture.samples_us, 99.0);
  capture.exact_fallback_reason = std::move(fallback_reason);
  capture.runtime_evidence =
      RuntimeEvidence(capture.route, capture.profile, capture.result_hash);
  capture.route_capability_consumed = true;
  return capture;
}

std::vector<RouteFamily> SelectedRouteFamilies() {
  return {
      {"sql_select_btree_ordered_range",
       "sql_select:btree:ordered_range",
       idx::IndexRouteKind::sql_select,
       idx::IndexFamily::btree},
      {"nosql_document_path_filter",
       "nosql_document:document_path:provider_index",
       idx::IndexRouteKind::nosql_document,
       idx::IndexFamily::document_path},
      {"dml_update_row_locator",
       "dml_update:btree:row_locator_stream",
       idx::IndexRouteKind::dml_update,
       idx::IndexFamily::btree},
  };
}

std::vector<std::string> RowsFor(const RouteFamily& route) {
  if (route.route == idx::IndexRouteKind::nosql_document) {
    return {"doc:001:/tenant/name=alpha", "doc:002:/tenant/name=bravo"};
  }
  if (route.route == idx::IndexRouteKind::dml_update) {
    return {"row:010:updated=v2", "row:011:conflict_checked=false"};
  }
  return {"row:001:key=a", "row:002:key=b", "row:003:key=c"};
}

std::vector<double> SamplesFor(ProfileKind kind) {
  switch (kind) {
    case ProfileKind::disabled_baseline:
      return {142.0, 143.0, 144.0, 145.0, 146.0, 147.0, 148.0};
    case ProfileKind::single_optimization:
      return {112.0, 113.0, 114.0, 115.0, 116.0, 117.0, 118.0};
    case ProfileKind::all_optimizations:
      return {94.0, 95.0, 96.0, 97.0, 98.0, 99.0, 100.0};
  }
  return {1.0};
}

opt::BenchmarkMethodologyRunEvidence MethodologyEvidence(
    const RouteRunCapture& capture) {
  opt::BenchmarkMethodologyRunEvidence run;
  run.run_id = capture.run_id;
  run.route_label = capture.route.route_label;
  run.cache_phase = "warm";
  run.scale_tier = "ci_smoke";
  run.skew_profile = "deterministic_orh287";
  run.repetition_count = capture.samples_us.size();
  run.sample_duration_us = capture.samples_us;
  run.p50_us = capture.p50_us;
  run.p95_us = capture.p95_us;
  run.p99_us = capture.p99_us;
  run.optimization_toggles = capture.profile.toggles;
  run.profiler_source_labels = capture.profiler_source_labels;
  run.latest_scratchbird_baseline_id = "scratchbird-main:orh287";
  run.latest_scratchbird_baseline_p50_us = 160.0;
  run.donor_equivalent_baseline_id = "firebird-equivalent:methodology-only";
  run.donor_equivalent_engine = "firebird";
  run.donor_equivalent_baseline_p50_us = 190.0;
  run.methodology_only = true;
  run.performance_proof = false;
  run.benchmark_clean_claim = false;
  run.diagnostic_code = "ORH_AB_BISECT.METHODOLOGY_CAPTURED";
  return run;
}

void AddDiagnostic(ABValidation* validation, std::string diagnostic) {
  validation->diagnostics.push_back(std::move(diagnostic));
}

bool RouteCapabilityIsConsumable(const RouteRunCapture& capture) {
  const auto* state = idx::FindBuiltinIndexRouteCapabilityState(
      capture.route.route, capture.route.family);
  return state != nullptr && state->route_complete() &&
         state->requires_mga_recheck && state->requires_security_recheck;
}

void ValidateCapture(const RouteRunCapture& capture,
                     ABValidation* validation) {
  if (capture.run_id.empty() ||
      capture.metadata.source_commit.empty() ||
      capture.metadata.dataset_lane_id.empty() ||
      capture.metadata.dataset_fingerprint.empty() ||
      capture.metadata.reproducible_seed.empty() ||
      !capture.metadata.reproducible) {
    AddDiagnostic(validation,
                  capture.run_id + ":ORH_AB_BISECT_RUN_METADATA_NOT_REPRODUCIBLE");
  }
  if (capture.metadata.stale_artifact ||
      capture.metadata.artifact_generation !=
          capture.metadata.expected_artifact_generation) {
    AddDiagnostic(validation,
                  capture.run_id + ":ORH_AB_BISECT_STALE_BENCHMARK_ARTIFACT");
  }
  if (capture.route.route_label.empty()) {
    AddDiagnostic(validation,
                  capture.run_id + ":ORH_AB_BISECT_ROUTE_LABEL_MISSING");
  }
  if (capture.result_hash.empty() ||
      capture.runtime_evidence.result_contract_hash.empty() ||
      !capture.result_contract_hash_matches_runtime ||
      capture.runtime_evidence.result_contract_hash != capture.result_hash) {
    AddDiagnostic(validation,
                  capture.run_id + ":ORH_AB_BISECT_RESULT_CONTRACT_MISMATCH");
  }
  if (capture.required_ordering.empty()) {
    AddDiagnostic(validation,
                  capture.run_id + ":ORH_AB_BISECT_REQUIRED_ORDERING_MISSING");
  }
  if (capture.samples_us.empty() || capture.p50_us <= 0.0 ||
      capture.p95_us <= 0.0 || capture.p99_us <= 0.0) {
    AddDiagnostic(validation,
                  capture.run_id + ":ORH_AB_BISECT_TIMING_FIELDS_MISSING");
  }
  if (capture.profile.kind == ProfileKind::disabled_baseline &&
      capture.exact_fallback_reason.empty()) {
    AddDiagnostic(validation,
                  capture.run_id + ":ORH_AB_BISECT_EXACT_FALLBACK_REASON_MISSING");
  }
  if (capture.donor_parser_client_authority) {
    AddDiagnostic(validation,
                  capture.run_id + ":ORH_AB_BISECT_UNSAFE_DONOR_PARSER_CLIENT_AUTHORITY");
  }
  if (!capture.mga_visibility_evidence_present ||
      !capture.security_recheck_evidence_present ||
      capture.runtime_evidence.transaction_snapshot_class.empty() ||
      capture.runtime_evidence.security_epoch == 0 ||
      capture.runtime_evidence.redaction_epoch == 0) {
    AddDiagnostic(validation,
                  capture.run_id + ":ORH_AB_BISECT_MGA_SECURITY_EVIDENCE_MISSING");
  }
  const auto runtime =
      opt::ValidateRuntimeOptimizedPathEvidence(capture.runtime_evidence);
  if (!runtime.ok ||
      runtime.state != opt::RuntimeConsumptionState::kRuntimeConsumed) {
    AddDiagnostic(validation,
                  capture.run_id + ":ORH_AB_BISECT_RUNTIME_CONSUMPTION_MISSING");
  }
  if (capture.runtime_evidence.contract_only) {
    AddDiagnostic(validation,
                  capture.run_id + ":ORH_AB_BISECT_CONTRACT_ONLY_EVIDENCE");
  }
  if (!capture.route_capability_consumed ||
      !RouteCapabilityIsConsumable(capture)) {
    AddDiagnostic(validation,
                  capture.run_id + ":ORH_AB_BISECT_ROUTE_CAPABILITY_NOT_CONSUMED");
  }
  auto warm = MethodologyEvidence(capture);
  auto cold = warm;
  cold.run_id += ":cold";
  cold.cache_phase = "cold";
  const auto methodology =
      opt::ValidateBenchmarkMethodologyEvidence({cold, warm});
  if (!methodology.ok) {
    for (const auto& item : methodology.diagnostics) {
      AddDiagnostic(validation,
                    capture.run_id + ":ORH_AB_BISECT_METHODOLOGY_INVALID:" +
                        item);
    }
  }
  if (capture.benchmark_clean_claim) {
    AddDiagnostic(validation,
                  capture.run_id + ":ORH_AB_BISECT_BENCHMARK_CLEAN_OVERCLAIM");
  }
}

std::string GroupKey(const RouteRunCapture& capture) {
  return capture.metadata.dataset_lane_id + "|" + capture.route.route_family_id;
}

ABValidation ValidateABBisectHarness(
    const std::vector<RouteRunCapture>& captures) {
  ABValidation validation;
  if (captures.empty()) {
    validation.diagnostic_code = "ORH_AB_BISECT_NO_CAPTURES";
    validation.diagnostics.push_back("capture set is empty");
    return validation;
  }
  for (const auto& capture : captures) {
    ValidateCapture(capture, &validation);
  }

  std::map<std::string, std::vector<const RouteRunCapture*>> groups;
  for (const auto& capture : captures) {
    groups[GroupKey(capture)].push_back(&capture);
  }
  for (const auto& [key, group] : groups) {
    const RouteRunCapture* baseline = nullptr;
    const RouteRunCapture* all = nullptr;
    std::vector<const RouteRunCapture*> singles;
    for (const auto* capture : group) {
      switch (capture->profile.kind) {
        case ProfileKind::disabled_baseline:
          baseline = capture;
          break;
        case ProfileKind::single_optimization:
          singles.push_back(capture);
          break;
        case ProfileKind::all_optimizations:
          all = capture;
          break;
      }
    }
    if (baseline == nullptr || all == nullptr || singles.empty()) {
      AddDiagnostic(&validation,
                    key + ":ORH_AB_BISECT_PROFILE_COVERAGE_INCOMPLETE");
      continue;
    }
    auto compare = [&](const RouteRunCapture& candidate,
                       const std::string& label) {
      if (candidate.result_hash != baseline->result_hash ||
          candidate.rows != baseline->rows ||
          candidate.required_ordering != baseline->required_ordering ||
          candidate.diagnostics != baseline->diagnostics) {
        AddDiagnostic(&validation,
                      key + ":" + std::string(kResultMismatch) + ":" + label);
        validation.bisection_evidence.push_back(
            "regression_isolated=" + candidate.profile.isolated_optimization_id);
      }
    };
    for (const auto* single : singles) {
      compare(*single, single->profile.isolated_optimization_id);
    }
    compare(*all, "all_optimizations");
    validation.bisection_evidence.push_back(
        key + ":baseline_hash=" + baseline->result_hash);
    validation.bisection_evidence.push_back(
        key + ":all_hash=" + all->result_hash);
    validation.bisection_evidence.push_back(
        key + ":exact_fallback_reason=" + baseline->exact_fallback_reason);
  }

  validation.ok = validation.diagnostics.empty();
  validation.benchmark_clean = validation.ok;
  validation.diagnostic_code = validation.ok ? std::string(kOk)
                                             : "ORH_AB_BISECT_FAIL_CLOSED";
  return validation;
}

std::vector<RouteRunCapture> ValidCaptureSet() {
  std::vector<RouteRunCapture> captures;
  for (const auto& route : SelectedRouteFamilies()) {
    const auto rows = RowsFor(route);
    captures.push_back(Capture(route,
                               BaselineProfile(),
                               "ci-smoke-lane",
                               rows,
                               SamplesFor(ProfileKind::disabled_baseline),
                               "optimization_disabled_exact_fallback"));
    captures.push_back(Capture(route,
                               SingleProfile(route.route_family_id),
                               "ci-smoke-lane",
                               rows,
                               SamplesFor(ProfileKind::single_optimization),
                               "none"));
    captures.push_back(Capture(route,
                               AllProfile(),
                               "ci-smoke-lane",
                               rows,
                               SamplesFor(ProfileKind::all_optimizations),
                               "none"));
  }
  return captures;
}

RouteRunCapture FirstSingle(std::vector<RouteRunCapture>* captures) {
  for (auto& capture : *captures) {
    if (capture.profile.kind == ProfileKind::single_optimization) {
      return capture;
    }
  }
  Fail("single optimization capture missing");
}

void ValidHarnessAcceptsABAndBisectEvidence() {
  const auto validation = ValidateABBisectHarness(ValidCaptureSet());
  if (!validation.ok) {
    Fail("valid A/B harness evidence failed: " +
         JoinDiagnostics(validation.diagnostics));
  }
  Require(validation.benchmark_clean,
          "valid A/B harness did not become benchmark-clean");
  Require(validation.diagnostic_code == kOk,
          "valid A/B harness diagnostic drifted");
  Require(HasDiagnostic(validation.bisection_evidence,
                        "exact_fallback_reason=optimization_disabled_exact_fallback"),
          "exact fallback reason evidence missing");
  Require(HasDiagnostic(validation.bisection_evidence, "baseline_hash="),
          "baseline hash evidence missing");
}

void ResultMismatchFailsClosedAndIsolatesOptimization() {
  auto captures = ValidCaptureSet();
  for (auto& capture : captures) {
    if (capture.profile.kind == ProfileKind::single_optimization) {
      capture.rows.push_back("unexpected-row");
      capture.result_hash = StableHash(capture.rows);
      capture.runtime_evidence.result_contract_hash = capture.result_hash;
      break;
    }
  }
  const auto validation = ValidateABBisectHarness(captures);
  Require(!validation.ok && !validation.benchmark_clean,
          "result mismatch accepted");
  Require(HasDiagnostic(validation.diagnostics, kResultMismatch),
          "result mismatch diagnostic missing");
  Require(HasDiagnostic(validation.bisection_evidence, "regression_isolated="),
          "bisection isolation evidence missing");
}

void ContractOnlyAndMissingRuntimeFailClosed() {
  auto captures = ValidCaptureSet();
  auto& contract = captures.front();
  contract.runtime_evidence.contract_only = true;
  contract.runtime_evidence.runtime_consumed = false;
  contract.runtime_evidence.live_execution = false;
  contract.runtime_evidence.consumed_module.clear();
  contract.runtime_evidence.fallback_reason = "contract_only_descriptor";
  const auto validation = ValidateABBisectHarness(captures);
  Require(!validation.ok, "contract-only evidence accepted");
  Require(HasDiagnostic(validation.diagnostics,
                        "ORH_AB_BISECT_CONTRACT_ONLY_EVIDENCE"),
          "contract-only diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics,
                        "ORH_AB_BISECT_RUNTIME_CONSUMPTION_MISSING"),
          "missing runtime diagnostic missing");
}

void MetadataAuthorityAndArtifactNegativesFailClosed() {
  auto captures = ValidCaptureSet();
  captures[0].metadata.stale_artifact = true;
  captures[1].donor_parser_client_authority = true;
  captures[2].metadata.reproducible = false;
  captures[3].mga_visibility_evidence_present = false;
  captures[4].security_recheck_evidence_present = false;
  captures[5].route.route_label.clear();
  captures[6].route_capability_consumed = false;
  captures[7].result_contract_hash_matches_runtime = false;
  captures[8].samples_us.clear();
  captures[8].p50_us = 0.0;
  const auto validation = ValidateABBisectHarness(captures);
  Require(!validation.ok, "negative capture set accepted");
  Require(HasDiagnostic(validation.diagnostics,
                        "ORH_AB_BISECT_STALE_BENCHMARK_ARTIFACT"),
          "stale artifact diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics,
                        "ORH_AB_BISECT_UNSAFE_DONOR_PARSER_CLIENT_AUTHORITY"),
          "unsafe authority diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics,
                        "ORH_AB_BISECT_RUN_METADATA_NOT_REPRODUCIBLE"),
          "non-reproducible metadata diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics,
                        "ORH_AB_BISECT_MGA_SECURITY_EVIDENCE_MISSING"),
          "MGA/security diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics,
                        "ORH_AB_BISECT_ROUTE_LABEL_MISSING"),
          "route label diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics,
                        "ORH_AB_BISECT_ROUTE_CAPABILITY_NOT_CONSUMED"),
          "route capability diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics,
                        "ORH_AB_BISECT_RESULT_CONTRACT_MISMATCH"),
          "result contract mismatch diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics,
                        "ORH_AB_BISECT_TIMING_FIELDS_MISSING"),
          "timing fields diagnostic missing");
}

void CoverageIncompleteAndOverclaimFailClosed() {
  auto captures = ValidCaptureSet();
  captures.erase(std::remove_if(captures.begin(),
                                captures.end(),
                                [](const auto& capture) {
                                  return capture.profile.kind ==
                                         ProfileKind::all_optimizations;
                                }),
                 captures.end());
  captures.front().benchmark_clean_claim = true;
  const auto validation = ValidateABBisectHarness(captures);
  Require(!validation.ok, "profile coverage/overclaim accepted");
  Require(HasDiagnostic(validation.diagnostics,
                        "ORH_AB_BISECT_PROFILE_COVERAGE_INCOMPLETE"),
          "profile coverage diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics,
                        "ORH_AB_BISECT_BENCHMARK_CLEAN_OVERCLAIM"),
          "benchmark clean overclaim diagnostic missing");
}

}  // namespace

int main() {
  ValidHarnessAcceptsABAndBisectEvidence();
  ResultMismatchFailsClosedAndIsolatesOptimization();
  ContractOnlyAndMissingRuntimeFailClosed();
  MetadataAuthorityAndArtifactNegativesFailClosed();
  CoverageIncompleteAndOverclaimFailClosed();
  std::cout << "optimizer_runtime_hot_path_orh_287_gate=passed\n";
  return EXIT_SUCCESS;
}
