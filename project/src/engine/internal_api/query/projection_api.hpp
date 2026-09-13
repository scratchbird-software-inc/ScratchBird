// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "../api_types.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_QUERY_PROJECTION_API
struct EngineProjectionFunctionArgument {
  std::string name;
  std::string type_name;
  std::string encoded_value;
  bool is_null = false;
  // Binary/UUID payloads have no encoded text alternative. Names remain
  // metadata; descriptor admission belongs to the bound query runtime.
  std::vector<std::uint8_t> binary_value;
  EngineValueState state = EngineValueState::value;
};

inline EngineProjectionFunctionArgument MakeProjectionFunctionArgument(
    std::string name, const EngineTypedValue& value) {
  return {std::move(name), value.descriptor.canonical_type_name,
          value.encoded_value, value.isSqlNull(), value.binary_value, value.state};
}

inline EngineProjectionFunctionArgument MakeProjectionFunctionArgument(
    std::string name, EngineTypedValue&& value) {
  // The only fallible descriptor copy precedes moving either payload. This
  // transfers vector storage, not a resource or final-publication capability.
  return {std::move(name), value.descriptor.canonical_type_name,
          std::move(value.encoded_value), value.isSqlNull(),
          std::move(value.binary_value), value.state};
}

struct EngineProjectionExpression {
  std::string name;
  std::string expression_kind;
  std::string type_name;
  std::string encoded_value;
  bool is_null = false;
  std::string function_id;
  EngineUuid function_uuid;
  std::string operator_id;
  std::string canonical_operator_id;
  std::string special_form_id;
  std::string sblr_binding;
  std::vector<EngineProjectionExpression> arguments;
  std::vector<std::uint8_t> binary_value;
};

struct EngineBindProjectionRequest : EngineApiRequest {};
struct EngineBindProjectionResult : EngineApiResult {
  std::vector<EngineProjectionExpression> bound_expressions;
  EngineProjectionEnvelope bound_projection;
  std::size_t validated_node_count = 0;
  std::size_t maximum_observed_depth = 0;
};
EngineBindProjectionResult EngineBindProjection(
    const EngineBindProjectionRequest& request);

struct EngineProjectionFunctionRequest {
  EngineRequestContext context;
  EngineUuid function_uuid;
  // Metadata only; dispatch authority is function_uuid.
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
