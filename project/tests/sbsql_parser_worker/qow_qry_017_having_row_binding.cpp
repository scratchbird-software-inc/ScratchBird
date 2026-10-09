// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/binary_uuid_fixture.hpp"
#include "../support/canonical_int64_literal_fixture.hpp"
#include "../support/native_int64_fixture.hpp"
#include "canonical_query_descriptor_support.hpp"
#include "canonical_relational_expression.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

namespace {

// QOW-TEST-QRY-017-HAVING-ROW-BINDING-V1
constexpr auto kCountUuid = scratchbird::tests::FixtureUuidLiteral("019de5fc-2400-784a-9aec-371f8b95b7ea");
constexpr auto kSumUuid = scratchbird::tests::FixtureUuidLiteral("019de5fc-2400-72e4-8549-82b2eef5a777");
constexpr auto kInt64TypeUuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d712");
constexpr auto kBooleanTypeUuid = scratchbird::tests::FixtureUuidLiteral("01000000-626f-7f6c-a561-6e0000000000");

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-017-HAVING-ROW-BINDING-V1: " << detail
              << '\n';
  }
  return condition;
}

api::RelationalTypeDescriptor Descriptor(
    const std::uint32_t id, const api::EngineUuid& uuid,
    const api::EngineUuid& type_uuid, const api::RelationalNullability nullable) {
  auto descriptor = scratchbird::tests::Int64LiteralDescriptor(id);
  if (type_uuid == kBooleanTypeUuid) {
    const auto identity = scratchbird::core::datatypes::LookupCanonicalBooleanTypeCodecIdentityV1(
        descriptor.datatype_catalog_snapshot_uuid, descriptor.datatype_catalog_generation,
        descriptor.datatype_registry_generation);
    if (!identity.ok) throw std::runtime_error("HAVING boolean fixture binding unavailable");
    descriptor.descriptor_uuid = identity.row.descriptor_uuid;
    descriptor.type_uuid = identity.row.type_uuid;
    descriptor.descriptor_generation = identity.row.descriptor_generation;
    descriptor.type_generation = identity.row.type_generation;
    descriptor.codec_id = identity.row.codec_id;
    descriptor.codec_version = identity.row.codec_version;
    descriptor.codec_generation = identity.row.codec_generation;
  } else {
    descriptor.descriptor_uuid = uuid;
  }
  descriptor.descriptor_id = id;
  descriptor.nullability = nullable;
  return descriptor;
}

api::RelationalExpressionRecord Function(
    const std::uint32_t id, const std::uint32_t descriptor_id,
    const api::EngineUuid& function_uuid,
    std::vector<std::uint32_t> children = {}) {
  api::RelationalExpressionRecord expression;
  expression.expression_id = id;
  expression.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  expression.child_expression_ids = std::move(children);
  expression.result_descriptor_id = descriptor_id;
  expression.function_uuid = function_uuid;
  return expression;
}

api::RelationalExpressionRecord Literal(const std::uint32_t id,
                                        const std::uint32_t descriptor_id,
                                        const std::string& encoded) {
  api::RelationalExpressionRecord expression;
  expression.expression_id = id;
  expression.expression_kind = api::RelationalExpressionKind::kLiteral;
  expression.result_descriptor_id = descriptor_id;
  expression.literal_kind = api::RelationalLiteralKind::kNumeric;
  expression.literal_or_parameter_ref = encoded;
  return expression;
}

api::RelationalExpressionRecord Binary(
    const std::uint32_t id, const std::uint32_t descriptor_id,
    const std::string_view operation, const std::uint32_t left,
    const std::uint32_t right) {
  api::RelationalExpressionRecord expression;
  expression.expression_id = id;
  expression.expression_kind = api::RelationalExpressionKind::kBinary;
  expression.child_expression_ids = {left, right};
  expression.result_descriptor_id = descriptor_id;
  expression.operator_name = operation;
  return expression;
}

api::RelationalExpressionRecord Unary(
    const std::uint32_t id, const std::uint32_t descriptor_id,
    const std::string_view operation, const std::uint32_t child) {
  api::RelationalExpressionRecord expression;
  expression.expression_id = id;
  expression.expression_kind = api::RelationalExpressionKind::kUnary;
  expression.child_expression_ids = {child};
  expression.result_descriptor_id = descriptor_id;
  expression.operator_name = operation;
  return expression;
}

api::TypedRelationalDag Dag(const std::string& count_threshold = "1",
                            const std::string& sum_threshold = "6") {
  api::TypedRelationalDag dag;
  dag.descriptors = {
      Descriptor(1, scratchbird::tests::FixtureUuidLiteral("019f3300-0000-7100-8000-000000000101"),
                 kInt64TypeUuid,
                 api::RelationalNullability::kNonNull),
      Descriptor(2, scratchbird::tests::FixtureUuidLiteral("019f3300-0000-7100-8000-000000000102"),
                 kInt64TypeUuid,
                 api::RelationalNullability::kNullable),
      Descriptor(3, scratchbird::tests::FixtureUuidLiteral("019f3300-0000-7100-8000-000000000103"),
                 kInt64TypeUuid,
                 api::RelationalNullability::kNonNull),
      Descriptor(4, scratchbird::tests::FixtureUuidLiteral("019f3300-0000-7100-8000-000000000104"),
                 kBooleanTypeUuid,
                 api::RelationalNullability::kNullable),
      Descriptor(5, scratchbird::tests::FixtureUuidLiteral("019f3300-0000-7100-8000-000000000105"),
                 kInt64TypeUuid,
                 api::RelationalNullability::kNullable),
      Descriptor(6, scratchbird::tests::FixtureUuidLiteral("019f3300-0000-7100-8000-000000000106"),
                 kInt64TypeUuid,
                 api::RelationalNullability::kNullable),
  };
  // INT64 has no collation/timezone/width/scale modifiers. Start with a valid
  // native descriptor and test attempted decoration as explicit refusals.

  api::RelationalExpressionRecord identifier;
  identifier.expression_id = 11;
  identifier.expression_kind = api::RelationalExpressionKind::kIdentifier;
  identifier.result_descriptor_id = 6;
  identifier.bound_name_uuid =
      scratchbird::tests::FixtureUuidLiteral("019f3300-0000-7400-8000-000000000401");

  dag.expressions = {
      Function(1, 1, kCountUuid),
      Function(2, 2, kSumUuid, {14}),
      Literal(3, 3, count_threshold),
      Literal(4, 3, sum_threshold),
      Binary(5, 4, ">", 1, 3),
      Binary(6, 4, ">", 2, 4),
      Binary(7, 4, "AND", 5, 6),
      Binary(8, 4, "OR", 5, 6),
      Unary(9, 4, "NOT", 6),
      Unary(10, 4, "NOT", 7),
      std::move(identifier),
      Function(12, 1, scratchbird::tests::FixtureUuidLiteral("019f3300-0000-7500-8000-000000000501")),
      Function(13, 5, scratchbird::tests::FixtureUuidLiteral("019f3300-0000-7500-8000-000000000502")),
      Binary(16, 4, ">", 11, 18),
      Binary(17, 4, "AND", 16, 6),
      Literal(18, 6, "1"),
  };
  for (auto& expression : dag.expressions) {
    if (expression.expression_kind != api::RelationalExpressionKind::kLiteral) continue;
    const auto& descriptor = dag.descriptors.at(expression.result_descriptor_id - 1);
    std::int64_t value = 0;
    const auto& text = *expression.literal_or_parameter_ref;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
      throw std::runtime_error("HAVING fixture integer literal invalid");
    scratchbird::tests::SetInt64Literal(expression, descriptor, value);
  }
  api::RelationalExpressionRecord sum_argument;
  sum_argument.expression_id = 14;
  sum_argument.expression_kind = api::RelationalExpressionKind::kIdentifier;
  sum_argument.result_descriptor_id = 2;
  sum_argument.bound_name_uuid =
      scratchbird::tests::FixtureUuidLiteral("019f3300-0000-7400-8000-000000000402");
  dag.expressions.push_back(std::move(sum_argument));
  api::RelationalExpressionRecord parameter;
  parameter.expression_id = 15;
  parameter.expression_kind = api::RelationalExpressionKind::kParameter;
  parameter.result_descriptor_id = 1;
  parameter.literal_or_parameter_ref = "parameter-1";
  dag.expressions.push_back(std::move(parameter));
  return dag;
}

const api::RelationalTypeDescriptor& FindDescriptor(
    const api::TypedRelationalDag& dag, const std::uint32_t id) {
  for (const auto& descriptor : dag.descriptors) {
    if (descriptor.descriptor_id == id) return descriptor;
  }
  std::abort();
}

api::RelationalExpressionRecord& FindExpression(api::TypedRelationalDag& dag,
                                                const std::uint32_t id) {
  for (auto& expression : dag.expressions) {
    if (expression.expression_id == id) return expression;
  }
  std::abort();
}

api::EngineDescriptor EngineDescriptor(const api::TypedRelationalDag& dag,
                                       const std::uint32_t id,
                                       const std::string_view type_name) {
  const auto& source = FindDescriptor(dag, id);
  api::EngineDescriptor descriptor;
  if (!sblr::BuildExactCanonicalScalarRuntimeDescriptorV1(source,
          scratchbird::core::datatypes::CanonicalTypeIdFromStableName(std::string(type_name)), &descriptor))
    throw std::runtime_error("HAVING fixture native scalar descriptor invalid");
  return descriptor;
}

api::EngineTypedValue Value(const api::TypedRelationalDag& dag,
                            const std::uint32_t descriptor_id,
                            const std::string_view type_name,
                            const std::string& encoded) {
  return scratchbird::tests::NativeInt64Fixture(EngineDescriptor(dag, descriptor_id, type_name), encoded);
}

std::vector<api::EngineTypedValue> Row(const api::TypedRelationalDag& dag,
                                       const std::int64_t count,
                                       const std::string& sum) {
  return {Value(dag, 1, "int64", std::to_string(count)),
          Value(dag, 2, "int64", sum),
          Value(dag, 5, "int64", "99"),
          Value(dag, 6, "int64", "99")};
}

std::vector<api::EngineTypedValue> NullSumRow(
    const api::TypedRelationalDag& dag, const std::int64_t count) {
  auto row = Row(dag, count, "0");
  row[1].encoded_value.clear();
  row[1].binary_value.clear();
  row[1].setState(api::EngineValueState::sql_null);
  return row;
}

std::vector<api::EngineTypedValue> NullGroupKeyRow(
    const api::TypedRelationalDag& dag, const std::int64_t count,
    const std::string& sum) {
  auto row = Row(dag, count, sum);
  row[3].encoded_value.clear();
  row[3].binary_value.clear();
  row[3].setState(api::EngineValueState::sql_null);
  return row;
}

sblr::CanonicalRelationalExpressionRowBinding BooleanBinding() {
  return {{1, 2, 5, 6}, {}, {{1, 1, 0}, {2, 2, 1}}};
}

sblr::CanonicalRelationalExpressionRowBinding SumBinding() {
  return {{1, 2, 5, 6}, {}, {{2, 2, 1}}};
}

sblr::CanonicalRelationalExpressionRowBinding GroupingKeyBinding() {
  return {{1, 2, 5, 6}, {},
          {{11, 6, 3,
            sblr::CanonicalRelationalExpressionRowSlotKind::grouping_key}}};
}

sblr::CanonicalRelationalExpressionRowBinding GroupedHavingBinding() {
  return {{1, 2, 5, 6}, {},
          {{2, 2, 1},
           {11, 6, 3,
            sblr::CanonicalRelationalExpressionRowSlotKind::grouping_key}}};
}

bool Evaluate(const api::TypedRelationalDag& dag, const std::uint32_t root,
              const sblr::CanonicalRelationalExpressionRowBinding& binding,
              const std::vector<api::EngineTypedValue>& row,
              const api::EngineSqlTruthValue expected) {
  sblr::CanonicalRelationalExpressionRuntime runtime(dag);
  api::EngineSqlTruthValue truth = api::EngineSqlTruthValue::unspecified;
  std::string refusal;
  const bool evaluated =
      runtime.EvaluatePredicate(root, binding, row, &truth, &refusal);
  return Require(evaluated && truth == expected && refusal.empty(),
                 "materialized HAVING predicate truth drifted" +
                     (refusal.empty() ? std::string{}
                                      : ": " + refusal));
}

bool Refuses(const api::TypedRelationalDag& dag, const std::uint32_t root,
             const sblr::CanonicalRelationalExpressionRowBinding& binding,
             const std::vector<api::EngineTypedValue>& row,
             const std::string_view detail) {
  sblr::CanonicalRelationalExpressionRuntime runtime(dag);
  api::EngineSqlTruthValue truth = api::EngineSqlTruthValue::unspecified;
  std::string refusal;
  return Require(!runtime.EvaluatePredicate(root, binding, row, &truth,
                                            &refusal) &&
                     !refusal.empty(),
                 detail);
}

bool ValidateFiveProfilesAndThreeValuedLogic() {
  const auto dag = Dag();
  struct Case {
    std::vector<api::EngineTypedValue> row;
    api::EngineSqlTruthValue sum;
    api::EngineSqlTruthValue not_sum;
    api::EngineSqlTruthValue conjunction;
    api::EngineSqlTruthValue disjunction;
    api::EngineSqlTruthValue not_conjunction;
  };
  using Truth = api::EngineSqlTruthValue;
  const std::vector<Case> cases{
      {Row(dag, 2, "7"), Truth::true_value, Truth::false_value,
       Truth::true_value, Truth::true_value, Truth::false_value},
      {Row(dag, 1, "7"), Truth::true_value, Truth::false_value,
       Truth::false_value, Truth::true_value, Truth::true_value},
      {Row(dag, 2, "5"), Truth::false_value, Truth::true_value,
       Truth::false_value, Truth::true_value, Truth::true_value},
      {Row(dag, 1, "5"), Truth::false_value, Truth::true_value,
       Truth::false_value, Truth::false_value, Truth::true_value},
      {NullSumRow(dag, 2), Truth::unknown, Truth::unknown, Truth::unknown,
       Truth::true_value, Truth::unknown},
      {NullSumRow(dag, 1), Truth::unknown, Truth::unknown, Truth::false_value,
       Truth::unknown, Truth::true_value},
  };
  bool passed = true;
  for (const auto& test : cases) {
    passed &= Evaluate(dag, 6, SumBinding(), test.row, test.sum);
    passed &= Evaluate(dag, 9, SumBinding(), test.row, test.not_sum);
    passed &= Evaluate(dag, 7, BooleanBinding(), test.row, test.conjunction);
    passed &= Evaluate(dag, 8, BooleanBinding(), test.row, test.disjunction);
    passed &= Evaluate(dag, 10, BooleanBinding(), test.row,
                       test.not_conjunction);
  }

  const auto shifted = Dag("0", "4");
  passed &= Evaluate(shifted, 7, BooleanBinding(), Row(shifted, 1, "5"),
                     Truth::true_value);
  passed &= Evaluate(shifted, 10, BooleanBinding(), Row(shifted, 1, "5"),
                     Truth::false_value);

  // RCP-026-TEST-GROUPING-KEY-HAVING-ROW-BINDING-V1
  passed &= Evaluate(dag, 16, GroupingKeyBinding(), Row(dag, 2, "7"),
                     Truth::true_value);
  auto false_key = Row(dag, 2, "7");
  false_key[3] = Value(dag, 6, "int64", "0");
  passed &= Evaluate(dag, 16, GroupingKeyBinding(), false_key,
                     Truth::false_value);
  passed &= Evaluate(dag, 16, GroupingKeyBinding(),
                     NullGroupKeyRow(dag, 2, "7"), Truth::unknown);
  passed &= Evaluate(dag, 17, GroupedHavingBinding(), Row(dag, 2, "7"),
                     Truth::true_value);
  passed &= Evaluate(dag, 17, GroupedHavingBinding(),
                     NullGroupKeyRow(dag, 2, "7"), Truth::unknown);
  passed &= Evaluate(dag, 7, BooleanBinding(),
                     Row(dag, INT64_MAX, "9223372036854775807"), Truth::true_value);
  passed &= Evaluate(dag, 8, BooleanBinding(),
                     Row(dag, INT64_MIN, "-9223372036854775808"), Truth::false_value);
  const auto limits = Dag("9223372036854775807", "-9223372036854775808");
  passed &= Evaluate(limits, 7, BooleanBinding(),
                     Row(limits, INT64_MAX, "-9223372036854775808"), Truth::false_value);
  return passed;
}

bool ValidateBatchRuntimeReuse() {
  const auto dag = Dag();
  sblr::CanonicalRelationalExpressionRuntime runtime(dag);
  const auto binding = BooleanBinding();
  using Truth = api::EngineSqlTruthValue;
  struct Case {
    std::vector<api::EngineTypedValue> row;
    Truth expected;
  };
  const std::vector<Case> cases{
      {Row(dag, 2, "7"), Truth::false_value},
      {Row(dag, 1, "5"), Truth::true_value},
      {NullSumRow(dag, 2), Truth::unknown},
      {Row(dag, 1, "5"), Truth::true_value},
  };
  bool passed = true;
  for (const auto& test : cases) {
    Truth truth = Truth::unspecified;
    std::string refusal;
    passed &= Require(runtime.EvaluatePredicate(10, binding, test.row, &truth,
                                                &refusal) &&
                          truth == test.expected && refusal.empty(),
                      "batch runtime leaked a prior row binding or truth");
  }
  return passed;
}

bool ValidateBindingRefusals() {
  const auto dag = Dag();
  const auto row = Row(dag, 2, "7");
  bool passed = true;

  auto binding = BooleanBinding();
  binding.slots.pop_back();
  passed &= Refuses(dag, 7, binding, row, "missing aggregate slot was admitted");

  binding = BooleanBinding();
  binding.slots[0].expression_id = 999;
  passed &= Refuses(dag, 7, binding, row, "unknown expression slot was admitted");
  binding = BooleanBinding();
  binding.slots[0].descriptor_id = 2;
  passed &= Refuses(dag, 7, binding, row, "wrong slot descriptor was admitted");
  binding = BooleanBinding();
  binding.slots[0].row_ordinal = 3;
  passed &= Refuses(dag, 7, binding, row,
                    "out-of-range slot row ordinal was admitted");
  binding = BooleanBinding();
  binding.slots.push_back({1, 1, 0});
  passed &= Refuses(dag, 7, binding, row, "duplicate expression was admitted");
  binding = BooleanBinding();
  binding.slots[1].row_ordinal = 0;
  passed &= Refuses(dag, 7, binding, row, "duplicate row ordinal was admitted");
  binding = SumBinding();
  binding.slots.push_back({13, 5, 2});
  passed &= Refuses(dag, 6, binding, row, "unreachable extra slot was admitted");

  binding = {{1, 2, 5, 6}, {}, {}};
  passed &= Refuses(dag, 11, binding, row,
                    "unbound reachable identifier was admitted");
  passed &= Refuses(dag, 12, binding, row,
                    "unbound reachable arbitrary function was admitted");
  passed &= Refuses(dag, 15, binding, row,
                    "unbound reachable parameter was admitted");
  binding.slots.push_back({11, 1, 0});
  passed &= Refuses(dag, 11, binding, row,
                    "identifier was admitted as a materialized function slot");
  binding = {{1, 2, 5, 6}, {},
             {{1, 1, 0,
               sblr::CanonicalRelationalExpressionRowSlotKind::grouping_key}}};
  passed &= Refuses(dag, 1, binding, row,
                    "aggregate function was admitted as a grouping-key slot");
  binding = GroupingKeyBinding();
  binding.slots[0].descriptor_id = 1;
  passed &= Refuses(dag, 16, binding, row,
                    "grouping-key slot descriptor drift was admitted");
  binding = GroupingKeyBinding();
  binding.slots[0].slot_kind =
      static_cast<sblr::CanonicalRelationalExpressionRowSlotKind>(255);
  passed &= Refuses(dag, 16, binding, row,
                    "unknown materialized row slot kind was admitted");
  binding = GroupingKeyBinding();
  {
    sblr::CanonicalRelationalExpressionRuntime runtime(dag);
    api::EngineSqlTruthValue truth = api::EngineSqlTruthValue::unspecified;
    std::string refusal;
    passed &= Require(
        !runtime.EvaluatePredicateForConsumer(
            16, binding, row,
            api::EngineCanonicalExpressionConsumer::filter, &truth,
            &refusal) &&
            !refusal.empty(),
        "grouping-key row slot escaped the aggregate/HAVING consumer");
  }

  binding = BooleanBinding();
  binding.row_descriptor_ids[2] = 2;
  passed &= Refuses(dag, 7, binding, row,
                    "duplicate row descriptor identity was admitted");
  binding = BooleanBinding();
  auto narrow_row = row;
  narrow_row.pop_back();
  passed &= Refuses(dag, 7, binding, narrow_row,
                    "wrong materialized row width was admitted");
  auto bad_state = row;
  bad_state[2].setState(api::EngineValueState::unknown);
  passed &= Refuses(dag, 7, binding, bad_state,
                    "non-value row state was admitted");
  auto null_payload = NullSumRow(dag, 2);
  null_payload[1].encoded_value = "0";
  passed &= Refuses(dag, 7, binding, null_payload,
                    "SQL NULL payload substitute was admitted");
  auto wrong_type = row;
  wrong_type[0].descriptor.canonical_type_name = "boolean";
  passed &= Refuses(dag, 7, binding, wrong_type,
                    "wrong aggregate slot type was admitted");
  for (std::size_t ordinal = 0; ordinal < row.size(); ++ordinal) {
    auto malformed = row;
    malformed[ordinal].binary_value.pop_back();
    passed &= Refuses(dag, 7, binding, malformed, "truncated native row value was admitted");
    malformed = row;
    malformed[ordinal].encoded_value = "7";
    passed &= Refuses(dag, 7, binding, malformed, "mixed native/text row carrier was admitted");
  }
  return passed;
}

bool ValidateFullDescriptorIdentity() {
  const auto dag = Dag();
  const auto binding = BooleanBinding();
  const auto row = Row(dag, 2, "7");
  bool passed = true;
  const auto mutate = [&](const std::string_view from,
                          const std::string_view to,
                          const std::string_view detail) {
    auto changed = row;
    auto& encoded = changed[2].descriptor.encoded_descriptor;
    const auto offset = encoded.find(from);
    if (offset == std::string::npos) return Require(false, detail);
    encoded.replace(offset, from.size(), to);
    return Refuses(dag, 7, binding, changed, detail);
  };

  auto changed = row;
  changed[2].descriptor.descriptor_uuid = scratchbird::tests::FixtureUuidLiteral("019f3300-0000-7100-8000-000000000999");
  passed &= Refuses(dag, 7, binding, changed,
                    "descriptor UUID drift was admitted");
  changed = row;
  changed[2].descriptor.descriptor_kind = "tuple";
  passed &= Refuses(dag, 7, binding, changed,
                    "descriptor kind drift was admitted");
  changed = row;
  changed[2].descriptor.type_uuid = scratchbird::tests::FixtureUuid(2011, 1);
  passed &= Refuses(dag, 7, binding, changed,
                    "binary type UUID drift was admitted");
  passed &= mutate("nullability=nullable", "nullability=non_null",
                   "encoded nullability drift was admitted");
  changed = row;
  changed[2].descriptor.collation_uuid = scratchbird::tests::FixtureUuid(2011, 2);
  passed &= Refuses(dag, 7, binding, changed,
                    "binary collation UUID drift was admitted");
  for (const auto modifier : {"timezone_profile_id=tz-profile-drift", "width=63", "precision=18", "scale=2"}) {
    changed = row;
    changed[2].descriptor.encoded_descriptor += std::string(";") + modifier;
    passed &= Refuses(dag, 7, binding, changed, "unsupported integer descriptor modifier was admitted");
  }
  for (const auto source : {"BIGINT", "int64"}) {
    changed = row;
    changed[0].descriptor.encoded_descriptor += std::string(";source_type=") + source;
    passed &= Evaluate(dag, 7, binding, changed, api::EngineSqlTruthValue::true_value);
  }
  for (const auto suffix : {";source_type=INTEGER", ";source_type=",
                             ";source_type=unknown", ";source_type=BIGINT;source_type=int64",
                             ";source_type=BIGINT;unrecognized=1"}) {
    changed = row;
    changed[0].descriptor.encoded_descriptor += suffix;
    passed &= Refuses(dag, 7, binding, changed,
                       "invalid DDL source-type annotation was admitted");
  }
  return passed;
}

bool ValidateMalformedGraphRefusals() {
  const auto binding = BooleanBinding();
  bool passed = true;

  auto malformed = Dag();
  passed &= Refuses(malformed, 999, binding, Row(malformed, 2, "7"),
                    "missing predicate root was admitted");
  malformed = Dag();
  FindExpression(malformed, 7).child_expression_ids[0] = 999;
  passed &= Refuses(malformed, 7, binding, Row(malformed, 2, "7"),
                    "dangling predicate child was admitted");
  malformed = Dag();
  FindExpression(malformed, 10).child_expression_ids.clear();
  passed &= Refuses(malformed, 10, binding, Row(malformed, 2, "7"),
                    "empty unary child list was admitted");
  malformed = Dag();
  FindExpression(malformed, 7).child_expression_ids.push_back(6);
  passed &= Refuses(malformed, 7, binding, Row(malformed, 2, "7"),
                    "extra binary child was admitted");
  malformed = Dag();
  FindExpression(malformed, 7).operator_name.reset();
  passed &= Refuses(malformed, 7, binding, Row(malformed, 2, "7"),
                    "missing binary operator was admitted");
  malformed = Dag();
  FindExpression(malformed, 7).operator_name = "NOR";
  passed &= Refuses(malformed, 7, binding, Row(malformed, 2, "7"),
                    "unsupported boolean operator was admitted");
  malformed = Dag();
  FindExpression(malformed, 7).child_expression_ids[0] = 3;
  passed &= Refuses(malformed, 7, binding, Row(malformed, 2, "7"),
                    "wrong logical child type was admitted");
  malformed = Dag();
  FindExpression(malformed, 3).child_expression_ids.push_back(1);
  passed &= Refuses(malformed, 7, binding, Row(malformed, 2, "7"),
                    "literal carrying a child was admitted");
  malformed = Dag();
  FindExpression(malformed, 3).literal_typed_value_v1.reset();
  passed &= Refuses(malformed, 7, binding, Row(malformed, 2, "7"),
                    "literal missing its payload was admitted");
  for (unsigned mutation = 0;
       mutation < std::size(scratchbird::tests::kInvalidInt64LiteralCases); ++mutation) {
    malformed = Dag();
    scratchbird::tests::InvalidateInt64Literal(mutation, malformed.descriptors[2],
                                              FindExpression(malformed, 3));
    passed &= Refuses(malformed, 7, binding, Row(malformed, 2, "7"),
                      scratchbird::tests::kInvalidInt64LiteralCases[mutation]);
  }
  return passed;
}

}  // namespace

int main() {
  bool passed = true;
  passed &= ValidateFiveProfilesAndThreeValuedLogic();
  passed &= ValidateBatchRuntimeReuse();
  passed &= ValidateBindingRefusals();
  passed &= ValidateFullDescriptorIdentity();
  passed &= ValidateMalformedGraphRefusals();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
