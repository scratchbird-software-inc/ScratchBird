// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "expression/expression_catalog.hpp"

#include "common/common.hpp"
#include "expression/reference_variable_compatibility.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"

#include <string_view>
#include <vector>

namespace scratchbird::parser::sbsql {
namespace {

bool HasPrefix(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool Contains(std::string_view value, std::string_view needle) {
  return value.find(needle) != std::string_view::npos;
}

std::string_view BehaviorDescriptorFor(const GeneratedSurfaceRegistryRow& row) {
  if (row.cluster_scope == "cluster_private") return "behavior.cluster_private.fail_closed_or_profile_gate";
  if (row.source_status == "native_future") return "behavior.native_future.exact_refusal_or_promoted_builtin";
  return "behavior.native_now.parse_bind_lower_engine_rule";
}

bool RequiresExactRefusal(const GeneratedSurfaceRegistryRow& row) {
  return row.source_status == "native_future" || row.cluster_scope == "cluster_private" ||
         Contains(row.final_acceptance_rule, "refusal") || Contains(row.engine_rule_key, "fail_closed");
}

std::string_view BinderDescriptorFor(const GeneratedSurfaceRegistryRow& row) {
  if (row.cluster_scope == "cluster_private") return "binder.expression_runtime.cluster_profile_gate";
  if (row.source_status == "native_future") return "binder.expression_runtime.native_future_refusal";
  if (row.surface_kind == "operator") return "binder.expression_runtime.operator";
  if (row.surface_kind == "variable") return "binder.expression_runtime.variable";
  return "binder.expression_runtime.function";
}

std::string_view ExpressionClassFor(const GeneratedSurfaceRegistryRow& row) {
  const auto upper = ToUpperAscii(row.canonical_name);
  if (row.surface_kind == "variable") return "session_variable_marker";
  if (row.surface_kind == "operator") {
    if (HasPrefix(row.canonical_name, "@@")) return "system_variable_operator";
    if (row.canonical_name == "f(name=>value)") return "named_argument_operator";
    return "operator";
  }
  if (Contains(upper, "OVER(") || Contains(upper, "WITHINGROUP(")) return "window_or_ordered_set_expression";
  if (Contains(upper, "JSON_")) return "json_expression";
  if (Contains(upper, "XML")) return "xml_expression";
  if (Contains(upper, "ST_")) return "spatial_expression";
  if (Contains(upper, "VECTOR")) return "vector_expression";
  if (Contains(upper, "UUID")) return "uuid_expression";
  if (Contains(upper, "REGEX")) return "regex_expression";
  if (Contains(upper, "CAST") || Contains(upper, "CONVERT")) return "conversion_expression";
  if (Contains(upper, "LITERAL")) return "literal_expression";
  if (Contains(upper, "SUM") || Contains(upper, "AVG") || Contains(upper, "COUNT") ||
      Contains(upper, "STDDEV") || Contains(upper, "VARIANCE") ||
      Contains(upper, "LISTAGG") || Contains(upper, "STRING_AGG")) {
    return "aggregate_expression";
  }
  return "scalar_or_special_form_expression";
}

std::string_view ArityClassFor(const GeneratedSurfaceRegistryRow& row) {
  if (row.surface_kind == "variable") return "variable_marker";
  if (row.surface_kind == "operator") {
    if (row.canonical_name == "@") return "prefix_variable_marker";
    if (HasPrefix(row.canonical_name, "@@")) return "zero_arg_system_variable";
    if (row.canonical_name == "f(name=>value)") return "named_argument_pair";
    return "operator_form";
  }
  if (!Contains(row.canonical_name, "(")) return "name_only";
  if (Contains(row.canonical_name, "...")) return "variadic";
  if (Contains(row.canonical_name, "[") || Contains(row.canonical_name, "|")) return "optional_or_alternation_signature";
  return "fixed_signature";
}

std::uint16_t PrecedenceFor(const GeneratedSurfaceRegistryRow& row) {
  if (row.surface_kind == "variable") return 100;
  if (row.canonical_name == "f(name=>value)") return 15;
  if (HasPrefix(row.canonical_name, "@@")) return 100;
  if (row.canonical_name == "@") return 100;
  return 0;
}

ExpressionAssociativity AssociativityFor(const GeneratedSurfaceRegistryRow& row) {
  if (row.surface_kind == "variable") return ExpressionAssociativity::kPrefix;
  if (row.canonical_name == "f(name=>value)") return ExpressionAssociativity::kRight;
  if (HasPrefix(row.canonical_name, "@@")) return ExpressionAssociativity::kNone;
  if (row.canonical_name == "@") return ExpressionAssociativity::kPrefix;
  if (row.surface_kind == "operator") return ExpressionAssociativity::kLeft;
  return ExpressionAssociativity::kNone;
}

ExpressionSurfaceDescriptor MakeDescriptor(const GeneratedSurfaceRegistryRow& row) {
  const auto parsed_kind = ParseExpressionSurfaceKind(row.surface_kind);
  return {
      row.surface_id,
      row.fixed_uuid_v7,
      row.canonical_name,
      parsed_kind.value_or(ExpressionSurfaceKind::kFunction),
      row.source_status,
      row.cluster_scope,
      row.parser_handler_key,
      BinderDescriptorFor(row),
      row.lowering_handler_key,
      row.engine_rule_key,
      row.diagnostic_key,
      row.final_acceptance_rule,
      BehaviorDescriptorFor(row),
      ExpressionClassFor(row),
      ArityClassFor(row),
      PrecedenceFor(row),
      AssociativityFor(row),
      RequiresExactRefusal(row),
  };
}

std::vector<ExpressionSurfaceDescriptor> BuildExpressionDescriptors() {
  std::vector<ExpressionSurfaceDescriptor> descriptors;
  for (const auto& row : GeneratedSurfaceRegistryRows()) {
    if (row.family != "expression_runtime") continue;
    if (IsReferenceVariableCompatibilitySurface(row.surface_id) ||
        IsReferenceVariableCompatibilitySpelling(row.canonical_name)) {
      continue;
    }
    descriptors.push_back(MakeDescriptor(row));
  }
  return descriptors;
}

const std::vector<ExpressionSurfaceDescriptor>& DescriptorStorage() {
  static const auto descriptors = BuildExpressionDescriptors();
  return descriptors;
}

} // namespace

std::span<const ExpressionSurfaceDescriptor> BuiltinExpressionSurfaceDescriptors() {
  const auto& descriptors = DescriptorStorage();
  return {descriptors.data(), descriptors.size()};
}

const ExpressionSurfaceDescriptor* FindExpressionSurfaceById(std::string_view surface_id) {
  for (const auto& descriptor : BuiltinExpressionSurfaceDescriptors()) {
    if (descriptor.surface_id == surface_id) return &descriptor;
  }
  return nullptr;
}

const ExpressionSurfaceDescriptor* FindExpressionSurfaceByName(std::string_view canonical_name) {
  const auto wanted = ToUpperAscii(canonical_name);
  for (const auto& descriptor : BuiltinExpressionSurfaceDescriptors()) {
    if (ToUpperAscii(descriptor.canonical_name) == wanted) return &descriptor;
  }
  return nullptr;
}

std::optional<ExpressionSurfaceKind> ParseExpressionSurfaceKind(std::string_view kind) {
  if (kind == "function") return ExpressionSurfaceKind::kFunction;
  if (kind == "operator") return ExpressionSurfaceKind::kOperator;
  if (kind == "variable") return ExpressionSurfaceKind::kVariable;
  return std::nullopt;
}

std::string_view ExpressionSurfaceKindName(ExpressionSurfaceKind kind) {
  switch (kind) {
    case ExpressionSurfaceKind::kFunction: return "function";
    case ExpressionSurfaceKind::kOperator: return "operator";
    case ExpressionSurfaceKind::kVariable: return "variable";
  }
  return "function";
}

std::string_view ExpressionAssociativityName(ExpressionAssociativity associativity) {
  switch (associativity) {
    case ExpressionAssociativity::kNone: return "none";
    case ExpressionAssociativity::kLeft: return "left";
    case ExpressionAssociativity::kRight: return "right";
    case ExpressionAssociativity::kPrefix: return "prefix";
    case ExpressionAssociativity::kPostfix: return "postfix";
  }
  return "none";
}

} // namespace scratchbird::parser::sbsql
