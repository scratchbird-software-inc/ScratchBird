// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/expression_api.hpp"
#include "engine/sblr/canonical_relational_expression.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

namespace {

std::size_t checks = 0;

bool Require(const bool condition, const std::string& detail) {
  ++checks;
  if (!condition) std::cerr << detail << '\n';
  return condition;
}

api::EngineDescriptor Descriptor(const std::string& uuid,
                                 const std::string& type,
                                 const bool nullable,
                                 const std::string& suffix = {}) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = type;
  descriptor.encoded_descriptor =
      "type_uuid=019e0000-0000-7000-8000-000000000001;nullability=" +
      std::string(nullable ? "nullable" : "non_null") + suffix;
  return descriptor;
}

api::EngineTypedValue Value(const api::EngineDescriptor& descriptor,
                            std::string encoded) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = std::move(encoded);
  value.setState(api::EngineValueState::value);
  return value;
}

api::EngineTypedValue Null(const api::EngineDescriptor& descriptor) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.setState(api::EngineValueState::sql_null);
  return value;
}

api::EngineTypedValue TruthValue(const api::EngineDescriptor& descriptor,
                                 const api::EngineSqlTruthValue truth) {
  if (truth == api::EngineSqlTruthValue::unknown) return Null(descriptor);
  return Value(descriptor,
               truth == api::EngineSqlTruthValue::true_value ? "true"
                                                              : "false");
}

api::EngineSqlTruthValue ExpectedAnd(const api::EngineSqlTruthValue left,
                                     const api::EngineSqlTruthValue right) {
  if (left == api::EngineSqlTruthValue::false_value ||
      right == api::EngineSqlTruthValue::false_value) {
    return api::EngineSqlTruthValue::false_value;
  }
  if (left == api::EngineSqlTruthValue::true_value) return right;
  if (right == api::EngineSqlTruthValue::true_value) return left;
  return api::EngineSqlTruthValue::unknown;
}

api::EngineSqlTruthValue ExpectedOr(const api::EngineSqlTruthValue left,
                                    const api::EngineSqlTruthValue right) {
  if (left == api::EngineSqlTruthValue::true_value ||
      right == api::EngineSqlTruthValue::true_value) {
    return api::EngineSqlTruthValue::true_value;
  }
  if (left == api::EngineSqlTruthValue::false_value) return right;
  if (right == api::EngineSqlTruthValue::false_value) return left;
  return api::EngineSqlTruthValue::unknown;
}

}  // namespace

// RCP-024-TEST-SCALAR-SEMANTICS-CLOSURE-V1
int main() {
  const auto int64_descriptor = Descriptor(
      "019e0000-0000-7000-8000-000000000101", "int64", true,
      ";width=64");
  const auto int8_descriptor = Descriptor(
      "019e0000-0000-7000-8000-000000000105", "int8", true,
      ";width=8");
  const auto real32_descriptor = Descriptor(
      "019e0000-0000-7000-8000-000000000106", "real32", true,
      ";width=32");
  const auto real64_descriptor = Descriptor(
      "019e0000-0000-7000-8000-000000000107", "real64", true,
      ";width=64");
  const auto boolean_descriptor = Descriptor(
      "019e0000-0000-7000-8000-000000000102", "boolean", true);
  const auto total_boolean_descriptor = Descriptor(
      "019e0000-0000-7000-8000-000000000103", "boolean", false);
  const auto text_descriptor = Descriptor(
      "019e0000-0000-7000-8000-000000000104", "text", true,
      ";collation_uuid=019e0000-0000-7000-8000-000000000301");
  const auto binary_descriptor = Descriptor(
      "019e0000-0000-7000-8000-000000000109", "binary", true);

  bool passed = true;
  std::string detail;
  api::EngineCanonicalExpressionEvaluationResult result;
  api::EngineCanonicalExpressionEvaluationRequest request;
  request.consumer = api::EngineCanonicalExpressionConsumer::projection;

  for (const auto& [operation, expected] :
       std::array<std::pair<api::EngineCanonicalExpressionOperation,
                            const char*>, 5>{
           {{api::EngineCanonicalExpressionOperation::numeric_add, "12"},
            {api::EngineCanonicalExpressionOperation::numeric_subtract, "2"},
            {api::EngineCanonicalExpressionOperation::numeric_multiply, "35"},
            {api::EngineCanonicalExpressionOperation::numeric_divide, "1"},
            {api::EngineCanonicalExpressionOperation::numeric_modulo, "2"}}}) {
    request.operation = operation;
    request.left_value = Value(int64_descriptor, "7");
    request.right_value = Value(int64_descriptor, "5");
    request.result_descriptor = int64_descriptor;
    request.numeric_context.precision = 19;
    request.numeric_context.scale = 0;
    detail.clear();
    passed &= Require(
        api::QowEvaluateCanonicalTypedExpressionV1(
            request, &result, &detail) &&
            result.value.encoded_value == expected,
        "canonical numeric operation diverged: " + detail);
  }

  request.operation = api::EngineCanonicalExpressionOperation::numeric_add;
  request.left_value = Value(int8_descriptor, "120");
  request.right_value = Value(int8_descriptor, "7");
  request.result_descriptor = int8_descriptor;
  request.numeric_context.precision = 3;
  request.numeric_context.scale = 0;
  detail.clear();
  passed &= Require(
      api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          result.value.encoded_value == "127",
      "bounded int8 arithmetic failed: " + detail);
  request.left_value = Value(int8_descriptor, "127");
  request.right_value = Value(int8_descriptor, "1");
  detail.clear();
  passed &= Require(
      !api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          result.diagnostic_id == "QOW-DIAG-RCP024-NUMERIC-REFUSAL-V1" &&
          detail.find("overflow") != std::string::npos,
      "bounded int8 overflow was admitted");

  request.operation = api::EngineCanonicalExpressionOperation::numeric_add;
  request.left_value = Value(real32_descriptor, "1.5");
  request.right_value = Value(real32_descriptor, "2.25");
  request.result_descriptor = real32_descriptor;
  request.numeric_context.precision = 9;
  request.numeric_context.scale = 0;
  detail.clear();
  passed &= Require(
      api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          result.value.encoded_value == "3.75",
      "canonical real32 arithmetic failed: " + detail);
  request.operation =
      api::EngineCanonicalExpressionOperation::numeric_multiply;
  request.left_value = Value(real64_descriptor, "1.5");
  request.right_value = Value(real64_descriptor, "2");
  request.result_descriptor = real64_descriptor;
  request.numeric_context.precision = 17;
  detail.clear();
  passed &= Require(
      api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          result.value.encoded_value == "3",
      "canonical real64 arithmetic failed: " + detail);
  request.operation = api::EngineCanonicalExpressionOperation::numeric_divide;
  request.right_value = Value(real64_descriptor, "0");
  detail.clear();
  passed &= Require(
      !api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          result.diagnostic_id == "QOW-DIAG-RCP024-NUMERIC-REFUSAL-V1" &&
          detail.find("zero") != std::string::npos,
      "canonical real64 division by zero was admitted");

  request.operation = api::EngineCanonicalExpressionOperation::numeric_add;
  request.left_value = Null(int64_descriptor);
  request.right_value = Value(int64_descriptor, "1");
  request.result_descriptor = int64_descriptor;
  request.numeric_context.precision = 19;
  detail.clear();
  passed &= Require(
      api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          result.value.isSqlNull() && result.value.encoded_value.empty(),
      "canonical numeric SQL NULL propagation failed: " + detail);

  for (const auto& [operation, expected] :
       std::array<std::pair<api::EngineCanonicalExpressionOperation, bool>,
                  6>{
           {{api::EngineCanonicalExpressionOperation::equal, false},
            {api::EngineCanonicalExpressionOperation::not_equal, true},
            {api::EngineCanonicalExpressionOperation::less_than, false},
            {api::EngineCanonicalExpressionOperation::less_than_or_equal,
             false},
            {api::EngineCanonicalExpressionOperation::greater_than, true},
            {api::EngineCanonicalExpressionOperation::greater_than_or_equal,
             true}}}) {
    request.operation = operation;
    request.left_value = Value(int64_descriptor, "7");
    request.right_value = Value(int64_descriptor, "5");
    request.result_descriptor = boolean_descriptor;
    request.precomputed_comparison.reset();
    detail.clear();
    passed &= Require(
        api::QowEvaluateCanonicalTypedExpressionV1(
            request, &result, &detail) &&
            result.truth ==
                (expected ? api::EngineSqlTruthValue::true_value
                          : api::EngineSqlTruthValue::false_value),
        "canonical comparison operation diverged: " + detail);
  }

  auto character_descriptor = text_descriptor;
  character_descriptor.descriptor_uuid.canonical =
      "019e0000-0000-7000-8000-000000000108";
  character_descriptor.canonical_type_name = "character";
  request.operation = api::EngineCanonicalExpressionOperation::equal;
  request.left_value = Value(text_descriptor, "ScratchBird");
  request.right_value = Value(character_descriptor, "ScratchBird");
  request.result_descriptor = boolean_descriptor;
  request.precomputed_comparison = 0;
  detail.clear();
  passed &= Require(
      api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          result.truth == api::EngineSqlTruthValue::true_value,
      "canonical text/character alias comparison diverged: " + detail);
  request.precomputed_comparison.reset();

  request.operation = api::EngineCanonicalExpressionOperation::text_concat;
  request.left_value = Value(text_descriptor, "Scratch");
  request.right_value = Value(text_descriptor, "Bird");
  request.result_descriptor = text_descriptor;
  detail.clear();
  passed &= Require(
      api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          result.value.encoded_value == "ScratchBird",
      "canonical text concatenation diverged: " + detail);

  for (const auto& [operation, expected] :
       std::array<std::pair<api::EngineCanonicalExpressionOperation, bool>,
                  2>{
           {{api::EngineCanonicalExpressionOperation::is_null, true},
            {api::EngineCanonicalExpressionOperation::is_not_null, false}}}) {
    request.operation = operation;
    request.left_value = Null(int64_descriptor);
    request.result_descriptor = total_boolean_descriptor;
    detail.clear();
    passed &= Require(
        api::QowEvaluateCanonicalTypedExpressionV1(
            request, &result, &detail) &&
            result.truth ==
                (expected ? api::EngineSqlTruthValue::true_value
                          : api::EngineSqlTruthValue::false_value),
        "canonical NULL predicate diverged: " + detail);
  }

  request.operation = api::EngineCanonicalExpressionOperation::logical_not;
  request.left_value = Value(boolean_descriptor, "true");
  request.result_descriptor = boolean_descriptor;
  detail.clear();
  passed &= Require(
      api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          result.truth == api::EngineSqlTruthValue::false_value,
      "canonical NOT diverged: " + detail);

  request.operation = api::EngineCanonicalExpressionOperation::identity;
  request.left_value = Value(int64_descriptor, "7");
  request.result_descriptor = int64_descriptor;
  detail.clear();
  passed &= Require(
      api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          result.value.encoded_value == "7",
      "canonical identity diverged: " + detail);

  request.operation =
      api::EngineCanonicalExpressionOperation::consume_truth;
  request.input_truth = api::EngineSqlTruthValue::true_value;
  request.result_descriptor = total_boolean_descriptor;
  detail.clear();
  passed &= Require(
      api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          result.passes_consumer &&
          result.truth == api::EngineSqlTruthValue::true_value,
      "canonical consumer truth materialization diverged: " + detail);

  request.operation =
      api::EngineCanonicalExpressionOperation::explicit_cast;
  request.left_value = Value(int64_descriptor, "42");
  request.result_descriptor = text_descriptor;
  passed &= Require(
      api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          result.value.encoded_value == "42" &&
          result.value.descriptor.descriptor_uuid.canonical ==
              text_descriptor.descriptor_uuid.canonical,
      "explicit canonical cast failed: " + detail);

  request.operation =
      api::EngineCanonicalExpressionOperation::implicit_cast;
  detail.clear();
  passed &= Require(
      !api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          result.diagnostic_id == "QOW-DIAG-RCP024-CAST-REFUSAL-V1",
      "lossy implicit cast was admitted or lacked a stable diagnostic");

  request.operation =
      api::EngineCanonicalExpressionOperation::numeric_modulo;
  request.left_value = Value(int64_descriptor, "17");
  request.right_value = Value(int64_descriptor, "5");
  request.result_descriptor = int64_descriptor;
  detail.clear();
  passed &= Require(
      api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          result.value.encoded_value == "2",
      "canonical modulo failed: " + detail);
  request.left_value = Value(int8_descriptor, "17");
  request.right_value = Value(int8_descriptor, "5");
  request.result_descriptor = int8_descriptor;
  detail.clear();
  passed &= Require(
      api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          result.value.encoded_value == "2",
      "bounded int8 modulo failed: " + detail);
  request.left_value = Value(int64_descriptor, "17");
  request.result_descriptor = int64_descriptor;
  request.right_value = Value(int64_descriptor, "0");
  detail.clear();
  std::string first_modulo_detail;
  std::string first_modulo_diagnostic;
  passed &= Require(
      !api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          result.diagnostic_id == "QOW-DIAG-RCP024-NUMERIC-REFUSAL-V1" &&
          detail.find("zero") != std::string::npos,
      "modulo-by-zero did not refuse deterministically");
  first_modulo_detail = detail;
  first_modulo_diagnostic = result.diagnostic_id;
  detail.clear();
  passed &= Require(
      !api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          result.diagnostic_id == first_modulo_diagnostic &&
          detail == first_modulo_detail,
      "replayed modulo refusal changed diagnostic identity or detail");

  const std::array truths{
      api::EngineSqlTruthValue::false_value,
      api::EngineSqlTruthValue::true_value,
      api::EngineSqlTruthValue::unknown,
  };
  for (const auto left_truth : truths) {
    for (const auto right_truth : truths) {
      for (const auto operation : {
               api::EngineCanonicalExpressionOperation::logical_and,
               api::EngineCanonicalExpressionOperation::logical_or,
               api::EngineCanonicalExpressionOperation::logical_xor}) {
        request.operation = operation;
        request.left_value = TruthValue(boolean_descriptor, left_truth);
        request.right_value = TruthValue(boolean_descriptor, right_truth);
        request.result_descriptor = boolean_descriptor;
        detail.clear();
        const auto expected =
            operation == api::EngineCanonicalExpressionOperation::logical_and
                ? ExpectedAnd(left_truth, right_truth)
                : operation ==
                          api::EngineCanonicalExpressionOperation::logical_or
                      ? ExpectedOr(left_truth, right_truth)
                      : (left_truth == api::EngineSqlTruthValue::unknown ||
                                 right_truth ==
                                     api::EngineSqlTruthValue::unknown
                             ? api::EngineSqlTruthValue::unknown
                             : (left_truth != right_truth
                                    ? api::EngineSqlTruthValue::true_value
                                    : api::EngineSqlTruthValue::false_value));
        passed &= Require(
            api::QowEvaluateCanonicalTypedExpressionV1(
                request, &result, &detail) &&
                result.truth == expected,
            "canonical three-valued truth table diverged: " + detail);
      }
    }
  }

  request.operation = api::EngineCanonicalExpressionOperation::like;
  request.left_value = Value(text_descriptor, "ScratchBird");
  request.right_value = Value(text_descriptor, "Scratch%");
  request.result_descriptor = boolean_descriptor;
  request.bound_text_authority = true;
  detail.clear();
  passed &= Require(
      api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          result.truth == api::EngineSqlTruthValue::true_value,
      "canonical LIKE failed: " + detail);
  request.operation = api::EngineCanonicalExpressionOperation::ilike;
  request.right_value = Value(text_descriptor, "scratch_ird");
  detail.clear();
  passed &= Require(
      api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          result.truth == api::EngineSqlTruthValue::true_value,
      "canonical ILIKE failed: " + detail);
  request.bound_text_authority = false;
  detail.clear();
  passed &= Require(
      !api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          result.diagnostic_id == "QOW-DIAG-RCP024-TEXT-REFUSAL-V1",
      "LIKE without collation authority was admitted");
  request.bound_text_authority = true;
  request.right_value = Value(text_descriptor, "broken\\");
  detail.clear();
  passed &= Require(
      !api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          detail.find("escape") != std::string::npos,
      "invalid LIKE escape was admitted");

  request.operation =
      api::EngineCanonicalExpressionOperation::is_distinct_from;
  request.left_value = Null(int64_descriptor);
  request.right_value = Value(int64_descriptor, "1");
  request.result_descriptor = total_boolean_descriptor;
  detail.clear();
  passed &= Require(
      api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          result.truth == api::EngineSqlTruthValue::true_value &&
          result.value.state == api::EngineValueState::value,
      "IS DISTINCT FROM did not totalize SQL NULL");
  request.operation =
      api::EngineCanonicalExpressionOperation::is_not_distinct_from;
  request.right_value = Null(int64_descriptor);
  detail.clear();
  passed &= Require(
      api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          result.truth == api::EngineSqlTruthValue::true_value,
      "IS NOT DISTINCT FROM did not equate SQL NULLs");

  api::EngineTypedValue function_value;
  function_value.descriptor = text_descriptor;
  function_value.encoded_value = "SCRATCHBIRD";
  function_value.setState(api::EngineValueState::value);
  request.operation =
      api::EngineCanonicalExpressionOperation::scalar_function;
  request.result_descriptor = text_descriptor;
  request.precomputed_value = function_value;
  detail.clear();
  passed &= Require(
      api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          result.value.encoded_value == "SCRATCHBIRD" &&
          result.value.descriptor.descriptor_uuid.canonical ==
              text_descriptor.descriptor_uuid.canonical,
      "canonical function-result validation failed: " + detail);
  function_value.descriptor.descriptor_uuid.canonical.clear();
  request.precomputed_value = function_value;
  detail.clear();
  passed &= Require(
      !api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          result.diagnostic_id ==
              "QOW-DIAG-RCP024-FUNCTION-REFUSAL-V1",
      "function result without a canonical descriptor was admitted");
  function_value.descriptor = binary_descriptor;
  function_value.encoded_value = "00ff";
  function_value.binary_value = {0x00, 0xff};
  request.result_descriptor = binary_descriptor;
  request.precomputed_value = function_value;
  detail.clear();
  passed &= Require(
      api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
          result.value.encoded_value == "00ff" &&
          result.value.binary_value == std::vector<std::uint8_t>({0x00, 0xff}),
      "canonical binary function result lost its binary payload: " + detail);

  api::TypedRelationalDag dag;
  dag.wire_version = 2;
  dag.descriptors = {
      {1, "019e0000-0000-7000-8000-000000000401",
       "019e0000-0000-7000-8000-000000000501",
       api::RelationalNullability::kNonNull,
       "019e0000-0000-7000-8000-000000000301"},
      {2, "019e0000-0000-7000-8000-000000000402",
       "019e0000-0000-7000-8000-000000000502",
       api::RelationalNullability::kNonNull,
       "019e0000-0000-7000-8000-000000000301"},
  };
  api::RelationalExpressionRecord literal;
  literal.expression_id = 1;
  literal.expression_kind = api::RelationalExpressionKind::kLiteral;
  literal.result_descriptor_id = 1;
  literal.literal_kind = api::RelationalLiteralKind::kString;
  literal.literal_or_parameter_ref = "scratchbird";
  api::RelationalExpressionRecord function;
  function.expression_id = 2;
  function.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  function.child_expression_ids = {1};
  function.result_descriptor_id = 2;
  function.function_uuid = "019e0000-0000-7000-8000-000000000601";
  dag.expressions = {literal, function};

  bool function_called = false;
  sblr::CanonicalRelationalExpressionRuntimeServices services;
  services.descriptor_type_resolver =
      [](const std::string_view,
         std::string* type_name,
         std::string*,
         std::string*) {
        *type_name = "character";
        return true;
      };
  services.function_evaluator =
      [&function_called, text_descriptor](
          const std::string_view function_uuid,
          const std::vector<api::EngineTypedValue>& arguments,
          api::EngineTypedValue* output,
          std::string*,
          std::string*) {
        function_called =
            function_uuid ==
                "019e0000-0000-7000-8000-000000000601" &&
            arguments.size() == 1 &&
            arguments.front().encoded_value == "scratchbird";
        output->descriptor = text_descriptor;
        output->encoded_value = "SCRATCHBIRD";
        output->setState(api::EngineValueState::value);
        return function_called;
      };
  sblr::CanonicalRelationalExpressionRuntime relational_runtime(
      dag, services);
  api::EngineTypedValue relational_value;
  detail.clear();
  passed &= Require(
      relational_runtime.EvaluateForConsumer(
          2, "text", api::EngineCanonicalExpressionConsumer::projection,
          &relational_value, &detail) &&
          function_called && relational_value.encoded_value == "SCRATCHBIRD" &&
          relational_value.descriptor.descriptor_uuid.canonical ==
              "019e0000-0000-7000-8000-000000000402",
      "bound function did not execute through the relational runtime: " +
          detail);

  api::TypedRelationalDag arithmetic_dag;
  arithmetic_dag.wire_version = 2;
  arithmetic_dag.descriptors = {
      {31, "019e0000-0000-7000-8000-000000000431",
       "019e0000-0000-7000-8000-000000000531",
       api::RelationalNullability::kNonNull, std::nullopt, std::nullopt, 8},
      {32, "019e0000-0000-7000-8000-000000000432",
       "019e0000-0000-7000-8000-000000000532",
       api::RelationalNullability::kNonNull, std::nullopt, std::nullopt, 8},
      {33, "019e0000-0000-7000-8000-000000000433",
       "019e0000-0000-7000-8000-000000000533",
       api::RelationalNullability::kNonNull, std::nullopt, std::nullopt, 8},
  };
  api::RelationalExpressionRecord arithmetic_left;
  arithmetic_left.expression_id = 31;
  arithmetic_left.expression_kind = api::RelationalExpressionKind::kLiteral;
  arithmetic_left.result_descriptor_id = 31;
  arithmetic_left.literal_kind = api::RelationalLiteralKind::kNumeric;
  arithmetic_left.literal_or_parameter_ref = "120";
  api::RelationalExpressionRecord arithmetic_right = arithmetic_left;
  arithmetic_right.expression_id = 32;
  arithmetic_right.result_descriptor_id = 32;
  arithmetic_right.literal_or_parameter_ref = "7";
  api::RelationalExpressionRecord arithmetic_add;
  arithmetic_add.expression_id = 33;
  arithmetic_add.expression_kind = api::RelationalExpressionKind::kBinary;
  arithmetic_add.child_expression_ids = {31, 32};
  arithmetic_add.result_descriptor_id = 33;
  arithmetic_add.operator_name = "+";
  arithmetic_dag.expressions = {arithmetic_left, arithmetic_right,
                                arithmetic_add};
  sblr::CanonicalRelationalExpressionRuntime arithmetic_runtime(
      arithmetic_dag);
  api::EngineTypedValue arithmetic_value;
  detail.clear();
  passed &= Require(
      arithmetic_runtime.EvaluateForConsumer(
          33, "int8", api::EngineCanonicalExpressionConsumer::projection,
          &arithmetic_value, &detail) &&
          arithmetic_value.encoded_value == "127" &&
          arithmetic_value.descriptor.canonical_type_name == "int8",
      "relational bounded int8 arithmetic did not use its descriptor: " +
          detail);

  api::TypedRelationalDag logic_dag;
  logic_dag.wire_version = 2;
  logic_dag.descriptors = {
      {41, "019e0000-0000-7000-8000-000000000441",
       "019e0000-0000-7000-8000-000000000541",
       api::RelationalNullability::kNonNull},
      {42, "019e0000-0000-7000-8000-000000000442",
       "019e0000-0000-7000-8000-000000000542",
       api::RelationalNullability::kNonNull},
      {43, "019e0000-0000-7000-8000-000000000443",
       "019e0000-0000-7000-8000-000000000543",
       api::RelationalNullability::kNonNull},
  };
  api::RelationalExpressionRecord truth_left;
  truth_left.expression_id = 41;
  truth_left.expression_kind = api::RelationalExpressionKind::kLiteral;
  truth_left.result_descriptor_id = 41;
  truth_left.literal_kind = api::RelationalLiteralKind::kBoolean;
  truth_left.literal_or_parameter_ref = "TRUE";
  api::RelationalExpressionRecord truth_right = truth_left;
  truth_right.expression_id = 42;
  truth_right.result_descriptor_id = 42;
  truth_right.literal_or_parameter_ref = "FALSE";
  api::RelationalExpressionRecord xor_expression;
  xor_expression.expression_id = 43;
  xor_expression.expression_kind = api::RelationalExpressionKind::kBinary;
  xor_expression.child_expression_ids = {41, 42};
  xor_expression.result_descriptor_id = 43;
  xor_expression.operator_name = "XOR";
  logic_dag.expressions = {truth_left, truth_right, xor_expression};
  sblr::CanonicalRelationalExpressionRuntime logic_runtime(logic_dag);
  api::EngineTypedValue logic_value;
  detail.clear();
  passed &= Require(
      logic_runtime.EvaluateForConsumer(
          43, "boolean", api::EngineCanonicalExpressionConsumer::projection,
          &logic_value, &detail) &&
          logic_value.encoded_value == "true",
      "relational XOR bypassed canonical three-valued logic: " + detail);

  api::TypedRelationalDag like_dag;
  like_dag.wire_version = 2;
  like_dag.descriptors = {
      {11, "019e0000-0000-7000-8000-000000000411",
       "019e0000-0000-7000-8000-000000000511",
       api::RelationalNullability::kNonNull,
       "019e0000-0000-7000-8000-000000000301"},
      {12, "019e0000-0000-7000-8000-000000000412",
       "019e0000-0000-7000-8000-000000000512",
       api::RelationalNullability::kNonNull,
       "019e0000-0000-7000-8000-000000000301"},
      {13, "019e0000-0000-7000-8000-000000000413",
       "019e0000-0000-7000-8000-000000000513",
       api::RelationalNullability::kNonNull},
  };
  api::RelationalExpressionRecord like_left;
  like_left.expression_id = 11;
  like_left.expression_kind = api::RelationalExpressionKind::kLiteral;
  like_left.result_descriptor_id = 11;
  like_left.literal_kind = api::RelationalLiteralKind::kString;
  like_left.literal_or_parameter_ref = "ScratchBird";
  api::RelationalExpressionRecord like_pattern = like_left;
  like_pattern.expression_id = 12;
  like_pattern.result_descriptor_id = 12;
  like_pattern.literal_or_parameter_ref = "Scratch%";
  api::RelationalExpressionRecord like_expression;
  like_expression.expression_id = 13;
  like_expression.expression_kind = api::RelationalExpressionKind::kBinary;
  like_expression.child_expression_ids = {11, 12};
  like_expression.result_descriptor_id = 13;
  like_expression.operator_name = "LIKE";
  like_dag.expressions = {like_left, like_pattern, like_expression};
  std::size_t comparison_calls = 0;
  sblr::CanonicalRelationalExpressionRuntimeServices compare_services;
  compare_services.comparison_evaluator =
      [&comparison_calls](const api::EngineTypedValue& left_value,
                          const api::EngineTypedValue&,
                          int* comparison,
                          std::string*,
                          std::string*) {
        ++comparison_calls;
        *comparison = left_value.descriptor.canonical_type_name == "timestamp"
                          ? -1
                          : 0;
        return true;
      };
  sblr::CanonicalRelationalExpressionRuntime like_runtime(
      like_dag, compare_services);
  api::EngineSqlTruthValue like_truth =
      api::EngineSqlTruthValue::unknown;
  detail.clear();
  passed &= Require(
      like_runtime.EvaluatePredicateForConsumer(
          13, api::EngineCanonicalExpressionConsumer::filter, &like_truth,
          &detail) &&
          like_truth == api::EngineSqlTruthValue::true_value &&
          comparison_calls == 1,
      "relational LIKE bypassed bound collation authority: " + detail);

  api::TypedRelationalDag temporal_dag;
  temporal_dag.wire_version = 2;
  temporal_dag.descriptors = {
      {21, "019e0000-0000-7000-8000-000000000421",
       "019e0000-0000-7000-8000-000000000521",
       api::RelationalNullability::kNonNull, std::nullopt,
       "timestamp_timezone_profile"},
      {22, "019e0000-0000-7000-8000-000000000422",
       "019e0000-0000-7000-8000-000000000522",
       api::RelationalNullability::kNonNull, std::nullopt,
       "timestamp_timezone_profile"},
      {23, "019e0000-0000-7000-8000-000000000423",
       "019e0000-0000-7000-8000-000000000523",
       api::RelationalNullability::kNonNull},
  };
  api::RelationalExpressionRecord temporal_left;
  temporal_left.expression_id = 21;
  temporal_left.expression_kind = api::RelationalExpressionKind::kLiteral;
  temporal_left.result_descriptor_id = 21;
  temporal_left.literal_kind = api::RelationalLiteralKind::kTemporal;
  temporal_left.literal_or_parameter_ref = "2026-08-03T10:00:00-04:00";
  api::RelationalExpressionRecord temporal_right = temporal_left;
  temporal_right.expression_id = 22;
  temporal_right.result_descriptor_id = 22;
  temporal_right.literal_or_parameter_ref = "2026-08-03T15:00:00Z";
  api::RelationalExpressionRecord temporal_compare;
  temporal_compare.expression_id = 23;
  temporal_compare.expression_kind =
      api::RelationalExpressionKind::kBinary;
  temporal_compare.child_expression_ids = {21, 22};
  temporal_compare.result_descriptor_id = 23;
  temporal_compare.operator_name = "<";
  temporal_dag.expressions = {temporal_left, temporal_right,
                              temporal_compare};
  sblr::CanonicalRelationalExpressionRuntime temporal_runtime(
      temporal_dag, compare_services);
  api::EngineSqlTruthValue temporal_truth =
      api::EngineSqlTruthValue::unknown;
  detail.clear();
  passed &= Require(
      temporal_runtime.EvaluatePredicateForConsumer(
          23, api::EngineCanonicalExpressionConsumer::join, &temporal_truth,
          &detail) &&
          temporal_truth == api::EngineSqlTruthValue::true_value &&
          comparison_calls == 2,
      "relational temporal comparison bypassed timezone authority: " +
          detail);

  if (!passed) return 1;
  std::cout << "qow_qry_008_scalar_semantics_closure=passed checks="
            << checks << '\n';
  return 0;
}
