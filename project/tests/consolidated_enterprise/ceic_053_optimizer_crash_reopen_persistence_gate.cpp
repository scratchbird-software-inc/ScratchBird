// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-053 focused validation for optimizer crash/reopen persistence evidence.
#include "optimizer_crash_reopen_persistence.hpp"

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

opt::OptimizerBenchmarkSampleGroupEvidence BenchmarkSample(std::string phase) {
  opt::OptimizerBenchmarkSampleGroupEvidence sample;
  sample.sample_group_id = "ceic053:" + phase;
  sample.cache_phase = phase;
  sample.latency_us_samples = {101.0, 111.0, 121.0, 131.0, 141.0};
  sample.p50_us = 121.0;
  sample.p95_us = 141.0;
  sample.p99_us = 141.0;
  sample.cpu_user_us = 2000;
  sample.cpu_system_us = 200;
  sample.memory_peak_bytes = 16 * 1024 * 1024;
  sample.memory_reserved_bytes = 20 * 1024 * 1024;
  sample.memory_released_bytes = 20 * 1024 * 1024;
  sample.io_read_bytes = 8192;
  sample.io_write_bytes = 8192;
  sample.io_read_ops = 2;
  sample.io_write_ops = 2;
  sample.cache_hits = 256;
  sample.cache_misses = 4;
  sample.page_cache_hits = 192;
  sample.page_cache_misses = 3;
  sample.estimated_rows = 10000.0;
  sample.actual_rows = 9980.0;
  sample.estimate_actual_ratio = 0.998;
  sample.skew_profile = "ceic053_tenant_zipf";
  sample.cardinality_profile = "ceic053_hll_hist_mcv";
  sample.metric_snapshot_digest = "sha256:ceic053-metric-" + phase;
  sample.profiler_digest = "sha256:ceic053-profiler-" + phase;
  sample.cold_cache_reset_proven = phase == "cold";
  sample.warm_cache_prepared_proven = phase == "warm";
  return sample;
}

opt::OptimizerBenchmarkDonorMethodologyEvidence BenchmarkDonor() {
  opt::OptimizerBenchmarkDonorMethodologyEvidence donor;
  donor.donor_engine = "postgresql";
  donor.donor_version = "ceic053-reference";
  donor.donor_native_method = "donor_native_reference_only";
  donor.comparable_status = "comparable";
  donor.dataset_schema_mapping_digest = "sha256:ceic053-donor-dataset";
  donor.workload_mapping_digest = "sha256:ceic053-donor-workload";
  donor.route_equivalence_contract_hash = "sha256:ceic053-donor-route";
  donor.donor_result_hash = "sha256:ceic053-donor-result";
  donor.donor_transaction_policy =
      "reference_only_scratchbird_mga_not_substituted";
  donor.donor_timing_policy = "reference_methodology_only";
  donor.donor_reference_only = true;
  return donor;
}

opt::PersistedOptimizerBenchmarkEvidenceRecord BenchmarkRecord() {
  opt::PersistedOptimizerBenchmarkEvidenceRecord record;
  record.artifact_uuid = "ceic053-benchmark-artifact";
  record.route_kind = "embedded";
  record.route_lane = "ceic053/crash_reopen/embedded";
  record.route_label = "ceic053/optimizer_crash_reopen";
  record.dataset_schema_uuid = "ceic053-dataset-schema-uuid";
  record.dataset_schema_digest = "sha256:ceic053-dataset";
  record.dataset_schema_version = "ceic053-v1";
  record.sblr_digest = "sha256:ceic053-sblr";
  record.logical_plan_hash = "sha256:ceic053-logical";
  record.physical_plan_hash = "sha256:ceic053-physical";
  record.plan_cache_key_hash = "sha256:ceic053-plan-cache";
  record.result_contract_hash = "sha256:ceic053-result-contract";
  record.result_hash = "sha256:ceic053-result";
  record.result_row_count = 777;
  record.optimizer_profile = "enterprise";
  record.optimizer_toggles = {"property_frontier", "crash_reopen_gate",
                              "governed_memory_feedback"};
  record.optimizer_toggles_digest = "sha256:ceic053-toggles";
  record.benchmark_profile = "ceic053-crash-reopen";
  record.benchmark_run_id = "ceic053-run";
  record.runner_id = "ceic053-runner";
  record.catalog_epoch = 5301;
  record.security_epoch = 5302;
  record.redaction_epoch = 5303;
  record.statistics_epoch = 5304;
  record.feedback_generation = 5305;
  record.memory_feedback_generation = 5306;
  record.provider_generation = 5307;
  record.provenance_digest = "sha256:ceic053-provenance";
  record.evidence_digest = "sha256:ceic053-evidence";
  record.redaction_digest = "sha256:ceic053-redaction";
  record.retention_class = "benchmark_evidence_redacted";
  record.freshness_microseconds = 1000;
  record.max_freshness_microseconds = 60000000;
  record.trusted_provenance = true;
  record.fresh = true;
  record.redaction_applied = true;
  record.production_benchmark_clean_claim = false;
  record.sample_groups = {BenchmarkSample("cold"), BenchmarkSample("warm")};
  record.donor_methodology = {BenchmarkDonor()};
  return record;
}

opt::OptimizerCrashReopenPersistenceRecord Record(
    opt::OptimizerPersistentArtifactKind artifact,
    opt::OptimizerCrashPoint point,
    opt::OptimizerReopenDecision decision) {
  const std::string artifact_name =
      opt::OptimizerPersistentArtifactKindName(artifact);
  const std::string point_name = opt::OptimizerCrashPointName(point);

  opt::OptimizerCrashReopenPersistenceRecord record;
  record.record_id = "ceic053-" + artifact_name + "-" + point_name;
  record.artifact_kind = artifact;
  record.crash_point = point;
  record.reopen_decision = decision;
  record.artifact_uuid = "artifact-uuid-" + artifact_name;
  record.artifact_generation_uuid = "generation-uuid-" + artifact_name;
  record.artifact_generation = 5301;
  record.persisted_generation = 5302;
  record.schema_digest = "sha256:ceic053-schema-" + artifact_name;
  record.payload_checksum = "sha256:ceic053-checksum-" + artifact_name;
  record.pre_crash_payload_digest = "sha256:ceic053-payload-" + artifact_name;
  record.post_reopen_payload_digest = record.pre_crash_payload_digest;
  record.route_kind = "embedded";
  record.route_label = "ceic053/optimizer_crash_reopen/" + artifact_name;
  record.result_contract_hash = "sha256:ceic053-result-contract";
  record.logical_plan_hash = "sha256:ceic053-logical-" + artifact_name;
  record.physical_plan_hash = "sha256:ceic053-physical-" + artifact_name;
  record.plan_cache_key_hash = "sha256:ceic053-plan-cache-" + artifact_name;
  record.statistics_snapshot_digest = "sha256:ceic053-stats-" + artifact_name;
  record.feedback_digest = "sha256:ceic053-feedback-" + artifact_name;
  record.memory_feedback_digest =
      "sha256:ceic053-memory-feedback-" + artifact_name;
  record.runtime_route_evidence_digest =
      "sha256:ceic053-runtime-route-" + artifact_name;
  record.catalog_epoch = 5311;
  record.security_epoch = 5312;
  record.redaction_epoch = 5313;
  record.statistics_epoch = 5314;
  record.feedback_generation = 5315;
  record.memory_feedback_generation = 5316;
  record.provider_generation = 5317;
  record.catalog_epoch_compatible = true;
  record.security_epoch_compatible = true;
  record.redaction_epoch_compatible = true;
  record.statistics_epoch_compatible = true;
  record.redaction_applied = true;
  record.trusted_provenance = true;
  record.fresh_after_reopen = true;
  record.reload_attempted = true;
  record.decision_diagnostic_code =
      "SB_OPT_CRASH_REOPEN." + std::string(opt::OptimizerReopenDecisionName(decision));
  record.checksum_valid = decision != opt::OptimizerReopenDecision::kQuarantined;
  record.mga_inventory.transaction_inventory_uuid =
      "ceic053-transaction-inventory";
  record.mga_inventory.transaction_uuid = "ceic053-transaction";
  record.mga_inventory.transaction_number = 530001;
  record.mga_inventory.inventory_generation = 5320;
  record.mga_inventory.inventory_snapshot_digest =
      "sha256:ceic053-inventory";
  record.mga_inventory.transaction_state_digest =
      "sha256:ceic053-transaction-state";
  record.mga_inventory.visibility_horizon_digest =
      "sha256:ceic053-horizon";
  record.mga_inventory.inventory_present = true;
  record.mga_inventory.validated_after_reopen = true;
  record.mga_inventory.checksum_valid = true;
  record.mga_inventory.generation_matches_artifact = true;

  switch (decision) {
    case opt::OptimizerReopenDecision::kReloadAccepted:
      record.durable_storage_write_completed = true;
      record.fsync_evidence_present = true;
      record.publish_generation_visible = true;
      record.mga_inventory.transaction_committed = true;
      break;
    case opt::OptimizerReopenDecision::kReloadRefused:
      record.refusal_diagnostic_code =
          "SB_OPT_CRASH_REOPEN.RELOAD_REFUSED";
      break;
    case opt::OptimizerReopenDecision::kRecoveredRebuilt:
      record.durable_storage_write_completed = true;
      record.fsync_evidence_present = true;
      record.replay_performed = true;
      record.recovery_rebuild_digest =
          "sha256:ceic053-rebuild-" + artifact_name;
      record.mga_inventory.transaction_committed = true;
      break;
    case opt::OptimizerReopenDecision::kQuarantined:
      record.refusal_diagnostic_code =
          "SB_OPT_CRASH_REOPEN.QUARANTINED";
      record.quarantine_digest = "sha256:ceic053-quarantine-" + artifact_name;
      record.checksum_valid = false;
      break;
  }

  switch (point) {
    case opt::OptimizerCrashPoint::kBeforePersist:
      record.durable_storage_write_completed = false;
      record.fsync_evidence_present = false;
      record.publish_generation_visible = false;
      record.mga_inventory.transaction_committed = false;
      break;
    case opt::OptimizerCrashPoint::kDuringWrite:
      record.partial_write_detected = true;
      record.durable_storage_write_completed = false;
      record.fsync_evidence_present = false;
      record.mga_inventory.transaction_committed = false;
      break;
    case opt::OptimizerCrashPoint::kAfterFsyncBeforeCommit:
      record.durable_storage_write_completed = true;
      record.fsync_evidence_present = true;
      record.mga_inventory.transaction_committed = false;
      break;
    case opt::OptimizerCrashPoint::kAfterCommitBeforePublish:
      record.durable_storage_write_completed = true;
      record.fsync_evidence_present = true;
      record.publish_generation_visible = false;
      record.mga_inventory.transaction_committed = true;
      break;
    case opt::OptimizerCrashPoint::kAfterPublish:
      record.durable_storage_write_completed = true;
      record.fsync_evidence_present = true;
      record.publish_generation_visible = true;
      record.mga_inventory.transaction_committed = true;
      break;
    case opt::OptimizerCrashPoint::kDuringReopen:
      record.durable_storage_write_completed = true;
      record.fsync_evidence_present = true;
      record.publish_generation_visible = true;
      record.mga_inventory.transaction_committed = true;
      break;
    case opt::OptimizerCrashPoint::kAfterReplayRefusal:
      record.replay_performed = true;
      record.replay_refusal_diagnostic_code =
          "SB_OPT_CRASH_REOPEN.REPLAY_REFUSED";
      break;
  }

  if (artifact == opt::OptimizerPersistentArtifactKind::kBenchmarkEvidence) {
    record.has_benchmark_record = true;
    record.benchmark_record = BenchmarkRecord();
  }
  return record;
}

std::vector<opt::OptimizerCrashReopenPersistenceRecord> Matrix() {
  return {
      Record(opt::OptimizerPersistentArtifactKind::kStatisticsSnapshot,
             opt::OptimizerCrashPoint::kBeforePersist,
             opt::OptimizerReopenDecision::kReloadRefused),
      Record(opt::OptimizerPersistentArtifactKind::kPlanCache,
             opt::OptimizerCrashPoint::kDuringWrite,
             opt::OptimizerReopenDecision::kQuarantined),
      Record(opt::OptimizerPersistentArtifactKind::kAdaptiveCardinalityFeedback,
             opt::OptimizerCrashPoint::kAfterFsyncBeforeCommit,
             opt::OptimizerReopenDecision::kReloadRefused),
      Record(opt::OptimizerPersistentArtifactKind::kMemoryFeedback,
             opt::OptimizerCrashPoint::kAfterCommitBeforePublish,
             opt::OptimizerReopenDecision::kRecoveredRebuilt),
      Record(opt::OptimizerPersistentArtifactKind::kBenchmarkEvidence,
             opt::OptimizerCrashPoint::kAfterPublish,
             opt::OptimizerReopenDecision::kReloadAccepted),
      Record(opt::OptimizerPersistentArtifactKind::kRuntimeRouteEvidence,
             opt::OptimizerCrashPoint::kDuringReopen,
             opt::OptimizerReopenDecision::kQuarantined),
      Record(opt::OptimizerPersistentArtifactKind::kPlanCache,
             opt::OptimizerCrashPoint::kAfterReplayRefusal,
             opt::OptimizerReopenDecision::kReloadRefused),
  };
}

void PositiveMatrixPasses() {
  const auto validation = opt::ValidateOptimizerCrashReopenPersistenceMatrix(
      Matrix(), opt::RequiredOptimizerCrashReopenArtifactKinds(),
      opt::RequiredOptimizerCrashPoints());
  Require(validation.ok, "CEIC-053 valid crash/reopen matrix rejected");
  Require(validation.crash_reopen_proven,
          "CEIC-053 valid crash/reopen matrix not marked proven");
}

void AcceptedReloadRequiresCommittedMgaInventory() {
  auto record = Record(opt::OptimizerPersistentArtifactKind::kPlanCache,
                       opt::OptimizerCrashPoint::kAfterPublish,
                       opt::OptimizerReopenDecision::kReloadAccepted);
  record.mga_inventory.transaction_committed = false;
  const auto validation =
      opt::ValidateOptimizerCrashReopenPersistenceRecord(record);
  Require(!validation.ok,
          "CEIC-053 accepted reload without committed MGA was accepted");
  Require(HasDiagnostic(validation.diagnostics,
                        "MGA_COMMITTED_INVENTORY_REQUIRED"),
          "CEIC-053 committed MGA diagnostic missing");
}

void PartialCorruptPlaceholderEvidenceFailsClosed() {
  auto record = Record(opt::OptimizerPersistentArtifactKind::kRuntimeRouteEvidence,
                       opt::OptimizerCrashPoint::kAfterPublish,
                       opt::OptimizerReopenDecision::kReloadAccepted);
  record.partial_write_detected = true;
  record.post_reopen_payload_digest = "sha256:different-payload";
  record.result_contract_hash = "result-contract-v1";
  record.catalog_epoch = 1;
  record.placeholder_runtime_evidence = true;
  record.synthetic_only_artifact = true;
  const auto validation =
      opt::ValidateOptimizerCrashReopenPersistenceRecord(record);
  Require(!validation.ok,
          "CEIC-053 corrupt placeholder artifact was accepted");
  Require(HasDiagnostic(validation.diagnostics, "PLACEHOLDER_EPOCH"),
          "CEIC-053 placeholder epoch diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "SYNTHETIC_OR_PLACEHOLDER"),
          "CEIC-053 placeholder artifact diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics,
                        "RELOAD_ACCEPTANCE_PROOF_MISSING"),
          "CEIC-053 reload acceptance diagnostic missing");
}

void CrashPointSemanticsFailClosed() {
  auto before_commit =
      Record(opt::OptimizerPersistentArtifactKind::kAdaptiveCardinalityFeedback,
             opt::OptimizerCrashPoint::kAfterFsyncBeforeCommit,
             opt::OptimizerReopenDecision::kReloadAccepted);
  before_commit.mga_inventory.transaction_committed = true;
  const auto validation =
      opt::ValidateOptimizerCrashReopenPersistenceRecord(before_commit);
  Require(!validation.ok,
          "CEIC-053 fsync-before-commit accepted reload was accepted");
  Require(HasDiagnostic(validation.diagnostics,
                        "FSYNC_BEFORE_COMMIT_ACCEPTED"),
          "CEIC-053 fsync-before-commit diagnostic missing");
}

void BenchmarkEvidenceMustUseCeic051Schema() {
  auto record = Record(opt::OptimizerPersistentArtifactKind::kBenchmarkEvidence,
                       opt::OptimizerCrashPoint::kAfterPublish,
                       opt::OptimizerReopenDecision::kReloadAccepted);
  record.benchmark_record.result_contract_hash = "result-contract-v1";
  record.benchmark_record.catalog_epoch = 1;
  const auto validation =
      opt::ValidateOptimizerCrashReopenPersistenceRecord(record);
  Require(!validation.ok,
          "CEIC-053 invalid nested benchmark artifact was accepted");
  Require(HasDiagnostic(validation.diagnostics, "BENCHMARK_RECORD_INVALID"),
          "CEIC-053 nested benchmark diagnostic missing");
}

void AuthorityDonorClusterAndBenchmarkOverclaimsFailClosed() {
  auto record = Record(opt::OptimizerPersistentArtifactKind::kPlanCache,
                       opt::OptimizerCrashPoint::kAfterPublish,
                       opt::OptimizerReopenDecision::kReloadAccepted);
  record.authority.transaction_finality_authority = true;
  record.authority.parser_authority = true;
  record.donor_reference_only = false;
  record.donor_as_authority = true;
  record.uses_donor_storage_or_finality_for_scratchbird = true;
  record.production_benchmark_clean_claim = true;
  const auto drift =
      opt::ValidateOptimizerCrashReopenPersistenceRecord(record);
  Require(!drift.ok, "CEIC-053 authority/donor overclaim was accepted");
  Require(HasDiagnostic(drift.diagnostics, "FORBIDDEN_AUTHORITY"),
          "CEIC-053 forbidden authority diagnostic missing");
  Require(HasDiagnostic(drift.diagnostics, "DONOR_AUTHORITY_DRIFT"),
          "CEIC-053 donor diagnostic missing");
  Require(HasDiagnostic(drift.diagnostics, "BENCHMARK_CLEAN_OVERCLAIM"),
          "CEIC-053 benchmark overclaim diagnostic missing");

  auto local_cluster = record;
  local_cluster.authority = {};
  local_cluster.donor_reference_only = true;
  local_cluster.donor_as_authority = false;
  local_cluster.uses_donor_storage_or_finality_for_scratchbird = false;
  local_cluster.production_benchmark_clean_claim = false;
  local_cluster.route_kind = "cluster";
  local_cluster.cluster_mode =
      opt::OptimizerCrashReopenClusterMode::kLocalClusterEvidence;
  const auto local_validation =
      opt::ValidateOptimizerCrashReopenPersistenceRecord(local_cluster);
  Require(!local_validation.ok,
          "CEIC-053 local cluster crash evidence was accepted");
  Require(HasDiagnostic(local_validation.diagnostics,
                        "LOCAL_CLUSTER_FORBIDDEN"),
          "CEIC-053 local cluster diagnostic missing");

  auto external_cluster =
      Record(opt::OptimizerPersistentArtifactKind::kRuntimeRouteEvidence,
             opt::OptimizerCrashPoint::kAfterPublish,
             opt::OptimizerReopenDecision::kReloadAccepted);
  external_cluster.cluster_mode =
      opt::OptimizerCrashReopenClusterMode::kExternalProviderDelegated;
  external_cluster.external_cluster_provider_id = "external-cluster-provider";
  external_cluster.cluster_claim_blocked = true;
  const auto external_validation =
      opt::ValidateOptimizerCrashReopenPersistenceRecord(external_cluster);
  Require(external_validation.ok,
          "CEIC-053 claim-blocked external cluster delegation was rejected");
  Require(!external_validation.crash_reopen_proven,
          "CEIC-053 external cluster delegation must not close local proof");

  external_cluster.cluster_claim_blocked = false;
  const auto external_overclaim =
      opt::ValidateOptimizerCrashReopenPersistenceRecord(external_cluster);
  Require(!external_overclaim.ok,
          "CEIC-053 unblocked external cluster delegation was accepted");
  Require(HasDiagnostic(external_overclaim.diagnostics,
                        "EXTERNAL_CLUSTER_CLAIM_BLOCK_REQUIRED"),
          "CEIC-053 external cluster diagnostic missing");
}

void MatrixGapsFailClosed() {
  auto matrix = Matrix();
  matrix.pop_back();
  matrix.erase(matrix.begin() + 3);
  const auto validation = opt::ValidateOptimizerCrashReopenPersistenceMatrix(
      matrix, opt::RequiredOptimizerCrashReopenArtifactKinds(),
      opt::RequiredOptimizerCrashPoints());
  Require(!validation.ok, "CEIC-053 incomplete matrix was accepted");
  Require(HasDiagnostic(validation.diagnostics, "MISSING_CRASH_POINT"),
          "CEIC-053 missing crash point diagnostic missing");
  Require(HasDiagnostic(validation.diagnostics, "MISSING_REOPEN_DECISION"),
          "CEIC-053 missing decision diagnostic missing");
}

}  // namespace

int main() {
  PositiveMatrixPasses();
  AcceptedReloadRequiresCommittedMgaInventory();
  PartialCorruptPlaceholderEvidenceFailsClosed();
  CrashPointSemanticsFailClosed();
  BenchmarkEvidenceMustUseCeic051Schema();
  AuthorityDonorClusterAndBenchmarkOverclaimsFailClosed();
  MatrixGapsFailClosed();
  std::cout << "ceic_053_optimizer_crash_reopen_persistence_gate=pass\n";
  return EXIT_SUCCESS;
}
