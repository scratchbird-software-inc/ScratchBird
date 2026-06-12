// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "optimizer_benchmark_evidence_schema.hpp"
#include "optimizer_correctness_oracle.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: CEIC_056_OPTIMIZER_WORKLOAD_REGRESSION_BUDGET
// Workload regression budget records are optimizer proof-control evidence only.
// They do not authorize execution and are not transaction finality, visibility,
// authorization/security, recovery, parser, reference, WAL, benchmark,
// optimizer-plan, index-finality, provider-finality, cluster, or agent-action
// authority.
inline constexpr const char* kOptimizerWorkloadRegressionBudgetSchemaId =
    "sb.optimizer.workload_regression_budget.v1";
inline constexpr std::uint32_t
    kOptimizerWorkloadRegressionBudgetSchemaMajor = 1;
inline constexpr std::uint32_t
    kOptimizerWorkloadRegressionBudgetSchemaMinor = 0;

enum class OptimizerWorkloadRegressionClass {
  kOltpPointLookup,
  kOltpRangeLookup,
  kOlapScanAggregate,
  kJoin,
  kDmlLocatorMutation,
  kDocumentPath,
  kVectorExact,
  kVectorApproximate,
  kTextWand,
  kGraphTraversal,
  kMixedSqlNosqlFusion,
  kBulkIngest,
};

enum class OptimizerExpectedAccessClass {
  kRowUuidLookup,
  kScalarBtreeLookup,
  kScalarBtreeRange,
  kHashEquality,
  kCoveringIndex,
  kBitmapSummary,
  kBoundedTableScan,
  kColumnarZoneScan,
  kJoinPlan,
  kDmlRowLocator,
  kDocumentPathProbe,
  kVectorExactSearch,
  kVectorApproximateCandidates,
  kFullTextWandProbe,
  kGraphSeedExpansion,
  kSqlNosqlFusion,
  kBulkIngestAppend,
};

enum class OptimizerTableScanFallbackPolicy {
  kForbidden,
  kAllowedOnlyWithBoundedProof,
  kExpectedForWorkloadOnly,
};

enum class OptimizerWorkloadRegressionClusterMode {
  kNoCluster,
  kExternalProviderDelegated,
  kLocalClusterEvidence,
};

struct OptimizerWorkloadRegressionAuthorityFlags {
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool authorization_security_authority = false;
  bool recovery_authority = false;
  bool parser_authority = false;
  bool reference_authority = false;
  bool wal_authority = false;
  bool benchmark_authority = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool provider_finality_authority = false;
  bool local_cluster_authority = false;
  bool cluster_authority = false;
  bool agent_action_authority = false;
};

struct OptimizerWorkloadRegressionBudgetRecord {
  std::string schema_id = kOptimizerWorkloadRegressionBudgetSchemaId;
  std::uint32_t schema_version_major =
      kOptimizerWorkloadRegressionBudgetSchemaMajor;
  std::uint32_t schema_version_minor =
      kOptimizerWorkloadRegressionBudgetSchemaMinor;

  std::string budget_id;
  OptimizerWorkloadRegressionClass workload_class =
      OptimizerWorkloadRegressionClass::kOltpPointLookup;
  std::string route_kind;
  std::string route_lane;
  std::string route_label;
  std::string cold_or_warm_lane;

  OptimizerExpectedAccessClass expected_access_class =
      OptimizerExpectedAccessClass::kRowUuidLookup;
  OptimizerTableScanFallbackPolicy table_scan_fallback_policy =
      OptimizerTableScanFallbackPolicy::kForbidden;
  bool table_scan_fallback_used = false;
  bool table_scan_fallback_bounded = false;
  std::string table_scan_fallback_reason;

  std::string result_contract_hash;
  std::string result_hash;
  std::string logical_plan_hash;
  std::string physical_plan_hash;
  std::string sblr_digest;
  std::string dataset_schema_digest;
  std::string optimizer_profile;
  std::string workload_budget_profile;

  double baseline_p95_us = 0.0;
  double optimized_p95_us = 0.0;
  double baseline_p99_us = 0.0;
  double optimized_p99_us = 0.0;
  double max_regression_ratio = 0.0;
  double observed_regression_ratio = 0.0;

  bool spill_bound_bytes_present = false;
  bool unbounded_spill = false;
  std::uint64_t max_spill_bytes = 0;
  std::uint64_t observed_spill_bytes = 0;
  std::uint64_t memory_reservation_bytes = 0;
  std::string memory_reservation_digest;

  std::string metric_snapshot_digest;
  std::string statistics_snapshot_digest;
  std::string provenance_digest;
  std::string redaction_digest;
  std::uint64_t metric_snapshot_generation = 0;
  std::uint64_t statistics_epoch = 0;
  std::uint64_t feedback_generation = 0;
  std::uint64_t memory_feedback_generation = 0;
  std::uint64_t provider_generation = 0;
  std::uint64_t metric_freshness_microseconds = 0;
  std::uint64_t max_metric_freshness_microseconds = 0;
  std::uint64_t statistics_freshness_microseconds = 0;
  std::uint64_t max_statistics_freshness_microseconds = 0;

  bool metrics_trusted = false;
  bool metrics_fresh = false;
  bool statistics_trusted = false;
  bool statistics_fresh = false;
  bool trusted_provenance = false;
  bool redaction_applied = false;
  bool evidence_only = true;
  bool production_regression_budget_claim = false;
  bool benchmark_clean_claim = false;

  bool placeholder_runtime_evidence = false;
  bool synthetic_statistics = false;
  bool local_default_statistics = false;
  bool policy_default_statistics = false;

  bool exact_fallback_required = false;
  bool exact_fallback_proven = false;
  bool exact_recheck_required = true;
  bool exact_recheck_proven = false;
  bool exact_rerank_required = false;
  bool exact_rerank_proven = false;
  bool mga_recheck_required = true;
  bool mga_recheck_proven = false;
  bool security_recheck_required = true;
  bool security_recheck_proven = false;

  bool benchmark_evidence_attached = false;
  PersistedOptimizerBenchmarkEvidenceRecord benchmark_evidence;
  bool correctness_oracle_attached = false;
  OptimizerCorrectnessOracleCase correctness_oracle_case;

  bool reference_reference_only = true;
  bool reference_as_authority = false;
  bool uses_reference_storage_or_finality_for_scratchbird = false;

  OptimizerWorkloadRegressionClusterMode cluster_mode =
      OptimizerWorkloadRegressionClusterMode::kNoCluster;
  std::string external_cluster_provider_id;
  bool cluster_claim_blocked = false;

  OptimizerWorkloadRegressionAuthorityFlags authority;
};

struct OptimizerWorkloadRegressionBudgetValidation {
  bool ok = false;
  bool regression_budget_proven = false;
  std::string diagnostic_code;
  std::vector<std::string> missing_fields;
  std::vector<std::string> diagnostics;
};

const char* OptimizerWorkloadRegressionClassName(
    OptimizerWorkloadRegressionClass value);

const char* OptimizerExpectedAccessClassName(
    OptimizerExpectedAccessClass value);

std::vector<OptimizerWorkloadRegressionClass>
RequiredOptimizerWorkloadRegressionClasses();

OptimizerWorkloadRegressionBudgetValidation
ValidateOptimizerWorkloadRegressionBudgetRecord(
    const OptimizerWorkloadRegressionBudgetRecord& record);

OptimizerWorkloadRegressionBudgetValidation
ValidateOptimizerWorkloadRegressionBudgetSuite(
    const std::vector<OptimizerWorkloadRegressionBudgetRecord>& records,
    const std::vector<OptimizerWorkloadRegressionClass>& required_classes,
    const std::vector<std::string>& required_routes);

}  // namespace scratchbird::engine::optimizer
