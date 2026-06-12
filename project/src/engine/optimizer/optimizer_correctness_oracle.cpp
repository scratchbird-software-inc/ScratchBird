// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_correctness_oracle.hpp"

#include <algorithm>
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

std::string CasePrefix(const OptimizerCorrectnessOracleCase& oracle_case) {
  if (!oracle_case.case_id.empty()) return oracle_case.case_id;
  if (!oracle_case.route_label.empty()) return oracle_case.route_label;
  return OptimizerCorrectnessClassName(oracle_case.correctness_class);
}

void AddDiagnostic(OptimizerCorrectnessOracleValidation* validation,
                   const OptimizerCorrectnessOracleCase& oracle_case,
                   std::string diagnostic) {
  validation->diagnostics.push_back(CasePrefix(oracle_case) + ":" +
                                    std::move(diagnostic));
}

void RequireField(OptimizerCorrectnessOracleValidation* validation,
                  bool present,
                  std::string field_name) {
  if (!present) validation->missing_fields.push_back(std::move(field_name));
}

bool HasAuthorityDrift(
    const OptimizerCorrectnessAuthorityFlags& authority) {
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

bool ResultEvidenceHasRequiredFields(
    const OptimizerResultOracleEvidence& evidence) {
  return !evidence.producer_label.empty() &&
         !IsPlaceholderResultContract(evidence.result_contract_hash) &&
         IsHashLike(evidence.result_contract_hash) &&
         IsHashLike(evidence.result_hash) &&
         evidence.result_row_count_observed &&
         IsHashLike(evidence.ordering_contract_hash) &&
         IsHashLike(evidence.null_semantics_hash) &&
         !evidence.error_diagnostic_code.empty() &&
         IsHashLike(evidence.diagnostic_digest) &&
         IsHashLike(evidence.recheck_evidence_digest);
}

bool IsDriverVisibleRoute(std::string_view route) {
  return route == "embedded" || route == "ipc" || route == "inet" ||
         route == "cli" || route == "driver";
}

bool IsSpecializedClass(OptimizerCorrectnessClass value) {
  return value == OptimizerCorrectnessClass::kDocumentPath ||
         value == OptimizerCorrectnessClass::kVector ||
         value == OptimizerCorrectnessClass::kTextSearch ||
         value == OptimizerCorrectnessClass::kGraph ||
         value == OptimizerCorrectnessClass::kMixedFusion;
}

bool RequiresExactRecheck(OptimizerCorrectnessClass value) {
  return value == OptimizerCorrectnessClass::kDmlLocator ||
         IsSpecializedClass(value);
}

bool RequiresExactRerank(OptimizerCorrectnessClass value) {
  return value == OptimizerCorrectnessClass::kVector ||
         value == OptimizerCorrectnessClass::kTextSearch ||
         value == OptimizerCorrectnessClass::kMixedFusion;
}

bool RequiresCorrelationContract(OptimizerCorrectnessClass value) {
  return value == OptimizerCorrectnessClass::kCorrelatedDependency ||
         value == OptimizerCorrectnessClass::kMixedFusion;
}

bool RequiresDmlLocatorContract(OptimizerCorrectnessClass value) {
  return value == OptimizerCorrectnessClass::kDmlLocator;
}

void ValidateResultSide(
    OptimizerCorrectnessOracleValidation* validation,
    const OptimizerCorrectnessOracleCase& oracle_case,
    const OptimizerResultOracleEvidence& evidence,
    std::string_view side,
    bool exact_recheck_required,
    bool exact_rerank_required,
    bool dml_locator_required,
    bool production_correctness_claim) {
  if (!ResultEvidenceHasRequiredFields(evidence)) {
    AddDiagnostic(validation, oracle_case,
                  std::string("SB_OPT_CORRECTNESS_ORACLE.RESULT_FIELDS_MISSING:") +
                      std::string(side));
  }
  if (production_correctness_claim &&
      (!evidence.live_route_executed || evidence.synthetic_result)) {
    AddDiagnostic(validation, oracle_case,
                  std::string("SB_OPT_CORRECTNESS_ORACLE.SYNTHETIC_PRODUCTION_RESULT:") +
                      std::string(side));
  }
  if (exact_recheck_required && !evidence.exact_recheck_proven) {
    AddDiagnostic(validation, oracle_case,
                  std::string("SB_OPT_CORRECTNESS_ORACLE.EXACT_RECHECK_MISSING:") +
                      std::string(side));
  }
  if (exact_rerank_required && !evidence.exact_rerank_proven) {
    AddDiagnostic(validation, oracle_case,
                  std::string("SB_OPT_CORRECTNESS_ORACLE.EXACT_RERANK_MISSING:") +
                      std::string(side));
  }
  if (!evidence.mga_recheck_proven) {
    AddDiagnostic(validation, oracle_case,
                  std::string("SB_OPT_CORRECTNESS_ORACLE.MGA_RECHECK_MISSING:") +
                      std::string(side));
  }
  if (!evidence.security_recheck_proven) {
    AddDiagnostic(validation, oracle_case,
                  std::string("SB_OPT_CORRECTNESS_ORACLE.SECURITY_RECHECK_MISSING:") +
                      std::string(side));
  }
  if (dml_locator_required &&
      (!IsHashLike(evidence.row_locator_contract_hash) ||
       !evidence.row_locator_mga_snapshot_proven)) {
    AddDiagnostic(validation, oracle_case,
                  std::string("SB_OPT_CORRECTNESS_ORACLE.DML_LOCATOR_PROOF_MISSING:") +
                      std::string(side));
  }
}

void ValidateResultEquivalence(
    OptimizerCorrectnessOracleValidation* validation,
    const OptimizerCorrectnessOracleCase& oracle_case) {
  const auto& baseline = oracle_case.baseline;
  const auto& optimized = oracle_case.optimized;
  if (baseline.accepted != optimized.accepted) {
    AddDiagnostic(validation, oracle_case,
                  "SB_OPT_CORRECTNESS_ORACLE.ACCEPTANCE_MISMATCH");
  }
  if (baseline.result_contract_hash != optimized.result_contract_hash) {
    AddDiagnostic(validation, oracle_case,
                  "SB_OPT_CORRECTNESS_ORACLE.RESULT_CONTRACT_MISMATCH");
  }
  if (baseline.result_hash != optimized.result_hash) {
    AddDiagnostic(validation, oracle_case,
                  "SB_OPT_CORRECTNESS_ORACLE.RESULT_HASH_MISMATCH");
  }
  if (baseline.result_row_count != optimized.result_row_count) {
    AddDiagnostic(validation, oracle_case,
                  "SB_OPT_CORRECTNESS_ORACLE.ROW_COUNT_MISMATCH");
  }
  if (baseline.ordering_contract_hash != optimized.ordering_contract_hash) {
    AddDiagnostic(validation, oracle_case,
                  "SB_OPT_CORRECTNESS_ORACLE.ORDERING_CONTRACT_MISMATCH");
  }
  if (baseline.null_semantics_hash != optimized.null_semantics_hash) {
    AddDiagnostic(validation, oracle_case,
                  "SB_OPT_CORRECTNESS_ORACLE.NULL_SEMANTICS_MISMATCH");
  }
  if (baseline.error_diagnostic_code != optimized.error_diagnostic_code ||
      baseline.diagnostic_digest != optimized.diagnostic_digest) {
    AddDiagnostic(validation, oracle_case,
                  "SB_OPT_CORRECTNESS_ORACLE.ERROR_DIAGNOSTIC_MISMATCH");
  }
  if (RequiresDmlLocatorContract(oracle_case.correctness_class) &&
      baseline.row_locator_contract_hash != optimized.row_locator_contract_hash) {
    AddDiagnostic(validation, oracle_case,
                  "SB_OPT_CORRECTNESS_ORACLE.DML_LOCATOR_CONTRACT_MISMATCH");
  }
}

}  // namespace

const char* OptimizerCorrectnessClassName(
    OptimizerCorrectnessClass value) {
  switch (value) {
    case OptimizerCorrectnessClass::kInnerJoin:
      return "inner_join";
    case OptimizerCorrectnessClass::kOuterJoin:
      return "outer_join";
    case OptimizerCorrectnessClass::kSemiJoin:
      return "semi_join";
    case OptimizerCorrectnessClass::kAntiJoin:
      return "anti_join";
    case OptimizerCorrectnessClass::kCorrelatedDependency:
      return "correlated_dependency";
    case OptimizerCorrectnessClass::kAggregation:
      return "aggregation";
    case OptimizerCorrectnessClass::kDistinct:
      return "distinct";
    case OptimizerCorrectnessClass::kWindow:
      return "window";
    case OptimizerCorrectnessClass::kTopN:
      return "topn";
    case OptimizerCorrectnessClass::kDmlLocator:
      return "dml_locator";
    case OptimizerCorrectnessClass::kDocumentPath:
      return "document_path";
    case OptimizerCorrectnessClass::kVector:
      return "vector";
    case OptimizerCorrectnessClass::kTextSearch:
      return "text_search";
    case OptimizerCorrectnessClass::kGraph:
      return "graph";
    case OptimizerCorrectnessClass::kMixedFusion:
      return "mixed_fusion";
  }
  return "unknown";
}

std::vector<OptimizerCorrectnessClass> RequiredOptimizerCorrectnessClasses() {
  return {
      OptimizerCorrectnessClass::kInnerJoin,
      OptimizerCorrectnessClass::kOuterJoin,
      OptimizerCorrectnessClass::kSemiJoin,
      OptimizerCorrectnessClass::kAntiJoin,
      OptimizerCorrectnessClass::kCorrelatedDependency,
      OptimizerCorrectnessClass::kAggregation,
      OptimizerCorrectnessClass::kDistinct,
      OptimizerCorrectnessClass::kWindow,
      OptimizerCorrectnessClass::kTopN,
      OptimizerCorrectnessClass::kDmlLocator,
      OptimizerCorrectnessClass::kDocumentPath,
      OptimizerCorrectnessClass::kVector,
      OptimizerCorrectnessClass::kTextSearch,
      OptimizerCorrectnessClass::kGraph,
      OptimizerCorrectnessClass::kMixedFusion,
  };
}

OptimizerCorrectnessOracleValidation ValidateOptimizerCorrectnessOracleCase(
    const OptimizerCorrectnessOracleCase& oracle_case) {
  OptimizerCorrectnessOracleValidation validation;

  RequireField(&validation,
               oracle_case.schema_id == kOptimizerCorrectnessOracleSchemaId,
               "schema_id");
  RequireField(&validation,
               oracle_case.schema_version_major ==
                   kOptimizerCorrectnessOracleSchemaMajor,
               "schema_version_major");
  RequireField(&validation,
               oracle_case.schema_version_minor ==
                   kOptimizerCorrectnessOracleSchemaMinor,
               "schema_version_minor");
  RequireField(&validation, !Empty(oracle_case.case_id), "case_id");
  RequireField(&validation, IsDriverVisibleRoute(oracle_case.route_kind),
               "route_kind");
  RequireField(&validation, !Empty(oracle_case.route_label), "route_label");
  RequireField(&validation, IsHashLike(oracle_case.dataset_schema_digest),
               "dataset_schema_digest");
  RequireField(&validation, IsHashLike(oracle_case.sblr_digest),
               "sblr_digest");
  RequireField(&validation, IsHashLike(oracle_case.logical_plan_hash),
               "logical_plan_hash");
  RequireField(&validation, IsHashLike(oracle_case.baseline_plan_hash),
               "baseline_plan_hash");
  RequireField(&validation, IsHashLike(oracle_case.optimized_plan_hash),
               "optimized_plan_hash");
  RequireField(&validation, IsHashLike(oracle_case.equivalence_contract_hash),
               "equivalence_contract_hash");

  if (RequiresCorrelationContract(oracle_case.correctness_class) &&
      !IsHashLike(oracle_case.correlation_dependency_contract_hash)) {
    AddDiagnostic(&validation, oracle_case,
                  "SB_OPT_CORRECTNESS_ORACLE.CORRELATION_CONTRACT_MISSING");
  }
  if (oracle_case.catalog_epoch <= 1 ||
      oracle_case.security_epoch <= 1 ||
      oracle_case.redaction_epoch <= 1 ||
      oracle_case.statistics_epoch <= 1 ||
      oracle_case.provider_generation <= 1) {
    AddDiagnostic(&validation, oracle_case,
                  "SB_OPT_CORRECTNESS_ORACLE.PLACEHOLDER_EPOCH");
  }
  if (!oracle_case.production_correctness_claim ||
      !oracle_case.evidence_only ||
      !oracle_case.baseline_is_engine_reference_route ||
      !oracle_case.optimized_route_consumed) {
    AddDiagnostic(&validation, oracle_case,
                  "SB_OPT_CORRECTNESS_ORACLE.PRODUCTION_ROUTE_PROOF_MISSING");
  }
  if (!oracle_case.mga_recheck_required ||
      !oracle_case.security_recheck_required) {
    AddDiagnostic(&validation, oracle_case,
                  "SB_OPT_CORRECTNESS_ORACLE.MGA_SECURITY_RECHECK_REQUIRED");
  }
  if (!oracle_case.reference_reference_only ||
      oracle_case.reference_as_authority ||
      oracle_case.uses_reference_storage_or_finality_for_scratchbird) {
    AddDiagnostic(&validation, oracle_case,
                  "SB_OPT_CORRECTNESS_ORACLE.REFERENCE_AUTHORITY_DRIFT");
  }
  if (HasAuthorityDrift(oracle_case.authority)) {
    AddDiagnostic(&validation, oracle_case,
                  "SB_OPT_CORRECTNESS_ORACLE.FORBIDDEN_AUTHORITY");
  }
  if (oracle_case.cluster_mode ==
          OptimizerCorrectnessClusterMode::kLocalClusterEvidence ||
      oracle_case.route_kind == "cluster") {
    AddDiagnostic(&validation, oracle_case,
                  "SB_OPT_CORRECTNESS_ORACLE.LOCAL_CLUSTER_FORBIDDEN");
  } else if (oracle_case.cluster_mode ==
             OptimizerCorrectnessClusterMode::kExternalProviderDelegated) {
    if (oracle_case.external_cluster_provider_id.empty() ||
        !oracle_case.cluster_claim_blocked ||
        oracle_case.production_correctness_claim) {
      AddDiagnostic(
          &validation, oracle_case,
          "SB_OPT_CORRECTNESS_ORACLE.EXTERNAL_CLUSTER_CLAIM_BLOCK_REQUIRED");
    }
  }

  const bool exact_recheck_required =
      oracle_case.exact_recheck_required ||
      RequiresExactRecheck(oracle_case.correctness_class);
  const bool exact_rerank_required =
      oracle_case.exact_rerank_required ||
      RequiresExactRerank(oracle_case.correctness_class);
  const bool dml_locator_required =
      RequiresDmlLocatorContract(oracle_case.correctness_class);

  ValidateResultSide(&validation, oracle_case, oracle_case.baseline,
                     "baseline", exact_recheck_required,
                     exact_rerank_required, dml_locator_required,
                     oracle_case.production_correctness_claim);
  ValidateResultSide(&validation, oracle_case, oracle_case.optimized,
                     "optimized", exact_recheck_required,
                     exact_rerank_required, dml_locator_required,
                     oracle_case.production_correctness_claim);
  ValidateResultEquivalence(&validation, oracle_case);

  validation.ok = validation.missing_fields.empty() &&
                  validation.diagnostics.empty();
  validation.correctness_proven =
      validation.ok && oracle_case.production_correctness_claim &&
      oracle_case.cluster_mode == OptimizerCorrectnessClusterMode::kNoCluster;
  validation.diagnostic_code =
      validation.ok ? "SB_OPT_CORRECTNESS_ORACLE.OK"
                    : (validation.missing_fields.empty()
                           ? "SB_OPT_CORRECTNESS_ORACLE.INVALID_CONTRACT"
                           : "SB_OPT_CORRECTNESS_ORACLE.MISSING_REQUIRED_FIELD");
  return validation;
}

OptimizerCorrectnessOracleValidation ValidateOptimizerCorrectnessOracleSuite(
    const std::vector<OptimizerCorrectnessOracleCase>& oracle_cases,
    const std::vector<OptimizerCorrectnessClass>& required_classes,
    const std::vector<std::string>& required_routes) {
  OptimizerCorrectnessOracleValidation validation;
  if (oracle_cases.empty()) {
    validation.diagnostic_code = "SB_OPT_CORRECTNESS_ORACLE.EMPTY_SUITE";
    validation.diagnostics.push_back(
        "SB_OPT_CORRECTNESS_ORACLE.EMPTY_SUITE");
    return validation;
  }

  std::set<std::string> seen_cases;
  std::set<OptimizerCorrectnessClass> seen_classes;
  std::set<std::string> seen_routes;
  bool all_proven = true;

  for (const auto& oracle_case : oracle_cases) {
    if (!oracle_case.case_id.empty() &&
        !seen_cases.insert(oracle_case.case_id).second) {
      AddDiagnostic(&validation, oracle_case,
                    "SB_OPT_CORRECTNESS_ORACLE.DUPLICATE_CASE");
    }
    seen_classes.insert(oracle_case.correctness_class);
    if (!oracle_case.route_kind.empty()) {
      seen_routes.insert(oracle_case.route_kind);
    }

    const auto case_validation =
        ValidateOptimizerCorrectnessOracleCase(oracle_case);
    if (!case_validation.ok) {
      validation.diagnostics.insert(validation.diagnostics.end(),
                                    case_validation.diagnostics.begin(),
                                    case_validation.diagnostics.end());
      validation.missing_fields.insert(validation.missing_fields.end(),
                                       case_validation.missing_fields.begin(),
                                       case_validation.missing_fields.end());
    }
    all_proven = all_proven && case_validation.correctness_proven;
  }

  for (const auto required_class : required_classes) {
    if (seen_classes.find(required_class) == seen_classes.end()) {
      validation.diagnostics.push_back(
          std::string(OptimizerCorrectnessClassName(required_class)) +
          ":SB_OPT_CORRECTNESS_ORACLE.MISSING_CLASS");
    }
  }
  for (const auto& required_route : required_routes) {
    if (seen_routes.find(required_route) == seen_routes.end()) {
      validation.diagnostics.push_back(
          required_route + ":SB_OPT_CORRECTNESS_ORACLE.MISSING_ROUTE");
    }
  }

  validation.ok = validation.missing_fields.empty() &&
                  validation.diagnostics.empty();
  validation.correctness_proven = validation.ok && all_proven;
  validation.diagnostic_code =
      validation.ok ? "SB_OPT_CORRECTNESS_ORACLE.SUITE_OK"
                    : "SB_OPT_CORRECTNESS_ORACLE.SUITE_INVALID";
  return validation;
}

}  // namespace scratchbird::engine::optimizer
