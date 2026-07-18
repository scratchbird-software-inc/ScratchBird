// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_dialect.hpp"
#include "firebird_worker_session.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>


namespace {

namespace fb = scratchbird::parser::firebird;

constexpr std::uint32_t kIscArgEnd = 0;
constexpr std::uint32_t kIscArgGds = 1;
constexpr std::uint32_t kIscArgString = 2;
constexpr std::uint32_t kIscArgNumber = 4;
constexpr std::uint32_t kIscArgSqlState = 19;
constexpr std::uint32_t kIscDsqlError = 335544569u;
constexpr std::uint32_t kIscSqlErr = 335544436u;
constexpr std::uint32_t kIscDsqlCommandErr = 335544570u;
constexpr std::uint32_t kIscDsqlFieldErr = 335544578u;
constexpr std::uint32_t kIscRandom = 335544382u;
constexpr std::uint32_t kIscDsqlLineColError = 336397208u;
constexpr std::uint32_t kIscDsqlDerivedFieldDupName = 336397221u;

void Require(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

std::uint32_t ReadU32(
    const std::vector<std::uint8_t>& bytes,
    std::size_t offset) {
  Require(offset + 4 <= bytes.size(), "truncated XDR integer");
  return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
         static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::string ReadString(
    const std::vector<std::uint8_t>& bytes,
    std::size_t* offset) {
  const auto size = ReadU32(bytes, *offset);
  *offset += 4;
  Require(*offset + size <= bytes.size(), "truncated XDR string");
  std::string value(
      reinterpret_cast<const char*>(bytes.data() + *offset), size);
  *offset += (size + 3u) & ~std::size_t{3u};
  Require(*offset <= bytes.size(), "truncated XDR string padding");
  return value;
}

struct StatusItem {
  std::uint32_t tag{0};
  std::uint32_t integer{0};
  std::string text;
};

struct DecodedResponse {
  std::string response_json;
  std::vector<StatusItem> status;
};

DecodedResponse DecodeResponse(const std::vector<std::uint8_t>& packet) {
  Require(packet.size() >= 20, "derived diagnostic packet is too short");
  Require(ReadU32(packet, 0) == 9, "derived diagnostic was not op_response");
  Require(ReadU32(packet, 4) == 0 && ReadU32(packet, 8) == 0 &&
              ReadU32(packet, 12) == 0,
          "derived diagnostic response leaked an object/blob handle");
  std::size_t offset = 16;
  DecodedResponse decoded;
  decoded.response_json = ReadString(packet, &offset);
  while (offset < packet.size()) {
    StatusItem item;
    item.tag = ReadU32(packet, offset);
    offset += 4;
    if (item.tag == kIscArgEnd) {
      decoded.status.push_back(std::move(item));
      break;
    }
    if (item.tag == kIscArgGds || item.tag == kIscArgNumber) {
      item.integer = ReadU32(packet, offset);
      offset += 4;
    } else if (item.tag == kIscArgString ||
               item.tag == kIscArgSqlState) {
      item.text = ReadString(packet, &offset);
    } else {
      Require(false, "unexpected status-vector tag");
    }
    decoded.status.push_back(std::move(item));
  }
  Require(!decoded.status.empty() &&
              decoded.status.back().tag == kIscArgEnd &&
              offset == packet.size(),
          "status vector did not terminate exactly at packet end");
  return decoded;
}


void RequireInteger(
    const StatusItem& item,
    std::uint32_t tag,
    std::uint32_t value,
    std::string_view message) {
  Require(item.tag == tag && item.integer == value, message);
}

void RequireText(
    const StatusItem& item,
    std::uint32_t tag,
    std::string_view value,
    std::string_view message) {
  Require(item.tag == tag && item.text == value, message);
}

void VerifyPacketBytes(
    const fb::FirebirdDerivedTableDiagnostic& diagnostic,
    std::string_view operation,
    const std::vector<std::uint8_t>& packet) {
  const auto decoded = DecodeResponse(packet);
  Require(decoded.response_json.find(
              "\"wire_operation\":\"" + std::string(operation) + "\"") !=
              std::string::npos,
          "derived diagnostic packet lost wire operation identity");

  const auto sqlcode = diagnostic.kind ==
                               fb::FirebirdDerivedTableDiagnosticKind::
                                   kDuplicateOutputName
                           ? -104
                           : -206;
  Require(decoded.status.size() >= 9, "derived status vector is incomplete");
  RequireInteger(decoded.status[0], kIscArgGds, kIscDsqlError,
                 "derived status did not begin with isc_dsql_error");
  RequireInteger(decoded.status[1], kIscArgGds, kIscSqlErr,
                 "derived status omitted isc_sqlerr");
  RequireInteger(
      decoded.status[2], kIscArgNumber,
      static_cast<std::uint32_t>(static_cast<std::int32_t>(sqlcode)),
      "derived status emitted wrong SQLCODE");

  if (diagnostic.kind ==
      fb::FirebirdDerivedTableDiagnosticKind::kDuplicateOutputName) {
    Require(decoded.status.size() == 9,
            "duplicate derived status vector has unexpected arguments");
    RequireInteger(decoded.status[3], kIscArgGds, kIscDsqlCommandErr,
                   "duplicate status omitted Invalid command");
    RequireInteger(decoded.status[4], kIscArgGds,
                   kIscDsqlDerivedFieldDupName,
                   "duplicate status emitted wrong native symbol");
    RequireText(decoded.status[5], kIscArgString, diagnostic.column_name,
                "duplicate status lost column argument");
    RequireText(decoded.status[6], kIscArgString,
                diagnostic.derived_table_alias,
                "duplicate status lost derived-table argument");
    RequireText(decoded.status[7], kIscArgSqlState, "42000",
                "duplicate status SQLSTATE was not terminal");
  } else {
    Require(decoded.status.size() == 11,
            "outer-reference status vector has unexpected arguments");
    RequireInteger(decoded.status[3], kIscArgGds, kIscDsqlFieldErr,
                   "outer-reference status omitted Column unknown");
    RequireInteger(decoded.status[4], kIscArgGds, kIscRandom,
                   "outer-reference status omitted field presentation");
    RequireText(decoded.status[5], kIscArgString,
                diagnostic.qualified_field_name,
                "outer-reference status lost qualified field");
    RequireInteger(decoded.status[6], kIscArgGds, kIscDsqlLineColError,
                   "outer-reference status omitted line/column symbol");
    RequireInteger(decoded.status[7], kIscArgNumber,
                   static_cast<std::uint32_t>(diagnostic.line),
                   "outer-reference status lost source line");
    RequireInteger(decoded.status[8], kIscArgNumber,
                   static_cast<std::uint32_t>(diagnostic.column),
                   "outer-reference status lost source column");
    RequireText(decoded.status[9], kIscArgSqlState, "42S22",
                "outer-reference status SQLSTATE was not terminal");
  }
  Require(decoded.status.back().tag == kIscArgEnd,
          "derived status vector omitted isc_arg_end");
}

void VerifyProductionDispatch(
    const fb::ParseResult& parsed,
    std::uint32_t opcode,
    std::string_view operation) {
  const auto packet =
      fb::DispatchFirebirdParserFailurePacketForOpcode(opcode, parsed);
  Require(packet.has_value(),
          "production opcode dispatcher refused typed parser failure");
  VerifyPacketBytes(*parsed.derived_table_diagnostic, operation, *packet);
}

fb::ParseResult RequireRefused(
    std::string_view sql,
    fb::FirebirdDerivedTableDiagnosticKind kind) {
  auto parsed = fb::ParseStatement(sql);
  Require(!parsed.ok, "derived diagnostic SQL was accepted");
  Require(parsed.statement_family == "query",
          "derived diagnostic did not remain in query binder family");
  Require(parsed.derived_table_diagnostic.has_value(),
          "derived diagnostic lost typed binder result");
  Require(parsed.derived_table_diagnostic->kind == kind,
          "derived diagnostic kind mismatch");
  Require(parsed.sblr_envelope.empty() && parsed.sblr_operation.empty() &&
              parsed.engine_api_function.empty(),
          "binder refusal created an SBLR/engine execution route");
  VerifyProductionDispatch(parsed, 68, "op_prepare_statement");
  VerifyProductionDispatch(parsed, 64, "op_exec_immediate");
  VerifyProductionDispatch(parsed, 75, "op_exec_immediate2");
  Require(!fb::DispatchFirebirdParserFailurePacketForOpcode(63, parsed)
               .has_value(),
          "production dispatcher accepted non-parser opcode");
  return parsed;
}

void RequireNoDerivedDiagnostic(std::string_view sql) {
  const auto diagnostic =
      fb::AnalyzeFirebirdDerivedTableDiagnostics(fb::LexTokens(sql), sql);
  Require(!diagnostic.has_value(),
          "near-miss SQL fabricated a derived-table diagnostic");
  const auto parsed = fb::ParseStatement(sql);
  Require(!parsed.derived_table_diagnostic.has_value(),
          "ParseStatement overlaid a derived-table diagnostic on near miss");
}


}  // namespace

int main() {
  const std::vector<std::string> duplicate_cases = {
      R"SQL(SELECT
  dt.*
FROM
(SELECT ID, ID FROM Table_10 t10) dt;)SQL",
      R"SQL(SELECT
  dt.*
FROM
(SELECT * FROM Table_10 t10) dt (ID, ID);)SQL",
      R"SQL(SELECT
  dt.*
FROM
  (SELECT ID, ID FROM Table_10 t10) dt;)SQL",
      R"SQL(SELECT
  dt.*
FROM
  (SELECT * FROM Table_10 t10) dt (ID, ID);)SQL"};

  for (const auto& sql : duplicate_cases) {
    const auto parsed = RequireRefused(
        sql, fb::FirebirdDerivedTableDiagnosticKind::kDuplicateOutputName);
    const auto& diagnostic = *parsed.derived_table_diagnostic;
    Require(diagnostic.column_name == "ID" &&
                diagnostic.derived_table_alias == "DT",
            "duplicate binder arguments do not match Firebird 04/05/26/27");
    Require(parsed.message_vector_json.find(
                "FIREBIRD.DSQL.DERIVED_FIELD_DUP_NAME") !=
                std::string::npos &&
                parsed.message_vector_json.find("\"primary_sqlcode\":\"-104\"") !=
                    std::string::npos &&
                parsed.message_vector_json.find("\"primary_sqlstate\":\"42000\"") !=
                    std::string::npos,
            "duplicate binder message vector is not exact");
  }

  const std::vector<std::string> outer_reference_cases = {
      R"SQL(SELECT
  dt.*
FROM
  Table_10 t10
JOIN (SELECT * FROM Table_10 t2 WHERE t2.ID = t10.ID) dt ON (1 = 1);)SQL",
      R"SQL(SELECT
  dt.*
FROM
  Table_10 t10
LEFT JOIN (SELECT * FROM Table_10 t2 WHERE t2.ID = t10.ID) dt ON (1 = 1);)SQL",
      R"SQL(SELECT
  dt.*
FROM
  Table_10 t10
FULL JOIN (SELECT * FROM Table_10 t2 WHERE t2.ID = t10.ID) dt ON (1 = 1);)SQL",
      R"SQL(SELECT
  dt.*
FROM
  Table_10 t10
FULL JOIN (SELECT * FROM Table_10 t2 WHERE t2.ID = t10.ID) dt ON (1 = 1);)SQL",
      R"SQL(SELECT
  dt.*
FROM
  Table_10 t10
  JOIN (SELECT * FROM Table_10 t2 WHERE t2.ID = t10.ID) dt ON (1 = 1);)SQL",
      R"SQL(SELECT
  dt.*
FROM
  Table_10 t10
  LEFT JOIN (SELECT * FROM Table_10 t2 WHERE t2.ID = t10.ID) dt ON (1 = 1);)SQL",
      R"SQL(SELECT
  dt.*
FROM
  Table_10 t10
  FULL JOIN (SELECT * FROM Table_10 t2 WHERE t2.ID = t10.ID) dt ON (1 = 1);)SQL"};

  const std::vector<std::size_t> expected_outer_reference_columns{
      53, 58, 58, 58, 55, 60, 60};
  Require(outer_reference_cases.size() ==
              expected_outer_reference_columns.size(),
          "outer-reference reference-fixture accounting drifted");
  for (std::size_t index = 0; index < outer_reference_cases.size(); ++index) {
    const auto& sql = outer_reference_cases[index];
    const auto parsed = RequireRefused(
        sql,
        fb::FirebirdDerivedTableDiagnosticKind::kIllegalOuterReference);
    const auto& diagnostic = *parsed.derived_table_diagnostic;
    Require(diagnostic.qualified_field_name == "T10.ID" &&
                diagnostic.outer_relation_alias == "T10" &&
                diagnostic.derived_table_alias == "DT" &&
                diagnostic.line == 5 &&
                diagnostic.column == expected_outer_reference_columns[index],
            "outer-reference binder arguments do not match Firebird 06-09/23-25");
    Require(parsed.message_vector_json.find(
                "FIREBIRD.DSQL.DERIVED_OUTER_REFERENCE") !=
                std::string::npos &&
                parsed.message_vector_json.find("\"primary_sqlcode\":\"-206\"") !=
                    std::string::npos &&
                parsed.message_vector_json.find("\"primary_sqlstate\":\"42S22\"") !=
                    std::string::npos,
            "outer-reference binder message vector is not exact");
  }

  const std::string original_with_body_comment = R"SQL(SELECT dt.*
FROM Table_10 t10
JOIN (
  SELECT *
  FROM Table_10 t2
  -- a stripped body comment must not move the reference source position
  WHERE t2.ID = t10.ID
) dt ON 1 = 1)SQL";
  const std::string stripped_for_lowering = R"SQL(SELECT dt.*
FROM Table_10 t10
JOIN (
  SELECT *
  FROM Table_10 t2
  WHERE t2.ID = t10.ID
) dt ON 1 = 1)SQL";
  const auto source_preserved = fb::ParseStatementWithOriginalSource(
      stripped_for_lowering, original_with_body_comment);
  Require(!source_preserved.ok &&
              source_preserved.derived_table_diagnostic.has_value() &&
              source_preserved.derived_table_diagnostic->line == 7 &&
              source_preserved.derived_table_diagnostic->column == 23,
          "worker lowering text moved the original derived diagnostic position");

  // Quoted uppercase ID denotes the same identifier as unquoted ID.
  RequireRefused(
      "SELECT dt.* FROM (SELECT ID, \"ID\" FROM Table_10) dt",
      fb::FirebirdDerivedTableDiagnosticKind::kDuplicateOutputName);

  const std::vector<std::string> near_misses = {
      "SELECT dt.* FROM (SELECT ID, DESCRIPTION FROM Table_10 t10) dt",
      "SELECT dt.* FROM (SELECT * FROM Table_10 t10) dt (ID, DESCRIPTION)",
      "SELECT dt.* FROM (SELECT ID, ID FROM Table_10 t10) dt (A, B)",
      "SELECT dt.* FROM (SELECT ID, \"id\" FROM Table_10 t10) dt",
      "SELECT dt.* FROM (SELECT \"ID, ID\", DESCRIPTION FROM Table_10 t10) dt",
      "SELECT dt.* FROM (SELECT ID /*, ID*/, DESCRIPTION FROM Table_10 t10) dt",
      "SELECT dt.* FROM (SELECT 'ID, ID' AS NOTE, ID FROM Table_10 t10) dt",
      "SELECT dt.* FROM Table_10 t10 JOIN LATERAL (SELECT * FROM Table_10 t2 WHERE t2.ID = t10.ID) dt ON 1 = 1",
      "SELECT dt.* FROM Table_10 t10 JOIN (SELECT * FROM Table_10 t10 WHERE t10.ID = 1) dt ON 1 = 1",
      "SELECT (SELECT ID, ID FROM Table_10 t2) FROM Table_10 t10",
      "SELECT dt.* FROM Table_10 t10 JOIN (SELECT 't10.ID' AS NOTE FROM Table_10 t2) dt ON 1 = 1",
      "SELECT dt.* FROM Table_10 t10 JOIN (SELECT * FROM Table_10 t2 -- t10.ID\nWHERE t2.ID = 1) dt ON 1 = 1"};
  for (const auto& sql : near_misses) RequireNoDerivedDiagnostic(sql);

  fb::FirebirdDerivedTableDiagnostic malformed;
  Require(!fb::RenderFirebirdDerivedTableDiagnosticPacket(
               malformed, "op_prepare_statement")
               .has_value(),
          "derived renderer accepted incomplete typed input");

  std::cout <<
      "firebird derived-table diagnostic probe passed: 11 exact cases, "
      "production numeric-opcode dispatch, and bounded near misses\n";
  return EXIT_SUCCESS;
}
