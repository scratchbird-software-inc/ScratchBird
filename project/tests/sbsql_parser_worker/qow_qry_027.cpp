// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/projection_api.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace api = scratchbird::engine::internal_api;

namespace scratchbird::engine::internal_api {
bool QowValidateCanonicalExpressionGraphV1(
    const std::vector<std::uint32_t>& expression_ids,
    const std::vector<std::vector<std::uint32_t>>& child_expression_ids,
    const std::vector<bool>& shareable,
    const std::vector<std::uint32_t>& root_expression_ids,
    std::size_t* validated_node_count,
    std::size_t* maximum_observed_depth,
    std::string* refusal_reason,
    std::string* refusal_detail);

bool QowReadCanonicalProjectionExpressionsV1(
    const EngineApiRequest& request,
    std::uint64_t projection_count,
    std::vector<EngineProjectionExpression>* expressions,
    std::size_t* validated_node_count,
    std::size_t* maximum_observed_depth,
    std::string* refusal_reason,
    std::string* refusal_detail);
}  // namespace scratchbird::engine::internal_api

namespace {

bool Require(const bool condition, const std::string_view message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

struct Graph {
  std::vector<std::uint32_t> ids;
  std::vector<std::vector<std::uint32_t>> children;
  std::vector<bool> shareable;
  std::vector<std::uint32_t> roots;
};

bool Validate(const Graph& graph,
              std::size_t* node_count,
              std::size_t* maximum_depth,
              std::string* reason,
              std::string* detail) {
  return api::QowValidateCanonicalExpressionGraphV1(
      graph.ids, graph.children, graph.shareable, graph.roots, node_count,
      maximum_depth, reason, detail);
}

bool RefusesAtomically(const Graph& graph,
                       const std::string_view expected_reason) {
  std::size_t node_count = 999;
  std::size_t maximum_depth = 999;
  std::string reason;
  std::string detail;
  const bool accepted =
      Validate(graph, &node_count, &maximum_depth, &reason, &detail);
  return !accepted && node_count == 0 && maximum_depth == 0 &&
         reason == expected_reason && !detail.empty();
}

Graph Chain(const std::size_t node_count) {
  Graph graph;
  graph.ids.reserve(node_count);
  graph.children.resize(node_count);
  graph.shareable.resize(node_count, false);
  for (std::size_t index = 0; index < node_count; ++index) {
    graph.ids.push_back(static_cast<std::uint32_t>(index + 1));
    if (index + 1 < node_count) {
      graph.children[index].push_back(static_cast<std::uint32_t>(index + 2));
    }
  }
  if (node_count != 0) graph.roots.push_back(1);
  return graph;
}

Graph Fanout(const std::size_t fanout) {
  Graph graph;
  graph.ids.reserve(fanout + 1);
  graph.children.resize(fanout + 1);
  graph.shareable.resize(fanout + 1, false);
  graph.ids.push_back(1);
  graph.roots.push_back(1);
  for (std::size_t index = 0; index < fanout; ++index) {
    const auto id = static_cast<std::uint32_t>(index + 2);
    graph.ids.push_back(id);
    graph.children[0].push_back(id);
  }
  return graph;
}

bool ValidateGraphAcceptanceAndSharing() {
  Graph graph{{1, 2, 3, 4}, {{2, 3}, {4}, {4}, {}},
              {false, false, false, true}, {1}};
  std::size_t node_count = 0;
  std::size_t maximum_depth = 0;
  std::string reason;
  std::string detail;
  bool passed = true;
  passed &= Require(
      Validate(graph, &node_count, &maximum_depth, &reason, &detail),
      "canonical shared expression DAG was refused");
  passed &= Require(node_count == 4 && maximum_depth == 3,
                    "accepted DAG did not report its complete shape");
  passed &= Require(reason.empty() && detail.empty(),
                    "accepted DAG retained refusal state");

  graph.shareable[3] = false;
  passed &= Require(
      RefusesAtomically(graph, "unshareable_reference"),
      "undeclared expression sharing was accepted");
  return passed;
}

bool ValidateStructuralRefusals() {
  bool passed = true;
  passed &= Require(
      RefusesAtomically(Graph{{1, 1}, {{}, {}}, {false, false}, {1}},
                        "node_identity"),
      "duplicate expression identifier was accepted");
  passed &= Require(
      RefusesAtomically(Graph{{1}, {{2}}, {false}, {1}},
                        "dangling_reference"),
      "dangling expression reference was accepted");
  passed &= Require(
      RefusesAtomically(Graph{{1, 2}, {{}, {}}, {false, false}, {1}},
                        "orphan_node"),
      "orphan expression node was accepted");
  passed &= Require(
      RefusesAtomically(Graph{{1, 2}, {{2}, {1}}, {true, false}, {1}},
                        "cycle"),
      "cyclic expression graph was accepted");
  passed &= Require(
      RefusesAtomically(Graph{{1}, {{}}, {false}, {1, 1}},
                        "root_identity"),
      "duplicate expression root was accepted");
  return passed;
}

bool ValidateFiniteLimits() {
  bool passed = true;
  {
    const auto graph = Chain(256);
    std::size_t node_count = 0;
    std::size_t maximum_depth = 0;
    std::string reason;
    std::string detail;
    passed &= Require(
        Validate(graph, &node_count, &maximum_depth, &reason, &detail) &&
            node_count == 256 && maximum_depth == 256,
        "depth-256 expression graph was not admitted exactly");
  }
  passed &= Require(RefusesAtomically(Chain(257), "maximum_depth"),
                    "depth-257 expression graph was accepted or truncated");
  {
    const auto graph = Fanout(1024);
    std::size_t node_count = 0;
    std::size_t maximum_depth = 0;
    std::string reason;
    std::string detail;
    passed &= Require(
        Validate(graph, &node_count, &maximum_depth, &reason, &detail) &&
            node_count == 1025 && maximum_depth == 2,
        "fanout-1024 expression graph was not admitted exactly");
  }
  passed &= Require(RefusesAtomically(Fanout(1025), "maximum_fanout"),
                    "fanout-1025 expression graph was accepted");
  return passed;
}

api::EngineApiRequest NestedOptionRequest(const std::size_t depth) {
  api::EngineApiRequest request;
  std::string prefix = "projection_0_";
  for (std::size_t level = 1; level < depth; ++level) {
    request.option_envelopes.push_back(prefix + "expr_kind:function");
    request.option_envelopes.push_back(prefix + "function_arg_count:1");
    prefix += "arg_0_";
  }
  request.option_envelopes.push_back(prefix + "expr_kind:literal");
  request.option_envelopes.push_back(prefix + "type:int64");
  request.option_envelopes.push_back(prefix + "value:deep-leaf");
  return request;
}

bool ReadOptions(const api::EngineApiRequest& request,
                 std::vector<api::EngineProjectionExpression>* expressions,
                 std::size_t* node_count,
                 std::size_t* maximum_depth,
                 std::string* reason,
                 std::string* detail) {
  return api::QowReadCanonicalProjectionExpressionsV1(
      request, 1, expressions, node_count, maximum_depth, reason, detail);
}

bool ValidateCompleteOptionTreeRead() {
  auto request = NestedOptionRequest(7);
  std::vector<api::EngineProjectionExpression> expressions;
  std::size_t node_count = 0;
  std::size_t maximum_depth = 0;
  std::string reason;
  std::string detail;
  bool passed = true;
  passed &= Require(
      ReadOptions(request, &expressions, &node_count, &maximum_depth, &reason,
                  &detail),
      "valid depth-seven projection expression was refused");
  passed &= Require(node_count == 7 && maximum_depth == 7 &&
                        expressions.size() == 1,
                    "projection reader did not preserve the complete graph");
  const api::EngineProjectionExpression* leaf =
      expressions.empty() ? nullptr : &expressions.front();
  for (std::size_t level = 1; level < 7 && leaf != nullptr; ++level) {
    leaf = leaf->arguments.size() == 1 ? &leaf->arguments.front() : nullptr;
  }
  passed &= Require(leaf != nullptr && leaf->encoded_value == "deep-leaf",
                    "projection reader silently truncated the deep leaf");
  return passed;
}

bool ValidateOptionTreeRefusals() {
  bool passed = true;
  {
    api::EngineApiRequest request;
    request.option_envelopes = {
        "projection_0_type:int64", "projection_0_value:1"};
    std::vector<api::EngineProjectionExpression> expressions(1);
    std::size_t node_count = 999;
    std::size_t maximum_depth = 999;
    std::string reason;
    std::string detail;
    const bool accepted = ReadOptions(request, &expressions, &node_count,
                                      &maximum_depth, &reason, &detail);
    passed &= Require(!accepted && expressions.empty() && node_count == 0 &&
                          maximum_depth == 0 && reason == "expression_kind",
                      "missing expression kind selected literal semantics");
  }
  {
    api::EngineApiRequest request;
    request.option_envelopes = {"projection_0_expr_kind:literal"};
    std::vector<api::EngineProjectionExpression> expressions(1);
    std::size_t node_count = 999;
    std::size_t maximum_depth = 999;
    std::string reason;
    std::string detail;
    const bool accepted = ReadOptions(request, &expressions, &node_count,
                                      &maximum_depth, &reason, &detail);
    passed &= Require(!accepted && expressions.empty() && node_count == 0 &&
                          maximum_depth == 0 && reason == "descriptor_missing",
                      "missing literal descriptor selected text semantics");
  }
  {
    api::EngineApiRequest request;
    request.option_envelopes = {
        "projection_0_expr_kind:literal",
        "projection_0_type:not_a_canonical_type",
        "projection_0_value:value"};
    std::vector<api::EngineProjectionExpression> expressions(1);
    std::size_t node_count = 999;
    std::size_t maximum_depth = 999;
    std::string reason;
    std::string detail;
    const bool accepted = ReadOptions(request, &expressions, &node_count,
                                      &maximum_depth, &reason, &detail);
    passed &= Require(!accepted && expressions.empty() && node_count == 0 &&
                          maximum_depth == 0 &&
                          reason == "descriptor_unsupported",
                      "unknown literal descriptor acquired text semantics");
  }
  {
    auto request = NestedOptionRequest(257);
    std::vector<api::EngineProjectionExpression> expressions(1);
    std::size_t node_count = 999;
    std::size_t maximum_depth = 999;
    std::string reason;
    std::string detail;
    const bool accepted = ReadOptions(request, &expressions, &node_count,
                                      &maximum_depth, &reason, &detail);
    passed &= Require(!accepted && expressions.empty() && node_count == 0 &&
                          maximum_depth == 0 && reason == "maximum_depth" &&
                          !detail.empty(),
                      "depth-257 option tree was accepted or partially returned");
  }
  {
    api::EngineApiRequest request;
    request.option_envelopes = {
        "projection_0_expr_kind:function",
        "projection_0_function_arg_count:1025"};
    std::vector<api::EngineProjectionExpression> expressions(1);
    std::size_t node_count = 999;
    std::size_t maximum_depth = 999;
    std::string reason;
    std::string detail;
    const bool accepted = ReadOptions(request, &expressions, &node_count,
                                      &maximum_depth, &reason, &detail);
    passed &= Require(!accepted && expressions.empty() && node_count == 0 &&
                          maximum_depth == 0 && reason == "maximum_fanout",
                      "over-width option tree was accepted or partially returned");
  }
  {
    api::EngineApiRequest request;
    request.option_envelopes = {
        "projection_0_expr_kind:function",
        "projection_0_function_arg_count:1x"};
    std::vector<api::EngineProjectionExpression> expressions(1);
    std::size_t node_count = 999;
    std::size_t maximum_depth = 999;
    std::string reason;
    std::string detail;
    const bool accepted = ReadOptions(request, &expressions, &node_count,
                                      &maximum_depth, &reason, &detail);
    passed &= Require(!accepted && expressions.empty() && node_count == 0 &&
                          maximum_depth == 0 && reason == "argument_count",
                      "malformed option-tree fanout was converted to a leaf");
  }
  return passed;
}

}  // namespace

// QOW-TEST-QRY-027-V1
int main() {
  bool passed = true;
  passed &= ValidateGraphAcceptanceAndSharing();
  passed &= ValidateStructuralRefusals();
  passed &= ValidateFiniteLimits();
  passed &= ValidateCompleteOptionTreeRead();
  passed &= ValidateOptionTreeRefusals();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
