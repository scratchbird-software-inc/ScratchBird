// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_current_heap_join_composition.hpp"
#include "canonical_query_aggregate_registration.hpp"
#include "canonical_query_descriptor_support.hpp"
#include "canonical_query_filter_registration.hpp"
#include "canonical_query_join_registration.hpp"
#include "canonical_query_model_family_composition_support.hpp"
#include "canonical_query_node_composition.hpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_query_object_free_profile.hpp"
#include "canonical_query_relational_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"
#include "canonical_relational_expression.hpp"

#include "engine/executor/descriptor_value_runtime.hpp"
#include "hash_digest.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "query/canonical_heap_optimizer_admission.hpp"
#include "query/expression_api.hpp"
#include "security/security_model.hpp"
#include "transaction/transaction_api.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace plan = scratchbird::engine::planner;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_CURRENT_HEAP_JOIN_COMPOSITION_AUTHORITY
// Owns admitted current-heap join trees, streaming hash reads, and bounded
// relational tails. Consumes engine-owned MGA visibility and revalidation;
// owns no snapshot creation, transaction finality, or public route selection.

CanonicalObjectFreeValuesExecutionResult ExecuteCanonicalCurrentHeapJoin(
    const CanonicalCurrentHeapExecutionRequest& input) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& dag = input.relational_dag;
  std::vector<const api::RelationalDagNode*> scans;
  std::vector<const api::RelationalDagNode*> joins;
  std::vector<const api::RelationalDagNode*> filters;
  std::vector<const api::RelationalDagNode*> projects;
  std::vector<const api::RelationalDagNode*> ctes;
  const api::RelationalDagNode* limit = nullptr;
  bool duplicate_or_unsupported_node = false;
  for (const auto& node : dag.nodes) {
    if (node.node_kind == api::RelationalDagNodeKind::kScan) {
      scans.push_back(&node);
    } else if (node.node_kind == api::RelationalDagNodeKind::kJoin) {
      joins.push_back(&node);
    } else if (node.node_kind == api::RelationalDagNodeKind::kFilter) {
      filters.push_back(&node);
    } else if (node.node_kind == api::RelationalDagNodeKind::kProject) {
      projects.push_back(&node);
    } else if (node.node_kind == api::RelationalDagNodeKind::kCte) {
      ctes.push_back(&node);
    } else if (node.node_kind == api::RelationalDagNodeKind::kLimit &&
               limit == nullptr) {
      limit = &node;
    } else {
      duplicate_or_unsupported_node = true;
    }
  }
  const auto node_for = [&](const std::uint32_t node_id) {
    const auto found = std::ranges::find_if(
        dag.nodes, [&](const auto& node) { return node.node_id == node_id; });
    return found == dag.nodes.end() ? nullptr : &*found;
  };
  const auto* terminal = node_for(dag.root_node_id);
  const auto* join = terminal;
  const auto consume_terminal = [&](const api::RelationalDagNodeKind kind,
                                    const api::RelationalDagNode* expected) {
    if (join == nullptr || join->node_kind != kind || join != expected ||
        join->input_node_ids.size() != 1) {
      return false;
    }
    join = node_for(join->input_node_ids.front());
    return join != nullptr;
  };
  bool exact_terminal_chain = !duplicate_or_unsupported_node;
  const api::RelationalDagNode* terminal_cte = nullptr;
  if (join != nullptr && join->node_kind == api::RelationalDagNodeKind::kCte) {
    terminal_cte = join;
    exact_terminal_chain =
        exact_terminal_chain &&
        consume_terminal(api::RelationalDagNodeKind::kCte, terminal_cte);
  }
  std::vector<const api::RelationalDagNode*> local_ctes;
  local_ctes.reserve(ctes.size());
  for (const auto* candidate : ctes) {
    if (candidate != terminal_cte) local_ctes.push_back(candidate);
  }
  const api::RelationalDagNode* terminal_limit = nullptr;
  if (join != nullptr &&
      join->node_kind == api::RelationalDagNodeKind::kLimit) {
    terminal_limit = join;
    exact_terminal_chain =
        exact_terminal_chain &&
        consume_terminal(api::RelationalDagNodeKind::kLimit, terminal_limit);
  }
  const api::RelationalDagNode* terminal_project = nullptr;
  if (join != nullptr &&
      join->node_kind == api::RelationalDagNodeKind::kProject) {
    terminal_project = join;
    exact_terminal_chain =
        exact_terminal_chain &&
        consume_terminal(api::RelationalDagNodeKind::kProject,
                         terminal_project);
  }
  std::vector<const api::RelationalDagNode*> local_projects;
  local_projects.reserve(projects.size());
  for (const auto* candidate : projects) {
    if (candidate != terminal_project) local_projects.push_back(candidate);
  }
  const api::RelationalDagNode* terminal_filter = nullptr;
  if (join != nullptr &&
      join->node_kind == api::RelationalDagNodeKind::kFilter) {
    terminal_filter = join;
    exact_terminal_chain =
        exact_terminal_chain &&
        consume_terminal(api::RelationalDagNodeKind::kFilter,
                         terminal_filter);
  }
  std::vector<const api::RelationalDagNode*> local_filters;
  local_filters.reserve(filters.size());
  for (const auto* candidate : filters) {
    if (candidate != terminal_filter) local_filters.push_back(candidate);
  }
  const auto bind_join_identity =
      [](const api::RelationalDagNode& node,
         exec::CanonicalAcceptedJoinKind* kind, std::string* component,
         std::string* operation) {
        if (kind == nullptr || component == nullptr || operation == nullptr) {
          return false;
        }
        *kind = exec::CanonicalAcceptedJoinKind::kCross;
        component->clear();
        operation->clear();
        if (node.semantic_variant_id == "join.cross.v1") {
          *component = "cross";
          *operation = "CROSS JOIN";
        } else if (node.semantic_variant_id == "join.inner.v1") {
          *kind = exec::CanonicalAcceptedJoinKind::kInner;
          *component = "inner";
          *operation = "INNER JOIN";
        } else if (node.semantic_variant_id == "join.left-outer.v1") {
          *kind = exec::CanonicalAcceptedJoinKind::kLeftOuter;
          *component = "left-outer";
          *operation = "LEFT OUTER JOIN";
        } else if (node.semantic_variant_id == "join.right-outer.v1") {
          *kind = exec::CanonicalAcceptedJoinKind::kRightOuter;
          *component = "right-outer";
          *operation = "RIGHT OUTER JOIN";
        } else if (node.semantic_variant_id == "join.full-outer.v1") {
          *kind = exec::CanonicalAcceptedJoinKind::kFullOuter;
          *component = "full-outer";
          *operation = "FULL OUTER JOIN";
        } else if (node.semantic_variant_id == "join.left-semi.v1") {
          *kind = exec::CanonicalAcceptedJoinKind::kLeftSemi;
          *component = "left-semi";
          *operation = "LEFT SEMI JOIN";
        } else if (node.semantic_variant_id == "join.left-anti.v1") {
          *kind = exec::CanonicalAcceptedJoinKind::kLeftAnti;
          *component = "left-anti";
          *operation = "LEFT ANTI JOIN";
        }
        return !component->empty();
      };
  auto join_kind = exec::CanonicalAcceptedJoinKind::kCross;
  std::string join_component;
  std::string join_operation;
  const bool accepted_join =
      join != nullptr && bind_join_identity(
                             *join, &join_kind, &join_component,
                             &join_operation);
  const bool predicate_join =
      accepted_join && join_kind != exec::CanonicalAcceptedJoinKind::kCross;
  if (dag.wire_version != 2 || scans.size() < 2 || scans.size() > 9 ||
      joins.size() != scans.size() - 1 || join == nullptr ||
      filters.size() > scans.size() + joins.size() ||
      projects.size() > scans.size() + joins.size() ||
      ctes.size() > scans.size() + joins.size() ||
      dag.nodes.size() > 68 ||
      (terminal_limit != nullptr &&
       (!local_filters.empty() || !local_projects.empty() || !ctes.empty())) ||
      (limit == nullptr) != (terminal_limit == nullptr) ||
      !exact_terminal_chain ||
      std::ranges::find(joins, join) == joins.end() ||
      dag.nodes.size() !=
          scans.size() + joins.size() + filters.size() +
              projects.size() + ctes.size() +
              static_cast<std::size_t>(limit != nullptr) ||
      !accepted_join ||
      join->bound_expression_ids.size() !=
          static_cast<std::size_t>(predicate_join)) {
    return result;
  }
  result.profile_matched = true;
  CanonicalObjectFreeValuesExecutionRequest response_context;
  response_context.context = input.context;
  response_context.relational_dag = input.relational_dag;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
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
    result.api_result = Failure(response_context, std::move(diagnostic_id),
                                std::move(detail));
    return result;
  };

  struct BoundHeapJoinNode {
    const api::RelationalDagNode* node{nullptr};
    exec::CanonicalAcceptedJoinKind kind{
        exec::CanonicalAcceptedJoinKind::kCross};
    std::string component;
    std::string operation;
    std::string implementation_id;
    std::string capability_uuid;
    CanonicalRelationalExpressionRowBinding predicate_row_binding;
  };
  struct BoundHeapFilterNode {
    const api::RelationalDagNode* node{nullptr};
    CanonicalRelationalExpressionRowBinding predicate_row_binding;
  };
  struct BoundHeapProjectNode {
    const api::RelationalDagNode* node{nullptr};
    std::vector<std::size_t> projected_columns;
  };
  struct BoundHeapCteNode {
    const api::RelationalDagNode* node{nullptr};
  };
  const auto unary_empty = [](const api::RelationalDagNode& node) {
    return node.required_object_uuids.empty() && node.values_row_ids.empty() &&
           node.required_property_uuids.empty() &&
           node.delivered_property_uuids.empty();
  };
  std::unordered_map<std::uint32_t, const api::RelationalDagNode*> nodes_by_id;
  for (const auto& node : dag.nodes) {
    if (node.node_id == 0 || !nodes_by_id.emplace(node.node_id, &node).second) {
      return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                    "object-backed join tree has a missing or duplicate node identity");
    }
  }
  std::unordered_set<std::uint32_t> local_filter_scan_input_node_ids;
  std::unordered_set<std::uint32_t> join_subtree_filter_node_ids;
  std::unordered_set<std::uint32_t> join_subtree_filter_base_node_ids;
  for (const auto* filter : local_filters) {
    const auto input_node =
        filter->input_node_ids.size() == 1
            ? nodes_by_id.find(filter->input_node_ids.front())
            : nodes_by_id.end();
    if (input_node == nodes_by_id.end()) {
      return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-FILTER-INPUT-V1",
                    "join FILTER input node is absent");
    }
    if (input_node->second->node_kind ==
        api::RelationalDagNodeKind::kScan) {
      if (!local_filter_scan_input_node_ids
               .insert(input_node->second->node_id)
               .second) {
        return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-FILTER-INPUT-V1",
                      "join scan has more than one local FILTER");
      }
      continue;
    }
    if (input_node->second != join &&
        input_node->second->node_kind ==
            api::RelationalDagNodeKind::kJoin &&
        join_subtree_filter_base_node_ids
            .insert(input_node->second->node_id)
            .second) {
      join_subtree_filter_node_ids.insert(filter->node_id);
      continue;
    }
    return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-FILTER-INPUT-V1",
                  "join subtree base admits at most one direct local FILTER");
  }
  const auto join_base_for_subtree_filter =
      [&](const api::RelationalDagNode* candidate)
      -> std::optional<std::uint32_t> {
    if (candidate == nullptr ||
        !join_subtree_filter_node_ids.contains(candidate->node_id) ||
        candidate->input_node_ids.size() != 1) {
      return std::nullopt;
    }
    const auto input = nodes_by_id.find(candidate->input_node_ids.front());
    if (input == nodes_by_id.end() || input->second == join ||
        input->second->node_kind != api::RelationalDagNodeKind::kJoin ||
        !join_subtree_filter_base_node_ids.contains(input->second->node_id)) {
      return std::nullopt;
    }
    return input->second->node_id;
  };
  std::unordered_set<std::uint32_t> join_subtree_project_node_ids;
  std::unordered_set<std::uint32_t> join_subtree_project_base_node_ids;
  for (const auto* project : local_projects) {
    const auto input_node =
        project->input_node_ids.size() == 1
            ? nodes_by_id.find(project->input_node_ids.front())
            : nodes_by_id.end();
    if (input_node == nodes_by_id.end()) {
      return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-PROJECT-INPUT-V1",
                    "join PROJECT input node is absent");
    }
    std::optional<std::uint32_t> join_base_node_id;
    if (input_node->second->node_kind ==
            api::RelationalDagNodeKind::kJoin &&
        input_node->second != join) {
      join_base_node_id = input_node->second->node_id;
    } else if (input_node->second->node_kind ==
               api::RelationalDagNodeKind::kFilter) {
      join_base_node_id = join_base_for_subtree_filter(input_node->second);
    }
    if (join_base_node_id.has_value()) {
      if (!join_subtree_project_base_node_ids
               .insert(*join_base_node_id)
               .second) {
        return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-PROJECT-INPUT-V1",
                      "join subtree base admits at most one local PROJECT");
      }
      join_subtree_project_node_ids.insert(project->node_id);
    }
  }
  const auto join_base_for_subtree_project =
      [&](const api::RelationalDagNode* candidate)
      -> std::optional<std::uint32_t> {
    if (candidate == nullptr ||
        !join_subtree_project_node_ids.contains(candidate->node_id) ||
        candidate->input_node_ids.size() != 1) {
      return std::nullopt;
    }
    const auto input = nodes_by_id.find(candidate->input_node_ids.front());
    if (input == nodes_by_id.end()) return std::nullopt;
    if (input->second != join &&
        input->second->node_kind == api::RelationalDagNodeKind::kJoin) {
      return join_subtree_project_base_node_ids.contains(input->second->node_id)
                 ? std::optional<std::uint32_t>{input->second->node_id}
                 : std::nullopt;
    }
    if (input->second->node_kind == api::RelationalDagNodeKind::kFilter) {
      const auto base = join_base_for_subtree_filter(input->second);
      return base.has_value() &&
                     join_subtree_project_base_node_ids.contains(*base)
                 ? base
                 : std::nullopt;
    }
    return std::nullopt;
  };
  std::unordered_set<std::uint32_t> join_subtree_cte_node_ids;
  std::unordered_set<std::uint32_t> join_subtree_cte_base_node_ids;
  for (const auto* cte : local_ctes) {
    const auto input_node =
        cte->input_node_ids.size() == 1
            ? nodes_by_id.find(cte->input_node_ids.front())
            : nodes_by_id.end();
    if (input_node == nodes_by_id.end()) {
      return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-CTE-INPUT-V1",
                    "join CTE input node is absent");
    }
    std::optional<std::uint32_t> join_base_node_id;
    if (input_node->second->node_kind ==
            api::RelationalDagNodeKind::kJoin &&
        input_node->second != join) {
      join_base_node_id = input_node->second->node_id;
    } else if (input_node->second->node_kind ==
               api::RelationalDagNodeKind::kFilter) {
      join_base_node_id = join_base_for_subtree_filter(input_node->second);
    } else if (input_node->second->node_kind ==
               api::RelationalDagNodeKind::kProject) {
      join_base_node_id = join_base_for_subtree_project(input_node->second);
    }
    if (join_base_node_id.has_value()) {
      if (!join_subtree_cte_base_node_ids.insert(*join_base_node_id).second) {
        return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-CTE-INPUT-V1",
                      "join subtree base admits at most one local CTE");
      }
      join_subtree_cte_node_ids.insert(cte->node_id);
    }
  }
  const auto lineage_output_node_for =
      [&](const api::RelationalDagNode* candidate) {
        for (std::size_t depth = 0;
             candidate != nullptr &&
                 candidate->node_kind == api::RelationalDagNodeKind::kCte &&
                 depth <= ctes.size();
             ++depth) {
          const auto input_node =
              candidate->input_node_ids.size() == 1
                  ? nodes_by_id.find(candidate->input_node_ids.front())
                  : nodes_by_id.end();
          candidate = input_node == nodes_by_id.end() ? nullptr
                                                       : input_node->second;
        }
        return candidate != nullptr &&
                       candidate->node_kind != api::RelationalDagNodeKind::kCte
                   ? candidate
                   : nullptr;
      };
  const auto exact_join_output_lineage =
      [&](const api::RelationalDagNode& join_node) {
        if (join_node.input_node_ids.size() != 2) return false;
        const auto left = nodes_by_id.find(join_node.input_node_ids[0]);
        const auto right = nodes_by_id.find(join_node.input_node_ids[1]);
        if (left == nodes_by_id.end() || right == nodes_by_id.end()) {
          return false;
        }
        const auto* left_output_node = lineage_output_node_for(left->second);
        const auto* right_output_node = lineage_output_node_for(right->second);
        if (left_output_node == nullptr || right_output_node == nullptr ||
            left_output_node->output_descriptor_ids !=
                left->second->output_descriptor_ids ||
            right_output_node->output_descriptor_ids !=
                right->second->output_descriptor_ids) {
          return false;
        }
        std::vector<const api::RelationalOutputRecord*> left_outputs;
        std::vector<const api::RelationalOutputRecord*> right_outputs;
        std::vector<const api::RelationalOutputRecord*> join_outputs;
        for (const auto& output : dag.outputs) {
          if (output.relation_node_id == left_output_node->node_id) {
            left_outputs.push_back(&output);
          }
          if (output.relation_node_id == right_output_node->node_id) {
            right_outputs.push_back(&output);
          }
          if (output.relation_node_id == join_node.node_id) {
            join_outputs.push_back(&output);
          }
        }
        std::ranges::sort(left_outputs, {},
                          &api::RelationalOutputRecord::ordinal);
        std::ranges::sort(right_outputs, {},
                          &api::RelationalOutputRecord::ordinal);
        std::ranges::sort(join_outputs, {},
                          &api::RelationalOutputRecord::ordinal);
        if (left_outputs.size() !=
                left->second->output_descriptor_ids.size() ||
            right_outputs.size() !=
                right->second->output_descriptor_ids.size()) {
          return false;
        }
        for (std::size_t ordinal = 0; ordinal < left_outputs.size();
             ++ordinal) {
          if (!left_outputs[ordinal]->visible ||
              left_outputs[ordinal]->ordinal != ordinal ||
              left_outputs[ordinal]->descriptor_id !=
                  left->second->output_descriptor_ids[ordinal]) {
            return false;
          }
        }
        for (std::size_t ordinal = 0; ordinal < right_outputs.size();
             ++ordinal) {
          if (!right_outputs[ordinal]->visible ||
              right_outputs[ordinal]->ordinal != ordinal ||
              right_outputs[ordinal]->descriptor_id !=
                  right->second->output_descriptor_ids[ordinal]) {
            return false;
          }
        }
        const bool left_only =
            join_node.semantic_variant_id == "join.left-semi.v1" ||
            join_node.semantic_variant_id == "join.left-anti.v1";
        std::vector<const api::RelationalOutputRecord*> expected_outputs =
            left_outputs;
        if (!left_only) {
          expected_outputs.insert(expected_outputs.end(), right_outputs.begin(),
                                  right_outputs.end());
        }
        if (join_outputs.size() != expected_outputs.size() ||
            join_outputs.size() != join_node.output_descriptor_ids.size()) {
          return false;
        }
        std::unordered_set<std::uint32_t> join_output_ids;
        for (std::size_t ordinal = 0; ordinal < join_outputs.size();
             ++ordinal) {
          const auto& output = *join_outputs[ordinal];
          const auto& expected = *expected_outputs[ordinal];
          if (!output.visible || output.ordinal != ordinal ||
              output.output_id == 0 ||
              !join_output_ids.insert(output.output_id).second ||
              output.descriptor_id != join_node.output_descriptor_ids[ordinal] ||
              output.descriptor_id != expected.descriptor_id ||
              output.expression_id != expected.expression_id ||
              output.output_name_utf8 != expected.output_name_utf8) {
            return false;
          }
        }
        return true;
      };
  if (std::ranges::any_of(joins, [&](const auto* join_node) {
        return !exact_join_output_lineage(*join_node);
      })) {
    return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-JOIN-LINEAGE-V1",
                  "join output records do not preserve exact child lineage");
  }
  std::unordered_set<std::uint32_t> visiting_join_nodes;
  std::unordered_set<std::uint32_t> completed_join_nodes;
  std::unordered_map<std::uint32_t, std::vector<bool>> nullable_by_node;
  std::vector<BoundHeapJoinNode> bound_joins;
  std::vector<BoundHeapFilterNode> bound_filters;
  std::vector<BoundHeapProjectNode> bound_projects;
  std::vector<BoundHeapCteNode> bound_ctes;
  std::unordered_set<std::uint32_t> bound_filter_predicate_expression_ids;
  const auto bind_project_lineage =
      [&](const api::RelationalDagNode& project,
          const api::RelationalDagNode& project_input,
          std::string* detail) {
        std::vector<const api::RelationalOutputRecord*> project_outputs;
        std::vector<const api::RelationalOutputRecord*> input_outputs;
        for (const auto& output : dag.outputs) {
          if (output.relation_node_id == project.node_id) {
            project_outputs.push_back(&output);
          }
          if (output.relation_node_id == project_input.node_id) {
            input_outputs.push_back(&output);
          }
        }
        std::ranges::sort(project_outputs, {},
                          &api::RelationalOutputRecord::ordinal);
        std::ranges::sort(input_outputs, {},
                          &api::RelationalOutputRecord::ordinal);
        bool exact_project =
            project.semantic_variant_id ==
                "project.catalog-visible-columns.v1" &&
            project.input_node_ids ==
                std::vector<std::uint32_t>{project_input.node_id} &&
            !project.bound_expression_ids.empty() &&
            project.bound_expression_ids.size() ==
                project.output_descriptor_ids.size() &&
            project_outputs.size() == project.output_descriptor_ids.size() &&
            input_outputs.size() == project_input.output_descriptor_ids.size() &&
            project.output_descriptor_ids.size() <
                project_input.output_descriptor_ids.size() &&
            unary_empty(project);
        for (std::size_t ordinal = 0;
             exact_project && ordinal < input_outputs.size(); ++ordinal) {
          exact_project = input_outputs[ordinal]->ordinal == ordinal &&
                          input_outputs[ordinal]->descriptor_id ==
                              project_input.output_descriptor_ids[ordinal];
        }
        std::unordered_set<std::size_t> selected_input_ordinals;
        auto project_nullable = std::vector<bool>{};
        project_nullable.reserve(project_outputs.size());
        BoundHeapProjectNode bound_project;
        bound_project.node = &project;
        bound_project.projected_columns.reserve(project_outputs.size());
        for (std::size_t ordinal = 0;
             exact_project && ordinal < project_outputs.size(); ++ordinal) {
          const auto& output = *project_outputs[ordinal];
          const auto expression = std::ranges::find_if(
              dag.expressions, [&](const auto& candidate) {
                return candidate.expression_id ==
                       project.bound_expression_ids[ordinal];
              });
          const auto source = std::ranges::find_if(
              input_outputs, [&](const auto* candidate) {
                return candidate->expression_id ==
                           project.bound_expression_ids[ordinal] &&
                       candidate->descriptor_id == output.descriptor_id;
              });
          if (expression == dag.expressions.end() ||
              source == input_outputs.end() || !output.visible ||
              output.ordinal != ordinal ||
              output.expression_id != project.bound_expression_ids[ordinal] ||
              output.descriptor_id != project.output_descriptor_ids[ordinal] ||
              output.output_name_utf8 != (*source)->output_name_utf8 ||
              expression->result_descriptor_id != output.descriptor_id) {
            exact_project = false;
            break;
          }
          const auto source_ordinal = static_cast<std::size_t>(
              std::distance(input_outputs.begin(), source));
          if (!selected_input_ordinals.insert(source_ordinal).second) {
            exact_project = false;
            break;
          }
          bound_project.projected_columns.push_back(source_ordinal);
          project_nullable.push_back(
              nullable_by_node.at(project_input.node_id)[source_ordinal]);
        }
        if (!exact_project ||
            !nullable_by_node.emplace(project.node_id,
                                      std::move(project_nullable)).second) {
          if (detail != nullptr && detail->empty()) {
            *detail = "object-backed join PROJECT lineage is not exact";
          }
          return false;
        }
        bound_projects.push_back(std::move(bound_project));
        return true;
      };
  std::function<bool(std::uint32_t, std::string*)> bind_tree =
      [&](const std::uint32_t node_id, std::string* detail) {
        const auto found = nodes_by_id.find(node_id);
        if (found == nodes_by_id.end()) {
          if (detail != nullptr) *detail = "join input node is absent";
          return false;
        }
        const auto& node = *found->second;
        if (node.node_kind == api::RelationalDagNodeKind::kScan) {
          if (!completed_join_nodes.insert(node_id).second ||
              node.semantic_variant_id != "relation.source.v1" ||
              !node.input_node_ids.empty() ||
              node.required_object_uuids.size() != 1 ||
              node.output_descriptor_ids.empty() ||
              node.bound_expression_ids.size() !=
                  node.output_descriptor_ids.size() ||
              !node.required_property_uuids.empty() ||
              !node.delivered_property_uuids.empty() ||
              !node.values_row_ids.empty()) {
            if (detail != nullptr) {
              *detail = "join leaf is not one exact object-backed relation scan";
            }
            return false;
          }
          auto& nullable = nullable_by_node[node_id];
          nullable.reserve(node.output_descriptor_ids.size());
          for (const auto descriptor_id : node.output_descriptor_ids) {
            const auto descriptor = std::ranges::find_if(
                dag.descriptors, [&](const auto& candidate) {
                  return candidate.descriptor_id == descriptor_id;
                });
            if (descriptor == dag.descriptors.end()) {
              if (detail != nullptr) {
                *detail = "join leaf output descriptor is unresolved";
              }
              return false;
            }
            nullable.push_back(
                descriptor->nullability == api::RelationalNullability::kNullable);
          }
          return true;
        }
        if (node.node_kind == api::RelationalDagNodeKind::kFilter) {
          const auto input_node =
              node.input_node_ids.size() == 1
                  ? nodes_by_id.find(node.input_node_ids.front())
                  : nodes_by_id.end();
          const bool scan_input =
              input_node != nodes_by_id.end() &&
              input_node->second->node_kind ==
                  api::RelationalDagNodeKind::kScan;
          const bool join_subtree_input =
              input_node != nodes_by_id.end() &&
              join_subtree_filter_node_ids.contains(node.node_id) &&
              input_node->second->node_kind ==
                  api::RelationalDagNodeKind::kJoin;
          if (std::ranges::find(local_filters, &node) == local_filters.end() ||
              input_node == nodes_by_id.end() ||
              (!scan_input && !join_subtree_input) ||
              node.semantic_variant_id !=
                  "filter.catalog-column-numeric-comparison.v1" ||
              node.input_node_ids.front() == node.node_id || node.shareable ||
              node.bound_expression_ids.size() != 1 ||
              node.output_descriptor_ids !=
                  input_node->second->output_descriptor_ids ||
              !bound_filter_predicate_expression_ids
                   .insert(node.bound_expression_ids.front())
                   .second ||
              !unary_empty(node) || completed_join_nodes.contains(node_id) ||
              !visiting_join_nodes.insert(node_id).second ||
              !bind_tree(node.input_node_ids.front(), detail)) {
            if (detail != nullptr && detail->empty()) {
              *detail =
                  "join input FILTER is not one exact schema-preserving predicate";
            }
            return false;
          }
          BoundHeapFilterNode bound_filter;
          bound_filter.node = &node;
          if (!PrepareInputRowBindingForComposition(
                  dag, node.bound_expression_ids.front(),
                  input_node->second->output_descriptor_ids,
                  &bound_filter.predicate_row_binding, detail)) {
            if (detail != nullptr && detail->empty()) {
              *detail = "join leaf FILTER predicate binding is not exact";
            }
            return false;
          }
          bound_filter.predicate_row_binding.row_nullable =
              nullable_by_node.at(input_node->second->node_id);
          nullable_by_node.emplace(
              node_id, nullable_by_node.at(input_node->second->node_id));
          bound_filters.push_back(std::move(bound_filter));
          visiting_join_nodes.erase(node_id);
          completed_join_nodes.insert(node_id);
          return true;
        }
        if (node.node_kind == api::RelationalDagNodeKind::kProject) {
          const auto input_node =
              node.input_node_ids.size() == 1
                  ? nodes_by_id.find(node.input_node_ids.front())
                  : nodes_by_id.end();
          const bool exact_input =
              input_node != nodes_by_id.end() &&
              (input_node->second->node_kind ==
                   api::RelationalDagNodeKind::kScan ||
               (input_node->second->node_kind ==
                    api::RelationalDagNodeKind::kFilter &&
                std::ranges::find(local_filters, input_node->second) !=
                    local_filters.end() &&
                (!join_subtree_filter_node_ids.contains(
                     input_node->second->node_id) ||
                 join_subtree_project_node_ids.contains(node.node_id))) ||
               (input_node->second->node_kind ==
                    api::RelationalDagNodeKind::kJoin &&
                join_subtree_project_node_ids.contains(node.node_id)));
          if (std::ranges::find(local_projects, &node) ==
                  local_projects.end() ||
              !exact_input || node.input_node_ids.front() == node.node_id ||
              node.shareable || completed_join_nodes.contains(node_id) ||
              !visiting_join_nodes.insert(node_id).second ||
              !bind_tree(node.input_node_ids.front(), detail) ||
              !bind_project_lineage(node, *input_node->second, detail)) {
            if (detail != nullptr && detail->empty()) {
              *detail =
                  "join input PROJECT is not one exact descriptor projection";
            }
            return false;
          }
          visiting_join_nodes.erase(node_id);
          completed_join_nodes.insert(node_id);
          return true;
        }
        if (node.node_kind == api::RelationalDagNodeKind::kCte) {
          const auto input_node =
              node.input_node_ids.size() == 1
                  ? nodes_by_id.find(node.input_node_ids.front())
                  : nodes_by_id.end();
          const bool join_subtree_cte =
              join_subtree_cte_node_ids.contains(node.node_id);
          const bool exact_input =
              input_node != nodes_by_id.end() &&
              (input_node->second->node_kind ==
                   api::RelationalDagNodeKind::kScan ||
               (input_node->second->node_kind ==
                    api::RelationalDagNodeKind::kFilter &&
                std::ranges::find(local_filters, input_node->second) !=
                    local_filters.end() &&
                (!join_subtree_filter_node_ids.contains(
                     input_node->second->node_id) ||
                 join_subtree_cte)) ||
               (input_node->second->node_kind ==
                    api::RelationalDagNodeKind::kProject &&
                std::ranges::find(local_projects, input_node->second) !=
                    local_projects.end() &&
                (!join_subtree_project_node_ids.contains(
                     input_node->second->node_id) ||
                 join_subtree_cte)) ||
               (input_node->second->node_kind ==
                    api::RelationalDagNodeKind::kJoin &&
                input_node->second != join && join_subtree_cte));
          const bool outputless =
              std::ranges::none_of(dag.outputs, [&](const auto& output) {
                return output.relation_node_id == node.node_id;
              });
          if (std::ranges::find(local_ctes, &node) == local_ctes.end() ||
              !exact_input || node.input_node_ids.front() == node.node_id ||
              node.semantic_variant_id != "cte.bound.v1" ||
              node.output_descriptor_ids !=
                  input_node->second->output_descriptor_ids ||
              !node.bound_expression_ids.empty() || !unary_empty(node) ||
              !outputless || completed_join_nodes.contains(node_id) ||
              !visiting_join_nodes.insert(node_id).second ||
              !bind_tree(node.input_node_ids.front(), detail)) {
            if (detail != nullptr && detail->empty()) {
              *detail =
                  "join leaf CTE is not one exact nonrecursive carrier";
            }
            return false;
          }
          nullable_by_node.emplace(
              node_id, nullable_by_node.at(input_node->second->node_id));
          bound_ctes.push_back({&node});
          visiting_join_nodes.erase(node_id);
          completed_join_nodes.insert(node_id);
          return true;
        }
        if (node.node_kind != api::RelationalDagNodeKind::kJoin ||
            node.input_node_ids.size() != 2 ||
            node.input_node_ids[0] == node.input_node_ids[1] ||
            !node.required_object_uuids.empty() ||
            !node.required_property_uuids.empty() ||
            !node.delivered_property_uuids.empty() ||
            !node.values_row_ids.empty() ||
            completed_join_nodes.contains(node_id) ||
            !visiting_join_nodes.insert(node_id).second) {
          if (detail != nullptr) {
            *detail = "join tree is cyclic, shared, or has an invalid binary node";
          }
          return false;
        }
        BoundHeapJoinNode bound;
        bound.node = &node;
        if (!bind_join_identity(node, &bound.kind, &bound.component,
                                &bound.operation)) {
          if (detail != nullptr) *detail = "join kind is outside the accepted set";
          return false;
        }
        const bool predicate =
            bound.kind != exec::CanonicalAcceptedJoinKind::kCross;
        if (node.bound_expression_ids.size() !=
            static_cast<std::size_t>(predicate)) {
          if (detail != nullptr) {
            *detail = "join predicate cardinality does not match its join kind";
          }
          return false;
        }
        if (!bind_tree(node.input_node_ids[0], detail) ||
            !bind_tree(node.input_node_ids[1], detail)) {
          return false;
        }
        const auto* left = nodes_by_id.at(node.input_node_ids[0]);
        const auto* right = nodes_by_id.at(node.input_node_ids[1]);
        std::vector<std::uint32_t> predicate_descriptors =
            left->output_descriptor_ids;
        predicate_descriptors.insert(predicate_descriptors.end(),
                                     right->output_descriptor_ids.begin(),
                                     right->output_descriptor_ids.end());
        const bool left_only =
            bound.kind == exec::CanonicalAcceptedJoinKind::kLeftSemi ||
            bound.kind == exec::CanonicalAcceptedJoinKind::kLeftAnti;
        const auto expected_descriptors =
            left_only ? left->output_descriptor_ids : predicate_descriptors;
        if (node.output_descriptor_ids != expected_descriptors) {
          if (detail != nullptr) {
            *detail = "join output descriptors do not preserve exact input lineage";
          }
          return false;
        }
        if (predicate) {
          auto predicate_nullable = nullable_by_node.at(left->node_id);
          const auto& right_nullable = nullable_by_node.at(right->node_id);
          predicate_nullable.insert(predicate_nullable.end(),
                                    right_nullable.begin(),
                                    right_nullable.end());
          if (!PrepareInputRowBindingForComposition(
                  dag, node.bound_expression_ids.front(),
                  predicate_descriptors, &bound.predicate_row_binding,
                  detail)) {
            if (detail != nullptr && detail->empty()) {
              *detail = "join predicate binding is not exact";
            }
            return false;
          }
          bound.predicate_row_binding.row_nullable =
              std::move(predicate_nullable);
        }
        auto output_nullable = nullable_by_node.at(left->node_id);
        if (!left_only) {
          auto right_nullable = nullable_by_node.at(right->node_id);
          if (bound.kind == exec::CanonicalAcceptedJoinKind::kRightOuter ||
              bound.kind == exec::CanonicalAcceptedJoinKind::kFullOuter) {
            std::ranges::fill(output_nullable, true);
          }
          if (bound.kind == exec::CanonicalAcceptedJoinKind::kLeftOuter ||
              bound.kind == exec::CanonicalAcceptedJoinKind::kFullOuter) {
            std::ranges::fill(right_nullable, true);
          }
          output_nullable.insert(output_nullable.end(), right_nullable.begin(),
                                 right_nullable.end());
        }
        nullable_by_node.emplace(node_id, std::move(output_nullable));
        bound_joins.push_back(std::move(bound));
        visiting_join_nodes.erase(node_id);
        completed_join_nodes.insert(node_id);
        return true;
      };
  std::string join_tree_detail;
  if (!bind_tree(join->node_id, &join_tree_detail) ||
      completed_join_nodes.size() !=
          scans.size() + joins.size() +
              local_filters.size() + local_projects.size() +
              local_ctes.size() ||
      bound_joins.size() != joins.size() ||
      bound_filters.size() != local_filters.size() ||
      bound_projects.size() != local_projects.size() ||
      bound_ctes.size() != local_ctes.size()) {
    return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-JOIN-TREE-V1",
                  join_tree_detail.empty()
                      ? "object-backed join tree is disconnected"
                      : join_tree_detail);
  }

  std::uint64_t row_limit = 0;
  std::uint64_t row_offset = 0;
  CanonicalRelationalExpressionRowBinding terminal_filter_row_binding;
  const auto* filter_input = join;
  if (terminal_filter != nullptr) {
    std::string detail;
    if (terminal_filter->semantic_variant_id !=
            "filter.catalog-column-numeric-comparison.v1" ||
        terminal_filter->input_node_ids !=
            std::vector<std::uint32_t>{filter_input->node_id} ||
        terminal_filter->bound_expression_ids.size() != 1 ||
        !bound_filter_predicate_expression_ids
             .insert(terminal_filter->bound_expression_ids.front())
             .second ||
        terminal_filter->output_descriptor_ids !=
            filter_input->output_descriptor_ids ||
        !unary_empty(*terminal_filter) ||
        !PrepareInputRowBindingForComposition(
            dag, terminal_filter->bound_expression_ids.front(),
            filter_input->output_descriptor_ids,
            &terminal_filter_row_binding,
            &detail)) {
      return refuse(
          "QOW-DIAG-PACKET7-OBJECT-HEAP-JOIN-TAIL-FILTER-V1",
          detail.empty()
              ? "object-backed join FILTER tail binding is not exact"
              : detail);
    }
    terminal_filter_row_binding.row_nullable = nullable_by_node.at(join->node_id);
    nullable_by_node.emplace(terminal_filter->node_id,
                             nullable_by_node.at(join->node_id));
    bound_filters.push_back(
        {terminal_filter, std::move(terminal_filter_row_binding)});
  }
  if (bound_filters.size() != filters.size()) {
    return refuse(
        "QOW-DIAG-PACKET7-OBJECT-HEAP-FILTER-INPUT-V1",
        "object-backed join FILTER configuration coverage is incomplete");
  }

  const auto* project_input =
      terminal_filter != nullptr ? terminal_filter : join;
  if (terminal_project != nullptr) {
    std::string detail;
    if (!bind_project_lineage(*terminal_project, *project_input, &detail)) {
      return refuse(
          "QOW-DIAG-PACKET7-OBJECT-HEAP-JOIN-TAIL-PROJECT-V1",
          detail.empty()
              ? "object-backed join PROJECT tail lineage is not exact"
              : detail);
    }
  }
  if (bound_projects.size() != projects.size()) {
    return refuse(
        "QOW-DIAG-PACKET7-OBJECT-HEAP-PROJECT-INPUT-V1",
        "object-backed join PROJECT configuration coverage is incomplete");
  }

  const auto* limit_input =
      terminal_project != nullptr
          ? terminal_project
          : (terminal_filter != nullptr ? terminal_filter : join);
  if (terminal_limit != nullptr) {
    if (terminal_filter != nullptr) {
      const auto predicate = std::ranges::find_if(
          dag.expressions, [&](const auto& expression) {
            return terminal_filter->bound_expression_ids.size() == 1 &&
                   expression.expression_id ==
                       terminal_filter->bound_expression_ids.front();
          });
      const api::RelationalExpressionRecord* filter_identifier = nullptr;
      const api::RelationalExpressionRecord* filter_value = nullptr;
      if (predicate != dag.expressions.end() &&
          predicate->child_expression_ids.size() == 2) {
        const auto identifier = std::ranges::find_if(
            dag.expressions, [&](const auto& expression) {
              return expression.expression_id ==
                     predicate->child_expression_ids[0];
            });
        const auto value = std::ranges::find_if(
            dag.expressions, [&](const auto& expression) {
              return expression.expression_id ==
                     predicate->child_expression_ids[1];
            });
        if (identifier != dag.expressions.end()) {
          filter_identifier = &*identifier;
        }
        if (value != dag.expressions.end()) filter_value = &*value;
      }
      const auto limit_value = std::ranges::find_if(
          dag.expressions, [&](const auto& expression) {
            return terminal_limit->bound_expression_ids.size() == 1 &&
                   expression.expression_id ==
                       terminal_limit->bound_expression_ids.front();
          });
      const auto descriptor_for = [&](const auto* expression) {
        return expression == nullptr
                   ? dag.descriptors.end()
                   : std::ranges::find_if(
                         dag.descriptors, [&](const auto& descriptor) {
                           return descriptor.descriptor_id ==
                                  expression->result_descriptor_id;
                         });
      };
      const auto identifier_descriptor = descriptor_for(filter_identifier);
      const auto literal_descriptor = descriptor_for(filter_value);
      const auto predicate_descriptor =
          predicate == dag.expressions.end()
              ? dag.descriptors.end()
              : descriptor_for(&*predicate);
      const auto limit_descriptor =
          limit_value == dag.expressions.end()
              ? dag.descriptors.end()
              : descriptor_for(&*limit_value);
      const bool accepted_operator =
          predicate != dag.expressions.end() &&
          predicate->operator_name.has_value() &&
          (*predicate->operator_name == "=" ||
           *predicate->operator_name == "<>" ||
           *predicate->operator_name == "!=" ||
           *predicate->operator_name == "<" ||
           *predicate->operator_name == "<=" ||
           *predicate->operator_name == ">" ||
           *predicate->operator_name == ">=");
      const bool literal_filter_value =
          filter_value != nullptr &&
          filter_value->expression_kind ==
              api::RelationalExpressionKind::kLiteral &&
          filter_value->literal_kind == api::RelationalLiteralKind::kNumeric &&
          filter_value->literal_typed_value_v1.has_value() &&
          !filter_value->parameter_typed_value_v1.has_value();
      const bool parameter_filter_value =
          filter_value != nullptr &&
          filter_value->expression_kind ==
              api::RelationalExpressionKind::kParameter &&
          !filter_value->literal_kind.has_value() &&
          !filter_value->literal_typed_value_v1.has_value() &&
          filter_value->parameter_typed_value_v1.has_value();
      const bool parameter_limit_value =
          limit_value != dag.expressions.end() &&
          limit_value->expression_kind ==
              api::RelationalExpressionKind::kParameter;
      const bool exact_operand_pair =
          literal_filter_value && parameter_limit_value;
      bool exact_filter_value = literal_filter_value || parameter_filter_value;
      if (exact_filter_value) {
        const auto& typed_bytes =
            literal_filter_value
                ? filter_value->literal_typed_value_v1->canonical_value_bytes
                : filter_value->parameter_typed_value_v1->canonical_value_bytes;
        const auto& typed_descriptor_uuid =
            literal_filter_value
                ? filter_value->literal_typed_value_v1->descriptor_uuid
                : filter_value->parameter_typed_value_v1->descriptor_uuid;
        const auto typed_descriptor_generation =
            literal_filter_value
                ? filter_value->literal_typed_value_v1->descriptor_generation
                : filter_value->parameter_typed_value_v1->descriptor_generation;
        const auto& typed_value_state =
            literal_filter_value
                ? filter_value->literal_typed_value_v1->value_state
                : filter_value->parameter_typed_value_v1->value_state;
        const auto& typed_sha =
            literal_filter_value
                ? filter_value->literal_typed_value_v1->canonical_value_sha256
                : filter_value->parameter_typed_value_v1->canonical_value_sha256;
        const auto digest = scratchbird::core::hash::ComputeSha256Digest(
            typed_bytes);
        exact_filter_value =
            typed_descriptor_generation != 0 &&
            typed_value_state == "value" && typed_bytes.size() == 8 &&
            (typed_bytes.back() & 0x80U) == 0 && digest.ok() &&
            digest.digest == typed_sha &&
            literal_descriptor != dag.descriptors.end() &&
            typed_descriptor_uuid == literal_descriptor->descriptor_uuid;
      }
      const auto identifier_source_count =
          filter_identifier == nullptr
              ? 0
              : std::ranges::count_if(dag.outputs, [&](const auto& output) {
                  return output.relation_node_id == join->node_id &&
                         output.visible &&
                         output.expression_id ==
                             filter_identifier->expression_id &&
                         output.descriptor_id ==
                             filter_identifier->result_descriptor_id;
                });
      const bool distinct_expression_ids =
          predicate != dag.expressions.end() &&
          filter_identifier != nullptr && filter_value != nullptr &&
          limit_value != dag.expressions.end() &&
          predicate->expression_id != filter_identifier->expression_id &&
          predicate->expression_id != filter_value->expression_id &&
          predicate->expression_id != limit_value->expression_id &&
          filter_identifier->expression_id != filter_value->expression_id &&
          filter_identifier->expression_id != limit_value->expression_id &&
          filter_value->expression_id != limit_value->expression_id;
      const bool distinct_descriptor_ids =
          identifier_descriptor != dag.descriptors.end() &&
          literal_descriptor != dag.descriptors.end() &&
          predicate_descriptor != dag.descriptors.end() &&
          limit_descriptor != dag.descriptors.end() &&
          identifier_descriptor->descriptor_id !=
              literal_descriptor->descriptor_id &&
          identifier_descriptor->descriptor_id !=
              predicate_descriptor->descriptor_id &&
          identifier_descriptor->descriptor_id !=
              limit_descriptor->descriptor_id &&
          literal_descriptor->descriptor_id !=
              predicate_descriptor->descriptor_id &&
          literal_descriptor->descriptor_id !=
              limit_descriptor->descriptor_id &&
          predicate_descriptor->descriptor_id !=
              limit_descriptor->descriptor_id;
      if (!accepted_operator || filter_identifier == nullptr ||
          filter_identifier->expression_kind !=
              api::RelationalExpressionKind::kIdentifier ||
          !filter_identifier->child_expression_ids.empty() ||
          filter_identifier->function_uuid.has_value() ||
          !filter_identifier->bound_name_uuid.has_value() ||
          filter_identifier->literal_kind.has_value() ||
          filter_identifier->operator_name.has_value() ||
          filter_identifier->literal_or_parameter_ref.has_value() ||
          filter_identifier->literal_typed_value_v1.has_value() ||
          filter_identifier->parameter_typed_value_v1.has_value() ||
          predicate == dag.expressions.end() ||
          predicate->expression_kind !=
              api::RelationalExpressionKind::kBinary ||
          predicate->function_uuid.has_value() ||
          predicate->bound_name_uuid.has_value() ||
          predicate->literal_kind.has_value() ||
          predicate->literal_or_parameter_ref.has_value() ||
          predicate->literal_typed_value_v1.has_value() ||
          predicate->parameter_typed_value_v1.has_value() ||
          filter_value == nullptr || !exact_operand_pair ||
          !filter_value->child_expression_ids.empty() ||
          filter_value->function_uuid.has_value() ||
          filter_value->bound_name_uuid.has_value() ||
          filter_value->operator_name.has_value() ||
          filter_value->literal_or_parameter_ref.has_value() ||
          !exact_filter_value || identifier_source_count != 1 ||
          identifier_descriptor == dag.descriptors.end() ||
          literal_descriptor == dag.descriptors.end() ||
          predicate_descriptor == dag.descriptors.end() ||
          ExactBoundedSignedIntegerTypeRankForComposition(
              identifier_descriptor->type_uuid) == 0 ||
          literal_descriptor->type_uuid !=
              ExactCanonicalInt64TypeUuidV1() ||
          literal_descriptor->nullability !=
              api::RelationalNullability::kNonNull ||
          predicate_descriptor->nullability !=
              api::RelationalNullability::kNullable ||
          predicate_descriptor->type_uuid !=
              ExactCanonicalCoreDatatypeUuidV1("boolean") ||
          identifier_descriptor->collation_uuid.has_value() ||
          literal_descriptor->collation_uuid.has_value() ||
          predicate_descriptor->collation_uuid.has_value() ||
          identifier_descriptor->timezone_profile_id.has_value() ||
          literal_descriptor->timezone_profile_id.has_value() ||
          predicate_descriptor->timezone_profile_id.has_value() ||
          identifier_descriptor->width.has_value() ||
          identifier_descriptor->precision.has_value() ||
          identifier_descriptor->scale.has_value() ||
          literal_descriptor->width.has_value() ||
          literal_descriptor->precision.has_value() ||
          literal_descriptor->scale.has_value() ||
          predicate_descriptor->width.has_value() ||
          predicate_descriptor->precision.has_value() ||
          predicate_descriptor->scale.has_value() ||
          limit_value == dag.expressions.end() ||
          !distinct_expression_ids || !distinct_descriptor_ids) {
        return refuse(
            "QOW-DIAG-PACKET7-OBJECT-HEAP-JOIN-TAIL-LIMIT-V1",
            "object-backed join FILTER and LIMIT operands are not composable");
      }
    }
    CanonicalRelationalExpressionRuntime expression_runtime(
        input.relational_dag, {});
    std::string detail;
    const auto terminal_limit_arity =
        terminal_limit->semantic_variant_id == "limit.bound-count.v1"
            ? std::size_t{1}
            : terminal_limit->semantic_variant_id ==
                      "limit.bound-count-offset.v1"
                  ? std::size_t{2}
                  : std::size_t{0};
    std::unordered_set<std::uint32_t> limit_expression_ids;
    std::unordered_set<std::uint32_t> limit_descriptor_ids;
    bool exact_limit_values = terminal_limit_arity != 0;
    for (const auto expression_id : terminal_limit->bound_expression_ids) {
      const auto expression = std::ranges::find_if(
          dag.expressions, [&](const auto& candidate) {
            return candidate.expression_id == expression_id;
          });
      const auto descriptor =
          expression == dag.expressions.end()
              ? dag.descriptors.end()
              : std::ranges::find_if(
                    dag.descriptors, [&](const auto& candidate) {
                      return candidate.descriptor_id ==
                             expression->result_descriptor_id;
                    });
      const bool literal_value =
          expression != dag.expressions.end() &&
          expression->expression_kind ==
              api::RelationalExpressionKind::kLiteral &&
          expression->literal_kind == api::RelationalLiteralKind::kNumeric &&
          !expression->parameter_typed_value_v1.has_value() &&
          (expression->literal_or_parameter_ref.has_value() ||
           expression->literal_typed_value_v1.has_value());
      const bool parameter_value =
          expression != dag.expressions.end() &&
          expression->expression_kind ==
              api::RelationalExpressionKind::kParameter &&
          !expression->literal_kind.has_value() &&
          !expression->literal_or_parameter_ref.has_value() &&
          !expression->literal_typed_value_v1.has_value() &&
          expression->parameter_typed_value_v1.has_value();
      exact_limit_values =
          exact_limit_values && expression != dag.expressions.end() &&
          limit_expression_ids.insert(expression_id).second &&
          (literal_value || parameter_value) &&
          expression->child_expression_ids.empty() &&
          !expression->function_uuid.has_value() &&
          !expression->bound_name_uuid.has_value() &&
          !expression->operator_name.has_value() &&
          descriptor != dag.descriptors.end() &&
          limit_descriptor_ids.insert(descriptor->descriptor_id).second &&
          descriptor->nullability ==
              api::RelationalNullability::kNonNull &&
          descriptor->type_uuid == ExactCanonicalInt64TypeUuidV1() &&
          !descriptor->collation_uuid.has_value() &&
          !descriptor->timezone_profile_id.has_value() &&
          !descriptor->width.has_value() && !descriptor->precision.has_value() &&
          !descriptor->scale.has_value();
    }
    if (terminal_limit->shareable ||
        terminal_limit_arity == 0 ||
        !exact_limit_values ||
        (terminal_limit_arity == 2 && terminal_filter != nullptr) ||
        terminal_limit->input_node_ids !=
            std::vector<std::uint32_t>{limit_input->node_id} ||
        terminal_limit->bound_expression_ids.size() != terminal_limit_arity ||
        terminal_limit->output_descriptor_ids !=
            limit_input->output_descriptor_ids ||
        !unary_empty(*terminal_limit) ||
        !EvaluateNonNegativeRowBoundForComposition(
            &expression_runtime,
            terminal_limit->bound_expression_ids.front(), &row_limit,
            &detail) ||
        (terminal_limit_arity == 2 &&
         !EvaluateNonNegativeRowBoundForComposition(
             &expression_runtime,
             terminal_limit->bound_expression_ids[1], &row_offset,
             &detail))) {
      return refuse(
          "QOW-DIAG-PACKET7-OBJECT-HEAP-JOIN-TAIL-LIMIT-V1",
          detail.empty()
              ? "object-backed join LIMIT tail binding is not exact"
              : detail);
    }
    std::vector<const api::RelationalOutputRecord*> input_outputs;
    std::vector<const api::RelationalOutputRecord*> limit_outputs;
    for (const auto& output : dag.outputs) {
      if (output.relation_node_id == limit_input->node_id) {
        input_outputs.push_back(&output);
      }
      if (output.relation_node_id == terminal_limit->node_id) {
        limit_outputs.push_back(&output);
      }
    }
    std::ranges::sort(input_outputs, {},
                      &api::RelationalOutputRecord::ordinal);
    std::ranges::sort(limit_outputs, {},
                      &api::RelationalOutputRecord::ordinal);
    bool exact_limit_outputs =
        input_outputs.size() == limit_input->output_descriptor_ids.size() &&
        limit_outputs.size() ==
            terminal_limit->output_descriptor_ids.size() &&
        input_outputs.size() == limit_outputs.size();
    std::unordered_set<std::uint32_t> limit_output_ids;
    for (std::size_t ordinal = 0;
         exact_limit_outputs && ordinal < limit_outputs.size(); ++ordinal) {
      exact_limit_outputs =
          input_outputs[ordinal]->visible &&
          input_outputs[ordinal]->ordinal == ordinal &&
          limit_outputs[ordinal]->visible &&
          limit_outputs[ordinal]->ordinal == ordinal &&
          limit_outputs[ordinal]->output_id != 0 &&
          limit_output_ids.insert(limit_outputs[ordinal]->output_id).second &&
          limit_outputs[ordinal]->descriptor_id ==
              terminal_limit->output_descriptor_ids[ordinal] &&
          limit_outputs[ordinal]->descriptor_id ==
              input_outputs[ordinal]->descriptor_id &&
          limit_outputs[ordinal]->expression_id ==
              input_outputs[ordinal]->expression_id &&
          limit_outputs[ordinal]->output_name_utf8 ==
              input_outputs[ordinal]->output_name_utf8;
    }
    if (!exact_limit_outputs) {
      return refuse(
          "QOW-DIAG-PACKET7-OBJECT-HEAP-JOIN-TAIL-LIMIT-V1",
          "object-backed join LIMIT output lineage is not exact");
    }
    nullable_by_node.emplace(terminal_limit->node_id,
                             nullable_by_node.at(limit_input->node_id));
  }

  const auto* cte_input =
      terminal_project != nullptr
          ? terminal_project
          : (terminal_filter != nullptr ? terminal_filter : join);
  if (terminal_cte != nullptr) {
    const bool outputless =
        std::ranges::none_of(dag.outputs, [&](const auto& output) {
          return output.relation_node_id == terminal_cte->node_id;
        });
    if (terminal_cte->semantic_variant_id != "cte.bound.v1" ||
        terminal_cte->input_node_ids !=
            std::vector<std::uint32_t>{cte_input->node_id} ||
        terminal_cte->output_descriptor_ids !=
            cte_input->output_descriptor_ids ||
        !terminal_cte->bound_expression_ids.empty() ||
        !unary_empty(*terminal_cte) || !outputless) {
      return refuse(
          "QOW-DIAG-PACKET7-OBJECT-HEAP-JOIN-TAIL-CTE-V1",
          "object-backed join nonrecursive CTE tail binding is not exact");
    }
    nullable_by_node.emplace(terminal_cte->node_id,
                             nullable_by_node.at(cte_input->node_id));
    bound_ctes.push_back({terminal_cte});
  }
  if (bound_ctes.size() != ctes.size()) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-CTE-INPUT-V1",
        "object-backed join CTE configuration coverage is incomplete");
  }
  const auto* final_output_node =
      terminal_limit != nullptr
          ? terminal_limit
          : terminal_cte != nullptr
          ? terminal_cte
          : (terminal_project != nullptr
                 ? terminal_project
                 : (terminal_filter != nullptr ? terminal_filter : join));
  // CTE is schema preserving and owns no public output records.
  const auto* publication_node =
      terminal_limit != nullptr
          ? terminal_limit
          : terminal_project != nullptr
          ? terminal_project
          : (terminal_filter != nullptr ? terminal_filter : join);

  std::size_t streaming_hash_left_key_ordinal = 0;
  std::size_t streaming_hash_right_key_ordinal = 0;
  std::uint32_t streaming_hash_key_expression_id = 0;
  const auto* streaming_hash_left_scan =
      join->input_node_ids.size() == 2 ? node_for(join->input_node_ids[0])
                                       : nullptr;
  const auto* streaming_hash_right_scan =
      join->input_node_ids.size() == 2 ? node_for(join->input_node_ids[1])
                                       : nullptr;
  const BoundHeapJoinNode* streaming_hash_join =
      bound_joins.size() == 1 ? &bound_joins.front() : nullptr;
  bool streaming_hash_profile =
      scans.size() == 2 && joins.size() == 1 &&
      streaming_hash_join != nullptr &&
      streaming_hash_join->kind ==
          exec::CanonicalAcceptedJoinKind::kInner &&
      streaming_hash_left_scan != nullptr &&
      streaming_hash_right_scan != nullptr &&
      streaming_hash_left_scan->node_kind ==
          api::RelationalDagNodeKind::kScan &&
      streaming_hash_right_scan->node_kind ==
          api::RelationalDagNodeKind::kScan &&
      local_filters.empty() && local_projects.empty() && local_ctes.empty() &&
      terminal_filter == nullptr && terminal_limit == nullptr &&
      terminal_cte == nullptr &&
      projects.size() == static_cast<std::size_t>(terminal_project != nullptr);
  if (streaming_hash_profile) {
    const auto expression_for = [&](const std::uint32_t expression_id) {
      const auto found = std::ranges::find_if(
          dag.expressions, [&](const auto& candidate) {
            return candidate.expression_id == expression_id;
          });
      return found == dag.expressions.end() ? nullptr : &*found;
    };
    const auto slot_for = [&](const std::uint32_t expression_id)
        -> std::optional<std::size_t> {
      const auto found = std::ranges::find_if(
          streaming_hash_join->predicate_row_binding.slots,
          [&](const auto& slot) {
            return slot.expression_id == expression_id &&
                   slot.slot_kind ==
                       CanonicalRelationalExpressionRowSlotKind::input_identifier;
          });
      return found == streaming_hash_join->predicate_row_binding.slots.end()
                 ? std::nullopt
                 : std::optional<std::size_t>{found->row_ordinal};
    };
    std::vector<std::uint32_t> pending{
        streaming_hash_join->node->bound_expression_ids.front()};
    bool exact_key_found = false;
    while (!pending.empty() && !exact_key_found) {
      const auto* expression = expression_for(pending.back());
      pending.pop_back();
      if (expression == nullptr || !expression->operator_name.has_value()) {
        continue;
      }
      if (*expression->operator_name == "AND" &&
          expression->child_expression_ids.size() == 2) {
        pending.insert(pending.end(), expression->child_expression_ids.begin(),
                       expression->child_expression_ids.end());
        continue;
      }
      if (*expression->operator_name != "=" ||
          expression->child_expression_ids.size() != 2) {
        continue;
      }
      const auto left_slot = slot_for(expression->child_expression_ids[0]);
      const auto right_slot = slot_for(expression->child_expression_ids[1]);
      if (!left_slot.has_value() || !right_slot.has_value()) continue;
      const auto left_width =
          streaming_hash_left_scan->output_descriptor_ids.size();
      std::size_t candidate_left = 0;
      std::size_t candidate_right = 0;
      if (*left_slot < left_width && *right_slot >= left_width) {
        candidate_left = *left_slot;
        candidate_right = *right_slot - left_width;
      } else if (*right_slot < left_width && *left_slot >= left_width) {
        candidate_left = *right_slot;
        candidate_right = *left_slot - left_width;
      } else {
        continue;
      }
      if (candidate_right >=
          streaming_hash_right_scan->output_descriptor_ids.size()) {
        continue;
      }
      const auto left_descriptor = std::ranges::find_if(
          dag.descriptors, [&](const auto& descriptor) {
            return descriptor.descriptor_id ==
                   streaming_hash_left_scan
                       ->output_descriptor_ids[candidate_left];
          });
      const auto right_descriptor = std::ranges::find_if(
          dag.descriptors, [&](const auto& descriptor) {
            return descriptor.descriptor_id ==
                   streaming_hash_right_scan
                       ->output_descriptor_ids[candidate_right];
          });
      if (left_descriptor == dag.descriptors.end() ||
          right_descriptor == dag.descriptors.end() ||
          ExactBoundedSignedIntegerTypeRankForComposition(left_descriptor->type_uuid) == 0 ||
          ExactBoundedSignedIntegerTypeRankForComposition(right_descriptor->type_uuid) == 0) {
        continue;
      }
      streaming_hash_left_key_ordinal = candidate_left;
      streaming_hash_right_key_ordinal = candidate_right;
      streaming_hash_key_expression_id = expression->expression_id;
      exact_key_found = true;
    }
    streaming_hash_profile = exact_key_found;
  }

  const auto admission = api::BuildCanonicalCurrentHeapOptimizerAdmission(
      {input.context, input.relational_dag});
  if (!admission.built || !admission.admission.admitted ||
      !admission.admission.planning_allowed ||
      admission.admission.data_access_allowed) {
    return refuse(
        admission.issue.diagnostic_id.empty()
            ? "QOW-DIAG-PACKET7-OBJECT-HEAP-CROSS-JOIN-ADMISSION-V1"
            : admission.issue.diagnostic_id,
        admission.issue.field_id.empty()
            ? "current object-backed heap CROSS JOIN admission failed"
            : admission.issue.field_id);
  }
  result.optimizer_admitted = true;
  result.optimizer_admission_degraded =
      admission.admission.degraded_for_unknown_statistics;
  result.optimizer_benchmark_clean_ready =
      admission.admission.benchmark_clean_ready;
  result.optimizer_admission_stage_count =
      admission.admission.evidence.size();

  CanonicalObjectFreeValuesExecutionRequest planning_request{
      input.context, input.relational_dag, admission.request,
      admission.admission};
  const auto& graph = admission.request.logical_graph;
  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + input.context.statement_uuid;
  const auto scan_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "heap-join-tree-scan.capability");
  for (auto& bound : bound_joins) {
    bound.implementation_id =
        streaming_hash_profile && bound.node == join
            ? "join.hash-inner.int64-equality.v1"
            : "join." + bound.component + ".3vl.nested.v1";
    bound.capability_uuid = DerivedCanonicalUuid(
        identity_scope, "heap-" + bound.component + ".capability");
    if (bound.capability_uuid.empty()) {
      return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-JOIN-TREE-V1",
                    "object-backed join capability identity is unavailable");
    }
  }
  const auto join_memory_grant =
      input.context.optimizer_memory_budget_bytes;
  if (join_memory_grant == 0 ||
      join_memory_grant >
          planning_request.optimizer_request.resource.memory_budget_bytes ||
      join_memory_grant >
          std::numeric_limits<std::size_t>::max()) {
    return refuse(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "object-backed heap JOIN memory grant is absent or outside the "
        "optimizer admission domain");
  }
  std::vector<LivePhysicalNodeProfile> profiles;
  for (const auto* scan : scans) {
    LivePhysicalNodeProfile profile;
    profile.logical_node_id = scan->node_id;
    profile.implementation_id = "scan.heap.v1";
    profile.capability_uuid = scan_capability_uuid;
    profile.logical_node_kind =
        plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
    profile.physical_node_kind = exec::PhysicalNodeKind::kScan;
    profile.transformation_rule_id = "canonical.heap.scan.v1";
    profile.estimated_rows = 1;
    profile.memory_bytes_required = join_memory_grant;
    profile.minimum_input_count = 0;
    profile.maximum_input_count = 0;
    profile.page_read_sequential_units = 1;
    profile.mga_visibility_checks_expected = 1;
    profile.storage_read_capable = true;
    profile.mga_visibility_capable = true;
    profiles.push_back(std::move(profile));
  }
  for (const auto& bound : bound_joins) {
    profiles.push_back(
        {bound.node->node_id, bound.implementation_id, bound.capability_uuid,
         plan::CanonicalLogicalRelationalNodeKind::kJoin,
         exec::PhysicalNodeKind::kJoin,
         streaming_hash_profile && bound.node == join
             ? "canonical.heap.join.hash-inner.int64-equality.v1"
             : "canonical.heap.join." + bound.component + ".v1", 1,
         join_memory_grant, 2, 2});
    profiles.back().runtime_peak_from_callback_batches = true;
  }
  std::string filter_capability_uuid;
  if (!filters.empty()) {
    filter_capability_uuid =
        DerivedCanonicalUuid(identity_scope, "heap-join-filter.capability");
    for (const auto* filter : filters) {
      profiles.push_back(
          {filter->node_id, "filter.3vl.row.v1", filter_capability_uuid,
           plan::CanonicalLogicalRelationalNodeKind::kFilter,
           exec::PhysicalNodeKind::kFilter,
           filter == terminal_filter
               ? "canonical.heap.join-tail.filter.v1"
               : (join_subtree_filter_node_ids.contains(filter->node_id)
                      ? "canonical.heap.join-subtree.filter.v1"
                      : "canonical.heap.join-input.filter.v1"),
           1, join_memory_grant, 1, 1});
      profiles.back().runtime_peak_from_callback_batches = true;
    }
  }
  std::string project_capability_uuid;
  if (!projects.empty()) {
    project_capability_uuid = DerivedCanonicalUuid(
        identity_scope, "heap-join-project.capability");
    for (const auto* project : projects) {
      profiles.push_back(
          {project->node_id, "project.descriptor-direct.v1",
           project_capability_uuid,
           plan::CanonicalLogicalRelationalNodeKind::kProject,
           exec::PhysicalNodeKind::kProject,
           project == terminal_project
               ? "canonical.heap.join-tail.project.visible-columns.v1"
               : (join_subtree_project_node_ids.contains(project->node_id)
                      ? "canonical.heap.join-subtree.project.visible-columns.v1"
                      : "canonical.heap.join-input.project.visible-columns.v1"),
           1, join_memory_grant, 1, 1});
      profiles.back().runtime_peak_from_callback_batches = true;
    }
  }
  std::string inline_cte_capability_uuid;
  std::string materialized_cte_capability_uuid;
  if (!ctes.empty()) {
    inline_cte_capability_uuid = DerivedCanonicalUuid(
        identity_scope, "heap-join-cte-inline.capability");
    materialized_cte_capability_uuid = DerivedCanonicalUuid(
        identity_scope, "heap-join-cte-materialized.capability");
    for (const auto* cte : ctes) {
      const auto implementation_id =
          cte->shareable ? "cte.bound.materialize.typed.v1"
                         : "cte.bound.inline.typed.v1";
      const auto& capability_uuid = cte->shareable
                                        ? materialized_cte_capability_uuid
                                        : inline_cte_capability_uuid;
      const bool terminal = cte == terminal_cte;
      const bool join_subtree =
          join_subtree_cte_node_ids.contains(cte->node_id);
      profiles.push_back(
          {cte->node_id, implementation_id, capability_uuid,
           plan::CanonicalLogicalRelationalNodeKind::kCte,
           exec::PhysicalNodeKind::kCte,
           terminal
               ? (cte->shareable
                      ? "canonical.heap.join-tail.cte.materialize.v1"
                      : "canonical.heap.join-tail.cte.inline.v1")
               : (join_subtree
                      ? (cte->shareable
                             ? "canonical.heap.join-subtree.cte.materialize.v1"
                             : "canonical.heap.join-subtree.cte.inline.v1")
                      : (cte->shareable
                             ? "canonical.heap.join-input.cte.materialize.v1"
                             : "canonical.heap.join-input.cte.inline.v1")),
           1, join_memory_grant, 1, 1});
      profiles.back().runtime_peak_from_callback_batches = true;
      profiles.back().runtime_auxiliary_from_first_input_batch =
          cte->shareable;
    }
  }
  std::string limit_capability_uuid;
  if (terminal_limit != nullptr) {
    limit_capability_uuid =
        DerivedCanonicalUuid(identity_scope, "heap-join-limit.capability");
    profiles.push_back(
        {terminal_limit->node_id, "limit.typed.v1", limit_capability_uuid,
         plan::CanonicalLogicalRelationalNodeKind::kLimit,
         exec::PhysicalNodeKind::kLimit,
         terminal_limit->semantic_variant_id == "limit.bound-count.v1"
             ? "canonical.heap.join-tail.limit.bound-count.v1"
             : "canonical.heap.join-tail.limit.bound-count-offset.v1",
         1,
         join_memory_grant, 1, 1});
    profiles.back().runtime_peak_from_callback_batches = true;
  }
  if (!CompleteLiveRuntimeMemoryReceipts(&profiles)) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "heap JOIN-tail runtime memory receipts are incomplete");
  }
  const auto physical = PlanAndPublishLivePhysicalDag(
      planning_request, profiles,
      terminal_limit != nullptr
          ? "heap-join-tail-limit.selected-plan"
          : terminal_cte != nullptr
          ? "heap-join-tail-cte.selected-plan"
          : (terminal_project != nullptr
                 ? "heap-join-tail-project.selected-plan"
                 : (terminal_filter != nullptr
                        ? "heap-join-tail-filter.selected-plan"
                        : (!local_ctes.empty()
                               ? "heap-join-input-cte.selected-plan"
                               : (!local_projects.empty()
                                      ? "heap-join-input-project.selected-plan"
                                      : (!local_filters.empty()
                                             ? "heap-join-input-filter.selected-plan"
                                             : "heap-join-tree.selected-plan"))))),
      "object-backed heap join tree with unary tail");
  if (!physical.ok) {
    return refuse(
        physical.diagnostic_id.empty()
            ? "QOW-DIAG-PACKET7-OBJECT-HEAP-CROSS-JOIN-PLANNING-V1"
            : physical.diagnostic_id,
        physical.detail.empty()
            ? "object-backed heap join tree physical DAG was not published"
            : physical.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = physical.physical_dag.nodes.size();
  result.selected_plan_uuid = physical.physical_dag.selected_plan_uuid;

  const auto bounded_size = [](const std::uint64_t value) {
    return static_cast<std::size_t>(std::min<std::uint64_t>(
        value, std::numeric_limits<std::size_t>::max()));
  };
  const auto maximum_scanned_row_versions = bounded_size(std::min(
      input.context.optimizer_maximum_search_steps,
      input.context.optimizer_maximum_candidate_count));
  const auto maximum_decoded_bytes =
      bounded_size(input.context.optimizer_memory_budget_bytes);
  const auto maximum_output_rows =
      bounded_size(input.context.optimizer_maximum_candidate_count);
  const auto widest_node = std::ranges::max_element(
      dag.nodes, {}, [](const auto& node) {
        return node.output_descriptor_ids.size();
      });
  const auto maximum_output_columns =
      widest_node == dag.nodes.end() ? 0 : widest_node->output_descriptor_ids.size();
  std::uint64_t maximum_pair_count_u64 = 0;
  if (maximum_scanned_row_versions == 0 || maximum_decoded_bytes == 0 ||
      maximum_output_rows == 0 || maximum_output_columns == 0 ||
      maximum_output_rows >
          std::numeric_limits<std::size_t>::max() / maximum_output_columns ||
      !CheckedMultiply(maximum_output_rows, maximum_output_rows,
                       &maximum_pair_count_u64) ||
      maximum_pair_count_u64 >
          std::numeric_limits<std::size_t>::max()) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "object-backed heap CROSS JOIN bounds are absent or overflow");
  }
  const auto maximum_pair_count =
      static_cast<std::size_t>(maximum_pair_count_u64);

  exec::CanonicalHeapPhysicalDagDispatchRequest heap_request;
  heap_request.context = &input.context;
  heap_request.relational_dag = &input.relational_dag;
  heap_request.borrowed_physical_dag = &physical.physical_dag;
  heap_request.heap_read_authority_cohort = admission.authority_cohort;
  heap_request.maximum_scanned_row_versions = maximum_scanned_row_versions;
  heap_request.maximum_decoded_bytes = maximum_decoded_bytes;
  heap_request.maximum_output_rows = maximum_output_rows;
  heap_request.maximum_output_columns = maximum_output_columns;
  heap_request.maximum_output_cells =
      maximum_output_rows * maximum_output_columns;
  static const std::function<bool()> kNeverCancelHeapJoin = [] {
    return false;
  };
  heap_request.borrowed_cancellation_requested =
      input.context.query_cancellation_requested
          ? &input.context.query_cancellation_requested
          : &kNeverCancelHeapJoin;
  auto heap_registration =
      exec::BuildCanonicalHeapPhysicalRegistration(heap_request);
  if (!heap_registration.diagnostic.ok ||
      !heap_registration.registration.has_value() ||
      heap_registration.mga_authority == nullptr) {
    return refuse(
        heap_registration.diagnostic.diagnostic_code.empty()
            ? "QOW-DIAG-PACKET7-OBJECT-HEAP-CROSS-JOIN-REGISTRATION-V1"
            : heap_registration.diagnostic.diagnostic_code,
        heap_registration.diagnostic.detail.empty()
            ? "object-backed heap CROSS JOIN scan registration is unavailable"
            : heap_registration.diagnostic.detail);
  }

  std::vector<const api::RelationalOutputRecord*> ordered_outputs;
  for (const auto& output : dag.outputs) {
    if (output.relation_node_id == publication_node->node_id) {
      ordered_outputs.push_back(&output);
    }
  }
  std::ranges::sort(ordered_outputs, {},
                    &api::RelationalOutputRecord::ordinal);
  if (ordered_outputs.size() != publication_node->output_descriptor_ids.size()) {
    return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-CROSS-JOIN-BINDING-V1",
                  "object-backed CROSS JOIN result bindings are incomplete");
  }
  std::unordered_set<std::uint32_t> root_output_ids;
  for (std::size_t ordinal = 0; ordinal < ordered_outputs.size(); ++ordinal) {
    const auto& output = *ordered_outputs[ordinal];
    if (!output.visible || output.ordinal != ordinal || output.output_id == 0 ||
        !root_output_ids.insert(output.output_id).second ||
        output.descriptor_id !=
            publication_node->output_descriptor_ids[ordinal]) {
      return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-CROSS-JOIN-BINDING-V1",
                    "object-backed join result lineage is not exact");
    }
  }

  // The generic selected-DAG dispatcher deliberately materializes every
  // physical input batch.  That is the correct fail-closed implementation for
  // arbitrary join trees, but it is not a legal implementation of the exact
  // two-source integer equality profile above: the two complete typed batches,
  // their Cartesian proof vector, and the result batch coexist.  Execute this
  // one admitted profile through the MGA visible-row stream instead.  The
  // stream owns visibility/chain/extent authority; QOW owns only typed binding,
  // hash lookup, residual predicate evaluation, and bounded cursor publication.
  if (streaming_hash_profile) {
    struct StreamingHashScanBinding {
      const api::RelationalDagNode* node{nullptr};
      std::string relation_uuid;
      api::MgaRelationStorageDescriptor persisted;
      std::vector<const api::MgaRelationColumnStorageDescriptor*> columns;
      std::vector<api::EngineDescriptor> descriptors;
    };
    struct StreamingCompactRow {
      std::uint64_t source_ordinal{0};
      std::int64_t key{0};
      bool key_present{false};
      std::vector<std::string> values;
      std::vector<bool> nulls;
    };
    struct StreamingHashEntry {
      std::int64_t key{0};
      std::uint64_t source_ordinal{0};
      std::size_t row_ordinal{0};
    };

    const auto add_memory = [](const std::uint64_t value,
                               std::uint64_t* total) {
      if (total == nullptr ||
          value > std::numeric_limits<std::uint64_t>::max() - *total) {
        return false;
      }
      *total += value;
      return true;
    };
    const auto multiply_memory = [](const std::uint64_t count,
                                    const std::uint64_t width,
                                    std::uint64_t* product) {
      if (product == nullptr ||
          (width != 0 &&
           count > std::numeric_limits<std::uint64_t>::max() / width)) {
        return false;
      }
      *product = count * width;
      return true;
    };
    const auto account_string = [&](const std::string& value,
                                    std::uint64_t* total) {
      return value.capacity() <
                 std::numeric_limits<std::uint64_t>::max() &&
             add_memory(static_cast<std::uint64_t>(value.capacity()) + 1,
                        total);
    };
    const auto account_descriptor = [&](const api::EngineDescriptor& descriptor,
                                        std::uint64_t* total) {
      return account_string(descriptor.descriptor_uuid, total) &&
             account_string(descriptor.descriptor_kind, total) &&
             account_string(descriptor.canonical_type_name, total) &&
             account_string(descriptor.encoded_descriptor, total);
    };
    const auto account_scan_binding = [&](const StreamingHashScanBinding& binding,
                                          std::uint64_t* total) {
      std::uint64_t allocation = 0;
      if (!account_string(binding.relation_uuid, total) ||
          !account_string(binding.persisted.descriptor_uuid, total) ||
          !account_string(binding.persisted.database_uuid, total) ||
          !account_string(binding.persisted.schema_uuid, total) ||
          !account_string(binding.persisted.relation_uuid, total) ||
          !account_string(binding.persisted.primary_filespace_uuid,
                          total) ||
          !account_string(binding.persisted.relation_kind, total) ||
          !account_string(binding.persisted.storage_profile, total) ||
          !multiply_memory(binding.persisted.columns.capacity(),
                           sizeof(api::MgaRelationColumnStorageDescriptor),
                           &allocation) ||
          !add_memory(allocation, total) ||
          !multiply_memory(binding.columns.capacity(),
                           sizeof(const api::MgaRelationColumnStorageDescriptor*),
                           &allocation) ||
          !add_memory(allocation, total) ||
          !multiply_memory(binding.descriptors.capacity(),
                           sizeof(api::EngineDescriptor), &allocation) ||
          !add_memory(allocation, total)) {
        return false;
      }
      for (const auto& column : binding.persisted.columns) {
        if (!account_string(column.column_uuid, total) ||
            !account_string(column.canonical_name_key, total) ||
            !account_descriptor(column.value_descriptor, total) ||
            !account_string(column.storage_class, total) ||
            !account_string(column.charset_uuid, total) ||
            !account_string(column.collation_uuid, total) ||
            !account_string(column.overflow_policy, total)) {
          return false;
        }
      }
      for (const auto& descriptor : binding.descriptors) {
        if (!account_descriptor(descriptor, total)) return false;
      }
      return true;
    };
    const auto compact_memory = [&](const StreamingHashScanBinding& left_binding,
                                    const StreamingHashScanBinding& right_binding,
                                    const std::vector<StreamingCompactRow>& left_rows,
                                    const std::vector<StreamingCompactRow>& right_rows,
                                    const std::vector<StreamingHashEntry>& index,
                                    std::uint64_t* total) {
      if (total == nullptr) return false;
      *total = sizeof(left_rows) + sizeof(right_rows) + sizeof(index) +
               sizeof(left_binding) + sizeof(right_binding);
      std::uint64_t allocation = 0;
      if (!account_scan_binding(left_binding, total) ||
          !account_scan_binding(right_binding, total) ||
          !multiply_memory(left_rows.capacity(), sizeof(StreamingCompactRow),
                           &allocation) ||
          !add_memory(allocation, total) ||
          !multiply_memory(right_rows.capacity(), sizeof(StreamingCompactRow),
                           &allocation) ||
          !add_memory(allocation, total) ||
          !multiply_memory(index.capacity(), sizeof(StreamingHashEntry),
                           &allocation) ||
          !add_memory(allocation, total)) {
        return false;
      }
      const auto account_rows = [&](const auto& rows) {
        for (const auto& row : rows) {
          if (!multiply_memory(row.values.capacity(), sizeof(std::string),
                               &allocation) ||
              !add_memory(allocation, total) ||
              !add_memory((row.nulls.capacity() + 7) / 8, total)) {
            return false;
          }
          for (const auto& value : row.values) {
            if (!account_string(value, total)) return false;
          }
        }
        return true;
      };
      return account_rows(left_rows) && account_rows(right_rows);
    };
    const auto compact_row_dynamic_memory =
        [&](const StreamingCompactRow& row, std::uint64_t* total) {
          if (total == nullptr) return false;
          *total = 0;
          std::uint64_t allocation = 0;
          if (!multiply_memory(row.values.capacity(), sizeof(std::string),
                               &allocation) ||
              !add_memory(allocation, total) ||
              !add_memory((row.nulls.capacity() + 7) / 8, total)) {
            return false;
          }
          for (const auto& value : row.values) {
            if (!account_string(value, total)) return false;
          }
          return true;
        };

    const auto relational_descriptor_for = [&](const std::uint32_t id) {
      const auto found = std::ranges::find_if(
          dag.descriptors,
          [&](const auto& candidate) { return candidate.descriptor_id == id; });
      return found == dag.descriptors.end() ? nullptr : &*found;
    };
    const auto expression_for = [&](const std::uint32_t id) {
      const auto found = std::ranges::find_if(
          dag.expressions,
          [&](const auto& candidate) { return candidate.expression_id == id; });
      return found == dag.expressions.end() ? nullptr : &*found;
    };
    const auto prepare_scan_binding = [&](const api::RelationalDagNode& scan,
                                          api::MgaRelationStorageDescriptor descriptor,
                                          StreamingHashScanBinding* binding,
                                          std::string* detail) {
      if (binding == nullptr || detail == nullptr ||
          scan.required_object_uuids.size() != 1 ||
          descriptor.relation_uuid !=
              scan.required_object_uuids.front() ||
          descriptor.descriptor_generation == 0 ||
          descriptor.columns.empty()) {
        if (detail != nullptr) {
          *detail = "streaming hash scan descriptor authority is incomplete";
        }
        return false;
      }
      StreamingHashScanBinding prepared;
      prepared.node = &scan;
      prepared.relation_uuid = scan.required_object_uuids.front();
      prepared.persisted = std::move(descriptor);
      prepared.columns.reserve(scan.output_descriptor_ids.size());
      prepared.descriptors.reserve(scan.output_descriptor_ids.size());
      std::unordered_set<std::string> projected_column_uuids;
      for (std::size_t ordinal = 0;
           ordinal < scan.output_descriptor_ids.size(); ++ordinal) {
        const auto* expression =
            expression_for(scan.bound_expression_ids[ordinal]);
        const auto* relational_descriptor =
            relational_descriptor_for(scan.output_descriptor_ids[ordinal]);
        const auto output = std::ranges::find_if(
            dag.outputs, [&](const auto& candidate) {
              return candidate.relation_node_id == scan.node_id &&
                     candidate.ordinal == ordinal;
            });
        if (expression == nullptr || relational_descriptor == nullptr ||
            output == dag.outputs.end() ||
            expression->expression_kind !=
                api::RelationalExpressionKind::kIdentifier ||
            !expression->bound_name_uuid.has_value() ||
            expression->result_descriptor_id !=
                relational_descriptor->descriptor_id ||
            !projected_column_uuids.insert(*expression->bound_name_uuid).second ||
            !output->visible || output->descriptor_id !=
                                    relational_descriptor->descriptor_id) {
          *detail = "streaming hash scan relational binding is not bijective";
          return false;
        }
        const auto column = std::ranges::find_if(
            prepared.persisted.columns, [&](const auto& candidate) {
              return candidate.column_uuid ==
                     *expression->bound_name_uuid;
            });
        if (column == prepared.persisted.columns.end()) {
          *detail =
              "streaming hash projected column is absent from persisted descriptor";
          return false;
        }
        const bool nullable = relational_descriptor->nullability ==
                              api::RelationalNullability::kNullable;
        const auto expected_collation =
            relational_descriptor->collation_uuid.has_value()
                ? std::optional<std::string_view>{
                      *relational_descriptor->collation_uuid}
                : std::nullopt;
        const auto expected_timezone =
            relational_descriptor->timezone_profile_id.has_value()
                ? std::optional<std::string_view>{
                      *relational_descriptor->timezone_profile_id}
                : std::nullopt;
        const auto canonical_nullability =
            CanonicalDescriptorFieldEqualsForComposition(
                column->value_descriptor, "nullability",
                std::optional<std::string_view>{
                    nullable ? "nullable" : "non_null"});
        const auto storage_nullability =
            CanonicalDescriptorFieldEqualsForComposition(
                column->value_descriptor, "nullable",
                std::optional<std::string_view>{nullable ? "true" : "false"});
        const auto canonical_nullability_absent =
            CanonicalDescriptorFieldEqualsForComposition(
                column->value_descriptor, "nullability", std::nullopt);
        const auto storage_nullability_absent =
            CanonicalDescriptorFieldEqualsForComposition(
                column->value_descriptor, "nullable", std::nullopt);
        const bool exact_nullability_carrier =
            (canonical_nullability && storage_nullability_absent) ||
            (canonical_nullability_absent && storage_nullability) ||
            (canonical_nullability && storage_nullability);
        if (column->ordinal >= prepared.persisted.columns.size() ||
            output->output_name_utf8 != column->canonical_name_key ||
            column->value_descriptor.descriptor_uuid !=
                relational_descriptor->descriptor_uuid ||
            column->value_descriptor.canonical_type_name.empty() ||
            column->nullable != nullable ||
            !CanonicalDescriptorFieldEqualsForComposition(
                column->value_descriptor, "type_uuid",
                std::optional<std::string_view>{
                    relational_descriptor->type_uuid}) ||
            !exact_nullability_carrier ||
            !CanonicalDescriptorFieldEqualsForComposition(
                column->value_descriptor, "collation_uuid",
                expected_collation) ||
            !CanonicalDescriptorFieldEqualsForComposition(
                column->value_descriptor, "timezone_profile_id",
                expected_timezone) ||
            (relational_descriptor->collation_uuid.has_value()
                 ? column->collation_uuid !=
                       *relational_descriptor->collation_uuid
                 : !column->collation_uuid.empty())) {
          *detail =
              "streaming hash scan persisted descriptor differs from binding";
          return false;
        }
        prepared.columns.push_back(&*column);
        prepared.descriptors.push_back(column->value_descriptor);
        prepared.descriptors.back().descriptor_kind = "scalar";
      }
      *binding = std::move(prepared);
      return true;
    };

    const auto& cancellation_requested =
        input.context.query_cancellation_requested
            ? input.context.query_cancellation_requested
            : kNeverCancelHeapJoin;
    if (cancellation_requested()) {
      return refuse("QOW-DIAG-QRY-012-JOIN-CANCELLED-V1",
                    "streaming hash join cancelled before source admission");
    }
    const auto inventory_guard =
        api::AcquireTransactionInventoryGuard(input.context.database_path);
    const auto entry_authority = exec::RevalidateCanonicalExecutionMgaAuthority(
        *heap_registration.mga_authority, physical.physical_dag);
    if (!entry_authority.ok) {
      return refuse(entry_authority.diagnostic_code, entry_authority.detail);
    }

    const auto authorize = [&](const std::string& relation_uuid,
                               std::string* detail) {
      const auto authorization = api::EvaluateMaterializedAuthorization(
          input.context, input.context.authorization_context, "SELECT",
          relation_uuid);
      if (!authorization.authorized || authorization.denied ||
          authorization.policy_recheck_required ||
          !authorization.diagnostics.empty()) {
        *detail = authorization.diagnostics.empty()
                      ? "streaming hash SELECT authorization is indeterminate"
                      : authorization.diagnostics.front().detail;
        return false;
      }
      return true;
    };
    std::string stream_detail;
    if (!authorize(streaming_hash_left_scan->required_object_uuids.front(),
                   &stream_detail) ||
        !authorize(streaming_hash_right_scan->required_object_uuids.front(),
                   &stream_detail)) {
      return refuse("QOW-DIAG-QRY-004-SCAN-SECURITY-DECISION-V1",
                    stream_detail);
    }

    StreamingHashScanBinding left_binding;
    StreamingHashScanBinding right_binding;
    std::vector<StreamingCompactRow> left_rows;
    std::vector<StreamingCompactRow> right_rows;
    std::vector<StreamingHashEntry> build_index;
    std::uint64_t retained_memory = 0;
    std::vector<std::pair<std::string, std::string>>
        left_descriptor_authority;
    std::vector<std::pair<std::string, std::string>>
        right_descriptor_authority;
    std::uint64_t left_expected_visible_rows = 0;
    std::uint64_t right_expected_visible_rows = 0;
    bool stream_preparation_resource_refusal = false;
    bool stream_preparation_descriptor_refusal = false;

    auto left_descriptor_load = api::LoadMgaRelationStorageDescriptor(
        input.context,
        streaming_hash_left_scan->required_object_uuids.front());
    auto right_descriptor_load = api::LoadMgaRelationStorageDescriptor(
        input.context,
        streaming_hash_right_scan->required_object_uuids.front());
    if (!left_descriptor_load.ok || !right_descriptor_load.ok ||
        !prepare_scan_binding(*streaming_hash_left_scan,
                              std::move(left_descriptor_load.descriptor),
                              &left_binding, &stream_detail) ||
        !prepare_scan_binding(*streaming_hash_right_scan,
                              std::move(right_descriptor_load.descriptor),
                              &right_binding, &stream_detail)) {
      return refuse(
          "SB_DIAG_MGA_READ_RELATION_DESCRIPTOR_INVALID",
          !stream_detail.empty()
              ? stream_detail
              : (!left_descriptor_load.ok
                     ? left_descriptor_load.diagnostic.detail
                     : right_descriptor_load.diagnostic.detail));
    }
    left_descriptor_authority =
        api::SerializeMgaRelationStorageDescriptor(left_binding.persisted);
    right_descriptor_authority =
        api::SerializeMgaRelationStorageDescriptor(right_binding.persisted);

    const auto maximum_row_growth = [&](const StreamingHashScanBinding& binding,
                                        std::uint64_t* growth) {
      if (growth == nullptr) return false;
      *growth = sizeof(StreamingCompactRow);
      std::uint64_t allocation = 0;
      if (!multiply_memory(binding.columns.size(), sizeof(std::string),
                           &allocation) ||
          !add_memory(allocation, growth) ||
          !add_memory((binding.columns.size() + 7) / 8, growth)) {
        return false;
      }
      for (const auto* column : binding.columns) {
        const auto value_bound =
            std::max<std::uint64_t>(column->max_inline_bytes, 64);
        if (!add_memory(value_bound + 1, growth)) return false;
      }
      return true;
    };
    const auto stream_rows = [&](const api::RelationalDagNode& scan,
                                 const std::size_t key_ordinal,
                                 StreamingHashScanBinding* binding,
                                 std::vector<StreamingCompactRow>* rows,
                                 std::uint64_t* expected_visible_rows,
                                 std::vector<std::pair<std::string, std::string>>*
                                     descriptor_authority,
                                 const std::function<bool(
                                     const StreamingCompactRow&)>*
                                     immediate_consumer) {
      api::MgaVisibleHeapRelationStreamRequest request;
      request.borrowed_relation_uuid = &scan.required_object_uuids.front();
      request.maximum_decoded_bytes_per_pass = join_memory_grant;
      request.maximum_memory_bytes = join_memory_grant;
      request.borrowed_cancellation_requested = &cancellation_requested;
      request.prepare_consumer_for_visible_rows =
          [&](const api::MgaRelationStorageDescriptor& descriptor,
              const std::uint64_t visible_rows,
              std::uint64_t* maximum_growth) {
            if (binding == nullptr || rows == nullptr ||
                expected_visible_rows == nullptr ||
                descriptor_authority == nullptr || maximum_growth == nullptr ||
                visible_rows > std::numeric_limits<std::size_t>::max()) {
              stream_preparation_resource_refusal = true;
              stream_detail =
                  "streaming hash source cardinality exceeds size_t";
              return false;
            }
            if (descriptor.relation_uuid != binding->relation_uuid ||
                api::SerializeMgaRelationStorageDescriptor(descriptor) !=
                    *descriptor_authority) {
              stream_preparation_descriptor_refusal = true;
              stream_detail =
                  "streaming hash descriptor changed before value delivery";
              return false;
            }
            if (!maximum_row_growth(*binding, maximum_growth) ||
                *maximum_growth > join_memory_grant) {
              stream_preparation_resource_refusal = true;
              stream_detail =
                  "streaming hash row growth bound is unavailable";
              return false;
            }
            std::uint64_t current_bytes = 0;
            std::uint64_t reserve_bytes = 0;
            if (!compact_memory(left_binding, right_binding, left_rows,
                                right_rows, build_index, &current_bytes) ||
                (immediate_consumer == nullptr &&
                 (!multiply_memory(visible_rows, sizeof(StreamingCompactRow),
                                   &reserve_bytes) ||
                  !add_memory(reserve_bytes, &current_bytes))) ||
                current_bytes > join_memory_grant) {
              stream_preparation_resource_refusal = true;
              stream_detail =
                  "streaming hash compact row reservation exceeds the join grant";
              return false;
            }
            if (immediate_consumer == nullptr) {
              try {
                rows->reserve(static_cast<std::size_t>(visible_rows));
              } catch (...) {
                stream_preparation_resource_refusal = true;
                stream_detail =
                    "streaming hash compact row reservation failed";
                return false;
              }
            }
            if (!compact_memory(left_binding, right_binding, left_rows,
                                right_rows, build_index, &current_bytes) ||
                current_bytes > join_memory_grant) {
              stream_preparation_resource_refusal = true;
              stream_detail =
                  "streaming hash compact row reservation exceeds the join grant";
              return false;
            }
            retained_memory = current_bytes;
            *expected_visible_rows = visible_rows;
            return true;
          };
      request.consumer_retained_memory_bytes = [&] {
        return retained_memory;
      };
      request.consume_visible_row =
          [&](const std::uint64_t source_ordinal,
              const api::CrudRowVersionRecord& stored) {
            if (binding == nullptr || rows == nullptr ||
                stored.values.size() != binding->persisted.columns.size() ||
                key_ordinal >= binding->columns.size()) {
              return false;
            }
            StreamingCompactRow row;
            row.source_ordinal = source_ordinal;
            row.values.resize(binding->columns.size());
            row.nulls.resize(binding->columns.size(), false);
            for (std::size_t ordinal = 0; ordinal < binding->columns.size();
                 ++ordinal) {
              const auto* column = binding->columns[ordinal];
              const std::string* payload = nullptr;
              for (const auto& [name, value] : stored.values) {
                if (name != column->canonical_name_key) continue;
                if (payload != nullptr) return false;
                payload = &value;
              }
              if (payload == nullptr) return false;
              if (*payload == "<NULL>") {
                row.nulls[ordinal] = true;
              } else {
                row.values[ordinal] = *payload;
              }
            }
            if (!row.nulls[key_ordinal]) {
              api::EngineTypedValue key;
              key.descriptor = binding->descriptors[key_ordinal];
              key.encoded_value = row.values[key_ordinal];
              key.state = api::EngineValueState::value;
              const auto decoded = exec::DecodeInt64Value(key);
              if (!decoded.ok()) return false;
              row.key = decoded.value;
              row.key_present = true;
            }
            if (immediate_consumer != nullptr) {
              return (*immediate_consumer)(row);
            }
            std::uint64_t row_dynamic_bytes = 0;
            auto prospective_retained = retained_memory;
            if (!compact_row_dynamic_memory(row, &row_dynamic_bytes) ||
                !add_memory(row_dynamic_bytes, &prospective_retained) ||
                prospective_retained > join_memory_grant) {
              stream_preparation_resource_refusal = true;
              stream_detail =
                  "streaming hash compact row exceeds the join grant";
              return false;
            }
            rows->push_back(std::move(row));
            retained_memory = prospective_retained;
            return true;
          };
      return api::StreamVisibleMgaHeapRelation(input.context, request);
    };
    const auto stream_failure_code = [&](const auto& stream) {
      if (stream.cancellation_observed) {
        return std::string("QOW-DIAG-QRY-012-JOIN-CANCELLED-V1");
      }
      if (stream_preparation_descriptor_refusal) {
        return std::string("SB_DIAG_MGA_READ_RELATION_DESCRIPTOR_INVALID");
      }
      if (stream_preparation_resource_refusal ||
          stream.failure_category ==
              api::MgaHeapReadFailureCategoryV1::kResource) {
        return std::string("SBLR.PLAN_TREE.RESOURCE_LIMIT");
      }
      return stream.diagnostic.code.empty()
                 ? std::string("QOW-DIAG-QRY-004-HEAP-READ-V1")
                 : stream.diagnostic.code;
    };

    auto left_stream = stream_rows(
        *streaming_hash_left_scan, streaming_hash_left_key_ordinal,
        &left_binding, &left_rows, &left_expected_visible_rows,
        &left_descriptor_authority, nullptr);
    if (!left_stream.ok || !left_stream.memory_receipt_complete ||
        !left_stream.complete_mga_chain_validation ||
        !left_stream.exact_segment_extent_revalidated ||
        left_stream.visible_row_count != left_expected_visible_rows ||
        left_stream.visible_row_count != left_rows.size() ||
        api::SerializeMgaRelationStorageDescriptor(left_stream.descriptor) !=
            left_descriptor_authority) {
      return refuse(
          stream_failure_code(left_stream),
          !stream_detail.empty()
              ? stream_detail
              : (left_stream.diagnostic.detail.empty()
              ? "streaming hash left source receipt is incomplete"
              : left_stream.diagnostic.detail));
    }
    std::uint64_t index_reserve_bytes = 0;
    if (!compact_memory(left_binding, right_binding, left_rows, right_rows,
                        build_index, &retained_memory) ||
        !multiply_memory(left_rows.size(), sizeof(StreamingHashEntry),
                         &index_reserve_bytes) ||
        !add_memory(index_reserve_bytes, &retained_memory) ||
        retained_memory > join_memory_grant) {
      return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    "streaming hash index reservation exceeds the join grant");
    }
    try {
      build_index.reserve(left_rows.size());
    } catch (...) {
      return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    "streaming hash index reservation failed");
    }
    for (std::size_t ordinal = 0; ordinal < left_rows.size(); ++ordinal) {
      if (left_rows[ordinal].key_present) {
        build_index.push_back({left_rows[ordinal].key,
                               left_rows[ordinal].source_ordinal, ordinal});
      }
    }
    std::ranges::sort(build_index, [](const auto& left, const auto& right) {
      if (left.key != right.key) return left.key < right.key;
      if (left.source_ordinal != right.source_ordinal) {
        return left.source_ordinal < right.source_ordinal;
      }
      return left.row_ordinal < right.row_ordinal;
    });
    if (!compact_memory(left_binding, right_binding, left_rows, right_rows,
                        build_index, &retained_memory) ||
        retained_memory > join_memory_grant) {
      return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    "streaming hash index exceeds the join grant");
    }

    std::vector<std::size_t> published_ordinals;
    if (terminal_project == nullptr) {
      published_ordinals.resize(join->output_descriptor_ids.size());
      std::iota(published_ordinals.begin(), published_ordinals.end(), 0);
    } else {
      const auto bound_project = std::ranges::find_if(
          bound_projects, [&](const auto& candidate) {
            return candidate.node == terminal_project;
          });
      if (bound_project == bound_projects.end()) {
        return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-JOIN-TAIL-PROJECT-V1",
                      "streaming hash result projection is unavailable");
      }
      published_ordinals = bound_project->projected_columns;
    }
    if (published_ordinals.size() != ordered_outputs.size()) {
      return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-CROSS-JOIN-BINDING-V1",
                    "streaming hash publication width differs from lineage");
    }

    std::vector<exec::CanonicalResultColumnBinding> column_bindings;
    column_bindings.reserve(ordered_outputs.size());
    exec::DescriptorBatch pending_page;
    pending_page.columns.reserve(ordered_outputs.size());
    for (std::size_t ordinal = 0; ordinal < ordered_outputs.size(); ++ordinal) {
      const auto* descriptor =
          relational_descriptor_for(ordered_outputs[ordinal]->descriptor_id);
      if (descriptor == nullptr) {
        return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-CROSS-JOIN-BINDING-V1",
                      "streaming hash result descriptor is unresolved");
      }
      exec::CanonicalResultColumnDescriptor published;
      published.ordinal = static_cast<std::uint32_t>(ordinal);
      published.name_utf8 = ordered_outputs[ordinal]->output_name_utf8;
      published.descriptor_uuid = descriptor->descriptor_uuid;
      published.type_uuid = descriptor->type_uuid;
      published.nullability = ResultNullability(descriptor->nullability);
      published.collation_uuid = descriptor->collation_uuid;
      published.timezone_profile_id = descriptor->timezone_profile_id;
      column_bindings.push_back({ordinal, true, std::move(published)});
      const auto source_ordinal = published_ordinals[ordinal];
      const auto left_width = left_binding.descriptors.size();
      const auto& runtime_descriptor =
          source_ordinal < left_width
              ? left_binding.descriptors[source_ordinal]
              : right_binding.descriptors[source_ordinal - left_width];
      pending_page.columns.push_back(
          {ordered_outputs[ordinal]->output_name_utf8, runtime_descriptor,
           descriptor->nullability == api::RelationalNullability::kNullable,
           descriptor->descriptor_id});
    }

    CanonicalRelationalExpressionRuntimeServices predicate_services;
    predicate_services.comparison_evaluator =
        [context = &input.context](const api::EngineTypedValue& left,
                                   const api::EngineTypedValue& right,
                                   int* comparison,
                                   std::string* diagnostic_id,
                                   std::string* refusal_detail) {
          return CompareCanonicalRelationalScalarsV1(
              *context, left, right, comparison, diagnostic_id,
              refusal_detail);
        };
    BindCanonicalPersistedRowDescriptorAuthorityForComposition(input.context,
                                                   &predicate_services);
    CanonicalRelationalExpressionRuntime predicate_runtime(
        dag, std::move(predicate_services));
    const auto materialize_row = [](const StreamingHashScanBinding& binding,
                                    const StreamingCompactRow& compact,
                                    std::vector<api::EngineTypedValue>* values) {
      values->clear();
      values->reserve(binding.descriptors.size());
      for (std::size_t ordinal = 0; ordinal < binding.descriptors.size();
           ++ordinal) {
        api::EngineTypedValue value;
        value.descriptor = binding.descriptors[ordinal];
        if (compact.nulls[ordinal]) {
          value.is_null = true;
          value.state = api::EngineValueState::sql_null;
        } else {
          value.encoded_value = compact.values[ordinal];
          value.state = api::EngineValueState::value;
        }
        values->push_back(std::move(value));
      }
    };
    std::vector<api::EngineTypedValue> left_values;
    std::vector<api::EngineTypedValue> right_values;
    left_values.reserve(left_binding.descriptors.size());
    right_values.reserve(right_binding.descriptors.size());

    constexpr std::size_t kStreamingHashPublicationRows = 128;
    pending_page.rows.reserve(kStreamingHashPublicationRows);
    std::shared_ptr<exec::CanonicalResultCursorSession> cursor_session;
    const auto cursor_uuid = DerivedCanonicalUuid(
        identity_scope, "heap-hash-inner-result.cursor");
    const auto execution_attempt_uuid = DerivedCanonicalUuid(
        identity_scope + ":" + input.context.current_monotonic_ns,
        "heap-hash-inner.execution-attempt");
    const auto transaction_effect_uuid = DerivedCanonicalUuid(
        identity_scope + ":" +
            std::to_string(input.context.local_transaction_id),
        "heap-hash-inner.transaction-effect-unchanged");
    std::uint64_t cursor_batch_ordinal = 0;
    std::uint64_t cursor_first_row_ordinal = 0;
    bool initial_cursor_page = true;
    std::uint64_t matched_row_count = 0;
    result.api_result.ok = true;
    result.api_result.operation_id = "query.execute";
    result.api_result.result_shape.result_kind = "rows";
    result.api_result.local_transaction_id =
        input.context.local_transaction_id;
    result.api_result.transaction_uuid = input.context.transaction_uuid;
    result.api_result.embedded_trust_mode_observed =
        input.context.trust_mode == api::EngineTrustMode::embedded_in_process;
    for (const auto& column : pending_page.columns) {
      result.api_result.result_shape.columns.push_back(column.descriptor);
    }
    const auto cancellation_diagnostic = [] {
      return exec::CanonicalResultDiagnosticRecord{
          "QOW-DIAGNOSTIC-INSTANCE-HASH-JOIN-CANCEL",
          "SB_EXECUTION_CANCELLED",
          exec::CanonicalResultDiagnosticSeverity::kError,
          "57014",
          "query.hash_join.cursor.delivery.cancelled",
          {},
          exec::CanonicalResultDiagnosticPhase::kFinalize,
          "result.cursor",
          "cancellation_probe",
          1,
          exec::CanonicalResultTransactionEffect::
              kStatementFailedTransactionUsable,
          exec::CanonicalResultRetryability::kNotRetryable};
    };
    const auto publish_page = [&](const bool final_page) {
      exec::CanonicalResultPublicationRequest publication;
      publication.statement_uuid = input.context.statement_uuid;
      publication.mga_authority = *heap_registration.mga_authority;
      publication.selected_physical_dag = physical.physical_dag;
      publication.selected_catalog_epoch_uuid =
          input.context.catalog_epoch_uuid;
      publication.execution_attempt_uuid = execution_attempt_uuid;
      publication.result_kind = exec::CanonicalResultKind::kCursor;
      publication.invocation_mode =
          exec::CanonicalResultInvocationMode::kDirect;
      publication.physical_output_batch = std::move(pending_page);
      publication.column_bindings = column_bindings;
      publication.cursor_state =
          final_page ? exec::CanonicalResultCursorState::kClosed
                     : exec::CanonicalResultCursorState::kOpen;
      publication.cursor_uuid = cursor_uuid;
      publication.cursor_session = cursor_session;
      publication.cursor_batch_ordinal = cursor_batch_ordinal;
      publication.cursor_first_row_ordinal = cursor_first_row_ordinal;
      publication.transaction_effect_evidence_uuid =
          transaction_effect_uuid;
      publication.maximum_row_count = maximum_output_rows;
      publication.maximum_rows_per_batch =
          kStreamingHashPublicationRows;
      if (initial_cursor_page) {
        publication.cursor_cancellation_requested = cancellation_requested;
        publication.cursor_cancellation_diagnostic =
            cancellation_diagnostic();
        publication.cursor_release = [](const auto) {};
      }
      auto published = exec::PublishCanonicalResultEnvelope(publication);
      pending_page = {};
      pending_page.columns = publication.physical_output_batch.columns;
      pending_page.rows.reserve(kStreamingHashPublicationRows);
      if (!published.published || !published.diagnostic.ok ||
          published.row_stream.rows.size() !=
              publication.physical_output_batch.rows.size() ||
          published.cursor_end_of_stream != final_page) {
        stream_detail = published.diagnostic.detail.empty()
                            ? "streaming hash cursor publication failed"
                            : published.diagnostic.detail;
        return false;
      }
      if (initial_cursor_page) {
        cursor_session = published.cursor_session;
        initial_cursor_page = false;
      }
      cursor_batch_ordinal = published.cursor_next_batch_ordinal;
      cursor_first_row_ordinal = published.cursor_next_row_ordinal;
      result.canonical_result_bytes =
          std::move(published.canonical_envelope_bytes);
      for (auto& row : published.row_stream.rows) {
        api::EngineRowValue api_row;
        for (std::size_t column = 0; column < row.values.size(); ++column) {
          api_row.fields.emplace_back(
              published.envelope.column_descriptors[column].name_utf8,
              std::move(row.values[column]));
        }
        result.api_result.result_shape.rows.push_back(std::move(api_row));
      }
      return true;
    };

    const auto append_match = [&](const StreamingCompactRow& left_row,
                                  const StreamingCompactRow& right_row) {
      if (matched_row_count >= maximum_output_rows ||
          matched_row_count == std::numeric_limits<std::uint64_t>::max()) {
        stream_detail =
            "streaming hash result exceeds the admitted cursor row bound";
        return false;
      }
      if (pending_page.rows.size() == kStreamingHashPublicationRows &&
          !publish_page(false)) {
        return false;
      }
      exec::DescriptorTuple output;
      output.values.reserve(published_ordinals.size());
      for (const auto source_ordinal : published_ordinals) {
        const bool from_left = source_ordinal < left_values.size();
        const auto value_ordinal =
            from_left ? source_ordinal : source_ordinal - left_values.size();
        output.values.push_back(from_left ? left_values[value_ordinal]
                                          : right_values[value_ordinal]);
      }
      pending_page.rows.push_back(std::move(output));
      ++matched_row_count;
      return true;
    };

    bool join_ok = true;
    const std::function<bool(const StreamingCompactRow&)> probe_right_row =
        [&](const StreamingCompactRow& right_row) {
      if (cancellation_requested()) {
        stream_detail = "streaming hash join cancelled while probing";
        join_ok = false;
        return false;
      }
      if (!right_row.key_present) return true;
      materialize_row(right_binding, right_row, &right_values);
      const auto lower = std::lower_bound(
          build_index.begin(), build_index.end(), right_row.key,
          [](const auto& entry, const std::int64_t key) {
            return entry.key < key;
          });
      const auto upper = std::upper_bound(
          lower, build_index.end(), right_row.key,
          [](const std::int64_t key, const auto& entry) {
            return key < entry.key;
          });
      for (auto match = lower; match != upper; ++match) {
        if (cancellation_requested()) {
          stream_detail = "streaming hash join cancelled while matching";
          join_ok = false;
          return false;
        }
        const auto& left_row = left_rows[match->row_ordinal];
        materialize_row(left_binding, left_row, &left_values);
        api::EngineSqlTruthValue truth = api::EngineSqlTruthValue::true_value;
        if (streaming_hash_join->node->bound_expression_ids.front() !=
            streaming_hash_key_expression_id) {
          std::string predicate_detail;
          if (!predicate_runtime.EvaluatePredicateForConsumer(
                  streaming_hash_join->node->bound_expression_ids.front(),
                  streaming_hash_join->predicate_row_binding,
                  CanonicalRelationalExpressionRowView{left_values,
                                                       right_values},
                  api::EngineCanonicalExpressionConsumer::join, &truth,
                  &predicate_detail)) {
            stream_detail = predicate_detail.empty()
                                ? "streaming hash residual predicate refused"
                                : predicate_detail;
            join_ok = false;
            return false;
          }
        }
        if (truth == api::EngineSqlTruthValue::true_value &&
            !append_match(left_row, right_row)) {
          join_ok = false;
          return false;
        }
      }
      return true;
    };
    auto right_stream = stream_rows(
        *streaming_hash_right_scan, streaming_hash_right_key_ordinal,
        &right_binding, &right_rows, &right_expected_visible_rows,
        &right_descriptor_authority, &probe_right_row);
    if (!join_ok) {
      return refuse(cancellation_requested()
                        ? "QOW-DIAG-QRY-012-JOIN-CANCELLED-V1"
                        : (stream_detail.find("row bound") !=
                                   std::string::npos
                               ? "SBLR.PLAN_TREE.RESOURCE_LIMIT"
                               : "QOW-DIAG-RELATIONAL-LIVE-JOIN-PAYLOAD-V1"),
                    stream_detail);
    }
    if (!right_stream.ok || !right_stream.memory_receipt_complete ||
        !right_stream.complete_mga_chain_validation ||
        !right_stream.exact_segment_extent_revalidated ||
        right_stream.visible_row_count != right_expected_visible_rows ||
        api::SerializeMgaRelationStorageDescriptor(right_stream.descriptor) !=
            right_descriptor_authority) {
      return refuse(
          stream_failure_code(right_stream),
          !stream_detail.empty()
              ? stream_detail
              : (right_stream.diagnostic.detail.empty()
                     ? "streaming hash right source receipt is incomplete"
                     : right_stream.diagnostic.detail));
    }
    if (!publish_page(true)) {
      return refuse(cancellation_requested()
                        ? "QOW-DIAG-QRY-012-JOIN-CANCELLED-V1"
                        : "QOW-RESULT-DIAGNOSTIC-ABI-V2",
                    stream_detail);
    }
    const auto result_authority =
        exec::RevalidateCanonicalExecutionMgaAuthority(
            *heap_registration.mga_authority, physical.physical_dag);
    if (!result_authority.ok) {
      return refuse(result_authority.diagnostic_code,
                    result_authority.detail);
    }
    result.physical_dag_executed = true;
    result.runtime_actuals_attached = true;
    result.canonical_result_published = true;
    result.canonical_result_column_count = ordered_outputs.size();
    result.canonical_result_row_count = matched_row_count;
    result.api_result.evidence.push_back(
        {"canonical.selected_plan", physical.physical_dag.selected_plan_uuid});
    result.api_result.evidence.push_back(
        {"canonical.result_abi", "QOW-RESULT-DIAGNOSTIC-ABI-V2"});
    result.api_result.evidence.push_back(
        {"canonical.heap_join_implementation",
         "join.hash-inner.int64-equality.v1"});
    result.api_result.evidence.push_back(
        {"canonical.heap_join_streaming_mga", "complete"});
    return result;
  }

  api::CanonicalOptimizerSelectedExecutionRequest selected;
  selected.borrowed_selected_physical_dag = &physical.physical_dag;
  selected.borrowed_mga_authority =
      heap_registration.mga_authority.get();
  selected.pre_access_statistics_snapshot_uuid =
      physical.physical_dag.statistics_snapshot_uuid;
  selected.available_executors.push_back(
      std::move(*heap_registration.registration));
  struct HeapJoinRegistrationGroup {
    std::string implementation_id;
    std::string capability_uuid;
    std::vector<LiveJoinRuntimeNodeConfiguration> node_configurations;
  };
  std::vector<HeapJoinRegistrationGroup> join_registration_groups;
  for (auto& bound : bound_joins) {
    auto group = std::ranges::find_if(
        join_registration_groups, [&](const auto& candidate) {
          return candidate.implementation_id == bound.implementation_id;
        });
    if (group == join_registration_groups.end()) {
      join_registration_groups.push_back(
          {bound.implementation_id, bound.capability_uuid, {}});
      group = std::prev(join_registration_groups.end());
    } else if (group->capability_uuid != bound.capability_uuid) {
      return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-JOIN-TREE-V1",
                    "join implementation capability identity changed");
    }
    LiveJoinRuntimeNodeConfiguration configuration;
    configuration.relational_node_id = bound.node->node_id;
    configuration.join_kind = bound.kind;
    configuration.operation_name =
        "object-backed heap " + bound.operation;
    if (bound.kind != exec::CanonicalAcceptedJoinKind::kCross) {
      configuration.predicate_expression_id =
          bound.node->bound_expression_ids.front();
      configuration.predicate_binding =
          std::move(bound.predicate_row_binding);
    }
    group->node_configurations.push_back(std::move(configuration));
  }
  for (auto& group : join_registration_groups) {
    if (group.node_configurations.empty()) {
      return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-JOIN-TREE-V1",
                    "join implementation configuration is absent");
    }
    CanonicalRelationalExpressionRuntimeServices predicate_services;
    predicate_services.comparison_evaluator =
        [context = &input.context](const api::EngineTypedValue& left,
                                   const api::EngineTypedValue& right,
                                   int* comparison,
                                   std::string* diagnostic_id,
                                   std::string* refusal_detail) {
          return CompareCanonicalRelationalScalarsV1(
              *context, left, right, comparison, diagnostic_id,
              refusal_detail);
        };
    BindCanonicalPersistedRowDescriptorAuthorityForComposition(input.context,
                                                   &predicate_services);
    const auto default_join_kind =
        group.node_configurations.front().join_kind;
    const auto default_operation_name =
        group.node_configurations.front().operation_name;
    auto node_configurations = std::move(group.node_configurations);
    selected.available_executors.push_back(MakeLiveJoinRegistration(
        group.implementation_id, group.capability_uuid, {},
        maximum_pair_count, maximum_output_rows,
        default_join_kind, default_operation_name, {}, true,
        0, {}, {}, std::move(predicate_services),
        std::move(node_configurations), std::nullopt,
        &input.relational_dag, &input.context,
        selected.borrowed_mga_authority));
  }
  if (!filters.empty()) {
    CanonicalRelationalExpressionRuntimeServices filter_services;
    filter_services.comparison_evaluator =
        [context = &input.context](const api::EngineTypedValue& left,
                                   const api::EngineTypedValue& right,
                                   int* comparison,
                                   std::string* diagnostic_id,
                                   std::string* refusal_detail) {
          return CompareCanonicalRelationalScalarsV1(
              *context, left, right, comparison, diagnostic_id,
              refusal_detail);
        };
    BindCanonicalPersistedRowDescriptorAuthorityForComposition(input.context,
                                                   &filter_services);
    std::vector<LiveFilterRuntimeNodeConfiguration>
        filter_node_configurations;
    filter_node_configurations.reserve(bound_filters.size());
    for (auto& bound_filter : bound_filters) {
      filter_node_configurations.push_back(
          {bound_filter.node->node_id,
           bound_filter.node->bound_expression_ids.front(),
           std::move(bound_filter.predicate_row_binding)});
    }
    selected.available_executors.push_back(
        MakeLiveHeapFilterRegistration(
            0, {}, {}, std::move(filter_services),
            filter_capability_uuid, maximum_output_rows, {},
            api::EngineCanonicalExpressionConsumer::filter,
            api::EnginePredicateConsumer::filter, &input.relational_dag,
            &input.context, selected.borrowed_mga_authority,
            std::move(filter_node_configurations)));
  }
  if (!projects.empty()) {
    std::vector<LiveProjectRuntimeNodeConfiguration>
        project_node_configurations;
    project_node_configurations.reserve(bound_projects.size());
    for (auto& bound_project : bound_projects) {
      project_node_configurations.push_back(
          {bound_project.node->node_id,
           std::move(bound_project.projected_columns)});
    }
    selected.available_executors.push_back(
        MakeLiveHeapProjectRegistration(
            {}, project_capability_uuid,
            maximum_output_rows, {}, &input.context,
            selected.borrowed_mga_authority,
            std::move(project_node_configurations)));
  }
  std::vector<LiveNonrecursiveCteRuntimeNodeConfiguration>
      inline_cte_node_configurations;
  std::vector<LiveNonrecursiveCteRuntimeNodeConfiguration>
      materialized_cte_node_configurations;
  for (const auto& bound_cte : bound_ctes) {
    auto& configurations = bound_cte.node->shareable
                               ? materialized_cte_node_configurations
                               : inline_cte_node_configurations;
    configurations.push_back({bound_cte.node->node_id});
  }
  if (!inline_cte_node_configurations.empty()) {
    selected.available_executors.push_back(
        MakeLiveNonrecursiveCteRegistration(
            "cte.bound.inline.typed.v1", inline_cte_capability_uuid,
            maximum_output_rows, {}, &input.context,
            selected.borrowed_mga_authority,
            std::move(inline_cte_node_configurations)));
  }
  if (!materialized_cte_node_configurations.empty()) {
    selected.available_executors.push_back(
        MakeLiveNonrecursiveCteRegistration(
            "cte.bound.materialize.typed.v1",
            materialized_cte_capability_uuid, maximum_output_rows, {},
            &input.context, selected.borrowed_mga_authority,
            std::move(materialized_cte_node_configurations)));
  }
  if (terminal_limit != nullptr) {
    selected.available_executors.push_back(MakeLiveLimitRegistration(
        "limit.typed.v1", limit_capability_uuid, row_limit, row_offset, false,
        maximum_output_rows, {}, &input.context,
        selected.borrowed_mga_authority));
  }
  std::uint64_t executor_registration_live_bytes =
      sizeof(selected.available_executors);
  std::uint64_t executor_registration_slots_bytes = 0;
  if (!CheckedMultiply(selected.available_executors.capacity(),
                       sizeof(exec::CanonicalPhysicalExecutorRegistration),
                       &executor_registration_slots_bytes) ||
      !CheckedAdd(executor_registration_live_bytes,
                  executor_registration_slots_bytes,
                  &executor_registration_live_bytes)) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "executor registration registry memory overflows");
  }
  for (const auto& registration : selected.available_executors) {
    if (registration.retained_live_memory_bytes_v1 == 0 ||
        registration.implementation_id.capacity() ==
            std::numeric_limits<std::size_t>::max() ||
        registration.executor_capability_uuid.capacity() ==
            std::numeric_limits<std::size_t>::max() ||
        !CheckedAdd(executor_registration_live_bytes,
                    registration.implementation_id.capacity() + 1,
                    &executor_registration_live_bytes) ||
        !CheckedAdd(executor_registration_live_bytes,
                    registration.executor_capability_uuid.capacity() + 1,
                    &executor_registration_live_bytes) ||
        !CheckedAdd(executor_registration_live_bytes,
                    registration.retained_live_memory_bytes_v1,
                    &executor_registration_live_bytes)) {
      return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    "executor registration retained-memory receipt is incomplete");
    }
  }
  selected.executor_registration_live_memory_bytes =
      executor_registration_live_bytes;
  selected.engine_execution_authorized = true;
  selected.result_publication_request.statement_uuid =
      input.context.statement_uuid;
  selected.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + input.context.current_monotonic_ns,
          "heap-join-tree.execution-attempt");
  selected.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(input.context.local_transaction_id) + ":" +
              std::to_string(
                  input.context.snapshot_visible_through_local_transaction_id),
          "heap-join-tree.transaction-effect-unchanged");
  selected.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  selected.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  for (std::size_t ordinal = 0; ordinal < ordered_outputs.size(); ++ordinal) {
    const auto& output = *ordered_outputs[ordinal];
    const auto descriptor = std::ranges::find_if(
        dag.descriptors, [&](const auto& candidate) {
          return candidate.descriptor_id == output.descriptor_id;
        });
    if (descriptor == dag.descriptors.end()) {
      return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-CROSS-JOIN-BINDING-V1",
                    "object-backed CROSS JOIN descriptor is unresolved");
    }
    exec::CanonicalResultColumnDescriptor published;
    published.ordinal = static_cast<std::uint32_t>(ordinal);
    published.name_utf8 = output.output_name_utf8;
    published.descriptor_uuid = descriptor->descriptor_uuid;
    published.type_uuid = descriptor->type_uuid;
    const auto& root_nullability =
        nullable_by_node.at(final_output_node->node_id);
    const bool null_extended_output = root_nullability[ordinal];
    published.nullability =
        null_extended_output ||
                descriptor->nullability ==
                    api::RelationalNullability::kNullable
            ? exec::CanonicalResultNullability::kNullable
            : exec::CanonicalResultNullability::kNonNull;
    published.collation_uuid = descriptor->collation_uuid;
    published.timezone_profile_id = descriptor->timezone_profile_id;
    selected.result_publication_request.column_bindings.push_back(
        {ordinal, true, std::move(published)});
  }
  selected.result_publication_request.maximum_row_count =
      terminal_limit == nullptr
          ? maximum_output_rows
          : std::max<std::size_t>(
                1, std::min<std::size_t>(
                       maximum_output_rows,
                       static_cast<std::size_t>(std::min<std::uint64_t>(
                           row_limit,
                           std::numeric_limits<std::size_t>::max()))));
  const auto execution = ExecuteSelectedCanonicalObjectFreeDag(
      input.context, selected, physical.ordinary_runtime_memory_receipts);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-PACKET7-OBJECT-HEAP-CROSS-JOIN-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "object-backed heap CROSS JOIN selected DAG was not completed"
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
  result.api_result = SuccessfulApiResult(planning_request, execution);
  return result;
}

}  // namespace scratchbird::engine::sblr
