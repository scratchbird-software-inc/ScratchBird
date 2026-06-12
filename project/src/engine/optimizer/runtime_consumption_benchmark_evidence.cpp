// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: OEIC_OPTIMIZER_MAINTAINABILITY_REFACTOR
// Runtime benchmark, reference-comparison, route-equivalence, and driver-visible
// evidence validators are kept separate from core runtime-consumption
// classification so the enterprise optimizer evidence surface remains
// auditable by domain.

#include "runtime_consumption_evidence.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <string_view>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

constexpr std::string_view kEmbeddedRoute = "embedded";
constexpr std::string_view kIpcRoute = "ipc";
constexpr std::string_view kInetRoute = "inet";
constexpr std::string_view kCliRoute = "cli";
constexpr std::string_view kDriverRoute = "driver";

bool Empty(std::string_view value) {
  return value.empty();
}

void RequireField(ReferenceDominanceTargetValidation* validation,
                  bool present,
                  std::string field_name) {
  if (!present) validation->missing_fields.push_back(std::move(field_name));
}

void RequireField(FixedRouteOverheadValidation* validation,
                  bool present,
                  std::string field_name) {
  if (!present) validation->missing_fields.push_back(std::move(field_name));
}

void RequireField(BenchmarkResultFastPathValidation* validation,
                  bool present,
                  std::string field_name) {
  if (!present) validation->missing_fields.push_back(std::move(field_name));
}

bool Positive(double value) {
  return value > 0.0;
}

// 24-reference benchmark comparison contract. References remain reference evidence only;
// none of these names can become ScratchBird transaction, visibility, storage,
// parser, security, or recovery authority.
bool IsReferenceEngine(std::string_view engine) {
  return engine == "firebird" || engine == "mysql" ||
         engine == "postgresql" || engine == "sqlite" ||
         engine == "mariadb" || engine == "oracle" ||
         engine == "sqlserver" || engine == "db2" ||
         engine == "sybase" || engine == "informix" ||
         engine == "teradata" || engine == "snowflake" ||
         engine == "bigquery" || engine == "redshift" ||
         engine == "clickhouse" || engine == "duckdb" ||
         engine == "mongodb" || engine == "cassandra" ||
         engine == "couchbase" || engine == "redis" ||
         engine == "neo4j" || engine == "elasticsearch" ||
         engine == "solr" || engine == "cockroachdb";
}

bool IsDriverVisibleRoute(std::string_view route_kind) {
  return route_kind == kEmbeddedRoute || route_kind == kIpcRoute ||
         route_kind == kInetRoute || route_kind == kCliRoute ||
         route_kind == kDriverRoute;
}

std::string RunPrefix(const BenchmarkMethodologyRunEvidence& run) {
  return run.run_id.empty() ? "unnamed" : run.run_id;
}

std::string LanePrefix(const OptimizerBenchmarkRouteLaneEvidence& lane) {
  if (!lane.lane_id.empty()) return lane.lane_id;
  return lane.route_label.empty() ? "unnamed_lane" : lane.route_label;
}

std::string RoutePrefix(const CrossRouteResultEvidence& route) {
  if (!route.route_kind.empty()) return route.route_kind;
  return route.route_label.empty() ? "unnamed_route" : route.route_label;
}

std::string RoutePrefix(const DriverVisibleExplainRouteEvidence& route) {
  if (!route.route_kind.empty()) return route.route_kind;
  return route.route_label.empty() ? "unnamed_route" : route.route_label;
}

void AddDiagnostic(BenchmarkEquivalenceValidation* validation,
                   std::string diagnostic) {
  validation->diagnostics.push_back(std::move(diagnostic));
}

void AddDiagnostic(BenchmarkMethodologyValidation* validation,
                   const BenchmarkMethodologyRunEvidence& run,
                   std::string diagnostic) {
  validation->diagnostics.push_back(RunPrefix(run) + ":" +
                                    std::move(diagnostic));
}

void AddDiagnostic(CrossRouteEquivalenceValidation* validation,
                   const CrossRouteResultEvidence& route,
                   std::string diagnostic) {
  validation->diagnostics.push_back(RoutePrefix(route) + ":" +
                                    std::move(diagnostic));
}

void AddDiagnostic(OptimizerBenchmarkRouteEvidenceValidation* validation,
                   const OptimizerBenchmarkRouteLaneEvidence& lane,
                   std::string diagnostic) {
  validation->diagnostics.push_back(LanePrefix(lane) + ":" +
                                    std::move(diagnostic));
}

void AddDiagnostic(DriverVisibleExplainRouteValidation* validation,
                   const DriverVisibleExplainRouteEvidence& route,
                   std::string diagnostic) {
  validation->diagnostics.push_back(RoutePrefix(route) + ":" +
                                    std::move(diagnostic));
}

bool HasAuthorityDrift(const OptimizerBenchmarkRouteLaneEvidence& lane) {
  return lane.reference_as_authority || lane.benchmark_evidence_authority ||
         lane.transaction_finality_authority || lane.visibility_authority ||
         lane.security_authority || lane.recovery_authority;
}

bool HasAuthorityDrift(const DriverVisibleExplainRouteEvidence& route) {
  return route.driver_or_benchmark_authority ||
         route.transaction_finality_authority || route.visibility_authority ||
         route.security_authority || route.recovery_authority;
}

bool ContainsEmptyLabel(const std::vector<std::string>& labels) {
  return std::any_of(labels.begin(), labels.end(), [](const auto& label) {
    return label.empty();
  });
}

double NearestRankPercentile(std::vector<double> samples,
                             double percentile) {
  if (samples.empty()) return 0.0;
  std::sort(samples.begin(), samples.end());
  const auto raw_rank =
      static_cast<std::size_t>(std::ceil((percentile / 100.0) *
                                         static_cast<double>(samples.size())));
  const auto index = raw_rank == 0 ? 0 : raw_rank - 1;
  return samples[std::min(index, samples.size() - 1)];
}

bool SameMetric(double lhs, double rhs) {
  return std::abs(lhs - rhs) <= 0.0001;
}

bool HasPlaceholderProductionEvidenceValues(
    const RuntimeOptimizedPathEvidence& evidence) {
  return evidence.catalog_epoch == 1 || evidence.security_epoch == 1 ||
         evidence.redaction_epoch == 1 || evidence.provider_generation == 1 ||
         evidence.result_contract_hash == "result-contract-v1";
}

bool RuntimeEvidenceIsCleanlyConsumed(
    const RuntimeOptimizedPathEvidence& evidence) {
  const auto validation = ValidateRuntimeOptimizedPathEvidence(evidence);
  return validation.ok &&
         validation.state == RuntimeConsumptionState::kRuntimeConsumed &&
         !HasPlaceholderProductionEvidenceValues(evidence);
}

bool RuntimeEvidenceSupportsExactDecision(
    const RuntimeOptimizedPathEvidence& evidence) {
  const auto validation = ValidateRuntimeOptimizedPathEvidence(evidence);
  return validation.ok && !evidence.diagnostic_code.empty() &&
         (validation.state == RuntimeConsumptionState::kRuntimeConsumed ||
          ((validation.state == RuntimeConsumptionState::kSelectionOnly ||
            validation.state == RuntimeConsumptionState::kContractOnlyBlocker) &&
           !evidence.fallback_reason.empty()));
}

bool UsesEngineMgaTransactionAuthority(std::string_view authority) {
  return authority == "engine.mga.transaction_inventory" ||
         authority == "engine.mga.transaction_manager";
}

bool FixedOverheadCountersAreZero(
    const FixedRouteOverheadEvidence& evidence) {
  return evidence.repeated_parse_count == 0 &&
         evidence.repeated_lower_count == 0 &&
         evidence.repeated_descriptor_build_count == 0 &&
         evidence.repeated_result_shape_build_count == 0 &&
         evidence.repeated_text_render_count == 0;
}

void AddFixedRouteFallback(FixedRouteOverheadValidation* validation,
                           std::string diagnostic) {
  validation->exact_fallback = true;
  validation->diagnostics.push_back(std::move(diagnostic));
}

void AddBinaryFastPathFallback(BenchmarkResultFastPathValidation* validation,
                               std::string diagnostic) {
  validation->exact_fallback = true;
  validation->diagnostics.push_back(std::move(diagnostic));
}

}  // namespace

ReferenceDominanceTargetValidation ValidateReferenceDominanceTargetEvidence(
    const ReferenceDominanceTargetEvidence& evidence) {
  ReferenceDominanceTargetValidation validation;

  RequireField(&validation, !Empty(evidence.workload), "workload");
  RequireField(&validation, !Empty(evidence.category), "category");
  RequireField(&validation,
               !Empty(evidence.comparable_status),
               "comparable_status");
  RequireField(&validation,
               Positive(evidence.scratchbird_current_duration_ms),
               "scratchbird_current_duration_ms");
  RequireField(&validation,
               !Empty(evidence.diagnostic_code),
               "diagnostic_code");

  if (evidence.comparable) {
    RequireField(&validation,
                 evidence.comparable_status == "comparable",
                 "comparable_status");
    RequireField(&validation,
                 !Empty(evidence.reference_best_engine),
                 "reference_best_engine");
    RequireField(&validation,
                 Positive(evidence.reference_best_duration_ms),
                 "reference_best_duration_ms");
    RequireField(&validation,
                 Positive(evidence.dominance_target_duration_ms),
                 "dominance_target_duration_ms");
    RequireField(&validation,
                 !Empty(evidence.dominance_target_rule),
                 "dominance_target_rule");
    RequireField(&validation,
                 !Empty(evidence.exact_blocker_rule),
                 "exact_blocker_rule");
    if (evidence.prior_scratchbird_duration_available) {
      RequireField(&validation,
                   Positive(evidence.scratchbird_prior_duration_ms),
                   "scratchbird_prior_duration_ms");
    }
    if (evidence.dominance_target_rule !=
        "strictly_less_than_reference_best_duration") {
      validation.diagnostics.push_back(
          "comparable dominance target must be strictly_less_than_reference_best_duration");
    }
    if (Positive(evidence.dominance_target_duration_ms) &&
        Positive(evidence.reference_best_duration_ms) &&
        evidence.dominance_target_duration_ms >= evidence.reference_best_duration_ms) {
      validation.diagnostics.push_back(
          "dominance target must be faster than reference_best_duration_ms");
    }
  } else {
    RequireField(&validation,
                 evidence.comparable_status == "non_comparable",
                 "comparable_status");
    RequireField(&validation,
                 !Empty(evidence.dominance_target_rule),
                 "dominance_target_rule");
    RequireField(&validation,
                 !Empty(evidence.exact_blocker_rule),
                 "exact_blocker_rule");
    if (!evidence.reference_best_engine.empty() ||
        Positive(evidence.reference_best_duration_ms)) {
      validation.diagnostics.push_back(
          "non-comparable workload cannot assert reference-best timing");
    }
  }

  validation.ok = validation.missing_fields.empty() &&
                  validation.diagnostics.empty();
  if (validation.ok) {
    validation.diagnostic_code = "SB_ORH_REFERENCE_DOMINANCE_TARGET.OK";
  } else if (!validation.missing_fields.empty()) {
    validation.diagnostic_code =
        "SB_ORH_REFERENCE_DOMINANCE_TARGET.MISSING_REQUIRED_FIELD";
  } else {
    validation.diagnostic_code =
        "SB_ORH_REFERENCE_DOMINANCE_TARGET.INVALID_CONTRACT";
  }
  return validation;
}

ReferenceDominanceTargetSetValidation ValidateReferenceDominanceTargetSet(
    const std::vector<ReferenceDominanceTargetEvidence>& evidence,
    const std::vector<std::string>& required_workloads) {
  ReferenceDominanceTargetSetValidation validation;
  std::set<std::string> seen;
  for (const auto& item : evidence) {
    const auto item_validation = ValidateReferenceDominanceTargetEvidence(item);
    if (!item_validation.ok) {
      validation.diagnostics.push_back(item.workload + ":" +
                                       item_validation.diagnostic_code);
    }
    if (!seen.insert(item.workload).second) {
      validation.diagnostics.push_back(
          item.workload + ":SB_ORH_REFERENCE_DOMINANCE_TARGET.DUPLICATE_WORKLOAD");
    }
  }
  for (const auto& workload : required_workloads) {
    if (seen.find(workload) == seen.end()) {
      validation.diagnostics.push_back(
          workload + ":SB_ORH_REFERENCE_DOMINANCE_TARGET.MISSING_WORKLOAD");
    }
  }

  validation.ok = validation.diagnostics.empty();
  validation.diagnostic_code =
      validation.ok ? "SB_ORH_REFERENCE_DOMINANCE_TARGET_SET.OK"
                    : "SB_ORH_REFERENCE_DOMINANCE_TARGET_SET.INVALID";
  return validation;
}

BenchmarkEquivalenceValidation ValidateBestMethodBenchmarkEquivalence(
    const std::vector<BenchmarkMethodEvidence>& methods,
    const std::vector<std::string>& required_engines) {
  BenchmarkEquivalenceValidation validation;
  if (methods.empty()) {
    validation.diagnostic_code =
        "SB_ORH_BEST_METHOD_EQUIVALENCE.NO_METHODS";
    validation.diagnostics.push_back("no benchmark methods provided");
    return validation;
  }

  const auto& first = methods.front();
  std::set<std::string> engines_seen;
  for (const auto& method : methods) {
    if (method.diagnostic_code.empty()) {
      AddDiagnostic(&validation,
                    method.engine +
                        ":SB_ORH_BEST_METHOD_EQUIVALENCE.DIAGNOSTIC_REQUIRED");
    }
    if (method.engine.empty() || method.logical_task.empty() ||
        method.workload_family.empty() || method.method.empty()) {
      AddDiagnostic(&validation,
                    method.engine +
                        ":SB_ORH_BEST_METHOD_EQUIVALENCE.MISSING_IDENTITY");
    }
    if (!method.best_normal_method) {
      AddDiagnostic(&validation,
                    method.engine +
                        ":SB_ORH_BEST_METHOD_EQUIVALENCE.NOT_BEST_NORMAL_METHOD");
    }
    if (!method.prepared_or_warmed) {
      AddDiagnostic(&validation,
                    method.engine +
                        ":SB_ORH_BEST_METHOD_EQUIVALENCE.NOT_PREPARED_OR_WARMED");
    }
    if (!method.output_suppressed) {
      AddDiagnostic(&validation,
                    method.engine +
                        ":SB_ORH_BEST_METHOD_EQUIVALENCE.OUTPUT_NOT_SUPPRESSED");
    }
    if (method.result_materialization_policy.empty() ||
        method.transaction_policy.empty() ||
        method.data_generator_id.empty() || method.scale_profile.empty() ||
        method.skew_profile.empty() || method.resource_budget_profile.empty() ||
        method.constraint_policy.empty()) {
      AddDiagnostic(&validation,
                    method.engine +
                        ":SB_ORH_BEST_METHOD_EQUIVALENCE.MISSING_CONTROL");
    }
    if (method.logical_task != first.logical_task ||
        method.workload_family != first.workload_family) {
      AddDiagnostic(&validation,
                    method.engine +
                        ":SB_ORH_BEST_METHOD_EQUIVALENCE.LOGICAL_TASK_MISMATCH");
    }
    if (method.result_materialization_policy !=
            first.result_materialization_policy ||
        method.transaction_policy != first.transaction_policy ||
        method.data_generator_id != first.data_generator_id ||
        method.scale_profile != first.scale_profile ||
        method.skew_profile != first.skew_profile ||
        method.resource_budget_profile != first.resource_budget_profile ||
        method.constraint_policy != first.constraint_policy) {
      AddDiagnostic(&validation,
                    method.engine +
                        ":SB_ORH_BEST_METHOD_EQUIVALENCE.CONTROL_MISMATCH");
    }
    if (!method.native_bulk_or_best_engine_path) {
      AddDiagnostic(&validation,
                    method.engine +
                        ":SB_ORH_BEST_METHOD_EQUIVALENCE.BEST_ENGINE_PATH_UNPROVEN");
    }
    if (IsReferenceEngine(method.engine) && !method.reference_reference_only) {
      AddDiagnostic(&validation,
                    method.engine +
                        ":SB_ORH_BEST_METHOD_EQUIVALENCE.REFERENCE_NOT_REFERENCE_ONLY");
    }
    if (method.uses_reference_storage_or_finality_for_scratchbird) {
      AddDiagnostic(&validation,
                    method.engine +
                        ":SB_ORH_BEST_METHOD_EQUIVALENCE.MGA_AUTHORITY_DRIFT");
    }
    engines_seen.insert(method.engine);
  }

  for (const auto& engine : required_engines) {
    if (engines_seen.find(engine) == engines_seen.end()) {
      AddDiagnostic(&validation,
                    engine +
                        ":SB_ORH_BEST_METHOD_EQUIVALENCE.MISSING_ENGINE");
    }
  }

  validation.ok = validation.diagnostics.empty();
  validation.diagnostic_code =
      validation.ok ? "SB_ORH_BEST_METHOD_EQUIVALENCE.OK"
                    : "SB_ORH_BEST_METHOD_EQUIVALENCE.FAILED";
  return validation;
}

BenchmarkMethodologyValidation ValidateBenchmarkMethodologyEvidence(
    const std::vector<BenchmarkMethodologyRunEvidence>& runs) {
  BenchmarkMethodologyValidation validation;
  if (runs.empty()) {
    validation.diagnostic_code = "SB_ORH_BENCHMARK_METHODOLOGY.NO_RUNS";
    validation.diagnostics.push_back("no benchmark methodology runs provided");
    return validation;
  }

  bool saw_cold = false;
  bool saw_warm = false;
  bool benchmark_clean_claimed = false;

  for (const auto& run : runs) {
    if (run.diagnostic_code.empty()) {
      AddDiagnostic(&validation,
                    run,
                    "SB_ORH_BENCHMARK_METHODOLOGY.DIAGNOSTIC_REQUIRED");
    }
    if (run.route_label.empty()) {
      AddDiagnostic(&validation,
                    run,
                    "SB_ORH_BENCHMARK_METHODOLOGY.MISSING_ROUTE_LABEL");
    }
    if (run.cache_phase.empty()) {
      AddDiagnostic(&validation,
                    run,
                    "SB_ORH_BENCHMARK_METHODOLOGY.CACHE_PHASE_MISSING");
    } else if (run.cache_phase == "cold") {
      saw_cold = true;
    } else if (run.cache_phase == "warm") {
      saw_warm = true;
    } else {
      AddDiagnostic(&validation,
                    run,
                    "SB_ORH_BENCHMARK_METHODOLOGY.CACHE_PHASE_INVALID");
    }
    if (run.scale_tier.empty()) {
      AddDiagnostic(&validation,
                    run,
                    "SB_ORH_BENCHMARK_METHODOLOGY.SCALE_TIER_MISSING");
    }
    if (run.skew_profile.empty()) {
      AddDiagnostic(&validation,
                    run,
                    "SB_ORH_BENCHMARK_METHODOLOGY.SKEW_PROFILE_MISSING");
    }
    if (run.repetition_count != run.sample_duration_us.size()) {
      AddDiagnostic(&validation,
                    run,
                    "SB_ORH_BENCHMARK_METHODOLOGY.SAMPLE_COUNT_MISMATCH");
    }
    if (run.repetition_count < 2 || run.sample_duration_us.size() < 5) {
      AddDiagnostic(&validation,
                    run,
                    "SB_ORH_BENCHMARK_METHODOLOGY.SAMPLES_INSUFFICIENT");
    }
    const bool has_zero_metric =
        run.p50_us <= 0.0 || run.p95_us <= 0.0 || run.p99_us <= 0.0 ||
        std::any_of(run.sample_duration_us.begin(),
                    run.sample_duration_us.end(),
                    [](double value) { return value <= 0.0; });
    if (has_zero_metric) {
      AddDiagnostic(&validation,
                    run,
                    "SB_ORH_BENCHMARK_METHODOLOGY.AMBIGUOUS_ZERO_METRIC");
    } else if (!run.sample_duration_us.empty()) {
      if (!SameMetric(run.p50_us,
                      NearestRankPercentile(run.sample_duration_us, 50.0)) ||
          !SameMetric(run.p95_us,
                      NearestRankPercentile(run.sample_duration_us, 95.0)) ||
          !SameMetric(run.p99_us,
                      NearestRankPercentile(run.sample_duration_us, 99.0)) ||
          run.p50_us > run.p95_us || run.p95_us > run.p99_us) {
        AddDiagnostic(&validation,
                      run,
                      "SB_ORH_BENCHMARK_METHODOLOGY.PERCENTILE_MISMATCH");
      }
    }
    if (run.optimization_toggles.empty() ||
        ContainsEmptyLabel(run.optimization_toggles)) {
      AddDiagnostic(&validation,
                    run,
                    "SB_ORH_BENCHMARK_METHODOLOGY.OPTIMIZATION_TOGGLES_MISSING");
    }
    if (run.profiler_source_labels.empty() ||
        ContainsEmptyLabel(run.profiler_source_labels)) {
      AddDiagnostic(&validation,
                    run,
                    "SB_ORH_BENCHMARK_METHODOLOGY.PROFILER_SOURCE_MISSING");
    }
    if (run.latest_scratchbird_baseline_id.empty() ||
        run.latest_scratchbird_baseline_p50_us <= 0.0) {
      AddDiagnostic(
          &validation,
          run,
          "SB_ORH_BENCHMARK_METHODOLOGY.LATEST_SCRATCHBIRD_BASELINE_MISSING");
    }
    if (run.reference_equivalent_baseline_id.empty() ||
        run.reference_equivalent_engine.empty() ||
        run.reference_equivalent_baseline_p50_us <= 0.0) {
      AddDiagnostic(
          &validation,
          run,
          "SB_ORH_BENCHMARK_METHODOLOGY.REFERENCE_EQUIVALENT_BASELINE_MISSING");
    } else if (!IsReferenceEngine(run.reference_equivalent_engine)) {
      AddDiagnostic(
          &validation,
          run,
          "SB_ORH_BENCHMARK_METHODOLOGY.REFERENCE_EQUIVALENT_ENGINE_UNSUPPORTED");
    }
    if (run.methodology_only == run.performance_proof) {
      AddDiagnostic(&validation,
                    run,
                    "SB_ORH_BENCHMARK_METHODOLOGY.RUN_PURPOSE_AMBIGUOUS");
    }
    if (run.benchmark_clean_claim) {
      benchmark_clean_claimed = true;
    }
    if (run.benchmark_clean_claim && !run.performance_proof) {
      AddDiagnostic(&validation,
                    run,
                    "SB_ORH_BENCHMARK_METHODOLOGY.BENCHMARK_CLEAN_OVERCLAIM");
    }
  }

  if (!saw_cold || !saw_warm) {
    validation.diagnostics.push_back(
        "SB_ORH_BENCHMARK_METHODOLOGY.COLD_WARM_PHASE_INCOMPLETE");
  }

  validation.ok = validation.diagnostics.empty();
  validation.benchmark_clean = validation.ok && benchmark_clean_claimed;
  validation.diagnostic_code =
      validation.ok
          ? "SB_ORH_BENCHMARK_METHODOLOGY.OK"
          : "SB_ORH_BENCHMARK_METHODOLOGY.FAILED";
  return validation;
}

CrossRouteEquivalenceValidation ValidateCrossRouteResultEquivalence(
    const std::vector<CrossRouteResultEvidence>& routes,
    const std::vector<std::string>& required_routes) {
  CrossRouteEquivalenceValidation validation;
  if (routes.empty()) {
    validation.exact_blocker = true;
    validation.diagnostic_code =
        "SB_ORH_CROSS_ROUTE_EQUIVALENCE.NO_ROUTE_EVIDENCE";
    validation.diagnostics.push_back(
        "SB_ORH_CROSS_ROUTE_EQUIVALENCE.NO_ROUTE_EVIDENCE");
    return validation;
  }

  std::set<std::string> seen_routes;
  const CrossRouteResultEvidence* reference = nullptr;
  bool all_live_routes_benchmark_clean = true;

  for (const auto& route : routes) {
    if (route.route_kind.empty()) {
      AddDiagnostic(&validation,
                    route,
                    "SB_ORH_CROSS_ROUTE_EQUIVALENCE.ROUTE_KIND_MISSING");
    } else if (!seen_routes.insert(route.route_kind).second) {
      AddDiagnostic(&validation,
                    route,
                    "SB_ORH_CROSS_ROUTE_EQUIVALENCE.DUPLICATE_ROUTE");
    }
    if (route.route_label.empty()) {
      AddDiagnostic(&validation,
                    route,
                    "SB_ORH_CROSS_ROUTE_EQUIVALENCE.ROUTE_LABEL_MISSING");
    }
    if (route.diagnostic_code.empty()) {
      AddDiagnostic(&validation,
                    route,
                    "SB_ORH_CROSS_ROUTE_EQUIVALENCE.DIAGNOSTIC_REQUIRED");
    }
    if (route.unsupported_route) {
      validation.exact_blocker = true;
      if (route.live_route_executed) {
        AddDiagnostic(
            &validation,
            route,
            "SB_ORH_CROSS_ROUTE_EQUIVALENCE.UNSUPPORTED_ROUTE_CANNOT_BE_LIVE");
      }
      if (route.failure_diagnostic_code.empty()) {
        AddDiagnostic(
            &validation,
            route,
            "SB_ORH_CROSS_ROUTE_EQUIVALENCE.UNSUPPORTED_ROUTE_BLOCKER_REQUIRED");
      }
      if (route.benchmark_clean_claim) {
        AddDiagnostic(
            &validation,
            route,
            "SB_ORH_CROSS_ROUTE_EQUIVALENCE.BENCHMARK_CLEAN_OVERCLAIM");
      }
      continue;
    }

    if (!route.benchmark_clean_claim) {
      all_live_routes_benchmark_clean = false;
    }

    if (!route.live_route_executed) {
      validation.exact_blocker = true;
      AddDiagnostic(&validation,
                    route,
                    "SB_ORH_CROSS_ROUTE_EQUIVALENCE.STATIC_DESCRIPTOR_ONLY");
    }
    if (route.database_parameters_hash.empty()) {
      AddDiagnostic(
          &validation,
          route,
          "SB_ORH_CROSS_ROUTE_EQUIVALENCE.DATABASE_PARAMETERS_MISSING");
    }
    if (route.session_rights_digest.empty() || route.security_epoch == 0) {
      AddDiagnostic(&validation,
                    route,
                    "SB_ORH_CROSS_ROUTE_EQUIVALENCE.SESSION_RIGHTS_MISSING");
    }
    if (route.transaction_snapshot_id.empty() ||
        route.local_transaction_id == 0) {
      AddDiagnostic(
          &validation,
          route,
          "SB_ORH_CROSS_ROUTE_EQUIVALENCE.TRANSACTION_SNAPSHOT_MISSING");
    }
    if (route.result_contract_hash.empty()) {
      AddDiagnostic(&validation,
                    route,
                    "SB_ORH_CROSS_ROUTE_EQUIVALENCE.RESULT_CONTRACT_MISSING");
    }
    if (route.required_ordering.empty()) {
      AddDiagnostic(&validation,
                    route,
                    "SB_ORH_CROSS_ROUTE_EQUIVALENCE.ORDERING_MISSING");
    }
    if (route.parser_owns_visibility_authority ||
        route.parser_owns_transaction_finality ||
        route.client_or_driver_owns_visibility_authority ||
        route.client_or_driver_owns_transaction_finality) {
      AddDiagnostic(&validation,
                    route,
                    "SB_ORH_CROSS_ROUTE_EQUIVALENCE.MGA_AUTHORITY_DRIFT");
    }
    if (!route.accepted &&
        (route.diagnostics.empty() || route.failure_diagnostic_code.empty())) {
      AddDiagnostic(
          &validation,
          route,
          "SB_ORH_CROSS_ROUTE_EQUIVALENCE.EXACT_FAILURE_DIAGNOSTIC_MISSING");
    }

    if (reference == nullptr) {
      reference = &route;
      continue;
    }

    if (route.database_parameters_hash != reference->database_parameters_hash) {
      AddDiagnostic(
          &validation,
          route,
          "SB_ORH_CROSS_ROUTE_EQUIVALENCE.DATABASE_PARAMETERS_MISMATCH");
    }
    if (route.session_rights_digest != reference->session_rights_digest ||
        route.security_epoch != reference->security_epoch ||
        route.redaction_epoch != reference->redaction_epoch) {
      AddDiagnostic(&validation,
                    route,
                    "SB_ORH_CROSS_ROUTE_EQUIVALENCE.SESSION_RIGHTS_MISMATCH");
    }
    if (route.transaction_snapshot_id != reference->transaction_snapshot_id ||
        route.local_transaction_id != reference->local_transaction_id) {
      AddDiagnostic(&validation,
                    route,
                    "SB_ORH_CROSS_ROUTE_EQUIVALENCE.SNAPSHOT_MISMATCH");
    }
    if (route.result_contract_hash != reference->result_contract_hash) {
      AddDiagnostic(&validation,
                    route,
                    "SB_ORH_CROSS_ROUTE_EQUIVALENCE.RESULT_CONTRACT_MISMATCH");
    }
    if (route.required_ordering != reference->required_ordering) {
      AddDiagnostic(&validation,
                    route,
                    "SB_ORH_CROSS_ROUTE_EQUIVALENCE.ORDERING_MISMATCH");
    }
    if (route.accepted != reference->accepted) {
      AddDiagnostic(&validation,
                    route,
                    "SB_ORH_CROSS_ROUTE_EQUIVALENCE.ACCEPTED_STATE_MISMATCH");
    } else if (route.accepted && route.rows != reference->rows) {
      AddDiagnostic(&validation,
                    route,
                    "SB_ORH_CROSS_ROUTE_EQUIVALENCE.ROWS_MISMATCH");
    } else if (!route.accepted &&
               route.diagnostics != reference->diagnostics) {
      AddDiagnostic(&validation,
                    route,
                    "SB_ORH_CROSS_ROUTE_EQUIVALENCE.DIAGNOSTICS_MISMATCH");
    }
  }

  for (const auto& required_route : required_routes) {
    if (seen_routes.find(required_route) == seen_routes.end()) {
      validation.exact_blocker = true;
      validation.diagnostics.push_back(
          required_route +
          ":SB_ORH_CROSS_ROUTE_EQUIVALENCE.MISSING_ROUTE_EVIDENCE");
    }
  }

  validation.ok = validation.diagnostics.empty() && !validation.exact_blocker;
  validation.benchmark_clean =
      validation.ok && all_live_routes_benchmark_clean;
  if (validation.ok) {
    validation.diagnostic_code =
        "SB_ORH_CROSS_ROUTE_EQUIVALENCE.OK";
  } else if (validation.exact_blocker) {
    validation.diagnostic_code =
        "SB_ORH_CROSS_ROUTE_EQUIVALENCE.EXACT_BLOCKER";
  } else {
    validation.diagnostic_code =
        "SB_ORH_CROSS_ROUTE_EQUIVALENCE.FAILED";
  }
  return validation;
}

FixedRouteOverheadValidation ValidateFixedRouteOverheadEvidence(
    const FixedRouteOverheadEvidence& evidence) {
  FixedRouteOverheadValidation validation;

  RequireField(&validation, !Empty(evidence.route_kind), "route_kind");
  RequireField(&validation,
               !Empty(evidence.statement_family),
               "statement_family");
  RequireField(&validation, !Empty(evidence.selected_path), "selected_path");
  RequireField(&validation,
               evidence.route_latency_budget_us > 0,
               "route_latency_budget_us");
  RequireField(&validation,
               evidence.route_latency_observed_us > 0,
               "route_latency_observed_us");
  RequireField(&validation,
               !Empty(evidence.transaction_authority),
               "transaction_authority");
  RequireField(&validation,
               !Empty(evidence.diagnostic_code),
               "diagnostic_code");

  const auto runtime_validation =
      ValidateRuntimeOptimizedPathEvidence(evidence.runtime_evidence);
  if (!runtime_validation.ok) {
    validation.diagnostics.push_back(
        "runtime_evidence:" + runtime_validation.diagnostic_code);
  }
  if (evidence.parser_or_cache_executes_sql) {
    validation.diagnostics.push_back(
        "parser/cache cannot execute SQL for ORH fixed-route reuse");
  }
  if (evidence.parser_or_cache_owns_transaction_finality) {
    validation.diagnostics.push_back(
        "parser/cache cannot own transaction finality");
  }
  if (!evidence.transaction_authority.empty() &&
      !UsesEngineMgaTransactionAuthority(evidence.transaction_authority)) {
    validation.diagnostics.push_back(
        "transaction authority must remain engine MGA-owned");
  }

  const bool coherent = validation.missing_fields.empty() &&
                        validation.diagnostics.empty();
  if (!coherent) {
    validation.ok = false;
    validation.diagnostic_code =
        validation.missing_fields.empty()
            ? "SB_ORH_FIXED_ROUTE_OVERHEAD.INVALID_CONTRACT"
            : "SB_ORH_FIXED_ROUTE_OVERHEAD.MISSING_REQUIRED_FIELD";
    return validation;
  }

  if (evidence.index_dependent && !evidence.index_correctness_proven) {
    AddFixedRouteFallback(
        &validation,
        "SB_ORH_FIXED_ROUTE_OVERHEAD.INDEX_CORRECTNESS_UNPROVEN");
  }
  if (!evidence.warmed_prepared_route) {
    AddFixedRouteFallback(&validation,
                          "SB_ORH_FIXED_ROUTE_OVERHEAD.NOT_WARMED_PREPARED");
  }
  if (!evidence.prepared_template_reused || !evidence.lowered_sblr_reused ||
      !evidence.descriptor_reused || !evidence.result_shape_reused) {
    AddFixedRouteFallback(
        &validation,
        "SB_ORH_FIXED_ROUTE_OVERHEAD.REUSE_EVIDENCE_INCOMPLETE");
  }
  if (!evidence.text_rendering_suppressed ||
      !FixedOverheadCountersAreZero(evidence)) {
    AddFixedRouteFallback(
        &validation,
        "SB_ORH_FIXED_ROUTE_OVERHEAD.REPEATED_FRONTEND_OR_RENDER_OVERHEAD");
  }
  if (evidence.route_latency_observed_us > evidence.route_latency_budget_us) {
    AddFixedRouteFallback(&validation,
                          "SB_ORH_FIXED_ROUTE_OVERHEAD.BUDGET_EXCEEDED");
  }
  if (evidence.benchmark_clean_candidate &&
      !RuntimeEvidenceIsCleanlyConsumed(evidence.runtime_evidence)) {
    AddFixedRouteFallback(
        &validation,
        "SB_ORH_FIXED_ROUTE_OVERHEAD.RUNTIME_CONSUMPTION_MISSING");
  }

  validation.benchmark_clean =
      evidence.benchmark_clean_candidate && !validation.exact_fallback;
  if (validation.exact_fallback &&
      (evidence.fallback_reason.empty() ||
       !RuntimeEvidenceSupportsExactDecision(evidence.runtime_evidence))) {
    validation.ok = false;
    validation.diagnostic_code =
        "SB_ORH_FIXED_ROUTE_OVERHEAD.FALLBACK_EVIDENCE_REQUIRED";
    return validation;
  }

  validation.ok = true;
  validation.diagnostic_code =
      validation.benchmark_clean
          ? "SB_ORH_FIXED_ROUTE_OVERHEAD.BENCHMARK_CLEAN"
          : (validation.exact_fallback
                 ? "SB_ORH_FIXED_ROUTE_OVERHEAD.EXACT_FALLBACK"
                 : "SB_ORH_FIXED_ROUTE_OVERHEAD.NON_BENCHMARK_ROUTE_OK");
  return validation;
}

BenchmarkResultFastPathValidation ValidateBenchmarkResultFastPathEvidence(
    const BenchmarkResultFastPathEvidence& evidence) {
  BenchmarkResultFastPathValidation validation;

  RequireField(&validation, !Empty(evidence.route_kind), "route_kind");
  RequireField(&validation,
               !Empty(evidence.statement_family),
               "statement_family");
  RequireField(&validation,
               !Empty(evidence.result_contract_hash),
               "result_contract_hash");
  RequireField(&validation,
               !Empty(evidence.transaction_authority),
               "transaction_authority");
  RequireField(&validation,
               !Empty(evidence.diagnostic_code),
               "diagnostic_code");
  if (evidence.binary_or_equivalent_frame_selected) {
    RequireField(&validation, !Empty(evidence.frame_kind), "frame_kind");
    RequireField(&validation, !Empty(evidence.frame_version), "frame_version");
  }

  const auto runtime_validation =
      ValidateRuntimeOptimizedPathEvidence(evidence.runtime_evidence);
  if (!runtime_validation.ok) {
    validation.diagnostics.push_back(
        "runtime_evidence:" + runtime_validation.diagnostic_code);
  }
  if (evidence.parser_or_cache_executes_sql) {
    validation.diagnostics.push_back(
        "parser/cache cannot execute SQL for benchmark result fast path");
  }
  if (evidence.parser_or_cache_owns_transaction_finality) {
    validation.diagnostics.push_back(
        "parser/cache cannot own transaction finality");
  }
  if (!evidence.transaction_authority.empty() &&
      !UsesEngineMgaTransactionAuthority(evidence.transaction_authority)) {
    validation.diagnostics.push_back(
        "transaction authority must remain engine MGA-owned");
  }

  const bool coherent = validation.missing_fields.empty() &&
                        validation.diagnostics.empty();
  if (!coherent) {
    validation.ok = false;
    validation.diagnostic_code =
        validation.missing_fields.empty()
            ? "SB_ORH_BINARY_RESULT_FAST_PATH.INVALID_CONTRACT"
            : "SB_ORH_BINARY_RESULT_FAST_PATH.MISSING_REQUIRED_FIELD";
    return validation;
  }

  if (evidence.disabled_or_fallback) {
    AddBinaryFastPathFallback(&validation,
                              "SB_ORH_BINARY_RESULT_FAST_PATH.DISABLED");
  }
  if (!evidence.binary_or_equivalent_frame_selected) {
    AddBinaryFastPathFallback(
        &validation,
        "SB_ORH_BINARY_RESULT_FAST_PATH.NO_BINARY_OR_EQUIVALENT_FRAME");
  }
  if (!evidence.equivalent_result_materialization ||
      !evidence.exact_diagnostics_preserved) {
    AddBinaryFastPathFallback(
        &validation,
        "SB_ORH_BINARY_RESULT_FAST_PATH.RESULT_OR_DIAGNOSTIC_PARITY_MISSING");
  }
  if (!evidence.nonessential_evidence_suppressed_during_timing ||
      !evidence.support_evidence_available_outside_timed_path ||
      !evidence.timed_path_text_rendering_suppressed) {
    AddBinaryFastPathFallback(
        &validation,
        "SB_ORH_BINARY_RESULT_FAST_PATH.EVIDENCE_SUPPRESSION_INCOMPLETE");
  }
  if (evidence.benchmark_clean_candidate &&
      !RuntimeEvidenceIsCleanlyConsumed(evidence.runtime_evidence)) {
    AddBinaryFastPathFallback(
        &validation,
        "SB_ORH_BINARY_RESULT_FAST_PATH.RUNTIME_CONSUMPTION_MISSING");
  }

  validation.benchmark_clean =
      evidence.benchmark_clean_candidate && !validation.exact_fallback;
  if (validation.exact_fallback &&
      (evidence.disabled_reason.empty() ||
       !RuntimeEvidenceSupportsExactDecision(evidence.runtime_evidence))) {
    validation.ok = false;
    validation.diagnostic_code =
        "SB_ORH_BINARY_RESULT_FAST_PATH.FALLBACK_EVIDENCE_REQUIRED";
    return validation;
  }

  validation.ok = true;
  validation.diagnostic_code =
      validation.benchmark_clean
          ? "SB_ORH_BINARY_RESULT_FAST_PATH.BENCHMARK_CLEAN"
          : (validation.exact_fallback
                 ? "SB_ORH_BINARY_RESULT_FAST_PATH.EXACT_FALLBACK"
                 : "SB_ORH_BINARY_RESULT_FAST_PATH.NON_BENCHMARK_ROUTE_OK");
  return validation;
}

OptimizerBenchmarkRouteEvidenceValidation ValidateOptimizerBenchmarkRouteEvidence(
    const std::vector<OptimizerBenchmarkRouteLaneEvidence>& lanes,
    bool reference_comparison_required) {
  OptimizerBenchmarkRouteEvidenceValidation validation;
  if (lanes.empty()) {
    validation.diagnostic_code = "SB_OPCH_BENCHMARK_ROUTE_EVIDENCE.NO_LANES";
    validation.diagnostics.push_back(
        "SB_OPCH_BENCHMARK_ROUTE_EVIDENCE.NO_LANES");
    return validation;
  }

  bool saw_cold = false;
  bool saw_warm = false;
  bool benchmark_clean_claimed = false;
  std::set<std::string> route_labels;

  for (const auto& lane : lanes) {
    if (lane.diagnostic_code.empty()) {
      AddDiagnostic(&validation,
                    lane,
                    "SB_OPCH_BENCHMARK_ROUTE_EVIDENCE.DIAGNOSTIC_REQUIRED");
    }
    if (lane.route_label.empty()) {
      AddDiagnostic(&validation,
                    lane,
                    "SB_OPCH_BENCHMARK_ROUTE_EVIDENCE.ROUTE_LABEL_MISSING");
    } else {
      route_labels.insert(lane.route_label);
    }
    if (lane.cache_phase == "cold") {
      saw_cold = true;
    } else if (lane.cache_phase == "warm") {
      saw_warm = true;
    } else {
      AddDiagnostic(&validation,
                    lane,
                    "SB_OPCH_BENCHMARK_ROUTE_EVIDENCE.CACHE_PHASE_INVALID");
    }
    if (lane.p50_us <= 0.0 || lane.p95_us <= 0.0 || lane.p99_us <= 0.0) {
      AddDiagnostic(&validation,
                    lane,
                    "SB_OPCH_BENCHMARK_ROUTE_EVIDENCE.PERCENTILES_MISSING");
    } else if (lane.p50_us > lane.p95_us || lane.p95_us > lane.p99_us) {
      AddDiagnostic(&validation,
                    lane,
                    "SB_OPCH_BENCHMARK_ROUTE_EVIDENCE.PERCENTILES_NOT_ORDERED");
    }
    if (!lane.trusted || !lane.fresh || lane.evidence_generation.empty()) {
      AddDiagnostic(&validation,
                    lane,
                    "SB_OPCH_BENCHMARK_ROUTE_EVIDENCE.STALE_OR_UNTRUSTED");
    }
    const bool reference_required =
        reference_comparison_required || lane.reference_comparison_required;
    if (reference_required &&
        (lane.reference_comparison_id.empty() || lane.reference_engine.empty() ||
         lane.reference_oracle_result_hash.empty())) {
      AddDiagnostic(
          &validation,
          lane,
          "SB_OPCH_BENCHMARK_ROUTE_EVIDENCE.REFERENCE_COMPARISON_MISSING");
    }
    if (!lane.reference_engine.empty() && !IsReferenceEngine(lane.reference_engine)) {
      AddDiagnostic(
          &validation,
          lane,
          "SB_OPCH_BENCHMARK_ROUTE_EVIDENCE.REFERENCE_ENGINE_UNSUPPORTED");
    }
    if (!lane.reference_reference_only || HasAuthorityDrift(lane)) {
      AddDiagnostic(&validation,
                    lane,
                    "SB_OPCH_BENCHMARK_ROUTE_EVIDENCE.REFERENCE_AUTHORITY_DRIFT");
    }
    if (lane.benchmark_clean_claim) benchmark_clean_claimed = true;
  }

  if (!saw_cold || !saw_warm) {
    validation.diagnostics.push_back(
        "SB_OPCH_BENCHMARK_ROUTE_EVIDENCE.COLD_WARM_LANES_MISSING");
  }
  if (route_labels.empty()) {
    validation.diagnostics.push_back(
        "SB_OPCH_BENCHMARK_ROUTE_EVIDENCE.NO_ROUTE_LABELS");
  }

  validation.ok = validation.diagnostics.empty();
  validation.benchmark_clean = validation.ok && benchmark_clean_claimed;
  validation.diagnostic_code =
      validation.ok ? "SB_OPCH_BENCHMARK_ROUTE_EVIDENCE.OK"
                    : "SB_OPCH_BENCHMARK_ROUTE_EVIDENCE.FAILED";
  return validation;
}

DriverVisibleExplainRouteValidation ValidateDriverVisibleExplainRouteEquivalence(
    const std::vector<DriverVisibleExplainRouteEvidence>& routes,
    const std::vector<std::string>& required_routes) {
  DriverVisibleExplainRouteValidation validation;
  if (routes.empty()) {
    validation.diagnostic_code =
        "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.NO_ROUTES";
    validation.diagnostics.push_back(
        "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.NO_ROUTES");
    return validation;
  }

  const DriverVisibleExplainRouteEvidence* reference = nullptr;
  std::set<std::string> seen_routes;
  for (const auto& route : routes) {
    if (route.route_kind.empty()) {
      AddDiagnostic(
          &validation,
          route,
          "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.ROUTE_KIND_MISSING");
    } else {
      if (!seen_routes.insert(route.route_kind).second) {
        AddDiagnostic(
            &validation,
            route,
            "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.DUPLICATE_ROUTE");
      }
      if (!IsDriverVisibleRoute(route.route_kind)) {
        AddDiagnostic(
            &validation,
            route,
            "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.ROUTE_UNSUPPORTED");
      }
    }
    if (route.route_label.empty()) {
      AddDiagnostic(
          &validation,
          route,
          "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.ROUTE_LABEL_MISSING");
    }
    if (!route.driver_visible_route) {
      AddDiagnostic(
          &validation,
          route,
          "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.DRIVER_ROUTE_MISSING");
    }
    if (route.plan_evidence_digest.empty()) {
      AddDiagnostic(
          &validation,
          route,
          "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.PLAN_EVIDENCE_MISSING");
    }
    if (route.explain_digest.empty()) {
      AddDiagnostic(
          &validation,
          route,
          "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.EXPLAIN_MISSING");
    }
    if (route.diagnostic_code.empty()) {
      AddDiagnostic(
          &validation,
          route,
          "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.DIAGNOSTIC_REQUIRED");
    }
    if (route.result_hash.empty()) {
      AddDiagnostic(
          &validation,
          route,
          "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.RESULT_HASH_MISSING");
    }
    if (route.redaction_digest.empty()) {
      AddDiagnostic(
          &validation,
          route,
          "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.REDACTION_MISSING");
    }
    if (HasAuthorityDrift(route)) {
      AddDiagnostic(
          &validation,
          route,
          "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.AUTHORITY_DRIFT");
    }

    if (reference == nullptr) {
      reference = &route;
      continue;
    }

    if (route.route_label != reference->route_label) {
      AddDiagnostic(
          &validation,
          route,
          "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.ROUTE_MISMATCH");
    }
    if (route.plan_evidence_digest != reference->plan_evidence_digest ||
        route.explain_digest != reference->explain_digest) {
      AddDiagnostic(
          &validation,
          route,
          "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.EXPLAIN_MISMATCH");
    }
    if (route.diagnostics != reference->diagnostics ||
        route.diagnostic_code != reference->diagnostic_code) {
      AddDiagnostic(
          &validation,
          route,
          "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.DIAGNOSTIC_MISMATCH");
    }
    if (route.result_hash != reference->result_hash) {
      AddDiagnostic(
          &validation,
          route,
          "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.RESULT_HASH_MISMATCH");
    }
    if (route.redaction_digest != reference->redaction_digest ||
        route.redaction_applied != reference->redaction_applied) {
      AddDiagnostic(
          &validation,
          route,
          "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.REDACTION_MISMATCH");
    }
  }

  for (const auto& required_route : required_routes) {
    if (seen_routes.find(required_route) == seen_routes.end()) {
      validation.diagnostics.push_back(
          required_route +
          ":SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.MISSING_DRIVER_VISIBLE_ROUTE");
    }
  }

  validation.ok = validation.diagnostics.empty();
  validation.diagnostic_code =
      validation.ok ? "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.OK"
                    : "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.FAILED";
  return validation;
}

}  // namespace scratchbird::engine::optimizer
