// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_workload_regression_budget.hpp"

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

bool IsPlaceholderResultContract(std::string_view value) {
  return value.empty() || value == "result-contract-v1" ||
         value == "sha256:result-contract-v1";
}

bool Positive(double value) {
  return value > 0.0 && std::isfinite(value);
}

bool SameMetric(double lhs, double rhs) {
  return std::abs(lhs - rhs) <= 0.0001;
}

std::string RecordPrefix(
    const OptimizerWorkloadRegressionBudgetRecord& record) {
  if (!record.budget_id.empty()) return record.budget_id;
  if (!record.route_lane.empty()) return record.route_lane;
  return OptimizerWorkloadRegressionClassName(record.workload_class);
}

void AddDiagnostic(OptimizerWorkloadRegressionBudgetValidation* validation,
                   const OptimizerWorkloadRegressionBudgetRecord& record,
                   std::string diagnostic) {
  validation->diagnostics.push_back(RecordPrefix(record) + ":" +
                                    std::move(diagnostic));
}

void RequireField(OptimizerWorkloadRegressionBudgetValidation* validation,
                  bool present,
                  std::string field_name) {
  if (!present) validation->missing_fields.push_back(std::move(field_name));
}

bool HasAuthorityDrift(
    const OptimizerWorkloadRegressionAuthorityFlags& authority) {
  return authority.transaction_finality_authority ||
         authority.visibility_authority ||
         authority.authorization_security_authority ||
         authority.recovery_authority ||
         authority.parser_authority ||
         authority.reference_authority ||
         authority.wal_authority ||
         authority.benchmark_authority ||
         authority.optimizer_plan_authority ||
         authority.index_finality_authority ||
         authority.provider_finality_authority ||
         authority.local_cluster_authority ||
         authority.cluster_authority ||
         authority.agent_action_authority;
}

bool IsDriverVisibleRoute(std::string_view route) {
  return route == "embedded" || route == "ipc" || route == "inet" ||
         route == "cli" || route == "driver";
}

bool ClassRequiresExactFallback(OptimizerWorkloadRegressionClass value) {
  return value == OptimizerWorkloadRegressionClass::kVectorApproximate ||
         value == OptimizerWorkloadRegressionClass::kTextWand ||
         value == OptimizerWorkloadRegressionClass::kMixedSqlNosqlFusion;
}

bool ClassRequiresExactRecheck(OptimizerWorkloadRegressionClass value) {
  return value == OptimizerWorkloadRegressionClass::kDmlLocatorMutation ||
         value == OptimizerWorkloadRegressionClass::kDocumentPath ||
         value == OptimizerWorkloadRegressionClass::kVectorApproximate ||
         value == OptimizerWorkloadRegressionClass::kTextWand ||
         value == OptimizerWorkloadRegressionClass::kGraphTraversal ||
         value == OptimizerWorkloadRegressionClass::kMixedSqlNosqlFusion;
}

bool ClassRequiresExactRerank(OptimizerWorkloadRegressionClass value) {
  return value == OptimizerWorkloadRegressionClass::kVectorApproximate ||
         value == OptimizerWorkloadRegressionClass::kTextWand ||
         value == OptimizerWorkloadRegressionClass::kMixedSqlNosqlFusion;
}

bool IsExpectedTableScanWorkload(OptimizerWorkloadRegressionClass value) {
  return value == OptimizerWorkloadRegressionClass::kOlapScanAggregate ||
         value == OptimizerWorkloadRegressionClass::kBulkIngest;
}

bool AccessMatchesClass(OptimizerWorkloadRegressionClass workload_class,
                        OptimizerExpectedAccessClass access_class) {
  switch (workload_class) {
    case OptimizerWorkloadRegressionClass::kOltpPointLookup:
      return access_class == OptimizerExpectedAccessClass::kRowUuidLookup ||
             access_class == OptimizerExpectedAccessClass::kScalarBtreeLookup ||
             access_class == OptimizerExpectedAccessClass::kHashEquality ||
             access_class == OptimizerExpectedAccessClass::kCoveringIndex;
    case OptimizerWorkloadRegressionClass::kOltpRangeLookup:
      return access_class == OptimizerExpectedAccessClass::kScalarBtreeRange ||
             access_class == OptimizerExpectedAccessClass::kCoveringIndex;
    case OptimizerWorkloadRegressionClass::kOlapScanAggregate:
      return access_class == OptimizerExpectedAccessClass::kBoundedTableScan ||
             access_class == OptimizerExpectedAccessClass::kColumnarZoneScan ||
             access_class == OptimizerExpectedAccessClass::kBitmapSummary;
    case OptimizerWorkloadRegressionClass::kJoin:
      return access_class == OptimizerExpectedAccessClass::kJoinPlan;
    case OptimizerWorkloadRegressionClass::kDmlLocatorMutation:
      return access_class == OptimizerExpectedAccessClass::kDmlRowLocator;
    case OptimizerWorkloadRegressionClass::kDocumentPath:
      return access_class == OptimizerExpectedAccessClass::kDocumentPathProbe;
    case OptimizerWorkloadRegressionClass::kVectorExact:
      return access_class == OptimizerExpectedAccessClass::kVectorExactSearch;
    case OptimizerWorkloadRegressionClass::kVectorApproximate:
      return access_class ==
             OptimizerExpectedAccessClass::kVectorApproximateCandidates;
    case OptimizerWorkloadRegressionClass::kTextWand:
      return access_class == OptimizerExpectedAccessClass::kFullTextWandProbe;
    case OptimizerWorkloadRegressionClass::kGraphTraversal:
      return access_class == OptimizerExpectedAccessClass::kGraphSeedExpansion;
    case OptimizerWorkloadRegressionClass::kMixedSqlNosqlFusion:
      return access_class == OptimizerExpectedAccessClass::kSqlNosqlFusion;
    case OptimizerWorkloadRegressionClass::kBulkIngest:
      return access_class == OptimizerExpectedAccessClass::kBulkIngestAppend ||
             access_class == OptimizerExpectedAccessClass::kBoundedTableScan;
  }
  return false;
}

bool BenchmarkEvidenceMatchesBudget(
    const OptimizerWorkloadRegressionBudgetRecord& record) {
  return record.benchmark_evidence.result_contract_hash ==
             record.result_contract_hash &&
         record.benchmark_evidence.result_hash == record.result_hash &&
         record.benchmark_evidence.route_kind == record.route_kind &&
         record.benchmark_evidence.route_lane == record.route_lane &&
         record.benchmark_evidence.logical_plan_hash ==
             record.logical_plan_hash &&
         record.benchmark_evidence.physical_plan_hash ==
             record.physical_plan_hash;
}

bool CorrectnessEvidenceMatchesBudget(
    const OptimizerWorkloadRegressionBudgetRecord& record) {
  return record.correctness_oracle_case.optimized.result_contract_hash ==
             record.result_contract_hash &&
         record.correctness_oracle_case.optimized.result_hash ==
             record.result_hash &&
         record.correctness_oracle_case.route_kind == record.route_kind &&
         record.correctness_oracle_case.route_label == record.route_label &&
         record.correctness_oracle_case.logical_plan_hash ==
             record.logical_plan_hash &&
         record.correctness_oracle_case.optimized_plan_hash ==
             record.physical_plan_hash;
}

}  // namespace

const char* OptimizerWorkloadRegressionClassName(
    OptimizerWorkloadRegressionClass value) {
  switch (value) {
    case OptimizerWorkloadRegressionClass::kOltpPointLookup:
      return "oltp_point_lookup";
    case OptimizerWorkloadRegressionClass::kOltpRangeLookup:
      return "oltp_range_lookup";
    case OptimizerWorkloadRegressionClass::kOlapScanAggregate:
      return "olap_scan_aggregate";
    case OptimizerWorkloadRegressionClass::kJoin:
      return "join";
    case OptimizerWorkloadRegressionClass::kDmlLocatorMutation:
      return "dml_locator_mutation";
    case OptimizerWorkloadRegressionClass::kDocumentPath:
      return "document_path";
    case OptimizerWorkloadRegressionClass::kVectorExact:
      return "vector_exact";
    case OptimizerWorkloadRegressionClass::kVectorApproximate:
      return "vector_approximate";
    case OptimizerWorkloadRegressionClass::kTextWand:
      return "text_wand";
    case OptimizerWorkloadRegressionClass::kGraphTraversal:
      return "graph_traversal";
    case OptimizerWorkloadRegressionClass::kMixedSqlNosqlFusion:
      return "mixed_sql_nosql_fusion";
    case OptimizerWorkloadRegressionClass::kBulkIngest:
      return "bulk_ingest";
  }
  return "unknown";
}

const char* OptimizerExpectedAccessClassName(
    OptimizerExpectedAccessClass value) {
  switch (value) {
    case OptimizerExpectedAccessClass::kRowUuidLookup:
      return "row_uuid_lookup";
    case OptimizerExpectedAccessClass::kScalarBtreeLookup:
      return "scalar_btree_lookup";
    case OptimizerExpectedAccessClass::kScalarBtreeRange:
      return "scalar_btree_range";
    case OptimizerExpectedAccessClass::kHashEquality:
      return "hash_equality";
    case OptimizerExpectedAccessClass::kCoveringIndex:
      return "covering_index";
    case OptimizerExpectedAccessClass::kBitmapSummary:
      return "bitmap_summary";
    case OptimizerExpectedAccessClass::kBoundedTableScan:
      return "bounded_table_scan";
    case OptimizerExpectedAccessClass::kColumnarZoneScan:
      return "columnar_zone_scan";
    case OptimizerExpectedAccessClass::kJoinPlan:
      return "join_plan";
    case OptimizerExpectedAccessClass::kDmlRowLocator:
      return "dml_row_locator";
    case OptimizerExpectedAccessClass::kDocumentPathProbe:
      return "document_path_probe";
    case OptimizerExpectedAccessClass::kVectorExactSearch:
      return "vector_exact_search";
    case OptimizerExpectedAccessClass::kVectorApproximateCandidates:
      return "vector_approximate_candidates";
    case OptimizerExpectedAccessClass::kFullTextWandProbe:
      return "full_text_wand_probe";
    case OptimizerExpectedAccessClass::kGraphSeedExpansion:
      return "graph_seed_expansion";
    case OptimizerExpectedAccessClass::kSqlNosqlFusion:
      return "sql_nosql_fusion";
    case OptimizerExpectedAccessClass::kBulkIngestAppend:
      return "bulk_ingest_append";
  }
  return "unknown";
}

std::vector<OptimizerWorkloadRegressionClass>
RequiredOptimizerWorkloadRegressionClasses() {
  return {
      OptimizerWorkloadRegressionClass::kOltpPointLookup,
      OptimizerWorkloadRegressionClass::kOltpRangeLookup,
      OptimizerWorkloadRegressionClass::kOlapScanAggregate,
      OptimizerWorkloadRegressionClass::kJoin,
      OptimizerWorkloadRegressionClass::kDmlLocatorMutation,
      OptimizerWorkloadRegressionClass::kDocumentPath,
      OptimizerWorkloadRegressionClass::kVectorExact,
      OptimizerWorkloadRegressionClass::kVectorApproximate,
      OptimizerWorkloadRegressionClass::kTextWand,
      OptimizerWorkloadRegressionClass::kGraphTraversal,
      OptimizerWorkloadRegressionClass::kMixedSqlNosqlFusion,
      OptimizerWorkloadRegressionClass::kBulkIngest,
  };
}

OptimizerWorkloadRegressionBudgetValidation
ValidateOptimizerWorkloadRegressionBudgetRecord(
    const OptimizerWorkloadRegressionBudgetRecord& record) {
  OptimizerWorkloadRegressionBudgetValidation validation;

  RequireField(&validation,
               record.schema_id ==
                   kOptimizerWorkloadRegressionBudgetSchemaId,
               "schema_id");
  RequireField(&validation,
               record.schema_version_major ==
                   kOptimizerWorkloadRegressionBudgetSchemaMajor,
               "schema_version_major");
  RequireField(&validation,
               record.schema_version_minor ==
                   kOptimizerWorkloadRegressionBudgetSchemaMinor,
               "schema_version_minor");
  RequireField(&validation, !Empty(record.budget_id), "budget_id");
  RequireField(&validation, IsDriverVisibleRoute(record.route_kind),
               "route_kind");
  RequireField(&validation, !Empty(record.route_lane), "route_lane");
  RequireField(&validation, !Empty(record.route_label), "route_label");
  RequireField(&validation, !Empty(record.cold_or_warm_lane),
               "cold_or_warm_lane");
  RequireField(&validation,
               !IsPlaceholderResultContract(record.result_contract_hash) &&
                   IsHashLike(record.result_contract_hash),
               "result_contract_hash");
  RequireField(&validation, IsHashLike(record.result_hash), "result_hash");
  RequireField(&validation, IsHashLike(record.logical_plan_hash),
               "logical_plan_hash");
  RequireField(&validation, IsHashLike(record.physical_plan_hash),
               "physical_plan_hash");
  RequireField(&validation, IsHashLike(record.sblr_digest), "sblr_digest");
  RequireField(&validation, IsHashLike(record.dataset_schema_digest),
               "dataset_schema_digest");
  RequireField(&validation, !Empty(record.optimizer_profile),
               "optimizer_profile");
  RequireField(&validation, !Empty(record.workload_budget_profile),
               "workload_budget_profile");
  RequireField(&validation, IsHashLike(record.metric_snapshot_digest),
               "metric_snapshot_digest");
  RequireField(&validation, IsHashLike(record.statistics_snapshot_digest),
               "statistics_snapshot_digest");
  RequireField(&validation, IsHashLike(record.provenance_digest),
               "provenance_digest");
  RequireField(&validation, IsHashLike(record.redaction_digest),
               "redaction_digest");
  RequireField(&validation, IsHashLike(record.memory_reservation_digest),
               "memory_reservation_digest");

  if (!AccessMatchesClass(record.workload_class,
                          record.expected_access_class)) {
    AddDiagnostic(&validation, record,
                  "SB_OPT_WORKLOAD_BUDGET.ACCESS_CLASS_MISMATCH");
  }

  if (record.metric_snapshot_generation <= 1 ||
      record.statistics_epoch <= 1 ||
      record.feedback_generation <= 1 ||
      record.memory_feedback_generation <= 1 ||
      record.provider_generation <= 1) {
    AddDiagnostic(&validation, record,
                  "SB_OPT_WORKLOAD_BUDGET.PLACEHOLDER_EPOCH");
  }

  if (!record.metrics_trusted || !record.metrics_fresh ||
      !record.statistics_trusted || !record.statistics_fresh ||
      !record.trusted_provenance || !record.redaction_applied ||
      !record.evidence_only || record.metric_freshness_microseconds == 0 ||
      record.max_metric_freshness_microseconds == 0 ||
      record.statistics_freshness_microseconds == 0 ||
      record.max_statistics_freshness_microseconds == 0 ||
      record.metric_freshness_microseconds >
          record.max_metric_freshness_microseconds ||
      record.statistics_freshness_microseconds >
          record.max_statistics_freshness_microseconds) {
    AddDiagnostic(&validation, record,
                  "SB_OPT_WORKLOAD_BUDGET.TRUST_FRESHNESS_INVALID");
  }

  if (record.placeholder_runtime_evidence || record.synthetic_statistics ||
      record.local_default_statistics || record.policy_default_statistics) {
    AddDiagnostic(&validation, record,
                  "SB_OPT_WORKLOAD_BUDGET.SYNTHETIC_OR_PLACEHOLDER");
  }

  if (HasAuthorityDrift(record.authority)) {
    AddDiagnostic(&validation, record,
                  "SB_OPT_WORKLOAD_BUDGET.FORBIDDEN_AUTHORITY");
  }
  if (!record.reference_reference_only || record.reference_as_authority ||
      record.uses_reference_storage_or_finality_for_scratchbird) {
    AddDiagnostic(&validation, record,
                  "SB_OPT_WORKLOAD_BUDGET.REFERENCE_AUTHORITY_DRIFT");
  }

  if (!Positive(record.baseline_p95_us) ||
      !Positive(record.optimized_p95_us) ||
      !Positive(record.baseline_p99_us) ||
      !Positive(record.optimized_p99_us) ||
      !Positive(record.max_regression_ratio) ||
      !Positive(record.observed_regression_ratio) ||
      record.max_regression_ratio < 1.0 ||
      record.baseline_p95_us > record.baseline_p99_us ||
      record.optimized_p95_us > record.optimized_p99_us) {
    AddDiagnostic(&validation, record,
                  "SB_OPT_WORKLOAD_BUDGET.REGRESSION_METRIC_INVALID");
  } else {
    const double computed_ratio =
        record.optimized_p95_us / record.baseline_p95_us;
    if (!SameMetric(record.observed_regression_ratio, computed_ratio)) {
      AddDiagnostic(&validation, record,
                    "SB_OPT_WORKLOAD_BUDGET.REGRESSION_RATIO_MISMATCH");
    }
    if (record.observed_regression_ratio > record.max_regression_ratio ||
        record.optimized_p99_us >
            record.baseline_p99_us * record.max_regression_ratio) {
      AddDiagnostic(&validation, record,
                    "SB_OPT_WORKLOAD_BUDGET.REGRESSION_BUDGET_EXCEEDED");
    }
  }

  if (!record.spill_bound_bytes_present || record.unbounded_spill ||
      record.observed_spill_bytes > record.max_spill_bytes) {
    AddDiagnostic(&validation, record,
                  "SB_OPT_WORKLOAD_BUDGET.UNBOUNDED_SPILL");
  }
  if (record.memory_reservation_bytes == 0) {
    AddDiagnostic(&validation, record,
                  "SB_OPT_WORKLOAD_BUDGET.MEMORY_RESERVATION_MISSING");
  }

  if (record.table_scan_fallback_used) {
    if (record.table_scan_fallback_policy ==
            OptimizerTableScanFallbackPolicy::kForbidden ||
        !record.table_scan_fallback_bounded ||
        record.table_scan_fallback_reason.empty()) {
      AddDiagnostic(&validation, record,
                    "SB_OPT_WORKLOAD_BUDGET.UNSAFE_TABLE_SCAN_FALLBACK");
    }
  } else if (record.expected_access_class ==
                 OptimizerExpectedAccessClass::kBoundedTableScan &&
             !IsExpectedTableScanWorkload(record.workload_class)) {
    AddDiagnostic(&validation, record,
                  "SB_OPT_WORKLOAD_BUDGET.UNEXPECTED_TABLE_SCAN_ACCESS");
  }

  const bool exact_fallback_required =
      record.exact_fallback_required ||
      ClassRequiresExactFallback(record.workload_class);
  const bool exact_recheck_required =
      record.exact_recheck_required ||
      ClassRequiresExactRecheck(record.workload_class);
  const bool exact_rerank_required =
      record.exact_rerank_required ||
      ClassRequiresExactRerank(record.workload_class);
  if (exact_fallback_required && !record.exact_fallback_proven) {
    AddDiagnostic(&validation, record,
                  "SB_OPT_WORKLOAD_BUDGET.EXACT_FALLBACK_MISSING");
  }
  if (exact_recheck_required && !record.exact_recheck_proven) {
    AddDiagnostic(&validation, record,
                  "SB_OPT_WORKLOAD_BUDGET.EXACT_RECHECK_MISSING");
  }
  if (exact_rerank_required && !record.exact_rerank_proven) {
    AddDiagnostic(&validation, record,
                  "SB_OPT_WORKLOAD_BUDGET.EXACT_RERANK_MISSING");
  }
  if (!record.mga_recheck_required || !record.mga_recheck_proven ||
      !record.security_recheck_required || !record.security_recheck_proven) {
    AddDiagnostic(&validation, record,
                  "SB_OPT_WORKLOAD_BUDGET.MGA_SECURITY_RECHECK_MISSING");
  }

  if (record.cluster_mode ==
          OptimizerWorkloadRegressionClusterMode::kLocalClusterEvidence ||
      record.route_kind == "cluster") {
    AddDiagnostic(&validation, record,
                  "SB_OPT_WORKLOAD_BUDGET.LOCAL_CLUSTER_FORBIDDEN");
  } else if (record.cluster_mode ==
             OptimizerWorkloadRegressionClusterMode::kExternalProviderDelegated) {
    if (record.external_cluster_provider_id.empty() ||
        !record.cluster_claim_blocked ||
        record.production_regression_budget_claim ||
        record.benchmark_clean_claim) {
      AddDiagnostic(
          &validation, record,
          "SB_OPT_WORKLOAD_BUDGET.EXTERNAL_CLUSTER_CLAIM_BLOCK_REQUIRED");
    }
    if ((record.benchmark_evidence_attached &&
         record.benchmark_evidence.production_benchmark_clean_claim) ||
        (record.correctness_oracle_attached &&
         record.correctness_oracle_case.production_correctness_claim)) {
      AddDiagnostic(
          &validation, record,
          "SB_OPT_WORKLOAD_BUDGET.EXTERNAL_CLUSTER_NESTED_CLAIM_BLOCK_REQUIRED");
    }
  }

  if (record.cluster_mode == OptimizerWorkloadRegressionClusterMode::kNoCluster &&
      record.production_regression_budget_claim) {
    if (!record.benchmark_clean_claim) {
      AddDiagnostic(&validation, record,
                    "SB_OPT_WORKLOAD_BUDGET.BENCHMARK_CLEAN_CLAIM_REQUIRED");
    }
    if (!record.benchmark_evidence_attached) {
      AddDiagnostic(&validation, record,
                    "SB_OPT_WORKLOAD_BUDGET.BENCHMARK_EVIDENCE_MISSING");
    } else {
      const auto benchmark_validation =
          ValidatePersistedOptimizerBenchmarkEvidenceRecord(
              record.benchmark_evidence);
      if (!benchmark_validation.ok ||
          !benchmark_validation.benchmark_clean_admissible ||
          !BenchmarkEvidenceMatchesBudget(record)) {
        AddDiagnostic(&validation, record,
                      "SB_OPT_WORKLOAD_BUDGET.BENCHMARK_EVIDENCE_INVALID");
        validation.diagnostics.insert(validation.diagnostics.end(),
                                      benchmark_validation.diagnostics.begin(),
                                      benchmark_validation.diagnostics.end());
      }
    }
    if (!record.correctness_oracle_attached) {
      AddDiagnostic(&validation, record,
                    "SB_OPT_WORKLOAD_BUDGET.CORRECTNESS_ORACLE_MISSING");
    } else {
      const auto correctness_validation =
          ValidateOptimizerCorrectnessOracleCase(
              record.correctness_oracle_case);
      if (!correctness_validation.ok ||
          !correctness_validation.correctness_proven ||
          !CorrectnessEvidenceMatchesBudget(record)) {
        AddDiagnostic(&validation, record,
                      "SB_OPT_WORKLOAD_BUDGET.CORRECTNESS_ORACLE_INVALID");
        validation.diagnostics.insert(validation.diagnostics.end(),
                                      correctness_validation.diagnostics.begin(),
                                      correctness_validation.diagnostics.end());
      }
    }
  } else if (record.production_regression_budget_claim &&
             record.cluster_mode !=
                 OptimizerWorkloadRegressionClusterMode::kNoCluster) {
    AddDiagnostic(&validation, record,
                  "SB_OPT_WORKLOAD_BUDGET.PRODUCTION_CLUSTER_OVERCLAIM");
  }

  validation.ok = validation.missing_fields.empty() &&
                  validation.diagnostics.empty();
  validation.regression_budget_proven =
      validation.ok && record.production_regression_budget_claim &&
      record.benchmark_clean_claim &&
      record.cluster_mode == OptimizerWorkloadRegressionClusterMode::kNoCluster;
  validation.diagnostic_code =
      validation.ok
          ? "SB_OPT_WORKLOAD_BUDGET.OK"
          : (validation.missing_fields.empty()
                 ? "SB_OPT_WORKLOAD_BUDGET.INVALID_CONTRACT"
                 : "SB_OPT_WORKLOAD_BUDGET.MISSING_REQUIRED_FIELD");
  return validation;
}

OptimizerWorkloadRegressionBudgetValidation
ValidateOptimizerWorkloadRegressionBudgetSuite(
    const std::vector<OptimizerWorkloadRegressionBudgetRecord>& records,
    const std::vector<OptimizerWorkloadRegressionClass>& required_classes,
    const std::vector<std::string>& required_routes) {
  OptimizerWorkloadRegressionBudgetValidation validation;
  if (records.empty()) {
    validation.diagnostic_code = "SB_OPT_WORKLOAD_BUDGET.EMPTY_SUITE";
    validation.diagnostics.push_back("SB_OPT_WORKLOAD_BUDGET.EMPTY_SUITE");
    return validation;
  }

  std::set<std::string> seen_budget_ids;
  std::set<OptimizerWorkloadRegressionClass> proven_classes;
  std::set<std::string> proven_routes;
  for (const auto& record : records) {
    if (!record.budget_id.empty() &&
        !seen_budget_ids.insert(record.budget_id).second) {
      AddDiagnostic(&validation, record,
                    "SB_OPT_WORKLOAD_BUDGET.DUPLICATE_BUDGET");
    }

    const auto record_validation =
        ValidateOptimizerWorkloadRegressionBudgetRecord(record);
    if (!record_validation.ok) {
      validation.diagnostics.insert(validation.diagnostics.end(),
                                    record_validation.diagnostics.begin(),
                                    record_validation.diagnostics.end());
      validation.missing_fields.insert(validation.missing_fields.end(),
                                       record_validation.missing_fields.begin(),
                                       record_validation.missing_fields.end());
    }
    if (record_validation.regression_budget_proven) {
      proven_classes.insert(record.workload_class);
      proven_routes.insert(record.route_kind);
    }
  }

  for (const auto required : required_classes) {
    if (proven_classes.find(required) == proven_classes.end()) {
      validation.diagnostics.push_back(
          std::string("SB_OPT_WORKLOAD_BUDGET.MISSING_CLASS:") +
          OptimizerWorkloadRegressionClassName(required));
    }
  }
  for (const auto& route : required_routes) {
    if (proven_routes.find(route) == proven_routes.end()) {
      validation.diagnostics.push_back(
          "SB_OPT_WORKLOAD_BUDGET.MISSING_ROUTE:" + route);
    }
  }

  validation.ok = validation.missing_fields.empty() &&
                  validation.diagnostics.empty();
  validation.regression_budget_proven = validation.ok;
  validation.diagnostic_code =
      validation.ok
          ? "SB_OPT_WORKLOAD_BUDGET.OK"
          : (validation.missing_fields.empty()
                 ? "SB_OPT_WORKLOAD_BUDGET.INVALID_CONTRACT"
                 : "SB_OPT_WORKLOAD_BUDGET.MISSING_REQUIRED_FIELD");
  return validation;
}

}  // namespace scratchbird::engine::optimizer
