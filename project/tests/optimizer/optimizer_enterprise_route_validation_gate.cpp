// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_plan_cache.hpp"
#include "optimizer_safety_gates.hpp"
#include "runtime_consumption_evidence.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace planner = scratchbird::engine::planner;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "OEIC enterprise route validation gate failure: "
              << message << '\n';
    return false;
  }
  return true;
}

bool Has(const std::vector<std::string>& values, const std::string& token) {
  return std::any_of(values.begin(), values.end(), [&](const auto& value) {
    return value.find(token) != std::string::npos;
  });
}

opt::RuntimeOptimizedPathEvidence LiveRuntimeEvidence(std::string route) {
  opt::RuntimeOptimizedPathEvidence evidence;
  evidence.selected_path = "enterprise.selected.local_index_path";
  evidence.runtime_consumed = true;
  evidence.consumed_module = "engine.executor.enterprise_route";
  evidence.route_kind = std::move(route);
  evidence.live_execution = true;
  evidence.contract_only = false;
  evidence.transaction_snapshot_class = "engine.mga.snapshot";
  evidence.catalog_epoch = 8101;
  evidence.security_epoch = 8102;
  evidence.redaction_epoch = 8103;
  evidence.provider_generation = 8104;
  evidence.result_contract_hash = "sha256:enterprise-result-contract";
  evidence.diagnostic_code = "SB_ORH_RUNTIME_CONSUMPTION_EVIDENCE.CONSUMED";
  return evidence;
}

bool ProductionGateRejectsUnsafeInputs() {
  // SEARCH_KEY: OEIC_OPTIMIZER_PRODUCTION_BUILD_GATE
  opt::OptimizerProductionBuildGateInput safe;
  const auto safe_result = opt::EvaluateOptimizerProductionBuildGate(safe);

  opt::OptimizerProductionBuildGateInput unsafe;
  unsafe.fixture_statistics_enabled = true;
  unsafe.local_default_statistics_enabled = true;
  unsafe.policy_default_statistics_enabled = true;
  unsafe.donor_produced_evidence_enabled = true;
  unsafe.relaxed_benchmark_clean_paths_enabled = true;
  unsafe.relaxed_metrics_enabled = true;
  unsafe.placeholder_runtime_evidence_enabled = true;
  unsafe.synthetic_agent_recommendations_enabled = true;
  unsafe.synthetic_feedback_enabled = true;
  unsafe.parser_execution_shortcuts_enabled = true;
  unsafe.forced_collision_hooks_enabled = true;
  unsafe.cluster_stub_live_claims_enabled = true;
  unsafe.debug_only_paths_enabled = true;
  const auto unsafe_result = opt::EvaluateOptimizerProductionBuildGate(unsafe);

  return Require(safe_result.ok, "safe production gate input was rejected") &&
         Require(!unsafe_result.ok, "unsafe production gate input was accepted") &&
         Require(Has(unsafe_result.diagnostics,
                     "SB_OPT_PRODUCTION_GATE_FIXTURE_STATS_FORBIDDEN"),
                 "fixture stats diagnostic missing") &&
         Require(Has(unsafe_result.diagnostics,
                     "SB_OPT_PRODUCTION_GATE_PLACEHOLDER_RUNTIME_EVIDENCE_FORBIDDEN"),
                 "placeholder runtime diagnostic missing") &&
         Require(Has(unsafe_result.diagnostics,
                     "SB_OPT_PRODUCTION_GATE_CLUSTER_STUB_LIVE_CLAIMS_FORBIDDEN"),
                 "cluster-stub diagnostic missing") &&
         Require(Has(unsafe_result.diagnostics,
                     "SB_OPT_PRODUCTION_GATE_DEBUG_ONLY_PATHS_FORBIDDEN"),
                 "debug-only diagnostic missing");
}

opt::OptimizerBenchmarkRouteLaneEvidence BenchmarkLane(std::string lane,
                                                       std::string phase,
                                                       std::string donor) {
  opt::OptimizerBenchmarkRouteLaneEvidence evidence;
  evidence.lane_id = std::move(lane);
  evidence.route_label = "enterprise/sql/customer_lookup";
  evidence.cache_phase = std::move(phase);
  evidence.p50_us = 120.0;
  evidence.p95_us = 170.0;
  evidence.p99_us = 220.0;
  evidence.benchmark_clean_claim = true;
  evidence.trusted = true;
  evidence.fresh = true;
  evidence.evidence_generation = "oeic-benchmark-gen-081";
  evidence.donor_comparison_required = true;
  evidence.donor_comparison_id = "donor:" + donor + ":customer_lookup";
  evidence.donor_engine = std::move(donor);
  evidence.donor_oracle_result_hash = "sha256:donor-oracle-result";
  evidence.donor_reference_only = true;
  evidence.diagnostic_code = "SB_OEIC_BENCHMARK_ROUTE_EVIDENCE.OK";
  return evidence;
}

std::vector<opt::BenchmarkMethodologyRunEvidence> ScaleRuns() {
  std::vector<opt::BenchmarkMethodologyRunEvidence> runs;
  const std::vector<std::string> tiers = {"10k", "100k", "1m", "gb"};
  for (const auto& tier : tiers) {
    for (const auto& phase : {"cold", "warm"}) {
      opt::BenchmarkMethodologyRunEvidence run;
      run.run_id = "oeic083:" + tier + ":" + phase;
      run.route_label = "enterprise/sql/customer_lookup";
      run.cache_phase = phase;
      run.scale_tier = tier;
      run.skew_profile = "zipf_1_1_uniform_tenant_mix";
      run.repetition_count = 5;
      run.sample_duration_us = {100.0, 110.0, 120.0, 130.0, 140.0};
      run.p50_us = 120.0;
      run.p95_us = 140.0;
      run.p99_us = 140.0;
      run.optimization_toggles = {"enterprise_optimizer", "property_frontier"};
      run.profiler_source_labels = {"engine_runtime_metrics", "support_bundle"};
      run.latest_scratchbird_baseline_id = "sb-baseline:" + tier;
      run.latest_scratchbird_baseline_p50_us = 160.0;
      run.donor_equivalent_baseline_id = "donor-baseline:postgresql:" + tier;
      run.donor_equivalent_engine = "postgresql";
      run.donor_equivalent_baseline_p50_us = 150.0;
      run.methodology_only = false;
      run.performance_proof = true;
      run.benchmark_clean_claim = true;
      run.diagnostic_code = "SB_OEIC_SCALE_BENCHMARK.RUN_OK";
      runs.push_back(run);
    }
  }
  return runs;
}

std::vector<std::string> DonorEngines() {
  return {"scratchbird", "firebird", "mysql", "postgresql", "sqlite",
          "mariadb", "oracle", "sqlserver", "db2", "sybase", "informix",
          "teradata", "snowflake", "bigquery", "redshift", "clickhouse",
          "duckdb", "mongodb", "cassandra", "couchbase", "redis", "neo4j",
          "elasticsearch", "solr", "cockroachdb"};
}

std::vector<opt::BenchmarkMethodEvidence> DonorMethodEvidence() {
  std::vector<opt::BenchmarkMethodEvidence> methods;
  for (const auto& engine : DonorEngines()) {
    opt::BenchmarkMethodEvidence method;
    method.engine = engine;
    method.logical_task = "customer_lookup";
    method.workload_family = "oltp_point_lookup";
    method.method = engine == "scratchbird" ? "scratchbird_best_route"
                                             : "donor_native_best_route";
    method.best_normal_method = true;
    method.native_bulk_or_best_engine_path = true;
    method.prepared_or_warmed = true;
    method.output_suppressed = true;
    method.result_materialization_policy = "binary_frame_equivalent";
    method.transaction_policy = "engine_mga_or_donor_native_reference_only";
    method.data_generator_id = "oeic083-generator-v1";
    method.scale_profile = "10k_100k_1m_gb";
    method.skew_profile = "zipf_1_1_uniform_tenant_mix";
    method.resource_budget_profile = "enterprise-standard";
    method.constraint_policy = "same_constraints_or_exact_noncomparable_reason";
    method.donor_reference_only = engine != "scratchbird";
    method.uses_donor_storage_or_finality_for_scratchbird = false;
    method.diagnostic_code = "SB_OEIC_DONOR_METHOD.OK";
    methods.push_back(method);
  }
  return methods;
}

bool BenchmarkCleanAndScaleProofPass() {
  // SEARCH_KEY: OEIC_BENCHMARK_CLEAN_OPTIMIZER_ROUTE
  const auto lane_validation = opt::ValidateOptimizerBenchmarkRouteEvidence(
      {BenchmarkLane("cold", "cold", "postgresql"),
       BenchmarkLane("warm", "warm", "postgresql")},
      true);
  if (!Require(lane_validation.ok && lane_validation.benchmark_clean,
               "benchmark-clean cold/warm lane evidence was rejected")) {
    return false;
  }

  auto placeholder = LiveRuntimeEvidence("embedded");
  placeholder.catalog_epoch = 1;
  placeholder.security_epoch = 1;
  placeholder.redaction_epoch = 1;
  placeholder.provider_generation = 1;
  placeholder.result_contract_hash = "result-contract-v1";
  const opt::RouteCompletionClaim claim{
      .route_kind = "embedded",
      .benchmark_clean = true,
      .live_route = true,
      .mark_complete = true,
  };
  const auto placeholder_result =
      opt::EvaluateRouteCompletionClaim(claim, {placeholder});
  const auto real_result =
      opt::EvaluateRouteCompletionClaim(claim, {LiveRuntimeEvidence("embedded")});
  if (!Require(!placeholder_result.can_mark_complete,
               "placeholder runtime evidence closed benchmark-clean route") ||
      !Require(real_result.can_mark_complete,
               "real runtime route evidence could not close benchmark-clean route")) {
    return false;
  }

  // SEARCH_KEY: OEIC_SCALE_DONOR_COMPARISON_SUITE
  const auto methodology =
      opt::ValidateBenchmarkMethodologyEvidence(ScaleRuns());
  if (!Require(methodology.ok && methodology.benchmark_clean,
               "scale methodology evidence was rejected")) {
    return false;
  }
  const auto donor =
      opt::ValidateBestMethodBenchmarkEquivalence(DonorMethodEvidence(),
                                                  DonorEngines());
  return Require(donor.ok, "24 donor method equivalence evidence was rejected");
}

opt::CrossRouteResultEvidence Route(std::string route) {
  opt::CrossRouteResultEvidence evidence;
  evidence.route_kind = std::move(route);
  evidence.route_label = "enterprise/customer_lookup";
  evidence.live_route_executed = true;
  evidence.benchmark_clean_claim = true;
  evidence.database_parameters_hash = "sha256:db-params";
  evidence.session_rights_digest = "sha256:session-rights";
  evidence.security_epoch = 8201;
  evidence.redaction_epoch = 8202;
  evidence.transaction_snapshot_id = "mga-snapshot:8203";
  evidence.local_transaction_id = 8204;
  evidence.result_contract_hash = "sha256:result-contract";
  evidence.required_ordering = "customer_id.asc";
  evidence.accepted = true;
  evidence.rows = {"1|alice", "2|bea"};
  evidence.diagnostic_code = "SB_OEIC_LIVE_ROUTE.OK";
  return evidence;
}

bool LiveRouteEquivalencePasses() {
  // SEARCH_KEY: OEIC_LIVE_PARSER_SBLR_OPTIMIZER_EXECUTOR_ROUTE
  const std::vector<std::string> routes = {"embedded", "ipc", "inet", "cli", "driver"};
  const auto validation = opt::ValidateCrossRouteResultEquivalence(
      {Route("embedded"), Route("ipc"), Route("inet"), Route("cli"), Route("driver")},
      routes);
  if (!Require(validation.ok && validation.benchmark_clean,
               "live cross-route equivalence was rejected")) {
    return false;
  }
  auto drift = Route("ipc");
  drift.parser_owns_transaction_finality = true;
  const auto drift_validation =
      opt::ValidateCrossRouteResultEquivalence({Route("embedded"), drift}, {"embedded", "ipc"});
  return Require(!drift_validation.ok,
                 "parser transaction authority drift was accepted");
}

opt::OptimizerPlanCacheKeyInput CacheInput() {
  opt::OptimizerPlanCacheKeyInput input;
  input.operation_id = "oeic084.select.customer";
  input.sblr_digest = "sha256:sblr-oeic084";
  input.descriptor_set_digest = "sha256:descriptor-oeic084";
  input.statistics_snapshot_id = "stats-snapshot:oeic084";
  input.catalog_stats_digest = "sha256:catalog-stats-oeic084";
  input.cost_profile_id = "cost-profile:enterprise";
  input.executor_capability_set_id = "executor-capability:local";
  input.route_capability_digest = "sha256:route-capability-local";
  input.security_policy_digest = "sha256:security-policy";
  input.redaction_route_digest = "sha256:redaction-route";
  input.normalized_optimizer_controls_digest = "sha256:optimizer-controls";
  input.parameter_shape_digest = "sha256:bind-profile";
  input.memory_grant_class = "memory-grant-class:governed";
  input.memory_grant_digest = "sha256:memory-grant";
  input.catalog_epoch = 8401;
  input.stats_epoch = 8402;
  input.security_epoch = 8403;
  input.redaction_epoch = 8404;
  input.policy_epoch = 8405;
  input.resource_epoch = 8406;
  input.name_resolution_epoch = 8407;
  input.memory_policy_epoch = 8408;
  input.memory_feedback_generation = 8409;
  input.compatibility_epoch = 8410;
  input.format_compatibility_epoch = 8411;
  input.route_epoch = 8412;
  input.object_uuids = {"rel.customer.084"};
  input.function_uuids = {"fn.mask.084"};
  input.index_uuids = {"idx.customer.084"};
  input.filespace_uuids = {"filespace.hot.084"};
  input.dependency_digests = {
      "sha256:dep-rel-084", "sha256:dep-idx-084",
      "sha256:dep-stats-084", "sha256:dep-route-084",
      "sha256:dep-memory-084"};
  return input;
}

opt::CachedOptimizerPlan CachePlan(const opt::OptimizerPlanCacheKeyInput& input) {
  opt::CachedOptimizerPlan plan;
  plan.key_input = input;
  plan.cache_key = opt::BuildOptimizerPlanCacheKey(input);
  plan.created_epoch = input.catalog_epoch;
  plan.result.ok = true;
  plan.result.diagnostic_code = "SB_OPT_OK";
  plan.result.plan_id = "oeic084.enterprise_plan";
  opt::PlanCandidate candidate;
  candidate.candidate_id = "btree.customer.084";
  candidate.access_kind = planner::PhysicalAccessKind::kScalarBtreeLookup;
  candidate.required_facts = {"rel.customer.084", "idx.customer.084"};
  plan.result.candidates.push_back(candidate);
  plan.metadata_only = true;
  plan.mga_visibility_recheck_required = true;
  plan.security_recheck_required = true;
  return plan;
}

opt::OptimizerPlanCachePersistenceRequest PersistenceRequest() {
  opt::OptimizerPlanCachePersistenceRequest request;
  request.storage_scope_uuid = "catalog.plan_cache.scope.084";
  request.persisted_by_principal_uuid = "principal.optimizer.084";
  request.persisted_epoch = 8420;
  request.catalog_epoch = 8401;
  request.stats_epoch = 8402;
  request.security_epoch = 8403;
  request.redaction_epoch = 8404;
  request.policy_epoch = 8405;
  request.resource_epoch = 8406;
  request.route_epoch = 8412;
  request.memory_policy_epoch = 8408;
  request.memory_feedback_generation = 8409;
  return request;
}

bool CrashRestartPersistencePasses() {
  // SEARCH_KEY: OEIC_OPTIMIZER_CRASH_RESTART_FEEDBACK
  opt::OptimizerPlanCache cache;
  const auto input = CacheInput();
  if (!Require(cache.PutEnterprise(CachePlan(input)).ok,
               "plan cache put failed before crash simulation")) {
    return false;
  }
  const auto envelope = cache.ExportPersistenceEnvelope(PersistenceRequest());
  if (!Require(envelope.ok, "plan cache persistence envelope was rejected")) {
    return false;
  }
  opt::OptimizerPlanCache recovered;
  if (!Require(recovered.ImportPersistenceEnvelope(envelope).ok,
               "plan cache recovery import failed")) {
    return false;
  }
  if (!Require(recovered.LookupEnterprise(input).hit,
               "recovered plan cache missed committed fact")) {
    return false;
  }
  auto partial = envelope;
  partial.request.mga_transaction_committed = false;
  partial.envelope_digest = opt::BuildOptimizerPlanCachePersistenceDigest(partial);
  return Require(!recovered.ImportPersistenceEnvelope(partial).ok,
                 "partial non-MGA plan-cache fact was accepted after crash");
}

bool SecurityNegativePasses() {
  // SEARCH_KEY: OEIC_OPTIMIZER_SECURITY_NEGATIVE_SUITE
  opt::DriverVisibleExplainRouteEvidence explain;
  explain.route_kind = "driver";
  explain.route_label = "enterprise/customer_lookup";
  explain.driver_visible_route = true;
  explain.plan_evidence_digest = "sha256:plan";
  explain.explain_digest = "sha256:explain";
  explain.diagnostics = {"SB_OPT_OK"};
  explain.result_hash = "sha256:result";
  explain.redaction_digest = "sha256:redaction";
  explain.redaction_applied = true;
  explain.diagnostic_code = "SB_OPT_OK";
  const auto ok =
      opt::ValidateDriverVisibleExplainRouteEquivalence({explain}, {"driver"});
  if (!Require(ok.ok, "safe driver-visible explain was rejected")) {
    return false;
  }
  explain.security_authority = true;
  const auto drift =
      opt::ValidateDriverVisibleExplainRouteEquivalence({explain}, {"driver"});
  return Require(!drift.ok, "driver/security authority drift was accepted") &&
         Require(Has(drift.diagnostics,
                     "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.AUTHORITY_DRIFT"),
                 "security authority drift diagnostic missing");
}

}  // namespace

int main() {
  if (!ProductionGateRejectsUnsafeInputs()) return EXIT_FAILURE;
  if (!BenchmarkCleanAndScaleProofPass()) return EXIT_FAILURE;
  if (!LiveRouteEquivalencePasses()) return EXIT_FAILURE;
  if (!CrashRestartPersistencePasses()) return EXIT_FAILURE;
  if (!SecurityNegativePasses()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
