// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

constexpr std::string_view kOctetFromInt64 =
    "data.scalar.octet_from_int64";
constexpr std::string_view kInt64FromFirstOctet =
    "data.scalar.int64_from_first_octet";
constexpr std::string_view kRound = "sb.scalar.round";
constexpr std::string_view kCast = "data.scalar.cast";
constexpr std::string_view kAcos = "sb.scalar.acos";
constexpr std::string_view kCot = "sb.scalar.cot";
constexpr std::string_view kSin = "sb.scalar.sin";
constexpr std::string_view kDateAdd = "sb.temporal.date_add";
constexpr std::string_view kDateDiff = "sb.temporal.date_diff";
constexpr std::string_view kDatePart = "sb.temporal.date_part";
constexpr std::string_view kBitAnd = "sb.scalar.bit_and";
constexpr std::string_view kBitOr = "sb.scalar.bit_or";
constexpr std::string_view kBitXor = "sb.scalar.bit_xor";
constexpr std::string_view kBitShiftLeft = "sb.scalar.bit_shift_left";
constexpr std::string_view kBitShiftRight = "sb.scalar.bit_shift_right";
constexpr std::string_view kLeft = "sb.scalar.left";
constexpr std::string_view kRight = "sb.scalar.right";
constexpr std::string_view kOverlay = "sb.scalar.overlay";
constexpr std::string_view kPosition = "sb.scalar.position";
constexpr std::string_view kInstr = "sb.scalar.instr";
constexpr std::string_view kReplace = "sb.scalar.replace";
constexpr std::string_view kReverse = "sb.scalar.reverse";
constexpr std::string_view kHash64 = "data.scalar.hash64";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

struct Expression {
  std::string type;
  std::string value;
  bool is_null = false;
  std::string function_id;
  std::vector<Expression> arguments;
};

Expression Literal(std::string type,
                   std::string value,
                   bool is_null = false) {
  return Expression{std::move(type), std::move(value), is_null, {}, {}};
}

Expression Function(std::string type,
                    std::string function_id,
                    std::vector<Expression> arguments) {
  return Expression{std::move(type), {}, false, std::move(function_id),
                    std::move(arguments)};
}

void AppendTextOperand(sblr::SblrOperationEnvelope* envelope,
                       std::string name,
                       std::string value) {
  Require(envelope != nullptr, "projection envelope is required");
  sblr::SblrOperand operand;
  operand.ordinal =
      static_cast<std::uint32_t>(envelope->operands.size() + 1u);
  operand.type = "text";
  operand.name = std::move(name);
  operand.value_kind = sblr::SblrValueKind::literal_typed;
  operand.value_body.assign(24, 0);
  // Exact statement-owned scalar text descriptor identity for this component
  // carrier. The projection evaluator consumes the canonical value bytes only
  // after SBOP validation.
  operand.value_body[0] = 0x01;
  operand.value_body[1] = 0x9f;
  operand.value_body[6] = 0x70;
  operand.value_body[8] = 0x80;
  operand.value_body[15] = 0x01;
  const auto size = static_cast<std::uint64_t>(value.size());
  for (unsigned byte = 0; byte < 8; ++byte) {
    operand.value_body[16 + byte] =
        static_cast<std::uint8_t>(size >> (byte * 8u));
  }
  operand.value_body.insert(operand.value_body.end(), value.begin(),
                            value.end());
  envelope->operands.push_back(std::move(operand));
}

void AppendExpression(sblr::SblrOperationEnvelope* envelope,
                      const std::string& prefix,
                      const Expression& expression) {
  Require(envelope != nullptr, "projection envelope is required");
  const bool is_function = !expression.function_id.empty();
  AppendTextOperand(envelope, prefix + "expr_kind",
                    is_function ? "function" : "literal");
  AppendTextOperand(envelope, prefix + "type", expression.type);
  AppendTextOperand(envelope, prefix + "value", expression.value);
  AppendTextOperand(envelope, prefix + "is_null",
                    expression.is_null ? "true" : "false");
  if (!is_function) return;

  AppendTextOperand(envelope, prefix + "function_id", expression.function_id);
  AppendTextOperand(envelope, prefix + "function_arg_count",
                    std::to_string(expression.arguments.size()));
  for (std::size_t index = 0; index < expression.arguments.size(); ++index) {
    AppendExpression(envelope,
                     prefix + "arg_" + std::to_string(index) + "_",
                     expression.arguments[index]);
  }
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sblr-scalar-octet-projection";
  context.database_uuid.canonical =
      "019f0000-0000-70c1-8a00-00000000b001";
  context.node_uuid.canonical =
      "019f0000-0000-70c1-8a00-00000000b002";
  context.session_uuid.canonical =
      "019f0000-0000-70c1-8a00-00000000b003";
  context.principal_uuid.canonical =
      "019f0000-0000-70c1-8a00-00000000b004";
  context.transaction_uuid.canonical =
      "019f0000-0000-70c1-8a00-00000000b005";
  context.statement_uuid.canonical =
      "019f0000-0000-70c1-8a00-00000000b006";
  context.local_transaction_id = 73;
  context.snapshot_visible_through_local_transaction_id = 73;
  context.transaction_isolation_level = "snapshot";
  context.security_context_present = true;
  return context;
}

sblr::SblrDispatchResult Dispatch(
    const std::vector<std::pair<std::string, Expression>>& projections) {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.evaluate_projection", "SBLR_QUERY_EVALUATE_PROJECTION",
      "SBLR-SCALAR-OCTET-PROJECTION-CONFORMANCE");
  envelope.opcode_code = 0x040eu;
  envelope.result_shape = "scalar_projection_row_v1";
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.parser_package_uuid =
      "019f0000-0000-70c1-8a00-00000000b101";
  envelope.registry_snapshot_uuid =
      "019f0000-0000-70c1-8a00-00000000b102";
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  AppendTextOperand(&envelope, "projection_count",
                    std::to_string(projections.size()));
  for (std::size_t index = 0; index < projections.size(); ++index) {
    const std::string prefix = "projection_" + std::to_string(index) + "_";
    AppendTextOperand(&envelope, prefix + "name", projections[index].first);
    AppendExpression(&envelope, prefix, projections[index].second);
  }

  sblr::SblrDispatchRequest request;
  request.context = EngineContext();
  request.envelope = std::move(envelope);
  return sblr::DispatchSblrOperation(std::move(request));
}

bool HasApiDiagnostic(const sblr::SblrDispatchResult& result,
                      std::string_view code) {
  for (const auto& diagnostic : result.api_result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasEvidence(const sblr::SblrDispatchResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.api_result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) {
      return true;
    }
  }
  return false;
}

void RequireSuccessfulProjection(
    const sblr::SblrDispatchResult& result,
    std::size_t expected_columns) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated && result.accepted &&
              result.dispatched_to_api && result.api_result.ok,
          "neutral scalar octet projection did not execute in the engine");
  Require(result.api_result.result_shape.result_kind ==
              "scalar_projection_rows" &&
              result.api_result.result_shape.rows.size() == 1 &&
              result.api_result.result_shape.rows.front().fields.size() ==
                  expected_columns,
          "neutral scalar octet projection result shape drifted");
}

void RequireFailedBeforeRow(
    const Expression& expression,
    std::string_view expected_diagnostic = "SB_DIAG_FUNCTION_INVALID_INPUT") {
  const auto result = Dispatch({{"invalid_value", expression}});
  Require(result.envelope_validated && result.accepted &&
              result.dispatched_to_api && !result.api_result.ok,
          "invalid neutral scalar projection did not fail in engine execution");
  if (!HasApiDiagnostic(result, expected_diagnostic)) {
    std::cerr << "projection expression type=" << expression.type
              << " function=" << expression.function_id << '\n';
    for (std::size_t index = 0; index < expression.arguments.size(); ++index) {
      std::cerr << "argument[" << index << "] type="
                << expression.arguments[index].type << " value="
                << expression.arguments[index].value << '\n';
    }
    for (const auto& diagnostic : result.api_result.diagnostics) {
      std::cerr << "unexpected diagnostic " << diagnostic.code << ':'
                << diagnostic.detail << '\n';
    }
  }
  Require(HasApiDiagnostic(result, expected_diagnostic),
          "invalid neutral scalar projection diagnostic drifted");
  Require(result.api_result.result_shape.rows.empty(),
          "invalid neutral scalar projection returned a partial row");
}

void RequireConversionInputFailure(const Expression& expression,
                                   std::string_view expected_input) {
  const auto result = Dispatch({{"conversion_failure", expression}});
  Require(result.envelope_validated && result.accepted &&
              result.dispatched_to_api && !result.api_result.ok,
          "invalid conversion did not fail in neutral engine execution");
  const api::EngineApiDiagnostic* conversion = nullptr;
  for (const auto& diagnostic : result.api_result.diagnostics) {
    if (diagnostic.code == "SB_DIAG_FUNCTION_CONVERSION_INPUT") {
      Require(conversion == nullptr,
              "conversion failure emitted duplicate engine diagnostics");
      conversion = &diagnostic;
    }
  }
  Require(conversion != nullptr,
          "conversion failure diagnostic code drifted");
  std::size_t matching_fields = 0;
  for (const auto& field : conversion->fields) {
    if (field.key == "conversion_input_text") {
      ++matching_fields;
      Require(field.value == expected_input,
              "conversion failure structured input drifted");
    }
  }
  Require(matching_fields == 1,
          "conversion failure requires one structured input field");
  Require(result.api_result.result_shape.rows.empty(),
          "conversion failure returned a partial row");
}

void RequireGenericInvalidInputWithoutConversionField(
    const Expression& expression) {
  const auto result = Dispatch({{"generic_invalid", expression}});
  Require(result.envelope_validated && result.accepted &&
              result.dispatched_to_api && !result.api_result.ok &&
              HasApiDiagnostic(result, "SB_DIAG_FUNCTION_INVALID_INPUT"),
          "generic invalid-input diagnostic drifted");
  for (const auto& diagnostic : result.api_result.diagnostics) {
    Require(diagnostic.code != "SB_DIAG_FUNCTION_CONVERSION_INPUT",
            "generic invalid input was reclassified as conversion input");
    for (const auto& field : diagnostic.fields) {
      Require(field.key != "conversion_input_text",
              "generic invalid input leaked conversion presentation field");
    }
  }
  Require(result.api_result.result_shape.rows.empty(),
          "generic invalid input returned a partial row");
}

Expression ProfiledDateAdd(std::string temporal_type,
                           std::string temporal_value,
                           std::string unit,
                           std::string amount,
                           std::string profile,
                           std::string amount_type = "int64",
                           std::string function_id = std::string(kDateAdd)) {
  std::string result_type = temporal_type;
  return Function(
      std::move(result_type), std::move(function_id),
      {Literal(std::move(temporal_type), std::move(temporal_value)),
       Literal("text", std::move(unit)),
       Literal(std::move(amount_type), std::move(amount)),
       Literal("text", std::move(profile))});
}

Expression ProfiledDateDiff(std::string temporal_type,
                            std::string start,
                            std::string finish,
                            std::string unit = "millisecond",
                            std::string profile =
                                "firebird.ticks_100us.v1") {
  return Function(
      "numeric.fixed", std::string(kDateDiff),
      {Literal("text", std::move(unit)),
       Literal(temporal_type, std::move(start)),
       Literal(std::move(temporal_type), std::move(finish)),
       Literal("text", std::move(profile))});
}

Expression ProfiledDatePart(std::string temporal_type,
                            std::string temporal_value,
                            std::string field = "millisecond",
                            std::string profile =
                                "firebird.ticks_100us.v1") {
  return Function(
      "numeric.fixed", std::string(kDatePart),
      {Literal("text", std::move(field)),
       Literal(std::move(temporal_type), std::move(temporal_value)),
       Literal("text", std::move(profile))});
}

}  // namespace

int main() {
  const auto valid = Dispatch({
      {"octet_a",
       Function("binary", std::string(kOctetFromInt64),
                {Literal("int64", "65")})},
      {"octet_zero",
       Function("binary", std::string(kOctetFromInt64),
                {Literal("int64", "0")})},
      {"octet_ff",
       Function("binary", std::string(kOctetFromInt64),
                {Literal("int64", "255")})},
      {"first_a",
       Function("int64", std::string(kInt64FromFirstOctet),
                {Literal("character.none", "A")})},
      {"first_utf8_octet",
       Function("int64", std::string(kInt64FromFirstOctet),
                {Literal("character.none", "\xc3\x83")})},
      {"first_utf8_ascii",
       Function("int64", std::string(kInt64FromFirstOctet),
                {Literal("character.utf8", "Hopl\xc3\xa4la")})},
      {"first_blob_octet",
       Function("int64", std::string(kInt64FromFirstOctet),
                {Literal("blob.binary", "A")})},
      {"first_binary_octet",
       Function("int64", std::string(kInt64FromFirstOctet),
                {Literal("binary", "ff")})},
      {"empty_octet",
       Function("int64", std::string(kInt64FromFirstOctet),
                {Literal("character.none", "")})},
      {"null_octet",
       Function("int64", std::string(kInt64FromFirstOctet),
                {Literal("character.utf8", "", true)})},
      {"nested_zero",
       Function("int64", std::string(kInt64FromFirstOctet),
                {Function("binary", std::string(kOctetFromInt64),
                          {Literal("int64", "0")})})},
      {"nested_null",
       Function("int64", std::string(kInt64FromFirstOctet),
                {Function("binary", std::string(kOctetFromInt64),
                          {Literal("int64", "", true)})})},
      {"round_fixed_qa",
       Function("numeric.fixed", std::string(kRound),
                {Literal("numeric.fixed", "5.7778"),
                 Literal("int64", "3")})},
      {"round_fixed_positive_tie",
       Function("numeric.fixed", std::string(kRound),
                {Literal("numeric.fixed", "1.25"),
                 Literal("int64", "1")})},
      {"round_fixed_negative_tie",
       Function("numeric.fixed", std::string(kRound),
                {Literal("numeric.fixed", "-1.25"),
                 Literal("int64", "1")})},
      {"round_fixed_noop",
       Function("numeric.fixed", std::string(kRound),
                {Literal("numeric.fixed", "5.7778"),
                 Literal("int64", "10")})},
      {"round_real64_legacy",
       Function("real64", std::string(kRound),
                {Literal("real64", "5.7778"),
                 Literal("int64", "3")})},
      {"firebird_decimal_acos",
       Function("decimal", std::string(kCast),
                {Function("real64", std::string(kAcos),
                          {Literal("int64", "1")}),
                 Literal("text", "decimal(18,15)"),
                 Literal("text", "half_up")})},
      {"firebird_decimal_cot",
       Function("decimal", std::string(kCast),
                {Function("real64", std::string(kCot),
                          {Literal("int64", "1")}),
                 Literal("text", "decimal(18,15)"),
                 Literal("text", "half_up")})},
      {"firebird_decimal_sin",
       Function("decimal", std::string(kCast),
                {Function("real64", std::string(kSin),
                          {Literal("int64", "12")}),
                 Literal("text", "decimal(18,15)"),
                 Literal("text", "half_up")})},
      {"decimal_positive_tie_half_even",
       Function("decimal", std::string(kCast),
                {Literal("numeric.fixed", "1.25"),
                 Literal("text", "decimal(2,1)")})},
      {"decimal_negative_tie_half_even",
       Function("decimal", std::string(kCast),
                {Literal("numeric.fixed", "-1.25"),
                 Literal("text", "decimal(2,1)")})},
      {"decimal_positive_tie_half_up",
       Function("decimal", std::string(kCast),
                {Literal("numeric.fixed", "1.25"),
                 Literal("text", "decimal(2,1)"),
                 Literal("text", "half_up")})},
      {"decimal_negative_tie_half_up",
       Function("decimal", std::string(kCast),
                {Literal("numeric.fixed", "-1.25"),
                 Literal("text", "decimal(2,1)"),
                 Literal("text", "half_up")})},
      {"decimal_null_source",
       Function("decimal", std::string(kCast),
                {Literal("numeric.fixed", "", true),
                 Literal("text", "decimal(18,15)"),
                 Literal("text", "half_up")})},
      {"firebird_datediff_second",
       Function("int64", std::string(kDateDiff),
                {Literal("text", "second"),
                 Literal("timestamp", "2008-12-02T13:33:33"),
                 Literal("timestamp", "2008-12-02T13:33:35")})},
      {"firebird_datediff_minute",
       Function("int64", std::string(kDateDiff),
                {Literal("text", "minute"),
                 Literal("timestamp", "2008-12-02T13:33:33"),
                 Literal("timestamp", "2008-12-02T13:34:35")})},
      {"firebird_datediff_hour",
       Function("int64", std::string(kDateDiff),
                {Literal("text", "hour"),
                 Literal("timestamp", "2008-12-02T13:33:33"),
                 Literal("timestamp", "2008-12-02T14:34:35")})},
      {"firebird_datediff_year",
       Function("int64", std::string(kDateDiff),
                {Literal("text", "year"),
                 Literal("timestamp", "2008-12-02T13:33:33"),
                 Literal("timestamp", "2009-12-02T13:34:35")})},
      {"firebird_datediff_month",
       Function("int64", std::string(kDateDiff),
                {Literal("text", "month"),
                 Literal("timestamp", "2008-12-02T13:33:33"),
                 Literal("timestamp", "2009-12-02T13:34:35")})},
      {"firebird_datediff_day",
       Function("int64", std::string(kDateDiff),
                {Literal("text", "day"),
                 Literal("timestamp", "2008-12-02T13:33:33"),
                 Literal("timestamp", "2009-12-02T13:34:35")})},
      {"firebird_extract_week_2008",
       Function("int64", std::string(kDatePart),
                {Literal("text", "week"),
                 Literal("date", "2008-12-30")})},
      {"firebird_extract_week_2009",
       Function("int64", std::string(kDatePart),
                {Literal("text", "week"),
                 Literal("date", "2009-12-30")})},
      {"firebird_dateadd_date_day_expanded",
       Function("date", std::string(kDateAdd),
                {Literal("date", "2008-02-06"),
                 Literal("interval", "-P1D")})},
      {"firebird_dateadd_date_day_comma",
       Function("date", std::string(kDateAdd),
                {Literal("date", "2008-02-06"),
                 Literal("interval", "-P1D")})},
      {"firebird_dateadd_date_year_expanded",
       Function("date", std::string(kDateAdd),
                {Literal("date", "2008-02-06"),
                 Literal("interval", "-P1Y")})},
      {"firebird_dateadd_date_year_comma",
       Function("date", std::string(kDateAdd),
                {Literal("date", "2008-02-06"),
                 Literal("interval", "-P1Y")})},
      {"firebird_dateadd_timestamp_day_expanded",
       Function("timestamp", std::string(kDateAdd),
                {Literal("timestamp", "2008-02-06T10:10:00"),
                 Literal("interval", "-P1D")})},
      {"firebird_dateadd_timestamp_day_comma",
       Function("timestamp", std::string(kDateAdd),
                {Literal("timestamp", "2008-02-06T10:10:00"),
                 Literal("interval", "-P1D")})},
  });
  RequireSuccessfulProjection(valid, 39);

  const auto& fields = valid.api_result.result_shape.rows.front().fields;
  const auto require_binary_octet = [&](std::size_t index,
                                        unsigned expected,
                                        std::string_view expected_hex) {
    const auto& value = fields[index].second;
    Require(value.descriptor.canonical_type_name == "binary" &&
                !value.is_null && value.encoded_value == expected_hex &&
                value.binary_value.size() == 1 &&
                value.binary_value.front() == expected,
            "octet_from_int64 projection value drifted");
  };
  const auto require_int64 = [&](std::size_t index, std::string_view expected) {
    const auto& value = fields[index].second;
    Require(value.descriptor.canonical_type_name == "int64" &&
                !value.is_null && value.encoded_value == expected,
            "int64_from_first_octet projection value drifted");
  };

  require_binary_octet(0, 65, "41");
  require_binary_octet(1, 0, "00");
  require_binary_octet(2, 255, "ff");
  require_int64(3, "65");
  require_int64(4, "195");
  require_int64(5, "72");
  require_int64(6, "65");
  require_int64(7, "255");
  require_int64(8, "0");
  Require(fields[9].second.descriptor.canonical_type_name == "int64" &&
              fields[9].second.is_null,
          "first-octet NULL propagation drifted");
  require_int64(10, "0");
  Require(fields[11].second.descriptor.canonical_type_name == "int64" &&
              fields[11].second.is_null,
          "nested octet NULL propagation drifted");
  const auto require_fixed = [&](std::size_t index,
                                 std::string_view expected) {
    const auto& value = fields[index].second;
    Require(value.descriptor.canonical_type_name == "numeric.fixed" &&
                !value.is_null && value.encoded_value == expected,
            "exact fixed ROUND projection value drifted");
  };
  require_fixed(12, "5.7780");
  require_fixed(13, "1.30");
  require_fixed(14, "-1.30");
  require_fixed(15, "5.7778");
  Require(fields[16].second.descriptor.canonical_type_name == "real64" &&
              !fields[16].second.is_null &&
              std::abs(std::stod(fields[16].second.encoded_value) - 5.778) <
                  1e-12,
          "ordinary real64 ROUND behavior drifted");

  const auto require_decimal = [&](std::size_t index,
                                   std::string_view expected) {
    const auto& value = fields[index].second;
    Require(value.descriptor.canonical_type_name == "decimal" &&
                !value.is_null && value.encoded_value == expected,
            "exact DECIMAL cast projection value drifted");
  };
  require_decimal(17, "0.000000000000000");
  require_decimal(18, "0.642092615934331");
  require_decimal(19, "-0.536572918000435");
  require_decimal(20, "1.2");
  require_decimal(21, "-1.2");
  require_decimal(22, "1.3");
  require_decimal(23, "-1.3");
  Require(fields[24].second.descriptor.canonical_type_name == "decimal" &&
              fields[24].second.is_null,
          "DECIMAL cast NULL propagation drifted");
  require_int64(25, "2");
  require_int64(26, "1");
  require_int64(27, "1");
  require_int64(28, "1");
  require_int64(29, "12");
  require_int64(30, "365");
  require_int64(31, "1");
  require_int64(32, "53");
  const auto require_temporal = [&](std::size_t index,
                                    std::string_view expected_type,
                                    std::string_view expected_value) {
    const auto& value = fields[index].second;
    Require(value.descriptor.canonical_type_name == expected_type &&
                !value.is_null && value.encoded_value == expected_value,
            "DATEADD engine temporal result drifted");
  };
  require_temporal(33, "date", "2008-02-05");
  require_temporal(34, "date", "2008-02-05");
  require_temporal(35, "date", "2007-02-06");
  require_temporal(36, "date", "2007-02-06");
  require_temporal(37, "timestamp", "2008-02-05T10:10:00");
  require_temporal(38, "timestamp", "2008-02-05T10:10:00");

  Require(HasEvidence(valid, "function_runtime", kCast) &&
              HasEvidence(valid, "function_runtime", kAcos) &&
              HasEvidence(valid, "function_runtime", kCot) &&
              HasEvidence(valid, "function_runtime", kSin) &&
              HasEvidence(valid, "function_runtime", kDateAdd) &&
              HasEvidence(valid, "function_runtime", kDateDiff) &&
              HasEvidence(valid, "function_runtime", kDatePart),
          "nested scalar function runtime evidence was not preserved");

  const auto firebird_profiled_temporal = Dispatch({
      {"leap_jan_31_plus_01_month",
       ProfiledDateAdd("date", "2004-01-31", "month", "1",
                       "firebird.calendar_month.v1")},
      {"leap_feb_28_plus_01_month",
       ProfiledDateAdd("date", "2004-02-28", "month", "1",
                       "firebird.calendar_month.v1")},
      {"leap_feb_29_plus_01_month",
       ProfiledDateAdd("date", "2004-02-29", "month", "1",
                       "firebird.calendar_month.v1")},
      {"leap_feb_28_minus_01_month",
       ProfiledDateAdd("date", "2004-02-28", "month", "-1",
                       "firebird.calendar_month.v1")},
      {"leap_feb_29_minus_01_month",
       ProfiledDateAdd("date", "2004-02-29", "month", "-1",
                       "firebird.calendar_month.v1")},
      {"leap_feb_28_plus_11_month",
       ProfiledDateAdd("date", "2004-02-28", "month", "11",
                       "firebird.calendar_month.v1")},
      {"leap_feb_29_plus_11_month",
       ProfiledDateAdd("date", "2004-02-29", "month", "11",
                       "firebird.calendar_month.v1")},
      {"leap_feb_28_plus_12_month",
       ProfiledDateAdd("date", "2004-02-28", "month", "12",
                       "firebird.calendar_month.v1")},
      {"leap_feb_29_plus_12_month",
       ProfiledDateAdd("date", "2004-02-29", "month", "12",
                       "firebird.calendar_month.v1")},
      {"leap_feb_28_minus_11_month",
       ProfiledDateAdd("date", "2004-02-28", "month", "-11",
                       "firebird.calendar_month.v1")},
      {"leap_feb_29_minus_11_month",
       ProfiledDateAdd("date", "2004-02-29", "month", "-11",
                       "firebird.calendar_month.v1")},
      {"leap_feb_28_minus_12_month",
       ProfiledDateAdd("date", "2004-02-28", "month", "-12",
                       "firebird.calendar_month.v1")},
      {"leap_feb_29_minus_12_month",
       ProfiledDateAdd("date", "2004-02-29", "month", "-12",
                       "firebird.calendar_month.v1")},
      {"leap_mar_31_minus_01_month",
       ProfiledDateAdd("date", "2004-03-31", "month", "-1",
                       "firebird.calendar_month.v1")},
      {"nonleap_jan_31_plus_01_month",
       ProfiledDateAdd("date", "2003-01-31", "month", "1",
                       "firebird.calendar_month.v1")},
      {"nonleap_feb_28_plus_01_month",
       ProfiledDateAdd("date", "2003-02-28", "month", "1",
                       "firebird.calendar_month.v1")},
      {"nonleap_feb_28_minus_01_month",
       ProfiledDateAdd("date", "2003-02-28", "month", "-1",
                       "firebird.calendar_month.v1")},
      {"nonleap_feb_28_plus_11_month",
       ProfiledDateAdd("date", "2003-02-28", "month", "11",
                       "firebird.calendar_month.v1")},
      {"nonleap_feb_28_plus_12_month",
       ProfiledDateAdd("date", "2003-02-28", "month", "12",
                       "firebird.calendar_month.v1")},
      {"nonleap_feb_28_minus_11_month",
       ProfiledDateAdd("date", "2003-02-28", "month", "-11",
                       "firebird.calendar_month.v1")},
      {"nonleap_feb_28_minus_12_month",
       ProfiledDateAdd("date", "2003-02-28", "month", "-12",
                       "firebird.calendar_month.v1")},
      {"nonleap_mar_31_minus_01_month",
       ProfiledDateAdd("date", "2003-03-31", "month", "-1",
                       "firebird.calendar_month.v1")},
      {"time_minus_hour",
       ProfiledDateAdd("time", "12:12:00", "hour", "-1",
                       "firebird.ticks_100us.v1")},
      {"time_minus_minute",
       ProfiledDateAdd("time", "12:12:00", "minute", "-1",
                       "firebird.ticks_100us.v1")},
      {"time_minus_second",
       ProfiledDateAdd("time", "12:12:00", "second", "-1",
                       "firebird.ticks_100us.v1")},
      {"time_minus_millisecond_colon_fraction",
       ProfiledDateAdd("time", "12:12:00:0000", "millisecond", "-1",
                       "firebird.ticks_100us.v1")},
      {"time_modulo_previous_day",
       ProfiledDateAdd("time", "00:00:00.0000", "millisecond", "-1",
                       "firebird.ticks_100us.v1")},
      {"datediff_timestamp_millisecond",
       ProfiledDateDiff("timestamp", "0001-01-01T00:00:00.0001",
                        "9999-12-31T23:59:59.9999")},
      {"datediff_time_millisecond",
       ProfiledDateDiff("time", "00:00:00.0001",
                        "23:59:59.9999")},
      {"extract_time_millisecond",
       ProfiledDatePart("time", "12:12:00.1111")},
      {"extract_timestamp_millisecond",
       ProfiledDatePart("timestamp", "2008-12-08T12:12:00.1111")},
      {"extract_nonzero_second_millisecond",
       ProfiledDatePart("time", "12:12:59.1111")},
  });
  RequireSuccessfulProjection(firebird_profiled_temporal, 32);
  const auto& profiled_fields =
      firebird_profiled_temporal.api_result.result_shape.rows.front().fields;
  const std::vector<std::string_view> expected_profiled_dates = {
      "2004-02-29", "2004-03-28", "2004-03-29", "2004-01-28",
      "2004-01-29", "2005-01-28", "2005-01-31", "2005-02-28",
      "2005-02-28", "2003-03-28", "2003-03-29", "2003-02-28",
      "2003-02-28", "2004-02-29", "2003-02-28", "2003-03-28",
      "2003-01-28", "2004-01-28", "2004-02-28", "2002-03-28",
      "2002-02-28", "2003-02-28"};
  for (std::size_t index = 0; index < expected_profiled_dates.size();
       ++index) {
    Require(profiled_fields[index].second.descriptor.canonical_type_name ==
                    "date" &&
                !profiled_fields[index].second.is_null &&
                profiled_fields[index].second.encoded_value ==
                    expected_profiled_dates[index],
            "profiled Firebird calendar MONTH result drifted");
  }
  const std::vector<std::string_view> expected_profiled_times = {
      "11:12:00.0000", "12:11:00.0000", "12:11:59.0000",
      "12:11:59.9990", "23:59:59.9990"};
  for (std::size_t offset = 0; offset < expected_profiled_times.size();
       ++offset) {
    const std::size_t index = expected_profiled_dates.size() + offset;
    Require(profiled_fields[index].second.descriptor.canonical_type_name ==
                    "time" &&
                !profiled_fields[index].second.is_null &&
                profiled_fields[index].second.encoded_value ==
                    expected_profiled_times[offset],
            "profiled Firebird exact TIME result drifted");
  }
  const auto require_profiled_fixed =
      [&](std::size_t index, std::string_view expected) {
        const auto& value = profiled_fields[index].second;
        Require(value.descriptor.canonical_type_name == "numeric.fixed" &&
                    !value.is_null && value.encoded_value == expected,
                "profiled Firebird exact numeric result drifted");
      };
  require_profiled_fixed(27, "315537897599999.8");
  require_profiled_fixed(28, "86399999.8");
  require_profiled_fixed(29, "111.1");
  require_profiled_fixed(30, "111.1");
  require_profiled_fixed(31, "111.1");

  Require(HasEvidence(firebird_profiled_temporal, "function_runtime",
                      kDateAdd) &&
              HasEvidence(firebird_profiled_temporal, "function_runtime",
                          kDateDiff) &&
              HasEvidence(firebird_profiled_temporal, "function_runtime",
                          kDatePart),
          "profiled temporal engine execution evidence was not preserved");

  // The five canonical Firebird BIN_* files contain ten statements in total:
  // 2 AND, 3 OR, 3 XOR, 1 SHL, and 1 SHR.
  const auto firebird_binary_scalar = Dispatch({
      {"bin_and_1_1",
       Function("int64", std::string(kBitAnd),
                {Literal("int64", "1"), Literal("int64", "1")})},
      {"bin_and_1_0",
       Function("int64", std::string(kBitAnd),
                {Literal("int64", "1"), Literal("int64", "0")})},
      {"bin_or_1_1",
       Function("int64", std::string(kBitOr),
                {Literal("int64", "1"), Literal("int64", "1")})},
      {"bin_or_1_0",
       Function("int64", std::string(kBitOr),
                {Literal("int64", "1"), Literal("int64", "0")})},
      {"bin_or_0_0",
       Function("int64", std::string(kBitOr),
                {Literal("int64", "0"), Literal("int64", "0")})},
      {"bin_xor_0_1",
       Function("int64", std::string(kBitXor),
                {Literal("int64", "0"), Literal("int64", "1")})},
      {"bin_xor_0_0",
       Function("int64", std::string(kBitXor),
                {Literal("int64", "0"), Literal("int64", "0")})},
      {"bin_xor_1_1",
       Function("int64", std::string(kBitXor),
                {Literal("int64", "1"), Literal("int64", "1")})},
      {"bin_shl_8_1",
       Function("int64", std::string(kBitShiftLeft),
                {Literal("int64", "8"), Literal("int64", "1")})},
      {"bin_shr_8_1",
       Function("int64", std::string(kBitShiftRight),
                {Literal("int64", "8"), Literal("int64", "1")})},
  });
  RequireSuccessfulProjection(firebird_binary_scalar, 10);
  const auto& binary_fields =
      firebird_binary_scalar.api_result.result_shape.rows.front().fields;
  const std::vector<std::string_view> expected_binary_values = {
      "1", "0", "1", "1", "0", "1", "0", "0", "16", "4"};
  for (std::size_t index = 0; index < expected_binary_values.size();
       ++index) {
    const auto& value = binary_fields[index].second;
    Require(value.descriptor.canonical_type_name == "int64" &&
                !value.is_null &&
                value.encoded_value == expected_binary_values[index],
            "canonical Firebird BIN_* neutral engine result drifted");
  }
  Require(HasEvidence(firebird_binary_scalar, "function_runtime", kBitAnd) &&
              HasEvidence(firebird_binary_scalar, "function_runtime", kBitOr) &&
              HasEvidence(firebird_binary_scalar, "function_runtime", kBitXor) &&
              HasEvidence(firebird_binary_scalar, "function_runtime",
                          kBitShiftLeft) &&
              HasEvidence(firebird_binary_scalar, "function_runtime",
                          kBitShiftRight),
          "canonical Firebird BIN_* function evidence was not preserved");

  // The six canonical Firebird literal-string QA files contain seven SELECT
  // statements and eight output columns. The three-argument POSITION form is
  // normalized by the standalone parser to INSTR(string, substring, start),
  // while every value operation remains owned by these neutral engine calls.
  const auto firebird_literal_string_scalar = Dispatch({
      {"left_bonjour_3",
       Function("character", std::string(kLeft),
                {Literal("character", "bonjour"), Literal("int64", "3")})},
      {"right_nord_pas_de_calais_13",
       Function("character", std::string(kRight),
                {Literal("character", "NORD PAS DE CALAIS"),
                 Literal("int64", "13")})},
      {"overlay_nord",
       Function("character", std::string(kOverlay),
                {Literal("character",
                         "il fait beau dans le sud  de la france"),
                 Literal("character", "NORD"), Literal("int64", "22"),
                 Literal("int64", "4")})},
      {"position_beau_in_sentence",
       Function("int64", std::string(kPosition),
                {Literal("character", "beau"),
                 Literal("character", "il fait beau dans le nord")})},
      {"position_beau_at_start",
       Function("int64", std::string(kPosition),
                {Literal("character", "beau"),
                 Literal("character", "beau,il fait beau")})},
      {"position_beau_from_2",
       Function("int64", std::string(kInstr),
                {Literal("character", "beau,il fait beau"),
                 Literal("character", "beau"), Literal("int64", "2")})},
      {"replace_toto_o_i",
       Function("character", std::string(kReplace),
                {Literal("character", "toto"), Literal("character", "o"),
                 Literal("character", "i")})},
      {"reverse_dron",
       Function("character", std::string(kReverse),
                {Literal("character", "DRON")})},
  });
  RequireSuccessfulProjection(firebird_literal_string_scalar, 8);
  const auto& string_fields =
      firebird_literal_string_scalar.api_result.result_shape.rows.front().fields;
  const auto require_character = [&](std::size_t index,
                                     std::string_view expected) {
    const auto& value = string_fields[index].second;
    Require(value.descriptor.canonical_type_name == "character" &&
                !value.is_null && value.encoded_value == expected,
            "canonical Firebird literal-string engine result drifted");
  };
  const auto require_string_int64 = [&](std::size_t index,
                                        std::string_view expected) {
    const auto& value = string_fields[index].second;
    Require(value.descriptor.canonical_type_name == "int64" &&
                !value.is_null && value.encoded_value == expected,
            "canonical Firebird string-position engine result drifted");
  };
  require_character(0, "bon");
  require_character(1, "PAS DE CALAIS");
  require_character(2, "il fait beau dans le NORD de la france");
  require_string_int64(3, "9");
  require_string_int64(4, "1");
  require_string_int64(5, "14");
  require_character(6, "titi");
  require_character(7, "NORD");
  Require(HasEvidence(firebird_literal_string_scalar, "function_runtime",
                      kLeft) &&
              HasEvidence(firebird_literal_string_scalar, "function_runtime",
                          kRight) &&
              HasEvidence(firebird_literal_string_scalar, "function_runtime",
                          kOverlay) &&
              HasEvidence(firebird_literal_string_scalar, "function_runtime",
                          kPosition) &&
              HasEvidence(firebird_literal_string_scalar, "function_runtime",
                          kInstr) &&
              HasEvidence(firebird_literal_string_scalar, "function_runtime",
                          kReplace) &&
              HasEvidence(firebird_literal_string_scalar, "function_runtime",
                          kReverse),
          "canonical Firebird literal-string function evidence was not preserved");

  const auto firebird_profiled_hash = Dispatch({
      {"firebird_hash_toto",
       Function("int64", std::string(kHash64),
                {Literal("character", "toto"),
                 Literal("text", "firebird.weak_hash.v1")})},
      {"firebird_hash_utf8_bytes",
       Function("int64", std::string(kHash64),
                {Literal("character", "\xc3\xa9"),
                 Literal("text", "firebird.weak_hash.v1")})},
      {"firebird_hash_high_nibble_fold",
       Function("int64", std::string(kHash64),
                {Literal("character", "abcdefghijklmno"),
                 Literal("text", "firebird.weak_hash.v1")})},
      {"generic_hash_toto",
       Function("uint64", std::string(kHash64),
                {Literal("character", "toto")})},
      {"firebird_hash_null",
       Function("int64", std::string(kHash64),
                {Literal("character", "", true),
                 Literal("text", "firebird.weak_hash.v1")})},
  });
  RequireSuccessfulProjection(firebird_profiled_hash, 5);
  const auto& hash_fields =
      firebird_profiled_hash.api_result.result_shape.rows.front().fields;
  Require(hash_fields[0].second.descriptor.canonical_type_name == "int64" &&
              !hash_fields[0].second.is_null &&
              hash_fields[0].second.encoded_value == "505519",
          "canonical Firebird HASH('toto') engine result drifted");
  Require(hash_fields[1].second.descriptor.canonical_type_name == "int64" &&
              !hash_fields[1].second.is_null &&
              hash_fields[1].second.encoded_value == "3289",
          "profiled Firebird HASH stopped operating on encoded bytes");
  Require(hash_fields[2].second.descriptor.canonical_type_name == "int64" &&
              !hash_fields[2].second.is_null &&
              hash_fields[2].second.encoded_value == "543154131059225647",
          "profiled Firebird HASH high-nibble folding drifted");
  Require(hash_fields[3].second.descriptor.canonical_type_name == "uint64" &&
              !hash_fields[3].second.is_null &&
              hash_fields[3].second.encoded_value == "17170989976038475891",
          "generic one-argument hash64/FNV behavior drifted");
  Require(hash_fields[4].second.descriptor.canonical_type_name == "int64" &&
              hash_fields[4].second.is_null,
          "profiled Firebird HASH NULL propagation drifted");
  Require(HasEvidence(firebird_profiled_hash, "function_runtime", kHash64),
          "profiled Firebird HASH function evidence was not preserved");

  const auto firebird_integer_cast = Dispatch({
      {"firebird_cast_1_25001_integer",
       Function(
           "int32", std::string(kCast),
           {Function("decimal", std::string(kCast),
                     {Literal("character.none", "1.25001"),
                      Literal("text", "decimal(18,0)"),
                      Literal("text", "half_up")}),
            Literal("text", "int32")})},
      {"firebird_cast_1_5001_integer",
       Function(
           "int32", std::string(kCast),
           {Function("decimal", std::string(kCast),
                     {Literal("character.none", "1.5001"),
                      Literal("text", "decimal(18,0)"),
                      Literal("text", "half_up")}),
            Literal("text", "int32")})},
  });
  RequireSuccessfulProjection(firebird_integer_cast, 2);
  const auto& integer_cast_fields =
      firebird_integer_cast.api_result.result_shape.rows.front().fields;
  Require(integer_cast_fields[0].second.descriptor.canonical_type_name ==
                  "int32" &&
              !integer_cast_fields[0].second.is_null &&
              integer_cast_fields[0].second.encoded_value == "1" &&
              integer_cast_fields[1].second.descriptor.canonical_type_name ==
                  "int32" &&
              !integer_cast_fields[1].second.is_null &&
              integer_cast_fields[1].second.encoded_value == "2",
          "canonical Firebird string-to-INTEGER CAST engine results drifted");
  Require(HasEvidence(firebird_integer_cast, "function_runtime", kCast),
          "nested Firebird INTEGER CAST engine evidence was not preserved");

  const auto firebird_numeric_literal_cast = Dispatch({
      {"firebird_cast_1_25001_char_21",
       Function("character", std::string(kCast),
                {Literal("numeric.fixed", "1.25001"),
                 Literal("text", "character")})},
      {"firebird_cast_1_25001_varchar_21",
       Function("varchar", std::string(kCast),
                {Literal("numeric.fixed", "1.25001"),
                 Literal("text", "varchar")})},
      {"firebird_cast_1_24999_numeric_2_1",
       Function("decimal", std::string(kCast),
                {Literal("numeric.fixed", "1.24999"),
                 Literal("text", "decimal(2,1)"),
                 Literal("text", "half_up")})},
      {"firebird_cast_1_25001_numeric_2_1",
       Function("decimal", std::string(kCast),
                {Literal("numeric.fixed", "1.25001"),
                 Literal("text", "decimal(2,1)"),
                 Literal("text", "half_up")})},
  });
  RequireSuccessfulProjection(firebird_numeric_literal_cast, 4);
  const auto& numeric_literal_cast_fields =
      firebird_numeric_literal_cast.api_result.result_shape.rows.front().fields;
  Require(numeric_literal_cast_fields[0].second.descriptor.canonical_type_name ==
                  "character" &&
              !numeric_literal_cast_fields[0].second.is_null &&
              numeric_literal_cast_fields[0].second.encoded_value ==
                  "1.25001" &&
              numeric_literal_cast_fields[1].second.descriptor.canonical_type_name ==
                  "varchar" &&
              !numeric_literal_cast_fields[1].second.is_null &&
              numeric_literal_cast_fields[1].second.encoded_value ==
                  "1.25001" &&
              numeric_literal_cast_fields[2].second.descriptor.canonical_type_name ==
                  "decimal" &&
              !numeric_literal_cast_fields[2].second.is_null &&
              numeric_literal_cast_fields[2].second.encoded_value == "1.2" &&
              numeric_literal_cast_fields[3].second.descriptor.canonical_type_name ==
                  "decimal" &&
              !numeric_literal_cast_fields[3].second.is_null &&
              numeric_literal_cast_fields[3].second.encoded_value == "1.3",
          "canonical Firebird numeric-literal CAST engine results drifted");
  Require(HasEvidence(firebird_numeric_literal_cast, "function_runtime", kCast),
          "Firebird numeric-literal CAST engine evidence was not preserved");

  const auto profiled_temporal_cast = [](std::string result_type,
                                         std::string target_descriptor,
                                         Expression source) {
    return Function(
        std::move(result_type), std::string(kCast),
        {std::move(source), Literal("text", std::move(target_descriptor)),
         Literal("text", "firebird.temporal_cast.v1")});
  };
  const auto firebird_temporal_cast = Dispatch({
      {"firebird_cast_character_date",
       profiled_temporal_cast("date", "date",
                              Literal("character.none", "28.1.2001"))},
      {"firebird_cast_character_time",
       profiled_temporal_cast("time", "time",
                              Literal("character.none", "14:34:59.1234"))},
      {"firebird_cast_character_timestamp",
       profiled_temporal_cast(
           "timestamp", "timestamp",
           Literal("character.none", "10.2.1489 14:34:59.1234"))},
      {"firebird_cast_date_character",
       profiled_temporal_cast(
           "character", "character(32)",
           profiled_temporal_cast("date", "date",
                                  Literal("character.none", "10.2.1973")))},
      {"firebird_cast_date_varchar",
       profiled_temporal_cast(
           "varchar", "varchar(40)",
           profiled_temporal_cast("date", "date",
                                  Literal("character.none", "10.2.1973")))},
      {"firebird_cast_date_timestamp",
       profiled_temporal_cast(
           "timestamp", "timestamp",
           profiled_temporal_cast("date", "date",
                                  Literal("character.none", "10.2.1973")))},
      {"firebird_cast_time_character",
       profiled_temporal_cast(
           "character", "character(32)",
           profiled_temporal_cast("time", "time",
                                  Literal("character.none", "13:28:45")))},
      {"firebird_cast_time_varchar",
       profiled_temporal_cast(
           "varchar", "varchar(32)",
           profiled_temporal_cast("time", "time",
                                  Literal("character.none", "13:28:45")))},
      {"firebird_cast_timestamp_character",
       profiled_temporal_cast(
           "character", "character(50)",
           profiled_temporal_cast(
               "timestamp", "timestamp",
               Literal("character.none", "1.4.2002 0:59:59.1")))},
      {"firebird_cast_timestamp_varchar",
       profiled_temporal_cast(
           "varchar", "varchar(50)",
           profiled_temporal_cast(
               "timestamp", "timestamp",
               Literal("character.none", "1.4.2002 0:59:59.1")))},
      {"firebird_cast_timestamp_date",
       profiled_temporal_cast(
           "date", "date",
           profiled_temporal_cast(
               "timestamp", "timestamp",
               Literal("character.none", "1.4.2002 0:59:59.1")))},
      {"firebird_cast_timestamp_time",
       profiled_temporal_cast(
           "time", "time",
           profiled_temporal_cast(
               "timestamp", "timestamp",
               Literal("character.none", "1.4.2002 0:59:59.1")))},
  });
  RequireSuccessfulProjection(firebird_temporal_cast, 12);
  const auto& temporal_cast_fields =
      firebird_temporal_cast.api_result.result_shape.rows.front().fields;
  const std::vector<std::pair<std::string_view, std::string_view>>
      expected_temporal_cast_values{
          {"date", "2001-01-28"},
          {"time", "14:34:59.1234"},
          {"timestamp", "1489-02-10T14:34:59.1234"},
          {"character", "1973-02-10"},
          {"varchar", "1973-02-10"},
          {"timestamp", "1973-02-10T00:00:00.0000"},
          {"character", "13:28:45.0000"},
          {"varchar", "13:28:45.0000"},
          {"character", "2002-04-01 00:59:59.1000"},
          {"varchar", "2002-04-01 00:59:59.1000"},
          {"date", "2002-04-01"},
          {"time", "00:59:59.1000"},
      };
  for (std::size_t index = 0;
       index < expected_temporal_cast_values.size(); ++index) {
    Require(temporal_cast_fields[index].second.descriptor.canonical_type_name ==
                    expected_temporal_cast_values[index].first &&
                !temporal_cast_fields[index].second.is_null &&
                temporal_cast_fields[index].second.encoded_value ==
                    expected_temporal_cast_values[index].second,
            "canonical Firebird temporal CAST engine result drifted");
  }
  Require(HasEvidence(firebird_temporal_cast, "function_runtime", kCast),
          "nested Firebird temporal CAST engine evidence was not preserved");

  // Fail closed before producing a row for malformed SBLR calls. These cover
  // wrong arity, typed-carrier mismatch, and one-based/domain violations.
  RequireFailedBeforeRow(Function(
      "character", std::string(kLeft), {Literal("character", "bonjour")}));
  RequireFailedBeforeRow(Function(
      "character", std::string(kRight),
      {Literal("character", "NORD PAS DE CALAIS"),
       Literal("real64", "13.5")}));
  RequireFailedBeforeRow(Function(
      "character", std::string(kOverlay),
      {Literal("character", "abcdef"), Literal("character", "X"),
       Literal("text", "2"), Literal("int64", "1")}));
  RequireFailedBeforeRow(Function(
      "character", std::string(kOverlay),
      {Literal("character", "abcdef"), Literal("character", "X"),
       Literal("int64", "0"), Literal("int64", "1")}));
  RequireFailedBeforeRow(Function(
      "int64", std::string(kPosition),
      {Literal("character", "beau"),
       Literal("character", "beau,il fait beau"), Literal("int64", "2")}));
  RequireFailedBeforeRow(Function(
      "int64", std::string(kInstr),
      {Literal("character", "beau,il fait beau"),
       Literal("character", "beau"), Literal("text", "2")}));
  RequireFailedBeforeRow(Function(
      "int64", std::string(kInstr),
      {Literal("character", "beau,il fait beau"),
       Literal("character", "beau"), Literal("int64", "0")}));
  RequireFailedBeforeRow(Function(
      "character", std::string(kReplace),
      {Literal("character", "toto"), Literal("character", "o")}));
  RequireFailedBeforeRow(Function(
      "character", std::string(kReverse),
      {Literal("character", "DRON"), Literal("character", "extra")}));
  RequireFailedBeforeRow(Function(
      "int64", std::string(kHash64), {}));
  RequireFailedBeforeRow(Function(
      "int64", std::string(kHash64),
      {Literal("text", "toto"),
       Literal("text", "firebird.weak_hash.v1")}));
  RequireFailedBeforeRow(Function(
      "int64", std::string(kHash64),
      {Literal("character", "toto"),
       Literal("character", "firebird.weak_hash.v1")}));
  RequireFailedBeforeRow(Function(
      "int64", std::string(kHash64),
      {Literal("character", "toto"),
       Literal("text", "firebird.unknown.v1")}));
  RequireFailedBeforeRow(Function(
      "int64", std::string(kHash64),
      {Literal("character", "toto"), Literal("text", "", true)}));
  RequireFailedBeforeRow(Function(
      "int64", std::string(kHash64),
      {Literal("character", "toto"),
       Literal("text", "firebird.weak_hash.v1"),
       Literal("text", "extra")}));
  RequireFailedBeforeRow(Function(
      "character", std::string(kCast),
      {Literal("numeric.fixed", "1.25001")}));
  RequireFailedBeforeRow(Function(
      "character", std::string(kCast),
      {Literal("numeric.fixed", "1.25001"),
       Literal("text", "character"), Literal("text", "half_up")}));
  RequireConversionInputFailure(
      profiled_temporal_cast(
          "date", "date",
          Function(
              "int32", std::string(kCast),
              {Function("decimal", std::string(kCast),
                        {Literal("numeric.fixed", "1.25001"),
                         Literal("text", "decimal(18,0)"),
                         Literal("text", "half_up")}),
               Literal("text", "int32")})),
      "1");
  RequireConversionInputFailure(profiled_temporal_cast(
                                    "date", "date",
                                    Literal("character.none", "29.2.2002")),
                                "29.2.2002");
  RequireConversionInputFailure(profiled_temporal_cast(
                                    "time", "time",
                                    Literal("character.none", "9:11:60")),
                                "9:11:60");
  RequireFailedBeforeRow(
      profiled_temporal_cast(
          "character", "character(5)",
          profiled_temporal_cast("date", "date",
                                 Literal("character.none", "10.2.1973"))),
      "SB_DIAG_FUNCTION_NUMERIC_OVERFLOW");
  RequireGenericInvalidInputWithoutConversionField(Function(
      "date", std::string(kCast),
      {Literal("character.none", "28.1.2001"), Literal("text", "date"),
       Literal("text", "firebird.temporal_cast.v2")}));

  RequireFailedBeforeRow(Function(
      "int64", std::string(kBitAnd), {Literal("int64", "1")}));
  RequireFailedBeforeRow(Function(
      "int64", std::string(kBitOr),
      {Literal("int64", "1"), Literal("int64", "0"),
       Literal("int64", "1")}));
  RequireFailedBeforeRow(Function(
      "int64", std::string(kBitXor),
      {Literal("real64", "1.5"), Literal("int64", "1")}));
  RequireFailedBeforeRow(Function(
      "int64", std::string(kBitShiftLeft),
      {Literal("text", "eight"), Literal("int64", "1")}));
  RequireFailedBeforeRow(Function(
      "int64", std::string(kBitShiftRight), {Literal("int64", "8")}));
  RequireFailedBeforeRow(
      Function("int64", std::string(kBitShiftLeft),
               {Literal("int64", "8"), Literal("int64", "-1")}),
      "SB_DIAG_FUNCTION_NUMERIC_DOMAIN");
  RequireFailedBeforeRow(
      Function("int64", std::string(kBitShiftRight),
               {Literal("int64", "8"), Literal("int64", "64")}),
      "SB_DIAG_FUNCTION_NUMERIC_DOMAIN");

  RequireFailedBeforeRow(ProfiledDateAdd(
      "date", "2004-01-31", "month", "1",
      "firebird.calendar_month.v1", "int64", "date_add"),
      "SB_DIAG_FUNCTION_NOT_REGISTERED");
  RequireFailedBeforeRow(ProfiledDateAdd(
      "date", "2004-01-31", "month", "1.0",
      "firebird.calendar_month.v1", "numeric.fixed"));
  RequireFailedBeforeRow(Function(
      "date", std::string(kDateAdd),
      {Literal("date", "2004-01-31"), Literal("int64", "1"),
       Literal("int64", "1"),
       Literal("text", "firebird.calendar_month.v1")}));
  RequireFailedBeforeRow(ProfiledDateAdd(
      "date", "2004-01-31", "day", "1",
      "firebird.calendar_month.v1"));
  RequireFailedBeforeRow(ProfiledDateAdd(
      "time", "12:12:00", "hour", "1", "firebird.unknown.v1"));
  RequireFailedBeforeRow(ProfiledDateAdd(
      "time", "12:12:00", "hour", "9223372036854775807",
      "firebird.ticks_100us.v1"),
      "SB_DIAG_FUNCTION_NUMERIC_OVERFLOW");
  RequireFailedBeforeRow(Function(
      "time", std::string(kDateAdd),
      {Literal("time", "12:12:00"), Literal("text", "hour"),
       Literal("int64", "-1")}));
  RequireFailedBeforeRow(ProfiledDateDiff(
      "timestamp", "0001-01-01T00:00:00.0001",
      "9999-12-31T23:59:59.9999", "second"));
  RequireFailedBeforeRow(ProfiledDateDiff(
      "time", "00:00:00.0001", "23:59:59.9999", "millisecond",
      "firebird.unknown.v1"));
  RequireFailedBeforeRow(Function(
      "numeric.fixed", std::string(kDateDiff),
      {Literal("text", "millisecond"), Literal("time", "00:00:00.0001"),
       Literal("time", "23:59:59.9999"), Literal("int64", "1")}));
  RequireFailedBeforeRow(ProfiledDatePart(
      "time", "12:12:00.11111"));
  RequireFailedBeforeRow(ProfiledDatePart(
      "time", "12:12:00.1111", "second"));
  RequireFailedBeforeRow(Function(
      "numeric.fixed", std::string(kDatePart),
      {Literal("int64", "1"), Literal("time", "12:12:00.1111"),
       Literal("text", "firebird.ticks_100us.v1")}));

  RequireFailedBeforeRow(
      Function("binary", std::string(kOctetFromInt64),
               {Literal("int64", "-1")}));
  RequireFailedBeforeRow(
      Function("binary", std::string(kOctetFromInt64),
               {Literal("int64", "256")}));
  RequireFailedBeforeRow(
      Function("binary", std::string(kOctetFromInt64),
               {Literal("int64", "not-an-integer")}),
      "SB_DIAG_FUNCTION_ARGUMENT_INVALID");
  RequireFailedBeforeRow(
      Function("int64", std::string(kInt64FromFirstOctet),
               {Literal("int64", "65")}));
  RequireFailedBeforeRow(
      Function("int64", std::string(kInt64FromFirstOctet),
               {Literal("character.utf8", "\xc3\x83")}));
  RequireFailedBeforeRow(
      Function("numeric.fixed", std::string(kRound),
               {Literal("numeric.fixed", "5.7778"),
                Literal("int64", "-1")}));
  RequireFailedBeforeRow(
      Function("numeric.fixed", std::string(kRound),
               {Literal("numeric.fixed", "not-a-decimal"),
                Literal("int64", "3")}));
  RequireFailedBeforeRow(
      Function("numeric.fixed", std::string(kRound),
               {Literal("numeric.fixed", "1234567890123456789.1"),
                Literal("int64", "1")}));
  RequireFailedBeforeRow(
      Function("decimal", std::string(kCast),
               {Literal("numeric.fixed", "1.25"),
                Literal("text", "decimal(18,19)")}));
  RequireFailedBeforeRow(
      Function("decimal", std::string(kCast),
               {Literal("numeric.fixed", "1.25"),
                Literal("text", "decimal(18,15)"),
                Literal("text", "bankers")}));
  RequireFailedBeforeRow(
      Function("decimal", std::string(kCast),
               {Literal("numeric.fixed", "99.95"),
                Literal("text", "decimal(3,1)"),
                Literal("text", "half_up")}),
      "SB_DIAG_FUNCTION_NUMERIC_OVERFLOW");
  RequireFailedBeforeRow(
      Function("decimal", std::string(kCast),
               {Literal("numeric.fixed", "not-a-decimal"),
                Literal("text", "decimal(18,15)")}));
  RequireFailedBeforeRow(
      Function("decimal", std::string(kCast),
               {Literal("numeric.fixed", "1.25"),
                Literal("text", "", true)}));
  RequireFailedBeforeRow(
      Function("decimal", std::string(kCast),
               {Literal("numeric.fixed", "1.25"),
                Literal("text", "decimal(18,15)"),
                Literal("text", "", true)}));
  RequireFailedBeforeRow(
      Function("decimal", std::string(kCast),
               {Literal("numeric.fixed", "1.25"),
                Literal("text", "decimal(18,15)"),
                Literal("int64", "1")}));
  RequireFailedBeforeRow(
      Function("real64", std::string(kCot),
               {Literal("real64", "0")}),
      "SB_DIAG_FUNCTION_NUMERIC_DOMAIN");
  RequireFailedBeforeRow(
      Function("real64", std::string(kCot),
               {Literal("real64", "-0")}),
      "SB_DIAG_FUNCTION_NUMERIC_DOMAIN");
  RequireFailedBeforeRow(
      Function("real64", std::string(kCot),
               {Literal("real64", "nan")}),
      "SB_DIAG_FUNCTION_ARGUMENT_INVALID");
  return EXIT_SUCCESS;
}
