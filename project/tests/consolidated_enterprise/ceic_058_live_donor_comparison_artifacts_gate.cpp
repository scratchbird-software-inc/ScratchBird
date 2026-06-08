// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-058 focused validation for live donor comparison artifacts.
#include "optimizer_live_donor_comparison_artifacts.hpp"

#include <cstddef>
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
  if (!condition) {
    Fail(message);
  }
}

bool HasDiagnostic(const std::vector<std::string>& diagnostics,
                   std::string_view token) {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

opt::OptimizerBenchmarkSampleGroupEvidence Sample(std::string phase) {
  opt::OptimizerBenchmarkSampleGroupEvidence sample;
  sample.sample_group_id = "ceic058:" + phase;
  sample.cache_phase = phase;
  sample.latency_us_samples = {100.0, 110.0, 120.0, 130.0, 140.0};
  sample.p50_us = 120.0;
  sample.p95_us = 140.0;
  sample.p99_us = 140.0;
  sample.cpu_user_us = 1000;
  sample.cpu_system_us = 100;
  sample.memory_peak_bytes = 8 * 1024 * 1024;
  sample.memory_reserved_bytes = 12 * 1024 * 1024;
  sample.memory_released_bytes = 12 * 1024 * 1024;
  sample.io_read_bytes = 4096;
  sample.io_write_bytes = 4096;
  sample.io_read_ops = 1;
  sample.io_write_ops = 1;
  sample.cache_hits = 128;
  sample.cache_misses = 3;
  sample.page_cache_hits = 96;
  sample.page_cache_misses = 2;
  sample.estimated_rows = 1000.0;
  sample.actual_rows = 1010.0;
  sample.estimate_actual_ratio = 1.01;
  sample.skew_profile = "tenant_zipf_1_1";
  sample.cardinality_profile = "hll_histogram_mcv_extended_stats";
  sample.metric_snapshot_digest = "sha256:ceic058-metric-" + phase;
  sample.profiler_digest = "sha256:ceic058-profiler-" + phase;
  sample.cold_cache_reset_proven = phase == "cold";
  sample.warm_cache_prepared_proven = phase == "warm";
  return sample;
}

opt::OptimizerBenchmarkDonorMethodologyEvidence DonorMethod(
    const std::string& donor) {
  opt::OptimizerBenchmarkDonorMethodologyEvidence method;
  method.donor_engine = donor;
  method.donor_version = "ceic058-version-" + donor;
  method.donor_native_method = "native_" + donor + "_specialty_route";
  method.comparable_status = "comparable";
  method.dataset_schema_mapping_digest =
      "sha256:ceic058-dataset-map-" + donor;
  method.workload_mapping_digest = "sha256:ceic058-workload-map-" + donor;
  method.route_equivalence_contract_hash =
      "sha256:ceic058-route-equivalence-" + donor;
  method.donor_result_hash = "sha256:ceic058-donor-result-" + donor;
  method.donor_transaction_policy =
      "donor_reference_only_scratchbird_mga_not_substituted";
  method.donor_timing_policy =
      "artifact_only_prepared_cold_warm_native_method_recorded";
  method.donor_reference_only = true;
  return method;
}

opt::PersistedOptimizerBenchmarkEvidenceRecord BenchmarkFor(
    const std::string& donor,
    const std::string& route) {
  opt::PersistedOptimizerBenchmarkEvidenceRecord record;
  record.artifact_uuid = "ceic058-benchmark-" + donor + "-" + route;
  record.route_kind = route;
  record.route_lane = "enterprise/donor_comparison/" + donor + "/" + route;
  record.route_label = "enterprise/donor_comparison/customer_lookup";
  record.dataset_schema_uuid = "dataset-schema-uuid-v7-ceic058";
  record.dataset_schema_digest = "sha256:ceic058-dataset-schema";
  record.dataset_schema_version = "dataset-v1";
  record.sblr_digest = "sha256:ceic058-sblr";
  record.logical_plan_hash = "sha256:ceic058-logical-plan";
  record.physical_plan_hash = "sha256:ceic058-physical-plan";
  record.plan_cache_key_hash = "sha256:ceic058-plan-cache";
  record.result_contract_hash = "sha256:ceic058-result-contract";
  record.result_hash = "sha256:ceic058-scratchbird-result";
  record.result_row_count = 42;
  record.optimizer_profile = "enterprise";
  record.optimizer_toggles = {"property_frontier", "mga_pressure_costing",
                              "governed_memory_feedback"};
  record.optimizer_toggles_digest = "sha256:ceic058-optimizer-toggles";
  record.benchmark_profile = "ceic058-donor-comparison";
  record.benchmark_run_id = "ceic058-run-" + donor + "-" + route;
  record.runner_id = "ceic058-runner";
  record.catalog_epoch = 9201;
  record.security_epoch = 9202;
  record.redaction_epoch = 9203;
  record.statistics_epoch = 9204;
  record.feedback_generation = 9205;
  record.memory_feedback_generation = 9206;
  record.provider_generation = 9207;
  record.provenance_digest = "sha256:ceic058-provenance-" + donor;
  record.evidence_digest = "sha256:ceic058-evidence-" + donor;
  record.redaction_digest = "sha256:ceic058-redaction";
  record.retention_class = "donor_comparison_artifact_redacted";
  record.freshness_microseconds = 1000;
  record.max_freshness_microseconds = 60000000;
  record.trusted_provenance = true;
  record.fresh = true;
  record.redaction_applied = true;
  record.production_benchmark_clean_claim = true;
  record.sample_groups = {Sample("cold"), Sample("warm")};
  record.donor_methodology = {DonorMethod(donor)};
  return record;
}

std::string SpecialtyFor(std::string_view donor) {
  if (donor == "mongodb") return "document";
  if (donor == "neo4j") return "graph";
  if (donor == "redis" || donor == "dynamodb") return "key_value";
  if (donor == "elasticsearch" || donor == "opensearch") return "search";
  if (donor == "cassandra") return "wide_column";
  if (donor == "clickhouse" || donor == "snowflake" ||
      donor == "bigquery" || donor == "duckdb") {
    return "analytics";
  }
  if (donor == "influxdb" || donor == "timescaledb") return "time_series";
  return "relational";
}

opt::OptimizerDonorComparisonArtifactRow Row(std::string donor,
                                             std::string route) {
  opt::OptimizerDonorComparisonArtifactRow row;
  row.row_id = "ceic058-row-" + donor + "-" + route;
  row.donor_engine = donor;
  row.donor_version = "ceic058-version-" + donor;
  row.donor_version_digest = "sha256:ceic058-version-digest-" + donor;
  row.donor_specialty_class = SpecialtyFor(donor);
  row.donor_native_method = "native_" + donor + "_specialty_route";
  row.run_mode = opt::OptimizerDonorComparisonRunMode::kArtifactOnlyEvidence;
  row.live_external_engine_run_observed = false;
  row.donor_run_id = "ceic058-donor-run-" + donor;
  row.donor_execution_artifact_digest =
      "sha256:ceic058-donor-execution-artifact-" + donor;
  row.scratchbird_workload_id = "ceic058-customer-lookup";
  row.route_kind = route;
  row.route_lane = "enterprise/donor_comparison/" + donor + "/" + route;
  row.route_label = "enterprise/donor_comparison/customer_lookup";
  row.dataset_schema_digest = "sha256:ceic058-dataset-schema";
  row.dataset_mapping_digest = "sha256:ceic058-dataset-map-" + donor;
  row.workload_query_mapping_digest =
      "sha256:ceic058-workload-map-" + donor;
  row.route_equivalence_contract_hash =
      "sha256:ceic058-route-equivalence-" + donor;
  row.scratchbird_result_contract_hash = "sha256:ceic058-result-contract";
  row.scratchbird_result_hash = "sha256:ceic058-scratchbird-result";
  row.donor_result_hash = "sha256:ceic058-donor-result-" + donor;
  row.scratchbird_result_row_count = 42;
  row.donor_result_row_count = 42;
  row.comparable_status = opt::OptimizerDonorComparisonStatus::kComparable;
  row.timing_policy =
      "artifact_only_prepared_cold_warm_native_method_recorded";
  row.transaction_policy =
      "scratchbird_mga_authority_donor_reference_transaction_policy_recorded";
  row.timing.scratchbird_p50_us = 120.0;
  row.timing.scratchbird_p95_us = 140.0;
  row.timing.scratchbird_p99_us = 140.0;
  row.timing.donor_p50_us = 130.0;
  row.timing.donor_p95_us = 150.0;
  row.timing.donor_p99_us = 150.0;
  row.provenance_digest = "sha256:ceic058-provenance-" + donor;
  row.evidence_digest = "sha256:ceic058-evidence-" + donor;
  row.redaction_digest = "sha256:ceic058-redaction";
  row.source_provenance =
      "ceic058.artifact_only_donor_comparison_capture";
  row.catalog_epoch = 9201;
  row.security_epoch = 9202;
  row.redaction_epoch = 9203;
  row.statistics_epoch = 9204;
  row.donor_artifact_generation = 9205;
  row.dataset_mapping_generation = 9206;
  row.route_equivalence_generation = 9207;
  row.provider_generation = 9208;
  row.freshness_microseconds = 1000;
  row.max_freshness_microseconds = 60000000;
  row.trusted_provenance = true;
  row.fresh = true;
  row.redaction_applied = true;
  row.evidence_only = true;
  row.production_donor_comparison_claim = true;
  row.donor_reference_only = true;
  row.ceic_051_benchmark_evidence_attached = true;
  row.benchmark_evidence = BenchmarkFor(donor, route);
  return row;
}

opt::OptimizerDonorComparisonReport Report() {
  opt::OptimizerDonorComparisonReport report;
  report.report_id = "ceic058-donor-comparison-report";
  report.comparison_matrix_id = "ceic058-24-plus-donor-matrix";
  report.dataset_schema_digest = "sha256:ceic058-dataset-schema";
  report.optimizer_profile = "enterprise";
  report.provenance_digest = "sha256:ceic058-report-provenance";
  report.evidence_digest = "sha256:ceic058-report-evidence";
  report.redaction_digest = "sha256:ceic058-redaction";
  report.trusted_provenance = true;
  report.fresh = true;
  report.redaction_applied = true;
  report.evidence_only = true;
  report.production_donor_matrix_claim = true;
  report.required_donor_engines =
      opt::RequiredOptimizerDonorComparisonEngines();
  report.required_route_kinds = {"embedded", "ipc", "inet", "cli", "driver"};
  const std::vector<std::string> routes = {"embedded", "ipc", "inet", "cli",
                                           "driver"};
  std::size_t route_index = 0;
  for (const auto& donor : report.required_donor_engines) {
    report.rows.push_back(Row(donor, routes[route_index % routes.size()]));
    ++route_index;
  }
  return report;
}

void PositiveReportCoversTwentyFourPlusDonors() {
  const auto validation =
      opt::ValidateOptimizerDonorComparisonReport(Report());
  Require(validation.ok, "CEIC-058 valid donor comparison report was rejected");
  Require(validation.comparison_artifact_proven,
          "CEIC-058 valid report did not close local artifact proof");
}

void ReleaseProfilesAreNotDonorEngines() {
  const auto donors = opt::RequiredOptimizerDonorComparisonEngines();
  for (const auto& donor : donors) {
    Require(donor != "mysql_lts",
            "CEIC-058 treated MySQL LTS as a separate donor engine");
  }
}

void MissingDonorVersionFailsClosed() {
  auto row = Row("postgresql", "embedded");
  row.donor_version.clear();
  const auto validation =
      opt::ValidateOptimizerDonorComparisonArtifactRow(row);
  Require(!validation.ok, "CEIC-058 accepted missing donor version");
  Require(!validation.missing_fields.empty(),
          "CEIC-058 missing donor version did not produce missing field");
}

void MissingDatasetOrQueryMappingFailsClosed() {
  auto row = Row("postgresql", "embedded");
  row.dataset_mapping_digest.clear();
  row.workload_query_mapping_digest = "placeholder";
  row.route_equivalence_contract_hash.clear();
  const auto validation =
      opt::ValidateOptimizerDonorComparisonArtifactRow(row);
  Require(!validation.ok, "CEIC-058 accepted missing mappings");
  Require(!validation.missing_fields.empty(),
          "CEIC-058 missing mappings did not produce missing fields");
}

void ComparableRowsNeedHashesAndCeic051() {
  auto row = Row("firebird", "ipc");
  row.donor_result_hash.clear();
  row.ceic_051_benchmark_evidence_attached = false;
  const auto validation =
      opt::ValidateOptimizerDonorComparisonArtifactRow(row);
  Require(!validation.ok, "CEIC-058 accepted comparable row without evidence");
  Require(HasDiagnostic(validation.diagnostics,
                        "COMPARABLE_RESULT_MISSING"),
          "CEIC-058 did not flag comparable result evidence");
  Require(HasDiagnostic(validation.diagnostics,
                        "CEIC051_EVIDENCE_REQUIRED"),
          "CEIC-058 did not require CEIC-051 evidence");
}

void NonComparableRowsNeedBlockers() {
  auto row = Row("mongodb", "driver");
  row.comparable_status = opt::OptimizerDonorComparisonStatus::kNonComparable;
  row.run_mode = opt::OptimizerDonorComparisonRunMode::kNonComparableEvidence;
  row.ceic_051_benchmark_evidence_attached = false;
  row.donor_result_hash.clear();
  row.donor_result_row_count = 0;
  const auto missing =
      opt::ValidateOptimizerDonorComparisonArtifactRow(row);
  Require(!missing.ok, "CEIC-058 accepted non-comparable row without blocker");
  Require(HasDiagnostic(missing.diagnostics,
                        "NON_COMPARABLE_BLOCKER_REQUIRED"),
          "CEIC-058 did not flag missing non-comparable blocker");

  row.non_comparable_blockers = {
      "donor_native_graph_semantics_not_result-comparable_for_this_workload"};
  const auto valid =
      opt::ValidateOptimizerDonorComparisonArtifactRow(row);
  Require(valid.ok,
          "CEIC-058 rejected valid non-comparable blocker evidence");
  Require(valid.comparison_artifact_proven,
          "CEIC-058 did not accept valid non-comparable artifact proof");
}

void DonorAuthorityDriftFailsClosed() {
  auto row = Row("mysql", "inet");
  row.donor_storage_or_finality_substitution = true;
  row.authority.donor_result_authority = true;
  const auto validation =
      opt::ValidateOptimizerDonorComparisonArtifactRow(row);
  Require(!validation.ok, "CEIC-058 accepted donor authority drift");
  Require(HasDiagnostic(validation.diagnostics, "FORBIDDEN_AUTHORITY"),
          "CEIC-058 did not flag donor authority drift");
}

void PlaceholderAndStaleEvidenceFailsClosed() {
  auto row = Row("duckdb", "cli");
  row.scratchbird_result_contract_hash = "result-contract-v1";
  row.trusted_provenance = false;
  row.redaction_applied = false;
  row.synthetic_statistics = true;
  const auto validation =
      opt::ValidateOptimizerDonorComparisonArtifactRow(row);
  Require(!validation.ok, "CEIC-058 accepted placeholder stale evidence");
  Require(!validation.missing_fields.empty(),
          "CEIC-058 placeholder contract did not produce missing field");
  Require(HasDiagnostic(validation.diagnostics,
                        "PROVENANCE_FRESHNESS_INVALID"),
          "CEIC-058 did not flag stale/untrusted/unredacted evidence");
  Require(HasDiagnostic(validation.diagnostics,
                        "SYNTHETIC_OR_PLACEHOLDER"),
          "CEIC-058 did not flag synthetic evidence");
}

void MissingDonorBreadthFailsClosed() {
  auto report = Report();
  report.rows.pop_back();
  const auto validation =
      opt::ValidateOptimizerDonorComparisonReport(report);
  Require(!validation.ok, "CEIC-058 accepted missing donor breadth");
  Require(HasDiagnostic(validation.diagnostics, "MISSING_REQUIRED_DONOR") ||
              HasDiagnostic(validation.diagnostics,
                            "DONOR_BREADTH_INSUFFICIENT"),
          "CEIC-058 did not flag missing donor breadth");
}

void LocalClusterAndExternalClusterBehavior() {
  auto row = Row("postgresql", "embedded");
  row.cluster_mode = opt::OptimizerDonorComparisonClusterMode::kLocalClusterEvidence;
  const auto local =
      opt::ValidateOptimizerDonorComparisonArtifactRow(row);
  Require(!local.ok, "CEIC-058 accepted local cluster evidence");
  Require(HasDiagnostic(local.diagnostics, "LOCAL_CLUSTER_FORBIDDEN"),
          "CEIC-058 did not flag local cluster evidence");

  auto external = Row("postgresql", "embedded");
  external.cluster_mode =
      opt::OptimizerDonorComparisonClusterMode::kExternalProviderDelegated;
  external.external_cluster_provider_id = "external-cluster-provider-v1";
  external.cluster_claim_blocked = true;
  external.production_donor_comparison_claim = false;
  external.benchmark_evidence.cluster_mode =
      opt::OptimizerBenchmarkClusterMode::kExternalProviderDelegated;
  external.benchmark_evidence.external_cluster_provider_id =
      "external-cluster-provider-v1";
  external.benchmark_evidence.cluster_claim_blocked = true;
  external.benchmark_evidence.production_benchmark_clean_claim = false;
  const auto delegated =
      opt::ValidateOptimizerDonorComparisonArtifactRow(external);
  Require(delegated.ok,
          "CEIC-058 rejected claim-blocked external-cluster row");
  Require(!delegated.comparison_artifact_proven,
          "CEIC-058 external-cluster row closed local proof");

  external.production_donor_comparison_claim = true;
  const auto overclaim =
      opt::ValidateOptimizerDonorComparisonArtifactRow(external);
  Require(!overclaim.ok,
          "CEIC-058 accepted external-cluster production overclaim");
  Require(HasDiagnostic(overclaim.diagnostics,
                        "EXTERNAL_CLUSTER_CLAIM_BLOCK_REQUIRED"),
          "CEIC-058 did not flag external cluster overclaim");
}

}  // namespace

int main() {
  PositiveReportCoversTwentyFourPlusDonors();
  ReleaseProfilesAreNotDonorEngines();
  MissingDonorVersionFailsClosed();
  MissingDatasetOrQueryMappingFailsClosed();
  ComparableRowsNeedHashesAndCeic051();
  NonComparableRowsNeedBlockers();
  DonorAuthorityDriftFailsClosed();
  PlaceholderAndStaleEvidenceFailsClosed();
  MissingDonorBreadthFailsClosed();
  LocalClusterAndExternalClusterBehavior();
  return EXIT_SUCCESS;
}
