// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_procedural_block.hpp"

#include "firebird_dialect.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::parser::firebird {
namespace {

struct TokenCursor {
  std::vector<Token> tokens;
  std::size_t offset{0};
};

std::string UnquoteIdentifier(std::string_view value) {
  if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
    return ToUpperAscii(value);
  }
  std::string unquoted;
  unquoted.reserve(value.size() - 2);
  for (std::size_t index = 1; index + 1 < value.size(); ++index) {
    if (value[index] == '"' && index + 2 < value.size() &&
        value[index + 1] == '"') {
      unquoted.push_back('"');
      ++index;
    } else {
      unquoted.push_back(value[index]);
    }
  }
  return unquoted;
}

std::optional<TokenCursor> MakeTokenCursor(std::string_view sql) {
  TokenCursor cursor;
  for (auto token : LexTokens(sql)) {
    if (token.kind == "line_comment") continue;
    if (token.kind == "block_comment") {
      if (!token.lexeme.ends_with("*/")) return std::nullopt;
      continue;
    }
    cursor.tokens.push_back(std::move(token));
  }
  return cursor;
}

bool ConsumeWord(TokenCursor* cursor, std::string_view word) {
  if (cursor == nullptr || cursor->offset >= cursor->tokens.size()) {
    return false;
  }
  const auto& token = cursor->tokens[cursor->offset];
  if (token.kind != "identifier_or_keyword" ||
      ToUpperAscii(token.lexeme) != word) {
    return false;
  }
  ++cursor->offset;
  return true;
}

bool ConsumePunctuation(TokenCursor* cursor, std::string_view punctuation) {
  if (cursor == nullptr || cursor->offset >= cursor->tokens.size()) {
    return false;
  }
  const auto& token = cursor->tokens[cursor->offset];
  if (token.kind != "punctuation" || token.lexeme != punctuation) {
    return false;
  }
  ++cursor->offset;
  return true;
}

std::optional<std::string> ConsumeIdentifier(TokenCursor* cursor) {
  if (cursor == nullptr || cursor->offset >= cursor->tokens.size()) {
    return std::nullopt;
  }
  const auto& token = cursor->tokens[cursor->offset];
  if (token.kind != "identifier_or_keyword" &&
      token.kind != "quoted_identifier") {
    return std::nullopt;
  }
  ++cursor->offset;
  return UnquoteIdentifier(token.lexeme);
}

std::optional<std::uint32_t> ConsumeBoundedUnsigned(TokenCursor* cursor,
                                                    std::uint32_t maximum) {
  if (cursor == nullptr || cursor->offset >= cursor->tokens.size()) {
    return std::nullopt;
  }
  const auto& token = cursor->tokens[cursor->offset];
  if (token.kind != "numeric_literal" || token.lexeme.empty() ||
      token.lexeme.find('.') != std::string::npos) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (const unsigned char ch : token.lexeme) {
    if (std::isdigit(ch) == 0) return std::nullopt;
    const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
    if (digit > maximum ||
        value > (static_cast<std::uint64_t>(maximum) - digit) / 10u) {
      return std::nullopt;
    }
    value = value * 10u + digit;
  }
  ++cursor->offset;
  return static_cast<std::uint32_t>(value);
}

bool ConsumeEnd(TokenCursor* cursor) {
  if (cursor == nullptr) return false;
  (void)ConsumePunctuation(cursor, ";");
  return cursor->offset == cursor->tokens.size();
}

std::string NormalizeCharset(std::string_view value) {
  const std::string normalized = ToUpperAscii(TrimAscii(value));
  return normalized.empty() ? "NONE" : normalized;
}

std::string CollationCharset(std::string_view collation) {
  const std::string normalized = ToUpperAscii(TrimAscii(collation));
  if (normalized == "WIN_PTBR") return "WIN1252";
  return {};
}

std::string EnvelopeHeader() {
  return "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
         "\"envelope_major\":3,"
         "\"sblr_version\":\"sblr_v3\","
         "\"operation_id\":\"transaction.execute_block\","
         "\"opcode\":\"SBLR_TRANSACTION_EXECUTE_BLOCK\","
         "\"operation_family\":\"sblr.transaction.control.v3\","
         "\"sblr_operation_family\":\"sblr.transaction.control.v3\","
         "\"result_shape\":\"engine.api.result.v1\","
         "\"diagnostic_shape\":\"engine.diagnostic.v1\","
         "\"parser_resolved_names_to_uuids\":true,"
         "\"requires_security_context\":true,"
         "\"requires_transaction_context\":true,"
         "\"requires_cluster_authority\":false,"
         "\"contains_sql_text\":false,"
         "\"identifier_profile_uuid\":\"firebird_v5\","
         "\"source_dialect\":\"firebird\",";
}

FirebirdBoundedExecuteBlockRoute ParseEmptyResult(
    TokenCursor cursor,
    const FirebirdExecuteBlockBindingContext& binding_context) {
  FirebirdBoundedExecuteBlockRoute route;
  if (!ConsumeWord(&cursor, "RETURNS") ||
      !ConsumePunctuation(&cursor, "(")) {
    return route;
  }
  const auto output_name = ConsumeIdentifier(&cursor);
  if (!output_name) return route;

  if (ConsumeWord(&cursor, "INTEGER")) {
    route.slot_type = FirebirdProceduralSlotType::kInt32;
    route.character_length = 0;
  } else if (ConsumeWord(&cursor, "VARCHAR")) {
    if (!ConsumePunctuation(&cursor, "(")) return {};
    const auto length = ConsumeBoundedUnsigned(&cursor, 32767);
    if (!length || *length == 0 ||
        !ConsumePunctuation(&cursor, ")")) {
      return {};
    }
    route.slot_type = FirebirdProceduralSlotType::kCharacter;
    route.character_length = *length;
  } else {
    return {};
  }

  std::string explicit_charset;
  std::string explicit_collation;
  if (ConsumeWord(&cursor, "CHARACTER")) {
    if (!ConsumeWord(&cursor, "SET")) return {};
    const auto charset = ConsumeIdentifier(&cursor);
    if (!charset) return {};
    explicit_charset = ToUpperAscii(*charset);
  }
  if (ConsumeWord(&cursor, "COLLATE")) {
    const auto collation = ConsumeIdentifier(&cursor);
    if (!collation) return {};
    explicit_collation = ToUpperAscii(*collation);
  }
  route.nullable = true;
  if (ConsumeWord(&cursor, "NOT")) {
    if (!ConsumeWord(&cursor, "NULL")) return {};
    route.nullable = false;
  }
  if (!ConsumePunctuation(&cursor, ")") ||
      !ConsumeWord(&cursor, "AS") || !ConsumeWord(&cursor, "BEGIN") ||
      !ConsumeWord(&cursor, "END") || !ConsumeEnd(&cursor)) {
    return {};
  }

  if (route.slot_type == FirebirdProceduralSlotType::kCharacter) {
    const std::string attachment_charset =
        NormalizeCharset(binding_context.attachment_charset);
    route.bound_character_set = explicit_charset.empty()
                                    ? attachment_charset
                                    : explicit_charset;
    route.bound_collation = explicit_collation;
    route.collation_bound_from_attachment =
        explicit_charset.empty() && !explicit_collation.empty();
    if (!explicit_collation.empty()) {
      const std::string required_charset =
          CollationCharset(explicit_collation);
      if (required_charset.empty() ||
          required_charset != route.bound_character_set) {
        // Deliberately do not fall back to database_default_charset here.
        return {};
      }
    }
  } else if (!explicit_charset.empty() || !explicit_collation.empty()) {
    return {};
  }

  route.kind = FirebirdBoundedExecuteBlockKind::kEmptyResult;
  route.slot_name = *output_name;
  return route;
}

FirebirdBoundedExecuteBlockRoute ParseTimestampAssignment(
    TokenCursor cursor) {
  FirebirdBoundedExecuteBlockRoute route;
  if (!ConsumeWord(&cursor, "AS") || !ConsumeWord(&cursor, "DECLARE")) {
    return route;
  }
  const auto local_name = ConsumeIdentifier(&cursor);
  if (!local_name || !ConsumeWord(&cursor, "VARCHAR") ||
      !ConsumePunctuation(&cursor, "(")) {
    return route;
  }
  const auto length = ConsumeBoundedUnsigned(&cursor, 32767);
  if (!length || *length == 0 || !ConsumePunctuation(&cursor, ")") ||
      !ConsumePunctuation(&cursor, ";") ||
      !ConsumeWord(&cursor, "BEGIN")) {
    return {};
  }
  const auto assigned_name = ConsumeIdentifier(&cursor);
  if (!assigned_name || ToUpperAscii(*assigned_name) !=
                            ToUpperAscii(*local_name) ||
      !ConsumePunctuation(&cursor, "=") ||
      !ConsumeWord(&cursor, "SUBSTRING") ||
      !ConsumePunctuation(&cursor, "(") ||
      !ConsumeWord(&cursor, "CURRENT_TIMESTAMP") ||
      !ConsumeWord(&cursor, "FROM")) {
    return {};
  }
  const auto start = ConsumeBoundedUnsigned(&cursor, 1);
  if (!start || *start != 1 || !ConsumePunctuation(&cursor, ")") ||
      !ConsumePunctuation(&cursor, ";") || !ConsumeWord(&cursor, "END") ||
      !ConsumeEnd(&cursor)) {
    return {};
  }
  route.kind =
      FirebirdBoundedExecuteBlockKind::kTimestampSubstringAssignment;
  route.slot_name = *local_name;
  route.slot_type = FirebirdProceduralSlotType::kCharacter;
  route.character_length = *length;
  route.nullable = true;
  return route;
}

}  // namespace

FirebirdBoundedExecuteBlockRoute ParseFirebirdBoundedExecuteBlockRoute(
    std::string_view firebird_sql,
    const FirebirdExecuteBlockBindingContext& binding_context) {
  const auto maybe_cursor = MakeTokenCursor(firebird_sql);
  if (!maybe_cursor) return {};
  TokenCursor cursor = *maybe_cursor;
  if (!ConsumeWord(&cursor, "EXECUTE") ||
      !ConsumeWord(&cursor, "BLOCK")) {
    return {};
  }
  if (cursor.offset < cursor.tokens.size() &&
      cursor.tokens[cursor.offset].kind == "identifier_or_keyword" &&
      ToUpperAscii(cursor.tokens[cursor.offset].lexeme) == "RETURNS") {
    return ParseEmptyResult(std::move(cursor), binding_context);
  }
  return ParseTimestampAssignment(std::move(cursor));
}

std::string EncodeFirebirdBoundedExecuteBlockEnvelope(
    const FirebirdBoundedExecuteBlockRoute& route) {
  if (!route.recognized()) return {};
  const bool has_local = route.has_local();
  const bool has_output = route.has_output();
  const std::uint32_t slot_count = has_local || has_output ? 1u : 0u;
  const std::uint32_t instruction_count = has_local ? 1u : 0u;

  std::ostringstream out;
  out << EnvelopeHeader()
      << "\"procedural_ir_contract\":\"sblr.procedural.block.v1\","
      << "\"procedural_block_kind\":\"anonymous\","
      << "\"procedural_input_count\":\"0\","
      << "\"procedural_local_count\":\"" << (has_local ? 1 : 0)
      << "\",\"procedural_output_count\":\"" << (has_output ? 1 : 0)
      << "\",\"procedural_slot_count\":\"" << slot_count
      << "\",\"procedural_instruction_count\":\"" << instruction_count
      << "\",\"procedural_yield_count\":\"0\"";
  if (slot_count != 0) {
    out << ",\"procedural_slot_0_id\":\""
        << (has_local ? "local.0" : "result.0") << "\""
        << ",\"procedural_slot_0_kind\":\""
        << (has_local ? "local" : "result") << "\""
        << ",\"procedural_slot_0_type\":\""
        << (route.slot_type == FirebirdProceduralSlotType::kCharacter
                ? "character"
                : "int32")
        << "\""
        << ",\"procedural_slot_0_nullable\":\""
        << (route.nullable ? "true" : "false") << "\"";
    if (route.slot_type == FirebirdProceduralSlotType::kCharacter) {
      if (route.character_length == 0 || route.character_length > 32767) {
        return {};
      }
      out << ",\"procedural_slot_0_character_length\":\""
          << route.character_length << "\"";
    }
  }
  if (has_local) {
    out << ",\"procedural_instruction_0_kind\":\"assign\""
        << ",\"procedural_instruction_0_target_slot\":\"local.0\""
        << ",\"procedural_instruction_0_expression_kind\":\"substring\""
        << ",\"procedural_instruction_0_source_kind\":\"context_variable\""
        << ",\"procedural_instruction_0_source_id\":\"ctx_current_timestamp\""
        << ",\"procedural_instruction_0_source_cast_type\":\"character\""
        << ",\"procedural_instruction_0_start_kind\":\"literal_int64\""
        << ",\"procedural_instruction_0_start_value\":\"1\""
        << ",\"procedural_instruction_0_length_kind\":\"to_end\"";
  }
  out << '}';
  return out.str();
}

std::string_view FirebirdBoundedExecuteBlockRouteName(
    FirebirdBoundedExecuteBlockKind kind) {
  switch (kind) {
    case FirebirdBoundedExecuteBlockKind::kEmptyResult:
      return "empty_zero_yield_result";
    case FirebirdBoundedExecuteBlockKind::kTimestampSubstringAssignment:
      return "timestamp_substring_assignment";
    case FirebirdBoundedExecuteBlockKind::kUnsupported:
      return "unsupported";
  }
  return "unsupported";
}

}  // namespace scratchbird::parser::firebird
