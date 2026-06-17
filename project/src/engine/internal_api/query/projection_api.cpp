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
  expression.name = ProjectionOptionValue(request, prefix + "name:");
  expression.expression_kind = ProjectionOptionValue(request, prefix + "expr_kind:");
  if (expression.expression_kind.empty()) expression.expression_kind = "literal";
  expression.type_name = ProjectionOptionValue(request, prefix + "type:");
  expression.encoded_value = ProjectionOptionValue(request, prefix + "value:");
  expression.is_null = ProjectionOptionIsTrue(request, prefix + "is_null:");
  expression.function_id = ProjectionOptionValue(request, prefix + "function_id:");
  expression.operator_id = ProjectionOptionValue(request, prefix + "operator_id:");
  expression.canonical_operator_id = ProjectionOptionValue(request, prefix + "canonical_operator_id:");
  expression.special_form_id = ProjectionOptionValue(request, prefix + "special_form_id:");
  expression.sblr_binding = ProjectionOptionValue(request, prefix + "sblr_binding:");
  if (depth > 4) return expression;

  std::uint64_t arg_count = ParseU64(ProjectionOptionValue(request, prefix + "function_arg_count:"));
  const std::uint64_t operator_arg_count =
      ParseU64(ProjectionOptionValue(request, prefix + "operator_arg_count:"));
  if (operator_arg_count > arg_count) arg_count = operator_arg_count;
  const std::uint64_t special_form_arg_count =
      ParseU64(ProjectionOptionValue(request, prefix + "special_form_arg_count:"));
  if (special_form_arg_count > arg_count) arg_count = special_form_arg_count;
  for (std::uint64_t arg_index = 0; arg_index < arg_count; ++arg_index) {
    expression.arguments.push_back(ReadProjectionExpression(
        request, prefix + "arg_" + std::to_string(arg_index) + "_", depth + 1));
  }
  return expression;
}

EngineProjectionFunctionResult EvaluateProjectionExpressionTree(
    const EngineEvaluateProjectionRequest& request,
    const EngineProjectionExpression& expression) {
  if (expression.expression_kind.empty() || expression.expression_kind == "literal") {
    EngineProjectionFunctionResult out;
    out.ok = true;
    out.value.descriptor = ProjectionDescriptor(expression.type_name);
    out.value.encoded_value = expression.encoded_value;
    out.value.is_null = expression.is_null;
    return out;
  }

  if (expression.expression_kind == "parameter") {
    EngineProjectionFunctionResult out;
    out.ok = true;
    out.value.descriptor.descriptor_kind = "parameter";
    out.value.descriptor.canonical_type_name =
        expression.type_name.empty() ? "unknown" : expression.type_name;
    out.value.descriptor.encoded_descriptor =
        "kind=input;type=" + out.value.descriptor.canonical_type_name;
    out.value.encoded_value = "unbound_parameter_descriptor";
    out.value.is_null = false;
    out.evidence.push_back({"query_parameter_value_execution", "false"});
    return out;
  }

  if (expression.expression_kind == "function") {
    if (!request.function_evaluator) {
      EngineProjectionFunctionResult out;
      out.ok = false;
      out.diagnostics.push_back(MakeInvalidRequestDiagnostic("query.evaluate_projection",
                                                             "function_projection_evaluator_required"));
      return out;
    }
    if (expression.function_id.empty()) {
      EngineProjectionFunctionResult out;
      out.ok = false;
      out.diagnostics.push_back(MakeInvalidRequestDiagnostic("query.evaluate_projection",
                                                             "function_projection_id_required"));
      return out;
    }

    EngineProjectionFunctionRequest function_request;
    function_request.context = request.context;
    function_request.function_id = expression.function_id;
    for (std::size_t arg_index = 0; arg_index < expression.arguments.size(); ++arg_index) {
      auto arg_result = EvaluateProjectionExpressionTree(request, expression.arguments[arg_index]);
      if (!arg_result.ok) return arg_result;
      EngineProjectionFunctionArgument argument;
      argument.name = expression.arguments[arg_index].name.empty()
                          ? "arg" + std::to_string(arg_index)
                          : expression.arguments[arg_index].name;
      argument.type_name = arg_result.value.descriptor.canonical_type_name;
      argument.encoded_value = arg_result.value.encoded_value;
      argument.is_null = arg_result.value.is_null;
      function_request.arguments.push_back(std::move(argument));
    }
    return request.function_evaluator(function_request);
  }

  if (expression.expression_kind == "operator" ||
      expression.expression_kind == "special_form") {
    if (!request.operator_evaluator) {
      EngineProjectionFunctionResult out;
      out.ok = false;
      out.diagnostics.push_back(MakeInvalidRequestDiagnostic("query.evaluate_projection",
                                                             "operator_projection_evaluator_required"));
      return out;
    }
    EngineProjectionOperatorRequest operator_request;
    operator_request.context = request.context;
    operator_request.expression = expression;
    return request.operator_evaluator(operator_request);
  }

  EngineProjectionFunctionResult out;
  out.ok = false;
  out.diagnostics.push_back(MakeInvalidRequestDiagnostic("query.evaluate_projection",
                                                         "unsupported_projection_expression_kind"));
  return out;
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
    auto expression_result = EvaluateProjectionExpressionTree(
        request, ReadProjectionExpression(request, prefix));
    if (!expression_result.ok) {
      if (expression_result.diagnostics.empty()) {
        return ProjectionFailure(request, "projection_expression_execution_failed");
      }
      return ProjectionFailure(request, std::move(expression_result.diagnostics.front()));
    }
    value = std::move(expression_result.value);
    for (auto& evidence : expression_result.evidence) {
      result.evidence.push_back(std::move(evidence));
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
