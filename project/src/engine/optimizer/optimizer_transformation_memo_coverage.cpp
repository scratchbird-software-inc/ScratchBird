// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_transformation_memo_coverage.hpp"

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

bool IsPlaceholderHash(std::string_view value) {
  return value.empty() || value == "result-contract-v1" ||
         value == "sha256:result-contract-v1" ||
         value == "sha256:placeholder" ||
         value == "sha256:logical-plan-v1" ||
         value == "sha256:physical-plan-v1" ||
         value.find("placeholder") != std::string_view::npos;
}

bool ValidProofHash(std::string_view value) {
  return IsHashLike(value) && !IsPlaceholderHash(value);
}

bool HasForbiddenAuthority(
    const OptimizerTransformationAuthorityFlags& authority) {
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

void AddDiagnostic(OptimizerTransformationMemoValidation* validation,
                   std::string diagnostic) {
  validation->diagnostics.push_back(std::move(diagnostic));
}

void RequireField(OptimizerTransformationMemoValidation* validation,
                  bool present,
                  std::string field_name) {
  if (!present) validation->missing_fields.push_back(std::move(field_name));
}

std::string ReportPrefix(
    const OptimizerTransformationMemoCoverageReport& report) {
  if (!report.memo_id.empty()) return report.memo_id;
  return "unnamed_transformation_memo_report";
}

std::string RulePrefix(const OptimizerTransformationRuleEvidence& rule) {
  if (!rule.rule_id.empty()) return rule.rule_id;
  if (!rule.rule_name.empty()) return rule.rule_name;
  return OptimizerTransformationFamilyName(rule.family);
}

void ValidateFamilySpecificProof(
    OptimizerTransformationMemoValidation* validation,
    const OptimizerTransformationRuleEvidence& rule) {
  const auto prefix = RulePrefix(rule);
  switch (rule.family) {
    case OptimizerTransformationFamily::kPredicatePushdown:
      if (!rule.predicate_scope_proven || !rule.predicate_side_effect_free ||
          !rule.null_semantics_preserved) {
        AddDiagnostic(
            validation,
            prefix +
                ":SB_OPT_TRANSFORMATION_MEMO.PREDICATE_PUSHDOWN_PROOF_MISSING");
      }
      break;
    case OptimizerTransformationFamily::kProjectionPruning:
      if (!rule.projection_dependency_closure_proven ||
          !rule.required_columns_preserved) {
        AddDiagnostic(
            validation,
            prefix +
                ":SB_OPT_TRANSFORMATION_MEMO.PROJECTION_PRUNING_PROOF_MISSING");
      }
      break;
    case OptimizerTransformationFamily::kJoinRewrite:
      if (!rule.join_legality_proven || !rule.row_multiplicity_preserved) {
        AddDiagnostic(validation,
                      prefix +
                          ":SB_OPT_TRANSFORMATION_MEMO.JOIN_REWRITE_PROOF_MISSING");
      }
      break;
    case OptimizerTransformationFamily::kSubqueryDecorrelation:
      if (!rule.correlation_dependency_proven ||
          !rule.duplicate_semantics_preserved ||
          !rule.null_semantics_preserved) {
        AddDiagnostic(
            validation,
            prefix +
                ":SB_OPT_TRANSFORMATION_MEMO.SUBQUERY_DECORRELATION_PROOF_MISSING");
      }
      break;
    case OptimizerTransformationFamily::kCteMaterialization:
      if (!rule.cte_materialization_choice_bounded ||
          !rule.cte_recursion_or_side_effect_proven) {
        AddDiagnostic(
            validation,
            prefix +
                ":SB_OPT_TRANSFORMATION_MEMO.CTE_MATERIALIZATION_PROOF_MISSING");
      }
      break;
    case OptimizerTransformationFamily::kAggregatePushdown:
      if (!rule.aggregate_grouping_semantics_proven ||
          !rule.aggregate_partial_final_contract_proven) {
        AddDiagnostic(
            validation,
            prefix +
                ":SB_OPT_TRANSFORMATION_MEMO.AGGREGATE_PUSHDOWN_PROOF_MISSING");
      }
      break;
    case OptimizerTransformationFamily::kWindowOrderReuse:
      if (!rule.ordering_contract_preserved ||
          !rule.window_frame_semantics_preserved) {
        AddDiagnostic(
            validation,
            prefix +
                ":SB_OPT_TRANSFORMATION_MEMO.WINDOW_ORDER_REUSE_PROOF_MISSING");
      }
      break;
    case OptimizerTransformationFamily::kMaterializedViewRewrite:
      if (!rule.materialized_view_freshness_proven ||
          !rule.materialized_view_security_proven ||
          !ValidProofHash(rule.materialized_view_freshness_digest) ||
          rule.unsafe_materialized_view_authority) {
        AddDiagnostic(
            validation,
            prefix +
                ":SB_OPT_TRANSFORMATION_MEMO.MATERIALIZED_VIEW_REWRITE_PROOF_MISSING");
      }
      break;
    case OptimizerTransformationFamily::kRuntimeFilterPlacement:
      if (!rule.runtime_filter_false_negative_safe ||
          !rule.runtime_filter_exact_recheck_preserved ||
          !ValidProofHash(rule.runtime_filter_safety_digest)) {
        AddDiagnostic(
            validation,
            prefix +
                ":SB_OPT_TRANSFORMATION_MEMO.RUNTIME_FILTER_PROOF_MISSING");
      }
      break;
    case OptimizerTransformationFamily::kSqlNosqlFusion:
      if (!rule.fusion_provider_generation_proven ||
          !rule.fusion_route_capability_proven ||
          !rule.fusion_exact_recheck_preserved ||
          !ValidProofHash(rule.provider_route_capability_digest) ||
          !ValidProofHash(rule.exact_recheck_digest) ||
          rule.unsafe_fusion_provider_authority) {
        AddDiagnostic(
            validation,
            prefix +
                ":SB_OPT_TRANSFORMATION_MEMO.SQL_NOSQL_FUSION_PROOF_MISSING");
      }
      break;
  }
}

void ValidateRule(OptimizerTransformationMemoValidation* validation,
                  const OptimizerTransformationRuleEvidence& rule) {
  const auto prefix = RulePrefix(rule);
  if (rule.rule_id.empty() || rule.rule_name.empty() ||
      rule.memo_group_id.empty() || rule.deterministic_rule_order == 0) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_TRANSFORMATION_MEMO.RULE_IDENTITY_MISSING");
  }
  if (!ValidProofHash(rule.input_logical_plan_hash) ||
      !ValidProofHash(rule.output_logical_plan_hash) ||
      !ValidProofHash(rule.equivalence_contract_hash) ||
      !ValidProofHash(rule.result_contract_hash) ||
      !ValidProofHash(rule.semantic_precondition_digest) ||
      !ValidProofHash(rule.semantic_context_digest) ||
      !ValidProofHash(rule.collation_contract_hash) ||
      !ValidProofHash(rule.determinism_contract_hash) ||
      !ValidProofHash(rule.redaction_contract_hash) ||
      !ValidProofHash(rule.rewrite_legality_digest) ||
      !ValidProofHash(rule.diagnostics_digest)) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_TRANSFORMATION_MEMO.PLACEHOLDER_OR_HASH_MISSING");
  }
  if (!rule.semantic_preconditions_proven ||
      !rule.result_contract_preserved || !rule.mga_recheck_preserved ||
      !rule.security_recheck_preserved || !rule.redaction_proof_present ||
      !rule.collation_proof_present || !rule.determinism_proof_present) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_TRANSFORMATION_MEMO.SEMANTIC_PRECONDITION_MISSING");
  }
  if (!rule.bounded_rule_expansion || !rule.deterministic_rule_position ||
      !rule.memo_group_bounded) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_TRANSFORMATION_MEMO.UNBOUNDED_OR_NONDETERMINISTIC_RULE");
  }
  if (!rule.no_plan_authority_claim || !rule.no_parser_reference_authority ||
      rule.placeholder_evidence || rule.reference_as_authority ||
      !rule.reference_reference_only ||
      rule.uses_reference_storage_or_finality_for_scratchbird ||
      HasForbiddenAuthority(rule.authority)) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_TRANSFORMATION_MEMO.FORBIDDEN_AUTHORITY");
  }
  if (rule.accepted) {
    ValidateFamilySpecificProof(validation, rule);
  }
}

void ValidateRejectedTransformation(
    OptimizerTransformationMemoValidation* validation,
    const OptimizerRejectedTransformationDiagnostic& rejected) {
  const auto prefix = rejected.attempted_rule_id.empty()
                          ? "rejected_transformation"
                          : rejected.attempted_rule_id;
  if (rejected.attempted_rule_id.empty() ||
      !ValidProofHash(rejected.input_logical_plan_hash) ||
      rejected.rejection_diagnostic_code.empty() ||
      !ValidProofHash(rejected.reason_digest) ||
      !ValidProofHash(rejected.semantic_precondition_digest)) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_TRANSFORMATION_MEMO.REJECTION_DIAGNOSTIC_MISSING");
  }
  if (!rejected.rejected_fail_closed || !rejected.no_memo_mutation ||
      !rejected.result_contract_unchanged ||
      HasForbiddenAuthority(rejected.authority)) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_TRANSFORMATION_MEMO.REJECTION_NOT_FAIL_CLOSED");
  }
}

bool ContainsFamily(const std::vector<OptimizerTransformationFamily>& values,
                    OptimizerTransformationFamily needle) {
  return std::find(values.begin(), values.end(), needle) != values.end();
}

}  // namespace

const char* OptimizerTransformationFamilyName(
    OptimizerTransformationFamily family) {
  switch (family) {
    case OptimizerTransformationFamily::kPredicatePushdown:
      return "predicate_pushdown";
    case OptimizerTransformationFamily::kProjectionPruning:
      return "projection_pruning";
    case OptimizerTransformationFamily::kJoinRewrite:
      return "join_rewrite";
    case OptimizerTransformationFamily::kSubqueryDecorrelation:
      return "subquery_decorrelation";
    case OptimizerTransformationFamily::kCteMaterialization:
      return "cte_materialization";
    case OptimizerTransformationFamily::kAggregatePushdown:
      return "aggregate_pushdown";
    case OptimizerTransformationFamily::kWindowOrderReuse:
      return "window_order_reuse";
    case OptimizerTransformationFamily::kMaterializedViewRewrite:
      return "materialized_view_rewrite";
    case OptimizerTransformationFamily::kRuntimeFilterPlacement:
      return "runtime_filter_placement";
    case OptimizerTransformationFamily::kSqlNosqlFusion:
      return "sql_nosql_fusion";
  }
  return "unknown";
}

std::vector<OptimizerTransformationFamily>
RequiredOptimizerTransformationFamilies() {
  return {
      OptimizerTransformationFamily::kPredicatePushdown,
      OptimizerTransformationFamily::kProjectionPruning,
      OptimizerTransformationFamily::kJoinRewrite,
      OptimizerTransformationFamily::kSubqueryDecorrelation,
      OptimizerTransformationFamily::kCteMaterialization,
      OptimizerTransformationFamily::kAggregatePushdown,
      OptimizerTransformationFamily::kWindowOrderReuse,
      OptimizerTransformationFamily::kMaterializedViewRewrite,
      OptimizerTransformationFamily::kRuntimeFilterPlacement,
      OptimizerTransformationFamily::kSqlNosqlFusion,
  };
}

// SEARCH_KEY: ValidateOptimizerTransformationMemoCoverageReport
OptimizerTransformationMemoValidation
ValidateOptimizerTransformationMemoCoverageReport(
    const OptimizerTransformationMemoCoverageReport& report) {
  OptimizerTransformationMemoValidation validation;
  const auto prefix = ReportPrefix(report);

  RequireField(&validation,
               report.schema_id == kOptimizerTransformationMemoSchemaId,
               "schema_id");
  RequireField(&validation,
               report.schema_version_major ==
                   kOptimizerTransformationMemoSchemaMajor,
               "schema_version_major");
  RequireField(&validation,
               report.schema_version_minor ==
                   kOptimizerTransformationMemoSchemaMinor,
               "schema_version_minor");
  RequireField(&validation, !Empty(report.memo_id), "memo_id");
  RequireField(&validation,
               !Empty(report.memo_generation_uuid),
               "memo_generation_uuid");
  RequireField(&validation, ValidProofHash(report.memo_digest),
               "memo_digest");
  RequireField(&validation,
               !Empty(report.optimizer_profile),
               "optimizer_profile");
  RequireField(&validation,
               ValidProofHash(report.canonical_rule_order_digest),
               "canonical_rule_order_digest");
  RequireField(&validation, ValidProofHash(report.memo_frontier_digest),
               "memo_frontier_digest");
  RequireField(&validation,
               ValidProofHash(report.equivalence_catalog_digest),
               "equivalence_catalog_digest");

  if (report.catalog_epoch <= 1 || report.security_epoch <= 1 ||
      report.redaction_epoch <= 1 || report.statistics_epoch <= 1 ||
      report.provider_generation <= 1) {
    AddDiagnostic(&validation,
                  prefix + ":SB_OPT_TRANSFORMATION_MEMO.PLACEHOLDER_EPOCH");
  }
  if (report.max_memo_groups == 0 ||
      report.max_alternatives_per_group == 0 ||
      report.max_rule_applications == 0 ||
      report.observed_memo_groups == 0 || report.observed_alternatives == 0 ||
      report.observed_rule_applications == 0 ||
      report.frontier_width_limit == 0 ||
      report.observed_memo_groups > report.max_memo_groups ||
      report.observed_alternatives >
          report.max_memo_groups * report.max_alternatives_per_group ||
      report.observed_rule_applications > report.max_rule_applications) {
    AddDiagnostic(&validation,
                  prefix +
                      ":SB_OPT_TRANSFORMATION_MEMO.MEMO_BOUNDS_INVALID");
  }
  if (!report.evidence_only || !report.deterministic_rule_order ||
      !report.bounded_memo || !report.bounded_frontier ||
      !report.memo_space_exhaustive_or_proven_bounded ||
      !report.semantic_canonicalization_proven ||
      report.transformations_claim_final_plan_authority) {
    AddDiagnostic(&validation,
                  prefix +
                      ":SB_OPT_TRANSFORMATION_MEMO.MEMO_NONDETERMINISTIC_OR_AUTHORITY");
  }
  if (!report.reference_reference_only || report.reference_as_authority ||
      report.uses_reference_storage_or_finality_for_scratchbird ||
      HasForbiddenAuthority(report.authority)) {
    AddDiagnostic(&validation,
                  prefix + ":SB_OPT_TRANSFORMATION_MEMO.FORBIDDEN_AUTHORITY");
  }
  if (report.cluster_mode ==
      OptimizerTransformationClusterMode::kLocalClusterEvidence) {
    AddDiagnostic(&validation,
                  prefix +
                      ":SB_OPT_TRANSFORMATION_MEMO.LOCAL_CLUSTER_FORBIDDEN");
  } else if (report.cluster_mode ==
             OptimizerTransformationClusterMode::kExternalProviderDelegated) {
    if (report.external_cluster_provider_id.empty() ||
        !report.cluster_claim_blocked ||
        report.production_transformation_claim) {
      AddDiagnostic(
          &validation,
          prefix +
              ":SB_OPT_TRANSFORMATION_MEMO.EXTERNAL_CLUSTER_CLAIM_BLOCK_REQUIRED");
    }
  }

  if (report.rule_evidence.empty()) {
    AddDiagnostic(&validation,
                  prefix + ":SB_OPT_TRANSFORMATION_MEMO.RULE_COVERAGE_MISSING");
  }
  if (report.rejected_transformations.empty()) {
    AddDiagnostic(&validation,
                  prefix +
                      ":SB_OPT_TRANSFORMATION_MEMO.REJECTION_DIAGNOSTIC_MISSING");
  }

  std::set<OptimizerTransformationFamily> accepted_families;
  std::set<std::uint64_t> rule_orders;
  std::set<std::string> memo_groups;
  for (const auto& rule : report.rule_evidence) {
    ValidateRule(&validation, rule);
    if (rule.accepted) accepted_families.insert(rule.family);
    if (rule.deterministic_rule_order != 0 &&
        !rule_orders.insert(rule.deterministic_rule_order).second) {
      AddDiagnostic(
          &validation,
          RulePrefix(rule) +
              ":SB_OPT_TRANSFORMATION_MEMO.DUPLICATE_RULE_ORDER");
    }
    if (!rule.memo_group_id.empty()) memo_groups.insert(rule.memo_group_id);
  }
  if (!memo_groups.empty() &&
      memo_groups.size() != report.observed_memo_groups) {
    AddDiagnostic(
        &validation,
        prefix +
            ":SB_OPT_TRANSFORMATION_MEMO.MEMO_GROUP_COUNT_MISMATCH");
  }
  if (report.rule_evidence.size() != report.observed_rule_applications) {
    AddDiagnostic(
        &validation,
        prefix +
            ":SB_OPT_TRANSFORMATION_MEMO.RULE_APPLICATION_COUNT_MISMATCH");
  }

  const auto canonical_required = RequiredOptimizerTransformationFamilies();
  const auto required = report.required_families.empty()
                            ? canonical_required
                            : report.required_families;
  for (const auto family : canonical_required) {
    if (!ContainsFamily(required, family)) {
      AddDiagnostic(
          &validation,
          std::string(OptimizerTransformationFamilyName(family)) +
              ":SB_OPT_TRANSFORMATION_MEMO.REQUIRED_FAMILY_LIST_INCOMPLETE");
    }
  }
  for (const auto family : required) {
    if (accepted_families.find(family) == accepted_families.end()) {
      AddDiagnostic(
          &validation,
          std::string(OptimizerTransformationFamilyName(family)) +
              ":SB_OPT_TRANSFORMATION_MEMO.TRANSFORMATION_FAMILY_MISSING");
    }
  }

  for (const auto& rejected : report.rejected_transformations) {
    ValidateRejectedTransformation(&validation, rejected);
  }

  validation.ok =
      validation.missing_fields.empty() && validation.diagnostics.empty();
  validation.coverage_proven =
      validation.ok && report.production_transformation_claim &&
      report.cluster_mode == OptimizerTransformationClusterMode::kNoCluster;
  validation.diagnostic_code =
      validation.ok ? "SB_OPT_TRANSFORMATION_MEMO.OK"
                    : "SB_OPT_TRANSFORMATION_MEMO.FAIL_CLOSED";
  return validation;
}

OptimizerTransformationMemoValidation
ValidateOptimizerTransformationMemoCoverageReportSet(
    const std::vector<OptimizerTransformationMemoCoverageReport>& reports,
    const std::vector<OptimizerTransformationFamily>& required_families) {
  OptimizerTransformationMemoValidation combined;
  std::set<OptimizerTransformationFamily> covered;

  if (reports.empty()) {
    AddDiagnostic(&combined,
                  "SB_OPT_TRANSFORMATION_MEMO.REPORT_SET_EMPTY");
  }
  for (const auto& report : reports) {
    auto validation = ValidateOptimizerTransformationMemoCoverageReport(report);
    combined.missing_fields.insert(combined.missing_fields.end(),
                                   validation.missing_fields.begin(),
                                   validation.missing_fields.end());
    combined.diagnostics.insert(combined.diagnostics.end(),
                                validation.diagnostics.begin(),
                                validation.diagnostics.end());
    if (validation.coverage_proven) {
      for (const auto& rule : report.rule_evidence) {
        if (rule.accepted) covered.insert(rule.family);
      }
    }
  }

  const auto required =
      required_families.empty() ? RequiredOptimizerTransformationFamilies()
                                : required_families;
  for (const auto family : required) {
    if (covered.find(family) == covered.end()) {
      AddDiagnostic(
          &combined,
          std::string(OptimizerTransformationFamilyName(family)) +
              ":SB_OPT_TRANSFORMATION_MEMO.REPORT_SET_FAMILY_MISSING");
    }
  }

  combined.ok =
      combined.missing_fields.empty() && combined.diagnostics.empty();
  combined.coverage_proven = combined.ok;
  combined.diagnostic_code =
      combined.ok ? "SB_OPT_TRANSFORMATION_MEMO.SET_OK"
                  : "SB_OPT_TRANSFORMATION_MEMO.SET_FAIL_CLOSED";
  return combined;
}

}  // namespace scratchbird::engine::optimizer
