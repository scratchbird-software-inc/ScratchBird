// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "relational_planner.hpp"

#include <algorithm>

namespace scratchbird::engine::optimizer {
namespace planner = scratchbird::engine::planner;
namespace {

CostVector CostFor(planner::PhysicalAccessKind kind, const char* operation_id) {
  return EstimateNodeCost(planner::MakeLogicalPlanNode(planner::LogicalPlanNodeKind::kDmlRead, kind, operation_id, planner::PhysicalAccessKindName(kind)));
}

void Finish(CostVector* cost) {
  cost->total_cost = cost->startup_cost + cost->row_cost + cost->io_cost + cost->memory_cost + cost->uncertainty_cost;
}

void AddProofDiagnostics(const RelationalPlannerProofMetadata& proof,
                         RelationalPlanDecision* decision) {
  if (!proof.transaction_proof_present) {
    decision->diagnostics.push_back("SB_OPT_TRANSACTION_PROOF_METADATA_MISSING");
    decision->cost.uncertainty_cost += 250;
  }
  if (!proof.visibility_proof_present) {
    decision->diagnostics.push_back("SB_OPT_VISIBILITY_PROOF_METADATA_MISSING");
    decision->cost.uncertainty_cost += 250;
  }
  if (!proof.memory_budget_present) {
    decision->diagnostics.push_back("SB_OPT_MEMORY_BUDGET_METADATA_MISSING");
    decision->cost.uncertainty_cost += 100;
  }
}

bool RejectUnsafeProofOrMetadata(
    const RelationalPlannerProofMetadata& proof,
    const planner::LogicalPlanPropertyMetadata& metadata,
    RelationalPlanDecision* decision) {
  if (!proof.parser_or_sql_authority_claimed &&
      planner::LogicalPlanPropertyMetadataSafe(metadata)) {
    return false;
  }
  decision->diagnostics.push_back("SB_OPT_PROPERTY_METADATA_UNSAFE_AUTHORITY");
  decision->cost = RejectedCost(decision->diagnostics.front());
  return true;
}

bool ExpressionEquivalent(const std::string& left,
                          const std::string& right,
                          const planner::LogicalPlanPropertyMetadata& metadata) {
  if (left == right) return true;
  for (const auto& fact : metadata.expression_equivalence_facts) {
    const bool has_left = std::find(fact.expression_ids.begin(),
                                    fact.expression_ids.end(),
                                    left) != fact.expression_ids.end();
    const bool has_right = std::find(fact.expression_ids.begin(),
                                     fact.expression_ids.end(),
                                     right) != fact.expression_ids.end();
    if (has_left && has_right) return true;
  }
  return false;
}

bool TermMatches(const planner::LogicalPlanOrderingTerm& required,
                 const planner::LogicalPlanOrderingTerm& available,
                 const planner::LogicalPlanPropertyMetadata& metadata) {
  if (!ExpressionEquivalent(required.expression_id, available.expression_id, metadata) &&
      std::find(available.equivalent_expression_ids.begin(),
                available.equivalent_expression_ids.end(),
                required.expression_id) == available.equivalent_expression_ids.end()) {
    return false;
  }
  if (required.direction != planner::LogicalPlanSortDirection::kUnspecified &&
      available.direction != planner::LogicalPlanSortDirection::kUnspecified &&
      required.direction != available.direction) {
    return false;
  }
  if (required.null_ordering != planner::LogicalPlanNullOrdering::kUnspecified &&
      available.null_ordering != planner::LogicalPlanNullOrdering::kUnspecified &&
      required.null_ordering != available.null_ordering) {
    return false;
  }
  return true;
}

bool OrderingSatisfied(
    const std::vector<planner::LogicalPlanOrderingTerm>& required,
    const planner::LogicalPlanPropertyMetadata& metadata) {
  if (required.empty()) return false;
  for (const auto& fact : metadata.ordering_facts) {
    if (fact.terms.size() < required.size()) continue;
    bool matches = true;
    for (std::size_t i = 0; i < required.size(); ++i) {
      if (!TermMatches(required[i], fact.terms[i], metadata)) {
        matches = false;
        break;
      }
    }
    if (matches) return true;
  }
  return false;
}

std::vector<planner::LogicalPlanOrderingTerm> TermsForExpressions(
    const std::vector<std::string>& expression_ids) {
  std::vector<planner::LogicalPlanOrderingTerm> terms;
  terms.reserve(expression_ids.size());
  for (const auto& expression_id : expression_ids) {
    planner::LogicalPlanOrderingTerm term;
    term.expression_id = expression_id;
    terms.push_back(std::move(term));
  }
  return terms;
}

bool GroupOrderingSatisfied(const AggregatePlanningInput& input) {
  // SEARCH_KEY: OEIC_ORDERING_WINDOW_STREAMING_ENTERPRISE
  if (input.input_ordered_by_group) return true;
  if (!input.proof.ordering_metadata_present) return false;
  if (!input.group_expression_ids.empty() &&
      OrderingSatisfied(TermsForExpressions(input.group_expression_ids),
                        input.property_metadata)) {
    return true;
  }
  for (const auto& grouping : input.property_metadata.grouping_facts) {
    if (OrderingSatisfied(TermsForExpressions(grouping.group_expression_ids),
                          input.property_metadata)) {
      return true;
    }
    if (OrderingSatisfied(TermsForExpressions(grouping.equivalent_group_expression_ids),
                          input.property_metadata)) {
      return true;
    }
  }
  return false;
}

bool WindowOrderingSatisfied(const WindowPlanningInput& input) {
  if (input.input_ordered) return true;
  if (!input.proof.ordering_metadata_present) return false;
  std::vector<planner::LogicalPlanOrderingTerm> required =
      TermsForExpressions(input.partition_expression_ids);
  required.insert(required.end(),
                  input.ordering_terms.begin(),
                  input.ordering_terms.end());
  if (OrderingSatisfied(required, input.property_metadata)) return true;

  for (const auto& window : input.property_metadata.window_facts) {
    if (window.partition_expression_ids.size() !=
        input.partition_expression_ids.size()) {
      continue;
    }
    bool partition_matches = true;
    for (std::size_t i = 0; i < input.partition_expression_ids.size(); ++i) {
      if (!ExpressionEquivalent(input.partition_expression_ids[i],
                                window.partition_expression_ids[i],
                                input.property_metadata)) {
        partition_matches = false;
        break;
      }
    }
    if (!partition_matches || window.ordering_terms.size() < input.ordering_terms.size()) {
      continue;
    }
    bool order_matches = true;
    for (std::size_t i = 0; i < input.ordering_terms.size(); ++i) {
      if (!TermMatches(input.ordering_terms[i],
                       window.ordering_terms[i],
                       input.property_metadata)) {
        order_matches = false;
        break;
      }
    }
    if (order_matches) return true;
  }
  return false;
}

void ApplyMemoryAndSpillDiagnostics(std::uint64_t bytes,
                                    std::uint64_t memory_budget_bytes,
                                    const RelationalPlannerProofMetadata& proof,
                                    const char* diagnostic_prefix,
                                    RelationalPlanDecision* decision) {
  if (memory_budget_bytes == 0 || bytes <= memory_budget_bytes) return;
  decision->cost.uncertainty_cost += (bytes - memory_budget_bytes) / 4096 + 1000;
  if (proof.spill_allowed) {
    decision->diagnostics.push_back(std::string(diagnostic_prefix) + "_SPILL_EXPECTED");
  } else {
    decision->diagnostics.push_back(std::string(diagnostic_prefix) +
                                    "_SPILL_PROOF_OR_POLICY_MISSING");
  }
}

}  // namespace

RelationalPlanDecision PlanAggregate(const AggregatePlanningInput& input) {
  RelationalPlanDecision decision;
  if (RejectUnsafeProofOrMetadata(input.proof, input.property_metadata, &decision)) {
    return decision;
  }
  if (!input.grouping_present && !input.distinct_present) {
    decision.ok = true;
    decision.access_kind = planner::PhysicalAccessKind::kAggregateGeneric;
    decision.cost = CostFor(decision.access_kind, "query.aggregate");
    decision.cost.row_cost += input.input_rows;
    AddProofDiagnostics(input.proof, &decision);
    Finish(&decision.cost);
    return decision;
  }

  const auto estimated_state_bytes = std::max<std::uint64_t>(input.group_count, 1) * std::max<std::uint64_t>(input.row_width_bytes, 32);
  if (GroupOrderingSatisfied(input)) {
    decision.ok = true;
    decision.access_kind = planner::PhysicalAccessKind::kAggregateGeneric;
    decision.cost = CostFor(decision.access_kind, "query.aggregate");
    decision.diagnostics.push_back(input.distinct_present
                                       ? "SB_OPT_AGGREGATE_STREAMING_DISTINCT_ORDER_REUSE"
                                       : "SB_OPT_AGGREGATE_STREAMING_ORDER_REUSE");
  } else {
    decision.ok = true;
    decision.access_kind = planner::PhysicalAccessKind::kAggregateHash;
    decision.cost = CostFor(decision.access_kind, "query.aggregate");
    decision.cost.memory_cost += estimated_state_bytes / 1024;
    if (!input.distinct_present && input.grouping_present) {
      decision.diagnostics.push_back("SB_OPT_AGGREGATE_ORDER_METADATA_INSUFFICIENT");
    }
    ApplyMemoryAndSpillDiagnostics(estimated_state_bytes,
                                   input.memory_budget_bytes,
                                   input.proof,
                                   "SB_OPT_AGGREGATE",
                                   &decision);
  }
  decision.cost.row_cost += input.input_rows;
  AddProofDiagnostics(input.proof, &decision);
  Finish(&decision.cost);
  return decision;
}

RelationalPlanDecision PlanWindow(const WindowPlanningInput& input) {
  RelationalPlanDecision decision;
  if (RejectUnsafeProofOrMetadata(input.proof, input.property_metadata, &decision)) {
    return decision;
  }
  decision.ok = true;
  const bool order_satisfied = WindowOrderingSatisfied(input);
  decision.access_kind = order_satisfied && !input.frame_requires_materialization ? planner::PhysicalAccessKind::kNone : planner::PhysicalAccessKind::kSortThenWindow;
  decision.cost = CostFor(decision.access_kind, "query.window");
  decision.cost.row_cost += input.input_rows;
  if (order_satisfied) {
    decision.diagnostics.push_back("SB_OPT_WINDOW_ORDER_REUSE");
  } else {
    decision.diagnostics.push_back("SB_OPT_WINDOW_ORDER_METADATA_INSUFFICIENT");
  }
  if (input.frame_requires_materialization) {
    decision.cost.memory_cost += std::max<std::uint64_t>(input.input_rows / std::max<std::uint64_t>(input.partition_count, 1), 1);
    decision.diagnostics.push_back("SB_OPT_WINDOW_FRAME_MATERIALIZATION_REQUIRED");
  }
  AddProofDiagnostics(input.proof, &decision);
  Finish(&decision.cost);
  return decision;
}

RelationalPlanDecision PlanSortLimit(const SortPlanningInput& input) {
  RelationalPlanDecision decision;
  if (RejectUnsafeProofOrMetadata(input.proof, input.property_metadata, &decision)) {
    return decision;
  }
  decision.ok = true;
  const bool ordering_satisfied =
      input.input_already_ordered ||
      (input.proof.ordering_metadata_present &&
       OrderingSatisfied(input.required_ordering, input.property_metadata));
  if (ordering_satisfied) {
    decision.access_kind = input.limit_present ? planner::PhysicalAccessKind::kTopN : planner::PhysicalAccessKind::kNone;
    decision.diagnostics.push_back("SB_OPT_SORT_ORDER_REUSED");
  } else {
    decision.access_kind = input.limit_present && input.limit_count < input.input_rows ? planner::PhysicalAccessKind::kTopN : planner::PhysicalAccessKind::kSort;
    decision.diagnostics.push_back("SB_OPT_SORT_ORDER_METADATA_INSUFFICIENT");
  }
  decision.cost = CostFor(decision.access_kind, input.limit_present ? "query.top_n" : "query.sort");
  const auto bytes = input.input_rows * std::max<std::uint64_t>(input.row_width_bytes, 8);
  decision.cost.memory_cost += bytes / 4096;
  ApplyMemoryAndSpillDiagnostics(bytes,
                                 input.memory_budget_bytes,
                                 input.proof,
                                 "SB_OPT_SORT",
                                 &decision);
  AddProofDiagnostics(input.proof, &decision);
  Finish(&decision.cost);
  return decision;
}

RelationalPlanDecision PlanSetOperation(std::uint64_t left_rows, std::uint64_t right_rows, bool distinct) {
  SetOperationPlanningInput input;
  input.left_rows = left_rows;
  input.right_rows = right_rows;
  input.distinct = distinct;
  return PlanSetOperation(input);
}

RelationalPlanDecision PlanSetOperation(const SetOperationPlanningInput& input) {
  RelationalPlanDecision decision;
  if (RejectUnsafeProofOrMetadata(input.proof, input.property_metadata, &decision)) {
    return decision;
  }
  decision.ok = true;
  decision.access_kind = planner::PhysicalAccessKind::kSetOperation;
  decision.cost = CostFor(decision.access_kind, "query.set_operation");
  decision.cost.row_cost += input.left_rows + input.right_rows;
  if (input.distinct) {
    if (input.inputs_ordered_compatibly && input.proof.ordering_metadata_present) {
      decision.diagnostics.push_back("SB_OPT_SET_OPERATION_STREAMING_DISTINCT_ORDER_REUSE");
    } else {
      decision.diagnostics.push_back("SB_OPT_SET_OPERATION_DISTINCT_ORDER_METADATA_INSUFFICIENT");
      const auto bytes = (input.left_rows + input.right_rows) *
                         std::max<std::uint64_t>(input.row_width_bytes, 32);
      decision.cost.memory_cost += (input.left_rows + input.right_rows) / 16;
      ApplyMemoryAndSpillDiagnostics(bytes,
                                     input.memory_budget_bytes,
                                     input.proof,
                                     "SB_OPT_SET_OPERATION",
                                     &decision);
    }
  }
  AddProofDiagnostics(input.proof, &decision);
  Finish(&decision.cost);
  return decision;
}

RelationalPlanDecision PlanLocalMutation(const std::string& mutation_kind, bool transaction_context_present, bool visibility_proven) {
  LocalMutationPlanningInput input;
  input.mutation_kind = mutation_kind;
  input.proof.transaction_proof_present = transaction_context_present;
  input.proof.visibility_proof_present = visibility_proven;
  input.proof.mutation_transaction_proof_present = transaction_context_present;
  input.proof.mutation_visibility_proof_present = visibility_proven;
  return PlanLocalMutation(input);
}

RelationalPlanDecision PlanLocalMutation(const LocalMutationPlanningInput& input) {
  RelationalPlanDecision decision;
  if (input.proof.parser_or_sql_authority_claimed) {
    decision.diagnostics.push_back("SB_OPT_MUTATION_UNSAFE_AUTHORITY");
    decision.cost = RejectedCost(decision.diagnostics.front());
    return decision;
  }
  const bool transaction_proven = input.proof.transaction_proof_present &&
                                  input.proof.mutation_transaction_proof_present;
  const bool visibility_proven = input.proof.visibility_proof_present &&
                                 input.proof.mutation_visibility_proof_present;
  if (!transaction_proven || !visibility_proven) {
    decision.diagnostics.push_back(transaction_proven ? "SB_OPT_MUTATION_VISIBILITY_NOT_PROVEN" : "SB_OPT_MUTATION_TRANSACTION_CONTEXT_REQUIRED");
    decision.cost = RejectedCost(decision.diagnostics.front());
    return decision;
  }
  decision.ok = true;
  decision.access_kind = planner::PhysicalAccessKind::kNone;
  decision.cost = EstimateNodeCost(planner::MakeLogicalPlanNode(planner::LogicalPlanNodeKind::kDmlMutation, planner::PhysicalAccessKind::kNone, "dml." + input.mutation_kind, "mutation_command"));
  decision.diagnostics.push_back("SB_OPT_MUTATION_TRANSACTION_VISIBILITY_PROOF_ACCEPTED");
  Finish(&decision.cost);
  return decision;
}

}  // namespace scratchbird::engine::optimizer
