// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: CEIC_055_OPTIMIZER_TRANSFORMATION_MEMO_COVERAGE
// Transformation memo evidence proves rewrite coverage and fail-closed
// diagnostics. It is optimizer evidence only. It is not transaction finality,
// visibility, authorization/security, recovery, parser, reference, WAL, benchmark,
// optimizer-plan, index-finality, provider-finality, cluster, or agent-action
// authority.
inline constexpr const char* kOptimizerTransformationMemoSchemaId =
    "sb.optimizer.transformation_memo_coverage.v1";
inline constexpr std::uint32_t kOptimizerTransformationMemoSchemaMajor = 1;
inline constexpr std::uint32_t kOptimizerTransformationMemoSchemaMinor = 0;

enum class OptimizerTransformationFamily {
  kPredicatePushdown,
  kProjectionPruning,
  kJoinRewrite,
  kSubqueryDecorrelation,
  kCteMaterialization,
  kAggregatePushdown,
  kWindowOrderReuse,
  kMaterializedViewRewrite,
  kRuntimeFilterPlacement,
  kSqlNosqlFusion,
};

enum class OptimizerTransformationClusterMode {
  kNoCluster,
  kExternalProviderDelegated,
  kLocalClusterEvidence,
};

struct OptimizerTransformationAuthorityFlags {
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

struct OptimizerTransformationRuleEvidence {
  OptimizerTransformationFamily family =
      OptimizerTransformationFamily::kPredicatePushdown;
  std::string rule_id;
  std::string rule_name;
  std::string memo_group_id;
  std::uint64_t deterministic_rule_order = 0;

  std::string input_logical_plan_hash;
  std::string output_logical_plan_hash;
  std::string equivalence_contract_hash;
  std::string result_contract_hash;
  std::string semantic_precondition_digest;
  std::string semantic_context_digest;
  std::string collation_contract_hash;
  std::string determinism_contract_hash;
  std::string redaction_contract_hash;
  std::string rewrite_legality_digest;
  std::string diagnostics_digest;
  std::string provider_route_capability_digest;
  std::string materialized_view_freshness_digest;
  std::string exact_recheck_digest;
  std::string runtime_filter_safety_digest;

  bool accepted = true;
  bool semantic_preconditions_proven = false;
  bool result_contract_preserved = false;
  bool mga_recheck_preserved = false;
  bool security_recheck_preserved = false;
  bool redaction_proof_present = false;
  bool collation_proof_present = false;
  bool determinism_proof_present = false;
  bool bounded_rule_expansion = false;
  bool deterministic_rule_position = false;
  bool memo_group_bounded = false;
  bool no_plan_authority_claim = true;
  bool no_parser_reference_authority = true;

  bool predicate_scope_proven = false;
  bool predicate_side_effect_free = false;
  bool null_semantics_preserved = false;
  bool projection_dependency_closure_proven = false;
  bool required_columns_preserved = false;
  bool join_legality_proven = false;
  bool row_multiplicity_preserved = false;
  bool correlation_dependency_proven = false;
  bool duplicate_semantics_preserved = false;
  bool cte_materialization_choice_bounded = false;
  bool cte_recursion_or_side_effect_proven = false;
  bool aggregate_grouping_semantics_proven = false;
  bool aggregate_partial_final_contract_proven = false;
  bool ordering_contract_preserved = false;
  bool window_frame_semantics_preserved = false;
  bool materialized_view_freshness_proven = false;
  bool materialized_view_security_proven = false;
  bool runtime_filter_false_negative_safe = false;
  bool runtime_filter_exact_recheck_preserved = false;
  bool fusion_provider_generation_proven = false;
  bool fusion_route_capability_proven = false;
  bool fusion_exact_recheck_preserved = false;

  bool unsafe_materialized_view_authority = false;
  bool unsafe_fusion_provider_authority = false;
  bool placeholder_evidence = false;
  bool reference_reference_only = true;
  bool reference_as_authority = false;
  bool uses_reference_storage_or_finality_for_scratchbird = false;
  OptimizerTransformationAuthorityFlags authority;
};

struct OptimizerRejectedTransformationDiagnostic {
  OptimizerTransformationFamily family =
      OptimizerTransformationFamily::kPredicatePushdown;
  std::string attempted_rule_id;
  std::string input_logical_plan_hash;
  std::string rejection_diagnostic_code;
  std::string reason_digest;
  std::string semantic_precondition_digest;
  bool rejected_fail_closed = false;
  bool no_memo_mutation = false;
  bool result_contract_unchanged = false;
  OptimizerTransformationAuthorityFlags authority;
};

struct OptimizerTransformationMemoCoverageReport {
  std::string schema_id = kOptimizerTransformationMemoSchemaId;
  std::uint32_t schema_version_major =
      kOptimizerTransformationMemoSchemaMajor;
  std::uint32_t schema_version_minor =
      kOptimizerTransformationMemoSchemaMinor;

  std::string memo_id;
  std::string memo_generation_uuid;
  std::string memo_digest;
  std::string optimizer_profile;
  std::string canonical_rule_order_digest;
  std::string memo_frontier_digest;
  std::string equivalence_catalog_digest;

  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::uint64_t statistics_epoch = 0;
  std::uint64_t provider_generation = 0;

  std::uint64_t max_memo_groups = 0;
  std::uint64_t max_alternatives_per_group = 0;
  std::uint64_t max_rule_applications = 0;
  std::uint64_t observed_memo_groups = 0;
  std::uint64_t observed_alternatives = 0;
  std::uint64_t observed_rule_applications = 0;
  std::uint64_t frontier_width_limit = 0;

  bool production_transformation_claim = false;
  bool evidence_only = true;
  bool deterministic_rule_order = false;
  bool bounded_memo = false;
  bool bounded_frontier = false;
  bool memo_space_exhaustive_or_proven_bounded = false;
  bool semantic_canonicalization_proven = false;
  bool transformations_claim_final_plan_authority = false;
  bool reference_reference_only = true;
  bool reference_as_authority = false;
  bool uses_reference_storage_or_finality_for_scratchbird = false;

  OptimizerTransformationClusterMode cluster_mode =
      OptimizerTransformationClusterMode::kNoCluster;
  std::string external_cluster_provider_id;
  bool cluster_claim_blocked = false;
  OptimizerTransformationAuthorityFlags authority;

  std::vector<OptimizerTransformationFamily> required_families;
  std::vector<OptimizerTransformationRuleEvidence> rule_evidence;
  std::vector<OptimizerRejectedTransformationDiagnostic>
      rejected_transformations;
};

struct OptimizerTransformationMemoValidation {
  bool ok = false;
  bool coverage_proven = false;
  std::string diagnostic_code;
  std::vector<std::string> missing_fields;
  std::vector<std::string> diagnostics;
};

const char* OptimizerTransformationFamilyName(
    OptimizerTransformationFamily family);

std::vector<OptimizerTransformationFamily>
RequiredOptimizerTransformationFamilies();

OptimizerTransformationMemoValidation
ValidateOptimizerTransformationMemoCoverageReport(
    const OptimizerTransformationMemoCoverageReport& report);

OptimizerTransformationMemoValidation
ValidateOptimizerTransformationMemoCoverageReportSet(
    const std::vector<OptimizerTransformationMemoCoverageReport>& reports,
    const std::vector<OptimizerTransformationFamily>& required_families);

}  // namespace scratchbird::engine::optimizer
