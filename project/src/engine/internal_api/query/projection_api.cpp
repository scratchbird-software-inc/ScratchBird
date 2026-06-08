// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/projection_api.hpp"

#include "behavior_support/api_behavior_store.hpp"
#include "api_diagnostics.hpp"
#include "security/security_model.hpp"

#include <cctype>
#include <string>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

std::uint64_t ParseU64(std::string value) {
  if (value.empty()) return 0;
  std::uint64_t parsed = 0;
  for (char ch : value) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) return 0;
    parsed = (parsed * 10) + static_cast<unsigned>(ch - '0');
  }
  return parsed;
}

EngineDescriptor ProjectionDescriptor(const std::string& type_name) {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = type_name.empty() ? "text" : type_name;
  descriptor.encoded_descriptor = "type=" + descriptor.canonical_type_name;
  return descriptor;
}

EngineEvaluateProjectionResult ProjectionFailure(const EngineEvaluateProjectionRequest& request,
                                                 EngineApiDiagnostic diagnostic) {
  EngineEvaluateProjectionResult result;
  result.ok = false;
  result.operation_id = "query.evaluate_projection";
  result.embedded_trust_mode_observed =
      request.context.trust_mode == EngineTrustMode::embedded_in_process;
  result.diagnostics.push_back(std::move(diagnostic));
  return result;
}

EngineEvaluateProjectionResult ProjectionFailure(const EngineEvaluateProjectionRequest& request,
                                                 std::string detail) {
  return ProjectionFailure(request,
                           MakeInvalidRequestDiagnostic("query.evaluate_projection",
                                                        std::move(detail)));
}

std::string ProjectionOptionValue(const EngineApiRequest& request, const std::string& prefix) {
  return SecurityOptionValue(request, prefix);
}

bool ProjectionOptionIsTrue(const EngineApiRequest& request, const std::string& prefix) {
  const std::string value = ProjectionOptionValue(request, prefix);
  return value == "true" || value == "1";
}

EngineTypedValue LiteralProjectionValue(const EngineApiRequest& request,
                                        const std::string& prefix) {
  EngineTypedValue value;
  const std::string type = ProjectionOptionValue(request, prefix + "type:");
  value.descriptor = ProjectionDescriptor(type);
  value.encoded_value = ProjectionOptionValue(request, prefix + "value:");
  value.is_null = ProjectionOptionIsTrue(request, prefix + "is_null:");
  return value;
}

EngineProjectionExpression ReadProjectionExpression(const EngineApiRequest& request,
                                                    const std::string& prefix,
                                                    std::uint32_t depth = 0) {
  EngineProjectionExpression expression;
  expression.expression_kind = ProjectionOptionValue(request, prefix + "expr_kind:");
  if (expression.expression_kind.empty()) expression.expression_kind = "literal";
  expression.type_name = ProjectionOptionValue(request, prefix + "type:");
  expression.encoded_value = ProjectionOptionValue(request, prefix + "value:");
  expression.is_null = ProjectionOptionIsTrue(request, prefix + "is_null:");
  expression.operator_id = ProjectionOptionValue(request, prefix + "operator_id:");
  expression.canonical_operator_id = ProjectionOptionValue(request, prefix + "canonical_operator_id:");
  expression.special_form_id = ProjectionOptionValue(request, prefix + "special_form_id:");
  expression.sblr_binding = ProjectionOptionValue(request, prefix + "sblr_binding:");
  if (depth > 4) return expression;

  std::uint64_t arg_count = ParseU64(ProjectionOptionValue(request, prefix + "operator_arg_count:"));
  if (arg_count == 0) {
    arg_count = ParseU64(ProjectionOptionValue(request, prefix + "special_form_arg_count:"));
  }
  for (std::uint64_t arg_index = 0; arg_index < arg_count; ++arg_index) {
    expression.arguments.push_back(ReadProjectionExpression(
        request, prefix + "arg_" + std::to_string(arg_index) + "_", depth + 1));
  }
  return expression;
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_QUERY_PROJECTION_API_BEHAVIOR
EngineBindProjectionResult EngineBindProjection(const EngineBindProjectionRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineBindProjectionResult>(request.context, "query.bind_projection");
  AddApiBehaviorEvidence(&result, "query_binding", "projection");
  AddApiBehaviorRow(&result, {{"projection_count", std::to_string(request.projection.canonical_projection_envelopes.size())}, {"payload", ApiBehaviorPayloadFromRequest(request)}});
  return result;
}

EngineEvaluateProjectionResult EngineEvaluateProjection(const EngineEvaluateProjectionRequest& request) {
  const std::uint64_t projection_count = ParseU64(SecurityOptionValue(request, "projection_count:"));
  if (projection_count == 0) {
    EngineEvaluateProjectionResult result;
    result.ok = false;
    result.operation_id = "query.evaluate_projection";
    result.embedded_trust_mode_observed =
        request.context.trust_mode == EngineTrustMode::embedded_in_process;
    result.diagnostics.push_back(MakeInvalidRequestDiagnostic("query.evaluate_projection",
                                                              "projection_operand_required"));
    return result;
  }

  EngineEvaluateProjectionResult result;
  result.ok = true;
  result.operation_id = "query.evaluate_projection";
  result.embedded_trust_mode_observed =
      request.context.trust_mode == EngineTrustMode::embedded_in_process;
  result.result_shape.result_kind = "scalar_projection_rows";

  EngineRowValue row;
  row.requested_row_uuid.canonical = "scalar-projection-row-0";
  for (std::uint64_t index = 0; index < projection_count; ++index) {
    const std::string prefix = "projection_" + std::to_string(index) + "_";
    std::string name = SecurityOptionValue(request, prefix + "name:");
    EngineTypedValue value;
    const std::string expression_kind = SecurityOptionValue(request, prefix + "expr_kind:");
    if (expression_kind == "function") {
      if (!request.function_evaluator) {
        return ProjectionFailure(request, "function_projection_evaluator_required");
      }
      EngineProjectionFunctionRequest function_request;
      function_request.context = request.context;
      function_request.function_id = SecurityOptionValue(request, prefix + "function_id:");
      if (function_request.function_id.empty()) {
        return ProjectionFailure(request, "function_projection_id_required");
      }
      const std::uint64_t arg_count = ParseU64(SecurityOptionValue(
          request, prefix + "function_arg_count:"));
      for (std::uint64_t arg_index = 0; arg_index < arg_count; ++arg_index) {
        const std::string arg_prefix = prefix + "arg_" + std::to_string(arg_index) + "_";
        EngineProjectionFunctionArgument argument;
        argument.name = SecurityOptionValue(request, arg_prefix + "name:");
        if (argument.name.empty()) argument.name = "arg" + std::to_string(arg_index);
        argument.type_name = SecurityOptionValue(request, arg_prefix + "type:");
        argument.encoded_value = SecurityOptionValue(request, arg_prefix + "value:");
        argument.is_null = ProjectionOptionIsTrue(request, arg_prefix + "is_null:");
        function_request.arguments.push_back(std::move(argument));
      }
      auto function_result = request.function_evaluator(function_request);
      if (!function_result.ok) {
        if (function_result.diagnostics.empty()) {
          return ProjectionFailure(request, "function_projection_execution_failed");
        }
        return ProjectionFailure(request, std::move(function_result.diagnostics.front()));
      }
      value = std::move(function_result.value);
      for (auto& evidence : function_result.evidence) {
        result.evidence.push_back(std::move(evidence));
      }
    } else if (expression_kind == "operator" || expression_kind == "special_form") {
      if (!request.operator_evaluator) {
        return ProjectionFailure(request, "operator_projection_evaluator_required");
      }
      EngineProjectionOperatorRequest operator_request;
      operator_request.context = request.context;
      operator_request.expression = ReadProjectionExpression(request, prefix);
      auto operator_result = request.operator_evaluator(operator_request);
      if (!operator_result.ok) {
        if (operator_result.diagnostics.empty()) {
          return ProjectionFailure(request, "operator_projection_execution_failed");
        }
        return ProjectionFailure(request, std::move(operator_result.diagnostics.front()));
      }
      value = std::move(operator_result.value);
      for (auto& evidence : operator_result.evidence) {
        result.evidence.push_back(std::move(evidence));
      }
    } else if (expression_kind == "parameter") {
      const std::string marker_kind = SecurityOptionValue(request, prefix + "parameter_marker_kind:");
      const std::string ordinal = SecurityOptionValue(request, prefix + "parameter_ordinal:");
      const std::string type = SecurityOptionValue(request, prefix + "parameter_type:").empty()
                                   ? SecurityOptionValue(request, prefix + "type:")
                                   : SecurityOptionValue(request, prefix + "parameter_type:");
      value.descriptor.descriptor_kind = "parameter";
      value.descriptor.canonical_type_name = type.empty() ? "unknown" : type;
      value.descriptor.encoded_descriptor =
          "kind=input;marker=" + (marker_kind.empty() ? "anonymous" : marker_kind) +
          ";ordinal=" + (ordinal.empty() ? "1" : ordinal) +
          ";type=" + value.descriptor.canonical_type_name;
      value.encoded_value = "unbound_parameter_descriptor";
      value.is_null = false;
      result.evidence.push_back({"query_parameter_descriptor",
                                 marker_kind.empty() ? "anonymous" : marker_kind});
      result.evidence.push_back({"query_parameter_ordinal",
                                 ordinal.empty() ? "1" : ordinal});
      result.evidence.push_back({"query_parameter_value_execution", "false"});
    } else {
      value = LiteralProjectionValue(request, prefix);
    }
    if (name.empty()) name = "column" + std::to_string(index + 1);
    row.fields.push_back({std::move(name), std::move(value)});
    result.result_shape.columns.push_back(row.fields.back().second.descriptor);
  }
  result.result_shape.rows.push_back(std::move(row));
  result.evidence.push_back({"query_projection", "constant_projection_engine_evaluated"});
  AddApiBehaviorEvidence(&result, "query_binding", "evaluate_projection");
  return result;
}

}  // namespace scratchbird::engine::internal_api
