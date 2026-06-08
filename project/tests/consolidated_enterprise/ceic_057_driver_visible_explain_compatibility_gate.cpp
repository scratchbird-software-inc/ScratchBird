// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-057 focused validation for driver-visible optimizer explain
// compatibility.
#include "optimizer_driver_explain_compatibility.hpp"

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
  sample.sample_group_id = "ceic057:" + phase;
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
  sample.skew_profile = "zipf_1_1_uniform_tenant_mix";
  sample.cardinality_profile = "hll_histogram_mcv_extended_stats";
  sample.metric_snapshot_digest = "sha256:ceic057-metric-" + phase;
  sample.profiler_digest = "sha256:ceic057-profiler-" + phase;
  sample.cold_cache_reset_proven = phase == "cold";
  sample.warm_cache_prepared_proven = phase == "warm";
  return sample;
}

opt::OptimizerBenchmarkDonorMethodologyEvidence Donor(std::string engine) {
  opt::OptimizerBenchmarkDonorMethodologyEvidence donor;
  donor.donor_engine = engine;
  donor.donor_version = "ceic057-donor-version";
  donor.donor_native_method = "donor_native_best_route";
  donor.comparable_status = "comparable";
  donor.dataset_schema_mapping_digest = "sha256:ceic057-donor-dataset-" + engine;
  donor.workload_mapping_digest = "sha256:ceic057-donor-workload-" + engine;
  donor.route_equivalence_contract_hash = "sha256:ceic057-donor-route-" + engine;
  donor.donor_result_hash = "sha256:ceic057-donor-result-" + engine;
  donor.donor_transaction_policy =
      "donor_reference_only_scratchbird_mga_not_substituted";
  donor.donor_timing_policy = "prepared_warm_output_suppressed";
  donor.donor_reference_only = true;
  return donor;
}

opt::PersistedOptimizerBenchmarkEvidenceRecord BenchmarkFor(std::string route) {
  opt::PersistedOptimizerBenchmarkEvidenceRecord record;
  record.artifact_uuid = "ceic057-benchmark-" + route;
  record.route_kind = route;
  record.route_lane = "enterprise/explain/customer_lookup/" + route;
  record.route_label = "enterprise/explain/customer_lookup";
  record.dataset_schema_uuid = "dataset-schema-uuid-v7-" + route;
  record.dataset_schema_digest = "sha256:ceic057-dataset-schema";
  record.dataset_schema_version = "dataset-v1";
  record.sblr_digest = "sha256:ceic057-sblr";
  record.logical_plan_hash = "sha256:ceic057-logical-plan";
  record.physical_plan_hash = "sha256:ceic057-plan";
  record.plan_cache_key_hash = "sha256:ceic057-plan-cache";
  record.result_contract_hash = "sha256:ceic057-result-contract";
  record.result_hash = "sha256:ceic057-result";
  record.result_row_count = 42;
  record.optimizer_profile = "enterprise";
  record.optimizer_toggles = {"property_frontier", "mga_pressure_costing",
                              "governed_memory_feedback"};
  record.optimizer_toggles_digest = "sha256:ceic057-optimizer-toggles";
  record.benchmark_profile = "ceic057-driver-visible-explain";
  record.benchmark_run_id = "ceic057-run-" + route;
  record.runner_id = "ceic057-runner";
  record.catalog_epoch = 9101;
  record.security_epoch = 9102;
  record.redaction_epoch = 9103;
  record.statistics_epoch = 9104;
  record.feedback_generation = 9105;
  record.memory_feedback_generation = 9106;
  record.provider_generation = 9107;
  record.provenance_digest = "sha256:ceic057-provenance-" + route;
  record.evidence_digest = "sha256:ceic057-evidence-" + route;
  record.redaction_digest = "sha256:ceic057-redaction";
  record.retention_class = "driver_explain_evidence_redacted";
  record.freshness_microseconds = 1000;
  record.max_freshness_microseconds = 60000000;
  record.trusted_provenance = true;
  record.fresh = true;
  record.redaction_applied = true;
  record.production_benchmark_clean_claim = true;
  record.sample_groups = {Sample("cold"), Sample("warm")};
  record.donor_methodology = {Donor("postgresql"), Donor("firebird")};
  return record;
}

std::string ExplainJson() {
  return "{\"schema_version\":\"sb.optimizer.explain.driver_visible_json.v1\","
         "\"plan_hash\":\"sha256:ceic057-plan\","
         "\"result_contract_hash\":\"sha256:ceic057-result-contract\","
         "\"redaction\":\"applied\","
         "\"diagnostics\":[\"SB_OPT_EXPLAIN.OK\"],"
         "\"route_label\":\"enterprise/explain/customer_lookup\"}";
}

opt::OptimizerDriverVisibleExplainRouteRecord Route(std::string route) {
  opt::OptimizerDriverVisibleExplainRouteRecord record;
  record.route_kind = route;
  record.route_label = "enterprise/explain/customer_lookup";
  record.embedded_reference_route = route == "embedded";
  record.claimed_driver_route = route == "driver";
  record.claimed_driver_name = route == "driver" ? "cpp" : "";
  record.driver_visible_route = true;
  record.explain_json = ExplainJson();
  record.json_canonicalization_digest = "sha256:ceic057-json-canonical";
  record.plan_hash = "sha256:ceic057-plan";
  record.plan_evidence_digest = "sha256:ceic057-plan-evidence";
  record.explain_digest = "sha256:ceic057-explain";
  record.result_contract_hash = "sha256:ceic057-result-contract";
  record.result_hash = "sha256:ceic057-result";
  record.diagnostic_code = "SB_OPT_EXPLAIN.OK";
  record.diagnostics = {"SB_OPT_EXPLAIN.OK"};
  record.redaction_digest = "sha256:ceic057-redaction";
  record.redaction_applied = true;
  record.no_sql_text_leak = true;
  record.no_raw_uuid_leak = true;
  record.no_protected_material_leak = true;
  record.redaction_proofs = {"public_explain_redaction_profile_v1",
                             "protected_material_redacted"};
  record.catalog_epoch = 9101;
  record.security_epoch = 9102;
  record.redaction_epoch = 9103;
  record.statistics_epoch = 9104;
  record.route_epoch = 9105;
  record.provider_generation = 9107;
  record.source_generation = 9108;
  record.freshness_microseconds = 1000;
  record.max_freshness_microseconds = 60000000;
  record.optimizer_profile = "enterprise";
  record.source_provenance = "engine.route.driver_visible_explain";
  record.provenance_digest = "sha256:ceic057-route-provenance-" + route;
  record.evidence_digest = "sha256:ceic057-route-evidence-" + route;
  record.trusted_provenance = true;
  record.fresh = true;
  record.evidence_only = true;
  record.ceic_051_benchmark_evidence_attached = true;
  record.benchmark_evidence = BenchmarkFor(route);
  return record;
}

opt::OptimizerDriverVisibleExplainCompatibilityReport Report() {
  opt::OptimizerDriverVisibleExplainCompatibilityReport report;
  report.report_id = "ceic057-driver-explain-report";
  report.dataset_schema_digest = "sha256:ceic057-dataset-schema";
  report.sblr_digest = "sha256:ceic057-sblr";
  report.logical_plan_hash = "sha256:ceic057-logical-plan";
  report.optimizer_profile = "enterprise";
  report.evidence_digest = "sha256:ceic057-report-evidence";
  report.provenance_digest = "sha256:ceic057-report-provenance";
  report.trusted_provenance = true;
  report.fresh = true;
  report.evidence_only = true;
  report.production_compatibility_claim = true;
  report.require_claimed_driver_route = true;
  report.required_route_kinds = {"embedded", "ipc", "inet", "cli", "driver"};
  report.claimed_driver_names = {"cpp"};
  report.routes = {Route("embedded"), Route("ipc"), Route("inet"),
                   Route("cli"), Route("driver")};
  return report;
}

void PositiveReportIsCompatible() {
  const auto validation =
      opt::ValidateOptimizerDriverVisibleExplainCompatibilityReport(Report());
  Require(validation.ok, "CEIC-057 valid explain report was rejected");
  Require(validation.compatibility_proven,
          "CEIC-057 valid explain report did not close local compatibility proof");
}

void MissingEmbeddedReferenceFailsClosed() {
  auto report = Report();
  report.routes.erase(report.routes.begin());
  const auto validation =
      opt::ValidateOptimizerDriverVisibleExplainCompatibilityReport(report);
  Require(!validation.ok, "CEIC-057 missing embedded reference was accepted");
  Require(HasDiagnostic(validation.diagnostics, "EMBEDDED_REFERENCE_MISSING"),
          "CEIC-057 embedded-reference diagnostic missing");
}

void MissingClaimedDriverFailsClosed() {
  auto report = Report();
  report.routes.pop_back();
  const auto validation =
      opt::ValidateOptimizerDriverVisibleExplainCompatibilityReport(report);
  Require(!validation.ok, "CEIC-057 missing claimed driver was accepted");
  Require(HasDiagnostic(validation.diagnostics, "CLAIMED_DRIVER_ROUTE_MISSING") ||
              HasDiagnostic(validation.diagnostics, "MISSING_REQUIRED_ROUTE"),
          "CEIC-057 claimed-driver diagnostic missing");
}

void ExplainPlanAndResultMismatchFailsClosed() {
  auto report = Report();
  report.routes[2].plan_hash = "sha256:ceic057-plan-different";
  report.routes[2].explain_digest = "sha256:ceic057-explain-different";
  report.routes[3].result_hash = "sha256:ceic057-result-different";
  const auto validation =
      opt::ValidateOptimizerDriverVisibleExplainCompatibilityReport(report);
  Require(!validation.ok, "CEIC-057 route mismatch was accepted");
  Require(HasDiagnostic(validation.diagnostics, "EXPLAIN_DIGEST_MISMATCH"),
          "CEIC-057 explain mismatch diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "RESULT_MISMATCH"),
          "CEIC-057 result mismatch diagnostic missing");
}

void DiagnosticAndRedactionMismatchFailsClosed() {
  auto report = Report();
  report.routes[1].diagnostic_code = "SB_OPT_EXPLAIN.DIFFERENT";
  report.routes[2].redaction_digest = "sha256:ceic057-redaction-different";
  const auto validation =
      opt::ValidateOptimizerDriverVisibleExplainCompatibilityReport(report);
  Require(!validation.ok, "CEIC-057 diagnostic/redaction mismatch was accepted");
  Require(HasDiagnostic(validation.diagnostics, "DIAGNOSTIC_MISMATCH"),
          "CEIC-057 diagnostic mismatch diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "REDACTION_MISMATCH"),
          "CEIC-057 redaction mismatch diagnostic missing");
}

void PlaceholderAndLeakageFailsClosed() {
  auto report = Report();
  auto& route = report.routes.front();
  route.result_contract_hash = "result-contract-v1";
  route.explain_json =
      "{\"schema_version\":\"sb.optimizer.explain.driver_visible_json.v1\","
      "\"sql_text\":\"SELECT secret FROM t\","
      "\"scope\":\"123e4567-e89b-12d3-a456-426614174000\","
      "\"plan_hash\":\"sha256:placeholder\","
      "\"result_contract_hash\":\"result-contract-v1\","
      "\"redaction\":\"applied\"}";
  route.no_sql_text_leak = false;
  route.no_raw_uuid_leak = false;
  route.no_protected_material_leak = false;
  const auto validation =
      opt::ValidateOptimizerDriverVisibleExplainCompatibilityReport(report);
  Require(!validation.ok, "CEIC-057 placeholder/leaky explain was accepted");
  Require(HasDiagnostic(validation.diagnostics, "SQL_TEXT_LEAK"),
          "CEIC-057 SQL leak diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "RAW_UUID_LEAK"),
          "CEIC-057 raw UUID diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "PROTECTED_MATERIAL_LEAK"),
          "CEIC-057 protected material diagnostic missing");
}

void StaleSyntheticAndAuthorityDriftFailClosed() {
  auto report = Report();
  auto& route = report.routes.front();
  route.source_generation = 1;
  route.freshness_microseconds = route.max_freshness_microseconds + 1;
  route.trusted_provenance = false;
  route.synthetic_statistics = true;
  route.authority.parser_authority = true;
  route.authority.donor_authority = true;
  route.authority.wal_authority = true;
  route.authority.optimizer_plan_authority = true;
  const auto validation =
      opt::ValidateOptimizerDriverVisibleExplainCompatibilityReport(report);
  Require(!validation.ok, "CEIC-057 stale synthetic authority drift was accepted");
  Require(HasDiagnostic(validation.diagnostics, "PLACEHOLDER_EPOCH"),
          "CEIC-057 placeholder epoch diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "PROVENANCE_FRESHNESS_INVALID"),
          "CEIC-057 provenance diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "SYNTHETIC_OR_PLACEHOLDER"),
          "CEIC-057 synthetic diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "FORBIDDEN_AUTHORITY"),
          "CEIC-057 forbidden authority diagnostic missing");
}

void NestedCeic051MismatchFailsClosed() {
  auto report = Report();
  report.routes[1].benchmark_evidence.result_hash =
      "sha256:ceic057-nested-result-mismatch";
  const auto validation =
      opt::ValidateOptimizerDriverVisibleExplainCompatibilityReport(report);
  Require(!validation.ok, "CEIC-057 nested CEIC-051 mismatch was accepted");
  Require(HasDiagnostic(validation.diagnostics, "CEIC051_ROUTE_MISMATCH"),
          "CEIC-057 nested CEIC-051 mismatch diagnostic missing");

  report = Report();
  report.routes[1].benchmark_evidence.result_contract_hash =
      "result-contract-v1";
  const auto invalid_nested =
      opt::ValidateOptimizerDriverVisibleExplainCompatibilityReport(report);
  Require(!invalid_nested.ok, "CEIC-057 invalid CEIC-051 evidence was accepted");
  Require(HasDiagnostic(invalid_nested.diagnostics, "NESTED_CEIC051_INVALID"),
          "CEIC-057 invalid nested CEIC-051 diagnostic missing");
}

void ClusterBoundariesFailClosed() {
  auto local = Report();
  local.cluster_mode =
      opt::OptimizerExplainCompatibilityClusterMode::kLocalClusterEvidence;
  const auto local_validation =
      opt::ValidateOptimizerDriverVisibleExplainCompatibilityReport(local);
  Require(!local_validation.ok,
          "CEIC-057 local cluster explain evidence was accepted");
  Require(HasDiagnostic(local_validation.diagnostics, "LOCAL_CLUSTER_FORBIDDEN"),
          "CEIC-057 local cluster diagnostic missing");

  auto external = Report();
  external.cluster_mode =
      opt::OptimizerExplainCompatibilityClusterMode::kExternalProviderDelegated;
  external.external_cluster_provider_id = "external-cluster-provider";
  external.cluster_claim_blocked = true;
  external.production_compatibility_claim = false;
  for (auto& route : external.routes) {
    route.benchmark_evidence.cluster_mode =
        opt::OptimizerBenchmarkClusterMode::kExternalProviderDelegated;
    route.benchmark_evidence.external_cluster_provider_id =
        "external-cluster-provider";
    route.benchmark_evidence.cluster_claim_blocked = true;
    route.benchmark_evidence.production_benchmark_clean_claim = false;
  }
  const auto external_validation =
      opt::ValidateOptimizerDriverVisibleExplainCompatibilityReport(external);
  Require(external_validation.ok,
          "CEIC-057 claim-blocked external cluster delegation was rejected");
  Require(!external_validation.compatibility_proven,
          "CEIC-057 external cluster delegation must not close local proof");

  auto nested_overclaim = external;
  nested_overclaim.routes.front().benchmark_evidence.production_benchmark_clean_claim =
      true;
  const auto nested =
      opt::ValidateOptimizerDriverVisibleExplainCompatibilityReport(
          nested_overclaim);
  Require(!nested.ok, "CEIC-057 nested external cluster claim was accepted");
  Require(HasDiagnostic(nested.diagnostics,
                        "EXTERNAL_CLUSTER_NESTED_CLAIM_BLOCK_REQUIRED"),
          "CEIC-057 nested external cluster diagnostic missing");

  external.production_compatibility_claim = true;
  const auto overclaim =
      opt::ValidateOptimizerDriverVisibleExplainCompatibilityReport(external);
  Require(!overclaim.ok, "CEIC-057 external cluster overclaim was accepted");
  Require(HasDiagnostic(overclaim.diagnostics,
                        "EXTERNAL_CLUSTER_CLAIM_BLOCK_REQUIRED"),
          "CEIC-057 external cluster overclaim diagnostic missing");
}

}  // namespace

int main() {
  PositiveReportIsCompatible();
  MissingEmbeddedReferenceFailsClosed();
  MissingClaimedDriverFailsClosed();
  ExplainPlanAndResultMismatchFailsClosed();
  DiagnosticAndRedactionMismatchFailsClosed();
  PlaceholderAndLeakageFailsClosed();
  StaleSyntheticAndAuthorityDriftFailClosed();
  NestedCeic051MismatchFailsClosed();
  ClusterBoundariesFailClosed();
  std::cout << "CEIC-057 driver-visible explain compatibility gate passed\n";
  return EXIT_SUCCESS;
}
