// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_execution_session.hpp"
#include "firebird_scalar_projection.hpp"
#include "firebird_worker_session.hpp"

#include <cctype>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using scratchbird::parser::firebird::EncodeFirebirdScalarProjectionEnvelope;
using scratchbird::parser::firebird::DecodeFirebirdScalarProjectionRows;
using scratchbird::parser::firebird::
    DescribeFirebirdScalarProjectionWireDescriptors;
using scratchbird::parser::firebird::FirebirdScalarProjectionBindOptions;
using scratchbird::parser::firebird::FirebirdScalarProjectionExpressionKind;
using scratchbird::parser::firebird::FirebirdScalarProjectionOutputKind;
using scratchbird::parser::firebird::FirebirdScalarProjectionRoute;
using scratchbird::parser::firebird::FirebirdScalarProjectionWireCell;
using scratchbird::parser::firebird::FirebirdScalarProjectionWireRow;
using scratchbird::parser::firebird::ParseFirebirdScalarProjectionRoute;
using scratchbird::parser::firebird::
    ValidateFirebirdScalarProjectionCompletePacket;

bool Contains(std::string_view text, std::string_view expected) {
  return text.find(expected) != std::string_view::npos;
}

bool Expect(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

FirebirdScalarProjectionRoute Parse(std::string_view sql,
                                    std::string attachment_charset = {}) {
  return ParseFirebirdScalarProjectionRoute(
      sql, FirebirdScalarProjectionBindOptions{std::move(attachment_charset)});
}

bool ExpectAsciiCharRoute() {
  const auto route =
      Parse("select ASCII_CHAR( 065 ) from rdb$database;");
  bool ok = true;
  ok = Expect(route.recognized() && route.items.size() == 1,
              "ASCII_CHAR scalar route was not recognized") && ok;
  if (!route.recognized() || route.items.size() != 1) return false;
  const auto& item = route.items.front();
  ok = Expect(item.result_name == "ASCII_CHAR" &&
                  item.output_kind ==
                      FirebirdScalarProjectionOutputKind::kBinaryOctet &&
                  !item.nullable,
              "ASCII_CHAR Firebird output metadata is not exact") && ok;
  ok = Expect(item.expression.kind ==
                  FirebirdScalarProjectionExpressionKind::kFunction &&
                  item.expression.type_name == "binary" &&
                  item.expression.encoded_value.empty() &&
                  item.expression.function_id ==
                      "data.scalar.octet_from_int64" &&
                  item.expression.arguments.size() == 1,
              "ASCII_CHAR was not bound to the neutral engine function") && ok;
  if (item.expression.arguments.size() == 1) {
    const auto& argument = item.expression.arguments.front();
    ok = Expect(argument.kind ==
                    FirebirdScalarProjectionExpressionKind::kLiteral &&
                    argument.type_name == "int64" &&
                    argument.encoded_value == "065" && !argument.is_null,
                "ASCII_CHAR integer input was not serialized as a typed literal") && ok;
  }

  const std::string envelope =
      EncodeFirebirdScalarProjectionEnvelope(route);
  ok = Expect(Contains(envelope,
                       "\"operation_id\":\"query.evaluate_projection\"") &&
                  Contains(envelope,
                           "\"opcode\":\"SBLR_QUERY_EVALUATE_PROJECTION\"") &&
                  Contains(envelope, "\"projection_count\":\"1\"") &&
                  Contains(envelope, "\"projection_0_name\":\"c0\"") &&
                  Contains(envelope,
                           "\"projection_0_function_id\":"
                           "\"data.scalar.octet_from_int64\"") &&
                  Contains(envelope,
                           "\"projection_0_arg_0_value\":\"065\"") &&
                  Contains(envelope,
                           "\"scalar_projection_parser_executes_sql\":false") &&
                  Contains(envelope, "\"contains_sql_text\":false"),
              "ASCII_CHAR neutral projection envelope is incomplete") && ok;
  ok = Expect(!Contains(envelope, "\"projection_0_value\":\"A\"") &&
                  !Contains(envelope, "RDB$DATABASE") &&
                  !Contains(envelope, "ASCII_CHAR"),
              "ASCII_CHAR parser envelope evaluated or leaked source SQL") && ok;
  return ok;
}

bool ExpectAbsAndPiQaRoutes() {
  const auto abs_route =
      Parse("select ABS( -1 ) from rdb$database;");
  const auto pi_route =
      Parse("select PI() from rdb$database;");
  bool ok = true;

  ok = Expect(abs_route.recognized() && abs_route.items.size() == 1,
              "ABS(-1) scalar route was not recognized") && ok;
  if (abs_route.recognized() && abs_route.items.size() == 1) {
    const auto& item = abs_route.items.front();
    ok = Expect(item.result_name == "ABS" &&
                    item.output_kind ==
                        FirebirdScalarProjectionOutputKind::kInt64 &&
                    !item.nullable,
                "ABS(-1) Firebird INT64 output metadata is not exact") && ok;
    ok = Expect(item.expression.kind ==
                    FirebirdScalarProjectionExpressionKind::kFunction &&
                    item.expression.type_name == "int64" &&
                    item.expression.function_id == "sb.scalar.abs" &&
                    item.expression.arguments.size() == 1,
                "ABS(-1) was not bound to the neutral engine function") && ok;
    if (item.expression.arguments.size() == 1) {
      const auto& argument = item.expression.arguments.front();
      ok = Expect(argument.kind ==
                      FirebirdScalarProjectionExpressionKind::kLiteral &&
                      argument.type_name == "int64" &&
                      argument.encoded_value == "-1" && !argument.is_null,
                  "ABS(-1) input was not serialized as an int64 literal") && ok;
    }
    const std::string envelope =
        EncodeFirebirdScalarProjectionEnvelope(abs_route);
    ok = Expect(Contains(envelope,
                         "\"projection_0_function_id\":\"sb.scalar.abs\"") &&
                    Contains(envelope,
                             "\"projection_0_arg_0_value\":\"-1\"") &&
                    !Contains(envelope, "RDB$DATABASE") &&
                    !Contains(envelope, "ABS("),
                "ABS(-1) neutral envelope evaluated or leaked source SQL") && ok;
  }

  ok = Expect(pi_route.recognized() && pi_route.items.size() == 1,
              "PI() scalar route was not recognized") && ok;
  if (pi_route.recognized() && pi_route.items.size() == 1) {
    const auto& item = pi_route.items.front();
    ok = Expect(item.result_name == "PI" &&
                    item.output_kind ==
                        FirebirdScalarProjectionOutputKind::kReal64 &&
                    !item.nullable,
                "PI() Firebird DOUBLE output metadata is not exact") && ok;
    ok = Expect(item.expression.kind ==
                    FirebirdScalarProjectionExpressionKind::kFunction &&
                    item.expression.type_name == "real64" &&
                    item.expression.function_id == "sb.scalar.pi" &&
                    item.expression.arguments.empty(),
                "PI() was not bound to the zero-argument neutral function") && ok;
    const std::string envelope =
        EncodeFirebirdScalarProjectionEnvelope(pi_route);
    ok = Expect(Contains(envelope,
                         "\"projection_0_function_id\":\"sb.scalar.pi\"") &&
                    Contains(envelope,
                             "\"projection_0_function_arg_count\":\"0\"") &&
                    !Contains(envelope, "3.141592653589793") &&
                    !Contains(envelope, "RDB$DATABASE"),
                "PI() neutral envelope evaluated or leaked source SQL") && ok;
  }

  const auto aliased = Parse(
      "select abs(-1) as magnitude, pi() circle from rdb$database");
  ok = Expect(aliased.recognized() && aliased.items.size() == 2,
              "aliased ABS/PI projection route was not recognized") && ok;
  if (aliased.recognized() && aliased.items.size() == 2) {
    ok = Expect(aliased.items[0].result_name == "MAGNITUDE" &&
                    aliased.items[1].result_name == "CIRCLE" &&
                    aliased.items[0].output_kind ==
                        FirebirdScalarProjectionOutputKind::kInt64 &&
                    aliased.items[1].output_kind ==
                        FirebirdScalarProjectionOutputKind::kReal64,
                "ABS/PI aliases or typed output metadata were not preserved") && ok;
  }
  return ok;
}

bool ExpectDirectDoubleQaRoutes() {
  struct Case {
    std::string_view sql;
    std::string_view result_name;
    std::string_view function_id;
    std::vector<std::string_view> arguments;
  };
  const std::vector<Case> cases{
      {"select asin( 1 ) from rdb$database;", "ASIN", "sb.scalar.asin",
       {"1"}},
      {"select ATAN2( 1, 1) from rdb$database;", "ATAN2",
       "sb.scalar.atan2", {"1", "1"}},
      {"select ATAN( 1 ) from rdb$database;", "ATAN", "sb.scalar.atan",
       {"1"}},
      {"select COS( 14) from rdb$database;", "COS", "sb.scalar.cos",
       {"14"}},
      {"select COS( 0) from rdb$database;", "COS", "sb.scalar.cos",
       {"0"}},
      {"select COSH( 1) from rdb$database;", "COSH", "sb.scalar.cosh",
       {"1"}},
      {"select COSH( 0) from rdb$database;", "COSH", "sb.scalar.cosh",
       {"0"}},
      {"select EXP(3) from rdb$database;", "EXP", "sb.scalar.exp", {"3"}},
      {"select ln(5) from rdb$database;", "LN", "sb.scalar.ln", {"5"}},
      {"select log10(6) from rdb$database;", "LOG10", "sb.scalar.log10",
       {"6"}},
      {"select log(6, 10) from rdb$database;", "LOG", "sb.scalar.log",
       {"6", "10"}},
      {"select power(2, 3) from rdb$database;", "POWER",
       "sb.scalar.power", {"2", "3"}},
      {"select SINH(4) from rdb$database;", "SINH", "sb.scalar.sinh",
       {"4"}},
      {"select SQRT(4) from rdb$database;", "SQRT", "sb.scalar.sqrt",
       {"4"}},
      {"select TAN(43) from rdb$database;", "TAN", "sb.scalar.tan",
       {"43"}},
      {"select TANH(5) from rdb$database;", "TANH", "sb.scalar.tanh",
       {"5"}},
  };

  bool ok = true;
  for (const auto& test : cases) {
    const auto route = Parse(test.sql);
    ok = Expect(route.recognized() && route.items.size() == 1,
                std::string("direct DOUBLE route rejected: ") +
                    std::string(test.sql)) && ok;
    if (!route.recognized() || route.items.size() != 1) continue;
    const auto& item = route.items.front();
    ok = Expect(item.result_name == test.result_name &&
                    item.output_kind ==
                        FirebirdScalarProjectionOutputKind::kReal64 &&
                    !item.nullable,
                std::string("direct DOUBLE metadata mismatch: ") +
                    std::string(test.sql)) && ok;
    ok = Expect(item.expression.kind ==
                    FirebirdScalarProjectionExpressionKind::kFunction &&
                    item.expression.type_name == "real64" &&
                    item.expression.encoded_value.empty() &&
                    item.expression.function_id == test.function_id &&
                    item.expression.arguments.size() == test.arguments.size(),
                std::string("direct DOUBLE neutral binding mismatch: ") +
                    std::string(test.sql)) && ok;
    if (item.expression.arguments.size() != test.arguments.size()) continue;
    for (std::size_t index = 0; index < test.arguments.size(); ++index) {
      const auto& argument = item.expression.arguments[index];
      ok = Expect(argument.kind ==
                      FirebirdScalarProjectionExpressionKind::kLiteral &&
                      argument.type_name == "real64" &&
                      argument.encoded_value == test.arguments[index] &&
                      !argument.is_null,
                  std::string("direct DOUBLE literal binding mismatch: ") +
                      std::string(test.sql)) && ok;
    }

    const std::string envelope =
        EncodeFirebirdScalarProjectionEnvelope(route);
    ok = Expect(Contains(envelope,
                         "\"projection_0_function_id\":\"" +
                             std::string(test.function_id) + "\"") &&
                    Contains(envelope,
                             "\"projection_0_function_arg_count\":\"" +
                                 std::to_string(test.arguments.size()) +
                                 "\"") &&
                    Contains(envelope, "\"projection_0_value\":\"\"") &&
                    !Contains(envelope, "RDB$DATABASE"),
                std::string("direct DOUBLE envelope mismatch: ") +
                    std::string(test.sql)) && ok;
  }

  const auto literals_and_aliases = Parse(
      "select atan(-1.25e+1) as angle, power(0.5, 2.0) \"Pow\" "
      "from rdb$database");
  ok = Expect(literals_and_aliases.recognized() &&
                  literals_and_aliases.items.size() == 2,
              "real literal or alias direct DOUBLE route was rejected") && ok;
  if (literals_and_aliases.recognized() &&
      literals_and_aliases.items.size() == 2 &&
      literals_and_aliases.items[0].expression.arguments.size() == 1 &&
      literals_and_aliases.items[1].expression.arguments.size() == 2) {
    const auto& atan = literals_and_aliases.items[0];
    const auto& power = literals_and_aliases.items[1];
    ok = Expect(atan.result_name == "ANGLE" && power.result_name == "Pow" &&
                    atan.expression.arguments[0].encoded_value == "-1.25e+1" &&
                    power.expression.arguments[0].encoded_value == "0.5" &&
                    power.expression.arguments[1].encoded_value == "2.0",
                "direct DOUBLE aliases or real literal spellings changed") && ok;
  } else if (literals_and_aliases.recognized()) {
    ok = Expect(false,
                "direct DOUBLE real literal argument arity changed") && ok;
  }
  return ok;
}

bool ExpectBoundNumericQaRoutes() {
  struct Argument {
    std::string_view type_name;
    std::string_view encoded_value;
  };
  struct Case {
    std::string_view sql;
    std::string_view result_name;
    std::string_view function_id;
    std::string_view neutral_result_type;
    FirebirdScalarProjectionOutputKind output_kind;
    std::int16_t scale;
    std::vector<Argument> arguments;
  };
  const std::vector<Case> cases{
      {"select MAXVALUE(54, 87, 10) from rdb$database;", "MAXVALUE",
       "sb.scalar.greatest", "int64",
       FirebirdScalarProjectionOutputKind::kInt32, 0,
       {{"int64", "54"}, {"int64", "87"}, {"int64", "10"}}},
      {"select MINVALUE(9, 7, 10) from rdb$database;", "MINVALUE",
       "sb.scalar.least", "int64",
       FirebirdScalarProjectionOutputKind::kInt32, 0,
       {{"int64", "9"}, {"int64", "7"}, {"int64", "10"}}},
      {"select MOD(11, 10) from rdb$database;", "MOD", "sb.scalar.mod",
       "int64", FirebirdScalarProjectionOutputKind::kInt32, 0,
       {{"int64", "11"}, {"int64", "10"}}},
      {"select SIGN(-9) from rdb$database;", "SIGN", "sb.scalar.sign",
       "int64", FirebirdScalarProjectionOutputKind::kInt16, 0,
       {{"int64", "-9"}}},
      {"select SIGN(8) from rdb$database;", "SIGN", "sb.scalar.sign",
       "int64", FirebirdScalarProjectionOutputKind::kInt16, 0,
       {{"int64", "8"}}},
      {"select SIGN(0) from rdb$database;", "SIGN", "sb.scalar.sign",
       "int64", FirebirdScalarProjectionOutputKind::kInt16, 0,
       {{"int64", "0"}}},
      {"select CEIL(2.1) from rdb$database;", "CEIL", "sb.scalar.ceil",
       "real64", FirebirdScalarProjectionOutputKind::kInt64, 0,
       {{"real64", "2.1"}}},
      {"select CEIL(-2.1) from rdb$database;", "CEIL", "sb.scalar.ceil",
       "real64", FirebirdScalarProjectionOutputKind::kInt64, 0,
       {{"real64", "-2.1"}}},
      {"select CEILING(2.1) from rdb$database;", "CEILING",
       "sb.scalar.ceil", "real64",
       FirebirdScalarProjectionOutputKind::kInt64, 0,
       {{"real64", "2.1"}}},
      {"select FLOOR(2.1) from rdb$database;", "FLOOR", "sb.scalar.floor",
       "real64", FirebirdScalarProjectionOutputKind::kInt64, 0,
       {{"real64", "2.1"}}},
      {"select FLOOR(-4.4) from rdb$database;", "FLOOR",
       "sb.scalar.floor", "real64",
       FirebirdScalarProjectionOutputKind::kInt64, 0,
       {{"real64", "-4.4"}}},
      {"select ROUND(5.7778, 3) from rdb$database;", "ROUND",
       "sb.scalar.round", "numeric.fixed",
       FirebirdScalarProjectionOutputKind::kExactInt64, -4,
       {{"numeric.fixed", "5.7778"}, {"int64", "3"}}},
      {"select TRUNC(-2.8) from rdb$database;", "TRUNC",
       "sb.scalar.trunc", "real64",
       FirebirdScalarProjectionOutputKind::kInt64, 0,
       {{"real64", "-2.8"}}},
      {"select TRUNC(2.8) from rdb$database;", "TRUNC", "sb.scalar.trunc",
       "real64", FirebirdScalarProjectionOutputKind::kInt64, 0,
       {{"real64", "2.8"}}},
      {"select TRUNC(987.65, 1) from rdb$database;", "TRUNC",
       "sb.scalar.trunc", "real64",
       FirebirdScalarProjectionOutputKind::kInt64, -2,
       {{"real64", "987.65"}, {"int64", "1"}}},
      {"select TRUNC(987.65, -1) from rdb$database;", "TRUNC",
       "sb.scalar.trunc", "real64",
       FirebirdScalarProjectionOutputKind::kInt64, -2,
       {{"real64", "987.65"}, {"int64", "-1"}}},
  };

  bool ok = true;
  for (const auto& test : cases) {
    const auto route = Parse(test.sql);
    ok = Expect(route.recognized() && route.items.size() == 1,
                std::string("bound numeric route rejected: ") +
                    std::string(test.sql)) && ok;
    if (!route.recognized() || route.items.size() != 1) continue;
    const auto& item = route.items.front();
    ok = Expect(item.result_name == test.result_name &&
                    item.output_kind == test.output_kind &&
                    item.scale == test.scale && item.subtype == 0 &&
                    !item.nullable,
                std::string("bound numeric SQLDA metadata mismatch: ") +
                    std::string(test.sql)) && ok;
    ok = Expect(item.expression.kind ==
                    FirebirdScalarProjectionExpressionKind::kFunction &&
                    item.expression.type_name == test.neutral_result_type &&
                    item.expression.encoded_value.empty() &&
                    item.expression.function_id == test.function_id &&
                    item.expression.arguments.size() == test.arguments.size(),
                std::string("bound numeric neutral binding mismatch: ") +
                    std::string(test.sql)) && ok;
    if (item.expression.arguments.size() != test.arguments.size()) continue;
    for (std::size_t index = 0; index < test.arguments.size(); ++index) {
      const auto& actual = item.expression.arguments[index];
      const auto& expected = test.arguments[index];
      ok = Expect(actual.kind ==
                      FirebirdScalarProjectionExpressionKind::kLiteral &&
                      actual.type_name == expected.type_name &&
                      actual.encoded_value == expected.encoded_value &&
                      !actual.is_null,
                  std::string("bound numeric literal mismatch: ") +
                      std::string(test.sql)) && ok;
    }
    const std::string envelope =
        EncodeFirebirdScalarProjectionEnvelope(route);
    ok = Expect(Contains(envelope,
                         "\"projection_0_function_id\":\"" +
                             std::string(test.function_id) + "\"") &&
                    Contains(envelope,
                             "\"projection_0_function_arg_count\":\"" +
                                 std::to_string(test.arguments.size()) +
                                 "\"") &&
                    Contains(envelope, "\"projection_0_value\":\"\"") &&
                    !Contains(envelope, "RDB$DATABASE"),
                std::string("bound numeric envelope mismatch: ") +
                    std::string(test.sql)) && ok;
  }

  const auto aliases = Parse(
      "select maxvalue(54,87,10) as mx, ceiling(-2.1) \"Top\", "
      "round(5.7778,3) rounded from rdb$database");
  ok = Expect(aliases.recognized() && aliases.items.size() == 3,
              "bound numeric aliases were rejected") && ok;
  if (aliases.recognized() && aliases.items.size() == 3) {
    ok = Expect(aliases.items[0].result_name == "MX" &&
                    aliases.items[1].result_name == "Top" &&
                    aliases.items[2].result_name == "ROUNDED" &&
                    aliases.items[0].scale == 0 &&
                    aliases.items[1].scale == 0 &&
                    aliases.items[2].scale == -4,
                "bound numeric aliases or scales were not preserved") && ok;
  }

  const auto int32_boundaries = Parse(
      "select mod(2147483647, -2147483648) from rdb$database");
  ok = Expect(int32_boundaries.recognized() &&
                  int32_boundaries.items.size() == 1 &&
                  int32_boundaries.items.front().output_kind ==
                      FirebirdScalarProjectionOutputKind::kInt32,
              "int32 boundary literals were not admitted exactly") && ok;
  return ok;
}

bool ExpectBoundBinaryQaRoutes() {
  struct Case {
    std::string_view surface_name;
    std::string_view function_id;
    std::string_view left;
    std::string_view right;
    std::string_view expected_result;
    FirebirdScalarProjectionOutputKind output_kind;
    std::uint32_t sql_type;
    std::uint32_t length;
  };
  const std::vector<Case> cases{
      {"BIN_AND", "sb.scalar.bit_and", "1", "1", "1",
       FirebirdScalarProjectionOutputKind::kInt32, 496, 4},
      {"BIN_AND", "sb.scalar.bit_and", "1", "0", "0",
       FirebirdScalarProjectionOutputKind::kInt32, 496, 4},
      {"BIN_OR", "sb.scalar.bit_or", "1", "1", "1",
       FirebirdScalarProjectionOutputKind::kInt32, 496, 4},
      {"BIN_OR", "sb.scalar.bit_or", "1", "0", "1",
       FirebirdScalarProjectionOutputKind::kInt32, 496, 4},
      {"BIN_OR", "sb.scalar.bit_or", "0", "0", "0",
       FirebirdScalarProjectionOutputKind::kInt32, 496, 4},
      {"BIN_SHL", "sb.scalar.bit_shift_left", "8", "1", "16",
       FirebirdScalarProjectionOutputKind::kInt64, 580, 8},
      {"BIN_SHR", "sb.scalar.bit_shift_right", "8", "1", "4",
       FirebirdScalarProjectionOutputKind::kInt64, 580, 8},
      {"BIN_XOR", "sb.scalar.bit_xor", "0", "1", "1",
       FirebirdScalarProjectionOutputKind::kInt32, 496, 4},
      {"BIN_XOR", "sb.scalar.bit_xor", "0", "0", "0",
       FirebirdScalarProjectionOutputKind::kInt32, 496, 4},
      {"BIN_XOR", "sb.scalar.bit_xor", "1", "1", "0",
       FirebirdScalarProjectionOutputKind::kInt32, 496, 4},
  };

  bool ok = Expect(cases.size() == 10,
                   "canonical binary statement inventory drifted");
  for (const auto& test : cases) {
    const std::string sql =
        "select " + std::string(test.surface_name) + "( " +
        std::string(test.left) + ", " + std::string(test.right) +
        ") from rdb$database";
    const auto route = Parse(sql);
    ok = Expect(route.recognized() && route.items.size() == 1,
                std::string("canonical binary route rejected: ") + sql) &&
         ok;
    if (!route.recognized() || route.items.size() != 1) continue;
    const auto& item = route.items.front();
    const auto& expression = item.expression;
    ok = Expect(item.result_name == test.surface_name &&
                    item.output_kind == test.output_kind && item.scale == 0 &&
                    item.subtype == 0 && !item.nullable &&
                    expression.kind ==
                        FirebirdScalarProjectionExpressionKind::kFunction &&
                    expression.type_name == "int64" &&
                    expression.encoded_value.empty() &&
                    expression.function_id == test.function_id &&
                    expression.arguments.size() == 2,
                "binary bind/result metadata drifted") && ok;
    if (expression.arguments.size() == 2) {
      ok = Expect(expression.arguments[0].kind ==
                          FirebirdScalarProjectionExpressionKind::kLiteral &&
                      expression.arguments[0].type_name == "int64" &&
                      expression.arguments[0].encoded_value == test.left &&
                      !expression.arguments[0].is_null &&
                      expression.arguments[1].kind ==
                          FirebirdScalarProjectionExpressionKind::kLiteral &&
                      expression.arguments[1].type_name == "int64" &&
                      expression.arguments[1].encoded_value == test.right &&
                      !expression.arguments[1].is_null,
                  "binary typed int64 literal lowering drifted") && ok;
    }

    const auto descriptors =
        DescribeFirebirdScalarProjectionWireDescriptors(route);
    ok = Expect(descriptors.size() == 1 &&
                    descriptors[0].name == test.surface_name &&
                    descriptors[0].source_name == "c0" &&
                    descriptors[0].relation.empty() &&
                    descriptors[0].owner.empty() &&
                    descriptors[0].sql_type == test.sql_type &&
                    descriptors[0].length == test.length &&
                    descriptors[0].scale == 0 &&
                    descriptors[0].subtype == 0 &&
                    !descriptors[0].nullable,
                "binary SQLDA descriptor drifted") && ok;

    const std::string envelope =
        EncodeFirebirdScalarProjectionEnvelope(route);
    ok = Expect(Contains(envelope,
                         "\"projection_0_function_id\":\"" +
                             std::string(test.function_id) + "\"") &&
                    Contains(envelope,
                             "\"projection_0_function_arg_count\":\"2\"") &&
                    Contains(envelope,
                             "\"projection_0_arg_0_type\":\"int64\"") &&
                    Contains(envelope,
                             "\"projection_0_arg_0_value\":\"" +
                                 std::string(test.left) + "\"") &&
                    Contains(envelope,
                             "\"projection_0_arg_1_value\":\"" +
                                 std::string(test.right) + "\"") &&
                    !Contains(envelope,
                              "\"projection_0_value\":\"" +
                                  std::string(test.expected_result) + "\"") &&
                    !Contains(envelope, test.surface_name) &&
                    !Contains(envelope, "RDB$DATABASE"),
                "binary envelope evaluated or leaked source SQL") && ok;
  }

  const auto aliases = Parse(
      "select bin_and(1, 1) as masked, bin_shl(8, 1) \"Shifted\" "
      "from rdb$database");
  const auto alias_descriptors =
      DescribeFirebirdScalarProjectionWireDescriptors(aliases);
  ok = Expect(aliases.recognized() && aliases.items.size() == 2 &&
                  aliases.items[0].result_name == "MASKED" &&
                  aliases.items[1].result_name == "Shifted" &&
                  alias_descriptors.size() == 2 &&
                  alias_descriptors[0].sql_type == 496 &&
                  alias_descriptors[0].length == 4 &&
                  alias_descriptors[1].sql_type == 580 &&
                  alias_descriptors[1].length == 8,
              "binary aliases or mixed SQLDA descriptors drifted") && ok;
  return ok;
}

bool ExpectBoundStringLiteralQaRoutes() {
  struct Argument {
    std::string_view type_name;
    std::string_view encoded_value;
  };
  struct Case {
    std::string_view sql;
    std::string_view result_name;
    std::string_view function_id;
    std::string_view expected_result;
    FirebirdScalarProjectionOutputKind output_kind;
    std::uint32_t sql_type;
    std::uint32_t declared_length;
    std::vector<Argument> arguments;
  };
  const std::vector<Case> cases{
      {"select left('bonjour', 3) from rdb$database;", "LEFT",
       "sb.scalar.left", "bon",
       FirebirdScalarProjectionOutputKind::kVaryingText, 448, 7,
       {{"text", "bonjour"}, {"int64", "3"}}},
      {"select OVERLAY('il fait beau dans le sud  de la france' PLACING "
       "'NORD' FROM 22 FOR 4) from rdb$database;",
       "OVERLAY", "sb.scalar.overlay",
       "il fait beau dans le NORD de la france",
       FirebirdScalarProjectionOutputKind::kVaryingText, 448, 42,
       {{"text", "il fait beau dans le sud  de la france"},
        {"text", "NORD"}, {"int64", "22"}, {"int64", "4"}}},
      {"select position('beau' IN 'il fait beau dans le nord') "
       "from rdb$database;",
       "POSITION", "sb.scalar.position", "9",
       FirebirdScalarProjectionOutputKind::kInt32, 496, 4,
       {{"text", "beau"}, {"text", "il fait beau dans le nord"}}},
      {"select replace('toto', 'o', 'i') from rdb$database;", "REPLACE",
       "sb.scalar.replace", "titi",
       FirebirdScalarProjectionOutputKind::kVaryingText, 448, 4,
       {{"text", "toto"}, {"text", "o"}, {"text", "i"}}},
      {"select reverse('DRON') from rdb$database;", "REVERSE",
       "sb.scalar.reverse", "NORD",
       FirebirdScalarProjectionOutputKind::kVaryingText, 448, 4,
       {{"text", "DRON"}}},
      {"select right('NORD PAS DE CALAIS', 13) from rdb$database;", "RIGHT",
       "sb.scalar.right", "PAS DE CALAIS",
       FirebirdScalarProjectionOutputKind::kVaryingText, 448, 18,
       {{"text", "NORD PAS DE CALAIS"}, {"int64", "13"}}},
  };

  bool ok = Expect(cases.size() == 6,
                   "canonical single-string SELECT inventory drifted");
  for (const auto& test : cases) {
    const auto route = Parse(test.sql, "NONE");
    ok = Expect(route.recognized() && route.items.size() == 1,
                std::string("canonical string route rejected: ") +
                    std::string(test.sql)) && ok;
    if (!route.recognized() || route.items.size() != 1) continue;
    const auto& item = route.items.front();
    const auto& expression = item.expression;
    const std::uint32_t item_declared_length =
        test.output_kind == FirebirdScalarProjectionOutputKind::kVaryingText
            ? test.declared_length
            : 0;
    ok = Expect(item.result_name == test.result_name &&
                    item.output_kind == test.output_kind && item.scale == 0 &&
                    item.subtype == 0 &&
                    item.declared_length == item_declared_length &&
                    !item.nullable &&
                    expression.kind ==
                        FirebirdScalarProjectionExpressionKind::kFunction &&
                    expression.type_name ==
                        (test.output_kind ==
                                 FirebirdScalarProjectionOutputKind::kInt32
                             ? "int64"
                             : "text") &&
                    expression.encoded_value.empty() &&
                    expression.function_id == test.function_id &&
                    expression.arguments.size() == test.arguments.size(),
                "string literal bind/result metadata drifted") && ok;
    if (expression.arguments.size() == test.arguments.size()) {
      for (std::size_t index = 0; index < test.arguments.size(); ++index) {
        ok = Expect(
                 expression.arguments[index].kind ==
                         FirebirdScalarProjectionExpressionKind::kLiteral &&
                     expression.arguments[index].type_name ==
                         test.arguments[index].type_name &&
                     expression.arguments[index].encoded_value ==
                         test.arguments[index].encoded_value &&
                     !expression.arguments[index].is_null,
                 "string literal typed argument lowering drifted") && ok;
      }
    }

    const auto descriptors =
        DescribeFirebirdScalarProjectionWireDescriptors(route);
    const bool varying = test.sql_type == 448;
    ok = Expect(descriptors.size() == 1 &&
                    descriptors[0].name == test.result_name &&
                    descriptors[0].source_name == "c0" &&
                    descriptors[0].relation.empty() &&
                    descriptors[0].owner.empty() &&
                    descriptors[0].sql_type == test.sql_type &&
                    descriptors[0].length == test.declared_length &&
                    descriptors[0].scale == 0 &&
                    descriptors[0].subtype == 0 &&
                    descriptors[0].character_set ==
                        (varying ? "NONE" : "") &&
                    descriptors[0].character_length ==
                        (varying ? test.declared_length : 0) &&
                    !descriptors[0].nullable,
                "string literal Firebird SQLDA descriptor drifted") && ok;

    const std::string envelope =
        EncodeFirebirdScalarProjectionEnvelope(route);
    ok = Expect(Contains(envelope,
                         "\"projection_0_function_id\":\"" +
                             std::string(test.function_id) + "\"") &&
                    Contains(envelope,
                             "\"projection_0_function_arg_count\":\"" +
                                 std::to_string(test.arguments.size()) +
                                 "\"") &&
                    Contains(envelope, "\"projection_0_value\":\"\"") &&
                    !Contains(envelope,
                              "\"projection_0_value\":\"" +
                                  std::string(test.expected_result) + "\"") &&
                    !Contains(envelope, "RDB$DATABASE") &&
                    !Contains(envelope, "SELECT "),
                "string literal envelope evaluated or leaked source SQL") &&
         ok;
  }

  const auto position_pair = Parse(
      "SELECT POSITION('beau','beau,il fait beau') C1, "
      "POSITION('beau','beau,il fait beau',2) C2 FROM RDB$DATABASE;",
      "NONE");
  ok = Expect(position_pair.recognized() && position_pair.items.size() == 2,
              "canonical multi-column POSITION route was rejected") && ok;
  if (position_pair.recognized() && position_pair.items.size() == 2) {
    const auto& first = position_pair.items[0];
    const auto& second = position_pair.items[1];
    ok = Expect(first.result_name == "C1" && second.result_name == "C2" &&
                    first.output_kind ==
                        FirebirdScalarProjectionOutputKind::kInt32 &&
                    second.output_kind ==
                        FirebirdScalarProjectionOutputKind::kInt32 &&
                    first.declared_length == 0 &&
                    second.declared_length == 0 &&
                    first.expression.function_id == "sb.scalar.position" &&
                    first.expression.arguments.size() == 2 &&
                    first.expression.arguments[0].encoded_value == "beau" &&
                    first.expression.arguments[1].encoded_value ==
                        "beau,il fait beau" &&
                    second.expression.function_id == "sb.scalar.instr" &&
                    second.expression.arguments.size() == 3 &&
                    second.expression.arguments[0].encoded_value ==
                        "beau,il fait beau" &&
                    second.expression.arguments[1].encoded_value == "beau" &&
                    second.expression.arguments[2].type_name == "int64" &&
                    second.expression.arguments[2].encoded_value == "2",
                "POSITION aliases or INSTR argument reordering drifted") && ok;
    const auto descriptors =
        DescribeFirebirdScalarProjectionWireDescriptors(position_pair);
    ok = Expect(descriptors.size() == 2 &&
                    descriptors[0].name == "C1" &&
                    descriptors[1].name == "C2" &&
                    descriptors[0].source_name == "c0" &&
                    descriptors[1].source_name == "c1" &&
                    descriptors[0].sql_type == 496 &&
                    descriptors[1].sql_type == 496 &&
                    descriptors[0].length == 4 &&
                    descriptors[1].length == 4 &&
                    descriptors[0].scale == 0 &&
                    descriptors[1].scale == 0 &&
                    descriptors[0].subtype == 0 &&
                    descriptors[1].subtype == 0,
                "multi-column POSITION SQLDA descriptors drifted") && ok;
    const std::string envelope =
        EncodeFirebirdScalarProjectionEnvelope(position_pair);
    ok = Expect(Contains(envelope,
                         "\"projection_0_function_id\":"
                         "\"sb.scalar.position\"") &&
                    Contains(envelope,
                             "\"projection_1_function_id\":"
                             "\"sb.scalar.instr\"") &&
                    Contains(envelope,
                             "\"projection_1_arg_0_value\":"
                             "\"beau,il fait beau\"") &&
                    Contains(envelope,
                             "\"projection_1_arg_1_value\":\"beau\"") &&
                    Contains(envelope,
                             "\"projection_1_arg_2_value\":\"2\"") &&
                    !Contains(envelope, "\"projection_0_value\":\"1\"") &&
                    !Contains(envelope, "\"projection_1_value\":\"14\"") &&
                    !Contains(envelope, "RDB$DATABASE"),
                "multi-column POSITION envelope evaluated or leaked SQL") &&
         ok;
  }

  const auto aliases = Parse(
      "select left('bonjour',3) as prefix, reverse('DRON') \"Back\" "
      "from rdb$database",
      "NONE");
  const auto alias_descriptors =
      DescribeFirebirdScalarProjectionWireDescriptors(aliases);
  ok = Expect(aliases.recognized() && aliases.items.size() == 2 &&
                  aliases.items[0].result_name == "PREFIX" &&
                  aliases.items[1].result_name == "Back" &&
                  alias_descriptors.size() == 2 &&
                  alias_descriptors[0].name == "PREFIX" &&
                  alias_descriptors[0].length == 7 &&
                  alias_descriptors[1].name == "Back" &&
                  alias_descriptors[1].length == 4,
              "bounded string aliases or declared lengths drifted") && ok;

  std::size_t canonical_output_count = 0;
  for (const auto& test : cases) {
    const auto route = Parse(test.sql, "NONE");
    canonical_output_count += route.items.size();
  }
  canonical_output_count += position_pair.items.size();
  ok = Expect(cases.size() + 1 == 7 && canonical_output_count == 8,
              "canonical string tranche is not 7 SELECTs / 8 columns") && ok;
  return ok;
}

bool ExpectBoundStringLiteralWireValidation() {
  const auto text_route = Parse(
      "select left('bonjour',3), "
      "overlay('il fait beau dans le sud  de la france' placing 'NORD' "
      "from 22 for 4), replace('toto','o','i'), reverse('DRON'), "
      "right('NORD PAS DE CALAIS',13) from rdb$database",
      "NONE");
  std::vector<FirebirdScalarProjectionWireRow> text_rows{{{
      {false, "bon"},
      {false, "il fait beau dans le NORD de la france"},
      {false, "titi"},
      {false, "NORD"},
      {false, "PAS DE CALAIS"},
  }}};
  std::string diagnostic;
  bool ok = Expect(text_route.recognized() && text_route.items.size() == 5 &&
                       DecodeFirebirdScalarProjectionRows(
                           text_route, &text_rows, &diagnostic) &&
                       text_rows[0].cells[0].text == "bon" &&
                       text_rows[0].cells[1].text ==
                           "il fait beau dans le NORD de la france" &&
                       diagnostic.empty(),
                   "bounded string results were not presentation-validated") ;

  auto oversized_rows = text_rows;
  oversized_rows[0].cells[0].text = "12345678";
  diagnostic.clear();
  ok = Expect(!DecodeFirebirdScalarProjectionRows(
                  text_route, &oversized_rows, &diagnostic) &&
                  diagnostic == "bounded_varying_text_result_required",
              "oversized neutral text result was not rejected") && ok;

  const auto position_route = Parse(
      "select position('beau','beau,il fait beau') C1, "
      "position('beau','beau,il fait beau',2) C2 from rdb$database",
      "NONE");
  std::vector<FirebirdScalarProjectionWireRow> position_rows{{{
      {false, "1"}, {false, "14"},
  }}};
  diagnostic.clear();
  ok = Expect(DecodeFirebirdScalarProjectionRows(
                  position_route, &position_rows, &diagnostic) &&
                  diagnostic.empty(),
              "POSITION int32 results were not presentation-validated") && ok;
  position_rows[0].cells[1].text = "2147483648";
  diagnostic.clear();
  ok = Expect(!DecodeFirebirdScalarProjectionRows(
                  position_route, &position_rows, &diagnostic) &&
                  diagnostic == "int32_result_required",
              "out-of-range POSITION result was not rejected") && ok;
  return ok;
}

bool ExpectBoundStringLiteralShapesFailClosed() {
  const std::vector<std::string_view> unsupported{
      "select left('bonjour',2) from rdb$database",
      "select left('bonjour',03) from rdb$database",
      "select right('NORD PAS DE CALAIS',12) from rdb$database",
      "select overlay('il fait beau dans le sud  de la france' placing "
      "'NORD' from 21 for 4) from rdb$database",
      "select position('BEAU' in 'il fait beau dans le nord') "
      "from rdb$database",
      "select position('beau','beau,il fait beau',1) from rdb$database",
      "select position('beau','beau,il fait beau',2,1) from rdb$database",
      "select replace('toto','o','x') from rdb$database",
      "select reverse('NORD') from rdb$database",
      "select left('bonjour',3) from other_table",
  };
  bool ok = true;
  for (const auto sql : unsupported) {
    ok = Expect(!Parse(sql, "NONE").recognized(),
                std::string("unmeasured string shape did not fail closed: ") +
                    std::string(sql)) && ok;
  }
  ok = Expect(!Parse("select left('bonjour',3) from rdb$database",
                     "UTF8")
                   .recognized(),
              "unmeasured UTF8 string descriptor shape did not fail closed") &&
       ok;
  return ok;
}

bool ExpectBoundDecimalMathCastQaRoutes() {
  struct Case {
    std::string_view sql;
    std::string_view inner_function_id;
    std::string_view inner_value;
  };
  const std::vector<Case> cases{
      {"select cast(acos(1) as decimal(18,15)) from rdb$database;",
       "sb.scalar.acos", "1"},
      {"select CAST(COT(1) AS DECIMAL(18,15)) from rdb$database;",
       "sb.scalar.cot", "1"},
      {"select cast(sin(12) as decimal(18,15)) from rdb$database;",
       "sb.scalar.sin", "12"},
  };

  bool ok = true;
  for (const auto& test : cases) {
    const auto route = Parse(test.sql);
    ok = Expect(route.recognized() && route.items.size() == 1,
                std::string("DECIMAL math CAST route rejected: ") +
                    std::string(test.sql)) && ok;
    if (!route.recognized() || route.items.size() != 1) continue;
    const auto& item = route.items.front();
    ok = Expect(item.result_name == "CAST" &&
                    item.output_kind ==
                        FirebirdScalarProjectionOutputKind::kExactInt64 &&
                    item.scale == -15 && item.subtype == 2 &&
                    !item.nullable,
                "DECIMAL math CAST SQLDA metadata drifted") && ok;
    const auto& expression = item.expression;
    ok = Expect(expression.kind ==
                    FirebirdScalarProjectionExpressionKind::kFunction &&
                    expression.type_name == "decimal" &&
                    expression.function_id == "data.scalar.cast" &&
                    expression.arguments.size() == 3,
                "DECIMAL math CAST outer neutral binding drifted") && ok;
    if (expression.arguments.size() != 3) continue;
    const auto& inner = expression.arguments[0];
    ok = Expect(inner.kind ==
                    FirebirdScalarProjectionExpressionKind::kFunction &&
                    inner.type_name == "real64" &&
                    inner.function_id == test.inner_function_id &&
                    inner.arguments.size() == 1,
                "DECIMAL math CAST inner neutral binding drifted") && ok;
    if (inner.arguments.size() == 1) {
      ok = Expect(inner.arguments[0].kind ==
                      FirebirdScalarProjectionExpressionKind::kLiteral &&
                      inner.arguments[0].type_name == "real64" &&
                      inner.arguments[0].encoded_value == test.inner_value &&
                      !inner.arguments[0].is_null,
                  "DECIMAL math CAST inner literal drifted") && ok;
    }
    ok = Expect(expression.arguments[1].type_name == "text" &&
                    expression.arguments[1].encoded_value ==
                        "decimal(18,15)" &&
                    expression.arguments[2].type_name == "text" &&
                    expression.arguments[2].encoded_value == "half_up",
                "DECIMAL math CAST target or rounding profile drifted") && ok;
    const std::string envelope =
        EncodeFirebirdScalarProjectionEnvelope(route);
    ok = Expect(Contains(envelope,
                         "\"projection_0_function_id\":"
                         "\"data.scalar.cast\"") &&
                    Contains(envelope,
                             "\"projection_0_arg_0_function_id\":\"" +
                                 std::string(test.inner_function_id) +
                                 "\"") &&
                    Contains(envelope, "decimal(18,15)") &&
                    Contains(envelope, "half_up") &&
                    !Contains(envelope, "RDB$DATABASE") &&
                    !Contains(envelope, "0.642092615934331") &&
                    !Contains(envelope, "-0.536572918000435"),
                "DECIMAL math CAST envelope evaluated or leaked SQL") && ok;
  }

  const auto aliases = Parse(
      "select cast(acos(1) as decimal(18,15)) as a, "
      "cast(cot(1) as decimal(18,15)) \"C\", "
      "cast(sin(12) as decimal(18,15)) s from rdb$database");
  ok = Expect(aliases.recognized() && aliases.items.size() == 3 &&
                  aliases.items[0].result_name == "A" &&
                  aliases.items[1].result_name == "C" &&
                  aliases.items[2].result_name == "S",
              "DECIMAL math CAST aliases were not preserved") && ok;
  return ok;
}

bool ExpectBoundDateDiffQaRoutes() {
  struct Case {
    std::string_view sql;
    std::string_view part;
    std::string_view begin;
    std::string_view end;
  };
  const std::vector<Case> cases{
      {"select datediff(second, "
       "cast('12/02/2008 13:33:33' as timestamp), "
       "cast('12/02/2008 13:33:35' as timestamp)) from rdb$database;",
       "second", "2008-12-02T13:33:33", "2008-12-02T13:33:35"},
      {"select datediff(second from "
       "cast('12/02/2008 13:33:33' as timestamp) to "
       "cast('12/02/2008 13:33:35' as timestamp)) from rdb$database;",
       "second", "2008-12-02T13:33:33", "2008-12-02T13:33:35"},
      {"select datediff(minute, "
       "cast('12/02/2008 13:33:33' as timestamp), "
       "cast('12/02/2008 13:34:35' as timestamp)) from rdb$database;",
       "minute", "2008-12-02T13:33:33", "2008-12-02T13:34:35"},
      {"select datediff(minute from "
       "cast('12/02/2008 13:33:33' as timestamp) to "
       "cast('12/02/2008 13:34:35' as timestamp)) from rdb$database;",
       "minute", "2008-12-02T13:33:33", "2008-12-02T13:34:35"},
      {"select datediff(hour, "
       "cast('12/02/2008 13:33:33' as timestamp), "
       "cast('12/02/2008 14:34:35' as timestamp)) from rdb$database;",
       "hour", "2008-12-02T13:33:33", "2008-12-02T14:34:35"},
      {"select datediff(hour from "
       "cast('12/02/2008 13:33:33' as timestamp) to "
       "cast('12/02/2008 14:34:35' as timestamp)) from rdb$database;",
       "hour", "2008-12-02T13:33:33", "2008-12-02T14:34:35"},
      {"select datediff(year, "
       "cast('12/02/2008 13:33:33' as timestamp), "
       "cast('12/02/2009 13:34:35' as timestamp)) from rdb$database;",
       "year", "2008-12-02T13:33:33", "2009-12-02T13:34:35"},
      {"select datediff(year from "
       "cast('12/02/2008 13:33:33' as timestamp) to "
       "cast('12/02/2009 13:34:35' as timestamp)) from rdb$database;",
       "year", "2008-12-02T13:33:33", "2009-12-02T13:34:35"},
      {"select datediff(month, "
       "cast('12/02/2008 13:33:33' as timestamp), "
       "cast('12/02/2009 13:34:35' as timestamp)) from rdb$database;",
       "month", "2008-12-02T13:33:33", "2009-12-02T13:34:35"},
      {"select datediff(month from "
       "cast('12/02/2008 13:33:33' as timestamp) to "
       "cast('12/02/2009 13:34:35' as timestamp)) from rdb$database;",
       "month", "2008-12-02T13:33:33", "2009-12-02T13:34:35"},
      {"select datediff(day, "
       "cast('12/02/2008 13:33:33' as timestamp), "
       "cast('12/02/2009 13:34:35' as timestamp)) from rdb$database;",
       "day", "2008-12-02T13:33:33", "2009-12-02T13:34:35"},
      {"select datediff(day from "
       "cast('12/02/2008 13:33:33' as timestamp) to "
       "cast('12/02/2009 13:34:35' as timestamp)) from rdb$database;",
       "day", "2008-12-02T13:33:33", "2009-12-02T13:34:35"},
  };

  bool ok = true;
  for (const auto& test : cases) {
    const auto route = Parse(test.sql);
    ok = Expect(route.recognized() && route.items.size() == 1,
                std::string("DATEDIFF QA route rejected: ") +
                    std::string(test.sql)) && ok;
    if (!route.recognized() || route.items.size() != 1) continue;
    const auto& item = route.items.front();
    ok = Expect(item.result_name == "DATEDIFF" &&
                    item.output_kind ==
                        FirebirdScalarProjectionOutputKind::kInt64 &&
                    item.scale == 0 && item.subtype == 0 && !item.nullable,
                "DATEDIFF Firebird INT64 metadata drifted") && ok;
    const auto& expression = item.expression;
    ok = Expect(expression.kind ==
                    FirebirdScalarProjectionExpressionKind::kFunction &&
                    expression.type_name == "int64" &&
                    expression.encoded_value.empty() &&
                    expression.function_id == "sb.temporal.date_diff" &&
                    expression.arguments.size() == 3,
                "DATEDIFF was not bound to the neutral engine function") && ok;
    if (expression.arguments.size() == 3) {
      const auto& part = expression.arguments[0];
      const auto& begin = expression.arguments[1];
      const auto& end = expression.arguments[2];
      ok = Expect(part.kind ==
                      FirebirdScalarProjectionExpressionKind::kLiteral &&
                      part.type_name == "text" &&
                      part.encoded_value == test.part && !part.is_null &&
                      begin.kind ==
                          FirebirdScalarProjectionExpressionKind::kLiteral &&
                      begin.type_name == "timestamp" &&
                      begin.encoded_value == test.begin && !begin.is_null &&
                      end.kind ==
                          FirebirdScalarProjectionExpressionKind::kLiteral &&
                      end.type_name == "timestamp" &&
                      end.encoded_value == test.end && !end.is_null,
                  "DATEDIFF typed literal binding drifted") && ok;
    }
    const std::string envelope =
        EncodeFirebirdScalarProjectionEnvelope(route);
    ok = Expect(Contains(envelope,
                         "\"projection_0_function_id\":"
                         "\"sb.temporal.date_diff\"") &&
                    Contains(envelope,
                             "\"projection_0_function_arg_count\":\"3\"") &&
                    Contains(envelope,
                             "\"projection_0_arg_0_value\":\"" +
                                 std::string(test.part) + "\"") &&
                    Contains(envelope,
                             "\"projection_0_arg_1_value\":\"" +
                                 std::string(test.begin) + "\"") &&
                    Contains(envelope,
                             "\"projection_0_arg_2_value\":\"" +
                                 std::string(test.end) + "\"") &&
                    !Contains(envelope, "12/02/") &&
                    !Contains(envelope, "RDB$DATABASE") &&
                    !Contains(envelope, "DATEDIFF(") &&
                    !Contains(envelope, "\"projection_0_value\":\"2\"") &&
                    !Contains(envelope, "\"projection_0_value\":\"365\""),
                "DATEDIFF envelope evaluated or leaked source SQL") && ok;
  }

  const auto aliases = Parse(
      "select datediff(second from "
      "cast('12/02/2008 13:33:33' as timestamp) to "
      "cast('12/02/2008 13:33:35' as timestamp)) as seconds_between, "
      "datediff(day, cast('12/02/2008 13:33:33' as timestamp), "
      "cast('12/02/2009 13:34:35' as timestamp)) \"Elapsed Days\" "
      "from rdb$database");
  ok = Expect(aliases.recognized() && aliases.items.size() == 2 &&
                  aliases.items[0].result_name == "SECONDS_BETWEEN" &&
                  aliases.items[1].result_name == "Elapsed Days",
              "DATEDIFF aliases were not preserved") && ok;
  return ok;
}

bool ExpectBoundExtractWeekQaRoute() {
  const auto route = Parse(
      "select extract(week from date '30.12.2008'), "
      "extract(week from date '30.12.2009') from rdb$database;");
  bool ok = Expect(route.recognized() && route.items.size() == 2,
                   "two-column EXTRACT(WEEK) QA route was rejected");
  if (!route.recognized() || route.items.size() != 2) return false;

  const std::vector<std::string_view> expected_dates{
      "2008-12-30", "2009-12-30"};
  for (std::size_t index = 0; index < route.items.size(); ++index) {
    const auto& item = route.items[index];
    ok = Expect(item.result_name == "EXTRACT" &&
                    item.output_kind ==
                        FirebirdScalarProjectionOutputKind::kInt16 &&
                    item.scale == 0 && item.subtype == 0 && !item.nullable,
                "EXTRACT(WEEK) Firebird SHORT metadata drifted") && ok;
    const auto& expression = item.expression;
    ok = Expect(expression.kind ==
                    FirebirdScalarProjectionExpressionKind::kFunction &&
                    expression.type_name == "int64" &&
                    expression.encoded_value.empty() &&
                    expression.function_id == "sb.temporal.date_part" &&
                    expression.arguments.size() == 2,
                "EXTRACT(WEEK) was not bound to the neutral engine function") && ok;
    if (expression.arguments.size() == 2) {
      const auto& part = expression.arguments[0];
      const auto& date = expression.arguments[1];
      ok = Expect(part.kind ==
                      FirebirdScalarProjectionExpressionKind::kLiteral &&
                      part.type_name == "text" &&
                      part.encoded_value == "week" && !part.is_null &&
                      date.kind ==
                          FirebirdScalarProjectionExpressionKind::kLiteral &&
                      date.type_name == "date" &&
                      date.encoded_value == expected_dates[index] &&
                      !date.is_null,
                  "EXTRACT(WEEK) typed literal binding drifted") && ok;
    }
  }

  const std::string envelope =
      EncodeFirebirdScalarProjectionEnvelope(route);
  ok = Expect(Contains(envelope, "\"projection_0_name\":\"c0\"") &&
                  Contains(envelope, "\"projection_1_name\":\"c1\"") &&
                  Contains(envelope,
                           "\"projection_0_function_id\":"
                           "\"sb.temporal.date_part\"") &&
                  Contains(envelope,
                           "\"projection_1_function_id\":"
                           "\"sb.temporal.date_part\"") &&
                  Contains(envelope,
                           "\"projection_0_arg_1_value\":\"2008-12-30\"") &&
                  Contains(envelope,
                           "\"projection_1_arg_1_value\":\"2009-12-30\"") &&
                  !Contains(envelope, "30.12.") &&
                  !Contains(envelope, "RDB$DATABASE") &&
                  !Contains(envelope, "EXTRACT(") &&
                  !Contains(envelope, "\"projection_0_value\":\"1\"") &&
                  !Contains(envelope, "\"projection_1_value\":\"53\""),
              "EXTRACT(WEEK) envelope evaluated or leaked source SQL") && ok;

  const auto aliases = Parse(
      "select extract(week from date '30.12.2008') as iso_week, "
      "extract(week from date '30.12.2009') \"ISO Week\" "
      "from rdb$database");
  ok = Expect(aliases.recognized() && aliases.items.size() == 2 &&
                  aliases.items[0].result_name == "ISO_WEEK" &&
                  aliases.items[1].result_name == "ISO Week",
              "EXTRACT(WEEK) aliases were not preserved") && ok;
  return ok;
}

bool ExpectBoundDateAddQaRoutes() {
  struct Case {
    std::string_view sql;
    std::string_view result_type;
    std::string_view input_value;
    std::string_view interval;
    FirebirdScalarProjectionOutputKind output_kind;
    std::uint32_t sql_type;
    std::uint32_t length;
  };
  const std::vector<Case> cases{
      {"select dateadd(-1 day TO date '2008-02-06' ) as yesterday "
       "from rdb$database;",
       "date", "2008-02-06", "-P1D",
       FirebirdScalarProjectionOutputKind::kDate, 570, 4},
      {"select dateadd(day,-1, date '2008-02-06' ) as yesterday "
       "from rdb$database;",
       "date", "2008-02-06", "-P1D",
       FirebirdScalarProjectionOutputKind::kDate, 570, 4},
      {"select dateadd(-1 year TO date '2008-02-06' ) as yesterday "
       "from rdb$database;",
       "date", "2008-02-06", "-P1Y",
       FirebirdScalarProjectionOutputKind::kDate, 570, 4},
      {"select dateadd(year,-1, date '2008-02-06' ) as yesterday "
       "from rdb$database;",
       "date", "2008-02-06", "-P1Y",
       FirebirdScalarProjectionOutputKind::kDate, 570, 4},
      {"select dateadd(-1 day TO timestamp '2008-02-06 10:10:00' ) "
       "as yesterday from rdb$database;",
       "timestamp", "2008-02-06T10:10:00", "-P1D",
       FirebirdScalarProjectionOutputKind::kTimestamp, 510, 8},
      {"select dateadd(day,-1, timestamp '2008-02-06 10:10:00' ) "
       "as yesterday from rdb$database;",
       "timestamp", "2008-02-06T10:10:00", "-P1D",
       FirebirdScalarProjectionOutputKind::kTimestamp, 510, 8},
  };

  bool ok = true;
  for (const auto& test : cases) {
    const auto route = Parse(test.sql);
    ok = Expect(route.recognized() && route.items.size() == 1,
                std::string("DATEADD QA route rejected: ") +
                    std::string(test.sql)) &&
         ok;
    if (!route.recognized() || route.items.size() != 1) continue;
    const auto& item = route.items.front();
    ok = Expect(item.result_name == "YESTERDAY" &&
                    item.output_kind == test.output_kind && item.scale == 0 &&
                    item.subtype == 0 && !item.nullable,
                "DATEADD Firebird result metadata drifted") &&
         ok;
    const auto& expression = item.expression;
    ok = Expect(expression.kind ==
                    FirebirdScalarProjectionExpressionKind::kFunction &&
                    expression.type_name == test.result_type &&
                    expression.encoded_value.empty() &&
                    expression.function_id == "sb.temporal.date_add" &&
                    expression.arguments.size() == 2,
                "DATEADD was not bound to the neutral engine function") &&
         ok;
    if (expression.arguments.size() == 2) {
      const auto& temporal = expression.arguments[0];
      const auto& interval = expression.arguments[1];
      ok = Expect(temporal.kind ==
                          FirebirdScalarProjectionExpressionKind::kLiteral &&
                      temporal.type_name == test.result_type &&
                      temporal.encoded_value == test.input_value &&
                      !temporal.is_null &&
                      interval.kind ==
                          FirebirdScalarProjectionExpressionKind::kLiteral &&
                      interval.type_name == "interval" &&
                      interval.encoded_value == test.interval &&
                      !interval.is_null,
                  "DATEADD typed literal or interval lowering drifted") &&
           ok;
    }

    const auto descriptors =
        DescribeFirebirdScalarProjectionWireDescriptors(route);
    ok = Expect(descriptors.size() == 1 &&
                    descriptors.front().name == "YESTERDAY" &&
                    descriptors.front().source_name == "c0" &&
                    descriptors.front().relation.empty() &&
                    descriptors.front().owner.empty() &&
                    descriptors.front().sql_type == test.sql_type &&
                    descriptors.front().length == test.length &&
                    descriptors.front().scale == 0 &&
                    descriptors.front().subtype == 0 &&
                    !descriptors.front().nullable,
                "DATEADD exact SQLDA presentation descriptor drifted") &&
         ok;

    const std::string envelope =
        EncodeFirebirdScalarProjectionEnvelope(route);
    ok = Expect(Contains(envelope,
                         "\"projection_0_function_id\":"
                         "\"sb.temporal.date_add\"") &&
                    Contains(envelope,
                             "\"projection_0_function_arg_count\":\"2\"") &&
                    Contains(envelope,
                             "\"projection_0_arg_0_value\":\"" +
                                 std::string(test.input_value) + "\"") &&
                    Contains(envelope,
                             "\"projection_0_arg_1_type\":\"interval\"") &&
                    Contains(envelope,
                             "\"projection_0_arg_1_value\":\"" +
                                 std::string(test.interval) + "\"") &&
                    !Contains(envelope, "RDB$DATABASE") &&
                    !Contains(envelope, "DATEADD(") &&
                    !Contains(envelope, "2008-02-05") &&
                    !Contains(envelope, "2007-02-06"),
                "DATEADD envelope evaluated or leaked source SQL") &&
         ok;
  }

  const auto aliases = Parse(
      "select dateadd(-1 day to date '2008-02-06') as shifted_date, "
      "dateadd(day, -1, timestamp '2008-02-06 10:10:00') "
      "\"Shifted Timestamp\" from rdb$database");
  const auto alias_descriptors =
      DescribeFirebirdScalarProjectionWireDescriptors(aliases);
  ok = Expect(aliases.recognized() && aliases.items.size() == 2 &&
                  aliases.items[0].result_name == "SHIFTED_DATE" &&
                  aliases.items[1].result_name == "Shifted Timestamp" &&
                  alias_descriptors.size() == 2 &&
                  alias_descriptors[0].sql_type == 570 &&
                  alias_descriptors[0].length == 4 &&
                  alias_descriptors[1].sql_type == 510 &&
                  alias_descriptors[1].length == 8,
              "DATEADD aliases or mixed temporal descriptors drifted") &&
       ok;
  return ok;
}

bool ExpectBoundDateAddRemainingQaRoutes() {
  struct MonthCase {
    std::string_view amount;
    std::string_view input;
    std::string_view alias;
    std::string_view expected_output;
  };
  const std::vector<MonthCase> month_cases{
      {"1", "2004-01-31", "leap_jan_31_plus__01_month", "2004-02-29"},
      {"1", "2004-02-28", "leap_feb_28_plus__01_month", "2004-03-28"},
      {"1", "2004-02-29", "leap_feb_29_plus__01_month", "2004-03-29"},
      {"-1", "2004-02-28", "leap_feb_28_minus_01_month", "2004-01-28"},
      {"-1", "2004-02-29", "leap_feb_29_minus_01_month", "2004-01-29"},
      {"11", "2004-02-28", "leap_feb_28_plus__11_month", "2005-01-28"},
      {"11", "2004-02-29", "leap_feb_29_plus__11_month", "2005-01-31"},
      {"12", "2004-02-28", "leap_feb_28_plus__12_month", "2005-02-28"},
      {"12", "2004-02-29", "leap_feb_29_plus__12_month", "2005-02-28"},
      {"-11", "2004-02-28", "leap_feb_28_minus_11_month", "2003-03-28"},
      {"-11", "2004-02-29", "leap_feb_29_minus_11_month", "2003-03-29"},
      {"-12", "2004-02-28", "leap_feb_28_minus_12_month", "2003-02-28"},
      {"-12", "2004-02-29", "leap_feb_29_minus_12_month", "2003-02-28"},
      {"-1", "2004-03-31", "leap_mar_31_minus_01_month", "2004-02-29"},
      {"1", "2003-01-31", "nonl_jan_31_plus__01_month", "2003-02-28"},
      {"1", "2003-02-28", "nonl_feb_28_plus__01_month", "2003-03-28"},
      {"-1", "2003-02-28", "nonl_feb_28_minus_01_month", "2003-01-28"},
      {"11", "2003-02-28", "nonl_feb_28_plus__11_month", "2004-01-28"},
      {"12", "2003-02-28", "nonl_feb_28_plus__12_month", "2004-02-28"},
      {"-11", "2003-02-28", "nonl_feb_28_minus_11_month", "2002-03-28"},
      {"-12", "2003-02-28", "nonl_feb_28_minus_12_month", "2002-02-28"},
      {"-1", "2003-03-31", "nonl_mar_31_minus_01_month", "2003-02-28"},
  };

  bool ok = true;
  for (const auto& test : month_cases) {
    const std::string sql =
        "select dateadd(" + std::string(test.amount) +
        " month to date '" + std::string(test.input) + "') as " +
        std::string(test.alias) + " from rdb$database";
    const auto route = Parse(sql);
    ok = Expect(route.recognized() && route.items.size() == 1,
                "canonical DATEADD(MONTH) route was rejected") && ok;
    if (!route.recognized() || route.items.size() != 1) continue;
    const auto& item = route.items.front();
    const auto& expression = item.expression;
    ok = Expect(item.output_kind ==
                        FirebirdScalarProjectionOutputKind::kDate &&
                    item.scale == 0 && item.subtype == 0 && !item.nullable &&
                    expression.kind ==
                        FirebirdScalarProjectionExpressionKind::kFunction &&
                    expression.type_name == "date" &&
                    expression.function_id == "sb.temporal.date_add" &&
                    expression.arguments.size() == 4,
                "DATEADD(MONTH) binding metadata drifted") && ok;
    // Unquoted aliases are upper-cased by Firebird presentation.
    std::string expected_alias(test.alias);
    for (char& ch : expected_alias) {
      ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    ok = Expect(item.result_name == expected_alias,
                "DATEADD(MONTH) alias presentation drifted") && ok;
    if (expression.arguments.size() == 4) {
      ok = Expect(expression.arguments[0].type_name == "date" &&
                      expression.arguments[0].encoded_value == test.input &&
                      expression.arguments[1].type_name == "text" &&
                      expression.arguments[1].encoded_value == "month" &&
                      expression.arguments[2].type_name == "int64" &&
                      expression.arguments[2].encoded_value == test.amount &&
                      expression.arguments[3].type_name == "text" &&
                      expression.arguments[3].encoded_value ==
                          "firebird.calendar_month.v1",
                  "DATEADD(MONTH) typed/profile arguments drifted") && ok;
    }
    const auto descriptors =
        DescribeFirebirdScalarProjectionWireDescriptors(route);
    ok = Expect(descriptors.size() == 1 &&
                    descriptors[0].sql_type == 570 &&
                    descriptors[0].length == 4 &&
                    descriptors[0].scale == 0 &&
                    descriptors[0].subtype == 0 &&
                    !descriptors[0].nullable,
                "DATEADD(MONTH) SQL_DATE descriptor drifted") && ok;
    const std::string envelope =
        EncodeFirebirdScalarProjectionEnvelope(route);
    ok = Expect(Contains(envelope,
                         "\"projection_0_function_arg_count\":\"4\"") &&
                    Contains(envelope, "firebird.calendar_month.v1") &&
                    !Contains(envelope, "RDB$DATABASE") &&
                    !Contains(envelope, "DATEADD(") &&
                    !Contains(envelope, test.expected_output),
                "DATEADD(MONTH) envelope evaluated or leaked source SQL") &&
         ok;
  }

  struct TimeCase {
    std::string_view sql;
    std::string_view unit;
    std::string_view expected_output;
  };
  const std::vector<TimeCase> time_cases{
      {"select dateadd(-1 hour to time '12:12:00') yesterday from rdb$database",
       "hour", "11:12:00.0000"},
      {"select dateadd(hour,-1,time '12:12:00') yesterday from rdb$database",
       "hour", "11:12:00.0000"},
      {"select dateadd(-1 minute to time '12:12:00') yesterday from rdb$database",
       "minute", "12:11:00.0000"},
      {"select dateadd(minute,-1,time '12:12:00') yesterday from rdb$database",
       "minute", "12:11:00.0000"},
      {"select dateadd(-1 second to time '12:12:00') tx_1 from rdb$database",
       "second", "12:11:59.0000"},
      {"select dateadd(second,-1,time '12:12:00') tx_2 from rdb$database",
       "second", "12:11:59.0000"},
      {"select dateadd(-1 millisecond to time '12:12:00:0000') test from rdb$database",
       "millisecond", "12:11:59.9990"},
      {"select dateadd(millisecond,-1,time '12:12:00:0000') test from rdb$database",
       "millisecond", "12:11:59.9990"},
  };
  for (const auto& test : time_cases) {
    const auto route = Parse(test.sql);
    ok = Expect(route.recognized() && route.items.size() == 1,
                "canonical DATEADD(TIME) route was rejected") && ok;
    if (!route.recognized() || route.items.size() != 1) continue;
    const auto& item = route.items.front();
    const auto& expression = item.expression;
    const auto descriptors =
        DescribeFirebirdScalarProjectionWireDescriptors(route);
    ok = Expect(item.output_kind ==
                        FirebirdScalarProjectionOutputKind::kTime &&
                    item.scale == 0 && item.subtype == 0 && !item.nullable &&
                    expression.type_name == "time" &&
                    expression.function_id == "sb.temporal.date_add" &&
                    expression.arguments.size() == 4 &&
                    descriptors.size() == 1 &&
                    descriptors[0].sql_type == 560 &&
                    descriptors[0].length == 4 &&
                    descriptors[0].scale == 0 &&
                    descriptors[0].subtype == 0 &&
                    !descriptors[0].nullable,
                "DATEADD(TIME) bind/SQL_TIME descriptor drifted") && ok;
    if (expression.arguments.size() == 4) {
      ok = Expect(expression.arguments[0].type_name == "time" &&
                      expression.arguments[0].encoded_value ==
                          "12:12:00.0000" &&
                      expression.arguments[1].encoded_value == test.unit &&
                      expression.arguments[2].type_name == "int64" &&
                      expression.arguments[2].encoded_value == "-1" &&
                      expression.arguments[3].encoded_value ==
                          "firebird.ticks_100us.v1",
                  "DATEADD(TIME) typed/profile arguments drifted") && ok;
    }
    const std::string envelope =
        EncodeFirebirdScalarProjectionEnvelope(route);
    ok = Expect(Contains(envelope, "firebird.ticks_100us.v1") &&
                    !Contains(envelope, "12:12:00:0000") &&
                    !Contains(envelope, test.expected_output) &&
                    !Contains(envelope, "RDB$DATABASE") &&
                    !Contains(envelope, "DATEADD("),
                "DATEADD(TIME) envelope evaluated or leaked source SQL") &&
         ok;
  }
  return ok;
}

bool ExpectBoundMillisecondTemporalQaRoutes() {
  struct DateDiffCase {
    std::string_view sql;
    std::string_view temporal_type;
    std::string_view begin;
    std::string_view end;
    std::string_view expected_output;
  };
  const std::vector<DateDiffCase> datediff_cases{
      {"select datediff(millisecond, "
       "cast('01.01.0001 00:00:00.0001' as timestamp), "
       "cast('31.12.9999 23:59:59.9999' as timestamp)) dd_01a "
       "from rdb$database",
       "timestamp", "0001-01-01T00:00:00.0001",
       "9999-12-31T23:59:59.9999", "315537897599999.8"},
      {"select datediff(millisecond, time '00:00:00.0001', "
       "time '23:59:59.9999') dd_01b from rdb$database",
       "time", "00:00:00.0001", "23:59:59.9999", "86399999.8"},
      {"select datediff(millisecond from "
       "cast('01.01.0001 00:00:00.0001' as timestamp) to "
       "cast('31.12.9999 23:59:59.9999' as timestamp)) dd_02a "
       "from rdb$database",
       "timestamp", "0001-01-01T00:00:00.0001",
       "9999-12-31T23:59:59.9999", "315537897599999.8"},
      {"select datediff(millisecond from cast('00:00:00.0001' as time) "
       "to cast('23:59:59.9999' as time)) dd_02b from rdb$database",
       "time", "00:00:00.0001", "23:59:59.9999", "86399999.8"},
  };

  bool ok = true;
  for (const auto& test : datediff_cases) {
    const auto route = Parse(test.sql);
    ok = Expect(route.recognized() && route.items.size() == 1,
                "canonical DATEDIFF(MILLISECOND) route was rejected") && ok;
    if (!route.recognized() || route.items.size() != 1) continue;
    const auto& item = route.items.front();
    const auto& expression = item.expression;
    const auto descriptors =
        DescribeFirebirdScalarProjectionWireDescriptors(route);
    ok = Expect(item.output_kind ==
                        FirebirdScalarProjectionOutputKind::kExactInt64 &&
                    item.scale == -1 && item.subtype == 0 && !item.nullable &&
                    expression.type_name == "numeric.fixed" &&
                    expression.function_id == "sb.temporal.date_diff" &&
                    expression.arguments.size() == 4 &&
                    descriptors.size() == 1 &&
                    descriptors[0].sql_type == 580 &&
                    descriptors[0].length == 8 &&
                    descriptors[0].scale == -1 &&
                    descriptors[0].subtype == 0 &&
                    !descriptors[0].nullable,
                "DATEDIFF(MILLISECOND) bind/SQL_INT64 descriptor drifted") &&
         ok;
    if (expression.arguments.size() == 4) {
      ok = Expect(expression.arguments[0].type_name == "text" &&
                      expression.arguments[0].encoded_value ==
                          "millisecond" &&
                      expression.arguments[1].type_name ==
                          test.temporal_type &&
                      expression.arguments[1].encoded_value == test.begin &&
                      expression.arguments[2].type_name ==
                          test.temporal_type &&
                      expression.arguments[2].encoded_value == test.end &&
                      expression.arguments[3].type_name == "text" &&
                      expression.arguments[3].encoded_value ==
                          "firebird.ticks_100us.v1",
                  "DATEDIFF(MILLISECOND) typed/profile arguments drifted") &&
           ok;
    }
    const std::string envelope =
        EncodeFirebirdScalarProjectionEnvelope(route);
    ok = Expect(Contains(envelope,
                         "\"projection_0_function_arg_count\":\"4\"") &&
                    Contains(envelope, "firebird.ticks_100us.v1") &&
                    !Contains(envelope, "01.01.0001") &&
                    !Contains(envelope, "31.12.9999") &&
                    !Contains(envelope, test.expected_output) &&
                    !Contains(envelope, "RDB$DATABASE") &&
                    !Contains(envelope, "DATEDIFF("),
                "DATEDIFF(MILLISECOND) envelope evaluated or leaked SQL") &&
         ok;
  }

  struct ExtractCase {
    std::string_view sql;
    std::string_view temporal_type;
    std::string_view input;
  };
  const std::vector<ExtractCase> extract_cases{
      {"select extract(millisecond from time '12:12:00.1111') test "
       "from rdb$database",
       "time", "12:12:00.1111"},
      {"select extract(millisecond from timestamp "
       "'2008-12-08 12:12:00.1111') test from rdb$database",
       "timestamp", "2008-12-08T12:12:00.1111"},
  };
  for (const auto& test : extract_cases) {
    const auto route = Parse(test.sql);
    ok = Expect(route.recognized() && route.items.size() == 1,
                "canonical EXTRACT(MILLISECOND) route was rejected") && ok;
    if (!route.recognized() || route.items.size() != 1) continue;
    const auto& item = route.items.front();
    const auto& expression = item.expression;
    const auto descriptors =
        DescribeFirebirdScalarProjectionWireDescriptors(route);
    ok = Expect(item.output_kind ==
                        FirebirdScalarProjectionOutputKind::kExactInt32 &&
                    item.scale == -1 && item.subtype == 0 && !item.nullable &&
                    expression.type_name == "numeric.fixed" &&
                    expression.function_id == "sb.temporal.date_part" &&
                    expression.arguments.size() == 3 &&
                    descriptors.size() == 1 &&
                    descriptors[0].sql_type == 496 &&
                    descriptors[0].length == 4 &&
                    descriptors[0].scale == -1 &&
                    descriptors[0].subtype == 0 &&
                    !descriptors[0].nullable,
                "EXTRACT(MILLISECOND) bind/SQL_LONG descriptor drifted") &&
         ok;
    if (expression.arguments.size() == 3) {
      ok = Expect(expression.arguments[0].encoded_value == "millisecond" &&
                      expression.arguments[1].type_name ==
                          test.temporal_type &&
                      expression.arguments[1].encoded_value == test.input &&
                      expression.arguments[2].encoded_value ==
                          "firebird.ticks_100us.v1",
                  "EXTRACT(MILLISECOND) typed/profile arguments drifted") &&
           ok;
    }
    const std::string envelope =
        EncodeFirebirdScalarProjectionEnvelope(route);
    ok = Expect(Contains(envelope,
                         "\"projection_0_function_arg_count\":\"3\"") &&
                    Contains(envelope, "firebird.ticks_100us.v1") &&
                    !Contains(envelope, "2008-12-08 12:12") &&
                    !Contains(envelope, "\"projection_0_value\":\"111.1\"") &&
                    !Contains(envelope, "RDB$DATABASE") &&
                    !Contains(envelope, "EXTRACT("),
                "EXTRACT(MILLISECOND) envelope evaluated or leaked SQL") &&
         ok;
  }
  return ok;
}

FirebirdScalarProjectionWireRow ExactWireRow(std::string value) {
  FirebirdScalarProjectionWireRow row;
  FirebirdScalarProjectionWireCell cell;
  cell.text = std::move(value);
  row.cells.push_back(std::move(cell));
  return row;
}

bool ExpectBoundIntegerCastQaRoutes() {
  struct Case {
    std::string_view sql;
    std::string_view input;
    std::string_view engine_result;
  };
  const std::vector<Case> cases{
      {"select cast('1.25001' as integer) from rdb$database;",
       "1.25001", "1"},
      {"select CAST('1.5001' AS INTEGER) from RDB$DATABASE;",
       "1.5001", "2"},
  };

  bool ok = true;
  for (const auto& test : cases) {
    const auto route = Parse(test.sql, "NONE");
    ok = Expect(route.recognized() && route.items.size() == 1,
                std::string("INTEGER CAST QA route rejected: ") +
                    std::string(test.sql)) &&
         ok;
    if (!route.recognized() || route.items.size() != 1) continue;

    const auto& item = route.items.front();
    ok = Expect(item.result_name == "CAST" &&
                    item.output_kind ==
                        FirebirdScalarProjectionOutputKind::kInt32 &&
                    item.scale == 0 && item.subtype == 0 &&
                    item.declared_length == 0 && !item.nullable,
                "INTEGER CAST Firebird SQL_LONG metadata drifted") &&
         ok;

    const auto& outer = item.expression;
    ok = Expect(outer.kind ==
                    FirebirdScalarProjectionExpressionKind::kFunction &&
                    outer.type_name == "int32" &&
                    outer.encoded_value.empty() &&
                    outer.function_id == "data.scalar.cast" &&
                    outer.arguments.size() == 2,
                "INTEGER CAST outer neutral binding drifted") &&
         ok;
    if (outer.arguments.size() == 2) {
      const auto& decimal = outer.arguments[0];
      const auto& integer_target = outer.arguments[1];
      ok = Expect(decimal.kind ==
                          FirebirdScalarProjectionExpressionKind::kFunction &&
                      decimal.type_name == "decimal" &&
                      decimal.encoded_value.empty() &&
                      decimal.function_id == "data.scalar.cast" &&
                      decimal.arguments.size() == 3 &&
                      integer_target.kind ==
                          FirebirdScalarProjectionExpressionKind::kLiteral &&
                      integer_target.type_name == "text" &&
                      integer_target.encoded_value == "int32" &&
                      !integer_target.is_null,
                  "INTEGER CAST nested conversion binding drifted") &&
           ok;
      if (decimal.arguments.size() == 3) {
        ok = Expect(
                 decimal.arguments[0].kind ==
                         FirebirdScalarProjectionExpressionKind::kLiteral &&
                     decimal.arguments[0].type_name == "character.none" &&
                     decimal.arguments[0].encoded_value == test.input &&
                     !decimal.arguments[0].is_null &&
                     decimal.arguments[1].kind ==
                         FirebirdScalarProjectionExpressionKind::kLiteral &&
                     decimal.arguments[1].type_name == "text" &&
                     decimal.arguments[1].encoded_value == "decimal(18,0)" &&
                     decimal.arguments[2].kind ==
                         FirebirdScalarProjectionExpressionKind::kLiteral &&
                     decimal.arguments[2].type_name == "text" &&
                     decimal.arguments[2].encoded_value == "half_up",
                 "INTEGER CAST source, target, or rounding profile drifted") &&
             ok;
      }
    }

    const auto descriptors =
        DescribeFirebirdScalarProjectionWireDescriptors(route);
    ok = Expect(descriptors.size() == 1 &&
                    descriptors.front().name == "CAST" &&
                    descriptors.front().source_name == "c0" &&
                    descriptors.front().relation.empty() &&
                    descriptors.front().owner.empty() &&
                    descriptors.front().sql_type == 496 &&
                    descriptors.front().length == 4 &&
                    descriptors.front().scale == 0 &&
                    descriptors.front().subtype == 0 &&
                    !descriptors.front().nullable,
                "INTEGER CAST exact SQLDA descriptor drifted") &&
         ok;

    const std::string envelope =
        EncodeFirebirdScalarProjectionEnvelope(route);
    ok = Expect(
             Contains(envelope,
                      "\"projection_0_function_id\":"
                      "\"data.scalar.cast\"") &&
                 Contains(envelope,
                          "\"projection_0_arg_0_function_id\":"
                          "\"data.scalar.cast\"") &&
                 Contains(envelope,
                          "\"projection_0_arg_0_arg_0_type\":"
                          "\"character.none\"") &&
                 Contains(envelope,
                          "\"projection_0_arg_0_arg_0_value\":\"" +
                              std::string(test.input) + "\"") &&
                 Contains(envelope,
                          "\"projection_0_arg_0_arg_1_value\":"
                          "\"decimal(18,0)\"") &&
                 Contains(envelope,
                          "\"projection_0_arg_0_arg_2_value\":"
                          "\"half_up\"") &&
                 Contains(envelope,
                          "\"projection_0_arg_1_value\":\"int32\"") &&
                 Contains(envelope,
                          "\"scalar_projection_parser_executes_sql\":false") &&
                 Contains(envelope, "\"contains_sql_text\":false") &&
                 !Contains(envelope, "RDB$DATABASE") &&
                 !Contains(envelope, "CAST(") &&
                 !Contains(envelope, "virtual_catalog_projection") &&
                 !Contains(envelope, "virtual_monitoring_rows") &&
                 !Contains(envelope, "\"projection_0_value\":\"1\"") &&
                 !Contains(envelope, "\"projection_0_value\":\"2\""),
             "INTEGER CAST envelope computed a result, leaked SQL, or "
             "selected virtual monitoring") &&
         ok;

    std::vector<FirebirdScalarProjectionWireRow> rows{
        ExactWireRow(std::string(test.engine_result))};
    std::string diagnostic;
    ok = Expect(DecodeFirebirdScalarProjectionRows(
                    route, &rows, &diagnostic) &&
                    rows.front().cells.front().text == test.engine_result,
                "INTEGER CAST worker rejected or changed engine SQL_LONG") &&
         ok;
  }

  const auto route = Parse(cases.front().sql, "NONE");
  if (route.recognized()) {
    const auto expect_rejected = [&](std::string_view value,
                                     std::string_view expected_diagnostic) {
      std::vector<FirebirdScalarProjectionWireRow> rows{
          ExactWireRow(std::string(value))};
      std::string diagnostic;
      return Expect(!DecodeFirebirdScalarProjectionRows(
                        route, &rows, &diagnostic) &&
                        diagnostic == expected_diagnostic,
                    "malformed INTEGER CAST result carrier was admitted");
    };
    ok = expect_rejected("1.0", "int64_result_required") && ok;
    ok = expect_rejected("computed", "int64_result_required") && ok;
    ok = expect_rejected("2147483648", "int32_result_required") && ok;
    ok = expect_rejected("-2147483649", "int32_result_required") && ok;

    std::vector<FirebirdScalarProjectionWireRow> null_rows(1);
    null_rows.front().cells.push_back({true, {}});
    std::string diagnostic;
    ok = Expect(!DecodeFirebirdScalarProjectionRows(
                    route, &null_rows, &diagnostic) &&
                    diagnostic == "unexpected_null_result",
                "unexpected NULL INTEGER CAST result was admitted") &&
         ok;
  }
  return ok;
}

bool ExpectBoundIntegerCastShapesFailClosed() {
  const std::vector<std::string_view> unsupported{
      "select cast('1.25001' as int) from rdb$database",
      "select cast('1.25001' as bigint) from rdb$database",
      "select cast('1.25001' as decimal(18,0)) from rdb$database",
      "select cast('1.4' as integer) from rdb$database",
      "select cast('1.50010' as integer) from rdb$database",
      "select cast(1.25001 as integer) from rdb$database",
      "select cast(null as integer) from rdb$database",
      "select cast('1.25001' as integer) from other_table",
      "select cast('1.25001' as integer) from rdb$database where 1 = 1",
  };
  bool ok = true;
  for (const auto sql : unsupported) {
    const auto route = Parse(sql, "NONE");
    ok = Expect(!route.recognized(),
                std::string("unsupported INTEGER CAST was admitted: ") +
                    std::string(sql)) &&
         ok;
    ok = Expect(EncodeFirebirdScalarProjectionEnvelope(route).empty(),
                "unsupported INTEGER CAST produced executable SBLR") &&
         ok;
  }
  ok = Expect(!Parse("select cast('1.25001' as integer) "
                     "from rdb$database",
                     "UTF8")
                   .recognized(),
              "UTF8 INTEGER CAST shape bypassed exact NONE binding") &&
       ok;
  return ok;
}

bool ExpectBoundNumericLiteralCastQaRoutes() {
  struct Case {
    std::string_view sql;
    std::string_view input;
    std::string_view target_descriptor;
    std::string_view result_type;
    FirebirdScalarProjectionOutputKind output_kind;
    std::uint32_t sql_type;
    std::uint32_t length;
    std::int16_t scale;
    std::int16_t subtype;
    std::string_view engine_result;
    bool fixed_text;
  };
  const std::vector<Case> cases{
      {"select cast(1.25001 as char(21)) from rdb$database;",
       "1.25001", "character", "character",
       FirebirdScalarProjectionOutputKind::kFixedText, 452, 21, 0, 0,
       "1.25001", true},
      {"select CAST(1.25001 AS VARCHAR(21)) from RDB$DATABASE;",
       "1.25001", "varchar", "varchar",
       FirebirdScalarProjectionOutputKind::kVaryingText, 448, 21, 0, 0,
       "1.25001", false},
      {"select cast(1.24999 as numeric(2,1)) from rdb$database;",
       "1.24999", "decimal(2,1)", "decimal",
       FirebirdScalarProjectionOutputKind::kExactInt16, 500, 2, -1, 1,
       "1.2", false},
      {"select cast(1.25001 as numeric(2,1)) from rdb$database;",
       "1.25001", "decimal(2,1)", "decimal",
       FirebirdScalarProjectionOutputKind::kExactInt16, 500, 2, -1, 1,
       "1.3", false},
  };

  bool ok = true;
  for (const auto& test : cases) {
    const auto route = Parse(test.sql, "NONE");
    ok = Expect(route.recognized() && route.items.size() == 1,
                std::string("numeric-literal CAST QA route rejected: ") +
                    std::string(test.sql)) &&
         ok;
    if (!route.recognized() || route.items.size() != 1) continue;

    const auto& item = route.items.front();
    const std::uint32_t expected_declared_length =
        test.sql_type == 452 || test.sql_type == 448 ? 21u : 0u;
    ok = Expect(item.result_name == "CAST" &&
                    item.output_kind == test.output_kind &&
                    item.scale == test.scale && item.subtype == test.subtype &&
                    item.declared_length == expected_declared_length &&
                    !item.nullable,
                "numeric-literal CAST bound Firebird metadata drifted") &&
         ok;

    const auto& expression = item.expression;
    ok = Expect(expression.kind ==
                        FirebirdScalarProjectionExpressionKind::kFunction &&
                    expression.type_name == test.result_type &&
                    expression.encoded_value.empty() &&
                    expression.function_id == "data.scalar.cast" &&
                    expression.arguments.size() ==
                        (test.scale < 0 ? 3u : 2u),
                "numeric-literal CAST neutral binding drifted") &&
         ok;
    if (expression.arguments.size() >= 2) {
      ok = Expect(
               expression.arguments[0].kind ==
                       FirebirdScalarProjectionExpressionKind::kLiteral &&
                   expression.arguments[0].type_name == "numeric.fixed" &&
                   expression.arguments[0].encoded_value == test.input &&
                   !expression.arguments[0].is_null &&
                   expression.arguments[1].kind ==
                       FirebirdScalarProjectionExpressionKind::kLiteral &&
                   expression.arguments[1].type_name == "text" &&
                   expression.arguments[1].encoded_value ==
                       test.target_descriptor &&
                   !expression.arguments[1].is_null,
               "numeric-literal CAST source or target literal drifted") &&
           ok;
    }
    if (test.scale < 0 && expression.arguments.size() == 3) {
      ok = Expect(expression.arguments[2].kind ==
                          FirebirdScalarProjectionExpressionKind::kLiteral &&
                      expression.arguments[2].type_name == "text" &&
                      expression.arguments[2].encoded_value == "half_up" &&
                      !expression.arguments[2].is_null,
                  "NUMERIC(2,1) CAST rounding profile drifted") &&
           ok;
    }

    const auto descriptors =
        DescribeFirebirdScalarProjectionWireDescriptors(route);
    ok = Expect(descriptors.size() == 1 &&
                    descriptors.front().name == "CAST" &&
                    descriptors.front().source_name == "c0" &&
                    descriptors.front().relation.empty() &&
                    descriptors.front().owner.empty() &&
                    descriptors.front().sql_type == test.sql_type &&
                    descriptors.front().length == test.length &&
                    descriptors.front().scale == test.scale &&
                    descriptors.front().subtype == test.subtype &&
                    descriptors.front().character_set ==
                        (test.sql_type == 452 || test.sql_type == 448
                             ? "NONE"
                             : "") &&
                    descriptors.front().character_length ==
                        expected_declared_length &&
                    !descriptors.front().nullable,
                "numeric-literal CAST exact SQLDA descriptor drifted") &&
         ok;

    const std::string envelope =
        EncodeFirebirdScalarProjectionEnvelope(route);
    ok = Expect(
             Contains(envelope,
                      "\"projection_0_function_id\":"
                      "\"data.scalar.cast\"") &&
                 Contains(envelope,
                          "\"projection_0_value\":\"\"") &&
                 Contains(envelope,
                          "\"projection_0_arg_0_type\":"
                          "\"numeric.fixed\"") &&
                 Contains(envelope,
                          "\"projection_0_arg_0_value\":\"" +
                              std::string(test.input) + "\"") &&
                 Contains(envelope,
                          "\"projection_0_arg_1_value\":\"" +
                              std::string(test.target_descriptor) + "\"") &&
                 (test.scale >= 0 ||
                  Contains(envelope,
                           "\"projection_0_arg_2_value\":\"half_up\"")) &&
                 Contains(envelope,
                          "\"scalar_projection_parser_executes_sql\":false") &&
                 Contains(envelope, "\"contains_sql_text\":false") &&
                 !Contains(envelope, "RDB$DATABASE") &&
                 !Contains(envelope, "CAST(") &&
                 !Contains(envelope, "virtual_catalog_projection") &&
                 !Contains(envelope, "virtual_monitoring_rows") &&
                 (test.scale >= 0 ||
                  !Contains(envelope,
                            "\"projection_0_value\":\"" +
                                std::string(test.engine_result) + "\"")),
             "numeric-literal CAST envelope computed a result, leaked SQL, "
             "or selected virtual monitoring") &&
         ok;

    std::vector<FirebirdScalarProjectionWireRow> rows{
        ExactWireRow(std::string(test.engine_result))};
    std::string diagnostic;
    std::string expected_wire_value(test.engine_result);
    if (test.fixed_text) expected_wire_value.resize(21, ' ');
    ok = Expect(DecodeFirebirdScalarProjectionRows(
                    route, &rows, &diagnostic) &&
                    rows.front().cells.front().text == expected_wire_value,
                "numeric-literal CAST worker rejected or mispresented the "
                "neutral engine result") &&
         ok;
  }

  const auto fixed_route = Parse(cases[0].sql, "NONE");
  if (fixed_route.recognized()) {
    std::vector<FirebirdScalarProjectionWireRow> rows{
        ExactWireRow("1234567890123456789012")};
    std::string diagnostic;
    ok = Expect(!DecodeFirebirdScalarProjectionRows(
                    fixed_route, &rows, &diagnostic) &&
                    diagnostic == "bounded_fixed_text_result_required",
                "oversized fixed-text CAST result was admitted") &&
         ok;
  }

  const auto numeric_route = Parse(cases[2].sql, "NONE");
  if (numeric_route.recognized()) {
    const auto expect_rejected = [&](std::string_view value) {
      std::vector<FirebirdScalarProjectionWireRow> rows{
          ExactWireRow(std::string(value))};
      std::string diagnostic;
      return Expect(!DecodeFirebirdScalarProjectionRows(
                        numeric_route, &rows, &diagnostic) &&
                        diagnostic ==
                            "canonical_exact_scaled_int16_result_required",
                    "malformed NUMERIC(2,1) CAST carrier was admitted");
    };
    ok = expect_rejected("1.20") && ok;
    ok = expect_rejected("1") && ok;
    ok = expect_rejected("3276.8") && ok;
    ok = expect_rejected("-3276.9") && ok;

    std::vector<FirebirdScalarProjectionWireRow> null_rows(1);
    null_rows.front().cells.push_back({true, {}});
    std::string diagnostic;
    ok = Expect(!DecodeFirebirdScalarProjectionRows(
                    numeric_route, &null_rows, &diagnostic) &&
                    diagnostic == "unexpected_null_result",
                "unexpected NULL NUMERIC(2,1) CAST result was admitted") &&
         ok;
  }
  return ok;
}

bool ExpectBoundNumericLiteralCastShapesFailClosed() {
  const std::vector<std::string_view> unsupported{
      "select cast(1.25001 as char(20)) from rdb$database",
      "select cast(1.25001 as char(22)) from rdb$database",
      "select cast(1.25001 as char) from rdb$database",
      "select cast(1.25001 as character(21)) from rdb$database",
      "select cast(1.24999 as char(21)) from rdb$database",
      "select cast(1.25001 as varchar(20)) from rdb$database",
      "select cast(1.24999 as varchar(21)) from rdb$database",
      "select cast(1.25001 as numeric(3,1)) from rdb$database",
      "select cast(1.25001 as numeric(2,2)) from rdb$database",
      "select cast(1.25001 as decimal(2,1)) from rdb$database",
      "select cast(1.25000 as numeric(2,1)) from rdb$database",
      "select cast(1.250010 as numeric(2,1)) from rdb$database",
      "select cast('1.25001' as numeric(2,1)) from rdb$database",
      "select cast(1.25001e0 as numeric(2,1)) from rdb$database",
      "select cast(1 + .25001 as numeric(2,1)) from rdb$database",
      "select cast(1.25001 as numeric(2,1)) from other_table",
      "select cast(1.25001 as numeric(2,1)) from rdb$database where 1 = 1",
  };
  bool ok = true;
  for (const auto sql : unsupported) {
    const auto route = Parse(sql, "NONE");
    ok = Expect(!route.recognized(),
                std::string("unsupported numeric-literal CAST was admitted: ") +
                    std::string(sql)) &&
         ok;
    ok = Expect(EncodeFirebirdScalarProjectionEnvelope(route).empty(),
                "unsupported numeric-literal CAST produced executable SBLR") &&
         ok;
  }
  ok = Expect(!Parse("select cast(1.25001 as char(21)) "
                     "from rdb$database",
                     "UTF8")
                   .recognized() &&
                  !Parse("select cast(1.25001 as numeric(2,1)) "
                         "from rdb$database",
                         "UTF8")
                       .recognized(),
              "UTF8 numeric-literal CAST bypassed exact NONE binding") &&
       ok;
  return ok;
}

bool ExpectBoundTemporalCastQaRoutes() {
  struct Case {
    std::string_view label;
    std::string_view sql;
    std::string_view source_literal;
    std::string_view result_type;
    std::string_view target_descriptor;
    FirebirdScalarProjectionOutputKind output_kind;
    std::uint32_t sql_type;
    std::uint32_t length;
    std::uint32_t declared_length;
    std::string_view engine_result;
  };
  const std::vector<Case> cases{
      {"test_08",
       "select cast('28.1.2001' as date) from rdb$database;",
       "28.1.2001", "date", "date",
       FirebirdScalarProjectionOutputKind::kDate, 570, 4, 0,
       "2001-01-28"},
      {"test_10",
       "select cast('14:34:59.1234' as time) from rdb$database;",
       "14:34:59.1234", "time", "time",
       FirebirdScalarProjectionOutputKind::kTime, 560, 4, 0,
       "14:34:59.1234"},
      {"test_11",
       "select cast('14:34:59.1234' as time) from rdb$database;",
       "14:34:59.1234", "time", "time",
       FirebirdScalarProjectionOutputKind::kTime, 560, 4, 0,
       "14:34:59.1234"},
      {"test_13",
       "select cast('10.2.1489 14:34:59.1234' as timestamp) "
       "from rdb$database;",
       "10.2.1489 14:34:59.1234", "timestamp", "timestamp",
       FirebirdScalarProjectionOutputKind::kTimestamp, 510, 8, 0,
       "1489-02-10T14:34:59.1234"},
      {"test_14",
       "select cast('10.2.1489 14:34:59.1234' as timestamp) "
       "from rdb$database;",
       "10.2.1489 14:34:59.1234", "timestamp", "timestamp",
       FirebirdScalarProjectionOutputKind::kTimestamp, 510, 8, 0,
       "1489-02-10T14:34:59.1234"},
      {"test_15",
       "select cast(cast('10.2.1973' as date) as char(32)) "
       "from rdb$database;",
       "10.2.1973", "character", "character(32)",
       FirebirdScalarProjectionOutputKind::kFixedText, 452, 32, 32,
       "1973-02-10"},
      {"test_16",
       "select cast(cast('10.2.1973' as date) as varchar(40)) "
       "from rdb$database;",
       "10.2.1973", "varchar", "varchar(40)",
       FirebirdScalarProjectionOutputKind::kVaryingText, 448, 40, 40,
       "1973-02-10"},
      {"test_17",
       "select cast(cast('10.2.1973' as date) as timestamp) "
       "from rdb$database;",
       "10.2.1973", "timestamp", "timestamp",
       FirebirdScalarProjectionOutputKind::kTimestamp, 510, 8, 0,
       "1973-02-10T00:00:00.0000"},
      {"test_18",
       "select cast(cast('13:28:45' as time) as char(32)) "
       "from rdb$database;",
       "13:28:45", "character", "character(32)",
       FirebirdScalarProjectionOutputKind::kFixedText, 452, 32, 32,
       "13:28:45.0000"},
      {"test_19",
       "select cast(cast('13:28:45' as time) as varchar(32)) "
       "from rdb$database;",
       "13:28:45", "varchar", "varchar(32)",
       FirebirdScalarProjectionOutputKind::kVaryingText, 448, 32, 32,
       "13:28:45.0000"},
      {"test_20",
       "select cast(cast('1.4.2002 0:59:59.1' as timestamp) as char(50)) "
       "from rdb$database;",
       "1.4.2002 0:59:59.1", "character", "character(50)",
       FirebirdScalarProjectionOutputKind::kFixedText, 452, 50, 50,
       "2002-04-01 00:59:59.1000"},
      {"test_21",
       "select cast(cast('1.4.2002 0:59:59.1' as timestamp) as varchar(50)) "
       "from rdb$database;",
       "1.4.2002 0:59:59.1", "varchar", "varchar(50)",
       FirebirdScalarProjectionOutputKind::kVaryingText, 448, 50, 50,
       "2002-04-01 00:59:59.1000"},
      {"test_22",
       "select cast(cast('1.4.2002 0:59:59.1' as timestamp) as date) "
       "from rdb$database;",
       "1.4.2002 0:59:59.1", "date", "date",
       FirebirdScalarProjectionOutputKind::kDate, 570, 4, 0,
       "2002-04-01"},
      {"test_23",
       "select cast(cast('1.4.2002 0:59:59.1' as timestamp) as time) "
       "from rdb$database;",
       "1.4.2002 0:59:59.1", "time", "time",
       FirebirdScalarProjectionOutputKind::kTime, 560, 4, 0,
       "00:59:59.1000"},
  };

  const auto validate_cast_tree = [&](const auto& self,
                                      const auto& expression,
                                      std::string_view source_literal) -> bool {
    bool tree_ok = Expect(
        expression.kind == FirebirdScalarProjectionExpressionKind::kFunction &&
            expression.function_id == "data.scalar.cast" &&
            expression.encoded_value.empty() &&
            expression.arguments.size() == 3 &&
            expression.arguments[1].kind ==
                FirebirdScalarProjectionExpressionKind::kLiteral &&
            expression.arguments[1].type_name == "text" &&
            !expression.arguments[1].is_null &&
            expression.arguments[2].kind ==
                FirebirdScalarProjectionExpressionKind::kLiteral &&
            expression.arguments[2].type_name == "text" &&
            expression.arguments[2].encoded_value ==
                "firebird.temporal_cast.v1" &&
            !expression.arguments[2].is_null,
        "temporal CAST tree omitted its neutral conversion profile");
    if (!tree_ok || expression.arguments.size() != 3) return false;
    const auto& source = expression.arguments.front();
    if (source.kind == FirebirdScalarProjectionExpressionKind::kFunction) {
      return self(self, source, source_literal) && tree_ok;
    }
    return Expect(source.kind ==
                          FirebirdScalarProjectionExpressionKind::kLiteral &&
                      source.type_name == "character.none" &&
                      source.encoded_value == source_literal &&
                      !source.is_null,
                  "temporal CAST source literal descriptor drifted") &&
           tree_ok;
  };

  bool ok = true;
  for (const auto& test : cases) {
    const auto route = Parse(test.sql, "NONE");
    ok = Expect(route.recognized() && route.items.size() == 1,
                std::string("temporal CAST QA route rejected: ") +
                    std::string(test.label)) &&
         ok;
    if (!route.recognized() || route.items.size() != 1) continue;
    const auto& item = route.items.front();
    ok = Expect(item.result_name == "CAST" &&
                    item.output_kind == test.output_kind && item.scale == 0 &&
                    item.subtype == 0 &&
                    item.declared_length == test.declared_length &&
                    !item.nullable &&
                    item.expression.type_name == test.result_type &&
                    item.expression.arguments.size() == 3 &&
                    item.expression.arguments[1].encoded_value ==
                        test.target_descriptor,
                "temporal CAST bound result metadata drifted") &&
         ok;
    ok = validate_cast_tree(validate_cast_tree, item.expression,
                            test.source_literal) &&
         ok;

    const auto descriptors =
        DescribeFirebirdScalarProjectionWireDescriptors(route);
    const bool character = test.sql_type == 452 || test.sql_type == 448;
    ok = Expect(descriptors.size() == 1 &&
                    descriptors.front().name == "CAST" &&
                    descriptors.front().source_name == "c0" &&
                    descriptors.front().sql_type == test.sql_type &&
                    descriptors.front().length == test.length &&
                    descriptors.front().scale == 0 &&
                    descriptors.front().subtype == 0 &&
                    descriptors.front().character_set ==
                        (character ? "NONE" : "") &&
                    descriptors.front().character_length ==
                        (character ? test.declared_length : 0u) &&
                    !descriptors.front().nullable,
                "temporal CAST Firebird SQLDA descriptor drifted") &&
         ok;

    const std::string envelope = EncodeFirebirdScalarProjectionEnvelope(route);
    ok = Expect(Contains(envelope, "\"data.scalar.cast\"") &&
                    Contains(envelope, "firebird.temporal_cast.v1") &&
                    Contains(envelope,
                             "\"projection_0_arg_1_value\":\"" +
                                 std::string(test.target_descriptor) + "\"") &&
                    Contains(envelope,
                             "\"scalar_projection_parser_executes_sql\":false") &&
                    Contains(envelope, "\"contains_sql_text\":false") &&
                    !Contains(envelope, "RDB$DATABASE") &&
                    !Contains(envelope, "CAST(") &&
                    !Contains(envelope, "virtual_catalog_projection") &&
                    !Contains(envelope, "virtual_monitoring_rows") &&
                    (test.engine_result == test.source_literal ||
                     !Contains(envelope, test.engine_result)),
                "temporal CAST envelope evaluated a result or leaked SQL") &&
         ok;

    std::vector<FirebirdScalarProjectionWireRow> rows{
        ExactWireRow(std::string(test.engine_result))};
    std::string diagnostic;
    std::string expected_wire_result(test.engine_result);
    if (test.output_kind == FirebirdScalarProjectionOutputKind::kFixedText) {
      expected_wire_result.resize(test.declared_length, ' ');
    }
    ok = Expect(DecodeFirebirdScalarProjectionRows(route, &rows, &diagnostic) &&
                    rows.front().cells.front().text == expected_wire_result,
                "temporal CAST worker rejected or changed the engine result") &&
         ok;
  }
  return ok;
}

bool ExpectBoundTemporalCastErrorQaRoutes() {
  struct LiteralCase {
    std::string_view label;
    std::string_view sql;
    std::string_view source;
    std::string_view target;
    FirebirdScalarProjectionOutputKind output_kind;
    std::uint32_t sql_type;
    std::uint32_t length;
  };
  const std::vector<LiteralCase> literal_cases{
      {"test_09",
       "select cast('29.2.2002' as date) from rdb$database;",
       "29.2.2002", "date", FirebirdScalarProjectionOutputKind::kDate,
       570, 4},
      {"test_12",
       "select cast('9:11:60' as time) from rdb$database;",
       "9:11:60", "time", FirebirdScalarProjectionOutputKind::kTime,
       560, 4},
  };

  bool ok = true;
  for (const auto& test : literal_cases) {
    const auto route = Parse(test.sql, "NONE");
    ok = Expect(route.recognized() && route.items.size() == 1,
                std::string("negative temporal CAST route rejected: ") +
                    std::string(test.label)) && ok;
    if (!route.recognized() || route.items.size() != 1) continue;
    const auto& item = route.items.front();
    const auto& expression = item.expression;
    ok = Expect(item.result_name == "CAST" &&
                    item.output_kind == test.output_kind &&
                    !item.nullable && expression.kind ==
                        FirebirdScalarProjectionExpressionKind::kFunction &&
                    expression.function_id == "data.scalar.cast" &&
                    expression.type_name == test.target &&
                    expression.arguments.size() == 3 &&
                    expression.arguments[0].kind ==
                        FirebirdScalarProjectionExpressionKind::kLiteral &&
                    expression.arguments[0].type_name == "character.none" &&
                    expression.arguments[0].encoded_value == test.source &&
                    expression.arguments[1].encoded_value == test.target &&
                    expression.arguments[2].encoded_value ==
                        "firebird.temporal_cast.v1",
                "negative temporal CAST was not bound as neutral input") && ok;
    const auto descriptors =
        DescribeFirebirdScalarProjectionWireDescriptors(route);
    ok = Expect(descriptors.size() == 1 &&
                    descriptors.front().sql_type == test.sql_type &&
                    descriptors.front().length == test.length &&
                    !descriptors.front().nullable,
                "negative temporal CAST SQLDA descriptor drifted") && ok;
    const std::string envelope = EncodeFirebirdScalarProjectionEnvelope(route);
    ok = Expect(Contains(envelope, test.source) &&
                    Contains(envelope, "firebird.temporal_cast.v1") &&
                    Contains(envelope, "\"contains_sql_text\":false") &&
                    Contains(envelope,
                             "\"scalar_projection_parser_executes_sql\":false") &&
                    !Contains(envelope, "RDB$DATABASE") &&
                    !Contains(envelope, "CAST(") &&
                    !Contains(envelope, "conversion error from string"),
                "negative temporal CAST leaked SQL or parser evaluation") && ok;
  }

  const auto nested = Parse(
      "select cast(cast(1.25001 as int) as date) from rdb$database;",
      "NONE");
  ok = Expect(nested.recognized() && nested.items.size() == 1,
              "test_03 nested numeric-to-date CAST route was rejected") && ok;
  if (nested.recognized() && nested.items.size() == 1) {
    const auto& outer = nested.items.front().expression;
    ok = Expect(nested.items.front().output_kind ==
                        FirebirdScalarProjectionOutputKind::kDate &&
                    outer.type_name == "date" &&
                    outer.function_id == "data.scalar.cast" &&
                    outer.arguments.size() == 3 &&
                    outer.arguments[1].encoded_value == "date" &&
                    outer.arguments[2].encoded_value ==
                        "firebird.temporal_cast.v1",
                "test_03 outer temporal conversion binding drifted") && ok;
    if (outer.arguments.size() == 3) {
      const auto& integer = outer.arguments[0];
      ok = Expect(integer.kind ==
                          FirebirdScalarProjectionExpressionKind::kFunction &&
                      integer.type_name == "int32" &&
                      integer.function_id == "data.scalar.cast" &&
                      integer.arguments.size() == 2 &&
                      integer.arguments[1].encoded_value == "int32",
                  "test_03 integer conversion binding drifted") && ok;
      if (integer.arguments.size() == 2) {
        const auto& decimal = integer.arguments[0];
        ok = Expect(decimal.kind ==
                            FirebirdScalarProjectionExpressionKind::kFunction &&
                        decimal.type_name == "decimal" &&
                        decimal.function_id == "data.scalar.cast" &&
                        decimal.arguments.size() == 3 &&
                        decimal.arguments[0].type_name == "numeric.fixed" &&
                        decimal.arguments[0].encoded_value == "1.25001" &&
                        decimal.arguments[1].encoded_value == "decimal(18,0)" &&
                        decimal.arguments[2].encoded_value == "half_up",
                    "test_03 source or engine rounding binding drifted") && ok;
      }
    }
    const auto descriptors =
        DescribeFirebirdScalarProjectionWireDescriptors(nested);
    const std::string envelope =
        EncodeFirebirdScalarProjectionEnvelope(nested);
    ok = Expect(descriptors.size() == 1 &&
                    descriptors.front().sql_type == 570 &&
                    descriptors.front().length == 4 &&
                    Contains(envelope, "\"numeric.fixed\"") &&
                    Contains(envelope, "\"1.25001\"") &&
                    Contains(envelope, "\"decimal(18,0)\"") &&
                    Contains(envelope, "\"half_up\"") &&
                    !Contains(envelope, "RDB$DATABASE") &&
                    !Contains(envelope, "CAST(") &&
                    !Contains(envelope, "\"projection_0_value\":\"1\"") &&
                    !Contains(envelope, "conversion error from string"),
                "test_03 envelope evaluated, rendered, or leaked SQL") && ok;
  }
  return ok;
}

bool ExpectBoundTemporalCastShapesFailClosed() {
  const std::vector<std::string_view> unsupported{
      "select cast('29.2.2003' as date) from rdb$database",
      "select cast('29.2.2002' as timestamp) from rdb$database",
      "select cast('9:11:61' as time) from rdb$database",
      "select cast(cast(1.25002 as int) as date) from rdb$database",
      "select cast(cast('1.25001' as int) as date) from rdb$database",
      "select cast(cast(1.25001 as integer) as date) from rdb$database",
      "select cast(cast(1.25001 as integer) as time) from rdb$database",
      "select cast('28.1.2001' as timestamp) from rdb$database",
      "select cast('14:34:59.1234' as date) from rdb$database",
      "select cast(cast('10.2.1973' as date) as char(31)) from rdb$database",
      "select cast(cast('10.2.1973' as date) as varchar(32)) from rdb$database",
      "select cast(cast('13:28:45' as time) as varchar(40)) from rdb$database",
      "select cast(cast('1.4.2002 0:59:59.1' as timestamp) as char(49)) from rdb$database",
      "select cast(cast('1.4.2002 0:59:59.1' as timestamp) as timestamp) from rdb$database",
      "select cast('28.1.2001' as date) from other_table",
      "select cast('28.1.2001' as date) from rdb$database where 1 = 1",
  };
  bool ok = true;
  for (const auto sql : unsupported) {
    const auto route = Parse(sql, "NONE");
    ok = Expect(!route.recognized(),
                std::string("unsupported temporal CAST was admitted: ") +
                    std::string(sql)) &&
         ok;
    ok = Expect(EncodeFirebirdScalarProjectionEnvelope(route).empty(),
                "unsupported temporal CAST produced executable SBLR") &&
         ok;
  }
  ok = Expect(!Parse("select cast('28.1.2001' as date) "
                     "from rdb$database",
                     "UTF8")
                   .recognized(),
              "UTF8 temporal CAST bypassed exact NONE binding") &&
       ok;
  ok = Expect(!Parse("select cast('29.2.2002' as date) "
                     "from rdb$database",
                     "UTF8")
                   .recognized() &&
                  !Parse("select cast(cast(1.25001 as int) as date) "
                         "from rdb$database",
                         "UTF8")
                       .recognized(),
              "negative temporal CAST bypassed exact NONE binding") && ok;
  return ok;
}

bool ExpectDateAddWirePresentationValidation() {
  const auto date_route = Parse(
      "select dateadd(-1 day to date '2008-02-06') from rdb$database");
  const auto timestamp_route = Parse(
      "select dateadd(day, -1, timestamp '2008-02-06 10:10:00') "
      "from rdb$database");
  bool ok = Expect(date_route.recognized() && timestamp_route.recognized(),
                   "DATEADD wire validation routes were not recognized");
  if (!date_route.recognized() || !timestamp_route.recognized()) return false;

  const auto expect_value = [&](const FirebirdScalarProjectionRoute& route,
                                std::string_view value,
                                bool expected,
                                std::string_view expected_diagnostic,
                                std::string_view label) {
    std::vector<FirebirdScalarProjectionWireRow> rows{
        ExactWireRow(std::string(value))};
    std::string diagnostic;
    const bool accepted =
        DecodeFirebirdScalarProjectionRows(route, &rows, &diagnostic);
    bool value_ok = Expect(accepted == expected, label);
    if (accepted && expected) {
      value_ok =
          Expect(rows.front().cells.front().text == value,
                 "DATEADD presentation validation changed engine output") &&
          value_ok;
    } else if (!accepted && !expected) {
      value_ok = Expect(diagnostic == expected_diagnostic,
                        "DATEADD presentation diagnostic drifted") &&
                 value_ok;
    }
    return value_ok;
  };

  ok = expect_value(date_route, "2008-02-05", true, {},
                    "canonical DATEADD date result was rejected") &&
       ok;
  ok = expect_value(date_route, "2008-02-30", false,
                    "canonical_date_result_required",
                    "invalid DATEADD date result was admitted") &&
       ok;
  ok = expect_value(timestamp_route, "2008-02-05T10:10:00", true, {},
                    "canonical DATEADD timestamp result was rejected") &&
       ok;
  ok = expect_value(timestamp_route, "2008-02-05 10:10:00", false,
                    "canonical_timestamp_result_required",
                    "noncanonical DATEADD timestamp result was admitted") &&
       ok;
  ok = expect_value(timestamp_route, "2008-02-05T24:10:00", false,
                    "canonical_timestamp_result_required",
                    "invalid DATEADD timestamp result was admitted") &&
       ok;
  return ok;
}

bool ExpectRemainingTemporalWirePresentationValidation() {
  const auto time_route = Parse(
      "select dateadd(-1 millisecond to time '12:12:00:0000') "
      "from rdb$database");
  const auto datediff_route = Parse(
      "select datediff(millisecond, time '00:00:00.0001', "
      "time '23:59:59.9999') from rdb$database");
  const auto extract_route = Parse(
      "select extract(millisecond from time '12:12:00.1111') "
      "from rdb$database");
  bool ok = Expect(time_route.recognized() && datediff_route.recognized() &&
                       extract_route.recognized(),
                   "remaining temporal wire routes were not recognized");
  if (!time_route.recognized() || !datediff_route.recognized() ||
      !extract_route.recognized()) {
    return false;
  }

  const auto expect_value = [&](const FirebirdScalarProjectionRoute& route,
                                std::string_view value,
                                bool expected,
                                std::string_view expected_diagnostic,
                                std::string_view label) {
    std::vector<FirebirdScalarProjectionWireRow> rows{
        ExactWireRow(std::string(value))};
    std::string diagnostic;
    const bool accepted =
        DecodeFirebirdScalarProjectionRows(route, &rows, &diagnostic);
    bool value_ok = Expect(accepted == expected, label);
    if (accepted && expected) {
      value_ok =
          Expect(rows.size() == 1 && rows.front().cells.size() == 1 &&
                     rows.front().cells.front().text == value,
                 "temporal presentation validation changed engine output") &&
          value_ok;
    } else if (!accepted && !expected) {
      value_ok = Expect(diagnostic == expected_diagnostic,
                        "temporal presentation diagnostic drifted") &&
                 value_ok;
    }
    return value_ok;
  };

  ok = expect_value(time_route, "12:11:59.9990", true, {},
                    "canonical TIME result was rejected") && ok;
  ok = expect_value(time_route, "11:12:00.0000", true, {},
                    "canonical whole-second TIME result was rejected") && ok;
  ok = expect_value(time_route, "12:11:59", false,
                    "canonical_time_result_required",
                    "fractionless TIME result was admitted") && ok;
  ok = expect_value(time_route, "12:11:59:9990", false,
                    "canonical_time_result_required",
                    "colon-fraction TIME result was admitted") && ok;
  ok = expect_value(time_route, "24:00:00.0000", false,
                    "canonical_time_result_required",
                    "out-of-range TIME result was admitted") && ok;

  ok = expect_value(datediff_route, "315537897599999.8", true, {},
                    "canonical scaled DATEDIFF result was rejected") && ok;
  ok = expect_value(datediff_route, "86399999.8", true, {},
                    "canonical time DATEDIFF result was rejected") && ok;
  ok = expect_value(datediff_route, "86399999.80", false,
                    "canonical_exact_scaled_int64_result_required",
                    "noncanonical DATEDIFF scale was admitted") && ok;
  ok = expect_value(datediff_route, "922337203685477580.8", false,
                    "canonical_exact_scaled_int64_result_required",
                    "overflowing DATEDIFF carrier was admitted") && ok;

  ok = expect_value(extract_route, "111.1", true, {},
                    "canonical scaled EXTRACT result was rejected") && ok;
  ok = expect_value(extract_route, "214748364.7", true, {},
                    "SQL_LONG maximum exact carrier was rejected") && ok;
  ok = expect_value(extract_route, "-214748364.8", true, {},
                    "SQL_LONG minimum exact carrier was rejected") && ok;
  ok = expect_value(extract_route, "214748364.8", false,
                    "canonical_exact_scaled_int32_result_required",
                    "SQL_LONG positive overflow carrier was admitted") && ok;
  ok = expect_value(extract_route, "111.10", false,
                    "canonical_exact_scaled_int32_result_required",
                    "noncanonical EXTRACT scale was admitted") && ok;

  const auto mixed_route = Parse(
      "select dateadd(11 month to date '2004-02-29') m, "
      "dateadd(-1 second to time '12:12:00') t, "
      "datediff(millisecond, time '00:00:00.0001', "
      "time '23:59:59.9999') d, "
      "extract(millisecond from time '12:12:00.1111') e "
      "from rdb$database");
  const auto mixed_descriptors =
      DescribeFirebirdScalarProjectionWireDescriptors(mixed_route);
  ok = Expect(mixed_route.recognized() && mixed_route.items.size() == 4 &&
                  mixed_descriptors.size() == 4 &&
                  mixed_descriptors[0].sql_type == 570 &&
                  mixed_descriptors[1].sql_type == 560 &&
                  mixed_descriptors[2].sql_type == 580 &&
                  mixed_descriptors[2].scale == -1 &&
                  mixed_descriptors[3].sql_type == 496 &&
                  mixed_descriptors[3].scale == -1,
              "mixed temporal projection cardinality/descriptors drifted") &&
       ok;
  if (mixed_route.recognized()) {
    FirebirdScalarProjectionWireRow complete_row;
    for (const std::string_view value : {
             std::string_view{"2005-01-31"},
             std::string_view{"12:11:59.0000"},
             std::string_view{"86399999.8"},
             std::string_view{"111.1"}}) {
      complete_row.cells.push_back({false, std::string(value)});
    }
    std::vector<FirebirdScalarProjectionWireRow> complete_rows{
        std::move(complete_row)};
    std::string diagnostic;
    ok = Expect(DecodeFirebirdScalarProjectionRows(
                    mixed_route, &complete_rows, &diagnostic),
                "complete four-column temporal engine row was rejected") &&
         ok;
    complete_rows.front().cells.pop_back();
    diagnostic.clear();
    ok = Expect(!DecodeFirebirdScalarProjectionRows(
                    mixed_route, &complete_rows, &diagnostic) &&
                    diagnostic == "result_column_count_mismatch",
                "incomplete temporal projection row was admitted") && ok;
  }
  return ok;
}

bool ExpectExactScalarWireValidation() {
  const auto decimal_route = Parse(
      "select cast(acos(1) as decimal(18,15)) from rdb$database");
  bool ok = Expect(decimal_route.recognized() &&
                       decimal_route.items.size() == 1,
                   "exact scalar wire test route was not recognized");
  if (!decimal_route.recognized() || decimal_route.items.size() != 1) {
    return false;
  }

  const auto expect_value = [&](const FirebirdScalarProjectionRoute& route,
                                std::string_view value,
                                bool expected,
                                std::string_view label) {
    std::vector<FirebirdScalarProjectionWireRow> rows{
        ExactWireRow(std::string(value))};
    std::string diagnostic;
    const bool accepted =
        DecodeFirebirdScalarProjectionRows(route, &rows, &diagnostic);
    bool value_ok = Expect(accepted == expected, label);
    if (expected && accepted) {
      value_ok =
          Expect(rows.size() == 1 && rows.front().cells.size() == 1 &&
                     rows.front().cells.front().text == value,
                 "exact scalar wire validation changed the engine value") &&
          value_ok;
    } else if (!expected && !accepted) {
      value_ok =
          Expect(diagnostic ==
                     "canonical_exact_scaled_int64_result_required",
                 "exact scalar wire rejection diagnostic drifted") &&
          value_ok;
    }
    return value_ok;
  };

  for (const std::string_view value : {
           std::string_view{"0.000000000000000"},
           std::string_view{"0.642092615934331"},
           std::string_view{"-0.536572918000435"},
       }) {
    ok = expect_value(decimal_route, value, true,
                      "valid DECIMAL(18,15) engine value was rejected") &&
         ok;
  }

  FirebirdScalarProjectionRoute int64_carrier_route;
  int64_carrier_route.items.emplace_back();
  int64_carrier_route.items.front().result_name = "EXACT";
  int64_carrier_route.items.front().output_kind =
      FirebirdScalarProjectionOutputKind::kExactInt64;
  int64_carrier_route.items.front().scale = 0;
  ok = expect_value(int64_carrier_route, "9223372036854775807", true,
                    "INT64 maximum exact carrier was rejected") &&
       ok;
  ok = expect_value(int64_carrier_route, "-9223372036854775808", true,
                    "INT64 minimum exact carrier was rejected") &&
       ok;
  ok = expect_value(int64_carrier_route, "9223372036854775808", false,
                    "INT64 positive overflow exact carrier was admitted") &&
       ok;
  ok = expect_value(int64_carrier_route, "-9223372036854775809", false,
                    "INT64 negative overflow exact carrier was admitted") &&
       ok;

  const std::string huge_positive =
      std::string(512, '9') + ".000000000000000";
  const std::string huge_negative = "-" + huge_positive;
  ok = expect_value(decimal_route, huge_positive, false,
                    "huge positive exact digit string was admitted") &&
       ok;
  ok = expect_value(decimal_route, huge_negative, false,
                    "huge negative exact digit string was admitted") &&
       ok;

  std::string diagnostic;
  std::vector<FirebirdScalarProjectionWireRow> no_rows;
  ok = Expect(!DecodeFirebirdScalarProjectionRows(
                  decimal_route, &no_rows, &diagnostic) &&
                  diagnostic ==
                      "exactly_one_complete_result_row_required",
              "zero-row constant scalar result was admitted") &&
       ok;
  std::vector<FirebirdScalarProjectionWireRow> multiple_rows{
      ExactWireRow("0.000000000000000"),
      ExactWireRow("0.642092615934331")};
  diagnostic.clear();
  ok = Expect(!DecodeFirebirdScalarProjectionRows(
                  decimal_route, &multiple_rows, &diagnostic) &&
                  diagnostic ==
                      "exactly_one_complete_result_row_required",
              "multiple-row constant scalar result was admitted") &&
       ok;
  std::vector<FirebirdScalarProjectionWireRow> incomplete_rows(1);
  diagnostic.clear();
  ok = Expect(!DecodeFirebirdScalarProjectionRows(
                  decimal_route, &incomplete_rows, &diagnostic) &&
                  diagnostic == "result_column_count_mismatch",
              "incomplete constant scalar result row was admitted") &&
       ok;

  diagnostic.clear();
  ok = Expect(ValidateFirebirdScalarProjectionCompletePacket(
                  decimal_route, 1, 1, false, &diagnostic),
              "complete constant scalar packet was rejected") &&
       ok;
  diagnostic.clear();
  ok = Expect(ValidateFirebirdScalarProjectionCompletePacket(
                  decimal_route, 0, 1, true, &diagnostic),
              "deferred constant scalar cursor transport was rejected") &&
       ok;
  diagnostic.clear();
  ok = Expect(!ValidateFirebirdScalarProjectionCompletePacket(
                  decimal_route, 0, 0, true, &diagnostic) &&
                  diagnostic ==
                      "exactly_one_complete_result_row_required",
              "zero-cardinality deferred scalar cursor was admitted") &&
       ok;
  diagnostic.clear();
  ok = Expect(!ValidateFirebirdScalarProjectionCompletePacket(
                  decimal_route, 1, 1, true, &diagnostic) &&
                  diagnostic ==
                      "constant_scalar_projection_incomplete_cursor",
              "non-exhausted fetched scalar cursor result was admitted") &&
       ok;
  diagnostic.clear();
  ok = Expect(!ValidateFirebirdScalarProjectionCompletePacket(
                  decimal_route, 1, 2, false, &diagnostic) &&
                  diagnostic == "result_row_count_mismatch",
              "constant scalar decoded/server row mismatch was admitted") &&
       ok;
  diagnostic.clear();
  ok = Expect(!ValidateFirebirdScalarProjectionCompletePacket(
                  decimal_route, 0, 0, false, &diagnostic) &&
                  diagnostic ==
                      "exactly_one_complete_result_row_required",
              "zero-row constant scalar packet was admitted") &&
       ok;
  diagnostic.clear();
  ok = Expect(!ValidateFirebirdScalarProjectionCompletePacket(
                  decimal_route, 2, 2, false, &diagnostic) &&
                  diagnostic ==
                      "exactly_one_complete_result_row_required",
              "multiple-row constant scalar packet was admitted") &&
       ok;
  return ok;
}

bool ExpectAsciiValQaRoute() {
  struct Case {
    std::string sql;
    std::string charset;
    std::string argument_type;
    std::string argument_value;
    bool nullable;
  };
  const std::vector<Case> cases{
      {"select ascii_val('A') from rdb$database", "NONE",
       "character.none", "A", false},
      {"select ascii_val('Ã') from rdb$database", "NONE",
       "character.none", "Ã", false},
      {"select ascii_val(cast('A' as BLOB)) from rdb$database", "NONE",
       "blob.binary", "A", false},
      {"select ascii_val(NULL) from rdb$database", "NONE",
       "character.none", "", true},
      {"select ascii_val('') from rdb$database", "NONE",
       "character.none", "", false},
  };

  bool ok = true;
  for (const auto& test : cases) {
    const auto route = Parse(test.sql, test.charset);
    ok = Expect(route.recognized() && route.items.size() == 1,
                std::string("ASCII_VAL route rejected: ") + test.sql) && ok;
    if (!route.recognized() || route.items.size() != 1) continue;
    const auto& item = route.items.front();
    ok = Expect(item.result_name == "ASCII_VAL" &&
                    item.output_kind ==
                        FirebirdScalarProjectionOutputKind::kInt16 &&
                    item.nullable == test.nullable,
                std::string("ASCII_VAL output metadata mismatch: ") +
                    test.sql) && ok;
    ok = Expect(item.expression.type_name == "int64" &&
                    item.expression.encoded_value.empty() &&
                    item.expression.function_id ==
                        "data.scalar.int64_from_first_octet" &&
                    item.expression.arguments.size() == 1,
                std::string("ASCII_VAL neutral function mismatch: ") +
                    test.sql) && ok;
    if (item.expression.arguments.size() != 1) continue;
    const auto& argument = item.expression.arguments.front();
    ok = Expect(argument.type_name == test.argument_type &&
                    argument.encoded_value == test.argument_value &&
                    argument.is_null == test.nullable,
                std::string("ASCII_VAL typed input mismatch: ") + test.sql) && ok;
  }
  return ok;
}

bool ExpectCore3227Route() {
  const auto first = Parse(
      "select ascii_val (cast('Hoplala' as char(12) character set utf8)) "
      "from rdb$database",
      "UTF8");
  const auto second = Parse(
      "select ascii_val (cast('Hopläla' as char(12) character set utf8)) "
      "from rdb$database",
      "UTF8");
  bool ok = true;
  for (const auto* route : {&first, &second}) {
    ok = Expect(route->recognized() && route->items.size() == 1,
                "CORE-3227 typed UTF8 ASCII_VAL route was rejected") && ok;
    if (!route->recognized() || route->items.size() != 1 ||
        route->items.front().expression.arguments.size() != 1) {
      continue;
    }
    const auto& item = route->items.front();
    const auto& argument = item.expression.arguments.front();
    ok = Expect(argument.type_name == "character.utf8" &&
                    !argument.is_null && !item.nullable,
                "CORE-3227 explicit UTF8 descriptor was not preserved") && ok;
  }
  if (first.recognized()) {
    ok = Expect(first.items.front().expression.arguments.front().encoded_value ==
                    "Hoplala",
                "CORE-3227 ASCII literal was modified by the parser") && ok;
  }
  if (second.recognized()) {
    ok = Expect(second.items.front().expression.arguments.front().encoded_value ==
                    "Hopläla",
                "CORE-3227 multibyte literal was modified by the parser") && ok;
  }
  return ok;
}

bool ExpectCore3479Route() {
  const auto route = Parse(
      "select ascii_val('') v1, ascii_val(ascii_char(0)) v2, "
      "ascii_val(ascii_char(null)) v3 from rdb$database",
      "NONE");
  bool ok = true;
  ok = Expect(route.recognized() && route.items.size() == 3,
              "CORE-3479 three-column scalar route was rejected") && ok;
  if (!route.recognized() || route.items.size() != 3) return false;
  ok = Expect(route.items[0].result_name == "V1" &&
                  route.items[1].result_name == "V2" &&
                  route.items[2].result_name == "V3" &&
                  !route.items[0].nullable && !route.items[1].nullable &&
                  route.items[2].nullable,
              "CORE-3479 aliases or SQLDA nullability metadata mismatch") && ok;
  for (std::size_t index : {std::size_t{1}, std::size_t{2}}) {
    const auto& outer = route.items[index].expression;
    ok = Expect(outer.function_id ==
                    "data.scalar.int64_from_first_octet" &&
                    outer.arguments.size() == 1 &&
                    outer.arguments.front().function_id ==
                        "data.scalar.octet_from_int64" &&
                    outer.arguments.front().type_name == "binary" &&
                    outer.arguments.front().arguments.size() == 1,
                "CORE-3479 nested ASCII expression tree mismatch") && ok;
  }
  const std::string envelope =
      EncodeFirebirdScalarProjectionEnvelope(route);
  ok = Expect(Contains(envelope, "\"projection_0_name\":\"c0\"") &&
                  Contains(envelope, "\"projection_1_name\":\"c1\"") &&
                  Contains(envelope, "\"projection_2_name\":\"c2\"") &&
                  Contains(envelope,
                           "\"projection_1_arg_0_function_id\":"
                           "\"data.scalar.octet_from_int64\"") &&
                  !Contains(envelope, "\"projection_0_name\":\"V1\""),
              "CORE-3479 ordinal neutral row keys are not exact") && ok;
  return ok;
}

bool ExpectUnsupportedShapesFailClosed() {
  const std::vector<std::string_view> unsupported{
      "select ascii_val(c) from t",
      "select ascii_char(1 + 1) from rdb$database",
      "select ascii_char(0xff) from rdb$database",
      "select ascii_val(ascii_val('A')) from rdb$database",
      "select ascii_val(cast('A' as integer)) from rdb$database",
      "select ascii_val(cast('' as char(12) character set utf8)) "
      "from rdb$database",
      "select ascii_val('A', 'B') from rdb$database",
      "select ascii_val('A') from rdb$database where 1 = 1",
      "select ascii_val(left(list(f,''),1)) from "
      "(select cast(ascii_char(255) as blob) f from rdb$database)",
      "select abs(1 + 1) from rdb$database",
      "select abs('1') from rdb$database",
      "select abs(null) from rdb$database",
      "select pi(1) from rdb$database",
      "select pi from rdb$database",
      "select asin(null) from rdb$database",
      "select sqrt(1 + 1) from rdb$database",
      "select atan2(1) from rdb$database",
      "select atan2(1, 1, 1) from rdb$database",
      "select log(10) from rdb$database",
      "select power(2, 3, 4) from rdb$database",
      "select cos(pi()) from rdb$database",
      "select cos('0') from rdb$database",
      "select cos(.5) from rdb$database",
      "select cos(2.) from rdb$database",
      "select tan(c) from rdb$database",
      "select acos(1) from rdb$database",
      "select sin(1) from rdb$database",
      "select cos(1) from other_table",
      "select maxvalue(54, 87) from rdb$database",
      "select maxvalue(54, 87, 10, 11) from rdb$database",
      "select minvalue(9, 7) from rdb$database",
      "select mod(11) from rdb$database",
      "select mod(11, 10, 9) from rdb$database",
      "select sign() from rdb$database",
      "select sign(-9, 8) from rdb$database",
      "select round(5.7778) from rdb$database",
      "select round(5.7778, 3, 1) from rdb$database",
      "select trunc(987.65, 1, 0) from rdb$database",
      "select maxvalue(2147483648, 1, 2) from rdb$database",
      "select minvalue(-2147483649, 1, 2) from rdb$database",
      "select mod(2147483648, 10) from rdb$database",
      "select maxvalue(null, 1, 2) from rdb$database",
      "select sign(null) from rdb$database",
      "select ceil(null) from rdb$database",
      "select floor(value) from rdb$database",
      "select mod('11', 10) from rdb$database",
      "select sign(abs(-9)) from rdb$database",
      "select ceil(1 + 1.1) from rdb$database",
      "select mod(11.0, 10) from rdb$database",
      "select sign(2.1) from rdb$database",
      "select ceil(2) from rdb$database",
      "select floor(2.1e0) from rdb$database",
      "select round(5.7778, 3.0) from rdb$database",
      "select trunc(2) from rdb$database",
      "select greatest(54, 87, 10) from rdb$database",
      "select least(9, 7, 10) from rdb$database",
      "select truncate(2.8) from rdb$database",
      "select maxvalue(54, 87, 10) from other_table",
      "select cot(1) from rdb$database",
      "select cast(acos(1) as decimal(18,14)) from rdb$database",
      "select cast(acos(1) as decimal(17,15)) from rdb$database",
      "select cast(acos(1.0) as decimal(18,15)) from rdb$database",
      "select cast(acos(null) as decimal(18,15)) from rdb$database",
      "select cast(acos(1 + 0) as decimal(18,15)) from rdb$database",
      "select cast(tan(1) as decimal(18,15)) from rdb$database",
      "select cast(acos(1) as decimal(18,15)) from other_table",
      "select bin_and(1) from rdb$database",
      "select bin_and(1, 1, 1) from rdb$database",
      "select bin_or() from rdb$database",
      "select bin_xor(0, 1, 1) from rdb$database",
      "select bin_shl(8) from rdb$database",
      "select bin_shr(8, 1, 0) from rdb$database",
      "select bin_and(1 + 0, 1) from rdb$database",
      "select bin_or('1', 0) from rdb$database",
      "select bin_xor(null, 1) from rdb$database",
      "select bin_shl(2147483648, 1) from rdb$database",
      "select bin_shr(-2147483649, 1) from rdb$database",
      "select bin_and(01, 1) from rdb$database",
      "select bin_or(+1, 0) from rdb$database",
      "select bin_and(0, 1) from rdb$database",
      "select bin_or(0, 1) from rdb$database",
      "select bin_xor(1, 0) from rdb$database",
      "select bin_shl(8, 2) from rdb$database",
      "select bin_shr(4, 1) from rdb$database",
      "select bin_and(1, 1) from other_table",
      "select datediff(millisecond, "
      "cast('12/02/2008 13:33:33' as timestamp), "
      "cast('12/02/2008 13:33:35' as timestamp)) from rdb$database",
      "select datediff(week from "
      "cast('12/02/2008 13:33:33' as timestamp) to "
      "cast('12/02/2008 13:33:35' as timestamp)) from rdb$database",
      "select datediff(second, '12/02/2008 13:33:33', "
      "'12/02/2008 13:33:35') from rdb$database",
      "select datediff(second, timestamp '2008-12-02 13:33:33', "
      "timestamp '2008-12-02 13:33:35') from rdb$database",
      "select datediff(second, "
      "cast('2008-12-02 13:33:33' as timestamp), "
      "cast('2008-12-02 13:33:35' as timestamp)) from rdb$database",
      "select datediff(second, "
      "cast('02/30/2008 13:33:33' as timestamp), "
      "cast('12/02/2008 13:33:35' as timestamp)) from rdb$database",
      "select datediff(second, "
      "cast('12/02/2008 24:33:33' as timestamp), "
      "cast('12/02/2008 13:33:35' as timestamp)) from rdb$database",
      "select datediff(second, cast('12/02/2008 13:33:33' as date), "
      "cast('12/02/2008 13:33:35' as timestamp)) from rdb$database",
      "select datediff(second from "
      "cast('12/02/2008 13:33:33' as timestamp) "
      "cast('12/02/2008 13:33:35' as timestamp)) from rdb$database",
      "select datediff(second, "
      "cast('12/02/2008 13:33:33' as timestamp), "
      "cast('12/02/2008 13:33:35' as timestamp), 1) from rdb$database",
      "select datediff(second, null, "
      "cast('12/02/2008 13:33:35' as timestamp)) from rdb$database",
      "select datediff(second, started_at, ended_at) from rdb$database",
      "select datediff(second, "
      "cast('12/02/2008 13:33:33' as timestamp), "
      "cast('12/02/2008 13:33:35' as timestamp)) from other_table",
      "select extract(day from date '30.12.2008') from rdb$database",
      "select extract(week from timestamp '2008-12-30 00:00:00') "
      "from rdb$database",
      "select extract(week from date '2008-12-30') from rdb$database",
      "select extract(week from date '31.02.2008') from rdb$database",
      "select extract(week from date '12/30/2008') from rdb$database",
      "select extract(week date '30.12.2008') from rdb$database",
      "select extract(week from date '30.12.2008') from other_table",
      "select dateadd(month, -1, date '2008-02-06') from rdb$database",
      "select dateadd(hour, -1, timestamp '2008-02-06 10:10:00') "
      "from rdb$database",
      "select dateadd(year, -1, timestamp '2008-02-06 10:10:00') "
      "from rdb$database",
      "select dateadd(day, -1, time '10:10:00') from rdb$database",
      "select dateadd(day, -1, '2008-02-06') from rdb$database",
      "select dateadd(day, -1, cast('2008-02-06' as date)) "
      "from rdb$database",
      "select dateadd(day, -1, current_date) from rdb$database",
      "select dateadd(day, null, date '2008-02-06') from rdb$database",
      "select dateadd(day, 1, date '2008-02-06') from rdb$database",
      "select dateadd(day, -01, date '2008-02-06') from rdb$database",
      "select dateadd(day, 1 + 1, date '2008-02-06') from rdb$database",
      "select dateadd(day, 1.0, date '2008-02-06') from rdb$database",
      "select dateadd(\"DAY\", -1, date '2008-02-06') from rdb$database",
      "select dateadd(day, -1, date '02/06/2008') from rdb$database",
      "select dateadd(day, -1, date '2008-02-30') from rdb$database",
      "select dateadd(day, -1, date '2007-02-29') from rdb$database",
      "select dateadd(day, -1, timestamp '2008-02-06T10:10:00') "
      "from rdb$database",
      "select dateadd(day, -1, timestamp '2008-02-06 10:10:00.0000') "
      "from rdb$database",
      "select dateadd(day, -1, timestamp '2008-02-06 24:10:00') "
      "from rdb$database",
      "select dateadd(-1 day date '2008-02-06') from rdb$database",
      "select dateadd(day -1, date '2008-02-06') from rdb$database",
      "select dateadd(day, -1, date '2008-02-06', 1) from rdb$database",
      "select dateadd(day, 2147483648, date '2008-02-06') "
      "from rdb$database",
      "select dateadd(day, -1, date '2008-02-06') from other_table",
      "select dateadd(1 month to date '2004-01-30') from rdb$database",
      "select dateadd(2 month to date '2004-01-31') from rdb$database",
      "select dateadd(month, 1, date '2004-01-31') from rdb$database",
      "select dateadd(+1 month to date '2004-01-31') from rdb$database",
      "select dateadd(-1 hour to time '12:13:00') from rdb$database",
      "select dateadd(1 hour to time '12:12:00') from rdb$database",
      "select dateadd(-1 day to time '12:12:00') from rdb$database",
      "select dateadd(-1 millisecond to time '12:12:00.000') "
      "from rdb$database",
      "select datediff(millisecond, time '00:00:00.0000', "
      "time '23:59:59.9999') from rdb$database",
      "select datediff(millisecond, time '00:00:00.0001', "
      "time '23:59:59.9998') from rdb$database",
      "select datediff(millisecond, time '00:00:00.0001', "
      "cast('31.12.9999 23:59:59.9999' as timestamp)) "
      "from rdb$database",
      "select datediff(millisecond, "
      "cast('01.01.0001 00:00:00.001' as timestamp), "
      "cast('31.12.9999 23:59:59.9999' as timestamp)) "
      "from rdb$database",
      "select extract(millisecond from time '12:12:00.1110') "
      "from rdb$database",
      "select extract(millisecond from time '12:12:00.111') "
      "from rdb$database",
      "select extract(millisecond from timestamp "
      "'2008-12-09 12:12:00.1111') from rdb$database",
      "select extract(millisecond from date '2008-12-08') "
      "from rdb$database",
  };
  bool ok = true;
  for (const auto sql : unsupported) {
    const auto route = Parse(sql, "NONE");
    ok = Expect(!route.recognized(),
                std::string("unsupported scalar shape was admitted: ") +
                    std::string(sql)) && ok;
    ok = Expect(EncodeFirebirdScalarProjectionEnvelope(route).empty(),
                "unrecognized scalar route produced executable SBLR") && ok;
  }
  return ok;
}

bool ExpectExecutionSessionIntegration() {
  scratchbird::parser::ipc::ParserClientConfig config;
  scratchbird::parser::firebird::FirebirdExecutionSession session(config);
  const auto result = session.RunStatement(
      "select ascii_val(ascii_char(null)) v3 from rdb$database",
      {}, false, false, 0, false, {}, "NONE");
  const auto math_result = session.RunStatement(
      "select abs(-1) a, pi() p from rdb$database",
      {}, false, false, 0, false, {}, "NONE");
  const auto direct_double_result = session.RunStatement(
      "select atan2(1, 1) a, power(2, 3) p from rdb$database",
      {}, false, false, 0, false, {}, "NONE");
  const auto bound_numeric_result = session.RunStatement(
      "select maxvalue(54, 87, 10) mx, round(5.7778, 3) r "
      "from rdb$database",
      {}, false, false, 0, false, {}, "NONE");
  const auto binary_result = session.RunStatement(
      "select bin_and(1, 0) a, bin_or(0, 0) o, "
      "bin_xor(0, 1) x, bin_shl(8, 1) l, bin_shr(8, 1) r "
      "from rdb$database",
      {}, false, false, 0, false, {}, "NONE");
  const auto decimal_cast_result = session.RunStatement(
      "select cast(cot(1) as decimal(18,15)) c from rdb$database",
      {}, false, false, 0, false, {}, "NONE");
  const auto integer_cast_result = session.RunStatement(
      "select cast('1.5001' as integer) from rdb$database",
      {}, false, false, 0, false, {}, "NONE");
  const auto numeric_literal_cast_result = session.RunStatement(
      "select cast(1.25001 as char(21)) c, "
      "cast(1.25001 as varchar(21)) v, "
      "cast(1.24999 as numeric(2,1)) n1, "
      "cast(1.25001 as numeric(2,1)) n2 from rdb$database",
      {}, false, false, 0, false, {}, "NONE");
  const auto temporal_result = session.RunStatement(
      "select datediff(second from "
      "cast('12/02/2008 13:33:33' as timestamp) to "
      "cast('12/02/2008 13:33:35' as timestamp)) d, "
      "extract(week from date '30.12.2008') w from rdb$database",
      {}, false, false, 0, false, {}, "NONE");
  const auto dateadd_result = session.RunStatement(
      "select dateadd(-1 year to date '2008-02-06') d, "
      "dateadd(day, -1, timestamp '2008-02-06 10:10:00') t "
      "from rdb$database",
      {}, false, false, 0, false, {}, "NONE");
  const auto exact_temporal_result = session.RunStatement(
      "select dateadd(11 month to date '2004-02-29') m, "
      "dateadd(-1 millisecond to time '12:12:00:0000') t, "
      "datediff(millisecond, time '00:00:00.0001', "
      "time '23:59:59.9999') d, "
      "extract(millisecond from time '12:12:00.1111') e "
      "from rdb$database",
      {}, false, false, 0, false, {}, "NONE");
  const auto string_result = session.RunStatement(
      "select position('beau','beau,il fait beau') C1, "
      "position('beau','beau,il fait beau',2) C2 from rdb$database",
      {}, false, false, 0, false, {}, "NONE");
  return Expect(result.accepted,
                "Firebird execution session rejected bounded scalar lowering") &&
         Expect(Contains(result.sblr_payload,
                         "\"operation_id\":\"query.evaluate_projection\"") &&
                    Contains(result.sblr_payload,
                             "\"projection_0_arg_0_function_id\":"
                             "\"data.scalar.octet_from_int64\""),
                "Firebird execution session did not use the scalar route") &&
         Expect(!result.parser_executes_sql &&
                    !result.cached_storage_authority &&
                    !result.cached_finality_authority,
                "Firebird scalar route claimed parser execution or authority") &&
         Expect(math_result.accepted &&
                    Contains(math_result.sblr_payload,
                             "\"projection_0_function_id\":\"sb.scalar.abs\"") &&
                    Contains(math_result.sblr_payload,
                             "\"projection_1_function_id\":\"sb.scalar.pi\"") &&
                    !math_result.parser_executes_sql &&
                    !math_result.cached_storage_authority &&
                    !math_result.cached_finality_authority,
                "Firebird execution session did not lower ABS/PI without "
                "claiming engine authority") &&
         Expect(direct_double_result.accepted &&
                    Contains(direct_double_result.sblr_payload,
                             "\"projection_0_function_id\":"
                             "\"sb.scalar.atan2\"") &&
                    Contains(direct_double_result.sblr_payload,
                             "\"projection_1_function_id\":"
                             "\"sb.scalar.power\"") &&
                    !direct_double_result.parser_executes_sql &&
                    !direct_double_result.cached_storage_authority &&
                    !direct_double_result.cached_finality_authority,
                "Firebird execution session did not lower direct DOUBLE "
                "calls without claiming engine authority") &&
         Expect(bound_numeric_result.accepted &&
                    Contains(bound_numeric_result.sblr_payload,
                             "\"projection_0_function_id\":"
                             "\"sb.scalar.greatest\"") &&
                    Contains(bound_numeric_result.sblr_payload,
                             "\"projection_1_function_id\":"
                             "\"sb.scalar.round\"") &&
                    !bound_numeric_result.parser_executes_sql &&
                    !bound_numeric_result.cached_storage_authority &&
                    !bound_numeric_result.cached_finality_authority,
                "Firebird execution session did not lower bound numeric "
                "calls without claiming engine authority") &&
         Expect(binary_result.accepted &&
                    Contains(binary_result.sblr_payload,
                             "\"projection_0_function_id\":"
                             "\"sb.scalar.bit_and\"") &&
                    Contains(binary_result.sblr_payload,
                             "\"projection_1_function_id\":"
                             "\"sb.scalar.bit_or\"") &&
                    Contains(binary_result.sblr_payload,
                             "\"projection_2_function_id\":"
                             "\"sb.scalar.bit_xor\"") &&
                    Contains(binary_result.sblr_payload,
                             "\"projection_3_function_id\":"
                             "\"sb.scalar.bit_shift_left\"") &&
                    Contains(binary_result.sblr_payload,
                             "\"projection_4_function_id\":"
                             "\"sb.scalar.bit_shift_right\"") &&
                    !Contains(binary_result.sblr_payload, "BIN_") &&
                    !Contains(binary_result.sblr_payload, "RDB$DATABASE") &&
                    !Contains(binary_result.sblr_payload,
                              "\"projection_3_value\":\"16\"") &&
                    !Contains(binary_result.sblr_payload,
                              "\"projection_4_value\":\"4\"") &&
                    !binary_result.parser_executes_sql &&
                    !binary_result.cached_storage_authority &&
                    !binary_result.cached_finality_authority,
                "Firebird execution session did not lower binary calls "
                "without local evaluation or engine authority") &&
         Expect(decimal_cast_result.accepted &&
                    Contains(decimal_cast_result.sblr_payload,
                             "\"projection_0_function_id\":"
                             "\"data.scalar.cast\"") &&
                    Contains(decimal_cast_result.sblr_payload,
                             "\"projection_0_arg_0_function_id\":"
                             "\"sb.scalar.cot\"") &&
                    !decimal_cast_result.parser_executes_sql &&
                    !decimal_cast_result.cached_storage_authority &&
                    !decimal_cast_result.cached_finality_authority,
                "Firebird execution session did not lower DECIMAL math CAST "
                "without claiming engine authority") &&
         Expect(integer_cast_result.accepted &&
                    Contains(integer_cast_result.sblr_payload,
                             "\"projection_0_function_id\":"
                             "\"data.scalar.cast\"") &&
                    Contains(integer_cast_result.sblr_payload,
                             "\"projection_0_arg_0_function_id\":"
                             "\"data.scalar.cast\"") &&
                    Contains(integer_cast_result.sblr_payload,
                             "\"projection_0_arg_0_arg_0_type\":"
                             "\"character.none\"") &&
                    Contains(integer_cast_result.sblr_payload,
                             "\"projection_0_arg_0_arg_0_value\":"
                             "\"1.5001\"") &&
                    Contains(integer_cast_result.sblr_payload,
                             "\"projection_0_arg_0_arg_1_value\":"
                             "\"decimal(18,0)\"") &&
                    Contains(integer_cast_result.sblr_payload,
                             "\"projection_0_arg_0_arg_2_value\":"
                             "\"half_up\"") &&
                    Contains(integer_cast_result.sblr_payload,
                             "\"projection_0_arg_1_value\":\"int32\"") &&
                    !Contains(integer_cast_result.sblr_payload,
                              "RDB$DATABASE") &&
                    !Contains(integer_cast_result.sblr_payload,
                              "virtual_catalog_projection") &&
                    !Contains(integer_cast_result.sblr_payload,
                              "virtual_monitoring_rows") &&
                    !Contains(integer_cast_result.sblr_payload,
                              "\"projection_0_value\":\"2\"") &&
                    !integer_cast_result.parser_executes_sql &&
                    !integer_cast_result.cached_storage_authority &&
                    !integer_cast_result.cached_finality_authority,
                "Firebird execution session did not preempt the legacy "
                "INTEGER CAST evaluator with neutral lowering") &&
         Expect(numeric_literal_cast_result.accepted &&
                    Contains(numeric_literal_cast_result.sblr_payload,
                             "\"projection_0_arg_0_type\":"
                             "\"numeric.fixed\"") &&
                    Contains(numeric_literal_cast_result.sblr_payload,
                             "\"projection_0_arg_1_value\":"
                             "\"character\"") &&
                    Contains(numeric_literal_cast_result.sblr_payload,
                             "\"projection_1_arg_1_value\":\"varchar\"") &&
                    Contains(numeric_literal_cast_result.sblr_payload,
                             "\"projection_2_arg_1_value\":"
                             "\"decimal(2,1)\"") &&
                    Contains(numeric_literal_cast_result.sblr_payload,
                             "\"projection_2_arg_2_value\":\"half_up\"") &&
                    Contains(numeric_literal_cast_result.sblr_payload,
                             "\"projection_3_arg_2_value\":\"half_up\"") &&
                    !Contains(numeric_literal_cast_result.sblr_payload,
                              "RDB$DATABASE") &&
                    !Contains(numeric_literal_cast_result.sblr_payload,
                              "virtual_catalog_projection") &&
                    !Contains(numeric_literal_cast_result.sblr_payload,
                              "virtual_monitoring_rows") &&
                    !Contains(numeric_literal_cast_result.sblr_payload,
                              "\"projection_2_value\":\"1.2\"") &&
                    !Contains(numeric_literal_cast_result.sblr_payload,
                              "\"projection_3_value\":\"1.3\"") &&
                    !numeric_literal_cast_result.parser_executes_sql &&
                    !numeric_literal_cast_result.cached_storage_authority &&
                    !numeric_literal_cast_result.cached_finality_authority,
                "Firebird execution session did not lower numeric-literal "
                "CAST profiles without local execution or authority") &&
         Expect(temporal_result.accepted &&
                    Contains(temporal_result.sblr_payload,
                             "\"projection_0_function_id\":"
                             "\"sb.temporal.date_diff\"") &&
                    Contains(temporal_result.sblr_payload,
                             "\"projection_1_function_id\":"
                             "\"sb.temporal.date_part\"") &&
                    Contains(temporal_result.sblr_payload,
                             "2008-12-02T13:33:33") &&
                    Contains(temporal_result.sblr_payload,
                             "2008-12-02T13:33:35") &&
                    Contains(temporal_result.sblr_payload,
                             "2008-12-30") &&
                    !Contains(temporal_result.sblr_payload, "12/02/") &&
                    !Contains(temporal_result.sblr_payload, "30.12.") &&
                    !Contains(temporal_result.sblr_payload, "RDB$DATABASE") &&
                    !temporal_result.parser_executes_sql &&
                    !temporal_result.cached_storage_authority &&
                    !temporal_result.cached_finality_authority,
                "Firebird execution session did not lower temporal QA "
                "calls without claiming engine authority") &&
         Expect(dateadd_result.accepted &&
                    Contains(dateadd_result.sblr_payload,
                             "\"projection_0_function_id\":"
                             "\"sb.temporal.date_add\"") &&
                    Contains(dateadd_result.sblr_payload,
                             "\"projection_1_function_id\":"
                             "\"sb.temporal.date_add\"") &&
                    Contains(dateadd_result.sblr_payload,
                             "\"projection_0_arg_0_type\":\"date\"") &&
                    Contains(dateadd_result.sblr_payload,
                             "\"projection_0_arg_1_value\":\"-P1Y\"") &&
                    Contains(dateadd_result.sblr_payload,
                             "\"projection_1_arg_0_type\":\"timestamp\"") &&
                    Contains(dateadd_result.sblr_payload,
                             "\"projection_1_arg_1_value\":\"-P1D\"") &&
                    Contains(dateadd_result.sblr_payload,
                             "2008-02-06T10:10:00") &&
                    !Contains(dateadd_result.sblr_payload, "RDB$DATABASE") &&
                    !Contains(dateadd_result.sblr_payload, "2008-02-05") &&
                    !Contains(dateadd_result.sblr_payload, "2007-02-06") &&
                    !dateadd_result.parser_executes_sql &&
                    !dateadd_result.cached_storage_authority &&
                    !dateadd_result.cached_finality_authority,
                "Firebird execution session did not lower DATEADD without "
                "claiming engine authority") &&
         Expect(string_result.accepted &&
                    Contains(string_result.sblr_payload,
                             "\"projection_0_function_id\":"
                             "\"sb.scalar.position\"") &&
                    Contains(string_result.sblr_payload,
                             "\"projection_1_function_id\":"
                             "\"sb.scalar.instr\"") &&
                    Contains(string_result.sblr_payload,
                             "\"projection_1_arg_0_value\":"
                             "\"beau,il fait beau\"") &&
                    Contains(string_result.sblr_payload,
                             "\"projection_1_arg_1_value\":\"beau\"") &&
                    Contains(string_result.sblr_payload,
                             "\"projection_1_arg_2_value\":\"2\"") &&
                    !Contains(string_result.sblr_payload,
                              "\"projection_0_value\":\"1\"") &&
                    !Contains(string_result.sblr_payload,
                              "\"projection_1_value\":\"14\"") &&
                    !Contains(string_result.sblr_payload, "POSITION") &&
                    !Contains(string_result.sblr_payload, "RDB$DATABASE") &&
                    !string_result.parser_executes_sql &&
                    !string_result.cached_storage_authority &&
                    !string_result.cached_finality_authority,
                "Firebird execution session did not preempt the legacy "
                "POSITION evaluator with neutral lowering") &&
         Expect(exact_temporal_result.accepted &&
                    Contains(exact_temporal_result.sblr_payload,
                             "firebird.calendar_month.v1") &&
                    Contains(exact_temporal_result.sblr_payload,
                             "firebird.ticks_100us.v1") &&
                    Contains(exact_temporal_result.sblr_payload,
                             "\"projection_0_function_id\":"
                             "\"sb.temporal.date_add\"") &&
                    Contains(exact_temporal_result.sblr_payload,
                             "\"projection_1_function_id\":"
                             "\"sb.temporal.date_add\"") &&
                    Contains(exact_temporal_result.sblr_payload,
                             "\"projection_2_function_id\":"
                             "\"sb.temporal.date_diff\"") &&
                    Contains(exact_temporal_result.sblr_payload,
                             "\"projection_3_function_id\":"
                             "\"sb.temporal.date_part\"") &&
                    !Contains(exact_temporal_result.sblr_payload,
                              "RDB$DATABASE") &&
                    !Contains(exact_temporal_result.sblr_payload,
                              "2005-01-31") &&
                    !Contains(exact_temporal_result.sblr_payload,
                              "12:11:59.9990") &&
                    !Contains(exact_temporal_result.sblr_payload,
                              "86399999.8") &&
                    !Contains(exact_temporal_result.sblr_payload,
                              "\"projection_3_value\":\"111.1\"") &&
                    !exact_temporal_result.parser_executes_sql &&
                    !exact_temporal_result.cached_storage_authority &&
                    !exact_temporal_result.cached_finality_authority,
                "Firebird exact temporal route did not preempt local "
                "evaluation without claiming engine authority");
}

}  // namespace

int main() {
  bool ok = true;
  ok = ExpectAsciiCharRoute() && ok;
  ok = ExpectAbsAndPiQaRoutes() && ok;
  ok = ExpectDirectDoubleQaRoutes() && ok;
  ok = ExpectBoundNumericQaRoutes() && ok;
  ok = ExpectBoundBinaryQaRoutes() && ok;
  ok = ExpectBoundStringLiteralQaRoutes() && ok;
  ok = ExpectBoundStringLiteralWireValidation() && ok;
  ok = ExpectBoundStringLiteralShapesFailClosed() && ok;
  ok = ExpectBoundDecimalMathCastQaRoutes() && ok;
  ok = ExpectBoundIntegerCastQaRoutes() && ok;
  ok = ExpectBoundIntegerCastShapesFailClosed() && ok;
  ok = ExpectBoundNumericLiteralCastQaRoutes() && ok;
  ok = ExpectBoundNumericLiteralCastShapesFailClosed() && ok;
  ok = ExpectBoundTemporalCastQaRoutes() && ok;
  ok = ExpectBoundTemporalCastErrorQaRoutes() && ok;
  ok = ExpectBoundTemporalCastShapesFailClosed() && ok;
  ok = ExpectBoundDateDiffQaRoutes() && ok;
  ok = ExpectBoundExtractWeekQaRoute() && ok;
  ok = ExpectBoundDateAddQaRoutes() && ok;
  ok = ExpectBoundDateAddRemainingQaRoutes() && ok;
  ok = ExpectBoundMillisecondTemporalQaRoutes() && ok;
  ok = ExpectDateAddWirePresentationValidation() && ok;
  ok = ExpectRemainingTemporalWirePresentationValidation() && ok;
  ok = ExpectExactScalarWireValidation() && ok;
  ok = ExpectAsciiValQaRoute() && ok;
  ok = ExpectCore3227Route() && ok;
  ok = ExpectCore3479Route() && ok;
  ok = ExpectUnsupportedShapesFailClosed() && ok;
  ok = ExpectExecutionSessionIntegration() && ok;
  return ok ? 0 : 1;
}
