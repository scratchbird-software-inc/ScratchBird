// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-058 focused validation for live reference comparison artifacts.
#include "optimizer_live_reference_comparison_artifacts.hpp"

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

opt::OptimizerBenchmarkReferenceMethodologyEvidence ReferenceMethod(
    const std::string& reference) {
  opt::OptimizerBenchmarkReferenceMethodologyEvidence method;
  method.reference_engine = reference;
  method.reference_version = "ceic058-version-" + reference;
  method.reference_native_method = "native_" + reference + "_specialty_route";
  method.comparable_status = "comparable";
  method.dataset_schema_mapping_digest =
      "sha256:ceic058-dataset-map-" + reference;
  method.workload_mapping_digest = "sha256:ceic058-workload-map-" + reference;
  method.route_equivalence_contract_hash =
      "sha256:ceic058-route-equivalence-" + reference;
  method.reference_result_hash = "sha256:ceic058-reference-result-" + reference;
  method.reference_transaction_policy =
      "reference_reference_only_scratchbird_mga_not_substituted";
  method.reference_timing_policy =
      "artifact_only_prepared_cold_warm_native_method_recorded";
  method.reference_reference_only = true;
  return method;
}

opt::PersistedOptimizerBenchmarkEvidenceRecord BenchmarkFor(
    const std::string& reference,
    const std::string& route) {
  opt::PersistedOptimizerBenchmarkEvidenceRecord record;
  record.artifact_uuid = "ceic058-benchmark-" + reference + "-" + route;
  record.route_kind = route;
  record.route_lane = "enterprise/reference_comparison/" + reference + "/" + route;
  record.route_label = "enterprise/reference_comparison/customer_lookup";
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
  record.benchmark_profile = "ceic058-reference-comparison";
  record.benchmark_run_id = "ceic058-run-" + reference + "-" + route;
  record.runner_id = "ceic058-runner";
  record.catalog_epoch = 9201;
  record.security_epoch = 9202;
  record.redaction_epoch = 9203;
  record.statistics_epoch = 9204;
  record.feedback_generation = 9205;
  record.memory_feedback_generation = 9206;
  record.provider_generation = 9207;
  record.provenance_digest = "sha256:ceic058-provenance-" + reference;
  record.evidence_digest = "sha256:ceic058-evidence-" + reference;
  record.redaction_digest = "sha256:ceic058-redaction";
  record.retention_class = "reference_comparison_artifact_redacted";
  record.freshness_microseconds = 1000;
  record.max_freshness_microseconds = 60000000;
  record.trusted_provenance = true;
  record.fresh = true;
  record.redaction_applied = true;
  record.production_benchmark_clean_claim = true;
  record.sample_groups = {Sample("cold"), Sample("warm")};
  record.reference_methodology = {ReferenceMethod(reference)};
  return record;
}

std::string SpecialtyFor(std::string_view reference) {
  if (reference == "mongodb") return "document";
  if (reference == "neo4j") return "graph";
  if (reference == "redis" || reference == "dynamodb") return "key_value";
  if (reference == "elasticsearch" || reference == "opensearch") return "search";
  if (reference == "cassandra") return "wide_column";
  if (reference == "clickhouse" || reference == "snowflake" ||
      reference == "bigquery" || reference == "duckdb") {
    return "analytics";
  }
  if (reference == "influxdb" || reference == "timescaledb") return "time_series";
  return "relational";
}

opt::OptimizerReferenceComparisonArtifactRow Row(std::string reference,
                                             std::string route) {
  opt::OptimizerReferenceComparisonArtifactRow row;
  row.row_id = "ceic058-row-" + reference + "-" + route;
  row.reference_engine = reference;
  row.reference_version = "ceic058-version-" + reference;
  row.reference_version_digest = "sha256:ceic058-version-digest-" + reference;
  row.reference_specialty_class = SpecialtyFor(reference);
  row.reference_native_method = "native_" + reference + "_specialty_route";
  row.run_mode = opt::OptimizerReferenceComparisonRunMode::kArtifactOnlyEvidence;
  row.live_external_engine_run_observed = false;
  row.reference_run_id = "ceic058-reference-run-" + reference;
  row.reference_execution_artifact_digest =
      "sha256:ceic058-reference-execution-artifact-" + reference;
  row.scratchbird_workload_id = "ceic058-customer-lookup";
  row.route_kind = route;
  row.route_lane = "enterprise/reference_comparison/" + reference + "/" + route;
  row.route_label = "enterprise/reference_comparison/customer_lookup";
  row.dataset_schema_digest = "sha256:ceic058-dataset-schema";
  row.dataset_mapping_digest = "sha256:ceic058-dataset-map-" + reference;
  row.workload_query_mapping_digest =
      "sha256:ceic058-workload-map-" + reference;
  row.route_equivalence_contract_hash =
      "sha256:ceic058-route-equivalence-" + reference;
  row.scratchbird_result_contract_hash = "sha256:ceic058-result-contract";
  row.scratchbird_result_hash = "sha256:ceic058-scratchbird-result";
  row.reference_result_hash = "sha256:ceic058-reference-result-" + reference;
  row.scratchbird_result_row_count = 42;
  row.reference_result_row_count = 42;
  row.comparable_status = opt::OptimizerReferenceComparisonStatus::kComparable;
  row.timing_policy =
      "artifact_only_prepared_cold_warm_native_method_recorded";
  row.transaction_policy =
      "scratchbird_mga_authority_reference_reference_transaction_policy_recorded";
  row.timing.scratchbird_p50_us = 120.0;
  row.timing.scratchbird_p95_us = 140.0;
  row.timing.scratchbird_p99_us = 140.0;
  row.timing.reference_p50_us = 130.0;
  row.timing.reference_p95_us = 150.0;
  row.timing.reference_p99_us = 150.0;
  row.provenance_digest = "sha256:ceic058-provenance-" + reference;
  row.evidence_digest = "sha256:ceic058-evidence-" + reference;
  row.redaction_digest = "sha256:ceic058-redaction";
  row.source_provenance =
      "ceic058.artifact_only_reference_comparison_capture";
  row.catalog_epoch = 9201;
  row.security_epoch = 9202;
  row.redaction_epoch = 9203;
  row.statistics_epoch = 9204;
  row.reference_artifact_generation = 9205;
  row.dataset_mapping_generation = 9206;
  row.route_equivalence_generation = 9207;
  row.provider_generation = 9208;
  row.freshness_microseconds = 1000;
  row.max_freshness_microseconds = 60000000;
  row.trusted_provenance = true;
  row.fresh = true;
  row.redaction_applied = true;
  row.evidence_only = true;
  row.production_reference_comparison_claim = true;
  row.reference_reference_only = true;
  row.ceic_051_benchmark_evidence_attached = true;
  row.benchmark_evidence = BenchmarkFor(reference, route);
  return row;
}

opt::OptimizerReferenceComparisonReport Report() {
  opt::OptimizerReferenceComparisonReport report;
  report.report_id = "ceic058-reference-comparison-report";
  report.comparison_matrix_id = "ceic058-24-plus-reference-matrix";
  report.dataset_schema_digest = "sha256:ceic058-dataset-schema";
  report.optimizer_profile = "enterprise";
  report.provenance_digest = "sha256:ceic058-report-provenance";
  report.evidence_digest = "sha256:ceic058-report-evidence";
  report.redaction_digest = "sha256:ceic058-redaction";
  report.trusted_provenance = true;
  report.fresh = true;
  report.redaction_applied = true;
  report.evidence_only = true;
  report.production_reference_matrix_claim = true;
  report.required_reference_engines =
      opt::RequiredOptimizerReferenceComparisonEngines();
  report.required_route_kinds = {"embedded", "ipc", "inet", "cli", "driver"};
  const std::vector<std::string> routes = {"embedded", "ipc", "inet", "cli",
                                           "driver"};
  std::size_t route_index = 0;
  for (const auto& reference : report.required_reference_engines) {
    report.rows.push_back(Row(reference, routes[route_index % routes.size()]));
    ++route_index;
  }
  return report;
}

void PositiveReportCoversTwentyFourPlusReferences() {
  const auto validation =
      opt::ValidateOptimizerReferenceComparisonReport(Report());
  Require(validation.ok, "CEIC-058 valid reference comparison report was rejected");
  Require(validation.comparison_artifact_proven,
          "CEIC-058 valid report did not close local artifact proof");
}

void ReleaseProfilesAreNotReferenceEngines() {
  const auto references = opt::RequiredOptimizerReferenceComparisonEngines();
  for (const auto& reference : references) {
    Require(reference != "mysql_lts",
            "CEIC-058 treated MySQL LTS as a separate reference engine");
  }
}

void MissingReferenceVersionFailsClosed() {
  auto row = Row("postgresql", "embedded");
  row.reference_version.clear();
  const auto validation =
      opt::ValidateOptimizerReferenceComparisonArtifactRow(row);
  Require(!validation.ok, "CEIC-058 accepted missing reference version");
  Require(!validation.missing_fields.empty(),
          "CEIC-058 missing reference version did not produce missing field");
}

void MissingDatasetOrQueryMappingFailsClosed() {
  auto row = Row("postgresql", "embedded");
  row.dataset_mapping_digest.clear();
  row.workload_query_mapping_digest = "placeholder";
  row.route_equivalence_contract_hash.clear();
  const auto validation =
      opt::ValidateOptimizerReferenceComparisonArtifactRow(row);
  Require(!validation.ok, "CEIC-058 accepted missing mappings");
  Require(!validation.missing_fields.empty(),
          "CEIC-058 missing mappings did not produce missing fields");
}

void ComparableRowsNeedHashesAndCeic051() {
  auto row = Row("firebird", "ipc");
  row.reference_result_hash.clear();
  row.ceic_051_benchmark_evidence_attached = false;
  const auto validation =
      opt::ValidateOptimizerReferenceComparisonArtifactRow(row);
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
  row.comparable_status = opt::OptimizerReferenceComparisonStatus::kNonComparable;
  row.run_mode = opt::OptimizerReferenceComparisonRunMode::kNonComparableEvidence;
  row.ceic_051_benchmark_evidence_attached = false;
  row.reference_result_hash.clear();
  row.reference_result_row_count = 0;
  const auto missing =
      opt::ValidateOptimizerReferenceComparisonArtifactRow(row);
  Require(!missing.ok, "CEIC-058 accepted non-comparable row without blocker");
  Require(HasDiagnostic(missing.diagnostics,
                        "NON_COMPARABLE_BLOCKER_REQUIRED"),
          "CEIC-058 did not flag missing non-comparable blocker");

  row.non_comparable_blockers = {
      "reference_native_graph_semantics_not_result-comparable_for_this_workload"};
  const auto valid =
      opt::ValidateOptimizerReferenceComparisonArtifactRow(row);
  Require(valid.ok,
          "CEIC-058 rejected valid non-comparable blocker evidence");
  Require(valid.comparison_artifact_proven,
          "CEIC-058 did not accept valid non-comparable artifact proof");
}

void ReferenceAuthorityDriftFailsClosed() {
  auto row = Row("mysql", "inet");
  row.reference_storage_or_finality_substitution = true;
  row.authority.reference_result_authority = true;
  const auto validation =
      opt::ValidateOptimizerReferenceComparisonArtifactRow(row);
  Require(!validation.ok, "CEIC-058 accepted reference authority drift");
  Require(HasDiagnostic(validation.diagnostics, "FORBIDDEN_AUTHORITY"),
          "CEIC-058 did not flag reference authority drift");
}

void PlaceholderAndStaleEvidenceFailsClosed() {
  auto row = Row("duckdb", "cli");
  row.scratchbird_result_contract_hash = "result-contract-v1";
  row.trusted_provenance = false;
  row.redaction_applied = false;
  row.synthetic_statistics = true;
  const auto validation =
      opt::ValidateOptimizerReferenceComparisonArtifactRow(row);
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

void MissingReferenceBreadthFailsClosed() {
  auto report = Report();
  report.rows.pop_back();
  const auto validation =
      opt::ValidateOptimizerReferenceComparisonReport(report);
  Require(!validation.ok, "CEIC-058 accepted missing reference breadth");
  Require(HasDiagnostic(validation.diagnostics, "MISSING_REQUIRED_REFERENCE") ||
              HasDiagnostic(validation.diagnostics,
                            "REFERENCE_BREADTH_INSUFFICIENT"),
          "CEIC-058 did not flag missing reference breadth");
}

void LocalClusterAndExternalClusterBehavior() {
  auto row = Row("postgresql", "embedded");
  row.cluster_mode = opt::OptimizerReferenceComparisonClusterMode::kLocalClusterEvidence;
  const auto local =
      opt::ValidateOptimizerReferenceComparisonArtifactRow(row);
  Require(!local.ok, "CEIC-058 accepted local cluster evidence");
  Require(HasDiagnostic(local.diagnostics, "LOCAL_CLUSTER_FORBIDDEN"),
          "CEIC-058 did not flag local cluster evidence");

  auto external = Row("postgresql", "embedded");
  external.cluster_mode =
      opt::OptimizerReferenceComparisonClusterMode::kExternalProviderDelegated;
  external.external_cluster_provider_id = "external-cluster-provider-v1";
  external.cluster_claim_blocked = true;
  external.production_reference_comparison_claim = false;
  external.benchmark_evidence.cluster_mode =
      opt::OptimizerBenchmarkClusterMode::kExternalProviderDelegated;
  external.benchmark_evidence.external_cluster_provider_id =
      "external-cluster-provider-v1";
  external.benchmark_evidence.cluster_claim_blocked = true;
  external.benchmark_evidence.production_benchmark_clean_claim = false;
  const auto delegated =
      opt::ValidateOptimizerReferenceComparisonArtifactRow(external);
  Require(delegated.ok,
          "CEIC-058 rejected claim-blocked external-cluster row");
  Require(!delegated.comparison_artifact_proven,
          "CEIC-058 external-cluster row closed local proof");

  external.production_reference_comparison_claim = true;
  const auto overclaim =
      opt::ValidateOptimizerReferenceComparisonArtifactRow(external);
  Require(!overclaim.ok,
          "CEIC-058 accepted external-cluster production overclaim");
  Require(HasDiagnostic(overclaim.diagnostics,
                        "EXTERNAL_CLUSTER_CLAIM_BLOCK_REQUIRED"),
          "CEIC-058 did not flag external cluster overclaim");
}

}  // namespace

int main() {
  PositiveReportCoversTwentyFourPlusReferences();
  ReleaseProfilesAreNotReferenceEngines();
  MissingReferenceVersionFailsClosed();
  MissingDatasetOrQueryMappingFailsClosed();
  ComparableRowsNeedHashesAndCeic051();
  NonComparableRowsNeedBlockers();
  ReferenceAuthorityDriftFailsClosed();
  PlaceholderAndStaleEvidenceFailsClosed();
  MissingReferenceBreadthFailsClosed();
  LocalClusterAndExternalClusterBehavior();
  return EXIT_SUCCESS;
}
