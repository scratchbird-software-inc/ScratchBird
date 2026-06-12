// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_relational_operator_enterprise.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace planner = scratchbird::engine::planner;
namespace {

void Add(std::vector<std::string>* values, std::string value) {
  values->push_back(std::move(value));
}

bool UnsafeAuthority(const EnterpriseRelationalOperatorAuthority& authority) {
  return !authority.engine_optimizer_authority ||
         authority.parser_or_sql_authority ||
         authority.reference_or_legacy_authority ||
         authority.client_finality_or_visibility_authority ||
         authority.metric_finality_or_visibility_authority ||
         authority.recovery_authority ||
         authority.cluster_authority ||
         authority.fixture_or_test_authority;
}

bool Materializes(planner::PhysicalAccessKind access_kind) {
  return access_kind == planner::PhysicalAccessKind::kJoinHash ||
         access_kind == planner::PhysicalAccessKind::kAggregateHash ||
         access_kind == planner::PhysicalAccessKind::kSort ||
         access_kind == planner::PhysicalAccessKind::kTopN ||
         access_kind == planner::PhysicalAccessKind::kSortThenWindow ||
         access_kind == planner::PhysicalAccessKind::kCteMaterialize;
}

EnterpriseRelationalOperatorResult Refuse(std::string code,
                                          std::string evidence) {
  EnterpriseRelationalOperatorResult result;
  result.ok = false;
  result.fail_closed = true;
  result.diagnostic_code = std::move(code);
  result.diagnostics.push_back(result.diagnostic_code);
  result.evidence.push_back(std::move(evidence));
  result.evidence.push_back("enterprise_relational_operator.accepted=false");
  return result;
}

void FinishAccepted(const EnterpriseRelationalOperatorRequest& request,
                    EnterpriseRelationalOperatorResult* result) {
  result->ok = true;
  result->fail_closed = false;
  result->diagnostic_code = "SB_OPT_ENTERPRISE_RELATIONAL_OPERATOR_ACCEPTED";
  Add(&result->evidence, "enterprise_relational_operator.accepted=true");
  Add(&result->evidence, std::string("enterprise_relational_operator.kind=") +
                         EnterpriseRelationalOperatorKindName(request.kind));
  Add(&result->evidence, "enterprise_relational_operator.route_label=" +
                         request.route_label);
  Add(&result->evidence, "enterprise_relational_operator.plan_node_id=" +
                         request.plan_node_id);
  Add(&result->evidence, "enterprise_relational_operator.result_contract_hash=" +
                         request.result_contract_hash);
  Add(&result->evidence, "enterprise_relational_operator.parser_authority=false");
  Add(&result->evidence, "enterprise_relational_operator.reference_authority=false");
  Add(&result->evidence, "enterprise_relational_operator.cluster_authority=false");
  Add(&result->evidence, "enterprise_relational_operator.transaction_visibility_proof=true");
  Add(&result->evidence, "enterprise_relational_operator.memory_budget_bytes=" +
                         std::to_string(request.memory_budget_bytes));
  Add(&result->evidence, std::string("enterprise_relational_operator.access_kind=") +
                         planner::PhysicalAccessKindName(result->access_kind));
  Add(&result->evidence, std::string("enterprise_relational_operator.materializes=") +
                         (result->materializes ? "true" : "false"));
  Add(&result->evidence, std::string("enterprise_relational_operator.feedback_applied=") +
                         (result->feedback_applied ? "true" : "false"));
}

EnterpriseRelationalOperatorResult FromDecision(
    const EnterpriseRelationalOperatorRequest& request,
    RelationalPlanDecision decision) {
  EnterpriseRelationalOperatorResult result;
  result.ok = decision.ok;
  result.fail_closed = !decision.ok;
  result.access_kind = decision.access_kind;
  result.cost = decision.cost;
  result.diagnostics = decision.diagnostics;
  result.materializes = Materializes(decision.access_kind);
  if (!decision.ok) {
    result.diagnostic_code = decision.diagnostics.empty()
                                 ? "SB_OPT_ENTERPRISE_RELATIONAL_OPERATOR_REFUSED"
                                 : decision.diagnostics.front();
    Add(&result.evidence, "enterprise_relational_operator.accepted=false");
    return result;
  }
  FinishAccepted(request, &result);
  return result;
}

void ApplyFeedbackIfPresent(const EnterpriseRelationalOperatorRequest& request,
                            EnterpriseRelationalOperatorResult* result) {
  if (!request.feedback_present || result == nullptr || !result->ok) return;
  const auto status = EvaluateOptimizerRuntimeFeedback(request.runtime_feedback);
  if (!status.ok || !status.applied) {
    *result = Refuse(status.diagnostic_code, "runtime_feedback_rejected_for_relational_operator");
    return;
  }
  result->cost = ApplyOptimizerRuntimeFeedbackCost(result->cost,
                                                   request.runtime_feedback);
  result->feedback_applied = true;
  Add(&result->evidence, "enterprise_relational_operator.feedback_applied=true");
  Add(&result->evidence, "enterprise_relational_operator.feedback_authority=advisory_only");
}

bool CommonScopeMissing(const EnterpriseRelationalOperatorRequest& request) {
  return request.route_label.empty() ||
         request.plan_node_id.empty() ||
         request.result_contract_hash.empty() ||
         request.memory_budget_bytes == 0;
}

AggregatePlanningInput AggregateInput(const EnterpriseRelationalOperatorRequest& request,
                                      bool distinct) {
  AggregatePlanningInput input;
  input.input_rows = request.input_rows;
  input.group_count = request.group_count;
  input.row_width_bytes = request.row_width_bytes;
  input.memory_budget_bytes = request.memory_budget_bytes;
  input.grouping_present = request.grouping_present || distinct;
  input.distinct_present = distinct;
  input.input_ordered_by_group = request.input_ordered;
  input.group_expression_ids = request.group_expression_ids;
  input.property_metadata = request.property_metadata;
  input.proof = request.proof;
  return input;
}

}  // namespace

const char* EnterpriseRelationalOperatorKindName(EnterpriseRelationalOperatorKind kind) {
  switch (kind) {
    case EnterpriseRelationalOperatorKind::kAggregate: return "aggregate";
    case EnterpriseRelationalOperatorKind::kDistinctAggregate: return "distinct_aggregate";
    case EnterpriseRelationalOperatorKind::kSort: return "sort";
    case EnterpriseRelationalOperatorKind::kTopN: return "top_n";
    case EnterpriseRelationalOperatorKind::kWindow: return "window";
    case EnterpriseRelationalOperatorKind::kSetOperation: return "set_operation";
    case EnterpriseRelationalOperatorKind::kCte: return "cte";
    case EnterpriseRelationalOperatorKind::kMutation: return "mutation";
    case EnterpriseRelationalOperatorKind::kResultStreaming: return "result_streaming";
  }
  return "aggregate";
}

EnterpriseRelationalOperatorResult PlanEnterpriseRelationalOperator(
    const EnterpriseRelationalOperatorRequest& request) {
  // SEARCH_KEY: OEIC_RELATIONAL_OPERATOR_ENTERPRISE_CLOSURE
  if (UnsafeAuthority(request.authority)) {
    return Refuse("SB_OPT_ENTERPRISE_RELATIONAL_OPERATOR_UNSAFE_AUTHORITY",
                  "unsafe_authority");
  }
  if (CommonScopeMissing(request)) {
    return Refuse("SB_OPT_ENTERPRISE_RELATIONAL_OPERATOR_SCOPE_REQUIRED",
                  "route_plan_result_contract_and_memory_budget_required");
  }
  if (!request.proof.transaction_proof_present ||
      !request.proof.visibility_proof_present ||
      !request.proof.memory_budget_present) {
    return Refuse("SB_OPT_ENTERPRISE_RELATIONAL_OPERATOR_PROOF_REQUIRED",
                  "transaction_visibility_and_memory_proof_required");
  }
  if (request.proof.parser_or_sql_authority_claimed) {
    return Refuse("SB_OPT_ENTERPRISE_RELATIONAL_OPERATOR_UNSAFE_AUTHORITY",
                  "parser_or_sql_proof_authority");
  }
  if (!planner::LogicalPlanPropertyMetadataSafe(request.property_metadata)) {
    return Refuse("SB_OPT_ENTERPRISE_RELATIONAL_OPERATOR_UNSAFE_PROPERTY_METADATA",
                  "unsafe_logical_property_metadata");
  }

  EnterpriseRelationalOperatorResult result;
  switch (request.kind) {
    case EnterpriseRelationalOperatorKind::kAggregate:
      result = FromDecision(request, PlanAggregate(AggregateInput(request, false)));
      break;
    case EnterpriseRelationalOperatorKind::kDistinctAggregate:
      result = FromDecision(request, PlanAggregate(AggregateInput(request, true)));
      break;
    case EnterpriseRelationalOperatorKind::kSort: {
      SortPlanningInput input;
      input.input_rows = request.input_rows;
      input.row_width_bytes = request.row_width_bytes;
      input.memory_budget_bytes = request.memory_budget_bytes;
      input.input_already_ordered = request.input_ordered;
      input.required_ordering = request.ordering_terms;
      input.property_metadata = request.property_metadata;
      input.proof = request.proof;
      result = FromDecision(request, PlanSortLimit(input));
      break;
    }
    case EnterpriseRelationalOperatorKind::kTopN: {
      SortPlanningInput input;
      input.input_rows = request.input_rows;
      input.row_width_bytes = request.row_width_bytes;
      input.memory_budget_bytes = request.memory_budget_bytes;
      input.input_already_ordered = request.input_ordered;
      input.limit_present = true;
      input.limit_count = request.limit_count == 0 ? request.input_rows : request.limit_count;
      input.required_ordering = request.ordering_terms;
      input.property_metadata = request.property_metadata;
      input.proof = request.proof;
      result = FromDecision(request, PlanSortLimit(input));
      break;
    }
    case EnterpriseRelationalOperatorKind::kWindow: {
      WindowPlanningInput input;
      input.input_rows = request.input_rows;
      input.partition_count = request.partition_count;
      input.input_ordered = request.input_ordered;
      input.partition_expression_ids = request.group_expression_ids;
      input.ordering_terms = request.ordering_terms;
      input.property_metadata = request.property_metadata;
      input.proof = request.proof;
      result = FromDecision(request, PlanWindow(input));
      break;
    }
    case EnterpriseRelationalOperatorKind::kSetOperation: {
      SetOperationPlanningInput input;
      input.left_rows = request.input_rows;
      input.right_rows = request.right_rows;
      input.distinct = request.set_distinct;
      input.inputs_ordered_compatibly = request.input_ordered;
      input.row_width_bytes = request.row_width_bytes;
      input.memory_budget_bytes = request.memory_budget_bytes;
      input.property_metadata = request.property_metadata;
      input.proof = request.proof;
      result = FromDecision(request, PlanSetOperation(input));
      break;
    }
    case EnterpriseRelationalOperatorKind::kCte: {
      result.ok = true;
      result.fail_closed = false;
      result.access_kind = (request.cte_reused || request.volatile_or_side_effecting ||
                            request.input_rows * std::max<std::uint64_t>(request.row_width_bytes, 8) >
                                request.memory_budget_bytes)
                               ? planner::PhysicalAccessKind::kCteMaterialize
                               : planner::PhysicalAccessKind::kCteInline;
      result.cost = EstimateNodeCost(planner::MakeLogicalPlanNode(
          planner::LogicalPlanNodeKind::kDmlRead, result.access_kind,
          "query.cte", "enterprise_cte"));
      result.cost.row_cost += request.input_rows;
      result.materializes = Materializes(result.access_kind);
      result.diagnostics.push_back(result.materializes
                                       ? "SB_OPT_CTE_MATERIALIZATION_SELECTED"
                                       : "SB_OPT_CTE_INLINE_SELECTED");
      FinishAccepted(request, &result);
      break;
    }
    case EnterpriseRelationalOperatorKind::kMutation: {
      LocalMutationPlanningInput input;
      input.mutation_kind = request.mutation_kind.empty() ? "mutation" : request.mutation_kind;
      input.proof = request.proof;
      result = FromDecision(request, PlanLocalMutation(input));
      break;
    }
    case EnterpriseRelationalOperatorKind::kResultStreaming: {
      if (!request.result_streaming_enabled || request.result_window_rows == 0) {
        return Refuse("SB_OPT_ENTERPRISE_RESULT_STREAMING_SCOPE_REQUIRED",
                      "streaming_enabled_and_window_required");
      }
      result.ok = true;
      result.fail_closed = false;
      result.streaming_result = true;
      result.access_kind = planner::PhysicalAccessKind::kNone;
      result.cost = EstimateNodeCost(planner::MakeLogicalPlanNode(
          planner::LogicalPlanNodeKind::kDmlRead, result.access_kind,
          "query.result_streaming", "enterprise_result_streaming"));
      result.cost.row_cost += std::min(request.input_rows, request.result_window_rows);
      result.cost.memory_cost +=
          (std::min(request.input_rows, request.result_window_rows) *
           std::max<std::uint64_t>(request.row_width_bytes, 8)) /
          1024;
      result.materializes = false;
      result.diagnostics.push_back(request.client_backpressure
                                       ? "SB_OPT_RESULT_STREAMING_BACKPRESSURE_PLANNED"
                                       : "SB_OPT_RESULT_STREAMING_WINDOW_PLANNED");
      FinishAccepted(request, &result);
      Add(&result.evidence, "enterprise_relational_operator.streaming_result=true");
      break;
    }
  }

  ApplyFeedbackIfPresent(request, &result);
  return result;
}

}  // namespace scratchbird::engine::optimizer
