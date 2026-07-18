// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_execution_session.hpp"

#include "firebird_dialect.hpp"
#include "firebird_scalar_projection.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace scratchbird::parser::firebird {
namespace {

FirebirdPipelineResult Rejected(std::string code, std::string message) {
  FirebirdPipelineResult result;
  result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
      std::move(code), "ERROR", std::move(message),
      "sbp_firebird.execution_session"));
  return result;
}

ipc::ServerExecutionResult RejectedExecution(std::string code,
                                             std::string message) {
  ipc::ServerExecutionResult result;
  result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
      std::move(code), "ERROR", std::move(message),
      "sbp_firebird.execution_session"));
  return result;
}

void ProjectNonAuthoritativeRowResult(
    const ipc::ServerExecutionResult& server,
    FirebirdPipelineResult* pipeline) {
  if (pipeline == nullptr) return;
  pipeline->server_row_count = server.row_count;
  pipeline->server_affected_rows = server.affected_rows;
  pipeline->server_affected_rows_present = server.affected_rows_present;
  pipeline->server_cursor_uuid = server.cursor_uuid;
  pipeline->server_result_payload = server.row_packet;
}

std::string PayloadLineValue(std::string_view payload, std::string_view key) {
  std::size_t start = 0;
  while (start <= payload.size()) {
    const std::size_t end = payload.find('\n', start);
    const std::string_view line = payload.substr(
        start,
        end == std::string_view::npos ? payload.size() - start : end - start);
    const std::size_t equals = line.find('=');
    if (equals != std::string_view::npos && line.substr(0, equals) == key) {
      return std::string(line.substr(equals + 1));
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return {};
}

ipc::ServerPrepareSblrResult RejectedPrepare(std::string code,
                                             std::string message) {
  ipc::ServerPrepareSblrResult result;
  result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
      std::move(code), "ERROR", std::move(message),
      "sbp_firebird.execution_session"));
  return result;
}

ipc::ServerClosePreparedSblrResult RejectedClosePrepared(
    std::string code,
    std::string message) {
  ipc::ServerClosePreparedSblrResult result;
  result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
      std::move(code), "ERROR", std::move(message),
      "sbp_firebird.execution_session"));
  return result;
}

ipc::PublicNameResolutionResult RejectedResolution(std::string code,
                                                   std::string message) {
  ipc::PublicNameResolutionResult result;
  result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
      std::move(code), "ERROR", std::move(message),
      "sbp_firebird.execution_session"));
  return result;
}

bool StartsWithCommand(std::string_view upper, std::string_view command) {
  return upper.starts_with(command) &&
         (upper.size() == command.size() ||
          upper[command.size()] == ' ' || upper[command.size()] == '\t' ||
          upper[command.size()] == '\r' || upper[command.size()] == '\n');
}

std::string StripStatementTerminator(std::string_view sql) {
  std::string value = TrimAscii(sql);
  while (!value.empty() && value.back() == ';') {
    value.pop_back();
    value = TrimAscii(value);
  }
  return value;
}

void SkipWhitespace(std::string_view text, std::size_t* offset) {
  while (*offset < text.size() &&
         std::isspace(static_cast<unsigned char>(text[*offset])) != 0) {
    ++*offset;
  }
}

bool ReadUnsignedInteger(std::string_view text,
                         std::size_t* offset,
                         std::uint64_t* value) {
  SkipWhitespace(text, offset);
  if (*offset >= text.size() ||
      std::isdigit(static_cast<unsigned char>(text[*offset])) == 0) {
    return false;
  }
  std::uint64_t parsed = 0;
  while (*offset < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[*offset])) != 0) {
    const std::uint64_t digit =
        static_cast<std::uint64_t>(text[*offset] - '0');
    if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
      return false;
    }
    parsed = parsed * 10 + digit;
    ++*offset;
  }
  *value = parsed;
  return true;
}

std::optional<std::pair<std::string, bool>> ReadIdentifier(
    std::string_view text,
    std::size_t* offset) {
  SkipWhitespace(text, offset);
  if (*offset >= text.size()) return std::nullopt;
  if (text[*offset] == '"') {
    std::string identifier;
    ++*offset;
    while (*offset < text.size()) {
      const char ch = text[*offset];
      ++*offset;
      if (ch != '"') {
        identifier.push_back(ch);
        continue;
      }
      if (*offset < text.size() && text[*offset] == '"') {
        identifier.push_back('"');
        ++*offset;
        continue;
      }
      return std::pair<std::string, bool>{std::move(identifier), true};
    }
    return std::nullopt;
  }
  const std::size_t begin = *offset;
  while (*offset < text.size()) {
    const unsigned char ch = static_cast<unsigned char>(text[*offset]);
    if (std::isalnum(ch) == 0 && text[*offset] != '_' &&
        text[*offset] != '$' && text[*offset] != '.') {
      break;
    }
    ++*offset;
  }
  if (*offset == begin) return std::nullopt;
  return std::pair<std::string, bool>{
      std::string(text.substr(begin, *offset - begin)), false};
}

std::optional<std::size_t> MatchingParenthesis(std::string_view text,
                                               std::size_t open) {
  if (open >= text.size() || text[open] != '(') return std::nullopt;
  std::size_t depth = 0;
  bool single_quoted = false;
  bool double_quoted = false;
  for (std::size_t i = open; i < text.size(); ++i) {
    const char ch = text[i];
    if (single_quoted) {
      if (ch == '\'' && i + 1 < text.size() && text[i + 1] == '\'') {
        ++i;
      } else if (ch == '\'') {
        single_quoted = false;
      }
      continue;
    }
    if (double_quoted) {
      if (ch == '"' && i + 1 < text.size() && text[i + 1] == '"') {
        ++i;
      } else if (ch == '"') {
        double_quoted = false;
      }
      continue;
    }
    if (ch == '\'') {
      single_quoted = true;
    } else if (ch == '"') {
      double_quoted = true;
    } else if (ch == '(') {
      ++depth;
    } else if (ch == ')' && --depth == 0) {
      return i;
    }
  }
  return std::nullopt;
}

std::vector<std::string> SplitTopLevel(std::string_view text) {
  std::vector<std::string> parts;
  std::size_t begin = 0;
  std::size_t depth = 0;
  bool single_quoted = false;
  bool double_quoted = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (single_quoted) {
      if (ch == '\'' && i + 1 < text.size() && text[i + 1] == '\'') {
        ++i;
      } else if (ch == '\'') {
        single_quoted = false;
      }
      continue;
    }
    if (double_quoted) {
      if (ch == '"' && i + 1 < text.size() && text[i + 1] == '"') {
        ++i;
      } else if (ch == '"') {
        double_quoted = false;
      }
      continue;
    }
    if (ch == '\'') {
      single_quoted = true;
    } else if (ch == '"') {
      double_quoted = true;
    } else if (ch == '(') {
      ++depth;
    } else if (ch == ')' && depth != 0) {
      --depth;
    } else if (ch == ',' && depth == 0) {
      parts.push_back(TrimAscii(text.substr(begin, i - begin)));
      begin = i + 1;
    }
  }
  parts.push_back(TrimAscii(text.substr(begin)));
  return parts;
}

bool IsIdentifierCharacter(char ch) {
  const unsigned char value = static_cast<unsigned char>(ch);
  return std::isalnum(value) != 0 || ch == '_' || ch == '$';
}

std::optional<std::size_t> FindTopLevelWord(std::string_view text,
                                            std::string_view word,
                                            std::size_t begin = 0) {
  const std::string upper = ToUpperAscii(text);
  const std::string expected = ToUpperAscii(word);
  std::size_t depth = 0;
  bool single_quoted = false;
  bool double_quoted = false;
  for (std::size_t i = begin; i + expected.size() <= upper.size(); ++i) {
    const char ch = text[i];
    if (single_quoted) {
      if (ch == '\'' && i + 1 < text.size() && text[i + 1] == '\'') {
        ++i;
      } else if (ch == '\'') {
        single_quoted = false;
      }
      continue;
    }
    if (double_quoted) {
      if (ch == '"' && i + 1 < text.size() && text[i + 1] == '"') {
        ++i;
      } else if (ch == '"') {
        double_quoted = false;
      }
      continue;
    }
    if (ch == '\'') {
      single_quoted = true;
      continue;
    }
    if (ch == '"') {
      double_quoted = true;
      continue;
    }
    if (ch == '(') {
      ++depth;
      continue;
    }
    if (ch == ')' && depth != 0) {
      --depth;
      continue;
    }
    if (depth != 0 || upper.compare(i, expected.size(), expected) != 0) {
      continue;
    }
    const bool left_boundary = i == 0 || !IsIdentifierCharacter(upper[i - 1]);
    const std::size_t end = i + expected.size();
    const bool right_boundary =
        end == upper.size() || !IsIdentifierCharacter(upper[end]);
    if (left_boundary && right_boundary) return i;
  }
  return std::nullopt;
}

bool ConsumeKeyword(std::string_view text,
                    std::size_t* offset,
                    std::string_view keyword);

bool ContainsTopLevelKeywordSequence(std::string_view text,
                                     std::string_view first,
                                     std::string_view second) {
  std::size_t search = 0;
  while (const auto found = FindTopLevelWord(text, first, search)) {
    std::size_t offset = *found + first.size();
    if (ConsumeKeyword(text, &offset, second)) return true;
    search = offset > *found ? offset : *found + first.size();
  }
  return false;
}

std::size_t FirstTopLevelClause(std::string_view sql,
                                std::size_t begin,
                                std::initializer_list<std::string_view> clauses) {
  std::size_t result = sql.size();
  for (const auto clause : clauses) {
    if (const auto found = FindTopLevelWord(sql, clause, begin)) {
      result = std::min(result, *found);
    }
  }
  return result;
}

std::string EscapeJson(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const unsigned char ch : value) {
    switch (ch) {
      case '\\': escaped += "\\\\"; break;
      case '"': escaped += "\\\""; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        if (ch < 0x20) {
          static constexpr char kHex[] = "0123456789abcdef";
          escaped += "\\u00";
          escaped.push_back(kHex[(ch >> 4) & 0xf]);
          escaped.push_back(kHex[ch & 0xf]);
        } else {
          escaped.push_back(static_cast<char>(ch));
        }
    }
  }
  return escaped;
}

std::string HexEncodeBytes(std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(value.size() * 2u);
  for (const unsigned char byte : value) {
    encoded.push_back(kHex[(byte >> 4u) & 0x0fu]);
    encoded.push_back(kHex[byte & 0x0fu]);
  }
  return encoded;
}

std::string JsonEnvelopeHeader(std::string_view operation_id,
                               std::string_view opcode,
                               std::string_view operation_family,
                               bool transaction_required) {
  return "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
         "\"envelope_major\":3,"
         "\"sblr_version\":\"sblr_v3\","
         "\"operation_id\":\"" + EscapeJson(operation_id) + "\","
         "\"opcode\":\"" + EscapeJson(opcode) + "\","
         "\"operation_family\":\"" + EscapeJson(operation_family) + "\","
         "\"sblr_operation_family\":\"" + EscapeJson(operation_family) + "\","
         "\"result_shape\":\"engine.api.result.v1\","
         "\"diagnostic_shape\":\"engine.diagnostic.v1\","
         "\"parser_resolved_names_to_uuids\":true,"
         "\"requires_security_context\":true,"
         "\"requires_transaction_context\":" +
         std::string(transaction_required ? "true," : "false,") +
         "\"requires_cluster_authority\":false,"
         "\"contains_sql_text\":false,"
         "\"identifier_profile_uuid\":\"firebird_v5\","
         "\"source_dialect\":\"firebird\",";
}

bool ConsumeKeyword(std::string_view text,
                    std::size_t* offset,
                    std::string_view keyword) {
  if (offset == nullptr) return false;
  SkipWhitespace(text, offset);
  if (*offset + keyword.size() > text.size()) return false;
  for (std::size_t index = 0; index < keyword.size(); ++index) {
    const unsigned char actual =
        static_cast<unsigned char>(text[*offset + index]);
    const unsigned char expected =
        static_cast<unsigned char>(keyword[index]);
    if (std::toupper(actual) != std::toupper(expected)) return false;
  }
  if (*offset != 0 && IsIdentifierCharacter(text[*offset - 1])) return false;
  const std::size_t end = *offset + keyword.size();
  if (end < text.size() && IsIdentifierCharacter(text[end])) return false;
  *offset = end;
  return true;
}

bool ConsumeCharacter(std::string_view text,
                      std::size_t* offset,
                      char expected) {
  if (offset == nullptr) return false;
  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != expected) return false;
  ++*offset;
  return true;
}

bool AtEnd(std::string_view text, std::size_t offset) {
  SkipWhitespace(text, &offset);
  return offset == text.size();
}

std::string FoldRoutineIdentifier(
    const std::pair<std::string, bool>& identifier) {
  return identifier.second ? identifier.first : ToUpperAscii(identifier.first);
}

bool SameRoutineIdentifier(std::string_view left,
                           std::string_view right) {
  return ToUpperAscii(left) == ToUpperAscii(right);
}

bool ReadSignedInteger(std::string_view text,
                       std::size_t* offset,
                       std::int64_t* value) {
  if (offset == nullptr || value == nullptr) return false;
  SkipWhitespace(text, offset);
  bool negative = false;
  if (*offset < text.size() &&
      (text[*offset] == '+' || text[*offset] == '-')) {
    negative = text[*offset] == '-';
    ++*offset;
  }
  std::uint64_t magnitude = 0;
  if (!ReadUnsignedInteger(text, offset, &magnitude)) return false;
  constexpr std::uint64_t kMaximumPositive =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (!negative) {
    if (magnitude > kMaximumPositive) return false;
    *value = static_cast<std::int64_t>(magnitude);
    return true;
  }
  if (magnitude > kMaximumPositive + 1u) return false;
  *value = magnitude == kMaximumPositive + 1u
               ? std::numeric_limits<std::int64_t>::min()
               : -static_cast<std::int64_t>(magnitude);
  return true;
}

std::optional<std::pair<std::string, bool>> ReadSimpleRoutineIdentifier(
    std::string_view text,
    std::size_t* offset) {
  auto identifier = ReadIdentifier(text, offset);
  if (!identifier || identifier->first.empty() ||
      identifier->first.find('.') != std::string::npos) {
    return std::nullopt;
  }
  return identifier;
}

bool ReadIntegerParameter(std::string_view text,
                          std::size_t* offset,
                          std::string* name) {
  const auto identifier = ReadSimpleRoutineIdentifier(text, offset);
  if (!identifier || !ConsumeKeyword(text, offset, "INTEGER")) return false;
  if (name != nullptr) *name = FoldRoutineIdentifier(*identifier);
  return true;
}

bool ReadRoutineVariable(std::string_view text,
                         std::size_t* offset,
                         std::string* name) {
  if (!ConsumeCharacter(text, offset, ':')) return false;
  const auto identifier = ReadSimpleRoutineIdentifier(text, offset);
  if (!identifier) return false;
  if (name != nullptr) *name = FoldRoutineIdentifier(*identifier);
  return true;
}

FirebirdBoundedProcedureRoute ParseBoundedProcedureRouteImpl(
    std::string_view input) {
  const std::string sql = StripStatementTerminator(input);
  FirebirdBoundedProcedureRoute route;
  std::size_t offset = 0;

  if (ConsumeKeyword(sql, &offset, "EXECUTE") &&
      ConsumeKeyword(sql, &offset, "PROCEDURE")) {
    const auto procedure = ReadSimpleRoutineIdentifier(sql, &offset);
    std::int64_t lower = 0;
    std::int64_t upper = 0;
    if (!procedure || !ConsumeCharacter(sql, &offset, '(') ||
        !ReadSignedInteger(sql, &offset, &lower) ||
        !ConsumeCharacter(sql, &offset, ',') ||
        !ReadSignedInteger(sql, &offset, &upper) ||
        !ConsumeCharacter(sql, &offset, ')') || !AtEnd(sql, offset)) {
      return route;
    }
    route.kind =
        FirebirdBoundedProcedureRouteKind::kInvokeLiteralIntegerPair;
    route.procedure_name = FoldRoutineIdentifier(*procedure);
    route.procedure_quoted = procedure->second;
    route.literal_arguments = {lower, upper};
    return route;
  }

  offset = 0;
  if (!ConsumeKeyword(sql, &offset, "CREATE") ||
      !ConsumeKeyword(sql, &offset, "OR") ||
      !ConsumeKeyword(sql, &offset, "ALTER") ||
      !ConsumeKeyword(sql, &offset, "PROCEDURE")) {
    return route;
  }
  const auto procedure = ReadSimpleRoutineIdentifier(sql, &offset);
  if (!procedure) return route;
  route.procedure_name = FoldRoutineIdentifier(*procedure);
  route.procedure_quoted = procedure->second;

  std::size_t metadata_offset = offset;
  if (ConsumeKeyword(sql, &metadata_offset, "AS") &&
      ConsumeKeyword(sql, &metadata_offset, "BEGIN") &&
      ConsumeKeyword(sql, &metadata_offset, "END") &&
      AtEnd(sql, metadata_offset)) {
    route.kind =
        FirebirdBoundedProcedureRouteKind::kCreateOrAlterMetadataOnly;
    return route;
  }

  std::string lower_parameter;
  std::string upper_parameter;
  std::string return_name;
  if (!ConsumeCharacter(sql, &offset, '(') ||
      !ReadIntegerParameter(sql, &offset, &lower_parameter) ||
      !ConsumeCharacter(sql, &offset, ',') ||
      !ReadIntegerParameter(sql, &offset, &upper_parameter) ||
      !ConsumeCharacter(sql, &offset, ')') ||
      !ConsumeKeyword(sql, &offset, "RETURNS") ||
      !ConsumeCharacter(sql, &offset, '(') ||
      !ReadIntegerParameter(sql, &offset, &return_name) ||
      !ConsumeCharacter(sql, &offset, ')') ||
      !ConsumeKeyword(sql, &offset, "AS") ||
      !ConsumeKeyword(sql, &offset, "BEGIN") ||
      !ConsumeKeyword(sql, &offset, "DELETE") ||
      !ConsumeKeyword(sql, &offset, "FROM")) {
    return FirebirdBoundedProcedureRoute{};
  }
  const auto relation = ReadSimpleRoutineIdentifier(sql, &offset);
  if (!relation || !ConsumeKeyword(sql, &offset, "WHERE")) {
    return FirebirdBoundedProcedureRoute{};
  }
  const auto column = ReadSimpleRoutineIdentifier(sql, &offset);
  std::string referenced_lower;
  std::string referenced_upper;
  if (!column || !ConsumeKeyword(sql, &offset, "BETWEEN") ||
      !ReadRoutineVariable(sql, &offset, &referenced_lower) ||
      !ConsumeKeyword(sql, &offset, "AND") ||
      !ReadRoutineVariable(sql, &offset, &referenced_upper) ||
      !ConsumeCharacter(sql, &offset, ';')) {
    return FirebirdBoundedProcedureRoute{};
  }
  const auto assigned_return = ReadSimpleRoutineIdentifier(sql, &offset);
  if (!assigned_return || !ConsumeCharacter(sql, &offset, '=') ||
      !ConsumeKeyword(sql, &offset, "ROW_COUNT") ||
      !ConsumeCharacter(sql, &offset, ';') ||
      !ConsumeKeyword(sql, &offset, "SUSPEND") ||
      !ConsumeCharacter(sql, &offset, ';') ||
      !ConsumeKeyword(sql, &offset, "END") || !AtEnd(sql, offset) ||
      !SameRoutineIdentifier(lower_parameter, referenced_lower) ||
      !SameRoutineIdentifier(upper_parameter, referenced_upper) ||
      !SameRoutineIdentifier(return_name,
                             FoldRoutineIdentifier(*assigned_return))) {
    return FirebirdBoundedProcedureRoute{};
  }

  route.kind = FirebirdBoundedProcedureRouteKind::
      kCreateOrAlterDeleteColumnRangeCount;
  route.parameter_names = {std::move(lower_parameter),
                           std::move(upper_parameter)};
  route.return_name = std::move(return_name);
  route.relation_name = FoldRoutineIdentifier(*relation);
  route.relation_quoted = relation->second;
  route.column_name = FoldRoutineIdentifier(*column);
  route.column_quoted = column->second;
  return route;
}

std::string EncodeBoundedProcedureEnvelopeImpl(
    const FirebirdBoundedProcedureRoute& route,
    std::string_view schema_uuid,
    std::string_view relation_uuid,
    std::string_view column_uuid,
    std::string_view procedure_uuid) {
  std::ostringstream out;
  if (route.kind ==
      FirebirdBoundedProcedureRouteKind::kCreateOrAlterMetadataOnly) {
    if (route.procedure_name.empty() || schema_uuid.empty()) return {};
    out << JsonEnvelopeHeader("ddl.create_procedure",
                              "SBLR_DDL_CREATE_PROCEDURE",
                              "sblr.catalog.mutation.v3", true)
        << "\"target_object_kind\":\"procedure\","
        << "\"procedure_name\":\"" << EscapeJson(route.procedure_name)
        << "\",\"target_schema_uuid\":\"" << EscapeJson(schema_uuid)
        << "\",\"executor\":\"metadata_only\","
        << "\"side_effect_class\":\"none\","
        << "\"executable_descriptor_kind\":\"create_or_alter_procedure\","
        << "\"body_compilation_included\":false,"
        << "\"routine_parameter_count\":\"0\","
        << "\"routine_return_count\":\"0\","
        << "\"permission\":\"manage_executable\"}";
    return out.str();
  }
  if (route.kind == FirebirdBoundedProcedureRouteKind::
                        kCreateOrAlterDeleteColumnRangeCount) {
    if (route.procedure_name.empty() || route.parameter_names.size() != 2 ||
        route.return_name.empty() || schema_uuid.empty() ||
        relation_uuid.empty() || column_uuid.empty()) {
      return {};
    }
    constexpr std::string_view kRoutineDescriptor =
        "engine.routine.delete_column_range_count.v1";
    constexpr std::string_view kRoutineSblrHash =
        "sha256:3f4bbd573a74f8a6a99d1073cc8f6f954f030e20f44dfbcebd2f4f3df953f861";
    out << JsonEnvelopeHeader("ddl.create_procedure",
                              "SBLR_DDL_CREATE_PROCEDURE",
                              "sblr.catalog.mutation.v3", true)
        << "\"target_object_kind\":\"procedure\","
        << "\"procedure_name\":\"" << EscapeJson(route.procedure_name)
        << "\",\"target_schema_uuid\":\"" << EscapeJson(schema_uuid)
        << "\",\"executor\":\"sblr\","
        << "\"sblr_hash\":\"" << kRoutineSblrHash << "\","
        << "\"sblr_provenance\":\"engine_compiled_uuid_bound_routine_v1\","
        << "\"side_effect_class\":\"data_mutation\","
        << "\"body_compilation_included\":true,"
        << "\"executable_descriptor_kind\":\"create_or_alter_procedure\","
        << "\"compiled_body_provenance\":\"engine_compiled_uuid_bound_routine_v1\","
        << "\"compiled_body_descriptor\":\"" << kRoutineDescriptor << '|'
        << EscapeJson(relation_uuid) << '|' << EscapeJson(column_uuid)
        << "|0|1|2|2\","
        << "\"routine_parameter_count\":\"2\","
        << "\"routine_parameter_0_name\":\""
        << EscapeJson(route.parameter_names[0]) << "\","
        << "\"routine_parameter_0_mode\":\"in\","
        << "\"routine_parameter_0_type\":\"integer\","
        << "\"routine_parameter_1_name\":\""
        << EscapeJson(route.parameter_names[1]) << "\","
        << "\"routine_parameter_1_mode\":\"in\","
        << "\"routine_parameter_1_type\":\"integer\","
        << "\"routine_return_count\":\"1\","
        << "\"routine_return_0_name\":\""
        << EscapeJson(route.return_name) << "\","
        << "\"routine_return_0_type\":\"integer\","
        << "\"related_object_0_uuid\":\"" << EscapeJson(relation_uuid)
        << "\",\"related_object_0_kind\":\"table\","
        << "\"permission\":\"manage_executable\"}";
    return out.str();
  }
  if (route.kind ==
      FirebirdBoundedProcedureRouteKind::kInvokeLiteralIntegerPair) {
    if (route.literal_arguments.size() != 2 || procedure_uuid.empty()) return {};
    out << JsonEnvelopeHeader("routine.procedure_invoke",
                              "SBLR_PROCEDURE_INVOKE",
                              "sblr.routine.execute.v3", true)
        << "\"target_object_uuid\":\"" << EscapeJson(procedure_uuid)
        << "\",\"target_object_kind\":\"procedure\","
        << "\"routine_argument_count\":\"2\","
        << "\"routine_argument_0_type\":\"integer\","
        << "\"routine_argument_0_value\":\""
        << route.literal_arguments[0] << "\","
        << "\"routine_argument_1_type\":\"integer\","
        << "\"routine_argument_1_value\":\""
        << route.literal_arguments[1] << "\","
        << "\"permission\":\"invoke_executable\"}";
    return out.str();
  }
  return {};
}

std::optional<std::string> EncodeDirectDefaultExpression(
    std::string_view expression);
bool IsDirectLiteral(std::string_view input);
std::string FoldColumnName(
    const std::pair<std::string, bool>& identifier);

struct FirebirdCreateColumn {
  std::string name;
  std::string type;
  std::string charset_name;
  bool charset_quoted{false};
  std::string collation_name;
  bool collation_quoted{false};
  std::string charset_uuid;
  std::string collation_uuid;
  std::uint32_t character_length{0};
  bool text_large_object{false};
  bool nullable{true};
  bool primary_key{false};
  bool unique{false};
  std::string default_expression;
};

struct FirebirdTableKey {
  std::vector<std::string> columns;
  bool primary_key{false};
  std::string constraint_name;
};

struct FirebirdCreateTable {
  std::string name;
  bool quoted{false};
  bool recreate{false};
  bool temporary{false};
  std::string on_commit_action{"delete_rows"};
  std::vector<FirebirdCreateColumn> columns;
  std::vector<FirebirdTableKey> table_keys;
};

bool ConsumeUnquotedKeyword(std::string_view text,
                            std::size_t* offset,
                            std::string_view expected) {
  const auto identifier = ReadIdentifier(text, offset);
  return identifier && !identifier->second &&
         ToUpperAscii(identifier->first) == expected;
}

bool ParseSingleIdentifierList(
    std::string_view text,
    std::size_t* offset,
    std::pair<std::string, bool>* identifier,
    std::string* refusal_detail) {
  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != '(') {
    if (refusal_detail != nullptr) *refusal_detail = "column_list_required";
    return false;
  }
  const auto close = MatchingParenthesis(text, *offset);
  if (!close) {
    if (refusal_detail != nullptr) *refusal_detail = "column_list_unterminated";
    return false;
  }
  const auto elements = SplitTopLevel(
      text.substr(*offset + 1, *close - *offset - 1));
  if (elements.size() != 1) {
    if (refusal_detail != nullptr) *refusal_detail = "composite_foreign_key_unsupported";
    return false;
  }
  std::size_t element_offset = 0;
  const auto parsed = ReadIdentifier(elements.front(), &element_offset);
  SkipWhitespace(elements.front(), &element_offset);
  if (!parsed || element_offset != elements.front().size()) {
    if (refusal_detail != nullptr) *refusal_detail = "simple_column_identifier_required";
    return false;
  }
  if (identifier != nullptr) *identifier = *parsed;
  *offset = *close + 1;
  return true;
}

FirebirdForeignKeyAlterRoute ParseForeignKeyAlterForExecution(
    std::string_view input) {
  FirebirdForeignKeyAlterRoute route;
  const std::string sql = StripStatementTerminator(input);
  std::size_t offset = 0;
  if (!ConsumeUnquotedKeyword(sql, &offset, "ALTER") ||
      !ConsumeUnquotedKeyword(sql, &offset, "TABLE")) {
    return route;
  }

  // Only claim ALTER TABLE statements that actually attempt a referential
  // constraint.  Other ALTER shapes remain available to their own routes.
  if (!ContainsTopLevelKeywordSequence(sql, "FOREIGN", "KEY") &&
      !FindTopLevelWord(sql, "REFERENCES")) {
    return route;
  }
  route.attempted = true;
  auto refuse = [&](std::string detail) {
    route.refusal_detail = std::move(detail);
    route.supported = false;
    return route;
  };

  const auto child_table = ReadIdentifier(sql, &offset);
  if (!child_table) return refuse("child_table_required");
  route.child_table_name = FoldColumnName(*child_table);
  route.child_table_quoted = child_table->second;
  if (!ConsumeUnquotedKeyword(sql, &offset, "ADD")) {
    return refuse("add_constraint_required");
  }
  std::size_t clause_offset = offset;
  const auto first = ReadIdentifier(sql, &clause_offset);
  if (!first || first->second) return refuse("foreign_key_clause_required");
  if (ToUpperAscii(first->first) == "CONSTRAINT") {
    const auto constraint = ReadIdentifier(sql, &clause_offset);
    if (!constraint) return refuse("constraint_name_required");
    route.constraint_name = FoldColumnName(*constraint);
    route.constraint_name_quoted = constraint->second;
    if (!ConsumeUnquotedKeyword(sql, &clause_offset, "FOREIGN")) {
      return refuse("foreign_key_clause_required");
    }
  } else if (ToUpperAscii(first->first) == "FOREIGN") {
    // Firebird permits an unnamed constraint; the engine will allocate its
    // identity and the presentation adapter uses the returned display name.
  } else {
    return refuse("foreign_key_clause_required");
  }
  if (!ConsumeUnquotedKeyword(sql, &clause_offset, "KEY")) {
    return refuse("foreign_key_clause_required");
  }
  std::pair<std::string, bool> child_column;
  if (!ParseSingleIdentifierList(sql, &clause_offset, &child_column,
                                 &route.refusal_detail)) {
    return route;
  }
  route.child_column_name = FoldColumnName(child_column);
  route.child_column_quoted = child_column.second;
  if (!ConsumeUnquotedKeyword(sql, &clause_offset, "REFERENCES")) {
    return refuse("references_clause_required");
  }
  const auto parent_table = ReadIdentifier(sql, &clause_offset);
  if (!parent_table) return refuse("parent_table_required");
  route.parent_table_name = FoldColumnName(*parent_table);
  route.parent_table_quoted = parent_table->second;
  std::pair<std::string, bool> parent_column;
  if (!ParseSingleIdentifierList(sql, &clause_offset, &parent_column,
                                 &route.refusal_detail)) {
    return route;
  }
  route.parent_column_name = FoldColumnName(parent_column);
  route.parent_column_quoted = parent_column.second;
  SkipWhitespace(sql, &clause_offset);
  if (clause_offset != sql.size()) {
    return refuse("referential_actions_or_deferred_timing_unsupported");
  }
  route.supported = true;
  return route;
}

const ipc::PublicRelationColumnDescriptor* BindForeignKeyColumn(
    const ipc::PublicRelationDescriptor& descriptor,
    std::string_view presented_name,
    bool quoted) {
  const ipc::PublicRelationColumnDescriptor* matched = nullptr;
  for (const auto& candidate : descriptor.columns) {
    const bool same = quoted
                          ? candidate.canonical_name_key == presented_name
                          : ToUpperAscii(candidate.canonical_name_key) ==
                                ToUpperAscii(presented_name);
    if (!same) continue;
    if (matched != nullptr) return nullptr;
    matched = &candidate;
  }
  return matched;
}

std::string ForeignKeyAlterEnvelope(
    const FirebirdForeignKeyAlterRoute& route,
    std::string_view child_table_uuid,
    std::string_view child_column_uuid,
    std::string_view parent_table_uuid,
    std::string_view parent_column_uuid,
    std::string_view child_descriptor_uuid,
    std::uint64_t child_descriptor_generation,
    std::string_view parent_descriptor_uuid,
    std::uint64_t parent_descriptor_generation) {
  std::ostringstream canonical;
  canonical << "descriptor_version=neutral_fk_single_column_v1"
            << ";child_table_uuid=" << child_table_uuid
            << ";child_column_uuid=" << child_column_uuid
            << ";child_relation_descriptor_uuid=" << child_descriptor_uuid
            << ";child_relation_descriptor_generation="
            << child_descriptor_generation
            << ";parent_table_uuid=" << parent_table_uuid
            << ";parent_column_uuid=" << parent_column_uuid
            << ";parent_relation_descriptor_uuid=" << parent_descriptor_uuid
            << ";parent_relation_descriptor_generation="
            << parent_descriptor_generation
            << ";referenced_table_uuid=" << parent_table_uuid
            << ";referenced_column_uuid=" << parent_column_uuid
            << ";referenced_column=" << route.parent_column_name
            << ";child_column=" << route.child_column_name
            << ";constraint_name_quoted="
            << (route.constraint_name_quoted ? "true" : "false")
            << ";on_update=no_action;on_delete=no_action"
            << ";referential_action=no_action"
            << ";enforcement_timing=immediate"
            << ";deferrable=false";
  std::ostringstream out;
  out << JsonEnvelopeHeader("ddl.constraint.alter",
                            "SBLR_DDL_CONSTRAINT_ALTER",
                            "sblr.catalog.mutation.v3", true)
      << "\"target_object_uuid\":\"" << EscapeJson(child_table_uuid)
      << "\",\"target_object_kind\":\"table\","
      << "\"owner_object_uuid\":\"" << EscapeJson(child_table_uuid)
      << "\",\"constraint_kind\":\"foreign_key\","
      << "\"constraint_name\":\"" << EscapeJson(route.constraint_name)
      << "\",\"enforcement_timing\":\"immediate\","
      << "\"canonical_constraint_envelope\":\""
      << EscapeJson(canonical.str()) << "\"}";
  return out.str();
}

bool FirebirdCharacterStringTypeRequiresLength(std::string_view type) {
  const std::string normalized = ToUpperAscii(TrimAscii(type));
  return normalized.starts_with("CHAR(") ||
         normalized.starts_with("CHAR (") ||
         normalized.starts_with("VARCHAR(") ||
         normalized.starts_with("VARCHAR (") ||
         normalized.starts_with("CHARACTER(") ||
         normalized.starts_with("CHARACTER (") ||
         normalized.starts_with("CHARACTER VARYING(") ||
         normalized.starts_with("CHARACTER VARYING (") ||
         normalized.starts_with("NCHAR(") ||
         normalized.starts_with("NCHAR (") ||
         normalized.starts_with("NATIONAL CHAR(") ||
         normalized.starts_with("NATIONAL CHAR (") ||
         normalized.starts_with("NATIONAL CHARACTER(") ||
         normalized.starts_with("NATIONAL CHARACTER (");
}

bool FirebirdTextLargeObjectType(std::string_view type) {
  const std::string normalized =
      ToUpperAscii(NormalizeWhitespace(type));
  if (!StartsWithCommand(normalized, "BLOB")) return false;
  const auto subtype = normalized.find(" SUB_TYPE ");
  if (subtype == std::string::npos) return false;
  const std::string_view value = std::string_view(normalized).substr(
      subtype + std::string_view(" SUB_TYPE ").size());
  return StartsWithCommand(value, "TEXT") ||
         StartsWithCommand(value, "1");
}

bool FirebirdNationalCharacterType(std::string_view type) {
  const std::string normalized = ToUpperAscii(NormalizeWhitespace(type));
  return normalized == "NCHAR" || normalized.starts_with("NCHAR(") ||
         normalized == "NATIONAL CHAR" ||
         normalized.starts_with("NATIONAL CHAR(") ||
         normalized == "NATIONAL CHARACTER" ||
         normalized.starts_with("NATIONAL CHARACTER(");
}

bool FirebirdOrdinaryCharacterType(std::string_view type) {
  const std::string normalized = ToUpperAscii(NormalizeWhitespace(type));
  return normalized == "CHAR" || normalized.starts_with("CHAR(") ||
         normalized == "CHARACTER" || normalized.starts_with("CHARACTER(") ||
         normalized.starts_with("CHARACTER VARYING(") ||
         normalized == "VARCHAR" || normalized.starts_with("VARCHAR(");
}

void ApplyFirebirdImplicitTextResourceNames(
    FirebirdCreateTable* table,
    std::string_view database_default_charset) {
  if (table == nullptr) return;
  const std::string default_charset =
      ToUpperAscii(TrimAscii(database_default_charset));
  for (auto& column : table->columns) {
    if (!column.charset_name.empty()) continue;
    if (FirebirdNationalCharacterType(column.type)) {
      // Firebird's NATIONAL CHARACTER family has its own deterministic
      // character-set default and does not inherit the database default.
      // The Firebird-facing spelling is ISO8859_1, while neutral resource
      // lookup binds the engine seed catalog's canonical ISO-8859-1 name.
      // Firebird result/catalog rendering maps the engine descriptor back to
      // ISO8859_1 and Firebird character-set id 21.
      column.charset_name = "ISO-8859-1";
      continue;
    }
    if ((FirebirdOrdinaryCharacterType(column.type) ||
         column.text_large_object) &&
        !default_charset.empty()) {
      column.charset_name = default_charset;
    }
  }
}

std::optional<std::uint32_t> FirebirdTextCharacterLength(
    std::string_view type) {
  const std::string normalized = ToUpperAscii(TrimAscii(type));
  if (!FirebirdCharacterStringTypeRequiresLength(normalized)) {
    return std::nullopt;
  }
  const auto open = normalized.find('(');
  const auto close = normalized.find(')', open == std::string::npos
                                           ? 0
                                           : open + 1);
  if (open == std::string::npos || close == std::string::npos ||
      close <= open + 1) {
    return std::nullopt;
  }
  const std::string length_text =
      TrimAscii(std::string_view(normalized).substr(open + 1,
                                                    close - open - 1));
  if (length_text.empty()) return std::nullopt;
  std::uint64_t value = 0;
  for (const char ch : length_text) {
    if (ch < '0' || ch > '9') return std::nullopt;
    value = value * 10u + static_cast<std::uint64_t>(ch - '0');
    if (value > std::numeric_limits<std::uint32_t>::max()) {
      return std::nullopt;
    }
  }
  if (value == 0) return std::nullopt;
  return static_cast<std::uint32_t>(value);
}

template <typename Resolver>
bool BindCreateTableTextResources(FirebirdCreateTable* table,
                                  Resolver&& resolve,
                                  ipc::MessageVectorSet* messages) {
  if (table == nullptr || messages == nullptr) return false;
  for (auto& column : table->columns) {
    if (column.charset_name.empty() && column.collation_name.empty()) {
      continue;
    }
    if (column.charset_name.empty()) {
      messages->diagnostics.push_back(ipc::MakeDiagnostic(
          "FIREBIRD.BIND.COLLATION_REQUIRES_CHARSET", "ERROR",
          "A Firebird column collation requires an explicit character set in this direct lowering slice.",
          "sbp_firebird.execution_session",
          {{"column_name", column.name},
           {"collation_name", column.collation_name}}));
      return false;
    }
    if (FirebirdCharacterStringTypeRequiresLength(column.type) &&
        column.character_length == 0) {
      messages->diagnostics.push_back(ipc::MakeDiagnostic(
          "FIREBIRD.BIND.CHARACTER_LENGTH_REQUIRED", "ERROR",
          "A Firebird character-set column requires an exact character length.",
          "sbp_firebird.execution_session",
          {{"column_name", column.name}}));
      return false;
    }

    const auto charset = resolve(column.charset_name,
                                 column.charset_quoted,
                                 "charset");
    if (!charset.resolved || charset.object_uuid.empty() ||
        !charset.resource_descriptor.present ||
        charset.resource_descriptor.resource_family != "charset") {
      *messages = charset.messages;
      messages->diagnostics.push_back(ipc::MakeDiagnostic(
          "FIREBIRD.BIND.CHARSET_NOT_FOUND", "ERROR",
          "The Firebird character set was not resolved to an engine-owned resource descriptor on the selected transaction.",
          "sbp_firebird.execution_session",
          {{"column_name", column.name},
           {"charset_name", column.charset_name}}));
      return false;
    }
    column.charset_uuid = charset.object_uuid;
    column.collation_uuid =
        charset.resource_descriptor.default_collation_uuid;
    if (column.collation_name.empty()) {
      if (column.collation_uuid.empty()) {
        messages->diagnostics.push_back(ipc::MakeDiagnostic(
            "FIREBIRD.BIND.DEFAULT_COLLATION_REQUIRED", "ERROR",
            "The engine-owned character-set descriptor has no default collation UUID.",
            "sbp_firebird.execution_session",
            {{"column_name", column.name},
             {"charset_uuid", column.charset_uuid}}));
        return false;
      }
      continue;
    }

    const auto collation = resolve(column.collation_name,
                                   column.collation_quoted,
                                   "collation");
    if (!collation.resolved || collation.object_uuid.empty() ||
        !collation.resource_descriptor.present ||
        collation.resource_descriptor.resource_family != "collation") {
      *messages = collation.messages;
      messages->diagnostics.push_back(ipc::MakeDiagnostic(
          "FIREBIRD.BIND.COLLATION_NOT_FOUND", "ERROR",
          "The Firebird collation was not resolved to an engine-owned resource descriptor on the selected transaction.",
          "sbp_firebird.execution_session",
          {{"column_name", column.name},
           {"collation_name", column.collation_name}}));
      return false;
    }
    if (collation.resource_descriptor.parent_resource_uuid !=
        column.charset_uuid) {
      messages->diagnostics.push_back(ipc::MakeDiagnostic(
          "FIREBIRD.BIND.COLLATION_CHARSET_MISMATCH", "ERROR",
          "The selected Firebird collation does not belong to the selected character set.",
          "sbp_firebird.execution_session",
          {{"column_name", column.name},
           {"charset_uuid", column.charset_uuid},
           {"collation_uuid", collation.object_uuid}}));
      return false;
    }
    column.collation_uuid = collation.object_uuid;
  }
  return true;
}

std::optional<FirebirdCreateTable> ParseCreateTableForExecution(
    std::string_view input) {
  const std::string sql = StripStatementTerminator(input);
  const std::string upper = ToUpperAscii(sql);
  std::size_t offset = 0;
  bool recreate = false;
  bool temporary = false;
  if (upper.starts_with("RECREATE GLOBAL TEMPORARY TABLE")) {
    offset = std::string_view("RECREATE GLOBAL TEMPORARY TABLE").size();
    recreate = true;
    temporary = true;
  } else if (upper.starts_with("CREATE GLOBAL TEMPORARY TABLE")) {
    offset = std::string_view("CREATE GLOBAL TEMPORARY TABLE").size();
    temporary = true;
  } else if (upper.starts_with("RECREATE TABLE")) {
    offset = std::string_view("RECREATE TABLE").size();
    recreate = true;
  } else if (upper.starts_with("CREATE TABLE")) {
    offset = std::string_view("CREATE TABLE").size();
  } else {
    return std::nullopt;
  }
  const auto table = ReadIdentifier(sql, &offset);
  if (!table) return std::nullopt;
  SkipWhitespace(sql, &offset);
  if (offset >= sql.size() || sql[offset] != '(') return std::nullopt;
  const auto close = MatchingParenthesis(sql, offset);
  if (!close) return std::nullopt;
  FirebirdCreateTable parsed;
  parsed.name = table->second ? table->first : ToUpperAscii(table->first);
  parsed.quoted = table->second;
  parsed.recreate = recreate;
  parsed.temporary = temporary;
  constexpr std::size_t kMaximumTableKeys = 64;
  constexpr std::size_t kMaximumTableKeyColumns = 64;
  std::vector<FirebirdTableKey> table_keys;
  for (const auto& definition :
       SplitTopLevel(std::string_view(sql).substr(offset + 1,
                                                  *close - offset - 1))) {
    std::size_t column_offset = 0;
    const auto column_name = ReadIdentifier(definition, &column_offset);
    if (!column_name) continue;
    const std::string first_upper =
        column_name->second ? std::string{} : ToUpperAscii(column_name->first);
    if (first_upper == "CONSTRAINT" || first_upper == "PRIMARY" ||
        first_upper == "UNIQUE") {
      std::size_t key_offset = column_offset;
      std::string key_kind = first_upper;
      std::string constraint_name;
      if (key_kind == "CONSTRAINT") {
        const auto parsed_constraint_name =
            ReadIdentifier(definition, &key_offset);
        const auto constrained_kind = ReadIdentifier(definition, &key_offset);
        if (!parsed_constraint_name || parsed_constraint_name->first.empty() ||
            !constrained_kind || constrained_kind->second) {
          return std::nullopt;
        }
        constraint_name = FoldColumnName(*parsed_constraint_name);
        key_kind = ToUpperAscii(constrained_kind->first);
      }
      const bool primary_key = key_kind == "PRIMARY";
      if (!primary_key && key_kind != "UNIQUE") return std::nullopt;
      if (primary_key) {
        const auto key_word = ReadIdentifier(definition, &key_offset);
        if (!key_word || key_word->second ||
            ToUpperAscii(key_word->first) != "KEY") {
          return std::nullopt;
        }
      }
      SkipWhitespace(definition, &key_offset);
      if (key_offset >= definition.size() || definition[key_offset] != '(') {
        return std::nullopt;
      }
      const auto key_close = MatchingParenthesis(definition, key_offset);
      if (!key_close) return std::nullopt;
      const auto key_columns = SplitTopLevel(std::string_view(definition).substr(
          key_offset + 1, *key_close - key_offset - 1));
      if (key_columns.empty() ||
          key_columns.size() > kMaximumTableKeyColumns) {
        return std::nullopt;
      }
      FirebirdTableKey table_key;
      table_key.primary_key = primary_key;
      table_key.constraint_name = std::move(constraint_name);
      table_key.columns.reserve(key_columns.size());
      for (const auto& raw_key_column : key_columns) {
        std::size_t key_column_offset = 0;
        const auto key_column =
            ReadIdentifier(raw_key_column, &key_column_offset);
        if (!key_column) return std::nullopt;
        SkipWhitespace(raw_key_column, &key_column_offset);
        if (key_column_offset != raw_key_column.size()) return std::nullopt;
        const std::string folded_key_column = FoldColumnName(*key_column);
        if (folded_key_column.empty() ||
            std::find(table_key.columns.begin(), table_key.columns.end(),
                      folded_key_column) != table_key.columns.end()) {
          return std::nullopt;
        }
        table_key.columns.push_back(folded_key_column);
      }
      key_offset = *key_close + 1;
      SkipWhitespace(definition, &key_offset);
      if (key_offset != definition.size()) return std::nullopt;
      table_keys.push_back(std::move(table_key));
      if (table_keys.size() > kMaximumTableKeys) return std::nullopt;
      continue;
    }
    if (first_upper == "FOREIGN" || first_upper == "CHECK") {
      return std::nullopt;
    }
    SkipWhitespace(definition, &column_offset);
    if (column_offset >= definition.size()) continue;
    const std::string tail = TrimAscii(
        std::string_view(definition).substr(column_offset));
    const std::string tail_upper = ToUpperAscii(tail);
    if (FindTopLevelWord(tail, "CHECK") ||
        FindTopLevelWord(tail, "REFERENCES") ||
        FindTopLevelWord(tail, "COMPUTED") ||
        FindTopLevelWord(tail, "GENERATED") ||
        FindTopLevelWord(tail, "USING")) {
      return std::nullopt;
    }
    std::size_t type_end = tail.size();
    for (const auto marker : {std::string_view(" CHARACTER SET "),
                              std::string_view(" COLLATE "),
                              std::string_view(" DEFAULT "),
                              std::string_view(" NOT NULL"),
                              std::string_view(" NULL"),
                              std::string_view(" PRIMARY KEY"),
                              std::string_view(" UNIQUE"),
                              std::string_view(" CHECK "),
                              std::string_view(" REFERENCES "),
                              std::string_view(" COMPUTED "),
                              std::string_view(" GENERATED "),
                              std::string_view(" CONSTRAINT ")}) {
      const auto found = tail_upper.find(marker);
      if (found != std::string::npos) type_end = std::min(type_end, found);
    }
    FirebirdCreateColumn column;
    column.name = column_name->second ? column_name->first
                                      : ToUpperAscii(column_name->first);
    column.type = TrimAscii(std::string_view(tail).substr(0, type_end));
    column.character_length =
        FirebirdTextCharacterLength(column.type).value_or(0);
    column.text_large_object = FirebirdTextLargeObjectType(column.type);
    if (const auto charset_clause =
            FindTopLevelWord(tail, "CHARACTER SET")) {
      std::size_t charset_offset =
          *charset_clause + std::string_view("CHARACTER SET").size();
      const auto charset = ReadIdentifier(tail, &charset_offset);
      if (!charset) return std::nullopt;
      column.charset_name = charset->second
                                ? charset->first
                                : ToUpperAscii(charset->first);
      column.charset_quoted = charset->second;
    }
    if (const auto collation_clause = FindTopLevelWord(tail, "COLLATE")) {
      std::size_t collation_offset =
          *collation_clause + std::string_view("COLLATE").size();
      const auto collation = ReadIdentifier(tail, &collation_offset);
      if (!collation) return std::nullopt;
      column.collation_name = collation->second
                                  ? collation->first
                                  : ToUpperAscii(collation->first);
      column.collation_quoted = collation->second;
    }
    column.primary_key = FindTopLevelWord(tail, "PRIMARY KEY").has_value();
    column.unique = column.primary_key ||
                    FindTopLevelWord(tail, "UNIQUE").has_value();
    if (FindTopLevelWord(tail, "CONSTRAINT") && !column.unique) {
      return std::nullopt;
    }
    column.nullable = tail_upper.find(" NOT NULL") == std::string::npos &&
                      !column.primary_key;
    if (const auto default_clause = FindTopLevelWord(tail, "DEFAULT")) {
      const std::size_t default_begin =
          *default_clause + std::string_view("DEFAULT").size();
      const std::size_t default_end = FirstTopLevelClause(
          tail, default_begin,
          {"NOT NULL", "PRIMARY KEY", "UNIQUE", "CHECK", "REFERENCES",
           "COLLATE", "CHARACTER SET", "CONSTRAINT"});
      const auto encoded_default = EncodeDirectDefaultExpression(
          std::string_view(tail).substr(default_begin,
                                        default_end - default_begin));
      if (!encoded_default) return std::nullopt;
      column.default_expression = *encoded_default;
    }
    if (!column.name.empty() && !column.type.empty()) {
      parsed.columns.push_back(std::move(column));
    }
  }
  std::size_t primary_key_count = 0;
  for (const auto& column : parsed.columns) {
    if (column.primary_key) ++primary_key_count;
  }
  for (const auto& key : table_keys) {
    if (key.primary_key) ++primary_key_count;
    for (const auto& key_column : key.columns) {
      auto column = std::find_if(
          parsed.columns.begin(), parsed.columns.end(),
          [&](const auto& candidate) {
            return candidate.name == key_column;
          });
      if (column == parsed.columns.end()) return std::nullopt;
      if (key.primary_key) column->nullable = false;
    }
  }
  if (primary_key_count > 1) return std::nullopt;
  parsed.table_keys = std::move(table_keys);
  std::size_t tail_offset = *close + 1;
  SkipWhitespace(sql, &tail_offset);
  const std::string table_tail =
      ToUpperAscii(TrimAscii(std::string_view(sql).substr(tail_offset)));
  if (parsed.temporary) {
    if (table_tail.empty() || table_tail == "ON COMMIT DELETE ROWS") {
      parsed.on_commit_action = "delete_rows";
    } else if (table_tail == "ON COMMIT PRESERVE ROWS") {
      parsed.on_commit_action = "preserve_rows";
    } else {
      return std::nullopt;
    }
  } else if (!table_tail.empty()) {
    return std::nullopt;
  }
  if (parsed.name.empty() || parsed.columns.empty()) return std::nullopt;
  return parsed;
}

std::string CreateTableEnvelope(const FirebirdCreateTable& table,
                                std::string_view target_schema_uuid) {
  std::ostringstream out;
  out << JsonEnvelopeHeader("ddl.create_table", "SBLR_DDL_CREATE_TABLE",
                            "sblr.catalog.mutation.v3", true)
      << "\"table_name\":\"" << EscapeJson(table.name) << "\","
      << "\"target_object_kind\":\"table\",";
  if (table.temporary) {
    out << "\"temporary\":true,"
        << "\"temporary_scope\":\"global\","
        << "\"on_commit_action\":\""
        << EscapeJson(table.on_commit_action) << "\",";
  }
  if (!target_schema_uuid.empty()) {
    out << "\"target_schema_uuid\":\"" << EscapeJson(target_schema_uuid) << "\","
        << "\"schema_uuid\":\"" << EscapeJson(target_schema_uuid) << "\",";
  }
  out << "\"column_count\":" << table.columns.size() << ','
      << "\"column_definition_count\":" << table.columns.size();
  for (std::size_t i = 0; i < table.columns.size(); ++i) {
    const auto& column = table.columns[i];
    out << ",\"column_" << i << "_name\":\"" << EscapeJson(column.name)
        << "\",\"column_" << i << "_type\":\"" << EscapeJson(column.type)
        << "\",\"column_" << i << "_nullable\":"
        << (column.nullable ? "true" : "false")
        << ",\"column_" << i << "_descriptor\":\"type="
        << EscapeJson(column.text_large_object ? std::string_view("BLOB")
                                               : std::string_view(column.type))
        << ";nullable="
        << (column.nullable ? "true" : "false");
    if (!column.default_expression.empty()) {
      out << ";default=" << EscapeJson(column.default_expression);
    }
    if (column.primary_key) {
      out << ";primary_key=true;unique=true";
    } else if (column.unique) {
      out << ";unique=true";
    }
    if (!column.charset_uuid.empty()) {
      out << ";charset_uuid=" << EscapeJson(column.charset_uuid);
    }
    if (!column.collation_uuid.empty()) {
      out << ";collation_uuid=" << EscapeJson(column.collation_uuid);
    }
    if (column.character_length != 0) {
      out << ";character_length=" << column.character_length;
    }
    if (column.text_large_object) {
      out << ";text_resource_storage=large_object";
    }
    out << "\"";
    if (!column.default_expression.empty()) {
      out << ",\"column_" << i << "_default\":\""
          << EscapeJson(column.default_expression) << "\"";
    }
  }
  if (!table.table_keys.empty()) {
    out << ",\"table_index_count\":" << table.table_keys.size();
    for (std::size_t index = 0; index < table.table_keys.size(); ++index) {
      const auto& key = table.table_keys[index];
      out << ",\"table_index_" << index << "_key_count\":"
          << key.columns.size();
      for (std::size_t key_ordinal = 0;
           key_ordinal < key.columns.size(); ++key_ordinal) {
        out << ",\"table_index_" << index << "_key_" << key_ordinal
            << "\":\"" << EscapeJson(key.columns[key_ordinal]) << "\"";
      }
      out << ",\"table_index_" << index << "_constraint_kind\":\""
          << (key.primary_key ? "primary_key" : "unique") << "\"";
      if (!key.constraint_name.empty()) {
        out << ",\"table_index_" << index
            << "_constraint_name\":\""
            << EscapeJson(key.constraint_name) << "\"";
      }
    }
  }
  out << '}';
  return out.str();
}

std::string CreateDefaultSchemaEnvelope() {
  return JsonEnvelopeHeader("ddl.create_schema", "SBLR_DDL_CREATE_SCHEMA",
                            "sblr.catalog.mutation.v3", true) +
         "\"schema_name\":\"PUBLIC\","
         "\"name\":\"PUBLIC\","
         "\"target_object_kind\":\"schema\"}";
}

struct FirebirdInsert {
  std::string target;
  bool quoted{false};
  bool default_values{false};
  bool returning{false};
  std::vector<std::string> columns;
  std::vector<std::vector<std::string>> rows;
};

std::optional<FirebirdInsert> ParseInsertForExecution(std::string_view input) {
  const std::string sql = StripStatementTerminator(input);
  const std::string upper = ToUpperAscii(sql);
  if (!upper.starts_with("INSERT INTO")) return std::nullopt;
  std::size_t offset = std::string_view("INSERT INTO").size();
  const auto target = ReadIdentifier(sql, &offset);
  if (!target) return std::nullopt;
  FirebirdInsert parsed;
  parsed.target = target->second ? target->first : ToUpperAscii(target->first);
  parsed.quoted = target->second;
  SkipWhitespace(sql, &offset);
  if (offset < sql.size() && sql[offset] == '(') {
    const auto close = MatchingParenthesis(sql, offset);
    if (!close) return std::nullopt;
    for (const auto& raw_column :
         SplitTopLevel(std::string_view(sql).substr(offset + 1,
                                                    *close - offset - 1))) {
      std::size_t column_offset = 0;
      const auto column = ReadIdentifier(raw_column, &column_offset);
      if (!column) return std::nullopt;
      SkipWhitespace(raw_column, &column_offset);
      if (column_offset != raw_column.size()) return std::nullopt;
      parsed.columns.push_back(FoldColumnName(*column));
    }
    offset = *close + 1;
  }
  SkipWhitespace(sql, &offset);
  const std::string remaining_upper =
      ToUpperAscii(std::string_view(sql).substr(offset));
  if (remaining_upper.starts_with("DEFAULT VALUES")) {
    offset += std::string_view("DEFAULT VALUES").size();
    SkipWhitespace(sql, &offset);
    if (offset < sql.size() && StartsWithCommand(
                                   ToUpperAscii(std::string_view(sql).substr(offset)),
                                   "RETURNING")) {
      const std::size_t returning_value =
          offset + std::string_view("RETURNING").size();
      if (TrimAscii(std::string_view(sql).substr(returning_value)).empty()) {
        return std::nullopt;
      }
      parsed.returning = true;
    } else if (offset < sql.size()) {
      return std::nullopt;
    }
    parsed.default_values = true;
    parsed.rows.emplace_back();
    return parsed;
  }
  if (!remaining_upper.starts_with("VALUES")) {
    return std::nullopt;
  }
  offset += std::string_view("VALUES").size();
  while (offset < sql.size()) {
    SkipWhitespace(sql, &offset);
    if (offset < sql.size() && sql[offset] == ',') {
      ++offset;
      SkipWhitespace(sql, &offset);
    }
    if (offset >= sql.size() || sql[offset] != '(') return std::nullopt;
    const auto close = MatchingParenthesis(sql, offset);
    if (!close) return std::nullopt;
    parsed.rows.push_back(SplitTopLevel(
        std::string_view(sql).substr(offset + 1, *close - offset - 1)));
    offset = *close + 1;
    SkipWhitespace(sql, &offset);
    if (offset >= sql.size()) break;
    if (StartsWithCommand(
            ToUpperAscii(std::string_view(sql).substr(offset)), "RETURNING")) {
      const std::size_t returning_value =
          offset + std::string_view("RETURNING").size();
      if (TrimAscii(std::string_view(sql).substr(returning_value)).empty()) {
        return std::nullopt;
      }
      parsed.returning = true;
      break;
    }
    if (sql[offset] != ',') return std::nullopt;
  }
  if (parsed.rows.empty()) return std::nullopt;
  const std::size_t width = parsed.rows.front().size();
  if (width == 0 || (!parsed.columns.empty() && parsed.columns.size() != width)) {
    return std::nullopt;
  }
  for (const auto& row : parsed.rows) {
    if (row.size() != width) return std::nullopt;
    for (const auto& value : row) {
      if (!IsDirectLiteral(value)) return std::nullopt;
    }
  }
  return parsed;
}

struct FirebirdLiteral {
  std::string type;
  std::string value;
  bool is_null{false};
};

std::optional<FirebirdLiteral> BindAsciiCharLiteralExpression(
    std::string_view input) {
  constexpr std::string_view kOctetFromInt64Scalar =
      "scalar.octet_from_int64.v1";
  const std::string value = TrimAscii(input);
  std::size_t offset = 0;
  const auto function = ReadIdentifier(value, &offset);
  if (!function || function->second ||
      ToUpperAscii(function->first) != "ASCII_CHAR") {
    return std::nullopt;
  }
  SkipWhitespace(value, &offset);
  if (offset >= value.size() || value[offset] != '(') return std::nullopt;
  const auto close = MatchingParenthesis(value, offset);
  if (!close || !TrimAscii(std::string_view(value).substr(*close + 1)).empty()) {
    return std::nullopt;
  }
  const std::string argument = TrimAscii(
      std::string_view(value).substr(offset + 1, *close - offset - 1));
  if (ToUpperAscii(argument) == "NULL") {
    return FirebirdLiteral{std::string(kOctetFromInt64Scalar), "", true};
  }
  std::size_t digit = 0;
  if (!argument.empty() &&
      (argument.front() == '+' || argument.front() == '-')) {
    digit = 1;
  }
  if (digit == argument.size()) return std::nullopt;
  for (; digit < argument.size(); ++digit) {
    if (std::isdigit(static_cast<unsigned char>(argument[digit])) == 0) {
      return std::nullopt;
    }
  }
  // This is bound scalar IR, not parser-side function execution.  The engine
  // parses the signed integer, enforces Firebird's octet range, and constructs
  // the CHARACTER SET NONE byte immediately before MGA insertion.
  return FirebirdLiteral{
      std::string(kOctetFromInt64Scalar), argument, false};
}

FirebirdLiteral ParseLiteral(std::string_view input) {
  const std::string value = TrimAscii(input);
  const std::string upper = ToUpperAscii(value);
  if (const auto ascii_character = BindAsciiCharLiteralExpression(value)) {
    return *ascii_character;
  }
  if (upper == "NULL") return {"null", "", true};
  if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
    std::string decoded;
    for (std::size_t i = 1; i + 1 < value.size(); ++i) {
      if (value[i] == '\'' && i + 2 < value.size() && value[i + 1] == '\'') {
        decoded.push_back('\'');
        ++i;
      } else {
        decoded.push_back(value[i]);
      }
    }
    return {"text", std::move(decoded), false};
  }
  if (upper == "TRUE" || upper == "FALSE") {
    return {"boolean", ToUpperAscii(value) == "TRUE" ? "true" : "false", false};
  }
  bool integer = !value.empty();
  bool numeric = !value.empty();
  bool dot_seen = false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const char ch = value[i];
    if ((ch == '+' || ch == '-') && i == 0) continue;
    if (std::isdigit(static_cast<unsigned char>(ch)) != 0) continue;
    integer = false;
    if (ch == '.' && !dot_seen) {
      dot_seen = true;
      continue;
    }
    numeric = false;
    break;
  }
  if (integer) return {"bigint", value, false};
  if (numeric && dot_seen) return {"numeric", value, false};
  return {"text", value, false};
}

bool IsDirectLiteral(std::string_view input) {
  const std::string value = TrimAscii(input);
  if (value.empty()) return false;
  if (BindAsciiCharLiteralExpression(value)) return true;
  const std::string upper = ToUpperAscii(value);
  if (upper == "NULL" || upper == "TRUE" || upper == "FALSE") return true;
  if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
    bool closed = false;
    for (std::size_t i = 1; i < value.size(); ++i) {
      if (value[i] != '\'') continue;
      if (i + 1 < value.size() && value[i + 1] == '\'') {
        ++i;
        continue;
      }
      closed = i + 1 == value.size();
      break;
    }
    return closed;
  }
  std::size_t offset = (value.front() == '+' || value.front() == '-') ? 1 : 0;
  bool digit = false;
  bool dot = false;
  for (; offset < value.size(); ++offset) {
    const unsigned char ch = static_cast<unsigned char>(value[offset]);
    if (std::isdigit(ch) != 0) {
      digit = true;
      continue;
    }
    if (value[offset] == '.' && !dot) {
      dot = true;
      continue;
    }
    return false;
  }
  return digit;
}

std::optional<std::string> EncodeDirectDefaultExpression(
    std::string_view expression) {
  if (!IsDirectLiteral(expression)) return std::nullopt;
  const auto literal = ParseLiteral(expression);
  if (literal.is_null) return std::string("null");
  return std::string("literal:") + literal.value;
}

std::string FoldColumnName(const std::pair<std::string, bool>& identifier) {
  if (identifier.second) return identifier.first;
  const auto separator = identifier.first.rfind('.');
  const std::string_view leaf =
      separator == std::string::npos
          ? std::string_view(identifier.first)
          : std::string_view(identifier.first).substr(separator + 1);
  return ToUpperAscii(leaf);
}

struct FirebirdBoundPredicate {
  std::string kind;
  std::string column;
  std::vector<FirebirdLiteral> values;
};

FirebirdBoundPredicate AlwaysFalsePredicate() {
  FirebirdBoundPredicate predicate;
  predicate.kind = "always_false";
  return predicate;
}

std::optional<FirebirdBoundPredicate> ParseBoundPredicate(
    std::string_view input) {
  const std::string expression = TrimAscii(input);
  if (expression.empty()) return std::nullopt;
  std::size_t offset = 0;
  const auto column = ReadIdentifier(expression, &offset);
  if (!column) return std::nullopt;
  const std::string folded_column = FoldColumnName(*column);
  SkipWhitespace(expression, &offset);
  if (offset >= expression.size()) return std::nullopt;

  const auto between = FindTopLevelWord(expression, "BETWEEN", offset);
  if (between && *between == offset) {
    const std::size_t lower_begin =
        *between + std::string_view("BETWEEN").size();
    const auto and_word = FindTopLevelWord(expression, "AND", lower_begin);
    if (!and_word) return std::nullopt;
    const std::string lower_text = TrimAscii(
        std::string_view(expression).substr(lower_begin,
                                             *and_word - lower_begin));
    const std::string upper_text = TrimAscii(
        std::string_view(expression).substr(
            *and_word + std::string_view("AND").size()));
    if (!IsDirectLiteral(lower_text) || !IsDirectLiteral(upper_text)) {
      return std::nullopt;
    }
    FirebirdLiteral lower = ParseLiteral(lower_text);
    FirebirdLiteral upper = ParseLiteral(upper_text);
    if (lower.is_null || upper.is_null) {
      // BETWEEN with either NULL bound evaluates to UNKNOWN.  A WHERE
      // predicate therefore admits no rows, so lower only the neutral
      // no-match predicate and never publish a partially bound range.
      return AlwaysFalsePredicate();
    }
    FirebirdBoundPredicate predicate;
    predicate.kind = "column_range";
    predicate.column = folded_column;
    predicate.values.push_back(std::move(lower));
    predicate.values.push_back(std::move(upper));
    return predicate;
  }

  std::string comparison_kind;
  if (expression.compare(offset, 2, ">=") == 0) {
    comparison_kind = "column_greater_equal";
    offset += 2;
  } else if (expression.compare(offset, 2, "<=") == 0) {
    comparison_kind = "column_less_equal";
    offset += 2;
  } else if (expression.compare(offset, 2, "<>") == 0 ||
             expression.compare(offset, 2, "!=") == 0) {
    comparison_kind = "column_not_equals";
    offset += 2;
  } else if (expression[offset] == '>') {
    comparison_kind = "column_greater";
    ++offset;
  } else if (expression[offset] == '<') {
    comparison_kind = "column_less";
    ++offset;
  } else if (expression[offset] == '=') {
    comparison_kind = "column_equals";
    ++offset;
  }
  if (!comparison_kind.empty()) {
    const std::string literal_text =
        TrimAscii(std::string_view(expression).substr(offset));
    if (!IsDirectLiteral(literal_text)) return std::nullopt;
    const auto literal = ParseLiteral(literal_text);
    FirebirdBoundPredicate predicate;
    if (literal.is_null) {
      // Firebird comparisons against NULL evaluate to UNKNOWN.  A WHERE
      // predicate therefore admits no rows, independently of the comparison
      // operator.  Keep that family semantic here and lower only the neutral
      // no-match predicate shape to the engine.
      return AlwaysFalsePredicate();
    }
    predicate.kind = std::move(comparison_kind);
    predicate.column = folded_column;
    predicate.values.push_back(literal);
    return predicate;
  }
  const std::string tail = ToUpperAscii(
      TrimAscii(std::string_view(expression).substr(offset)));
  FirebirdBoundPredicate predicate;
  predicate.column = folded_column;
  if (tail == "IS NULL") {
    predicate.kind = "columns_all_null";
    return predicate;
  }
  if (tail == "IS NOT NULL") {
    predicate.kind = "columns_all_not_null";
    return predicate;
  }
  return std::nullopt;
}

std::string EncodePredicateBoundValue(const FirebirdLiteral& literal) {
  if (literal.type != "text") return literal.value;
  std::string encoded{"'"};
  encoded.reserve(literal.value.size() + 2);
  for (const char ch : literal.value) {
    if (ch == '\'') encoded.push_back('\'');
    encoded.push_back(ch);
  }
  encoded.push_back('\'');
  return encoded;
}

void AppendPredicateFields(std::ostringstream* out,
                           const FirebirdBoundPredicate& predicate) {
  *out << ",\"predicate_kind\":\"" << EscapeJson(predicate.kind) << "\""
       << ",\"predicate_column\":\"" << EscapeJson(predicate.column) << "\"";
  if (!predicate.values.empty()) {
    std::ostringstream values;
    std::ostringstream types;
    for (std::size_t index = 0; index < predicate.values.size(); ++index) {
      if (index != 0) {
        values << ',';
        types << ',';
      }
      values << EncodePredicateBoundValue(predicate.values[index]);
      types << predicate.values[index].type;
    }
    *out << ",\"predicate_value\":\"" << EscapeJson(values.str()) << "\""
         << ",\"predicate_value_type\":\"" << EscapeJson(types.str())
         << "\"";
  }
}

struct FirebirdUpdateAssignment {
  std::string target;
  std::string source;
  std::string operation;
  FirebirdLiteral literal;
};

struct FirebirdMutationWindow {
  std::uint64_t limit{0};
  std::uint64_t offset{0};
  bool no_rows{false};
};

std::optional<FirebirdMutationWindow> ParseMutationRowsClause(
    std::string_view clause) {
  const std::string rows = TrimAscii(clause);
  const std::string upper = ToUpperAscii(rows);
  if (!StartsWithCommand(upper, "ROWS")) return std::nullopt;
  std::size_t offset = std::string_view("ROWS").size();
  std::uint64_t first_row = 0;
  if (!ReadUnsignedInteger(rows, &offset, &first_row)) {
    return std::nullopt;
  }
  SkipWhitespace(rows, &offset);
  if (offset == rows.size()) {
    if (first_row == 0) return FirebirdMutationWindow{0, 0, true};
    return FirebirdMutationWindow{first_row, 0, false};
  }
  // Preserve the direct Firebird ROWS 0 form above.  The two-bound form is
  // one-based; reject a zero lower bound instead of underflowing the neutral
  // zero-based offset.
  if (first_row == 0) return std::nullopt;
  const auto to = ReadIdentifier(rows, &offset);
  if (!to || to->second || ToUpperAscii(to->first) != "TO") {
    return std::nullopt;
  }
  std::uint64_t last_row = 0;
  if (!ReadUnsignedInteger(rows, &offset, &last_row) ||
      last_row < first_row) {
    return std::nullopt;
  }
  SkipWhitespace(rows, &offset);
  if (offset != rows.size()) return std::nullopt;
  return FirebirdMutationWindow{last_row - first_row + 1,
                                first_row - 1,
                                false};
}

std::optional<FirebirdUpdateAssignment> ParseUpdateAssignment(
    std::string_view input) {
  const std::string assignment = TrimAscii(input);
  std::size_t offset = 0;
  const auto target = ReadIdentifier(assignment, &offset);
  if (!target) return std::nullopt;
  SkipWhitespace(assignment, &offset);
  if (offset >= assignment.size() || assignment[offset] != '=') {
    return std::nullopt;
  }
  const std::string folded_target = FoldColumnName(*target);
  const std::string expression =
      TrimAscii(std::string_view(assignment).substr(offset + 1));
  if (IsDirectLiteral(expression)) {
    const auto literal = ParseLiteral(expression);
    return FirebirdUpdateAssignment{folded_target, {}, "literal",
                                    literal};
  }

  std::size_t source_offset = 0;
  const auto source = ReadIdentifier(expression, &source_offset);
  if (!source) return std::nullopt;
  const std::string folded_source = FoldColumnName(*source);
  SkipWhitespace(expression, &source_offset);
  if (source_offset == expression.size()) {
    return FirebirdUpdateAssignment{folded_target, folded_source,
                                    "copy_column", {"text", {}, false}};
  }
  std::string operation;
  if (expression.compare(source_offset, 2, "||") == 0) {
    operation = "concat";
    source_offset += 2;
  } else if (expression[source_offset] == '+') {
    operation = "add";
    ++source_offset;
  } else if (expression[source_offset] == '-') {
    operation = "subtract";
    ++source_offset;
  } else if (expression[source_offset] == '*') {
    operation = "multiply";
    ++source_offset;
  } else {
    return std::nullopt;
  }
  const std::string literal_text =
      TrimAscii(std::string_view(expression).substr(source_offset));
  if (!IsDirectLiteral(literal_text)) return std::nullopt;
  return FirebirdUpdateAssignment{folded_target, folded_source,
                                  std::move(operation),
                                  ParseLiteral(literal_text)};
}

struct FirebirdUpdate {
  std::string target;
  bool quoted{false};
  std::vector<FirebirdUpdateAssignment> assignments;
  std::optional<FirebirdBoundPredicate> predicate;
  std::optional<FirebirdMutationWindow> window;
  bool returning{false};
};

std::optional<FirebirdUpdate> ParseUpdateForExecution(std::string_view input) {
  const std::string sql = StripStatementTerminator(input);
  const std::string upper = ToUpperAscii(sql);
  if (!StartsWithCommand(upper, "UPDATE")) return std::nullopt;
  std::size_t offset = std::string_view("UPDATE").size();
  const auto target = ReadIdentifier(sql, &offset);
  if (!target) return std::nullopt;
  SkipWhitespace(sql, &offset);
  auto set = FindTopLevelWord(sql, "SET", offset);
  if (set && *set != offset) {
    std::size_t alias_offset = offset;
    const auto alias_or_as = ReadIdentifier(sql, &alias_offset);
    if (!alias_or_as) return std::nullopt;
    if (!alias_or_as->second && ToUpperAscii(alias_or_as->first) == "AS") {
      const auto alias = ReadIdentifier(sql, &alias_offset);
      if (!alias) return std::nullopt;
    }
    SkipWhitespace(sql, &alias_offset);
    if (alias_offset != *set) return std::nullopt;
    offset = alias_offset;
  }
  set = FindTopLevelWord(sql, "SET", offset);
  if (!set || *set != offset) {
    return std::nullopt;
  }
  offset = *set + std::string_view("SET").size();
  const std::size_t assignment_end =
      FirstTopLevelClause(sql, offset, {"WHERE", "ROWS", "RETURNING"});
  FirebirdUpdate parsed;
  parsed.target = target->second ? target->first : ToUpperAscii(target->first);
  parsed.quoted = target->second;
  for (const auto& raw_assignment :
       SplitTopLevel(std::string_view(sql).substr(offset,
                                                 assignment_end - offset))) {
    auto assignment = ParseUpdateAssignment(raw_assignment);
    if (!assignment) return std::nullopt;
    parsed.assignments.push_back(std::move(*assignment));
  }
  if (parsed.assignments.empty()) return std::nullopt;
  if (parsed.assignments.size() > 1 ||
      parsed.assignments.front().operation != "literal") {
    for (const auto& assignment : parsed.assignments) {
      if (assignment.target.find_first_of("|;") != std::string::npos ||
          assignment.source.find_first_of("|;") != std::string::npos ||
          assignment.literal.value.find_first_of("|;") != std::string::npos ||
          assignment.literal.type.find_first_of("|;") != std::string::npos) {
        return std::nullopt;
      }
    }
  }
  std::size_t clause_offset = assignment_end;
  const auto returning = FindTopLevelWord(sql, "RETURNING", assignment_end);
  auto rows = FindTopLevelWord(sql, "ROWS", assignment_end);
  if (rows && returning && *rows > *returning) rows.reset();
  auto where = FindTopLevelWord(sql, "WHERE", assignment_end);
  if (where && ((rows && *where > *rows) ||
                (returning && *where > *returning))) {
    where.reset();
  }
  if (where) {
    if (*where != clause_offset) return std::nullopt;
    const std::size_t predicate_begin = *where + std::string_view("WHERE").size();
    const std::size_t predicate_end =
        FirstTopLevelClause(sql, predicate_begin, {"ROWS", "RETURNING"});
    parsed.predicate = ParseBoundPredicate(
        std::string_view(sql).substr(predicate_begin,
                                     predicate_end - predicate_begin));
    if (!parsed.predicate) return std::nullopt;
    clause_offset = predicate_end;
    SkipWhitespace(sql, &clause_offset);
  }
  if (rows) {
    if (*rows != clause_offset) return std::nullopt;
    const std::size_t rows_end = returning ? *returning : sql.size();
    if (rows_end <= *rows) return std::nullopt;
    parsed.window = ParseMutationRowsClause(
        std::string_view(sql).substr(*rows, rows_end - *rows));
    if (!parsed.window) return std::nullopt;
    if (parsed.window->no_rows) {
      parsed.predicate = AlwaysFalsePredicate();
      parsed.window.reset();
    }
    clause_offset = rows_end;
    SkipWhitespace(sql, &clause_offset);
  }
  if (returning) {
    if (*returning != clause_offset ||
        TrimAscii(std::string_view(sql).substr(
            *returning + std::string_view("RETURNING").size())).empty()) {
      return std::nullopt;
    }
    parsed.returning = true;
    clause_offset = sql.size();
  }
  if (clause_offset != sql.size()) return std::nullopt;
  return parsed;
}

std::string UpdateEnvelope(const FirebirdUpdate& update,
                           std::string_view target_uuid) {
  std::ostringstream out;
  out << JsonEnvelopeHeader("dml.update_rows", "SBLR_DML_UPDATE_ROWS",
                            "sblr.dml.update.v3", true)
      << "\"target_object_uuid\":\"" << EscapeJson(target_uuid) << "\","
      << "\"target_object_kind\":\"table\","
      << "\"dml_surface_variant\":\"update\"";
  if (update.assignments.size() == 1 &&
      update.assignments.front().operation == "literal") {
    const auto& assignment = update.assignments.front();
    out << ",\"assignment_column\":\"" << EscapeJson(assignment.target) << "\""
        << ",\"assignment_value\":\"" << EscapeJson(assignment.literal.value) << "\""
        << ",\"assignment_value_type\":\"" << EscapeJson(assignment.literal.type)
        << "\"";
  } else {
    std::ostringstream plan;
    for (std::size_t index = 0; index < update.assignments.size(); ++index) {
      const auto& assignment = update.assignments[index];
      if (index != 0) plan << ';';
      plan << assignment.target << '|' << assignment.source << '|'
           << assignment.operation << '|' << assignment.literal.value << '|'
           << assignment.literal.type;
    }
    out << ",\"assignment_plan\":\"" << EscapeJson(plan.str()) << "\"";
  }
  if (update.predicate) AppendPredicateFields(&out, *update.predicate);
  if (update.window) {
    out << ",\"limit\":" << update.window->limit;
    if (update.window->offset != 0) {
      out << ",\"offset\":" << update.window->offset;
    }
  }
  if (update.returning) {
    out << ",\"result_payload_policy\":\"full_payload\"";
  }
  out << '}';
  return out.str();
}

struct FirebirdDelete {
  std::string target;
  bool quoted{false};
  std::optional<FirebirdBoundPredicate> predicate;
  std::optional<FirebirdMutationWindow> window;
  bool returning{false};
};

std::optional<FirebirdDelete> ParseDeleteForExecution(std::string_view input) {
  const std::string sql = StripStatementTerminator(input);
  const std::string upper = ToUpperAscii(sql);
  if (!StartsWithCommand(upper, "DELETE FROM")) return std::nullopt;
  std::size_t offset = std::string_view("DELETE FROM").size();
  const auto target = ReadIdentifier(sql, &offset);
  if (!target) return std::nullopt;
  FirebirdDelete parsed;
  parsed.target = target->second ? target->first : ToUpperAscii(target->first);
  parsed.quoted = target->second;
  SkipWhitespace(sql, &offset);
  std::size_t clause_offset = offset;
  const auto returning = FindTopLevelWord(sql, "RETURNING", offset);
  auto rows = FindTopLevelWord(sql, "ROWS", offset);
  if (rows && returning && *rows > *returning) rows.reset();
  auto where = FindTopLevelWord(sql, "WHERE", offset);
  if (where && ((rows && *where > *rows) ||
                (returning && *where > *returning))) {
    where.reset();
  }
  if (where) {
    if (*where != clause_offset) return std::nullopt;
    const std::size_t predicate_begin = *where + std::string_view("WHERE").size();
    const std::size_t predicate_end =
        FirstTopLevelClause(sql, predicate_begin, {"ROWS", "RETURNING"});
    parsed.predicate = ParseBoundPredicate(
        std::string_view(sql).substr(predicate_begin,
                                     predicate_end - predicate_begin));
    if (!parsed.predicate) return std::nullopt;
    clause_offset = predicate_end;
    SkipWhitespace(sql, &clause_offset);
  }
  if (rows) {
    if (*rows != clause_offset) return std::nullopt;
    const std::size_t rows_end = returning ? *returning : sql.size();
    if (rows_end <= *rows) return std::nullopt;
    parsed.window = ParseMutationRowsClause(
        std::string_view(sql).substr(*rows, rows_end - *rows));
    if (!parsed.window) return std::nullopt;
    if (parsed.window->no_rows) {
      parsed.predicate = AlwaysFalsePredicate();
      parsed.window.reset();
    }
    clause_offset = rows_end;
    SkipWhitespace(sql, &clause_offset);
  }
  if (returning) {
    if (*returning != clause_offset ||
        TrimAscii(std::string_view(sql).substr(
            *returning + std::string_view("RETURNING").size())).empty()) {
      return std::nullopt;
    }
    parsed.returning = true;
    clause_offset = sql.size();
  }
  if (clause_offset != sql.size()) return std::nullopt;
  return parsed;
}

std::string DeleteEnvelope(const FirebirdDelete& deletion,
                           std::string_view target_uuid) {
  std::ostringstream out;
  out << JsonEnvelopeHeader("dml.delete_rows", "SBLR_DML_DELETE_ROWS",
                            "sblr.dml.delete.v3", true)
      << "\"target_object_uuid\":\"" << EscapeJson(target_uuid) << "\","
      << "\"target_object_kind\":\"table\","
      << "\"dml_surface_variant\":\"delete\"";
  if (deletion.predicate) AppendPredicateFields(&out, *deletion.predicate);
  if (deletion.window) {
    out << ",\"limit\":" << deletion.window->limit;
    if (deletion.window->offset != 0) {
      out << ",\"offset\":" << deletion.window->offset;
    }
  }
  if (deletion.returning) {
    out << ",\"result_payload_policy\":\"full_payload\"";
  }
  out << '}';
  return out.str();
}

struct FirebirdDropTable {
  std::string target;
  bool quoted{false};
};

std::optional<FirebirdDropTable> ParseDropTableForExecution(
    std::string_view input) {
  const std::string sql = StripStatementTerminator(input);
  const std::string upper = ToUpperAscii(sql);
  if (!StartsWithCommand(upper, "DROP TABLE")) return std::nullopt;
  std::size_t offset = std::string_view("DROP TABLE").size();
  const auto target = ReadIdentifier(sql, &offset);
  if (!target) return std::nullopt;
  const std::string tail =
      ToUpperAscii(TrimAscii(std::string_view(sql).substr(offset)));
  // The neutral drop API currently enforces dependency-safe restriction.  A
  // Firebird CASCADE request must not be silently weakened to RESTRICT.
  if (!tail.empty() && tail != "RESTRICT") {
    return std::nullopt;
  }
  return FirebirdDropTable{
      target->second ? target->first : ToUpperAscii(target->first),
      target->second};
}

std::string DropTableEnvelope(std::string_view target_uuid) {
  return JsonEnvelopeHeader("ddl.drop_object", "SBLR_DDL_DROP_OBJECT",
                            "sblr.catalog.mutation.v3", true) +
         "\"target_object_uuid\":\"" + EscapeJson(target_uuid) + "\","
         "\"target_object_kind\":\"table\","
         "\"drop_mode\":\"restrict\"}";
}

std::string InsertEnvelope(const FirebirdInsert& insert,
                           std::string_view target_uuid) {
  std::ostringstream out;
  out << JsonEnvelopeHeader("dml.insert_rows", "SBLR_DML_INSERT_ROWS",
                            "sblr.dml.insert.v3", true)
      << "\"target_object_uuid\":\"" << EscapeJson(target_uuid) << "\","
      << "\"target_object_kind\":\"table\","
      << "\"dml_surface_variant\":\"firebird_insert_values\",";
  if (insert.default_values) {
    out << "\"insert_default_values_row_count\":\"1\",";
  }
  out << "\"insert_values_row_count\":\"" << insert.rows.size() << "\","
      << "\"insert_values_column_count\":\"" << insert.rows.front().size() << "\","
      << "\"insert_values_column_list_present\":"
      << (!insert.columns.empty() ? "true" : "false") << ','
      << "\"insert_values_parser_executes_sql\":false";
  if (insert.returning) {
    out << ",\"result_payload_policy\":\"full_payload\"";
  }
  std::string compact_payload;
  for (std::size_t row = 0; row < insert.rows.size(); ++row) {
    for (std::size_t column = 0; column < insert.rows[row].size(); ++column) {
      const FirebirdLiteral literal = ParseLiteral(insert.rows[row][column]);
      const std::string field_name =
          column < insert.columns.size() ? insert.columns[column]
                                         : "c" + std::to_string(column);
      if (!compact_payload.empty()) compact_payload.push_back(';');
      compact_payload += HexEncodeBytes(field_name);
      compact_payload.push_back('|');
      compact_payload += HexEncodeBytes(literal.type);
      compact_payload.push_back('|');
      compact_payload += HexEncodeBytes(literal.value);
      compact_payload.push_back('|');
      compact_payload += literal.is_null ? "1" : "0";
    }
  }
  if (!compact_payload.empty()) {
    // This is a family-neutral SBLR cell packet.  Hex encoding is required for
    // CHARACTER SET NONE values, which may contain arbitrary bytes that are
    // neither valid UTF-8 nor safely representable as JSON string text.
    out << ",\"insert_values_compact_format\":"
           "\"sblr.dml.insert.cells.hex.v1\""
        << ",\"insert_values_compact_payload\":\""
        << compact_payload << "\"";
  }
  out << '}';
  return out.str();
}

struct FirebirdSimpleSelect {
  std::string source;
  bool quoted{false};
  std::vector<std::string> projection;
  bool count_all{false};
  std::string count_column_name;
  std::optional<FirebirdBoundPredicate> predicate;
  std::string order_by;
  std::string order_direction{"asc"};
  std::uint64_t limit{0};
  std::uint64_t offset{0};
};

std::optional<std::string> ParseCountAllProjectionName(
    std::string_view projection) {
  const std::string text = TrimAscii(projection);
  std::size_t offset = 0;
  const auto function = ReadIdentifier(text, &offset);
  if (!function || function->second ||
      ToUpperAscii(function->first) != "COUNT") {
    return std::nullopt;
  }
  SkipWhitespace(text, &offset);
  if (offset >= text.size() || text[offset] != '(') return std::nullopt;
  const auto close = MatchingParenthesis(text, offset);
  if (!close || TrimAscii(std::string_view(text).substr(
                    offset + 1, *close - offset - 1)) != "*") {
    return std::nullopt;
  }
  offset = *close + 1;
  SkipWhitespace(text, &offset);
  if (offset == text.size()) return std::string("COUNT");

  auto alias = ReadIdentifier(text, &offset);
  if (!alias) return std::nullopt;
  if (!alias->second && ToUpperAscii(alias->first) == "AS") {
    alias = ReadIdentifier(text, &offset);
    if (!alias) return std::nullopt;
  }
  SkipWhitespace(text, &offset);
  if (offset != text.size()) return std::nullopt;
  return FoldColumnName(*alias);
}

std::optional<FirebirdSimpleSelect> ParseSimpleSelectForExecution(
    std::string_view input) {
  const std::string sql = StripStatementTerminator(input);
  const std::string upper = ToUpperAscii(sql);
  if (!upper.starts_with("SELECT ")) return std::nullopt;
  const std::size_t from = upper.find(" FROM ");
  if (from == std::string::npos) return std::nullopt;
  const std::string raw_projection_text = TrimAscii(
      std::string_view(sql).substr(std::string_view("SELECT").size(),
                                   from - std::string_view("SELECT").size()));
  std::size_t projection_begin = 0;
  std::uint64_t requested_limit = 0;
  std::uint64_t requested_offset = 0;
  bool first_present = false;
  bool skip_present = false;
  bool no_rows = false;
  while (projection_begin < raw_projection_text.size()) {
    SkipWhitespace(raw_projection_text, &projection_begin);
    const std::string remaining = ToUpperAscii(
        std::string_view(raw_projection_text).substr(projection_begin));
    if (!first_present && StartsWithCommand(remaining, "FIRST")) {
      projection_begin += std::string_view("FIRST").size();
      if (!ReadUnsignedInteger(raw_projection_text, &projection_begin,
                               &requested_limit) ||
          projection_begin >= raw_projection_text.size() ||
          std::isspace(static_cast<unsigned char>(
              raw_projection_text[projection_begin])) == 0) {
        return std::nullopt;
      }
      no_rows = requested_limit == 0;
      first_present = true;
      continue;
    }
    if (!skip_present && StartsWithCommand(remaining, "SKIP")) {
      projection_begin += std::string_view("SKIP").size();
      if (!ReadUnsignedInteger(raw_projection_text, &projection_begin,
                               &requested_offset) ||
          projection_begin >= raw_projection_text.size() ||
          std::isspace(static_cast<unsigned char>(
              raw_projection_text[projection_begin])) == 0) {
        return std::nullopt;
      }
      skip_present = true;
      continue;
    }
    break;
  }
  const std::string projection_text = TrimAscii(
      std::string_view(raw_projection_text).substr(projection_begin));
  std::vector<std::string> projection;
  const auto count_column_name =
      ParseCountAllProjectionName(projection_text);
  if (count_column_name && (first_present || skip_present)) {
    // FIRST/SKIP applies after aggregation in Firebird.  The neutral table
    // count route intentionally refuses to reinterpret it as an input-row
    // window.
    return std::nullopt;
  }
  if (!count_column_name && projection_text != "*") {
    for (const auto& item : SplitTopLevel(projection_text)) {
      std::size_t projection_offset = 0;
      const auto identifier = ReadIdentifier(item, &projection_offset);
      if (!identifier) return std::nullopt;
      SkipWhitespace(item, &projection_offset);
      if (projection_offset != item.size()) return std::nullopt;
      projection.push_back(FoldColumnName(*identifier));
      if (projection.size() > 16) return std::nullopt;
    }
    if (projection.empty()) return std::nullopt;
  }
  std::size_t offset = from + std::string_view(" FROM ").size();
  SkipWhitespace(sql, &offset);
  if (offset >= sql.size() || sql[offset] == '(') return std::nullopt;
  const auto source = ReadIdentifier(sql, &offset);
  if (!source) return std::nullopt;
  SkipWhitespace(sql, &offset);
  const auto where_at_source = FindTopLevelWord(sql, "WHERE", offset);
  const auto order_at_source = FindTopLevelWord(sql, "ORDER BY", offset);
  const auto rows_at_source = FindTopLevelWord(sql, "ROWS", offset);
  if (offset < sql.size() &&
      (!where_at_source || *where_at_source != offset) &&
      (!order_at_source || *order_at_source != offset) &&
      (!rows_at_source || *rows_at_source != offset)) {
    std::size_t alias_offset = offset;
    const auto alias_or_as = ReadIdentifier(sql, &alias_offset);
    if (!alias_or_as) return std::nullopt;
    const std::string alias_upper =
        alias_or_as->second ? std::string{} : ToUpperAscii(alias_or_as->first);
    if (alias_upper == "AS") {
      const auto alias = ReadIdentifier(sql, &alias_offset);
      if (!alias) return std::nullopt;
    } else if (alias_upper == "ROWS" || alias_upper == "GROUP" ||
               alias_upper == "HAVING" || alias_upper == "UNION" ||
               alias_upper == "JOIN" || alias_upper == "LEFT" ||
               alias_upper == "RIGHT" || alias_upper == "FULL" ||
               alias_upper == "INNER" || alias_upper == "CROSS") {
      return std::nullopt;
    }
    SkipWhitespace(sql, &alias_offset);
    offset = alias_offset;
  }
  FirebirdSimpleSelect parsed;
  parsed.source = source->second ? source->first : ToUpperAscii(source->first);
  parsed.quoted = source->second;
  parsed.projection = std::move(projection);
  parsed.count_all = count_column_name.has_value();
  if (count_column_name) parsed.count_column_name = *count_column_name;
  parsed.limit = requested_limit;
  parsed.offset = requested_offset;

  std::size_t clause_offset = offset;
  if (const auto where = FindTopLevelWord(sql, "WHERE", clause_offset)) {
    if (*where != clause_offset) return std::nullopt;
    const std::size_t predicate_begin = *where + std::string_view("WHERE").size();
    const std::size_t predicate_end =
        FirstTopLevelClause(sql, predicate_begin, {"ORDER BY", "ROWS"});
    parsed.predicate = ParseBoundPredicate(
        std::string_view(sql).substr(predicate_begin,
                                     predicate_end - predicate_begin));
    if (!parsed.predicate ||
        (parsed.predicate->kind != "column_equals" &&
         parsed.predicate->kind != "column_not_equals" &&
         parsed.predicate->kind != "column_less" &&
         parsed.predicate->kind != "column_less_equal" &&
         parsed.predicate->kind != "column_greater" &&
         parsed.predicate->kind != "column_greater_equal" &&
         parsed.predicate->kind != "column_range" &&
         parsed.predicate->kind != "always_false" &&
         parsed.predicate->kind != "columns_all_null" &&
         parsed.predicate->kind != "columns_all_not_null")) {
      return std::nullopt;
    }
    clause_offset = predicate_end;
    SkipWhitespace(sql, &clause_offset);
  }

  if (clause_offset < sql.size()) {
    const auto order_clause = FindTopLevelWord(sql, "ORDER BY", clause_offset);
    if (order_clause && *order_clause == clause_offset) {
      std::size_t order_offset =
          *order_clause + std::string_view("ORDER BY").size();
      const std::size_t order_end =
          FirstTopLevelClause(sql, order_offset, {"ROWS"});
      const auto order = ReadIdentifier(sql, &order_offset);
      if (!order || order_offset > order_end) return std::nullopt;
      parsed.order_by = FoldColumnName(*order);
      SkipWhitespace(sql, &order_offset);
      if (order_offset < order_end) {
        const std::string direction = ToUpperAscii(TrimAscii(
            std::string_view(sql).substr(order_offset,
                                         order_end - order_offset)));
        if (direction != "ASC" && direction != "DESC") return std::nullopt;
        parsed.order_direction = direction == "DESC" ? "desc" : "asc";
      }
      clause_offset = order_end;
      SkipWhitespace(sql, &clause_offset);
    }
  }

  if (clause_offset < sql.size()) {
    const auto rows_clause = FindTopLevelWord(sql, "ROWS", clause_offset);
    if (!rows_clause || *rows_clause != clause_offset || first_present ||
        skip_present || parsed.count_all) {
      return std::nullopt;
    }
    std::size_t rows_offset =
        *rows_clause + std::string_view("ROWS").size();
    std::uint64_t first_row = 0;
    if (!ReadUnsignedInteger(sql, &rows_offset, &first_row)) {
      return std::nullopt;
    }
    SkipWhitespace(sql, &rows_offset);
    if (rows_offset == sql.size()) {
      if (first_row == 0) {
        no_rows = true;
      } else {
        parsed.limit = first_row;
        parsed.offset = 0;
      }
    } else {
      if (first_row == 0) return std::nullopt;
      const std::string remaining =
          ToUpperAscii(std::string_view(sql).substr(rows_offset));
      if (!StartsWithCommand(remaining, "TO")) return std::nullopt;
      rows_offset += std::string_view("TO").size();
      std::uint64_t last_row = 0;
      if (!ReadUnsignedInteger(sql, &rows_offset, &last_row) ||
          last_row < first_row) {
        return std::nullopt;
      }
      SkipWhitespace(sql, &rows_offset);
      if (rows_offset != sql.size()) return std::nullopt;
      parsed.offset = first_row - 1;
      parsed.limit = last_row - first_row + 1;
    }
  }
  if (no_rows) parsed.predicate = AlwaysFalsePredicate();
  return parsed;
}

std::string SelectEnvelope(const FirebirdSimpleSelect& select,
                           std::string_view source_uuid) {
  std::ostringstream out;
  out << JsonEnvelopeHeader("dml.select_rows", "SBLR_DML_SELECT_ROWS",
                            "sblr.query.relational.v3", true)
      << "\"target_object_uuid\":\"" << EscapeJson(source_uuid) << "\","
      << "\"target_object_kind\":\"table\","
      << "\"source_uuid\":\"" << EscapeJson(source_uuid) << "\","
      << "\"source_kind\":\"table\","
      << "\"dml_surface_variant\":\"firebird_select\"";
  if (select.count_all) {
    out << ",\"result_projection\":\"count\""
        << ",\"actual_column_name\":\""
        << EscapeJson(select.count_column_name) << "\"";
  } else if (!select.projection.empty()) {
    out << ",\"projection_count\":" << select.projection.size();
    for (std::size_t index = 0; index < select.projection.size(); ++index) {
      out << ",\"projection_" << index << "\":\""
          << EscapeJson(select.projection[index]) << "\"";
    }
  }
  if (select.predicate) AppendPredicateFields(&out, *select.predicate);
  if (!select.order_by.empty()) {
    out << ",\"order_by\":\"" << EscapeJson(select.order_by)
        << "\",\"order_direction\":\""
        << EscapeJson(select.order_direction) << "\"";
  }
  if (select.limit != 0) {
    out << ",\"limit\":" << select.limit;
  }
  if (select.offset != 0) {
    out << ",\"offset\":" << select.offset;
  }
  out << '}';
  return out.str();
}

std::string ResultRowField(std::string_view payload, std::string_view field) {
  const std::string marker = std::string(field) + "=";
  const std::size_t row = payload.find("row[0]=");
  if (row == std::string_view::npos) return {};
  std::size_t begin = payload.find(marker, row + 7);
  if (begin == std::string_view::npos) return {};
  begin += marker.size();
  const std::size_t end = payload.find_first_of(";\r\n", begin);
  return std::string(payload.substr(
      begin, end == std::string_view::npos ? payload.size() - begin
                                           : end - begin));
}

std::string TransactionSavepointName(std::string_view sql,
                                     std::string_view upper) {
  for (const auto prefix : {std::string_view("ROLLBACK TRANSACTION TO SAVEPOINT "),
                            std::string_view("ROLLBACK TRANSACTION TO "),
                            std::string_view("ROLLBACK WORK TO SAVEPOINT "),
                            std::string_view("ROLLBACK WORK TO "),
                            std::string_view("ROLLBACK TO SAVEPOINT "),
                            std::string_view("ROLLBACK TO "),
                            std::string_view("RELEASE SAVEPOINT "),
                            std::string_view("SAVEPOINT ")}) {
    if (upper.starts_with(prefix)) {
      return TrimAscii(sql.substr(prefix.size()));
    }
  }
  return {};
}

std::string TransactionControlEnvelope(
    std::string_view operation_id,
    std::string_view opcode,
    bool retaining = false,
    std::string_view encoded_policy_fields = {}) {
  std::string envelope =
      "envelope=SBLRExecutionEnvelope.v3\n"
      "envelope_major=3\n"
      "sblr_version=sblr_v3\n"
      "operation_id=" + std::string(operation_id) + "\n"
      "opcode=" + std::string(opcode) + "\n"
      "sblr_operation_family=sblr.transaction.control.v3\n"
      "result_shape=engine.api.result.v1\n"
      "diagnostic_shape=engine.diagnostic.v1\n"
      "parser_resolved_names_to_uuids=true\n"
      "requires_security_context=true\n"
      "requires_transaction_context=" +
      std::string(operation_id == "transaction.begin" ? "false\n" : "true\n") +
      "requires_cluster_authority=false\n"
      "contains_sql_text=false\n"
      "source_dialect=firebird\n";
  if (retaining) envelope += "retaining=true\n";
  envelope.append(encoded_policy_fields);
  if (!encoded_policy_fields.empty() && encoded_policy_fields.back() != '\n') {
    envelope.push_back('\n');
  }
  return envelope;
}

std::string FirebirdTransactionEnvelope(std::string_view sql) {
  const std::string normalized = NormalizeWhitespace(sql);
  const std::string upper = ToUpperAscii(normalized);
  std::string operation_id;
  std::string opcode;
  if (StartsWithCommand(upper, "BEGIN TRANSACTION") ||
      StartsWithCommand(upper, "SET TRANSACTION")) {
    operation_id = "transaction.begin";
    opcode = "SBLR_TRANSACTION_BEGIN";
  } else if (StartsWithCommand(upper, "COMMIT")) {
    operation_id = "transaction.commit";
    opcode = "SBLR_TRANSACTION_COMMIT";
  } else if (StartsWithCommand(upper, "ROLLBACK TO") ||
             StartsWithCommand(upper, "ROLLBACK WORK TO") ||
             StartsWithCommand(upper, "ROLLBACK TRANSACTION TO")) {
    operation_id = "transaction.rollback_to_savepoint";
    opcode = "SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT";
  } else if (StartsWithCommand(upper, "ROLLBACK")) {
    operation_id = "transaction.rollback";
    opcode = "SBLR_TRANSACTION_ROLLBACK";
  } else if (StartsWithCommand(upper, "RELEASE SAVEPOINT")) {
    operation_id = "transaction.release_savepoint";
    opcode = "SBLR_TRANSACTION_RELEASE_SAVEPOINT";
  } else if (StartsWithCommand(upper, "SAVEPOINT")) {
    operation_id = "transaction.create_savepoint";
    opcode = "SBLR_TRANSACTION_CREATE_SAVEPOINT";
  }
  if (operation_id.empty()) return {};
  const bool retaining =
      operation_id == "transaction.commit" ||
              operation_id == "transaction.rollback"
          ? FindTopLevelWord(normalized, "RETAINING").has_value()
          : false;
  std::string envelope =
      TransactionControlEnvelope(operation_id, opcode, retaining);
  const std::string savepoint_name = TransactionSavepointName(normalized, upper);
  if (!savepoint_name.empty()) {
    envelope += "savepoint_name=" + savepoint_name + "\n";
  }
  return envelope;
}

std::optional<std::string> BindForeignKeyAlterEnvelopeExact(
    FirebirdExecutionSession* session,
    const FirebirdForeignKeyAlterRoute& route,
    const ipc::ParserTransactionSelector& transaction,
    ipc::MessageVectorSet* messages) {
  if (session == nullptr || messages == nullptr) return std::nullopt;
  const auto child = session->ResolveRelationDescriptorPublicOnTransaction(
      route.child_table_name, route.child_table_quoted, transaction);
  if (!child.resolved || child.object_uuid.empty() ||
      !child.relation_descriptor.present ||
      child.relation_descriptor.relation_uuid != child.object_uuid ||
      child.relation_descriptor.descriptor_uuid.empty()) {
    *messages = child.messages;
    messages->diagnostics.push_back(ipc::MakeDiagnostic(
        "FIREBIRD.FKEY.CHILD_DESCRIPTOR_REQUIRED", "ERROR",
        "The foreign-key child relation must resolve to one complete engine-owned relation descriptor on the exact transaction.",
        "sbp_firebird.execution_session",
        {{"presented_name", route.child_table_name}}));
    return std::nullopt;
  }
  const auto parent = session->ResolveRelationDescriptorPublicOnTransaction(
      route.parent_table_name, route.parent_table_quoted, transaction);
  if (!parent.resolved || parent.object_uuid.empty() ||
      !parent.relation_descriptor.present ||
      parent.relation_descriptor.relation_uuid != parent.object_uuid ||
      parent.relation_descriptor.descriptor_uuid.empty()) {
    *messages = parent.messages;
    messages->diagnostics.push_back(ipc::MakeDiagnostic(
        "FIREBIRD.FKEY.PARENT_DESCRIPTOR_REQUIRED", "ERROR",
        "The foreign-key parent relation must resolve to one complete engine-owned relation descriptor on the exact transaction.",
        "sbp_firebird.execution_session",
        {{"presented_name", route.parent_table_name}}));
    return std::nullopt;
  }
  const auto* child_column = BindForeignKeyColumn(
      child.relation_descriptor, route.child_column_name,
      route.child_column_quoted);
  const auto* parent_column = BindForeignKeyColumn(
      parent.relation_descriptor, route.parent_column_name,
      route.parent_column_quoted);
  if (child_column == nullptr || child_column->column_uuid.empty() ||
      parent_column == nullptr || parent_column->column_uuid.empty()) {
    messages->diagnostics.push_back(ipc::MakeDiagnostic(
        "FIREBIRD.FKEY.EXACT_COLUMN_BINDING_REQUIRED", "ERROR",
        "A single-column foreign key requires exact engine-owned child and parent column UUID bindings.",
        "sbp_firebird.execution_session",
        {{"child_relation_uuid", child.object_uuid},
         {"child_column", route.child_column_name},
         {"parent_relation_uuid", parent.object_uuid},
         {"parent_column", route.parent_column_name}}));
    return std::nullopt;
  }
  return ForeignKeyAlterEnvelope(route,
                                 child.object_uuid,
                                 child_column->column_uuid,
                                 parent.object_uuid,
                                 parent_column->column_uuid,
                                 child.relation_descriptor.descriptor_uuid,
                                 child.relation_descriptor.descriptor_generation,
                                 parent.relation_descriptor.descriptor_uuid,
                                 parent.relation_descriptor.descriptor_generation);
}

} // namespace

void ApplyFirebirdOrdinaryRelationSelectFallback(
    FirebirdPipelineResult* result,
    std::string ordinary_select_sblr) {
  if (result == nullptr) return;
  result->global_aggregate_view_select_route = {};
  result->global_aggregate_view_result_kind =
      FirebirdGlobalAvgResultKind::kUnsupported;
  result->global_aggregate_view_result_alias.clear();
  result->relation_projection_view_select_route = {};
  result->relation_projection_view_outputs.clear();
  result->relation_projection_view_uuid.clear();
  result->relation_projection_view_descriptor_uuid.clear();
  result->relation_projection_view_descriptor_generation = 0;
  result->sblr_payload = std::move(ordinary_select_sblr);
}

FirebirdBoundedProcedureRoute ParseFirebirdBoundedProcedureRoute(
    std::string_view firebird_sql) {
  return ParseBoundedProcedureRouteImpl(firebird_sql);
}

FirebirdForeignKeyAlterRoute ParseFirebirdForeignKeyAlterRoute(
    std::string_view firebird_sql) {
  return ParseForeignKeyAlterForExecution(firebird_sql);
}

std::string EncodeFirebirdBoundedProcedureEnvelope(
    const FirebirdBoundedProcedureRoute& route,
    std::string_view schema_uuid,
    std::string_view relation_uuid,
    std::string_view column_uuid,
    std::string_view procedure_uuid) {
  return EncodeBoundedProcedureEnvelopeImpl(route, schema_uuid, relation_uuid,
                                            column_uuid, procedure_uuid);
}

std::string_view FirebirdBoundedProcedureRouteName(
    FirebirdBoundedProcedureRouteKind kind) {
  switch (kind) {
    case FirebirdBoundedProcedureRouteKind::kCreateOrAlterMetadataOnly:
      return "create_or_alter_metadata_only";
    case FirebirdBoundedProcedureRouteKind::
        kCreateOrAlterDeleteColumnRangeCount:
      return "create_or_alter_delete_column_range_count";
    case FirebirdBoundedProcedureRouteKind::kInvokeLiteralIntegerPair:
      return "invoke_literal_integer_pair";
    case FirebirdBoundedProcedureRouteKind::kUnsupported:
      break;
  }
  return "unsupported";
}

FirebirdExecutionSession::FirebirdExecutionSession(
    ipc::ParserClientConfig config)
    : config_(std::move(config)), client_(config_.server_endpoint) {
  // Firebird exposes concurrent transaction handles.  It must never attach
  // through the scalar SBPS V1 transaction projection.
  config_.require_transaction_routing_v2 = true;
  // Firebird metadata prepares are commonly owned by D__trans while the
  // resulting executable is invoked on M__trans.  The engine retains the
  // immutable metadata binding; this parser retains only its opaque prepared
  // identity and the two engine-issued transaction selectors.
  config_.require_prepared_metadata_transfer_v1 = true;
  // Physical relation SQLDA/catalog metadata must come from the persisted
  // engine descriptor, not a parser-family overlay reconstructed from DDL.
  config_.require_relation_descriptor_projection_v3 = true;
}

bool FirebirdExecutionSession::HasExecutionRoute() const {
  return !config_.server_endpoint.empty();
}

bool FirebirdExecutionSession::AuthenticateCredentials(
    const ipc::AuthCredentialEnvelope& credentials,
    ipc::MessageVectorSet* messages) {
  if (!HasExecutionRoute()) {
    if (messages != nullptr) {
      messages->diagnostics.push_back(ipc::MakeDiagnostic(
          "FIREBIRD.SERVER.UNAVAILABLE", "ERROR",
          "Firebird authentication requires the neutral parser-server route.",
          "sbp_firebird.execution_session"));
    }
    return false;
  }
  if (session_.authenticated) {
    if (messages != nullptr) {
      messages->diagnostics.push_back(ipc::MakeDiagnostic(
          "FIREBIRD.AUTH.ALREADY_ATTACHED", "ERROR",
          "A Firebird execution session cannot attach a second logical session on its physical parser-server channel.",
          "sbp_firebird.execution_session"));
    }
    return false;
  }
  if (!client_.AuthenticateAndAttach(credentials, config_, &session_, messages)) {
    return false;
  }
  if (session_.transaction_routing_v2_negotiated &&
      session_.prepared_metadata_transfer_v1_negotiated &&
      session_.relation_descriptor_projection_v3_negotiated &&
      InitialTransactionSelector().present()) {
    return true;
  }
  if (messages != nullptr) {
    messages->diagnostics.push_back(ipc::MakeDiagnostic(
        "FIREBIRD.PREPARED_METADATA_TRANSFER.V1_ATTACH_INCOMPLETE", "ERROR",
        "Firebird attach did not publish the required transaction-routing, prepared-metadata-transfer, relation-projection capabilities and initial engine-issued transaction selector.",
        "sbp_firebird.execution_session"));
  }
  ipc::MessageVectorSet cleanup_messages;
  (void)client_.DisconnectSession(session_, &cleanup_messages);
  session_ = {};
  return false;
}

ipc::ParserTransactionSelector
FirebirdExecutionSession::InitialTransactionSelector() const {
  return {session_.local_transaction_id, session_.transaction_uuid};
}

ipc::ServerExecutionResult FirebirdExecutionSession::BeginAdditional(
    const FirebirdTransactionPolicy& policy) const {
  if (!HasExecutionRoute()) {
    return RejectedExecution(
        "FIREBIRD.SERVER.UNAVAILABLE",
        "Firebird transaction begin requires the neutral parser-server route.");
  }
  if (!session_.authenticated) {
    return RejectedExecution(
        "FIREBIRD.AUTH.REQUIRED",
        "Firebird transaction begin requires an authenticated server session.");
  }
  ipc::ParserTransactionRouting routing;
  routing.route = ipc::ParserTransactionRoute::kBeginAdditional;
  return client_.ExecuteSblrRouted(
      session_,
      TransactionControlEnvelope(
          "transaction.begin",
          "SBLR_TRANSACTION_BEGIN",
          false,
          EncodeNeutralTransactionSblrFields(policy)),
      routing,
      false);
}

ipc::ServerExecutionResult FirebirdExecutionSession::ExecuteSblrRouted(
    std::string_view encoded_sblr_envelope,
    const ipc::ParserTransactionSelector& transaction,
    bool cursor_requested) const {
  if (!HasExecutionRoute()) {
    return RejectedExecution(
        "FIREBIRD.SERVER.UNAVAILABLE",
        "Firebird SBLR submission requires the neutral parser-server route.");
  }
  if (!session_.authenticated) {
    return RejectedExecution(
        "FIREBIRD.AUTH.REQUIRED",
        "Firebird SBLR submission requires an authenticated server session.");
  }
  if (!transaction.present()) {
    return RejectedExecution(
        "FIREBIRD.TRANSACTION.SELECTOR_REQUIRED",
        "Firebird SBLR submission requires an exact engine-issued transaction selector.");
  }
  ipc::ParserTransactionRouting routing;
  routing.route = ipc::ParserTransactionRoute::kSelected;
  routing.selector = transaction;
  return client_.ExecuteSblrRouted(session_, encoded_sblr_envelope, routing,
                                   cursor_requested);
}

ipc::ServerPrepareSblrResult FirebirdExecutionSession::PrepareSblrRouted(
    std::string_view encoded_sblr_envelope,
    const ipc::ParserTransactionSelector& transaction) const {
  if (!HasExecutionRoute()) {
    return RejectedPrepare(
        "FIREBIRD.SERVER.UNAVAILABLE",
        "Firebird SBLR prepare requires the neutral parser-server route.");
  }
  if (!session_.authenticated) {
    return RejectedPrepare(
        "FIREBIRD.AUTH.REQUIRED",
        "Firebird SBLR prepare requires an authenticated server session.");
  }
  if (!transaction.present()) {
    return RejectedPrepare(
        "FIREBIRD.TRANSACTION.SELECTOR_REQUIRED",
        "Firebird SBLR prepare requires an exact engine-issued transaction selector.");
  }
  return client_.PrepareSblrRouted(session_, encoded_sblr_envelope,
                                   transaction);
}

ipc::ServerExecutionResult
FirebirdExecutionSession::ExecutePreparedSblrRouted(
    std::string_view prepared_statement_uuid,
    const ipc::ParserTransactionSelector& transaction,
    std::string_view encoded_sblr_envelope,
    const std::vector<std::uint8_t>& data_packet,
    bool cursor_requested) const {
  if (!HasExecutionRoute()) {
    return RejectedExecution(
        "FIREBIRD.SERVER.UNAVAILABLE",
        "Firebird prepared SBLR execution requires the neutral parser-server route.");
  }
  if (!session_.authenticated) {
    return RejectedExecution(
        "FIREBIRD.AUTH.REQUIRED",
        "Firebird prepared SBLR execution requires an authenticated server session.");
  }
  if (!transaction.present()) {
    return RejectedExecution(
        "FIREBIRD.TRANSACTION.SELECTOR_REQUIRED",
        "Firebird prepared SBLR execution requires an exact engine-issued transaction selector.");
  }
  return client_.ExecutePreparedSblrRouted(session_,
                                           prepared_statement_uuid,
                                           transaction,
                                           encoded_sblr_envelope,
                                           data_packet,
                                           cursor_requested);
}

ipc::ServerClosePreparedSblrResult
FirebirdExecutionSession::ClosePreparedSblrOnRoute(
    std::string_view prepared_statement_uuid) const {
  if (!HasExecutionRoute()) {
    return RejectedClosePrepared(
        "FIREBIRD.SERVER.UNAVAILABLE",
        "Firebird prepared SBLR close requires the neutral parser-server route.");
  }
  if (!session_.authenticated) {
    return RejectedClosePrepared(
        "FIREBIRD.AUTH.REQUIRED",
        "Firebird prepared SBLR close requires an authenticated server session.");
  }
  if (prepared_statement_uuid.empty()) {
    return RejectedClosePrepared(
        "FIREBIRD.DSQL.PREPARED_HANDLE_REQUIRED",
        "Firebird prepared SBLR close requires an opaque neutral prepared statement identity.");
  }
  // Closing is session-owned resource retirement.  It intentionally carries
  // no transaction selector and never asks the parser to infer MGA finality.
  return client_.ClosePreparedSblr(session_, prepared_statement_uuid);
}

ipc::PublicNameResolutionResult
FirebirdExecutionSession::ResolveNamePublicOnTransaction(
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class,
    const ipc::ParserTransactionSelector& transaction) const {
  if (!HasExecutionRoute()) {
    return RejectedResolution(
        "FIREBIRD.SERVER.UNAVAILABLE",
        "Firebird name resolution requires the neutral parser-server route.");
  }
  if (!session_.authenticated) {
    return RejectedResolution(
        "FIREBIRD.AUTH.REQUIRED",
        "Firebird name resolution requires an authenticated server session.");
  }
  if (!transaction.present()) {
    return RejectedResolution(
        "FIREBIRD.TRANSACTION.SELECTOR_REQUIRED",
        "Firebird name resolution requires an exact engine-issued transaction selector.");
  }
  return client_.ResolveNamePublicOnTransaction(session_,
                                                presented_name,
                                                quoted,
                                                object_class,
                                                config_,
                                                transaction);
}

ipc::PublicNameResolutionResult
FirebirdExecutionSession::ResolveNameSemanticPublicOnTransaction(
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class,
    const ipc::ParserTransactionSelector& transaction) const {
  if (!HasExecutionRoute()) {
    return RejectedResolution(
        "FIREBIRD.SERVER.UNAVAILABLE",
        "Firebird semantic name resolution requires the neutral parser-server route.");
  }
  if (!session_.authenticated) {
    return RejectedResolution(
        "FIREBIRD.AUTH.REQUIRED",
        "Firebird semantic name resolution requires an authenticated server session.");
  }
  if (!transaction.present()) {
    return RejectedResolution(
        "FIREBIRD.TRANSACTION.SELECTOR_REQUIRED",
        "Firebird semantic name resolution requires an exact engine-issued transaction selector.");
  }
  return client_.ResolveNameSemanticPublicOnTransaction(
      session_, presented_name, quoted, object_class, config_, transaction);
}

ipc::PublicNameResolutionResult
FirebirdExecutionSession::ResolveRelationDescriptorPublicOnTransaction(
    std::string_view presented_name,
    bool quoted,
    const ipc::ParserTransactionSelector& transaction) const {
  if (!HasExecutionRoute()) {
    return RejectedResolution(
        "FIREBIRD.SERVER.UNAVAILABLE",
        "Firebird persisted relation description requires the neutral parser-server route.");
  }
  if (!session_.authenticated) {
    return RejectedResolution(
        "FIREBIRD.AUTH.REQUIRED",
        "Firebird persisted relation description requires an authenticated server session.");
  }
  if (!transaction.present()) {
    return RejectedResolution(
        "FIREBIRD.TRANSACTION.SELECTOR_REQUIRED",
        "Firebird persisted relation description requires an exact engine-issued transaction selector.");
  }
  return client_.ResolveRelationDescriptorPublicOnTransaction(
      session_, presented_name, quoted, "table", config_, transaction);
}

ipc::ServerExecutionResult FirebirdExecutionSession::CommitTransaction(
    const ipc::ParserTransactionSelector& transaction) const {
  return ExecuteSblrRouted(
      TransactionControlEnvelope("transaction.commit",
                                 "SBLR_TRANSACTION_COMMIT"),
      transaction);
}

ipc::ServerExecutionResult FirebirdExecutionSession::RollbackTransaction(
    const ipc::ParserTransactionSelector& transaction) const {
  return ExecuteSblrRouted(
      TransactionControlEnvelope("transaction.rollback",
                                 "SBLR_TRANSACTION_ROLLBACK"),
      transaction);
}

ipc::ServerExecutionResult
FirebirdExecutionSession::CommitRetainingTransaction(
    const ipc::ParserTransactionSelector& transaction) const {
  return ExecuteSblrRouted(
      TransactionControlEnvelope("transaction.commit",
                                 "SBLR_TRANSACTION_COMMIT",
                                 true),
      transaction);
}

ipc::ServerExecutionResult
FirebirdExecutionSession::RollbackRetainingTransaction(
    const ipc::ParserTransactionSelector& transaction) const {
  return ExecuteSblrRouted(
      TransactionControlEnvelope("transaction.rollback",
                                 "SBLR_TRANSACTION_ROLLBACK",
                                 true),
      transaction);
}

FirebirdPipelineResult FirebirdExecutionSession::BindAndLowerForPrepare(
    std::string_view firebird_sql,
    const ipc::ParserTransactionSelector& transaction,
    std::string_view database_default_charset,
    std::string_view attachment_charset) {
  const auto parsed = ParseStatement(firebird_sql);
  const auto bounded_procedure =
      ParseFirebirdBoundedProcedureRoute(firebird_sql);
  const auto bounded_execute_block = ParseFirebirdBoundedExecuteBlockRoute(
      firebird_sql,
      {std::string(database_default_charset), std::string(attachment_charset)});
  const auto foreign_key_alter =
      ParseFirebirdForeignKeyAlterRoute(firebird_sql);
  auto reject = [&](std::string code, std::string message) {
    auto result = Rejected(std::move(code), std::move(message));
    result.statement_family = parsed.statement_family;
    result.operation_family = parsed.operation_family;
    return result;
  };
  if (!parsed.ok) {
    return reject("FIREBIRD.PARSE.REJECTED",
                  "The Firebird-owned parser rejected the statement.");
  }
  if (foreign_key_alter.attempted && !foreign_key_alter.supported) {
    return reject(
        "FIREBIRD.FKEY.SHAPE_UNSUPPORTED",
        "The standalone Firebird lowerer supports only one immediate single-column FOREIGN KEY with NO ACTION semantics (" +
            foreign_key_alter.refusal_detail + ").");
  }

  for (const auto& token : LexTokens(firebird_sql)) {
    if (!bounded_procedure.recognized() && token.kind == "parameter") {
      return reject(
          "FIREBIRD.DSQL.PARAMETER_PACKET_UNAVAILABLE",
          "Prepared Firebird input parameters require a canonical neutral data packet encoder.");
    }
  }

  auto create_table = ParseCreateTableForExecution(firebird_sql);
  if (create_table) {
    ApplyFirebirdImplicitTextResourceNames(&*create_table,
                                           database_default_charset);
  }
  const auto insert = ParseInsertForExecution(firebird_sql);
  const auto update = ParseUpdateForExecution(firebird_sql);
  const auto deletion = ParseDeleteForExecution(firebird_sql);
  const auto drop_table = ParseDropTableForExecution(firebird_sql);
  const auto scalar_projection =
      ParseFirebirdScalarProjectionRoute(
          firebird_sql, {std::string(attachment_charset)});
  const auto global_count_projection =
      ParseFirebirdGlobalCountProjectionRoute(firebird_sql);
  const auto global_avg_projection =
      ParseFirebirdGlobalAvgProjectionRoute(firebird_sql);
  const auto relation_projection_view_create =
      ParseFirebirdRelationProjectionViewCreateRoute(firebird_sql);
  const auto relation_projection_view_create_v2 =
      ParseFirebirdRelationProjectionViewCreateV2Route(firebird_sql);
  const auto relation_projection_view_delete_v2 =
      ParseFirebirdRelationProjectionViewDeleteV2Route(firebird_sql);
  const auto relation_projection_view_select =
      ParseFirebirdRelationProjectionViewSelectRoute(firebird_sql);
  const auto global_aggregate_view_create =
      ParseFirebirdGlobalAggregateViewCreateRoute(firebird_sql);
  const auto global_aggregate_view_select =
      ParseFirebirdGlobalAggregateViewSelectRoute(firebird_sql);
  const auto select = ParseSimpleSelectForExecution(firebird_sql);
  if (parsed.statement_family == "transaction") {
    return reject(
        "FIREBIRD.DSQL.PREPARED_TRANSACTION_CONTROL_UNSUPPORTED",
        "Prepared transaction control must use the Firebird transaction operation path.");
  }
  if (create_table && create_table->recreate) {
    // Prepared DSQL must remain mutation-free, but Firebird clients prepare
    // RECREATE before executing it.  Lower the CREATE half here and let the
    // worker perform the optional DROP prelude on the same exact MGA
    // transaction immediately before prepared execution.
    create_table->recreate = false;
  }
  if (global_count_projection.attempted &&
      !global_count_projection.recognized()) {
    return reject(
        "FIREBIRD.AGGREGATE.SHAPE_UNSUPPORTED",
        "The standalone Firebird prepared lowerer supports only COUNT(*), COUNT(field), COUNT(DISTINCT field) in the exact three-output global aggregate shape.");
  }
  if (global_avg_projection.attempted &&
      !global_avg_projection.recognized()) {
    return reject(
        "FIREBIRD.AGGREGATE.SHAPE_UNSUPPORTED",
        "The standalone Firebird prepared lowerer supports only one direct-relation AVG([DISTINCT] field) projection with an optional AS alias.");
  }
  if (relation_projection_view_create_v2.attempted &&
      !relation_projection_view_create_v2.recognized()) {
    return reject(
        "FIREBIRD.RELATION_VIEW.V2.CREATE_SHAPE_UNSUPPORTED",
        "The standalone Firebird prepared lowerer supports only CREATE VIEW v [(out)] AS SELECT direct_int32_column FROM one_table for the V2 route.");
  }
  if (!relation_projection_view_create_v2.attempted &&
      relation_projection_view_create.attempted &&
      !relation_projection_view_create.recognized()) {
    return reject(
        "FIREBIRD.RELATION_VIEW.SHAPE_UNSUPPORTED",
        "The standalone Firebird prepared lowerer supports only CREATE VIEW name [(source_name, literal_name)] AS SELECT int32_column, int32_literal AS literal_name FROM table with unquoted output identities.");
  }
  if (!relation_projection_view_create_v2.attempted &&
      !relation_projection_view_create.attempted &&
      global_aggregate_view_create.attempted &&
      !global_aggregate_view_create.recognized()) {
    return reject(
        "FIREBIRD.AGGREGATE_VIEW.SHAPE_UNSUPPORTED",
        "The standalone Firebird prepared lowerer supports only CREATE [OR ALTER] VIEW name AS SELECT AVG(int32_literal * int32_field) AS alias FROM relation.");
  }
  if (!bounded_procedure.recognized() && !bounded_execute_block.recognized() &&
      !foreign_key_alter.recognized() && !create_table && !insert && !update &&
      !deletion && !drop_table &&
      !scalar_projection.recognized() &&
      !global_count_projection.recognized() &&
      !global_avg_projection.recognized() &&
      !relation_projection_view_create_v2.recognized() &&
      !relation_projection_view_create.recognized() &&
      !relation_projection_view_delete_v2.recognized() &&
      !relation_projection_view_select.recognized() &&
      !global_aggregate_view_create.recognized() &&
      !global_aggregate_view_select.recognized() && !select) {
    return reject(
        "FIREBIRD.SBLR.LOWERING_UNAVAILABLE",
        "The Firebird-owned prepared lowerer does not support this statement shape.");
  }
  if (!HasExecutionRoute()) {
    return reject("FIREBIRD.SERVER.UNAVAILABLE",
                  "Firebird prepared lowering requires an execution route for exact name resolution.");
  }
  if (!session_.authenticated) {
    return reject("FIREBIRD.AUTH.REQUIRED",
                  "Firebird prepared lowering requires an authenticated server session.");
  }
  if (!transaction.present()) {
    return reject(
        "FIREBIRD.TRANSACTION.SELECTOR_REQUIRED",
        "Firebird prepared lowering requires an exact engine-issued transaction selector.");
  }

  FirebirdPipelineResult result;
  result.accepted = true;
  result.statement_family = parsed.statement_family;
  result.operation_family = parsed.operation_family;
  result.procedural_block_route = bounded_execute_block;
  result.global_count_projection_route = global_count_projection;
  result.global_avg_projection_route = global_avg_projection;
  result.relation_projection_view_create_route =
      relation_projection_view_create;
  result.relation_projection_view_create_v2_route =
      relation_projection_view_create_v2;
  result.relation_projection_view_delete_v2_route =
      relation_projection_view_delete_v2;
  result.relation_projection_view_select_route =
      relation_projection_view_select;
  result.global_aggregate_view_create_route = global_aggregate_view_create;
  result.global_aggregate_view_select_route = global_aggregate_view_select;

  auto unresolved = [&](std::string_view presented_name,
                        std::string_view action,
                        ipc::MessageVectorSet messages) {
    result.accepted = false;
    result.messages = std::move(messages);
    if (result.messages.diagnostics.empty()) {
      result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
          "FIREBIRD.BIND.UNRESOLVED_RELATION", "ERROR",
          "The Firebird-owned prepared binder could not resolve the relation.",
          "sbp_firebird.execution_session",
          {{"object_class", "table"},
           {"presented_name", std::string(presented_name)},
           {"binding_action", std::string(action)}}));
    }
    return result;
  };
  auto resolve_table = [&](std::string_view presented_name,
                           bool quoted,
                           std::string_view action)
      -> std::optional<std::string> {
    const auto resolved = ResolveNamePublicOnTransaction(
        presented_name, quoted, "table", transaction);
    if (!resolved.resolved || resolved.object_uuid.empty()) {
      result = unresolved(presented_name, action, resolved.messages);
      return std::nullopt;
    }
    return resolved.object_uuid;
  };

  if (foreign_key_alter.recognized()) {
    const auto envelope = BindForeignKeyAlterEnvelopeExact(
        this, foreign_key_alter, transaction, &result.messages);
    if (!envelope) {
      result.accepted = false;
      return result;
    }
    result.sblr_payload = *envelope;
  } else if (bounded_procedure.recognized()) {
    if (bounded_procedure.kind ==
        FirebirdBoundedProcedureRouteKind::kInvokeLiteralIntegerPair) {
      const auto resolved = ResolveNamePublicOnTransaction(
          bounded_procedure.procedure_name,
          bounded_procedure.procedure_quoted,
          "procedure",
          transaction);
      if (!resolved.resolved || resolved.object_uuid.empty()) {
        result.accepted = false;
        result.messages = resolved.messages;
        if (result.messages.diagnostics.empty()) {
          result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
              "FIREBIRD.BIND.UNRESOLVED_PROCEDURE", "ERROR",
              "The Firebird-owned prepared binder could not resolve the procedure on the exact transaction.",
              "sbp_firebird.execution_session",
              {{"object_class", "procedure"},
               {"presented_name", bounded_procedure.procedure_name}}));
        }
        return result;
      }
      result.sblr_payload = EncodeFirebirdBoundedProcedureEnvelope(
          bounded_procedure, {}, {}, {}, resolved.object_uuid);
    } else {
      const auto resolved_schema = ResolveNamePublicOnTransaction(
          "users.public", true, "schema", transaction);
      if (!resolved_schema.resolved ||
          resolved_schema.object_uuid.empty()) {
        result.accepted = false;
        result.messages = resolved_schema.messages;
        result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
            "FIREBIRD.BIND.DEFAULT_SCHEMA_REQUIRED_FOR_ROUTINE", "ERROR",
            "CREATE OR ALTER PROCEDURE requires the engine bootstrap users.public schema on the exact transaction.",
            "sbp_firebird.execution_session"));
        return result;
      }

      std::string relation_uuid;
      std::string column_uuid;
      if (bounded_procedure.kind == FirebirdBoundedProcedureRouteKind::
                                        kCreateOrAlterDeleteColumnRangeCount) {
        const auto resolved_relation =
            ResolveRelationDescriptorPublicOnTransaction(
                bounded_procedure.relation_name,
                bounded_procedure.relation_quoted,
                transaction);
        const auto& descriptor = resolved_relation.relation_descriptor;
        if (!resolved_relation.resolved ||
            resolved_relation.object_uuid.empty() || !descriptor.present ||
            descriptor.relation_uuid != resolved_relation.object_uuid ||
            descriptor.descriptor_uuid.empty()) {
          result.accepted = false;
          result.messages = resolved_relation.messages;
          result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
              "FIREBIRD.ROUTINE.RELATION_DESCRIPTOR_REQUIRED", "ERROR",
              "The bounded Firebird procedure requires the complete engine-owned relation descriptor on the exact transaction.",
              "sbp_firebird.execution_session",
              {{"presented_name", bounded_procedure.relation_name}}));
          return result;
        }
        relation_uuid = resolved_relation.object_uuid;
        const ipc::PublicRelationColumnDescriptor* matched_column = nullptr;
        for (const auto& candidate : descriptor.columns) {
          const bool matches = bounded_procedure.column_quoted
                                   ? candidate.canonical_name_key ==
                                         bounded_procedure.column_name
                                   : ToUpperAscii(candidate.canonical_name_key) ==
                                         ToUpperAscii(
                                             bounded_procedure.column_name);
          if (!matches) continue;
          if (matched_column != nullptr) {
            result.accepted = false;
            result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
                "FIREBIRD.ROUTINE.COLUMN_BINDING_AMBIGUOUS", "ERROR",
                "The bounded Firebird procedure column binding is ambiguous in the persisted relation descriptor.",
                "sbp_firebird.execution_session",
                {{"relation_uuid", relation_uuid},
                 {"column_name", bounded_procedure.column_name}}));
            return result;
          }
          matched_column = &candidate;
        }
        const std::string canonical_type =
            matched_column == nullptr
                ? std::string{}
                : ToUpperAscii(TrimAscii(
                      matched_column->canonical_type_name));
        if (matched_column == nullptr ||
            matched_column->column_uuid.empty() ||
            (canonical_type != "INTEGER" && canonical_type != "INT" &&
             canonical_type != "INT32")) {
          result.accepted = false;
          result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
              "FIREBIRD.ROUTINE.INTEGER_COLUMN_BINDING_REQUIRED", "ERROR",
              "The bounded Firebird procedure requires one exact engine-owned INTEGER column UUID.",
              "sbp_firebird.execution_session",
              {{"relation_uuid", relation_uuid},
               {"column_name", bounded_procedure.column_name}}));
          return result;
        }
        column_uuid = matched_column->column_uuid;
      }
      result.sblr_payload = EncodeFirebirdBoundedProcedureEnvelope(
          bounded_procedure,
          resolved_schema.object_uuid,
          relation_uuid,
          column_uuid,
          {});
    }
  } else if (bounded_execute_block.recognized()) {
    result.sblr_payload =
        EncodeFirebirdBoundedExecuteBlockEnvelope(bounded_execute_block);
  } else if (relation_projection_view_create_v2.recognized()) {
    const auto resolved_schema = ResolveNamePublicOnTransaction(
        "users.public", true, "schema", transaction);
    const auto resolved_relation =
        ResolveRelationDescriptorPublicOnTransaction(
            relation_projection_view_create_v2.source_relation,
            relation_projection_view_create_v2.source_relation_quoted,
            transaction);
    auto bound = BindFirebirdRelationProjectionViewCreateV2(
        relation_projection_view_create_v2, resolved_schema,
        resolved_relation);
    if (!bound.accepted) {
      result.accepted = false;
      result.messages = std::move(bound.messages);
      return result;
    }
    result.sblr_payload =
        EncodeFirebirdRelationProjectionViewCreateV2Envelope(bound);
  } else if (relation_projection_view_create.recognized()) {
    const auto resolved_schema = ResolveNamePublicOnTransaction(
        "users.public", true, "schema", transaction);
    const auto resolved_relation =
        ResolveRelationDescriptorPublicOnTransaction(
            relation_projection_view_create.source_relation,
            relation_projection_view_create.source_relation_quoted,
            transaction);
    auto bound = BindFirebirdRelationProjectionViewCreate(
        relation_projection_view_create, resolved_schema, resolved_relation);
    if (!bound.accepted) {
      result.accepted = false;
      result.messages = std::move(bound.messages);
      return result;
    }
    result.sblr_payload =
        EncodeFirebirdRelationProjectionViewCreateEnvelope(bound);
  } else if (global_aggregate_view_create.recognized()) {
    const auto resolved_schema = ResolveNamePublicOnTransaction(
        "users.public", true, "schema", transaction);
    const auto resolved_relation =
        ResolveRelationDescriptorPublicOnTransaction(
            global_aggregate_view_create.source_relation,
            global_aggregate_view_create.source_relation_quoted,
            transaction);
    auto bound = BindFirebirdGlobalAggregateViewCreate(
        global_aggregate_view_create, resolved_schema, resolved_relation);
    if (!bound.accepted) {
      result.accepted = false;
      result.messages = std::move(bound.messages);
      return result;
    }
    result.sblr_payload =
        EncodeFirebirdGlobalAggregateViewCreateEnvelope(bound);
  } else if (create_table) {
    if (!BindCreateTableTextResources(
            &*create_table,
            [&](std::string_view presented_name,
                bool quoted,
                std::string_view object_class) {
              return ResolveNamePublicOnTransaction(
                  presented_name, quoted, object_class, transaction);
            },
            &result.messages)) {
      result.accepted = false;
      return result;
    }
    // Firebird's unqualified user objects bind to its PUBLIC schema view;
    // the neutral engine identity is the bootstrap users.public path.
    const auto resolved_schema = ResolveNamePublicOnTransaction(
        "users.public", true, "schema", transaction);
    if (!resolved_schema.resolved || resolved_schema.object_uuid.empty()) {
      result.accepted = false;
      result.messages = resolved_schema.messages;
      result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
          "FIREBIRD.BIND.DEFAULT_SCHEMA_REQUIRED_FOR_PREPARE", "ERROR",
          "Prepared CREATE TABLE requires the engine bootstrap users.public schema; prepare cannot create it.",
          "sbp_firebird.execution_session"));
      return result;
    }
    result.sblr_payload =
        CreateTableEnvelope(*create_table, resolved_schema.object_uuid);
  } else if (insert) {
    const auto target_uuid =
        resolve_table(insert->target, insert->quoted, "insert");
    if (!target_uuid) return result;
    result.sblr_payload = InsertEnvelope(*insert, *target_uuid);
  } else if (update) {
    const auto target_uuid =
        resolve_table(update->target, update->quoted, "update");
    if (!target_uuid) return result;
    result.sblr_payload = UpdateEnvelope(*update, *target_uuid);
  } else if (deletion) {
    // Generic DELETE keeps its exact table-first binding.  Only after that
    // public lookup reports no table may the independently recognized V2
    // shape request the exact public view descriptor; generic resolution is
    // never widened from table to view.
    const auto resolved_table = ResolveNamePublicOnTransaction(
        deletion->target, deletion->quoted, "table", transaction);
    if (resolved_table.resolved && !resolved_table.object_uuid.empty()) {
      result.sblr_payload =
          DeleteEnvelope(*deletion, resolved_table.object_uuid);
    } else if (resolved_table.resolved !=
               !resolved_table.object_uuid.empty()) {
      result.accepted = false;
      result.messages = resolved_table.messages;
      result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
          "FIREBIRD.BIND.TABLE_RESOLUTION_INVALID", "ERROR",
          "The table-first Firebird DELETE lookup returned an incoherent resolution identity.",
          "sbp_firebird.execution_session",
          {{"presented_name", deletion->target},
           {"resolved", resolved_table.resolved ? "true" : "false"},
           {"object_uuid_present",
            resolved_table.object_uuid.empty() ? "false" : "true"}}));
      return result;
    } else {
      const std::string table_resolution_code =
          resolved_table.messages.diagnostics.empty()
              ? std::string{}
              : resolved_table.messages.diagnostics.front().code;
      const bool table_absent =
          !resolved_table.resolved && resolved_table.object_uuid.empty() &&
          table_resolution_code ==
              "PARSER_SERVER_IPC.NAME_NOT_FOUND_OR_NOT_VISIBLE";
      if (!table_absent) {
        result = unresolved(deletion->target, "delete",
                            resolved_table.messages);
        return result;
      }
      if (!relation_projection_view_delete_v2.recognized()) {
        if (relation_projection_view_delete_v2.attempted) {
          return reject(
              "FIREBIRD.RELATION_VIEW.V2.DELETE_SHAPE_UNSUPPORTED",
              "After exact table resolution failed, the standalone Firebird view-delete route refused a non-exact DELETE FROM v WHERE out = int32_literal shape.");
        }
        result = unresolved(deletion->target, "delete",
                            resolved_table.messages);
        return result;
      }
      const auto resolved_view = ResolveNameSemanticPublicOnTransaction(
          relation_projection_view_delete_v2.view_name,
          relation_projection_view_delete_v2.view_name_quoted,
          "view", transaction);
      auto bound = BindFirebirdRelationProjectionViewDeleteV2(
          relation_projection_view_delete_v2, resolved_view);
      if (!bound.accepted) {
        result.accepted = false;
        result.messages = std::move(bound.messages);
        return result;
      }
      result.sblr_payload =
          EncodeFirebirdRelationProjectionViewDeleteV2Envelope(bound);
    }
  } else if (drop_table) {
    const auto target_uuid =
        resolve_table(drop_table->target, drop_table->quoted, "drop");
    if (!target_uuid) return result;
    result.sblr_payload = DropTableEnvelope(*target_uuid);
  } else if (global_count_projection.recognized()) {
    const auto resolved = ResolveRelationDescriptorPublicOnTransaction(
        global_count_projection.source_relation,
        global_count_projection.source_relation_quoted,
        transaction);
    auto bound =
        BindFirebirdGlobalCountProjection(global_count_projection, resolved);
    if (!bound.accepted) {
      result.accepted = false;
      result.messages = std::move(bound.messages);
      return result;
    }
    result.sblr_payload =
        EncodeFirebirdGlobalCountProjectionEnvelope(bound);
  } else if (global_avg_projection.recognized()) {
    const auto resolved = ResolveRelationDescriptorPublicOnTransaction(
        global_avg_projection.source_relation,
        global_avg_projection.source_relation_quoted,
        transaction);
    auto bound =
        BindFirebirdGlobalAvgProjection(global_avg_projection, resolved);
    if (!bound.accepted) {
      result.accepted = false;
      result.messages = std::move(bound.messages);
      return result;
    }
    result.global_avg_projection_result_kind = bound.result_kind;
    result.sblr_payload = EncodeFirebirdGlobalAvgProjectionEnvelope(bound);
  } else if (relation_projection_view_select.recognized() ||
             global_aggregate_view_select.recognized()) {
    const auto resolved_view = ResolveNameSemanticPublicOnTransaction(
        relation_projection_view_select.recognized()
            ? relation_projection_view_select.view_name
            : global_aggregate_view_select.view_name,
        relation_projection_view_select.recognized()
            ? relation_projection_view_select.view_name_quoted
            : global_aggregate_view_select.view_name_quoted,
        "view",
        transaction);
    if (resolved_view.resolved && !resolved_view.object_uuid.empty()) {
      if (resolved_view.resolution_detail.starts_with(
              std::string(kFirebirdRelationProjectionViewSelectPacketV1) +
              "|")) {
        auto bound = BindFirebirdRelationProjectionViewSelect(
            relation_projection_view_select, resolved_view);
        if (!bound.accepted) {
          result.accepted = false;
          result.messages = std::move(bound.messages);
          return result;
        }
        result.global_aggregate_view_select_route = {};
        result.relation_projection_view_outputs = bound.outputs;
        result.relation_projection_view_uuid = bound.view_uuid;
        result.relation_projection_view_descriptor_uuid =
            bound.view_descriptor_uuid;
        result.relation_projection_view_descriptor_generation =
            bound.view_descriptor_generation;
        result.sblr_payload =
            EncodeFirebirdRelationProjectionViewSelectEnvelope(bound);
      } else if (resolved_view.resolution_detail.starts_with("gavs1|")) {
        auto bound = BindFirebirdGlobalAggregateViewSelect(
            global_aggregate_view_select, resolved_view);
        if (!bound.accepted) {
          result.accepted = false;
          result.messages = std::move(bound.messages);
          return result;
        }
        result.relation_projection_view_select_route = {};
        result.global_aggregate_view_result_kind = bound.result_kind;
        result.global_aggregate_view_result_alias = bound.result_alias;
        result.sblr_payload =
            EncodeFirebirdGlobalAggregateViewSelectEnvelope(bound);
      } else {
        result.accepted = false;
        result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
            "FIREBIRD.VIEW.SEMANTIC_FAMILY_UNSUPPORTED", "ERROR",
            "The exact engine-owned view descriptor is not one of the standalone Firebird semantic families admitted by this SELECT shape.",
            "sbp_firebird.execution_session"));
        return result;
      }
    } else {
      const std::string code =
          resolved_view.messages.diagnostics.empty()
              ? std::string{}
              : resolved_view.messages.diagnostics.front().code;
      if (code != "PARSER_SERVER_IPC.NAME_NOT_FOUND_OR_NOT_VISIBLE") {
        result.accepted = false;
        result.messages = resolved_view.messages;
        if (result.messages.diagnostics.empty()) {
          result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
              "FIREBIRD.AGGREGATE_VIEW.SEMANTIC_RESOLUTION_FAILED", "ERROR",
              "The exact semantic view resolution failed.",
              "sbp_firebird.execution_session"));
        }
        return result;
      }
      if (!select) {
        return reject(
            "FIREBIRD.SBLR.LOWERING_UNAVAILABLE",
            "The Firebird-owned SELECT * route has no ordinary table fallback.");
      }
      const auto source_uuid =
          resolve_table(select->source, select->quoted, "select");
      if (!source_uuid) return result;
      ApplyFirebirdOrdinaryRelationSelectFallback(
          &result, SelectEnvelope(*select, *source_uuid));
    }
  } else if (scalar_projection.recognized()) {
    result.sblr_payload =
        EncodeFirebirdScalarProjectionEnvelope(scalar_projection);
  } else if (select) {
    const auto source_uuid =
        resolve_table(select->source, select->quoted, "select");
    if (!source_uuid) return result;
    result.sblr_payload = SelectEnvelope(*select, *source_uuid);
  }

  if (result.sblr_payload.empty()) {
    return reject(
        "FIREBIRD.SBLR.LOWERING_UNAVAILABLE",
        "The Firebird-owned prepared lowerer did not produce executable SBLR.");
  }
  return result;
}

FirebirdPipelineResult
FirebirdExecutionSession::BindAndLowerCatalogProjection(
    const FirebirdCatalogProjectionRoute& route,
    const ipc::ParserTransactionSelector& transaction) {
  auto reject = [&](std::string code, std::string message) {
    auto result = Rejected(std::move(code), std::move(message));
    result.statement_family = route.is_select() ? "dml" : "ddl";
    result.operation_family = "firebird.catalog_relation_projection";
    return result;
  };
  if (!route.recognized()) {
    return reject(
        "FIREBIRD.CATALOG_PROJECTION.ROUTE_REQUIRED",
        "The bounded Firebird catalog projection route is not recognized.");
  }
  if (!HasExecutionRoute()) {
    return reject(
        "FIREBIRD.SERVER.UNAVAILABLE",
        "Firebird catalog projection binding requires the neutral parser-server route.");
  }
  if (!session_.authenticated) {
    return reject(
        "FIREBIRD.AUTH.REQUIRED",
        "Firebird catalog projection binding requires an authenticated server session.");
  }
  if (!transaction.present()) {
    return reject(
        "FIREBIRD.TRANSACTION.SELECTOR_REQUIRED",
        "Firebird catalog projection binding requires an exact engine-issued transaction selector.");
  }

  FirebirdPipelineResult result;
  result.accepted = true;
  result.statement_family = route.is_select() ? "dml" : "ddl";
  result.operation_family = "firebird.catalog_relation_projection";
  result.catalog_projection_route = route;

  if (route.kind ==
      FirebirdCatalogProjectionRouteKind::kCreateTypeNameFunction) {
    const auto schema = ResolveNamePublicOnTransaction(
        "users.public", true, "schema", transaction);
    if (!schema.resolved || schema.object_uuid.empty()) {
      result.accepted = false;
      result.messages = schema.messages;
      result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
          "FIREBIRD.CATALOG_PROJECTION.DEFAULT_SCHEMA_REQUIRED", "ERROR",
          "The bounded Firebird function requires the engine bootstrap users.public schema on the exact transaction.",
          "sbp_firebird.execution_session"));
      return result;
    }
    result.sblr_payload = EncodeFirebirdCatalogProjectionFunctionEnvelope(
        route, schema.object_uuid);
  } else if (route.kind ==
                 FirebirdCatalogProjectionRouteKind::kCreateFieldsInfoTypeView ||
             route.kind == FirebirdCatalogProjectionRouteKind::
                               kCreateFieldsInfoCharsetView) {
    const auto schema = ResolveNamePublicOnTransaction(
        "users.public", true, "schema", transaction);
    if (!schema.resolved || schema.object_uuid.empty()) {
      result.accepted = false;
      result.messages = schema.messages;
      result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
          "FIREBIRD.CATALOG_PROJECTION.DEFAULT_SCHEMA_REQUIRED", "ERROR",
          "The bounded Firebird view requires the engine bootstrap users.public schema on the exact transaction.",
          "sbp_firebird.execution_session"));
      return result;
    }
    std::string function_uuid;
    if (route.kind ==
        FirebirdCatalogProjectionRouteKind::kCreateFieldsInfoTypeView) {
      const auto function = ResolveNamePublicOnTransaction(
          route.function_name, false, "function", transaction);
      if (!function.resolved || function.object_uuid.empty()) {
        result.accepted = false;
        result.messages = function.messages;
        result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
            "FIREBIRD.CATALOG_PROJECTION.TYPE_FUNCTION_REQUIRED", "ERROR",
            "The bounded Firebird type view requires its engine-owned function UUID on the exact transaction.",
            "sbp_firebird.execution_session"));
        return result;
      }
      function_uuid = function.object_uuid;
    }
    result.sblr_payload = EncodeFirebirdCatalogProjectionViewEnvelope(
        route, schema.object_uuid, function_uuid);
  } else if (route.kind ==
             FirebirdCatalogProjectionRouteKind::kSelectFieldsInfoUnbound) {
    const auto view = ResolveNameSemanticPublicOnTransaction(
        route.view_name, false, "view", transaction);
    if (!view.resolved || view.object_uuid.empty() ||
        view.object_class != "view") {
      result.accepted = false;
      result.messages = view.messages;
      result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
          "FIREBIRD.CATALOG_PROJECTION.VIEW_REQUIRED", "ERROR",
          "The bounded Firebird SELECT requires its engine-owned persisted view on the exact transaction.",
          "sbp_firebird.execution_session"));
      return result;
    }
    const auto bound = BindFirebirdCatalogProjectionViewVariant(
        route, view.resolution_detail);
    if (!bound) {
      result.accepted = false;
      result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
          "FIREBIRD.CATALOG_PROJECTION.VIEW_DESCRIPTOR_INVALID", "ERROR",
          "The exact engine-owned view descriptor is missing, malformed, or incompatible with the Firebird SELECT presentation.",
          "sbp_firebird.execution_session"));
      return result;
    }
    result.catalog_projection_route = *bound;

    const auto relation = ResolveRelationDescriptorPublicOnTransaction(
        bound->source_relation_name, false, transaction);
    const auto& descriptor = relation.relation_descriptor;
    if (!relation.resolved || relation.object_uuid.empty() ||
        relation.object_class != "table" || !descriptor.present ||
        descriptor.relation_uuid != relation.object_uuid ||
        descriptor.descriptor_uuid.empty() ||
        descriptor.descriptor_generation == 0 ||
        descriptor.validated_resource_epoch == 0) {
      result.accepted = false;
      result.messages = relation.messages;
      result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
          "FIREBIRD.CATALOG_PROJECTION.RELATION_DESCRIPTOR_REQUIRED", "ERROR",
          "The bounded Firebird SELECT requires the complete engine-owned relation descriptor and resource epoch on the exact transaction.",
          "sbp_firebird.execution_session"));
      return result;
    }
    result.catalog_relation_uuid = relation.object_uuid;
    result.catalog_relation_descriptor_uuid = descriptor.descriptor_uuid;
    result.catalog_relation_descriptor_generation =
        descriptor.descriptor_generation;
    result.sblr_payload = EncodeFirebirdCatalogProjectionSelectEnvelope(
        *bound, view.object_uuid, relation.object_uuid,
        descriptor.descriptor_uuid, descriptor.descriptor_generation);
  } else {
    return reject(
        "FIREBIRD.CATALOG_PROJECTION.ROUTE_UNBOUND",
        "The bounded Firebird catalog projection route is not valid for exact binding.");
  }

  if (result.sblr_payload.empty()) {
    return reject(
        "FIREBIRD.CATALOG_PROJECTION.LOWERING_UNAVAILABLE",
        "The bounded Firebird catalog projection did not produce executable SBLR.");
  }
  return result;
}

FirebirdPipelineResult FirebirdExecutionSession::PrepareCatalogProjection(
    const FirebirdCatalogProjectionRoute& route,
    const ipc::ParserTransactionSelector& transaction) {
  auto result = BindAndLowerCatalogProjection(route, transaction);
  if (!result.accepted) return result;

  const auto prepared = PrepareSblrRouted(result.sblr_payload, transaction);
  result.server_prepare = prepared;
  result.accepted = prepared.accepted;
  result.messages = prepared.messages;
  if (!prepared.accepted) return result;
  if (prepared.prepared_statement_uuid.empty()) {
    result.accepted = false;
    result.server_prepare.accepted = false;
    result.server_prepare.outcome_unknown = true;
    result.server_prepare.caller_cleanup_required = true;
    result.server_prepare.detail = "accepted_prepare_identity_missing";
    result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
        "FIREBIRD.DSQL.PREPARED_UUID_MISSING", "ERROR",
        "The neutral prepare route accepted the Firebird catalog projection without publishing an opaque prepared statement identity.",
        "sbp_firebird.execution_session"));
    return result;
  }
  result.prepared_statement_uuid = prepared.prepared_statement_uuid;
  return result;
}

FirebirdPipelineResult FirebirdExecutionSession::RunCatalogProjection(
    const FirebirdCatalogProjectionRoute& route,
    const ipc::ParserTransactionSelector& transaction,
    bool submit) {
  auto result = BindAndLowerCatalogProjection(route, transaction);
  if (!result.accepted || !submit) return result;

  // Catalog projection rows are deliberately returned in the initial neutral
  // packet. This prevents parser-local metadata from becoming cursor state.
  const auto executed = ExecuteSblrRouted(result.sblr_payload, transaction,
                                          false);
  result.accepted = executed.accepted;
  result.server_execution = executed;
  ProjectNonAuthoritativeRowResult(executed, &result);
  result.messages = executed.messages;
  if (executed.accepted && result.catalog_projection_route.is_select() &&
      !executed.cursor_uuid.empty()) {
    result.accepted = false;
    result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
        "FIREBIRD.CATALOG_PROJECTION.UNEXPECTED_CURSOR", "ERROR",
        "The bounded catalog projection unexpectedly returned a server cursor instead of one complete neutral row packet.",
        "sbp_firebird.execution_session"));
  }
  return result;
}

FirebirdPipelineResult FirebirdExecutionSession::PrepareStatement(
    std::string_view firebird_sql,
    const ipc::ParserTransactionSelector& transaction,
    std::string_view database_default_charset,
    std::string_view attachment_charset) {
  auto result = BindAndLowerForPrepare(firebird_sql, transaction,
                                       database_default_charset,
                                       attachment_charset);
  if (!result.accepted) return result;

  const auto prepared = PrepareSblrRouted(result.sblr_payload, transaction);
  result.server_prepare = prepared;
  result.accepted = prepared.accepted;
  result.messages = prepared.messages;
  if (!prepared.accepted) return result;
  if (prepared.prepared_statement_uuid.empty()) {
    result.accepted = false;
    result.server_prepare.accepted = false;
    result.server_prepare.outcome_unknown = true;
    result.server_prepare.caller_cleanup_required = true;
    result.server_prepare.detail = "accepted_prepare_identity_missing";
    result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
        "FIREBIRD.DSQL.PREPARED_UUID_MISSING", "ERROR",
        "The neutral prepare route accepted Firebird SBLR without publishing an opaque prepared statement identity.",
        "sbp_firebird.execution_session"));
    return result;
  }
  result.prepared_statement_uuid = prepared.prepared_statement_uuid;
  return result;
}

FirebirdPipelineResult FirebirdExecutionSession::ExecuteRecreateDropPrelude(
    std::string_view presented_name,
    bool quoted,
    const ipc::ParserTransactionSelector& transaction) {
  FirebirdPipelineResult result;
  if (!transaction.present()) {
    return Rejected(
        "FIREBIRD.TRANSACTION.SELECTOR_REQUIRED",
        "A prepared RECREATE prelude requires the exact engine-issued transaction selector.");
  }
  const auto existing = ResolveNamePublicOnTransaction(
      presented_name, quoted, "table", transaction);
  if (!existing.resolved || existing.object_uuid.empty()) {
    const std::string code = existing.messages.diagnostics.empty()
                                 ? std::string{}
                                 : existing.messages.diagnostics.front().code;
    if (!code.empty() &&
        code != "PARSER_SERVER_IPC.NAME_NOT_FOUND_OR_NOT_VISIBLE") {
      result.messages = existing.messages;
      return result;
    }
    // Name lookup deliberately collapses absent and not-visible.  In either
    // case do not attempt a DROP; the prepared CREATE remains authoritative
    // and will reject a hidden duplicate at engine admission.
    result.accepted = true;
    result.statement_family = "ddl";
    result.operation_family = "ddl.recreate_table.absent_prelude";
    return result;
  }
  return RunSblrEnvelope(DropTableEnvelope(existing.object_uuid), transaction,
                         false);
}

FirebirdPipelineResult FirebirdExecutionSession::RunStatement(
    std::string_view firebird_sql,
    const ipc::ParserTransactionSelector& transaction,
    bool submit,
    bool cursor_requested,
    std::uint64_t stream_row_count,
    bool autocommit_emulation,
    std::string_view database_default_charset,
    std::string_view attachment_charset) {
  (void)stream_row_count;
  (void)autocommit_emulation;
  const auto parsed = ParseStatement(firebird_sql);
  if (!parsed.ok) {
    auto result = Rejected("FIREBIRD.PARSE.REJECTED",
                           "The Firebird-owned parser rejected the statement.");
    result.statement_family = parsed.statement_family;
    result.operation_family = parsed.operation_family;
    return result;
  }

  const auto bounded_procedure =
      ParseFirebirdBoundedProcedureRoute(firebird_sql);
  const auto bounded_execute_block = ParseFirebirdBoundedExecuteBlockRoute(
      firebird_sql,
      {std::string(database_default_charset), std::string(attachment_charset)});
  const auto foreign_key_alter =
      ParseFirebirdForeignKeyAlterRoute(firebird_sql);
  const auto relation_projection_view_create =
      ParseFirebirdRelationProjectionViewCreateRoute(firebird_sql);
  const auto relation_projection_view_create_v2 =
      ParseFirebirdRelationProjectionViewCreateV2Route(firebird_sql);
  const auto relation_projection_view_delete_v2 =
      ParseFirebirdRelationProjectionViewDeleteV2Route(firebird_sql);
  const auto relation_projection_view_select =
      ParseFirebirdRelationProjectionViewSelectRoute(firebird_sql);
  const auto global_aggregate_view_create =
      ParseFirebirdGlobalAggregateViewCreateRoute(firebird_sql);
  const auto global_aggregate_view_select =
      ParseFirebirdGlobalAggregateViewSelectRoute(firebird_sql);
  if (foreign_key_alter.attempted && !foreign_key_alter.supported) {
    return Rejected(
        "FIREBIRD.FKEY.SHAPE_UNSUPPORTED",
        "The standalone Firebird lowerer supports only one immediate single-column FOREIGN KEY with NO ACTION semantics (" +
            foreign_key_alter.refusal_detail + ").");
  }
  if (relation_projection_view_create_v2.attempted &&
      !relation_projection_view_create_v2.recognized()) {
    return Rejected(
        "FIREBIRD.RELATION_VIEW.V2.CREATE_SHAPE_UNSUPPORTED",
        "The standalone Firebird lowerer rejected the bounded V2 direct-column view shape.");
  }
  if (!relation_projection_view_create_v2.attempted &&
      relation_projection_view_create.attempted &&
      !relation_projection_view_create.recognized()) {
    return Rejected(
        "FIREBIRD.RELATION_VIEW.SHAPE_UNSUPPORTED",
        "The standalone Firebird lowerer rejected the bounded relation-projection view shape.");
  }
  if (!relation_projection_view_create_v2.attempted &&
      !relation_projection_view_create.attempted &&
      global_aggregate_view_create.attempted &&
      !global_aggregate_view_create.recognized()) {
    return Rejected(
        "FIREBIRD.AGGREGATE_VIEW.SHAPE_UNSUPPORTED",
        "The standalone Firebird lowerer supports only the bounded AVG(int32_literal * int32_field) view definition.");
  }
  if (submit &&
      (bounded_procedure.recognized() || bounded_execute_block.recognized() ||
       foreign_key_alter.recognized() ||
       relation_projection_view_create_v2.recognized() ||
       relation_projection_view_create.recognized() ||
       relation_projection_view_delete_v2.recognized() ||
       relation_projection_view_select.recognized() ||
       global_aggregate_view_create.recognized() ||
       global_aggregate_view_select.recognized())) {
    auto result = BindAndLowerForPrepare(firebird_sql, transaction,
                                         database_default_charset,
                                         attachment_charset);
    if (!result.accepted) return result;
    const auto executed = ExecuteSblrRouted(
        result.sblr_payload,
        transaction,
        (relation_projection_view_select.recognized() ||
         global_aggregate_view_select.recognized()) && cursor_requested);
    result.accepted = executed.accepted;
    result.server_execution = executed;
    ProjectNonAuthoritativeRowResult(executed, &result);
    result.messages = executed.messages;
    if (executed.accepted &&
        relation_projection_view_delete_v2.recognized()) {
      const bool exact_selected_transaction_echo =
          executed.selected_transaction_present &&
          executed.selected_transaction.present() &&
          executed.selected_transaction.local_transaction_id ==
              transaction.local_transaction_id &&
          executed.selected_transaction.transaction_uuid ==
              transaction.transaction_uuid;
      if (!exact_selected_transaction_echo) {
        result.accepted = false;
        result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
            "FIREBIRD.RELATION_VIEW.V2.SELECTED_TRANSACTION_MISMATCH",
            "ERROR",
            "The engine relation-view DELETE did not echo the exact selected MGA transaction.",
            "sbp_firebird.execution_session"));
      }
    }
    if (executed.accepted && bounded_execute_block.recognized()) {
      const bool exact_zero_yield_result =
          PayloadLineValue(executed.row_packet, "result_kind") ==
              "sblr.procedural.block.rows.v1" &&
          executed.row_count == 0 && executed.cursor_uuid.empty();
      if (!exact_zero_yield_result) {
        result.accepted = false;
        result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
            "FIREBIRD.PSQL.EXECUTE_BLOCK_RESULT_INVALID", "ERROR",
            "The neutral engine procedural block did not return the exact zero-yield result contract.",
            "sbp_firebird.execution_session"));
      }
    }
    return result;
  }

  FirebirdPipelineResult result;
  result.accepted = true;
  result.statement_family = parsed.statement_family;
  result.operation_family = parsed.operation_family;
  result.procedural_block_route = bounded_execute_block;
  result.relation_projection_view_create_v2_route =
      relation_projection_view_create_v2;
  result.relation_projection_view_delete_v2_route =
      relation_projection_view_delete_v2;
  auto create_table = ParseCreateTableForExecution(firebird_sql);
  if (create_table) {
    ApplyFirebirdImplicitTextResourceNames(&*create_table,
                                           database_default_charset);
  }
  const auto insert = ParseInsertForExecution(firebird_sql);
  const auto update = ParseUpdateForExecution(firebird_sql);
  const auto deletion = ParseDeleteForExecution(firebird_sql);
  const auto drop_table = ParseDropTableForExecution(firebird_sql);
  const auto scalar_projection =
      ParseFirebirdScalarProjectionRoute(
          firebird_sql, {std::string(attachment_charset)});
  const auto select = ParseSimpleSelectForExecution(firebird_sql);
  bool executable_lowering_available = false;
  if (parsed.statement_family == "transaction") {
    result.sblr_payload = FirebirdTransactionEnvelope(firebird_sql);
    executable_lowering_available = !result.sblr_payload.empty();
  } else if (bounded_execute_block.recognized()) {
    result.sblr_payload =
        EncodeFirebirdBoundedExecuteBlockEnvelope(bounded_execute_block);
    executable_lowering_available = !result.sblr_payload.empty();
  } else if (foreign_key_alter.recognized()) {
    // Parse-only evidence intentionally carries no object identity.  Submit
    // takes the exact binder path above and replaces all four UUID operands.
    result.sblr_payload = ForeignKeyAlterEnvelope(
        foreign_key_alter, {}, {}, {}, {}, {}, 0, {}, 0);
    executable_lowering_available = !result.sblr_payload.empty();
  } else if (create_table) {
    // Exact schema identity is resolved below on the selected transaction
    // before submission.  A parse-only call receives family-owned lowering
    // evidence but cannot publish or cache an object identity.
    result.sblr_payload = CreateTableEnvelope(*create_table, {});
    executable_lowering_available = !result.sblr_payload.empty();
  } else if (insert) {
    // Parse-only probes may inspect the binary-safe Firebird literal packet.
    // The empty target UUID is evidence only; submit always resolves the table
    // on the exact selected engine transaction before execution.
    result.sblr_payload = InsertEnvelope(*insert, {});
    executable_lowering_available = !result.sblr_payload.empty();
  } else if (deletion) {
    // Parse-only probes may inspect the exact Firebird-owned predicate
    // encoding.  The empty target UUID is deliberate evidence only; submit
    // always resolves and replaces it on the selected engine transaction.
    result.sblr_payload = DeleteEnvelope(*deletion, {});
    executable_lowering_available = !result.sblr_payload.empty();
  } else if (scalar_projection.recognized()) {
    result.sblr_payload =
        EncodeFirebirdScalarProjectionEnvelope(scalar_projection);
    executable_lowering_available = !result.sblr_payload.empty();
  } else if (select) {
    result.sblr_payload = SelectEnvelope(*select, {});
    executable_lowering_available = !result.sblr_payload.empty();
  } else {
    // ParseStatement publishes a Firebird-owned evidence envelope for parser
    // conformance.  It is not executable SBLR and must never be submitted to
    // the engine when no exact Firebird binder/lowerer route matched.
    result.sblr_payload = parsed.sblr_envelope;
  }
  if (!submit) {
    if (result.sblr_payload.empty()) {
      return Rejected("FIREBIRD.SBLR.LOWERING_UNAVAILABLE",
                      "The Firebird-owned lowerer could not encode the statement.");
    }
    return result;
  }
  const bool exact_executable_route =
      executable_lowering_available || insert.has_value() || update.has_value() ||
      deletion.has_value() || drop_table.has_value() ||
      scalar_projection.recognized() || select.has_value();
  if (!exact_executable_route) {
    return Rejected("FIREBIRD.SBLR.LOWERING_UNAVAILABLE",
                    "The Firebird-owned lowerer did not produce executable SBLR for the statement.");
  }
  if (!HasExecutionRoute()) {
    return Rejected("FIREBIRD.SERVER.UNAVAILABLE",
                    "Firebird SBLR submission requires an execution route.");
  }
  if (!session_.authenticated) {
    return Rejected("FIREBIRD.AUTH.REQUIRED",
                    "Firebird SBLR submission requires an authenticated server session.");
  }
  if (!transaction.present()) {
    return Rejected(
        "FIREBIRD.TRANSACTION.SELECTOR_REQUIRED",
        "Firebird statement execution requires an exact engine-issued transaction selector.");
  }
  if (parsed.statement_family == "transaction") {
    const std::string upper =
        ToUpperAscii(NormalizeWhitespace(firebird_sql));
    if (StartsWithCommand(upper, "BEGIN TRANSACTION") ||
        StartsWithCommand(upper, "SET TRANSACTION")) {
      return Rejected(
          "FIREBIRD.TRANSACTION.BEGIN_ADDITIONAL_REQUIRED",
          "Firebird transaction begin must use BeginAdditional with a parsed policy envelope.");
    }
  }

  if (create_table) {
    if (!BindCreateTableTextResources(
            &*create_table,
            [&](std::string_view presented_name,
                bool quoted,
                std::string_view object_class) {
              return ResolveNamePublicOnTransaction(
                  presented_name, quoted, object_class, transaction);
            },
            &result.messages)) {
      result.accepted = false;
      return result;
    }
    if (create_table->recreate) {
      const auto resolved = ResolveNamePublicOnTransaction(
          create_table->name, create_table->quoted, "table", transaction);
      const std::string existing_uuid =
          resolved.resolved ? resolved.object_uuid : std::string{};
      if (!existing_uuid.empty()) {
        const auto dropped = ExecuteSblrRouted(
            DropTableEnvelope(existing_uuid), transaction, false);
        if (!dropped.accepted) {
          result.accepted = false;
          result.server_execution = dropped;
          ProjectNonAuthoritativeRowResult(dropped, &result);
          result.messages = dropped.messages;
          return result;
        }
      }
    }
    std::string default_schema_uuid;
    const auto resolved_schema = ResolveNamePublicOnTransaction(
        "users.public", true, "schema", transaction);
    if (resolved_schema.resolved && !resolved_schema.object_uuid.empty()) {
      default_schema_uuid = resolved_schema.object_uuid;
    } else {
      const auto created_schema = ExecuteSblrRouted(
          CreateDefaultSchemaEnvelope(), transaction, false);
      if (!created_schema.accepted) {
        result.accepted = false;
        result.server_execution = created_schema;
        ProjectNonAuthoritativeRowResult(created_schema, &result);
        result.messages = created_schema.messages;
        return result;
      }
      default_schema_uuid =
          ResultRowField(created_schema.row_packet, "object_uuid");
      if (default_schema_uuid.empty()) {
        result.accepted = false;
        result.server_execution = created_schema;
        ProjectNonAuthoritativeRowResult(created_schema, &result);
        result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
            "FIREBIRD.BIND.DEFAULT_SCHEMA_UUID_MISSING", "ERROR",
            "The engine did not publish the Firebird default schema identity.",
            "sbp_firebird.execution_session"));
        return result;
      }
    }
    result.sblr_payload = CreateTableEnvelope(*create_table, default_schema_uuid);
    executable_lowering_available = !result.sblr_payload.empty();
  }
  auto resolve_table = [&](std::string_view presented_name,
                           bool quoted,
                           ipc::MessageVectorSet* messages) -> std::string {
    const auto resolved = ResolveNamePublicOnTransaction(
        presented_name, quoted, "table", transaction);
    if (messages != nullptr) *messages = resolved.messages;
    return resolved.resolved ? resolved.object_uuid : std::string{};
  };
  if (insert) {
    ipc::MessageVectorSet resolution_messages;
    const std::string target_uuid =
        resolve_table(insert->target, insert->quoted, &resolution_messages);
    if (target_uuid.empty()) {
      result.accepted = false;
      result.messages = std::move(resolution_messages);
      if (result.messages.diagnostics.empty()) {
        result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
            "FIREBIRD.BIND.UNRESOLVED_RELATION", "ERROR",
            "The Firebird-owned binder could not resolve the target relation.",
            "sbp_firebird.execution_session",
            {{"object_class", "table"}, {"presented_name", insert->target}}));
      }
      return result;
    }
    result.sblr_payload = InsertEnvelope(*insert, target_uuid);
    executable_lowering_available = !result.sblr_payload.empty();
  } else if (update) {
    ipc::MessageVectorSet resolution_messages;
    const std::string target_uuid =
        resolve_table(update->target, update->quoted, &resolution_messages);
    if (target_uuid.empty()) {
      result.accepted = false;
      result.messages = std::move(resolution_messages);
      if (result.messages.diagnostics.empty()) {
        result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
            "FIREBIRD.BIND.UNRESOLVED_RELATION", "ERROR",
            "The Firebird-owned binder could not resolve the update relation.",
            "sbp_firebird.execution_session",
            {{"object_class", "table"}, {"presented_name", update->target}}));
      }
      return result;
    }
    result.sblr_payload = UpdateEnvelope(*update, target_uuid);
    executable_lowering_available = !result.sblr_payload.empty();
  } else if (deletion) {
    ipc::MessageVectorSet resolution_messages;
    const std::string target_uuid =
        resolve_table(deletion->target, deletion->quoted, &resolution_messages);
    if (target_uuid.empty()) {
      result.accepted = false;
      result.messages = std::move(resolution_messages);
      if (result.messages.diagnostics.empty()) {
        result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
            "FIREBIRD.BIND.UNRESOLVED_RELATION", "ERROR",
            "The Firebird-owned binder could not resolve the delete relation.",
            "sbp_firebird.execution_session",
            {{"object_class", "table"}, {"presented_name", deletion->target}}));
      }
      return result;
    }
    result.sblr_payload = DeleteEnvelope(*deletion, target_uuid);
    executable_lowering_available = !result.sblr_payload.empty();
  } else if (drop_table) {
    ipc::MessageVectorSet resolution_messages;
    const std::string target_uuid =
        resolve_table(drop_table->target, drop_table->quoted,
                      &resolution_messages);
    if (target_uuid.empty()) {
      result.accepted = false;
      result.messages = std::move(resolution_messages);
      if (result.messages.diagnostics.empty()) {
        result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
            "FIREBIRD.BIND.UNRESOLVED_RELATION", "ERROR",
            "The Firebird-owned binder could not resolve the table to drop.",
            "sbp_firebird.execution_session",
            {{"object_class", "table"}, {"presented_name", drop_table->target}}));
      }
      return result;
    }
    result.sblr_payload = DropTableEnvelope(target_uuid);
    executable_lowering_available = !result.sblr_payload.empty();
  } else if (scalar_projection.recognized()) {
    result.sblr_payload =
        EncodeFirebirdScalarProjectionEnvelope(scalar_projection);
    executable_lowering_available = !result.sblr_payload.empty();
  } else if (select) {
    ipc::MessageVectorSet resolution_messages;
    const std::string source_uuid =
        resolve_table(select->source, select->quoted, &resolution_messages);
    if (source_uuid.empty()) {
      result.accepted = false;
      result.messages = std::move(resolution_messages);
      if (result.messages.diagnostics.empty()) {
        result.messages.diagnostics.push_back(ipc::MakeDiagnostic(
            "FIREBIRD.BIND.UNRESOLVED_RELATION", "ERROR",
            "The Firebird-owned binder could not resolve the source relation.",
            "sbp_firebird.execution_session",
            {{"object_class", "table"}, {"presented_name", select->source}}));
      }
      return result;
    }
    result.sblr_payload = SelectEnvelope(*select, source_uuid);
    executable_lowering_available = !result.sblr_payload.empty();
  }
  if (!executable_lowering_available || result.sblr_payload.empty()) {
    return Rejected("FIREBIRD.SBLR.LOWERING_UNAVAILABLE",
                    "The Firebird-owned lowerer did not produce executable SBLR for the statement.");
  }
  const auto executed =
      ExecuteSblrRouted(result.sblr_payload, transaction, cursor_requested);
  result.accepted = executed.accepted;
  result.server_execution = executed;
  ProjectNonAuthoritativeRowResult(executed, &result);
  result.messages = executed.messages;
  return result;
}

FirebirdPipelineResult FirebirdExecutionSession::RunSblrEnvelope(
    std::string_view encoded_sblr_envelope,
    const ipc::ParserTransactionSelector& transaction,
    bool cursor_requested) {
  if (!HasExecutionRoute()) {
    return Rejected("FIREBIRD.SERVER.UNAVAILABLE",
                    "Firebird SBLR submission requires an execution route.");
  }
  if (!session_.authenticated) {
    return Rejected("FIREBIRD.AUTH.REQUIRED",
                    "Firebird SBLR submission requires an authenticated server session.");
  }
  const auto executed =
      ExecuteSblrRouted(encoded_sblr_envelope, transaction, cursor_requested);
  FirebirdPipelineResult result;
  result.accepted = executed.accepted;
  result.sblr_payload = std::string(encoded_sblr_envelope);
  result.server_execution = executed;
  ProjectNonAuthoritativeRowResult(executed, &result);
  result.messages = executed.messages;
  return result;
}

ipc::ServerFetchResult FirebirdExecutionSession::FetchCursorOnRoute(
    std::string_view cursor_uuid,
    std::uint64_t max_rows,
    std::uint64_t max_bytes,
    std::uint32_t fetch_flags) {
  return client_.FetchCursor(session_, cursor_uuid, max_rows, max_bytes,
                             fetch_flags);
}

ipc::ServerCloseCursorResult FirebirdExecutionSession::CloseCursorOnRoute(
    std::string_view cursor_uuid) {
  return client_.CloseCursor(session_, cursor_uuid);
}

ipc::ServerCloseCursorResult FirebirdExecutionSession::CancelCursorOnRoute(
    std::string_view cursor_uuid) {
  return client_.CancelCursor(session_, cursor_uuid);
}

bool FirebirdExecutionSession::DisconnectSession(
    ipc::MessageVectorSet* messages) {
  if (!session_.authenticated) return true;
  if (!client_.DisconnectSession(session_, messages)) return false;
  session_ = {};
  return true;
}

} // namespace scratchbird::parser::firebird
