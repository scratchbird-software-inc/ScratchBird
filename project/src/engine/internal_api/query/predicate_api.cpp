// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/predicate_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "datatype_operations.hpp"

#include <string>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

const char* PredicateConsumerName(const EnginePredicateConsumer consumer) {
  switch (consumer) {
    case EnginePredicateConsumer::filter:
      return "filter";
    case EnginePredicateConsumer::join_on:
      return "join_on";
    case EnginePredicateConsumer::having:
      return "having";
    case EnginePredicateConsumer::qualify:
      return "qualify";
    case EnginePredicateConsumer::unspecified:
      return "unspecified";
  }
  return "invalid";
}

const char* PredicateOperatorName(
    const EngineComparisonPredicateOperator operation) {
  switch (operation) {
    case EngineComparisonPredicateOperator::equal:
      return "equal";
    case EngineComparisonPredicateOperator::not_equal:
      return "not_equal";
    case EngineComparisonPredicateOperator::less_than:
      return "less_than";
    case EngineComparisonPredicateOperator::less_than_or_equal:
      return "less_than_or_equal";
    case EngineComparisonPredicateOperator::greater_than:
      return "greater_than";
    case EngineComparisonPredicateOperator::greater_than_or_equal:
      return "greater_than_or_equal";
    case EngineComparisonPredicateOperator::is_null:
      return "is_null";
    case EngineComparisonPredicateOperator::is_not_null:
      return "is_not_null";
    case EngineComparisonPredicateOperator::logical_not:
      return "logical_not";
    case EngineComparisonPredicateOperator::logical_and:
      return "logical_and";
    case EngineComparisonPredicateOperator::logical_or:
      return "logical_or";
    case EngineComparisonPredicateOperator::unspecified:
      return "unspecified";
  }
  return "invalid";
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_QUERY_PREDICATE_API_BEHAVIOR
EngineBindPredicateResult EngineBindPredicate(const EngineBindPredicateRequest& request) {
  const auto refuse = [&](std::string detail) {
    return MakeApiBehaviorDiagnostic<EngineBindPredicateResult>(
        request.context, "query.bind_predicate",
        MakeEngineApiDiagnostic(
            "QOW-DIAG-QRY-025-BIND-REFUSAL-V1",
            "engine.query.predicate_binding_refused", std::move(detail)));
  };
  if (request.predicate.predicate_kind.empty() ||
      request.predicate.canonical_predicate_envelope.empty()) {
    return refuse(
        "canonical predicate kind and immutable envelope are required");
  }
  const auto& predicate_kind = request.predicate.predicate_kind;
  if (predicate_kind != "equal" && predicate_kind != "not_equal" &&
      predicate_kind != "less_than" &&
      predicate_kind != "less_than_or_equal" &&
      predicate_kind != "greater_than" &&
      predicate_kind != "greater_than_or_equal" &&
      predicate_kind != "is_null" && predicate_kind != "is_not_null" &&
      predicate_kind != "logical_not" && predicate_kind != "logical_and" &&
      predicate_kind != "logical_or") {
    return refuse("canonical predicate kind is unsupported");
  }
  for (const auto& value : request.predicate.bound_values) {
    if (!QowCanonicalDescriptorIdentityV1(value.descriptor) ||
        scratchbird::core::datatypes::CanonicalTypeIdFromStableName(
            value.descriptor.canonical_type_name) ==
            scratchbird::core::datatypes::CanonicalTypeId::unknown ||
        (value.isSqlNull() && !QowCanonicalSqlNullStateV1(value)) ||
        (!value.isSqlNull() &&
         (value.state != EngineValueState::value || value.is_null))) {
      return refuse("predicate bound value descriptor or state is invalid");
    }
  }
  auto result = MakeApiBehaviorSuccess<EngineBindPredicateResult>(request.context, "query.bind_predicate");
  result.bound_predicate = request.predicate;
  result.result_shape.result_kind = "bound_predicate";
  AddApiBehaviorEvidence(&result, "query_binding", "predicate");
  AddApiBehaviorRow(&result, {{"predicate_kind", request.predicate.predicate_kind}, {"predicate_envelope", request.predicate.canonical_predicate_envelope}, {"bound_value_count", std::to_string(request.predicate.bound_values.size())}});
  return result;
}

// QOW-CONSUMER-QRY-017-V1
EngineEvaluatePredicateResult EngineEvaluatePredicate(
    const EngineEvaluatePredicateRequest& request) {
  constexpr const char* kOperation = "query.evaluate_predicate";
  const auto refuse = [&](std::string detail) {
    return MakeApiBehaviorDiagnostic<EngineEvaluatePredicateResult>(
        request.context,
        kOperation,
        MakeEngineApiDiagnostic(
            "QOW-DIAG-QRY-017-3VL-REFUSAL-V1",
            "engine.query.typed_predicate_refused", std::move(detail)));
  };

  EngineSqlTruthValue truth = EngineSqlTruthValue::unknown;
  int comparison = 0;
  std::string refusal_detail;
  switch (request.predicate_operator) {
    case EngineComparisonPredicateOperator::logical_not:
      if (!QowCanonicalTruthValueV1(request.left_truth)) {
        return refuse("logical NOT input is not a canonical truth value");
      }
      truth = QowSqlNotV1(request.left_truth);
      break;
    case EngineComparisonPredicateOperator::logical_and:
      if (!QowCanonicalTruthValueV1(request.left_truth) ||
          !QowCanonicalTruthValueV1(request.right_truth)) {
        return refuse("logical AND input is not a canonical truth value");
      }
      truth = QowSqlAndV1(request.left_truth, request.right_truth);
      break;
    case EngineComparisonPredicateOperator::logical_or:
      if (!QowCanonicalTruthValueV1(request.left_truth) ||
          !QowCanonicalTruthValueV1(request.right_truth)) {
        return refuse("logical OR input is not a canonical truth value");
      }
      truth = QowSqlOrV1(request.left_truth, request.right_truth);
      break;
    case EngineComparisonPredicateOperator::is_null:
    case EngineComparisonPredicateOperator::is_not_null:
      if (!QowEvaluateCanonicalNullPredicateV1(
              request.left_value,
              request.predicate_operator ==
                  EngineComparisonPredicateOperator::is_not_null,
              &truth, &refusal_detail)) {
        return refuse(std::move(refusal_detail));
      }
      break;
    case EngineComparisonPredicateOperator::equal:
    case EngineComparisonPredicateOperator::not_equal:
    case EngineComparisonPredicateOperator::less_than:
    case EngineComparisonPredicateOperator::less_than_or_equal:
    case EngineComparisonPredicateOperator::greater_than:
    case EngineComparisonPredicateOperator::greater_than_or_equal: {
      const auto type_id =
          scratchbird::core::datatypes::CanonicalTypeIdFromStableName(
              request.left_value.descriptor.canonical_type_name);
      if (type_id ==
          scratchbird::core::datatypes::CanonicalTypeId::character) {
        EngineCompareScalarValuesRequest compare_request;
        compare_request.context = request.context;
        compare_request.left_value = request.left_value;
        compare_request.right_value = request.right_value;
        const auto compared = EngineCompareScalarValues(compare_request);
        if (!compared.ok) {
          const std::string detail =
              compared.diagnostics.empty()
                  ? "catalog-bound character comparison refused"
                  : (compared.diagnostics.front().code + ":" +
                     compared.diagnostics.front().detail);
          return refuse(detail);
        }
        comparison = compared.comparison;
      } else if (!request.left_value.isSqlNull() &&
                 !request.right_value.isSqlNull() &&
                 !QowCompareCanonicalNonCollatedScalarsV1(
                     request.left_value, request.right_value, &comparison,
                     &refusal_detail)) {
        return refuse(std::move(refusal_detail));
      }
      if (!QowEvaluateCanonicalComparisonTruthV1(
              request.left_value, request.right_value, comparison,
              request.predicate_operator, &truth, &refusal_detail)) {
        return refuse(std::move(refusal_detail));
      }
      break;
    }
    case EngineComparisonPredicateOperator::unspecified:
    default:
      return refuse("typed predicate operator is not bound");
  }

  EngineCanonicalExpressionConsumer expression_consumer =
      EngineCanonicalExpressionConsumer::unspecified;
  if (request.consumer == EnginePredicateConsumer::filter) {
    expression_consumer = EngineCanonicalExpressionConsumer::filter;
  } else if (request.consumer == EnginePredicateConsumer::join_on) {
    expression_consumer = EngineCanonicalExpressionConsumer::join;
  } else if (request.consumer == EnginePredicateConsumer::having) {
    expression_consumer = EngineCanonicalExpressionConsumer::aggregate;
  } else if (request.consumer == EnginePredicateConsumer::qualify) {
    expression_consumer = EngineCanonicalExpressionConsumer::window;
  }
  EngineCanonicalExpressionEvaluationRequest expression_request;
  expression_request.consumer = expression_consumer;
  expression_request.operation =
      EngineCanonicalExpressionOperation::consume_truth;
  expression_request.input_truth = truth;
  expression_request.result_descriptor = request.result_descriptor;
  EngineCanonicalExpressionEvaluationResult expression_result;
  if (!QowEvaluateCanonicalTypedExpressionV1(
          expression_request, &expression_result, &refusal_detail)) {
    return refuse(std::move(refusal_detail));
  }

  auto result = MakeApiBehaviorSuccess<EngineEvaluatePredicateResult>(
      request.context, kOperation);
  result.truth_value = truth;
  result.value = std::move(expression_result.value);
  result.comparison = comparison;
  result.passes_consumer = expression_result.passes_consumer;
  result.result_shape.result_kind = "typed_value";
  result.result_shape.columns.push_back(result.value.descriptor);
  AddApiBehaviorEvidence(&result, "three_valued_runtime",
                         "QOW-SOURCE-QRY-017-V1");
  AddApiBehaviorEvidence(&result, "predicate_operator",
                         PredicateOperatorName(request.predicate_operator));
  AddApiBehaviorEvidence(&result, "predicate_consumer",
                         PredicateConsumerName(request.consumer));
  AddApiBehaviorEvidence(&result, "sql_truth_value",
                         EngineSqlTruthValueName(truth));
  return result;
}

}  // namespace scratchbird::engine::internal_api
