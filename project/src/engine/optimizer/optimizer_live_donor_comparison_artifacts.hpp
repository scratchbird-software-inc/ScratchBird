// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "optimizer_benchmark_evidence_schema.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: CEIC_058_LIVE_DONOR_COMPARISON_ARTIFACTS
// Donor comparison artifacts preserve donor method/version/result facts for
// optimizer benchmark review only. They are evidence-only and cannot become
// ScratchBird execution correctness, transaction finality, visibility,
// authorization/security, recovery, parser, donor, WAL, benchmark-dominance,
// optimizer-plan, index-finality, provider-finality, cluster, or agent-action
// authority.
inline constexpr const char* kOptimizerLiveDonorComparisonSchemaId =
    "sb.optimizer.live_donor_comparison_artifacts.v1";
inline constexpr std::uint32_t kOptimizerLiveDonorComparisonSchemaMajor = 1;
inline constexpr std::uint32_t kOptimizerLiveDonorComparisonSchemaMinor = 0;
inline constexpr std::size_t kMinimumOptimizerDonorComparisonEngines = 24;

enum class OptimizerDonorComparisonStatus {
  kComparable,
  kNonComparable,
};

enum class OptimizerDonorComparisonRunMode {
  kLiveExternalRun,
  kArtifactOnlyEvidence,
  kNonComparableEvidence,
};

enum class OptimizerDonorComparisonClusterMode {
  kNoCluster,
  kExternalProviderDelegated,
  kLocalClusterEvidence,
};

struct OptimizerDonorComparisonAuthorityFlags {
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool authorization_security_authority = false;
  bool recovery_authority = false;
  bool parser_authority = false;
  bool donor_authority = false;
  bool donor_result_authority = false;
  bool donor_execution_authority = false;
  bool donor_storage_authority = false;
  bool wal_authority = false;
  bool benchmark_authority = false;
  bool benchmark_dominance_authority = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool provider_finality_authority = false;
  bool local_cluster_authority = false;
  bool cluster_authority = false;
  bool agent_action_authority = false;
};

struct OptimizerDonorTimingEvidence {
  double scratchbird_p50_us = 0.0;
  double scratchbird_p95_us = 0.0;
  double scratchbird_p99_us = 0.0;
  double donor_p50_us = 0.0;
  double donor_p95_us = 0.0;
  double donor_p99_us = 0.0;
};

struct OptimizerDonorComparisonArtifactRow {
  std::string row_id;
  std::string donor_engine;
  std::string donor_version;
  std::string donor_version_digest;
  std::string donor_specialty_class;
  std::string donor_native_method;
  OptimizerDonorComparisonRunMode run_mode =
      OptimizerDonorComparisonRunMode::kArtifactOnlyEvidence;
  bool live_external_engine_run_observed = false;
  std::string donor_run_id;
  std::string donor_execution_artifact_digest;

  std::string scratchbird_workload_id;
  std::string route_kind;
  std::string route_lane;
  std::string route_label;
  std::string dataset_schema_digest;
  std::string dataset_mapping_digest;
  std::string workload_query_mapping_digest;
  std::string route_equivalence_contract_hash;

  std::string scratchbird_result_contract_hash;
  std::string scratchbird_result_hash;
  std::string donor_result_hash;
  std::uint64_t scratchbird_result_row_count = 0;
  std::uint64_t donor_result_row_count = 0;

  OptimizerDonorComparisonStatus comparable_status =
      OptimizerDonorComparisonStatus::kComparable;
  std::vector<std::string> non_comparable_blockers;
  std::string timing_policy;
  std::string transaction_policy;
  OptimizerDonorTimingEvidence timing;

  std::string provenance_digest;
  std::string evidence_digest;
  std::string redaction_digest;
  std::string source_provenance;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::uint64_t statistics_epoch = 0;
  std::uint64_t donor_artifact_generation = 0;
  std::uint64_t dataset_mapping_generation = 0;
  std::uint64_t route_equivalence_generation = 0;
  std::uint64_t provider_generation = 0;
  std::uint64_t freshness_microseconds = 0;
  std::uint64_t max_freshness_microseconds = 0;
  bool trusted_provenance = false;
  bool fresh = false;
  bool redaction_applied = false;
  bool evidence_only = true;
  bool production_donor_comparison_claim = false;

  bool placeholder_runtime_evidence = false;
  bool synthetic_statistics = false;
  bool local_default_statistics = false;
  bool policy_default_statistics = false;

  bool donor_reference_only = true;
  bool donor_as_authority = false;
  bool donor_result_authority = false;
  bool donor_execution_authority = false;
  bool donor_storage_or_finality_substitution = false;
  bool donor_transaction_authority = false;
  bool donor_security_authority = false;
  bool donor_recovery_authority = false;
  bool donor_optimizer_plan_authority = false;
  bool benchmark_dominance_claimed = false;

  bool ceic_051_benchmark_evidence_attached = false;
  PersistedOptimizerBenchmarkEvidenceRecord benchmark_evidence;

  OptimizerDonorComparisonClusterMode cluster_mode =
      OptimizerDonorComparisonClusterMode::kNoCluster;
  std::string external_cluster_provider_id;
  bool cluster_claim_blocked = false;

  OptimizerDonorComparisonAuthorityFlags authority;
};

struct OptimizerDonorComparisonReport {
  std::string schema_id = kOptimizerLiveDonorComparisonSchemaId;
  std::uint32_t schema_version_major =
      kOptimizerLiveDonorComparisonSchemaMajor;
  std::uint32_t schema_version_minor =
      kOptimizerLiveDonorComparisonSchemaMinor;

  std::string report_id;
  std::string comparison_matrix_id;
  std::string dataset_schema_digest;
  std::string optimizer_profile;
  std::string provenance_digest;
  std::string evidence_digest;
  std::string redaction_digest;
  bool trusted_provenance = false;
  bool fresh = false;
  bool redaction_applied = false;
  bool evidence_only = true;
  bool production_donor_matrix_claim = false;
  bool benchmark_dominance_claimed = false;

  std::vector<std::string> required_donor_engines;
  std::vector<std::string> required_route_kinds;
  std::vector<OptimizerDonorComparisonArtifactRow> rows;

  OptimizerDonorComparisonClusterMode cluster_mode =
      OptimizerDonorComparisonClusterMode::kNoCluster;
  std::string external_cluster_provider_id;
  bool cluster_claim_blocked = false;

  OptimizerDonorComparisonAuthorityFlags authority;
};

struct OptimizerDonorComparisonValidation {
  bool ok = false;
  bool comparison_artifact_proven = false;
  std::string diagnostic_code;
  std::vector<std::string> missing_fields;
  std::vector<std::string> diagnostics;
};

std::vector<std::string> RequiredOptimizerDonorComparisonEngines();

OptimizerDonorComparisonValidation
ValidateOptimizerDonorComparisonArtifactRow(
    const OptimizerDonorComparisonArtifactRow& row);

OptimizerDonorComparisonValidation
ValidateOptimizerDonorComparisonReport(
    const OptimizerDonorComparisonReport& report);

}  // namespace scratchbird::engine::optimizer
