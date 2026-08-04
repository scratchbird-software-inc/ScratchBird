// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/projection_api.hpp"

#ifndef SCRATCHBIRD_QOW_TYPED_PARAMETER_CONTRACT_ONLY
#include "behavior_support/api_behavior_store.hpp"
#include "api_diagnostics.hpp"
#include "security/security_model.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <utility>

namespace scratchbird::engine::internal_api {

// QOW-SOURCE-QRY-026-V1
bool QowBindCanonicalParameterSlotsV1(
    const std::vector<EngineDescriptor>& parameter_descriptors,
    const std::vector<std::pair<std::string, EngineTypedValue>>& supplied_parameters,
    std::vector<EngineTypedValue>* bound_values,
    std::string* refusal_reason,
    std::string* refusal_detail) {
  if (bound_values == nullptr || refusal_reason == nullptr ||
      refusal_detail == nullptr) {
    return false;
  }
  bound_values->clear();
  refusal_reason->clear();
  refusal_detail->clear();
  const auto refuse = [&](std::string reason, std::string detail) {
    bound_values->clear();
    *refusal_reason = std::move(reason);
    *refusal_detail = std::move(detail);
    return false;
  };
  const auto canonical_uuid = [](const std::string& value) {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
        value[18] != '-' || value[23] != '-') {
      return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
      if (index == 8 || index == 13 || index == 18 || index == 23) continue;
      const auto ch = static_cast<unsigned char>(value[index]);
      if (!std::isxdigit(ch) || std::isupper(ch)) return false;
    }
    return true;
  };
  const auto descriptor_field = [](const std::string& encoded,
                                   const std::string& key) {
    const std::string prefix = key + "=";
    std::string value;
    bool found = false;
    std::size_t start = 0;
    while (start <= encoded.size()) {
      const auto end = encoded.find(';', start);
      const auto field = encoded.substr(
          start, end == std::string::npos ? std::string::npos : end - start);
      if (field.rfind(prefix, 0) == 0) {
        if (found) return std::string{};
        value = field.substr(prefix.size());
        found = true;
      }
      if (end == std::string::npos) break;
      start = end + 1;
    }
    return value;
  };

  if (parameter_descriptors.empty()) {
    return refuse("parameter_slots_missing",
                  "typed parameter binding requires declared parameter slots");
  }
  if (supplied_parameters.size() < parameter_descriptors.size()) {
    return refuse("parameter_missing",
                  "one or more typed parameter values are missing");
  }
  if (supplied_parameters.size() > parameter_descriptors.size()) {
    return refuse("parameter_extra",
                  "typed parameter input contains an undeclared value");
  }

  std::vector<std::string> names;
  names.reserve(supplied_parameters.size());
  bound_values->reserve(parameter_descriptors.size());
  for (std::size_t index = 0; index < parameter_descriptors.size(); ++index) {
    const auto& slot = parameter_descriptors[index];
    const auto& supplied = supplied_parameters[index];
    const auto nullability =
        descriptor_field(slot.encoded_descriptor, "nullability");
    if (!canonical_uuid(slot.descriptor_uuid.canonical) ||
        slot.descriptor_kind != "scalar" || slot.canonical_type_name.empty() ||
        slot.canonical_type_name == "unknown" ||
        slot.encoded_descriptor.empty() ||
        (nullability != "nullable" && nullability != "non_null")) {
      return refuse("parameter_descriptor_invalid",
                    "declared parameter slot descriptor is not canonical");
    }
    if (supplied.first.empty() ||
        std::find(names.begin(), names.end(), supplied.first) != names.end()) {
      return refuse("parameter_name_invalid",
                    "typed parameter names must be nonempty and unique");
    }
    names.push_back(supplied.first);
    const auto& value = supplied.second;
    if (value.descriptor.descriptor_uuid.canonical !=
            slot.descriptor_uuid.canonical ||
        value.descriptor.descriptor_kind != slot.descriptor_kind ||
        value.descriptor.canonical_type_name != slot.canonical_type_name ||
        value.descriptor.encoded_descriptor != slot.encoded_descriptor) {
      return refuse("parameter_wrong_type",
                    "typed parameter value does not match its declared descriptor");
    }
    if (value.state == EngineValueState::sql_null) {
      if (!value.is_null || !value.encoded_value.empty() ||
          !value.binary_value.empty() || nullability != "nullable") {
        return refuse("parameter_null_invalid",
                      "SQL NULL is invalid for the declared parameter slot");
      }
    } else if (value.state != EngineValueState::value || value.is_null) {
      return refuse("parameter_value_state_invalid",
                    "typed parameter value state is not executable");
    }
    bound_values->push_back(value);
  }
  return true;
}

#ifndef SCRATCHBIRD_QOW_TYPED_PARAMETER_CONTRACT_ONLY
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
    if (!request.descriptors.empty() || !request.assignments.empty()) {
      std::vector<EngineTypedValue> bound_values;
      std::string refusal_reason;
      std::string refusal_detail;
      if (!QowBindCanonicalParameterSlotsV1(
              request.descriptors, request.assignments, &bound_values,
              &refusal_reason, &refusal_detail)) {
        EngineProjectionFunctionResult out;
        out.ok = false;
        auto diagnostic = MakeEngineApiDiagnostic(
            "QOW-DIAG-QRY-026-REFUSAL-V1",
            "engine.query.typed_parameter_binding_refused",
            std::move(refusal_detail));
        diagnostic.fields.push_back(
            {"reason", std::move(refusal_reason)});
        out.diagnostics.push_back(std::move(diagnostic));
        return out;
      }
      for (std::size_t index = 0; index < request.assignments.size(); ++index) {
        if (request.assignments[index].first != expression.name) continue;
        EngineProjectionFunctionResult out;
        out.ok = true;
        out.value = std::move(bound_values[index]);
        if (!expression.type_name.empty() &&
            expression.type_name != out.value.descriptor.canonical_type_name) {
          out.ok = false;
          out.value = EngineTypedValue{};
          out.diagnostics.push_back(MakeEngineApiDiagnostic(
              "QOW-DIAG-QRY-026-REFUSAL-V1",
              "engine.query.typed_parameter_binding_refused",
              "projection parameter type does not match the bound slot"));
          return out;
        }
        out.evidence.push_back(
            {"query_parameter_value_execution", "true"});
        out.evidence.push_back(
            {"query_parameter_name", expression.name});
        return out;
      }
      EngineProjectionFunctionResult out;
      out.ok = false;
      out.diagnostics.push_back(MakeEngineApiDiagnostic(
          "QOW-DIAG-QRY-026-REFUSAL-V1",
          "engine.query.typed_parameter_binding_refused",
          "projection parameter name does not resolve to a bound slot"));
      return out;
    }
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
    std::vector<EngineEvidenceReference> argument_evidence;
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
      argument_evidence.insert(argument_evidence.end(),
                               arg_result.evidence.begin(),
                               arg_result.evidence.end());
    }
    auto out = request.function_evaluator(function_request);
    if (out.ok) {
      out.evidence.insert(out.evidence.end(),
                          argument_evidence.begin(),
                          argument_evidence.end());
    }
    return out;
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

#endif  // SCRATCHBIRD_QOW_TYPED_PARAMETER_CONTRACT_ONLY

}  // namespace scratchbird::engine::internal_api
