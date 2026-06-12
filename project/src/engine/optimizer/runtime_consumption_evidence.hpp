// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: ORH_RUNTIME_CONSUMPTION_EVIDENCE
struct RuntimeOptimizedPathEvidence {
  std::string selected_path;
  bool runtime_consumed = false;
  std::string consumed_module;
  std::string route_kind;
  bool live_execution = false;
  bool contract_only = false;
  std::string transaction_snapshot_class;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::uint64_t provider_generation = 0;
  std::string result_contract_hash;
  std::string fallback_reason;
  std::string diagnostic_code;
};

enum class RuntimeConsumptionState {
  kInvalid,
  kSelectionOnly,
  kRuntimeConsumed,
  kContractOnlyBlocker,
};

struct RuntimeConsumptionValidation {
  bool ok = false;
  RuntimeConsumptionState state = RuntimeConsumptionState::kInvalid;
  std::string diagnostic_code;
  std::vector<std::string> missing_fields;
  std::vector<std::string> diagnostics;
};

struct RouteCompletionClaim {
  std::string route_kind;
  bool benchmark_clean = false;
  bool live_route = false;
  bool mark_complete = false;
};

struct RouteClaimGuardResult {
  bool can_mark_complete = false;
  bool exact_blocker = false;
  std::string diagnostic_code;
  std::vector<std::string> diagnostics;
};

// SEARCH_KEY: ORH_REFERENCE_DOMINANCE_TARGET_CONTRACT
struct ReferenceDominanceTargetEvidence {
  std::string workload;
  std::string category;
  bool comparable = false;
  std::string comparable_status;
  std::string reference_best_engine;
  double reference_best_duration_ms = 0.0;
  double scratchbird_current_duration_ms = 0.0;
  bool prior_scratchbird_duration_available = false;
  double scratchbird_prior_duration_ms = 0.0;
  double dominance_target_duration_ms = 0.0;
  std::string dominance_target_rule;
  std::string exact_blocker_rule;
  std::string diagnostic_code;
};

struct ReferenceDominanceTargetValidation {
  bool ok = false;
  std::string diagnostic_code;
  std::vector<std::string> missing_fields;
  std::vector<std::string> diagnostics;
};

struct ReferenceDominanceTargetSetValidation {
  bool ok = false;
  std::string diagnostic_code;
  std::vector<std::string> diagnostics;
};

// SEARCH_KEY: ORH_BEST_METHOD_BENCHMARK_EQUIVALENCE
struct BenchmarkMethodEvidence {
  std::string engine;
  std::string logical_task;
  std::string workload_family;
  std::string method;
  bool best_normal_method = false;
  bool native_bulk_or_best_engine_path = false;
  bool prepared_or_warmed = false;
  bool output_suppressed = false;
  std::string result_materialization_policy;
  std::string transaction_policy;
  std::string data_generator_id;
  std::string scale_profile;
  std::string skew_profile;
  std::string resource_budget_profile;
  std::string constraint_policy;
  bool reference_reference_only = true;
  bool uses_reference_storage_or_finality_for_scratchbird = false;
  std::string diagnostic_code;
};

struct BenchmarkEquivalenceValidation {
  bool ok = false;
  std::string diagnostic_code;
  std::vector<std::string> diagnostics;
};

// SEARCH_KEY: ORH_BENCHMARK_METHODOLOGY_NOISE_CONTROL
struct BenchmarkMethodologyRunEvidence {
  std::string run_id;
  std::string route_label;
  std::string cache_phase;
  std::string scale_tier;
  std::string skew_profile;
  std::uint64_t repetition_count = 0;
  std::vector<double> sample_duration_us;
  double p50_us = 0.0;
  double p95_us = 0.0;
  double p99_us = 0.0;
  std::vector<std::string> optimization_toggles;
  std::vector<std::string> profiler_source_labels;
  std::string latest_scratchbird_baseline_id;
  double latest_scratchbird_baseline_p50_us = 0.0;
  std::string reference_equivalent_baseline_id;
  std::string reference_equivalent_engine;
  double reference_equivalent_baseline_p50_us = 0.0;
  bool methodology_only = false;
  bool performance_proof = false;
  bool benchmark_clean_claim = false;
  std::string diagnostic_code;
};

struct BenchmarkMethodologyValidation {
  bool ok = false;
  bool benchmark_clean = false;
  std::string diagnostic_code;
  std::vector<std::string> diagnostics;
};

// SEARCH_KEY: ORH_CROSS_ROUTE_RESULT_EQUIVALENCE
struct CrossRouteResultEvidence {
  std::string route_kind;
  std::string route_label;
  bool live_route_executed = false;
  bool unsupported_route = false;
  bool benchmark_clean_claim = false;
  std::string database_parameters_hash;
  std::string session_rights_digest;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::string transaction_snapshot_id;
  std::uint64_t local_transaction_id = 0;
  std::string result_contract_hash;
  std::string required_ordering;
  bool accepted = true;
  std::vector<std::string> rows;
  std::vector<std::string> diagnostics;
  std::string failure_diagnostic_code;
  bool parser_owns_visibility_authority = false;
  bool parser_owns_transaction_finality = false;
  bool client_or_driver_owns_visibility_authority = false;
  bool client_or_driver_owns_transaction_finality = false;
  std::string diagnostic_code;
};

struct CrossRouteEquivalenceValidation {
  bool ok = false;
  bool benchmark_clean = false;
  bool exact_blocker = false;
  std::string diagnostic_code;
  std::vector<std::string> diagnostics;
};

// SEARCH_KEY: ORH_FIXED_ROUTE_OVERHEAD_REMOVAL
struct FixedRouteOverheadEvidence {
  std::string route_kind;
  std::string statement_family;
  std::string selected_path;
  bool benchmark_clean_candidate = false;
  bool warmed_prepared_route = false;
  bool prepared_template_reused = false;
  bool lowered_sblr_reused = false;
  bool descriptor_reused = false;
  bool result_shape_reused = false;
  bool text_rendering_suppressed = false;
  std::uint64_t repeated_parse_count = 0;
  std::uint64_t repeated_lower_count = 0;
  std::uint64_t repeated_descriptor_build_count = 0;
  std::uint64_t repeated_result_shape_build_count = 0;
  std::uint64_t repeated_text_render_count = 0;
  std::uint64_t route_latency_budget_us = 0;
  std::uint64_t route_latency_observed_us = 0;
  bool index_dependent = false;
  bool index_correctness_proven = false;
  bool parser_or_cache_executes_sql = false;
  bool parser_or_cache_owns_transaction_finality = false;
  std::string transaction_authority;
  RuntimeOptimizedPathEvidence runtime_evidence;
  std::string fallback_reason;
  std::string diagnostic_code;
};

struct FixedRouteOverheadValidation {
  bool ok = false;
  bool benchmark_clean = false;
  bool exact_fallback = false;
  std::string diagnostic_code;
  std::vector<std::string> missing_fields;
  std::vector<std::string> diagnostics;
};

// SEARCH_KEY: ORH_BINARY_BENCHMARK_RESULT_FAST_PATH
struct BenchmarkResultFastPathEvidence {
  std::string route_kind;
  std::string statement_family;
  bool benchmark_clean_candidate = false;
  bool binary_or_equivalent_frame_selected = false;
  std::string frame_kind;
  std::string frame_version;
  bool equivalent_result_materialization = false;
  bool exact_diagnostics_preserved = false;
  bool nonessential_evidence_suppressed_during_timing = false;
  bool support_evidence_available_outside_timed_path = false;
  bool timed_path_text_rendering_suppressed = false;
  bool disabled_or_fallback = false;
  std::string disabled_reason;
  bool parser_or_cache_executes_sql = false;
  bool parser_or_cache_owns_transaction_finality = false;
  std::string transaction_authority;
  RuntimeOptimizedPathEvidence runtime_evidence;
  std::string result_contract_hash;
  std::string diagnostic_code;
};

struct BenchmarkResultFastPathValidation {
  bool ok = false;
  bool benchmark_clean = false;
  bool exact_fallback = false;
  std::string diagnostic_code;
  std::vector<std::string> missing_fields;
  std::vector<std::string> diagnostics;
};

// SEARCH_KEY: OPCH_BENCHMARK_ROUTE_EVIDENCE
// Benchmark lanes are proof material only. Reference comparison/oracle data may be
// recorded for comparable workloads, but reference output cannot become optimizer,
// transaction finality, visibility, security, authorization, or recovery
// authority.
struct OptimizerBenchmarkRouteLaneEvidence {
  std::string lane_id;
  std::string route_label;
  std::string cache_phase;
  double p50_us = 0.0;
  double p95_us = 0.0;
  double p99_us = 0.0;
  bool benchmark_clean_claim = false;
  bool trusted = false;
  bool fresh = false;
  std::string evidence_generation;
  bool reference_comparison_required = false;
  std::string reference_comparison_id;
  std::string reference_engine;
  std::string reference_oracle_result_hash;
  bool reference_reference_only = true;
  bool reference_as_authority = false;
  bool benchmark_evidence_authority = false;
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool security_authority = false;
  bool recovery_authority = false;
  std::string diagnostic_code;
};

struct OptimizerBenchmarkRouteEvidenceValidation {
  bool ok = false;
  bool benchmark_clean = false;
  std::string diagnostic_code;
  std::vector<std::string> diagnostics;
};

// SEARCH_KEY: OPCH_DRIVER_VISIBLE_ROUTE_EXPLAIN_EQUIVALENCE
// Driver-visible route explain evidence must be equivalent across embedded,
// IPC, INET, CLI, and external-driver routes while preserving redaction.
struct DriverVisibleExplainRouteEvidence {
  std::string route_kind;
  std::string route_label;
  bool driver_visible_route = false;
  std::string plan_evidence_digest;
  std::string explain_digest;
  std::vector<std::string> diagnostics;
  std::string result_hash;
  std::string redaction_digest;
  bool redaction_applied = false;
  bool driver_or_benchmark_authority = false;
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool security_authority = false;
  bool recovery_authority = false;
  std::string diagnostic_code;
};

struct DriverVisibleExplainRouteValidation {
  bool ok = false;
  std::string diagnostic_code;
  std::vector<std::string> diagnostics;
};

RuntimeConsumptionValidation ValidateRuntimeOptimizedPathEvidence(
    const RuntimeOptimizedPathEvidence& evidence);
RuntimeConsumptionState ClassifyRuntimeOptimizedPathEvidence(
    const RuntimeOptimizedPathEvidence& evidence);
RuntimeOptimizedPathEvidence MakeSelectionOnlyRuntimeEvidence(
    std::string selected_path,
    std::string route_kind,
    std::string diagnostic_code,
    std::string fallback_reason);
RuntimeOptimizedPathEvidence MarkRuntimeEvidenceConsumed(
    RuntimeOptimizedPathEvidence evidence,
    std::string consumed_module);

// SEARCH_KEY: ORH_CONTRACT_ONLY_LIVE_ROUTE_GUARD
RouteClaimGuardResult EvaluateRouteCompletionClaim(
    const RouteCompletionClaim& claim,
    const std::vector<RuntimeOptimizedPathEvidence>& evidence);

ReferenceDominanceTargetValidation ValidateReferenceDominanceTargetEvidence(
    const ReferenceDominanceTargetEvidence& evidence);
ReferenceDominanceTargetSetValidation ValidateReferenceDominanceTargetSet(
    const std::vector<ReferenceDominanceTargetEvidence>& evidence,
    const std::vector<std::string>& required_workloads);
BenchmarkEquivalenceValidation ValidateBestMethodBenchmarkEquivalence(
    const std::vector<BenchmarkMethodEvidence>& methods,
    const std::vector<std::string>& required_engines);
BenchmarkMethodologyValidation ValidateBenchmarkMethodologyEvidence(
    const std::vector<BenchmarkMethodologyRunEvidence>& runs);
CrossRouteEquivalenceValidation ValidateCrossRouteResultEquivalence(
    const std::vector<CrossRouteResultEvidence>& routes,
    const std::vector<std::string>& required_routes);
FixedRouteOverheadValidation ValidateFixedRouteOverheadEvidence(
    const FixedRouteOverheadEvidence& evidence);
BenchmarkResultFastPathValidation ValidateBenchmarkResultFastPathEvidence(
    const BenchmarkResultFastPathEvidence& evidence);
OptimizerBenchmarkRouteEvidenceValidation ValidateOptimizerBenchmarkRouteEvidence(
    const std::vector<OptimizerBenchmarkRouteLaneEvidence>& lanes,
    bool reference_comparison_required);
DriverVisibleExplainRouteValidation ValidateDriverVisibleExplainRouteEquivalence(
    const std::vector<DriverVisibleExplainRouteEvidence>& routes,
    const std::vector<std::string>& required_routes);

}  // namespace scratchbird::engine::optimizer
