// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "logical_plan.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace plan = scratchbird::engine::planner;

namespace {

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-OPT-001-V1: " << detail << '\n';
  return condition;
}

std::string Uuid(const std::uint64_t suffix) {
  auto text = std::string("019f0000-0000-7600-8000-000000000000");
  const auto digits = std::to_string(suffix);
  text.replace(text.size() - digits.size(), digits.size(), digits);
  return text;
}

plan::CanonicalLogicalRelationalNode Node(
    const std::uint32_t id,
    const plan::CanonicalLogicalRelationalNodeKind kind,
    std::vector<std::uint32_t> inputs,
    const std::uint32_t descriptor_id,
    std::string semantic_variant) {
  plan::CanonicalLogicalRelationalNode node;
  node.logical_node_id = id;
  node.node_kind = kind;
  node.input_logical_node_ids = std::move(inputs);
  node.output_descriptor_ids = {descriptor_id};
  node.bound_expression_ids = {1000 + id};
  node.origin_relational_node_ids = {id};
  node.semantic_variant_id = std::move(semantic_variant);
  return node;
}

plan::CanonicalLogicalRelationalGraph Graph() {
  plan::CanonicalLogicalRelationalGraph graph;
  graph.bound_sblr_tree_uuid = Uuid(1);
  graph.catalog_epoch_uuid = Uuid(2);
  graph.security_context_uuid = Uuid(3);
  graph.local_transaction_id = 701;
  graph.statement_snapshot_id = 702;
  graph.root_logical_node_id = 17;
  graph.result_descriptor_ids = {117};
  graph.nodes = {
      Node(1, plan::CanonicalLogicalRelationalNodeKind::kRelationSource, {},
           101, "source.bound-relation.v1"),
      Node(2, plan::CanonicalLogicalRelationalNodeKind::kValues, {}, 102,
           "values.literal-table.v1"),
      Node(3, plan::CanonicalLogicalRelationalNodeKind::kJoin, {1, 2}, 103,
           "join.inner.v1"),
      Node(4, plan::CanonicalLogicalRelationalNodeKind::kFilter, {3}, 104,
           "filter.where.v1"),
      Node(5, plan::CanonicalLogicalRelationalNodeKind::kProject, {4}, 105,
           "project.select-list.v1"),
      Node(6, plan::CanonicalLogicalRelationalNodeKind::kAggregate, {5}, 106,
           "aggregate.grouped.v1"),
      Node(7, plan::CanonicalLogicalRelationalNodeKind::kSort, {6}, 107,
           "sort.required-order.v1"),
      Node(8, plan::CanonicalLogicalRelationalNodeKind::kWindow, {7}, 108,
           "window.bound-spec.v1"),
      Node(9, plan::CanonicalLogicalRelationalNodeKind::kLimit, {8}, 109,
           "limit.bound-count.v1"),
      Node(10, plan::CanonicalLogicalRelationalNodeKind::kSubquery, {9}, 110,
           "subquery.table.v1"),
      Node(11, plan::CanonicalLogicalRelationalNodeKind::kCte, {10}, 111,
           "cte.bound.v1"),
      Node(12, plan::CanonicalLogicalRelationalNodeKind::kRecursiveCte,
           {11, 2}, 112, "recursive-cte.union-all.v1"),
      Node(13, plan::CanonicalLogicalRelationalNodeKind::kPivot, {12}, 113,
           "pivot.bound.v1"),
      Node(14, plan::CanonicalLogicalRelationalNodeKind::kUnpivot, {13}, 114,
           "unpivot.bound.v1"),
      Node(15, plan::CanonicalLogicalRelationalNodeKind::kMatchRecognize,
           {14}, 115, "match-recognize.bound.v1"),
      Node(16,
           plan::CanonicalLogicalRelationalNodeKind::kTableFunctionInvoke,
           {15}, 116, "table-function.bound.v1"),
      Node(17, plan::CanonicalLogicalRelationalNodeKind::kSetOperation,
           {16, 2}, 117, "set-operation.union-all.v1"),
  };
  graph.nodes[1].shareable = true;
  graph.nodes[0].required_object_uuids = {Uuid(101)};
  graph.nodes[15].required_object_uuids = {Uuid(102)};
  return graph;
}

bool HasIssue(
    const plan::CanonicalLogicalRelationalGraphValidationResult& result,
    const std::string_view diagnostic_id,
    const std::string_view field_id) {
  for (const auto& issue : result.issues) {
    if (issue.diagnostic_id == diagnostic_id && issue.field_id == field_id) {
      return true;
    }
  }
  return false;
}

// QOW-TEST-OPT-001-V1
bool ValidateCompleteLogicalKinds() {
  const auto result = plan::ValidateCanonicalLogicalRelationalGraph(Graph());
  bool passed = true;
  passed &= Require(result.accepted && result.validated_node_count == 17,
                    "complete canonical relational graph was refused");
  passed &= Require(result.maximum_observed_depth == 16,
                    "logical dependency depth was not retained");
  for (std::uint8_t value = 1; value <= 17; ++value) {
    passed &= Require(
        std::string_view(plan::CanonicalLogicalRelationalNodeKindName(
            static_cast<plan::CanonicalLogicalRelationalNodeKind>(value))) !=
            "unknown",
        "canonical relational node kind has no stable identity");
  }
  return passed;
}

bool ValidateGraphAndAuthorityRefusal() {
  bool passed = true;
  auto graph = Graph();
  graph.nodes[2].input_logical_node_ids[1] = 999;
  auto result = plan::ValidateCanonicalLogicalRelationalGraph(graph);
  passed &= Require(!result.accepted &&
                        HasIssue(result, "SBLR.PLAN_TREE.INVALID_HANDLE",
                                 "input_logical_node_ids"),
                    "dangling logical input was accepted");

  graph = Graph();
  graph.nodes[1].shareable = false;
  result = plan::ValidateCanonicalLogicalRelationalGraph(graph);
  passed &= Require(!result.accepted &&
                        HasIssue(result, "SBLR.PLAN_TREE.INVALID_HANDLE",
                                 "shareable"),
                    "undeclared shared logical node was accepted");

  graph = Graph();
  graph.nodes[0].input_logical_node_ids = {17};
  result = plan::ValidateCanonicalLogicalRelationalGraph(graph);
  passed &= Require(!result.accepted &&
                        HasIssue(result, "SBLR.PLAN_TREE.INVALID_HANDLE",
                                 "cycle"),
                    "cyclic logical graph was accepted");

  graph = Graph();
  graph.raw_sql_text_present = true;
  result = plan::ValidateCanonicalLogicalRelationalGraph(graph);
  passed &= Require(!result.accepted &&
                        HasIssue(result,
                                 "QOW-DIAG-LOGICAL-GRAPH-AUTHORITY-V1",
                                 "forbidden_authority_claim"),
                    "parser SQL text entered the logical graph");
  return passed;
}

bool ValidatePhysicalSelectionRefusal() {
  auto graph = Graph();
  graph.nodes[2].semantic_variant_id = "join.hash.v1";
  auto result = plan::ValidateCanonicalLogicalRelationalGraph(graph);
  bool passed = true;
  passed &= Require(!result.accepted &&
                        HasIssue(result, "SBLR.PLAN_TREE.INVALID_HANDLE",
                                 "logical_node_record"),
                    "physical join algorithm entered a logical node");

  graph = Graph();
  graph.nodes[0].semantic_variant_id = "source.index.v1";
  result = plan::ValidateCanonicalLogicalRelationalGraph(graph);
  passed &= Require(!result.accepted &&
                        HasIssue(result, "SBLR.PLAN_TREE.INVALID_HANDLE",
                                 "logical_node_record"),
                    "physical access path entered a logical source node");

  graph = Graph();
  graph.nodes.push_back(Node(
      18, plan::CanonicalLogicalRelationalNodeKind::kValues, {}, 118,
      "values.orphan.v1"));
  result = plan::ValidateCanonicalLogicalRelationalGraph(graph);
  passed &= Require(!result.accepted &&
                        HasIssue(result, "SBLR.PLAN_TREE.INVALID_HANDLE",
                                 "orphan_logical_node"),
                    "orphan logical node was accepted");
  return passed;
}

bool ValidateResourceRefusal() {
  auto graph = Graph();
  plan::CanonicalLogicalRelationalGraphLimits limits;
  limits.maximum_depth = 15;
  auto result =
      plan::ValidateCanonicalLogicalRelationalGraph(graph, limits);
  bool passed = true;
  passed &= Require(!result.accepted &&
                        HasIssue(result, "SBLR.PLAN_TREE.RESOURCE_LIMIT",
                                 "maximum_depth"),
                    "logical graph depth bound was ignored");

  limits = {};
  limits.maximum_bound_references = 10;
  result = plan::ValidateCanonicalLogicalRelationalGraph(graph, limits);
  passed &= Require(!result.accepted &&
                        HasIssue(result, "SBLR.PLAN_TREE.RESOURCE_LIMIT",
                                 "bound_reference_count"),
                    "logical bound-reference budget was ignored");
  return passed;
}

}  // namespace

int main() {
  if (!ValidateCompleteLogicalKinds() || !ValidateGraphAndAuthorityRefusal() ||
      !ValidatePhysicalSelectionRefusal() || !ValidateResourceRefusal()) {
    return 1;
  }
  std::cout << "QOW-TEST-OPT-001-V1: PASS\n";
  return 0;
}
