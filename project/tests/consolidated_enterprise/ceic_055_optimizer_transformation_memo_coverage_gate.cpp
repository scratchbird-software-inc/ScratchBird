// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-055 focused validation for optimizer transformation memo coverage.
#include "optimizer_transformation_memo_coverage.hpp"

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

std::string FamilyId(opt::OptimizerTransformationFamily family) {
  return opt::OptimizerTransformationFamilyName(family);
}

opt::OptimizerTransformationRuleEvidence Rule(
    opt::OptimizerTransformationFamily family,
    std::uint64_t order) {
  const auto id = FamilyId(family);
  opt::OptimizerTransformationRuleEvidence rule;
  rule.family = family;
  rule.rule_id = "ceic055-rule-" + id;
  rule.rule_name = "enterprise_" + id;
  rule.memo_group_id = "ceic055-group-" + id;
  rule.deterministic_rule_order = order;
  rule.input_logical_plan_hash = "sha256:ceic055-input-" + id;
  rule.output_logical_plan_hash = "sha256:ceic055-output-" + id;
  rule.equivalence_contract_hash = "sha256:ceic055-equiv-" + id;
  rule.result_contract_hash = "sha256:ceic055-result-" + id;
  rule.semantic_precondition_digest = "sha256:ceic055-precondition-" + id;
  rule.semantic_context_digest = "sha256:ceic055-context-" + id;
  rule.collation_contract_hash = "sha256:ceic055-collation-" + id;
  rule.determinism_contract_hash = "sha256:ceic055-determinism-" + id;
  rule.redaction_contract_hash = "sha256:ceic055-redaction-" + id;
  rule.rewrite_legality_digest = "sha256:ceic055-legality-" + id;
  rule.diagnostics_digest = "sha256:ceic055-diagnostic-" + id;
  rule.provider_route_capability_digest = "sha256:ceic055-provider-" + id;
  rule.materialized_view_freshness_digest = "sha256:ceic055-mv-" + id;
  rule.exact_recheck_digest = "sha256:ceic055-exact-" + id;
  rule.runtime_filter_safety_digest = "sha256:ceic055-runtime-filter-" + id;

  rule.accepted = true;
  rule.semantic_preconditions_proven = true;
  rule.result_contract_preserved = true;
  rule.mga_recheck_preserved = true;
  rule.security_recheck_preserved = true;
  rule.redaction_proof_present = true;
  rule.collation_proof_present = true;
  rule.determinism_proof_present = true;
  rule.bounded_rule_expansion = true;
  rule.deterministic_rule_position = true;
  rule.memo_group_bounded = true;

  rule.predicate_scope_proven = true;
  rule.predicate_side_effect_free = true;
  rule.null_semantics_preserved = true;
  rule.projection_dependency_closure_proven = true;
  rule.required_columns_preserved = true;
  rule.join_legality_proven = true;
  rule.row_multiplicity_preserved = true;
  rule.correlation_dependency_proven = true;
  rule.duplicate_semantics_preserved = true;
  rule.cte_materialization_choice_bounded = true;
  rule.cte_recursion_or_side_effect_proven = true;
  rule.aggregate_grouping_semantics_proven = true;
  rule.aggregate_partial_final_contract_proven = true;
  rule.ordering_contract_preserved = true;
  rule.window_frame_semantics_preserved = true;
  rule.materialized_view_freshness_proven = true;
  rule.materialized_view_security_proven = true;
  rule.runtime_filter_false_negative_safe = true;
  rule.runtime_filter_exact_recheck_preserved = true;
  rule.fusion_provider_generation_proven = true;
  rule.fusion_route_capability_proven = true;
  rule.fusion_exact_recheck_preserved = true;
  return rule;
}

opt::OptimizerRejectedTransformationDiagnostic Rejection() {
  opt::OptimizerRejectedTransformationDiagnostic rejected;
  rejected.family = opt::OptimizerTransformationFamily::kRuntimeFilterPlacement;
  rejected.attempted_rule_id = "ceic055-reject-runtime-filter-without-exact";
  rejected.input_logical_plan_hash = "sha256:ceic055-reject-input";
  rejected.rejection_diagnostic_code =
      "SB_OPT_TRANSFORMATION_MEMO.RUNTIME_FILTER_EXACT_RECHECK_REQUIRED";
  rejected.reason_digest = "sha256:ceic055-reject-reason";
  rejected.semantic_precondition_digest = "sha256:ceic055-reject-precondition";
  rejected.rejected_fail_closed = true;
  rejected.no_memo_mutation = true;
  rejected.result_contract_unchanged = true;
  return rejected;
}

opt::OptimizerTransformationMemoCoverageReport Report() {
  opt::OptimizerTransformationMemoCoverageReport report;
  report.memo_id = "ceic055-transformation-memo";
  report.memo_generation_uuid = "ceic055-generation-uuid";
  report.memo_digest = "sha256:ceic055-memo-digest";
  report.optimizer_profile = "enterprise";
  report.canonical_rule_order_digest = "sha256:ceic055-rule-order";
  report.memo_frontier_digest = "sha256:ceic055-frontier";
  report.equivalence_catalog_digest = "sha256:ceic055-equivalence-catalog";
  report.catalog_epoch = 5501;
  report.security_epoch = 5502;
  report.redaction_epoch = 5503;
  report.statistics_epoch = 5504;
  report.provider_generation = 5505;
  report.max_memo_groups = 64;
  report.max_alternatives_per_group = 16;
  report.max_rule_applications = 256;
  report.frontier_width_limit = 16;
  report.production_transformation_claim = true;
  report.deterministic_rule_order = true;
  report.bounded_memo = true;
  report.bounded_frontier = true;
  report.memo_space_exhaustive_or_proven_bounded = true;
  report.semantic_canonicalization_proven = true;
  report.required_families = opt::RequiredOptimizerTransformationFamilies();
  std::uint64_t order = 1;
  for (const auto family : report.required_families) {
    report.rule_evidence.push_back(Rule(family, order++));
  }
  report.observed_memo_groups = report.rule_evidence.size();
  report.observed_alternatives = report.rule_evidence.size();
  report.observed_rule_applications = report.rule_evidence.size();
  report.rejected_transformations = {Rejection()};
  return report;
}

void PositiveReportIsAdmissible() {
  const auto validation =
      opt::ValidateOptimizerTransformationMemoCoverageReportSet(
          {Report()}, opt::RequiredOptimizerTransformationFamilies());
  Require(validation.ok, "CEIC-055 valid memo coverage report was rejected");
  Require(validation.coverage_proven,
          "CEIC-055 valid memo coverage was not proven");
}

void SemanticAndResultContractsFailClosed() {
  auto report = Report();
  report.rule_evidence.front().equivalence_contract_hash = "";
  report.rule_evidence.front().result_contract_preserved = false;
  report.rule_evidence.front().mga_recheck_preserved = false;
  report.rule_evidence.front().security_recheck_preserved = false;
  const auto validation =
      opt::ValidateOptimizerTransformationMemoCoverageReport(report);
  Require(!validation.ok,
          "CEIC-055 missing equivalence/result/recheck proof was accepted");
  Require(HasDiagnostic(validation.diagnostics, "PLACEHOLDER_OR_HASH_MISSING"),
          "CEIC-055 missing equivalence diagnostic absent");
  Require(HasDiagnostic(validation.diagnostics, "SEMANTIC_PRECONDITION_MISSING"),
          "CEIC-055 missing semantic/recheck diagnostic absent");
}

void MemoBoundsAndRuleOrderFailClosed() {
  auto report = Report();
  report.bounded_memo = false;
  report.bounded_frontier = false;
  report.rule_evidence.front().bounded_rule_expansion = false;
  report.rule_evidence.front().deterministic_rule_position = false;
  report.rule_evidence[1].deterministic_rule_order =
      report.rule_evidence.front().deterministic_rule_order;
  const auto validation =
      opt::ValidateOptimizerTransformationMemoCoverageReport(report);
  Require(!validation.ok,
          "CEIC-055 unbounded/nondeterministic memo was accepted");
  Require(HasDiagnostic(validation.diagnostics,
                        "MEMO_NONDETERMINISTIC_OR_AUTHORITY"),
          "CEIC-055 memo nondeterministic diagnostic absent");
  Require(HasDiagnostic(validation.diagnostics,
                        "UNBOUNDED_OR_NONDETERMINISTIC_RULE"),
          "CEIC-055 unbounded rule diagnostic absent");
  Require(HasDiagnostic(validation.diagnostics, "DUPLICATE_RULE_ORDER"),
          "CEIC-055 duplicate order diagnostic absent");
}

void RequiredFamilyCoverageFailClosed() {
  auto report = Report();
  report.rule_evidence.pop_back();
  --report.observed_memo_groups;
  --report.observed_alternatives;
  --report.observed_rule_applications;
  const auto validation =
      opt::ValidateOptimizerTransformationMemoCoverageReport(report);
  Require(!validation.ok, "CEIC-055 family coverage gap was accepted");
  Require(HasDiagnostic(validation.diagnostics,
                        "TRANSFORMATION_FAMILY_MISSING"),
          "CEIC-055 missing family diagnostic absent");

  auto incomplete_list = Report();
  incomplete_list.required_families.pop_back();
  const auto list_validation =
      opt::ValidateOptimizerTransformationMemoCoverageReport(incomplete_list);
  Require(!list_validation.ok,
          "CEIC-055 incomplete required-family list was accepted");
  Require(HasDiagnostic(list_validation.diagnostics,
                        "REQUIRED_FAMILY_LIST_INCOMPLETE"),
          "CEIC-055 required-family list diagnostic absent");
}

void FamilySpecificProofsFailClosed() {
  auto mv = Report();
  for (auto& rule : mv.rule_evidence) {
    if (rule.family ==
        opt::OptimizerTransformationFamily::kMaterializedViewRewrite) {
      rule.materialized_view_freshness_proven = false;
      rule.unsafe_materialized_view_authority = true;
    }
  }
  const auto mv_validation =
      opt::ValidateOptimizerTransformationMemoCoverageReport(mv);
  Require(!mv_validation.ok, "CEIC-055 unsafe MV rewrite was accepted");
  Require(HasDiagnostic(mv_validation.diagnostics,
                        "MATERIALIZED_VIEW_REWRITE_PROOF_MISSING"),
          "CEIC-055 MV diagnostic absent");

  auto fusion = Report();
  for (auto& rule : fusion.rule_evidence) {
    if (rule.family == opt::OptimizerTransformationFamily::kSqlNosqlFusion) {
      rule.fusion_route_capability_proven = false;
      rule.unsafe_fusion_provider_authority = true;
      rule.authority.provider_finality_authority = true;
    }
  }
  const auto fusion_validation =
      opt::ValidateOptimizerTransformationMemoCoverageReport(fusion);
  Require(!fusion_validation.ok, "CEIC-055 unsafe fusion was accepted");
  Require(HasDiagnostic(fusion_validation.diagnostics,
                        "SQL_NOSQL_FUSION_PROOF_MISSING"),
          "CEIC-055 fusion diagnostic absent");
  Require(HasDiagnostic(fusion_validation.diagnostics, "FORBIDDEN_AUTHORITY"),
          "CEIC-055 fusion authority diagnostic absent");
}

void RejectionDiagnosticsFailClosed() {
  auto report = Report();
  report.rejected_transformations.clear();
  const auto missing =
      opt::ValidateOptimizerTransformationMemoCoverageReport(report);
  Require(!missing.ok, "CEIC-055 missing rejection diagnostics were accepted");
  Require(HasDiagnostic(missing.diagnostics, "REJECTION_DIAGNOSTIC_MISSING"),
          "CEIC-055 missing rejection diagnostic absent");

  report = Report();
  report.rejected_transformations.front().rejected_fail_closed = false;
  report.rejected_transformations.front().no_memo_mutation = false;
  const auto unsafe =
      opt::ValidateOptimizerTransformationMemoCoverageReport(report);
  Require(!unsafe.ok,
          "CEIC-055 unsafe rejection diagnostic was accepted");
  Require(HasDiagnostic(unsafe.diagnostics, "REJECTION_NOT_FAIL_CLOSED"),
          "CEIC-055 unsafe rejection diagnostic absent");
}

void AuthorityAndPlaceholderEvidenceFailClosed() {
  auto report = Report();
  report.authority.parser_authority = true;
  report.authority.wal_authority = true;
  report.transformations_claim_final_plan_authority = true;
  report.rule_evidence.front().placeholder_evidence = true;
  report.rule_evidence.front().result_contract_hash = "result-contract-v1";
  report.catalog_epoch = 1;
  report.provider_generation = 1;
  const auto validation =
      opt::ValidateOptimizerTransformationMemoCoverageReport(report);
  Require(!validation.ok,
          "CEIC-055 placeholder or authority evidence was accepted");
  Require(HasDiagnostic(validation.diagnostics, "FORBIDDEN_AUTHORITY"),
          "CEIC-055 forbidden authority diagnostic absent");
  Require(HasDiagnostic(validation.diagnostics, "PLACEHOLDER_OR_HASH_MISSING"),
          "CEIC-055 placeholder hash diagnostic absent");
  Require(HasDiagnostic(validation.diagnostics, "PLACEHOLDER_EPOCH"),
          "CEIC-055 placeholder epoch diagnostic absent");
}

void ClusterModesRemainClaimBlocked() {
  auto local = Report();
  local.cluster_mode =
      opt::OptimizerTransformationClusterMode::kLocalClusterEvidence;
  const auto local_validation =
      opt::ValidateOptimizerTransformationMemoCoverageReport(local);
  Require(!local_validation.ok,
          "CEIC-055 local cluster transformation was accepted");
  Require(HasDiagnostic(local_validation.diagnostics,
                        "LOCAL_CLUSTER_FORBIDDEN"),
          "CEIC-055 local cluster diagnostic absent");

  auto external = Report();
  external.cluster_mode =
      opt::OptimizerTransformationClusterMode::kExternalProviderDelegated;
  external.external_cluster_provider_id = "external-cluster-provider";
  external.cluster_claim_blocked = true;
  external.production_transformation_claim = false;
  const auto external_validation =
      opt::ValidateOptimizerTransformationMemoCoverageReport(external);
  Require(external_validation.ok,
          "CEIC-055 blocked external cluster delegation was rejected");
  Require(!external_validation.coverage_proven,
          "CEIC-055 external cluster delegation became coverage proof");

  external.production_transformation_claim = true;
  const auto overclaim =
      opt::ValidateOptimizerTransformationMemoCoverageReport(external);
  Require(!overclaim.ok, "CEIC-055 external cluster overclaim was accepted");
  Require(HasDiagnostic(overclaim.diagnostics,
                        "EXTERNAL_CLUSTER_CLAIM_BLOCK_REQUIRED"),
          "CEIC-055 external cluster overclaim diagnostic absent");
}

}  // namespace

int main() {
  PositiveReportIsAdmissible();
  SemanticAndResultContractsFailClosed();
  MemoBoundsAndRuleOrderFailClosed();
  RequiredFamilyCoverageFailClosed();
  FamilySpecificProofsFailClosed();
  RejectionDiagnosticsFailClosed();
  AuthorityAndPlaceholderEvidenceFailClosed();
  ClusterModesRemainClaimBlocked();
  std::cout << "ceic_055_optimizer_transformation_memo_coverage_gate=pass\n";
  return EXIT_SUCCESS;
}
