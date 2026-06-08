// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_live_donor_comparison_artifacts.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

bool Empty(std::string_view value) {
  return value.empty();
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.substr(0, prefix.size()) == prefix;
}

bool IsHashLike(std::string_view value) {
  return StartsWith(value, "sha256:");
}

bool PlaceholderDigest(std::string_view value) {
  return value.empty() || value == "result-contract-v1" ||
         value == "sha256:result-contract-v1" ||
         value == "sha256:placeholder" || value == "placeholder" ||
         value.find("placeholder") != std::string_view::npos ||
         value.find("dummy") != std::string_view::npos ||
         value.find("test-only") != std::string_view::npos;
}

bool Positive(double value) {
  return value > 0.0 && std::isfinite(value);
}

std::string RowPrefix(const OptimizerDonorComparisonArtifactRow& row) {
  if (!row.row_id.empty()) return row.row_id;
  if (!row.donor_engine.empty()) return row.donor_engine;
  return "unnamed_donor_comparison_row";
}

std::string ReportPrefix(const OptimizerDonorComparisonReport& report) {
  return report.report_id.empty() ? "unnamed_donor_comparison_report"
                                  : report.report_id;
}

void RequireField(OptimizerDonorComparisonValidation* validation,
                  bool present,
                  std::string field_name) {
  if (!present) validation->missing_fields.push_back(std::move(field_name));
}

void AddDiagnostic(OptimizerDonorComparisonValidation* validation,
                   std::string diagnostic) {
  validation->diagnostics.push_back(std::move(diagnostic));
}

void AddRowDiagnostic(OptimizerDonorComparisonValidation* validation,
                      const OptimizerDonorComparisonArtifactRow& row,
                      std::string diagnostic) {
  AddDiagnostic(validation, RowPrefix(row) + ":" + std::move(diagnostic));
}

bool HasAuthorityDrift(
    const OptimizerDonorComparisonAuthorityFlags& authority) {
  return authority.transaction_finality_authority ||
         authority.visibility_authority ||
         authority.authorization_security_authority ||
         authority.recovery_authority ||
         authority.parser_authority ||
         authority.donor_authority ||
         authority.donor_result_authority ||
         authority.donor_execution_authority ||
         authority.donor_storage_authority ||
         authority.wal_authority ||
         authority.benchmark_authority ||
         authority.benchmark_dominance_authority ||
         authority.optimizer_plan_authority ||
         authority.index_finality_authority ||
         authority.provider_finality_authority ||
         authority.local_cluster_authority ||
         authority.cluster_authority ||
         authority.agent_action_authority;
}

bool IsSupportedRoute(std::string_view route_kind) {
  return route_kind == "embedded" || route_kind == "ipc" ||
         route_kind == "inet" || route_kind == "cli" ||
         route_kind == "driver";
}

bool TimingValid(const OptimizerDonorTimingEvidence& timing) {
  return Positive(timing.scratchbird_p50_us) &&
         Positive(timing.scratchbird_p95_us) &&
         Positive(timing.scratchbird_p99_us) &&
         Positive(timing.donor_p50_us) &&
         Positive(timing.donor_p95_us) &&
         Positive(timing.donor_p99_us) &&
         timing.scratchbird_p50_us <= timing.scratchbird_p95_us &&
         timing.scratchbird_p95_us <= timing.scratchbird_p99_us &&
         timing.donor_p50_us <= timing.donor_p95_us &&
         timing.donor_p95_us <= timing.donor_p99_us;
}

bool BenchmarkEvidenceIsClaimBlockedExternal(
    const PersistedOptimizerBenchmarkEvidenceRecord& benchmark) {
  return benchmark.cluster_mode ==
             OptimizerBenchmarkClusterMode::kExternalProviderDelegated &&
         !benchmark.external_cluster_provider_id.empty() &&
         benchmark.cluster_claim_blocked &&
         !benchmark.production_benchmark_clean_claim;
}

bool BenchmarkEvidenceMatchesRow(
    const OptimizerDonorComparisonArtifactRow& row) {
  const auto& benchmark = row.benchmark_evidence;
  if (benchmark.route_kind != row.route_kind ||
      benchmark.route_lane != row.route_lane ||
      benchmark.route_label != row.route_label ||
      benchmark.dataset_schema_digest != row.dataset_schema_digest ||
      benchmark.result_contract_hash != row.scratchbird_result_contract_hash ||
      benchmark.result_hash != row.scratchbird_result_hash) {
    return false;
  }

  return std::any_of(
      benchmark.donor_methodology.begin(),
      benchmark.donor_methodology.end(),
      [&](const OptimizerBenchmarkDonorMethodologyEvidence& donor) {
        return donor.donor_engine == row.donor_engine &&
               donor.donor_version == row.donor_version &&
               donor.donor_native_method == row.donor_native_method &&
               donor.dataset_schema_mapping_digest ==
                   row.dataset_mapping_digest &&
               donor.workload_mapping_digest ==
                   row.workload_query_mapping_digest &&
               donor.route_equivalence_contract_hash ==
                   row.route_equivalence_contract_hash &&
               donor.donor_result_hash == row.donor_result_hash &&
               donor.comparable_status == "comparable" &&
               donor.donor_reference_only &&
               !donor.donor_as_authority &&
               !donor.uses_donor_storage_or_finality_for_scratchbird;
      });
}

bool RowClaimBlockedExternal(
    const OptimizerDonorComparisonArtifactRow& row) {
  return row.cluster_mode ==
             OptimizerDonorComparisonClusterMode::kExternalProviderDelegated &&
         !row.external_cluster_provider_id.empty() &&
         row.cluster_claim_blocked &&
         !row.production_donor_comparison_claim;
}

}  // namespace

std::vector<std::string> RequiredOptimizerDonorComparisonEngines() {
  return {
      "firebird",
      "postgresql",
      "mysql",
      "mariadb",
      "sqlite",
      "duckdb",
      "oracle",
      "sql_server",
      "db2",
      "cockroachdb",
      "yugabytedb",
      "clickhouse",
      "snowflake",
      "bigquery",
      "cassandra",
      "mongodb",
      "redis",
      "neo4j",
      "xtdb",
      "elasticsearch",
      "opensearch",
      "influxdb",
      "timescaledb",
      "dynamodb",
      "arangodb",
  };
}

OptimizerDonorComparisonValidation
ValidateOptimizerDonorComparisonArtifactRow(
    const OptimizerDonorComparisonArtifactRow& row) {
  OptimizerDonorComparisonValidation validation;

  RequireField(&validation, !Empty(row.row_id), "row_id");
  RequireField(&validation, !Empty(row.donor_engine), "donor_engine");
  RequireField(&validation, !Empty(row.donor_version), "donor_version");
  RequireField(&validation,
               IsHashLike(row.donor_version_digest) &&
                   !PlaceholderDigest(row.donor_version_digest),
               "donor_version_digest");
  RequireField(&validation,
               !Empty(row.donor_specialty_class),
               "donor_specialty_class");
  RequireField(&validation,
               !Empty(row.donor_native_method),
               "donor_native_method");
  RequireField(&validation, !Empty(row.donor_run_id), "donor_run_id");
  RequireField(&validation,
               IsHashLike(row.donor_execution_artifact_digest) &&
                   !PlaceholderDigest(row.donor_execution_artifact_digest),
               "donor_execution_artifact_digest");
  RequireField(&validation,
               !Empty(row.scratchbird_workload_id),
               "scratchbird_workload_id");
  RequireField(&validation, !Empty(row.route_kind), "route_kind");
  RequireField(&validation, !Empty(row.route_lane), "route_lane");
  RequireField(&validation, !Empty(row.route_label), "route_label");
  RequireField(&validation,
               IsHashLike(row.dataset_schema_digest) &&
                   !PlaceholderDigest(row.dataset_schema_digest),
               "dataset_schema_digest");
  RequireField(&validation,
               IsHashLike(row.dataset_mapping_digest) &&
                   !PlaceholderDigest(row.dataset_mapping_digest),
               "dataset_mapping_digest");
  RequireField(&validation,
               IsHashLike(row.workload_query_mapping_digest) &&
                   !PlaceholderDigest(row.workload_query_mapping_digest),
               "workload_query_mapping_digest");
  RequireField(&validation,
               IsHashLike(row.route_equivalence_contract_hash) &&
                   !PlaceholderDigest(row.route_equivalence_contract_hash),
               "route_equivalence_contract_hash");
  RequireField(&validation,
               IsHashLike(row.scratchbird_result_contract_hash) &&
                   !PlaceholderDigest(row.scratchbird_result_contract_hash),
               "scratchbird_result_contract_hash");
  RequireField(&validation,
               IsHashLike(row.scratchbird_result_hash) &&
                   !PlaceholderDigest(row.scratchbird_result_hash),
               "scratchbird_result_hash");
  RequireField(&validation, !Empty(row.timing_policy), "timing_policy");
  RequireField(&validation,
               !Empty(row.transaction_policy),
               "transaction_policy");
  RequireField(&validation,
               IsHashLike(row.provenance_digest) &&
                   !PlaceholderDigest(row.provenance_digest),
               "provenance_digest");
  RequireField(&validation,
               IsHashLike(row.evidence_digest) &&
                   !PlaceholderDigest(row.evidence_digest),
               "evidence_digest");
  RequireField(&validation,
               IsHashLike(row.redaction_digest) &&
                   !PlaceholderDigest(row.redaction_digest),
               "redaction_digest");
  RequireField(&validation, !Empty(row.source_provenance), "source_provenance");

  if (!IsSupportedRoute(row.route_kind)) {
    AddRowDiagnostic(&validation, row,
                     "SB_OPT_DONOR_COMPARE.ROUTE_UNSUPPORTED");
  }
  if (row.scratchbird_result_row_count == 0) {
    AddRowDiagnostic(&validation, row,
                     "SB_OPT_DONOR_COMPARE.SB_RESULT_ROW_COUNT_MISSING");
  }
  if (row.catalog_epoch <= 1 || row.security_epoch <= 1 ||
      row.redaction_epoch <= 1 || row.statistics_epoch <= 1 ||
      row.donor_artifact_generation <= 1 ||
      row.dataset_mapping_generation <= 1 ||
      row.route_equivalence_generation <= 1 ||
      row.provider_generation <= 1) {
    AddRowDiagnostic(&validation, row,
                     "SB_OPT_DONOR_COMPARE.PLACEHOLDER_EPOCH");
  }
  if (!row.trusted_provenance || !row.fresh || !row.redaction_applied ||
      !row.evidence_only || row.freshness_microseconds == 0 ||
      row.max_freshness_microseconds == 0 ||
      row.freshness_microseconds > row.max_freshness_microseconds) {
    AddRowDiagnostic(&validation, row,
                     "SB_OPT_DONOR_COMPARE.PROVENANCE_FRESHNESS_INVALID");
  }
  if (row.placeholder_runtime_evidence || row.synthetic_statistics ||
      row.local_default_statistics || row.policy_default_statistics) {
    AddRowDiagnostic(&validation, row,
                     "SB_OPT_DONOR_COMPARE.SYNTHETIC_OR_PLACEHOLDER");
  }
  if (HasAuthorityDrift(row.authority) || !row.donor_reference_only ||
      row.donor_as_authority || row.donor_result_authority ||
      row.donor_execution_authority ||
      row.donor_storage_or_finality_substitution ||
      row.donor_transaction_authority || row.donor_security_authority ||
      row.donor_recovery_authority || row.donor_optimizer_plan_authority ||
      row.benchmark_dominance_claimed) {
    AddRowDiagnostic(&validation, row,
                     "SB_OPT_DONOR_COMPARE.FORBIDDEN_AUTHORITY");
  }

  if (row.run_mode == OptimizerDonorComparisonRunMode::kLiveExternalRun &&
      !row.live_external_engine_run_observed) {
    AddRowDiagnostic(&validation, row,
                     "SB_OPT_DONOR_COMPARE.LIVE_RUN_NOT_OBSERVED");
  }
  if (row.run_mode == OptimizerDonorComparisonRunMode::kArtifactOnlyEvidence &&
      row.live_external_engine_run_observed) {
    AddRowDiagnostic(&validation, row,
                     "SB_OPT_DONOR_COMPARE.ARTIFACT_MODE_LIVE_OVERCLAIM");
  }
  if (row.comparable_status ==
      OptimizerDonorComparisonStatus::kComparable) {
    if (!IsHashLike(row.donor_result_hash) ||
        PlaceholderDigest(row.donor_result_hash) ||
        row.donor_result_row_count == 0 || !TimingValid(row.timing)) {
      AddRowDiagnostic(&validation, row,
                       "SB_OPT_DONOR_COMPARE.COMPARABLE_RESULT_MISSING");
    }
    if (!row.non_comparable_blockers.empty() ||
        row.run_mode ==
            OptimizerDonorComparisonRunMode::kNonComparableEvidence) {
      AddRowDiagnostic(&validation, row,
                       "SB_OPT_DONOR_COMPARE.COMPARABLE_BLOCKER_CONFLICT");
    }
    if (!row.ceic_051_benchmark_evidence_attached) {
      AddRowDiagnostic(&validation, row,
                       "SB_OPT_DONOR_COMPARE.CEIC051_EVIDENCE_REQUIRED");
    } else {
      const auto nested = ValidatePersistedOptimizerBenchmarkEvidenceRecord(
          row.benchmark_evidence);
      const bool claim_blocked_external =
          BenchmarkEvidenceIsClaimBlockedExternal(row.benchmark_evidence);
      if (!nested.ok ||
          (!nested.benchmark_clean_admissible && !claim_blocked_external)) {
        AddRowDiagnostic(&validation, row,
                         "SB_OPT_DONOR_COMPARE.CEIC051_EVIDENCE_INVALID");
        validation.missing_fields.insert(validation.missing_fields.end(),
                                         nested.missing_fields.begin(),
                                         nested.missing_fields.end());
        validation.diagnostics.insert(validation.diagnostics.end(),
                                      nested.diagnostics.begin(),
                                      nested.diagnostics.end());
      } else if (!BenchmarkEvidenceMatchesRow(row)) {
        AddRowDiagnostic(&validation, row,
                         "SB_OPT_DONOR_COMPARE.CEIC051_EVIDENCE_MISMATCH");
      }
    }
  } else {
    if (row.non_comparable_blockers.empty()) {
      AddRowDiagnostic(
          &validation,
          row,
          "SB_OPT_DONOR_COMPARE.NON_COMPARABLE_BLOCKER_REQUIRED");
    }
    if (row.run_mode !=
        OptimizerDonorComparisonRunMode::kNonComparableEvidence) {
      AddRowDiagnostic(&validation, row,
                       "SB_OPT_DONOR_COMPARE.NON_COMPARABLE_MODE_REQUIRED");
    }
  }

  if (row.cluster_mode ==
          OptimizerDonorComparisonClusterMode::kLocalClusterEvidence ||
      row.route_kind == "cluster") {
    AddRowDiagnostic(&validation, row,
                     "SB_OPT_DONOR_COMPARE.LOCAL_CLUSTER_FORBIDDEN");
  } else if (row.cluster_mode ==
             OptimizerDonorComparisonClusterMode::kExternalProviderDelegated) {
    if (row.external_cluster_provider_id.empty() ||
        !row.cluster_claim_blocked ||
        row.production_donor_comparison_claim) {
      AddRowDiagnostic(
          &validation,
          row,
          "SB_OPT_DONOR_COMPARE.EXTERNAL_CLUSTER_CLAIM_BLOCK_REQUIRED");
    }
    if (row.ceic_051_benchmark_evidence_attached &&
        !BenchmarkEvidenceIsClaimBlockedExternal(row.benchmark_evidence)) {
      AddRowDiagnostic(
          &validation,
          row,
          "SB_OPT_DONOR_COMPARE.EXTERNAL_CLUSTER_NESTED_CLAIM_BLOCK_REQUIRED");
    }
  }

  validation.ok = validation.missing_fields.empty() &&
                  validation.diagnostics.empty();
  validation.comparison_artifact_proven =
      validation.ok && row.production_donor_comparison_claim &&
      row.cluster_mode == OptimizerDonorComparisonClusterMode::kNoCluster;
  validation.diagnostic_code =
      validation.ok
          ? "SB_OPT_DONOR_COMPARE.ROW_OK"
          : (validation.missing_fields.empty()
                 ? "SB_OPT_DONOR_COMPARE.ROW_INVALID"
                 : "SB_OPT_DONOR_COMPARE.MISSING_REQUIRED_FIELD");
  return validation;
}

OptimizerDonorComparisonValidation ValidateOptimizerDonorComparisonReport(
    const OptimizerDonorComparisonReport& report) {
  OptimizerDonorComparisonValidation validation;
  const auto prefix = ReportPrefix(report);

  RequireField(&validation,
               report.schema_id == kOptimizerLiveDonorComparisonSchemaId,
               "schema_id");
  RequireField(&validation,
               report.schema_version_major ==
                   kOptimizerLiveDonorComparisonSchemaMajor,
               "schema_version_major");
  RequireField(&validation,
               report.schema_version_minor ==
                   kOptimizerLiveDonorComparisonSchemaMinor,
               "schema_version_minor");
  RequireField(&validation, !Empty(report.report_id), "report_id");
  RequireField(&validation,
               !Empty(report.comparison_matrix_id),
               "comparison_matrix_id");
  RequireField(&validation,
               IsHashLike(report.dataset_schema_digest) &&
                   !PlaceholderDigest(report.dataset_schema_digest),
               "dataset_schema_digest");
  RequireField(&validation,
               !Empty(report.optimizer_profile),
               "optimizer_profile");
  RequireField(&validation,
               IsHashLike(report.provenance_digest) &&
                   !PlaceholderDigest(report.provenance_digest),
               "provenance_digest");
  RequireField(&validation,
               IsHashLike(report.evidence_digest) &&
                   !PlaceholderDigest(report.evidence_digest),
               "evidence_digest");
  RequireField(&validation,
               IsHashLike(report.redaction_digest) &&
                   !PlaceholderDigest(report.redaction_digest),
               "redaction_digest");

  if (!report.trusted_provenance || !report.fresh ||
      !report.redaction_applied || !report.evidence_only) {
    AddDiagnostic(&validation,
                  prefix + ":SB_OPT_DONOR_COMPARE.REPORT_PROVENANCE_INVALID");
  }
  if (HasAuthorityDrift(report.authority) ||
      report.benchmark_dominance_claimed) {
    AddDiagnostic(&validation,
                  prefix + ":SB_OPT_DONOR_COMPARE.REPORT_FORBIDDEN_AUTHORITY");
  }
  if (report.required_donor_engines.size() <
      kMinimumOptimizerDonorComparisonEngines) {
    AddDiagnostic(&validation,
                  prefix + ":SB_OPT_DONOR_COMPARE.REQUIRED_DONOR_SET_TOO_SMALL");
  }
  if (report.required_route_kinds.empty()) {
    AddDiagnostic(&validation,
                  prefix + ":SB_OPT_DONOR_COMPARE.REQUIRED_ROUTES_MISSING");
  }
  if (report.rows.empty()) {
    AddDiagnostic(&validation,
                  prefix + ":SB_OPT_DONOR_COMPARE.ROWS_MISSING");
  }

  if (report.cluster_mode ==
      OptimizerDonorComparisonClusterMode::kLocalClusterEvidence) {
    AddDiagnostic(&validation,
                  prefix + ":SB_OPT_DONOR_COMPARE.LOCAL_CLUSTER_FORBIDDEN");
  } else if (report.cluster_mode ==
             OptimizerDonorComparisonClusterMode::kExternalProviderDelegated) {
    if (report.external_cluster_provider_id.empty() ||
        !report.cluster_claim_blocked ||
        report.production_donor_matrix_claim) {
      AddDiagnostic(
          &validation,
          prefix + ":SB_OPT_DONOR_COMPARE.EXTERNAL_CLUSTER_CLAIM_BLOCK_REQUIRED");
    }
  }

  std::set<std::string> seen_row_ids;
  std::set<std::string> proven_donors;
  std::set<std::string> proven_routes;
  bool saw_artifact_only = false;
  for (const auto& row : report.rows) {
    if (!row.row_id.empty() && !seen_row_ids.insert(row.row_id).second) {
      AddRowDiagnostic(&validation, row,
                       "SB_OPT_DONOR_COMPARE.DUPLICATE_ROW_ID");
    }
    if (!report.dataset_schema_digest.empty() &&
        row.dataset_schema_digest != report.dataset_schema_digest) {
      AddRowDiagnostic(&validation, row,
                       "SB_OPT_DONOR_COMPARE.REPORT_DATASET_MISMATCH");
    }
    if (row.run_mode ==
        OptimizerDonorComparisonRunMode::kArtifactOnlyEvidence) {
      saw_artifact_only = true;
    }

    const auto row_validation =
        ValidateOptimizerDonorComparisonArtifactRow(row);
    if (!row_validation.ok) {
      AddDiagnostic(&validation,
                    RowPrefix(row) + ":" + row_validation.diagnostic_code);
      validation.missing_fields.insert(
          validation.missing_fields.end(),
          row_validation.missing_fields.begin(),
          row_validation.missing_fields.end());
      validation.diagnostics.insert(validation.diagnostics.end(),
                                    row_validation.diagnostics.begin(),
                                    row_validation.diagnostics.end());
    }
    if (row_validation.comparison_artifact_proven) {
      proven_donors.insert(row.donor_engine);
      proven_routes.insert(row.route_kind);
    }
  }

  for (const auto& donor_engine : report.required_donor_engines) {
    if (donor_engine.empty()) {
      AddDiagnostic(&validation,
                    prefix + ":SB_OPT_DONOR_COMPARE.EMPTY_REQUIRED_DONOR");
    } else if (proven_donors.find(donor_engine) == proven_donors.end()) {
      AddDiagnostic(&validation,
                    donor_engine +
                        ":SB_OPT_DONOR_COMPARE.MISSING_REQUIRED_DONOR");
    }
  }
  for (const auto& route_kind : report.required_route_kinds) {
    if (proven_routes.find(route_kind) == proven_routes.end()) {
      AddDiagnostic(&validation,
                    route_kind +
                        ":SB_OPT_DONOR_COMPARE.MISSING_REQUIRED_ROUTE");
    }
  }
  if (proven_donors.size() < kMinimumOptimizerDonorComparisonEngines) {
    AddDiagnostic(&validation,
                  prefix + ":SB_OPT_DONOR_COMPARE.DONOR_BREADTH_INSUFFICIENT");
  }
  if (saw_artifact_only && !report.evidence_only) {
    AddDiagnostic(&validation,
                  prefix + ":SB_OPT_DONOR_COMPARE.ARTIFACT_ONLY_OVERCLAIM");
  }

  validation.ok = validation.missing_fields.empty() &&
                  validation.diagnostics.empty();
  validation.comparison_artifact_proven =
      validation.ok && report.production_donor_matrix_claim &&
      report.cluster_mode == OptimizerDonorComparisonClusterMode::kNoCluster;
  validation.diagnostic_code =
      validation.ok
          ? "SB_OPT_DONOR_COMPARE.REPORT_OK"
          : (validation.missing_fields.empty()
                 ? "SB_OPT_DONOR_COMPARE.REPORT_INVALID"
                 : "SB_OPT_DONOR_COMPARE.MISSING_REQUIRED_FIELD");
  return validation;
}

}  // namespace scratchbird::engine::optimizer
