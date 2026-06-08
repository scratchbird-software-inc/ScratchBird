// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-056 focused validation for workload-specific optimizer regression
// budgets.
#include "optimizer_workload_regression_budget.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace opt = scratchbird::engine::optimizer;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

bool HasDiagnostic(const std::vector<std::string>& diagnostics,
                   std::string_view token) {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.find(token) != std::string::npos) return true;
  }
  return false;
}

bool NeedsExactFallback(opt::OptimizerWorkloadRegressionClass value) {
  return value == opt::OptimizerWorkloadRegressionClass::kVectorApproximate ||
         value == opt::OptimizerWorkloadRegressionClass::kTextWand ||
         value == opt::OptimizerWorkloadRegressionClass::kMixedSqlNosqlFusion;
}

bool NeedsExactRerank(opt::OptimizerWorkloadRegressionClass value) {
  return value == opt::OptimizerWorkloadRegressionClass::kVectorExact ||
         value == opt::OptimizerWorkloadRegressionClass::kVectorApproximate ||
         value == opt::OptimizerWorkloadRegressionClass::kTextWand ||
         value == opt::OptimizerWorkloadRegressionClass::kMixedSqlNosqlFusion;
}

opt::OptimizerCorrectnessClass CorrectnessClass(
    opt::OptimizerWorkloadRegressionClass value) {
  switch (value) {
    case opt::OptimizerWorkloadRegressionClass::kOltpPointLookup:
      return opt::OptimizerCorrectnessClass::kTopN;
    case opt::OptimizerWorkloadRegressionClass::kOltpRangeLookup:
      return opt::OptimizerCorrectnessClass::kDistinct;
    case opt::OptimizerWorkloadRegressionClass::kOlapScanAggregate:
      return opt::OptimizerCorrectnessClass::kAggregation;
    case opt::OptimizerWorkloadRegressionClass::kJoin:
      return opt::OptimizerCorrectnessClass::kInnerJoin;
    case opt::OptimizerWorkloadRegressionClass::kDmlLocatorMutation:
      return opt::OptimizerCorrectnessClass::kDmlLocator;
    case opt::OptimizerWorkloadRegressionClass::kDocumentPath:
      return opt::OptimizerCorrectnessClass::kDocumentPath;
    case opt::OptimizerWorkloadRegressionClass::kVectorExact:
    case opt::OptimizerWorkloadRegressionClass::kVectorApproximate:
      return opt::OptimizerCorrectnessClass::kVector;
    case opt::OptimizerWorkloadRegressionClass::kTextWand:
      return opt::OptimizerCorrectnessClass::kTextSearch;
    case opt::OptimizerWorkloadRegressionClass::kGraphTraversal:
      return opt::OptimizerCorrectnessClass::kGraph;
    case opt::OptimizerWorkloadRegressionClass::kMixedSqlNosqlFusion:
      return opt::OptimizerCorrectnessClass::kMixedFusion;
    case opt::OptimizerWorkloadRegressionClass::kBulkIngest:
      return opt::OptimizerCorrectnessClass::kAggregation;
  }
  return opt::OptimizerCorrectnessClass::kInnerJoin;
}

opt::OptimizerExpectedAccessClass AccessClass(
    opt::OptimizerWorkloadRegressionClass value) {
  switch (value) {
    case opt::OptimizerWorkloadRegressionClass::kOltpPointLookup:
      return opt::OptimizerExpectedAccessClass::kScalarBtreeLookup;
    case opt::OptimizerWorkloadRegressionClass::kOltpRangeLookup:
      return opt::OptimizerExpectedAccessClass::kScalarBtreeRange;
    case opt::OptimizerWorkloadRegressionClass::kOlapScanAggregate:
      return opt::OptimizerExpectedAccessClass::kColumnarZoneScan;
    case opt::OptimizerWorkloadRegressionClass::kJoin:
      return opt::OptimizerExpectedAccessClass::kJoinPlan;
    case opt::OptimizerWorkloadRegressionClass::kDmlLocatorMutation:
      return opt::OptimizerExpectedAccessClass::kDmlRowLocator;
    case opt::OptimizerWorkloadRegressionClass::kDocumentPath:
      return opt::OptimizerExpectedAccessClass::kDocumentPathProbe;
    case opt::OptimizerWorkloadRegressionClass::kVectorExact:
      return opt::OptimizerExpectedAccessClass::kVectorExactSearch;
    case opt::OptimizerWorkloadRegressionClass::kVectorApproximate:
      return opt::OptimizerExpectedAccessClass::kVectorApproximateCandidates;
    case opt::OptimizerWorkloadRegressionClass::kTextWand:
      return opt::OptimizerExpectedAccessClass::kFullTextWandProbe;
    case opt::OptimizerWorkloadRegressionClass::kGraphTraversal:
      return opt::OptimizerExpectedAccessClass::kGraphSeedExpansion;
    case opt::OptimizerWorkloadRegressionClass::kMixedSqlNosqlFusion:
      return opt::OptimizerExpectedAccessClass::kSqlNosqlFusion;
    case opt::OptimizerWorkloadRegressionClass::kBulkIngest:
      return opt::OptimizerExpectedAccessClass::kBulkIngestAppend;
  }
  return opt::OptimizerExpectedAccessClass::kRowUuidLookup;
}

opt::OptimizerBenchmarkSampleGroupEvidence Sample(std::string phase) {
  opt::OptimizerBenchmarkSampleGroupEvidence sample;
  sample.sample_group_id = "ceic056:" + phase;
  sample.cache_phase = phase;
  sample.latency_us_samples = {100.0, 110.0, 120.0, 130.0, 140.0};
  sample.p50_us = 120.0;
  sample.p95_us = 140.0;
  sample.p99_us = 140.0;
  sample.cpu_user_us = 2000;
  sample.cpu_system_us = 200;
  sample.memory_peak_bytes = 16 * 1024 * 1024;
  sample.memory_reserved_bytes = 20 * 1024 * 1024;
  sample.memory_released_bytes = 20 * 1024 * 1024;
  sample.io_read_bytes = 8192;
  sample.io_write_bytes = 4096;
  sample.io_read_ops = 2;
  sample.io_write_ops = 1;
  sample.cache_hits = 256;
  sample.cache_misses = 4;
  sample.page_cache_hits = 192;
  sample.page_cache_misses = 3;
  sample.estimated_rows = 1000.0;
  sample.actual_rows = 1020.0;
  sample.estimate_actual_ratio = 1.02;
  sample.skew_profile = "ceic056_skew_control";
  sample.cardinality_profile = "ceic056_histogram_mcv_hll_extended";
  sample.metric_snapshot_digest = "sha256:ceic056-metric-" + phase;
  sample.profiler_digest = "sha256:ceic056-profiler-" + phase;
  sample.cold_cache_reset_proven = phase == "cold";
  sample.warm_cache_prepared_proven = phase == "warm";
  return sample;
}

opt::OptimizerBenchmarkDonorMethodologyEvidence Donor() {
  opt::OptimizerBenchmarkDonorMethodologyEvidence donor;
  donor.donor_engine = "postgresql";
  donor.donor_version = "ceic056-reference-version";
  donor.donor_native_method = "donor_native_best_method_reference_only";
  donor.comparable_status = "comparable";
  donor.dataset_schema_mapping_digest = "sha256:ceic056-donor-dataset-map";
  donor.workload_mapping_digest = "sha256:ceic056-donor-workload-map";
  donor.route_equivalence_contract_hash = "sha256:ceic056-donor-route-contract";
  donor.donor_result_hash = "sha256:ceic056-donor-result";
  donor.donor_transaction_policy =
      "donor_reference_only_scratchbird_mga_not_substituted";
  donor.donor_timing_policy = "prepared_best_method_output_suppressed";
  donor.donor_reference_only = true;
  return donor;
}

opt::OptimizerResultOracleEvidence ResultSide(std::string side,
                                              const std::string& cls,
                                              bool exact_rerank,
                                              bool dml_locator) {
  opt::OptimizerResultOracleEvidence result;
  result.producer_label = std::move(side);
  result.result_contract_hash = "sha256:ceic056-result-contract-" + cls;
  result.result_hash = "sha256:ceic056-result-" + cls;
  result.result_row_count = 56 + cls.size();
  result.result_row_count_observed = true;
  result.ordering_contract_hash = "sha256:ceic056-ordering-" + cls;
  result.null_semantics_hash = "sha256:ceic056-null-semantics-" + cls;
  result.error_diagnostic_code = "SB_OK";
  result.diagnostic_digest = "sha256:ceic056-diagnostic-" + cls;
  result.recheck_evidence_digest = "sha256:ceic056-recheck-" + cls;
  result.row_locator_contract_hash =
      dml_locator ? "sha256:ceic056-row-locator-" + cls : "";
  result.accepted = true;
  result.live_route_executed = true;
  result.synthetic_result = false;
  result.exact_recheck_proven = true;
  result.exact_rerank_proven = exact_rerank;
  result.mga_recheck_proven = true;
  result.security_recheck_proven = true;
  result.row_locator_mga_snapshot_proven = dml_locator;
  return result;
}

opt::OptimizerWorkloadRegressionBudgetRecord Budget(
    opt::OptimizerWorkloadRegressionClass workload_class,
    std::string route) {
  const std::string cls =
      opt::OptimizerWorkloadRegressionClassName(workload_class);
  const bool exact_fallback = NeedsExactFallback(workload_class);
  const bool exact_rerank = NeedsExactRerank(workload_class);
  const bool dml_locator =
      workload_class == opt::OptimizerWorkloadRegressionClass::kDmlLocatorMutation;

  opt::OptimizerWorkloadRegressionBudgetRecord record;
  record.budget_id = "ceic056-budget-" + cls;
  record.workload_class = workload_class;
  record.route_kind = std::move(route);
  record.route_lane = "enterprise/ceic056/" + cls + "/" + record.route_kind;
  record.route_label = "ceic056/workload_budget/" + cls;
  record.cold_or_warm_lane = "warm";
  record.expected_access_class = AccessClass(workload_class);
  record.result_contract_hash = "sha256:ceic056-result-contract-" + cls;
  record.result_hash = "sha256:ceic056-result-" + cls;
  record.logical_plan_hash = "sha256:ceic056-logical-plan-" + cls;
  record.physical_plan_hash = "sha256:ceic056-physical-plan-" + cls;
  record.sblr_digest = "sha256:ceic056-sblr-" + cls;
  record.dataset_schema_digest = "sha256:ceic056-dataset-" + cls;
  record.optimizer_profile = "enterprise";
  record.workload_budget_profile = "ceic056-workload-regression";
  record.baseline_p95_us = 100.0;
  record.optimized_p95_us = 105.0;
  record.baseline_p99_us = 130.0;
  record.optimized_p99_us = 136.5;
  record.max_regression_ratio = 1.20;
  record.observed_regression_ratio = 1.05;
  record.spill_bound_bytes_present = true;
  record.max_spill_bytes = 16 * 1024 * 1024;
  record.observed_spill_bytes = 4 * 1024 * 1024;
  record.memory_reservation_bytes = 32 * 1024 * 1024;
  record.memory_reservation_digest = "sha256:ceic056-memory-reservation-" + cls;
  record.metric_snapshot_digest = "sha256:ceic056-metrics-" + cls;
  record.statistics_snapshot_digest = "sha256:ceic056-statistics-" + cls;
  record.provenance_digest = "sha256:ceic056-provenance-" + cls;
  record.redaction_digest = "sha256:ceic056-redaction-" + cls;
  record.metric_snapshot_generation = 5601;
  record.statistics_epoch = 5602;
  record.feedback_generation = 5603;
  record.memory_feedback_generation = 5604;
  record.provider_generation = 5605;
  record.metric_freshness_microseconds = 1000;
  record.max_metric_freshness_microseconds = 60000000;
  record.statistics_freshness_microseconds = 2000;
  record.max_statistics_freshness_microseconds = 60000000;
  record.metrics_trusted = true;
  record.metrics_fresh = true;
  record.statistics_trusted = true;
  record.statistics_fresh = true;
  record.trusted_provenance = true;
  record.redaction_applied = true;
  record.production_regression_budget_claim = true;
  record.benchmark_clean_claim = true;
  record.exact_fallback_required = exact_fallback;
  record.exact_fallback_proven = exact_fallback;
  record.exact_recheck_required = true;
  record.exact_recheck_proven = true;
  record.exact_rerank_required = exact_rerank;
  record.exact_rerank_proven = exact_rerank;
  record.mga_recheck_required = true;
  record.mga_recheck_proven = true;
  record.security_recheck_required = true;
  record.security_recheck_proven = true;

  record.benchmark_evidence_attached = true;
  record.benchmark_evidence.artifact_uuid = "ceic056-artifact-" + cls;
  record.benchmark_evidence.route_kind = record.route_kind;
  record.benchmark_evidence.route_lane = record.route_lane;
  record.benchmark_evidence.route_label = record.route_label;
  record.benchmark_evidence.dataset_schema_uuid = "dataset-schema-uuid-" + cls;
  record.benchmark_evidence.dataset_schema_digest = record.dataset_schema_digest;
  record.benchmark_evidence.dataset_schema_version = "ceic056-dataset-v1";
  record.benchmark_evidence.sblr_digest = record.sblr_digest;
  record.benchmark_evidence.logical_plan_hash = record.logical_plan_hash;
  record.benchmark_evidence.physical_plan_hash = record.physical_plan_hash;
  record.benchmark_evidence.plan_cache_key_hash =
      "sha256:ceic056-plan-cache-" + cls;
  record.benchmark_evidence.result_contract_hash = record.result_contract_hash;
  record.benchmark_evidence.result_hash = record.result_hash;
  record.benchmark_evidence.result_row_count = 56 + cls.size();
  record.benchmark_evidence.optimizer_profile = record.optimizer_profile;
  record.benchmark_evidence.optimizer_toggles = {
      "property_frontier", "workload_budget_gate", "governed_memory_feedback"};
  record.benchmark_evidence.optimizer_toggles_digest =
      "sha256:ceic056-optimizer-toggles-" + cls;
  record.benchmark_evidence.benchmark_profile =
      "ceic056-workload-regression-budget";
  record.benchmark_evidence.benchmark_run_id = "ceic056-run-" + cls;
  record.benchmark_evidence.runner_id = "ceic056-runner";
  record.benchmark_evidence.catalog_epoch = 5606;
  record.benchmark_evidence.security_epoch = 5607;
  record.benchmark_evidence.redaction_epoch = 5608;
  record.benchmark_evidence.statistics_epoch = record.statistics_epoch;
  record.benchmark_evidence.feedback_generation = record.feedback_generation;
  record.benchmark_evidence.memory_feedback_generation =
      record.memory_feedback_generation;
  record.benchmark_evidence.provider_generation = record.provider_generation;
  record.benchmark_evidence.provenance_digest = record.provenance_digest;
  record.benchmark_evidence.evidence_digest = "sha256:ceic056-evidence-" + cls;
  record.benchmark_evidence.redaction_digest = record.redaction_digest;
  record.benchmark_evidence.retention_class = "optimizer_workload_budget";
  record.benchmark_evidence.freshness_microseconds = 1000;
  record.benchmark_evidence.max_freshness_microseconds = 60000000;
  record.benchmark_evidence.trusted_provenance = true;
  record.benchmark_evidence.fresh = true;
  record.benchmark_evidence.redaction_applied = true;
  record.benchmark_evidence.production_benchmark_clean_claim = true;
  record.benchmark_evidence.sample_groups = {Sample("cold"), Sample("warm")};
  record.benchmark_evidence.donor_methodology = {Donor()};

  record.correctness_oracle_attached = true;
  record.correctness_oracle_case.case_id = "ceic056-correctness-" + cls;
  record.correctness_oracle_case.correctness_class =
      CorrectnessClass(workload_class);
  record.correctness_oracle_case.route_kind = record.route_kind;
  record.correctness_oracle_case.route_label = record.route_label;
  record.correctness_oracle_case.dataset_schema_digest =
      record.dataset_schema_digest;
  record.correctness_oracle_case.sblr_digest = record.sblr_digest;
  record.correctness_oracle_case.logical_plan_hash = record.logical_plan_hash;
  record.correctness_oracle_case.baseline_plan_hash =
      "sha256:ceic056-baseline-plan-" + cls;
  record.correctness_oracle_case.optimized_plan_hash =
      record.physical_plan_hash;
  record.correctness_oracle_case.equivalence_contract_hash =
      "sha256:ceic056-equivalence-" + cls;
  if (record.correctness_oracle_case.correctness_class ==
      opt::OptimizerCorrectnessClass::kMixedFusion) {
    record.correctness_oracle_case.correlation_dependency_contract_hash =
        "sha256:ceic056-correlation-" + cls;
  }
  record.correctness_oracle_case.catalog_epoch = 5611;
  record.correctness_oracle_case.security_epoch = 5612;
  record.correctness_oracle_case.redaction_epoch = 5613;
  record.correctness_oracle_case.statistics_epoch = record.statistics_epoch;
  record.correctness_oracle_case.provider_generation =
      record.provider_generation;
  record.correctness_oracle_case.baseline =
      ResultSide("engine_baseline", cls, exact_rerank, dml_locator);
  record.correctness_oracle_case.optimized =
      ResultSide("optimized_engine_route", cls, exact_rerank, dml_locator);
  record.correctness_oracle_case.production_correctness_claim = true;
  record.correctness_oracle_case.evidence_only = true;
  record.correctness_oracle_case.baseline_is_engine_reference_route = true;
  record.correctness_oracle_case.optimized_route_consumed = true;
  record.correctness_oracle_case.exact_recheck_required = true;
  record.correctness_oracle_case.exact_rerank_required = exact_rerank;
  record.correctness_oracle_case.mga_recheck_required = true;
  record.correctness_oracle_case.security_recheck_required = true;
  record.correctness_oracle_case.donor_reference_only = true;

  return record;
}

std::vector<opt::OptimizerWorkloadRegressionBudgetRecord> FullSuite() {
  const std::vector<std::string> routes = {
      "embedded", "ipc", "inet", "cli", "driver"};
  std::vector<opt::OptimizerWorkloadRegressionBudgetRecord> suite;
  const auto classes = opt::RequiredOptimizerWorkloadRegressionClasses();
  for (std::size_t index = 0; index < classes.size(); ++index) {
    suite.push_back(Budget(classes[index], routes[index % routes.size()]));
  }
  return suite;
}

void FullSuiteIsProven() {
  const auto validation =
      opt::ValidateOptimizerWorkloadRegressionBudgetSuite(
          FullSuite(), opt::RequiredOptimizerWorkloadRegressionClasses(),
          {"embedded", "ipc", "inet", "cli", "driver"});
  Require(validation.ok, "CEIC-056 valid workload budget suite was rejected");
  Require(validation.regression_budget_proven,
          "CEIC-056 valid workload budget suite was not proven");
}

void UnsafeTableScanFallbackFailsClosed() {
  auto record = Budget(opt::OptimizerWorkloadRegressionClass::kOltpPointLookup,
                       "embedded");
  record.expected_access_class = opt::OptimizerExpectedAccessClass::kBoundedTableScan;
  record.table_scan_fallback_used = true;
  record.table_scan_fallback_policy =
      opt::OptimizerTableScanFallbackPolicy::kForbidden;
  const auto validation =
      opt::ValidateOptimizerWorkloadRegressionBudgetRecord(record);
  Require(!validation.ok, "CEIC-056 unsafe table scan fallback was accepted");
  Require(HasDiagnostic(validation.diagnostics, "UNSAFE_TABLE_SCAN_FALLBACK"),
          "CEIC-056 table scan fallback diagnostic missing");
}

void PlaceholderSyntheticAndUntrustedEvidenceFailClosed() {
  auto record =
      Budget(opt::OptimizerWorkloadRegressionClass::kOlapScanAggregate, "ipc");
  record.result_contract_hash = "result-contract-v1";
  record.metric_snapshot_generation = 1;
  record.provider_generation = 1;
  record.metrics_trusted = false;
  record.statistics_fresh = false;
  record.synthetic_statistics = true;
  record.placeholder_runtime_evidence = true;
  const auto validation =
      opt::ValidateOptimizerWorkloadRegressionBudgetRecord(record);
  Require(!validation.ok,
          "CEIC-056 placeholder/synthetic/untrusted evidence was accepted");
  Require(HasDiagnostic(validation.diagnostics, "PLACEHOLDER_EPOCH"),
          "CEIC-056 placeholder epoch diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "TRUST_FRESHNESS_INVALID"),
          "CEIC-056 trust/freshness diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "SYNTHETIC_OR_PLACEHOLDER"),
          "CEIC-056 synthetic/placeholder diagnostic missing");
}

void SpillAndRegressionBudgetFailClosed() {
  auto record = Budget(opt::OptimizerWorkloadRegressionClass::kJoin, "inet");
  record.spill_bound_bytes_present = false;
  record.unbounded_spill = true;
  record.observed_spill_bytes = record.max_spill_bytes + 1;
  record.optimized_p95_us = 150.0;
  record.optimized_p99_us = 210.0;
  record.observed_regression_ratio = 1.50;
  const auto validation =
      opt::ValidateOptimizerWorkloadRegressionBudgetRecord(record);
  Require(!validation.ok,
          "CEIC-056 unbounded spill/regression breach was accepted");
  Require(HasDiagnostic(validation.diagnostics, "UNBOUNDED_SPILL"),
          "CEIC-056 unbounded spill diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "REGRESSION_BUDGET_EXCEEDED"),
          "CEIC-056 regression budget diagnostic missing");
}

void SpecializedExactProofsAreMandatory() {
  auto record =
      Budget(opt::OptimizerWorkloadRegressionClass::kVectorApproximate,
             "driver");
  record.exact_fallback_proven = false;
  record.exact_recheck_proven = false;
  record.exact_rerank_proven = false;
  const auto validation =
      opt::ValidateOptimizerWorkloadRegressionBudgetRecord(record);
  Require(!validation.ok,
          "CEIC-056 missing exact fallback/recheck/rerank was accepted");
  Require(HasDiagnostic(validation.diagnostics, "EXACT_FALLBACK_MISSING"),
          "CEIC-056 exact fallback diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "EXACT_RECHECK_MISSING"),
          "CEIC-056 exact recheck diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "EXACT_RERANK_MISSING"),
          "CEIC-056 exact rerank diagnostic missing");
}

void AuthorityAndDonorDriftFailClosed() {
  auto record =
      Budget(opt::OptimizerWorkloadRegressionClass::kDocumentPath, "cli");
  record.authority.transaction_finality_authority = true;
  record.authority.visibility_authority = true;
  record.authority.parser_authority = true;
  record.authority.wal_authority = true;
  record.donor_reference_only = false;
  record.donor_as_authority = true;
  record.uses_donor_storage_or_finality_for_scratchbird = true;
  const auto validation =
      opt::ValidateOptimizerWorkloadRegressionBudgetRecord(record);
  Require(!validation.ok, "CEIC-056 authority/donor drift was accepted");
  Require(HasDiagnostic(validation.diagnostics, "FORBIDDEN_AUTHORITY"),
          "CEIC-056 forbidden authority diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "DONOR_AUTHORITY_DRIFT"),
          "CEIC-056 donor authority diagnostic missing");
}

void NestedBenchmarkAndCorrectnessProofsAreRequired() {
  auto missing =
      Budget(opt::OptimizerWorkloadRegressionClass::kTextWand, "embedded");
  missing.benchmark_evidence_attached = false;
  missing.correctness_oracle_attached = false;
  const auto missing_validation =
      opt::ValidateOptimizerWorkloadRegressionBudgetRecord(missing);
  Require(!missing_validation.ok,
          "CEIC-056 missing nested CEIC-051/052 proof was accepted");
  Require(HasDiagnostic(missing_validation.diagnostics,
                        "BENCHMARK_EVIDENCE_MISSING"),
          "CEIC-056 missing benchmark evidence diagnostic absent");
  Require(HasDiagnostic(missing_validation.diagnostics,
                        "CORRECTNESS_ORACLE_MISSING"),
          "CEIC-056 missing correctness oracle diagnostic absent");

  auto mismatch =
      Budget(opt::OptimizerWorkloadRegressionClass::kGraphTraversal, "ipc");
  mismatch.benchmark_evidence.result_contract_hash =
      "sha256:ceic056-wrong-contract";
  mismatch.correctness_oracle_case.optimized.result_hash =
      "sha256:ceic056-wrong-result";
  const auto mismatch_validation =
      opt::ValidateOptimizerWorkloadRegressionBudgetRecord(mismatch);
  Require(!mismatch_validation.ok,
          "CEIC-056 mismatched nested CEIC-051/052 proof was accepted");
  Require(HasDiagnostic(mismatch_validation.diagnostics,
                        "BENCHMARK_EVIDENCE_INVALID"),
          "CEIC-056 invalid benchmark evidence diagnostic absent");
  Require(HasDiagnostic(mismatch_validation.diagnostics,
                        "CORRECTNESS_ORACLE_INVALID"),
          "CEIC-056 invalid correctness oracle diagnostic absent");
}

void ClusterBoundariesAreClaimBlocked() {
  auto local =
      Budget(opt::OptimizerWorkloadRegressionClass::kBulkIngest, "cluster");
  local.cluster_mode =
      opt::OptimizerWorkloadRegressionClusterMode::kLocalClusterEvidence;
  const auto local_validation =
      opt::ValidateOptimizerWorkloadRegressionBudgetRecord(local);
  Require(!local_validation.ok, "CEIC-056 local cluster budget was accepted");
  Require(HasDiagnostic(local_validation.diagnostics, "LOCAL_CLUSTER_FORBIDDEN"),
          "CEIC-056 local cluster diagnostic missing");

  auto external =
      Budget(opt::OptimizerWorkloadRegressionClass::kBulkIngest, "embedded");
  external.cluster_mode =
      opt::OptimizerWorkloadRegressionClusterMode::kExternalProviderDelegated;
  external.external_cluster_provider_id = "external-cluster-provider";
  external.cluster_claim_blocked = true;
  external.production_regression_budget_claim = false;
  external.benchmark_clean_claim = false;
  external.benchmark_evidence.production_benchmark_clean_claim = false;
  external.correctness_oracle_case.production_correctness_claim = false;
  const auto external_validation =
      opt::ValidateOptimizerWorkloadRegressionBudgetRecord(external);
  Require(external_validation.ok,
          "CEIC-056 claim-blocked external cluster delegation was rejected");
  Require(!external_validation.regression_budget_proven,
          "CEIC-056 external cluster delegation closed local proof");

  external.production_regression_budget_claim = true;
  const auto overclaim =
      opt::ValidateOptimizerWorkloadRegressionBudgetRecord(external);
  Require(!overclaim.ok, "CEIC-056 external cluster overclaim was accepted");
  Require(HasDiagnostic(overclaim.diagnostics,
                        "EXTERNAL_CLUSTER_CLAIM_BLOCK_REQUIRED"),
          "CEIC-056 external cluster overclaim diagnostic missing");
}

void RequiredClassAndRouteCoverageIsExplicit() {
  auto suite = FullSuite();
  suite.pop_back();
  const auto validation =
      opt::ValidateOptimizerWorkloadRegressionBudgetSuite(
          suite, opt::RequiredOptimizerWorkloadRegressionClasses(),
          {"embedded", "ipc", "inet", "cli", "driver", "missing_route"});
  Require(!validation.ok, "CEIC-056 required class/route gap was accepted");
  Require(HasDiagnostic(validation.diagnostics, "MISSING_CLASS"),
          "CEIC-056 missing class diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "MISSING_ROUTE"),
          "CEIC-056 missing route diagnostic missing");
}

}  // namespace

int main() {
  FullSuiteIsProven();
  UnsafeTableScanFallbackFailsClosed();
  PlaceholderSyntheticAndUntrustedEvidenceFailClosed();
  SpillAndRegressionBudgetFailClosed();
  SpecializedExactProofsAreMandatory();
  AuthorityAndDonorDriftFailClosed();
  NestedBenchmarkAndCorrectnessProofsAreRequired();
  ClusterBoundariesAreClaimBlocked();
  RequiredClassAndRouteCoverageIsExplicit();
  std::cout << "ceic_056_optimizer_workload_regression_budget_gate=pass\n";
  return EXIT_SUCCESS;
}
