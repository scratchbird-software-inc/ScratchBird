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
    std::cerr << "OEIC ordering/window gate failure: " << message << '\n';
    return false;
  }
  return true;
}

bool Has(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

plan::LogicalPlanOrderingTerm Term(std::string expression_id,
                                   plan::LogicalPlanSortDirection direction,
                                   plan::LogicalPlanNullOrdering nulls) {
  plan::LogicalPlanOrderingTerm term;
  term.expression_id = std::move(expression_id);
  term.direction = direction;
  term.null_ordering = nulls;
  return term;
}

plan::LogicalPlanPropertyMetadata Metadata() {
  // SEARCH_KEY: OEIC_ORDERING_WINDOW_STREAMING_ENTERPRISE
  plan::LogicalPlanPropertyMetadata metadata;
  metadata.metadata_present = true;

  plan::LogicalPlanOrderingFact order;
  order.fact_id = "order:tenant_created";
  order.terms = {
      Term("expr.tenant_id", plan::LogicalPlanSortDirection::kAscending,
           plan::LogicalPlanNullOrdering::kNullsLast),
      Term("expr.created_at", plan::LogicalPlanSortDirection::kDescending,
           plan::LogicalPlanNullOrdering::kNullsFirst)};
  metadata.ordering_facts.push_back(order);

  plan::LogicalPlanGroupingFact grouping;
  grouping.fact_id = "group:tenant";
  grouping.group_expression_ids = {"expr.tenant_id_alias"};
  grouping.equivalent_group_expression_ids = {"expr.tenant_id"};
  metadata.grouping_facts.push_back(grouping);

  plan::LogicalPlanWindowFact window;
  window.fact_id = "window:tenant_created";
  window.partition_expression_ids = {"expr.tenant_id_alias"};
  window.ordering_terms = {order.terms[1]};
  metadata.window_facts.push_back(window);

  plan::LogicalPlanExpressionEquivalenceFact equivalence;
  equivalence.equivalence_class_id = "equiv:tenant";
  equivalence.expression_ids = {"expr.tenant_id", "expr.tenant_id_alias"};
  metadata.expression_equivalence_facts.push_back(equivalence);
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

bool ReusesDistinctGroupingAndWindowMetadata() {
  const auto metadata = Metadata();
  const auto proof = Proof();

  opt::AggregatePlanningInput distinct;
  distinct.input_rows = 10000;
  distinct.group_count = 100;
  distinct.row_width_bytes = 96;
  distinct.memory_budget_bytes = 4 * 1024 * 1024;
  distinct.grouping_present = true;
  distinct.distinct_present = true;
  distinct.group_expression_ids = {"expr.tenant_id_alias"};
  distinct.property_metadata = metadata;
  distinct.proof = proof;
  const auto distinct_decision = opt::PlanAggregate(distinct);

  opt::WindowPlanningInput window;
  window.input_rows = 10000;
  window.partition_count = 100;
  window.partition_expression_ids = {"expr.tenant_id"};
  window.ordering_terms = {
      Term("expr.created_at", plan::LogicalPlanSortDirection::kDescending,
           plan::LogicalPlanNullOrdering::kNullsFirst)};
  window.property_metadata = metadata;
  window.proof = proof;
  const auto window_decision = opt::PlanWindow(window);

  return Require(distinct_decision.ok, "distinct aggregate planning failed") &&
         Require(distinct_decision.access_kind == plan::PhysicalAccessKind::kAggregateGeneric,
                 "distinct aggregate did not use streaming aggregate") &&
         Require(Has(distinct_decision.diagnostics,
                     "SB_OPT_AGGREGATE_STREAMING_DISTINCT_ORDER_REUSE"),
                 "distinct streaming diagnostic missing") &&
         Require(window_decision.ok, "window planning failed") &&
         Require(window_decision.access_kind == plan::PhysicalAccessKind::kNone,
                 "window planner did not reuse partition/order metadata") &&
         Require(Has(window_decision.diagnostics, "SB_OPT_WINDOW_ORDER_REUSE"),
                 "window reuse diagnostic missing");
}

bool NullOrderMismatchForcesSort() {
  const auto metadata = Metadata();
  auto proof = Proof();

  opt::SortPlanningInput sort;
  sort.input_rows = 1000;
  sort.row_width_bytes = 128;
  sort.memory_budget_bytes = 4 * 1024 * 1024;
  sort.required_ordering = {
      Term("expr.tenant_id_alias", plan::LogicalPlanSortDirection::kAscending,
           plan::LogicalPlanNullOrdering::kNullsFirst)};
  sort.property_metadata = metadata;
  sort.proof = proof;
  const auto decision = opt::PlanSortLimit(sort);

  return Require(decision.ok, "sort planning failed") &&
         Require(decision.access_kind == plan::PhysicalAccessKind::kSort,
                 "sort planner reused ordering despite null-order mismatch") &&
         Require(Has(decision.diagnostics, "SB_OPT_SORT_ORDER_METADATA_INSUFFICIENT"),
                 "null-order mismatch diagnostic missing");
}

bool UnsafeParserMetadataFailsClosed() {
  auto metadata = Metadata();
  metadata.raw_sql_text_present = true;

  opt::AggregatePlanningInput aggregate;
  aggregate.input_rows = 10;
  aggregate.group_count = 1;
  aggregate.row_width_bytes = 32;
  aggregate.memory_budget_bytes = 1024 * 1024;
  aggregate.grouping_present = true;
  aggregate.group_expression_ids = {"expr.tenant_id"};
  aggregate.property_metadata = metadata;
  aggregate.proof = Proof();
  const auto decision = opt::PlanAggregate(aggregate);

  return Require(!decision.ok, "unsafe parser metadata was accepted") &&
         Require(Has(decision.diagnostics, "SB_OPT_PROPERTY_METADATA_UNSAFE_AUTHORITY"),
                 "unsafe metadata diagnostic missing");
}

}  // namespace

int main() {
  if (!ReusesDistinctGroupingAndWindowMetadata()) return EXIT_FAILURE;
  if (!NullOrderMismatchForcesSort()) return EXIT_FAILURE;
  if (!UnsafeParserMetadataFailsClosed()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
