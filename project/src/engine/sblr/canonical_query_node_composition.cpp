// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_node_composition.hpp"

#include "canonical_query_aggregate_composition.hpp"
#include "canonical_query_aggregate_registration.hpp"
#include "canonical_query_correlated_registration.hpp"
#include "canonical_query_descriptor_support.hpp"
#include "canonical_query_filter_registration.hpp"
#include "canonical_query_join_composition.hpp"
#include "canonical_query_join_registration.hpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_query_physical_registration.hpp"
#include "canonical_query_projection_registration.hpp"
#include "canonical_query_recursive_registration.hpp"
#include "canonical_query_relational_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"
#include "canonical_query_set_composition.hpp"
#include "canonical_query_set_registration.hpp"
#include "canonical_query_sort_registration.hpp"
#include "canonical_query_window_registration.hpp"

#include "datatype_catalog_manifest.hpp"
#include "datatype_operations.hpp"
#include "engine/executor/executor_foundation.hpp"
#include "engine/optimizer/relational_planner.hpp"
#include "query/expression_api.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {

namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;
namespace exec = scratchbird::engine::executor;
namespace plan = scratchbird::engine::planner;

namespace {

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_NODE_COMPOSITION_AUTHORITY
// Composes admitted object-free logical nodes over optimizer-published
// physical authority. It consumes and revalidates the engine-selected MGA
// statement context only; it cannot create/refresh snapshots, access durable
// storage, or finalize transactions.

constexpr std::string_view kValuesImplementationId =
    "values.materialize.canonical.v1";
constexpr std::uint64_t kCanonicalAggregateKernelBaseMemoryBytes = 1024;

}  // namespace

// The compiler admits a descriptor-valid unary tail containing at most one
// FILTER, PROJECT, query DISTINCT, SORT, exact global integer-ranking WINDOW,
// and LIMIT/FETCH node over either one canonical VALUES leaf, a two-VALUES
// accepted JOIN-kind branch, or an exact or losslessly reconciled ordinal/BY
// NAME quantified set-operation subtree.
// Every node still executes through the ordinary optimizer-published ABI-v2
// DAG and its canonical executor.
CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeNodeDrivenCompositionQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  // One producer plus one unary operator is already a composed DAG.
  // The node-specific validation below, not a three-node minimum, admits it.
  if (graph.nodes.size() < 2) return result;

  const auto find_node = [&](const std::uint32_t node_id) {
    return std::ranges::find_if(graph.nodes, [&](const auto& node) {
      return node.logical_node_id == node_id;
    });
  };
  auto current = find_node(graph.root_logical_node_id);
  if (current == graph.nodes.end()) return result;

  std::vector<const plan::CanonicalLogicalRelationalNode*> reverse_chain;
  std::unordered_set<std::uint32_t> visited;
  std::unordered_set<plan::CanonicalLogicalRelationalNodeKind> unary_kinds;
  const plan::CanonicalLogicalRelationalNode* join_left_node = nullptr;
  const plan::CanonicalLogicalRelationalNode* join_right_node = nullptr;
  std::optional<exec::CanonicalAcceptedJoinKind> join_kind;
  LiveLateralSubqueryProfile lateral_subquery_profile;
  bool correlated_subquery_base = false;
  LiveRecursiveCteProfile recursive_cte_profile;
  const plan::CanonicalLogicalRelationalNode* recursive_anchor_node = nullptr;
  const plan::CanonicalLogicalRelationalNode* recursive_term_node = nullptr;
  bool recursive_cte_base = false;
  std::string join_component;
  std::string join_operation_name;
  std::unordered_map<
      std::uint32_t, const plan::CanonicalLogicalRelationalNode*>
      set_base_nodes;
  std::unordered_map<std::uint32_t, LiveSetOperationProfile>
      set_profiles;
  bool registry_aggregate_composable = false;
  LiveUnaryAggregateExpressionProfile global_aggregate_profile;
  LivePairStatisticalExpressionProfile pair_aggregate_profile;
  LiveStringAggregateExpressionProfile string_aggregate_profile;
  LiveOrderedSingleCollectionExpressionProfile
      ordered_collection_profile;
  LiveJsonObjectAggregateExpressionProfile json_object_profile;
  LiveListaggExpressionProfile listagg_profile;
  LiveOrderedSetExpressionProfile ordered_set_profile;
  LiveApproximateExpressionProfile approximate_profile;
  LiveGroupedCountSumProfile grouped_aggregate_profile;
  std::size_t sort_count = 0;
  std::size_t window_count = 0;
  while (current != graph.nodes.end()) {
    if (!visited.insert(current->logical_node_id).second ||
        !current->required_object_uuids.empty()) {
      return result;
    }
    reverse_chain.push_back(&*current);
    const auto current_window_invocation = std::ranges::find_if(
        request.relational_dag.window_invocations, [&](const auto& invocation) {
          return invocation.relation_node_id == current->logical_node_id;
        });
    const bool single_current_window_invocation =
        current_window_invocation !=
            request.relational_dag.window_invocations.end() &&
        std::ranges::count_if(
            request.relational_dag.window_invocations,
            [&](const auto& invocation) {
              return invocation.relation_node_id == current->logical_node_id;
            }) == 1;
    const auto* current_window_aggregate_row =
        single_current_window_invocation
            ? exec::LookupCanonicalAggregateByUuidV1(
                  current_window_invocation->function_uuid)
            : nullptr;
    const bool current_count_star_window =
        current->node_kind ==
            plan::CanonicalLogicalRelationalNodeKind::kWindow &&
        current->semantic_variant_id == "window.aggregate-bridge.v1" &&
        current_window_aggregate_row != nullptr &&
        current_window_aggregate_row->function ==
            exec::CanonicalAggregateFunction::count &&
        current_window_aggregate_row->builtin_id ==
            current_window_invocation->builtin_id &&
        current_window_aggregate_row->abi_version ==
            current_window_invocation->function_abi_version &&
        current_window_invocation->argument_expression_ids.empty();
    if (current->node_kind ==
        plan::CanonicalLogicalRelationalNodeKind::kValues) {
      if (current->semantic_variant_id != "values.literal-table.v1" ||
          !current->input_logical_node_ids.empty() ||
          !current->required_object_uuids.empty()) {
        return result;
      }
      break;
    }
    if (current->node_kind ==
        plan::CanonicalLogicalRelationalNodeKind::kJoin) {
      lateral_subquery_profile =
          MatchLiveLateralSubqueryProfileForComposition(
              current->semantic_variant_id);
      if (lateral_subquery_profile.matched) {
        join_component = "lateral";
        join_operation_name = "LATERAL/APPLY";
      } else if (current->semantic_variant_id == "join.inner.v1") {
        join_kind = exec::CanonicalAcceptedJoinKind::kInner;
        join_component = "inner";
        join_operation_name = "INNER JOIN";
      } else if (current->semantic_variant_id == "join.cross.v1") {
        join_kind = exec::CanonicalAcceptedJoinKind::kCross;
        join_component = "cross";
        join_operation_name = "CROSS JOIN";
      } else if (current->semantic_variant_id == "join.left-outer.v1") {
        join_kind = exec::CanonicalAcceptedJoinKind::kLeftOuter;
        join_component = "left-outer";
        join_operation_name = "LEFT OUTER JOIN";
      } else if (current->semantic_variant_id == "join.right-outer.v1") {
        join_kind = exec::CanonicalAcceptedJoinKind::kRightOuter;
        join_component = "right-outer";
        join_operation_name = "RIGHT OUTER JOIN";
      } else if (current->semantic_variant_id == "join.full-outer.v1") {
        join_kind = exec::CanonicalAcceptedJoinKind::kFullOuter;
        join_component = "full-outer";
        join_operation_name = "FULL OUTER JOIN";
      } else if (current->semantic_variant_id == "join.left-semi.v1") {
        join_kind = exec::CanonicalAcceptedJoinKind::kLeftSemi;
        join_component = "left-semi";
        join_operation_name = "LEFT SEMI JOIN";
      } else if (current->semantic_variant_id == "join.left-anti.v1") {
        join_kind = exec::CanonicalAcceptedJoinKind::kLeftAnti;
        join_component = "left-anti";
        join_operation_name = "LEFT ANTI JOIN";
      } else {
        return result;
      }
      const auto expected_expression_count =
          lateral_subquery_profile.matched
              ? 0U
              : (*join_kind == exec::CanonicalAcceptedJoinKind::kCross
                     ? 0U
                     : 1U);
      if (current->input_logical_node_ids.size() != 2 ||
          current->input_logical_node_ids[0] ==
              current->input_logical_node_ids[1] ||
          current->bound_expression_ids.size() != expected_expression_count ||
          !current->required_object_uuids.empty() ||
          !current->required_property_uuids.empty() ||
          !current->delivered_property_uuids.empty()) {
        return result;
      }
      const auto left = find_node(current->input_logical_node_ids[0]);
      const auto right = find_node(current->input_logical_node_ids[1]);
      if (left == graph.nodes.end() || right == graph.nodes.end() ||
          left->node_kind !=
              plan::CanonicalLogicalRelationalNodeKind::kValues ||
          right->node_kind !=
              plan::CanonicalLogicalRelationalNodeKind::kValues ||
          left->semantic_variant_id != "values.literal-table.v1" ||
          right->semantic_variant_id != "values.literal-table.v1" ||
          !left->input_logical_node_ids.empty() ||
          !right->input_logical_node_ids.empty() ||
          !left->required_object_uuids.empty() ||
          !right->required_object_uuids.empty() ||
          !left->required_property_uuids.empty() ||
          !right->required_property_uuids.empty() ||
          !left->delivered_property_uuids.empty() ||
          !right->delivered_property_uuids.empty() ||
          !visited.insert(left->logical_node_id).second ||
          !visited.insert(right->logical_node_id).second) {
        return result;
      }
      join_left_node = &*left;
      join_right_node = &*right;
      break;
    }
    if (current->node_kind ==
            plan::CanonicalLogicalRelationalNodeKind::kSubquery &&
        (current->semantic_variant_id ==
             "subquery.correlated-int64-equality.v1" ||
         current->semantic_variant_id ==
             "subquery.correlated-typed-equality.v1")) {
      if (current->input_logical_node_ids.size() != 2 ||
          current->input_logical_node_ids[0] ==
              current->input_logical_node_ids[1] ||
          !current->bound_expression_ids.empty() ||
          !current->required_property_uuids.empty() ||
          !current->delivered_property_uuids.empty()) {
        return result;
      }
      const auto outer = find_node(current->input_logical_node_ids[0]);
      const auto inner = find_node(current->input_logical_node_ids[1]);
      if (outer == graph.nodes.end() || inner == graph.nodes.end() ||
          outer->node_kind !=
              plan::CanonicalLogicalRelationalNodeKind::kValues ||
          inner->node_kind !=
              plan::CanonicalLogicalRelationalNodeKind::kValues ||
          outer->semantic_variant_id != "values.literal-table.v1" ||
          inner->semantic_variant_id != "values.literal-table.v1" ||
          !outer->input_logical_node_ids.empty() ||
          !inner->input_logical_node_ids.empty() ||
          !outer->required_object_uuids.empty() ||
          !inner->required_object_uuids.empty() ||
          !outer->required_property_uuids.empty() ||
          !inner->required_property_uuids.empty() ||
          !outer->delivered_property_uuids.empty() ||
          !inner->delivered_property_uuids.empty() ||
          !visited.insert(outer->logical_node_id).second ||
          !visited.insert(inner->logical_node_id).second) {
        return result;
      }
      join_left_node = &*outer;
      join_right_node = &*inner;
      correlated_subquery_base = true;
      break;
    }
    if (current->node_kind ==
        plan::CanonicalLogicalRelationalNodeKind::kRecursiveCte) {
      recursive_cte_profile =
          MatchLiveRecursiveCteProfileForComposition(current->semantic_variant_id);
      if (recursive_cte_profile.matched) {
        if (current->input_logical_node_ids.size() != 2 ||
            current->input_logical_node_ids[0] ==
                current->input_logical_node_ids[1] ||
            current->bound_expression_ids.size() != 1 ||
            !current->required_object_uuids.empty() ||
            !current->required_property_uuids.empty() ||
            !current->delivered_property_uuids.empty()) {
          return result;
        }
        const auto anchor =
            find_node(current->input_logical_node_ids[0]);
        const auto term = find_node(current->input_logical_node_ids[1]);
        if (anchor == graph.nodes.end() || term == graph.nodes.end() ||
            anchor->node_kind !=
                plan::CanonicalLogicalRelationalNodeKind::kValues ||
            anchor->semantic_variant_id != "values.literal-table.v1" ||
            !anchor->input_logical_node_ids.empty() ||
            term->node_kind !=
                plan::CanonicalLogicalRelationalNodeKind::kCte ||
            term->semantic_variant_id !=
                "cte.recursive-term-int64-increment.v1" ||
            !term->input_logical_node_ids.empty() ||
            !term->bound_expression_ids.empty() ||
            anchor->output_descriptor_ids !=
                term->output_descriptor_ids ||
            (!recursive_cte_profile.search_cycle &&
             current->output_descriptor_ids !=
                 anchor->output_descriptor_ids) ||
            (recursive_cte_profile.search_cycle &&
             (current->output_descriptor_ids.size() !=
                  anchor->output_descriptor_ids.size() + 2 ||
              !std::equal(anchor->output_descriptor_ids.begin(),
                          anchor->output_descriptor_ids.end(),
                          current->output_descriptor_ids.begin()))) ||
            !anchor->required_object_uuids.empty() ||
            !term->required_object_uuids.empty() ||
            !anchor->required_property_uuids.empty() ||
            !term->required_property_uuids.empty() ||
            !anchor->delivered_property_uuids.empty() ||
            !term->delivered_property_uuids.empty() ||
            !visited.insert(anchor->logical_node_id).second ||
            !visited.insert(term->logical_node_id).second) {
          return result;
        }
        recursive_anchor_node = &*anchor;
        recursive_term_node = &*term;
        recursive_cte_base = true;
        break;
      }
    }
    if (current->node_kind ==
        plan::CanonicalLogicalRelationalNodeKind::kSetOperation) {
      std::vector<std::uint64_t> pending_set_base{
          current->logical_node_id};
      while (!pending_set_base.empty()) {
        const auto node_id = pending_set_base.back();
        pending_set_base.pop_back();
        if (set_base_nodes.contains(node_id)) continue;
        const auto node = find_node(node_id);
        if (node == graph.nodes.end() ||
            !node->required_object_uuids.empty() ||
            !node->required_property_uuids.empty() ||
            !node->delivered_property_uuids.empty() ||
            (node_id != current->logical_node_id &&
             visited.contains(node_id))) {
          return result;
        }
        set_base_nodes.emplace(node_id, &*node);
        if (node->node_kind ==
            plan::CanonicalLogicalRelationalNodeKind::kValues) {
          if (node->semantic_variant_id != "values.literal-table.v1" ||
              !node->input_logical_node_ids.empty()) {
            return result;
          }
          continue;
        }
        if (node->node_kind !=
            plan::CanonicalLogicalRelationalNodeKind::kSetOperation) {
          return result;
        }
        auto profile =
            ResolveLiveSetOperationProfileForComposition(node->semantic_variant_id);
        if (!profile.matched ||
            (profile.alignment !=
                 exec::CanonicalSetOperationAlignment::kOrdinal &&
             profile.alignment !=
                 exec::CanonicalSetOperationAlignment::kByName) ||
            (profile.type_profile !=
                 exec::CanonicalSetOperationTypeProfile::kExact &&
             profile.type_profile !=
                 exec::CanonicalSetOperationTypeProfile::kLosslessImplicit) ||
            (profile.equality_profile !=
                 exec::CanonicalSetOperationEqualityProfile::kExactTyped &&
             profile.equality_profile !=
                 exec::CanonicalSetOperationEqualityProfile::
                     kNullEqualBoundCollation) ||
            node->input_logical_node_ids.size() != 2 ||
            node->input_logical_node_ids[0] ==
                node->input_logical_node_ids[1] ||
            !node->bound_expression_ids.empty()) {
          return result;
        }
        set_profiles.emplace(node_id, std::move(profile));
        pending_set_base.insert(pending_set_base.end(),
                                node->input_logical_node_ids.begin(),
                                node->input_logical_node_ids.end());
      }
      for (const auto& [node_id, node] : set_base_nodes) {
        (void)node;
        if (node_id != current->logical_node_id &&
            !visited.insert(node_id).second) {
          return result;
        }
      }
      break;
    }
    if (current->input_logical_node_ids.size() != 1 ||
        (current->node_kind != plan::CanonicalLogicalRelationalNodeKind::kCte &&
         !unary_kinds.insert(current->node_kind).second)) {
      return result;
    }
    switch (current->node_kind) {
      case plan::CanonicalLogicalRelationalNodeKind::kFilter:
        if ((current->semantic_variant_id != "filter.where.v1" &&
             !IsLiveGroupedHavingProfileForComposition(
                 current->semantic_variant_id)) ||
            !current->required_property_uuids.empty() ||
            !current->delivered_property_uuids.empty()) {
          return result;
        }
        break;
      case plan::CanonicalLogicalRelationalNodeKind::kProject:
        if (current->semantic_variant_id != "project.select-list.v1" ||
            !current->required_property_uuids.empty() ||
            !current->delivered_property_uuids.empty()) {
          return result;
        }
        break;
      case plan::CanonicalLogicalRelationalNodeKind::kAggregate:
        global_aggregate_profile =
            MatchLiveUnaryAggregateExpressionProfileForComposition(
                current->semantic_variant_id);
        pair_aggregate_profile =
            MatchLivePairStatisticalExpressionProfileForComposition(
                current->semantic_variant_id);
        string_aggregate_profile =
            MatchLiveStringAggregateExpressionProfileForComposition(
                current->semantic_variant_id);
        ordered_collection_profile =
            MatchLiveOrderedSingleCollectionExpressionProfileForComposition(
                current->semantic_variant_id);
        json_object_profile =
            MatchLiveJsonObjectAggregateExpressionProfileForComposition(
                current->semantic_variant_id);
        listagg_profile = MatchLiveListaggExpressionProfileForComposition(
            current->semantic_variant_id);
        ordered_set_profile = MatchLiveOrderedSetExpressionProfileForComposition(
            current->semantic_variant_id);
        approximate_profile = MatchLiveApproximateExpressionProfileForComposition(
            current->semantic_variant_id);
        grouped_aggregate_profile =
            MatchLiveGroupedCountSumProfileForComposition(current->semantic_variant_id);
        registry_aggregate_composable =
            global_aggregate_profile.matched &&
            (global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::count ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::sum ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::avg ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::min ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::max ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::bool_and ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::bool_or ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::every);
        if (global_aggregate_profile.matched) {
          const auto function = global_aggregate_profile.function;
          registry_aggregate_composable =
              registry_aggregate_composable ||
              function == exec::CanonicalAggregateFunction::stddev_pop ||
              function == exec::CanonicalAggregateFunction::variance_pop ||
              function == exec::CanonicalAggregateFunction::stddev ||
              function == exec::CanonicalAggregateFunction::variance ||
              function == exec::CanonicalAggregateFunction::stddev_samp ||
              function == exec::CanonicalAggregateFunction::variance_samp;
        }
        registry_aggregate_composable =
            registry_aggregate_composable || pair_aggregate_profile.matched ||
            string_aggregate_profile.matched ||
            ordered_collection_profile.matched ||
            json_object_profile.matched || listagg_profile.matched ||
            ordered_set_profile.matched || approximate_profile.matched ||
            grouped_aggregate_profile.matched;
        if ((current->semantic_variant_id !=
                 "aggregate.query-distinct.v1" &&
             !registry_aggregate_composable) ||
            !current->required_property_uuids.empty() ||
            !current->delivered_property_uuids.empty()) {
          return result;
        }
        break;
      case plan::CanonicalLogicalRelationalNodeKind::kSort:
        ++sort_count;
        if (current->semantic_variant_id != "sort.required-order.v1" ||
            current->required_property_uuids.size() != 1 ||
            current->delivered_property_uuids.size() != 1) {
          return result;
        }
        break;
      case plan::CanonicalLogicalRelationalNodeKind::kWindow:
        ++window_count;
        if ((current->semantic_variant_id != "window.row-number.v1" &&
             current->semantic_variant_id != "window.rank.v1" &&
             current->semantic_variant_id != "window.dense-rank.v1" &&
             current->semantic_variant_id != "window.percent-rank.v1" &&
             current->semantic_variant_id != "window.cume-dist.v1" &&
             current->semantic_variant_id != "window.ntile.v1" &&
             current->semantic_variant_id != "window.lag.v1" &&
             current->semantic_variant_id != "window.lead.v1" &&
             current->semantic_variant_id != "window.first-value.v1" &&
             current->semantic_variant_id != "window.last-value.v1" &&
             current->semantic_variant_id != "window.nth-value.v1" &&
             current->semantic_variant_id !=
                 "window.aggregate-bridge.v1") ||
            current->logical_node_id != graph.root_logical_node_id ||
            current->bound_expression_ids.size() !=
                ((current->semantic_variant_id == "window.ntile.v1" ||
                  current->semantic_variant_id == "window.lag.v1" ||
                  current->semantic_variant_id == "window.lead.v1" ||
                  current->semantic_variant_id == "window.first-value.v1" ||
                  current->semantic_variant_id == "window.last-value.v1" ||
                  current->semantic_variant_id == "window.nth-value.v1" ||
                  current->semantic_variant_id ==
                      "window.aggregate-bridge.v1")
                     ? (current->semantic_variant_id == "window.nth-value.v1"
                            ? 4U
                            : (current_count_star_window ? 2U : 3U))
                     : 2U) ||
            current->required_property_uuids.size() != 1 ||
            current->delivered_property_uuids.size() != 2) {
          return result;
        }
        break;
      case plan::CanonicalLogicalRelationalNodeKind::kSubquery:
        if (const auto predicate_profile =
                MatchLivePredicateSubqueryProfileForComposition(
                    current->semantic_variant_id);
            ((current->semantic_variant_id == "subquery.table.v1" ||
              current->semantic_variant_id == "subquery.scalar.v1" ||
              current->semantic_variant_id == "subquery.row.v1")
                 ? !current->bound_expression_ids.empty()
                 : (!predicate_profile.matched ||
                    current->bound_expression_ids.size() !=
                        (predicate_profile.kind ==
                                 LivePredicateSubqueryKind::kExists
                             ? 1
                             : 2))) ||
            !current->required_property_uuids.empty() ||
            !current->delivered_property_uuids.empty()) {
          return result;
        }
        break;
      case plan::CanonicalLogicalRelationalNodeKind::kCte:
        if (current->semantic_variant_id != "cte.bound.v1" ||
            !current->bound_expression_ids.empty() ||
            !current->required_object_uuids.empty() ||
            !current->required_property_uuids.empty() ||
            !current->delivered_property_uuids.empty()) {
          return result;
        }
        break;
      case plan::CanonicalLogicalRelationalNodeKind::kLimit:
        if ((current->semantic_variant_id != "limit.bound-count.v1" &&
             current->semantic_variant_id !=
                 "limit.bound-count-offset.v1" &&
             current->semantic_variant_id !=
                 "fetch.first-rows-only-offset.v1") ||
            !current->required_property_uuids.empty() ||
            !current->delivered_property_uuids.empty()) {
          return result;
        }
        break;
      default:
        return result;
    }
    current = find_node(current->input_logical_node_ids.front());
  }
  if (reverse_chain.empty() ||
      (reverse_chain.back()->node_kind !=
           plan::CanonicalLogicalRelationalNodeKind::kValues &&
       reverse_chain.back()->node_kind !=
           plan::CanonicalLogicalRelationalNodeKind::kJoin &&
       reverse_chain.back()->node_kind !=
           plan::CanonicalLogicalRelationalNodeKind::kSetOperation &&
       !(recursive_cte_base &&
         reverse_chain.back()->node_kind ==
             plan::CanonicalLogicalRelationalNodeKind::kRecursiveCte) &&
       !(correlated_subquery_base &&
         reverse_chain.back()->node_kind ==
             plan::CanonicalLogicalRelationalNodeKind::kSubquery)) ||
      visited.size() != graph.nodes.size() || sort_count > 1 ||
      window_count > 1 ||
      request.optimizer_request.logical_properties.properties.size() !=
          sort_count + window_count) {
    return result;
  }
  std::ranges::reverse(reverse_chain);

  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_admitted = false;
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result = Failure(request, std::move(diagnostic_id),
                                std::move(detail));
    return result;
  };
  constexpr std::string_view kPayloadDiagnostic =
      "QOW-DIAG-RELATIONAL-LIVE-NODE-COMPOSITION-PAYLOAD-V1";
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-NODE-COMPOSITION-ADMISSION-V1",
        "node-driven composition lacks optimizer admission");
  }

  MaterializedValues state;
  std::optional<MaterializedValues> join_left_values;
  std::optional<MaterializedValues> join_right_values;
  std::optional<PreparedJoinRoot> prepared_join;
  std::vector<api::EngineSqlTruthValue> join_truth_values;
  std::size_t join_pair_count = 0;
  std::size_t join_output_row_bound = 0;
  std::string join_implementation_id;
  std::optional<PreparedCorrelatedSubqueryRoot>
      prepared_correlated_subquery;
  std::optional<PreparedCorrelatedSubqueryRoot> prepared_lateral_subquery;
  std::unordered_map<std::uint32_t, MaterializedValues>
      set_materialized_values;
  std::unordered_map<std::uint64_t, PreparedLiveSetNode>
      prepared_set_nodes;
  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag, request.expression_services);

  std::optional<PreparedFilterRoot> prepared_filter;
  std::size_t filter_input_row_count = 0;
  api::EngineCanonicalExpressionConsumer filter_expression_consumer =
      api::EngineCanonicalExpressionConsumer::filter;
  api::EnginePredicateConsumer filter_predicate_consumer =
      api::EnginePredicateConsumer::filter;
  std::optional<PreparedProjectRoot> prepared_project;
  std::size_t project_input_row_count = 0;
  std::string project_implementation_id;
  std::optional<PreparedDistinctRoot> prepared_distinct;
  std::size_t distinct_input_row_count = 0;
  std::size_t distinct_comparison_bound = 0;
  std::optional<PreparedGlobalAggregateRoot> prepared_count_star;
  std::size_t count_star_input_row_count = 0;
  std::optional<PreparedGlobalAggregateRoot> prepared_registry_aggregate;
  std::size_t registry_aggregate_input_row_count = 0;
  std::uint64_t registry_aggregate_filter_truth_memory_bytes = 0;
  std::optional<PreparedGroupedCountSumRoot> prepared_grouped_aggregate;
  std::size_t grouped_aggregate_input_row_count = 0;
  std::size_t grouped_aggregate_output_row_bound = 0;
  std::optional<PreparedSortRoot> prepared_sort;
  std::optional<exec::ExecutorColumnDescriptor> prepared_row_number;
  std::optional<exec::ExecutorColumnDescriptor> prepared_ntile;
  std::optional<exec::CanonicalDescriptorOrderTerm> prepared_ntile_order_term;
  std::optional<api::EngineTypedValue> prepared_ntile_bucket_count_operand;
  std::optional<exec::ExecutorColumnDescriptor> prepared_peer_ranking;
  std::optional<exec::CanonicalDescriptorOrderTerm>
      prepared_peer_ranking_order_term;
  GlobalRankingWindowProfile prepared_peer_ranking_profile;
  std::optional<exec::ExecutorColumnDescriptor> prepared_navigation_window;
  std::optional<exec::CanonicalDescriptorOrderTerm>
      prepared_navigation_order_term;
  std::optional<std::size_t> prepared_navigation_value_column;
  std::optional<api::EngineTypedValue>
      prepared_navigation_nth_value_position_operand;
  std::string prepared_navigation_frame_descriptor_uuid;
  GlobalRankingWindowProfile prepared_navigation_profile;
  std::string navigation_order_term_binding_evidence_uuid;
  std::string navigation_frame_property_binding_evidence_uuid;
  std::string navigation_capability_uuid;
  std::size_t navigation_maximum_pair_comparisons = 0;
  std::size_t navigation_maximum_effective_row_references = 0;
  std::optional<exec::ExecutorColumnDescriptor> prepared_aggregate_window;
  std::optional<exec::CanonicalDescriptorOrderTerm>
      prepared_aggregate_window_order_term;
  std::optional<std::size_t> prepared_aggregate_window_value_column;
  exec::CanonicalAggregateDescriptor prepared_aggregate_window_descriptor;
  std::string prepared_aggregate_window_frame_descriptor_uuid;
  std::string aggregate_window_order_term_binding_evidence_uuid;
  std::string aggregate_window_frame_property_binding_evidence_uuid;
  std::string aggregate_window_capability_uuid;
  std::size_t aggregate_window_maximum_pair_comparisons = 0;
  std::size_t aggregate_window_maximum_effective_row_references = 0;
  std::size_t aggregate_window_maximum_transition_count = 0;
  std::string row_number_order_evidence_uuid;
  std::string ntile_order_term_binding_evidence_uuid;
  std::string peer_ranking_order_term_binding_evidence_uuid;
  std::size_t peer_ranking_maximum_peer_comparisons = 0;
  std::size_t sort_input_row_count = 0;
  std::size_t sort_comparison_bound = 0;
  bool prepared_table_subquery = false;
  std::size_t table_subquery_input_row_count = 0;
  std::optional<LiveCardinalitySubqueryRegistrationProfile>
      prepared_cardinality_subquery;
  std::size_t cardinality_subquery_input_row_count = 0;
  std::optional<LivePredicateSubqueryRegistrationProfile>
      prepared_predicate_subquery;
  std::size_t predicate_subquery_input_row_count = 0;
  bool prepared_nonrecursive_cte = false;
  std::size_t nonrecursive_cte_input_row_count = 0;
  std::string nonrecursive_cte_implementation_id;
  std::optional<PreparedRecursiveCteRoot> prepared_recursive_cte;
  std::optional<PreparedLimitRoot> prepared_limit;
  std::size_t limit_input_row_count = 0;
  std::uint64_t row_limit = 0;
  std::uint64_t row_offset = 0;
  bool fetch_first_rows_only = false;
  std::string limit_implementation_id;
  bool planning_values_exact = true;

  const auto identity_scope = graph.bound_sblr_tree_uuid + ":" +
                              request.context.statement_uuid;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "composition.values.capability");
  const auto join_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "composition.join.capability");
  std::unordered_map<std::string, std::string> set_capability_uuids;
  for (const auto& [node_id, profile] : set_profiles) {
    (void)node_id;
    set_capability_uuids.try_emplace(
        profile.implementation_id,
        DerivedCanonicalUuid(
            identity_scope,
            "composition.set." + profile.implementation_id +
                ".capability"));
  }
  const auto filter_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "composition.filter.capability");
  const auto project_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "composition.project.capability");
  const auto distinct_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "composition.distinct.capability");
  const auto count_star_capability_uuid = DerivedCanonicalUuid(
      identity_scope, "composition.count-star.capability");
  const auto registry_aggregate_capability_uuid = DerivedCanonicalUuid(
      identity_scope, "composition.aggregate-registry.capability");
  const auto grouped_aggregate_capability_uuid = DerivedCanonicalUuid(
      identity_scope, "composition.grouped-aggregate.capability");
  const auto sort_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "composition.sort.capability");
  const auto window_capability_uuid = DerivedCanonicalUuid(
      identity_scope, "composition.window.integer-ranking.capability");
  const auto subquery_capability_uuid = DerivedCanonicalUuid(
      identity_scope, "composition.subquery.capability");
  const auto cte_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "composition.cte.capability");
  const auto recursive_term_capability_uuid = DerivedCanonicalUuid(
      identity_scope, "composition.recursive-cte-term.capability");
  const auto recursive_root_capability_uuid = DerivedCanonicalUuid(
      identity_scope, "composition.recursive-cte-root.capability");
  const auto limit_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "composition.limit.capability");

  std::vector<LivePhysicalNodeProfile> profiles;
  std::uint64_t total_work = 0;
  struct CorrelationPlanningState {
    MaterializedValues values;
    std::size_t pair_count{0};
    std::size_t matched_row_count{0};
    bool comparison_authority_required{false};
    std::uint64_t comparison_authority_memory_bytes{0};
  };
  const auto materialize_correlation =
      [&](const MaterializedValues& outer,
          const MaterializedValues& inner,
          const plan::CanonicalLogicalRelationalNode& root,
          const bool lateral,
          const bool null_extend,
          const std::string_view required_operand_type) {
        CorrelationPlanningState planning;
        const auto outer_type =
            outer.batch.columns.empty()
                ? dt::CanonicalTypeId::unknown
                : dt::CanonicalTypeIdFromStableName(
                      outer.batch.columns.front()
                          .descriptor.canonical_type_name);
        const auto inner_type =
            inner.batch.columns.empty()
                ? dt::CanonicalTypeId::unknown
                : dt::CanonicalTypeIdFromStableName(
                      inner.batch.columns.front()
                          .descriptor.canonical_type_name);
        if (outer_type == dt::CanonicalTypeId::unknown ||
            outer_type != inner_type ||
            (!required_operand_type.empty() &&
             (outer.batch.columns.front()
                      .descriptor.canonical_type_name !=
                  required_operand_type ||
              inner.batch.columns.front()
                      .descriptor.canonical_type_name !=
                  required_operand_type)) ||
            outer.result_bindings.size() != outer.batch.columns.size() ||
            inner.result_bindings.size() != inner.batch.columns.size() ||
            (outer.batch.rows.size() != 0 &&
             inner.batch.rows.size() >
                 std::numeric_limits<std::size_t>::max() /
                     outer.batch.rows.size())) {
          planning.values.detail =
              "correlation inputs are not bounded type-compatible key "
              "relations";
          return planning;
        }
        planning.pair_count =
            outer.batch.rows.size() * inner.batch.rows.size();
        planning.comparison_authority_required =
            CanonicalRelationalComparisonAuthorityRequiredV1(
                outer.batch.columns.front().descriptor,
                inner.batch.columns.front().descriptor);
        if (planning.comparison_authority_required &&
            !CheckedMultiply(
                static_cast<std::uint64_t>(planning.pair_count),
                sizeof(std::optional<int>),
                &planning.comparison_authority_memory_bytes)) {
          planning.values.detail =
              "correlation comparison authority carrier size overflowed";
          return planning;
        }

        const auto compare_keys =
            [&](const api::EngineTypedValue& left,
                const api::EngineTypedValue& right,
                int* comparison,
                std::string* detail) {
              std::optional<int> precomputed;
              if (!BindCanonicalRelationalComparisonAuthorityV1(
                      left, right, request.expression_services,
                      &precomputed, detail)) {
                return false;
              }
              if (precomputed.has_value()) {
                *comparison = *precomputed;
                return true;
              }
              return api::QowCompareCanonicalNonCollatedScalarsV1(
                  left, right, comparison, detail);
            };

        const auto validate_keys = [&](const exec::DescriptorBatch& batch) {
          for (const auto& row : batch.rows) {
            const auto& value = row.values.front();
            if (value.state == api::EngineValueState::sql_null) {
              continue;
            }
            int comparison = 0;
            std::string detail;
            if (!compare_keys(value, value, &comparison, &detail)) {
              planning.values.detail = detail;
              return false;
            }
          }
          return true;
        };
        if (!validate_keys(outer.batch) || !validate_keys(inner.batch)) {
          return planning;
        }

        planning.values.batch.columns =
            lateral ? outer.batch.columns : inner.batch.columns;
        if (lateral) {
          planning.values.batch.columns.insert(
              planning.values.batch.columns.end(),
              inner.batch.columns.begin(), inner.batch.columns.end());
          planning.values.result_bindings = outer.result_bindings;
          planning.values.result_bindings.insert(
              planning.values.result_bindings.end(),
              inner.result_bindings.begin(), inner.result_bindings.end());
        } else {
          planning.values.result_bindings = inner.result_bindings;
        }
        const auto inner_output_start =
            lateral ? outer.batch.columns.size() : 0;
        if (null_extend) {
          for (std::size_t column = inner_output_start;
               column < planning.values.batch.columns.size(); ++column) {
            planning.values.batch.columns[column].nullable = true;
            if (!exec::DeriveCanonicalNullableDescriptorEncoding(
                    &planning.values.batch.columns[column].descriptor)) {
              planning.values.detail =
                  "correlated NULL extension lacks a nullable descriptor carrier";
              return planning;
            }
          }
        }
        std::size_t visible_ordinal = 0;
        for (std::size_t column = 0;
             column < planning.values.result_bindings.size(); ++column) {
          auto& binding = planning.values.result_bindings[column];
          binding.physical_column_ordinal = column;
          if (!binding.visible || !binding.published_descriptor.has_value()) {
            continue;
          }
          binding.published_descriptor->ordinal =
              static_cast<std::uint32_t>(visible_ordinal++);
          if (null_extend && column >= inner_output_start) {
            binding.published_descriptor->nullability =
                exec::CanonicalResultNullability::kNullable;
          }
        }

        for (std::size_t outer_row = 0;
             outer_row < outer.batch.rows.size(); ++outer_row) {
          bool matched = false;
          const auto& outer_key = outer.batch.rows[outer_row].values.front();
          if (outer_key.state != api::EngineValueState::sql_null) {
            for (std::size_t inner_row = 0;
                 inner_row < inner.batch.rows.size(); ++inner_row) {
              const auto& inner_key =
                  inner.batch.rows[inner_row].values.front();
              if (inner_key.state == api::EngineValueState::sql_null) continue;
              int comparison = 0;
              std::string detail;
              if (!compare_keys(
                      outer_key, inner_key, &comparison, &detail)) {
                planning.values.detail = detail;
                return planning;
              }
              if (comparison != 0) continue;
              matched = true;
              ++planning.matched_row_count;
              if (lateral) {
                auto row = outer.batch.rows[outer_row];
                row.values.insert(row.values.end(),
                                  inner.batch.rows[inner_row].values.begin(),
                                  inner.batch.rows[inner_row].values.end());
                planning.values.batch.rows.push_back(std::move(row));
              } else {
                planning.values.batch.rows.push_back(
                    inner.batch.rows[inner_row]);
              }
            }
          }
          if (lateral && null_extend && !matched) {
            auto row = outer.batch.rows[outer_row];
            for (const auto& column : inner.batch.columns) {
              api::EngineTypedValue value;
              value.descriptor = column.descriptor;
              value.is_null = true;
              value.state = api::EngineValueState::sql_null;
              row.values.push_back(std::move(value));
            }
            planning.values.batch.rows.push_back(std::move(row));
          }
        }
        if (null_extend) {
          for (auto& row : planning.values.batch.rows) {
            for (std::size_t column = inner_output_start;
                 column < planning.values.batch.columns.size(); ++column) {
              row.values[column].descriptor =
                  planning.values.batch.columns[column].descriptor;
            }
          }
        }
        const auto validated = exec::ValidateCanonicalDescriptorBatch(
            planning.values.batch, root.output_descriptor_ids);
        if (!validated.ok) {
          planning.values.batch = {};
          planning.values.result_bindings.clear();
          planning.values.detail = validated.detail;
          return planning;
        }
        planning.values.ok = true;
        return planning;
      };
  if (recursive_cte_base &&
      reverse_chain.front()->node_kind ==
          plan::CanonicalLogicalRelationalNodeKind::kRecursiveCte) {
    const auto poll_recursive_planning_cancellation = [&]()
        -> std::optional<CanonicalObjectFreeValuesExecutionResult> {
      if (!request.context.query_cancellation_requested) return std::nullopt;
      try {
        if (!request.context.query_cancellation_requested()) {
          return std::nullopt;
        }
        return refuse(
            "QOW-DIAG-QRY-004-PHYSICAL-DISPATCH-CANCELLED-V1",
            "recursive CTE cancellation observed during bounded planning");
      } catch (const std::exception& error) {
        return refuse(
            "QOW-DIAG-QRY-004-PHYSICAL-CANCELLATION-PROBE-V1",
            std::string("recursive CTE planning cancellation probe failed:") +
                error.what());
      } catch (...) {
        return refuse(
            "QOW-DIAG-QRY-004-PHYSICAL-CANCELLATION-PROBE-V1",
            "recursive CTE planning cancellation probe failed");
      }
    };
    if (auto cancelled = poll_recursive_planning_cancellation()) {
      return std::move(*cancelled);
    }
    auto anchor = MaterializeValues(
        request.relational_dag, *recursive_anchor_node,
        request.expression_services);
    if (!anchor.ok || anchor.batch.columns.size() != 1 ||
        anchor.batch.columns.front().descriptor.canonical_type_name !=
            "int64") {
      return refuse(
          std::string(kPayloadDiagnostic),
          anchor.ok
              ? "recursive CTE anchor is not one typed int64 column"
              : "recursive CTE anchor: " + anchor.detail);
    }
    std::uint64_t upper_bound = 0;
    std::string bound_detail;
    if (!EvaluateNonNegativeRowBoundForComposition(
            &expression_runtime,
            reverse_chain.front()->bound_expression_ids.front(),
            &upper_bound, &bound_detail) ||
        upper_bound > static_cast<std::uint64_t>(
                          std::numeric_limits<std::int64_t>::max())) {
      return refuse(
          std::string(kPayloadDiagnostic),
          "recursive CTE upper bound: " + bound_detail);
    }
    if (recursive_cte_profile.search_cycle) {
      if (anchor.batch.rows.size() != 1 ||
          anchor.batch.rows.front().values.size() != 1 ||
          upper_bound == 0) {
        return refuse(
            std::string(kPayloadDiagnostic),
            "recursive SEARCH/CYCLE requires one int64 anchor and a "
            "positive upper bound");
      }
      const auto anchor_value =
          exec::DecodeInt64Value(anchor.batch.rows.front().values.front());
      if (!anchor_value.ok() || anchor_value.value != 1) {
        return refuse(
            std::string(kPayloadDiagnostic),
            "recursive SEARCH/CYCLE requires the canonical anchor value 1");
      }
    }
    const auto prepared_recursive_term = PrepareLiveRecursiveCteTerm(
        recursive_cte_profile, anchor.batch.columns,
        static_cast<std::int64_t>(upper_bound));

    const auto row_key = [](const exec::DescriptorTuple& row,
                            std::string* key,
                            std::string* detail) {
      if (row.values.size() != 1) {
        *detail = "recursive CTE row is ragged";
        return false;
      }
      const auto& value = row.values.front();
      if (value.state == api::EngineValueState::sql_null ||
          value.is_null) {
        *key = "null";
        return true;
      }
      const auto decoded = exec::DecodeInt64Value(value);
      if (!decoded.ok()) {
        *detail = decoded.diagnostic.diagnostic_code + ":" +
                  decoded.diagnostic.detail;
        return false;
      }
      *key = "int64:" + std::to_string(decoded.value);
      return true;
    };

    exec::DescriptorBatch accumulated;
    accumulated.columns = anchor.batch.columns;
    exec::DescriptorBatch working;
    working.columns = anchor.batch.columns;
    std::unordered_set<std::string> seen;
    std::string recursion_detail;
    for (const auto& row : anchor.batch.rows) {
      if (auto cancelled = poll_recursive_planning_cancellation()) {
        return std::move(*cancelled);
      }
      if (recursive_cte_profile.search_cycle ||
          recursive_cte_profile.union_mode ==
              exec::CanonicalRecursiveCteUnionMode::kDistinct) {
        std::string key;
        if (!row_key(row, &key, &recursion_detail)) {
          return refuse(std::string(kPayloadDiagnostic), recursion_detail);
        }
        if (!seen.insert(std::move(key)).second) continue;
      }
      accumulated.rows.push_back(row);
      working.rows.push_back(row);
    }

    std::size_t iteration_count = 0;
    std::size_t maximum_working = working.rows.size();
    std::size_t maximum_term_output = 0;
    std::uint64_t recursive_work = 0;
    std::uint64_t recursive_value_comparison_count =
        anchor.batch.rows.size();
    std::vector<exec::CanonicalResultColumnBinding>
        recursive_generated_bindings;
    exec::ExecutorColumnDescriptor search_sequence_column;
    exec::ExecutorColumnDescriptor cycle_mark_column;
    while (!working.rows.empty()) {
      if (auto cancelled = poll_recursive_planning_cancellation()) {
        return std::move(*cancelled);
      }
      if (iteration_count ==
          request.optimizer_request.resource.maximum_candidate_count) {
        return refuse(
            "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
            "recursive CTE did not converge within its admitted bound");
      }
      ++iteration_count;
      std::optional<CanonicalObjectFreeValuesExecutionResult>
          term_cancellation;
      auto term_execution = ExecutePreparedRecursiveCteTerm(
          prepared_recursive_term, working, iteration_count,
          request.optimizer_request.resource.maximum_candidate_count,
          [&](const std::size_t) {
            auto cancelled = poll_recursive_planning_cancellation();
            if (!cancelled.has_value()) return false;
            term_cancellation = std::move(*cancelled);
            return true;
          });
      if (term_cancellation.has_value()) {
        return std::move(*term_cancellation);
      }
      if (!term_execution.ok) {
        return refuse(std::string(kPayloadDiagnostic),
                      term_execution.detail);
      }
      auto generated = std::move(term_execution.generated.batch);
      maximum_term_output =
          std::max(maximum_term_output, generated.rows.size());
      if (!CheckedAdd(recursive_work, working.rows.size(),
                      &recursive_work) ||
          !CheckedAdd(recursive_work, generated.rows.size(),
                      &recursive_work) ||
          !CheckedAdd(recursive_value_comparison_count,
                      generated.rows.size(),
                      &recursive_value_comparison_count)) {
        return refuse(
            "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
            "recursive CTE work overflowed");
      }
      bool search_cycle_closed = false;
      if (recursive_cte_profile.search_cycle) {
        exec::DescriptorBatch admitted;
        admitted.columns = generated.columns;
        for (const auto& row : generated.rows) {
          if (auto cancelled = poll_recursive_planning_cancellation()) {
            return std::move(*cancelled);
          }
          std::string key;
          if (!row_key(row, &key, &recursion_detail)) {
            return refuse(std::string(kPayloadDiagnostic),
                          recursion_detail);
          }
          if (!seen.insert(std::move(key)).second) {
            admitted.rows.push_back(row);
            search_cycle_closed = true;
            break;
          }
          admitted.rows.push_back(row);
        }
        generated = std::move(admitted);
      } else if (recursive_cte_profile.union_mode ==
          exec::CanonicalRecursiveCteUnionMode::kDistinct) {
        exec::DescriptorBatch distinct;
        distinct.columns = generated.columns;
        for (const auto& row : generated.rows) {
          if (auto cancelled = poll_recursive_planning_cancellation()) {
            return std::move(*cancelled);
          }
          std::string key;
          if (!row_key(row, &key, &recursion_detail)) {
            return refuse(std::string(kPayloadDiagnostic),
                          recursion_detail);
          }
          if (seen.insert(std::move(key)).second) {
            distinct.rows.push_back(row);
          }
        }
        generated = std::move(distinct);
      }
      if (generated.rows.size() >
          request.optimizer_request.resource.maximum_candidate_count -
              std::min<std::size_t>(
                  accumulated.rows.size(),
                  request.optimizer_request.resource
                      .maximum_candidate_count)) {
        return refuse(
            "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
            "recursive CTE result exceeded its admitted bound");
      }
      accumulated.rows.insert(accumulated.rows.end(),
                              generated.rows.begin(),
                              generated.rows.end());
      maximum_working =
          std::max(maximum_working, generated.rows.size());
      if (search_cycle_closed) {
        working.columns = generated.columns;
        working.rows.clear();
      } else {
        working = std::move(generated);
      }
    }
    if (recursive_cte_profile.search_cycle) {
      std::uint64_t adjacent = 0;
      std::uint64_t triangular = 0;
      if (!CheckedAdd(upper_bound, 1, &adjacent) ||
          ((upper_bound & 1U) == 0
               ? !CheckedMultiply(upper_bound / 2, adjacent, &triangular)
               : !CheckedMultiply(upper_bound, adjacent / 2, &triangular)) ||
          !CheckedAdd(triangular, 2,
                      &recursive_value_comparison_count)) {
        return refuse(
            "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
            "recursive SEARCH/CYCLE comparison work overflowed");
      }
    } else if (recursive_cte_profile.union_mode !=
               exec::CanonicalRecursiveCteUnionMode::kDistinct) {
      recursive_value_comparison_count = 1;
    }
    if (recursive_value_comparison_count == 0 ||
        recursive_value_comparison_count >
            std::numeric_limits<std::size_t>::max() ||
        !CheckedAdd(recursive_work, recursive_value_comparison_count,
                    &recursive_work) ||
        recursive_work > std::numeric_limits<std::size_t>::max()) {
      return refuse(
          "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
          "recursive CTE comparison work exceeds its native bound");
    }
    if (recursive_cte_profile.search_cycle) {
      const auto core_manifest =
          dt::LoadCurrentCoreDatatypeCatalogManifest();
      if (!core_manifest.ok()) {
        return refuse(std::string(kPayloadDiagnostic),
                      "recursive SEARCH/CYCLE core datatype catalog is "
                      "unavailable");
      }
      const auto int64_type_uuid =
          ExactCanonicalCoreDatatypeTypeUuidV1("int64");
      const auto boolean_type_uuid =
          ExactCanonicalCoreDatatypeTypeUuidV1("boolean");
      std::unordered_set<std::uint32_t> anchor_descriptor_ids;
      std::unordered_set<std::uint32_t> generated_descriptor_ids;
      std::unordered_set<std::string_view> anchor_descriptor_uuids;
      std::unordered_set<std::string_view> generated_descriptor_uuids;
      std::unordered_set<std::string_view> generated_type_uuids;
      if (!CanonicalUuidText(int64_type_uuid) ||
          !CanonicalUuidText(boolean_type_uuid)) {
        return refuse(std::string(kPayloadDiagnostic),
                      "recursive SEARCH/CYCLE core type identity is not exact");
      }
      generated_type_uuids.insert(int64_type_uuid);
      generated_type_uuids.insert(boolean_type_uuid);
      for (const auto& anchor_column : anchor.batch.columns) {
        const auto descriptor = std::ranges::find_if(
            request.relational_dag.descriptors, [&](const auto& candidate) {
              return candidate.descriptor_id == anchor_column.descriptor_id;
            });
        if (descriptor == request.relational_dag.descriptors.end() ||
            !CanonicalUuidText(descriptor->descriptor_uuid) ||
            !CanonicalUuidText(descriptor->type_uuid)) {
          return refuse(
              std::string(kPayloadDiagnostic),
              "recursive SEARCH/CYCLE anchor identity is not exact");
        }
        anchor_descriptor_ids.insert(descriptor->descriptor_id);
        anchor_descriptor_uuids.insert(descriptor->descriptor_uuid);
        generated_type_uuids.insert(descriptor->type_uuid);
      }
      const auto make_generated_column =
          [&](const std::uint32_t descriptor_id,
              const std::string_view type_name,
              const std::string& type_uuid,
              const std::string_view column_name,
              const std::uint32_t result_ordinal,
              exec::ExecutorColumnDescriptor* column,
              exec::CanonicalResultColumnBinding* binding) {
            const auto descriptor = std::ranges::find_if(
                request.relational_dag.descriptors,
                [&](const auto& candidate) {
                  return candidate.descriptor_id == descriptor_id;
                });
            if (descriptor == request.relational_dag.descriptors.end() ||
                type_uuid.empty() ||
                descriptor->type_uuid != type_uuid ||
                !CanonicalUuidText(descriptor->descriptor_uuid) ||
                anchor_descriptor_ids.contains(descriptor_id) ||
                !generated_descriptor_ids.insert(descriptor_id).second ||
                anchor_descriptor_uuids.contains(
                    descriptor->descriptor_uuid) ||
                generated_type_uuids.contains(descriptor->descriptor_uuid) ||
                !generated_descriptor_uuids
                     .insert(descriptor->descriptor_uuid)
                     .second ||
                descriptor->nullability !=
                    api::RelationalNullability::kNonNull ||
                descriptor->collation_uuid.has_value() ||
                descriptor->timezone_profile_id.has_value()) {
              recursion_detail =
                  "recursive SEARCH/CYCLE generated descriptor is not "
                  "exact";
              return false;
            }
            api::EngineDescriptor engine_descriptor;
            engine_descriptor.descriptor_uuid =
                descriptor->descriptor_uuid;
            engine_descriptor.descriptor_kind = "scalar";
            engine_descriptor.canonical_type_name =
                std::string(type_name);
            engine_descriptor.encoded_descriptor =
                "type_uuid=" + descriptor->type_uuid +
                ";nullability=non_null";
            *column = {std::string(column_name), engine_descriptor, false,
                       descriptor_id};
            binding->physical_column_ordinal = result_ordinal;
            binding->visible = true;
            binding->published_descriptor =
                exec::CanonicalResultColumnDescriptor{
                    result_ordinal, std::string(column_name),
                    descriptor->descriptor_uuid, descriptor->type_uuid,
                    exec::CanonicalResultNullability::kNonNull,
                    std::nullopt, std::nullopt};
            return true;
          };
      exec::CanonicalResultColumnBinding sequence_binding;
      exec::CanonicalResultColumnBinding cycle_binding;
      const auto anchor_width = anchor.batch.columns.size();
      if (anchor_width != 1 || anchor.result_bindings.size() != 1 ||
          !make_generated_column(
              reverse_chain.front()->output_descriptor_ids[anchor_width],
              "int64", int64_type_uuid, "search_sequence",
              static_cast<std::uint32_t>(anchor_width),
              &search_sequence_column, &sequence_binding) ||
          !make_generated_column(
              reverse_chain.front()->output_descriptor_ids[anchor_width + 1],
              "boolean", boolean_type_uuid, "cycle_mark",
              static_cast<std::uint32_t>(anchor_width + 1),
              &cycle_mark_column, &cycle_binding)) {
        return refuse(std::string(kPayloadDiagnostic), recursion_detail);
      }
      accumulated.columns.push_back(search_sequence_column);
      accumulated.columns.push_back(cycle_mark_column);
      for (std::size_t row = 0; row < accumulated.rows.size(); ++row) {
        if (auto cancelled = poll_recursive_planning_cancellation()) {
          return std::move(*cancelled);
        }
        api::EngineTypedValue sequence;
        sequence.descriptor = search_sequence_column.descriptor;
        sequence.encoded_value = std::to_string(row + 1);
        sequence.state = api::EngineValueState::value;
        api::EngineTypedValue cycle;
        cycle.descriptor = cycle_mark_column.descriptor;
        cycle.encoded_value =
            row + 1 == accumulated.rows.size() ? "true" : "false";
        cycle.state = api::EngineValueState::value;
        accumulated.rows[row].values.push_back(std::move(sequence));
        accumulated.rows[row].values.push_back(std::move(cycle));
      }
      recursive_generated_bindings.push_back(
          std::move(sequence_binding));
      recursive_generated_bindings.push_back(std::move(cycle_binding));
      std::uint64_t metadata_work = 0;
      if (!CheckedMultiply(accumulated.rows.size(), 2, &metadata_work) ||
          !CheckedAdd(metadata_work, 1, &metadata_work) ||
          !CheckedAdd(recursive_work, metadata_work, &recursive_work)) {
        return refuse(
            "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
            "recursive SEARCH/CYCLE metadata work overflowed");
      }
    }
    std::optional<CanonicalObjectFreeValuesExecutionResult>
        result_validation_cancellation;
    bool result_validation_cancelled = false;
    const auto recursive_validated = exec::ValidateCanonicalDescriptorBatch(
        accumulated, reverse_chain.front()->output_descriptor_ids,
        [&]() {
          auto cancelled = poll_recursive_planning_cancellation();
          if (!cancelled.has_value()) return false;
          result_validation_cancellation = std::move(*cancelled);
          return true;
        },
        &result_validation_cancelled);
    if (result_validation_cancelled &&
        result_validation_cancellation.has_value()) {
      return std::move(*result_validation_cancellation);
    }
    if (!recursive_validated.ok) {
      return refuse(std::string(kPayloadDiagnostic),
                    "recursive CTE result: " +
                        recursive_validated.detail);
    }

    std::uint64_t anchor_memory = 1;
    std::uint64_t result_memory = 1;
    std::uint64_t recursive_memory = 1;
    if (!AddBatchMemoryBytes(anchor.batch, &anchor_memory) ||
        !AddBatchMemoryBytes(accumulated, &result_memory) ||
        !CheckedAdd(anchor_memory, result_memory, &recursive_memory) ||
        recursive_memory >
            request.optimizer_request.resource.memory_budget_bytes ||
        !CheckedAdd(total_work, anchor.batch.rows.size(), &total_work) ||
        !CheckedAdd(total_work, 1, &total_work) ||
        !CheckedAdd(total_work, std::max<std::uint64_t>(1, recursive_work),
                    &total_work) ||
        total_work >
            request.optimizer_request.resource.maximum_candidate_count) {
      return refuse(
          "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
          "recursive CTE exceeds an admitted resource bound");
    }

    PreparedRecursiveCteRoot prepared;
    prepared.profile = recursive_cte_profile;
    prepared.anchor_columns = anchor.batch.columns;
    prepared.term = prepared_recursive_term;
    prepared.search_sequence_column = search_sequence_column;
    prepared.cycle_mark_column = cycle_mark_column;
    prepared.maximum_anchor_row_count = anchor.batch.rows.size();
    prepared.maximum_iteration_count =
        std::max<std::size_t>(1, iteration_count);
    prepared.maximum_working_row_count =
        std::max<std::size_t>(
            anchor.batch.rows.size(),
            std::max<std::size_t>(1, maximum_working));
    prepared.maximum_term_output_row_count =
        std::max<std::size_t>(1, maximum_term_output);
    prepared.maximum_result_row_count =
        std::max<std::size_t>(1, accumulated.rows.size());
    prepared.maximum_value_comparison_count =
        static_cast<std::size_t>(recursive_value_comparison_count);
    prepared.rows_examined = static_cast<std::size_t>(recursive_work);
    if (!BindPreparedRecursiveCtePeakMemory(
            &prepared,
            request.optimizer_request.resource.memory_budget_bytes)) {
      return refuse(
          "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
          "recursive CTE materialized payload peak exceeds its admitted "
          "memory budget");
    }
    prepared_recursive_cte = prepared;

    profiles.push_back(
        {recursive_anchor_node->logical_node_id,
         std::string(kValuesImplementationId), values_capability_uuid,
         plan::CanonicalLogicalRelationalNodeKind::kValues,
         exec::PhysicalNodeKind::kValues,
         "canonical.values.materialize.v1", anchor.batch.rows.size(),
         anchor_memory, 0, 0});
    profiles.push_back(
        {recursive_term_node->logical_node_id,
         "cte.recursive-term.int64-increment.typed.v1",
         recursive_term_capability_uuid,
         plan::CanonicalLogicalRelationalNodeKind::kCte,
         exec::PhysicalNodeKind::kCte,
         "canonical.cte.recursive-term-int64-increment.v1", 0, 1, 0, 0});
    profiles.push_back(
        {reverse_chain.front()->logical_node_id,
         recursive_cte_profile.implementation_id,
         recursive_root_capability_uuid,
         plan::CanonicalLogicalRelationalNodeKind::kRecursiveCte,
         exec::PhysicalNodeKind::kRecursiveCte,
         recursive_cte_profile.transformation_id,
         accumulated.rows.size(), prepared.planned_peak_memory_bytes,
         2, 2});
    state.ok = true;
    state.batch = std::move(accumulated);
    state.result_bindings = anchor.result_bindings;
    state.result_bindings.insert(
        state.result_bindings.end(),
        std::make_move_iterator(recursive_generated_bindings.begin()),
        std::make_move_iterator(recursive_generated_bindings.end()));
  } else if (reverse_chain.front()->node_kind ==
      plan::CanonicalLogicalRelationalNodeKind::kValues) {
    state = MaterializeValues(request.relational_dag,
                              *reverse_chain.front(),
                              request.expression_services);
    if (!state.ok) {
      return refuse(std::string(kPayloadDiagnostic),
                    "composition VALUES: " + state.detail);
    }
    std::uint64_t values_memory = 1;
    if (!AddBatchMemoryBytes(state.batch, &values_memory) ||
        values_memory >
            request.optimizer_request.resource.memory_budget_bytes) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                    "composition VALUES exceeds the admitted memory budget");
    }
    profiles.push_back(
        {reverse_chain.front()->logical_node_id,
         std::string(kValuesImplementationId), values_capability_uuid,
         plan::CanonicalLogicalRelationalNodeKind::kValues,
         exec::PhysicalNodeKind::kValues,
         "canonical.values.materialize.v1", state.batch.rows.size(),
         values_memory, 0, 0});
    total_work = state.batch.rows.size();
  } else if (correlated_subquery_base &&
             reverse_chain.front()->node_kind ==
                 plan::CanonicalLogicalRelationalNodeKind::kSubquery) {
    join_left_values = MaterializeValues(
        request.relational_dag, *join_left_node,
        request.expression_services);
    join_right_values = MaterializeValues(
        request.relational_dag, *join_right_node,
        request.expression_services);
    if (!join_left_values->ok || !join_right_values->ok ||
        reverse_chain.front()->output_descriptor_ids !=
            join_right_node->output_descriptor_ids) {
      return refuse(
          std::string(kPayloadDiagnostic),
          !join_left_values->ok
              ? "composition correlated outer VALUES: " +
                    join_left_values->detail
              : (!join_right_values->ok
                     ? "composition correlated inner VALUES: " +
                           join_right_values->detail
                     : "correlated subquery output is not its inner schema"));
    }
    auto correlated = materialize_correlation(
        *join_left_values, *join_right_values,
        *reverse_chain.front(), false, false,
        reverse_chain.front()->semantic_variant_id ==
                "subquery.correlated-int64-equality.v1"
            ? "int64"
            : "");
    if (!correlated.values.ok) {
      return refuse(std::string(kPayloadDiagnostic),
                    correlated.values.detail);
    }
    PreparedCorrelatedSubqueryRoot prepared;
    prepared.outer_binding_descriptor_id =
        join_left_values->batch.columns.front().descriptor_id;
    prepared.inner_reference_descriptor_id =
        join_right_values->batch.columns.front().descriptor_id;
    prepared.outer_row_count =
        join_left_values->batch.rows.size();
    prepared.inner_row_count =
        join_right_values->batch.rows.size();
    prepared.pair_count = correlated.pair_count;
    prepared.output_row_bound = correlated.values.batch.rows.size();
    prepared.comparison_authority_required =
        correlated.comparison_authority_required;
    prepared.comparison_authority_memory_bytes =
        correlated.comparison_authority_memory_bytes;
    prepared.implementation_id =
        reverse_chain.front()->semantic_variant_id ==
                "subquery.correlated-int64-equality.v1"
            ? "subquery.correlated.int64-equality.typed.v1"
            : "subquery.correlated.equality.typed.v1";
    prepared.transformation_id =
        "canonical." + reverse_chain.front()->semantic_variant_id;
    prepared_correlated_subquery = prepared;
    state = std::move(correlated.values);

    std::uint64_t outer_memory = 1;
    std::uint64_t inner_memory = 1;
    std::uint64_t correlated_memory = 1;
    if (!AddBatchMemoryBytes(join_left_values->batch, &outer_memory) ||
        !AddBatchMemoryBytes(join_right_values->batch, &inner_memory) ||
        !AddBatchMemoryBytes(state.batch, &correlated_memory) ||
        !CheckedAdd(correlated_memory, outer_memory,
                    &correlated_memory) ||
        !CheckedAdd(correlated_memory, inner_memory,
                    &correlated_memory) ||
        !CheckedAdd(correlated_memory,
                    prepared.comparison_authority_memory_bytes,
                    &correlated_memory) ||
        correlated_memory >
            request.optimizer_request.resource.memory_budget_bytes ||
        !CheckedAdd(prepared.outer_row_count,
                    prepared.inner_row_count, &total_work) ||
        !CheckedAdd(total_work, prepared.pair_count, &total_work) ||
        total_work >
            request.optimizer_request.resource.maximum_candidate_count) {
      return refuse(
          "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
          "composition correlated subquery exceeds an admitted bound");
    }
    profiles.push_back(
        {join_left_node->logical_node_id,
         std::string(kValuesImplementationId), values_capability_uuid,
         plan::CanonicalLogicalRelationalNodeKind::kValues,
         exec::PhysicalNodeKind::kValues,
         "canonical.values.materialize.v1", prepared.outer_row_count,
         outer_memory, 0, 0});
    profiles.push_back(
        {join_right_node->logical_node_id,
         std::string(kValuesImplementationId), values_capability_uuid,
         plan::CanonicalLogicalRelationalNodeKind::kValues,
         exec::PhysicalNodeKind::kValues,
         "canonical.values.materialize.v1", prepared.inner_row_count,
         inner_memory, 0, 0});
    profiles.push_back(
        {reverse_chain.front()->logical_node_id,
         prepared.implementation_id,
         subquery_capability_uuid,
         plan::CanonicalLogicalRelationalNodeKind::kSubquery,
         exec::PhysicalNodeKind::kSubquery,
         prepared.transformation_id,
         prepared.output_row_bound, correlated_memory, 2, 2});
    profiles.back().runtime_accounted_auxiliary_memory_bytes =
        prepared.comparison_authority_memory_bytes;
  } else if (reverse_chain.front()->node_kind ==
             plan::CanonicalLogicalRelationalNodeKind::kJoin) {
    join_left_values = MaterializeValues(
        request.relational_dag, *join_left_node,
        request.expression_services);
    join_right_values = MaterializeValues(
        request.relational_dag, *join_right_node,
        request.expression_services);
    if (!join_left_values->ok || !join_right_values->ok) {
      return refuse(
          std::string(kPayloadDiagnostic),
          !join_left_values->ok
              ? "composition JOIN left VALUES: " + join_left_values->detail
              : "composition JOIN right VALUES: " +
                    join_right_values->detail);
    }
    if (lateral_subquery_profile.matched) {
      std::vector<std::uint32_t> expected_output =
          join_left_node->output_descriptor_ids;
      expected_output.insert(expected_output.end(),
                             join_right_node->output_descriptor_ids.begin(),
                             join_right_node->output_descriptor_ids.end());
      if (reverse_chain.front()->output_descriptor_ids != expected_output) {
        return refuse(
            std::string(kPayloadDiagnostic),
            "LATERAL/APPLY output does not concatenate its outer and inner "
            "schemas");
      }
      const bool null_extend =
          lateral_subquery_profile.form ==
              exec::CanonicalLateralJoinForm::kLeftLateral ||
          lateral_subquery_profile.form ==
              exec::CanonicalLateralJoinForm::kOuterApply;
      auto lateral = materialize_correlation(
          *join_left_values, *join_right_values,
          *reverse_chain.front(), true, null_extend,
          lateral_subquery_profile.required_operand_type);
      if (!lateral.values.ok) {
        return refuse(std::string(kPayloadDiagnostic),
                      lateral.values.detail);
      }
      PreparedCorrelatedSubqueryRoot prepared;
      prepared.outer_binding_descriptor_id =
          join_left_values->batch.columns.front().descriptor_id;
      prepared.inner_reference_descriptor_id =
          join_right_values->batch.columns.front().descriptor_id;
      prepared.outer_row_count =
          join_left_values->batch.rows.size();
      prepared.inner_row_count =
          join_right_values->batch.rows.size();
      prepared.pair_count = lateral.pair_count;
      prepared.output_row_bound = lateral.values.batch.rows.size();
      prepared.comparison_authority_required =
          lateral.comparison_authority_required;
      prepared.comparison_authority_memory_bytes =
          lateral.comparison_authority_memory_bytes;
      prepared_lateral_subquery = prepared;
      state = std::move(lateral.values);
      join_pair_count = prepared.pair_count;
      join_output_row_bound = prepared.output_row_bound;
      join_implementation_id =
          lateral_subquery_profile.implementation_id;

      std::uint64_t left_memory = 1;
      std::uint64_t right_memory = 1;
      std::uint64_t lateral_memory = 1;
      if (!AddBatchMemoryBytes(join_left_values->batch, &left_memory) ||
          !AddBatchMemoryBytes(join_right_values->batch, &right_memory) ||
          !AddBatchMemoryBytes(state.batch, &lateral_memory) ||
          !CheckedAdd(lateral_memory, left_memory, &lateral_memory) ||
          !CheckedAdd(lateral_memory, right_memory, &lateral_memory) ||
          !CheckedAdd(lateral_memory,
                      prepared.comparison_authority_memory_bytes,
                      &lateral_memory) ||
          lateral_memory >
              request.optimizer_request.resource.memory_budget_bytes ||
          !CheckedAdd(prepared.outer_row_count,
                      prepared.inner_row_count, &total_work) ||
          !CheckedAdd(total_work, prepared.pair_count, &total_work) ||
          total_work >
              request.optimizer_request.resource.maximum_candidate_count) {
        return refuse(
            "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
            "composition LATERAL/APPLY exceeds an admitted bound");
      }
      profiles.push_back(
          {join_left_node->logical_node_id,
           std::string(kValuesImplementationId), values_capability_uuid,
           plan::CanonicalLogicalRelationalNodeKind::kValues,
           exec::PhysicalNodeKind::kValues,
           "canonical.values.materialize.v1", prepared.outer_row_count,
           left_memory, 0, 0});
      profiles.push_back(
          {join_right_node->logical_node_id,
           std::string(kValuesImplementationId), values_capability_uuid,
           plan::CanonicalLogicalRelationalNodeKind::kValues,
           exec::PhysicalNodeKind::kValues,
           "canonical.values.materialize.v1", prepared.inner_row_count,
           right_memory, 0, 0});
      profiles.push_back(
          {reverse_chain.front()->logical_node_id,
           lateral_subquery_profile.implementation_id,
           join_capability_uuid,
           plan::CanonicalLogicalRelationalNodeKind::kJoin,
           exec::PhysicalNodeKind::kJoin,
           lateral_subquery_profile.transformation_id,
           prepared.output_row_bound, lateral_memory, 2, 2});
      profiles.back().runtime_accounted_auxiliary_memory_bytes =
          prepared.comparison_authority_memory_bytes;
    } else {
    prepared_join = PrepareJoinRootForComposition(
        request.relational_dag, *reverse_chain.front(), *join_left_node,
        *join_right_node, *join_left_values, *join_right_values, *join_kind);
    if (!prepared_join->ok) {
      return refuse(std::string(kPayloadDiagnostic), prepared_join->detail);
    }

    const auto left_count = join_left_values->batch.rows.size();
    const auto right_count = join_right_values->batch.rows.size();
    if (left_count != 0 &&
        right_count > std::numeric_limits<std::size_t>::max() / left_count) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "composition JOIN pair cardinality overflowed");
    }
    join_pair_count = left_count * right_count;
    join_truth_values.reserve(join_pair_count);
    if (*join_kind == exec::CanonicalAcceptedJoinKind::kCross) {
      join_truth_values.assign(
          join_pair_count, api::EngineSqlTruthValue::true_value);
    } else {
      std::vector<api::EngineTypedValue> predicate_row_values;
      predicate_row_values.reserve(
          join_left_values->batch.columns.size() +
          join_right_values->batch.columns.size());
      for (const auto& left_row : join_left_values->batch.rows) {
        for (const auto& right_row : join_right_values->batch.rows) {
          predicate_row_values.clear();
          predicate_row_values.insert(predicate_row_values.end(),
                                      left_row.values.begin(),
                                      left_row.values.end());
          predicate_row_values.insert(predicate_row_values.end(),
                                      right_row.values.begin(),
                                      right_row.values.end());
          api::EngineSqlTruthValue truth =
              api::EngineSqlTruthValue::unknown;
          std::string detail;
          if (!expression_runtime.EvaluatePredicateForConsumer(
                  prepared_join->predicate_expression_id,
                  prepared_join->predicate_row_binding,
                  predicate_row_values,
                  api::EngineCanonicalExpressionConsumer::join, &truth,
                  &detail)) {
            return refuse(
                std::string(kPayloadDiagnostic),
                "composition JOIN predicate pair " +
                    std::to_string(join_truth_values.size()) + ": " +
                    detail);
          }
          join_truth_values.push_back(truth);
        }
      }
    }

    std::vector<std::size_t> accepted_pairs;
    std::vector<bool> matched_left(left_count, false);
    std::vector<bool> matched_right(right_count, false);
    for (std::size_t pair = 0; pair < join_truth_values.size(); ++pair) {
      if (join_truth_values[pair] !=
          api::EngineSqlTruthValue::true_value) {
        continue;
      }
      accepted_pairs.push_back(pair);
      matched_left[pair / right_count] = true;
      matched_right[pair % right_count] = true;
    }

    state.ok = true;
    const bool left_only =
        *join_kind == exec::CanonicalAcceptedJoinKind::kLeftSemi ||
        *join_kind == exec::CanonicalAcceptedJoinKind::kLeftAnti;
    state.batch.columns = join_left_values->batch.columns;
    if (!left_only) {
      state.batch.columns.insert(state.batch.columns.end(),
                                 join_right_values->batch.columns.begin(),
                                 join_right_values->batch.columns.end());
    }
    const auto left_width = join_left_values->batch.columns.size();
    std::vector<bool> derived_nullable_columns(state.batch.columns.size(),
                                               false);
    if (*join_kind == exec::CanonicalAcceptedJoinKind::kRightOuter ||
        *join_kind == exec::CanonicalAcceptedJoinKind::kFullOuter) {
      for (std::size_t column = 0; column < left_width; ++column) {
        state.batch.columns[column].nullable = true;
        if (!exec::DeriveCanonicalNullableDescriptorEncoding(
                &state.batch.columns[column].descriptor)) {
          return refuse(
              std::string(kPayloadDiagnostic),
              "outer join left result lacks a nullable descriptor carrier");
        }
        derived_nullable_columns[column] = true;
      }
    }
    if (*join_kind == exec::CanonicalAcceptedJoinKind::kLeftOuter ||
        *join_kind == exec::CanonicalAcceptedJoinKind::kFullOuter) {
      for (std::size_t column = left_width;
           column < state.batch.columns.size(); ++column) {
        state.batch.columns[column].nullable = true;
        if (!exec::DeriveCanonicalNullableDescriptorEncoding(
                &state.batch.columns[column].descriptor)) {
          return refuse(
              std::string(kPayloadDiagnostic),
              "outer join right result lacks a nullable descriptor carrier");
        }
        derived_nullable_columns[column] = true;
      }
    }
    const auto append_joined = [&](const std::size_t left_ordinal,
                                   const std::size_t right_ordinal) {
      exec::DescriptorTuple row =
          join_left_values->batch.rows[left_ordinal];
      const auto& right_values =
          join_right_values->batch.rows[right_ordinal].values;
      row.values.insert(row.values.end(), right_values.begin(),
                        right_values.end());
      state.batch.rows.push_back(std::move(row));
    };
    const auto append_unmatched_left = [&](const std::size_t left_ordinal) {
      exec::DescriptorTuple row =
          join_left_values->batch.rows[left_ordinal];
      for (const auto& column : join_right_values->batch.columns) {
        api::EngineTypedValue null_value;
        null_value.descriptor = column.descriptor;
        null_value.is_null = true;
        null_value.state = api::EngineValueState::sql_null;
        row.values.push_back(std::move(null_value));
      }
      state.batch.rows.push_back(std::move(row));
    };
    const auto append_unmatched_right = [&](const std::size_t right_ordinal) {
      exec::DescriptorTuple row;
      for (const auto& column : join_left_values->batch.columns) {
        api::EngineTypedValue null_value;
        null_value.descriptor = column.descriptor;
        null_value.is_null = true;
        null_value.state = api::EngineValueState::sql_null;
        row.values.push_back(std::move(null_value));
      }
      const auto& right_values =
          join_right_values->batch.rows[right_ordinal].values;
      row.values.insert(row.values.end(), right_values.begin(),
                        right_values.end());
      state.batch.rows.push_back(std::move(row));
    };

    if (left_only) {
      const bool emit_matches =
          *join_kind == exec::CanonicalAcceptedJoinKind::kLeftSemi;
      for (std::size_t left = 0; left < left_count; ++left) {
        if (matched_left[left] == emit_matches) {
          state.batch.rows.push_back(join_left_values->batch.rows[left]);
        }
      }
    } else if (*join_kind ==
               exec::CanonicalAcceptedJoinKind::kRightOuter) {
      for (std::size_t right = 0; right < right_count; ++right) {
        bool emitted = false;
        for (const auto pair : accepted_pairs) {
          if (pair % right_count == right) {
            append_joined(pair / right_count, right);
            emitted = true;
          }
        }
        if (!emitted) append_unmatched_right(right);
      }
    } else if (*join_kind ==
                   exec::CanonicalAcceptedJoinKind::kLeftOuter ||
               *join_kind ==
                   exec::CanonicalAcceptedJoinKind::kFullOuter) {
      std::size_t accepted = 0;
      for (std::size_t left = 0; left < left_count; ++left) {
        bool emitted = false;
        while (accepted < accepted_pairs.size() &&
               accepted_pairs[accepted] / right_count == left) {
          append_joined(left, accepted_pairs[accepted] % right_count);
          ++accepted;
          emitted = true;
        }
        if (!emitted) append_unmatched_left(left);
      }
      if (*join_kind == exec::CanonicalAcceptedJoinKind::kFullOuter) {
        for (std::size_t right = 0; right < right_count; ++right) {
          if (!matched_right[right]) append_unmatched_right(right);
        }
      }
    } else {
      for (const auto pair : accepted_pairs) {
        append_joined(pair / right_count, pair % right_count);
      }
    }
    for (auto& row : state.batch.rows) {
      for (std::size_t column = 0; column < state.batch.columns.size();
           ++column) {
        if (derived_nullable_columns[column]) {
          row.values[column].descriptor =
              state.batch.columns[column].descriptor;
        }
      }
    }
    state.result_bindings = prepared_join->result_bindings;
    join_output_row_bound = state.batch.rows.size();
    join_implementation_id =
        "join." + join_component + ".3vl.nested.v1";

    std::uint64_t left_memory = 1;
    std::uint64_t right_memory = 1;
    std::uint64_t join_memory = 1;
    std::uint64_t join_state_memory = 0;
    if (!AddBatchMemoryBytes(join_left_values->batch, &left_memory) ||
        !AddBatchMemoryBytes(join_right_values->batch, &right_memory) ||
        !AddBatchMemoryBytes(state.batch, &join_memory) ||
        !exec::BoundCanonicalJoinRetainedStateBytes(
            left_count, right_count, &join_state_memory) ||
        !CheckedAdd(join_memory, left_memory, &join_memory) ||
        !CheckedAdd(join_memory, right_memory, &join_memory) ||
        !CheckedAdd(join_memory, join_state_memory, &join_memory) ||
        join_memory >
            request.optimizer_request.resource.memory_budget_bytes) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                    "composition JOIN exceeds the admitted memory budget");
    }
    profiles.push_back(
        {join_left_node->logical_node_id,
         std::string(kValuesImplementationId), values_capability_uuid,
         plan::CanonicalLogicalRelationalNodeKind::kValues,
         exec::PhysicalNodeKind::kValues,
         "canonical.values.materialize.v1", left_count, left_memory, 0, 0});
    profiles.push_back(
        {join_right_node->logical_node_id,
         std::string(kValuesImplementationId), values_capability_uuid,
         plan::CanonicalLogicalRelationalNodeKind::kValues,
         exec::PhysicalNodeKind::kValues,
         "canonical.values.materialize.v1", right_count, right_memory, 0, 0});
    profiles.push_back(
        {reverse_chain.front()->logical_node_id, join_implementation_id,
         join_capability_uuid,
         plan::CanonicalLogicalRelationalNodeKind::kJoin,
         exec::PhysicalNodeKind::kJoin,
         "canonical." + join_implementation_id, join_output_row_bound,
         request.optimizer_request.resource.memory_budget_bytes, 2, 2});
    profiles.back().runtime_accounted_auxiliary_memory_bytes =
        join_state_memory;
    if (!CheckedAdd(left_count, right_count, &total_work) ||
        !CheckedAdd(total_work, join_pair_count, &total_work) ||
        total_work >
            request.optimizer_request.resource.maximum_candidate_count) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                    "composition JOIN work exceeds the admitted bound");
    }
    }
  } else {
    for (const auto& [node_id, node] : set_base_nodes) {
      if (node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues) {
        continue;
      }
      auto materialized = MaterializeValues(
          request.relational_dag, *node, request.expression_services);
      if (!materialized.ok) {
        return refuse(std::string(kPayloadDiagnostic),
                      "composition SET VALUES: " + materialized.detail);
      }
      std::uint64_t values_memory = 1;
      if (!AddBatchMemoryBytes(materialized.batch, &values_memory) ||
          values_memory >
              request.optimizer_request.resource.memory_budget_bytes ||
          !CheckedAdd(total_work, materialized.batch.rows.size(),
                      &total_work) ||
          total_work >
              request.optimizer_request.resource.maximum_candidate_count) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                      "composition SET VALUES exceeds an admitted bound");
      }
      profiles.push_back(
          {node_id, std::string(kValuesImplementationId),
           values_capability_uuid,
           plan::CanonicalLogicalRelationalNodeKind::kValues,
           exec::PhysicalNodeKind::kValues,
           "canonical.values.materialize.v1", materialized.batch.rows.size(),
           values_memory, 0, 0});
      set_materialized_values.emplace(node_id, std::move(materialized));
    }

    std::unordered_set<std::uint32_t> pending_set_nodes;
    for (const auto& [node_id, profile] : set_profiles) {
      (void)profile;
      pending_set_nodes.insert(node_id);
    }
    while (!pending_set_nodes.empty()) {
      bool progressed = false;
      for (auto pending = pending_set_nodes.begin();
           pending != pending_set_nodes.end();) {
        const auto node_id = *pending;
        const auto* node = set_base_nodes.at(node_id);
        const auto left = set_materialized_values.find(
            node->input_logical_node_ids[0]);
        const auto right = set_materialized_values.find(
            node->input_logical_node_ids[1]);
        if (left == set_materialized_values.end() ||
            right == set_materialized_values.end()) {
          ++pending;
          continue;
        }
        auto prepared = PrepareSetOperationRootForComposition(
            request.context, request.relational_dag, *node, left->second,
            right->second, set_profiles.at(node_id));
        if (!prepared.ok) {
          return refuse(std::string(kPayloadDiagnostic), prepared.detail);
        }
        auto materialized = MaterializeSetOperationPlanningStateForComposition(
            prepared, set_profiles.at(node_id), left->second, right->second);
        if (!materialized.values.ok ||
            materialized.output_bound >
                std::numeric_limits<std::size_t>::max() ||
            materialized.comparison_bound >
                std::numeric_limits<std::size_t>::max()) {
          return refuse(
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
              materialized.values.detail.empty()
                  ? "composition SET row/comparison bound overflowed"
                  : materialized.values.detail);
        }

        std::uint64_t left_memory = 1;
        std::uint64_t right_memory = 1;
        std::uint64_t set_memory = 1;
        if (!AddBatchMemoryBytes(left->second.batch, &left_memory) ||
            !AddBatchMemoryBytes(right->second.batch, &right_memory) ||
            !AddBatchMemoryBytes(materialized.values.batch, &set_memory) ||
            !CheckedAdd(set_memory, left_memory, &set_memory) ||
            !CheckedAdd(set_memory, right_memory, &set_memory) ||
            !CheckedAdd(set_memory, materialized.work_bound, &set_memory) ||
            set_memory >
                request.optimizer_request.resource.memory_budget_bytes ||
            !CheckedAdd(total_work, materialized.work_bound, &total_work) ||
            total_work >
                request.optimizer_request.resource.maximum_candidate_count) {
          return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                        "composition SET exceeds an admitted bound");
        }

        const auto& profile = set_profiles.at(node_id);
        profiles.push_back(
            {node_id, profile.implementation_id,
             set_capability_uuids.at(profile.implementation_id),
             plan::CanonicalLogicalRelationalNodeKind::kSetOperation,
             exec::PhysicalNodeKind::kSetOperation,
             profile.physical_semantic_id,
             static_cast<std::size_t>(materialized.output_bound),
             request.optimizer_request.resource.memory_budget_bytes,
             2, 2});
        profiles.back().runtime_accounted_auxiliary_memory_bytes =
            materialized.work_bound;
        prepared_set_nodes.emplace(
            node_id,
            PreparedLiveSetNode{
                profile, std::move(prepared),
                static_cast<std::size_t>(materialized.output_bound),
                static_cast<std::size_t>(std::max<std::uint64_t>(
                    1, materialized.comparison_bound))});
        set_materialized_values.emplace(
            node_id, std::move(materialized.values));
        pending = pending_set_nodes.erase(pending);
        progressed = true;
      }
      if (!progressed) {
        return refuse(std::string(kPayloadDiagnostic),
                      "composition SET subtree is cyclic or unresolved");
      }
    }
    state = set_materialized_values.at(
        reverse_chain.front()->logical_node_id);
  }
  const auto add_work = [&](const std::uint64_t work) {
    return CheckedAdd(total_work, work, &total_work) &&
           total_work <=
               request.optimizer_request.resource.maximum_candidate_count;
  };
  for (std::size_t index = 1; index < reverse_chain.size(); ++index) {
    const auto& node = *reverse_chain[index];
    const auto& input_node = *reverse_chain[index - 1];
    const auto& input_batch = state.batch;
    const auto input_bindings = state.result_bindings;
    const auto input_row_count = input_batch.rows.size();
    std::uint64_t input_memory = 1;
    if (!AddBatchMemoryBytes(input_batch, &input_memory)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "composition input memory overflowed");
    }

    std::string implementation_id;
    std::string capability_uuid;
    std::string transformation_rule;
    exec::PhysicalNodeKind physical_kind = exec::PhysicalNodeKind::kValues;
    std::uint64_t auxiliary_memory = 0;
    std::uint64_t registry_aggregate_distinct_peak_memory = 0;
    std::vector<std::string> required_property_uuids;
    std::vector<std::string> delivered_property_uuids;
    std::vector<plan::CanonicalLogicalPropertyKind> property_kinds;

    switch (node.node_kind) {
      case plan::CanonicalLogicalRelationalNodeKind::kFilter: {
        PreparedFilterRoot prepared;
        filter_expression_consumer =
            api::EngineCanonicalExpressionConsumer::filter;
        filter_predicate_consumer = api::EnginePredicateConsumer::filter;
        if (IsLiveGroupedHavingProfileForComposition(node.semantic_variant_id)) {
          filter_expression_consumer =
              api::EngineCanonicalExpressionConsumer::aggregate;
          filter_predicate_consumer = api::EnginePredicateConsumer::having;
          if (!prepared_grouped_aggregate.has_value() ||
              input_node.node_kind !=
                  plan::CanonicalLogicalRelationalNodeKind::kAggregate ||
              input_node.input_logical_node_ids.size() != 1) {
            return refuse(std::string(kPayloadDiagnostic),
                          "composed HAVING lacks its grouped aggregate");
          }
          const auto aggregate_input = std::ranges::find_if(
              graph.nodes, [&](const auto& candidate) {
                return candidate.logical_node_id ==
                       input_node.input_logical_node_ids.front();
              });
          if (aggregate_input == graph.nodes.end()) {
            return refuse(std::string(kPayloadDiagnostic),
                          "composed HAVING aggregate input is unresolved");
          }
          const auto having = PrepareGroupedHavingRootForComposition(
              request.relational_dag, node, input_node, *aggregate_input,
              *prepared_grouped_aggregate);
          if (!having.ok ||
              having.output_column_count != state.batch.columns.size()) {
            return refuse(std::string(kPayloadDiagnostic), having.detail);
          }
          prepared.predicate_expression_id =
              having.predicate_expression_id;
          prepared.predicate_row_binding = having.row_binding;
          prepared.result_bindings = state.result_bindings;
          prepared.ok = true;
        } else {
          prepared = PrepareFilterRootForComposition(request.relational_dag, node,
                                       input_node, state);
        }
        if (!prepared.ok) {
          return refuse(std::string(kPayloadDiagnostic), prepared.detail);
        }
        std::vector<api::EngineSqlTruthValue> truth_values;
        truth_values.reserve(input_row_count);
        exec::DescriptorBatch output;
        output.columns = input_batch.columns;
        output.rows.reserve(input_row_count);
        if (planning_values_exact) {
          for (std::size_t row_ordinal = 0;
               row_ordinal < input_batch.rows.size(); ++row_ordinal) {
            const auto& row = input_batch.rows[row_ordinal];
            api::EngineSqlTruthValue truth =
                api::EngineSqlTruthValue::unknown;
            std::string detail;
            if (!expression_runtime.EvaluatePredicateForConsumer(
                    prepared.predicate_expression_id,
                    prepared.predicate_row_binding, row.values,
                    filter_expression_consumer, &truth, &detail)) {
              return refuse(
                  std::string(kPayloadDiagnostic),
                  "FILTER row " + std::to_string(row_ordinal) +
                      ": " + detail);
            }
            if (truth == api::EngineSqlTruthValue::true_value) {
              output.rows.push_back(row);
            }
          }
        } else {
          // The complex aggregate callback owns the exact value. Preserve
          // its one-row planning batch as a conservative cardinality/schema
          // upper bound; the filter registration evaluates this predicate
          // again against the actual callback batch under current MGA.
          output.rows = input_batch.rows;
        }
        if (!add_work(input_row_count)) {
          return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                        "composition FILTER work exceeds the admitted bound");
        }
        prepared_filter = std::move(prepared);
        filter_input_row_count = input_row_count;
        state.batch = std::move(output);
        state.result_bindings = input_bindings;
        implementation_id = "filter.3vl.row.v1";
        capability_uuid = filter_capability_uuid;
        transformation_rule = "canonical.filter.composed-row.3vl.v1";
        physical_kind = exec::PhysicalNodeKind::kFilter;
        if (!CheckedMultiply(input_row_count,
                             sizeof(api::EngineSqlTruthValue),
                             &auxiliary_memory)) {
          return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                        "composition FILTER state memory overflowed");
        }
        break;
      }
      case plan::CanonicalLogicalRelationalNodeKind::kProject: {
        auto prepared = node.bound_expression_ids.empty()
            ? PrepareDescriptorDirectProjectRootForComposition(
                  request.relational_dag, node, input_node, state)
            : PrepareExpressionProjectRootForComposition(
                  request.relational_dag, node, input_node, state,
                  request.expression_services);
        if (!prepared.ok) {
          return refuse(std::string(kPayloadDiagnostic), prepared.detail);
        }
        exec::DescriptorBatch output;
        if (prepared.expression_projection) {
          output = prepared.expression_output_batch;
          std::uint64_t work = 0;
          if (!CheckedMultiply(input_row_count,
                               prepared.expressions.size(), &work) ||
              !add_work(work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                "composition PROJECT work exceeds the admitted bound");
          }
        } else {
          output.columns.reserve(prepared.projected_columns.size());
          for (const auto column : prepared.projected_columns) {
            output.columns.push_back(input_batch.columns[column]);
          }
          output.rows.reserve(input_row_count);
          for (const auto& row : input_batch.rows) {
            exec::DescriptorTuple projected;
            projected.values.reserve(prepared.projected_columns.size());
            for (const auto column : prepared.projected_columns) {
              projected.values.push_back(row.values[column]);
            }
            output.rows.push_back(std::move(projected));
          }
          if (!add_work(input_row_count)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                "composition PROJECT work exceeds the admitted bound");
          }
        }
        prepared_project = std::move(prepared);
        project_input_row_count = input_row_count;
        project_implementation_id =
            prepared_project->expression_projection
                ? "project.typed.expression-row.v1"
                : "project.typed.row.v1";
        state.batch = std::move(output);
        state.result_bindings = prepared_project->result_bindings;
        implementation_id = project_implementation_id;
        capability_uuid = project_capability_uuid;
        transformation_rule = prepared_project->expression_projection
            ? "canonical.project.composed-expression-row.v1"
            : "canonical.project.composed-descriptor-row.v1";
        physical_kind = exec::PhysicalNodeKind::kProject;
        break;
      }
      case plan::CanonicalLogicalRelationalNodeKind::kAggregate: {
        if (node.semantic_variant_id ==
            "aggregate.global-count-star.v1") {
          if (input_row_count >
              static_cast<std::size_t>(
                  std::numeric_limits<std::int64_t>::max())) {
            return refuse("QOW-DIAG-QRY-007-AGGREGATE-OVERFLOW-V1",
                          "COUNT(*) exceeds int64 result width");
          }
          auto prepared = PrepareGlobalAggregateRootForComposition(
              request.relational_dag, node, input_node, state,
              exec::CanonicalAggregateFunction::count, true, false, false);
          if (!prepared.ok || !add_work(input_row_count)) {
            return refuse(
                prepared.ok
                    ? "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"
                    : std::string(kPayloadDiagnostic),
                prepared.ok
                    ? "composition COUNT(*) work exceeds the admitted bound"
                    : prepared.detail);
          }
          exec::DescriptorBatch output;
          output.columns.push_back(prepared.result_column);
          exec::DescriptorTuple tuple;
          api::EngineTypedValue count;
          count.descriptor = prepared.result_column.descriptor;
          count.encoded_value = std::to_string(input_row_count);
          count.is_null = false;
          count.state = api::EngineValueState::value;
          tuple.values.push_back(std::move(count));
          output.rows.push_back(std::move(tuple));
          const auto canonical = exec::ValidateCanonicalDescriptorBatch(
              output, node.output_descriptor_ids);
          const auto values = exec::ValidateDescriptorBatch(output);
          if (!canonical.ok || !values.ok) {
            return refuse(
                std::string(kPayloadDiagnostic),
                !canonical.ok
                    ? "COUNT(*) planning state: " + canonical.detail
                    : "COUNT(*) planning state: " + values.detail);
          }
          prepared_count_star = std::move(prepared);
          count_star_input_row_count = input_row_count;
          state.batch = std::move(output);
          state.result_bindings =
              prepared_count_star->result_bindings;
          implementation_id = "aggregate.count-star.v1";
          capability_uuid = count_star_capability_uuid;
          transformation_rule =
              "canonical.aggregate.composed-global-count-star.v1";
          physical_kind = exec::PhysicalNodeKind::kAggregate;
          auxiliary_memory = sizeof(std::int64_t);
          break;
        }
        if (global_aggregate_profile.matched &&
            global_aggregate_profile.function ==
                exec::CanonicalAggregateFunction::count) {
          auto prepared = PrepareGlobalAggregateRootForComposition(
              request.relational_dag, node, input_node, state,
              exec::CanonicalAggregateFunction::count, false,
              global_aggregate_profile.distinct,
              global_aggregate_profile.has_filter);
          if (!prepared.ok || prepared.value_columns.size() != 1) {
            return refuse(std::string(kPayloadDiagnostic), prepared.detail);
          }
          PlanningAggregateModifierState modifiers;
          std::string modifier_detail;
          if (!InitializePlanningAggregateModifierStateForComposition(
                  input_batch, prepared, input_memory,
                  request.optimizer_request.resource.memory_budget_bytes,
                  &modifiers, &modifier_detail)) {
            return refuse(std::string(kPayloadDiagnostic),
                          "composition COUNT modifiers: " + modifier_detail);
          }
          std::uint64_t aggregate_work = modifiers.admitted_row_count;
          if (prepared.distinct &&
              (!CheckedAdd(aggregate_work,
                           modifiers.distinct_generation_bound,
                           &aggregate_work) ||
               !CheckedAdd(aggregate_work,
                           modifiers.distinct_comparison_bound,
                           &aggregate_work))) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition COUNT(DISTINCT) work bound overflowed");
          }
          if (!add_work(aggregate_work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                "composition COUNT(expression) work exceeds the admitted "
                "bound");
          }
          std::size_t count_value = 0;
          const auto value_column = prepared.value_columns.front();
          {
            PlanningAggregateDistinctState distinct_state;
            if (!InitializePlanningAggregateDistinctStateForComposition(
                    request.context, input_batch, prepared,
                    modifiers.distinct_memory_bound, &distinct_state,
                    &modifier_detail, modifiers.admitted_row_count)) {
              return refuse(std::string(kPayloadDiagnostic),
                            "composition COUNT(DISTINCT): " +
                                modifier_detail);
            }
            for (std::size_t row = 0; row < input_batch.rows.size(); ++row) {
              if (modifiers.filter_truth_values.has_value() &&
                  (*modifiers.filter_truth_values)[row] !=
                      api::EngineSqlTruthValue::true_value) {
                continue;
              }
              const auto& value = input_batch.rows[row].values[value_column];
              if (prepared.distinct) {
                bool distinct_admitted = false;
                const std::array<const api::EngineTypedValue*, 2> values = {
                    &value, nullptr};
                if (!AdmitPlanningAggregateDistinctTupleForComposition(
                        values, 1, modifiers.distinct_generation_bound,
                        modifiers.distinct_comparison_bound,
                        modifiers.distinct_memory_bound, &distinct_state,
                        &distinct_admitted, &modifier_detail)) {
                  return refuse(std::string(kPayloadDiagnostic),
                                "composition COUNT(DISTINCT): " +
                                    modifier_detail);
                }
                if (!distinct_admitted) {
                  continue;
                }
              }
              if (value.state == api::EngineValueState::sql_null ||
                  value.is_null) {
                continue;
              }
              ++count_value;
            }
            registry_aggregate_distinct_peak_memory =
                distinct_state.peak_memory_bytes;
          }
          exec::DescriptorBatch output;
          output.columns.push_back(prepared.result_column);
          exec::DescriptorTuple tuple;
          api::EngineTypedValue count;
          count.descriptor = prepared.result_column.descriptor;
          count.encoded_value = std::to_string(count_value);
          count.is_null = false;
          count.state = api::EngineValueState::value;
          tuple.values.push_back(std::move(count));
          output.rows.push_back(std::move(tuple));
          const auto canonical = exec::ValidateCanonicalDescriptorBatch(
              output, node.output_descriptor_ids);
          const auto values = exec::ValidateDescriptorBatch(output);
          if (!canonical.ok || !values.ok) {
            return refuse(
                std::string(kPayloadDiagnostic),
                !canonical.ok
                    ? "COUNT(expression) planning state: " + canonical.detail
                    : "COUNT(expression) planning state: " + values.detail);
          }
          prepared_registry_aggregate = std::move(prepared);
          registry_aggregate_input_row_count = input_row_count;
          state.batch = std::move(output);
          state.result_bindings =
              prepared_registry_aggregate->result_bindings;
          implementation_id = "aggregate.registry-core.v1";
          capability_uuid = registry_aggregate_capability_uuid;
          transformation_rule =
              "canonical.aggregate.composed-global-count-expression.v1";
          physical_kind = exec::PhysicalNodeKind::kAggregate;
          if (!CheckedAdd(modifiers.filter_truth_memory_bytes,
                          modifiers.transition_memory_bytes,
                          &auxiliary_memory)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition COUNT modifier memory overflowed");
          }
          registry_aggregate_filter_truth_memory_bytes =
              modifiers.filter_truth_memory_bytes;
          break;
        }
        if (global_aggregate_profile.matched &&
            (global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::stddev_pop ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::variance_pop ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::stddev ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::variance ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::stddev_samp ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::variance_samp)) {
          const auto function = global_aggregate_profile.function;
          auto prepared = PrepareGlobalAggregateRootForComposition(
              request.relational_dag, node, input_node, state, function,
              false, global_aggregate_profile.distinct,
              global_aggregate_profile.has_filter);
          if (!prepared.ok || prepared.value_columns.size() != 1) {
            return refuse(std::string(kPayloadDiagnostic), prepared.detail);
          }
          PlanningAggregateModifierState modifiers;
          std::string modifier_detail;
          if (!InitializePlanningAggregateModifierStateForComposition(
                  input_batch, prepared, input_memory,
                  request.optimizer_request.resource.memory_budget_bytes,
                  &modifiers, &modifier_detail)) {
            return refuse(std::string(kPayloadDiagnostic),
                          "composition statistical modifiers: " +
                              modifier_detail);
          }
          std::uint64_t aggregate_work = modifiers.admitted_row_count;
          if (prepared.distinct &&
              (!CheckedAdd(aggregate_work,
                           modifiers.distinct_generation_bound,
                           &aggregate_work) ||
               !CheckedAdd(aggregate_work,
                           modifiers.distinct_comparison_bound,
                           &aggregate_work))) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition statistical DISTINCT work bound overflowed");
          }
          std::uint64_t non_null_count = 0;
          long double numeric_mean = 0.0L;
          long double numeric_m2 = 0.0L;
          const auto value_column = prepared.value_columns.front();
          {
            PlanningAggregateDistinctState distinct_state;
            if (!InitializePlanningAggregateDistinctStateForComposition(
                    request.context, input_batch, prepared,
                    modifiers.distinct_memory_bound, &distinct_state,
                    &modifier_detail, modifiers.admitted_row_count)) {
              return refuse(std::string(kPayloadDiagnostic),
                            "composition statistical DISTINCT: " +
                                modifier_detail);
            }
            for (std::size_t row = 0; row < input_batch.rows.size(); ++row) {
              if (modifiers.filter_truth_values.has_value() &&
                  (*modifiers.filter_truth_values)[row] !=
                      api::EngineSqlTruthValue::true_value) {
                continue;
              }
              const auto& value = input_batch.rows[row].values[value_column];
              if (prepared.distinct) {
                bool distinct_admitted = false;
                const std::array<const api::EngineTypedValue*, 2> values = {
                    &value, nullptr};
                if (!AdmitPlanningAggregateDistinctTupleForComposition(
                        values, 1, modifiers.distinct_generation_bound,
                        modifiers.distinct_comparison_bound,
                        modifiers.distinct_memory_bound, &distinct_state,
                        &distinct_admitted, &modifier_detail)) {
                  return refuse(std::string(kPayloadDiagnostic),
                                "composition statistical DISTINCT: " +
                                    modifier_detail);
                }
                if (!distinct_admitted) continue;
              }
              if (value.state == api::EngineValueState::sql_null ||
                  value.is_null) {
                continue;
              }
              if (value.descriptor.canonical_type_name != "int64") {
                return refuse(
                    std::string(kPayloadDiagnostic),
                    "composition statistical input is not canonical int64");
              }
              std::int64_t decoded = 0;
              const auto [end, error] = std::from_chars(
                  value.encoded_value.data(),
                  value.encoded_value.data() + value.encoded_value.size(),
                  decoded);
              if (error != std::errc{} ||
                  end != value.encoded_value.data() +
                             value.encoded_value.size() ||
                  non_null_count ==
                      std::numeric_limits<std::uint64_t>::max()) {
                return refuse(
                    std::string(kPayloadDiagnostic),
                    "composition statistical input or count is invalid");
              }
              ++non_null_count;
              const auto numeric = static_cast<long double>(decoded);
              const auto count = static_cast<long double>(non_null_count);
              const auto delta = numeric - numeric_mean;
              numeric_mean += delta / count;
              const auto delta2 = numeric - numeric_mean;
              numeric_m2 += delta * delta2;
              if (!std::isfinite(static_cast<double>(numeric_mean)) ||
                  !std::isfinite(static_cast<double>(numeric_m2))) {
                return refuse(std::string(kPayloadDiagnostic),
                              "composition statistical state overflowed");
              }
            }
            registry_aggregate_distinct_peak_memory =
                distinct_state.peak_memory_bytes;
          }
          if (!add_work(aggregate_work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                "composition statistical work exceeds the admitted bound");
          }
          const bool population =
              function == exec::CanonicalAggregateFunction::stddev_pop ||
              function == exec::CanonicalAggregateFunction::variance_pop;
          const bool deviation =
              function == exec::CanonicalAggregateFunction::stddev_pop ||
              function == exec::CanonicalAggregateFunction::stddev ||
              function == exec::CanonicalAggregateFunction::stddev_samp;
          const bool has_result = non_null_count >= (population ? 1U : 2U);
          exec::DescriptorBatch output;
          output.columns.push_back(prepared.result_column);
          exec::DescriptorTuple tuple;
          api::EngineTypedValue statistic_value;
          statistic_value.descriptor = prepared.result_column.descriptor;
          statistic_value.is_null = !has_result;
          statistic_value.state = has_result
                                      ? api::EngineValueState::value
                                      : api::EngineValueState::sql_null;
          if (has_result) {
            const auto denominator = static_cast<long double>(
                population ? non_null_count : non_null_count - 1);
            auto statistic = numeric_m2 / denominator;
            if (statistic < 0.0L && statistic > -1e-18L) statistic = 0.0L;
            if (deviation) statistic = std::sqrt(statistic);
            if (!std::isfinite(static_cast<double>(statistic))) {
              return refuse(std::string(kPayloadDiagnostic),
                            "composition statistical result overflowed");
            }
            std::ostringstream encoded;
            encoded << std::setprecision(17)
                    << static_cast<double>(statistic);
            statistic_value.encoded_value = encoded.str();
          }
          tuple.values.push_back(std::move(statistic_value));
          output.rows.push_back(std::move(tuple));
          const auto canonical = exec::ValidateCanonicalDescriptorBatch(
              output, node.output_descriptor_ids);
          const auto values = exec::ValidateDescriptorBatch(output);
          if (!canonical.ok || !values.ok) {
            return refuse(
                std::string(kPayloadDiagnostic),
                !canonical.ok
                    ? "statistical planning state: " + canonical.detail
                    : "statistical planning state: " + values.detail);
          }
          prepared_registry_aggregate = std::move(prepared);
          registry_aggregate_input_row_count = input_row_count;
          state.batch = std::move(output);
          state.result_bindings =
              prepared_registry_aggregate->result_bindings;
          implementation_id = "aggregate.registry-core.v1";
          capability_uuid = registry_aggregate_capability_uuid;
          transformation_rule =
              global_aggregate_profile.transformation_id + ".composed";
          physical_kind = exec::PhysicalNodeKind::kAggregate;
          if (!CheckedAdd(modifiers.filter_truth_memory_bytes,
                          modifiers.transition_memory_bytes,
                          &auxiliary_memory)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition statistical modifier memory overflowed");
          }
          registry_aggregate_filter_truth_memory_bytes =
              modifiers.filter_truth_memory_bytes;
          break;
        }
        if (global_aggregate_profile.matched &&
            global_aggregate_profile.function ==
                exec::CanonicalAggregateFunction::avg) {
          auto prepared = PrepareGlobalAggregateRootForComposition(
              request.relational_dag, node, input_node, state,
              exec::CanonicalAggregateFunction::avg, false,
              global_aggregate_profile.distinct,
              global_aggregate_profile.has_filter);
          if (!prepared.ok || prepared.value_columns.size() != 1) {
            return refuse(std::string(kPayloadDiagnostic), prepared.detail);
          }
          PlanningAggregateModifierState modifiers;
          std::string modifier_detail;
          if (!InitializePlanningAggregateModifierStateForComposition(
                  input_batch, prepared, input_memory,
                  request.optimizer_request.resource.memory_budget_bytes,
                  &modifiers, &modifier_detail)) {
            return refuse(std::string(kPayloadDiagnostic),
                          "composition AVG modifiers: " + modifier_detail);
          }
          std::uint64_t aggregate_work = modifiers.admitted_row_count;
          if (prepared.distinct &&
              (!CheckedAdd(aggregate_work,
                           modifiers.distinct_generation_bound,
                           &aggregate_work) ||
               !CheckedAdd(aggregate_work,
                           modifiers.distinct_comparison_bound,
                           &aggregate_work))) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition AVG(DISTINCT) work bound overflowed");
          }
          long double real_sum = 0.0L;
          std::uint64_t non_null_count = 0;
          const auto value_column = prepared.value_columns.front();
          {
            PlanningAggregateDistinctState distinct_state;
            if (!InitializePlanningAggregateDistinctStateForComposition(
                    request.context, input_batch, prepared,
                    modifiers.distinct_memory_bound, &distinct_state,
                    &modifier_detail, modifiers.admitted_row_count)) {
              return refuse(std::string(kPayloadDiagnostic),
                            "composition AVG(DISTINCT): " +
                                modifier_detail);
            }
            for (std::size_t row = 0; row < input_batch.rows.size(); ++row) {
              if (modifiers.filter_truth_values.has_value() &&
                  (*modifiers.filter_truth_values)[row] !=
                      api::EngineSqlTruthValue::true_value) {
                continue;
              }
              const auto& value = input_batch.rows[row].values[value_column];
              if (prepared.distinct) {
                bool distinct_admitted = false;
                const std::array<const api::EngineTypedValue*, 2> values = {
                    &value, nullptr};
                if (!AdmitPlanningAggregateDistinctTupleForComposition(
                        values, 1, modifiers.distinct_generation_bound,
                        modifiers.distinct_comparison_bound,
                        modifiers.distinct_memory_bound, &distinct_state,
                        &distinct_admitted, &modifier_detail)) {
                  return refuse(std::string(kPayloadDiagnostic),
                                "composition AVG(DISTINCT): " +
                                    modifier_detail);
                }
                if (!distinct_admitted) continue;
              }
              if (value.state == api::EngineValueState::sql_null ||
                  value.is_null) {
                continue;
              }
              if (value.descriptor.canonical_type_name != "int64") {
                return refuse(std::string(kPayloadDiagnostic),
                              "composition AVG input is not canonical int64");
              }
              std::int64_t decoded = 0;
              const auto [end, error] = std::from_chars(
                  value.encoded_value.data(),
                  value.encoded_value.data() + value.encoded_value.size(),
                  decoded);
              if (error != std::errc{} ||
                  end != value.encoded_value.data() +
                             value.encoded_value.size()) {
                return refuse(std::string(kPayloadDiagnostic),
                              "composition AVG input is not exact int64");
              }
              real_sum += static_cast<long double>(decoded);
              if (!std::isfinite(static_cast<double>(real_sum)) ||
                  non_null_count ==
                      std::numeric_limits<std::uint64_t>::max()) {
                return refuse(std::string(kPayloadDiagnostic),
                              "composition AVG state overflowed");
              }
              ++non_null_count;
            }
            registry_aggregate_distinct_peak_memory =
                distinct_state.peak_memory_bytes;
          }
          if (!add_work(aggregate_work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                "composition AVG work exceeds the admitted bound");
          }
          exec::DescriptorBatch output;
          output.columns.push_back(prepared.result_column);
          exec::DescriptorTuple tuple;
          api::EngineTypedValue average;
          average.descriptor = prepared.result_column.descriptor;
          average.is_null = non_null_count == 0;
          average.state = non_null_count == 0
                              ? api::EngineValueState::sql_null
                              : api::EngineValueState::value;
          if (non_null_count != 0) {
            const auto result =
                real_sum / static_cast<long double>(non_null_count);
            if (!std::isfinite(static_cast<double>(result))) {
              return refuse(std::string(kPayloadDiagnostic),
                            "composition AVG result overflowed");
            }
            std::ostringstream encoded;
            encoded << std::setprecision(17) << static_cast<double>(result);
            average.encoded_value = encoded.str();
          }
          tuple.values.push_back(std::move(average));
          output.rows.push_back(std::move(tuple));
          const auto canonical = exec::ValidateCanonicalDescriptorBatch(
              output, node.output_descriptor_ids);
          const auto values = exec::ValidateDescriptorBatch(output);
          if (!canonical.ok || !values.ok) {
            return refuse(
                std::string(kPayloadDiagnostic),
                !canonical.ok
                    ? "AVG planning state: " + canonical.detail
                    : "AVG planning state: " + values.detail);
          }
          prepared_registry_aggregate = std::move(prepared);
          registry_aggregate_input_row_count = input_row_count;
          state.batch = std::move(output);
          state.result_bindings =
              prepared_registry_aggregate->result_bindings;
          implementation_id = "aggregate.registry-core.v1";
          capability_uuid = registry_aggregate_capability_uuid;
          transformation_rule =
              "canonical.aggregate.composed-global-avg-expression.v1";
          physical_kind = exec::PhysicalNodeKind::kAggregate;
          if (!CheckedAdd(modifiers.filter_truth_memory_bytes,
                          modifiers.transition_memory_bytes,
                          &auxiliary_memory)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition AVG modifier memory overflowed");
          }
          registry_aggregate_filter_truth_memory_bytes =
              modifiers.filter_truth_memory_bytes;
          break;
        }
        if (global_aggregate_profile.matched &&
            global_aggregate_profile.function ==
                exec::CanonicalAggregateFunction::sum) {
          auto prepared = PrepareGlobalAggregateRootForComposition(
              request.relational_dag, node, input_node, state,
              exec::CanonicalAggregateFunction::sum, false,
              global_aggregate_profile.distinct,
              global_aggregate_profile.has_filter);
          if (!prepared.ok || prepared.value_columns.size() != 1) {
            return refuse(std::string(kPayloadDiagnostic), prepared.detail);
          }
          PlanningAggregateModifierState modifiers;
          std::string modifier_detail;
          if (!InitializePlanningAggregateModifierStateForComposition(
                  input_batch, prepared, input_memory,
                  request.optimizer_request.resource.memory_budget_bytes,
                  &modifiers, &modifier_detail)) {
            return refuse(std::string(kPayloadDiagnostic),
                          "composition SUM modifiers: " + modifier_detail);
          }
          std::uint64_t aggregate_work = modifiers.admitted_row_count;
          if (prepared.distinct &&
              (!CheckedAdd(aggregate_work,
                           modifiers.distinct_generation_bound,
                           &aggregate_work) ||
               !CheckedAdd(aggregate_work,
                           modifiers.distinct_comparison_bound,
                           &aggregate_work))) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition SUM(DISTINCT) work bound overflowed");
          }
          std::int64_t sum_value = 0;
          bool has_value = false;
          const auto value_column = prepared.value_columns.front();
          {
            PlanningAggregateDistinctState distinct_state;
            if (!InitializePlanningAggregateDistinctStateForComposition(
                    request.context, input_batch, prepared,
                    modifiers.distinct_memory_bound, &distinct_state,
                    &modifier_detail, modifiers.admitted_row_count)) {
              return refuse(std::string(kPayloadDiagnostic),
                            "composition SUM(DISTINCT): " +
                                modifier_detail);
            }
            for (std::size_t row = 0; row < input_batch.rows.size(); ++row) {
              if (modifiers.filter_truth_values.has_value() &&
                  (*modifiers.filter_truth_values)[row] !=
                      api::EngineSqlTruthValue::true_value) {
                continue;
              }
              const auto& value = input_batch.rows[row].values[value_column];
              if (prepared.distinct) {
                bool distinct_admitted = false;
                const std::array<const api::EngineTypedValue*, 2> values = {
                    &value, nullptr};
                if (!AdmitPlanningAggregateDistinctTupleForComposition(
                        values, 1, modifiers.distinct_generation_bound,
                        modifiers.distinct_comparison_bound,
                        modifiers.distinct_memory_bound, &distinct_state,
                        &distinct_admitted, &modifier_detail)) {
                  return refuse(std::string(kPayloadDiagnostic),
                                "composition SUM(DISTINCT): " +
                                    modifier_detail);
                }
                if (!distinct_admitted) continue;
              }
              if (value.state == api::EngineValueState::sql_null ||
                  value.is_null) {
                continue;
              }
              if (!exec::IsCanonicalBoundedSignedIntegerDescriptor(
                      value.descriptor)) {
                return refuse(std::string(kPayloadDiagnostic),
                              "composition SUM input is not canonical "
                              "bounded-signed");
              }
              const auto decoded = exec::DecodeInt64Value(value);
              if (!decoded.ok() ||
                  (decoded.value > 0 &&
                   sum_value >
                       std::numeric_limits<std::int64_t>::max() -
                           decoded.value) ||
                  (decoded.value < 0 &&
                   sum_value <
                       std::numeric_limits<std::int64_t>::min() -
                           decoded.value)) {
                return refuse(std::string(kPayloadDiagnostic),
                              "composition SUM input or result overflowed");
              }
              sum_value += decoded.value;
              has_value = true;
            }
            registry_aggregate_distinct_peak_memory =
                distinct_state.peak_memory_bytes;
          }
          if (!add_work(aggregate_work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                "composition SUM work exceeds the admitted bound");
          }
          exec::DescriptorBatch output;
          output.columns.push_back(prepared.result_column);
          exec::DescriptorTuple tuple;
          api::EngineTypedValue sum;
          sum.descriptor = prepared.result_column.descriptor;
          sum.encoded_value = has_value ? std::to_string(sum_value) : "";
          sum.is_null = !has_value;
          sum.state = has_value ? api::EngineValueState::value
                                : api::EngineValueState::sql_null;
          tuple.values.push_back(std::move(sum));
          output.rows.push_back(std::move(tuple));
          const auto canonical = exec::ValidateCanonicalDescriptorBatch(
              output, node.output_descriptor_ids);
          const auto values = exec::ValidateDescriptorBatch(output);
          if (!canonical.ok || !values.ok) {
            return refuse(
                std::string(kPayloadDiagnostic),
                !canonical.ok
                    ? "SUM planning state: " + canonical.detail
                    : "SUM planning state: " + values.detail);
          }
          prepared_registry_aggregate = std::move(prepared);
          registry_aggregate_input_row_count = input_row_count;
          state.batch = std::move(output);
          state.result_bindings =
              prepared_registry_aggregate->result_bindings;
          implementation_id = "aggregate.registry-core.v1";
          capability_uuid = registry_aggregate_capability_uuid;
          transformation_rule =
              "canonical.aggregate.composed-global-sum-expression.v1";
          physical_kind = exec::PhysicalNodeKind::kAggregate;
          if (!CheckedAdd(modifiers.filter_truth_memory_bytes,
                          modifiers.transition_memory_bytes,
                          &auxiliary_memory)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition SUM modifier memory overflowed");
          }
          registry_aggregate_filter_truth_memory_bytes =
              modifiers.filter_truth_memory_bytes;
          break;
        }
        if (global_aggregate_profile.matched &&
            (global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::min ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::max)) {
          const auto function = global_aggregate_profile.function;
          const bool minimum =
              function == exec::CanonicalAggregateFunction::min;
          auto prepared = PrepareGlobalAggregateRootForComposition(
              request.relational_dag, node, input_node, state, function,
              false, global_aggregate_profile.distinct,
              global_aggregate_profile.has_filter);
          if (!prepared.ok || prepared.value_columns.size() != 1 ||
              prepared.value_descriptor_ids.size() != 1) {
            return refuse(std::string(kPayloadDiagnostic), prepared.detail);
          }
          PlanningAggregateModifierState modifiers;
          std::string modifier_detail;
          if (!InitializePlanningAggregateModifierStateForComposition(
                  input_batch, prepared, input_memory,
                  request.optimizer_request.resource.memory_budget_bytes,
                  &modifiers, &modifier_detail)) {
            return refuse(std::string(kPayloadDiagnostic),
                          "composition extremum modifiers: " +
                              modifier_detail);
          }
          const auto value_column = prepared.value_columns.front();
          exec::CanonicalDescriptorOrderTerm comparison_term;
          comparison_term.column = value_column;
          comparison_term.expression_descriptor_id =
              prepared.value_descriptor_ids.front();
          const auto order_validation =
              exec::ValidateCanonicalDescriptorOrderTerm(
                  comparison_term, input_batch.columns[value_column]);
          if (!order_validation.ok) {
            return refuse(std::string(kPayloadDiagnostic),
                          "composition extremum comparison: " +
                              order_validation.detail);
          }
          std::uint64_t aggregate_work = modifiers.admitted_row_count;
          if (prepared.distinct &&
              (!CheckedAdd(aggregate_work,
                           modifiers.distinct_generation_bound,
                           &aggregate_work) ||
               !CheckedAdd(aggregate_work,
                           modifiers.distinct_comparison_bound,
                           &aggregate_work))) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition extremum DISTINCT work bound overflowed");
          }
          std::optional<api::EngineTypedValue> extremum;
          {
            PlanningAggregateDistinctState distinct_state;
            if (!InitializePlanningAggregateDistinctStateForComposition(
                    request.context, input_batch, prepared,
                    modifiers.distinct_memory_bound, &distinct_state,
                    &modifier_detail, modifiers.admitted_row_count)) {
              return refuse(std::string(kPayloadDiagnostic),
                            "composition extremum DISTINCT: " +
                                modifier_detail);
            }
            for (std::size_t row = 0; row < input_batch.rows.size(); ++row) {
              if (modifiers.filter_truth_values.has_value() &&
                  (*modifiers.filter_truth_values)[row] !=
                      api::EngineSqlTruthValue::true_value) {
                continue;
              }
              const auto& value = input_batch.rows[row].values[value_column];
              if (prepared.distinct) {
                bool distinct_admitted = false;
                const std::array<const api::EngineTypedValue*, 2> values = {
                    &value, nullptr};
                if (!AdmitPlanningAggregateDistinctTupleForComposition(
                        values, 1, modifiers.distinct_generation_bound,
                        modifiers.distinct_comparison_bound,
                        modifiers.distinct_memory_bound, &distinct_state,
                        &distinct_admitted, &modifier_detail)) {
                  return refuse(std::string(kPayloadDiagnostic),
                                "composition extremum DISTINCT: " +
                                    modifier_detail);
                }
                if (!distinct_admitted) continue;
              }
              if (value.state == api::EngineValueState::sql_null ||
                  value.is_null) {
                continue;
              }
              if (!exec::IsCanonicalBoundedSignedIntegerDescriptor(
                      value.descriptor)) {
                return refuse(std::string(kPayloadDiagnostic),
                              "composition extremum input is not canonical "
                              "bounded-signed");
              }
              if (!extremum.has_value()) {
                extremum = value;
                continue;
              }
              const auto compared =
                  exec::CompareCanonicalDescriptorOrderValues(
                      value, *extremum, comparison_term);
              if (!compared.diagnostic.ok) {
                return refuse(std::string(kPayloadDiagnostic),
                              "composition extremum comparison: " +
                                  compared.diagnostic.detail);
              }
              if ((minimum && compared.comparison < 0) ||
                  (!minimum && compared.comparison > 0)) {
                extremum = value;
              }
            }
            registry_aggregate_distinct_peak_memory =
                distinct_state.peak_memory_bytes;
          }
          if (!add_work(aggregate_work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                "composition extremum work exceeds the admitted bound");
          }
          exec::DescriptorBatch output;
          output.columns.push_back(prepared.result_column);
          exec::DescriptorTuple tuple;
          api::EngineTypedValue value;
          if (extremum.has_value()) value = std::move(*extremum);
          value.descriptor = prepared.result_column.descriptor;
          if (!extremum.has_value()) {
            value.encoded_value.clear();
            value.binary_value.clear();
            value.is_null = true;
            value.state = api::EngineValueState::sql_null;
          }
          tuple.values.push_back(std::move(value));
          output.rows.push_back(std::move(tuple));
          const auto canonical = exec::ValidateCanonicalDescriptorBatch(
              output, node.output_descriptor_ids);
          const auto values = exec::ValidateDescriptorBatch(output);
          if (!canonical.ok || !values.ok) {
            return refuse(
                std::string(kPayloadDiagnostic),
                !canonical.ok
                    ? "extremum planning state: " + canonical.detail
                    : "extremum planning state: " + values.detail);
          }
          prepared_registry_aggregate = std::move(prepared);
          registry_aggregate_input_row_count = input_row_count;
          state.batch = std::move(output);
          state.result_bindings =
              prepared_registry_aggregate->result_bindings;
          implementation_id = "aggregate.registry-core.v1";
          capability_uuid = registry_aggregate_capability_uuid;
          transformation_rule =
              minimum
                  ? "canonical.aggregate.composed-global-min-expression.v1"
                  : "canonical.aggregate.composed-global-max-expression.v1";
          physical_kind = exec::PhysicalNodeKind::kAggregate;
          if (!CheckedAdd(modifiers.filter_truth_memory_bytes,
                          modifiers.transition_memory_bytes,
                          &auxiliary_memory)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition extremum modifier memory overflowed");
          }
          registry_aggregate_filter_truth_memory_bytes =
              modifiers.filter_truth_memory_bytes;
          break;
        }
        if (global_aggregate_profile.matched &&
            (global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::bool_and ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::bool_or ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::every)) {
          const auto function = global_aggregate_profile.function;
          auto prepared = PrepareGlobalAggregateRootForComposition(
              request.relational_dag, node, input_node, state, function,
              false, global_aggregate_profile.distinct,
              global_aggregate_profile.has_filter);
          if (!prepared.ok || prepared.value_columns.size() != 1) {
            return refuse(std::string(kPayloadDiagnostic), prepared.detail);
          }
          PlanningAggregateModifierState modifiers;
          std::string modifier_detail;
          if (!InitializePlanningAggregateModifierStateForComposition(
                  input_batch, prepared, input_memory,
                  request.optimizer_request.resource.memory_budget_bytes,
                  &modifiers, &modifier_detail)) {
            return refuse(std::string(kPayloadDiagnostic),
                          "composition boolean aggregate modifiers: " +
                              modifier_detail);
          }
          std::uint64_t aggregate_work = modifiers.admitted_row_count;
          if (prepared.distinct &&
              (!CheckedAdd(aggregate_work,
                           modifiers.distinct_generation_bound,
                           &aggregate_work) ||
               !CheckedAdd(aggregate_work,
                           modifiers.distinct_comparison_bound,
                           &aggregate_work))) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition boolean aggregate DISTINCT work bound overflowed");
          }
          bool saw_value = false;
          bool saw_true = false;
          bool saw_false = false;
          const auto value_column = prepared.value_columns.front();
          {
            PlanningAggregateDistinctState distinct_state;
            if (!InitializePlanningAggregateDistinctStateForComposition(
                    request.context, input_batch, prepared,
                    modifiers.distinct_memory_bound, &distinct_state,
                    &modifier_detail, modifiers.admitted_row_count)) {
              return refuse(std::string(kPayloadDiagnostic),
                            "composition boolean aggregate DISTINCT: " +
                                modifier_detail);
            }
            for (std::size_t row = 0; row < input_batch.rows.size(); ++row) {
              if (modifiers.filter_truth_values.has_value() &&
                  (*modifiers.filter_truth_values)[row] !=
                      api::EngineSqlTruthValue::true_value) {
                continue;
              }
              const auto& value = input_batch.rows[row].values[value_column];
              if (prepared.distinct) {
                bool distinct_admitted = false;
                const std::array<const api::EngineTypedValue*, 2> values = {
                    &value, nullptr};
                if (!AdmitPlanningAggregateDistinctTupleForComposition(
                        values, 1, modifiers.distinct_generation_bound,
                        modifiers.distinct_comparison_bound,
                        modifiers.distinct_memory_bound, &distinct_state,
                        &distinct_admitted, &modifier_detail)) {
                  return refuse(std::string(kPayloadDiagnostic),
                                "composition boolean aggregate DISTINCT: " +
                                    modifier_detail);
                }
                if (!distinct_admitted) continue;
              }
              if (value.state == api::EngineValueState::sql_null ||
                  value.is_null) {
                continue;
              }
              if (value.descriptor.canonical_type_name != "boolean") {
                return refuse(std::string(kPayloadDiagnostic),
                              "composition boolean aggregate input is not "
                              "canonical boolean");
              }
              api::EngineSqlTruthValue truth =
                  api::EngineSqlTruthValue::unspecified;
              std::string truth_detail;
              if (!api::QowCanonicalTruthFromTypedValueV1(
                      value, &truth, &truth_detail) ||
                  (truth != api::EngineSqlTruthValue::true_value &&
                   truth != api::EngineSqlTruthValue::false_value)) {
                return refuse(
                    std::string(kPayloadDiagnostic),
                    "composition boolean aggregate input: " + truth_detail);
              }
              saw_value = true;
              saw_true = saw_true ||
                         truth == api::EngineSqlTruthValue::true_value;
              saw_false = saw_false ||
                          truth == api::EngineSqlTruthValue::false_value;
            }
            registry_aggregate_distinct_peak_memory =
                distinct_state.peak_memory_bytes;
          }
          if (!add_work(aggregate_work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                "composition boolean aggregate work exceeds the admitted "
                "bound");
          }
          exec::DescriptorBatch output;
          output.columns.push_back(prepared.result_column);
          exec::DescriptorTuple tuple;
          api::EngineTypedValue value;
          value.descriptor = prepared.result_column.descriptor;
          value.is_null = !saw_value;
          value.state = saw_value ? api::EngineValueState::value
                                  : api::EngineValueState::sql_null;
          if (saw_value) {
            const bool aggregate_truth =
                function == exec::CanonicalAggregateFunction::bool_or
                    ? saw_true
                    : !saw_false;
            value.encoded_value = aggregate_truth ? "true" : "false";
          }
          tuple.values.push_back(std::move(value));
          output.rows.push_back(std::move(tuple));
          const auto canonical = exec::ValidateCanonicalDescriptorBatch(
              output, node.output_descriptor_ids);
          const auto values = exec::ValidateDescriptorBatch(output);
          if (!canonical.ok || !values.ok) {
            return refuse(
                std::string(kPayloadDiagnostic),
                !canonical.ok
                    ? "boolean aggregate planning state: " +
                          canonical.detail
                    : "boolean aggregate planning state: " + values.detail);
          }
          prepared_registry_aggregate = std::move(prepared);
          registry_aggregate_input_row_count = input_row_count;
          state.batch = std::move(output);
          state.result_bindings =
              prepared_registry_aggregate->result_bindings;
          implementation_id = "aggregate.registry-core.v1";
          capability_uuid = registry_aggregate_capability_uuid;
          if (function == exec::CanonicalAggregateFunction::bool_and) {
            transformation_rule =
                "canonical.aggregate.composed-global-bool-and-expression.v1";
          } else if (function == exec::CanonicalAggregateFunction::bool_or) {
            transformation_rule =
                "canonical.aggregate.composed-global-bool-or-expression.v1";
          } else {
            transformation_rule =
                "canonical.aggregate.composed-global-every-expression.v1";
          }
          physical_kind = exec::PhysicalNodeKind::kAggregate;
          if (!CheckedAdd(modifiers.filter_truth_memory_bytes,
                          modifiers.transition_memory_bytes,
                          &auxiliary_memory)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition boolean aggregate modifier memory overflowed");
          }
          registry_aggregate_filter_truth_memory_bytes =
              modifiers.filter_truth_memory_bytes;
          break;
        }
        if (pair_aggregate_profile.matched) {
          const auto function = pair_aggregate_profile.function;
          auto prepared = PrepareGlobalAggregateRootForComposition(
              request.relational_dag, node, input_node, state, function,
              false, pair_aggregate_profile.distinct,
              pair_aggregate_profile.has_filter);
          if (!prepared.ok || prepared.value_columns.size() != 2) {
            return refuse(std::string(kPayloadDiagnostic), prepared.detail);
          }
          PlanningAggregateModifierState modifiers;
          std::string modifier_detail;
          if (!InitializePlanningAggregateModifierStateForComposition(
                  input_batch, prepared, input_memory,
                  request.optimizer_request.resource.memory_budget_bytes,
                  &modifiers, &modifier_detail)) {
            return refuse(std::string(kPayloadDiagnostic),
                          "composition pair statistical modifiers: " +
                              modifier_detail);
          }
          std::uint64_t aggregate_work = modifiers.admitted_row_count;
          if (prepared.distinct &&
              (!CheckedAdd(aggregate_work,
                           modifiers.distinct_generation_bound,
                           &aggregate_work) ||
               !CheckedAdd(aggregate_work,
                           modifiers.distinct_comparison_bound,
                           &aggregate_work))) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition pair statistical DISTINCT work bound overflowed");
          }
          std::uint64_t non_null_count = 0;
          long double mean_x = 0.0L;
          long double mean_y = 0.0L;
          long double m2_x = 0.0L;
          long double m2_y = 0.0L;
          long double comoment = 0.0L;
          const auto y_column = prepared.value_columns[0];
          const auto x_column = prepared.value_columns[1];
          {
          PlanningAggregateDistinctState distinct_state;
          if (!InitializePlanningAggregateDistinctStateForComposition(
                  request.context, input_batch, prepared,
                  modifiers.distinct_memory_bound, &distinct_state,
                  &modifier_detail, modifiers.admitted_row_count)) {
            return refuse(std::string(kPayloadDiagnostic),
                          "composition pair statistical DISTINCT: " +
                              modifier_detail);
          }
          for (std::size_t row = 0; row < input_batch.rows.size(); ++row) {
            if (modifiers.filter_truth_values.has_value() &&
                (*modifiers.filter_truth_values)[row] !=
                    api::EngineSqlTruthValue::true_value) {
              continue;
            }
            const auto& y_value = input_batch.rows[row].values[y_column];
            const auto& x_value = input_batch.rows[row].values[x_column];
            if (prepared.distinct) {
              bool distinct_admitted = false;
              const std::array<const api::EngineTypedValue*, 2> values = {
                  &y_value, &x_value};
              if (!AdmitPlanningAggregateDistinctTupleForComposition(
                      values, 2, modifiers.distinct_generation_bound,
                      modifiers.distinct_comparison_bound,
                      modifiers.distinct_memory_bound, &distinct_state,
                      &distinct_admitted, &modifier_detail)) {
                return refuse(std::string(kPayloadDiagnostic),
                              "composition pair statistical DISTINCT: " +
                                  modifier_detail);
              }
              if (!distinct_admitted) continue;
            }
            if (y_value.state == api::EngineValueState::sql_null ||
                y_value.is_null ||
                x_value.state == api::EngineValueState::sql_null ||
                x_value.is_null) {
              continue;
            }
            if (y_value.descriptor.canonical_type_name != "int64" ||
                x_value.descriptor.canonical_type_name != "int64" ||
                !y_value.binary_value.empty() ||
                !x_value.binary_value.empty()) {
              return refuse(
                  std::string(kPayloadDiagnostic),
                  "composition pair statistical input is not canonical "
                  "int64");
            }
            const auto decode = [](const api::EngineTypedValue& value,
                                   std::int64_t* decoded) {
              const auto [end, error] = std::from_chars(
                  value.encoded_value.data(),
                  value.encoded_value.data() + value.encoded_value.size(),
                  *decoded);
              return error == std::errc{} &&
                     end == value.encoded_value.data() +
                                value.encoded_value.size();
            };
            std::int64_t y_decoded = 0;
            std::int64_t x_decoded = 0;
            if (!decode(y_value, &y_decoded) ||
                !decode(x_value, &x_decoded) ||
                non_null_count == std::numeric_limits<std::uint64_t>::max()) {
              return refuse(
                  std::string(kPayloadDiagnostic),
                  "composition pair statistical input or count is invalid");
            }
            ++non_null_count;
            const auto y = static_cast<long double>(y_decoded);
            const auto x = static_cast<long double>(x_decoded);
            const auto count = static_cast<long double>(non_null_count);
            const auto delta_x = x - mean_x;
            mean_x += delta_x / count;
            const auto delta_y = y - mean_y;
            mean_y += delta_y / count;
            m2_x += delta_x * (x - mean_x);
            m2_y += delta_y * (y - mean_y);
            comoment += delta_x * (y - mean_y);
            if (!std::isfinite(static_cast<double>(mean_x)) ||
                !std::isfinite(static_cast<double>(mean_y)) ||
                !std::isfinite(static_cast<double>(m2_x)) ||
                !std::isfinite(static_cast<double>(m2_y)) ||
                !std::isfinite(static_cast<double>(comoment))) {
              return refuse(
                  std::string(kPayloadDiagnostic),
                  "composition pair statistical state overflowed");
            }
          }
          registry_aggregate_distinct_peak_memory =
              distinct_state.peak_memory_bytes;
          }
          if (!add_work(aggregate_work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                "composition pair statistical work exceeds the admitted "
                "bound");
          }
          bool result_available = non_null_count != 0;
          long double statistic = 0.0L;
          switch (function) {
            case exec::CanonicalAggregateFunction::corr:
              result_available =
                  non_null_count >= 2 && m2_x > 0.0L && m2_y > 0.0L;
              if (result_available) {
                statistic = comoment / std::sqrt(m2_x * m2_y);
              }
              break;
            case exec::CanonicalAggregateFunction::covar_pop:
              if (result_available) {
                statistic =
                    comoment / static_cast<long double>(non_null_count);
              }
              break;
            case exec::CanonicalAggregateFunction::covar_samp:
              result_available = non_null_count >= 2;
              if (result_available) {
                statistic = comoment /
                            static_cast<long double>(non_null_count - 1);
              }
              break;
            case exec::CanonicalAggregateFunction::regr_avgx:
              statistic = mean_x;
              break;
            case exec::CanonicalAggregateFunction::regr_avgy:
              statistic = mean_y;
              break;
            case exec::CanonicalAggregateFunction::regr_intercept:
              result_available = result_available && m2_x != 0.0L;
              if (result_available) {
                statistic = mean_y - mean_x * comoment / m2_x;
              }
              break;
            case exec::CanonicalAggregateFunction::regr_r2:
              result_available = result_available && m2_x != 0.0L;
              if (result_available) {
                statistic = m2_y == 0.0L
                                ? 1.0L
                                : comoment * comoment / (m2_x * m2_y);
              }
              break;
            case exec::CanonicalAggregateFunction::regr_slope:
              result_available = result_available && m2_x != 0.0L;
              if (result_available) statistic = comoment / m2_x;
              break;
            case exec::CanonicalAggregateFunction::regr_sxx:
              statistic = m2_x;
              break;
            case exec::CanonicalAggregateFunction::regr_sxy:
              statistic = comoment;
              break;
            case exec::CanonicalAggregateFunction::regr_syy:
              statistic = m2_y;
              break;
            case exec::CanonicalAggregateFunction::regr_count:
              result_available = true;
              break;
            default:
              return refuse(
                  std::string(kPayloadDiagnostic),
                  "composition pair statistical function is unresolved");
          }
          if (result_available &&
              function != exec::CanonicalAggregateFunction::regr_count &&
              !std::isfinite(static_cast<double>(statistic))) {
            return refuse(
                std::string(kPayloadDiagnostic),
                "composition pair statistical result overflowed");
          }
          exec::DescriptorBatch output;
          output.columns.push_back(prepared.result_column);
          exec::DescriptorTuple tuple;
          api::EngineTypedValue result_value;
          result_value.descriptor = prepared.result_column.descriptor;
          result_value.is_null = !result_available;
          result_value.state = result_available
                                   ? api::EngineValueState::value
                                   : api::EngineValueState::sql_null;
          if (result_available) {
            if (function == exec::CanonicalAggregateFunction::regr_count) {
              if (non_null_count > static_cast<std::uint64_t>(
                                       std::numeric_limits<std::int64_t>::max())) {
                return refuse(
                    std::string(kPayloadDiagnostic),
                    "composition REGR_COUNT exceeded int64 result width");
              }
              result_value.encoded_value = std::to_string(non_null_count);
            } else {
              std::ostringstream encoded;
              encoded << std::setprecision(17)
                      << static_cast<double>(statistic);
              result_value.encoded_value = encoded.str();
            }
          }
          tuple.values.push_back(std::move(result_value));
          output.rows.push_back(std::move(tuple));
          const auto canonical = exec::ValidateCanonicalDescriptorBatch(
              output, node.output_descriptor_ids);
          const auto values = exec::ValidateDescriptorBatch(output);
          if (!canonical.ok || !values.ok) {
            return refuse(
                std::string(kPayloadDiagnostic),
                !canonical.ok
                    ? "pair statistical planning state: " +
                          canonical.detail
                    : "pair statistical planning state: " + values.detail);
          }
          prepared_registry_aggregate = std::move(prepared);
          registry_aggregate_input_row_count = input_row_count;
          state.batch = std::move(output);
          state.result_bindings =
              prepared_registry_aggregate->result_bindings;
          implementation_id = "aggregate.registry-core.v1";
          capability_uuid = registry_aggregate_capability_uuid;
          transformation_rule =
              pair_aggregate_profile.transformation_id + ".composed";
          physical_kind = exec::PhysicalNodeKind::kAggregate;
          if (!CheckedAdd(modifiers.filter_truth_memory_bytes,
                          modifiers.transition_memory_bytes,
                          &auxiliary_memory)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition pair statistical modifier memory overflowed");
          }
          registry_aggregate_filter_truth_memory_bytes =
              modifiers.filter_truth_memory_bytes;
          break;
        }
        if (string_aggregate_profile.matched ||
            ordered_collection_profile.matched ||
            json_object_profile.matched || listagg_profile.matched ||
            ordered_set_profile.matched || approximate_profile.matched) {
          exec::CanonicalAggregateFunction function =
              exec::CanonicalAggregateFunction::unknown;
          bool distinct = false;
          bool has_filter = false;
          bool comparison_sensitive = false;
          std::string transformation_id;
          if (string_aggregate_profile.matched) {
            function = exec::CanonicalAggregateFunction::string_agg;
            distinct = string_aggregate_profile.distinct;
            has_filter = string_aggregate_profile.has_filter;
            comparison_sensitive = string_aggregate_profile.ordered;
            transformation_id =
                string_aggregate_profile.transformation_id;
          } else if (ordered_collection_profile.matched) {
            function = ordered_collection_profile.function;
            distinct = ordered_collection_profile.distinct;
            has_filter = ordered_collection_profile.has_filter;
            comparison_sensitive = true;
            transformation_id =
                ordered_collection_profile.transformation_id;
          } else if (json_object_profile.matched) {
            function = exec::CanonicalAggregateFunction::json_object_agg;
            distinct = json_object_profile.distinct;
            has_filter = json_object_profile.has_filter;
            comparison_sensitive = true;
            transformation_id = json_object_profile.transformation_id;
          } else if (listagg_profile.matched) {
            function = exec::CanonicalAggregateFunction::listagg;
            distinct = listagg_profile.distinct;
            has_filter = listagg_profile.has_filter;
            comparison_sensitive = true;
            transformation_id = listagg_profile.transformation_id;
          } else if (ordered_set_profile.matched) {
            function = ordered_set_profile.function;
            distinct = ordered_set_profile.distinct;
            has_filter = ordered_set_profile.has_filter;
            comparison_sensitive = true;
            transformation_id = ordered_set_profile.transformation_id;
          } else {
            function = approximate_profile.function;
            distinct = approximate_profile.distinct;
            has_filter = approximate_profile.has_filter;
            comparison_sensitive = true;
            transformation_id = approximate_profile.transformation_id;
          }
          auto prepared = PrepareGlobalAggregateRootForComposition(
              request.relational_dag, node, input_node, state,
              function, false, distinct, has_filter);
          if (!prepared.ok) {
            return refuse(std::string(kPayloadDiagnostic), prepared.detail);
          }
          PlanningAggregateModifierState modifiers;
          std::string modifier_detail;
          if (!InitializePlanningAggregateModifierStateForComposition(
                  input_batch, prepared, input_memory,
                  request.optimizer_request.resource.memory_budget_bytes,
                  &modifiers, &modifier_detail)) {
            return refuse(std::string(kPayloadDiagnostic),
                          "composition complex aggregate modifiers: " +
                              modifier_detail);
          }
          std::uint64_t aggregate_work = modifiers.admitted_row_count;
          if (prepared.distinct &&
              (!CheckedAdd(aggregate_work,
                           modifiers.distinct_generation_bound,
                           &aggregate_work) ||
               !CheckedAdd(aggregate_work,
                           modifiers.distinct_comparison_bound,
                           &aggregate_work))) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition complex aggregate DISTINCT work overflowed");
          }
          if (comparison_sensitive || prepared.distinct ||
              !prepared.aggregate_order_terms.empty()) {
            std::uint64_t comparison_work = 0;
            if ((modifiers.admitted_row_count > 1 &&
                 !CheckedMultiply(modifiers.admitted_row_count,
                                  modifiers.admitted_row_count - 1,
                                  &comparison_work)) ||
                (comparison_work /= 2,
                !CheckedMultiply(
                    comparison_work,
                    std::max<std::size_t>(1,
                        prepared.value_columns.size()),
                    &comparison_work)) ||
                !CheckedAdd(aggregate_work, comparison_work,
                            &aggregate_work)) {
              return refuse(
                  "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "composition complex aggregate work overflowed");
            }
          }
          if (!add_work(aggregate_work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                "composition complex aggregate work exceeds the admitted "
                  "bound");
          }
          if (prepared.distinct) {
            PlanningAggregateDistinctState distinct_state;
            if (!InitializePlanningAggregateDistinctStateForComposition(
                    request.context, input_batch, prepared,
                    modifiers.distinct_memory_bound, &distinct_state,
                    &modifier_detail, modifiers.admitted_row_count)) {
              return refuse(std::string(kPayloadDiagnostic),
                            "composition complex aggregate DISTINCT: " +
                                modifier_detail);
            }
            for (std::size_t row = 0; row < input_batch.rows.size(); ++row) {
              if (modifiers.filter_truth_values.has_value() &&
                  (*modifiers.filter_truth_values)[row] !=
                      api::EngineSqlTruthValue::true_value) {
                continue;
              }
              std::array<const api::EngineTypedValue*, 2> values{};
              for (std::size_t value = 0;
                   value < prepared.value_columns.size(); ++value) {
                values[value] = &input_batch.rows[row].values[
                    prepared.value_columns[value]];
              }
              bool admitted = false;
              if (!AdmitPlanningAggregateDistinctTupleForComposition(
                      values, prepared.value_columns.size(),
                      modifiers.distinct_generation_bound,
                      modifiers.distinct_comparison_bound,
                      modifiers.distinct_memory_bound, &distinct_state,
                      &admitted, &modifier_detail)) {
                return refuse(std::string(kPayloadDiagnostic),
                              "composition complex aggregate DISTINCT: " +
                                  modifier_detail);
              }
            }
            registry_aggregate_distinct_peak_memory =
                distinct_state.peak_memory_bytes;
          }
          exec::DescriptorBatch output;
          output.columns.push_back(prepared.result_column);
          exec::DescriptorTuple tuple;
          api::EngineTypedValue placeholder;
          placeholder.descriptor = prepared.result_column.descriptor;
          if (prepared.result_column.nullable) {
            placeholder.is_null = true;
            placeholder.state = api::EngineValueState::sql_null;
          } else if (
              placeholder.descriptor.canonical_type_name == "int64" ||
              placeholder.descriptor.canonical_type_name == "real64") {
            placeholder.encoded_value = "0";
            placeholder.state = api::EngineValueState::value;
          } else {
            return refuse(
                std::string(kPayloadDiagnostic),
                "complex aggregate non-null planning descriptor is not "
                "cardinality-only safe");
          }
          tuple.values.push_back(std::move(placeholder));
          output.rows.push_back(std::move(tuple));
          const auto canonical = exec::ValidateCanonicalDescriptorBatch(
              output, node.output_descriptor_ids);
          // Cardinality-only planning carries a canonical SQL NULL for the
          // derived ARRAY_AGG list descriptor; scalar value validation does
          // not own derived collection encodings.
          const bool derived_list_planning_descriptor =
              output.columns.front().descriptor.canonical_type_name.rfind(
                  "list<", 0) == 0;
          const auto values = derived_list_planning_descriptor
                                  ? exec::DescriptorRuntimeDiagnostic{}
                                  : exec::ValidateDescriptorBatch(output);
          if (!canonical.ok ||
              (!derived_list_planning_descriptor && !values.ok)) {
            return refuse(
                std::string(kPayloadDiagnostic),
                !canonical.ok
                    ? "complex aggregate planning state: " +
                          canonical.detail
                    : "complex aggregate planning state: " + values.detail);
          }
          prepared_registry_aggregate = std::move(prepared);
          registry_aggregate_input_row_count = input_row_count;
          state.batch = std::move(output);
          state.result_bindings =
              prepared_registry_aggregate->result_bindings;
          implementation_id = "aggregate.registry-core.v1";
          capability_uuid = registry_aggregate_capability_uuid;
          transformation_rule = transformation_id + ".composed";
          physical_kind = exec::PhysicalNodeKind::kAggregate;
          if (!CheckedAdd(modifiers.filter_truth_memory_bytes,
                          modifiers.transition_memory_bytes,
                          &auxiliary_memory)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition complex aggregate modifier memory overflowed");
          }
          registry_aggregate_filter_truth_memory_bytes =
              modifiers.filter_truth_memory_bytes;
          planning_values_exact = false;
          break;
        }
        if (grouped_aggregate_profile.matched) {
          auto prepared = PrepareGroupedCountSumRootForComposition(
              request.relational_dag, node, input_node, state,
              grouped_aggregate_profile);
          const auto expected_projection_count =
              grouped_aggregate_profile.projects_grouping_metadata
                  ? grouped_aggregate_profile.key_count + 1
                  : 0;
          if (!prepared.ok ||
              prepared.key_terms.size() !=
                  grouped_aggregate_profile.key_count ||
              prepared.key_result_columns.size() !=
                  grouped_aggregate_profile.key_count ||
              prepared.grouping_sets.empty() ||
              prepared.count.value_columns.size() != 0 ||
              prepared.sum.value_columns.size() != 1 ||
              prepared.grouping_projection_columns.size() !=
                  expected_projection_count) {
            return refuse(std::string(kPayloadDiagnostic), prepared.detail);
          }
          if (!BindPreparedGroupedComparisonCeilings(
                  &prepared, input_row_count)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition grouped aggregate comparison bound overflowed");
          }
          struct GroupPlanningState {
            std::size_t grouping_set_ordinal{0};
            std::vector<bool> grouping_indicators;
            std::uint64_t grouping_id{0};
            std::size_t representative_row{0};
            bool has_representative{false};
            std::uint64_t count{0};
            __int128 sum{0};
            bool has_sum{false};
          };
          std::vector<GroupPlanningState> groups;
          std::uint64_t output_row_bound = 0;
          if (!CheckedMultiply(
                  std::max<std::uint64_t>(1, input_row_count),
                  prepared.grouping_sets.size(), &output_row_bound) ||
              output_row_bound >
                  static_cast<std::uint64_t>(
                      std::numeric_limits<std::size_t>::max())) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition grouped aggregate output bound overflowed");
          }
          groups.reserve(static_cast<std::size_t>(output_row_bound));
          const auto sum_column = prepared.sum.value_columns.front();
          for (std::size_t set_ordinal = 0;
               set_ordinal < prepared.grouping_sets.size(); ++set_ordinal) {
            const auto& grouping_set = prepared.grouping_sets[set_ordinal];
            std::vector<bool> included(prepared.key_terms.size(), false);
            for (const auto key_ordinal :
                 grouping_set.key_term_ordinals) {
              included[key_ordinal] = true;
            }
            const auto metadata =
                exec::ComputeCanonicalAggregateGroupingMetadata(
                    prepared.key_terms.size(), grouping_set);
            if (!metadata.diagnostic.ok) {
              return refuse(std::string(kPayloadDiagnostic),
                            "composed grouped metadata: " +
                                metadata.diagnostic.detail);
            }
            const auto set_begin = groups.size();
            if (std::ranges::find(included, true) == included.end()) {
              GroupPlanningState grand_total;
              grand_total.grouping_set_ordinal = set_ordinal;
              grand_total.grouping_indicators =
                  metadata.grouping_indicators;
              grand_total.grouping_id = metadata.grouping_id;
              groups.push_back(std::move(grand_total));
            }
            for (std::size_t row_ordinal = 0;
                 row_ordinal < input_batch.rows.size(); ++row_ordinal) {
              const auto& row = input_batch.rows[row_ordinal];
              if (sum_column >= row.values.size() ||
                  std::ranges::any_of(
                      prepared.key_terms, [&](const auto& term) {
                        return term.column >= row.values.size();
                      })) {
                return refuse(std::string(kPayloadDiagnostic),
                              "composed grouped row shape is incomplete");
              }
              auto group = groups.end();
              for (auto candidate =
                       groups.begin() +
                           static_cast<std::ptrdiff_t>(set_begin);
                   candidate != groups.end(); ++candidate) {
                bool matches = true;
                for (std::size_t key_ordinal = 0;
                     key_ordinal < prepared.key_terms.size();
                     ++key_ordinal) {
                  if (!included[key_ordinal]) continue;
                  const auto& key_term = prepared.key_terms[key_ordinal];
                  const auto compared =
                      exec::CompareCanonicalDescriptorOrderValues(
                          row.values[key_term.column],
                          input_batch.rows[candidate->representative_row]
                              .values[key_term.column],
                          key_term);
                  if (!compared.diagnostic.ok) {
                    return refuse(
                        std::string(kPayloadDiagnostic),
                        "composed grouped key comparison: " +
                            compared.diagnostic.detail);
                  }
                  if (compared.comparison != 0) {
                    matches = false;
                    break;
                  }
                }
                if (matches) {
                  group = candidate;
                  break;
                }
              }
              if (group == groups.end()) {
                GroupPlanningState created;
                created.grouping_set_ordinal = set_ordinal;
                created.grouping_indicators =
                    metadata.grouping_indicators;
                created.grouping_id = metadata.grouping_id;
                created.representative_row = row_ordinal;
                created.has_representative = true;
                groups.push_back(std::move(created));
                group = std::prev(groups.end());
              } else if (!group->has_representative) {
                group->representative_row = row_ordinal;
                group->has_representative = true;
              }
              if (group->count ==
                  static_cast<std::uint64_t>(
                      std::numeric_limits<std::int64_t>::max())) {
                return refuse(std::string(kPayloadDiagnostic),
                              "composed grouped COUNT(*) overflowed");
              }
              ++group->count;
              const auto& amount = row.values[sum_column];
              if (amount.state == api::EngineValueState::sql_null ||
                  amount.is_null) {
                continue;
              }
              if (!exec::IsCanonicalBoundedSignedIntegerDescriptor(
                      amount.descriptor)) {
                return refuse(std::string(kPayloadDiagnostic),
                              "composed grouped SUM input is not "
                              "bounded-signed");
              }
              const auto decoded = exec::DecodeInt64Value(amount);
              if (!decoded.ok()) {
                return refuse(std::string(kPayloadDiagnostic),
                              "composed grouped SUM input is invalid");
              }
              group->sum += static_cast<__int128>(decoded.value);
              group->has_sum = true;
            }
          }
          const auto comparison_work = static_cast<std::uint64_t>(
              prepared.maximum_combined_grouping_key_comparison_count);
          std::uint64_t transition_work = 0;
          std::uint64_t aggregate_work = 0;
          if (!CheckedMultiply(input_row_count,
                               prepared.grouping_sets.size(),
                               &transition_work) ||
              !CheckedAdd(comparison_work, transition_work,
                          &aggregate_work) ||
              !add_work(aggregate_work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                "composition grouped aggregate work exceeds the admitted "
                "bound");
          }
          exec::DescriptorBatch output;
          output.columns = prepared.key_result_columns;
          output.columns.push_back(prepared.count.result_column);
          output.columns.push_back(prepared.sum.result_column);
          output.columns.insert(
              output.columns.end(),
              prepared.grouping_projection_columns.begin(),
              prepared.grouping_projection_columns.end());
          output.rows.reserve(groups.size());
          for (auto& group : groups) {
            exec::DescriptorTuple tuple;
            const auto& grouping_set =
                prepared.grouping_sets[group.grouping_set_ordinal];
            for (std::size_t key_ordinal = 0;
                 key_ordinal < prepared.key_terms.size(); ++key_ordinal) {
              api::EngineTypedValue key;
              key.descriptor =
                  prepared.key_result_columns[key_ordinal].descriptor;
              const bool included =
                  std::ranges::find(grouping_set.key_term_ordinals,
                                    key_ordinal) !=
                  grouping_set.key_term_ordinals.end();
              if (included) {
                if (!group.has_representative) {
                  return refuse(std::string(kPayloadDiagnostic),
                                "composed grouped key has no representative");
                }
                key = input_batch.rows[group.representative_row]
                          .values[prepared.key_terms[key_ordinal].column];
                key.descriptor =
                    prepared.key_result_columns[key_ordinal].descriptor;
              } else {
                key.is_null = true;
                key.state = api::EngineValueState::sql_null;
              }
              tuple.values.push_back(std::move(key));
            }
            api::EngineTypedValue count;
            count.descriptor = prepared.count.result_column.descriptor;
            count.encoded_value = std::to_string(group.count);
            count.state = api::EngineValueState::value;
            tuple.values.push_back(std::move(count));
            api::EngineTypedValue sum;
            sum.descriptor = prepared.sum.result_column.descriptor;
            sum.is_null = !group.has_sum;
            sum.state = group.has_sum ? api::EngineValueState::value
                                      : api::EngineValueState::sql_null;
            if (group.has_sum) {
              if (group.sum < static_cast<__int128>(
                                  std::numeric_limits<std::int64_t>::min()) ||
                  group.sum > static_cast<__int128>(
                                  std::numeric_limits<std::int64_t>::max())) {
                return refuse(std::string(kPayloadDiagnostic),
                              "composed grouped SUM result overflowed");
              }
              sum.encoded_value = std::to_string(
                  static_cast<std::int64_t>(group.sum));
            }
            tuple.values.push_back(std::move(sum));
            for (std::size_t key_ordinal = 0;
                 key_ordinal < prepared.key_terms.size() &&
                 !prepared.grouping_projection_columns.empty();
                 ++key_ordinal) {
              api::EngineTypedValue indicator;
              indicator.descriptor =
                  prepared.grouping_projection_columns[key_ordinal]
                      .descriptor;
              indicator.encoded_value =
                  group.grouping_indicators[key_ordinal] ? "1" : "0";
              indicator.state = api::EngineValueState::value;
              tuple.values.push_back(std::move(indicator));
            }
            if (!prepared.grouping_projection_columns.empty()) {
              api::EngineTypedValue grouping_id;
              grouping_id.descriptor =
                  prepared.grouping_projection_columns.back().descriptor;
              grouping_id.encoded_value =
                  std::to_string(group.grouping_id);
              grouping_id.state = api::EngineValueState::value;
              tuple.values.push_back(std::move(grouping_id));
            }
            output.rows.push_back(std::move(tuple));
          }
          const auto canonical = exec::ValidateCanonicalDescriptorBatch(
              output, node.output_descriptor_ids);
          const auto values = exec::ValidateDescriptorBatch(output);
          if (!canonical.ok || !values.ok) {
            return refuse(
                std::string(kPayloadDiagnostic),
                !canonical.ok
                    ? "grouped planning state: " + canonical.detail
                    : "grouped planning state: " + values.detail);
          }
          grouped_aggregate_input_row_count = input_row_count;
          grouped_aggregate_output_row_bound =
              static_cast<std::size_t>(output_row_bound);
          prepared_grouped_aggregate = std::move(prepared);
          state.batch = std::move(output);
          state.result_bindings =
              prepared_grouped_aggregate->result_bindings;
          implementation_id = "aggregate.registry-grouping-sets.v1";
          capability_uuid = grouped_aggregate_capability_uuid;
          transformation_rule =
              grouped_aggregate_profile.transformation_id + ".composed";
          physical_kind = exec::PhysicalNodeKind::kAggregate;
          auxiliary_memory = 0;
          break;
        }
        auto prepared = PrepareQueryDistinctRootForComposition(
            request.context, request.relational_dag, node, input_node, state);
        if (!prepared.ok) {
          return refuse(std::string(kPayloadDiagnostic), prepared.detail);
        }
        std::uint64_t pair_count = 0;
        std::uint64_t pair_value_count = 0;
        std::uint64_t self_value_count = 0;
        std::uint64_t comparison_bound = 0;
        if (!CheckedMultiply(input_row_count, input_row_count, &pair_count) ||
            !CheckedMultiply(pair_count, input_batch.columns.size(),
                             &pair_value_count) ||
            !CheckedMultiply(input_row_count, input_batch.columns.size(),
                             &self_value_count) ||
            !CheckedAdd(pair_value_count, self_value_count,
                        &comparison_bound) ||
            comparison_bound > std::numeric_limits<std::size_t>::max() ||
            !add_work(comparison_bound)) {
          return refuse(
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
              "composition DISTINCT comparison bound overflowed or was exhausted");
        }
        std::vector<std::size_t> representatives;
        representatives.reserve(input_row_count);
        for (std::size_t row = 0; row < input_row_count; ++row) {
          bool duplicate = false;
          for (const auto representative : representatives) {
            bool equal = true;
            for (const auto& term : prepared.equality_terms) {
              const auto compared = exec::CompareCanonicalDescriptorOrderValues(
                  input_batch.rows[row].values[term.column],
                  input_batch.rows[representative].values[term.column], term);
              if (!compared.diagnostic.ok) {
                return refuse(std::string(kPayloadDiagnostic),
                              "DISTINCT comparison: " +
                                  compared.diagnostic.detail);
              }
              if (compared.comparison != 0) {
                equal = false;
                break;
              }
            }
            if (equal) {
              duplicate = true;
              break;
            }
          }
          if (!duplicate) representatives.push_back(row);
        }
        exec::DescriptorBatch output;
        output.columns = input_batch.columns;
        output.rows.reserve(representatives.size());
        for (const auto row : representatives) {
          output.rows.push_back(input_batch.rows[row]);
        }
        prepared_distinct = std::move(prepared);
        distinct_input_row_count = input_row_count;
        distinct_comparison_bound =
            std::max<std::size_t>(1,
                static_cast<std::size_t>(comparison_bound));
        state.batch = std::move(output);
        state.result_bindings = prepared_distinct->result_bindings;
        implementation_id = "aggregate.query-distinct.typed.v1";
        capability_uuid = distinct_capability_uuid;
        transformation_rule =
            "canonical.aggregate.composed-query-distinct.v1";
        physical_kind = exec::PhysicalNodeKind::kAggregate;
        if (!QueryDistinctAuxiliaryMemoryBytes(
                input_row_count, input_batch.columns.size(),
                &auxiliary_memory)) {
          return refuse(
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
              "composition DISTINCT resident state size overflowed");
        }
        break;
      }
      case plan::CanonicalLogicalRelationalNodeKind::kSort: {
        const bool expression_ordering = std::ranges::any_of(
            node.bound_expression_ids, [&](const auto expression_id) {
              return std::ranges::find(input_node.bound_expression_ids,
                                       expression_id) ==
                     input_node.bound_expression_ids.end();
            });
        auto prepared =
            expression_ordering
                ? PrepareExpressionSortRootForComposition(
                      request.context, request.relational_dag,
                      request.optimizer_request.logical_properties, node,
                      input_node, state, request.expression_services)
                : PrepareSortRootForComposition(
                      request.context, request.relational_dag,
                      request.optimizer_request.logical_properties, node,
                      input_node, state);
        if (!prepared.ok) {
          return refuse(std::string(kPayloadDiagnostic), prepared.detail);
        }
        std::uint64_t expression_work = 0;
        std::uint64_t expression_memory = 1;
        if (prepared.expression_ordering &&
            (!CheckedMultiply(input_row_count, prepared.expressions.size(),
                              &expression_work) ||
             !AddBatchMemoryBytes(prepared.expression_input_batch,
                                  &expression_memory) ||
             !add_work(expression_work))) {
          return refuse(
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
              "composition expression SORT evaluation bound overflowed or "
              "was exhausted");
        }
        std::uint64_t comparison_bound = 0;
        std::uint64_t row_order_memory = 0;
        if (!CheckedMultiply(input_row_count, input_row_count,
                             &comparison_bound) ||
            !CheckedMultiply(input_row_count, sizeof(std::size_t),
                             &row_order_memory) ||
            comparison_bound > std::numeric_limits<std::size_t>::max() ||
            !CheckedAdd(comparison_bound, row_order_memory,
                        &auxiliary_memory) ||
            (prepared.expression_ordering &&
             !CheckedAdd(auxiliary_memory, expression_memory,
                         &auxiliary_memory)) ||
            !add_work(comparison_bound)) {
          return refuse(
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
              "composition SORT comparison bound overflowed or was exhausted");
        }
        const auto& order_key_batch = prepared.expression_ordering
                                          ? prepared.expression_input_batch
                                          : input_batch;
        std::vector<std::int8_t> comparisons(
            static_cast<std::size_t>(comparison_bound), 0);
        for (std::size_t left = 0; left < input_row_count; ++left) {
          for (std::size_t right = left + 1; right < input_row_count;
               ++right) {
            int comparison = 0;
            for (const auto& term : prepared.order_terms) {
              const auto compared = exec::CompareCanonicalDescriptorOrderValues(
                  order_key_batch.rows[left].values[term.column],
                  order_key_batch.rows[right].values[term.column], term);
              if (!compared.diagnostic.ok) {
                return refuse(std::string(kPayloadDiagnostic),
                              "SORT comparison: " +
                                  compared.diagnostic.detail);
              }
              comparison = compared.comparison;
              if (comparison != 0) break;
            }
            comparisons[left * input_row_count + right] =
                static_cast<std::int8_t>(comparison);
            comparisons[right * input_row_count + left] =
                static_cast<std::int8_t>(-comparison);
          }
        }
        std::vector<std::size_t> row_order(input_row_count);
        std::iota(row_order.begin(), row_order.end(), 0);
        std::stable_sort(row_order.begin(), row_order.end(),
                         [&](const auto left, const auto right) {
                           return comparisons[left * input_row_count + right] <
                                  0;
                         });
        exec::DescriptorBatch output;
        output.columns = input_batch.columns;
        output.rows.reserve(input_row_count);
        for (const auto row : row_order) {
          output.rows.push_back(input_batch.rows[row]);
        }
        prepared_sort = std::move(prepared);
        sort_input_row_count = input_row_count;
        sort_comparison_bound = std::max<std::size_t>(
            1, static_cast<std::size_t>(comparison_bound));
        state.batch = std::move(output);
        state.result_bindings = prepared_sort->result_bindings;
        implementation_id = prepared_sort->expression_ordering
                                ? "sort.typed.expression-row.v1"
                                : "sort.typed.terms.v1";
        capability_uuid = sort_capability_uuid;
        transformation_rule = prepared_sort->expression_ordering
                                  ? "canonical.sort.composed-expression-row.v1"
                                  : "canonical.sort.composed-typed-terms.v1";
        physical_kind = exec::PhysicalNodeKind::kSort;
        delivered_property_uuids = {prepared_sort->ordering_property_uuid};
        property_kinds = {
            plan::CanonicalLogicalPropertyKind::kOrdering};
        break;
      }
      case plan::CanonicalLogicalRelationalNodeKind::kWindow: {
        const bool rank_window =
            node.semantic_variant_id == "window.rank.v1";
        const bool dense_rank_window =
            node.semantic_variant_id == "window.dense-rank.v1";
        const bool percent_rank_window =
            node.semantic_variant_id == "window.percent-rank.v1";
        const bool cume_dist_window =
            node.semantic_variant_id == "window.cume-dist.v1";
        const bool ntile_window =
            node.semantic_variant_id == "window.ntile.v1";
        const bool lag_window =
            node.semantic_variant_id == "window.lag.v1";
        const bool lead_window =
            node.semantic_variant_id == "window.lead.v1";
        const bool first_value_window =
            node.semantic_variant_id == "window.first-value.v1";
        const bool last_value_window =
            node.semantic_variant_id == "window.last-value.v1";
        const bool nth_value_window =
            node.semantic_variant_id == "window.nth-value.v1";
        const bool aggregate_window =
            node.semantic_variant_id == "window.aggregate-bridge.v1";
        const bool navigation_window = lag_window || lead_window;
        const bool value_window =
            navigation_window || first_value_window || last_value_window ||
            nth_value_window || aggregate_window;
        const bool peer_ranking_window =
            rank_window || dense_rank_window || percent_rank_window ||
            cume_dist_window;
        const bool real_ranking_window =
            percent_rank_window || cume_dist_window;
        auto ranking_profile =
            aggregate_window
                ? GlobalAggregateWindowProfileForComposition(
                      request.relational_dag, node.logical_node_id)
                : value_window
                ? (first_value_window
                       ? kGlobalFirstValueProfile
                       : (last_value_window
                              ? kGlobalLastValueProfile
                              : (nth_value_window
                                     ? kGlobalNthValueProfile
                                     : (lag_window ? kGlobalLagProfile
                                                   : kGlobalLeadProfile))))
                : (ntile_window
                ? kGlobalNtileProfile
                : (cume_dist_window
                ? kGlobalCumeDistProfile
                : (percent_rank_window
                       ? kGlobalPercentRankProfile
                       : (dense_rank_window
                              ? kGlobalDenseRankProfile
                              : (rank_window ? kGlobalRankProfile
                                             : kGlobalRowNumberProfile)))));
        const bool aggregate_count_window =
            aggregate_window &&
            ranking_profile.builtin_id == "sb.aggregate.count";
        const bool aggregate_boolean_window =
            aggregate_window &&
            (ranking_profile.builtin_id == "sb.aggregate.bool_and" ||
             ranking_profile.builtin_id == "sb.aggregate.bool_or" ||
             ranking_profile.builtin_id == "sb.aggregate.every");
        const bool aggregate_bounded_signed_window =
            aggregate_window && !aggregate_count_window &&
            !aggregate_boolean_window;
        const std::string_view ranking_name = ranking_profile.display_name;
        const auto typed_consumer = std::ranges::find_if(
            request.relational_dag.nodes, [&](const auto& candidate) {
              return candidate.node_id == node.logical_node_id;
            });
        if (typed_consumer == request.relational_dag.nodes.end() ||
            typed_consumer->node_kind != api::RelationalDagNodeKind::kWindow ||
            !prepared_sort.has_value() ||
            ((peer_ranking_window || ntile_window || value_window) &&
             (prepared_sort->expression_ordering ||
              prepared_sort->order_terms.size() != 1))) {
          return refuse(std::string(kPayloadDiagnostic),
                        "composition " + std::string(ranking_name) +
                            " input binding is unresolved");
        }
        const auto core_manifest =
            dt::LoadCurrentCoreDatatypeCatalogManifest();
        if (!core_manifest.ok()) {
          return refuse(std::string(kPayloadDiagnostic),
                        "composition " + std::string(ranking_name) +
                            " core datatype catalog is unavailable");
        }
        const auto order_type_uuid =
            ExactCanonicalCoreDatatypeTypeUuidV1("int64");
        const auto boolean_type_uuid =
            ExactCanonicalCoreDatatypeTypeUuidV1("boolean");
        if (!aggregate_window &&
            DirectValueWindowUsesExactTypeForComposition(
                request.relational_dag, node.logical_node_id,
                ranking_profile.builtin_id, boolean_type_uuid)) {
          ranking_profile.result_type_name = "boolean";
        }
        const auto result_type_uuid = ExactCanonicalCoreDatatypeTypeUuidV1(
            ranking_profile.result_type_name);
        const std::array<std::string, 4> bounded_signed_type_uuids = {
            ExactCanonicalCoreDatatypeTypeUuidV1("int8"),
            ExactCanonicalCoreDatatypeTypeUuidV1("int16"),
            ExactCanonicalCoreDatatypeTypeUuidV1("int32"),
            order_type_uuid};
        const auto ranking = PrepareGlobalRankingWindowBindingForComposition(
            request.relational_dag,
            request.optimizer_request.logical_properties, *typed_consumer,
            node, input_node, *prepared_sort, input_batch.columns.size(),
            input_bindings.size(), result_type_uuid, order_type_uuid,
            boolean_type_uuid, bounded_signed_type_uuids, "composition",
            ranking_profile);
        if (!ranking.ok) {
          return refuse(ranking.diagnostic_id, ranking.detail);
        }
        if (ntile_window &&
            !ranking.ntile_bucket_count_operand.has_value()) {
          return refuse(std::string(kPayloadDiagnostic),
                        "composition NTILE bucket operand is unresolved");
        }
        if (value_window && !ranking.aggregate_count_star &&
            !ranking.navigation_value_column.has_value()) {
          return refuse(std::string(kPayloadDiagnostic),
                        "composition " + std::string(ranking_name) +
                            " value column is unresolved");
        }
        if (nth_value_window &&
            !ranking.nth_value_position_operand.has_value()) {
          return refuse(std::string(kPayloadDiagnostic),
                        "composition NTH_VALUE position is unresolved");
        }
        if (input_row_count > static_cast<std::size_t>(
                                  std::numeric_limits<std::int64_t>::max())) {
          return refuse(std::string(kPayloadDiagnostic),
                        "composition " + std::string(ranking_name) +
                            " exceeds its supported partition cardinality");
        }
        const auto* output_descriptor = ranking.result_descriptor;
        if (aggregate_window) {
          const auto order_column = prepared_sort->order_terms.front().column;
          const auto order_relational_descriptor = std::ranges::find_if(
              request.relational_dag.descriptors, [&](const auto& candidate) {
                return order_column < input_node.output_descriptor_ids.size() &&
                       candidate.descriptor_id ==
                           input_node.output_descriptor_ids[order_column];
              });
          if (order_column >= input_batch.columns.size() ||
              order_relational_descriptor ==
                  request.relational_dag.descriptors.end() ||
              !ExactCanonicalBoundedSignedWindowOrderForComposition(
                  *order_relational_descriptor,
                  input_batch.columns[order_column].descriptor,
                  input_batch.columns[order_column].nullable,
                  bounded_signed_type_uuids,
                  ranking_profile.function_uuid,
                  output_descriptor->descriptor_uuid,
                  result_type_uuid,
                  prepared_sort->ordering_property_uuid,
                  ranking.window_property_uuid,
                  ranking.window_frame_descriptor_uuid) ||
              (!ranking.aggregate_count_star &&
               (*ranking.navigation_value_column >=
                    input_batch.columns.size() ||
                (order_column != *ranking.navigation_value_column &&
                 input_batch.columns[order_column]
                         .descriptor.descriptor_uuid ==
                     input_batch.columns[*ranking.navigation_value_column]
                         .descriptor.descriptor_uuid)))) {
            return refuse(
                std::string(kPayloadDiagnostic),
                "composition " + std::string(ranking_name) +
                    " order key is not one exact core bounded-signed "
                    "column");
          }
        }
        if (aggregate_count_window && !ranking.aggregate_count_star) {
          const auto source_column = *ranking.navigation_value_column;
          const auto order_column = prepared_sort->order_terms.front().column;
          const auto source_relational_descriptor = std::ranges::find_if(
              request.relational_dag.descriptors, [&](const auto& candidate) {
                return source_column < input_node.output_descriptor_ids.size() &&
                       candidate.descriptor_id ==
                           input_node.output_descriptor_ids[source_column];
              });
          const auto order_relational_descriptor = std::ranges::find_if(
              request.relational_dag.descriptors, [&](const auto& candidate) {
                return order_column < input_node.output_descriptor_ids.size() &&
                       candidate.descriptor_id ==
                           input_node.output_descriptor_ids[order_column];
              });
          if (source_column >= input_batch.columns.size() ||
              order_column >= input_batch.columns.size() ||
              source_relational_descriptor ==
                  request.relational_dag.descriptors.end() ||
              order_relational_descriptor ==
                  request.relational_dag.descriptors.end() ||
              !ExactCanonicalScalarWindowOperandForComposition(
                  *source_relational_descriptor,
                  input_batch.columns[source_column].descriptor,
                  input_batch.columns[source_column].nullable,
                  ranking_profile.function_uuid,
                  output_descriptor->descriptor_uuid, result_type_uuid,
                  input_batch.columns[order_column]
                      .descriptor.descriptor_uuid,
                  order_relational_descriptor->type_uuid,
                  source_column == order_column,
                  prepared_sort->ordering_property_uuid,
                  ranking.window_property_uuid,
                  ranking.window_frame_descriptor_uuid)) {
            return refuse(
                std::string(kPayloadDiagnostic),
                "composition COUNT source descriptor is not exact");
          }
        }
        if (value_window && !aggregate_window) {
          const auto source_column = *ranking.navigation_value_column;
          const auto order_column = prepared_sort->order_terms.front().column;
          const auto source_relational_descriptor = std::ranges::find_if(
              request.relational_dag.descriptors, [&](const auto& candidate) {
                return source_column < input_node.output_descriptor_ids.size() &&
                       candidate.descriptor_id ==
                           input_node.output_descriptor_ids[source_column];
              });
          const auto order_relational_descriptor = std::ranges::find_if(
              request.relational_dag.descriptors, [&](const auto& candidate) {
                return order_column < input_node.output_descriptor_ids.size() &&
                       candidate.descriptor_id ==
                           input_node.output_descriptor_ids[order_column];
              });
          if (source_column >= input_batch.columns.size() ||
              order_column >= input_batch.columns.size() ||
              source_relational_descriptor ==
                  request.relational_dag.descriptors.end() ||
              order_relational_descriptor ==
                  request.relational_dag.descriptors.end() ||
              !ExactCanonicalScalarWindowOperandForComposition(
                  *source_relational_descriptor,
                  input_batch.columns[source_column].descriptor,
                  input_batch.columns[source_column].nullable,
                  ranking_profile.function_uuid,
                  output_descriptor->descriptor_uuid, result_type_uuid,
                  input_batch.columns[order_column]
                      .descriptor.descriptor_uuid,
                  order_relational_descriptor->type_uuid,
                  source_column == order_column,
                  prepared_sort->ordering_property_uuid,
                  ranking.window_property_uuid,
                  ranking.window_frame_descriptor_uuid) ||
              !ExactCanonicalScalarWindowOperandForComposition(
                  *order_relational_descriptor,
                  input_batch.columns[order_column].descriptor,
                  input_batch.columns[order_column].nullable,
                  ranking_profile.function_uuid,
                  output_descriptor->descriptor_uuid, result_type_uuid,
                  input_batch.columns[source_column]
                      .descriptor.descriptor_uuid,
                  source_relational_descriptor->type_uuid,
                  source_column == order_column,
                  prepared_sort->ordering_property_uuid,
                  ranking.window_property_uuid,
                  ranking.window_frame_descriptor_uuid)) {
            return refuse(
                std::string(kPayloadDiagnostic),
                "composition " + std::string(ranking_name) +
                    " source/order descriptors are not exact");
          }
        }
        if (aggregate_boolean_window || aggregate_bounded_signed_window) {
          const auto source_column = *ranking.navigation_value_column;
          const auto source_relational_descriptor = std::ranges::find_if(
              request.relational_dag.descriptors, [&](const auto& candidate) {
                return source_column < input_node.output_descriptor_ids.size() &&
                       candidate.descriptor_id ==
                           input_node.output_descriptor_ids[source_column];
              });
          if (source_column >= input_batch.columns.size() ||
              source_relational_descriptor ==
                  request.relational_dag.descriptors.end() ||
              (aggregate_boolean_window
                   ? !ExactCanonicalBooleanWindowSourceForComposition(
                         *source_relational_descriptor,
                         input_batch.columns[source_column].descriptor,
                         input_batch.columns[source_column].nullable,
                         boolean_type_uuid, ranking_profile.function_uuid,
                         output_descriptor->descriptor_uuid,
                         prepared_sort->ordering_property_uuid,
                         ranking.window_property_uuid,
                         ranking.window_frame_descriptor_uuid)
                   : !ExactCanonicalBoundedSignedWindowSourceForComposition(
                         *source_relational_descriptor,
                         input_batch.columns[source_column].descriptor,
                         input_batch.columns[source_column].nullable,
                         bounded_signed_type_uuids,
                         ranking_profile.function_uuid,
                         output_descriptor->descriptor_uuid,
                         prepared_sort->ordering_property_uuid,
                         ranking.window_property_uuid,
                         ranking.window_frame_descriptor_uuid))) {
            return refuse(
                std::string(kPayloadDiagnostic),
                "composition " + std::string(ranking_name) +
                    " source is not one exact core " +
                    (aggregate_boolean_window ? "boolean"
                                              : "bounded-signed") +
                    " column");
          }
        }
        api::EngineDescriptor descriptor;
        std::uint64_t navigation_value_payload_memory = 0;
        std::uint64_t maximum_value_payload_memory = 0;
        std::uint64_t result_value_payload_memory = 0;
        std::uint64_t nth_position_operand_memory = 0;
        std::uint64_t nth_position_vector_payload_memory = 0;
        std::uint64_t nth_position = 0;
        if (nth_value_window) {
          const auto decoded = exec::DecodeInt64Value(
              *ranking.nth_value_position_operand);
          if (!decoded.ok() || decoded.value <= 0 ||
              !RuntimeTypedValueMemoryBytes(
                  *ranking.nth_value_position_operand,
                  &nth_position_operand_memory) ||
              !CheckedMultiply(nth_position_operand_memory, input_row_count,
                               &nth_position_vector_payload_memory)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition NTH_VALUE position is invalid or exceeds its "
                "resource bound");
          }
          nth_position = static_cast<std::uint64_t>(decoded.value);
        }
        if (ranking.aggregate_count_star) {
          if (!CheckedMultiply(20, input_row_count,
                               &result_value_payload_memory)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition COUNT(*) repeated result payload overflowed");
          }
          std::uint64_t retained_input_and_output_memory = 0;
          if (!CheckedAdd(input_memory, input_memory,
                          &retained_input_and_output_memory) ||
              !CheckedAdd(retained_input_and_output_memory,
                          result_value_payload_memory,
                          &retained_input_and_output_memory) ||
              retained_input_and_output_memory >
                  request.optimizer_request.resource.memory_budget_bytes) {
            return refuse(
                "QOW-DIAG-OPT-017-REFUSAL-V1",
                "composition COUNT(*) result materialization exceeds its "
                "memory budget");
          }
          descriptor.descriptor_uuid =
              output_descriptor->descriptor_uuid;
          descriptor.descriptor_kind = "scalar";
          descriptor.canonical_type_name = "int64";
          descriptor.encoded_descriptor =
              "type_uuid=" + output_descriptor->type_uuid +
              ";nullability=non_null";
        } else if (value_window) {
          for (const auto& row : input_batch.rows) {
            const auto& value =
                row.values[*ranking.navigation_value_column];
            std::uint64_t value_payload_memory = 0;
            if (!CheckedAdd(value.encoded_value.size(),
                            value.binary_value.size(),
                            &value_payload_memory) ||
                !CheckedAdd(navigation_value_payload_memory,
                            value_payload_memory,
                            &navigation_value_payload_memory)) {
              return refuse(
                  "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "composition " + std::string(ranking_name) +
                      " value payload size overflowed");
            }
            maximum_value_payload_memory =
                std::max(maximum_value_payload_memory,
                         value_payload_memory);
          }
          if (aggregate_window) {
            const std::uint64_t maximum_aggregate_text_bytes =
                ranking_profile.result_type_name == "boolean" ? 5 : 20;
            if (!CheckedMultiply(maximum_aggregate_text_bytes, input_row_count,
                                 &result_value_payload_memory)) {
              return refuse(
                  "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "composition " + std::string(ranking_name) +
                      " repeated result payload overflowed");
            }
          } else if (first_value_window || last_value_window ||
                     nth_value_window) {
            if (!CheckedMultiply(maximum_value_payload_memory,
                                 input_row_count,
                                 &result_value_payload_memory)) {
              return refuse(
                  "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "composition " + std::string(ranking_name) +
                      " repeated result payload overflowed");
            }
          } else {
            result_value_payload_memory = navigation_value_payload_memory;
          }
          std::uint64_t retained_input_and_output_memory = 0;
          if (!CheckedAdd(input_memory, input_memory,
                          &retained_input_and_output_memory) ||
              !CheckedAdd(retained_input_and_output_memory,
                          result_value_payload_memory,
                          &retained_input_and_output_memory) ||
              retained_input_and_output_memory >
                  request.optimizer_request.resource.memory_budget_bytes) {
            return refuse(
                "QOW-DIAG-OPT-017-REFUSAL-V1",
                "composition " + std::string(ranking_name) +
                    " result materialization exceeds its memory budget");
          }
          if (aggregate_bounded_signed_window) {
            descriptor.descriptor_uuid =
                output_descriptor->descriptor_uuid;
            descriptor.descriptor_kind = "scalar";
            descriptor.canonical_type_name = "int64";
            descriptor.encoded_descriptor =
                "type_uuid=" + output_descriptor->type_uuid +
                ";nullability=nullable";
          } else if (!aggregate_count_window) {
            descriptor = input_batch
                             .columns[*ranking.navigation_value_column]
                             .descriptor;
            descriptor.descriptor_uuid =
                output_descriptor->descriptor_uuid;
            if (!exec::DeriveCanonicalNullableDescriptorEncoding(&descriptor) ||
                !exec::CanonicalDerivedDescriptorTypeMatches(
                    input_batch.columns[*ranking.navigation_value_column]
                        .descriptor,
                    input_batch.columns[*ranking.navigation_value_column]
                        .nullable,
                    descriptor, true) ||
                (aggregate_boolean_window &&
                 descriptor.encoded_descriptor !=
                     "type_uuid=" + boolean_type_uuid +
                         ";nullability=nullable")) {
              return refuse(std::string(kPayloadDiagnostic),
                            "composition " + std::string(ranking_name) +
                                " nullable result descriptor does not "
                            "preserve its exact source shape");
            }
          } else {
            descriptor.descriptor_uuid =
                output_descriptor->descriptor_uuid;
            descriptor.descriptor_kind = "scalar";
            descriptor.canonical_type_name = "int64";
            descriptor.encoded_descriptor =
                "type_uuid=" + output_descriptor->type_uuid +
                ";nullability=non_null";
          }
        } else {
          descriptor.descriptor_uuid =
              output_descriptor->descriptor_uuid;
          descriptor.descriptor_kind = "scalar";
          descriptor.canonical_type_name =
              std::string(ranking_profile.result_type_name);
          descriptor.encoded_descriptor =
              "type_uuid=" + output_descriptor->type_uuid +
              ";nullability=non_null";
        }
        exec::ExecutorColumnDescriptor ranking_column{
            ranking.outputs.back()->output_name_utf8, descriptor,
            value_window && !aggregate_count_window,
            output_descriptor->descriptor_id};
        if (!api::QowCanonicalDescriptorIdentityV1(descriptor)) {
          return refuse(std::string(kPayloadDiagnostic),
                        "composition " + std::string(ranking_name) +
                            " descriptor is invalid");
        }
        exec::CanonicalResultColumnBinding ranking_binding;
        ranking_binding.physical_column_ordinal =
            input_batch.columns.size();
        ranking_binding.visible = true;
        ranking_binding.published_descriptor =
            exec::CanonicalResultColumnDescriptor{
                static_cast<std::uint32_t>(input_batch.columns.size()),
                ranking_column.stable_name,
                output_descriptor->descriptor_uuid,
                output_descriptor->type_uuid,
                value_window && !aggregate_count_window
                    ? exec::CanonicalResultNullability::kNullable
                    : exec::CanonicalResultNullability::kNonNull,
                std::nullopt,
                std::nullopt};
        std::uint64_t frame_value_comparison_workspace_bytes = 0;
        if (last_value_window || nth_value_window || aggregate_window) {
          const auto& order_term = prepared_sort->order_terms.front();
          if (aggregate_window) {
            std::uint64_t maximum_key_workspace_bytes = 0;
            for (const auto& row : input_batch.rows) {
              const auto key_plan =
                  exec::PlanCanonicalDescriptorEqualityKey(
                      row.values[order_term.column], order_term);
              if (!key_plan.diagnostic.ok ||
                  key_plan.peak_workspace_bytes >
                      std::numeric_limits<std::uint64_t>::max()) {
                return refuse(
                    "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "composition " + std::string(ranking_name) +
                        " comparison workspace was refused");
              }
              maximum_key_workspace_bytes = std::max(
                  maximum_key_workspace_bytes,
                  static_cast<std::uint64_t>(
                      key_plan.peak_workspace_bytes));
            }
            if (!CheckedMultiply(maximum_key_workspace_bytes, 2,
                                 &frame_value_comparison_workspace_bytes)) {
              return refuse(
                  "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "composition " + std::string(ranking_name) +
                      " comparison workspace overflowed");
            }
          } else {
            for (std::size_t row = 1; row < input_batch.rows.size(); ++row) {
              const auto& left_value =
                  input_batch.rows[row - 1].values[order_term.column];
              const auto& right_value =
                  input_batch.rows[row].values[order_term.column];
              const auto left_plan =
                  exec::PlanCanonicalDescriptorEqualityKey(left_value,
                                                            order_term);
              const auto right_plan =
                  exec::PlanCanonicalDescriptorEqualityKey(right_value,
                                                            order_term);
              std::uint64_t pair_workspace_bytes = 0;
              if (!left_plan.diagnostic.ok || !right_plan.diagnostic.ok ||
                  left_plan.peak_workspace_bytes >
                      std::numeric_limits<std::uint64_t>::max() ||
                  right_plan.peak_workspace_bytes >
                      std::numeric_limits<std::uint64_t>::max() ||
                  !CheckedAdd(
                      static_cast<std::uint64_t>(
                          left_plan.peak_workspace_bytes),
                      static_cast<std::uint64_t>(
                          right_plan.peak_workspace_bytes),
                      &pair_workspace_bytes)) {
                return refuse(
                    "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "composition " + std::string(ranking_name) +
                        " comparison workspace overflowed or was refused");
              }
              frame_value_comparison_workspace_bytes =
                  std::max(frame_value_comparison_workspace_bytes,
                           pair_workspace_bytes);
            }
          }
          std::uint64_t last_value_materialization_memory_bytes = 0;
          if (!CheckedAdd(input_memory, input_memory,
                          &last_value_materialization_memory_bytes) ||
              !CheckedAdd(last_value_materialization_memory_bytes,
                          result_value_payload_memory,
                          &last_value_materialization_memory_bytes) ||
              !CheckedAdd(last_value_materialization_memory_bytes,
                          frame_value_comparison_workspace_bytes,
                          &last_value_materialization_memory_bytes) ||
              !CheckedAdd(last_value_materialization_memory_bytes,
                          nth_position_operand_memory,
                          &last_value_materialization_memory_bytes) ||
              last_value_materialization_memory_bytes >
                  request.optimizer_request.resource.memory_budget_bytes) {
            return refuse(
                "QOW-DIAG-OPT-017-REFUSAL-V1",
                "composition " + std::string(ranking_name) +
                    " peer comparison exceeds its memory budget");
          }
        }
        exec::DescriptorBatch output = input_batch;
        output.columns.push_back(ranking_column);
        std::uint64_t ntile_bucket_count = 0;
        if (ntile_window) {
          const auto decoded = exec::DecodeInt64Value(
              *ranking.ntile_bucket_count_operand);
          if (!decoded.ok() || decoded.value <= 0) {
            return refuse(std::string(kPayloadDiagnostic),
                          "composition NTILE bucket operand is invalid");
          }
          ntile_bucket_count = static_cast<std::uint64_t>(decoded.value);
        }
        std::string cume_dist_detail;
        const auto append_cume_dist_group =
            [&](const std::size_t peer_begin,
                const std::size_t peer_end_exclusive) {
              for (std::size_t peer_row = peer_begin;
                   peer_row < peer_end_exclusive; ++peer_row) {
                exec::CanonicalWindowCumeDistValueRequest rank_request;
                rank_request.function_abi_version = 1;
                rank_request.builtin_id =
                    std::string(ranking_profile.builtin_id);
                rank_request.function_uuid =
                    std::string(ranking_profile.function_uuid);
                rank_request.output_descriptor = descriptor;
                rank_request.cumulative_row_count = peer_end_exclusive;
                rank_request.partition_row_count = input_row_count;
                auto rank =
                    exec::ComputeCanonicalWindowCumeDistValue(rank_request);
                if (!rank.diagnostic.ok) {
                  cume_dist_detail = rank.diagnostic.detail;
                  return false;
                }
                output.rows[peer_row].values.push_back(
                    std::move(rank.value));
              }
              return true;
            };
        std::size_t peer_begin = 0;
        std::size_t frame_value_peer_end = 0;
        std::size_t current_rank = 1;
        std::uint64_t rank_comparison_workspace_bytes = 0;
        for (std::size_t row = 0; row < output.rows.size(); ++row) {
          if (peer_ranking_window && row != 0) {
            const auto& order_term = prepared_sort->order_terms.front();
            const auto& left_value =
                input_batch.rows[row - 1].values[order_term.column];
            const auto& right_value =
                input_batch.rows[row].values[order_term.column];
            const auto left_plan =
                exec::PlanCanonicalDescriptorEqualityKey(left_value,
                                                          order_term);
            const auto right_plan =
                exec::PlanCanonicalDescriptorEqualityKey(right_value,
                                                          order_term);
            std::uint64_t pair_workspace_bytes = 0;
            if (!left_plan.diagnostic.ok || !right_plan.diagnostic.ok ||
                left_plan.peak_workspace_bytes >
                    std::numeric_limits<std::uint64_t>::max() ||
                right_plan.peak_workspace_bytes >
                    std::numeric_limits<std::uint64_t>::max() ||
                !CheckedAdd(
                    static_cast<std::uint64_t>(
                        left_plan.peak_workspace_bytes),
                    static_cast<std::uint64_t>(
                        right_plan.peak_workspace_bytes),
                    &pair_workspace_bytes)) {
              return refuse(
                  "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "composition " + std::string(ranking_name) +
                      " comparison workspace overflowed or was refused");
            }
            rank_comparison_workspace_bytes = std::max(
                rank_comparison_workspace_bytes, pair_workspace_bytes);
            const auto compared = exec::CompareCanonicalDescriptorOrderValues(
                left_value, right_value, order_term);
            if (!compared.diagnostic.ok || compared.comparison > 0) {
              return refuse(
                  std::string(kPayloadDiagnostic),
                  !compared.diagnostic.ok
                      ? "composition " + std::string(ranking_name) +
                            " peer comparison: " +
                            compared.diagnostic.detail
                      : "composition " + std::string(ranking_name) +
                            " input is not canonically ordered");
            }
            if (compared.comparison != 0) {
              if (cume_dist_window) {
                if (!append_cume_dist_group(peer_begin, row)) {
                  return refuse(std::string(kPayloadDiagnostic),
                                cume_dist_detail);
                }
                peer_begin = row;
              } else {
                current_rank =
                    dense_rank_window ? current_rank + 1 : row + 1;
              }
            }
          }
          if (cume_dist_window) continue;
          api::EngineTypedValue value;
          if (aggregate_window) {
            // Planning retains schema/cardinality only. The selected
            // aggregate-window executor publishes the canonical registry
            // result after the physical DAG is immutable.
            value.descriptor = descriptor;
            value.is_null = !aggregate_count_window;
            value.state = aggregate_count_window
                              ? api::EngineValueState::value
                              : api::EngineValueState::sql_null;
            if (aggregate_count_window) value.encoded_value = "0";
          } else if (value_window) {
            value.descriptor = descriptor;
            std::optional<std::size_t> target_row;
            if (first_value_window && !input_batch.rows.empty()) {
              target_row = 0;
            } else if (last_value_window || nth_value_window) {
              if (row >= frame_value_peer_end) {
                frame_value_peer_end = row + 1;
                const auto& order_term = prepared_sort->order_terms.front();
                while (frame_value_peer_end < input_batch.rows.size()) {
                  const auto& left_value =
                      input_batch.rows[frame_value_peer_end - 1]
                          .values[order_term.column];
                  const auto& right_value =
                      input_batch.rows[frame_value_peer_end]
                          .values[order_term.column];
                  const auto compared =
                      exec::CompareCanonicalDescriptorOrderValues(
                          left_value, right_value, order_term);
                  if (!compared.diagnostic.ok || compared.comparison > 0) {
                    return refuse(
                        std::string(kPayloadDiagnostic),
                        !compared.diagnostic.ok
                            ? "composition " + std::string(ranking_name) +
                                  " peer comparison: " +
                                  compared.diagnostic.detail
                            : "composition " + std::string(ranking_name) +
                                  " input is not canonically ordered");
                  }
                  if (compared.comparison != 0) break;
                  ++frame_value_peer_end;
                }
              }
              if (last_value_window) {
                target_row = frame_value_peer_end - 1;
              } else if (nth_position <= frame_value_peer_end) {
                target_row = static_cast<std::size_t>(nth_position - 1);
              }
            } else if (lag_window ? row != 0
                                  : row + 1 < input_batch.rows.size()) {
              target_row = lag_window ? row - 1 : row + 1;
            }
            if (!target_row.has_value()) {
              value.is_null = true;
              value.state = api::EngineValueState::sql_null;
            } else {
              value = input_batch.rows[*target_row]
                          .values[*ranking.navigation_value_column];
              value.descriptor = descriptor;
            }
          } else if (ntile_window) {
            exec::CanonicalWindowNtileValueRequest ntile_request;
            ntile_request.function_abi_version = 1;
            ntile_request.builtin_id =
                std::string(ranking_profile.builtin_id);
            ntile_request.function_uuid =
                std::string(ranking_profile.function_uuid);
            ntile_request.output_descriptor = descriptor;
            ntile_request.zero_based_partition_position = row;
            ntile_request.partition_row_count = input_row_count;
            ntile_request.bucket_count = ntile_bucket_count;
            auto ntile = exec::ComputeCanonicalWindowNtileValue(ntile_request);
            if (!ntile.diagnostic.ok) {
              return refuse(std::string(kPayloadDiagnostic),
                            ntile.diagnostic.detail);
            }
            value = std::move(ntile.value);
          } else if (percent_rank_window) {
            exec::CanonicalWindowPercentRankValueRequest rank_request;
            rank_request.function_abi_version = 1;
            rank_request.builtin_id = std::string(ranking_profile.builtin_id);
            rank_request.function_uuid =
                std::string(ranking_profile.function_uuid);
            rank_request.output_descriptor = descriptor;
            rank_request.one_based_rank = current_rank;
            rank_request.partition_row_count = input_row_count;
            auto rank =
                exec::ComputeCanonicalWindowPercentRankValue(rank_request);
            if (!rank.diagnostic.ok) {
              return refuse(std::string(kPayloadDiagnostic),
                            rank.diagnostic.detail);
            }
            value = std::move(rank.value);
          } else if (peer_ranking_window) {
            exec::CanonicalWindowIntegerRankValueRequest rank_request;
            rank_request.function_abi_version = 1;
            rank_request.builtin_id = std::string(ranking_profile.builtin_id);
            rank_request.function_uuid =
                std::string(ranking_profile.function_uuid);
            rank_request.output_descriptor = descriptor;
            rank_request.one_based_rank = current_rank;
            auto rank =
                exec::ComputeCanonicalWindowIntegerRankValue(rank_request);
            if (!rank.diagnostic.ok) {
              return refuse(std::string(kPayloadDiagnostic),
                            rank.diagnostic.detail);
            }
            value = std::move(rank.value);
          } else {
            value.descriptor = descriptor;
            value.state = api::EngineValueState::value;
            value.encoded_value = std::to_string(row + 1);
          }
          output.rows[row].values.push_back(std::move(value));
        }
        if (cume_dist_window &&
            !append_cume_dist_group(peer_begin, output.rows.size())) {
          return refuse(std::string(kPayloadDiagnostic), cume_dist_detail);
        }
        const auto canonical = exec::ValidateCanonicalDescriptorBatch(
            output, node.output_descriptor_ids);
        const auto values = exec::ValidateDescriptorBatch(output);
        const auto peer_comparison_count =
            (peer_ranking_window || last_value_window || nth_value_window) &&
                    input_row_count != 0
                ? input_row_count - 1
                : 0;
        std::uint64_t ranking_work = 0;
        if (!CheckedAdd(input_row_count, peer_comparison_count,
                        &ranking_work) ||
            !canonical.ok || !values.ok || !add_work(ranking_work)) {
          return refuse(
              std::string(kPayloadDiagnostic),
              !canonical.ok
                  ? "composition " + std::string(ranking_name) +
                        " output: " + canonical.detail
                  : (!values.ok
                         ? "composition " + std::string(ranking_name) +
                               " output: " + values.detail
                         : "composition " + std::string(ranking_name) +
                               " work exceeds its admitted bound"));
        }
        state.batch = std::move(output);
        state.result_bindings = input_bindings;
        state.result_bindings.push_back(std::move(ranking_binding));
        if (aggregate_window) {
          const auto* aggregate_row =
              exec::LookupCanonicalAggregateByUuidV1(
                  ranking_profile.function_uuid);
          if (aggregate_row == nullptr || !aggregate_row->executable ||
              !aggregate_row->aggregate_as_window ||
              aggregate_row->builtin_id != ranking_profile.builtin_id ||
              aggregate_row->function_uuid != ranking_profile.function_uuid) {
            return refuse(std::string(kPayloadDiagnostic),
                          "composition " + std::string(ranking_name) +
                              " aggregate registry identity drifted");
          }
          prepared_aggregate_window = std::move(ranking_column);
          prepared_aggregate_window_order_term =
              prepared_sort->order_terms.front();
          prepared_aggregate_window_value_column =
              ranking.navigation_value_column;
          prepared_aggregate_window_descriptor = {
              aggregate_row->abi_version, aggregate_row->function,
              aggregate_row->builtin_id, aggregate_row->function_uuid,
              ranking.aggregate_count_star};
          prepared_aggregate_window_frame_descriptor_uuid =
              ranking.window_frame_descriptor_uuid;
          planning_values_exact = false;
        } else if (value_window) {
          prepared_navigation_window = std::move(ranking_column);
          prepared_navigation_order_term =
              prepared_sort->order_terms.front();
          prepared_navigation_value_column =
              *ranking.navigation_value_column;
          prepared_navigation_nth_value_position_operand =
              ranking.nth_value_position_operand;
          prepared_navigation_frame_descriptor_uuid =
              ranking.window_frame_descriptor_uuid;
          prepared_navigation_profile = ranking_profile;
        } else if (peer_ranking_window) {
          prepared_peer_ranking = std::move(ranking_column);
          prepared_peer_ranking_order_term =
              prepared_sort->order_terms.front();
          prepared_peer_ranking_profile = ranking_profile;
          peer_ranking_maximum_peer_comparisons =
              std::max<std::size_t>(1, peer_comparison_count);
          auxiliary_memory = rank_comparison_workspace_bytes;
        } else if (ntile_window) {
          prepared_ntile = std::move(ranking_column);
          prepared_ntile_order_term = prepared_sort->order_terms.front();
          prepared_ntile_bucket_count_operand =
              *ranking.ntile_bucket_count_operand;
        } else {
          prepared_row_number = std::move(ranking_column);
        }
        row_number_order_evidence_uuid = DerivedCanonicalUuid(
            identity_scope + ":" + prepared_sort->ordering_property_uuid,
            "node-composition.window.deterministic-order");
        if (peer_ranking_window || ntile_window || value_window) {
          std::uint64_t planned_receipt_workspace_bytes = 0;
          std::uint64_t actual_receipt_workspace_bytes = 0;
          if (!exec::PlanCanonicalDescriptorOrderTermBindingDigestWorkspace(
                  prepared_sort->order_terms.front(),
                  prepared_sort->ordering_property_uuid,
                  &planned_receipt_workspace_bytes) ||
              planned_receipt_workspace_bytes >
                  request.optimizer_request.resource.memory_budget_bytes) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                "composition " + std::string(ranking_name) +
                    " order-term receipt exceeds its memory budget");
          }
          const auto order_term_binding_evidence_uuid =
              exec::ComputeCanonicalDescriptorOrderTermBindingDigest(
                  prepared_sort->order_terms.front(),
                  prepared_sort->ordering_property_uuid,
                  planned_receipt_workspace_bytes,
                  &actual_receipt_workspace_bytes);
          if (order_term_binding_evidence_uuid.empty() ||
              actual_receipt_workspace_bytes !=
                  planned_receipt_workspace_bytes) {
            return refuse(
                std::string(kPayloadDiagnostic),
                "composition " + std::string(ranking_name) +
                    " order-term binding receipt failed");
          }
          auxiliary_memory =
              std::max(auxiliary_memory, actual_receipt_workspace_bytes);
          if (value_window) {
            auto& bound_order_evidence =
                aggregate_window
                    ? aggregate_window_order_term_binding_evidence_uuid
                    : navigation_order_term_binding_evidence_uuid;
            auto& bound_frame_evidence =
                aggregate_window
                    ? aggregate_window_frame_property_binding_evidence_uuid
                    : navigation_frame_property_binding_evidence_uuid;
            auto& bound_capability =
                aggregate_window ? aggregate_window_capability_uuid
                                 : navigation_capability_uuid;
            const auto function_bound_identity_scope =
                aggregate_window
                    ? identity_scope + ":" +
                          std::string(ranking_profile.function_uuid)
                    : identity_scope;
            bound_order_evidence = order_term_binding_evidence_uuid;
            bound_frame_evidence = DerivedCanonicalUuid(
                function_bound_identity_scope + ":" +
                    ranking.window_property_uuid + ":" +
                    ranking.window_frame_descriptor_uuid,
                aggregate_window
                    ? "node-composition.window.aggregate-sum.frame-property-binding"
                    : "node-composition.window.frame-property-binding");
            bound_capability = DerivedCanonicalUuid(
                function_bound_identity_scope + ":" +
                    ranking.result_descriptor->descriptor_uuid + ":" +
                    bound_order_evidence + ":" + bound_frame_evidence,
                aggregate_window
                    ? "node-composition.window.aggregate-sum.capability"
                    : first_value_window
                    ? "node-composition.window.first-value.capability"
                    : (last_value_window
                           ? "node-composition.window.last-value.capability"
                           : (nth_value_window
                                  ? "node-composition.window.nth-value.capability"
                                  : (lag_window
                                         ? "node-composition.window.lag.capability"
                                         : "node-composition.window.lead.capability"))));
            if (bound_capability.empty() ||
                bound_capability == bound_order_evidence ||
                bound_capability == bound_frame_evidence) {
              return refuse(
                  std::string(kPayloadDiagnostic),
                  "composition " + std::string(ranking_name) +
                      " capability identity is not independent");
            }
            std::uint64_t navigation_pair_count = 0;
            std::uint64_t navigation_reference_bytes = 0;
            std::uint64_t navigation_matrix_bytes = 0;
            std::uint64_t navigation_metadata_bytes = 0;
            std::uint64_t navigation_copied_row_metadata_bytes = 0;
            std::uint64_t navigation_value_vector_bytes = 0;
            std::uint64_t navigation_partition_index_bytes = 0;
            std::uint64_t navigation_partition_vector_bytes = 0;
            std::uint64_t navigation_workspace_bytes = 0;
            if (!CheckedMultiply(input_row_count, input_row_count,
                                 &navigation_pair_count) ||
                !CheckedMultiply(navigation_pair_count, sizeof(std::size_t),
                                 &navigation_reference_bytes) ||
                !CheckedMultiply(navigation_pair_count, 2,
                                 &navigation_matrix_bytes) ||
                !CheckedMultiply(
                    input_row_count,
                    sizeof(exec::CanonicalWindowRowPeerMetadata) +
                        sizeof(exec::CanonicalWindowEffectiveFrame),
                    &navigation_metadata_bytes) ||
                !CheckedMultiply(
                    input_row_count,
                    sizeof(exec::CanonicalWindowRowPeerMetadata),
                    &navigation_copied_row_metadata_bytes) ||
                !CheckedMultiply(
                    input_row_count,
                    (nth_value_window ? 3 : 2) *
                            sizeof(api::EngineTypedValue) +
                        sizeof(std::uint64_t),
                    &navigation_value_vector_bytes) ||
                !CheckedMultiply(input_row_count, sizeof(std::size_t),
                                 &navigation_partition_index_bytes) ||
                !CheckedMultiply(input_row_count,
                                 sizeof(std::vector<std::size_t>),
                                 &navigation_partition_vector_bytes) ||
                !CheckedAdd(navigation_reference_bytes,
                            navigation_matrix_bytes,
                            &navigation_workspace_bytes) ||
                !CheckedAdd(navigation_workspace_bytes,
                            navigation_metadata_bytes,
                            &navigation_workspace_bytes) ||
                !CheckedAdd(navigation_workspace_bytes,
                            navigation_copied_row_metadata_bytes,
                            &navigation_workspace_bytes) ||
                !CheckedAdd(navigation_workspace_bytes,
                            navigation_value_vector_bytes,
                            &navigation_workspace_bytes) ||
                !CheckedAdd(navigation_workspace_bytes,
                            navigation_partition_index_bytes,
                            &navigation_workspace_bytes) ||
                !CheckedAdd(navigation_workspace_bytes,
                            navigation_partition_vector_bytes,
                            &navigation_workspace_bytes) ||
                !CheckedAdd(
                    navigation_workspace_bytes,
                    sizeof(std::vector<std::vector<std::size_t>>),
                    &navigation_workspace_bytes) ||
                !CheckedAdd(navigation_workspace_bytes,
                            actual_receipt_workspace_bytes,
                            &navigation_workspace_bytes) ||
                !CheckedAdd(navigation_workspace_bytes, input_memory,
                            &navigation_workspace_bytes) ||
                !CheckedAdd(navigation_workspace_bytes, input_memory,
                            &navigation_workspace_bytes) ||
                !CheckedAdd(navigation_workspace_bytes,
                            navigation_value_payload_memory,
                            &navigation_workspace_bytes) ||
                !CheckedAdd(navigation_workspace_bytes,
                            nth_position_operand_memory,
                            &navigation_workspace_bytes) ||
                !CheckedAdd(navigation_workspace_bytes,
                            nth_position_vector_payload_memory,
                            &navigation_workspace_bytes) ||
                !CheckedAdd(navigation_workspace_bytes,
                            frame_value_comparison_workspace_bytes,
                            &navigation_workspace_bytes) ||
                navigation_pair_count >
                    std::numeric_limits<std::size_t>::max()) {
              return refuse(
                  "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "composition " + std::string(ranking_name) +
                      " navigation workspace overflowed");
            }
            if (aggregate_window) {
              std::uint64_t aggregate_extra_reference_bytes = 0;
              std::uint64_t aggregate_extra_metadata_bytes = 0;
              std::uint64_t aggregate_extra_value_vector_bytes = 0;
              std::uint64_t aggregate_extra_partition_vector_bytes = 0;
              std::uint64_t aggregate_extra_input_bytes = 0;
              constexpr std::uint64_t kAdditionalFrameGraphCopies = 4;
              if (!CheckedMultiply(
                      navigation_pair_count,
                      kAdditionalFrameGraphCopies * sizeof(std::size_t),
                      &aggregate_extra_reference_bytes) ||
                  !CheckedMultiply(
                      input_row_count,
                      2 * sizeof(exec::CanonicalWindowRowPeerMetadata) +
                          3 * sizeof(exec::CanonicalWindowEffectiveFrame),
                      &aggregate_extra_metadata_bytes) ||
                  !CheckedMultiply(input_row_count,
                                   sizeof(api::EngineTypedValue),
                                   &aggregate_extra_value_vector_bytes) ||
                  !CheckedMultiply(
                      input_row_count,
                      kAdditionalFrameGraphCopies *
                          sizeof(std::vector<std::size_t>),
                      &aggregate_extra_partition_vector_bytes) ||
                  !CheckedMultiply(input_memory, 2,
                                   &aggregate_extra_input_bytes) ||
                  !CheckedAdd(navigation_workspace_bytes,
                              aggregate_extra_reference_bytes,
                              &navigation_workspace_bytes) ||
                  !CheckedAdd(navigation_workspace_bytes,
                              aggregate_extra_metadata_bytes,
                              &navigation_workspace_bytes) ||
                  !CheckedAdd(navigation_workspace_bytes,
                              aggregate_extra_value_vector_bytes,
                              &navigation_workspace_bytes) ||
                  !CheckedAdd(navigation_workspace_bytes,
                              aggregate_extra_partition_vector_bytes,
                              &navigation_workspace_bytes) ||
                  !CheckedAdd(
                      navigation_workspace_bytes,
                      kAdditionalFrameGraphCopies *
                          sizeof(std::vector<std::vector<std::size_t>>),
                      &navigation_workspace_bytes) ||
                  !CheckedAdd(navigation_workspace_bytes,
                              aggregate_extra_input_bytes,
                              &navigation_workspace_bytes)) {
                return refuse(
                    "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "composition " + std::string(ranking_name) +
                        " retained frame-carrier workspace overflowed");
              }
            }
            std::uint64_t navigation_work = 0;
            if (!CheckedMultiply(navigation_pair_count, 2,
                                 &navigation_work) ||
                !add_work(navigation_work)) {
              return refuse(
                  "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "composition " + std::string(ranking_name) +
                      " navigation work exceeds its admitted bound");
            }
            auto& maximum_pair_comparisons =
                aggregate_window
                    ? aggregate_window_maximum_pair_comparisons
                    : navigation_maximum_pair_comparisons;
            auto& maximum_effective_row_references =
                aggregate_window
                    ? aggregate_window_maximum_effective_row_references
                    : navigation_maximum_effective_row_references;
            maximum_pair_comparisons = std::max<std::size_t>(
                1, static_cast<std::size_t>(navigation_pair_count));
            maximum_effective_row_references = maximum_pair_comparisons;
            if (aggregate_window) {
              aggregate_window_maximum_transition_count =
                  maximum_effective_row_references;
            }
            auxiliary_memory =
                std::max(auxiliary_memory, navigation_workspace_bytes);
          } else if (ntile_window) {
            std::uint64_t operand_memory_bytes = 0;
            if (!RuntimeTypedValueMemoryBytes(
                    *ranking.ntile_bucket_count_operand,
                    &operand_memory_bytes) ||
                !CheckedAdd(auxiliary_memory, operand_memory_bytes,
                            &auxiliary_memory)) {
              return refuse(
                  "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "composition NTILE operand receipt size overflowed");
            }
            ntile_order_term_binding_evidence_uuid =
                order_term_binding_evidence_uuid;
          } else {
            peer_ranking_order_term_binding_evidence_uuid =
                order_term_binding_evidence_uuid;
          }
          if (real_ranking_window) {
            auxiliary_memory = std::max(
                auxiliary_memory,
                kRealRankingConversionWorkspaceMaximumBytes);
          }
          std::uint64_t real_ranking_payload_reserve = 0;
          if (real_ranking_window &&
              (!CheckedMultiply(input_row_count,
                                kRealRankingRatioTextMaximumBytes,
                                &real_ranking_payload_reserve) ||
               !CheckedAdd(auxiliary_memory, real_ranking_payload_reserve,
                           &auxiliary_memory))) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition " + std::string(ranking_name) +
                    " payload reserve overflowed");
          }
        }
        implementation_id =
            aggregate_window
                ? "window.aggregate-registry-frame-recompute.v1"
                : std::string(ranking_profile.semantic_variant_id);
        capability_uuid =
            aggregate_window
                ? aggregate_window_capability_uuid
                : value_window
                ? navigation_capability_uuid
                : (ntile_window
                ? ntile_order_term_binding_evidence_uuid
                : (peer_ranking_window
                       ? peer_ranking_order_term_binding_evidence_uuid
                       : window_capability_uuid));
        transformation_rule =
            aggregate_window
                ? "canonical.window.composed-aggregate-registry-frame-recompute.v1"
                : value_window
                ? (first_value_window
                       ? "canonical.window.composed-first-value.v1"
                       : (last_value_window
                              ? "canonical.window.composed-last-value.v1"
                              : (nth_value_window
                                     ? "canonical.window.composed-nth-value.v1"
                                     : (lag_window
                                            ? "canonical.window.composed-lag.v1"
                                            : "canonical.window.composed-lead.v1"))))
                : (ntile_window
                ? "canonical.window.composed-ntile.v1"
                : (cume_dist_window
                ? "canonical.window.composed-cume-dist.v1"
                : (percent_rank_window
                       ? "canonical.window.composed-percent-rank.v1"
                       : (dense_rank_window
                              ? "canonical.window.composed-dense-rank.v1"
                              : (rank_window
                                     ? "canonical.window.composed-rank.v1"
                                     : "canonical.window.composed-row-number.v1")))));
        physical_kind = exec::PhysicalNodeKind::kWindow;
        required_property_uuids = node.required_property_uuids;
        delivered_property_uuids = node.delivered_property_uuids;
        property_kinds = {
            plan::CanonicalLogicalPropertyKind::kOrdering,
            plan::CanonicalLogicalPropertyKind::kWindow};
        break;
      }
      case plan::CanonicalLogicalRelationalNodeKind::kSubquery: {
        const auto predicate_profile =
            MatchLivePredicateSubqueryProfileForComposition(node.semantic_variant_id);
        const bool predicate_subquery = predicate_profile.matched;
        if ((!predicate_subquery &&
             node.output_descriptor_ids !=
                 input_node.output_descriptor_ids) ||
            state.result_bindings.size() != input_batch.columns.size()) {
          return refuse(
              std::string(kPayloadDiagnostic),
              predicate_subquery
                  ? "predicate subquery input schema is incomplete"
                  : "subquery does not preserve its bound input schema");
        }
        const auto validated = exec::ValidateCanonicalDescriptorBatch(
            input_batch, input_node.output_descriptor_ids);
        if (!validated.ok) {
          return refuse(std::string(kPayloadDiagnostic),
                        "subquery input: " + validated.detail);
        }
        const auto materialization_work =
            std::max<std::size_t>(1, input_row_count);
        if (!add_work(materialization_work)) {
          return refuse(
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
              "composition subquery materialization exhausted its "
              "admitted bound");
        }
        auxiliary_memory = input_memory;
        capability_uuid = subquery_capability_uuid;
        physical_kind = exec::PhysicalNodeKind::kSubquery;
        if (predicate_subquery) {
          auto result_node = node;
          result_node.bound_expression_ids = {
              node.bound_expression_ids.front()};
          auto projected = PrepareExpressionProjectRootForComposition(
              request.relational_dag, result_node, input_node, state,
              request.expression_services);
          if (!projected.ok ||
              projected.expression_output_batch.columns.size() != 1 ||
              projected.result_bindings.size() != 1) {
            return refuse(
                std::string(kPayloadDiagnostic),
                projected.detail.empty()
                    ? "predicate subquery result binding is incomplete"
                    : projected.detail);
          }

          LivePredicateSubqueryRegistrationProfile prepared;
          prepared.kind = predicate_profile.kind;
          prepared.result_column =
              projected.expression_output_batch.columns.front();
          prepared.implementation_id =
              predicate_profile.implementation_id;
          prepared.transformation_id =
              predicate_profile.transformation_id;
          const bool exists =
              prepared.kind == LivePredicateSubqueryKind::kExists;
          const auto canonical_boolean_type_uuid =
              ExactCanonicalCoreDatatypeUuidV1("boolean");
          if (prepared.result_column.descriptor.canonical_type_name !=
                  "boolean" ||
              prepared.result_column.nullable == exists ||
              canonical_boolean_type_uuid.empty() ||
              !CanonicalDescriptorFieldEqualsForComposition(
                  prepared.result_column.descriptor, "type_uuid",
                  std::string_view(canonical_boolean_type_uuid)) ||
              prepared.result_column.descriptor.descriptor_uuid ==
                  canonical_boolean_type_uuid) {
            return refuse(
                std::string(kPayloadDiagnostic),
                exists
                    ? "EXISTS result is not a bound non-null boolean"
                    : "quantified result is not a bound nullable boolean");
          }

          api::EngineSqlTruthValue truth =
              exists
                  ? (input_row_count == 0
                         ? api::EngineSqlTruthValue::false_value
                         : api::EngineSqlTruthValue::true_value)
                  : api::EngineSqlTruthValue::unspecified;
          if (!exists) {
            if (input_batch.columns.size() != 1) {
              return refuse(
                  std::string(kPayloadDiagnostic),
                  "quantified subquery requires one bound scalar column");
            }
            const auto& right_operand_type =
                input_batch.columns.front().descriptor.canonical_type_name;
            if (right_operand_type.empty() ||
                (!predicate_profile.required_operand_type.empty() &&
                 right_operand_type !=
                     predicate_profile.required_operand_type)) {
              return refuse(
                  std::string(kPayloadDiagnostic),
                  "quantified subquery operand type does not match its "
                  "bound profile");
            }
            const auto left_expression_id =
                node.bound_expression_ids[1];
            const auto left_expression = std::ranges::find_if(
                request.relational_dag.expressions,
                [&](const auto& candidate) {
                  return candidate.expression_id == left_expression_id;
                });
            const auto left_descriptor =
                left_expression == request.relational_dag.expressions.end()
                    ? request.relational_dag.descriptors.end()
                    : std::ranges::find_if(
                          request.relational_dag.descriptors,
                          [&](const auto& candidate) {
                            return candidate.descriptor_id ==
                                   left_expression->result_descriptor_id;
                          });
            CanonicalRelationalExpressionRowBinding left_binding;
            std::string detail;
            if (left_expression ==
                    request.relational_dag.expressions.end() ||
                left_descriptor ==
                    request.relational_dag.descriptors.end() ||
                !PrepareInputRowBindingForComposition(
                    request.relational_dag, left_expression_id,
                    input_node.output_descriptor_ids, &left_binding,
                    &detail) ||
                !left_binding.slots.empty()) {
              return refuse(
                  std::string(kPayloadDiagnostic),
                  detail.empty()
                      ? "quantified left operand is not an independent "
                        "bound expression"
                      : detail);
            }
            std::vector<api::EngineTypedValue> descriptor_only_input;
            if (input_batch.rows.empty()) {
              descriptor_only_input.reserve(input_batch.columns.size());
              for (const auto& column : input_batch.columns) {
                api::EngineTypedValue value;
                value.descriptor = column.descriptor;
                value.state = api::EngineValueState::value;
                descriptor_only_input.push_back(std::move(value));
              }
            }
            const auto& input_row =
                input_batch.rows.empty()
                    ? descriptor_only_input
                    : input_batch.rows.front().values;
            if (!expression_runtime.EvaluateForConsumer(
                    left_expression_id, right_operand_type, left_binding,
                    input_row,
                    api::EngineCanonicalExpressionConsumer::subquery,
                    &prepared.left_value, &detail)) {
              return refuse(
                  std::string(kPayloadDiagnostic),
                  "quantified left operand: " + detail);
            }
            if (prepared.left_value.descriptor.canonical_type_name !=
                right_operand_type) {
              return refuse(
                  std::string(kPayloadDiagnostic),
                  "quantified comparison operands are not "
                  "descriptor-compatible");
            }
            prepared.left_operand_column = {
                "quantified_left", prepared.left_value.descriptor,
                left_descriptor->nullability ==
                    api::RelationalNullability::kNullable,
                left_descriptor->descriptor_id};
            prepared.right_expression_descriptor_id =
                input_batch.columns.front().descriptor_id;
            prepared.comparison_operator =
                predicate_profile.comparison_operator;
            prepared.quantifier = predicate_profile.quantifier;
            std::uint64_t left_operand_memory = 0;
            if (!RuntimeTypedValueMemoryBytes(
                    prepared.left_value, &left_operand_memory) ||
                !CheckedAdd(auxiliary_memory, left_operand_memory,
                            &auxiliary_memory)) {
              return refuse(
                  "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "quantified left operand receipt size overflowed");
            }
            prepared.comparison_authority_required =
                CanonicalRelationalComparisonAuthorityRequiredV1(
                    prepared.left_value.descriptor,
                    input_batch.columns.front().descriptor);

            if (prepared.comparison_authority_required) {
              std::uint64_t comparison_authority_memory = 0;
              if (!CheckedMultiply(
                      static_cast<std::uint64_t>(input_row_count),
                      sizeof(std::optional<int>),
                      &comparison_authority_memory) ||
                  !CheckedAdd(auxiliary_memory,
                              comparison_authority_memory,
                              &auxiliary_memory)) {
                return refuse(
                    "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "quantified comparison authority carrier size "
                    "overflowed");
              }
            }

            api::EngineCanonicalExpressionOperation operation =
                api::EngineCanonicalExpressionOperation::equal;
            using Comparison = api::EngineComparisonPredicateOperator;
            if (prepared.comparison_operator == Comparison::not_equal) {
              operation =
                  api::EngineCanonicalExpressionOperation::not_equal;
            } else if (prepared.comparison_operator ==
                       Comparison::less_than) {
              operation = api::EngineCanonicalExpressionOperation::less_than;
            } else if (prepared.comparison_operator ==
                       Comparison::less_than_or_equal) {
              operation =
                  api::EngineCanonicalExpressionOperation::less_than_or_equal;
            } else if (prepared.comparison_operator ==
                       Comparison::greater_than) {
              operation =
                  api::EngineCanonicalExpressionOperation::greater_than;
            } else if (prepared.comparison_operator ==
                       Comparison::greater_than_or_equal) {
              operation = api::EngineCanonicalExpressionOperation::
                  greater_than_or_equal;
            }
            const bool any =
                prepared.quantifier ==
                exec::CanonicalQuantifiedSubqueryQuantifier::kAny;
            truth = any ? api::EngineSqlTruthValue::false_value
                        : api::EngineSqlTruthValue::true_value;
            for (const auto& row : input_batch.rows) {
              api::EngineCanonicalExpressionEvaluationRequest evaluation;
              evaluation.consumer =
                  api::EngineCanonicalExpressionConsumer::subquery;
              evaluation.operation = operation;
              evaluation.left_value = prepared.left_value;
              evaluation.right_value = row.values.front();
              evaluation.result_descriptor =
                  prepared.result_column.descriptor;
              api::EngineCanonicalExpressionEvaluationResult evaluated;
              if (!BindCanonicalRelationalComparisonAuthorityV1(
                      evaluation.left_value, evaluation.right_value,
                      request.expression_services,
                      &evaluation.precomputed_comparison, &detail) ||
                  !api::QowEvaluateCanonicalTypedExpressionV1(
                      evaluation, &evaluated, &detail)) {
                return refuse(
                    std::string(kPayloadDiagnostic),
                    "quantified comparison: " + detail);
              }
              if (any) {
                if (evaluated.truth ==
                    api::EngineSqlTruthValue::true_value) {
                  truth = api::EngineSqlTruthValue::true_value;
                } else if (evaluated.truth ==
                               api::EngineSqlTruthValue::unknown &&
                           truth ==
                               api::EngineSqlTruthValue::false_value) {
                  truth = api::EngineSqlTruthValue::unknown;
                }
              } else if (evaluated.truth ==
                         api::EngineSqlTruthValue::false_value) {
                truth = api::EngineSqlTruthValue::false_value;
              } else if (evaluated.truth ==
                             api::EngineSqlTruthValue::unknown &&
                         truth ==
                             api::EngineSqlTruthValue::true_value) {
                truth = api::EngineSqlTruthValue::unknown;
              }
            }
            if (!add_work(input_row_count)) {
              return refuse(
                  "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "composition quantified comparisons exhausted their "
                  "admitted bound");
            }
          }

          api::EngineCanonicalExpressionEvaluationRequest truth_request;
          truth_request.consumer =
              api::EngineCanonicalExpressionConsumer::subquery;
          truth_request.operation =
              api::EngineCanonicalExpressionOperation::consume_truth;
          truth_request.input_truth = truth;
          truth_request.result_descriptor =
              prepared.result_column.descriptor;
          api::EngineCanonicalExpressionEvaluationResult truth_result;
          std::string truth_detail;
          if (!api::QowEvaluateCanonicalTypedExpressionV1(
                  truth_request, &truth_result, &truth_detail)) {
            return refuse(
                std::string(kPayloadDiagnostic),
                "predicate subquery truth publication: " + truth_detail);
          }
          exec::DescriptorBatch output;
          output.columns = {prepared.result_column};
          output.rows = {{{std::move(truth_result.value)}}};
          const auto output_validated =
              exec::ValidateCanonicalDescriptorBatch(
                  output, node.output_descriptor_ids);
          if (!output_validated.ok) {
            return refuse(
                std::string(kPayloadDiagnostic),
                "predicate subquery output: " + output_validated.detail);
          }
          predicate_subquery_input_row_count = input_row_count;
          prepared_predicate_subquery = std::move(prepared);
          state.batch = std::move(output);
          state.result_bindings = std::move(projected.result_bindings);
          implementation_id =
              prepared_predicate_subquery->implementation_id;
          transformation_rule =
              prepared_predicate_subquery->transformation_id;
          break;
        }
        if (node.semantic_variant_id == "subquery.table.v1") {
          prepared_table_subquery = true;
          table_subquery_input_row_count = input_row_count;
          auxiliary_memory = 0;
          implementation_id = "subquery.table.materialize.typed.v1";
          transformation_rule =
              "canonical.subquery.composed-table-materialize.v1";
          break;
        }
        const bool scalar =
            node.semantic_variant_id == "subquery.scalar.v1";
        if ((scalar && input_batch.columns.size() != 1) ||
            input_batch.columns.empty()) {
          return refuse(
              std::string(kPayloadDiagnostic),
              "cardinality subquery result width is invalid");
        }
        if (input_row_count > 1) {
          return refuse(
              std::string(kPayloadDiagnostic),
              scalar ? "scalar subquery produced more than one row"
                     : "row subquery produced more than one row");
        }
        exec::DescriptorBatch output;
        output.columns = input_batch.columns;
        auto result_bindings = state.result_bindings;
        std::unordered_set<std::string> cardinality_result_identity_domain;
        if (!CanonicalUuidText(subquery_capability_uuid)) {
          return refuse(
              std::string(kPayloadDiagnostic),
              "cardinality subquery capability identity is unresolved");
        }
        cardinality_result_identity_domain.insert(subquery_capability_uuid);
        for (const auto& source_column : input_batch.columns) {
          const auto source_type_uuid = ExactEncodedDescriptorField(
              source_column.descriptor.encoded_descriptor, "type_uuid");
          if (!CanonicalUuidText(
                  source_column.descriptor.descriptor_uuid) ||
              !source_type_uuid.has_value() ||
              !CanonicalUuidText(*source_type_uuid)) {
            return refuse(
                std::string(kPayloadDiagnostic),
                "cardinality subquery source identity domain is unresolved");
          }
          cardinality_result_identity_domain.insert(
              source_column.descriptor.descriptor_uuid);
          cardinality_result_identity_domain.insert(*source_type_uuid);
        }
        for (std::size_t column = 0; column < output.columns.size(); ++column) {
          if (!output.columns[column].nullable) {
            output.columns[column].nullable = true;
            if (!exec::DeriveCanonicalNullableDescriptorEncoding(
                    &output.columns[column].descriptor)) {
              return refuse(
                  std::string(kPayloadDiagnostic),
                  "cardinality subquery result lacks a nullable descriptor carrier");
            }
          }
          const auto result_descriptor_uuid = DerivedCanonicalUuid(
              identity_scope,
              "composition.subquery.cardinality.result." +
                  std::to_string(node.logical_node_id) + "." +
                  node.semantic_variant_id + "." + std::to_string(column) +
                  "." +
                  input_batch.columns[column]
                      .descriptor.descriptor_uuid);
          if (!CanonicalUuidText(result_descriptor_uuid) ||
              !cardinality_result_identity_domain
                   .insert(result_descriptor_uuid)
                   .second) {
            return refuse(
                std::string(kPayloadDiagnostic),
                "cardinality subquery result descriptor identity collides with its bound role domain");
          }
          output.columns[column].descriptor.descriptor_uuid =
              result_descriptor_uuid;
          if (result_bindings[column].visible) {
            if (!result_bindings[column].published_descriptor.has_value()) {
              return refuse(
                  std::string(kPayloadDiagnostic),
                  "cardinality subquery visible result binding is unresolved");
            }
            result_bindings[column].published_descriptor->descriptor_uuid =
                result_descriptor_uuid;
            result_bindings[column].published_descriptor->nullability =
                exec::CanonicalResultNullability::kNullable;
          }
        }
        if (input_row_count == 1) {
          auto row = input_batch.rows.front();
          for (std::size_t column = 0; column < row.values.size(); ++column) {
            row.values[column].descriptor = output.columns[column].descriptor;
          }
          output.rows.push_back(std::move(row));
        } else {
          exec::DescriptorTuple row;
          row.values.reserve(output.columns.size());
          for (const auto& column : output.columns) {
            api::EngineTypedValue value;
            value.descriptor = column.descriptor;
            value.is_null = true;
            value.state = api::EngineValueState::sql_null;
            row.values.push_back(std::move(value));
          }
          output.rows.push_back(std::move(row));
        }
        const auto output_validated =
            exec::ValidateCanonicalDescriptorBatch(
                output, node.output_descriptor_ids);
        if (!output_validated.ok) {
          return refuse(std::string(kPayloadDiagnostic),
                        "cardinality subquery output: " +
                            output_validated.detail);
        }
        LiveCardinalitySubqueryRegistrationProfile prepared;
        prepared.kind = scalar ? LiveCardinalitySubqueryKind::kScalar
                               : LiveCardinalitySubqueryKind::kRow;
        prepared.result_columns = output.columns;
        prepared.implementation_id =
            scalar ? "subquery.scalar.cardinality.typed.v1"
                   : "subquery.row.cardinality.typed.v1";
        cardinality_subquery_input_row_count = input_row_count;
        prepared_cardinality_subquery = std::move(prepared);
        state.batch = std::move(output);
        state.result_bindings = std::move(result_bindings);
        implementation_id =
            prepared_cardinality_subquery->implementation_id;
        transformation_rule =
            scalar ? "canonical.subquery.composed-scalar-cardinality.v1"
                   : "canonical.subquery.composed-row-cardinality.v1";
        break;
      }
      case plan::CanonicalLogicalRelationalNodeKind::kCte: {
        if (node.output_descriptor_ids != input_node.output_descriptor_ids ||
            state.result_bindings.size() != input_batch.columns.size()) {
          return refuse(
              std::string(kPayloadDiagnostic),
              "nonrecursive CTE does not preserve its bound input schema");
        }
        const auto validated = exec::ValidateCanonicalDescriptorBatch(
            input_batch, node.output_descriptor_ids);
        if (!validated.ok) {
          return refuse(std::string(kPayloadDiagnostic),
                        "nonrecursive CTE input: " + validated.detail);
        }
        const auto carriage_work =
            std::max<std::size_t>(1, input_row_count);
        if (!add_work(carriage_work)) {
          return refuse(
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
              "composition nonrecursive CTE carriage exhausted its "
              "admitted bound");
        }
        prepared_nonrecursive_cte = true;
        nonrecursive_cte_input_row_count = input_row_count;
        auxiliary_memory = node.shareable ? input_memory : 0;
        nonrecursive_cte_implementation_id =
            node.shareable ? "cte.bound.materialize.typed.v1"
                           : "cte.bound.inline.typed.v1";
        implementation_id = nonrecursive_cte_implementation_id;
        capability_uuid = cte_capability_uuid;
        transformation_rule =
            node.shareable ? "canonical.cte.composed-materialize.v1"
                           : "canonical.cte.composed-inline.v1";
        physical_kind = exec::PhysicalNodeKind::kCte;
        break;
      }
      case plan::CanonicalLogicalRelationalNodeKind::kLimit: {
        auto prepared = PrepareLimitRootForComposition(request.relational_dag, node,
                                         input_node, state);
        if (!prepared.ok) {
          return refuse(std::string(kPayloadDiagnostic), prepared.detail);
        }
        fetch_first_rows_only =
            node.semantic_variant_id ==
            "fetch.first-rows-only-offset.v1";
        const bool has_offset = node.bound_expression_ids.size() == 2;
        const auto expected_arity =
            node.semantic_variant_id == "limit.bound-count.v1" ? 1U : 2U;
        if (node.bound_expression_ids.size() != expected_arity) {
          return refuse(std::string(kPayloadDiagnostic),
                        "LIMIT/FETCH bound arity is not exact");
        }
        std::string detail;
        if (!EvaluateNonNegativeRowBoundForComposition(
                &expression_runtime, node.bound_expression_ids.front(),
                &row_limit, &detail) ||
            (has_offset &&
             !EvaluateNonNegativeRowBoundForComposition(
                 &expression_runtime, node.bound_expression_ids[1],
                 &row_offset, &detail))) {
          return refuse(std::string(kPayloadDiagnostic),
                        "LIMIT/FETCH bound: " + detail);
        }
        const auto offset = row_offset > input_row_count
                                ? input_row_count
                                : static_cast<std::size_t>(row_offset);
        const auto remaining = input_row_count - offset;
        const auto count = row_limit > remaining
                               ? remaining
                               : static_cast<std::size_t>(row_limit);
        exec::DescriptorBatch output;
        output.columns = input_batch.columns;
        output.rows.reserve(count);
        for (std::size_t row = 0; row < count; ++row) {
          output.rows.push_back(input_batch.rows[offset + row]);
        }
        if (!add_work(node.bound_expression_ids.size())) {
          return refuse(
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
              "composition LIMIT/FETCH work exceeds the admitted bound");
        }
        prepared_limit = std::move(prepared);
        limit_input_row_count = input_row_count;
        state.batch = std::move(output);
        state.result_bindings = prepared_limit->result_bindings;
        limit_implementation_id = fetch_first_rows_only
            ? "fetch.native.rows-only.v1"
            : "limit.typed.v1";
        implementation_id = limit_implementation_id;
        capability_uuid = limit_capability_uuid;
        transformation_rule = fetch_first_rows_only
            ? "canonical.fetch.composed-first-rows-only-offset.v1"
            : "canonical.limit.composed-bound-count-offset.v1";
        physical_kind = exec::PhysicalNodeKind::kLimit;
        break;
      }
      default:
        return refuse(std::string(kPayloadDiagnostic),
                      "composition node kind changed after shape admission");
    }

    std::uint64_t output_memory = 1;
    std::uint64_t operator_memory = 0;
    std::uint64_t aggregate_workspace_memory = 0;
    const bool query_distinct =
        physical_kind == exec::PhysicalNodeKind::kAggregate &&
        implementation_id == "aggregate.query-distinct.typed.v1";
    const bool count_star_aggregate =
        implementation_id == "aggregate.count-star.v1";
    const bool grouped_registry_aggregate =
        implementation_id == "aggregate.registry-grouping-sets.v1";
    const bool exact_registry_aggregate =
        implementation_id == "aggregate.registry-core.v1" &&
        planning_values_exact;
    const bool dynamic_registry_aggregate =
        implementation_id == "aggregate.registry-core.v1" &&
        !planning_values_exact;
    if (physical_kind == exec::PhysicalNodeKind::kAggregate &&
        !query_distinct && !count_star_aggregate &&
        !grouped_registry_aggregate &&
        !exact_registry_aggregate &&
        !dynamic_registry_aggregate &&
        (!CheckedMultiply(
             input_row_count,
             sizeof(std::size_t), &aggregate_workspace_memory) ||
         !CheckedAdd(aggregate_workspace_memory,
                     kCanonicalAggregateKernelBaseMemoryBytes,
                     &aggregate_workspace_memory))) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "composition aggregate workspace size overflowed");
    }
    if (!AddBatchMemoryBytes(state.batch, &output_memory)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "composition output memory overflowed");
    }
    if (dynamic_registry_aggregate || grouped_registry_aggregate) {
      operator_memory =
          request.optimizer_request.resource.memory_budget_bytes;
    } else if (exact_registry_aggregate) {
      std::uint64_t aggregate_output_phase_memory = 0;
      if (!CheckedAdd(output_memory,
                      kCanonicalAggregateKernelBaseMemoryBytes,
                      &aggregate_output_phase_memory) ||
          !CheckedAdd(input_memory, auxiliary_memory, &operator_memory) ||
          !CheckedAdd(operator_memory,
                      std::max(registry_aggregate_distinct_peak_memory,
                               aggregate_output_phase_memory),
                      &operator_memory)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "composition registry aggregate memory overflowed");
      }
    } else if (!CheckedAdd(input_memory, output_memory, &operator_memory) ||
               !CheckedAdd(operator_memory, auxiliary_memory,
                           &operator_memory) ||
               (physical_kind == exec::PhysicalNodeKind::kAggregate &&
                !query_distinct &&
                !CheckedAdd(operator_memory,
                            aggregate_workspace_memory,
                            &operator_memory))) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "composition node memory overflowed");
    }
    if (operator_memory >
        request.optimizer_request.resource.memory_budget_bytes) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                    "composition node exceeds the admitted memory budget");
    }
    std::uint64_t runtime_accounted_auxiliary_memory = auxiliary_memory;
    if (implementation_id == "aggregate.registry-core.v1" &&
        !exact_registry_aggregate && !dynamic_registry_aggregate &&
        !CheckedAdd(runtime_accounted_auxiliary_memory,
                    aggregate_workspace_memory,
                    &runtime_accounted_auxiliary_memory)) {
      return refuse(
          "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
          "composition aggregate runtime auxiliary receipt overflowed");
    }
    const bool dynamic_subquery =
        physical_kind == exec::PhysicalNodeKind::kSubquery;
    const bool subquery_retains_intermediate_table =
        dynamic_subquery &&
        implementation_id != "subquery.table.materialize.typed.v1";
    const bool dynamic_nonrecursive_cte =
        physical_kind == exec::PhysicalNodeKind::kCte;
    const bool strict_grouped_filter =
        physical_kind == exec::PhysicalNodeKind::kFilter &&
        prepared_grouped_aggregate.has_value();
    if (strict_grouped_filter ||
        ((physical_kind == exec::PhysicalNodeKind::kFilter ||
         physical_kind == exec::PhysicalNodeKind::kProject ||
         physical_kind == exec::PhysicalNodeKind::kSort ||
         physical_kind == exec::PhysicalNodeKind::kWindow ||
         physical_kind == exec::PhysicalNodeKind::kLimit ||
         dynamic_subquery || dynamic_nonrecursive_cte ||
         query_distinct || count_star_aggregate) &&
         !planning_values_exact)) {
      // The preceding complex aggregate owns its live result values.  The
      // planning batch is only a schema/cardinality placeholder, so bind the
      // dynamic FILTER, PROJECT, DISTINCT, COUNT(*), SORT, WINDOW, LIMIT,
      // SUBQUERY, or nonrecursive CTE to the full selected budget and let its
      // issuer/executor charge exact callback payloads and auxiliary state.
      // A grouped FILTER also needs the full strict callback envelope even
      // when its small planning batch is exact: the live row-bound expression
      // validates the operator-local DAG and materialized aggregate slots.
      operator_memory =
          request.optimizer_request.resource.memory_budget_bytes;
    }
    if (!planning_values_exact && subquery_retains_intermediate_table) {
      if (runtime_accounted_auxiliary_memory < input_memory) {
        return refuse(
            "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
            "dynamic subquery auxiliary receipt is incomplete");
      }
      // The planning input is only a placeholder. Carry fixed state here;
      // the runtime receipt reconstructs the exact intermediate-table bytes
      // from the first dispatch input.
      runtime_accounted_auxiliary_memory -= input_memory;
    }
    profiles.push_back(
        {node.logical_node_id, implementation_id, capability_uuid,
         node.node_kind, physical_kind, transformation_rule,
         state.batch.rows.size(), operator_memory, 1, 1,
         std::move(required_property_uuids),
         std::move(delivered_property_uuids), std::move(property_kinds)});
    profiles.back().runtime_accounted_auxiliary_memory_bytes =
        runtime_accounted_auxiliary_memory;
    profiles.back().runtime_peak_from_callback_batches =
        grouped_registry_aggregate || !planning_values_exact;
    profiles.back().runtime_auxiliary_from_first_input_batch =
        (!planning_values_exact && subquery_retains_intermediate_table) ||
        (dynamic_nonrecursive_cte &&
         implementation_id == "cte.bound.materialize.typed.v1");
  }

  if (!CompleteLiveRuntimeMemoryReceipts(&profiles)) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "composition runtime memory receipts are incomplete");
  }
  auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "node-composition.selected-plan",
      "node-driven composition");
  if (!planning.ok) return refuse(planning.diagnostic_id, planning.detail);
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  if (join_kind.has_value() || lateral_subquery_profile.matched ||
      correlated_subquery_base) {
    values_batches.emplace(join_left_node->logical_node_id,
                           std::move(join_left_values->batch));
    values_batches.emplace(join_right_node->logical_node_id,
                           std::move(join_right_values->batch));
  } else if (!set_base_nodes.empty()) {
    for (auto& [node_id, materialized] : set_materialized_values) {
      const auto base_node = set_base_nodes.at(node_id);
      if (base_node->node_kind ==
          plan::CanonicalLogicalRelationalNodeKind::kValues) {
        values_batches.emplace(node_id, std::move(materialized.batch));
      }
    }
  } else if (recursive_cte_base) {
    auto anchor = MaterializeValues(
        request.relational_dag, *recursive_anchor_node,
        request.expression_services);
    if (!anchor.ok) {
      return refuse(std::string(kPayloadDiagnostic),
                    "recursive CTE anchor replay: " + anchor.detail);
    }
    values_batches.emplace(recursive_anchor_node->logical_node_id,
                           std::move(anchor.batch));
  } else {
    auto values = MaterializeValues(request.relational_dag,
                                    *reverse_chain.front(),
                                    request.expression_services);
    if (!values.ok) {
      return refuse(std::string(kPayloadDiagnostic),
                    "composition VALUES replay: " + values.detail);
    }
    values_batches.emplace(reverse_chain.front()->logical_node_id,
                           std::move(values.batch));
  }

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority = BuildCanonicalExecutionMgaAuthority(
      request.context, planning.physical_dag);
  execution_request.selected_physical_dag =
      std::move(planning.physical_dag);
  execution_request.available_executors.push_back(
      MakeLiveValuesRegistration(
          std::move(values_batches), values_capability_uuid,
          "QOW-DIAG-RELATIONAL-LIVE-NODE-COMPOSITION-VALUES-V1",
          "node-driven composition"));
  if (prepared_join.has_value()) {
    execution_request.available_executors.push_back(
        MakeLiveJoinRegistration(
            join_implementation_id, join_capability_uuid,
            std::move(join_truth_values), join_pair_count,
            join_output_row_bound, *join_kind, join_operation_name,
            request.context));
  }
  if (prepared_correlated_subquery.has_value()) {
    execution_request.available_executors.push_back(
        MakeLiveCorrelatedSubqueryRegistration(
            *prepared_correlated_subquery, subquery_capability_uuid,
            request.expression_services, request.context));
  }
  if (prepared_lateral_subquery.has_value()) {
    execution_request.available_executors.push_back(
        MakeLiveLateralSubqueryRegistration(
            *prepared_lateral_subquery, lateral_subquery_profile,
            join_capability_uuid, request.expression_services,
            request.context));
  }
  std::unordered_set<std::string> registered_set_implementations;
  for (const auto& [node_id, prepared] : prepared_set_nodes) {
    (void)node_id;
    if (!registered_set_implementations
             .insert(prepared.profile.implementation_id)
             .second) {
      continue;
    }
    execution_request.available_executors.push_back(
        MakeLiveSetOperationRegistration(
            MakeLiveSetRegistrationProfilesForComposition(prepared_set_nodes),
            prepared.profile.implementation_id,
            set_capability_uuids.at(prepared.profile.implementation_id),
            request.context));
  }
  if (prepared_filter.has_value()) {
    if (prepared_grouped_aggregate.has_value()) {
      execution_request.available_executors.push_back(
          MakeLiveHeapFilterRegistration(
              prepared_filter->predicate_expression_id,
              prepared_filter->predicate_row_binding,
              {}, request.expression_services,
              filter_capability_uuid, filter_input_row_count,
              {}, filter_expression_consumer,
              filter_predicate_consumer, &request.relational_dag,
              &request.context, &execution_request.mga_authority));
    } else {
      execution_request.available_executors.push_back(
          MakeLiveHeapFilterRegistration(
              prepared_filter->predicate_expression_id,
              prepared_filter->predicate_row_binding,
              request.relational_dag, request.expression_services,
              filter_capability_uuid, filter_input_row_count,
              request.context, filter_expression_consumer,
              filter_predicate_consumer));
    }
  }
  if (prepared_project.has_value()) {
    execution_request.available_executors.push_back(
        MakeLiveProjectRegistration(
            MakeLiveProjectRegistrationProfileForComposition(*prepared_project),
            project_implementation_id,
            project_capability_uuid, project_input_row_count,
            request.relational_dag, request.expression_services,
            request.context, true));
  }
  if (prepared_distinct.has_value()) {
    execution_request.available_executors.push_back(
        MakeLiveQueryDistinctRegistration(
            std::move(prepared_distinct->equality_terms),
            distinct_capability_uuid, distinct_input_row_count,
            distinct_comparison_bound, request.context));
  }
  if (prepared_count_star.has_value()) {
    execution_request.available_executors.push_back(
        MakeLiveCountStarRegistration(
            prepared_count_star->result_column,
            count_star_capability_uuid, count_star_input_row_count,
            request.context));
  }
  if (prepared_registry_aggregate.has_value()) {
    execution_request.available_executors.push_back(
        MakeLiveAggregateRegistryRegistration(
            *prepared_registry_aggregate,
            registry_aggregate_capability_uuid,
            registry_aggregate_input_row_count,
            registry_aggregate_filter_truth_memory_bytes,
            request.context, !planning_values_exact));
  }
  if (prepared_grouped_aggregate.has_value()) {
    execution_request.available_executors.push_back(
        MakeLiveGroupedCountSumRegistration(
            *prepared_grouped_aggregate,
            grouped_aggregate_capability_uuid,
            grouped_aggregate_input_row_count,
            grouped_aggregate_output_row_bound, request.context));
  }
  if (prepared_sort.has_value()) {
    const auto deterministic_tie_evidence_uuid = DerivedCanonicalUuid(
        identity_scope + ":" + prepared_sort->ordering_property_uuid,
        "node-composition.deterministic-tie");
    if (prepared_sort->expression_ordering) {
      execution_request.available_executors.push_back(
          MakeLiveExpressionSortRegistration(
              std::move(*prepared_sort),
              deterministic_tie_evidence_uuid, sort_capability_uuid,
              sort_input_row_count, sort_comparison_bound,
              request.relational_dag, request.expression_services,
              request.context));
    } else {
      execution_request.available_executors.push_back(
          MakeLiveSortRegistration(
              std::move(prepared_sort->order_terms),
              deterministic_tie_evidence_uuid, sort_capability_uuid,
              sort_input_row_count, sort_comparison_bound,
              request.context));
    }
  }
  if (prepared_row_number.has_value()) {
    execution_request.available_executors.push_back(
        MakeLiveRowNumberRegistration(
            *prepared_row_number, row_number_order_evidence_uuid,
            window_capability_uuid, sort_input_row_count, request.context));
  }
  if (prepared_ntile.has_value() && prepared_ntile_order_term.has_value() &&
      prepared_ntile_bucket_count_operand.has_value()) {
    execution_request.available_executors.push_back(
        MakeLiveNtileRegistration(
            *prepared_ntile, *prepared_ntile_order_term,
            *prepared_ntile_bucket_count_operand,
            std::string(kGlobalNtileProfile.function_uuid),
            row_number_order_evidence_uuid,
            ntile_order_term_binding_evidence_uuid, sort_input_row_count,
            request.context));
  }
  if (prepared_navigation_window.has_value() &&
      prepared_navigation_order_term.has_value() &&
      prepared_navigation_value_column.has_value()) {
    execution_request.available_executors.push_back(
        MakeLiveNavigationWindowRegistration(
            *prepared_navigation_window,
            *prepared_navigation_order_term,
            *prepared_navigation_value_column,
            prepared_navigation_nth_value_position_operand,
            prepared_navigation_frame_descriptor_uuid,
            row_number_order_evidence_uuid,
            navigation_frame_property_binding_evidence_uuid,
            navigation_capability_uuid,
            sort_input_row_count, navigation_maximum_pair_comparisons,
            navigation_maximum_effective_row_references,
            prepared_navigation_profile,
            request.context));
  }
  if (prepared_aggregate_window.has_value() &&
      prepared_aggregate_window_order_term.has_value()) {
    execution_request.available_executors.push_back(
        MakeLiveAggregateWindowRegistration(
            *prepared_aggregate_window,
            *prepared_aggregate_window_order_term,
            prepared_aggregate_window_value_column,
            prepared_aggregate_window_descriptor,
            prepared_aggregate_window_frame_descriptor_uuid,
            row_number_order_evidence_uuid,
            aggregate_window_frame_property_binding_evidence_uuid,
            aggregate_window_capability_uuid, sort_input_row_count,
            aggregate_window_maximum_pair_comparisons,
            aggregate_window_maximum_effective_row_references,
            aggregate_window_maximum_transition_count,
            request.context));
  }
  if (prepared_peer_ranking.has_value() &&
      prepared_peer_ranking_order_term.has_value()) {
    execution_request.available_executors.push_back(
        MakeLivePeerRankingRegistration(
            *prepared_peer_ranking, *prepared_peer_ranking_order_term,
            row_number_order_evidence_uuid,
            peer_ranking_order_term_binding_evidence_uuid,
            sort_input_row_count, peer_ranking_maximum_peer_comparisons,
            prepared_peer_ranking_profile,
            request.context));
  }
  if (prepared_table_subquery) {
    execution_request.available_executors.push_back(
        MakeLiveTableSubqueryRegistration(
            subquery_capability_uuid, table_subquery_input_row_count,
            request.context));
  }
  if (prepared_cardinality_subquery.has_value()) {
    execution_request.available_executors.push_back(
        MakeLiveCardinalitySubqueryRegistration(
            std::move(*prepared_cardinality_subquery),
            subquery_capability_uuid,
            cardinality_subquery_input_row_count, request.context));
  }
  if (prepared_predicate_subquery.has_value()) {
    execution_request.available_executors.push_back(
        MakeLivePredicateSubqueryRegistration(
            std::move(*prepared_predicate_subquery),
            subquery_capability_uuid,
            predicate_subquery_input_row_count,
            request.expression_services, request.context));
  }
  if (prepared_nonrecursive_cte) {
    execution_request.available_executors.push_back(
        MakeLiveNonrecursiveCteRegistration(
            nonrecursive_cte_implementation_id, cte_capability_uuid,
            nonrecursive_cte_input_row_count, request.context));
  }
  if (prepared_recursive_cte.has_value()) {
    execution_request.available_executors.push_back(
        MakeLiveRecursiveCteTermRegistration(
            prepared_recursive_cte->term,
            recursive_term_capability_uuid, request.context));
    execution_request.available_executors.push_back(
        MakeLiveRecursiveCteRegistration(
            std::move(*prepared_recursive_cte),
            recursive_term_capability_uuid,
            recursive_root_capability_uuid, request.context));
  }
  if (prepared_limit.has_value()) {
    if (planning_values_exact) {
      execution_request.available_executors.push_back(
          MakeLiveLimitRegistration(
              limit_implementation_id, limit_capability_uuid, row_limit,
              row_offset, fetch_first_rows_only, limit_input_row_count,
              request.context));
    } else {
      execution_request.available_executors.push_back(
          MakeLiveLimitRegistration(
              limit_implementation_id, limit_capability_uuid, row_limit,
              row_offset, fetch_first_rows_only, limit_input_row_count, {},
              &request.context, &execution_request.mga_authority));
    }
  }

  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "node-composition.execution-attempt");
  execution_request.result_publication_request
      .transaction_effect_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" +
          std::to_string(request.context.local_transaction_id) + ":" +
          std::to_string(
              request.context.snapshot_visible_through_local_transaction_id),
      "node-composition.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(state.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, state.batch.rows.size());

  const auto execution =
      ExecuteSelectedCanonicalObjectFreeDag(
          request.context, execution_request,
          planning.ordinary_runtime_memory_receipts);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-NODE-COMPOSITION-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "node-driven composition selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

}  // namespace scratchbird::engine::sblr
