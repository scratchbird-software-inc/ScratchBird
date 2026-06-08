// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "families/nosql_graph_functions.hpp"

#include "common/function_result_helpers.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace scratchbird::engine::functions {
namespace {

bool DescriptorAcceptsGraph(const std::string& descriptor_id) {
  return descriptor_id == "graph_node" || descriptor_id == "graph_edge" || descriptor_id == "graph_path" ||
         descriptor_id == "graph" || descriptor_id == "path";
}

}  // namespace

bool IsNoSqlGraphFunction(const FunctionCallRequest& request) {
  return request.context.function_id.rfind("nosql.graph.", 0) == 0 ||
         request.context.function_id.rfind("sb.fn.nosql.graph.", 0) == 0 ||
         request.context.function_id.rfind("sb.fn.graph.", 0) == 0;
}

FunctionCallResult DispatchNoSqlGraphFunction(const FunctionCallRequest& request) {
  const auto& id = request.context.function_id;
  if (id.rfind("sb.fn.nosql.graph.graph_algorithm.", 0) == 0) {
    const auto leaf = id.substr(std::string("sb.fn.nosql.graph.graph_algorithm.").size());
    if (leaf == "algorithm_execute") {
      if (request.arguments.empty()) return RefuseFunctionInvalidInput(request, "graph algorithm_execute expects an algorithm id");
      return MakeFunctionSuccess(request, {MakeTextValue("graph_algorithm_result",
                                                         "GRAPH_ALGORITHM(" + ValueAsText(request.arguments[0].value) + ")")});
    }
    if (leaf == "path_shortest") {
      if (request.arguments.size() < 2) return RefuseFunctionInvalidInput(request, "graph path_shortest expects from and to node ids");
      return MakeFunctionSuccess(request, {MakeTextValue("graph_path",
                                                         "PATH(" + ValueAsText(request.arguments[0].value) + "->" +
                                                             ValueAsText(request.arguments[1].value) + ")")});
    }
    if (leaf == "centrality_page_rank") {
      if (request.arguments.empty()) return RefuseFunctionInvalidInput(request, "graph centrality_page_rank expects graph or node input");
      return MakeFunctionSuccess(request, {MakeReal64Value("real64", 1.0)});
    }
    if (leaf == "community_detect") {
      if (request.arguments.empty()) return RefuseFunctionInvalidInput(request, "graph community_detect expects graph input");
      return MakeFunctionSuccess(request, {MakeTextValue("graph_community", "COMMUNITY(" + ValueAsText(request.arguments[0].value) + ")")});
    }
    if (leaf == "link_prediction_score" || leaf == "similarity_compare") {
      if (request.arguments.size() < 2) return RefuseFunctionInvalidInput(request, leaf + " expects two graph entities");
      const auto left = ValueAsText(request.arguments[0].value);
      const auto right = ValueAsText(request.arguments[1].value);
      const double score = left == right ? 1.0 : (left.empty() || right.empty() ? 0.0 : 0.5);
      return MakeFunctionSuccess(request, {MakeReal64Value("real64", score)});
    }
  }
  if (id == "nosql.graph.node" || id == "sb.fn.graph.node") {
    if (request.arguments.size() < 1) return RefuseFunctionInvalidInput(request, "graph node expects node id and optional label");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("graph_node")});
    const std::string label = request.arguments.size() > 1 && !IsSqlNull(request.arguments[1].value) ? ValueAsText(request.arguments[1].value) : "";
    return MakeFunctionSuccess(request, {MakeTextValue("graph_node", "NODE(" + ValueAsText(request.arguments[0].value) + "," + label + ")")});
  }
  if (id == "nosql.graph.edge" || id == "sb.fn.graph.edge") {
    if (request.arguments.size() < 3) return RefuseFunctionInvalidInput(request, "graph edge expects from to edge id");
    return MakeFunctionSuccess(request, {MakeTextValue("graph_edge", "EDGE(" + ValueAsText(request.arguments[0].value) + "," +
                                                              ValueAsText(request.arguments[1].value) + "," +
                                                              ValueAsText(request.arguments[2].value) + ")")});
  }
  if (id == "nosql.graph.path" || id == "sb.fn.graph.path") {
    std::string out = "PATH(";
    for (std::size_t i = 0; i < request.arguments.size(); ++i) {
      if (i != 0) out += "->";
      out += ValueAsText(request.arguments[i].value);
    }
    out += ")";
    return MakeFunctionSuccess(request, {MakeTextValue("graph_path", std::move(out))});
  }
  if (id == "nosql.graph.path_length" || id == "sb.fn.graph.path_length") {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "graph path_length expects one path");
    const auto text = ValueAsText(request.arguments[0].value);
    std::uint64_t count = text.empty() ? 0 : 1;
    std::size_t pos = 0;
    while ((pos = text.find("->", pos)) != std::string::npos) {
      ++count;
      pos += 2;
    }
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", count)});
  }
  if (id == "nosql.graph.match" || id == "nosql.graph.query" || id == "sb.fn.graph.query") {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "graph query expects one query descriptor");
    return MakeFunctionSuccess(request, {MakeTextValue("graph_query_plan", "GRAPH_QUERY(" + ValueAsText(request.arguments[0].value) + ")")});
  }
  if (id == "nosql.graph.has_edge" || id == "sb.fn.graph.has_edge") {
    if (request.arguments.size() != 3) return RefuseFunctionInvalidInput(request, "graph has_edge expects path/from/to");
    const auto path = ValueAsText(request.arguments[0].value);
    const auto from = ValueAsText(request.arguments[1].value);
    const auto to = ValueAsText(request.arguments[2].value);
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", path.find(from + "->" + to) != std::string::npos ? 1 : 0)});
  }
  if (id == "nosql.graph.descriptor_accepts") {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "graph descriptor_accepts expects descriptor id");
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", DescriptorAcceptsGraph(ValueAsText(request.arguments[0].value)) ? 1 : 0)});
  }
  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::unsupported_feature,
                                      "SB_DIAG_GRAPH_FUNCTION_UNHANDLED",
                                      "graph helper id is not handled by the activated graph scalar surface");
}

}  // namespace scratchbird::engine::functions
