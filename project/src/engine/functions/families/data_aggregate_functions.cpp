// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "families/data_aggregate_functions.hpp"

#include "common/function_result_helpers.hpp"
#include "sblr/sblr_aggregate_window_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace scratchbird::engine::functions {
namespace {

std::string Lower(std::string_view input) {
  std::string out(input);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

std::string AggregateResultDescriptor(std::string_view function_id) {
  const std::string id = Lower(function_id);
  if (id.find("top_k") != std::string::npos || id.find("json") != std::string::npos) return "json";
  if (id.find("listagg") != std::string::npos ||
      id.find("string_agg") != std::string::npos ||
      id.find("group_concat") != std::string::npos) {
    return "text";
  }
  if (id.find("every") != std::string::npos ||
      id.find("bool_and") != std::string::npos ||
      id.find("boolean_and") != std::string::npos ||
      id.find("any") != std::string::npos ||
      id.find("bool_or") != std::string::npos ||
      id.find("boolean_or") != std::string::npos) {
    return "boolean";
  }
  if (id.find("count") != std::string::npos) return "int64";
  return "real64";
}

bool IsDataSketchAggregateFunction(std::string_view id) {
  return id.rfind("sb.fn.data.aggregate.sketch_", 0) == 0;
}

std::string SketchLeaf(std::string_view id) {
  const auto dot = id.rfind('.');
  return dot == std::string_view::npos ? std::string(id) : std::string(id.substr(dot + 1));
}

std::string SketchFamily(std::string_view id) {
  if (id.find("sketch_bloom.") != std::string_view::npos) return "bloom";
  if (id.find("sketch_cms.") != std::string_view::npos) return "count_min_sketch";
  if (id.find("sketch_topk.") != std::string_view::npos) return "topk";
  return "aggregate_sketch";
}

std::string SketchArgumentList(const FunctionCallRequest& request, std::size_t first = 0) {
  std::ostringstream out;
  out << "[";
  for (std::size_t i = first; i < request.arguments.size(); ++i) {
    if (i != first) out << ",";
    out << ValueAsText(request.arguments[i].value);
  }
  out << "]";
  return out.str();
}

FunctionCallResult DispatchDataSketchAggregateFunction(const FunctionCallRequest& request) {
  const std::string leaf = SketchLeaf(request.context.function_id);
  const std::string family = SketchFamily(request.context.function_id);
  if (leaf.find("create") != std::string::npos) {
    return MakeFunctionSuccess(request, {MakeTextValue("aggregate_sketch_descriptor",
                                                       family + ".create" + SketchArgumentList(request))});
  }
  if (leaf.find("info") != std::string::npos) {
    if (request.arguments.size() != 1) {
      return RefuseFunctionInvalidInput(request, leaf + " expects one sketch descriptor");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("aggregate_sketch_info",
                                                       family + ".info(" + ValueAsText(request.arguments[0].value) + ")")});
  }
  if (leaf.find("contains") != std::string::npos || leaf.find("query") != std::string::npos) {
    if (request.arguments.size() < 2) {
      return RefuseFunctionInvalidInput(request, leaf + " expects sketch descriptor and query value");
    }
    const auto sketch = ValueAsText(request.arguments[0].value);
    const auto probe = ValueAsText(request.arguments[1].value);
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", sketch.find(probe) == std::string::npos ? 0 : 1)});
  }
  if (leaf.find("merge") != std::string::npos) {
    if (request.arguments.size() < 2) return RefuseFunctionInvalidInput(request, leaf + " expects at least two sketch descriptors");
    return MakeFunctionSuccess(request, {MakeTextValue("aggregate_sketch_descriptor",
                                                       family + ".merge" + SketchArgumentList(request))});
  }
  if (leaf.find("add") != std::string::npos || leaf.find("increment") != std::string::npos ||
      leaf.find("operation") != std::string::npos) {
    if (request.arguments.empty()) {
      return RefuseFunctionInvalidInput(request, leaf + " expects a sketch descriptor or value argument");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("aggregate_sketch_descriptor",
                                                       family + "." + leaf + SketchArgumentList(request))});
  }
  return RefuseFunctionInvalidInput(request, leaf + " is not a recognized aggregate sketch operation");
}

}  // namespace

bool IsDataAggregateFunction(const FunctionCallRequest& request) {
  return request.context.function_id.rfind("data.aggregate.", 0) == 0 ||
         IsDataSketchAggregateFunction(request.context.function_id) ||
         request.context.function_id.rfind("sb.aggregate.", 0) == 0 ||
         request.context.function_id.rfind("data.window.", 0) == 0 ||
         request.context.function_id.rfind("sb.window.", 0) == 0 ||
         scratchbird::engine::sblr::IsSblrAggregateFunctionSupported(request.context.function_id) ||
         scratchbird::engine::sblr::IsSblrWindowFunctionSupported(request.context.function_id);
}

FunctionCallResult DispatchDataAggregateFunction(const FunctionCallRequest& request) {
  if (IsDataSketchAggregateFunction(request.context.function_id)) {
    return DispatchDataSketchAggregateFunction(request);
  }

  if (scratchbird::engine::sblr::IsSblrWindowFunctionSupported(request.context.function_id) &&
      !scratchbird::engine::sblr::IsSblrAggregateFunctionSupported(request.context.function_id)) {
    auto diagnostic = scratchbird::engine::sblr::MakeSblrRefusalDiagnostic(
        "SB_DIAG_WINDOW_FRAME_CONTEXT_REQUIRED",
        request.context.sblr_context,
        "window and ordered-set ranking functions require SBLR window/ordered-set frame context");
    diagnostic.fields.push_back({"function_id", request.context.function_id});
    diagnostic.fields.push_back({"function_uuid", request.context.function_uuid});
    return FunctionCallResult{scratchbird::engine::sblr::MakeSblrFailure(
        scratchbird::engine::sblr::SblrStatusCode::execution_failed,
        request.context.function_id,
        std::move(diagnostic))};
  }

  scratchbird::engine::sblr::SblrAggregateWindowState state;
  auto init = scratchbird::engine::sblr::InitializeSblrAggregateState(request.context.function_id,
                                                                     request.context.function_uuid,
                                                                     AggregateResultDescriptor(request.context.function_id),
                                                                     request.context.sblr_context,
                                                                     &state);
  if (!init.ok()) return FunctionCallResult{std::move(init)};

  scratchbird::engine::sblr::SblrAggregateUpdateRequest update;
  update.context = request.context.sblr_context;
  update.values.reserve(request.arguments.size());
  for (const auto& argument : request.arguments) {
    update.values.push_back(argument.value);
  }
  auto update_result = scratchbird::engine::sblr::UpdateSblrAggregateState(&state, update);
  if (!update_result.ok()) return FunctionCallResult{std::move(update_result)};

  scratchbird::engine::sblr::SblrAggregateFinalizeRequest finalize;
  finalize.context = request.context.sblr_context;
  return FunctionCallResult{scratchbird::engine::sblr::FinalizeSblrAggregateState(state, finalize)};
}

}  // namespace scratchbird::engine::functions
