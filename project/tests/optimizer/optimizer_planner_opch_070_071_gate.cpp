// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "logical_plan.hpp"
#include "relational_planner.hpp"

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
    std::cerr << "OPCH-070/071 gate failure: " << message << '\n';
    return false;
  }
  return true;
}

bool Has(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

plan::LogicalPlanOrderingTerm Term(
    std::string expression_id,
    plan::LogicalPlanSortDirection direction,
    plan::LogicalPlanNullOrdering null_ordering) {
  plan::LogicalPlanOrderingTerm term;
  term.expression_id = std::move(expression_id);
  term.direction = direction;
  term.null_ordering = null_ordering;
  return term;
}

plan::LogicalPlanPropertyMetadata PropertyMetadata() {
  // SEARCH_KEY: OPCH_LOGICAL_PROPERTY_METADATA
  plan::LogicalPlanPropertyMetadata metadata;
  metadata.metadata_present = true;

  plan::LogicalPlanOrderingFact customer_order;
  customer_order.fact_id = "order:customer_created";
  customer_order.terms.push_back(Term("expr.customer_id",
                                      plan::LogicalPlanSortDirection::kAscending,
                                      plan::LogicalPlanNullOrdering::kNullsLast));
  customer_order.terms.push_back(Term("expr.created_at",
                                      plan::LogicalPlanSortDirection::kDescending,
                                      plan::LogicalPlanNullOrdering::kNullsFirst));
  metadata.ordering_facts.push_back(customer_order);

  plan::LogicalPlanGroupingFact grouping;
  grouping.fact_id = "group:customer";
  grouping.group_expression_ids.push_back("expr.customer_id_alias");
  grouping.equivalent_group_expression_ids.push_back("expr.customer_id");
  metadata.grouping_facts.push_back(grouping);

  plan::LogicalPlanWindowFact window;
  window.fact_id = "window:customer_created";
  window.partition_expression_ids.push_back("expr.customer_id");
  window.ordering_terms.push_back(Term("expr.created_at",
                                       plan::LogicalPlanSortDirection::kDescending,
                                       plan::LogicalPlanNullOrdering::kNullsFirst));
  metadata.window_facts.push_back(window);

  plan::LogicalPlanExpressionEquivalenceFact equivalent;
  equivalent.equivalence_class_id = "equiv:customer_id";
  equivalent.expression_ids = {"expr.customer_id", "expr.customer_id_alias"};
  metadata.expression_equivalence_facts.push_back(equivalent);

  return metadata;
}

opt::RelationalPlannerProofMetadata FullProof() {
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

bool MetadataCarriesNormalizedFactsWithoutAuthority() {
  const auto metadata = PropertyMetadata();
  auto logical = plan::BuildQueryShapePlan({plan::QueryShapeKind::kWindowQuery});
  logical.property_metadata = metadata;
  const auto json = plan::SerializeLogicalPlanToJson(logical);

  return Require(plan::LogicalPlanPropertyMetadataSafe(metadata),
                 "normalized logical property metadata was rejected") &&
         Require(metadata.ordering_facts.front().terms.front().direction ==
                     plan::LogicalPlanSortDirection::kAscending,
                 "ordering direction metadata missing") &&
         Require(metadata.ordering_facts.front().terms.front().null_ordering ==
                     plan::LogicalPlanNullOrdering::kNullsLast,
                 "null-order metadata missing") &&
         Require(!metadata.raw_sql_text_present &&
                     !metadata.parser_execution_authority_claimed &&
                     !metadata.parser_visibility_or_finality_authority_claimed,
                 "logical property metadata claimed parser/sql authority") &&
         Require(json.find("\"ordering_fact_count\": 1") != std::string::npos,
                 "logical plan serialization omitted ordering fact count") &&
         Require(json.find("SELECT") == std::string::npos,
                 "logical plan serialization exposed SQL text");
}

bool ReusesOrderingForSortAggregateAndWindow() {
  const auto metadata = PropertyMetadata();
  const auto proof = FullProof();

  opt::SortPlanningInput sort;
  sort.input_rows = 500;
  sort.row_width_bytes = 64;
  sort.memory_budget_bytes = 1024 * 1024;
  sort.required_ordering = metadata.ordering_facts.front().terms;
  sort.property_metadata = metadata;
  sort.proof = proof;
  const auto sort_decision = opt::PlanSortLimit(sort);

  opt::AggregatePlanningInput aggregate;
  aggregate.input_rows = 500;
  aggregate.group_count = 10;
  aggregate.row_width_bytes = 64;
  aggregate.memory_budget_bytes = 1024 * 1024;
  aggregate.grouping_present = true;
  aggregate.group_expression_ids = {"expr.customer_id_alias"};
  aggregate.property_metadata = metadata;
  aggregate.proof = proof;
  const auto aggregate_decision = opt::PlanAggregate(aggregate);

  opt::WindowPlanningInput window;
  window.input_rows = 500;
  window.partition_count = 10;
  window.partition_expression_ids = {"expr.customer_id"};
  window.ordering_terms.push_back(metadata.ordering_facts.front().terms[1]);
  window.property_metadata = metadata;
  window.proof = proof;
  const auto window_decision = opt::PlanWindow(window);

  return Require(sort_decision.ok &&
                     sort_decision.access_kind == plan::PhysicalAccessKind::kNone,
                 "sort was not avoided with matching ordering metadata") &&
         Require(Has(sort_decision.diagnostics, "SB_OPT_SORT_ORDER_REUSED"),
                 "sort reuse diagnostic missing") &&
         Require(aggregate_decision.ok &&
                     aggregate_decision.access_kind ==
                         plan::PhysicalAccessKind::kAggregateGeneric,
                 "streaming aggregate did not reuse equivalent group ordering") &&
         Require(Has(aggregate_decision.diagnostics,
                     "SB_OPT_AGGREGATE_STREAMING_ORDER_REUSE"),
                 "streaming aggregate reuse diagnostic missing") &&
         Require(window_decision.ok &&
                     window_decision.access_kind == plan::PhysicalAccessKind::kNone,
                 "window did not reuse partition/order metadata") &&
         Require(Has(window_decision.diagnostics, "SB_OPT_WINDOW_ORDER_REUSE"),
                 "window order reuse diagnostic missing");
}

bool FallbacksAndSpillDiagnosticsAreDeterministic() {
  auto proof = FullProof();
  proof.ordering_metadata_present = false;

  opt::SortPlanningInput sort;
  sort.input_rows = 1000;
  sort.row_width_bytes = 256;
  sort.memory_budget_bytes = 4096;
  sort.limit_present = true;
  sort.limit_count = 10;
  sort.proof = proof;
  const auto sort_decision = opt::PlanSortLimit(sort);

  proof.spill_allowed = false;
  opt::SetOperationPlanningInput set_op;
  set_op.left_rows = 1000;
  set_op.right_rows = 1000;
  set_op.row_width_bytes = 128;
  set_op.memory_budget_bytes = 4096;
  set_op.distinct = true;
  set_op.inputs_ordered_compatibly = false;
  set_op.proof = proof;
  const auto set_decision = opt::PlanSetOperation(set_op);

  return Require(sort_decision.ok &&
                     sort_decision.access_kind == plan::PhysicalAccessKind::kTopN,
                 "sort/limit fallback did not choose TopN") &&
         Require(Has(sort_decision.diagnostics,
                     "SB_OPT_SORT_ORDER_METADATA_INSUFFICIENT"),
                 "sort fallback diagnostic missing") &&
         Require(Has(sort_decision.diagnostics, "SB_OPT_SORT_SPILL_EXPECTED"),
                 "sort spill diagnostic missing") &&
         Require(set_decision.ok &&
                     set_decision.access_kind ==
                         plan::PhysicalAccessKind::kSetOperation,
                 "set operation fallback was rejected") &&
         Require(Has(set_decision.diagnostics,
                     "SB_OPT_SET_OPERATION_DISTINCT_ORDER_METADATA_INSUFFICIENT"),
                 "set operation fallback diagnostic missing") &&
         Require(Has(set_decision.diagnostics,
                     "SB_OPT_SET_OPERATION_SPILL_PROOF_OR_POLICY_MISSING"),
                 "set operation spill-policy diagnostic missing");
}

bool MutationPlanningRequiresTransactionAndVisibilityProof() {
  auto proof = FullProof();
  proof.mutation_transaction_proof_present = false;
  auto missing_txn = opt::PlanLocalMutation({"insert_rows", proof});

  proof = FullProof();
  proof.mutation_visibility_proof_present = false;
  auto missing_visibility = opt::PlanLocalMutation({"update_rows", proof});

  auto accepted = opt::PlanLocalMutation({"delete_rows", FullProof()});

  return Require(!missing_txn.ok,
                 "local mutation accepted missing transaction proof") &&
         Require(Has(missing_txn.diagnostics,
                     "SB_OPT_MUTATION_TRANSACTION_CONTEXT_REQUIRED"),
                 "missing transaction proof diagnostic changed") &&
         Require(!missing_visibility.ok,
                 "local mutation accepted missing visibility proof") &&
         Require(Has(missing_visibility.diagnostics,
                     "SB_OPT_MUTATION_VISIBILITY_NOT_PROVEN"),
                 "missing visibility proof diagnostic changed") &&
         Require(accepted.ok &&
                     Has(accepted.diagnostics,
                         "SB_OPT_MUTATION_TRANSACTION_VISIBILITY_PROOF_ACCEPTED"),
                 "local mutation did not accept complete MGA proof metadata");
}

}  // namespace

int main() {
  if (!MetadataCarriesNormalizedFactsWithoutAuthority()) return EXIT_FAILURE;
  if (!ReusesOrderingForSortAggregateAndWindow()) return EXIT_FAILURE;
  if (!FallbacksAndSpillDiagnosticsAreDeterministic()) return EXIT_FAILURE;
  if (!MutationPlanningRequiresTransactionAndVisibilityProof()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
