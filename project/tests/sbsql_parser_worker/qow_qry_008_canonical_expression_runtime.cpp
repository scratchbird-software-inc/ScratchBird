// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/expression_api.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

namespace api = scratchbird::engine::internal_api;

namespace {

bool Require(const bool condition, const std::string& detail) {
  if (!condition) std::cerr << detail << '\n';
  return condition;
}

api::EngineDescriptor Descriptor(const std::string& uuid,
                                 const std::string& type,
                                 const bool nullable) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = type;
  descriptor.encoded_descriptor =
      "type_uuid=019dffbb-f000-7000-8000-000000000101;nullability=" +
      std::string(nullable ? "nullable" : "non_null");
  return descriptor;
}

api::EngineTypedValue Value(const api::EngineDescriptor& descriptor,
                            std::string encoded) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = std::move(encoded);
  value.state = api::EngineValueState::value;
  return value;
}

api::EngineTypedValue Null(const api::EngineDescriptor& descriptor) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.setState(api::EngineValueState::sql_null);
  return value;
}

}  // namespace

// RCP-023-TEST-CANONICAL-TYPED-EXPRESSION-RUNTIME-V1
int main() {
  const auto int64_descriptor = Descriptor(
      "019dffbb-f000-7000-8000-000000000201", "int64", false);
  const auto nullable_boolean_descriptor = Descriptor(
      "019dffbb-f000-7000-8000-000000000202", "boolean", true);
  const auto left = Value(int64_descriptor, "7");
  const auto right = Value(int64_descriptor, "5");
  const std::array consumers{
      api::EngineCanonicalExpressionConsumer::filter,
      api::EngineCanonicalExpressionConsumer::projection,
      api::EngineCanonicalExpressionConsumer::join,
      api::EngineCanonicalExpressionConsumer::aggregate,
      api::EngineCanonicalExpressionConsumer::window,
      api::EngineCanonicalExpressionConsumer::subquery,
  };

  bool passed = true;
  for (const auto consumer : consumers) {
    api::EngineCanonicalExpressionEvaluationRequest arithmetic;
    arithmetic.consumer = consumer;
    arithmetic.operation =
        api::EngineCanonicalExpressionOperation::numeric_add;
    arithmetic.left_value = left;
    arithmetic.right_value = right;
    arithmetic.result_descriptor = int64_descriptor;
    arithmetic.numeric_context.precision = 19;
    arithmetic.numeric_context.scale = 0;
    api::EngineCanonicalExpressionEvaluationResult arithmetic_result;
    std::string detail;
    passed &= Require(
        api::QowEvaluateCanonicalTypedExpressionV1(
            arithmetic, &arithmetic_result, &detail) &&
            arithmetic_result.value.descriptor.descriptor_uuid.canonical ==
                int64_descriptor.descriptor_uuid.canonical &&
            arithmetic_result.value.encoded_value == "12" &&
            arithmetic_result.value.state == api::EngineValueState::value,
        "consumer arithmetic diverged: " + detail);

    api::EngineCanonicalExpressionEvaluationRequest comparison;
    comparison.consumer = consumer;
    comparison.operation =
        api::EngineCanonicalExpressionOperation::greater_than;
    comparison.left_value = left;
    comparison.right_value = right;
    comparison.result_descriptor = nullable_boolean_descriptor;
    api::EngineCanonicalExpressionEvaluationResult comparison_result;
    detail.clear();
    passed &= Require(
        api::QowEvaluateCanonicalTypedExpressionV1(
            comparison, &comparison_result, &detail) &&
            comparison_result.truth ==
                api::EngineSqlTruthValue::true_value &&
            comparison_result.passes_consumer &&
            comparison_result.value.encoded_value == "true",
        "consumer comparison diverged: " + detail);

    comparison.left_value = Null(int64_descriptor);
    detail.clear();
    passed &= Require(
        api::QowEvaluateCanonicalTypedExpressionV1(
            comparison, &comparison_result, &detail) &&
            comparison_result.truth == api::EngineSqlTruthValue::unknown &&
            !comparison_result.passes_consumer &&
            comparison_result.value.state ==
                api::EngineValueState::sql_null &&
            comparison_result.value.encoded_value.empty(),
        "consumer SQL NULL comparison diverged: " + detail);

    bool consumes = true;
    detail.clear();
    passed &= Require(
        api::QowCanonicalExpressionConsumerPassesV1(
            consumer, api::EngineSqlTruthValue::false_value, &consumes,
            &detail) &&
            !consumes,
        "consumer FALSE admission diverged: " + detail);
  }

  api::EngineCanonicalExpressionEvaluationRequest overflow;
  overflow.consumer = api::EngineCanonicalExpressionConsumer::aggregate;
  overflow.operation = api::EngineCanonicalExpressionOperation::numeric_add;
  overflow.left_value =
      Value(int64_descriptor, std::to_string(
                                  std::numeric_limits<std::int64_t>::max()));
  overflow.right_value = Value(int64_descriptor, "1");
  overflow.result_descriptor = int64_descriptor;
  overflow.numeric_context.precision = 19;
  api::EngineCanonicalExpressionEvaluationResult refused;
  std::string refusal_detail;
  passed &= Require(
      !api::QowEvaluateCanonicalTypedExpressionV1(
          overflow, &refused, &refusal_detail) &&
          refused.value.state == api::EngineValueState::error &&
          refusal_detail.find("overflow") != std::string::npos,
      "canonical overflow produced a substitute value");

  overflow.consumer = api::EngineCanonicalExpressionConsumer::unspecified;
  overflow.left_value = left;
  overflow.right_value = right;
  refusal_detail.clear();
  passed &= Require(
      !api::QowEvaluateCanonicalTypedExpressionV1(
          overflow, &refused, &refusal_detail) &&
          refusal_detail.find("consumer") != std::string::npos,
      "unbound expression consumer was admitted");

  if (!passed) return 1;
  std::cout << "qow_qry_008_canonical_expression_runtime=passed\n";
  return 0;
}
