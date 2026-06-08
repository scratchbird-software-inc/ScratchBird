// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_relational_operator_enterprise.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "OEIC relational operator gate failure: " << message << '\n';
    return false;
  }
  return true;
}

bool Has(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool HasPrefix(const std::vector<std::string>& values, const std::string& prefix) {
  return std::any_of(values.begin(), values.end(), [&](const std::string& value) {
    return value.find(prefix) == 0;
  });
}

plan::LogicalPlanOrderingTerm Term(std::string expression_id) {
  plan::LogicalPlanOrderingTerm term;
  term.expression_id = std::move(expression_id);
  term.direction = plan::LogicalPlanSortDirection::kAscending;
  term.null_ordering = plan::LogicalPlanNullOrdering::kNullsLast;
  return term;
}

plan::LogicalPlanPropertyMetadata Metadata() {
  plan::LogicalPlanPropertyMetadata metadata;
  metadata.metadata_present = true;
  plan::LogicalPlanOrderingFact order;
  order.fact_id = "order:tenant";
  order.terms = {Term("expr.tenant_id")};
  metadata.ordering_facts.push_back(order);
  plan::LogicalPlanGroupingFact grouping;
  grouping.fact_id = "group:tenant";
  grouping.group_expression_ids = {"expr.tenant_id"};
  metadata.grouping_facts.push_back(grouping);
  return metadata;
}

opt::RelationalPlannerProofMetadata Proof() {
  opt::RelationalPlannerProofMetadata proof;
  proof.ordering_metadata_present = true;
  proof.memory_budget_present = true;
  proof.spill_allowed = true;
  proof.visibility_proof_present = true;
  proof.transaction_proof_present = true;
  proof.mutation_visibility_proof_present = true;
  proof.mutation_transaction_proof_present = true;
  return proof;
}

opt::OptimizerRuntimeFeedback Feedback(std::string family) {
  opt::OptimizerRuntimeFeedback feedback;
  feedback.operator_family = std::move(family);
  feedback.plan_shape = "enterprise.relational";
  feedback.cost_profile_id = "enterprise";
  feedback.estimated_rows = 100;
  feedback.actual_rows = 1000;
  feedback.actual_rows_examined = 1200;
  feedback.actual_rows_filtered = 200;
  feedback.loop_count = 1;
  feedback.estimated_pages = 10;
  feedback.actual_pages = 30;
  feedback.estimated_io_operations = 10;
  feedback.actual_io_operations = 30;
  feedback.estimated_visibility_recheck_rows = 10;
  feedback.actual_visibility_recheck_rows = 20;
  feedback.memory_grant_bytes = 64 * 1024;
  feedback.peak_memory_bytes = 96 * 1024;
  feedback.estimated_latency_microseconds = 1000;
  feedback.actual_latency_microseconds = 5000;
  feedback.estimated_resource_units = 100;
  feedback.actual_resource_units = 1000;
  feedback.policy_allowed = true;
  feedback.advisory_only = true;
  feedback.mga_visibility_recheck_preserved = true;
  feedback.transaction_finality_authority = "engine_transaction_inventory";
  return feedback;
}

opt::EnterpriseRelationalOperatorRequest Base(opt::EnterpriseRelationalOperatorKind kind) {
  // SEARCH_KEY: OEIC_RELATIONAL_OPERATOR_ENTERPRISE_CLOSURE
  opt::EnterpriseRelationalOperatorRequest request;
  request.kind = kind;
  request.route_label = "embedded:oeic043";
  request.plan_node_id = "plan-node-oeic043";
  request.result_contract_hash = "result-contract-oeic043";
  request.input_rows = 1000;
  request.right_rows = 900;
  request.group_count = 100;
  request.partition_count = 20;
  request.row_width_bytes = 96;
  request.memory_budget_bytes = 4 * 1024 * 1024;
  request.grouping_present = true;
  request.limit_present = true;
  request.limit_count = 50;
  request.set_distinct = true;
  request.group_expression_ids = {"expr.tenant_id"};
  request.ordering_terms = {Term("expr.tenant_id")};
  request.result_streaming_enabled = true;
  request.result_window_rows = 128;
  request.property_metadata = Metadata();
  request.proof = Proof();
  return request;
}

bool PlansAllRelationalOperators() {
  const std::vector<opt::EnterpriseRelationalOperatorKind> kinds = {
      opt::EnterpriseRelationalOperatorKind::kAggregate,
      opt::EnterpriseRelationalOperatorKind::kDistinctAggregate,
      opt::EnterpriseRelationalOperatorKind::kSort,
      opt::EnterpriseRelationalOperatorKind::kTopN,
      opt::EnterpriseRelationalOperatorKind::kWindow,
      opt::EnterpriseRelationalOperatorKind::kSetOperation,
      opt::EnterpriseRelationalOperatorKind::kCte,
      opt::EnterpriseRelationalOperatorKind::kMutation,
      opt::EnterpriseRelationalOperatorKind::kResultStreaming,
  };

  for (const auto kind : kinds) {
    auto request = Base(kind);
    if (kind == opt::EnterpriseRelationalOperatorKind::kMutation) {
      request.mutation_kind = "update_rows";
    }
    if (kind == opt::EnterpriseRelationalOperatorKind::kCte) {
      request.cte_reused = true;
    }
    if (kind == opt::EnterpriseRelationalOperatorKind::kResultStreaming) {
      request.client_backpressure = true;
    }
    const auto result = opt::PlanEnterpriseRelationalOperator(request);
    if (!Require(result.ok, std::string("operator rejected: ") +
                              opt::EnterpriseRelationalOperatorKindName(kind))) {
      return false;
    }
    if (!Require(HasPrefix(result.evidence, "enterprise_relational_operator.kind="),
                 "operator evidence missing kind")) return false;
    if (!Require(Has(result.evidence, "enterprise_relational_operator.parser_authority=false"),
                 "parser non-authority evidence missing")) return false;
    if (!Require(Has(result.evidence, "enterprise_relational_operator.transaction_visibility_proof=true"),
                 "transaction/visibility proof evidence missing")) return false;
    if (kind == opt::EnterpriseRelationalOperatorKind::kResultStreaming) {
      if (!Require(result.streaming_result,
                   "result streaming operator did not set streaming flag")) return false;
      if (!Require(Has(result.diagnostics,
                       "SB_OPT_RESULT_STREAMING_BACKPRESSURE_PLANNED"),
                   "result streaming backpressure diagnostic missing")) return false;
    }
  }
  return true;
}

bool AppliesRuntimeFeedbackSafely() {
  auto request = Base(opt::EnterpriseRelationalOperatorKind::kSort);
  request.feedback_present = true;
  request.runtime_feedback = Feedback("sort");
  const auto result = opt::PlanEnterpriseRelationalOperator(request);

  auto unsafe = request;
  unsafe.runtime_feedback.parser_or_donor_authority = true;
  const auto unsafe_result = opt::PlanEnterpriseRelationalOperator(unsafe);

  return Require(result.ok, "feedback-backed sort was rejected") &&
         Require(result.feedback_applied, "feedback was not applied") &&
         Require(Has(result.evidence, "enterprise_relational_operator.feedback_authority=advisory_only"),
                 "feedback authority evidence missing") &&
         Require(!unsafe_result.ok &&
                     unsafe_result.diagnostic_code == "SB_OPTIMIZER_FEEDBACK.REJECTED_UNSAFE",
                 "unsafe runtime feedback was not refused");
}

bool UnsafeAuthorityAndMissingProofFailClosed() {
  auto unsafe = Base(opt::EnterpriseRelationalOperatorKind::kAggregate);
  unsafe.authority.parser_or_sql_authority = true;
  const auto unsafe_result = opt::PlanEnterpriseRelationalOperator(unsafe);

  auto missing_proof = Base(opt::EnterpriseRelationalOperatorKind::kAggregate);
  missing_proof.proof.transaction_proof_present = false;
  const auto missing_proof_result = opt::PlanEnterpriseRelationalOperator(missing_proof);

  auto unsafe_metadata = Base(opt::EnterpriseRelationalOperatorKind::kAggregate);
  unsafe_metadata.property_metadata.parser_execution_authority_claimed = true;
  const auto unsafe_metadata_result = opt::PlanEnterpriseRelationalOperator(unsafe_metadata);

  return Require(!unsafe_result.ok &&
                     unsafe_result.diagnostic_code == "SB_OPT_ENTERPRISE_RELATIONAL_OPERATOR_UNSAFE_AUTHORITY",
                 "parser authority was not refused") &&
         Require(!missing_proof_result.ok &&
                     missing_proof_result.diagnostic_code == "SB_OPT_ENTERPRISE_RELATIONAL_OPERATOR_PROOF_REQUIRED",
                 "missing transaction proof was not refused") &&
         Require(!unsafe_metadata_result.ok &&
                     unsafe_metadata_result.diagnostic_code == "SB_OPT_ENTERPRISE_RELATIONAL_OPERATOR_UNSAFE_PROPERTY_METADATA",
                 "unsafe property metadata was not refused");
}

}  // namespace

int main() {
  if (!PlansAllRelationalOperators()) return EXIT_FAILURE;
  if (!AppliesRuntimeFeedbackSafely()) return EXIT_FAILURE;
  if (!UnsafeAuthorityAndMissingProofFailClosed()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
