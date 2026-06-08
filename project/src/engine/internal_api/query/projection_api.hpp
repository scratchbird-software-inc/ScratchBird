// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <functional>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_QUERY_PROJECTION_API
struct EngineBindProjectionRequest : EngineApiRequest {};
struct EngineBindProjectionResult : EngineApiResult {};
EngineBindProjectionResult EngineBindProjection(const EngineBindProjectionRequest& request);

struct EngineProjectionFunctionArgument {
  std::string name;
  std::string type_name;
  std::string encoded_value;
  bool is_null = false;
};

struct EngineProjectionExpression {
  std::string expression_kind;
  std::string type_name;
  std::string encoded_value;
  bool is_null = false;
  std::string operator_id;
  std::string canonical_operator_id;
  std::string special_form_id;
  std::string sblr_binding;
  std::vector<EngineProjectionExpression> arguments;
};

struct EngineProjectionFunctionRequest {
  EngineRequestContext context;
  std::string function_id;
  std::vector<EngineProjectionFunctionArgument> arguments;
};

struct EngineProjectionFunctionResult {
  bool ok = false;
  EngineTypedValue value;
  std::vector<EngineApiDiagnostic> diagnostics;
  std::vector<EngineEvidenceReference> evidence;
};

using EngineProjectionFunctionEvaluator =
    std::function<EngineProjectionFunctionResult(const EngineProjectionFunctionRequest&)>;

struct EngineProjectionOperatorRequest {
  EngineRequestContext context;
  EngineProjectionExpression expression;
};

using EngineProjectionOperatorEvaluator =
    std::function<EngineProjectionFunctionResult(const EngineProjectionOperatorRequest&)>;

struct EngineEvaluateProjectionRequest : EngineApiRequest {
  EngineProjectionFunctionEvaluator function_evaluator;
  EngineProjectionOperatorEvaluator operator_evaluator;
};
struct EngineEvaluateProjectionResult : EngineApiResult {};
EngineEvaluateProjectionResult EngineEvaluateProjection(const EngineEvaluateProjectionRequest& request);

}  // namespace scratchbird::engine::internal_api
