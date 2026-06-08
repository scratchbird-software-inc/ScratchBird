// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: ARHC_OPTIMIZER_RECOMMENDATION_EVIDENCE_CONTRACT
// Agent recommendations consumed by the optimizer are advisory evidence only.
// They are not optimizer, parser, donor, transaction, visibility, recovery, or
// security authority.

#include "agent_runtime.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents {

// SEARCH_KEY: CEIC_084_AGENT_INDEX_OPTIMIZER_BOUNDARY
// This boundary consumes CEIC-042 index readiness and CEIC-062 optimizer
// readiness evidence before allowing agent recommendations to reach
// index/optimizer maintenance workflows. Outputs are recommendation-only.

enum class AgentIndexOptimizerBoundaryActionKind {
  analyze_recommendation,
  index_rebuild_request,
  shadow_rollout_recommendation,
  cleanup_recommendation,
  statistics_refresh_recommendation,
  optimizer_learning_advisory_note
};

struct AgentOptimizerRecommendationEvidence {
  std::string recommendation_uuid;
  std::string agent_type_id;
  std::string evidence_uuid;
  std::string metric_digest;
  std::string scope_uuid;
  std::string recommendation_kind;
  std::string redaction_class = "standard";
  std::string principal_uuid;
  u64 policy_generation = 0;
  u64 observed_policy_generation = 0;
  bool durable_catalog_state = false;
  bool in_memory_bootstrap_state = false;
  bool sidecar_only_state = false;
  bool strict_metric_snapshot = false;
  bool relaxed_metric_path = false;
  bool metric_trusted = false;
  bool metric_fresh = false;
  bool protected_material = false;
  bool parser_authority = false;
  bool client_authority = false;
  bool donor_authority = false;
  bool sidecar_authority = false;
  bool wal_authority = false;
  bool benchmark_authority = false;
  bool transaction_authority = false;
  bool visibility_authority = false;
  bool finality_authority = false;
  bool recovery_authority = false;
  bool security_authority = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool provider_finality_authority = false;
  bool cluster_authority = false;
  bool memory_authority = false;
  bool agent_action_authority = false;
};

struct AgentIndexReadinessEvidence {
  std::string manifest_kind = "index_readiness_manifest";
  std::string slice_id = "CEIC-042";
  std::string evidence_digest;
  std::string family_id;
  u64 manifest_generation = 0;
  u64 observed_manifest_generation = 0;
  bool present = false;
  bool ceic_042_complete = false;
  bool freshness_gate_complete = false;
  bool route_capability_complete = false;
  bool provider_closure_complete = false;
  bool metric_producer_complete = false;
  bool crash_cleanup_corruption_complete = false;
  bool artifact_registration_complete = false;
  bool stale_manifest = false;
  bool static_or_smoke_only = false;
  bool placeholder_evidence = false;
  bool donor_runtime_claim = false;
  bool policy_blocked_runtime_claim = false;
  bool all_index_readiness_claimed = false;
  bool enterprise_readiness_claimed = false;
  bool cluster_evidence_present = false;
  bool external_provider_proof = false;
  bool parser_authority = false;
  bool donor_authority = false;
  bool wal_authority = false;
  bool benchmark_authority = false;
  bool transaction_authority = false;
  bool visibility_authority = false;
  bool finality_authority = false;
  bool recovery_authority = false;
  bool security_authority = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool provider_finality_authority = false;
  bool cluster_authority = false;
  bool agent_action_authority = false;
};

struct AgentOptimizerReadinessEvidence {
  std::string manifest_kind = "optimizer_readiness_manifest";
  std::string slice_id = "CEIC-062";
  std::string evidence_digest;
  u64 manifest_generation = 0;
  u64 observed_manifest_generation = 0;
  bool present = false;
  bool ceic_062_complete = false;
  bool live_routes_complete = false;
  bool benchmark_evidence_complete = false;
  bool correctness_oracles_complete = false;
  bool crash_reopen_complete = false;
  bool metrics_feedback_complete = false;
  bool transformation_memo_complete = false;
  bool workload_regression_complete = false;
  bool driver_explain_complete = false;
  bool donor_comparison_complete = false;
  bool memory_feedback_complete = false;
  bool index_readiness_coupling_complete = false;
  bool llvm_memory_accounting_complete = false;
  bool stale_manifest = false;
  bool static_only_proof = false;
  bool placeholder_runtime_evidence = false;
  bool synthetic_statistics = false;
  bool local_default_statistics = false;
  bool policy_default_statistics = false;
  bool local_cluster_evidence_present = false;
  bool external_cluster_provider_proof = false;
  bool external_cluster_overclaim = false;
  bool parser_authority = false;
  bool donor_authority = false;
  bool wal_authority = false;
  bool benchmark_authority = false;
  bool benchmark_dominance_claimed = false;
  bool transaction_authority = false;
  bool visibility_authority = false;
  bool finality_authority = false;
  bool recovery_authority = false;
  bool security_authority = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool provider_finality_authority = false;
  bool cluster_authority = false;
  bool memory_authority = false;
  bool agent_action_authority = false;
};

struct AgentOptimizerRecommendationValidation {
  AgentRuntimeStatus status;
  bool optimizer_visible = false;
  bool fail_closed = true;
  bool redacted = false;
  bool protected_material_suppressed = false;
  std::vector<std::string> evidence;
};

struct AgentIndexOptimizerBoundaryRequest {
  AgentOptimizerRecommendationEvidence agent_evidence;
  AgentIndexReadinessEvidence index_readiness;
  AgentOptimizerReadinessEvidence optimizer_readiness;
  AgentIndexOptimizerBoundaryActionKind requested_action =
      AgentIndexOptimizerBoundaryActionKind::optimizer_learning_advisory_note;
  std::string workflow_uuid;
  bool recommendation_only = true;
  bool scheduling_output_allowed = true;
  bool bounded_diagnostics_required = true;
  bool support_bundle_rows_required = true;
  bool optimizer_selected_plan = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool row_visibility_authority = false;
  bool security_authority = false;
  bool transaction_finality_authority = false;
  bool recovery_authority = false;
  bool parser_authority = false;
  bool donor_authority = false;
  bool wal_authority = false;
  bool benchmark_authority = false;
  bool provider_finality_authority = false;
  bool cluster_authority = false;
  bool memory_authority = false;
  bool agent_action_authority = false;
};

struct AgentIndexOptimizerBoundaryResult {
  AgentRuntimeStatus status;
  bool ok = false;
  bool fail_closed = true;
  bool recommendation_only = true;
  bool support_bundle_rows_bounded = false;
  bool optimizer_plan_authority = false;
  bool optimizer_selected_plan = false;
  bool index_finality_authority = false;
  bool row_visibility_authority = false;
  bool security_authority = false;
  bool transaction_finality_authority = false;
  bool recovery_authority = false;
  AgentIndexOptimizerBoundaryActionKind output_action =
      AgentIndexOptimizerBoundaryActionKind::optimizer_learning_advisory_note;
  std::string workflow_output;
  AgentOptimizerRecommendationValidation agent_validation;
  std::vector<std::string> evidence;
};

AgentOptimizerRecommendationValidation ValidateAgentOptimizerRecommendationEvidence(
    const AgentOptimizerRecommendationEvidence& input);

const char* AgentIndexOptimizerBoundaryActionKindName(
    AgentIndexOptimizerBoundaryActionKind action);

AgentIndexOptimizerBoundaryResult EvaluateAgentIndexOptimizerBoundary(
    const AgentIndexOptimizerBoundaryRequest& request);

}  // namespace scratchbird::core::agents
