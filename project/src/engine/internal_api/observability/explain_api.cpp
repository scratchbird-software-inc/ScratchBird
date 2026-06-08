// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "observability/explain_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "query/plan_api.hpp"

namespace scratchbird::engine::internal_api {
namespace {

EnginePlanOperationRequest PlanRequestFromExplain(const EngineExplainOperationRequest& request) {
  EnginePlanOperationRequest plan;
  plan.context = request.context;
  plan.operation_id = "query.plan_operation";
  plan.execute = false;
  plan.target_database = request.target_database;
  plan.target_schema = request.target_schema;
  plan.target_object = request.target_object;
  plan.related_objects = request.related_objects;
  plan.bound_object_identity = request.bound_object_identity;
  plan.descriptors = request.descriptors;
  plan.rows = request.rows;
  plan.predicate = request.predicate;
  plan.projection = request.projection;
  plan.ordering = request.ordering;
  plan.option_envelopes = request.option_envelopes;
  if (request.operation_id == "query.join") {
    plan.query_operation = "join";
  } else if (request.operation_id == "query.aggregate") {
    plan.query_operation = "aggregate";
  } else if (request.operation_id == "query.window") {
    plan.query_operation = "window";
  } else if (request.operation_id == "query.set_operation") {
    plan.query_operation = "set_operation";
  } else if (request.operation_id == "query.cte_subquery") {
    plan.query_operation = "cte";
  } else {
    plan.query_operation = "scan";
  }
  return plan;
}

void CopyPlanEvidenceToExplain(const EnginePlanOperationResult& plan_result,
                               EngineExplainOperationResult* result) {
  result->evidence.insert(result->evidence.end(), plan_result.evidence.begin(), plan_result.evidence.end());
  result->evidence.push_back({"explain_plan_kind", plan_result.plan_kind});
  result->evidence.push_back({"explain_output_row_count", std::to_string(plan_result.output_row_count)});
  for (const auto& evidence : plan_result.evidence) {
    if (evidence.evidence_kind.rfind("optimizer_", 0) == 0 ||
        evidence.evidence_kind == "logical_plan_id" ||
        evidence.evidence_kind == "query_plan") {
      AddApiBehaviorRow(result,
                        {{"operation_id", result->operation_id},
                         {"plan_kind", plan_result.plan_kind},
                         {"evidence_kind", evidence.evidence_kind},
                         {"evidence_id", evidence.evidence_id}});
    }
  }
}

template <typename TResult>
TResult InvalidExplainSurfaceResult(const EngineRequestContext& context,
                                    const std::string& operation_id,
                                    const PerformanceOptimizationSurfaceValidationResult& validation) {
  return MakeApiBehaviorDiagnostic<TResult>(
      context,
      operation_id,
      MakeEngineApiDiagnostic(validation.diagnostic_code,
                              "observability.explain.invalid_performance_surface",
                              validation.detail,
                              true));
}

void AddExplainOdf108Surface(
    EngineApiResult* result,
    const PerformanceOptimizationSurfaceSnapshot& snapshot) {
  AddPerformanceOptimizationSurfaceRow(result, snapshot);
  AddApiBehaviorEvidence(result,
                         "management_explain_support_surface",
                         "ODF-108");
  AddApiBehaviorEvidence(result,
                         "explain_performance_optimization_surface",
                         PerformanceOptimizationSurfaceSchemaId());
  AddApiBehaviorEvidence(result,
                         "odf108_explain_selected_path_count",
                         std::to_string(snapshot.odf108_selected_paths.size()));
  AddApiBehaviorEvidence(result,
                         "odf108_explain_feature_gate_count",
                         std::to_string(snapshot.odf108_feature_gates.size()));
  AddApiBehaviorEvidence(result,
                         "odf108_explain_quota_state_count",
                         std::to_string(snapshot.odf108_quotas.size()));
  AddApiBehaviorEvidence(
      result,
      "odf108_explain_runtime_compatibility_count",
      std::to_string(snapshot.odf108_runtime_compatibility.size()));
  AddApiBehaviorEvidence(result, "parser_finality_authority", "false");
  AddApiBehaviorEvidence(result, "donor_finality_authority", "false");
  AddApiBehaviorEvidence(result, "wal_recovery_authority", "false");
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_OBSERVABILITY_EXPLAIN_API_BEHAVIOR
EngineExplainOperationResult EngineExplainOperation(const EngineExplainOperationRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineExplainOperationResult>(request.context, "observability.explain_operation");
  AddApiBehaviorEvidence(&result, "explain", request.operation_id.empty() ? request.target_object.object_kind : request.operation_id);
  const auto plan_result = EnginePlanOperation(PlanRequestFromExplain(request));
  if (!plan_result.ok) {
    result.ok = false;
    result.diagnostics = plan_result.diagnostics;
    result.evidence.insert(result.evidence.end(), plan_result.evidence.begin(), plan_result.evidence.end());
    return result;
  }
  CopyPlanEvidenceToExplain(plan_result, &result);
  const auto snapshot =
      request.performance_optimization_snapshot_present
          ? request.performance_optimization_snapshot
          : DefaultPerformanceOptimizationSurfaceSnapshot();
  const auto validation =
      ValidatePerformanceOptimizationSurfaceSnapshot(snapshot);
  if (!validation.ok) {
    return InvalidExplainSurfaceResult<EngineExplainOperationResult>(
        request.context,
        "observability.explain_operation",
        validation);
  }
  AddExplainOdf108Surface(&result, snapshot);
  AddApiBehaviorRow(&result, {{"operation_id", request.operation_id}, {"target_uuid", request.target_object.uuid.canonical}, {"plan_kind", plan_result.plan_kind}, {"payload", ApiBehaviorPayloadFromRequest(request)}});
  return result;
}

EngineExplainOptimizerEvidenceResult EngineExplainOptimizerEvidence(
    const EngineExplainOptimizerEvidenceRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineExplainOptimizerEvidenceResult>(request.context, "observability.explain_optimizer_evidence");
  AddApiBehaviorEvidence(&result, "optimizer_explain", request.operation_id.empty() ? "optimizer" : request.operation_id);
  const auto snapshot =
      request.performance_optimization_snapshot_present
          ? request.performance_optimization_snapshot
          : DefaultPerformanceOptimizationSurfaceSnapshot();
  const auto validation =
      ValidatePerformanceOptimizationSurfaceSnapshot(snapshot);
  if (!validation.ok) {
    return InvalidExplainSurfaceResult<EngineExplainOptimizerEvidenceResult>(
        request.context,
        "observability.explain_optimizer_evidence",
        validation);
  }
  AddExplainOdf108Surface(&result, snapshot);
  for (const auto& row : request.candidate_evidence_rows) {
    AddApiBehaviorRow(&result, {{"operation_id", request.operation_id}, {"candidate_evidence", row}});
  }
  if (request.candidate_evidence_rows.empty()) {
    AddApiBehaviorRow(&result, {{"operation_id", request.operation_id}, {"candidate_evidence", "no_candidates"}});
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
