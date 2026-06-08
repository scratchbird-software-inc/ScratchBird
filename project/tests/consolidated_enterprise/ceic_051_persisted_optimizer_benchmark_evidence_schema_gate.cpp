// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-051 focused validation for persisted optimizer benchmark evidence schema.
#include "optimizer_benchmark_evidence_schema.hpp"

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
  sample.sample_group_id = "ceic051:" + phase;
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
  sample.metric_snapshot_digest = "sha256:metric-snapshot-" + phase;
  sample.profiler_digest = "sha256:profiler-" + phase;
  sample.cold_cache_reset_proven = phase == "cold";
  sample.warm_cache_prepared_proven = phase == "warm";
  return sample;
}

opt::OptimizerBenchmarkDonorMethodologyEvidence Donor(std::string engine) {
  opt::OptimizerBenchmarkDonorMethodologyEvidence donor;
  donor.donor_engine = engine;
  donor.donor_version = "ceic051-donor-version";
  donor.donor_native_method = "donor_native_best_route";
  donor.comparable_status = "comparable";
  donor.dataset_schema_mapping_digest = "sha256:donor-dataset-map-" + engine;
  donor.workload_mapping_digest = "sha256:donor-workload-map-" + engine;
  donor.route_equivalence_contract_hash = "sha256:donor-route-contract-" + engine;
  donor.donor_result_hash = "sha256:donor-result-" + engine;
  donor.donor_transaction_policy =
      "donor_native_reference_only_scratchbird_mga_not_substituted";
  donor.donor_timing_policy = "prepared_warm_output_suppressed_best_normal_method";
  donor.donor_reference_only = true;
  return donor;
}

opt::PersistedOptimizerBenchmarkEvidenceRecord Record(std::string route) {
  opt::PersistedOptimizerBenchmarkEvidenceRecord record;
  record.artifact_uuid = "ceic051-artifact-" + route;
  record.route_kind = route;
  record.route_lane = "enterprise/customer_lookup/" + route;
  record.route_label = "enterprise/sql/customer_lookup";
  record.dataset_schema_uuid = "dataset-schema-uuid-v7-" + route;
  record.dataset_schema_digest = "sha256:dataset-schema-" + route;
  record.dataset_schema_version = "dataset-v1";
  record.sblr_digest = "sha256:sblr-" + route;
  record.logical_plan_hash = "sha256:logical-plan-" + route;
  record.physical_plan_hash = "sha256:physical-plan-" + route;
  record.plan_cache_key_hash = "sha256:plan-cache-" + route;
  record.result_contract_hash = "sha256:result-contract-" + route;
  record.result_hash = "sha256:result-" + route;
  record.result_row_count = 42;
  record.optimizer_profile = "enterprise";
  record.optimizer_toggles = {"property_frontier", "mga_pressure_costing",
                              "governed_memory_feedback"};
  record.optimizer_toggles_digest = "sha256:optimizer-toggles-" + route;
  record.benchmark_profile = "ceic051-persisted-schema";
  record.benchmark_run_id = "ceic051-run-" + route;
  record.runner_id = "ceic051-runner";
  record.catalog_epoch = 9101;
  record.security_epoch = 9102;
  record.redaction_epoch = 9103;
  record.statistics_epoch = 9104;
  record.feedback_generation = 9105;
  record.memory_feedback_generation = 9106;
  record.provider_generation = 9107;
  record.provenance_digest = "sha256:provenance-" + route;
  record.evidence_digest = "sha256:evidence-" + route;
  record.redaction_digest = "sha256:redaction-" + route;
  record.retention_class = "benchmark_evidence_redacted";
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

std::vector<opt::PersistedOptimizerBenchmarkEvidenceRecord> RouteSet() {
  return {Record("embedded"), Record("ipc"), Record("inet"),
          Record("cli"), Record("driver")};
}

void PositiveRouteSetIsAdmissible() {
  const auto validation =
      opt::ValidatePersistedOptimizerBenchmarkEvidenceSet(
          RouteSet(), {"embedded", "ipc", "inet", "cli", "driver"});
  Require(validation.ok, "CEIC-051 valid persisted evidence set was rejected");
  Require(validation.benchmark_clean_admissible,
          "CEIC-051 valid persisted evidence set was not benchmark-clean admissible");
}

void PlaceholderAndSyntheticEvidenceFailsClosed() {
  auto record = Record("embedded");
  record.result_contract_hash = "result-contract-v1";
  record.catalog_epoch = 1;
  record.provider_generation = 1;
  record.placeholder_runtime_evidence = true;
  record.synthetic_statistics = true;
  const auto validation =
      opt::ValidatePersistedOptimizerBenchmarkEvidenceRecord(record);
  Require(!validation.ok, "CEIC-051 placeholder/synthetic evidence was accepted");
  Require(HasDiagnostic(validation.diagnostics, "PLACEHOLDER_EPOCH"),
          "CEIC-051 placeholder epoch diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "SYNTHETIC_OR_PLACEHOLDER"),
          "CEIC-051 synthetic/placeholder diagnostic missing");
}

void DonorAndAuthorityDriftFailsClosed() {
  auto record = Record("embedded");
  record.donor_methodology.front().donor_reference_only = false;
  record.donor_methodology.front().donor_as_authority = true;
  record.donor_methodology.front()
      .uses_donor_storage_or_finality_for_scratchbird = true;
  record.authority.transaction_finality_authority = true;
  record.authority.visibility_authority = true;
  record.authority.parser_authority = true;
  const auto validation =
      opt::ValidatePersistedOptimizerBenchmarkEvidenceRecord(record);
  Require(!validation.ok, "CEIC-051 donor/authority drift was accepted");
  Require(HasDiagnostic(validation.diagnostics, "DONOR_AUTHORITY_DRIFT"),
          "CEIC-051 donor authority drift diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "FORBIDDEN_AUTHORITY"),
          "CEIC-051 forbidden authority diagnostic missing");
}

void LocalClusterAndExternalClusterClaimsFailClosed() {
  auto local = Record("cluster");
  local.cluster_mode = opt::OptimizerBenchmarkClusterMode::kLocalClusterEvidence;
  const auto local_validation =
      opt::ValidatePersistedOptimizerBenchmarkEvidenceRecord(local);
  Require(!local_validation.ok,
          "CEIC-051 local cluster evidence was accepted");
  Require(HasDiagnostic(local_validation.diagnostics, "LOCAL_CLUSTER_FORBIDDEN"),
          "CEIC-051 local cluster diagnostic missing");

  auto external = Record("embedded");
  external.cluster_mode =
      opt::OptimizerBenchmarkClusterMode::kExternalProviderDelegated;
  external.external_cluster_provider_id = "external-cluster-provider";
  external.cluster_claim_blocked = true;
  external.production_benchmark_clean_claim = false;
  const auto external_validation =
      opt::ValidatePersistedOptimizerBenchmarkEvidenceRecord(external);
  Require(external_validation.ok,
          "CEIC-051 claim-blocked external cluster delegation was rejected");
  Require(!external_validation.benchmark_clean_admissible,
          "CEIC-051 external cluster delegation must not become benchmark-clean");

  external.production_benchmark_clean_claim = true;
  const auto overclaim =
      opt::ValidatePersistedOptimizerBenchmarkEvidenceRecord(external);
  Require(!overclaim.ok,
          "CEIC-051 external cluster benchmark-clean claim was accepted");
  Require(HasDiagnostic(overclaim.diagnostics,
                        "EXTERNAL_CLUSTER_CLAIM_BLOCK_REQUIRED"),
          "CEIC-051 external cluster overclaim diagnostic missing");
}

void MissingSamplesAndCountersFailClosed() {
  auto record = Record("embedded");
  record.sample_groups.pop_back();
  record.sample_groups.front().p95_us = 120.0;
  record.sample_groups.front().cpu_user_us = 0;
  record.sample_groups.front().cpu_system_us = 0;
  record.sample_groups.front().memory_peak_bytes = 0;
  record.sample_groups.front().io_read_bytes = 0;
  record.sample_groups.front().io_write_bytes = 0;
  record.sample_groups.front().cache_hits = 0;
  record.sample_groups.front().cache_misses = 0;
  const auto validation =
      opt::ValidatePersistedOptimizerBenchmarkEvidenceRecord(record);
  Require(!validation.ok, "CEIC-051 incomplete sample evidence was accepted");
  Require(HasDiagnostic(validation.diagnostics, "COLD_WARM_REQUIRED"),
          "CEIC-051 cold/warm diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "PERCENTILE_MISMATCH"),
          "CEIC-051 percentile mismatch diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "CPU_COUNTER_MISSING"),
          "CEIC-051 CPU counter diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "MEMORY_COUNTER_MISSING"),
          "CEIC-051 memory counter diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "IO_COUNTER_MISSING"),
          "CEIC-051 IO counter diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "CACHE_COUNTER_MISSING"),
          "CEIC-051 cache counter diagnostic missing");
}

void RequiredRoutesRemainExplicit() {
  auto routes = RouteSet();
  routes.pop_back();
  const auto validation =
      opt::ValidatePersistedOptimizerBenchmarkEvidenceSet(
          routes, {"embedded", "ipc", "inet", "cli", "driver"});
  Require(!validation.ok, "CEIC-051 missing driver route was accepted");
  Require(HasDiagnostic(validation.diagnostics, "MISSING_ROUTE"),
          "CEIC-051 missing route diagnostic absent");
}

}  // namespace

int main() {
  PositiveRouteSetIsAdmissible();
  PlaceholderAndSyntheticEvidenceFailsClosed();
  DonorAndAuthorityDriftFailsClosed();
  LocalClusterAndExternalClusterClaimsFailClosed();
  MissingSamplesAndCountersFailClosed();
  RequiredRoutesRemainExplicit();
  std::cout << "ceic_051_persisted_optimizer_benchmark_evidence_schema_gate=pass\n";
  return EXIT_SUCCESS;
}
