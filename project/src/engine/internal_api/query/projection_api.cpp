// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/projection_api.hpp"

#include "datatype_operations.hpp"

#ifndef SCRATCHBIRD_QOW_TYPED_PARAMETER_CONTRACT_ONLY
#include "behavior_support/api_behavior_store.hpp"
#include "api_diagnostics.hpp"
#include "security/security_model.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <limits>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>

namespace scratchbird::engine::internal_api {

namespace {

constexpr std::size_t kQowMaximumProjectionExpressionNodesV1 = 131072;
constexpr std::size_t kQowMaximumProjectionExpressionDepthV1 = 256;
constexpr std::size_t kQowMaximumProjectionExpressionFanoutV1 = 1024;
constexpr std::size_t kQowMaximumProjectionExpressionReferencesV1 = 1048576;

std::string QowProjectionOptionValueV1(const EngineApiRequest& request,
                                       const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    if (option.rfind(prefix, 0) == 0) return option.substr(prefix.size());
  }
  return {};
}

bool QowParseProjectionU64V1(const std::string& encoded,
                             const std::uint64_t maximum,
                             std::uint64_t* value) {
  if (value == nullptr || encoded.empty()) return false;
  std::uint64_t parsed = 0;
  for (const char ch : encoded) {
    if (ch < '0' || ch > '9') return false;
    const auto digit = static_cast<std::uint64_t>(ch - '0');
    if (parsed > (maximum - digit) / 10) return false;
    parsed = parsed * 10 + digit;
  }
  *value = parsed;
  return true;
}

bool QowProjectionResourceLimitReasonV1(const std::string& reason) {
  return reason == "node_count" || reason == "reference_count" ||
         reason == "maximum_fanout" || reason == "maximum_depth";
}

scratchbird::core::datatypes::CanonicalTypeId
QowProjectionCanonicalTypeIdV1(const std::string& type_name) {
  using scratchbird::core::datatypes::CanonicalTypeId;
  const auto direct =
      scratchbird::core::datatypes::CanonicalTypeIdFromStableName(type_name);
  if (direct != CanonicalTypeId::unknown) return direct;
  // These are closed projection descriptor profiles already emitted and
  // consumed by the neutral scalar runtime. They retain their profile spelling
  // at execution, but their storage/type authority is the exact Core base
  // descriptor below.
  if (type_name == "numeric.fixed") return CanonicalTypeId::decimal;
  if (type_name == "character.none" || type_name == "character.utf8") {
    return CanonicalTypeId::character;
  }
  if (type_name == "blob.binary") return CanonicalTypeId::blob;
  return CanonicalTypeId::unknown;
}

}  // namespace

// QOW-SOURCE-QRY-027-V1
bool QowValidateCanonicalExpressionGraphV1(
    const std::vector<std::uint32_t>& expression_ids,
    const std::vector<std::vector<std::uint32_t>>& child_expression_ids,
    const std::vector<bool>& shareable,
    const std::vector<std::uint32_t>& root_expression_ids,
    std::size_t* validated_node_count,
    std::size_t* maximum_observed_depth,
    std::string* refusal_reason,
    std::string* refusal_detail) {
  if (validated_node_count == nullptr || maximum_observed_depth == nullptr ||
      refusal_reason == nullptr || refusal_detail == nullptr) {
    return false;
  }
  *validated_node_count = 0;
  *maximum_observed_depth = 0;
  refusal_reason->clear();
  refusal_detail->clear();
  const auto refuse = [&](std::string reason, std::string detail) {
    *validated_node_count = 0;
    *maximum_observed_depth = 0;
    *refusal_reason = std::move(reason);
    *refusal_detail = std::move(detail);
    return false;
  };

  if (expression_ids.empty() || root_expression_ids.empty()) {
    return refuse("root_expression_missing",
                  "canonical expression graph requires at least one root");
  }
  if (expression_ids.size() != child_expression_ids.size() ||
      expression_ids.size() != shareable.size()) {
    return refuse("graph_shape",
                  "expression graph node vectors have different cardinalities");
  }
  if (expression_ids.size() > kQowMaximumProjectionExpressionNodesV1 ||
      root_expression_ids.size() > kQowMaximumProjectionExpressionNodesV1) {
    return refuse("node_count",
                  "canonical expression graph exceeds the node limit");
  }

  std::unordered_map<std::uint32_t, std::size_t> node_indexes;
  node_indexes.reserve(expression_ids.size());
  for (std::size_t index = 0; index < expression_ids.size(); ++index) {
    if (expression_ids[index] == 0 ||
        !node_indexes.emplace(expression_ids[index], index).second) {
      return refuse("node_identity",
                    "expression identifiers must be nonzero and unique");
    }
  }

  std::vector<std::size_t> incoming_references(expression_ids.size(), 0);
  std::size_t reference_count = 0;
  for (std::size_t index = 0; index < expression_ids.size(); ++index) {
    if (child_expression_ids[index].size() >
        kQowMaximumProjectionExpressionFanoutV1) {
      return refuse("maximum_fanout",
                    "canonical expression node exceeds the fanout limit");
    }
    if (child_expression_ids[index].size() >
        kQowMaximumProjectionExpressionReferencesV1 - reference_count) {
      return refuse("reference_count",
                    "canonical expression graph exceeds the reference limit");
    }
    reference_count += child_expression_ids[index].size();
    for (const auto child_id : child_expression_ids[index]) {
      const auto child = node_indexes.find(child_id);
      if (child_id == 0 || child == node_indexes.end()) {
        return refuse("dangling_reference",
                      "expression child identifier does not resolve");
      }
      ++incoming_references[child->second];
    }
  }

  std::vector<bool> root_seen(expression_ids.size(), false);
  for (const auto root_id : root_expression_ids) {
    const auto root = node_indexes.find(root_id);
    if (root_id == 0 || root == node_indexes.end() || root_seen[root->second]) {
      return refuse("root_identity",
                    "expression root identifiers must resolve uniquely");
    }
    root_seen[root->second] = true;
    ++incoming_references[root->second];
  }
  for (std::size_t index = 0; index < expression_ids.size(); ++index) {
    if (incoming_references[index] > 1 && !shareable[index]) {
      return refuse("unshareable_reference",
                    "multiply referenced expression is not declared shareable");
    }
  }

  std::vector<bool> reachable(expression_ids.size(), false);
  std::vector<std::size_t> pending;
  pending.reserve(expression_ids.size());
  for (const auto root_id : root_expression_ids) {
    pending.push_back(node_indexes.at(root_id));
  }
  while (!pending.empty()) {
    const auto index = pending.back();
    pending.pop_back();
    if (reachable[index]) continue;
    reachable[index] = true;
    for (const auto child_id : child_expression_ids[index]) {
      pending.push_back(node_indexes.at(child_id));
    }
  }
  if (std::find(reachable.begin(), reachable.end(), false) != reachable.end()) {
    return refuse("orphan_node",
                  "canonical expression graph contains an unreachable node");
  }

  std::vector<std::size_t> indegree(expression_ids.size(), 0);
  for (const auto& children : child_expression_ids) {
    for (const auto child_id : children) {
      ++indegree[node_indexes.at(child_id)];
    }
  }
  std::queue<std::size_t> ready;
  for (std::size_t index = 0; index < indegree.size(); ++index) {
    if (indegree[index] == 0) ready.push(index);
  }
  std::vector<std::size_t> depth(expression_ids.size(), 0);
  for (const auto root_id : root_expression_ids) {
    depth[node_indexes.at(root_id)] = 1;
  }
  std::size_t visited = 0;
  while (!ready.empty()) {
    const auto index = ready.front();
    ready.pop();
    ++visited;
    for (const auto child_id : child_expression_ids[index]) {
      const auto child_index = node_indexes.at(child_id);
      if (depth[index] != 0) {
        depth[child_index] =
            std::max(depth[child_index], depth[index] + 1);
      }
      if (--indegree[child_index] == 0) ready.push(child_index);
    }
  }
  if (visited != expression_ids.size()) {
    return refuse("cycle",
                  "canonical expression graph contains a cycle");
  }
  const auto observed_depth =
      *std::max_element(depth.begin(), depth.end());
  if (observed_depth > kQowMaximumProjectionExpressionDepthV1) {
    return refuse("maximum_depth",
                  "canonical expression graph exceeds the depth limit");
  }

  *validated_node_count = expression_ids.size();
  *maximum_observed_depth = observed_depth;
  return true;
}

namespace {

struct QowProjectionExpressionBuildStateV1 {
  std::vector<std::uint32_t> expression_ids;
  std::vector<std::vector<std::uint32_t>> child_expression_ids;
  std::vector<bool> shareable;
  std::vector<std::uint32_t> root_expression_ids;
};

bool QowReadProjectionExpressionV1(
    const EngineApiRequest& request,
    const std::string& prefix,
    const std::size_t depth,
    QowProjectionExpressionBuildStateV1* graph,
    EngineProjectionExpression* expression,
    std::string* refusal_reason,
    std::string* refusal_detail) {
  if (graph == nullptr || expression == nullptr || refusal_reason == nullptr ||
      refusal_detail == nullptr) {
    return false;
  }
  const auto refuse = [&](std::string reason, std::string detail) {
    *expression = EngineProjectionExpression{};
    *refusal_reason = std::move(reason);
    *refusal_detail = std::move(detail);
    return false;
  };
  if (depth > kQowMaximumProjectionExpressionDepthV1) {
    return refuse("maximum_depth",
                  "projection expression exceeds the canonical depth limit");
  }
  if (graph->expression_ids.size() >=
      kQowMaximumProjectionExpressionNodesV1) {
    return refuse("node_count",
                  "projection expression exceeds the canonical node limit");
  }

  const auto node_index = graph->expression_ids.size();
  const auto expression_id = static_cast<std::uint32_t>(node_index + 1);
  graph->expression_ids.push_back(expression_id);
  graph->child_expression_ids.emplace_back();
  graph->shareable.push_back(false);

  expression->name = QowProjectionOptionValueV1(request, prefix + "name:");
  expression->expression_kind =
      QowProjectionOptionValueV1(request, prefix + "expr_kind:");
  if (expression->expression_kind != "literal" &&
      expression->expression_kind != "parameter" &&
      expression->expression_kind != "function" &&
      expression->expression_kind != "operator" &&
      expression->expression_kind != "special_form") {
    return refuse("expression_kind",
                  "projection expression kind is missing or unsupported");
  }
  expression->type_name = QowProjectionOptionValueV1(request, prefix + "type:");
  if (expression->type_name.empty()) {
    return refuse("descriptor_missing",
                  "projection expression requires a resolved type descriptor");
  }
  if (QowProjectionCanonicalTypeIdV1(expression->type_name) ==
          scratchbird::core::datatypes::CanonicalTypeId::unknown) {
    return refuse("descriptor_unsupported",
                  "projection expression type descriptor is unsupported");
  }
  expression->encoded_value = QowProjectionOptionValueV1(request, prefix + "value:");
  const auto is_null = QowProjectionOptionValueV1(request, prefix + "is_null:");
  expression->is_null = is_null == "true" || is_null == "1";
  expression->function_id =
      QowProjectionOptionValueV1(request, prefix + "function_id:");
  expression->operator_id =
      QowProjectionOptionValueV1(request, prefix + "operator_id:");
  expression->canonical_operator_id =
      QowProjectionOptionValueV1(request, prefix + "canonical_operator_id:");
  expression->special_form_id =
      QowProjectionOptionValueV1(request, prefix + "special_form_id:");
  expression->sblr_binding =
      QowProjectionOptionValueV1(request, prefix + "sblr_binding:");

  std::string argument_count_key;
  if (expression->expression_kind == "function") {
    if (expression->function_id.empty()) {
      return refuse("function_id", "projection function id is required");
    }
    argument_count_key = "function_arg_count:";
  } else if (expression->expression_kind == "operator") {
    if (expression->operator_id.empty() &&
        expression->canonical_operator_id.empty()) {
      return refuse("operator_id", "projection operator id is required");
    }
    argument_count_key = "operator_arg_count:";
  } else if (expression->expression_kind == "special_form") {
    if (expression->special_form_id.empty()) {
      return refuse("special_form_id",
                    "projection special-form id is required");
    }
    argument_count_key = "special_form_arg_count:";
  } else if (expression->expression_kind == "parameter" &&
             expression->name.empty()) {
    return refuse("parameter_name", "projection parameter name is required");
  }
  for (const auto& candidate : {std::string("function_arg_count:"),
                                std::string("operator_arg_count:"),
                                std::string("special_form_arg_count:")}) {
    if (candidate != argument_count_key &&
        !QowProjectionOptionValueV1(request, prefix + candidate).empty()) {
      return refuse("argument_count_kind",
                    "projection argument count does not match expression kind");
    }
  }
  std::uint64_t argument_count = 0;
  if (!argument_count_key.empty()) {
    const auto encoded_count =
        QowProjectionOptionValueV1(request, prefix + argument_count_key);
    if (encoded_count.empty() ||
        !QowParseProjectionU64V1(
            encoded_count, std::numeric_limits<std::uint64_t>::max(),
            &argument_count)) {
      return refuse(
          "argument_count",
          "projection expression argument count is required and must be canonical");
    }
  }
  if (argument_count > kQowMaximumProjectionExpressionFanoutV1) {
    return refuse("maximum_fanout",
                  "projection expression exceeds the canonical fanout limit");
  }

  expression->arguments.reserve(static_cast<std::size_t>(argument_count));
  graph->child_expression_ids[node_index].reserve(
      static_cast<std::size_t>(argument_count));
  for (std::uint64_t index = 0; index < argument_count; ++index) {
    EngineProjectionExpression child;
    const auto child_id =
        static_cast<std::uint32_t>(graph->expression_ids.size() + 1);
    if (!QowReadProjectionExpressionV1(
            request, prefix + "arg_" + std::to_string(index) + "_", depth + 1,
            graph, &child, refusal_reason, refusal_detail)) {
      *expression = EngineProjectionExpression{};
      return false;
    }
    graph->child_expression_ids[node_index].push_back(child_id);
    expression->arguments.push_back(std::move(child));
  }
  return true;
}

}  // namespace

bool QowReadCanonicalProjectionExpressionsV1(
    const EngineApiRequest& request,
    const std::uint64_t projection_count,
    std::vector<EngineProjectionExpression>* expressions,
    std::size_t* validated_node_count,
    std::size_t* maximum_observed_depth,
    std::string* refusal_reason,
    std::string* refusal_detail) {
  if (expressions == nullptr || validated_node_count == nullptr ||
      maximum_observed_depth == nullptr || refusal_reason == nullptr ||
      refusal_detail == nullptr) {
    return false;
  }
  expressions->clear();
  *validated_node_count = 0;
  *maximum_observed_depth = 0;
  refusal_reason->clear();
  refusal_detail->clear();
  const auto refuse = [&](std::string reason, std::string detail) {
    expressions->clear();
    *validated_node_count = 0;
    *maximum_observed_depth = 0;
    *refusal_reason = std::move(reason);
    *refusal_detail = std::move(detail);
    return false;
  };
  if (projection_count == 0) {
    return refuse("root_expression_missing",
                  "projection expression graph requires at least one root");
  }
  if (projection_count > kQowMaximumProjectionExpressionNodesV1) {
    return refuse("node_count",
                  "projection count exceeds the canonical expression node limit");
  }

  QowProjectionExpressionBuildStateV1 graph;
  graph.expression_ids.reserve(static_cast<std::size_t>(projection_count));
  graph.child_expression_ids.reserve(static_cast<std::size_t>(projection_count));
  graph.shareable.reserve(static_cast<std::size_t>(projection_count));
  graph.root_expression_ids.reserve(static_cast<std::size_t>(projection_count));
  expressions->reserve(static_cast<std::size_t>(projection_count));
  for (std::uint64_t index = 0; index < projection_count; ++index) {
    EngineProjectionExpression expression;
    const auto root_id =
        static_cast<std::uint32_t>(graph.expression_ids.size() + 1);
    if (!QowReadProjectionExpressionV1(
            request, "projection_" + std::to_string(index) + "_", 1, &graph,
            &expression, refusal_reason, refusal_detail)) {
      expressions->clear();
      *validated_node_count = 0;
      *maximum_observed_depth = 0;
      return false;
    }
    graph.root_expression_ids.push_back(root_id);
    expressions->push_back(std::move(expression));
  }
  if (!QowValidateCanonicalExpressionGraphV1(
          graph.expression_ids, graph.child_expression_ids, graph.shareable,
          graph.root_expression_ids, validated_node_count,
          maximum_observed_depth, refusal_reason, refusal_detail)) {
    expressions->clear();
    return false;
  }
  return true;
}

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

EngineDescriptor ProjectionDescriptor(const std::string& type_name) {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = type_name;
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

EngineProjectionFunctionResult EvaluateProjectionExpressionTree(
    const EngineEvaluateProjectionRequest& request,
    const EngineProjectionExpression& expression) {
  if (expression.expression_kind == "literal") {
    EngineProjectionFunctionResult out;
    out.ok = true;
    out.value.descriptor = ProjectionDescriptor(expression.type_name);
    out.value.encoded_value = expression.encoded_value;
    out.value.is_null = expression.is_null;
    return out;
  }

  if (expression.expression_kind == "parameter") {
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
      diagnostic.fields.push_back({"reason", std::move(refusal_reason)});
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
      out.evidence.push_back({"query_parameter_value_execution", "true"});
      out.evidence.push_back({"query_parameter_name", expression.name});
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

EngineApiDiagnostic ProjectionGraphDiagnostic(std::string reason,
                                              std::string detail) {
  const auto resource_limit = QowProjectionResourceLimitReasonV1(reason);
  auto diagnostic = MakeEngineApiDiagnostic(
      resource_limit ? "SBLR.PLAN_TREE.RESOURCE_LIMIT"
                     : "SBLR.PLAN_TREE.INVALID_HANDLE",
      resource_limit ? "engine.query.expression_graph_resource_limit"
                     : "engine.query.expression_graph_invalid_handle",
      std::move(detail));
  diagnostic.fields.push_back({"reason", std::move(reason)});
  return diagnostic;
}

EngineBindProjectionResult BindProjectionGraphFailure(
    const EngineBindProjectionRequest& request,
    std::string reason,
    std::string detail) {
  EngineBindProjectionResult result;
  result.ok = false;
  result.operation_id = "query.bind_projection";
  result.embedded_trust_mode_observed =
      request.context.trust_mode == EngineTrustMode::embedded_in_process;
  result.diagnostics.push_back(
      ProjectionGraphDiagnostic(std::move(reason), std::move(detail)));
  return result;
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_QUERY_PROJECTION_API_BEHAVIOR
EngineBindProjectionResult EngineBindProjection(const EngineBindProjectionRequest& request) {
  std::size_t validated_node_count = 0;
  std::size_t maximum_observed_depth = 0;
  std::string refusal_reason;
  std::string refusal_detail;
  std::vector<EngineProjectionExpression> bound_expressions;
  const auto encoded_projection_count =
      QowProjectionOptionValueV1(request, "projection_count:");
  if (!encoded_projection_count.empty()) {
    std::uint64_t projection_count = 0;
    if (!QowParseProjectionU64V1(
            encoded_projection_count, std::numeric_limits<std::uint64_t>::max(),
            &projection_count)) {
      return BindProjectionGraphFailure(
          request, "root_count",
          "projection root count is malformed");
    }
    if (!QowReadCanonicalProjectionExpressionsV1(
            request, projection_count, &bound_expressions,
            &validated_node_count,
            &maximum_observed_depth, &refusal_reason, &refusal_detail)) {
      return BindProjectionGraphFailure(
          request, std::move(refusal_reason), std::move(refusal_detail));
    }
  } else if (!request.projection.canonical_projection_envelopes.empty()) {
    return BindProjectionGraphFailure(
        request, "projection_envelope_not_decoded",
        "opaque projection envelopes cannot be accepted as bound expressions");
  } else {
    return BindProjectionGraphFailure(
        request, "root_expression_missing",
        "canonical projection binding input is required");
  }
  auto result = MakeApiBehaviorSuccess<EngineBindProjectionResult>(request.context, "query.bind_projection");
  result.bound_expressions = std::move(bound_expressions);
  result.bound_projection = request.projection;
  result.validated_node_count = validated_node_count;
  result.maximum_observed_depth = maximum_observed_depth;
  result.result_shape.result_kind = "bound_projection";
  AddApiBehaviorEvidence(&result, "query_binding", "projection");
  if (validated_node_count != 0) {
    AddApiBehaviorEvidence(
        &result, "query_expression_graph_validated_nodes",
        std::to_string(validated_node_count));
    AddApiBehaviorEvidence(
        &result, "query_expression_graph_maximum_depth",
        std::to_string(maximum_observed_depth));
  }
  AddApiBehaviorRow(&result, {{"projection_count", std::to_string(request.projection.canonical_projection_envelopes.size())}, {"payload", ApiBehaviorPayloadFromRequest(request)}});
  return result;
}

EngineEvaluateProjectionResult EngineEvaluateProjection(const EngineEvaluateProjectionRequest& request) {
  const auto encoded_projection_count =
      QowProjectionOptionValueV1(request, "projection_count:");
  std::uint64_t projection_count = 0;
  if (!QowParseProjectionU64V1(
          encoded_projection_count, std::numeric_limits<std::uint64_t>::max(),
          &projection_count) ||
      projection_count == 0) {
    return ProjectionFailure(
        request,
        ProjectionGraphDiagnostic(
            encoded_projection_count.empty() ? "root_expression_missing"
                                             : "root_count",
            encoded_projection_count.empty()
                ? "projection operand is required"
                : "projection root count is malformed"));
  }

  std::vector<EngineProjectionExpression> expressions;
  std::size_t validated_node_count = 0;
  std::size_t maximum_observed_depth = 0;
  std::string refusal_reason;
  std::string refusal_detail;
  if (!QowReadCanonicalProjectionExpressionsV1(
          request, projection_count, &expressions, &validated_node_count,
          &maximum_observed_depth, &refusal_reason, &refusal_detail)) {
    return ProjectionFailure(
        request, ProjectionGraphDiagnostic(
                     std::move(refusal_reason), std::move(refusal_detail)));
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
        request, expressions[static_cast<std::size_t>(index)]);
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
  result.evidence.push_back(
      {"query_expression_graph_validated_nodes",
       std::to_string(validated_node_count)});
  result.evidence.push_back(
      {"query_expression_graph_maximum_depth",
       std::to_string(maximum_observed_depth)});
  result.evidence.push_back({"query_projection", "constant_projection_engine_evaluated"});
  AddApiBehaviorEvidence(&result, "query_binding", "evaluate_projection");
  return result;
}

#endif  // SCRATCHBIRD_QOW_TYPED_PARAMETER_CONTRACT_ONLY

}  // namespace scratchbird::engine::internal_api
