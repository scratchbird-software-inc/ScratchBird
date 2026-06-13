// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "join_planner_full.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace planner = scratchbird::engine::planner;
namespace {

constexpr std::uint64_t kMaxCost = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t kMaxDpRelations = 16;

std::uint64_t SaturatingAdd(std::uint64_t lhs, std::uint64_t rhs) {
  if (kMaxCost - lhs < rhs) return kMaxCost;
  return lhs + rhs;
}

std::uint64_t SaturatingSub(std::uint64_t lhs, std::uint64_t rhs) {
  return lhs > rhs ? lhs - rhs : 0;
}

std::uint64_t SaturatingMul(std::uint64_t lhs, std::uint64_t rhs) {
  if (lhs == 0 || rhs == 0) return 0;
  if (lhs > kMaxCost / rhs) return kMaxCost;
  return lhs * rhs;
}

std::uint64_t SaturatingScale(std::uint64_t value, double factor) {
  const long double clamped = static_cast<long double>(std::clamp(factor, 0.000001, 1.0));
  const long double scaled = static_cast<long double>(value) * clamped;
  if (scaled >= static_cast<long double>(kMaxCost)) return kMaxCost;
  if (scaled <= 0.0L) return 0;
  return static_cast<std::uint64_t>(scaled);
}

void FinishCost(CostVector* cost) {
  cost->total_cost = SaturatingAdd(
      SaturatingAdd(SaturatingAdd(cost->startup_cost, cost->row_cost),
                    SaturatingAdd(cost->io_cost, cost->memory_cost)),
      cost->uncertainty_cost);
}

void AddCost(CostVector* destination, const CostVector& source) {
  destination->startup_cost = SaturatingAdd(destination->startup_cost, source.startup_cost);
  destination->row_cost = SaturatingAdd(destination->row_cost, source.row_cost);
  destination->io_cost = SaturatingAdd(destination->io_cost, source.io_cost);
  destination->memory_cost = SaturatingAdd(destination->memory_cost, source.memory_cost);
  destination->uncertainty_cost = SaturatingAdd(destination->uncertainty_cost, source.uncertainty_cost);
  destination->selectable = destination->selectable && source.selectable;
  if (!source.rejection_reason.empty() && destination->rejection_reason.empty()) {
    destination->rejection_reason = source.rejection_reason;
  }
  if (source.confidence > destination->confidence) {
    destination->confidence = source.confidence;
  }
  FinishCost(destination);
}

double CombinedSelectivity(const std::vector<JoinPredicateEdge>& predicates) {
  long double value = 1.0L;
  for (const auto& predicate : predicates) {
    value *= static_cast<long double>(std::clamp(predicate.selectivity, 0.000001, 1.0));
  }
  return static_cast<double>(std::clamp(value, 0.000001L, 1.0L));
}

bool IsOuterJoin(JoinSemanticKind kind) {
  return kind == JoinSemanticKind::kLeftOuter ||
         kind == JoinSemanticKind::kRightOuter ||
         kind == JoinSemanticKind::kFullOuter;
}

bool IsSemiJoin(JoinSemanticKind kind) {
  return kind == JoinSemanticKind::kSemi;
}

bool IsAntiJoin(JoinSemanticKind kind) {
  return kind == JoinSemanticKind::kAnti;
}

void AddUniqueDiagnostic(std::vector<std::string>* diagnostics, std::string diagnostic) {
  if (std::find(diagnostics->begin(), diagnostics->end(), diagnostic) == diagnostics->end()) {
    diagnostics->push_back(std::move(diagnostic));
  }
}

std::vector<std::string> SemanticBarrierDiagnostics(const JoinGraph& graph) {
  std::vector<std::string> diagnostics;
  if (graph.contains_outer_join) {
    AddUniqueDiagnostic(&diagnostics, "SB_OPT_JOIN_ORDER_PRESERVED_OUTER_JOIN");
  }
  if (graph.contains_semi_or_anti) {
    AddUniqueDiagnostic(&diagnostics, "SB_OPT_JOIN_ORDER_PRESERVED_SEMI_OR_ANTI_JOIN");
  }
  if (graph.contains_correlation) {
    AddUniqueDiagnostic(&diagnostics, "SB_OPT_JOIN_ORDER_PRESERVED_CORRELATION");
  }
  if (graph.contains_lateral) {
    AddUniqueDiagnostic(&diagnostics, "SB_OPT_JOIN_ORDER_PRESERVED_LATERAL");
  }
  if (graph.contains_volatile) {
    AddUniqueDiagnostic(&diagnostics, "SB_OPT_JOIN_ORDER_PRESERVED_VOLATILE");
  }
  if (graph.contains_explicit_barrier) {
    AddUniqueDiagnostic(&diagnostics, "SB_OPT_JOIN_ORDER_PRESERVED_EXPLICIT_BARRIER");
  }
  for (const auto& relation : graph.relations) {
    if (relation.semantic_order_barrier) {
      AddUniqueDiagnostic(&diagnostics, "SB_OPT_JOIN_ORDER_PRESERVED_EXPLICIT_BARRIER");
    }
    if (relation.correlated_dependency) {
      AddUniqueDiagnostic(&diagnostics, "SB_OPT_JOIN_ORDER_PRESERVED_CORRELATION");
    }
    if (relation.lateral_dependency) {
      AddUniqueDiagnostic(&diagnostics, "SB_OPT_JOIN_ORDER_PRESERVED_LATERAL");
    }
    if (relation.volatile_dependency) {
      AddUniqueDiagnostic(&diagnostics, "SB_OPT_JOIN_ORDER_PRESERVED_VOLATILE");
    }
  }
  for (const auto& predicate : graph.predicates) {
    if (IsOuterJoin(predicate.semantic_kind) || predicate.outer_join_sensitive) {
      AddUniqueDiagnostic(&diagnostics, "SB_OPT_JOIN_ORDER_PRESERVED_OUTER_JOIN");
    }
    if (IsSemiJoin(predicate.semantic_kind)) {
      AddUniqueDiagnostic(&diagnostics, "SB_OPT_JOIN_ORDER_PRESERVED_SEMI_JOIN");
    }
    if (IsAntiJoin(predicate.semantic_kind)) {
      AddUniqueDiagnostic(&diagnostics, "SB_OPT_JOIN_ORDER_PRESERVED_ANTI_JOIN");
    }
    if (predicate.nullable) {
      AddUniqueDiagnostic(&diagnostics, "SB_OPT_JOIN_ORDER_PRESERVED_NULLABLE_EDGE");
    }
    if (predicate.correlated) {
      AddUniqueDiagnostic(&diagnostics, "SB_OPT_JOIN_ORDER_PRESERVED_CORRELATION");
    }
    if (predicate.lateral) {
      AddUniqueDiagnostic(&diagnostics, "SB_OPT_JOIN_ORDER_PRESERVED_LATERAL");
    }
    if (predicate.volatile_predicate) {
      AddUniqueDiagnostic(&diagnostics, "SB_OPT_JOIN_ORDER_PRESERVED_VOLATILE");
    }
    if (predicate.explicit_order_barrier) {
      AddUniqueDiagnostic(&diagnostics, "SB_OPT_JOIN_ORDER_PRESERVED_EXPLICIT_BARRIER");
    }
  }
  if (!diagnostics.empty()) {
    AddUniqueDiagnostic(&diagnostics, "SB_OPT_JOIN_ORDER_PRESERVED_BY_SEMANTIC_BARRIER");
  }
  return diagnostics;
}

std::optional<std::size_t> RelationIndex(const JoinGraph& graph, const std::string& relation_uuid) {
  for (std::size_t i = 0; i < graph.relations.size(); ++i) {
    if (graph.relations[i].relation_uuid == relation_uuid) return i;
  }
  return std::nullopt;
}

struct IndexedEdge {
  std::uint64_t left_mask = 0;
  std::uint64_t right_mask = 0;
  const JoinPredicateEdge* edge = nullptr;
};

std::vector<IndexedEdge> BuildIndexedEdges(const JoinGraph& graph) {
  std::vector<IndexedEdge> indexed;
  for (const auto& predicate : graph.predicates) {
    const auto left = RelationIndex(graph, predicate.left_relation_uuid);
    const auto right = RelationIndex(graph, predicate.right_relation_uuid);
    if (!left || !right || *left == *right) continue;
    indexed.push_back({std::uint64_t{1} << *left, std::uint64_t{1} << *right, &predicate});
  }
  return indexed;
}

bool EdgeConnectsSubsets(const IndexedEdge& edge, std::uint64_t left_mask, std::uint64_t right_mask) {
  return ((edge.left_mask & left_mask) != 0 && (edge.right_mask & right_mask) != 0) ||
         ((edge.right_mask & left_mask) != 0 && (edge.left_mask & right_mask) != 0);
}

bool HasConnectingEdge(const std::vector<IndexedEdge>& edges, std::uint64_t left_mask, std::uint64_t right_mask) {
  return std::any_of(edges.begin(), edges.end(), [&](const IndexedEdge& edge) {
    return EdgeConnectsSubsets(edge, left_mask, right_mask);
  });
}

bool HasEqualityEdge(const std::vector<IndexedEdge>& edges, std::uint64_t left_mask, std::uint64_t right_mask) {
  return std::any_of(edges.begin(), edges.end(), [&](const IndexedEdge& edge) {
    return edge.edge->equality && EdgeConnectsSubsets(edge, left_mask, right_mask);
  });
}

double SelectivityBetween(const std::vector<IndexedEdge>& edges, std::uint64_t left_mask, std::uint64_t right_mask) {
  long double value = 1.0L;
  bool found = false;
  for (const auto& edge : edges) {
    if (!EdgeConnectsSubsets(edge, left_mask, right_mask)) continue;
    value *= static_cast<long double>(std::clamp(edge.edge->selectivity, 0.000001, 1.0));
    found = true;
  }
  if (!found) return 1.0;
  return static_cast<double>(std::clamp(value, 0.000001L, 1.0L));
}

bool InputsOrdered(const JoinGraph& graph, std::uint64_t mask) {
  for (std::size_t i = 0; i < graph.relations.size(); ++i) {
    if ((mask & (std::uint64_t{1} << i)) != 0 && !graph.relations[i].order_preserving_required) {
      return false;
    }
  }
  return true;
}

std::uint64_t EstimatedJoinRows(std::uint64_t left_rows, std::uint64_t right_rows, double selectivity) {
  const auto pairs = SaturatingMul(std::max<std::uint64_t>(1, left_rows),
                                   std::max<std::uint64_t>(1, right_rows));
  return std::max<std::uint64_t>(1, SaturatingScale(pairs, selectivity));
}

std::uint64_t DeterministicDpBudget(std::uint64_t memory_budget_bytes) {
  if (memory_budget_bytes == 0) return 256;
  return std::clamp<std::uint64_t>(
      std::max<std::uint64_t>(1, memory_budget_bytes / 4096), 1, 4096);
}

std::size_t PopCount(std::uint64_t value) {
  std::size_t count = 0;
  while (value != 0) {
    value &= value - 1;
    ++count;
  }
  return count;
}

bool OrderLess(const JoinGraph& graph, const std::vector<std::size_t>& lhs, const std::vector<std::size_t>& rhs) {
  const auto common = std::min(lhs.size(), rhs.size());
  for (std::size_t i = 0; i < common; ++i) {
    const auto& left = graph.relations[lhs[i]];
    const auto& right = graph.relations[rhs[i]];
    if (left.estimated_rows != right.estimated_rows) return left.estimated_rows < right.estimated_rows;
    if (left.relation_uuid != right.relation_uuid) return left.relation_uuid < right.relation_uuid;
  }
  return lhs.size() < rhs.size();
}

std::vector<std::size_t> JoinOrderForStates(const JoinGraph& graph,
                                            const std::vector<std::size_t>& left,
                                            const std::vector<std::size_t>& right) {
  std::vector<std::size_t> left_right = left;
  left_right.insert(left_right.end(), right.begin(), right.end());
  std::vector<std::size_t> right_left = right;
  right_left.insert(right_left.end(), left.begin(), left.end());
  return OrderLess(graph, right_left, left_right) ? right_left : left_right;
}

struct JoinMethodCost {
  planner::PhysicalAccessKind method = planner::PhysicalAccessKind::kJoinNestedLoop;
  CostVector cost;
};

std::vector<JoinMethodCost> JoinMethodAlternatives(std::uint64_t left_rows,
                                                   std::uint64_t right_rows,
                                                   bool equi_join,
                                                   bool inputs_ordered,
                                                   std::uint64_t memory_budget_bytes,
                                                   double selectivity) {
  std::vector<JoinMethodCost> alternatives;
  JoinMethodCost nested;
  nested.method = planner::PhysicalAccessKind::kJoinNestedLoop;
  nested.cost = CostNestedLoopJoin(left_rows, right_rows, selectivity);
  alternatives.push_back(std::move(nested));
  if (!equi_join) return alternatives;

  const auto build_rows = std::min(left_rows, right_rows);
  const auto probe_rows = std::max(left_rows, right_rows);
  JoinMethodCost hash;
  hash.method = planner::PhysicalAccessKind::kJoinHash;
  hash.cost = CostHashJoin(build_rows, probe_rows, memory_budget_bytes, selectivity);
  alternatives.push_back(std::move(hash));
  if (inputs_ordered) {
    JoinMethodCost merge;
    merge.method = planner::PhysicalAccessKind::kJoinMerge;
    merge.cost = CostMergeJoin(left_rows, right_rows, true, selectivity);
    alternatives.push_back(std::move(merge));
  }
  return alternatives;
}

JoinMethodCost BestJoinMethodCost(std::uint64_t left_rows,
                                  std::uint64_t right_rows,
                                  bool equi_join,
                                  bool inputs_ordered,
                                  std::uint64_t memory_budget_bytes,
                                  double selectivity) {
  auto alternatives = JoinMethodAlternatives(left_rows,
                                            right_rows,
                                            equi_join,
                                            inputs_ordered,
                                            memory_budget_bytes,
                                            selectivity);
  JoinMethodCost best = alternatives.front();
  for (const auto& alternative : alternatives) {
    if (IsBetterCost(alternative.cost, best.cost)) best = alternative;
  }
  return best;
}

struct DpState {
  bool present = false;
  std::uint64_t mask = 0;
  std::uint64_t estimated_rows = 0;
  std::uint64_t memory_profile_kib = 0;
  CostVector cost;
  planner::PhysicalAccessKind method = planner::PhysicalAccessKind::kJoinNestedLoop;
  std::vector<std::size_t> order;
  bool ordered_output = false;
  bool covering_output = false;
  bool visibility_preserved = true;
  bool exact_output_preserved = true;
  bool materializes = false;
  bool spill_risk = false;
  bool parallel_eligible = true;
  bool correlated_or_lateral = false;
  std::string property_signature;
};

bool MethodMaterializes(planner::PhysicalAccessKind method) {
  return method == planner::PhysicalAccessKind::kJoinHash;
}

bool MethodPreservesOrder(planner::PhysicalAccessKind method) {
  return method == planner::PhysicalAccessKind::kJoinMerge;
}

bool CostHasSpillRisk(const CostVector& cost) {
  return cost.reason.find("spill_expected") != std::string::npos ||
         cost.uncertainty_cost >= 1000;
}

std::uint64_t BytesToKib(std::uint64_t bytes) {
  if (bytes == 0) return 0;
  return (bytes + 1023) / 1024;
}

const char* MemoryProfileClass(const DpState& state) {
  if (state.spill_risk) return "spill";
  if (state.memory_profile_kib == 0) return "zero";
  if (state.memory_profile_kib <= 1024) return "small";
  if (state.memory_profile_kib <= 64 * 1024) return "medium";
  return "large";
}

std::string BoolChar(bool value) {
  return value ? "1" : "0";
}

std::string PropertySignature(const DpState& state) {
  // SEARCH_KEY: OEIC_ENTERPRISE_JOIN_MEMO_FRONTIER
  return std::string("ordered=") + BoolChar(state.ordered_output) +
         "|covering=" + BoolChar(state.covering_output) +
         "|visibility=" + BoolChar(state.visibility_preserved) +
         "|exact=" + BoolChar(state.exact_output_preserved) +
         "|materializes=" + BoolChar(state.materializes) +
         "|spill=" + BoolChar(state.spill_risk) +
         "|memory=" + MemoryProfileClass(state) +
         "|parallel=" + BoolChar(state.parallel_eligible) +
         "|correlated=" + BoolChar(state.correlated_or_lateral);
}

void RefreshPropertySignature(DpState* state) {
  if (state == nullptr) return;
  state->property_signature = PropertySignature(*state);
}

bool PropertyAtLeastAsGood(const DpState& lhs, const DpState& rhs) {
  return (!rhs.ordered_output || lhs.ordered_output) &&
         (!rhs.covering_output || lhs.covering_output) &&
         (!rhs.visibility_preserved || lhs.visibility_preserved) &&
         (!rhs.exact_output_preserved || lhs.exact_output_preserved) &&
         lhs.memory_profile_kib <= rhs.memory_profile_kib &&
         (!rhs.parallel_eligible || lhs.parallel_eligible) &&
         (!lhs.materializes || rhs.materializes) &&
         (!lhs.spill_risk || rhs.spill_risk) &&
         (!lhs.correlated_or_lateral || rhs.correlated_or_lateral);
}

bool StateDominates(const JoinGraph& graph, const DpState& lhs, const DpState& rhs) {
  if (!lhs.present || !rhs.present) return false;
  if (!PropertyAtLeastAsGood(lhs, rhs)) return false;
  if (IsBetterCost(lhs.cost, rhs.cost)) return true;
  if (IsBetterCost(rhs.cost, lhs.cost)) return false;
  return OrderLess(graph, lhs.order, rhs.order);
}

bool DpStateBetter(const JoinGraph& graph, const DpState& candidate, const DpState& current) {
  if (!current.present) return true;
  if (IsBetterCost(candidate.cost, current.cost)) return true;
  if (IsBetterCost(current.cost, candidate.cost)) return false;
  if (PropertyAtLeastAsGood(candidate, current) && !PropertyAtLeastAsGood(current, candidate)) return true;
  return OrderLess(graph, candidate.order, current.order);
}

void AddToFrontier(const JoinGraph& graph,
                   const DpState& candidate,
                   std::size_t max_width,
                   std::vector<DpState>* frontier,
                   JoinOrderPlan* plan) {
  if (frontier == nullptr || plan == nullptr || !candidate.present) return;
  for (const auto& state : *frontier) {
    if (StateDominates(graph, state, candidate)) {
      ++plan->dominated_states;
      return;
    }
  }
  frontier->erase(std::remove_if(frontier->begin(),
                                 frontier->end(),
                                 [&](const DpState& state) {
                                   const bool dominated = StateDominates(graph, candidate, state);
                                   if (dominated) ++plan->dominated_states;
                                   return dominated;
                                 }),
                  frontier->end());
  auto same_signature = std::find_if(frontier->begin(),
                                     frontier->end(),
                                     [&](const DpState& state) {
                                       return state.property_signature == candidate.property_signature;
                                     });
  if (same_signature != frontier->end()) {
    if (DpStateBetter(graph, candidate, *same_signature)) {
      *same_signature = candidate;
    } else {
      ++plan->dominated_states;
    }
  } else {
    frontier->push_back(candidate);
  }
  std::sort(frontier->begin(), frontier->end(), [&](const DpState& left, const DpState& right) {
    return DpStateBetter(graph, left, right);
  });
  const auto cap = std::max<std::size_t>(1, max_width);
  if (frontier->size() > cap) {
    plan->pruned_alternatives += frontier->size() - cap;
    frontier->resize(cap);
  }
  plan->max_frontier_width = std::max(plan->max_frontier_width, frontier->size());
}

std::vector<std::string> OrderUuidsForState(const JoinGraph& graph, const DpState& state) {
  std::vector<std::string> order;
  for (const auto index : state.order) order.push_back(graph.relations[index].relation_uuid);
  return order;
}

std::vector<std::string> InputOrderUuids(const JoinGraph& graph) {
  std::vector<std::string> order;
  for (const auto& relation : graph.relations) order.push_back(relation.relation_uuid);
  return order;
}

DpState CostInputOrder(const JoinGraph& graph,
                       const std::vector<IndexedEdge>& edges,
                       std::uint64_t memory_budget_bytes) {
  DpState state;
  if (graph.relations.empty()) return state;
  state.present = true;
  state.mask = 1;
  state.estimated_rows = std::max<std::uint64_t>(1, graph.relations.front().estimated_rows);
  state.memory_profile_kib = BytesToKib(graph.relations.front().memory_profile_bytes);
  state.order.push_back(0);
  state.ordered_output = graph.relations.front().order_preserving_required;
  state.covering_output = graph.relations.front().covering_path_available;
  state.visibility_preserved = graph.relations.front().native_visibility_preserved;
  state.exact_output_preserved = graph.relations.front().exact_output_preserved;
  state.materializes = graph.relations.front().materialization_required;
  state.parallel_eligible = graph.relations.front().parallel_eligible;
  state.correlated_or_lateral = graph.relations.front().correlated_dependency ||
                                graph.relations.front().lateral_dependency;
  RefreshPropertySignature(&state);
  for (std::size_t i = 1; i < graph.relations.size(); ++i) {
    const auto next_mask = std::uint64_t{1} << i;
    const auto current_mask = state.mask;
    const auto selectivity = HasConnectingEdge(edges, current_mask, next_mask)
                                 ? SelectivityBetween(edges, current_mask, next_mask)
                                 : CombinedSelectivity(graph.predicates);
    const auto method = BestJoinMethodCost(state.estimated_rows,
                                           std::max<std::uint64_t>(1, graph.relations[i].estimated_rows),
                                           HasEqualityEdge(edges, current_mask, next_mask),
                                           InputsOrdered(graph, current_mask | next_mask),
                                           memory_budget_bytes,
                                           selectivity);
    AddCost(&state.cost, method.cost);
    state.method = method.method;
    state.estimated_rows = EstimatedJoinRows(state.estimated_rows,
                                             std::max<std::uint64_t>(1, graph.relations[i].estimated_rows),
                                             selectivity);
    state.memory_profile_kib = SaturatingAdd(
        SaturatingAdd(state.memory_profile_kib,
                      BytesToKib(graph.relations[i].memory_profile_bytes)),
        method.cost.memory_cost);
    state.mask |= next_mask;
    state.order.push_back(i);
    state.ordered_output = state.ordered_output &&
                           graph.relations[i].order_preserving_required;
    state.covering_output = state.covering_output &&
                            graph.relations[i].covering_path_available;
    state.visibility_preserved = state.visibility_preserved &&
                                 graph.relations[i].native_visibility_preserved;
    state.exact_output_preserved = state.exact_output_preserved &&
                                   graph.relations[i].exact_output_preserved;
    state.materializes = state.materializes ||
                         graph.relations[i].materialization_required ||
                         MethodMaterializes(method.method);
    state.spill_risk = state.spill_risk || CostHasSpillRisk(method.cost);
    state.parallel_eligible = state.parallel_eligible &&
                              graph.relations[i].parallel_eligible &&
                              !state.correlated_or_lateral;
    state.correlated_or_lateral = state.correlated_or_lateral ||
                                  graph.relations[i].correlated_dependency ||
                                  graph.relations[i].lateral_dependency;
    RefreshPropertySignature(&state);
  }
  return state;
}

}  // namespace

JoinGraph BuildJoinGraph(std::vector<JoinRelationNode> relations,
                         std::vector<JoinPredicateEdge> predicates,
                         bool contains_outer_join,
                         bool contains_semi_or_anti) {
  JoinGraph graph;
  graph.relations = std::move(relations);
  graph.predicates = std::move(predicates);
  graph.contains_outer_join = contains_outer_join;
  graph.contains_semi_or_anti = contains_semi_or_anti;
  for (const auto& relation : graph.relations) {
    graph.contains_explicit_barrier = graph.contains_explicit_barrier || relation.semantic_order_barrier;
    graph.contains_correlation = graph.contains_correlation || relation.correlated_dependency;
    graph.contains_lateral = graph.contains_lateral || relation.lateral_dependency;
    graph.contains_volatile = graph.contains_volatile || relation.volatile_dependency;
  }
  for (const auto& predicate : graph.predicates) {
    graph.contains_outer_join = graph.contains_outer_join || IsOuterJoin(predicate.semantic_kind) ||
                                predicate.outer_join_sensitive;
    graph.contains_semi_or_anti = graph.contains_semi_or_anti || IsSemiJoin(predicate.semantic_kind) ||
                                  IsAntiJoin(predicate.semantic_kind);
    graph.contains_correlation = graph.contains_correlation || predicate.correlated;
    graph.contains_lateral = graph.contains_lateral || predicate.lateral;
    graph.contains_volatile = graph.contains_volatile || predicate.volatile_predicate;
    graph.contains_explicit_barrier = graph.contains_explicit_barrier || predicate.explicit_order_barrier;
  }
  return graph;
}

bool JoinReorderAllowed(const JoinGraph& graph) {
  return SemanticBarrierDiagnostics(graph).empty();
}

CostVector CostNestedLoopJoin(std::uint64_t outer_rows, std::uint64_t inner_rows, double selectivity) {
  CostVector cost = EstimateNodeCost(planner::MakeLogicalPlanNode(planner::LogicalPlanNodeKind::kDmlRead, planner::PhysicalAccessKind::kJoinNestedLoop, "query.join", "nested_loop_join"));
  const auto pairs = SaturatingMul(std::max<std::uint64_t>(outer_rows, 1),
                                   std::max<std::uint64_t>(inner_rows, 1));
  cost.row_cost = SaturatingAdd(cost.row_cost, SaturatingScale(pairs, selectivity));
  FinishCost(&cost);
  cost.confidence = CostConfidence::kMedium;
  return cost;
}

CostVector CostHashJoin(std::uint64_t build_rows, std::uint64_t probe_rows, std::uint64_t memory_budget_bytes, double selectivity) {
  CostVector cost = EstimateNodeCost(planner::MakeLogicalPlanNode(planner::LogicalPlanNodeKind::kDmlRead, planner::PhysicalAccessKind::kJoinHash, "query.join", "hash_join"));
  const auto build_bytes = SaturatingMul(build_rows, 64);
  cost.memory_cost = SaturatingAdd(cost.memory_cost, build_bytes / 1024);
  cost.row_cost = SaturatingAdd(cost.row_cost,
                                SaturatingScale(SaturatingAdd(build_rows, probe_rows), selectivity));
  if (memory_budget_bytes != 0 && build_bytes > memory_budget_bytes) {
    cost.uncertainty_cost = SaturatingAdd(cost.uncertainty_cost,
                                          SaturatingAdd(SaturatingSub(build_bytes, memory_budget_bytes) / 1024,
                                                        1000));
    cost.reason = "hash_join_spill_expected";
  }
  FinishCost(&cost);
  cost.confidence = CostConfidence::kMedium;
  return cost;
}

CostVector CostMergeJoin(std::uint64_t left_rows, std::uint64_t right_rows, bool inputs_ordered, double selectivity) {
  CostVector cost = EstimateNodeCost(planner::MakeLogicalPlanNode(planner::LogicalPlanNodeKind::kDmlRead, planner::PhysicalAccessKind::kJoinMerge, "query.join", "merge_join"));
  cost.row_cost = SaturatingAdd(cost.row_cost,
                                SaturatingScale(SaturatingAdd(left_rows, right_rows), selectivity));
  if (!inputs_ordered) {
    cost.startup_cost = SaturatingAdd(cost.startup_cost,
                                      EstimateNodeCost(planner::MakeLogicalPlanNode(planner::LogicalPlanNodeKind::kDmlRead, planner::PhysicalAccessKind::kSort, "query.sort", "sort_for_merge_join")).total_cost);
  }
  FinishCost(&cost);
  cost.confidence = inputs_ordered ? CostConfidence::kMedium : CostConfidence::kLow;
  return cost;
}

const char* JoinSearchStrategyName(JoinSearchStrategy strategy) {
  switch (strategy) {
    case JoinSearchStrategy::kAuto: return "auto";
    case JoinSearchStrategy::kExhaustiveDp: return "exhaustive_dp";
    case JoinSearchStrategy::kBoundedDp: return "bounded_dp";
    case JoinSearchStrategy::kHypergraphGreedy: return "hypergraph_greedy";
    case JoinSearchStrategy::kHeuristicGreedy: return "heuristic_greedy";
    case JoinSearchStrategy::kInputOrder: return "input_order";
  }
  return "auto";
}

namespace {

JoinOrderPlan PlanFromState(const JoinGraph& graph,
                            const DpState& state,
                            JoinSearchStrategy requested,
                            JoinSearchStrategy selected) {
  JoinOrderPlan plan;
  plan.requested_strategy = requested;
  plan.selected_strategy = selected;
  plan.ok = state.present;
  plan.ordered_relation_uuids = state.present ? OrderUuidsForState(graph, state) : std::vector<std::string>{};
  plan.method = state.method;
  plan.cost = state.cost;
  plan.estimated_rows = state.estimated_rows;
  plan.memory_profile_kib = state.memory_profile_kib;
  plan.exact_output_preserved = state.exact_output_preserved;
  plan.selected_property_signature = state.property_signature;
  plan.max_frontier_width = state.present ? 1 : 0;
  return plan;
}

JoinOrderPlan InputOrderPlan(const JoinGraph& graph,
                             const std::vector<IndexedEdge>& indexed_edges,
                             std::uint64_t memory_budget_bytes,
                             JoinSearchStrategy requested,
                             std::string fallback = {}) {
  auto state = CostInputOrder(graph, indexed_edges, memory_budget_bytes);
  auto plan = PlanFromState(graph, state, requested, JoinSearchStrategy::kInputOrder);
  plan.ordered_relation_uuids = InputOrderUuids(graph);
  plan.semantic_order_preserved = true;
  plan.fallback_reason = std::move(fallback);
  if (!plan.fallback_reason.empty()) {
    plan.diagnostics.push_back(plan.fallback_reason);
  }
  return plan;
}

JoinOrderPlan GreedyJoinOrderPlan(const JoinGraph& graph,
                                  const std::vector<IndexedEdge>& indexed_edges,
                                  const JoinSearchPolicy& policy,
                                  JoinSearchStrategy selected) {
  if (graph.relations.empty()) {
    JoinOrderPlan empty;
    empty.requested_strategy = policy.strategy;
    empty.selected_strategy = selected;
    empty.diagnostics.push_back("SB_OPT_JOIN_GRAPH_EMPTY");
    return empty;
  }
  std::vector<bool> used(graph.relations.size(), false);
  std::vector<std::size_t> order;
  std::size_t start = 0;
  for (std::size_t i = 1; i < graph.relations.size(); ++i) {
    if (graph.relations[i].estimated_rows < graph.relations[start].estimated_rows ||
        (graph.relations[i].estimated_rows == graph.relations[start].estimated_rows &&
         graph.relations[i].relation_uuid < graph.relations[start].relation_uuid)) {
      start = i;
    }
  }
  used[start] = true;
  order.push_back(start);
  std::uint64_t current_mask = std::uint64_t{1} << start;
  while (order.size() < graph.relations.size()) {
    std::size_t best = graph.relations.size();
    double best_selectivity = 2.0;
    for (std::size_t i = 0; i < graph.relations.size(); ++i) {
      if (used[i]) continue;
      const auto next_mask = std::uint64_t{1} << i;
      const bool connected = HasConnectingEdge(indexed_edges, current_mask, next_mask);
      const double selectivity = connected ? SelectivityBetween(indexed_edges, current_mask, next_mask) : 1.0;
      if (best == graph.relations.size() ||
          connected > HasConnectingEdge(indexed_edges, current_mask, std::uint64_t{1} << best) ||
          selectivity < best_selectivity ||
          (selectivity == best_selectivity &&
           graph.relations[i].estimated_rows < graph.relations[best].estimated_rows) ||
          (selectivity == best_selectivity &&
           graph.relations[i].estimated_rows == graph.relations[best].estimated_rows &&
           graph.relations[i].relation_uuid < graph.relations[best].relation_uuid)) {
        best = i;
        best_selectivity = selectivity;
      }
    }
    used[best] = true;
    order.push_back(best);
    current_mask |= std::uint64_t{1} << best;
  }

  DpState state;
  state.present = true;
  state.mask = std::uint64_t{1} << order.front();
  state.estimated_rows = std::max<std::uint64_t>(1, graph.relations[order.front()].estimated_rows);
  state.memory_profile_kib = BytesToKib(graph.relations[order.front()].memory_profile_bytes);
  state.order.push_back(order.front());
  state.ordered_output = graph.relations[order.front()].order_preserving_required;
  state.covering_output = graph.relations[order.front()].covering_path_available;
  state.visibility_preserved = graph.relations[order.front()].native_visibility_preserved;
  state.exact_output_preserved = graph.relations[order.front()].exact_output_preserved;
  state.materializes = graph.relations[order.front()].materialization_required;
  state.parallel_eligible = graph.relations[order.front()].parallel_eligible;
  state.correlated_or_lateral = graph.relations[order.front()].correlated_dependency ||
                                graph.relations[order.front()].lateral_dependency;
  RefreshPropertySignature(&state);
  for (std::size_t position = 1; position < order.size(); ++position) {
    const auto index = order[position];
    const auto next_mask = std::uint64_t{1} << index;
    const auto selectivity = HasConnectingEdge(indexed_edges, state.mask, next_mask)
                                 ? SelectivityBetween(indexed_edges, state.mask, next_mask)
                                 : CombinedSelectivity(graph.predicates);
    const auto method = BestJoinMethodCost(state.estimated_rows,
                                           std::max<std::uint64_t>(1, graph.relations[index].estimated_rows),
                                           HasEqualityEdge(indexed_edges, state.mask, next_mask),
                                           InputsOrdered(graph, state.mask | next_mask),
                                           policy.memory_budget_bytes,
                                           selectivity);
    AddCost(&state.cost, method.cost);
    state.method = method.method;
    state.estimated_rows = EstimatedJoinRows(state.estimated_rows,
                                             std::max<std::uint64_t>(1, graph.relations[index].estimated_rows),
                                             selectivity);
    state.memory_profile_kib = SaturatingAdd(
        SaturatingAdd(state.memory_profile_kib,
                      BytesToKib(graph.relations[index].memory_profile_bytes)),
        method.cost.memory_cost);
    state.mask |= next_mask;
    state.order.push_back(index);
    state.ordered_output = state.ordered_output &&
                           graph.relations[index].order_preserving_required;
    state.covering_output = state.covering_output &&
                            graph.relations[index].covering_path_available;
    state.visibility_preserved = state.visibility_preserved &&
                                 graph.relations[index].native_visibility_preserved;
    state.exact_output_preserved = state.exact_output_preserved &&
                                   graph.relations[index].exact_output_preserved;
    state.materializes = state.materializes ||
                         graph.relations[index].materialization_required ||
                         MethodMaterializes(method.method);
    state.spill_risk = state.spill_risk || CostHasSpillRisk(method.cost);
    state.parallel_eligible = state.parallel_eligible &&
                              graph.relations[index].parallel_eligible &&
                              !state.correlated_or_lateral;
    state.correlated_or_lateral = state.correlated_or_lateral ||
                                  graph.relations[index].correlated_dependency ||
                                  graph.relations[index].lateral_dependency;
    RefreshPropertySignature(&state);
  }
  auto plan = PlanFromState(graph, state, policy.strategy, selected);
  plan.reorder_applied = plan.ordered_relation_uuids != InputOrderUuids(graph);
  plan.transitions_considered = graph.relations.size() > 0 ? graph.relations.size() - 1 : 0;
  plan.enumerated_subsets = graph.relations.size();
  plan.frontier_entries_retained = graph.relations.size();
  plan.max_frontier_width = 1;
  plan.diagnostics.push_back(selected == JoinSearchStrategy::kHypergraphGreedy
                                 ? "SB_OPT_JOIN_HYPERGRAPH_GREEDY_SELECTED"
                                 : "SB_OPT_JOIN_HEURISTIC_GREEDY_SELECTED");
  return plan;
}

JoinOrderPlan EnumerateDpJoinOrder(const JoinGraph& graph, const JoinSearchPolicy& policy) {
  JoinOrderPlan plan;
  plan.requested_strategy = policy.strategy;
  plan.selected_strategy = policy.strategy == JoinSearchStrategy::kExhaustiveDp
                               ? JoinSearchStrategy::kExhaustiveDp
                               : JoinSearchStrategy::kBoundedDp;
  if (graph.relations.empty()) {
    plan.diagnostics.push_back("SB_OPT_JOIN_GRAPH_EMPTY");
    return plan;
  }

  const auto indexed_edges = BuildIndexedEdges(graph);
  const auto semantic_diagnostics = SemanticBarrierDiagnostics(graph);
  if (!semantic_diagnostics.empty()) {
    plan = InputOrderPlan(graph,
                          indexed_edges,
                          policy.memory_budget_bytes,
                          policy.strategy,
                          "SB_OPT_JOIN_INPUT_ORDER_SELECTED_BY_SEMANTIC_BARRIER");
    plan.diagnostics = semantic_diagnostics;
    plan.diagnostics.push_back("SB_OPT_JOIN_INPUT_ORDER_SELECTED_BY_SEMANTIC_BARRIER");
    return plan;
  }

  plan.bounded_enumeration_applied = graph.relations.size() > 1;
  if (plan.bounded_enumeration_applied) {
    plan.diagnostics.push_back("SB_OPT_JOIN_DP_BOUNDED_ENUMERATION_APPLIED");
    plan.diagnostics.push_back("SB_OPT_JOIN_DP_CONNECTED_SUBSETS_ENUMERATED");
  }

  const auto relation_limit = policy.strategy == JoinSearchStrategy::kExhaustiveDp
                                  ? policy.exhaustive_relation_limit
                                  : policy.bounded_relation_limit;
  if (graph.relations.size() > relation_limit || graph.relations.size() > kMaxDpRelations) {
    plan = GreedyJoinOrderPlan(graph,
                               indexed_edges,
                               policy,
                               policy.strategy == JoinSearchStrategy::kExhaustiveDp
                                   ? JoinSearchStrategy::kHeuristicGreedy
                                   : JoinSearchStrategy::kInputOrder);
    plan.pruning_applied = true;
    plan.pruned_alternatives = 1;
    plan.fallback_reason = "SB_OPT_JOIN_DP_RELATION_LIMIT_FALLBACK";
    plan.diagnostics.push_back(plan.fallback_reason);
    plan.diagnostics.push_back("SB_OPT_JOIN_DP_PRUNED_BY_BUDGET");
    return plan;
  }

  const std::size_t n = graph.relations.size();
  const std::uint64_t full_mask = (std::uint64_t{1} << n) - 1;
  std::vector<std::vector<DpState>> frontiers(static_cast<std::size_t>(full_mask + 1));
  for (std::size_t i = 0; i < n; ++i) {
    const auto mask = std::uint64_t{1} << i;
    DpState state;
    state.present = true;
    state.mask = mask;
    state.estimated_rows = std::max<std::uint64_t>(1, graph.relations[i].estimated_rows);
    state.memory_profile_kib = BytesToKib(graph.relations[i].memory_profile_bytes);
    state.order.push_back(i);
    state.ordered_output = graph.relations[i].order_preserving_required;
    state.covering_output = graph.relations[i].covering_path_available;
    state.visibility_preserved = graph.relations[i].native_visibility_preserved;
    state.exact_output_preserved = graph.relations[i].exact_output_preserved;
    state.materializes = graph.relations[i].materialization_required;
    state.parallel_eligible = graph.relations[i].parallel_eligible;
    state.correlated_or_lateral = graph.relations[i].correlated_dependency ||
                                  graph.relations[i].lateral_dependency;
    RefreshPropertySignature(&state);
    frontiers[static_cast<std::size_t>(mask)].push_back(std::move(state));
    plan.max_frontier_width = std::max<std::size_t>(plan.max_frontier_width, 1);
    ++plan.enumerated_subsets;
  }

  const auto budget = policy.strategy == JoinSearchStrategy::kExhaustiveDp
                          ? std::numeric_limits<std::uint64_t>::max()
                          : (policy.transition_budget != 0
                                 ? policy.transition_budget
                                 : DeterministicDpBudget(policy.memory_budget_bytes));
  for (std::size_t subset_size = 2; subset_size <= n; ++subset_size) {
    for (std::uint64_t mask = 1; mask <= full_mask; ++mask) {
      if (PopCount(mask) != subset_size) continue;
      for (std::uint64_t left_mask = (mask - 1) & mask; left_mask != 0; left_mask = (left_mask - 1) & mask) {
        const auto right_mask = mask ^ left_mask;
        if (left_mask > right_mask) continue;
        const auto& left_frontier = frontiers[static_cast<std::size_t>(left_mask)];
        const auto& right_frontier = frontiers[static_cast<std::size_t>(right_mask)];
        if (left_frontier.empty() || right_frontier.empty()) continue;
        if (!HasConnectingEdge(indexed_edges, left_mask, right_mask)) continue;
        for (const auto& left : left_frontier) {
          for (const auto& right : right_frontier) {
            if (plan.transitions_considered >= budget) {
              ++plan.pruned_alternatives;
              continue;
            }
            ++plan.transitions_considered;
            const auto selectivity = SelectivityBetween(indexed_edges, left_mask, right_mask);
            const auto methods = JoinMethodAlternatives(left.estimated_rows,
                                                        right.estimated_rows,
                                                        HasEqualityEdge(indexed_edges, left_mask, right_mask),
                                                        InputsOrdered(graph, mask),
                                                        policy.memory_budget_bytes,
                                                        selectivity);
            for (const auto& method : methods) {
              DpState candidate;
              candidate.present = true;
              candidate.mask = mask;
              candidate.estimated_rows = EstimatedJoinRows(left.estimated_rows, right.estimated_rows, selectivity);
              candidate.memory_profile_kib = SaturatingAdd(
                  SaturatingAdd(left.memory_profile_kib, right.memory_profile_kib),
                  method.cost.memory_cost);
              candidate.cost = left.cost;
              AddCost(&candidate.cost, right.cost);
              AddCost(&candidate.cost, method.cost);
              candidate.method = method.method;
              candidate.order = JoinOrderForStates(graph, left.order, right.order);
              candidate.ordered_output = MethodPreservesOrder(method.method) ||
                                         (left.ordered_output && right.ordered_output && InputsOrdered(graph, mask));
              candidate.covering_output = left.covering_output && right.covering_output;
              candidate.visibility_preserved = left.visibility_preserved && right.visibility_preserved;
              candidate.exact_output_preserved = left.exact_output_preserved &&
                                                 right.exact_output_preserved;
              candidate.materializes = left.materializes || right.materializes ||
                                       MethodMaterializes(method.method);
              candidate.spill_risk = left.spill_risk || right.spill_risk || CostHasSpillRisk(method.cost);
              candidate.parallel_eligible = left.parallel_eligible && right.parallel_eligible &&
                                            !left.correlated_or_lateral &&
                                            !right.correlated_or_lateral;
              candidate.correlated_or_lateral = left.correlated_or_lateral || right.correlated_or_lateral;
              RefreshPropertySignature(&candidate);
              if (policy.preserve_property_frontier) {
                AddToFrontier(graph,
                              candidate,
                              policy.frontier_width,
                              &frontiers[static_cast<std::size_t>(mask)],
                              &plan);
              } else {
                auto& frontier = frontiers[static_cast<std::size_t>(mask)];
                if (frontier.empty() || DpStateBetter(graph, candidate, frontier.front())) {
                  frontier = {candidate};
                  plan.max_frontier_width = std::max<std::size_t>(plan.max_frontier_width, 1);
                }
              }
            }
          }
        }
      }
      if (!frontiers[static_cast<std::size_t>(mask)].empty()) {
        ++plan.enumerated_subsets;
      }
    }
  }

  if (plan.pruned_alternatives != 0) {
    plan.pruning_applied = true;
    plan.diagnostics.push_back("SB_OPT_JOIN_DP_PRUNED_BY_BUDGET");
  }

  auto& final_frontier = frontiers[static_cast<std::size_t>(full_mask)];
  if (final_frontier.empty()) {
    auto fallback = InputOrderPlan(
        graph,
        indexed_edges,
        policy.memory_budget_bytes,
        policy.strategy,
        "SB_OPT_JOIN_DP_GRAPH_DISCONNECTED_INPUT_ORDER");
    fallback.bounded_enumeration_applied = plan.bounded_enumeration_applied;
    fallback.pruned_alternatives =
        std::max(fallback.pruned_alternatives, plan.pruned_alternatives);
    fallback.pruning_applied = plan.pruning_applied ||
                               fallback.pruned_alternatives != 0;
    fallback.enumerated_subsets = plan.enumerated_subsets;
    fallback.transitions_considered = plan.transitions_considered;
    fallback.max_frontier_width = plan.max_frontier_width;
    fallback.diagnostics.insert(fallback.diagnostics.end(),
                                plan.diagnostics.begin(),
                                plan.diagnostics.end());
    plan = std::move(fallback);
    plan.diagnostics.push_back("SB_OPT_JOIN_DP_GRAPH_DISCONNECTED_INPUT_ORDER");
    return plan;
  }

  auto best_it = std::min_element(final_frontier.begin(), final_frontier.end(), [&](const DpState& left, const DpState& right) {
    return DpStateBetter(graph, left, right);
  });
  const auto& final_state = *best_it;
  plan.ordered_relation_uuids = OrderUuidsForState(graph, final_state);
  plan.method = final_state.method;
  plan.cost = final_state.cost;
  plan.estimated_rows = final_state.estimated_rows;
  plan.memory_profile_kib = final_state.memory_profile_kib;
  plan.exact_output_preserved = final_state.exact_output_preserved;
  plan.selected_property_signature = final_state.property_signature;
  plan.frontier_entries_retained = 0;
  for (const auto& frontier : frontiers) {
    plan.frontier_entries_retained += frontier.size();
  }
  plan.property_frontier_retained = policy.preserve_property_frontier &&
                                    plan.frontier_entries_retained > plan.enumerated_subsets;
  if (plan.property_frontier_retained) {
    plan.diagnostics.push_back("SB_OPT_JOIN_FRONTIER_PROPERTY_RETENTION");
  }
  if (plan.selected_strategy == JoinSearchStrategy::kExhaustiveDp) {
    plan.diagnostics.push_back("SB_OPT_JOIN_EXHAUSTIVE_DP_SELECTED");
  } else {
    plan.diagnostics.push_back("SB_OPT_JOIN_BOUNDED_DP_SELECTED");
  }
  plan.reorder_applied = plan.ordered_relation_uuids != InputOrderUuids(graph);
  plan.ok = true;
  return plan;
}

}  // namespace

JoinOrderPlan EnumerateJoinOrderWithPolicy(const JoinGraph& graph, const JoinSearchPolicy& policy) {
  const auto indexed_edges = BuildIndexedEdges(graph);
  JoinSearchStrategy selected = policy.strategy;
  if (selected == JoinSearchStrategy::kAuto) {
    selected = graph.relations.size() <= policy.exhaustive_relation_limit
                   ? JoinSearchStrategy::kExhaustiveDp
                   : (graph.relations.size() <= policy.bounded_relation_limit
                          ? JoinSearchStrategy::kBoundedDp
                          : JoinSearchStrategy::kHypergraphGreedy);
  }
  if (selected == JoinSearchStrategy::kInputOrder) {
    return InputOrderPlan(graph, indexed_edges, policy.memory_budget_bytes, policy.strategy);
  }
  if (selected == JoinSearchStrategy::kHeuristicGreedy ||
      selected == JoinSearchStrategy::kHypergraphGreedy) {
    return GreedyJoinOrderPlan(graph, indexed_edges, policy, selected);
  }
  auto dp_policy = policy;
  dp_policy.strategy = selected;
  return EnumerateDpJoinOrder(graph, dp_policy);
}

JoinOrderPlan EnumerateDeterministicJoinOrder(const JoinGraph& graph, std::uint64_t memory_budget_bytes) {
  JoinSearchPolicy policy;
  policy.strategy = JoinSearchStrategy::kBoundedDp;
  policy.memory_budget_bytes = memory_budget_bytes;
  policy.preserve_property_frontier = true;
  return EnumerateJoinOrderWithPolicy(graph, policy);
}

bool SemiAntiJoinCanDecorrelate(const JoinPredicateEdge& predicate) {
  return predicate.equality && !predicate.nullable && !predicate.outer_join_sensitive &&
         !predicate.correlated && !predicate.lateral && !predicate.volatile_predicate &&
         !predicate.explicit_order_barrier;
}

}  // namespace scratchbird::engine::optimizer
