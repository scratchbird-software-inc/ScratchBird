// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/expression_api.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace api = scratchbird::engine::internal_api;

namespace {

using Truth = api::EngineSqlTruthValue;
using Operator = api::EngineComparisonPredicateOperator;

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-QRY-017-V1: " << detail << '\n';
  return condition;
}

api::EngineDescriptor DecimalDescriptor(const std::string& uuid) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "decimal";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000000901;"
      "nullability=nullable;precision=12;scale=2";
  return descriptor;
}

api::EngineDescriptor BooleanDescriptor() {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      "019f0000-0000-7200-8000-000000000902";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "boolean";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000000903;"
      "nullability=nullable";
  return descriptor;
}

api::EngineDescriptor TextDescriptor(const std::string& uuid,
                                     const std::string_view collation_uuid) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "text";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000000910;"
      "nullability=nullable;collation_uuid=" +
      std::string(collation_uuid);
  return descriptor;
}

api::EngineTypedValue DecimalValue(const std::string& uuid,
                                   const std::string& encoded) {
  api::EngineTypedValue value;
  value.descriptor = DecimalDescriptor(uuid);
  value.encoded_value = encoded;
  value.state = api::EngineValueState::value;
  return value;
}

api::EngineTypedValue SqlNullDecimal(const std::string& uuid) {
  auto value = DecimalValue(uuid, {});
  value.is_null = true;
  value.state = api::EngineValueState::sql_null;
  return value;
}

bool ValidateLogicalTruthTables() {
  constexpr std::array<Truth, 3> values{
      Truth::false_value, Truth::true_value, Truth::unknown};
  constexpr Truth expected_and[3][3] = {
      {Truth::false_value, Truth::false_value, Truth::false_value},
      {Truth::false_value, Truth::true_value, Truth::unknown},
      {Truth::false_value, Truth::unknown, Truth::unknown},
  };
  constexpr Truth expected_or[3][3] = {
      {Truth::false_value, Truth::true_value, Truth::unknown},
      {Truth::true_value, Truth::true_value, Truth::true_value},
      {Truth::unknown, Truth::true_value, Truth::unknown},
  };
  bool passed = true;
  for (std::size_t left = 0; left < values.size(); ++left) {
    for (std::size_t right = 0; right < values.size(); ++right) {
      passed &= Require(
          api::QowSqlAndV1(values[left], values[right]) ==
              expected_and[left][right],
          "SQL AND truth table drifted");
      passed &= Require(
          api::QowSqlOrV1(values[left], values[right]) ==
              expected_or[left][right],
          "SQL OR truth table drifted");
    }
  }
  passed &= Require(api::QowSqlNotV1(Truth::false_value) ==
                        Truth::true_value &&
                    api::QowSqlNotV1(Truth::true_value) ==
                        Truth::false_value &&
                    api::QowSqlNotV1(Truth::unknown) == Truth::unknown,
                    "SQL NOT truth table drifted");
  return passed;
}

bool ValidateTypedComparison() {
  const auto negative = DecimalValue(
      "019f0000-0000-7200-8000-000000000904", "-1.00");
  const auto zero = DecimalValue(
      "019f0000-0000-7200-8000-000000000905", "0.00");
  int comparison = 9;
  std::string refusal;
  bool passed = true;
  passed &= Require(api::QowCompareCanonicalNonCollatedScalarsV1(
                        negative, zero, &comparison, &refusal) &&
                        comparison == -1 && refusal.empty(),
                    "core numeric comparator was not used");

  struct Case {
    Operator operation;
    int comparison;
    Truth expected;
  };
  constexpr std::array<Case, 8> cases{{
      {Operator::equal, 0, Truth::true_value},
      {Operator::equal, -1, Truth::false_value},
      {Operator::not_equal, 1, Truth::true_value},
      {Operator::less_than, -1, Truth::true_value},
      {Operator::less_than_or_equal, 0, Truth::true_value},
      {Operator::greater_than, 1, Truth::true_value},
      {Operator::greater_than_or_equal, 0, Truth::true_value},
      {Operator::greater_than, -1, Truth::false_value},
  }};
  for (const auto& test : cases) {
    Truth truth = Truth::unknown;
    refusal.clear();
    passed &= Require(api::QowEvaluateCanonicalComparisonTruthV1(
                          negative, zero, test.comparison, test.operation,
                          &truth, &refusal) &&
                          truth == test.expected && refusal.empty(),
                      "typed comparison did not map ordering to SQL truth");
  }
  return passed;
}

bool ValidateNullSemantics() {
  const auto value = DecimalValue(
      "019f0000-0000-7200-8000-000000000906", "4.00");
  const auto null_value = SqlNullDecimal(
      "019f0000-0000-7200-8000-000000000907");
  bool passed = true;
  for (const auto operation :
       {Operator::equal, Operator::not_equal, Operator::less_than,
        Operator::less_than_or_equal, Operator::greater_than,
        Operator::greater_than_or_equal}) {
    Truth truth = Truth::false_value;
    std::string refusal;
    passed &= Require(api::QowEvaluateCanonicalComparisonTruthV1(
                          null_value, value, 0, operation, &truth,
                          &refusal) &&
                          truth == Truth::unknown && refusal.empty(),
                      "SQL NULL comparison did not yield UNKNOWN");
  }

  Truth truth = Truth::unknown;
  std::string refusal;
  passed &= Require(api::QowEvaluateCanonicalNullPredicateV1(
                        null_value, false, &truth, &refusal) &&
                        truth == Truth::true_value,
                    "IS NULL did not accept canonical SQL NULL");
  passed &= Require(api::QowEvaluateCanonicalNullPredicateV1(
                        null_value, true, &truth, &refusal) &&
                        truth == Truth::false_value,
                    "IS NOT NULL did not reject canonical SQL NULL");
  passed &= Require(api::QowEvaluateCanonicalNullPredicateV1(
                        value, true, &truth, &refusal) &&
                        truth == Truth::true_value,
                    "IS NOT NULL did not accept a value state");

  constexpr std::string_view kCollation =
      "019f0000-0000-7400-8000-000000000911";
  api::EngineTypedValue null_text;
  null_text.descriptor = TextDescriptor(
      "019f0000-0000-7200-8000-000000000912", kCollation);
  null_text.state = api::EngineValueState::sql_null;
  api::EngineTypedValue text_value;
  text_value.descriptor = TextDescriptor(
      "019f0000-0000-7200-8000-000000000913", kCollation);
  text_value.encoded_value = "alpha";
  text_value.state = api::EngineValueState::value;
  refusal.clear();
  passed &= Require(api::QowEvaluateCanonicalComparisonTruthV1(
                        null_text, text_value, 0, Operator::equal, &truth,
                        &refusal) &&
                        truth == Truth::unknown,
                    "collated character NULL comparison was not UNKNOWN");
  auto mismatched_text = text_value;
  mismatched_text.descriptor = TextDescriptor(
      "019f0000-0000-7200-8000-000000000914",
      "019f0000-0000-7400-8000-000000000915");
  refusal.clear();
  passed &= Require(!api::QowEvaluateCanonicalComparisonTruthV1(
                        null_text, mismatched_text, 0, Operator::equal,
                        &truth, &refusal),
                    "NULL bypassed mismatched collation authority");

  auto substituted = null_value;
  substituted.encoded_value = "0.00";
  refusal.clear();
  passed &= Require(!api::QowEvaluateCanonicalComparisonTruthV1(
                        substituted, value, 0, Operator::equal, &truth,
                        &refusal) &&
                        !refusal.empty(),
                    "NULL carrying a zero substitute became UNKNOWN data");
  auto legacy_flag_only = null_value;
  legacy_flag_only.state = api::EngineValueState::value;
  refusal.clear();
  passed &= Require(!api::QowEvaluateCanonicalComparisonTruthV1(
                        legacy_flag_only, value, 0, Operator::equal, &truth,
                        &refusal),
                    "legacy null flag overrode the canonical value state");
  auto missing = value;
  missing.state = api::EngineValueState::missing;
  refusal.clear();
  passed &= Require(!api::QowEvaluateCanonicalNullPredicateV1(
                        missing, false, &truth, &refusal),
                    "missing runtime sentinel became SQL NULL");
  return passed;
}

bool ValidateTruthMaterializationAndConsumers() {
  const auto descriptor = BooleanDescriptor();
  bool passed = true;
  for (const auto truth :
       {Truth::false_value, Truth::true_value, Truth::unknown}) {
    api::EngineTypedValue value;
    std::string refusal;
    passed &= Require(api::QowMaterializeCanonicalTruthValueV1(
                          truth, descriptor, &value, &refusal),
                      "canonical truth materialization was refused");
    passed &= Require(
        value.descriptor.descriptor_uuid.canonical ==
            descriptor.descriptor_uuid.canonical,
        "truth value lost its bound boolean descriptor");
    if (truth == Truth::unknown) {
      passed &= Require(value.state == api::EngineValueState::sql_null &&
                            value.is_null && value.encoded_value.empty() &&
                            value.binary_value.empty(),
                        "UNKNOWN was encoded as character or numeric data");
    } else {
      passed &= Require(
          value.state == api::EngineValueState::value && !value.is_null &&
              value.encoded_value ==
                  (truth == Truth::true_value ? "true" : "false"),
          "TRUE/FALSE was not materialized as typed boolean data");
    }
  }

  for (const auto consumer :
       {api::EnginePredicateConsumer::filter,
        api::EnginePredicateConsumer::join_on,
        api::EnginePredicateConsumer::having,
        api::EnginePredicateConsumer::qualify}) {
    for (const auto truth :
         {Truth::false_value, Truth::true_value, Truth::unknown}) {
      bool admits = true;
      std::string refusal;
      passed &= Require(api::QowPredicateConsumerPassesV1(
                            truth, consumer, &admits, &refusal) &&
                            admits == (truth == Truth::true_value),
                        "predicate consumer did not share TRUE-only admission");
    }
  }
  bool admits = true;
  std::string refusal;
  passed &= Require(!api::QowPredicateConsumerPassesV1(
                        Truth::true_value,
                        api::EnginePredicateConsumer::unspecified, &admits,
                        &refusal) &&
                        !admits,
                    "unbound predicate consumer was admitted");
  api::EngineTypedValue invalid_value;
  refusal.clear();
  passed &= Require(!api::QowMaterializeCanonicalTruthValueV1(
                        Truth::unspecified, descriptor, &invalid_value,
                        &refusal) &&
                        invalid_value.state == api::EngineValueState::error,
                    "unbound truth value was materialized as predicate data");
  auto non_null_descriptor = descriptor;
  non_null_descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000000903;"
      "nullability=non_null";
  refusal.clear();
  passed &= Require(!api::QowMaterializeCanonicalTruthValueV1(
                        Truth::unknown, non_null_descriptor, &invalid_value,
                        &refusal),
                    "UNKNOWN violated a non-NULL boolean result descriptor");
  return passed;
}

bool ValidateCollationAndDescriptorRefusal() {
  auto left = DecimalValue(
      "019f0000-0000-7200-8000-000000000908", "1.00");
  auto right = DecimalValue(
      "019f0000-0000-7200-8000-000000000909", "1.00");
  left.descriptor.canonical_type_name = "text";
  right.descriptor.canonical_type_name = "text";
  int comparison = 0;
  std::string refusal;
  bool passed = true;
  passed &= Require(!api::QowCompareCanonicalNonCollatedScalarsV1(
                        left, right, &comparison, &refusal) &&
                        refusal.find("collation") != std::string::npos,
                    "character comparison bypassed bound collation authority");

  const auto numeric = DecimalValue(
      "019f0000-0000-7200-8000-000000000916", "1.00");
  auto malformed_numeric = DecimalValue(
      "019f0000-0000-7200-8000-000000000917", "not-a-number");
  refusal.clear();
  passed &= Require(!api::QowCompareCanonicalNonCollatedScalarsV1(
                        numeric, malformed_numeric, &comparison, &refusal) &&
                        !refusal.empty(),
                    "malformed numeric comparison became zero data");

  auto malformed = DecimalValue("not-a-uuid", "1.00");
  Truth truth = Truth::true_value;
  refusal.clear();
  passed &= Require(!api::QowEvaluateCanonicalComparisonTruthV1(
                        malformed, right, 0, Operator::equal, &truth,
                        &refusal),
                    "malformed descriptor identity became predicate data");
  return passed;
}

}  // namespace

// QOW-TEST-QRY-017-V1
int main() {
  bool passed = true;
  passed &= ValidateLogicalTruthTables();
  passed &= ValidateTypedComparison();
  passed &= ValidateNullSemantics();
  passed &= ValidateTruthMaterializationAndConsumers();
  passed &= ValidateCollationAndDescriptorRefusal();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
