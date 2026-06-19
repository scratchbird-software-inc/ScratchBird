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

std::string ExplainOptionValue(const EngineApiRequest& request,
                               const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    if (option.rfind(prefix, 0) == 0) {
      return option.substr(prefix.size());
    }
  }
  return {};
}

EngineApiU64 ExplainOptionU64(const EngineApiRequest& request,
                              const std::string& prefix) {
  const auto value = ExplainOptionValue(request, prefix);
  if (value.empty()) return 0;
  EngineApiU64 parsed = 0;
  for (const unsigned char ch : value) {
    if (ch < '0' || ch > '9') return 0;
    parsed = parsed * 10u + static_cast<EngineApiU64>(ch - '0');
  }
  return parsed;
}

bool ExplainOptionBool(const EngineApiRequest& request,
                       const std::string& prefix,
                       bool fallback = false) {
  const auto value = ExplainOptionValue(request, prefix);
  if (value.empty()) return fallback;
  return value == "true" || value == "1" || value == "yes" || value == "on";
}

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
  for (std::size_t index = 0; index < 16; ++index) {
    const std::string prefix = "related_object_" + std::to_string(index) + "_";
    EngineObjectReference related;
    related.uuid.canonical = ExplainOptionValue(request, prefix + "uuid:");
    related.object_kind = ExplainOptionValue(request, prefix + "kind:");
    if (!related.uuid.canonical.empty()) {
      if (related.object_kind.empty()) related.object_kind = "table";
      plan.related_objects.push_back(std::move(related));
    }
  }
  const std::string lowered_query_operation =
      ExplainOptionValue(request, "query_operation:");
  if (!lowered_query_operation.empty()) {
    plan.query_operation = lowered_query_operation;
  } else if (request.operation_id == "query.join") {
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
  plan.join_algorithm = ExplainOptionValue(request, "join_algorithm:");
  if (plan.join_algorithm.empty()) plan.join_algorithm = "hash";
  const auto set_operation = ExplainOptionValue(request, "set_operation:");
  if (!set_operation.empty()) plan.set_operation = set_operation;
  plan.set_by_name = ExplainOptionBool(request, "set_by_name:", false);
  plan.left_key_column = ExplainOptionU64(request, "left_key_column:");
  plan.right_key_column = ExplainOptionU64(request, "right_key_column:");
  plan.left_key_field = ExplainOptionValue(request, "left_key_field:");
  plan.right_key_field = ExplainOptionValue(request, "right_key_field:");
  plan.group_key_column = ExplainOptionU64(request, "group_key_column:");
  plan.aggregate_value_column = ExplainOptionU64(request, "aggregate_value_column:");
  plan.aggregate_pair_value_column =
      ExplainOptionU64(request, "aggregate_pair_value_column:");
  plan.group_key_field = ExplainOptionValue(request, "group_key_field:");
  plan.aggregate_value_field =
      ExplainOptionValue(request, "aggregate_value_field:");
  plan.aggregate_pair_value_field =
      ExplainOptionValue(request, "aggregate_pair_value_field:");
  const auto aggregate_function =
      ExplainOptionValue(request, "aggregate_function:");
  if (!aggregate_function.empty()) plan.aggregate_function = aggregate_function;
  plan.order_column = ExplainOptionU64(request, "order_column:");
  plan.order_field = ExplainOptionValue(request, "order_by:");
  const auto order_direction = ExplainOptionValue(request, "order_direction:");
  if (!order_direction.empty()) plan.ascending = order_direction != "desc";
  plan.window_function = ExplainOptionValue(request, "window_function:");
  if (plan.window_function.empty()) plan.window_function = "row_number";
  plan.window_value_column = ExplainOptionU64(request, "window_value_column:");
  plan.window_value_field = ExplainOptionValue(request, "window_value_field:");
  plan.partition_key_column = ExplainOptionU64(request, "partition_column:");
  plan.partition_key_field = ExplainOptionValue(request, "partition_by:");
  plan.window_n = ExplainOptionU64(request, "window_n:");
  if (plan.window_n == 0) {
    plan.window_n = ExplainOptionU64(request, "window_bucket_count:");
  }
  if (plan.window_n == 0) plan.window_n = 1;
  plan.limit = ExplainOptionU64(request, "limit:");
  plan.offset = ExplainOptionU64(request, "offset:");
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
  AddApiBehaviorEvidence(result, "reference_finality_authority", "false");
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
