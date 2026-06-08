// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_optimizer_recommendation.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace scratchbird::core::agents {
namespace {

void Add(std::vector<std::string>* evidence, std::string value) {
  evidence->push_back(std::move(value));
}

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

bool UnsafeAuthority(const AgentOptimizerRecommendationEvidence& input) {
  return input.parser_authority || input.client_authority ||
         input.donor_authority || input.sidecar_authority ||
         input.wal_authority || input.benchmark_authority ||
         input.transaction_authority || input.visibility_authority ||
         input.finality_authority || input.recovery_authority ||
         input.security_authority || input.optimizer_plan_authority ||
         input.index_finality_authority || input.provider_finality_authority ||
         input.cluster_authority || input.memory_authority ||
         input.agent_action_authority;
}

bool UnsafeAuthority(const AgentIndexReadinessEvidence& input) {
  return input.parser_authority || input.donor_authority ||
         input.wal_authority || input.benchmark_authority ||
         input.transaction_authority || input.visibility_authority ||
         input.finality_authority || input.recovery_authority ||
         input.security_authority || input.optimizer_plan_authority ||
         input.index_finality_authority || input.provider_finality_authority ||
         input.cluster_authority || input.agent_action_authority ||
         input.donor_runtime_claim || input.policy_blocked_runtime_claim ||
         input.all_index_readiness_claimed ||
         input.enterprise_readiness_claimed;
}

bool UnsafeAuthority(const AgentOptimizerReadinessEvidence& input) {
  return input.parser_authority || input.donor_authority ||
         input.wal_authority || input.benchmark_authority ||
         input.benchmark_dominance_claimed ||
         input.transaction_authority || input.visibility_authority ||
         input.finality_authority || input.recovery_authority ||
         input.security_authority || input.optimizer_plan_authority ||
         input.index_finality_authority || input.provider_finality_authority ||
         input.cluster_authority || input.memory_authority ||
         input.agent_action_authority || input.external_cluster_overclaim;
}

bool UnsafeAuthority(const AgentIndexOptimizerBoundaryRequest& request) {
  return request.optimizer_selected_plan || request.optimizer_plan_authority ||
         request.index_finality_authority || request.row_visibility_authority ||
         request.security_authority ||
         request.transaction_finality_authority || request.recovery_authority ||
         request.parser_authority || request.donor_authority ||
         request.wal_authority || request.benchmark_authority ||
         request.provider_finality_authority || request.cluster_authority ||
         request.memory_authority || request.agent_action_authority ||
         UnsafeAuthority(request.agent_evidence) ||
         UnsafeAuthority(request.index_readiness) ||
         UnsafeAuthority(request.optimizer_readiness);
}

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool RecommendationKindClaimsAuthority(std::string value) {
  value = Lower(std::move(value));
  const char* forbidden[] = {
      "optimizer_selected", "selected_plan", "plan_authority",
      "optimizer_plan_truth", "index_finality", "row_visibility",
      "security_authority", "transaction_finality", "finality_authority",
      "recovery_authority", "parser_authority", "donor_authority",
      "benchmark_authority", "provider_finality", "cluster_authority",
      "memory_authority", "agent_action_authority", "wal_authority"};
  for (const char* token : forbidden) {
    if (value.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool AllowedAction(AgentIndexOptimizerBoundaryActionKind action) {
  switch (action) {
    case AgentIndexOptimizerBoundaryActionKind::analyze_recommendation:
    case AgentIndexOptimizerBoundaryActionKind::index_rebuild_request:
    case AgentIndexOptimizerBoundaryActionKind::shadow_rollout_recommendation:
    case AgentIndexOptimizerBoundaryActionKind::cleanup_recommendation:
    case AgentIndexOptimizerBoundaryActionKind::statistics_refresh_recommendation:
    case AgentIndexOptimizerBoundaryActionKind::optimizer_learning_advisory_note:
      return true;
  }
  return false;
}

bool IndexReadinessMissing(const AgentIndexReadinessEvidence& input) {
  return !input.present || input.manifest_kind != "index_readiness_manifest" ||
         input.slice_id != "CEIC-042" || input.evidence_digest.empty() ||
         input.manifest_generation == 0 ||
         input.observed_manifest_generation == 0 ||
         !input.ceic_042_complete || !input.freshness_gate_complete ||
         !input.route_capability_complete ||
         !input.provider_closure_complete ||
         !input.metric_producer_complete ||
         !input.crash_cleanup_corruption_complete ||
         !input.artifact_registration_complete;
}

bool IndexReadinessStale(const AgentIndexReadinessEvidence& input) {
  return input.stale_manifest ||
         input.manifest_generation != input.observed_manifest_generation;
}

bool OptimizerReadinessMissing(const AgentOptimizerReadinessEvidence& input) {
  return !input.present ||
         input.manifest_kind != "optimizer_readiness_manifest" ||
         input.slice_id != "CEIC-062" || input.evidence_digest.empty() ||
         input.manifest_generation == 0 ||
         input.observed_manifest_generation == 0 ||
         !input.ceic_062_complete || !input.live_routes_complete ||
         !input.benchmark_evidence_complete ||
         !input.correctness_oracles_complete ||
         !input.crash_reopen_complete || !input.metrics_feedback_complete ||
         !input.transformation_memo_complete ||
         !input.workload_regression_complete ||
         !input.driver_explain_complete ||
         !input.donor_comparison_complete ||
         !input.memory_feedback_complete ||
         !input.index_readiness_coupling_complete ||
         !input.llvm_memory_accounting_complete;
}

bool OptimizerReadinessStale(const AgentOptimizerReadinessEvidence& input) {
  return input.stale_manifest ||
         input.manifest_generation != input.observed_manifest_generation;
}

bool ClusterEvidenceWithoutProvider(
    const AgentIndexOptimizerBoundaryRequest& request) {
  return (request.index_readiness.cluster_evidence_present &&
          !request.index_readiness.external_provider_proof) ||
         (request.optimizer_readiness.local_cluster_evidence_present &&
          !request.optimizer_readiness.external_cluster_provider_proof);
}

AgentOptimizerRecommendationValidation Finish(
    const AgentOptimizerRecommendationEvidence& input,
    bool ok,
    std::string code,
    std::string detail) {
  AgentOptimizerRecommendationValidation result;
  result.status = {ok, std::move(code), std::move(detail)};
  result.optimizer_visible = ok;
  result.fail_closed = !ok;
  result.redacted = input.protected_material ||
                    input.redaction_class == "protected_material" ||
                    input.redaction_class == "restricted";
  result.protected_material_suppressed = input.protected_material ||
                                         input.redaction_class == "protected_material";
  Add(&result.evidence, "ARHC_OPTIMIZER_RECOMMENDATION_EVIDENCE_CONTRACT");
  Add(&result.evidence, "agent_optimizer_recommendation.diagnostic_code=" +
                            result.status.diagnostic_code);
  Add(&result.evidence, "agent_optimizer_recommendation.agent_type_id=" +
                            input.agent_type_id);
  Add(&result.evidence, "agent_optimizer_recommendation.recommendation_kind=" +
                            input.recommendation_kind);
  Add(&result.evidence, "agent_optimizer_recommendation.durable_catalog_state=" +
                            BoolText(input.durable_catalog_state));
  Add(&result.evidence, "agent_optimizer_recommendation.strict_metric_snapshot=" +
                            BoolText(input.strict_metric_snapshot));
  Add(&result.evidence, "agent_optimizer_recommendation.metric_digest_present=" +
                            BoolText(!input.metric_digest.empty()));
  Add(&result.evidence, "agent_optimizer_recommendation.policy_generation=" +
                            std::to_string(input.policy_generation));
  Add(&result.evidence, "agent_optimizer_recommendation.scope_uuid_present=" +
                            BoolText(!input.scope_uuid.empty()));
  Add(&result.evidence, "agent_optimizer_recommendation.redacted=" +
                            BoolText(result.redacted));
  Add(&result.evidence,
      "agent_optimizer_recommendation.protected_material_suppressed=" +
          BoolText(result.protected_material_suppressed));
  Add(&result.evidence, "agent_optimizer_recommendation.parser_authority=false");
  Add(&result.evidence, "agent_optimizer_recommendation.client_authority=false");
  Add(&result.evidence, "agent_optimizer_recommendation.donor_authority=false");
  Add(&result.evidence, "agent_optimizer_recommendation.sidecar_authority=false");
  Add(&result.evidence, "agent_optimizer_recommendation.wal_authority=false");
  Add(&result.evidence, "agent_optimizer_recommendation.benchmark_authority=false");
  Add(&result.evidence, "agent_optimizer_recommendation.transaction_authority=false");
  Add(&result.evidence, "agent_optimizer_recommendation.visibility_authority=false");
  Add(&result.evidence, "agent_optimizer_recommendation.finality_authority=false");
  Add(&result.evidence, "agent_optimizer_recommendation.recovery_authority=false");
  Add(&result.evidence, "agent_optimizer_recommendation.security_authority=false");
  Add(&result.evidence, "agent_optimizer_recommendation.optimizer_plan_authority=false");
  Add(&result.evidence, "agent_optimizer_recommendation.index_finality_authority=false");
  Add(&result.evidence, "agent_optimizer_recommendation.provider_finality_authority=false");
  Add(&result.evidence, "agent_optimizer_recommendation.cluster_authority=false");
  Add(&result.evidence, "agent_optimizer_recommendation.memory_authority=false");
  Add(&result.evidence, "agent_optimizer_recommendation.agent_action_authority=false");
  return result;
}

AgentIndexOptimizerBoundaryResult FinishBoundary(
    const AgentIndexOptimizerBoundaryRequest& request,
    AgentOptimizerRecommendationValidation validation,
    bool ok,
    std::string code,
    std::string detail) {
  AgentIndexOptimizerBoundaryResult result;
  result.status = {ok, std::move(code), std::move(detail)};
  result.ok = ok;
  result.fail_closed = !ok;
  result.recommendation_only = true;
  result.support_bundle_rows_bounded =
      request.bounded_diagnostics_required &&
      request.support_bundle_rows_required;
  result.output_action = ok
                             ? request.requested_action
                             : AgentIndexOptimizerBoundaryActionKind::
                                   optimizer_learning_advisory_note;
  result.workflow_output =
      std::string("recommendation_only.") +
      AgentIndexOptimizerBoundaryActionKindName(result.output_action);
  result.agent_validation = std::move(validation);

  Add(&result.evidence, "CEIC_084_AGENT_INDEX_OPTIMIZER_BOUNDARY");
  Add(&result.evidence, "agent_index_optimizer_boundary.diagnostic_code=" +
                            result.status.diagnostic_code);
  Add(&result.evidence, "agent_index_optimizer_boundary.action=" +
                            std::string(AgentIndexOptimizerBoundaryActionKindName(
                                result.output_action)));
  Add(&result.evidence,
      "agent_index_optimizer_boundary.workflow_output=" +
          result.workflow_output);
  Add(&result.evidence,
      "agent_index_optimizer_boundary.recommendation_only=true");
  Add(&result.evidence,
      "agent_index_optimizer_boundary.support_bundle_rows_bounded=" +
          BoolText(result.support_bundle_rows_bounded));
  Add(&result.evidence,
      "agent_index_optimizer_boundary.index_manifest_kind=" +
          request.index_readiness.manifest_kind);
  Add(&result.evidence,
      "agent_index_optimizer_boundary.index_slice_id=" +
          request.index_readiness.slice_id);
  Add(&result.evidence,
      "agent_index_optimizer_boundary.index_digest_present=" +
          BoolText(!request.index_readiness.evidence_digest.empty()));
  Add(&result.evidence,
      "agent_index_optimizer_boundary.index_generation=" +
          std::to_string(request.index_readiness.manifest_generation));
  Add(&result.evidence,
      "agent_index_optimizer_boundary.optimizer_manifest_kind=" +
          request.optimizer_readiness.manifest_kind);
  Add(&result.evidence,
      "agent_index_optimizer_boundary.optimizer_slice_id=" +
          request.optimizer_readiness.slice_id);
  Add(&result.evidence,
      "agent_index_optimizer_boundary.optimizer_digest_present=" +
          BoolText(!request.optimizer_readiness.evidence_digest.empty()));
  Add(&result.evidence,
      "agent_index_optimizer_boundary.optimizer_generation=" +
          std::to_string(request.optimizer_readiness.manifest_generation));
  Add(&result.evidence,
      "agent_index_optimizer_boundary.optimizer_plan_authority=false");
  Add(&result.evidence,
      "agent_index_optimizer_boundary.optimizer_selected_plan=false");
  Add(&result.evidence,
      "agent_index_optimizer_boundary.index_finality_authority=false");
  Add(&result.evidence,
      "agent_index_optimizer_boundary.row_visibility_authority=false");
  Add(&result.evidence,
      "agent_index_optimizer_boundary.security_authority=false");
  Add(&result.evidence,
      "agent_index_optimizer_boundary.transaction_finality_authority=false");
  Add(&result.evidence,
      "agent_index_optimizer_boundary.recovery_authority=false");
  Add(&result.evidence,
      "agent_index_optimizer_boundary.parser_authority=false");
  Add(&result.evidence,
      "agent_index_optimizer_boundary.donor_authority=false");
  Add(&result.evidence,
      "agent_index_optimizer_boundary.wal_authority=false");
  Add(&result.evidence,
      "agent_index_optimizer_boundary.benchmark_authority=false");
  Add(&result.evidence,
      "agent_index_optimizer_boundary.provider_finality_authority=false");
  Add(&result.evidence,
      "agent_index_optimizer_boundary.cluster_authority=false");
  Add(&result.evidence,
      "agent_index_optimizer_boundary.memory_authority=false");
  Add(&result.evidence,
      "agent_index_optimizer_boundary.agent_action_authority=false");
  return result;
}

}  // namespace

AgentOptimizerRecommendationValidation ValidateAgentOptimizerRecommendationEvidence(
    const AgentOptimizerRecommendationEvidence& input) {
  if (input.recommendation_uuid.empty() || input.agent_type_id.empty() ||
      input.evidence_uuid.empty() || input.recommendation_kind.empty()) {
    return Finish(input, false,
                  "SB_AGENT_OPTIMIZER_RECOMMENDATION.IDENTITY_REQUIRED",
                  "optimizer recommendation identity and evidence are required");
  }
  if (!input.durable_catalog_state || input.in_memory_bootstrap_state ||
      input.sidecar_only_state) {
    return Finish(input, false,
                  "SB_AGENT_OPTIMIZER_RECOMMENDATION.DURABLE_STATE_REQUIRED",
                  "optimizer recommendations require durable agent runtime state");
  }
  if (!input.strict_metric_snapshot || input.relaxed_metric_path ||
      !input.metric_trusted || !input.metric_fresh ||
      input.metric_digest.empty()) {
    return Finish(input, false,
                  "SB_AGENT_OPTIMIZER_RECOMMENDATION.STRICT_METRIC_REQUIRED",
                  "optimizer recommendations require fresh trusted metric digest evidence");
  }
  if (input.scope_uuid.empty()) {
    return Finish(input, false,
                  "SB_AGENT_OPTIMIZER_RECOMMENDATION.SCOPE_REQUIRED",
                  "optimizer recommendations require a durable scope UUID");
  }
  if (input.policy_generation == 0 ||
      input.observed_policy_generation == 0 ||
      input.policy_generation != input.observed_policy_generation) {
    return Finish(input, false,
                  "SB_AGENT_OPTIMIZER_RECOMMENDATION.POLICY_GENERATION_REQUIRED",
                  "optimizer recommendations require current policy generation evidence");
  }
  if (UnsafeAuthority(input)) {
    return Finish(input, false,
                  "SB_AGENT_OPTIMIZER_RECOMMENDATION.UNSAFE_AUTHORITY",
                  "agent recommendations cannot provide engine authority");
  }
  return Finish(input, true,
                "SB_AGENT_OPTIMIZER_RECOMMENDATION.OK",
                "durable scoped metric-backed recommendation accepted as advisory evidence");
}

const char* AgentIndexOptimizerBoundaryActionKindName(
    AgentIndexOptimizerBoundaryActionKind action) {
  switch (action) {
    case AgentIndexOptimizerBoundaryActionKind::analyze_recommendation:
      return "analyze_recommendation";
    case AgentIndexOptimizerBoundaryActionKind::index_rebuild_request:
      return "index_rebuild_request";
    case AgentIndexOptimizerBoundaryActionKind::shadow_rollout_recommendation:
      return "shadow_rollout_recommendation";
    case AgentIndexOptimizerBoundaryActionKind::cleanup_recommendation:
      return "cleanup_recommendation";
    case AgentIndexOptimizerBoundaryActionKind::statistics_refresh_recommendation:
      return "statistics_refresh_recommendation";
    case AgentIndexOptimizerBoundaryActionKind::optimizer_learning_advisory_note:
      return "optimizer_learning_advisory_note";
  }
  return "refused";
}

AgentIndexOptimizerBoundaryResult EvaluateAgentIndexOptimizerBoundary(
    const AgentIndexOptimizerBoundaryRequest& request) {
  auto validation = ValidateAgentOptimizerRecommendationEvidence(
      request.agent_evidence);
  if (!validation.status.ok || !validation.optimizer_visible) {
    const auto diagnostic_code = validation.status.diagnostic_code;
    const auto diagnostic_detail = validation.status.detail;
    return FinishBoundary(
        request,
        std::move(validation),
        false,
        diagnostic_code,
        diagnostic_detail);
  }
  if (!AllowedAction(request.requested_action) ||
      !request.recommendation_only || !request.scheduling_output_allowed) {
    return FinishBoundary(
        request,
        std::move(validation),
        false,
        "SB_AGENT_INDEX_OPTIMIZER_BOUNDARY.ACTION_REFUSED",
        "only recommendation scheduling and coordination outputs are allowed");
  }
  if (request.workflow_uuid.empty()) {
    return FinishBoundary(
        request,
        std::move(validation),
        false,
        "SB_AGENT_INDEX_OPTIMIZER_BOUNDARY.WORKFLOW_REQUIRED",
        "recommendation-only index optimizer outputs require workflow identity");
  }
  if (RecommendationKindClaimsAuthority(
          request.agent_evidence.recommendation_kind)) {
    return FinishBoundary(
        request,
        std::move(validation),
        false,
        "SB_AGENT_INDEX_OPTIMIZER_BOUNDARY.UNSAFE_AUTHORITY",
        "agent recommendation kind attempted to claim final authority");
  }
  if (IndexReadinessMissing(request.index_readiness)) {
    return FinishBoundary(
        request,
        std::move(validation),
        false,
        "SB_AGENT_INDEX_OPTIMIZER_BOUNDARY.INDEX_READINESS_REQUIRED",
        "CEIC-042 index readiness evidence is required");
  }
  if (IndexReadinessStale(request.index_readiness)) {
    return FinishBoundary(
        request,
        std::move(validation),
        false,
        "SB_AGENT_INDEX_OPTIMIZER_BOUNDARY.INDEX_READINESS_STALE",
        "CEIC-042 index readiness evidence is stale");
  }
  if (request.index_readiness.static_or_smoke_only ||
      request.index_readiness.placeholder_evidence) {
    return FinishBoundary(
        request,
        std::move(validation),
        false,
        "SB_AGENT_INDEX_OPTIMIZER_BOUNDARY.INDEX_READINESS_PLACEHOLDER",
        "CEIC-042 index readiness cannot be placeholder or static-only proof");
  }
  if (OptimizerReadinessMissing(request.optimizer_readiness)) {
    return FinishBoundary(
        request,
        std::move(validation),
        false,
        "SB_AGENT_INDEX_OPTIMIZER_BOUNDARY.OPTIMIZER_READINESS_REQUIRED",
        "CEIC-062 optimizer readiness evidence is required");
  }
  if (OptimizerReadinessStale(request.optimizer_readiness)) {
    return FinishBoundary(
        request,
        std::move(validation),
        false,
        "SB_AGENT_INDEX_OPTIMIZER_BOUNDARY.OPTIMIZER_READINESS_STALE",
        "CEIC-062 optimizer readiness evidence is stale");
  }
  if (request.optimizer_readiness.static_only_proof ||
      request.optimizer_readiness.placeholder_runtime_evidence ||
      request.optimizer_readiness.synthetic_statistics ||
      request.optimizer_readiness.local_default_statistics ||
      request.optimizer_readiness.policy_default_statistics) {
    return FinishBoundary(
        request,
        std::move(validation),
        false,
        "SB_AGENT_INDEX_OPTIMIZER_BOUNDARY.OPTIMIZER_READINESS_PLACEHOLDER",
        "CEIC-062 optimizer readiness cannot use placeholder static or default-stat proof");
  }
  if (ClusterEvidenceWithoutProvider(request)) {
    return FinishBoundary(
        request,
        std::move(validation),
        false,
        "SB_AGENT_INDEX_OPTIMIZER_BOUNDARY.CLUSTER_PROVIDER_REQUIRED",
        "cluster evidence requires external provider proof");
  }
  if (UnsafeAuthority(request)) {
    return FinishBoundary(
        request,
        std::move(validation),
        false,
        "SB_AGENT_INDEX_OPTIMIZER_BOUNDARY.UNSAFE_AUTHORITY",
        "agent index optimizer boundary cannot provide final authority");
  }

  return FinishBoundary(
      request,
      std::move(validation),
      true,
      "SB_AGENT_INDEX_OPTIMIZER_BOUNDARY.OK",
      "agent recommendation admitted as recommendation-only index optimizer workflow output");
}

}  // namespace scratchbird::core::agents
