// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_relational_expression.hpp"

#include "datatype_operations.hpp"
#include "query/expression_api.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;
namespace {

std::string UpperAscii(std::string value) {
  std::ranges::transform(value, value.begin(), [](const unsigned char byte) {
    return static_cast<char>(std::toupper(byte));
  });
  return value;
}

bool IsCanonicalUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto byte = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(byte) || std::isupper(byte)) return false;
  }
  return true;
}

bool ParseInt64(const std::string& encoded, std::int64_t* value) {
  if (value == nullptr || encoded.empty()) return false;
  const auto [end, error] = std::from_chars(
      encoded.data(), encoded.data() + encoded.size(), *value);
  return error == std::errc{} && end == encoded.data() + encoded.size();
}

bool TruthFromValue(const api::EngineTypedValue& value,
                    api::EngineSqlTruthValue* truth,
                    std::string* refusal_detail) {
  if (truth == nullptr || refusal_detail == nullptr) return false;
  if (value.descriptor.canonical_type_name != "boolean") {
    *refusal_detail = "logical operand is not canonical boolean";
    return false;
  }
  if (value.isSqlNull()) {
    *truth = api::EngineSqlTruthValue::unknown;
    return true;
  }
  if (value.state != api::EngineValueState::value || value.is_null ||
      (value.encoded_value != "true" && value.encoded_value != "false")) {
    *refusal_detail = "logical operand has a noncanonical boolean payload";
    return false;
  }
  *truth = value.encoded_value == "true"
               ? api::EngineSqlTruthValue::true_value
               : api::EngineSqlTruthValue::false_value;
  return true;
}

bool IsComparisonOperator(const std::string_view operation) {
  return operation == "=" || operation == "<>" || operation == "!=" ||
         operation == "<" || operation == "<=" || operation == ">" ||
         operation == ">=";
}

api::EngineComparisonPredicateOperator ComparisonOperator(
    const std::string_view operation) {
  if (operation == "=") return api::EngineComparisonPredicateOperator::equal;
  if (operation == "<>" || operation == "!=") {
    return api::EngineComparisonPredicateOperator::not_equal;
  }
  if (operation == "<") {
    return api::EngineComparisonPredicateOperator::less_than;
  }
  if (operation == "<=") {
    return api::EngineComparisonPredicateOperator::less_than_or_equal;
  }
  if (operation == ">") {
    return api::EngineComparisonPredicateOperator::greater_than;
  }
  return api::EngineComparisonPredicateOperator::greater_than_or_equal;
}

bool IsAdmittedComparisonType(const std::string_view type_name) {
  // Character comparison requires catalog-bound collation authority.
  return type_name == "int64" || type_name == "boolean";
}

}  // namespace

CanonicalRelationalExpressionRuntime::CanonicalRelationalExpressionRuntime(
    const api::TypedRelationalDag& dag) {
  for (const auto& descriptor : dag.descriptors) {
    descriptors_.emplace(descriptor.descriptor_id, &descriptor);
  }
  for (const auto& expression : dag.expressions) {
    expressions_.emplace(expression.expression_id, &expression);
  }
}

bool CanonicalRelationalExpressionRuntime::BindDescriptorType(
    const std::uint32_t descriptor_id,
    const std::string_view type_name,
    std::string* refusal_detail) {
  if (refusal_detail == nullptr || type_name.empty() || type_name == "null" ||
      !descriptors_.contains(descriptor_id)) {
    if (refusal_detail != nullptr) {
      *refusal_detail = "expression result descriptor type is unresolved";
    }
    return false;
  }
  const auto [entry, inserted] = descriptor_type_names_.emplace(
      descriptor_id, std::string(type_name));
  if (!inserted && entry->second != type_name) {
    *refusal_detail = "expression descriptor has incompatible inferred types";
    return false;
  }
  return true;
}

bool CanonicalRelationalExpressionRuntime::InferType(
    const std::uint32_t expression_id,
    const std::optional<std::string_view> expected_type,
    std::string* canonical_type_name,
    std::string* refusal_detail) {
  if (canonical_type_name == nullptr || refusal_detail == nullptr) return false;
  canonical_type_name->clear();
  refusal_detail->clear();
  return InferTypeInternal(expression_id, expected_type, canonical_type_name,
                           refusal_detail);
}

bool CanonicalRelationalExpressionRuntime::InferTypeInternal(
    const std::uint32_t expression_id,
    const std::optional<std::string_view> expected_type,
    std::string* canonical_type_name,
    std::string* refusal_detail) {
  const auto found = expressions_.find(expression_id);
  if (found == expressions_.end()) {
    *refusal_detail = "expression handle is absent from the bound graph";
    return false;
  }
  if (!inference_stack_.insert(expression_id).second) {
    *refusal_detail = "expression graph cycle reached scalar runtime";
    return false;
  }
  const auto leave = [&](const bool result) {
    inference_stack_.erase(expression_id);
    return result;
  };
  const auto& expression = *found->second;
  const auto finish_type = [&](std::string type_name) {
    if (type_name == "null" && expected_type.has_value()) {
      type_name = std::string(*expected_type);
    }
    if (expected_type.has_value() && type_name != *expected_type) {
      *refusal_detail = "expression type contradicts its bound consumer type";
      return leave(false);
    }
    if (type_name != "null" &&
        !BindDescriptorType(expression.result_descriptor_id, type_name,
                            refusal_detail)) {
      return leave(false);
    }
    *canonical_type_name = std::move(type_name);
    return leave(true);
  };

  switch (expression.expression_kind) {
    case api::RelationalExpressionKind::kLiteral: {
      if (!expression.literal_kind.has_value() ||
          !expression.literal_or_parameter_ref.has_value()) {
        *refusal_detail = "literal expression payload is incomplete";
        return leave(false);
      }
      switch (*expression.literal_kind) {
        case api::RelationalLiteralKind::kNumeric: {
          std::int64_t decoded = 0;
          if (!ParseInt64(*expression.literal_or_parameter_ref, &decoded)) {
            *refusal_detail = "numeric literal is outside exact int64 admission";
            return leave(false);
          }
          return finish_type("int64");
        }
        case api::RelationalLiteralKind::kString:
          return finish_type("text");
        case api::RelationalLiteralKind::kUuid:
          if (!IsCanonicalUuid(*expression.literal_or_parameter_ref)) {
            *refusal_detail = "UUID literal is not canonical lowercase text";
            return leave(false);
          }
          return finish_type("uuid");
        case api::RelationalLiteralKind::kBoolean: {
          const auto boolean = UpperAscii(*expression.literal_or_parameter_ref);
          if (boolean != "TRUE" && boolean != "FALSE") {
            *refusal_detail = "boolean literal payload is invalid";
            return leave(false);
          }
          return finish_type("boolean");
        }
        case api::RelationalLiteralKind::kNull:
          return finish_type("null");
        case api::RelationalLiteralKind::kTemporal:
          *refusal_detail =
              "temporal literal lacks a bound DATE/TIME/TIMESTAMP subtype";
          return leave(false);
        default:
          *refusal_detail = "literal type is outside object-free scalar admission";
          return leave(false);
      }
    }
    case api::RelationalExpressionKind::kParenthesized: {
      std::string child_type;
      if (!InferTypeInternal(expression.child_expression_ids.front(),
                             expected_type, &child_type, refusal_detail)) {
        return leave(false);
      }
      return finish_type(std::move(child_type));
    }
    case api::RelationalExpressionKind::kUnary: {
      const auto operation = UpperAscii(*expression.operator_name);
      const std::string_view operand_type =
          operation == "NOT" ? std::string_view("boolean")
                             : std::string_view("int64");
      if (operation != "NOT" && operation != "+" && operation != "-") {
        *refusal_detail = "unary scalar operator is not admitted";
        return leave(false);
      }
      std::string child_type;
      if (!InferTypeInternal(expression.child_expression_ids.front(), operand_type,
                             &child_type, refusal_detail)) {
        return leave(false);
      }
      return finish_type(std::string(operand_type));
    }
    case api::RelationalExpressionKind::kBinary: {
      const auto operation = UpperAscii(*expression.operator_name);
      if (operation == "+" || operation == "-" || operation == "*" ||
          operation == "/") {
        std::string left_type;
        std::string right_type;
        if (!InferTypeInternal(expression.child_expression_ids[0], "int64",
                               &left_type, refusal_detail) ||
            !InferTypeInternal(expression.child_expression_ids[1], "int64",
                               &right_type, refusal_detail)) {
          return leave(false);
        }
        return finish_type("int64");
      }
      if (operation == "AND" || operation == "OR") {
        std::string left_type;
        std::string right_type;
        if (!InferTypeInternal(expression.child_expression_ids[0], "boolean",
                               &left_type, refusal_detail) ||
            !InferTypeInternal(expression.child_expression_ids[1], "boolean",
                               &right_type, refusal_detail)) {
          return leave(false);
        }
        return finish_type("boolean");
      }
      if (operation == "||") {
        std::string left_type;
        std::string right_type;
        if (!InferTypeInternal(expression.child_expression_ids[0], "text",
                               &left_type, refusal_detail) ||
            !InferTypeInternal(expression.child_expression_ids[1], "text",
                               &right_type, refusal_detail)) {
          return leave(false);
        }
        return finish_type("text");
      }
      if (operation == "IS") {
        bool negate = false;
        if (!IsNullPredicateRight(expression.child_expression_ids[1], &negate)) {
          *refusal_detail = "IS expression is not a bound NULL predicate";
          return leave(false);
        }
        std::string left_type;
        if (!InferTypeInternal(expression.child_expression_ids[0], std::nullopt,
                               &left_type, refusal_detail) ||
            left_type == "null") {
          if (refusal_detail->empty()) {
            *refusal_detail = "IS NULL operand descriptor type is unresolved";
          }
          return leave(false);
        }
        return finish_type("boolean");
      }
      if (IsComparisonOperator(operation)) {
        std::string left_type;
        std::string right_type;
        if (!InferTypeInternal(expression.child_expression_ids[0], std::nullopt,
                               &left_type, refusal_detail) ||
            !InferTypeInternal(expression.child_expression_ids[1], std::nullopt,
                               &right_type, refusal_detail)) {
          return leave(false);
        }
        if (left_type == "null" && right_type == "null") {
          *refusal_detail = "comparison operand descriptors have no bound type";
          return leave(false);
        }
        const std::string operand_type =
            left_type == "null" ? right_type : left_type;
        if (right_type != "null" && right_type != operand_type) {
          *refusal_detail = "comparison operands have incompatible types";
          return leave(false);
        }
        if (!IsAdmittedComparisonType(operand_type)) {
          *refusal_detail =
              operand_type == "text"
                  ? "character comparison requires catalog-bound collation authority"
                  : "comparison type is outside object-free scalar admission";
          return leave(false);
        }
        if (left_type == "null" &&
            !InferTypeInternal(expression.child_expression_ids[0], operand_type,
                               &left_type, refusal_detail)) {
          return leave(false);
        }
        if (right_type == "null" &&
            !InferTypeInternal(expression.child_expression_ids[1], operand_type,
                               &right_type, refusal_detail)) {
          return leave(false);
        }
        return finish_type("boolean");
      }
      *refusal_detail = "binary scalar operator is not admitted";
      return leave(false);
    }
    case api::RelationalExpressionKind::kParameter:
      *refusal_detail = "parameter expression lacks an engine-bound runtime value";
      return leave(false);
    case api::RelationalExpressionKind::kIdentifier:
      *refusal_detail = "identifier expression requires an input row binding";
      return leave(false);
    case api::RelationalExpressionKind::kFunctionCall:
      *refusal_detail = "function expression requires a bound function runtime";
      return leave(false);
  }
  *refusal_detail = "expression kind is outside scalar runtime admission";
  return leave(false);
}

bool CanonicalRelationalExpressionRuntime::BuildDescriptor(
    const std::uint32_t descriptor_id,
    const std::string_view type_name,
    api::EngineDescriptor* descriptor,
    std::string* refusal_detail) const {
  if (descriptor == nullptr || refusal_detail == nullptr) return false;
  const auto found = descriptors_.find(descriptor_id);
  if (found == descriptors_.end() || type_name.empty() || type_name == "null") {
    *refusal_detail = "scalar result descriptor is unresolved";
    return false;
  }
  const auto& source = *found->second;
  descriptor->descriptor_uuid.canonical = source.descriptor_uuid;
  descriptor->descriptor_kind = "scalar";
  descriptor->canonical_type_name = std::string(type_name);
  const char* nullability = "unknown";
  if (source.nullability == api::RelationalNullability::kNonNull) {
    nullability = "non_null";
  } else if (source.nullability == api::RelationalNullability::kNullable) {
    nullability = "nullable";
  }
  descriptor->encoded_descriptor =
      "type_uuid=" + source.type_uuid + ";nullability=" + nullability;
  if (source.collation_uuid.has_value()) {
    descriptor->encoded_descriptor += ";collation_uuid=" + *source.collation_uuid;
  }
  if (source.timezone_profile_id.has_value()) {
    descriptor->encoded_descriptor +=
        ";timezone_profile_id=" + *source.timezone_profile_id;
  }
  if (source.width.has_value()) {
    descriptor->encoded_descriptor += ";width=" + std::to_string(*source.width);
  }
  if (source.precision.has_value()) {
    descriptor->encoded_descriptor +=
        ";precision=" + std::to_string(*source.precision);
  }
  if (source.scale.has_value()) {
    descriptor->encoded_descriptor += ";scale=" + std::to_string(*source.scale);
  }
  return true;
}

bool CanonicalRelationalExpressionRuntime::FinishValue(
    const std::uint32_t descriptor_id,
    api::EngineTypedValue value,
    api::EngineTypedValue* output,
    std::string* refusal_detail) const {
  const auto descriptor = descriptors_.find(descriptor_id);
  if (output == nullptr || refusal_detail == nullptr ||
      descriptor == descriptors_.end()) {
    return false;
  }
  if (value.isSqlNull() &&
      descriptor->second->nullability == api::RelationalNullability::kNonNull) {
    *refusal_detail = "SQL NULL contradicts a non-null expression descriptor";
    return false;
  }
  if (!value.isSqlNull() &&
      (value.state != api::EngineValueState::value || value.is_null)) {
    *refusal_detail = "scalar expression produced a non-value runtime sentinel";
    return false;
  }
  *output = std::move(value);
  return true;
}

bool CanonicalRelationalExpressionRuntime::IsNullPredicateRight(
    const std::uint32_t expression_id,
    bool* negate) const {
  if (negate == nullptr) return false;
  *negate = false;
  const auto found = expressions_.find(expression_id);
  if (found == expressions_.end()) return false;
  const auto& expression = *found->second;
  if (expression.expression_kind == api::RelationalExpressionKind::kLiteral &&
      expression.literal_kind == api::RelationalLiteralKind::kNull) {
    return true;
  }
  if (expression.expression_kind != api::RelationalExpressionKind::kUnary ||
      UpperAscii(*expression.operator_name) != "NOT") {
    return false;
  }
  const auto child = expressions_.find(expression.child_expression_ids.front());
  if (child == expressions_.end() ||
      child->second->expression_kind !=
          api::RelationalExpressionKind::kLiteral ||
      child->second->literal_kind != api::RelationalLiteralKind::kNull) {
    return false;
  }
  *negate = true;
  return true;
}

bool CanonicalRelationalExpressionRuntime::Evaluate(
    const std::uint32_t expression_id,
    const std::string_view expected_type,
    api::EngineTypedValue* value,
    std::string* refusal_detail) {
  if (value == nullptr || refusal_detail == nullptr || expected_type.empty()) {
    return false;
  }
  *value = {};
  value->state = api::EngineValueState::error;
  refusal_detail->clear();
  std::string inferred_type;
  if (!InferType(expression_id, expected_type, &inferred_type, refusal_detail)) {
    return false;
  }
  return EvaluateInternal(expression_id, expected_type, value, refusal_detail);
}

bool CanonicalRelationalExpressionRuntime::EvaluateInternal(
    const std::uint32_t expression_id,
    const std::string_view expected_type,
    api::EngineTypedValue* value,
    std::string* refusal_detail) {
  const auto& expression = *expressions_.at(expression_id);
  std::string inferred_type;
  if (!InferTypeInternal(expression_id, expected_type, &inferred_type,
                         refusal_detail)) {
    return false;
  }
  api::EngineDescriptor result_descriptor;
  if (!BuildDescriptor(expression.result_descriptor_id, inferred_type,
                       &result_descriptor, refusal_detail)) {
    return false;
  }
  const auto finish = [&](api::EngineTypedValue computed) {
    computed.descriptor = result_descriptor;
    return FinishValue(expression.result_descriptor_id, std::move(computed),
                       value, refusal_detail);
  };

  if (expression.expression_kind == api::RelationalExpressionKind::kLiteral) {
    api::EngineTypedValue literal;
    literal.descriptor = result_descriptor;
    if (*expression.literal_kind == api::RelationalLiteralKind::kNull) {
      literal.setState(api::EngineValueState::sql_null);
      return finish(std::move(literal));
    }
    literal.setState(api::EngineValueState::value);
    switch (*expression.literal_kind) {
      case api::RelationalLiteralKind::kNumeric: {
        std::int64_t decoded = 0;
        if (!ParseInt64(*expression.literal_or_parameter_ref, &decoded)) return false;
        literal.encoded_value = std::to_string(decoded);
        break;
      }
      case api::RelationalLiteralKind::kBoolean:
        literal.encoded_value =
            UpperAscii(*expression.literal_or_parameter_ref) == "TRUE"
                ? "true"
                : "false";
        break;
      default:
        literal.encoded_value = *expression.literal_or_parameter_ref;
        break;
    }
    return finish(std::move(literal));
  }

  if (expression.expression_kind ==
      api::RelationalExpressionKind::kParenthesized) {
    api::EngineTypedValue child;
    if (!EvaluateInternal(expression.child_expression_ids.front(), inferred_type,
                          &child, refusal_detail)) {
      return false;
    }
    return finish(std::move(child));
  }

  if (expression.expression_kind == api::RelationalExpressionKind::kUnary) {
    const auto operation = UpperAscii(*expression.operator_name);
    api::EngineTypedValue child;
    const std::string_view child_type =
        operation == "NOT" ? std::string_view("boolean")
                           : std::string_view("int64");
    if (!EvaluateInternal(expression.child_expression_ids.front(), child_type,
                          &child, refusal_detail)) {
      return false;
    }
    if (operation == "+") return finish(std::move(child));
    if (operation == "-") {
      api::EngineTypedValue zero;
      zero.descriptor = child.descriptor;
      zero.encoded_value = "0";
      zero.setState(api::EngineValueState::value);
      dt::DatatypeNumericContext context;
      context.precision = 19;
      context.scale = 0;
      api::EngineTypedValue computed;
      if (!api::QowApplyCanonicalNumericScalarV1(
              zero, child, result_descriptor,
              dt::DatatypeNumericOperationKind::subtract, context, &computed,
              refusal_detail)) {
        return false;
      }
      return finish(std::move(computed));
    }
    api::EngineSqlTruthValue child_truth;
    if (!TruthFromValue(child, &child_truth, refusal_detail)) return false;
    api::EngineTypedValue computed;
    if (!api::QowMaterializeCanonicalTruthValueV1(
            api::QowSqlNotV1(child_truth), result_descriptor, &computed,
            refusal_detail)) {
      return false;
    }
    return finish(std::move(computed));
  }

  if (expression.expression_kind != api::RelationalExpressionKind::kBinary) {
    *refusal_detail = "expression kind has no object-free evaluator";
    return false;
  }
  const auto operation = UpperAscii(*expression.operator_name);
  if (operation == "+" || operation == "-" || operation == "*" ||
      operation == "/") {
    api::EngineTypedValue left;
    api::EngineTypedValue right;
    if (!EvaluateInternal(expression.child_expression_ids[0], "int64", &left,
                          refusal_detail) ||
        !EvaluateInternal(expression.child_expression_ids[1], "int64", &right,
                          refusal_detail)) {
      return false;
    }
    dt::DatatypeNumericOperationKind numeric_operation =
        dt::DatatypeNumericOperationKind::add;
    if (operation == "-") {
      numeric_operation = dt::DatatypeNumericOperationKind::subtract;
    } else if (operation == "*") {
      numeric_operation = dt::DatatypeNumericOperationKind::multiply;
    } else if (operation == "/") {
      numeric_operation = dt::DatatypeNumericOperationKind::divide;
    }
    dt::DatatypeNumericContext context;
    context.precision = 19;
    context.scale = 0;
    api::EngineTypedValue computed;
    if (!api::QowApplyCanonicalNumericScalarV1(
            left, right, result_descriptor, numeric_operation, context, &computed,
            refusal_detail)) {
      return false;
    }
    return finish(std::move(computed));
  }

  if (operation == "AND" || operation == "OR") {
    api::EngineTypedValue left;
    api::EngineTypedValue right;
    if (!EvaluateInternal(expression.child_expression_ids[0], "boolean", &left,
                          refusal_detail) ||
        !EvaluateInternal(expression.child_expression_ids[1], "boolean", &right,
                          refusal_detail)) {
      return false;
    }
    api::EngineSqlTruthValue left_truth;
    api::EngineSqlTruthValue right_truth;
    if (!TruthFromValue(left, &left_truth, refusal_detail) ||
        !TruthFromValue(right, &right_truth, refusal_detail)) {
      return false;
    }
    const auto result_truth = operation == "AND"
                                  ? api::QowSqlAndV1(left_truth, right_truth)
                                  : api::QowSqlOrV1(left_truth, right_truth);
    api::EngineTypedValue computed;
    if (!api::QowMaterializeCanonicalTruthValueV1(
            result_truth, result_descriptor, &computed, refusal_detail)) {
      return false;
    }
    return finish(std::move(computed));
  }

  if (operation == "||") {
    api::EngineTypedValue left;
    api::EngineTypedValue right;
    if (!EvaluateInternal(expression.child_expression_ids[0], "text", &left,
                          refusal_detail) ||
        !EvaluateInternal(expression.child_expression_ids[1], "text", &right,
                          refusal_detail)) {
      return false;
    }
    api::EngineTypedValue computed;
    if (left.isSqlNull() || right.isSqlNull()) {
      computed.setState(api::EngineValueState::sql_null);
    } else {
      computed.encoded_value = left.encoded_value + right.encoded_value;
      computed.setState(api::EngineValueState::value);
    }
    return finish(std::move(computed));
  }

  if (operation == "IS") {
    bool negate = false;
    if (!IsNullPredicateRight(expression.child_expression_ids[1], &negate)) {
      return false;
    }
    std::string left_type;
    if (!InferTypeInternal(expression.child_expression_ids[0], std::nullopt,
                           &left_type, refusal_detail) ||
        left_type == "null") {
      return false;
    }
    api::EngineTypedValue left;
    if (!EvaluateInternal(expression.child_expression_ids[0], left_type, &left,
                          refusal_detail)) {
      return false;
    }
    api::EngineSqlTruthValue truth;
    if (!api::QowEvaluateCanonicalNullPredicateV1(
            left, negate, &truth, refusal_detail)) {
      return false;
    }
    api::EngineTypedValue computed;
    if (!api::QowMaterializeCanonicalTruthValueV1(
            truth, result_descriptor, &computed, refusal_detail)) {
      return false;
    }
    return finish(std::move(computed));
  }

  if (IsComparisonOperator(operation)) {
    std::string left_type;
    std::string right_type;
    if (!InferTypeInternal(expression.child_expression_ids[0], std::nullopt,
                           &left_type, refusal_detail) ||
        !InferTypeInternal(expression.child_expression_ids[1], std::nullopt,
                           &right_type, refusal_detail)) {
      return false;
    }
    const std::string operand_type =
        left_type == "null" ? right_type : left_type;
    api::EngineTypedValue left;
    api::EngineTypedValue right;
    if (!EvaluateInternal(expression.child_expression_ids[0], operand_type, &left,
                          refusal_detail) ||
        !EvaluateInternal(expression.child_expression_ids[1], operand_type, &right,
                          refusal_detail)) {
      return false;
    }
    int comparison = 0;
    if (!left.isSqlNull() && !right.isSqlNull() &&
        !api::QowCompareCanonicalNonCollatedScalarsV1(
            left, right, &comparison, refusal_detail)) {
      return false;
    }
    api::EngineSqlTruthValue truth;
    if (!api::QowEvaluateCanonicalComparisonTruthV1(
            left, right, comparison, ComparisonOperator(operation), &truth,
            refusal_detail)) {
      return false;
    }
    api::EngineTypedValue computed;
    if (!api::QowMaterializeCanonicalTruthValueV1(
            truth, result_descriptor, &computed, refusal_detail)) {
      return false;
    }
    return finish(std::move(computed));
  }

  *refusal_detail = "binary scalar operator has no object-free evaluator";
  return false;
}

}  // namespace scratchbird::engine::sblr
