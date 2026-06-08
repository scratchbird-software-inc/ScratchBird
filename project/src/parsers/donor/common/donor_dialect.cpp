// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "donor_dialect.hpp"

#include <cctype>
#include <iomanip>
#include <sstream>
#include <utility>

namespace scratchbird::parser::donor {
namespace {

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool IsIdentifierChar(char ch) {
  const auto c = static_cast<unsigned char>(ch);
  return std::isalnum(c) != 0 || ch == '_' || ch == '$';
}

bool IsCommandBoundary(char ch) {
  return std::isspace(static_cast<unsigned char>(ch)) != 0 ||
         ch == ';' || ch == '(' || ch == '\'' || ch == '"' ||
         ch == '`' || ch == '[' || ch == '/' || ch == '-';
}

bool StartsWithCommand(std::string_view value, std::string_view prefix) {
  if (!StartsWith(value, prefix)) return false;
  return value.size() == prefix.size() || IsCommandBoundary(value[prefix.size()]);
}

bool Contains(std::string_view value, std::string_view needle) {
  return value.find(needle) != std::string_view::npos;
}

std::string_view TrimAsciiView(std::string_view text);

bool ContainsDelimitedFragment(std::string_view value,
                               std::string_view fragment) {
  std::size_t pos = value.find(fragment);
  while (pos != std::string_view::npos) {
    const std::size_t end = pos + fragment.size();
    if (fragment.empty() ||
        !IsIdentifierChar(fragment.back()) ||
        end >= value.size() ||
        !IsIdentifierChar(value[end])) {
      return true;
    }
    pos = value.find(fragment, pos + 1);
  }
  return false;
}

bool ContainsWord(std::string_view value, std::string_view word) {
  std::size_t pos = value.find(word);
  while (pos != std::string_view::npos) {
    const bool left_boundary = pos == 0 || !IsIdentifierChar(value[pos - 1]);
    const std::size_t end = pos + word.size();
    const bool right_boundary = end >= value.size() || !IsIdentifierChar(value[end]);
    if (left_boundary && right_boundary) return true;
    pos = value.find(word, pos + 1);
  }
  return false;
}

bool ContainsFunctionCall(std::string_view value,
                          std::string_view function_name) {
  std::size_t pos = value.find(function_name);
  while (pos != std::string_view::npos) {
    const bool left_boundary = pos == 0 || !IsIdentifierChar(value[pos - 1]);
    std::size_t end = pos + function_name.size();
    const bool right_boundary =
        end >= value.size() || !IsIdentifierChar(value[end]);
    while (end < value.size() &&
           std::isspace(static_cast<unsigned char>(value[end])) != 0) {
      ++end;
    }
    if (left_boundary && right_boundary && end < value.size() &&
        value[end] == '(') {
      return true;
    }
    pos = value.find(function_name, pos + 1);
  }
  return false;
}

std::size_t SkipAsciiWhitespace(std::string_view value, std::size_t pos) {
  while (pos < value.size() &&
         std::isspace(static_cast<unsigned char>(value[pos])) != 0) {
    ++pos;
  }
  return pos;
}

bool RelationNameAt(std::string_view value,
                    std::size_t pos,
                    std::string_view relation_name) {
  pos = SkipAsciiWhitespace(value, pos);
  std::size_t token_end = pos;
  while (token_end < value.size() && IsIdentifierChar(value[token_end])) {
    ++token_end;
  }
  if (token_end < value.size() && value[token_end] == '.') {
    const std::size_t relation_pos = token_end + 1;
    if (value.substr(relation_pos, relation_name.size()) != relation_name) {
      return false;
    }
    const std::size_t end = relation_pos + relation_name.size();
    const bool right_boundary =
        end >= value.size() || !IsIdentifierChar(value[end]);
    return right_boundary;
  }
  if (value.substr(pos, relation_name.size()) != relation_name) return false;
  const std::size_t end = pos + relation_name.size();
  return end >= value.size() || !IsIdentifierChar(value[end]);
}

bool ContainsRelationReference(std::string_view value,
                               std::string_view relation_name) {
  constexpr std::string_view kIntroducers[] = {"FROM", "JOIN", "TABLE"};
  for (const auto introducer : kIntroducers) {
    std::size_t pos = value.find(introducer);
    while (pos != std::string_view::npos) {
      const bool left_boundary = pos == 0 || !IsIdentifierChar(value[pos - 1]);
      const std::size_t end = pos + introducer.size();
      const bool right_boundary =
          end >= value.size() || !IsIdentifierChar(value[end]);
      if (left_boundary && right_boundary &&
          RelationNameAt(value, end, relation_name)) {
        return true;
      }
      pos = value.find(introducer, pos + 1);
    }
  }
  return false;
}

bool ConsumeCommand(std::string_view& value, std::string_view keyword) {
  value = TrimAsciiView(value);
  if (!StartsWithCommand(value, keyword)) return false;
  value = TrimAsciiView(value.substr(keyword.size()));
  return true;
}

bool MatchesLoadDataInfileSyntax(std::string_view value, bool local_infile) {
  if (!ConsumeCommand(value, "LOAD")) return false;
  if (!ConsumeCommand(value, "DATA")) return false;
  if (StartsWithCommand(value, "LOW_PRIORITY")) {
    ConsumeCommand(value, "LOW_PRIORITY");
  } else if (StartsWithCommand(value, "CONCURRENT")) {
    ConsumeCommand(value, "CONCURRENT");
  }
  if (local_infile && !ConsumeCommand(value, "LOCAL")) return false;
  return ConsumeCommand(value, "INFILE");
}

bool MatchesCreateTablePrefix(std::string_view value) {
  if (!ConsumeCommand(value, "CREATE")) return false;
  if (StartsWithCommand(value, "OR")) {
    if (!ConsumeCommand(value, "OR")) return false;
    if (!ConsumeCommand(value, "REPLACE")) return false;
  }
  if (StartsWithCommand(value, "TEMPORARY")) {
    if (!ConsumeCommand(value, "TEMPORARY")) return false;
  }
  if (!ConsumeCommand(value, "TABLE")) return false;
  if (StartsWithCommand(value, "IF")) {
    if (!ConsumeCommand(value, "IF")) return false;
    if (!ConsumeCommand(value, "NOT")) return false;
    if (!ConsumeCommand(value, "EXISTS")) return false;
  }
  return true;
}

bool ContainsCreateTableEngineClause(std::string_view value,
                                     std::string_view engine_name) {
  if (!MatchesCreateTablePrefix(value)) return false;
  std::size_t pos = value.find("ENGINE");
  while (pos != std::string_view::npos) {
    const bool left_boundary = pos == 0 || !IsIdentifierChar(value[pos - 1]);
    std::size_t cursor = pos + std::string_view("ENGINE").size();
    const bool right_boundary =
        cursor >= value.size() || !IsIdentifierChar(value[cursor]);
    cursor = SkipAsciiWhitespace(value, cursor);
    if (left_boundary && right_boundary) {
      if (cursor < value.size() && value[cursor] == '=') {
        cursor = SkipAsciiWhitespace(value, cursor + 1);
      }
      if (value.substr(cursor, engine_name.size()) == engine_name) {
        const std::size_t end = cursor + engine_name.size();
        if (end >= value.size() || !IsIdentifierChar(value[end])) {
          return true;
        }
      }
    }
    pos = value.find("ENGINE", pos + 1);
  }
  return false;
}

std::string MaskInactiveSqlText(std::string_view text) {
  std::string masked;
  masked.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    const char ch = text[i];
    const char next = i + 1 < text.size() ? text[i + 1] : '\0';
    if (ch == '-' && next == '-') {
      masked.append(2, ' ');
      i += 2;
      while (i < text.size() && text[i] != '\n') {
        masked.push_back(' ');
        ++i;
      }
      continue;
    }
    if (ch == '#') {
      masked.push_back(' ');
      ++i;
      while (i < text.size() && text[i] != '\n') {
        masked.push_back(' ');
        ++i;
      }
      continue;
    }
    if (ch == '/' && next == '*') {
      masked.append(2, ' ');
      i += 2;
      while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) {
        masked.push_back(' ');
        ++i;
      }
      if (i + 1 < text.size()) {
        masked.append(2, ' ');
        i += 2;
      } else {
        while (i < text.size()) {
          masked.push_back(' ');
          ++i;
        }
      }
      continue;
    }
    if (ch == '\'' || ch == '"' || ch == '`') {
      const char quote = ch;
      masked.push_back(quote);
      ++i;
      while (i < text.size()) {
        if (text[i] == quote && i + 1 < text.size() && text[i + 1] == quote) {
          masked.append(2, ' ');
          i += 2;
          continue;
        }
        if (quote == '\'' && text[i] == '\\' && i + 1 < text.size()) {
          masked.append(2, ' ');
          i += 2;
          continue;
        }
        if (text[i] == quote) {
          masked.push_back(quote);
          ++i;
          break;
        }
        masked.push_back(' ');
        ++i;
      }
      continue;
    }
    masked.push_back(ch);
    ++i;
  }
  return masked;
}

std::string_view TrimAsciiView(std::string_view text);
std::string BoolJson(bool value);

std::string_view ConsumeLeadingCommand(std::string_view value,
                                       std::string_view keyword) {
  value = TrimAsciiView(value);
  if (!StartsWithCommand(value, keyword)) return {};
  return TrimAsciiView(value.substr(keyword.size()));
}

bool IsCreateIndexRest(std::string_view rest) {
  rest = TrimAsciiView(rest);
  bool advanced = true;
  while (advanced) {
    advanced = false;
    for (const auto keyword : {"UNIQUE", "FULLTEXT", "SPATIAL", "ASC",
                               "ASCENDING", "DESC", "DESCENDING"}) {
      if (StartsWithCommand(rest, keyword)) {
        rest = TrimAsciiView(rest.substr(std::string_view(keyword).size()));
        advanced = true;
        break;
      }
    }
  }
  return StartsWithCommand(rest, "INDEX");
}

bool IsCreateIndexKeywordSequence(const std::vector<Token>& tokens) {
  std::vector<std::string> keywords;
  for (const auto& token : tokens) {
    if (token.kind == "line_comment" || token.kind == "block_comment") continue;
    if (token.kind == "identifier_or_keyword") {
      keywords.push_back(ToUpperAscii(token.lexeme));
    }
    if (keywords.size() >= 5) break;
  }
  if (keywords.empty() || keywords[0] != "CREATE") return false;
  for (std::size_t i = 1; i < keywords.size(); ++i) {
    if (keywords[i] == "INDEX") return true;
    if (keywords[i] == "UNIQUE" || keywords[i] == "FULLTEXT" ||
        keywords[i] == "SPATIAL" || keywords[i] == "ASC" ||
        keywords[i] == "ASCENDING" || keywords[i] == "DESC" ||
        keywords[i] == "DESCENDING") {
      continue;
    }
    return false;
  }
  return false;
}

bool HasIndexSemanticDefaultsProfile(std::string_view dialect_id) {
  return dialect_id == "firebird" || dialect_id == "mysql" ||
         dialect_id == "postgresql";
}

bool HasConstraintSemanticDefaultsProfile(std::string_view dialect_id) {
  return dialect_id == "firebird" || dialect_id == "mysql" ||
         dialect_id == "postgresql";
}

std::string_view IdentifierNameResolutionProfileUuid(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "019e13c0-1200-7000-8000-000000000302";
  if (dialect_id == "mysql") return "019e13c0-1200-7000-8000-000000000303";
  if (dialect_id == "postgresql") return "019e13c0-1200-7000-8000-000000000304";
  return "00000000-0000-0000-0000-000000000000";
}

std::string_view ScalarExpressionSemanticProfileUuid(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "019e13c0-1400-7000-8000-000000000302";
  if (dialect_id == "mysql") return "019e13c0-1400-7000-8000-000000000303";
  if (dialect_id == "postgresql") return "019e13c0-1400-7000-8000-000000000304";
  return "00000000-0000-0000-0000-000000000000";
}

std::string_view DmlMutationSemanticProfileUuid(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "019e13c0-1500-7000-8000-000000000302";
  if (dialect_id == "mysql") return "019e13c0-1500-7000-8000-000000000303";
  if (dialect_id == "postgresql") return "019e13c0-1500-7000-8000-000000000304";
  return "00000000-0000-0000-0000-000000000000";
}

std::string_view TransactionSessionSemanticProfileUuid(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "019e13c0-1600-7000-8000-000000000302";
  if (dialect_id == "mysql") return "019e13c0-1600-7000-8000-000000000303";
  if (dialect_id == "postgresql") return "019e13c0-1600-7000-8000-000000000304";
  return "00000000-0000-0000-0000-000000000000";
}

std::string_view TemporarySessionObjectSemanticProfileUuid(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "019e13c0-1700-7000-8000-000000000302";
  if (dialect_id == "mysql") return "019e13c0-1700-7000-8000-000000000303";
  if (dialect_id == "postgresql") return "019e13c0-1700-7000-8000-000000000304";
  return "00000000-0000-0000-0000-000000000000";
}

std::string_view DependencyBearingDdlSemanticProfileUuid(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "019e13c0-1800-7000-8000-000000000302";
  if (dialect_id == "mysql") return "019e13c0-1800-7000-8000-000000000303";
  if (dialect_id == "postgresql") return "019e13c0-1800-7000-8000-000000000304";
  return "00000000-0000-0000-0000-000000000000";
}

std::string_view DdlTransactionBehaviorSemanticProfileUuid(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "019e13c0-1900-7000-8000-000000000302";
  if (dialect_id == "mysql") return "019e13c0-1900-7000-8000-000000000303";
  if (dialect_id == "postgresql") return "019e13c0-1900-7000-8000-000000000304";
  return "00000000-0000-0000-0000-000000000000";
}

std::string_view ResourceTextSemanticProfileUuid(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "019e13c0-1a00-7000-8000-000000000302";
  if (dialect_id == "mysql") return "019e13c0-1a00-7000-8000-000000000303";
  if (dialect_id == "postgresql") return "019e13c0-1a00-7000-8000-000000000304";
  return "00000000-0000-0000-0000-000000000000";
}

std::string_view StatisticsOptimizerSemanticProfileUuid(
    std::string_view dialect_id) {
  if (dialect_id == "firebird") return "019e13c0-1b00-7000-8000-000000000302";
  if (dialect_id == "mysql") return "019e13c0-1b00-7000-8000-000000000303";
  if (dialect_id == "postgresql") return "019e13c0-1b00-7000-8000-000000000304";
  return "00000000-0000-0000-0000-000000000000";
}

std::string_view LocksIsolationSemanticProfileUuid(
    std::string_view dialect_id) {
  if (dialect_id == "firebird") return "019e13c0-1c00-7000-8000-000000000302";
  if (dialect_id == "mysql") return "019e13c0-1c00-7000-8000-000000000303";
  if (dialect_id == "postgresql") return "019e13c0-1c00-7000-8000-000000000304";
  return "00000000-0000-0000-0000-000000000000";
}

std::string_view SystemCatalogDefaultsSemanticProfileUuid(
    std::string_view dialect_id) {
  if (dialect_id == "firebird") return "019e13c0-1d00-7000-8000-000000000302";
  if (dialect_id == "mysql") return "019e13c0-1d00-7000-8000-000000000303";
  if (dialect_id == "postgresql") return "019e13c0-1d00-7000-8000-000000000304";
  return "00000000-0000-0000-0000-000000000000";
}

std::string_view SessionSettingsDiagnosticsSemanticProfileUuid(
    std::string_view dialect_id) {
  if (dialect_id == "firebird") return "019e13c0-1e00-7000-8000-000000000302";
  if (dialect_id == "mysql") return "019e13c0-1e00-7000-8000-000000000303";
  if (dialect_id == "postgresql") return "019e13c0-1e00-7000-8000-000000000304";
  return "00000000-0000-0000-0000-000000000000";
}

std::string_view IndexDdlSurface(std::string_view upper) {
  upper = TrimAsciiView(upper);
  if (StartsWithCommand(upper, "CREATE")) {
    return ContainsWord(upper, "UNIQUE") ? "create_unique_index"
                                         : "create_index";
  }
  if (StartsWithCommand(upper, "ALTER TABLE")) {
    return "alter_table_index";
  }
  if (StartsWithCommand(upper, "ALTER INDEX")) {
    return "alter_index";
  }
  return "index_ddl";
}

std::string_view DonorProfileUuid(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "019e13c0-0000-7000-8000-000000000302";
  if (dialect_id == "mysql") return "019e13c0-0000-7000-8000-000000000303";
  if (dialect_id == "postgresql") return "019e13c0-0000-7000-8000-000000000304";
  return "00000000-0000-0000-0000-000000000000";
}

std::string_view IndexSemanticProfileUuid(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "019e13c0-1000-7000-8000-000000000302";
  if (dialect_id == "mysql") return "019e13c0-1000-7000-8000-000000000303";
  if (dialect_id == "postgresql") return "019e13c0-1000-7000-8000-000000000304";
  return "00000000-0000-0000-0000-000000000000";
}

std::string_view ConstraintSemanticProfileUuid(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "019e13c0-1100-7000-8000-000000000302";
  if (dialect_id == "mysql") return "019e13c0-1100-7000-8000-000000000303";
  if (dialect_id == "postgresql") return "019e13c0-1100-7000-8000-000000000304";
  return "00000000-0000-0000-0000-000000000000";
}

std::string_view SequenceIdentitySemanticProfileUuid(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "019e13c0-1300-7000-8000-000000000302";
  if (dialect_id == "mysql") return "019e13c0-1300-7000-8000-000000000303";
  if (dialect_id == "postgresql") return "019e13c0-1300-7000-8000-000000000304";
  return "00000000-0000-0000-0000-000000000000";
}

std::string_view IndexProfileName(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "firebird.index_optimizer_translation_profile";
  if (dialect_id == "mysql") return "mysql.index_optimizer_translation_profile";
  if (dialect_id == "postgresql") return "postgresql.index_optimizer_translation_profile";
  return "unknown.index_optimizer_translation_profile";
}

std::string_view ConstraintProfileName(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "firebird.table_constraint_defaults_profile";
  if (dialect_id == "mysql") return "mysql.table_constraint_defaults_profile";
  if (dialect_id == "postgresql") return "postgresql.table_constraint_defaults_profile";
  return "unknown.table_constraint_defaults_profile";
}

std::string_view SequenceIdentityProfileName(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "firebird.sequence_generator_identity_profile";
  if (dialect_id == "mysql") return "mysql.auto_increment_identity_profile";
  if (dialect_id == "postgresql") return "postgresql.sequence_serial_identity_profile";
  return "unknown.sequence_identity_profile";
}

std::string_view IdentifierNameResolutionProfileName(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "firebird.identifier_name_resolution_profile";
  if (dialect_id == "mysql") return "mysql.identifier_name_resolution_profile";
  if (dialect_id == "postgresql") return "postgresql.identifier_name_resolution_profile";
  return "unknown.identifier_name_resolution_profile";
}

std::string_view ScalarExpressionProfileName(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "firebird.scalar_expression_semantics_profile";
  if (dialect_id == "mysql") return "mysql.scalar_expression_semantics_profile";
  if (dialect_id == "postgresql") return "postgresql.scalar_expression_semantics_profile";
  return "unknown.scalar_expression_semantics_profile";
}

std::string_view DmlMutationProfileName(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "firebird.dml_mutation_semantics_profile";
  if (dialect_id == "mysql") return "mysql.dml_mutation_semantics_profile";
  if (dialect_id == "postgresql") return "postgresql.dml_mutation_semantics_profile";
  return "unknown.dml_mutation_semantics_profile";
}

std::string_view TransactionSessionProfileName(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "firebird.transaction_session_semantics_profile";
  if (dialect_id == "mysql") return "mysql.transaction_session_semantics_profile";
  if (dialect_id == "postgresql") return "postgresql.transaction_session_semantics_profile";
  return "unknown.transaction_session_semantics_profile";
}

std::string_view TemporarySessionObjectProfileName(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "firebird.global_temporary_table_semantics_profile";
  if (dialect_id == "mysql") return "mysql.session_temporary_table_semantics_profile";
  if (dialect_id == "postgresql") return "postgresql.temporary_table_semantics_profile";
  return "unknown.temporary_session_object_semantics_profile";
}

std::string_view DependencyBearingDdlProfileName(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "firebird.dependency_bearing_ddl_semantics_profile";
  if (dialect_id == "mysql") return "mysql.dependency_bearing_ddl_semantics_profile";
  if (dialect_id == "postgresql") return "postgresql.dependency_bearing_ddl_semantics_profile";
  return "unknown.dependency_bearing_ddl_semantics_profile";
}

std::string_view DdlTransactionBehaviorProfileName(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "firebird.ddl_transaction_behavior_semantics_profile";
  if (dialect_id == "mysql") return "mysql.ddl_transaction_behavior_semantics_profile";
  if (dialect_id == "postgresql") return "postgresql.ddl_transaction_behavior_semantics_profile";
  return "unknown.ddl_transaction_behavior_semantics_profile";
}

std::string_view ResourceTextProfileName(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "firebird.resource_text_semantics_profile";
  if (dialect_id == "mysql") return "mysql.resource_text_semantics_profile";
  if (dialect_id == "postgresql") return "postgresql.resource_text_semantics_profile";
  return "unknown.resource_text_semantics_profile";
}

std::string_view StatisticsOptimizerProfileName(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird.statistics_optimizer_metadata_semantics_profile";
  }
  if (dialect_id == "mysql") {
    return "mysql.statistics_optimizer_metadata_semantics_profile";
  }
  if (dialect_id == "postgresql") {
    return "postgresql.statistics_optimizer_metadata_semantics_profile";
  }
  return "unknown.statistics_optimizer_metadata_semantics_profile";
}

std::string_view LocksIsolationProfileName(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird.locks_isolation_syntax_semantics_profile";
  }
  if (dialect_id == "mysql") {
    return "mysql.locks_isolation_syntax_semantics_profile";
  }
  if (dialect_id == "postgresql") {
    return "postgresql.locks_isolation_syntax_semantics_profile";
  }
  return "unknown.locks_isolation_syntax_semantics_profile";
}

std::string_view SystemCatalogDefaultsProfileName(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird.system_catalog_defaults_semantics_profile";
  }
  if (dialect_id == "mysql") {
    return "mysql.system_catalog_defaults_semantics_profile";
  }
  if (dialect_id == "postgresql") {
    return "postgresql.system_catalog_defaults_semantics_profile";
  }
  return "unknown.system_catalog_defaults_semantics_profile";
}

std::string_view SessionSettingsDiagnosticsProfileName(
    std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird.session_settings_diagnostics_semantics_profile";
  }
  if (dialect_id == "mysql") {
    return "mysql.session_settings_diagnostics_semantics_profile";
  }
  if (dialect_id == "postgresql") {
    return "postgresql.session_settings_diagnostics_semantics_profile";
  }
  return "unknown.session_settings_diagnostics_semantics_profile";
}

std::string_view DdlOperationKind(std::string_view upper) {
  upper = TrimAsciiView(upper);
  if (StartsWithCommand(upper, "CREATE OR REPLACE VIEW")) return "create_or_replace_view";
  if (StartsWithCommand(upper, "CREATE MATERIALIZED VIEW")) return "create_materialized_view";
  if (StartsWithCommand(upper, "CREATE VIEW")) return "create_view";
  if (StartsWithCommand(upper, "CREATE GLOBAL TEMPORARY TABLE") ||
      StartsWithCommand(upper, "CREATE GLOBAL TEMP TABLE") ||
      StartsWithCommand(upper, "CREATE LOCAL TEMPORARY TABLE") ||
      StartsWithCommand(upper, "CREATE LOCAL TEMP TABLE") ||
      StartsWithCommand(upper, "CREATE TEMPORARY TABLE") ||
      StartsWithCommand(upper, "CREATE TEMP TABLE") ||
      StartsWithCommand(upper, "CREATE TABLE")) {
    return "create_table";
  }
  if (IsCreateIndexKeywordSequence(LexTokens(upper))) return "create_index";
  if (StartsWithCommand(upper, "ALTER TABLE")) return "alter_table";
  if (StartsWithCommand(upper, "ALTER INDEX")) return "alter_index";
  if (StartsWithCommand(upper, "ALTER VIEW")) return "alter_view";
  if (StartsWithCommand(upper, "DROP TABLE")) return "drop_table";
  if (StartsWithCommand(upper, "DROP INDEX")) return "drop_index";
  if (StartsWithCommand(upper, "DROP VIEW")) return "drop_view";
  if (StartsWithCommand(upper, "TRUNCATE")) return "truncate";
  if (StartsWithCommand(upper, "COMMENT")) return "comment";
  if (StartsWithCommand(upper, "CREATE")) return "create";
  if (StartsWithCommand(upper, "ALTER")) return "alter";
  if (StartsWithCommand(upper, "DROP")) return "drop";
  if (StartsWithCommand(upper, "RECREATE")) return "recreate";
  return "ddl";
}

std::string_view DdlTransactionPolicy(std::string_view dialect_id,
                                      std::string_view upper) {
  if (dialect_id == "firebird") {
    return "firebird_transactional_ddl_engine_mga_descriptor_required";
  }
  if (dialect_id == "mysql") {
    return "mysql_implicit_commit_ddl_descriptor_required";
  }
  if (dialect_id == "postgresql") {
    if (ContainsWord(upper, "CONCURRENTLY")) {
      return "postgresql_concurrent_ddl_nontransactional_policy_descriptor";
    }
    return "postgresql_transactional_ddl_descriptor_required";
  }
  return "unknown_ddl_transaction_policy";
}

std::string_view DdlAutocommitBoundary(std::string_view dialect_id,
                                       std::string_view upper) {
  if (dialect_id == "firebird") {
    return "none_parser_does_not_commit_engine_transaction";
  }
  if (dialect_id == "mysql") {
    return "implicit_commit_before_and_after_ddl_engine_policy";
  }
  if (dialect_id == "postgresql") {
    if (ContainsWord(upper, "CONCURRENTLY")) {
      return "concurrent_index_requires_top_level_engine_policy";
    }
    return "none_parser_does_not_commit_engine_transaction";
  }
  return "unknown_ddl_autocommit_boundary";
}

std::string_view DdlMetadataVisibilityEpoch(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "transaction_local_until_engine_commit_then_catalog_epoch";
  }
  if (dialect_id == "mysql") {
    return "post_implicit_commit_catalog_epoch";
  }
  if (dialect_id == "postgresql") {
    return "transaction_local_until_engine_commit_then_catalog_epoch";
  }
  return "unknown_metadata_visibility_epoch";
}

std::string_view DdlRollbackPolicy(std::string_view dialect_id,
                                  std::string_view upper) {
  if (dialect_id == "firebird") {
    return "ddl_rollback_requires_engine_mga_transaction_rollback";
  }
  if (dialect_id == "mysql") {
    return "mysql_ddl_not_rolled_back_by_user_transaction_descriptor";
  }
  if (dialect_id == "postgresql") {
    if (ContainsWord(upper, "CONCURRENTLY")) {
      return "postgresql_concurrent_index_rollback_policy_engine_owned";
    }
    return "ddl_rollback_requires_engine_mga_transaction_rollback";
  }
  return "unknown_ddl_rollback_policy";
}

std::string_view DdlInvalidObjectStatePolicy(std::string_view dialect_id,
                                            std::string_view upper) {
  if (dialect_id == "firebird") {
    return "firebird_metadata_invalid_state_catalog_descriptor_engine_authority";
  }
  if (dialect_id == "mysql") {
    return "mysql_atomic_ddl_dictionary_state_engine_authority";
  }
  if (dialect_id == "postgresql") {
    if (DdlOperationKind(upper) == std::string_view("create_index")) {
      return "postgresql_index_invalid_state_descriptor_engine_authority";
    }
    return "postgresql_catalog_invalid_state_descriptor_engine_authority";
  }
  return "unknown_invalid_object_state_policy";
}

std::string_view DdlDiagnosticMapRef(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "firebird_ddl_transaction_behavior_diagnostic_map";
  if (dialect_id == "mysql") return "mysql_ddl_transaction_behavior_diagnostic_map";
  if (dialect_id == "postgresql") return "postgresql_ddl_transaction_behavior_diagnostic_map";
  return "unknown_ddl_transaction_behavior_diagnostic_map";
}

std::string_view ResourceTextDiagnosticMapRef(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "firebird_resource_text_semantics_diagnostic_map";
  if (dialect_id == "mysql") return "mysql_resource_text_semantics_diagnostic_map";
  if (dialect_id == "postgresql") return "postgresql_resource_text_semantics_diagnostic_map";
  return "unknown_resource_text_semantics_diagnostic_map";
}

std::string_view StatisticsOptimizerDiagnosticMapRef(
    std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_statistics_optimizer_semantics_diagnostic_map";
  }
  if (dialect_id == "mysql") {
    return "mysql_statistics_optimizer_semantics_diagnostic_map";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_statistics_optimizer_semantics_diagnostic_map";
  }
  return "unknown_statistics_optimizer_semantics_diagnostic_map";
}

std::string_view LocksIsolationDiagnosticMapRef(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_locks_isolation_semantics_diagnostic_map";
  }
  if (dialect_id == "mysql") {
    return "mysql_locks_isolation_semantics_diagnostic_map";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_locks_isolation_semantics_diagnostic_map";
  }
  return "unknown_locks_isolation_semantics_diagnostic_map";
}

std::string_view SystemCatalogNamespaceRootPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_rdb_mon_sec_information_schema_projected_from_engine_catalog_uuid_root";
  }
  if (dialect_id == "mysql") {
    return "mysql_information_schema_mysql_performance_schema_sys_projected_from_connected_catalog_root";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_pg_catalog_information_schema_projected_from_connected_database_catalog_root";
  }
  return "unknown_system_catalog_namespace_root_policy";
}

std::string_view SystemCatalogVisibilityProjectionPolicy(
    std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_system_relations_visible_through_engine_privilege_filtered_projection";
  }
  if (dialect_id == "mysql") {
    return "mysql_show_describe_information_schema_privilege_filtered_projection";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_pg_catalog_information_schema_privilege_filtered_projection";
  }
  return "unknown_system_catalog_visibility_projection_policy";
}

std::string_view GeneratedCatalogNamePolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_generated_rdb_names_projected_as_catalog_descriptors_not_parser_names";
  }
  if (dialect_id == "mysql") {
    return "mysql_generated_constraint_index_names_projected_from_engine_dictionary_descriptors";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_generated_pg_class_pg_constraint_names_projected_from_engine_catalog_descriptors";
  }
  return "unknown_generated_catalog_name_policy";
}

std::string_view DependencyProjectionPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_rdb_dependencies_projected_from_engine_dependency_graph";
  }
  if (dialect_id == "mysql") {
    return "mysql_information_schema_dependencies_projected_without_parser_dependency_authority";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_pg_depend_projection_from_engine_dependency_graph";
  }
  return "unknown_dependency_projection_policy";
}

std::string_view SourceVisibilityPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_rdb_source_columns_redacted_or_projected_by_engine_source_retention_policy";
  }
  if (dialect_id == "mysql") {
    return "mysql_routine_trigger_view_source_redacted_or_projected_by_engine_source_policy";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_pg_proc_pg_views_source_redacted_or_projected_by_engine_source_policy";
  }
  return "unknown_source_visibility_policy";
}

std::string_view HiddenSystemObjectPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_rdb_system_flag_hidden_objects_privilege_filtered_engine_projection";
  }
  if (dialect_id == "mysql") {
    return "mysql_data_dictionary_hidden_objects_privilege_filtered_engine_projection";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_toast_temp_internal_objects_privilege_filtered_engine_projection";
  }
  return "unknown_hidden_system_object_policy";
}

std::string_view GrantPrivilegeProjectionPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_rdb_user_privileges_sec_projection_engine_security_authority";
  }
  if (dialect_id == "mysql") {
    return "mysql_grants_information_schema_projection_engine_security_authority";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_acl_roles_information_schema_projection_engine_security_authority";
  }
  return "unknown_grant_privilege_projection_policy";
}

std::string_view SystemCatalogSblrOpcode(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "SBLR_DONOR_FIREBIRD_CATALOG_PROJECT";
  if (dialect_id == "mysql") return "SBLR_DONOR_MYSQL_CATALOG_PROJECT";
  if (dialect_id == "postgresql") {
    return "SBLR_DONOR_POSTGRESQL_CATALOG_PROJECT";
  }
  return "SBLR_DONOR_UNKNOWN_CATALOG_PROJECT";
}

std::string_view SystemCatalogDiagnosticMapRef(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_system_catalog_defaults_semantics_diagnostic_map";
  }
  if (dialect_id == "mysql") {
    return "mysql_system_catalog_defaults_semantics_diagnostic_map";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_system_catalog_defaults_semantics_diagnostic_map";
  }
  return "unknown_system_catalog_defaults_semantics_diagnostic_map";
}

std::string_view SessionSettingsDiagnosticsDiagnosticMapRef(
    std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_session_settings_diagnostics_semantics_diagnostic_map";
  }
  if (dialect_id == "mysql") {
    return "mysql_session_settings_diagnostics_semantics_diagnostic_map";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_session_settings_diagnostics_semantics_diagnostic_map";
  }
  return "unknown_session_settings_diagnostics_semantics_diagnostic_map";
}

std::string_view SessionSettingsDiagnosticsSurface(std::string_view dialect_id,
                                                   std::string_view upper) {
  if (dialect_id == "firebird") {
    if (StartsWithCommand(upper, "SET SQL DIALECT")) return "firebird_set_sql_dialect";
    if (StartsWithCommand(upper, "SET NAMES")) return "firebird_set_names";
    if (StartsWithCommand(upper, "SHOW SQL DIALECT")) return "firebird_show_sql_dialect";
    if (StartsWithCommand(upper, "SHOW WARNINGS")) return "firebird_show_warnings";
    return "firebird_session_settings_diagnostics";
  }
  if (dialect_id == "mysql") {
    if (StartsWithCommand(upper, "SET") && ContainsWord(upper, "SQL_MODE")) {
      return "mysql_set_sql_mode";
    }
    if (StartsWithCommand(upper, "SHOW WARNINGS")) return "mysql_show_warnings";
    if (StartsWithCommand(upper, "SHOW VARIABLES")) return "mysql_show_variables";
    if (StartsWithCommand(upper, "USE")) return "mysql_use_database";
    return "mysql_session_settings_diagnostics";
  }
  if (dialect_id == "postgresql") {
    if (StartsWithCommand(upper, "SET") && ContainsWord(upper, "SEARCH_PATH")) {
      return "postgresql_set_search_path";
    }
    if (StartsWithCommand(upper, "SET") &&
        ContainsWord(upper, "STATEMENT_TIMEOUT")) {
      return "postgresql_set_statement_timeout";
    }
    if (StartsWithCommand(upper, "RESET") &&
        ContainsWord(upper, "SEARCH_PATH")) {
      return "postgresql_reset_search_path";
    }
    if (StartsWithCommand(upper, "DISCARD ALL")) return "postgresql_discard_all";
    if (StartsWithCommand(upper, "SHOW") && ContainsWord(upper, "SEARCH_PATH")) {
      return "postgresql_show_search_path";
    }
    return "postgresql_session_settings_diagnostics";
  }
  return "unknown_session_settings_diagnostics";
}

std::string_view CompatibilityModePolicy(std::string_view dialect_id,
                                         std::string_view upper) {
  if (dialect_id == "firebird") {
    if (StartsWithCommand(upper, "SET SQL DIALECT")) {
      return "firebird_sql_dialect_session_descriptor_engine_applies";
    }
    if (StartsWithCommand(upper, "SET NAMES")) {
      return "firebird_character_set_session_descriptor_engine_applies";
    }
    return "firebird_isql_show_session_descriptor_projection";
  }
  if (dialect_id == "mysql") {
    if (StartsWithCommand(upper, "SET") && ContainsWord(upper, "SQL_MODE")) {
      return "mysql_sql_mode_compatibility_descriptor_engine_applies";
    }
    if (StartsWithCommand(upper, "USE")) {
      return "mysql_default_schema_compatibility_descriptor_engine_applies";
    }
    return "mysql_show_compatibility_projection_descriptor";
  }
  if (dialect_id == "postgresql") {
    if (StartsWithCommand(upper, "SET") && ContainsWord(upper, "SEARCH_PATH")) {
      return "postgresql_search_path_compatibility_descriptor_engine_applies";
    }
    if (StartsWithCommand(upper, "SET") &&
        ContainsWord(upper, "STATEMENT_TIMEOUT")) {
      return "postgresql_guc_timeout_descriptor_engine_applies";
    }
    if (StartsWithCommand(upper, "RESET") || StartsWithCommand(upper, "DISCARD")) {
      return "postgresql_guc_reset_compatibility_descriptor_engine_applies";
    }
    return "postgresql_show_guc_projection_descriptor";
  }
  return "unknown_compatibility_mode_policy";
}

std::string_view WarningPolicy(std::string_view dialect_id,
                               std::string_view upper) {
  if (dialect_id == "firebird") {
    return "firebird_status_vector_warning_diagnostics_engine_rendered";
  }
  if (dialect_id == "mysql") {
    if (StartsWithCommand(upper, "SHOW WARNINGS")) {
      return "mysql_show_warnings_diagnostic_rows_engine_rendered";
    }
    return "mysql_warning_count_diagnostic_area_engine_rendered";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_warning_diagnostics_engine_rendered";
  }
  return "unknown_warning_policy";
}

std::string_view NoticePolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_status_vector_notice_mapping_engine_rendered";
  }
  if (dialect_id == "mysql") {
    return "mysql_notes_warnings_errors_diagnostic_area_engine_rendered";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_notice_warning_guc_diagnostics_engine_rendered";
  }
  return "unknown_notice_policy";
}

std::string_view CurrentSchemaPolicy(std::string_view dialect_id,
                                     std::string_view upper) {
  if (dialect_id == "firebird") {
    return "firebird_current_schema_context_engine_session_descriptor";
  }
  if (dialect_id == "mysql") {
    if (StartsWithCommand(upper, "USE")) {
      return "mysql_use_database_sets_current_schema_engine_session_descriptor";
    }
    return "mysql_default_database_engine_session_descriptor";
  }
  if (dialect_id == "postgresql") {
    if (ContainsWord(upper, "SEARCH_PATH")) {
      return "postgresql_current_schema_resolved_from_engine_search_path_descriptor";
    }
    return "postgresql_current_schema_guc_projection_engine_session_descriptor";
  }
  return "unknown_current_schema_policy";
}

std::string_view SearchPathPolicy(std::string_view dialect_id,
                                  std::string_view upper) {
  if (dialect_id == "firebird") {
    return "firebird_no_search_path_single_attachment_schema_context";
  }
  if (dialect_id == "mysql") {
    return "mysql_no_search_path_current_database_descriptor_only";
  }
  if (dialect_id == "postgresql") {
    if (StartsWithCommand(upper, "RESET") && ContainsWord(upper, "SEARCH_PATH")) {
      return "postgresql_reset_search_path_to_engine_default_descriptor";
    }
    if (StartsWithCommand(upper, "SHOW") && ContainsWord(upper, "SEARCH_PATH")) {
      return "postgresql_show_search_path_engine_projection";
    }
    if (ContainsWord(upper, "SEARCH_PATH")) {
      return "postgresql_search_path_list_descriptor_uuid_resolved_engine_applies";
    }
    return "postgresql_search_path_unchanged_engine_session_descriptor";
  }
  return "unknown_search_path_policy";
}

std::string_view DateTimeFormatPolicy(std::string_view dialect_id,
                                      std::string_view upper) {
  if (dialect_id == "firebird") {
    return "firebird_date_time_format_stable_dialect_descriptor";
  }
  if (dialect_id == "mysql") {
    if (StartsWithCommand(upper, "SET") && ContainsWord(upper, "SQL_MODE")) {
      return "mysql_sql_mode_date_time_format_descriptor_engine_applies";
    }
    return "mysql_date_time_format_descriptor_engine_session_defaults";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_datestyle_intervalstyle_descriptor_engine_applies";
  }
  return "unknown_date_time_format_policy";
}

std::string_view TimeoutPolicy(std::string_view dialect_id,
                               std::string_view upper) {
  if (dialect_id == "firebird") {
    return "firebird_no_statement_timeout_session_setting_descriptor";
  }
  if (dialect_id == "mysql") {
    return "mysql_timeout_settings_not_first_tranche_descriptor_only";
  }
  if (dialect_id == "postgresql") {
    if (ContainsWord(upper, "STATEMENT_TIMEOUT")) {
      return "postgresql_statement_timeout_engine_session_descriptor";
    }
    return "postgresql_timeout_settings_unchanged_engine_session_descriptor";
  }
  return "unknown_timeout_policy";
}

std::string_view ResetPolicy(std::string_view dialect_id,
                             std::string_view upper) {
  if (dialect_id == "firebird") {
    return "firebird_session_setting_reset_not_requested";
  }
  if (dialect_id == "mysql") {
    return "mysql_session_setting_reset_not_requested";
  }
  if (dialect_id == "postgresql") {
    if (StartsWithCommand(upper, "DISCARD ALL")) {
      return "postgresql_discard_all_requests_engine_session_reset_descriptor";
    }
    if (StartsWithCommand(upper, "RESET")) {
      return "postgresql_reset_guc_requests_engine_session_reset_descriptor";
    }
    return "postgresql_session_setting_reset_not_requested";
  }
  return "unknown_reset_policy";
}

bool IsViewDependencyDdl(std::string_view upper) {
  return StartsWithCommand(upper, "CREATE VIEW") ||
         StartsWithCommand(upper, "CREATE OR REPLACE VIEW") ||
         StartsWithCommand(upper, "ALTER VIEW") ||
         StartsWithCommand(upper, "DROP VIEW");
}

bool IsMaterializedViewDependencyDdl(std::string_view upper) {
  return StartsWithCommand(upper, "CREATE MATERIALIZED VIEW") ||
         StartsWithCommand(upper, "CREATE OR REPLACE MATERIALIZED VIEW") ||
         StartsWithCommand(upper, "ALTER MATERIALIZED VIEW") ||
         StartsWithCommand(upper, "DROP MATERIALIZED VIEW") ||
         StartsWithCommand(upper, "REFRESH MATERIALIZED VIEW");
}

bool IsTriggerDependencyDdl(std::string_view upper) {
  return StartsWithCommand(upper, "CREATE TRIGGER") ||
         StartsWithCommand(upper, "CREATE OR ALTER TRIGGER") ||
         StartsWithCommand(upper, "CREATE OR REPLACE TRIGGER") ||
         StartsWithCommand(upper, "ALTER TRIGGER") ||
         StartsWithCommand(upper, "DROP TRIGGER") ||
         StartsWithCommand(upper, "RECREATE TRIGGER");
}

bool IsRoutineDependencyDdl(std::string_view upper) {
  return StartsWithCommand(upper, "CREATE PROCEDURE") ||
         StartsWithCommand(upper, "CREATE FUNCTION") ||
         StartsWithCommand(upper, "CREATE OR ALTER PROCEDURE") ||
         StartsWithCommand(upper, "CREATE OR ALTER FUNCTION") ||
         StartsWithCommand(upper, "CREATE OR REPLACE PROCEDURE") ||
         StartsWithCommand(upper, "CREATE OR REPLACE FUNCTION") ||
         StartsWithCommand(upper, "ALTER PROCEDURE") ||
         StartsWithCommand(upper, "ALTER FUNCTION") ||
         StartsWithCommand(upper, "DROP PROCEDURE") ||
         StartsWithCommand(upper, "DROP FUNCTION") ||
         StartsWithCommand(upper, "RECREATE PROCEDURE") ||
         StartsWithCommand(upper, "RECREATE FUNCTION");
}

bool IsPackageDependencyDdl(std::string_view upper) {
  return StartsWithCommand(upper, "CREATE PACKAGE") ||
         StartsWithCommand(upper, "CREATE PACKAGE BODY") ||
         StartsWithCommand(upper, "CREATE OR ALTER PACKAGE") ||
         StartsWithCommand(upper, "CREATE OR ALTER PACKAGE BODY") ||
         StartsWithCommand(upper, "ALTER PACKAGE") ||
         StartsWithCommand(upper, "DROP PACKAGE") ||
         StartsWithCommand(upper, "RECREATE PACKAGE") ||
         StartsWithCommand(upper, "RECREATE PACKAGE BODY");
}

bool IsRuleDependencyDdl(std::string_view upper) {
  return StartsWithCommand(upper, "CREATE RULE") ||
         StartsWithCommand(upper, "CREATE OR REPLACE RULE") ||
         StartsWithCommand(upper, "DROP RULE");
}

bool IsEventDependencyDdl(std::string_view upper) {
  return StartsWithCommand(upper, "CREATE EVENT") ||
         StartsWithCommand(upper, "ALTER EVENT") ||
         StartsWithCommand(upper, "DROP EVENT");
}

std::string_view DependencyDdlSurface(std::string_view dialect_id,
                                      std::string_view upper) {
  if (dialect_id == "firebird") {
    if (StartsWithCommand(upper, "CREATE VIEW")) return "firebird_create_view";
    if (StartsWithCommand(upper, "ALTER VIEW")) return "firebird_alter_view";
    if (StartsWithCommand(upper, "DROP VIEW")) return "firebird_drop_view";
    if (IsTriggerDependencyDdl(upper)) return "firebird_trigger_ddl";
    if (IsPackageDependencyDdl(upper)) return "firebird_package_or_package_body_ddl";
    if (IsRoutineDependencyDdl(upper)) return "firebird_procedure_function_ddl";
    return "firebird_dependency_bearing_ddl";
  }
  if (dialect_id == "mysql") {
    if (StartsWithCommand(upper, "CREATE OR REPLACE VIEW")) {
      return "mysql_create_or_replace_view";
    }
    if (StartsWithCommand(upper, "CREATE VIEW")) return "mysql_create_view";
    if (StartsWithCommand(upper, "ALTER VIEW")) return "mysql_alter_view";
    if (StartsWithCommand(upper, "DROP VIEW")) return "mysql_drop_view";
    if (IsTriggerDependencyDdl(upper)) return "mysql_trigger_ddl";
    if (IsEventDependencyDdl(upper)) return "mysql_event_scheduler_ddl";
    if (IsRoutineDependencyDdl(upper)) return "mysql_procedure_function_ddl";
    return "mysql_dependency_bearing_ddl";
  }
  if (dialect_id == "postgresql") {
    if (IsMaterializedViewDependencyDdl(upper)) return "postgresql_materialized_view_ddl";
    if (StartsWithCommand(upper, "CREATE OR REPLACE VIEW")) {
      return "postgresql_create_or_replace_view";
    }
    if (StartsWithCommand(upper, "CREATE VIEW")) return "postgresql_create_view";
    if (StartsWithCommand(upper, "ALTER VIEW")) return "postgresql_alter_view";
    if (StartsWithCommand(upper, "DROP VIEW")) return "postgresql_drop_view";
    if (IsRuleDependencyDdl(upper)) return "postgresql_rule_rewrite_ddl";
    if (IsTriggerDependencyDdl(upper)) return "postgresql_trigger_ddl";
    if (IsRoutineDependencyDdl(upper)) return "postgresql_procedure_function_ddl";
    return "postgresql_dependency_bearing_ddl";
  }
  return "unknown_dependency_bearing_ddl_surface";
}

std::string_view DependencyBindingPolicy(std::string_view dialect_id,
                                         std::string_view upper) {
  if (dialect_id == "firebird") {
    if (IsPackageDependencyDdl(upper)) {
      return "firebird_package_dependency_binding_uuid_catalog_descriptors";
    }
    if (IsTriggerDependencyDdl(upper)) {
      return "firebird_trigger_relation_event_dependency_binding_uuid_descriptors";
    }
    return "firebird_rdb_dependency_binding_uuid_catalog_descriptors";
  }
  if (dialect_id == "mysql") {
    if (IsEventDependencyDdl(upper)) {
      return "mysql_event_scheduler_dependency_binding_uuid_descriptors";
    }
    if (IsTriggerDependencyDdl(upper)) {
      return "mysql_trigger_table_dependency_binding_uuid_descriptors";
    }
    return "mysql_routine_view_dependency_binding_uuid_descriptors";
  }
  if (dialect_id == "postgresql") {
    if (IsRuleDependencyDdl(upper)) {
      return "postgresql_rewrite_rule_dependency_binding_uuid_descriptors";
    }
    if (IsMaterializedViewDependencyDdl(upper)) {
      return "postgresql_materialized_view_dependency_binding_uuid_descriptors";
    }
    return "postgresql_pg_depend_dependency_binding_uuid_descriptors";
  }
  return "unknown_dependency_binding_policy";
}

std::string_view DependencyInvalidationPolicy(std::string_view dialect_id,
                                              std::string_view upper) {
  if (dialect_id == "firebird") {
    return "firebird_metadata_dependency_invalidation_engine_catalog_authority";
  }
  if (dialect_id == "mysql") {
    return "mysql_metadata_dependency_invalidation_engine_catalog_authority";
  }
  if (dialect_id == "postgresql") {
    if (IsMaterializedViewDependencyDdl(upper)) {
      return "postgresql_materialized_view_refresh_dependency_invalidation_engine_catalog_authority";
    }
    return "postgresql_pg_depend_invalidation_engine_catalog_authority";
  }
  return "unknown_dependency_invalidation_policy";
}

std::string_view DependencyExecutionBodyPolicy(std::string_view dialect_id,
                                               std::string_view upper) {
  const bool body_surface = IsRoutineDependencyDdl(upper) ||
                            IsTriggerDependencyDdl(upper) ||
                            IsPackageDependencyDdl(upper) ||
                            IsEventDependencyDdl(upper) ||
                            IsRuleDependencyDdl(upper);
  if (dialect_id == "firebird") {
    return body_surface
               ? "firebird_psql_body_stored_as_catalog_reference_and_lowered_to_sblr_uuid"
               : "firebird_view_query_dependency_descriptor_no_parser_execution";
  }
  if (dialect_id == "mysql") {
    return body_surface
               ? "mysql_routine_trigger_event_body_routes_to_trusted_udr_lowering"
               : "mysql_view_definition_descriptor_no_parser_execution";
  }
  if (dialect_id == "postgresql") {
    return body_surface
               ? "postgresql_routine_trigger_rule_body_routes_to_trusted_udr_lowering"
               : "postgresql_view_query_dependency_descriptor_no_parser_execution";
  }
  return "unknown_dependency_execution_body_policy";
}

std::string_view DependencyCatalogStoragePolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_rdb_catalog_projection_stores_uuid_dependency_descriptors";
  }
  if (dialect_id == "mysql") {
    return "mysql_information_schema_catalog_projection_stores_uuid_dependency_descriptors";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_pg_catalog_projection_stores_uuid_dependency_descriptors";
  }
  return "unknown_dependency_catalog_storage_policy";
}

bool IsCreateTemporaryTableStatement(std::string_view dialect_id,
                                     std::string_view upper) {
  if (dialect_id == "firebird") {
    return StartsWithCommand(upper, "CREATE GLOBAL TEMPORARY TABLE");
  }
  if (dialect_id == "mysql") {
    return StartsWithCommand(upper, "CREATE TEMPORARY TABLE");
  }
  if (dialect_id == "postgresql") {
    return StartsWithCommand(upper, "CREATE TEMP TABLE") ||
           StartsWithCommand(upper, "CREATE TEMPORARY TABLE") ||
           StartsWithCommand(upper, "CREATE LOCAL TEMP TABLE") ||
           StartsWithCommand(upper, "CREATE LOCAL TEMPORARY TABLE") ||
           StartsWithCommand(upper, "CREATE GLOBAL TEMP TABLE") ||
           StartsWithCommand(upper, "CREATE GLOBAL TEMPORARY TABLE");
  }
  return false;
}

bool IsDropTemporaryTableStatement(std::string_view dialect_id,
                                   std::string_view upper) {
  if (dialect_id == "mysql") {
    return StartsWithCommand(upper, "DROP TEMPORARY TABLE");
  }
  if (dialect_id == "postgresql") {
    return StartsWithCommand(upper, "DROP TABLE") &&
           (Contains(upper, "PG_TEMP.") || Contains(upper, "TEMP"));
  }
  if (dialect_id == "firebird") {
    return StartsWithCommand(upper, "DROP TABLE") &&
           (Contains(upper, "TEMP") || Contains(upper, "GTT"));
  }
  return false;
}

bool IsAlterTemporaryTableStatement(std::string_view dialect_id,
                                    std::string_view upper) {
  if (!(StartsWithCommand(upper, "ALTER TABLE") ||
        StartsWithCommand(upper, "ALTER GLOBAL TEMPORARY TABLE") ||
        StartsWithCommand(upper, "ALTER LOCAL TEMPORARY TABLE") ||
        StartsWithCommand(upper, "ALTER TEMPORARY TABLE") ||
        StartsWithCommand(upper, "ALTER TEMP TABLE"))) {
    return false;
  }
  if (dialect_id == "firebird" || dialect_id == "postgresql" ||
      dialect_id == "mysql") {
    return Contains(upper, "TEMP") || Contains(upper, "GTT") ||
           Contains(upper, "PG_TEMP.");
  }
  return false;
}

std::string_view TemporaryObjectSurface(std::string_view dialect_id,
                                        std::string_view upper) {
  const bool delete_rows = Contains(upper, "ON COMMIT DELETE ROWS");
  const bool preserve_rows = Contains(upper, "ON COMMIT PRESERVE ROWS");
  const bool drop_rows = Contains(upper, "ON COMMIT DROP");
  if (dialect_id == "firebird") {
    if (IsCreateTemporaryTableStatement(dialect_id, upper)) {
      if (delete_rows) {
        return "firebird_create_global_temporary_table_on_commit_delete_rows";
      }
      if (preserve_rows) {
        return "firebird_create_global_temporary_table_on_commit_preserve_rows";
      }
      return "firebird_create_global_temporary_table_default_preserve_rows";
    }
    if (IsAlterTemporaryTableStatement(dialect_id, upper)) {
      return "firebird_alter_table_catalog_temp_resolution";
    }
    if (IsDropTemporaryTableStatement(dialect_id, upper)) {
      return "firebird_drop_table_catalog_temp_resolution";
    }
    return "firebird_temporary_session_object";
  }
  if (dialect_id == "mysql") {
    if (StartsWithCommand(upper, "CREATE TEMPORARY TABLE")) {
      return "mysql_create_temporary_table_name_shadowing";
    }
    if (StartsWithCommand(upper, "DROP TEMPORARY TABLE")) {
      return "mysql_drop_temporary_table_session_object";
    }
    if (IsDropTemporaryTableStatement(dialect_id, upper)) {
      return "mysql_drop_table_catalog_temp_resolution";
    }
    return "mysql_temporary_session_object";
  }
  if (dialect_id == "postgresql") {
    if (IsCreateTemporaryTableStatement(dialect_id, upper)) {
      if (drop_rows) return "postgresql_create_temp_table_on_commit_drop";
      if (delete_rows) return "postgresql_create_temp_table_on_commit_delete_rows";
      if (preserve_rows) return "postgresql_create_temp_table_on_commit_preserve_rows";
      if (StartsWithCommand(upper, "CREATE LOCAL TEMP") ||
          StartsWithCommand(upper, "CREATE LOCAL TEMPORARY")) {
        return "postgresql_create_local_temp_table_default_preserve_rows";
      }
      if (StartsWithCommand(upper, "CREATE GLOBAL TEMP") ||
          StartsWithCommand(upper, "CREATE GLOBAL TEMPORARY")) {
        return "postgresql_create_global_temp_table_compatibility_keyword";
      }
      return "postgresql_create_temp_table_default_preserve_rows";
    }
    if (IsDropTemporaryTableStatement(dialect_id, upper)) {
      return "postgresql_drop_table_pg_temp_resolution";
    }
    if (IsAlterTemporaryTableStatement(dialect_id, upper)) {
      return "postgresql_alter_table_pg_temp_resolution";
    }
    return "postgresql_temporary_session_object";
  }
  return "unknown_temporary_session_object_surface";
}

std::string_view TemporaryObjectKindPolicy(std::string_view dialect_id,
                                           std::string_view upper) {
  if (dialect_id == "firebird") {
    return "firebird_global_temporary_table_metadata_persistent_rows_session_or_transaction_scoped";
  }
  if (dialect_id == "mysql") {
    return "mysql_session_temporary_table_private_name_shadowing_regular_table";
  }
  if (dialect_id == "postgresql") {
    if (StartsWithCommand(upper, "CREATE GLOBAL TEMP") ||
        StartsWithCommand(upper, "CREATE GLOBAL TEMPORARY")) {
      return "postgresql_global_temp_keyword_accepted_as_local_temp_semantics";
    }
    if (StartsWithCommand(upper, "CREATE LOCAL TEMP") ||
        StartsWithCommand(upper, "CREATE LOCAL TEMPORARY")) {
      return "postgresql_local_temp_schema_session_private";
    }
    return "postgresql_temp_schema_session_private_table_object";
  }
  return "unknown_temporary_object_kind_policy";
}

std::string_view OnCommitPolicy(std::string_view dialect_id,
                                std::string_view upper) {
  if (Contains(upper, "ON COMMIT DELETE ROWS")) {
    if (dialect_id == "firebird") {
      return "firebird_on_commit_delete_rows_engine_transaction_end_cleanup";
    }
    if (dialect_id == "postgresql") {
      return "postgresql_on_commit_delete_rows_engine_transaction_end_cleanup";
    }
  }
  if (Contains(upper, "ON COMMIT PRESERVE ROWS")) {
    if (dialect_id == "firebird") {
      return "firebird_on_commit_preserve_rows_engine_session_lifetime";
    }
    if (dialect_id == "postgresql") {
      return "postgresql_on_commit_preserve_rows_engine_session_lifetime";
    }
  }
  if (Contains(upper, "ON COMMIT DROP")) {
    if (dialect_id == "postgresql") {
      return "postgresql_on_commit_drop_engine_transaction_end_catalog_cleanup";
    }
  }
  if (dialect_id == "firebird") {
    return "firebird_default_on_commit_preserve_rows_descriptor";
  }
  if (dialect_id == "mysql") {
    return "mysql_no_on_commit_clause_table_lifetime_session_end";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_default_on_commit_preserve_rows_descriptor";
  }
  return "unknown_on_commit_policy";
}

std::string_view OnCommitDeleteRowsPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_delete_rows_supported_engine_mga_transaction_boundary";
  }
  if (dialect_id == "mysql") return "mysql_delete_rows_not_supported";
  if (dialect_id == "postgresql") {
    return "postgresql_delete_rows_supported_engine_mga_transaction_boundary";
  }
  return "unknown_delete_rows_policy";
}

std::string_view OnCommitPreserveRowsPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_preserve_rows_supported_engine_session_lifetime";
  }
  if (dialect_id == "mysql") return "mysql_preserve_rows_is_session_lifetime_default";
  if (dialect_id == "postgresql") {
    return "postgresql_preserve_rows_supported_engine_session_lifetime";
  }
  return "unknown_preserve_rows_policy";
}

std::string_view OnCommitDropPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "firebird_on_commit_drop_not_supported";
  if (dialect_id == "mysql") return "mysql_on_commit_drop_not_supported";
  if (dialect_id == "postgresql") {
    return "postgresql_on_commit_drop_supported_engine_session_catalog_cleanup";
  }
  return "unknown_on_commit_drop_policy";
}

std::string_view SessionVisibilityPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_gtt_data_is_attachment_private_metadata_global_catalog_visible";
  }
  if (dialect_id == "mysql") {
    return "mysql_temporary_table_session_private_name_shadowing_visible_to_connection";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_pg_temp_schema_session_private_search_path_visible";
  }
  return "unknown_session_visibility_policy";
}

std::string_view CatalogVisibilityPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_persistent_catalog_descriptor_marks_global_temporary_table";
  }
  if (dialect_id == "mysql") {
    return "mysql_temporary_table_not_persistent_information_schema_object";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_catalog_pg_class_pg_namespace_temp_schema_descriptor";
  }
  return "unknown_catalog_visibility_policy";
}

std::string_view TemporaryObjectLifetimePolicy(std::string_view dialect_id,
                                               std::string_view upper) {
  if (dialect_id == "firebird") {
    if (Contains(upper, "ON COMMIT DELETE ROWS")) {
      return "firebird_rows_cleared_at_engine_mga_transaction_end_metadata_survives";
    }
    return "firebird_rows_survive_until_engine_attachment_end_metadata_survives";
  }
  if (dialect_id == "mysql") {
    return "mysql_temp_table_dropped_at_engine_session_end_or_explicit_drop";
  }
  if (dialect_id == "postgresql") {
    if (Contains(upper, "ON COMMIT DROP")) {
      return "postgresql_temp_table_dropped_at_engine_mga_transaction_end";
    }
    if (Contains(upper, "ON COMMIT DELETE ROWS")) {
      return "postgresql_temp_rows_deleted_at_engine_mga_transaction_end";
    }
    return "postgresql_temp_table_lives_until_engine_session_end_or_explicit_drop";
  }
  return "unknown_temporary_object_lifetime_policy";
}

std::string_view SchemaRootSandboxPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_donor_schema_root_uuid_required_no_cross_root_temp_access";
  }
  if (dialect_id == "mysql") {
    return "mysql_connected_database_root_uuid_required_temp_shadowing_root_local";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_connected_database_schema_root_uuid_required_pg_temp_root_local";
  }
  return "unknown_schema_root_sandbox_policy";
}

std::string_view DmlMutationSurface(std::string_view dialect_id,
                                    std::string_view upper) {
  if (dialect_id == "firebird") {
    if (StartsWithCommand(upper, "UPDATE OR INSERT")) {
      const bool matching = ContainsWord(upper, "MATCHING");
      if (matching && ContainsWord(upper, "RETURNING")) {
        return "firebird_update_or_insert_matching_returning";
      }
      if (matching) return "firebird_update_or_insert_matching";
      return ContainsWord(upper, "RETURNING")
                 ? "firebird_update_or_insert_primary_key_returning"
                 : "firebird_update_or_insert_primary_key";
    }
    if (StartsWithCommand(upper, "MERGE")) {
      return ContainsWord(upper, "RETURNING") ? "firebird_merge_returning"
                                              : "firebird_merge";
    }
    if (Contains(upper, " WHERE CURRENT OF ")) {
      if (StartsWithCommand(upper, "UPDATE")) return "firebird_update_current_of";
      if (StartsWithCommand(upper, "DELETE")) return "firebird_delete_current_of";
    }
    if (StartsWithCommand(upper, "INSERT")) {
      return ContainsWord(upper, "RETURNING") ? "firebird_insert_returning"
                                              : "firebird_insert";
    }
    if (StartsWithCommand(upper, "UPDATE")) {
      return ContainsWord(upper, "RETURNING") ? "firebird_update_returning"
                                              : "firebird_update";
    }
    if (StartsWithCommand(upper, "DELETE")) {
      return ContainsWord(upper, "RETURNING") ? "firebird_delete_returning"
                                              : "firebird_delete";
    }
    return "firebird_dml_mutation";
  }
  if (dialect_id == "mysql") {
    if (StartsWithCommand(upper, "REPLACE")) return "mysql_replace";
    if (StartsWithCommand(upper, "INSERT") &&
        Contains(upper, " ON DUPLICATE KEY UPDATE")) {
      return "mysql_insert_on_duplicate_key_update";
    }
    if (StartsWithCommand(upper, "INSERT")) return "mysql_insert";
    if (StartsWithCommand(upper, "UPDATE")) return "mysql_update";
    if (StartsWithCommand(upper, "DELETE")) return "mysql_delete";
    return "mysql_dml_mutation";
  }
  if (dialect_id == "postgresql") {
    if (StartsWithCommand(upper, "INSERT") && Contains(upper, " ON CONFLICT ")) {
      if (Contains(upper, " DO UPDATE")) return "postgresql_insert_on_conflict_do_update";
      if (Contains(upper, " DO NOTHING")) return "postgresql_insert_on_conflict_do_nothing";
      return "postgresql_insert_on_conflict";
    }
    if (StartsWithCommand(upper, "MERGE")) {
      return ContainsWord(upper, "RETURNING") ? "postgresql_merge_returning"
                                              : "postgresql_merge";
    }
    if (Contains(upper, " WHERE CURRENT OF ")) {
      if (StartsWithCommand(upper, "UPDATE")) return "postgresql_update_current_of";
      if (StartsWithCommand(upper, "DELETE")) return "postgresql_delete_current_of";
    }
    if (StartsWithCommand(upper, "INSERT")) {
      return ContainsWord(upper, "RETURNING") ? "postgresql_insert_returning"
                                              : "postgresql_insert";
    }
    if (StartsWithCommand(upper, "UPDATE")) {
      return ContainsWord(upper, "RETURNING") ? "postgresql_update_returning"
                                              : "postgresql_update";
    }
    if (StartsWithCommand(upper, "DELETE")) {
      return ContainsWord(upper, "RETURNING") ? "postgresql_delete_returning"
                                              : "postgresql_delete";
    }
    return "postgresql_dml_mutation";
  }
  return "unknown_dml_mutation_surface";
}

bool IsRollbackToSavepoint(std::string_view upper) {
  return StartsWithCommand(upper, "ROLLBACK TO SAVEPOINT") ||
         StartsWithCommand(upper, "ROLLBACK TO") ||
         StartsWithCommand(upper, "ROLLBACK WORK TO SAVEPOINT") ||
         StartsWithCommand(upper, "ROLLBACK WORK TO") ||
         StartsWithCommand(upper, "ROLLBACK TRANSACTION TO SAVEPOINT") ||
         StartsWithCommand(upper, "ROLLBACK TRANSACTION TO");
}

bool IsSetTransactionIsolation(std::string_view upper) {
  return (StartsWithCommand(upper, "SET") &&
          (Contains(upper, " TRANSACTION ISOLATION ") ||
           Contains(upper, " TRANSACTION_ISOLATION"))) ||
         Contains(upper, " ISOLATION LEVEL ");
}

std::string_view TransactionSessionSurface(std::string_view dialect_id,
                                           std::string_view upper) {
  if (dialect_id == "firebird") {
    if (StartsWithCommand(upper, "SET TRANSACTION")) {
      if (Contains(upper, "READ ONLY") &&
          (ContainsWord(upper, "WAIT") || Contains(upper, "NO WAIT")) &&
          (ContainsWord(upper, "SNAPSHOT") ||
           Contains(upper, "READ COMMITTED") ||
           Contains(upper, "TABLE STABILITY"))) {
        return "firebird_set_transaction_read_only_wait_isolation";
      }
      if (Contains(upper, "READ ONLY")) {
        return "firebird_set_transaction_read_only";
      }
      if (Contains(upper, "READ WRITE")) {
        return "firebird_set_transaction_read_write";
      }
      return "firebird_set_transaction";
    }
    if (StartsWithCommand(upper, "COMMIT")) {
      return ContainsWord(upper, "RETAIN") || ContainsWord(upper, "RETAINING")
                 ? "firebird_commit_retaining"
                 : "firebird_commit";
    }
    if (IsRollbackToSavepoint(upper)) return "firebird_rollback_to_savepoint";
    if (StartsWithCommand(upper, "ROLLBACK")) {
      return ContainsWord(upper, "RETAIN") || ContainsWord(upper, "RETAINING")
                 ? "firebird_rollback_retaining"
                 : "firebird_rollback";
    }
    if (StartsWithCommand(upper, "RELEASE SAVEPOINT")) {
      return "firebird_release_savepoint";
    }
    if (StartsWithCommand(upper, "SAVEPOINT")) return "firebird_savepoint";
    return "firebird_transaction_session";
  }
  if (dialect_id == "mysql") {
    if (StartsWithCommand(upper, "START TRANSACTION")) {
      if (Contains(upper, "READ ONLY")) return "mysql_start_transaction_read_only";
      if (Contains(upper, "READ WRITE")) return "mysql_start_transaction_read_write";
      return "mysql_start_transaction";
    }
    if (StartsWithCommand(upper, "BEGIN")) return "mysql_begin";
    if (StartsWithCommand(upper, "COMMIT")) return "mysql_commit";
    if (IsRollbackToSavepoint(upper)) return "mysql_rollback_to_savepoint";
    if (StartsWithCommand(upper, "ROLLBACK")) return "mysql_rollback";
    if (StartsWithCommand(upper, "RELEASE SAVEPOINT")) return "mysql_release_savepoint";
    if (StartsWithCommand(upper, "SAVEPOINT")) return "mysql_savepoint";
    if (StartsWithCommand(upper, "SET") && ContainsWord(upper, "AUTOCOMMIT")) {
      return "mysql_set_autocommit";
    }
    if (StartsWithCommand(upper, "SET") && Contains(upper, " TRANSACTION ISOLATION ")) {
      return "mysql_set_session_transaction_isolation";
    }
    if (StartsWithCommand(upper, "SET") && ContainsWord(upper, "SQL_MODE")) {
      return "mysql_set_sql_mode";
    }
    return "mysql_transaction_session";
  }
  if (dialect_id == "postgresql") {
    if (StartsWithCommand(upper, "BEGIN")) return "postgresql_begin";
    if (StartsWithCommand(upper, "START TRANSACTION")) return "postgresql_start_transaction";
    if (StartsWithCommand(upper, "COMMIT")) return "postgresql_commit";
    if (IsRollbackToSavepoint(upper)) return "postgresql_rollback_to_savepoint";
    if (StartsWithCommand(upper, "ROLLBACK")) return "postgresql_rollback";
    if (StartsWithCommand(upper, "RELEASE SAVEPOINT")) {
      return "postgresql_release_savepoint";
    }
    if (StartsWithCommand(upper, "SAVEPOINT")) return "postgresql_savepoint";
    if (StartsWithCommand(upper, "SET TRANSACTION")) {
      if (ContainsWord(upper, "SERIALIZABLE") &&
          Contains(upper, "READ ONLY") && ContainsWord(upper, "DEFERRABLE")) {
        return "postgresql_set_transaction_serializable_read_only_deferrable";
      }
      return "postgresql_set_transaction";
    }
    if (StartsWithCommand(upper, "SET LOCAL") &&
        ContainsWord(upper, "TRANSACTION_ISOLATION")) {
      return "postgresql_set_local_transaction_isolation";
    }
    if (StartsWithCommand(upper, "SET SESSION") &&
        ContainsWord(upper, "TRANSACTION_ISOLATION")) {
      return "postgresql_set_session_transaction_isolation";
    }
    if (StartsWithCommand(upper, "SET") && ContainsWord(upper, "STATEMENT_TIMEOUT")) {
      return "postgresql_set_statement_timeout";
    }
    if (StartsWithCommand(upper, "SET") && ContainsWord(upper, "SEARCH_PATH")) {
      return "postgresql_set_search_path";
    }
    return "postgresql_transaction_session";
  }
  return "unknown_transaction_session_surface";
}

std::string_view TransactionSessionFamilyLinkage(std::string_view dialect_id,
                                                 std::string_view upper) {
  if (dialect_id == "mysql") {
    if (StartsWithCommand(upper, "SET") &&
        (ContainsWord(upper, "AUTOCOMMIT") || ContainsWord(upper, "SQL_MODE"))) {
      return "session";
    }
    if (StartsWithCommand(upper, "SET") && Contains(upper, " TRANSACTION ISOLATION ")) {
      return "session";
    }
  }
  if (dialect_id == "postgresql") {
    if (StartsWithCommand(upper, "SET LOCAL") ||
        StartsWithCommand(upper, "SET SESSION") ||
        (StartsWithCommand(upper, "SET") &&
         (ContainsWord(upper, "STATEMENT_TIMEOUT") ||
          ContainsWord(upper, "SEARCH_PATH")))) {
      return "session";
    }
  }
  return "transaction";
}

std::string_view BeginAutocommitPolicy(std::string_view dialect_id,
                                       std::string_view upper) {
  if (dialect_id == "firebird") {
    if (StartsWithCommand(upper, "SET TRANSACTION")) {
      return "firebird_set_transaction_requests_engine_mga_transaction_handle";
    }
    return "firebird_existing_engine_transaction_required";
  }
  if (dialect_id == "mysql") {
    if (StartsWithCommand(upper, "SET") && ContainsWord(upper, "AUTOCOMMIT")) {
      return "mysql_autocommit_session_descriptor_engine_transaction_profile";
    }
    if (StartsWithCommand(upper, "START TRANSACTION") ||
        StartsWithCommand(upper, "BEGIN")) {
      return "mysql_explicit_begin_requests_engine_mga_transaction_handle_autocommit_suspended";
    }
    return "mysql_existing_engine_transaction_or_session_descriptor";
  }
  if (dialect_id == "postgresql") {
    if (StartsWithCommand(upper, "BEGIN") ||
        StartsWithCommand(upper, "START TRANSACTION")) {
      return "postgresql_explicit_begin_requests_engine_mga_transaction_handle";
    }
    return "postgresql_existing_engine_transaction_or_session_descriptor";
  }
  return "unknown_begin_autocommit_policy";
}

std::string_view IsolationReadOnlyDeferrablePolicy(std::string_view dialect_id,
                                                   std::string_view upper) {
  if (dialect_id == "firebird") {
    if (StartsWithCommand(upper, "SET TRANSACTION")) {
      return "firebird_set_transaction_access_isolation_wait_descriptor_engine_enforced";
    }
    return "firebird_transaction_control_does_not_change_isolation_descriptor";
  }
  if (dialect_id == "mysql") {
    return "mysql_transaction_access_mode_isolation_descriptor_engine_enforced";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_isolation_read_only_deferrable_descriptor_engine_enforced";
  }
  return "unknown_isolation_read_only_deferrable_policy";
}

std::string_view SessionVariableSqlModePolicy(std::string_view dialect_id,
                                              std::string_view upper) {
  if (dialect_id == "firebird") {
    return "firebird_no_sql_mode_session_variable_transaction_surface";
  }
  if (dialect_id == "mysql") {
    if (ContainsWord(upper, "SQL_MODE")) {
      return "mysql_sql_mode_session_descriptor_uuid_profile_engine_applies";
    }
    if (ContainsWord(upper, "AUTOCOMMIT")) {
      return "mysql_autocommit_session_descriptor_engine_applies";
    }
    return "mysql_session_transaction_descriptor_engine_applies";
  }
  if (dialect_id == "postgresql") {
    if (ContainsWord(upper, "SEARCH_PATH")) {
      return "postgresql_search_path_descriptor_uuid_profile_engine_applies";
    }
    if (ContainsWord(upper, "STATEMENT_TIMEOUT")) {
      return "postgresql_statement_timeout_descriptor_engine_applies";
    }
    if (ContainsWord(upper, "TRANSACTION_ISOLATION")) {
      return "postgresql_transaction_isolation_guc_descriptor_engine_applies";
    }
    return "postgresql_session_guc_descriptor_engine_applies";
  }
  return "unknown_session_variable_sql_mode_policy";
}

std::string_view UpsertMergeConflictPolicy(std::string_view dialect_id,
                                           std::string_view upper) {
  if (dialect_id == "firebird") {
    if (StartsWithCommand(upper, "UPDATE OR INSERT")) {
      return ContainsWord(upper, "MATCHING")
                 ? "firebird_update_or_insert_matching_descriptor_uuid_key_required"
                 : "firebird_update_or_insert_primary_key_match_descriptor_required";
    }
    if (StartsWithCommand(upper, "MERGE")) {
      return "firebird_merge_descriptor_source_target_uuid_binding_required";
    }
    return "firebird_no_upsert_merge_surface_observed";
  }
  if (dialect_id == "mysql") {
    if (StartsWithCommand(upper, "REPLACE")) {
      return "mysql_replace_delete_insert_semantics_descriptor_engine_executes";
    }
    if (StartsWithCommand(upper, "INSERT") &&
        Contains(upper, " ON DUPLICATE KEY UPDATE")) {
      return "mysql_on_duplicate_key_update_descriptor_unique_probe_engine_authority";
    }
    return "mysql_no_upsert_surface_observed";
  }
  if (dialect_id == "postgresql") {
    if (StartsWithCommand(upper, "INSERT") && Contains(upper, " ON CONFLICT ")) {
      if (Contains(upper, " DO UPDATE")) {
        return "postgresql_on_conflict_do_update_descriptor_inference_uuid_required";
      }
      if (Contains(upper, " DO NOTHING")) {
        return "postgresql_on_conflict_do_nothing_descriptor_inference_uuid_required";
      }
      return "postgresql_on_conflict_descriptor_inference_uuid_required";
    }
    if (StartsWithCommand(upper, "MERGE")) {
      return "postgresql_merge_descriptor_source_target_uuid_binding_required";
    }
    return "postgresql_no_conflict_or_merge_surface_observed";
  }
  return "unknown_upsert_merge_conflict_policy";
}

std::string_view ReturningOutputProjectionPolicy(std::string_view dialect_id,
                                                 std::string_view upper) {
  const bool returning = ContainsWord(upper, "RETURNING");
  if (dialect_id == "firebird") {
    return returning ? "firebird_returning_projection_descriptor_single_or_multirow_by_statement_kind"
                     : "firebird_no_returning_projection_observed";
  }
  if (dialect_id == "mysql") {
    return "mysql_no_native_dml_returning_projection_descriptor_rowcount_generated_keys_only";
  }
  if (dialect_id == "postgresql") {
    return returning ? "postgresql_returning_projection_descriptor_result_relation_uuid_bound"
                     : "postgresql_no_returning_projection_observed";
  }
  return "unknown_returning_output_projection_policy";
}

std::string_view CursorPositionedDmlPolicy(std::string_view dialect_id,
                                           std::string_view upper) {
  const bool current_of = Contains(upper, " WHERE CURRENT OF ");
  if (dialect_id == "firebird") {
    return current_of
               ? "firebird_where_current_of_cursor_descriptor_engine_cursor_authority"
               : "firebird_no_cursor_positioned_dml_observed";
  }
  if (dialect_id == "mysql") {
    return "mysql_no_native_where_current_of_descriptor";
  }
  if (dialect_id == "postgresql") {
    return current_of
               ? "postgresql_where_current_of_cursor_descriptor_engine_cursor_authority"
               : "postgresql_no_cursor_positioned_dml_observed";
  }
  return "unknown_cursor_positioned_dml_policy";
}

std::string_view AffectedRowCountPolicy(std::string_view dialect_id,
                                        std::string_view upper) {
  if (dialect_id == "firebird") {
    if (StartsWithCommand(upper, "UPDATE OR INSERT")) {
      return "firebird_row_count_update_or_insert_returning_descriptor_engine_reported";
    }
    return "firebird_row_count_descriptor_engine_reported_no_parser_finality";
  }
  if (dialect_id == "mysql") {
    if (StartsWithCommand(upper, "REPLACE")) {
      return "mysql_replace_row_count_delete_plus_insert_descriptor_engine_reported";
    }
    if (Contains(upper, " ON DUPLICATE KEY UPDATE")) {
      return "mysql_on_duplicate_key_affected_rows_client_found_rows_sensitive_descriptor";
    }
    return "mysql_affected_rows_descriptor_client_found_rows_profile_bound";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_command_tag_row_count_descriptor_engine_reported";
  }
  return "unknown_affected_row_count_policy";
}

std::string_view TriggerDefaultGeneratedColumnInteractionPolicy(
    std::string_view dialect_id,
    std::string_view upper) {
  const bool default_present = ContainsWord(upper, "DEFAULT");
  const bool generated_present = ContainsWord(upper, "GENERATED");
  if (dialect_id == "firebird") {
    if (StartsWithCommand(upper, "UPDATE OR INSERT")) {
      return "firebird_update_or_insert_defaults_triggers_returning_descriptor_engine_order";
    }
    if (default_present || generated_present || ContainsWord(upper, "RETURNING")) {
      return "firebird_defaults_triggers_generated_columns_descriptor_engine_order";
    }
    return "firebird_trigger_default_generated_column_descriptor_required";
  }
  if (dialect_id == "mysql") {
    if (StartsWithCommand(upper, "REPLACE")) {
      return "mysql_replace_defaults_generated_columns_triggers_descriptor_engine_order";
    }
    if (Contains(upper, " ON DUPLICATE KEY UPDATE")) {
      return "mysql_on_duplicate_defaults_generated_columns_triggers_descriptor_engine_order";
    }
    if (default_present || generated_present) {
      return "mysql_defaults_generated_columns_trigger_descriptor_engine_order";
    }
    return "mysql_trigger_default_generated_column_descriptor_required";
  }
  if (dialect_id == "postgresql") {
    if (Contains(upper, " ON CONFLICT ")) {
      return "postgresql_on_conflict_defaults_generated_columns_triggers_descriptor_engine_order";
    }
    if (default_present || generated_present || ContainsWord(upper, "RETURNING")) {
      return "postgresql_defaults_generated_columns_triggers_returning_descriptor_engine_order";
    }
    return "postgresql_trigger_default_generated_column_descriptor_required";
  }
  return "unknown_trigger_default_generated_column_policy";
}

std::string_view QueryExpressionSurface(std::string_view upper) {
  if (StartsWithCommand(upper, "WITH")) return "with_query_scalar_expression";
  if (StartsWithCommand(upper, "SELECT")) return "select_scalar_expression";
  return "query_scalar_expression";
}

std::string_view CastTypeCoercionProfile(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_cast_domain_charset_decfloat_int128_descriptor";
  }
  if (dialect_id == "mysql") {
    return "mysql_cast_convert_type_coercion_sql_mode_descriptor";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_cast_operator_type_resolution_descriptor";
  }
  return "unknown_cast_type_coercion_profile";
}

std::string_view NullThreeValuedLogicProfile(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_boolean_unknown_three_valued_logic_profile";
  }
  if (dialect_id == "mysql") {
    return "mysql_three_valued_logic_null_safe_equality_descriptor";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_three_valued_logic_is_distinct_from_descriptor";
  }
  return "unknown_null_three_valued_logic_profile";
}

std::string_view BooleanLiteralProfile(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_boolean_true_false_unknown_literal_profile";
  }
  if (dialect_id == "mysql") {
    return "mysql_truthy_numeric_boolean_literal_profile";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_strict_boolean_type_literal_profile";
  }
  return "unknown_boolean_literal_profile";
}

std::string_view StringComparisonCollationProfile(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_charset_collation_descriptor_no_parser_collation_authority";
  }
  if (dialect_id == "mysql") {
    return "mysql_charset_collation_coercibility_descriptor_no_parser_collation_authority";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_collation_operator_resolution_descriptor_no_parser_collation_authority";
  }
  return "unknown_string_comparison_collation_profile";
}

std::string_view TemporalLiteralCurrentTimestampDateArithmeticProfile(
    std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_date_time_timestamp_dateadd_datediff_descriptor";
  }
  if (dialect_id == "mysql") {
    return "mysql_datetime_timestamp_current_timestamp_date_add_sql_mode_descriptor";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_timestamp_timestamptz_interval_timezone_descriptor";
  }
  return "unknown_temporal_expression_profile";
}

std::string_view NumericDivisionRoundingOverflowProfile(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_exact_numeric_decfloat_int128_division_rounding_overflow_descriptor";
  }
  if (dialect_id == "mysql") {
    return "mysql_division_rounding_overflow_sql_mode_descriptor";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_numeric_division_rounding_overflow_descriptor";
  }
  return "unknown_numeric_division_rounding_overflow_profile";
}

std::string_view PatternMatchingProfile(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_like_similar_to_containing_starting_with_descriptor";
  }
  if (dialect_id == "mysql") {
    return "mysql_like_regexp_rlike_collation_descriptor";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_like_similar_to_regex_operator_descriptor";
  }
  return "unknown_pattern_matching_profile";
}

std::string_view ResourceTextSurface(std::string_view dialect_id,
                                     std::string_view upper) {
  const bool ddl_surface = StartsWithCommand(upper, "CREATE") ||
                           StartsWithCommand(upper, "ALTER") ||
                           StartsWithCommand(upper, "DROP") ||
                           StartsWithCommand(upper, "RECREATE");
  const bool dml_surface = StartsWithCommand(upper, "INSERT") ||
                           StartsWithCommand(upper, "UPDATE") ||
                           StartsWithCommand(upper, "DELETE") ||
                           StartsWithCommand(upper, "MERGE") ||
                           StartsWithCommand(upper, "REPLACE");
  const bool pattern_surface = Contains(upper, " LIKE ") ||
                               Contains(upper, " SIMILAR TO ") ||
                               Contains(upper, " REGEXP ") ||
                               Contains(upper, " RLIKE ") ||
                               Contains(upper, " STARTING WITH ") ||
                               Contains(upper, " CONTAINING ") ||
                               Contains(upper, " ILIKE ") ||
                               Contains(upper, " ~ ") ||
                               Contains(upper, " ~* ") ||
                               Contains(upper, " !~ ") ||
                               Contains(upper, " !~* ");
  const bool timezone_surface =
      Contains(upper, "WITH TIME ZONE") || ContainsWord(upper, "TIMEZONE") ||
      ContainsWord(upper, "TIMESTAMPTZ") || ContainsWord(upper, "DATETIME") ||
      ContainsWord(upper, "TIMESTAMP");
  const bool binary_surface = ContainsWord(upper, "BLOB") ||
                              ContainsWord(upper, "BINARY") ||
                              ContainsWord(upper, "VARBINARY") ||
                              ContainsWord(upper, "BYTEA");
  if (dialect_id == "firebird") {
    if (ddl_surface) return "firebird_ddl_charset_collation_text_blob";
    if (dml_surface) return "firebird_dml_text_resource_descriptor";
    if (pattern_surface) return "firebird_query_like_similar_containing";
    if (timezone_surface) return "firebird_query_temporal_timezone_resource";
    if (binary_surface) return "firebird_binary_blob_text_resource";
    return "firebird_query_resource_text_semantics";
  }
  if (dialect_id == "mysql") {
    if (ddl_surface) return "mysql_ddl_charset_collation_text_binary";
    if (dml_surface) return "mysql_dml_text_resource_descriptor";
    if (pattern_surface) return "mysql_query_like_regexp_rlike";
    if (timezone_surface) return "mysql_query_datetime_timestamp_resource";
    if (binary_surface) return "mysql_binary_text_resource";
    return "mysql_query_resource_text_semantics";
  }
  if (dialect_id == "postgresql") {
    if (ddl_surface) return "postgresql_ddl_text_collation_bytea";
    if (dml_surface) return "postgresql_dml_text_resource_descriptor";
    if (pattern_surface) return "postgresql_query_like_similar_regex_collation";
    if (timezone_surface) return "postgresql_query_timestamptz_timezone_resource";
    if (binary_surface) return "postgresql_bytea_text_resource";
    return "postgresql_query_resource_text_semantics";
  }
  return "unknown_resource_text_surface";
}

std::string_view CharsetPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_character_set_descriptor_uuid_required_engine_applies";
  }
  if (dialect_id == "mysql") {
    return "mysql_character_set_descriptor_uuid_required_engine_applies";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_database_encoding_descriptor_uuid_required_engine_applies";
  }
  return "unknown_charset_policy";
}

std::string_view TimezonePolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_session_timezone_descriptor_engine_authority";
  }
  if (dialect_id == "mysql") {
    return "mysql_session_time_zone_descriptor_engine_authority";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_time_zone_guc_descriptor_engine_authority";
  }
  return "unknown_timezone_policy";
}

std::string_view CalendarPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_temporal_calendar_descriptor_engine_authority";
  }
  if (dialect_id == "mysql") {
    return "mysql_temporal_calendar_sql_mode_descriptor_engine_authority";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_datestyle_intervalstyle_calendar_descriptor_engine_authority";
  }
  return "unknown_calendar_policy";
}

std::string_view ComparisonPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_text_comparison_charset_collation_descriptor_engine_authority";
  }
  if (dialect_id == "mysql") {
    return "mysql_text_comparison_charset_collation_coercibility_descriptor_engine_authority";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_text_comparison_collation_operator_descriptor_engine_authority";
  }
  return "unknown_comparison_policy";
}

std::string_view BinaryTextPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_blob_sub_type_binary_text_descriptor_required";
  }
  if (dialect_id == "mysql") {
    return "mysql_binary_varbinary_blob_text_descriptor_required";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_bytea_text_cast_descriptor_required";
  }
  return "unknown_binary_text_policy";
}

std::string_view ResourceEpochPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_resource_text_descriptor_epoch_engine_mga_catalog_bound";
  }
  if (dialect_id == "mysql") {
    return "mysql_resource_text_descriptor_epoch_engine_catalog_bound";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_resource_text_descriptor_epoch_engine_mga_catalog_bound";
  }
  return "unknown_resource_epoch_policy";
}

std::string_view TextIndexCompatibilityPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_text_index_charset_collation_compatibility_engine_validated";
  }
  if (dialect_id == "mysql") {
    return "mysql_text_index_prefix_charset_collation_compatibility_engine_validated";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_text_index_operator_class_collation_compatibility_engine_validated";
  }
  return "unknown_text_index_compatibility_policy";
}

std::string_view StatisticsOptimizerSurface(std::string_view dialect_id,
                                            std::string_view upper) {
  if (dialect_id == "firebird") {
    if (StartsWithCommand(upper, "SET STATISTICS INDEX")) {
      return "firebird_set_statistics_index_selectivity_descriptor";
    }
    if (Contains(upper, "RDB$INDICES") ||
        Contains(upper, "RDB$INDEX_SEGMENTS")) {
      return "firebird_statistics_catalog_projection_descriptor";
    }
    return "firebird_statistics_optimizer_metadata_surface";
  }
  if (dialect_id == "mysql") {
    if (StartsWithCommand(upper, "EXPLAIN")) {
      return "mysql_explain_plan_catalog_projection_descriptor";
    }
    if (StartsWithCommand(upper, "ANALYZE TABLE")) {
      return "mysql_analyze_table_statistics_update_refused_descriptor";
    }
    if (StartsWithCommand(upper, "OPTIMIZE TABLE")) {
      return "mysql_optimize_table_statistics_rebuild_refused_descriptor";
    }
    return "mysql_statistics_optimizer_metadata_surface";
  }
  if (dialect_id == "postgresql") {
    if (StartsWithCommand(upper, "EXPLAIN")) {
      return "postgresql_explain_plan_catalog_projection_descriptor";
    }
    if (StartsWithCommand(upper, "ANALYZE")) {
      return "postgresql_analyze_statistics_update_refused_descriptor";
    }
    if (StartsWithCommand(upper, "VACUUM")) {
      return "postgresql_vacuum_analyze_statistics_refused_descriptor";
    }
    if (StartsWithCommand(upper, "REINDEX")) {
      return "postgresql_reindex_statistics_dependency_refused_descriptor";
    }
    if (StartsWithCommand(upper, "CREATE STATISTICS")) {
      return "postgresql_create_statistics_catalog_descriptor";
    }
    if (StartsWithCommand(upper, "DROP STATISTICS")) {
      return "postgresql_drop_statistics_catalog_descriptor";
    }
    return "postgresql_statistics_optimizer_metadata_surface";
  }
  return "unknown_statistics_optimizer_metadata_surface";
}

std::string_view StatisticsCommandPolicy(std::string_view dialect_id,
                                         std::string_view upper) {
  if (dialect_id == "firebird") {
    if (StartsWithCommand(upper, "SET STATISTICS INDEX")) {
      return "firebird_set_statistics_index_descriptor_only_engine_recomputes_selectivity";
    }
    return "firebird_statistics_metadata_catalog_descriptor_only";
  }
  if (dialect_id == "mysql") {
    if (StartsWithCommand(upper, "ANALYZE TABLE") ||
        StartsWithCommand(upper, "OPTIMIZE TABLE")) {
      return "mysql_statistics_maintenance_command_refused_no_donor_execution";
    }
    return "mysql_optimizer_metadata_catalog_projection_only";
  }
  if (dialect_id == "postgresql") {
    if (StartsWithCommand(upper, "ANALYZE") ||
        StartsWithCommand(upper, "VACUUM") ||
        StartsWithCommand(upper, "REINDEX")) {
      return "postgresql_statistics_maintenance_command_refused_no_donor_execution";
    }
    return "postgresql_optimizer_metadata_catalog_projection_only";
  }
  return "unknown_statistics_command_policy";
}

std::string_view HistogramPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_index_selectivity_descriptor_engine_statistics_authority";
  }
  if (dialect_id == "mysql") {
    return "mysql_histogram_descriptor_engine_statistics_authority";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_pg_statistic_histogram_descriptor_engine_statistics_authority";
  }
  return "unknown_histogram_policy";
}

std::string_view SelectivityPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_rdb_index_statistics_selectivity_descriptor_engine_authority";
  }
  if (dialect_id == "mysql") {
    return "mysql_index_cardinality_selectivity_descriptor_engine_authority";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_ndistinct_mcv_selectivity_descriptor_engine_authority";
  }
  return "unknown_selectivity_policy";
}

std::string_view StaleStatisticsPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_stale_statistics_recompute_requires_engine_statistics_epoch";
  }
  if (dialect_id == "mysql") {
    return "mysql_persistent_statistics_staleness_descriptor_engine_epoch";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_autovacuum_analyze_staleness_descriptor_engine_epoch";
  }
  return "unknown_stale_statistics_policy";
}

std::string_view StatisticsIndexEligibilityPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_index_selectivity_eligibility_engine_index_descriptor";
  }
  if (dialect_id == "mysql") {
    return "mysql_visible_index_optimizer_eligibility_engine_descriptor";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_index_valid_ready_predicate_eligibility_engine_descriptor";
  }
  return "unknown_index_eligibility_policy";
}

std::string_view PlanInvalidationPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_plan_invalidation_engine_metadata_statistics_epoch";
  }
  if (dialect_id == "mysql") {
    return "mysql_plan_invalidation_engine_dictionary_statistics_epoch";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_plan_cache_invalidation_engine_catalog_statistics_epoch";
  }
  return "unknown_plan_invalidation_policy";
}

std::string_view AnalyzeCommandPolicy(std::string_view dialect_id,
                                      std::string_view upper) {
  if (dialect_id == "firebird") {
    if (StartsWithCommand(upper, "SET STATISTICS INDEX")) {
      return "firebird_set_statistics_index_maps_to_engine_statistics_descriptor_request";
    }
    return "firebird_no_analyze_command_descriptor_policy";
  }
  if (dialect_id == "mysql") {
    return StartsWithCommand(upper, "ANALYZE TABLE")
               ? "mysql_analyze_table_refused_descriptor_no_donor_execution"
               : "mysql_analyze_table_policy_descriptor_required";
  }
  if (dialect_id == "postgresql") {
    return StartsWithCommand(upper, "ANALYZE")
               ? "postgresql_analyze_refused_descriptor_no_donor_execution"
               : "postgresql_analyze_policy_descriptor_required";
  }
  return "unknown_analyze_command_policy";
}

std::string_view ExplainPlanPolicy(std::string_view dialect_id,
                                   std::string_view upper) {
  if (dialect_id == "firebird") {
    return "firebird_plan_metadata_descriptor_only_no_parser_optimizer_authority";
  }
  if (dialect_id == "mysql") {
    return StartsWithCommand(upper, "EXPLAIN")
               ? "mysql_explain_catalog_projection_descriptor_no_plan_authority"
               : "mysql_explain_policy_descriptor_required";
  }
  if (dialect_id == "postgresql") {
    return StartsWithCommand(upper, "EXPLAIN")
               ? "postgresql_explain_catalog_projection_descriptor_no_plan_authority"
               : "postgresql_explain_policy_descriptor_required";
  }
  return "unknown_explain_plan_policy";
}

std::string_view StatisticsCatalogProjectionPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_rdb_indices_statistics_catalog_projection_uuid_required";
  }
  if (dialect_id == "mysql") {
    return "mysql_information_schema_statistics_projection_uuid_required";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_pg_statistic_pg_stats_projection_uuid_required";
  }
  return "unknown_statistics_catalog_projection_policy";
}

bool IsRowLockQuery(std::string_view upper) {
  if (!(StartsWithCommand(upper, "SELECT") || StartsWithCommand(upper, "WITH") ||
        (StartsWith(upper, "(") && Contains(upper, "SELECT")))) {
    return false;
  }
  return Contains(upper, " FOR UPDATE") || Contains(upper, " FOR SHARE") ||
         Contains(upper, " LOCK IN SHARE MODE");
}

std::string_view LocksIsolationSurface(std::string_view dialect_id,
                                       std::string_view upper) {
  if (dialect_id == "firebird") {
    if (StartsWithCommand(upper, "SET TRANSACTION")) {
      if (ContainsWord(upper, "NOWAIT")) {
        return "firebird_set_transaction_nowait_isolation_descriptor";
      }
      if (ContainsWord(upper, "WAIT")) {
        return "firebird_set_transaction_wait_isolation_descriptor";
      }
      return "firebird_set_transaction_isolation_descriptor";
    }
    if (Contains(upper, " FOR UPDATE")) {
      return "firebird_select_for_update_row_lock_descriptor";
    }
    return "firebird_locks_isolation_syntax_surface";
  }
  if (dialect_id == "mysql") {
    if (StartsWithCommand(upper, "LOCK TABLES")) {
      return "mysql_lock_tables_table_lock_descriptor";
    }
    if (StartsWithCommand(upper, "UNLOCK TABLES")) {
      return "mysql_unlock_tables_engine_lock_release_descriptor";
    }
    if (Contains(upper, " FOR UPDATE")) {
      return "mysql_select_for_update_row_lock_descriptor";
    }
    if (Contains(upper, " FOR SHARE") ||
        Contains(upper, " LOCK IN SHARE MODE")) {
      return "mysql_select_for_share_row_lock_descriptor";
    }
    if (StartsWithCommand(upper, "SET TRANSACTION") ||
        StartsWithCommand(upper, "START TRANSACTION")) {
      return "mysql_transaction_isolation_read_write_descriptor";
    }
    return "mysql_locks_isolation_syntax_surface";
  }
  if (dialect_id == "postgresql") {
    if (StartsWithCommand(upper, "LOCK TABLE")) {
      return ContainsWord(upper, "NOWAIT")
                 ? "postgresql_lock_table_mode_nowait_descriptor"
                 : "postgresql_lock_table_mode_descriptor";
    }
    if (Contains(upper, " FOR UPDATE")) {
      return "postgresql_select_for_update_row_lock_descriptor";
    }
    if (Contains(upper, " FOR SHARE")) {
      return "postgresql_select_for_share_row_lock_descriptor";
    }
    if (StartsWithCommand(upper, "SET TRANSACTION") ||
        StartsWithCommand(upper, "BEGIN") ||
        StartsWithCommand(upper, "START TRANSACTION")) {
      return "postgresql_transaction_isolation_read_write_descriptor";
    }
    return "postgresql_locks_isolation_syntax_surface";
  }
  return "unknown_locks_isolation_syntax_surface";
}

std::string_view IsolationProfileUuidOrPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_tpb_isolation_descriptor_uuid_required_engine_mga_authority";
  }
  if (dialect_id == "mysql") {
    return "mysql_transaction_isolation_descriptor_uuid_required_engine_mga_authority";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_transaction_isolation_descriptor_uuid_required_engine_mga_authority";
  }
  return "unknown_isolation_descriptor_policy";
}

std::string_view LockClausePolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_for_update_wait_no_wait_descriptor_engine_lock_authority";
  }
  if (dialect_id == "mysql") {
    return "mysql_lock_tables_and_for_update_descriptor_engine_lock_authority";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_lock_table_and_row_lock_descriptor_engine_lock_authority";
  }
  return "unknown_lock_clause_policy";
}

std::string_view NowaitPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_nowait_tpb_descriptor_engine_lock_wait_policy";
  }
  if (dialect_id == "mysql") {
    return "mysql_nowait_descriptor_engine_lock_wait_policy";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_nowait_descriptor_engine_lock_wait_policy";
  }
  return "unknown_nowait_policy";
}

std::string_view SkipLockedPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_skip_locked_not_supported_descriptor_refusal_policy";
  }
  if (dialect_id == "mysql") {
    return "mysql_skip_locked_descriptor_engine_row_lock_policy";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_skip_locked_descriptor_engine_row_lock_policy";
  }
  return "unknown_skip_locked_policy";
}

std::string_view AdvisoryLockPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_no_advisory_lock_surface_descriptor_refusal_policy";
  }
  if (dialect_id == "mysql") {
    return "mysql_get_lock_release_lock_advisory_descriptor_engine_policy";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_pg_advisory_lock_descriptor_engine_policy";
  }
  return "unknown_advisory_lock_policy";
}

std::string_view TableLockPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_explicit_table_lock_not_supported_descriptor_refusal_policy";
  }
  if (dialect_id == "mysql") {
    return "mysql_lock_tables_descriptor_engine_lock_authority";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_lock_table_mode_descriptor_engine_lock_authority";
  }
  return "unknown_table_lock_policy";
}

std::string_view RowLockPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_for_update_descriptor_engine_cursor_lock_authority";
  }
  if (dialect_id == "mysql") {
    return "mysql_for_update_for_share_descriptor_engine_row_lock_authority";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_for_update_for_share_descriptor_engine_row_lock_authority";
  }
  return "unknown_row_lock_policy";
}

std::string_view ReadWritePolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_read_only_read_write_tpb_descriptor_engine_intent_authority";
  }
  if (dialect_id == "mysql") {
    return "mysql_read_only_read_write_descriptor_engine_intent_authority";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_read_only_read_write_descriptor_engine_intent_authority";
  }
  return "unknown_read_write_policy";
}

std::string_view DeadlockDiagnosticPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_deadlock_diagnostic_map_descriptor_engine_lock_manager_authority";
  }
  if (dialect_id == "mysql") {
    return "mysql_deadlock_diagnostic_map_descriptor_engine_lock_manager_authority";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_deadlock_diagnostic_map_descriptor_engine_lock_manager_authority";
  }
  return "unknown_deadlock_diagnostic_policy";
}

std::string_view ConditionalExpressionProfile(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_coalesce_case_iif_nullif_decode_descriptor";
  }
  if (dialect_id == "mysql") {
    return "mysql_case_if_ifnull_nullif_coalesce_descriptor";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_case_coalesce_nullif_descriptor";
  }
  return "unknown_conditional_expression_profile";
}

std::string_view ExpressionBuiltinProfile(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_expression_builtin_profile_iif_dateadd_decfloat_int128";
  }
  if (dialect_id == "mysql") {
    return "mysql_expression_builtin_profile_if_ifnull_date_add_regexp";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_expression_operator_resolution_profile_is_distinct_from_similar_regex";
  }
  return "unknown_expression_builtin_profile";
}

bool HasCastOrCoercionSurface(std::string_view upper) {
  return Contains(upper, "CAST(") || Contains(upper, "CONVERT(") ||
         Contains(upper, "::");
}

bool HasNullLogicSurface(std::string_view upper) {
  return ContainsWord(upper, "NULL") || Contains(upper, " IS DISTINCT FROM") ||
         Contains(upper, " IS NOT DISTINCT FROM") || Contains(upper, "<=>");
}

bool HasBooleanLiteralSurface(std::string_view upper) {
  return ContainsWord(upper, "TRUE") || ContainsWord(upper, "FALSE") ||
         ContainsWord(upper, "UNKNOWN");
}

bool HasStringComparisonSurface(std::string_view upper) {
  return ContainsWord(upper, "COLLATE") || Contains(upper, " LIKE ") ||
         Contains(upper, " SIMILAR TO ") || Contains(upper, " REGEXP ") ||
         Contains(upper, " RLIKE ") || Contains(upper, " STARTING WITH ") ||
         Contains(upper, " CONTAINING ") || Contains(upper, " ILIKE ");
}

bool HasTemporalExpressionSurface(std::string_view upper) {
  return ContainsWord(upper, "CURRENT_DATE") ||
         ContainsWord(upper, "CURRENT_TIME") ||
         ContainsWord(upper, "CURRENT_TIMESTAMP") ||
         ContainsWord(upper, "LOCALTIMESTAMP") ||
         ContainsWord(upper, "LOCALTIME") || Contains(upper, "DATEADD(") ||
         Contains(upper, "DATEDIFF(") || Contains(upper, "DATE_ADD(") ||
         Contains(upper, "DATE_SUB(") || Contains(upper, "TIMESTAMPDIFF(") ||
         Contains(upper, "EXTRACT(") || Contains(upper, "DATE_PART(") ||
         Contains(upper, "DATE_TRUNC(") || Contains(upper, "NOW(") ||
         ContainsWord(upper, "INTERVAL") || Contains(upper, "TIMESTAMP '") ||
         Contains(upper, "TIMESTAMPTZ '") || Contains(upper, "DATE '") ||
         Contains(upper, "TIME '");
}

bool HasNumericExpressionSurface(std::string_view upper) {
  return Contains(upper, "/") || Contains(upper, " DIV ") ||
         Contains(upper, "ROUND(") || Contains(upper, "TRUNC(") ||
         Contains(upper, "TRUNCATE(") || Contains(upper, "MOD(") ||
         Contains(upper, "POWER(") || Contains(upper, "POW(") ||
         Contains(upper, "SQRT(") || ContainsWord(upper, "DECFLOAT") ||
         ContainsWord(upper, "INT128") || ContainsWord(upper, "NUMERIC") ||
         ContainsWord(upper, "DECIMAL");
}

bool HasPatternMatchingSurface(std::string_view upper) {
  return Contains(upper, " LIKE ") || Contains(upper, " SIMILAR TO ") ||
         Contains(upper, " REGEXP ") || Contains(upper, " RLIKE ") ||
         Contains(upper, " STARTING WITH ") || Contains(upper, " CONTAINING ") ||
         Contains(upper, " ILIKE ") || Contains(upper, " ~ ") ||
         Contains(upper, " ~* ") || Contains(upper, " !~ ") ||
         Contains(upper, " !~* ");
}

bool HasFunctionCall(std::string_view upper, std::string_view name) {
  return ContainsWord(upper, name) &&
         Contains(upper, std::string(name) + "(");
}

bool HasConditionalExpressionSurface(std::string_view upper) {
  return HasFunctionCall(upper, "COALESCE") ||
         HasFunctionCall(upper, "NULLIF") ||
         HasFunctionCall(upper, "IIF") ||
         HasFunctionCall(upper, "IF") ||
         HasFunctionCall(upper, "IFNULL") ||
         HasFunctionCall(upper, "DECODE") ||
         Contains(upper, "CASE ");
}

std::string_view UnquotedIdentifierPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") return "firebird_unquoted_identifiers_fold_to_uppercase";
  if (dialect_id == "mysql") {
    return "mysql_unquoted_identifiers_preserve_spelling_table_name_case_bound_by_lower_case_table_names";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_unquoted_identifiers_fold_to_lowercase";
  }
  return "unknown_unquoted_identifier_policy";
}

std::string_view QuotedIdentifierPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_double_quoted_identifiers_preserve_exact_case";
  }
  if (dialect_id == "mysql") {
    return "mysql_quoted_identifiers_preserve_exact_case_backtick_default_ansi_quotes_profile_bound";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_double_quoted_identifiers_preserve_exact_case";
  }
  return "unknown_quoted_identifier_policy";
}

std::string_view SchemaRootResolutionPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_single_database_root_uuid_catalog_resolution_required";
  }
  if (dialect_id == "mysql") {
    return "mysql_database_schema_root_uuid_resolution_required_no_filesystem_authority";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_database_schema_search_path_uuid_resolution_required";
  }
  return "unknown_schema_root_resolution_policy";
}

std::string_view GeneratedCatalogNameBehavior(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_rdb_generated_names_catalog_descriptor_required";
  }
  if (dialect_id == "mysql") {
    return "mysql_engine_generated_names_descriptor_required_lower_case_table_names_bound";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_catalog_generated_names_descriptor_required";
  }
  return "unknown_generated_catalog_name_behavior";
}

std::string_view NamespaceCollisionBehavior(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_catalog_namespace_collision_resolved_by_uuid_descriptor";
  }
  if (dialect_id == "mysql") {
    return "mysql_schema_table_namespace_collision_resolved_by_uuid_descriptor_and_lctn_profile";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_schema_namespace_collision_resolved_by_uuid_descriptor_and_search_path";
  }
  return "unknown_namespace_collision_behavior";
}

std::string_view ResultMetadataLabelPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_result_labels_follow_identifier_fold_alias_descriptor";
  }
  if (dialect_id == "mysql") {
    return "mysql_result_labels_preserve_alias_spelling_descriptor";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_result_labels_follow_lowercase_fold_alias_descriptor";
  }
  return "unknown_result_metadata_label_policy";
}

std::string_view TableNameFilesystemCasePolicy(std::string_view dialect_id) {
  if (dialect_id == "mysql") {
    return "mysql_lower_case_table_names_filesystem_sensitive_bound_descriptor";
  }
  return "not_filesystem_sensitive_table_name_policy";
}

std::string_view IndexMethod(std::string_view dialect_id,
                             std::string_view upper) {
  if (dialect_id == "firebird") {
    if (ContainsWord(upper, "DESC") || ContainsWord(upper, "DESCENDING")) {
      return "firebird_btree_descending_index_profile";
    }
    return "firebird_btree_ascending_index_profile";
  }
  if (dialect_id == "mysql") {
    if (ContainsWord(upper, "FULLTEXT")) return "mysql_fulltext_index_profile";
    if (ContainsWord(upper, "SPATIAL")) return "mysql_spatial_index_profile";
    if (Contains(upper, " USING HASH")) {
      return "mysql_hash_index_method_requested_engine_validated";
    }
    return "mysql_innodb_btree_index_profile";
  }
  if (dialect_id == "postgresql") {
    if (Contains(upper, " USING HASH")) return "postgresql_hash_access_method_explicit";
    if (Contains(upper, " USING GIN")) return "postgresql_gin_access_method_explicit";
    if (Contains(upper, " USING GIST")) return "postgresql_gist_access_method_explicit";
    if (Contains(upper, " USING BRIN")) return "postgresql_brin_access_method_explicit";
    if (Contains(upper, " USING SPGIST")) return "postgresql_spgist_access_method_explicit";
    return "postgresql_btree_access_method_default";
  }
  return "unknown_index_method";
}

std::string_view UniqueNullPolicy(std::string_view dialect_id,
                                  std::string_view upper) {
  if (!ContainsWord(upper, "UNIQUE")) return "not_unique_index_not_applicable";
  if (dialect_id == "firebird") {
    return "firebird_unique_index_nulls_are_distinct_profile";
  }
  if (dialect_id == "mysql") {
    return "mysql_innodb_unique_index_allows_multiple_nulls_profile";
  }
  if (dialect_id == "postgresql") {
    if (Contains(upper, "NULLS NOT DISTINCT")) {
      return "postgresql_unique_nulls_not_distinct_requested";
    }
    return "postgresql_unique_nulls_distinct_default";
  }
  return "unique_null_policy_unknown";
}

std::string_view NullOrdering(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_nulls_first_for_ascending_index_profile";
  }
  if (dialect_id == "mysql") {
    return "mysql_nulls_low_ascending_index_profile";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_nulls_last_for_ascending_btree_default";
  }
  return "unknown_null_ordering";
}

std::string_view CollationPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_column_charset_collation_descriptor";
  }
  if (dialect_id == "mysql") {
    return "mysql_character_set_collation_weight_string_descriptor";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_per_expression_collation_descriptor";
  }
  return "unknown_collation_policy";
}

std::string_view OperatorFamilyPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_builtin_comparison_no_named_operator_family";
  }
  if (dialect_id == "mysql") {
    return "mysql_builtin_comparison_no_named_operator_family";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_default_operator_class_and_family_resolution";
  }
  return "unknown_operator_family_policy";
}

std::string_view PredicateOrExpressionPolicy(std::string_view dialect_id,
                                             std::string_view upper) {
  const bool expression = Contains(upper, "((") ||
                          Contains(upper, "COMPUTED BY");
  const bool predicate = ContainsWord(upper, "WHERE");
  if (dialect_id == "firebird") {
    return expression ? "firebird_computed_by_expression_index_descriptor"
                      : "firebird_column_index_no_partial_predicate";
  }
  if (dialect_id == "mysql") {
    return expression ? "mysql_functional_key_part_descriptor"
                      : "mysql_column_index_no_partial_predicate";
  }
  if (dialect_id == "postgresql") {
    if (expression && predicate) {
      return "postgresql_expression_and_partial_predicate_descriptor";
    }
    if (expression) return "postgresql_expression_index_descriptor";
    if (predicate) return "postgresql_partial_predicate_descriptor";
    return "postgresql_column_index_no_predicate_descriptor";
  }
  return "unknown_predicate_or_expression_policy";
}

std::string_view ValidationState(std::string_view dialect_id,
                                 std::string_view upper) {
  if (dialect_id == "firebird") {
    if (ContainsWord(upper, "INACTIVE")) return "firebird_index_inactive_requested";
    return "firebird_index_active_default";
  }
  if (dialect_id == "mysql") {
    if (ContainsWord(upper, "INVISIBLE")) return "mysql_index_invisible_requested";
    return "mysql_index_visible_default";
  }
  if (dialect_id == "postgresql") {
    if (StartsWithCommand(TrimAsciiView(upper), "ALTER INDEX")) {
      return "postgresql_index_catalog_validity_preserved_by_alter";
    }
    return "postgresql_index_valid_after_build_default";
  }
  return "unknown_validation_state";
}

std::string_view BuildMode(std::string_view dialect_id,
                           std::string_view upper) {
  if (dialect_id == "firebird") {
    if (StartsWithCommand(TrimAsciiView(upper), "ALTER INDEX")) {
      return "firebird_index_metadata_state_change_no_parser_build";
    }
    return "firebird_immediate_index_build_default";
  }
  if (dialect_id == "mysql") {
    if (StartsWithCommand(TrimAsciiView(upper), "ALTER TABLE")) {
      return "mysql_alter_index_visibility_or_metadata_route";
    }
    return "mysql_engine_selected_online_ddl_default";
  }
  if (dialect_id == "postgresql") {
    if (ContainsWord(upper, "CONCURRENTLY")) {
      return "postgresql_concurrent_index_build_requested";
    }
    if (StartsWithCommand(TrimAsciiView(upper), "ALTER INDEX")) {
      return "postgresql_alter_index_metadata_route";
    }
    return "postgresql_nonconcurrent_index_build_default";
  }
  return "unknown_build_mode";
}

std::string_view StatisticsPolicyRef(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_index_selectivity_statistics_profile";
  }
  if (dialect_id == "mysql") {
    return "mysql_innodb_persistent_index_statistics_profile";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_pg_statistic_and_pg_class_index_statistics_profile";
  }
  return "unknown_statistics_policy";
}

std::string_view PrimaryKeyBehavior(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_primary_key_not_null_unique_index_descriptor";
  }
  if (dialect_id == "mysql") {
    return "mysql_primary_key_not_null_unique_index_innodb_descriptor";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_primary_key_not_null_unique_btree_descriptor";
  }
  return "unknown_primary_key_behavior";
}

std::string_view ConstraintUniqueNullPolicy(std::string_view dialect_id,
                                            std::string_view upper) {
  if (!ContainsWord(upper, "UNIQUE")) {
    return "not_unique_constraint_not_applicable";
  }
  if (dialect_id == "firebird") {
    return "firebird_unique_constraint_nulls_are_distinct_profile";
  }
  if (dialect_id == "mysql") {
    return "mysql_unique_constraint_allows_multiple_nulls_profile";
  }
  if (dialect_id == "postgresql") {
    if (Contains(upper, "NULLS NOT DISTINCT")) {
      return "postgresql_unique_constraint_nulls_not_distinct_requested";
    }
    return "postgresql_unique_constraint_nulls_distinct_default";
  }
  return "unknown_unique_constraint_null_policy";
}

std::string_view ForeignKeyActionDefaults(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_foreign_key_default_no_action_update_no_action_delete_descriptor";
  }
  if (dialect_id == "mysql") {
    return "mysql_innodb_foreign_key_default_restrict_update_restrict_delete_descriptor";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_foreign_key_default_no_action_update_no_action_delete_descriptor";
  }
  return "unknown_foreign_key_action_defaults";
}

std::string_view CheckTruthTableNullBehavior(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_check_constraint_false_fails_unknown_passes_profile";
  }
  if (dialect_id == "mysql") {
    return "mysql_check_constraint_false_fails_unknown_passes_profile";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_check_constraint_false_fails_unknown_passes_profile";
  }
  return "unknown_check_truth_table_null_behavior";
}

std::string_view DefaultExpressionPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_default_expression_descriptor_runtime_equivalence_pending";
  }
  if (dialect_id == "mysql") {
    return "mysql_default_literal_or_parenthesized_expression_descriptor";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_variable_free_default_expression_descriptor";
  }
  return "unknown_default_expression_policy";
}

std::string_view GeneratedIdentityAutoincrementPolicy(std::string_view dialect_id,
                                                      std::string_view upper) {
  if (dialect_id == "firebird") {
    if (ContainsWord(upper, "GENERATED")) {
      return "firebird_generated_identity_sequence_backed_descriptor";
    }
    return "firebird_no_implicit_autoincrement_default";
  }
  if (dialect_id == "mysql") {
    if (ContainsWord(upper, "AUTO_INCREMENT")) {
      return "mysql_auto_increment_column_attribute_descriptor";
    }
    return "mysql_no_implicit_autoincrement_default";
  }
  if (dialect_id == "postgresql") {
    if (ContainsWord(upper, "GENERATED")) {
      return "postgresql_sql_identity_sequence_backed_descriptor";
    }
    if (ContainsWord(upper, "SERIAL") || ContainsWord(upper, "BIGSERIAL") ||
        ContainsWord(upper, "SMALLSERIAL")) {
      return "postgresql_serial_pseudo_type_sequence_default_descriptor";
    }
    return "postgresql_no_implicit_autoincrement_default";
  }
  return "unknown_generated_identity_autoincrement_policy";
}

std::string_view SequenceIdentitySurface(std::string_view dialect_id,
                                         std::string_view upper) {
  if (dialect_id == "firebird") {
    if (StartsWithCommand(upper, "CREATE GENERATOR")) return "firebird_create_generator";
    if (StartsWithCommand(upper, "CREATE SEQUENCE")) return "firebird_create_sequence";
    if (StartsWithCommand(upper, "ALTER SEQUENCE") ||
        StartsWithCommand(upper, "ALTER GENERATOR")) {
      return "firebird_alter_sequence_generator";
    }
    if (Contains(upper, "GEN_ID(") || Contains(upper, "NEXT VALUE FOR")) {
      return "firebird_generator_value_expression";
    }
    if (ContainsWord(upper, "GENERATED") && ContainsWord(upper, "IDENTITY")) {
      return "firebird_identity_column_sequence_descriptor";
    }
    return "firebird_sequence_generator_descriptor";
  }
  if (dialect_id == "mysql") {
    if (Contains(upper, "LAST_INSERT_ID(")) {
      return "mysql_last_insert_id_session_function";
    }
    if (ContainsWord(upper, "AUTO_INCREMENT")) {
      return "mysql_auto_increment_column_or_table_option";
    }
    return "mysql_auto_increment_descriptor";
  }
  if (dialect_id == "postgresql") {
    if (StartsWithCommand(upper, "CREATE SEQUENCE")) return "postgresql_create_sequence";
    if (StartsWithCommand(upper, "ALTER SEQUENCE")) return "postgresql_alter_sequence";
    if (Contains(upper, "NEXTVAL(") || Contains(upper, "CURRVAL(") ||
        Contains(upper, "SETVAL(")) {
      return "postgresql_sequence_function_expression";
    }
    if ((ContainsWord(upper, "SERIAL") || ContainsWord(upper, "BIGSERIAL") ||
         ContainsWord(upper, "SMALLSERIAL")) &&
        ContainsWord(upper, "IDENTITY")) {
      return "postgresql_serial_and_identity_sequence_defaults";
    }
    if (ContainsWord(upper, "SERIAL") || ContainsWord(upper, "BIGSERIAL") ||
        ContainsWord(upper, "SMALLSERIAL")) {
      return "postgresql_serial_pseudo_type_sequence_default";
    }
    if (ContainsWord(upper, "GENERATED") && ContainsWord(upper, "IDENTITY")) {
      return "postgresql_sql_identity_sequence_default";
    }
    return "postgresql_sequence_descriptor";
  }
  return "unknown_sequence_identity_surface";
}

std::string_view SequenceObjectIdentityPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_sequence_generator_uuid_required_no_source_name_binding";
  }
  if (dialect_id == "mysql") {
    return "mysql_table_column_auto_increment_uuid_required_no_source_name_binding";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_sequence_and_owned_default_uuid_required_no_source_name_binding";
  }
  return "unknown_sequence_identity_uuid_policy";
}

std::string_view EngineCatalogSequenceDescriptorPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_engine_catalog_generator_sequence_descriptor_policy";
  }
  if (dialect_id == "mysql") {
    return "mysql_engine_catalog_auto_increment_counter_descriptor_policy";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_engine_catalog_sequence_descriptor_policy";
  }
  return "unknown_engine_catalog_sequence_descriptor_policy";
}

std::string_view SequenceAllocationFinalityPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_generator_nontransactional_allocation_descriptor_parser_not_allocator";
  }
  if (dialect_id == "mysql") {
    return "mysql_auto_increment_lower_layer_allocation_descriptor_parser_not_allocator";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_sequence_allocation_descriptor_parser_not_allocator";
  }
  return "unknown_sequence_allocation_policy";
}

std::string_view LowerLayerAllocationPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_engine_sequence_catalog_allocates_values_outside_parser";
  }
  if (dialect_id == "mysql") {
    return "mysql_storage_engine_auto_increment_allocator_policy_descriptor";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_sequence_access_method_and_catalog_allocator_policy";
  }
  return "unknown_lower_layer_allocation_policy";
}

std::string_view SequenceValueFunctionProfile(std::string_view dialect_id,
                                              std::string_view upper) {
  if (dialect_id == "firebird") {
    if (Contains(upper, "GEN_ID(") && Contains(upper, "NEXT VALUE FOR")) {
      return "firebird_gen_id_and_next_value_for_descriptor";
    }
    if (Contains(upper, "GEN_ID(")) return "firebird_gen_id_descriptor";
    if (Contains(upper, "NEXT VALUE FOR")) {
      return "firebird_next_value_for_descriptor";
    }
    return "firebird_generator_function_not_observed";
  }
  if (dialect_id == "mysql") {
    if (Contains(upper, "LAST_INSERT_ID(")) {
      return "mysql_last_insert_id_session_visible_descriptor";
    }
    return "mysql_last_insert_id_not_observed";
  }
  if (dialect_id == "postgresql") {
    if (Contains(upper, "NEXTVAL(") && Contains(upper, "CURRVAL(") &&
        Contains(upper, "SETVAL(")) {
      return "postgresql_nextval_currval_setval_descriptor";
    }
    if (Contains(upper, "NEXTVAL(")) return "postgresql_nextval_descriptor";
    if (Contains(upper, "CURRVAL(")) return "postgresql_currval_descriptor";
    if (Contains(upper, "SETVAL(")) return "postgresql_setval_descriptor";
    return "postgresql_sequence_function_not_observed";
  }
  return "unknown_sequence_value_function_profile";
}

std::string_view SequenceSessionVisibilityPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_generator_values_visible_immediately_no_parser_session_state";
  }
  if (dialect_id == "mysql") {
    return "mysql_last_insert_id_connection_session_visible_descriptor";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_currval_session_requires_prior_nextval_descriptor";
  }
  return "unknown_sequence_session_visibility_policy";
}

std::string_view SequenceBackedDefaultPolicy(std::string_view dialect_id,
                                             std::string_view upper) {
  if (dialect_id == "firebird") {
    if (ContainsWord(upper, "GENERATED") && ContainsWord(upper, "IDENTITY")) {
      return "firebird_identity_column_backed_by_sequence_descriptor";
    }
    return "firebird_no_identity_default_observed";
  }
  if (dialect_id == "mysql") {
    if (ContainsWord(upper, "AUTO_INCREMENT")) {
      return "mysql_auto_increment_column_counter_default_descriptor";
    }
    return "mysql_no_auto_increment_default_observed";
  }
  if (dialect_id == "postgresql") {
    if ((ContainsWord(upper, "SERIAL") || ContainsWord(upper, "BIGSERIAL") ||
         ContainsWord(upper, "SMALLSERIAL")) &&
        ContainsWord(upper, "IDENTITY")) {
      return "postgresql_serial_and_identity_sequence_backed_defaults";
    }
    if (ContainsWord(upper, "SERIAL") || ContainsWord(upper, "BIGSERIAL") ||
        ContainsWord(upper, "SMALLSERIAL")) {
      return "postgresql_serial_pseudo_type_sequence_backed_default";
    }
    if (ContainsWord(upper, "GENERATED") && ContainsWord(upper, "IDENTITY")) {
      return "postgresql_identity_sequence_backed_default";
    }
    return "postgresql_no_sequence_default_observed";
  }
  return "unknown_sequence_backed_default_policy";
}

std::string_view RestartIncrementDescriptorPolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_restart_with_and_increment_by_descriptor";
  }
  if (dialect_id == "mysql") {
    return "mysql_auto_increment_table_option_descriptor";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_start_restart_increment_min_max_cache_cycle_descriptor";
  }
  return "unknown_restart_increment_descriptor_policy";
}

std::string_view GeneratedConstraintNamePolicy(std::string_view dialect_id) {
  if (dialect_id == "firebird") {
    return "firebird_system_generated_constraint_names_rdb_descriptor_required";
  }
  if (dialect_id == "mysql") {
    return "mysql_engine_generated_constraint_names_descriptor_required";
  }
  if (dialect_id == "postgresql") {
    return "postgresql_catalog_generated_constraint_names_descriptor_required";
  }
  return "unknown_generated_constraint_name_policy";
}

std::string_view DeferrabilityPolicy(std::string_view dialect_id,
                                     std::string_view upper) {
  if (dialect_id == "firebird") {
    return "firebird_constraints_not_deferrable_immediate_profile";
  }
  if (dialect_id == "mysql") {
    return "mysql_constraints_not_deferrable_immediate_profile";
  }
  if (dialect_id == "postgresql") {
    if (Contains(upper, "NOT DEFERRABLE")) {
      return "postgresql_not_deferrable_initially_immediate_default_profile";
    }
    if (ContainsWord(upper, "DEFERRABLE")) {
      return "postgresql_deferrability_requested_descriptor";
    }
    return "postgresql_not_deferrable_initially_immediate_default_profile";
  }
  return "unknown_deferrability_policy";
}

std::string_view EnforcementTiming(std::string_view dialect_id,
                                   std::string_view upper) {
  if (dialect_id == "firebird") {
    return "firebird_immediate_constraint_validation_profile";
  }
  if (dialect_id == "mysql") {
    return "mysql_innodb_immediate_constraint_validation_profile";
  }
  if (dialect_id == "postgresql") {
    if (Contains(upper, "NOT DEFERRABLE")) {
      return "postgresql_immediate_constraint_validation_default_profile";
    }
    if (ContainsWord(upper, "DEFERRABLE")) {
      return "postgresql_constraint_timing_descriptor_requested_runtime_proven";
    }
    return "postgresql_immediate_constraint_validation_default_profile";
  }
  return "unknown_enforcement_timing";
}

struct ParserEvidence {
  std::string statement_kind;
  std::size_t token_count{0};
  std::size_t source_span_count{0};
  ProceduralFunctionalEncodingSpanMetadata procedural_span_metadata;
  ProceduralSourceRetentionMetadata procedural_source_retention_metadata;
  std::size_t clause_count{0};
  std::size_t parameter_count{0};
  std::size_t object_reference_count{0};
  std::size_t function_reference_count{0};
  std::size_t datatype_reference_count{0};
  std::size_t catalog_reference_count{0};
  std::string datatype_profile_evidence_json;
  std::string index_semantic_defaults_upper_sql;
  std::string constraint_semantic_defaults_upper_sql;
  std::string sequence_identity_semantic_upper_sql;
  std::string identifier_name_resolution_upper_sql;
  std::string scalar_expression_semantic_upper_sql;
  std::string dml_mutation_semantic_upper_sql;
  std::string transaction_session_semantic_upper_sql;
  std::string temporary_session_object_semantic_upper_sql;
  std::string dependency_bearing_ddl_semantic_upper_sql;
  std::string ddl_transaction_behavior_semantic_upper_sql;
  std::string resource_text_semantic_upper_sql;
  std::string statistics_optimizer_semantic_upper_sql;
  std::string locks_isolation_semantic_upper_sql;
  std::string system_catalog_defaults_semantic_operation_id;
  std::string session_settings_diagnostics_semantic_upper_sql;
  bool cst_materialized{false};
  bool ast_materialized{false};
  bool bound_ast_materialized{false};
  bool source_text_redacted{true};
  bool descriptor_uuid_required{true};
  bool parser_has_transaction_finality{false};
  bool parser_has_storage_authority{false};
  bool parser_has_sequence_value_authority{false};
  bool datatype_descriptor_evidence_required{false};
  bool index_semantic_defaults_evidence_required{false};
  bool constraint_semantic_defaults_evidence_required{false};
  bool sequence_identity_semantic_evidence_required{false};
  bool identifier_name_resolution_evidence_required{false};
  bool scalar_expression_semantic_evidence_required{false};
  bool dml_mutation_semantic_evidence_required{false};
  bool transaction_session_semantic_evidence_required{false};
  bool temporary_session_object_semantic_evidence_required{false};
  bool dependency_bearing_ddl_semantic_evidence_required{false};
  bool ddl_transaction_behavior_semantic_evidence_required{false};
  bool resource_text_semantic_evidence_required{false};
  bool statistics_optimizer_semantic_evidence_required{false};
  bool locks_isolation_semantic_evidence_required{false};
  bool system_catalog_defaults_semantic_evidence_required{false};
  bool session_settings_diagnostics_semantic_evidence_required{false};
  bool procedural_body_source_retention_required{false};
};

bool IsKeywordToken(const Token& token) {
  return token.kind == "identifier_or_keyword";
}

bool IsNoiseToken(const Token& token) {
  return token.kind == "line_comment" || token.kind == "block_comment";
}

std::string TokenUpper(const Token& token) {
  return ToUpperAscii(token.lexeme);
}

bool TokenEquals(const Token& token, std::string_view value) {
  return IsKeywordToken(token) && TokenUpper(token) == value;
}

bool MayContainRelationLiteralQueryFlow(std::span<const Token> tokens) {
  for (const auto& token : tokens) {
    if (IsNoiseToken(token)) continue;
    return TokenEquals(token, "SELECT") || TokenEquals(token, "WITH") ||
           TokenEquals(token, "CREATE") || TokenEquals(token, "INSERT") ||
           TokenEquals(token, "DESCRIBE") || TokenEquals(token, "SUMMARIZE");
  }
  return false;
}

std::size_t NextSemanticToken(std::span<const Token> tokens,
                              std::size_t cursor) {
  while (cursor < tokens.size() && IsNoiseToken(tokens[cursor])) {
    ++cursor;
  }
  return cursor;
}

bool SqlStringLiteralBodyStartsWith(std::string_view literal,
                                    std::string_view upper_prefix) {
  if (literal.size() < 2 || literal.front() != '\'') return false;
  std::size_t matched = 0;
  for (std::size_t i = 1; i < literal.size() && matched < upper_prefix.size();) {
    char ch = literal[i];
    if (ch == '\'') {
      if (i + 1 < literal.size() && literal[i + 1] == '\'') {
        i += 2;
      } else {
        break;
      }
    } else if (ch == '\\' && i + 1 < literal.size()) {
      ch = literal[i + 1];
      i += 2;
    } else {
      ++i;
    }

    const char upper_ch =
        static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    const char expected_ch = static_cast<char>(
        std::toupper(static_cast<unsigned char>(upper_prefix[matched])));
    if (upper_ch != expected_ch) return false;
    ++matched;
  }
  return matched == upper_prefix.size();
}

bool StringLiteralMatchesAnyUriScheme(std::string_view literal,
                                      std::string_view upper_schemes) {
  std::size_t begin = 0;
  while (begin < upper_schemes.size()) {
    std::size_t end = upper_schemes.find("||", begin);
    if (end == std::string_view::npos) end = upper_schemes.size();
    const auto scheme =
        TrimAsciiView(upper_schemes.substr(begin, end - begin));
    if (!scheme.empty() && SqlStringLiteralBodyStartsWith(literal, scheme)) {
      return true;
    }
    if (end == upper_schemes.size()) break;
    begin = end + 2;
  }
  return false;
}

bool ContainsFromStringLiteralUriScheme(std::span<const Token> tokens,
                                        std::string_view upper_schemes) {
  if (!MayContainRelationLiteralQueryFlow(tokens)) return false;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (!TokenEquals(tokens[i], "FROM")) continue;
    const std::size_t literal_index = NextSemanticToken(tokens, i + 1);
    if (literal_index >= tokens.size()) continue;
    const auto& literal = tokens[literal_index];
    if (literal.kind == "string_literal" &&
        StringLiteralMatchesAnyUriScheme(literal.lexeme, upper_schemes)) {
      return true;
    }
  }
  return false;
}

bool IsRestMethodToken(const Token& token) {
  return TokenEquals(token, "GET") || TokenEquals(token, "POST") ||
         TokenEquals(token, "PUT") || TokenEquals(token, "DELETE") ||
         TokenEquals(token, "HEAD") || TokenEquals(token, "PATCH");
}

bool RestRequestPath(std::string_view upper,
                     std::span<const Token> tokens,
                     std::string_view* path) {
  const std::size_t method_index = NextSemanticToken(tokens, 0);
  if (method_index >= tokens.size() || !IsRestMethodToken(tokens[method_index])) {
    return false;
  }
  std::size_t cursor =
      tokens[method_index].offset + tokens[method_index].lexeme.size();
  if (cursor >= upper.size() ||
      std::isspace(static_cast<unsigned char>(upper[cursor])) == 0) {
    return false;
  }
  cursor = SkipAsciiWhitespace(upper, cursor);
  if (cursor >= upper.size() || upper[cursor] != '/') return false;
  const std::size_t path_begin = cursor;
  while (cursor < upper.size() &&
         std::isspace(static_cast<unsigned char>(upper[cursor])) == 0) {
    ++cursor;
  }
  *path = upper.substr(path_begin, cursor - path_begin);
  return !path->empty();
}

std::vector<std::string_view> SplitPathSegments(std::string_view path) {
  std::vector<std::string_view> segments;
  for (std::size_t i = 0; i < path.size();) {
    if (path[i] == '/') {
      ++i;
      continue;
    }
    if (path[i] == '?' || path[i] == '#') break;
    const std::size_t begin = i;
    while (i < path.size() && path[i] != '/' && path[i] != '?' &&
           path[i] != '#') {
      ++i;
    }
    if (i > begin) segments.push_back(path.substr(begin, i - begin));
    if (i < path.size() && (path[i] == '?' || path[i] == '#')) break;
  }
  return segments;
}

bool MatchesRestPathSegment(std::string_view upper,
                            std::span<const Token> tokens,
                            std::string_view pattern) {
  std::string_view path;
  if (!RestRequestPath(upper, tokens, &path)) return false;
  auto path_segments = SplitPathSegments(path);
  auto pattern_segments = SplitPathSegments(pattern);
  if (pattern_segments.empty()) {
    pattern_segments.push_back(TrimAsciiView(pattern));
  }
  if (pattern_segments.empty() || pattern_segments.front().empty() ||
      path_segments.size() < pattern_segments.size()) {
    return false;
  }
  for (std::size_t begin = 0; begin < path_segments.size(); ++begin) {
    if (begin + pattern_segments.size() > path_segments.size()) break;
    bool matched = true;
    for (std::size_t offset = 0; offset < pattern_segments.size(); ++offset) {
      if (path_segments[begin + offset] != pattern_segments[offset]) {
        matched = false;
        break;
      }
    }
    if (matched) return true;
  }
  return false;
}

bool MatchesRestMethodRoute(std::string_view upper,
                            std::span<const Token> tokens,
                            std::string_view pattern) {
  const auto trimmed = TrimAsciiView(pattern);
  const std::size_t alternative = trimmed.find("||");
  if (alternative != std::string_view::npos) {
    std::size_t begin = 0;
    while (begin < trimmed.size()) {
      std::size_t end = trimmed.find("||", begin);
      if (end == std::string_view::npos) end = trimmed.size();
      if (MatchesRestMethodRoute(upper, tokens,
                                 trimmed.substr(begin, end - begin))) {
        return true;
      }
      if (end == trimmed.size()) break;
      begin = end + 2;
    }
    return false;
  }
  const std::size_t method_end = trimmed.find(' ');
  const auto method =
      method_end == std::string_view::npos ? trimmed
                                           : trimmed.substr(0, method_end);
  const std::size_t method_index = NextSemanticToken(tokens, 0);
  if (method_index >= tokens.size() || !TokenEquals(tokens[method_index], method)) {
    return false;
  }
  std::string_view path;
  if (!RestRequestPath(upper, tokens, &path)) return false;
  if (method_end == std::string_view::npos) return true;
  const auto path_prefix = TrimAsciiView(trimmed.substr(method_end));
  return path_prefix.empty() || StartsWith(path, path_prefix);
}

bool MatchesPplPipelineStage(std::span<const Token> tokens,
                             std::string_view pattern) {
  const auto keyword = ToUpperAscii(TrimAsciiView(pattern));
  if (keyword.empty()) return false;
  const std::size_t first = NextSemanticToken(tokens, 0);
  if (first < tokens.size() && TokenEquals(tokens[first], keyword)) return true;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].kind != "symbol" || tokens[i].lexeme != "|") continue;
    const std::size_t stage = NextSemanticToken(tokens, i + 1);
    if (stage < tokens.size() && TokenEquals(tokens[stage], keyword)) {
      return true;
    }
  }
  return false;
}

bool IsClauseKeyword(std::string_view upper) {
  return upper == "SELECT" || upper == "WITH" || upper == "FROM" ||
         upper == "WHERE" || upper == "GROUP" || upper == "HAVING" ||
         upper == "ORDER" || upper == "LIMIT" || upper == "OFFSET" ||
         upper == "RETURNING" || upper == "VALUES" || upper == "SET" ||
         upper == "JOIN" || upper == "ON" || upper == "USING" ||
         upper == "INTO" || upper == "TABLE" || upper == "DATABASE" ||
         upper == "INDEX" || upper == "VIEW" || upper == "PROCEDURE" ||
         upper == "FUNCTION" || upper == "TRIGGER" || upper == "ROLE" ||
         upper == "USER" || upper == "TABLESPACE";
}

bool IntroducesObjectReference(std::string_view upper) {
  return upper == "FROM" || upper == "JOIN" || upper == "UPDATE" ||
         upper == "INTO" || upper == "TABLE" || upper == "DATABASE" ||
         upper == "INDEX" || upper == "VIEW" || upper == "ON" ||
         upper == "PROCEDURE" || upper == "FUNCTION" ||
         upper == "TRIGGER" || upper == "ROLE" || upper == "USER" ||
         upper == "TABLESPACE" || upper == "EXTENSION" ||
         upper == "SERVER" || upper == "POLICY";
}

bool IsBuiltinSqlKeyword(std::string_view upper) {
  return upper == "SELECT" || upper == "WITH" || upper == "FROM" ||
         upper == "WHERE" || upper == "GROUP" || upper == "BY" ||
         upper == "HAVING" || upper == "ORDER" || upper == "LIMIT" ||
         upper == "OFFSET" || upper == "INSERT" || upper == "UPDATE" ||
         upper == "DELETE" || upper == "CREATE" || upper == "ALTER" ||
         upper == "DROP" || upper == "TABLE" || upper == "INDEX" ||
         upper == "VIEW" || upper == "DATABASE" || upper == "VALUES" ||
         upper == "INTO" || upper == "SET" || upper == "AND" ||
         upper == "OR" || upper == "NOT" || upper == "NULL" ||
         upper == "TRUE" || upper == "FALSE" || upper == "CASE" ||
         upper == "WHEN" || upper == "THEN" || upper == "ELSE" ||
         upper == "END" || upper == "AS" || upper == "ON" ||
         upper == "JOIN" || upper == "LEFT" || upper == "RIGHT" ||
         upper == "INNER" || upper == "OUTER" || upper == "FULL";
}

bool IsDdlStatementSubtype(std::string_view upper) {
  return upper == "TABLE" || upper == "TEMP" || upper == "VIRTUAL" ||
         upper == "INDEX" || upper == "DATABASE" || upper == "USER" ||
         upper == "ROLE" || upper == "EVENT" || upper == "TRIGGER" ||
         upper == "PROCEDURE" || upper == "FUNCTION" || upper == "POLICY" ||
         upper == "RULE" || upper == "EXTENSION" || upper == "FOREIGN" ||
         upper == "SERVER" || upper == "PUBLICATION" ||
         upper == "SUBSCRIPTION" || upper == "TABLESPACE" ||
         upper == "DOMAIN" || upper == "SEQUENCE" ||
         upper == "DICTIONARY" || upper == "SECRET" ||
         upper == "PLACEMENT" || upper == "RESOURCE" ||
         upper == "CHANGEFEED" || upper == "VSCHEMA" ||
         upper == "CDC" || upper == "RETENTION" ||
         upper == "CONSTRAINT" || upper == "KEYSPACE" ||
         upper == "COLLECTION" || upper == "CACHE";
}

bool SurfaceMentions(std::string_view upper_sql,
                     std::span<const SurfaceDescriptor> surfaces) {
  for (const auto& surface : surfaces) {
    std::size_t begin = 0;
    while (begin < surface.surface.size()) {
      std::size_t end = surface.surface.find(';', begin);
      if (end == std::string_view::npos) end = surface.surface.size();
      const auto raw = TrimAsciiView(surface.surface.substr(begin, end - begin));
      const auto token = ToUpperAscii(raw);
      if (!token.empty() && token != "ST_*" &&
          token != "JSONB_*" && token != "JSON_*" &&
          token != "PG_*_IS_VISIBLE" &&
          (ContainsWord(upper_sql, token) || Contains(upper_sql, token))) {
        return true;
      }
      if (end == surface.surface.size()) break;
      begin = end + 1;
    }
  }
  return false;
}

bool IsEvidenceWordToken(const Token& token) {
  return token.kind == "identifier_or_keyword";
}

std::string EvidenceTokenUpper(const Token& token) {
  return ToUpperAscii(token.lexeme);
}

bool IsEvidenceSymbolToken(const Token& token, std::string_view symbol) {
  return (token.kind == "symbol" || token.kind == "punctuation") &&
         token.lexeme == symbol;
}

struct DatatypeFamilyProfile {
  bool numeric{false};
  bool exact_decimal{false};
  bool floating{false};
  bool text{false};
  bool charset_collation_sensitive_text{false};
  bool binary_blob{false};
  bool temporal{false};
  bool boolean{false};
  bool json_document{false};
  bool uuid{false};
  bool array{false};
  bool enum_set{false};
  bool network{false};
  bool geometric_spatial{false};
  bool range_domain_composite{false};
};

bool DetectTokenFamily(std::string_view dialect_id,
                       std::string_view upper,
                       DatatypeFamilyProfile* profile) {
  bool detected = false;
  const auto mark_numeric = [&] {
    profile->numeric = true;
    detected = true;
  };
  const auto mark_exact_decimal = [&] {
    profile->numeric = true;
    profile->exact_decimal = true;
    detected = true;
  };
  const auto mark_floating = [&] {
    profile->numeric = true;
    profile->floating = true;
    detected = true;
  };

  if (upper == "TINYINT" || upper == "SMALLINT" ||
      upper == "MEDIUMINT" || upper == "INT" ||
      upper == "INTEGER" || upper == "BIGINT" ||
      upper == "INT128" || upper == "SERIAL" ||
      upper == "SMALLSERIAL" || upper == "BIGSERIAL" ||
      upper == "MONEY") {
    mark_numeric();
  } else if (upper == "NUMERIC" || upper == "DECIMAL") {
    mark_exact_decimal();
  } else if (upper == "REAL" || upper == "FLOAT" ||
             upper == "DOUBLE" || upper == "DECFLOAT") {
    mark_floating();
  } else if (upper == "CHAR" || upper == "VARCHAR" ||
             upper == "CHARACTER" || upper == "NCHAR" ||
             upper == "TEXT" || upper == "TINYTEXT" ||
             upper == "MEDIUMTEXT" || upper == "LONGTEXT" ||
             upper == "NAME") {
    profile->text = true;
    detected = true;
  } else if (upper == "CHARSET" || upper == "COLLATE" ||
             upper == "COLLATION") {
    profile->charset_collation_sensitive_text = true;
    detected = true;
  } else if (upper == "BINARY" || upper == "VARBINARY" ||
             upper == "BLOB" || upper == "TINYBLOB" ||
             upper == "MEDIUMBLOB" || upper == "LONGBLOB" ||
             upper == "BYTEA") {
    profile->binary_blob = true;
    detected = true;
  } else if (upper == "DATE" || upper == "TIME" ||
             upper == "TIMETZ" || upper == "TIMESTAMP" ||
             upper == "TIMESTAMPTZ" || upper == "DATETIME" ||
             upper == "YEAR" || upper == "INTERVAL") {
    profile->temporal = true;
    detected = true;
  } else if (upper == "BOOL" || upper == "BOOLEAN") {
    profile->boolean = true;
    detected = true;
  } else if (upper == "JSON" || upper == "JSONB" ||
             upper == "JSONPATH") {
    profile->json_document = true;
    detected = true;
  } else if (upper == "UUID") {
    profile->uuid = true;
    detected = true;
  } else if (upper == "ARRAY") {
    profile->array = true;
    detected = true;
  } else if (upper == "ENUM") {
    profile->enum_set = true;
    detected = true;
  } else if (upper == "CIDR" || upper == "INET" ||
             upper == "MACADDR" || upper == "MACADDR8") {
    profile->network = true;
    detected = true;
  } else if (upper == "GEOMETRY" || upper == "POINT" ||
             upper == "LINESTRING" || upper == "POLYGON" ||
             upper == "LINE" || upper == "LSEG" ||
             upper == "BOX" || upper == "PATH" ||
             upper == "CIRCLE") {
    profile->geometric_spatial = true;
    detected = true;
  } else if (upper == "DOMAIN" || upper == "COMPOSITE" ||
             upper == "INT4RANGE" || upper == "INT8RANGE" ||
             upper == "NUMRANGE" || upper == "DATERANGE" ||
             upper == "TSRANGE" || upper == "TSTZRANGE" ||
             upper == "INT4MULTIRANGE" || upper == "INT8MULTIRANGE" ||
             upper == "NUMMULTIRANGE" || upper == "DATEMULTIRANGE" ||
             upper == "TSMULTIRANGE" || upper == "TSTZMULTIRANGE") {
    profile->range_domain_composite = true;
    detected = true;
  }
  return detected;
}

std::size_t DetectedFamilyCount(const DatatypeFamilyProfile& profile) {
  return static_cast<std::size_t>(profile.numeric) +
         static_cast<std::size_t>(profile.exact_decimal) +
         static_cast<std::size_t>(profile.floating) +
         static_cast<std::size_t>(profile.text) +
         static_cast<std::size_t>(profile.charset_collation_sensitive_text) +
         static_cast<std::size_t>(profile.binary_blob) +
         static_cast<std::size_t>(profile.temporal) +
         static_cast<std::size_t>(profile.boolean) +
         static_cast<std::size_t>(profile.json_document) +
         static_cast<std::size_t>(profile.uuid) +
         static_cast<std::size_t>(profile.array) +
         static_cast<std::size_t>(profile.enum_set) +
         static_cast<std::size_t>(profile.network) +
         static_cast<std::size_t>(profile.geometric_spatial) +
         static_cast<std::size_t>(profile.range_domain_composite);
}

std::string DetectedFamilyList(const DatatypeFamilyProfile& profile) {
  std::string families;
  const auto append = [&](bool present, std::string_view family) {
    if (!present) return;
    if (!families.empty()) families.push_back(',');
    families.append(family);
  };
  append(profile.numeric, "numeric");
  append(profile.exact_decimal, "exact_decimal");
  append(profile.floating, "floating");
  append(profile.text, "text");
  append(profile.charset_collation_sensitive_text,
         "charset_collation_sensitive_text");
  append(profile.binary_blob, "binary_blob");
  append(profile.temporal, "temporal");
  append(profile.boolean, "boolean");
  append(profile.json_document, "json_document");
  append(profile.uuid, "uuid");
  append(profile.array, "array");
  append(profile.enum_set, "enum_set");
  append(profile.network, "network");
  append(profile.geometric_spatial, "geometric_spatial");
  append(profile.range_domain_composite, "range_domain_composite");
  return families;
}

std::string StatementKindFromTokens(const std::vector<Token>& tokens) {
  std::vector<std::string> keywords;
  for (const auto& token : tokens) {
    if (IsNoiseToken(token)) continue;
    if (IsKeywordToken(token)) keywords.push_back(TokenUpper(token));
    if (keywords.size() >= 3) break;
  }
  if (keywords.empty()) return "unknown";
  if (keywords.size() >= 2) {
    if (keywords[0] == "CREATE" || keywords[0] == "ALTER" ||
        keywords[0] == "DROP") {
      if (keywords[0] == "CREATE" && IsCreateIndexKeywordSequence(tokens)) {
        return "CREATE_INDEX";
      }
      if (IsDdlStatementSubtype(keywords[1])) {
        return keywords[0] + "_" + keywords[1];
      }
      return keywords[0];
    }
    if (keywords[0] == "LOAD" && keywords[1] == "DATA") return "LOAD_DATA";
    if (keywords[0] == "START" && keywords[1] == "TRANSACTION") {
      return "START_TRANSACTION";
    }
    if (keywords[0] == "COPY" && keywords.size() >= 3) return "COPY";
  }
  return keywords[0];
}

ParserEvidence BuildParserEvidence(std::string_view upper,
                                   const std::vector<Token>& tokens,
                                   const DialectProfile& profile) {
  ParserEvidence evidence;
  evidence.statement_kind = StatementKindFromTokens(tokens);
  evidence.token_count = tokens.size();
  evidence.source_span_count = tokens.empty() ? 0 : tokens.size();
  evidence.cst_materialized = !tokens.empty();
  evidence.ast_materialized = evidence.cst_materialized &&
                              evidence.statement_kind != "unknown";
  evidence.bound_ast_materialized = evidence.ast_materialized;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const auto& token = tokens[i];
    if (IsNoiseToken(token)) continue;
    if (token.kind == "parameter_or_variable") {
      ++evidence.parameter_count;
      continue;
    }
    if (!IsKeywordToken(token)) continue;
    const auto upper_token = TokenUpper(token);
    if (IsClauseKeyword(upper_token)) ++evidence.clause_count;
    if (IntroducesObjectReference(upper_token) && i + 1 < tokens.size()) {
      for (std::size_t j = i + 1; j < tokens.size(); ++j) {
        if (IsNoiseToken(tokens[j])) continue;
        if (tokens[j].kind == "identifier_or_keyword" ||
            tokens[j].kind == "quoted_identifier" ||
            tokens[j].kind == "string_literal") {
          ++evidence.object_reference_count;
        }
        break;
      }
    }
    if (i + 1 < tokens.size() && tokens[i + 1].kind == "symbol" &&
        tokens[i + 1].lexeme == "(" && !IsBuiltinSqlKeyword(upper_token)) {
      ++evidence.function_reference_count;
    }
  }
  evidence.datatype_reference_count =
      SurfaceMentions(upper, profile.datatype_surfaces) ? 1 : 0;
  evidence.datatype_profile_evidence_json =
      DatatypeProfileEvidenceJson(profile.dialect_id, tokens);
  evidence.catalog_reference_count =
      SurfaceMentions(upper, profile.catalog_overlay_surfaces) ? 1 : 0;
  return evidence;
}

std::string ParserEvidenceJson(const DialectProfile& profile,
                               const ParserEvidence& evidence) {
  std::ostringstream out;
  out << "{\"dialect\":\"" << EscapeJson(profile.dialect_id)
      << "\",\"statement_kind\":\"" << EscapeJson(evidence.statement_kind)
      << "\",\"cst_materialized\":" << BoolJson(evidence.cst_materialized)
      << ",\"ast_materialized\":" << BoolJson(evidence.ast_materialized)
      << ",\"bound_ast_materialized\":"
      << BoolJson(evidence.bound_ast_materialized)
      << ",\"token_count\":" << evidence.token_count
      << ",\"source_span_count\":" << evidence.source_span_count
      << ",\"clause_count\":" << evidence.clause_count
      << ",\"parameter_count\":" << evidence.parameter_count
      << ",\"object_reference_count\":" << evidence.object_reference_count
      << ",\"function_reference_count\":" << evidence.function_reference_count
      << ",\"datatype_reference_count\":"
      << evidence.datatype_reference_count
      << ",\"catalog_reference_count\":"
      << evidence.catalog_reference_count
      << ",\"source_text_redacted\":"
      << BoolJson(evidence.source_text_redacted)
      << ",\"descriptor_uuid_required\":"
      << BoolJson(evidence.descriptor_uuid_required)
      << ",\"parser_transaction_finality_authority\":"
      << BoolJson(evidence.parser_has_transaction_finality)
      << ",\"parser_storage_authority\":"
      << BoolJson(evidence.parser_has_storage_authority)
      << ",\"parser_sequence_value_authority\":"
      << BoolJson(evidence.parser_has_sequence_value_authority);
  if (evidence.datatype_descriptor_evidence_required) {
    out << ",\"datatype_descriptor_evidence\":"
        << DatatypeDescriptorEvidenceJson(evidence.datatype_reference_count);
    if (!evidence.datatype_profile_evidence_json.empty()) {
      out << ",\"datatype_profile_evidence\":"
          << evidence.datatype_profile_evidence_json;
    }
  }
  if (evidence.index_semantic_defaults_evidence_required) {
    out << ",\"index_semantic_defaults_evidence\":"
        << IndexSemanticDefaultsEvidenceJson(profile.dialect_id,
                                             profile.release_profile,
                                             evidence.index_semantic_defaults_upper_sql);
  }
  if (evidence.constraint_semantic_defaults_evidence_required) {
    out << ",\"constraint_semantic_defaults_evidence\":"
        << ConstraintSemanticDefaultsEvidenceJson(
               profile.dialect_id, profile.release_profile,
               evidence.constraint_semantic_defaults_upper_sql);
  }
  if (evidence.sequence_identity_semantic_evidence_required) {
    out << ",\"sequence_identity_semantic_evidence\":"
        << SequenceIdentitySemanticEvidenceJson(
               profile.dialect_id, profile.release_profile,
               evidence.sequence_identity_semantic_upper_sql);
  }
  if (evidence.identifier_name_resolution_evidence_required) {
    out << ",\"identifier_name_resolution_evidence\":"
        << IdentifierNameResolutionEvidenceJson(
               profile.dialect_id, profile.release_profile,
               evidence.identifier_name_resolution_upper_sql);
  }
  if (evidence.scalar_expression_semantic_evidence_required) {
    out << ",\"scalar_expression_semantic_evidence\":"
        << ScalarExpressionSemanticEvidenceJson(
               profile.dialect_id, profile.release_profile,
               evidence.scalar_expression_semantic_upper_sql);
  }
  if (evidence.dml_mutation_semantic_evidence_required) {
    out << ",\"dml_mutation_semantic_evidence\":"
        << DmlMutationSemanticEvidenceJson(
               profile.dialect_id, profile.release_profile,
               evidence.dml_mutation_semantic_upper_sql);
  }
  if (evidence.transaction_session_semantic_evidence_required) {
    out << ",\"transaction_session_semantic_evidence\":"
        << TransactionSessionSemanticEvidenceJson(
               profile.dialect_id, profile.release_profile,
               evidence.transaction_session_semantic_upper_sql);
  }
  if (evidence.temporary_session_object_semantic_evidence_required) {
    out << ",\"temporary_session_object_semantic_evidence\":"
        << TemporarySessionObjectSemanticEvidenceJson(
               profile.dialect_id, profile.release_profile,
               evidence.temporary_session_object_semantic_upper_sql);
  }
  if (evidence.dependency_bearing_ddl_semantic_evidence_required) {
    out << ",\"dependency_bearing_ddl_semantic_evidence\":"
        << DependencyBearingDdlSemanticEvidenceJson(
               profile.dialect_id, profile.release_profile,
               evidence.dependency_bearing_ddl_semantic_upper_sql);
  }
  if (evidence.ddl_transaction_behavior_semantic_evidence_required) {
    out << ",\"ddl_transaction_behavior_semantic_evidence\":"
        << DdlTransactionBehaviorSemanticEvidenceJson(
               profile.dialect_id, profile.release_profile,
               evidence.ddl_transaction_behavior_semantic_upper_sql);
  }
  if (evidence.resource_text_semantic_evidence_required) {
    out << ",\"resource_text_semantic_evidence\":"
        << ResourceTextSemanticEvidenceJson(
               profile.dialect_id, profile.release_profile,
               evidence.resource_text_semantic_upper_sql);
  }
  if (evidence.statistics_optimizer_semantic_evidence_required) {
    out << ",\"statistics_optimizer_semantic_evidence\":"
        << StatisticsOptimizerSemanticEvidenceJson(
               profile.dialect_id, profile.release_profile,
               evidence.statistics_optimizer_semantic_upper_sql);
  }
  if (evidence.locks_isolation_semantic_evidence_required) {
    out << ",\"locks_isolation_semantic_evidence\":"
        << LocksIsolationSemanticEvidenceJson(
               profile.dialect_id, profile.release_profile,
               evidence.locks_isolation_semantic_upper_sql);
  }
  if (evidence.system_catalog_defaults_semantic_evidence_required) {
    out << ",\"system_catalog_defaults_semantic_evidence\":"
        << SystemCatalogDefaultsSemanticEvidenceJson(
               profile.dialect_id,
               evidence.system_catalog_defaults_semantic_operation_id,
               profile.catalog_overlay_surfaces);
  }
  if (evidence.session_settings_diagnostics_semantic_evidence_required) {
    out << ",\"session_settings_diagnostics_semantic_evidence\":"
        << SessionSettingsDiagnosticsSemanticEvidenceJson(
               profile.dialect_id, profile.release_profile,
               evidence.session_settings_diagnostics_semantic_upper_sql);
  }
  if (evidence.procedural_body_source_retention_required) {
    out << ",\"procedural_body_source_retention_evidence\":"
        << ProceduralBodySourceRetentionEvidenceJson(
               evidence.procedural_source_retention_metadata)
        << ",\"procedural_functional_encoding_source_span_uuid_binding_evidence\":"
        << ProceduralFunctionalEncodingEvidenceJson(
               evidence.source_span_count, evidence.cst_materialized,
               evidence.ast_materialized, evidence.bound_ast_materialized,
               evidence.procedural_span_metadata);
  }
  out << ",\"enterprise_readiness_evidence\":"
      << EnterpriseReadinessEvidenceJson();
  out << "}";
  return out.str();
}

bool Matches(std::string_view upper,
             std::span<const Token> tokens,
             const OperationPattern& pattern) {
  switch (pattern.match_kind) {
    case PatternMatch::kPrefix:
      return StartsWithCommand(upper, pattern.match);
    case PatternMatch::kContains:
      return Contains(upper, pattern.match);
    case PatternMatch::kPrefixAndContains: {
      const auto delimiter = pattern.match.find("||");
      if (delimiter == std::string_view::npos) return false;
      return StartsWithCommand(upper, pattern.match.substr(0, delimiter)) &&
             ContainsDelimitedFragment(
                 upper, pattern.match.substr(delimiter + 2));
    }
    case PatternMatch::kContainsFunctionCall:
      return ContainsFunctionCall(upper, pattern.match);
    case PatternMatch::kLoadDataLocalInfile:
      return MatchesLoadDataInfileSyntax(upper, true);
    case PatternMatch::kLoadDataServerInfile:
      return MatchesLoadDataInfileSyntax(upper, false);
    case PatternMatch::kCreateTableEngineClause:
      return ContainsCreateTableEngineClause(upper, pattern.match);
    case PatternMatch::kFromStringLiteralUriScheme:
      return ContainsFromStringLiteralUriScheme(tokens, pattern.match);
    case PatternMatch::kRestPathSegment:
      return MatchesRestPathSegment(upper, tokens, pattern.match);
    case PatternMatch::kRestMethodRoute:
      return MatchesRestMethodRoute(upper, tokens, pattern.match);
    case PatternMatch::kPplPipelineStage:
      return MatchesPplPipelineStage(tokens, pattern.match);
    case PatternMatch::kWord:
      return ContainsWord(upper, pattern.match);
    case PatternMatch::kRelationReference:
      return ContainsRelationReference(upper, pattern.match);
  }
  return false;
}

std::string_view TrimAsciiView(std::string_view text) {
  std::size_t begin = 0;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return text.substr(begin, end - begin);
}

Diagnostic MakeDiagnostic(std::string code,
                          std::string severity,
                          std::string message,
                          std::string component,
                          std::vector<Field> fields = {}) {
  return {std::move(code), std::move(severity), std::move(message),
          std::move(component), std::move(fields)};
}

std::string BoolJson(bool value) {
  return value ? "true" : "false";
}

bool HasBalancedDelimiters(std::string_view text) {
  int paren_depth = 0;
  bool in_single_quote = false;
  bool in_double_quote = false;
  bool in_backtick = false;
  bool in_line_comment = false;
  bool in_block_comment = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    const char next = i + 1 < text.size() ? text[i + 1] : '\0';
    if (in_line_comment) {
      if (ch == '\n') in_line_comment = false;
      continue;
    }
    if (in_block_comment) {
      if (ch == '*' && next == '/') {
        in_block_comment = false;
        ++i;
      }
      continue;
    }
    if (in_single_quote) {
      if (ch == '\'' && next == '\'') {
        ++i;
        continue;
      }
      if (ch == '\\' && next != '\0') {
        ++i;
        continue;
      }
      if (ch == '\'') in_single_quote = false;
      continue;
    }
    if (in_double_quote) {
      if (ch == '"' && next == '"') {
        ++i;
        continue;
      }
      if (ch == '"') in_double_quote = false;
      continue;
    }
    if (in_backtick) {
      if (ch == '`' && next == '`') {
        ++i;
        continue;
      }
      if (ch == '`') in_backtick = false;
      continue;
    }
    if (ch == '-' && next == '-') {
      in_line_comment = true;
      ++i;
      continue;
    }
    if (ch == '#') {
      in_line_comment = true;
      continue;
    }
    if (ch == '/' && next == '*') {
      in_block_comment = true;
      ++i;
      continue;
    }
    if (ch == '\'') {
      in_single_quote = true;
      continue;
    }
    if (ch == '"') {
      in_double_quote = true;
      continue;
    }
    if (ch == '`') {
      in_backtick = true;
      continue;
    }
    if (ch == '(') {
      ++paren_depth;
      continue;
    }
    if (ch == ')') {
      --paren_depth;
      if (paren_depth < 0) return false;
    }
  }
  return paren_depth == 0 && !in_single_quote && !in_double_quote &&
         !in_backtick && !in_block_comment;
}

bool IsCatalogMutation(std::string_view upper, const DialectProfile& profile) {
  if (!(StartsWithCommand(upper, "INSERT") ||
        StartsWithCommand(upper, "UPDATE") ||
        StartsWithCommand(upper, "DELETE") ||
        StartsWithCommand(upper, "MERGE") ||
        StartsWithCommand(upper, "TRUNCATE"))) {
    return false;
  }
  return SurfaceMentions(upper, profile.catalog_overlay_surfaces);
}

std::string MakeSblrEnvelope(const DialectProfile& profile,
                             const OperationPattern& pattern,
                             const ParserEvidence& evidence) {
  const bool lifecycle_api =
      pattern.disposition == MappingDisposition::kScratchBirdLifecycleApi;
  const bool support_udr =
      pattern.disposition == MappingDisposition::kParserSupportUdr;
  const bool catalog_projection =
      pattern.disposition == MappingDisposition::kCatalogProjection;
  const bool fail_closed =
      pattern.disposition == MappingDisposition::kPolicyRefusal ||
      pattern.disposition == MappingDisposition::kSecurityRefusal ||
      pattern.disposition == MappingDisposition::kUnsupportedRefusal;
  return "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
         "\"dialect\":\"" + EscapeJson(profile.dialect_id) + "\","
         "\"statement_family\":\"" + EscapeJson(pattern.statement_family) + "\","
         "\"operation_family\":\"" + EscapeJson(pattern.operation_family) + "\","
         "\"operation_id\":\"" + EscapeJson(pattern.mapping_key) + "\","
         "\"sblr_operation\":\"" + EscapeJson(pattern.sblr_operation) + "\","
         "\"sblr_operation_family\":\"" +
         EscapeJson(profile.sblr_operation_family) + "\","
         "\"engine_api_function\":\"" +
         EscapeJson(pattern.engine_api_function) + "\","
         "\"mapping_key\":\"" + EscapeJson(pattern.mapping_key) + "\","
         "\"mapping_disposition\":\"" +
         EscapeJson(MappingDispositionName(pattern.disposition)) + "\","
         "\"parser_evidence\":" + ParserEvidenceJson(profile, evidence) + ","
         "\"enterprise_readiness_evidence\":" +
         EnterpriseReadinessEvidenceJson() + ","
         "\"descriptor_resolution\":\"uuid_required\","
         "\"engine_authority\":\"scratchbird\","
         "\"scratchbird_lifecycle_api\":" + BoolJson(lifecycle_api) + ","
         "\"parser_support_udr_route\":" + BoolJson(support_udr) + ","
         "\"catalog_projection_only\":" + BoolJson(catalog_projection) + ","
         "\"fail_closed_refusal\":" + BoolJson(fail_closed) + ","
         "\"real_donor_file_effects\":false,"
         "\"donor_engine_sql_executed\":false,"
         "\"full_declared_surface_assignment\":true,"
         "\"sql_text_included\":false}";
}

ParseResult Reject(const DialectProfile& profile,
                   std::string_view code,
                   std::string_view message,
                   std::vector<Field> fields = {}) {
  ParseResult result;
  result.ok = false;
  result.emulation_diagnostic_code = std::string(code);
  result.authority_disposition = "fail_closed";
  result.fail_closed_refusal = true;
  result.message_vector_json = MessageVectorToJson({
      MakeDiagnostic(std::string(code), "ERROR", std::string(message),
                     std::string(profile.parser_package_name) + ".parser",
                     std::move(fields)),
  });
  return result;
}

} // namespace

std::string DatatypeDescriptorEvidenceJson(std::size_t datatype_reference_count) {
  std::ostringstream out;
  out << "{\"evidence_contract\":\"donor_datatype_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"datatype_reference_count\":" << datatype_reference_count << ','
      << "\"datatype_surface_matched\":true,"
      << "\"catalog_descriptor_required\":true,"
      << "\"wire_literal_cast_comparison_required\":true,"
      << "\"collation_charset_profile_required\":true,"
      << "\"donor_datatype_profile_required\":true,"
      << "\"generic_text_fallback_allowed\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"donor_sql_executed\":false,"
      << "\"exactness_status\":\"descriptor_surface_recorded_exactness_proof_pending\","
      << "\"enterprise_readiness\":\"not_enterprise_ready\"}";
  return out.str();
}

std::string DatatypeProfileEvidenceJson(std::string_view dialect_id,
                                        std::span<const Token> active_tokens) {
  if (dialect_id != "firebird" && dialect_id != "mysql" &&
      dialect_id != "postgresql") {
    return {};
  }

  DatatypeFamilyProfile profile;
  for (std::size_t i = 0; i < active_tokens.size(); ++i) {
    const auto& token = active_tokens[i];
    if (dialect_id != "mysql" && IsEvidenceSymbolToken(token, "[")) {
      profile.array = true;
    }
    if (!IsEvidenceWordToken(token)) continue;
    const auto upper = EvidenceTokenUpper(token);
    DetectTokenFamily(dialect_id, upper, &profile);

    if (upper == "CHARACTER" && i + 1 < active_tokens.size() &&
        IsEvidenceWordToken(active_tokens[i + 1]) &&
        EvidenceTokenUpper(active_tokens[i + 1]) == "SET") {
      profile.charset_collation_sensitive_text = true;
    }
    if (dialect_id == "mysql" && upper == "SET" &&
        i + 1 < active_tokens.size() &&
        IsEvidenceSymbolToken(active_tokens[i + 1], "(")) {
      profile.enum_set = true;
    }
  }

  const std::size_t detected_count = DetectedFamilyCount(profile);
  if (detected_count == 0) return {};

  std::ostringstream out;
  out << "{\"evidence_contract\":\"donor_datatype_profile_family_detection.v1\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"donor_profile_uuid\":\"" << DonorProfileUuid(dialect_id) << "\","
      << "\"descriptor_authority\":\"scratchbird_engine_catalog\","
      << "\"numeric\":" << BoolJson(profile.numeric) << ','
      << "\"exact_decimal\":" << BoolJson(profile.exact_decimal) << ','
      << "\"floating\":" << BoolJson(profile.floating) << ','
      << "\"text\":" << BoolJson(profile.text) << ','
      << "\"charset_collation_sensitive_text\":"
      << BoolJson(profile.charset_collation_sensitive_text) << ','
      << "\"binary_blob\":" << BoolJson(profile.binary_blob) << ','
      << "\"temporal\":" << BoolJson(profile.temporal) << ','
      << "\"boolean\":" << BoolJson(profile.boolean) << ','
      << "\"json_document\":" << BoolJson(profile.json_document) << ','
      << "\"uuid\":" << BoolJson(profile.uuid) << ','
      << "\"array\":" << BoolJson(profile.array) << ','
      << "\"enum_set\":" << BoolJson(profile.enum_set) << ','
      << "\"network\":" << BoolJson(profile.network) << ','
      << "\"geometric_spatial\":"
      << BoolJson(profile.geometric_spatial) << ','
      << "\"range_domain_composite\":"
      << BoolJson(profile.range_domain_composite) << ','
      << "\"detected_family_count\":" << detected_count << ','
      << "\"detected_families\":\""
      << EscapeJson(DetectedFamilyList(profile)) << "\","
      << "\"source_text_included\":false,"
      << "\"generic_text_fallback_allowed\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"donor_sql_executed\":false,"
      << "\"exact_binary_wire_literal_cast_comparison_required\":true,"
      << "\"runtime_equivalence_status\":"
      << "\"pending_donor_native_exactness_replay\","
      << "\"enterprise_readiness\":\"not_enterprise_ready\"}";
  return out.str();
}

bool IsIndexSemanticDefaultsStatement(std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  if (StartsWithCommand(upper, "ALTER INDEX")) return true;
  if (StartsWithCommand(upper, "ALTER TABLE")) {
    return Contains(upper, " ALTER INDEX ") ||
           Contains(upper, " ADD INDEX ") ||
           Contains(upper, " ADD UNIQUE INDEX ") ||
           Contains(upper, " DROP INDEX ");
  }
  if (!StartsWithCommand(upper, "CREATE")) return false;
  const auto rest = ConsumeLeadingCommand(upper, "CREATE");
  return IsCreateIndexRest(rest);
}

std::string IndexSemanticDefaultsEvidenceJson(std::string_view dialect_id,
                                              std::string_view release_profile,
                                              std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool unique_requested = ContainsWord(upper, "UNIQUE");
  const bool predicate_present = ContainsWord(upper, "WHERE");
  const bool expression_key_present = Contains(upper, "((") ||
                                      Contains(upper, "COMPUTED BY");
  const bool concurrently_requested = ContainsWord(upper, "CONCURRENTLY");
  const bool descending_requested = ContainsWord(upper, "DESC") ||
                                    ContainsWord(upper, "DESCENDING");
  const bool nulls_not_distinct_requested =
      Contains(upper, "NULLS NOT DISTINCT");

  std::ostringstream out;
  out << "{\"evidence_contract\":\"donor_index_semantic_defaults_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"donor_profile_uuid\":\"" << DonorProfileUuid(dialect_id) << "\","
      << "\"semantic_profile_uuid\":\""
      << IndexSemanticProfileUuid(dialect_id) << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"index_profile\":\"" << IndexProfileName(dialect_id) << "\","
      << "\"ddl_surface\":\"" << IndexDdlSurface(upper) << "\","
      << "\"index_method\":\"" << IndexMethod(dialect_id, upper) << "\","
      << "\"unique_requested\":" << BoolJson(unique_requested) << ','
      << "\"unique_null_policy\":\""
      << UniqueNullPolicy(dialect_id, upper) << "\","
      << "\"null_ordering\":\"" << NullOrdering(dialect_id) << "\","
      << "\"collation_policy\":\"" << CollationPolicy(dialect_id) << "\","
      << "\"operator_family_policy\":\""
      << OperatorFamilyPolicy(dialect_id) << "\","
      << "\"predicate_or_expression_policy\":\""
      << PredicateOrExpressionPolicy(dialect_id, upper) << "\","
      << "\"predicate_present\":" << BoolJson(predicate_present) << ','
      << "\"expression_key_present\":"
      << BoolJson(expression_key_present) << ','
      << "\"concurrently_requested\":"
      << BoolJson(concurrently_requested) << ','
      << "\"descending_requested\":"
      << BoolJson(descending_requested) << ','
      << "\"nulls_not_distinct_requested\":"
      << BoolJson(nulls_not_distinct_requested) << ','
      << "\"validation_state\":\"" << ValidationState(dialect_id, upper)
      << "\","
      << "\"build_mode\":\"" << BuildMode(dialect_id, upper) << "\","
      << "\"statistics_policy_ref\":\""
      << StatisticsPolicyRef(dialect_id) << "\","
      << "\"catalog_descriptor_required\":true,"
      << "\"generic_index_default_allowed\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"donor_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"not_enterprise_proven_pending\","
      << "\"descriptor_exactness_status\":\"parser_descriptor_defaults_recorded_runtime_equivalence_pending\","
      << "\"enterprise_readiness\":\"not_enterprise_ready\"}";
  return out.str();
}

bool IsConstraintSemanticDefaultsStatement(std::string_view active_upper_sql) {
  auto rest = ConsumeLeadingCommand(active_upper_sql, "CREATE");
  if (rest.empty()) return false;
  for (const auto keyword : {"GLOBAL", "LOCAL", "TEMPORARY", "TEMP",
                             "UNLOGGED"}) {
    if (StartsWithCommand(rest, keyword)) {
      rest = TrimAsciiView(rest.substr(std::string_view(keyword).size()));
    }
  }
  if (StartsWithCommand(rest, "IF")) {
    rest = ConsumeLeadingCommand(rest, "IF");
    rest = ConsumeLeadingCommand(rest, "NOT");
    rest = ConsumeLeadingCommand(rest, "EXISTS");
  }
  if (!StartsWithCommand(rest, "TABLE")) return false;
  const auto upper = TrimAsciiView(active_upper_sql);
  return Contains(upper, "PRIMARY KEY") ||
         ContainsWord(upper, "UNIQUE") ||
         Contains(upper, "FOREIGN KEY") ||
         ContainsWord(upper, "REFERENCES") ||
         ContainsWord(upper, "CHECK") ||
         ContainsWord(upper, "DEFAULT") ||
         ContainsWord(upper, "GENERATED") ||
         ContainsWord(upper, "AUTO_INCREMENT") ||
         ContainsWord(upper, "SERIAL") ||
         ContainsWord(upper, "BIGSERIAL") ||
         ContainsWord(upper, "SMALLSERIAL");
}

std::string ConstraintSemanticDefaultsEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool primary_key_present = Contains(upper, "PRIMARY KEY");
  const bool unique_constraint_present = ContainsWord(upper, "UNIQUE");
  const bool foreign_key_reference_present =
      Contains(upper, "FOREIGN KEY") || ContainsWord(upper, "REFERENCES");
  const bool check_constraint_present = ContainsWord(upper, "CHECK");
  const bool default_clause_present = ContainsWord(upper, "DEFAULT");
  const bool generated_identity_or_autoincrement_present =
      ContainsWord(upper, "GENERATED") ||
      ContainsWord(upper, "AUTO_INCREMENT") ||
      ContainsWord(upper, "SERIAL") ||
      ContainsWord(upper, "BIGSERIAL") ||
      ContainsWord(upper, "SMALLSERIAL");
  const bool explicit_constraint_names_present = ContainsWord(upper, "CONSTRAINT");

  std::ostringstream out;
  out << "{\"evidence_contract\":\"donor_constraint_semantic_defaults_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"donor_profile_uuid\":\"" << DonorProfileUuid(dialect_id) << "\","
      << "\"semantic_profile_uuid\":\""
      << ConstraintSemanticProfileUuid(dialect_id) << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"constraint_profile\":\"" << ConstraintProfileName(dialect_id) << "\","
      << "\"ddl_surface\":\"create_table\","
      << "\"primary_key_present\":"
      << BoolJson(primary_key_present) << ','
      << "\"primary_key_behavior\":\"" << PrimaryKeyBehavior(dialect_id) << "\","
      << "\"unique_constraint_present\":"
      << BoolJson(unique_constraint_present) << ','
      << "\"unique_null_policy\":\""
      << ConstraintUniqueNullPolicy(dialect_id, upper) << "\","
      << "\"foreign_key_reference_present\":"
      << BoolJson(foreign_key_reference_present) << ','
      << "\"foreign_key_action_defaults\":\""
      << ForeignKeyActionDefaults(dialect_id) << "\","
      << "\"check_constraint_present\":"
      << BoolJson(check_constraint_present) << ','
      << "\"check_truth_table_null_behavior\":\""
      << CheckTruthTableNullBehavior(dialect_id) << "\","
      << "\"default_clause_present\":"
      << BoolJson(default_clause_present) << ','
      << "\"default_expression_policy\":\""
      << DefaultExpressionPolicy(dialect_id) << "\","
      << "\"generated_identity_or_autoincrement_present\":"
      << BoolJson(generated_identity_or_autoincrement_present) << ','
      << "\"generated_identity_autoincrement_policy\":\""
      << GeneratedIdentityAutoincrementPolicy(dialect_id, upper) << "\","
      << "\"explicit_constraint_names_present\":"
      << BoolJson(explicit_constraint_names_present) << ','
      << "\"generated_name_policy\":\""
      << GeneratedConstraintNamePolicy(dialect_id) << "\","
      << "\"deferrability_policy\":\"" << DeferrabilityPolicy(dialect_id, upper)
      << "\","
      << "\"enforcement_timing\":\"" << EnforcementTiming(dialect_id, upper)
      << "\","
      << "\"catalog_descriptor_required\":true,"
      << "\"generic_constraint_default_allowed\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"donor_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"not_enterprise_proven_pending\","
      << "\"descriptor_exactness_status\":\"parser_constraint_defaults_recorded_runtime_equivalence_pending\","
      << "\"enterprise_readiness\":\"not_enterprise_ready\"}";
  return out.str();
}

bool HasSequenceIdentitySemanticProfile(std::string_view dialect_id) {
  return dialect_id == "firebird" || dialect_id == "mysql" ||
         dialect_id == "postgresql";
}

bool IsSequenceIdentitySemanticStatement(std::string_view dialect_id,
                                         std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  if (!HasSequenceIdentitySemanticProfile(dialect_id)) return false;

  if (dialect_id == "firebird") {
    return StartsWithCommand(upper, "CREATE SEQUENCE") ||
           StartsWithCommand(upper, "CREATE GENERATOR") ||
           StartsWithCommand(upper, "ALTER SEQUENCE") ||
           StartsWithCommand(upper, "ALTER GENERATOR") ||
           StartsWithCommand(upper, "DROP SEQUENCE") ||
           StartsWithCommand(upper, "DROP GENERATOR") ||
           Contains(upper, "GEN_ID(") ||
           Contains(upper, "NEXT VALUE FOR") ||
           (ContainsWord(upper, "GENERATED") && ContainsWord(upper, "IDENTITY"));
  }

  if (dialect_id == "mysql") {
    return ContainsWord(upper, "AUTO_INCREMENT") ||
           Contains(upper, "LAST_INSERT_ID(");
  }

  if (dialect_id == "postgresql") {
    auto rest = ConsumeLeadingCommand(upper, "CREATE");
    if (!rest.empty()) {
      for (const auto keyword : {"GLOBAL", "LOCAL", "TEMPORARY", "TEMP",
                                 "UNLOGGED"}) {
        if (StartsWithCommand(rest, keyword)) {
          rest = TrimAsciiView(rest.substr(std::string_view(keyword).size()));
        }
      }
      if (StartsWithCommand(rest, "SEQUENCE")) return true;
    }
    return StartsWithCommand(upper, "ALTER SEQUENCE") ||
           StartsWithCommand(upper, "DROP SEQUENCE") ||
           Contains(upper, "NEXTVAL(") ||
           Contains(upper, "CURRVAL(") ||
           Contains(upper, "SETVAL(") ||
           ContainsWord(upper, "SERIAL") ||
           ContainsWord(upper, "BIGSERIAL") ||
           ContainsWord(upper, "SMALLSERIAL") ||
           (ContainsWord(upper, "GENERATED") && ContainsWord(upper, "IDENTITY"));
  }

  return false;
}

std::string SequenceIdentitySemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool create_sequence_or_generator_surface =
      StartsWithCommand(upper, "CREATE SEQUENCE") ||
      StartsWithCommand(upper, "CREATE GENERATOR");
  const bool alter_sequence_surface =
      StartsWithCommand(upper, "ALTER SEQUENCE") ||
      StartsWithCommand(upper, "ALTER GENERATOR");
  const bool auto_increment_surface = ContainsWord(upper, "AUTO_INCREMENT");
  const bool last_insert_id_surface = Contains(upper, "LAST_INSERT_ID(");
  const bool next_value_surface =
      Contains(upper, "GEN_ID(") || Contains(upper, "NEXT VALUE FOR") ||
      Contains(upper, "NEXTVAL(");
  const bool currval_surface = Contains(upper, "CURRVAL(");
  const bool setval_surface = Contains(upper, "SETVAL(");
  const bool sequence_backed_default_present =
      auto_increment_surface ||
      ContainsWord(upper, "SERIAL") ||
      ContainsWord(upper, "BIGSERIAL") ||
      ContainsWord(upper, "SMALLSERIAL") ||
      (ContainsWord(upper, "GENERATED") && ContainsWord(upper, "IDENTITY"));
  const bool restart_descriptor_present =
      ContainsWord(upper, "RESTART") || Contains(upper, "START WITH");
  const bool increment_descriptor_present =
      ContainsWord(upper, "INCREMENT") || auto_increment_surface;
  const bool min_value_descriptor_present =
      ContainsWord(upper, "MINVALUE") || Contains(upper, "NO MINVALUE");
  const bool max_value_descriptor_present =
      ContainsWord(upper, "MAXVALUE") || Contains(upper, "NO MAXVALUE");
  const bool cycle_descriptor_present =
      ContainsWord(upper, "CYCLE") || Contains(upper, "NO CYCLE");
  const bool cache_descriptor_present =
      ContainsWord(upper, "CACHE") || Contains(upper, "NO CACHE");
  const bool session_visible_state_surface =
      last_insert_id_surface || currval_surface || setval_surface;

  std::ostringstream out;
  out << "{\"evidence_contract\":\"donor_sequence_identity_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"donor_profile_uuid\":\"" << DonorProfileUuid(dialect_id) << "\","
      << "\"semantic_profile_uuid\":\""
      << SequenceIdentitySemanticProfileUuid(dialect_id) << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"sequence_identity_profile\":\""
      << SequenceIdentityProfileName(dialect_id) << "\","
      << "\"sequence_identity_surface\":\""
      << SequenceIdentitySurface(dialect_id, upper) << "\","
      << "\"create_sequence_or_generator_surface\":"
      << BoolJson(create_sequence_or_generator_surface) << ','
      << "\"alter_sequence_surface\":"
      << BoolJson(alter_sequence_surface) << ','
      << "\"auto_increment_surface\":"
      << BoolJson(auto_increment_surface) << ','
      << "\"last_insert_id_surface\":"
      << BoolJson(last_insert_id_surface) << ','
      << "\"next_value_surface\":"
      << BoolJson(next_value_surface) << ','
      << "\"currval_surface\":"
      << BoolJson(currval_surface) << ','
      << "\"setval_surface\":"
      << BoolJson(setval_surface) << ','
      << "\"sequence_backed_default_present\":"
      << BoolJson(sequence_backed_default_present) << ','
      << "\"restart_descriptor_present\":"
      << BoolJson(restart_descriptor_present) << ','
      << "\"increment_descriptor_present\":"
      << BoolJson(increment_descriptor_present) << ','
      << "\"min_value_descriptor_present\":"
      << BoolJson(min_value_descriptor_present) << ','
      << "\"max_value_descriptor_present\":"
      << BoolJson(max_value_descriptor_present) << ','
      << "\"cycle_descriptor_present\":"
      << BoolJson(cycle_descriptor_present) << ','
      << "\"cache_descriptor_present\":"
      << BoolJson(cache_descriptor_present) << ','
      << "\"session_visible_state_surface\":"
      << BoolJson(session_visible_state_surface) << ','
      << "\"object_identity_policy\":\""
      << SequenceObjectIdentityPolicy(dialect_id) << "\","
      << "\"uuid_required_object_identity\":true,"
      << "\"engine_catalog_sequence_descriptor_policy\":\""
      << EngineCatalogSequenceDescriptorPolicy(dialect_id) << "\","
      << "\"allocation_finality_policy\":\""
      << SequenceAllocationFinalityPolicy(dialect_id) << "\","
      << "\"lower_layer_allocation_policy\":\""
      << LowerLayerAllocationPolicy(dialect_id) << "\","
      << "\"value_function_profile\":\""
      << SequenceValueFunctionProfile(dialect_id, upper) << "\","
      << "\"session_visibility_policy\":\""
      << SequenceSessionVisibilityPolicy(dialect_id) << "\","
      << "\"sequence_backed_default_policy\":\""
      << SequenceBackedDefaultPolicy(dialect_id, upper) << "\","
      << "\"restart_increment_descriptor_policy\":\""
      << RestartIncrementDescriptorPolicy(dialect_id) << "\","
      << "\"engine_authority\":\"scratchbird\","
      << "\"catalog_descriptor_required\":true,"
      << "\"source_sql_text_included\":false,"
      << "\"original_sql_identifier_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_sequence_value_authority\":false,"
      << "\"donor_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"not_enterprise_proven_pending\","
      << "\"descriptor_exactness_status\":\"parser_sequence_identity_descriptor_recorded_runtime_equivalence_pending\","
      << "\"enterprise_readiness\":\"not_enterprise_ready\"}";
  return out.str();
}

bool HasIdentifierNameResolutionProfile(std::string_view dialect_id) {
  return dialect_id == "firebird" || dialect_id == "mysql" ||
         dialect_id == "postgresql";
}

std::string IdentifierNameResolutionEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool create_surface = StartsWithCommand(upper, "CREATE");
  const bool alter_surface = StartsWithCommand(upper, "ALTER");
  const bool drop_surface = StartsWithCommand(upper, "DROP");
  const bool quoted_identifier_syntax_observed =
      Contains(upper, "\"") || Contains(upper, "`");
  const bool qualified_name_syntax_observed = Contains(upper, ".");

  std::ostringstream out;
  out << "{\"evidence_contract\":\"donor_identifier_name_resolution_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"donor_profile_uuid\":\"" << DonorProfileUuid(dialect_id) << "\","
      << "\"semantic_profile_uuid\":\""
      << IdentifierNameResolutionProfileUuid(dialect_id) << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"name_resolution_profile\":\""
      << IdentifierNameResolutionProfileName(dialect_id) << "\","
      << "\"unquoted_identifier_policy\":\""
      << UnquotedIdentifierPolicy(dialect_id) << "\","
      << "\"quoted_identifier_policy\":\""
      << QuotedIdentifierPolicy(dialect_id) << "\","
      << "\"schema_root_resolution_policy\":\""
      << SchemaRootResolutionPolicy(dialect_id) << "\","
      << "\"generated_catalog_name_behavior\":\""
      << GeneratedCatalogNameBehavior(dialect_id) << "\","
      << "\"namespace_collision_behavior\":\""
      << NamespaceCollisionBehavior(dialect_id) << "\","
      << "\"result_metadata_label_policy\":\""
      << ResultMetadataLabelPolicy(dialect_id) << "\","
      << "\"table_name_filesystem_case_policy\":\""
      << TableNameFilesystemCasePolicy(dialect_id) << "\","
      << "\"release_profile_variant_bound_to_base_donor\":"
      << BoolJson(dialect_id == "mysql") << ','
      << "\"create_surface\":" << BoolJson(create_surface) << ','
      << "\"alter_surface\":" << BoolJson(alter_surface) << ','
      << "\"drop_surface\":" << BoolJson(drop_surface) << ','
      << "\"quoted_identifier_syntax_observed\":"
      << BoolJson(quoted_identifier_syntax_observed) << ','
      << "\"qualified_name_syntax_observed\":"
      << BoolJson(qualified_name_syntax_observed) << ','
      << "\"uuid_descriptor_resolution_required\":true,"
      << "\"catalog_descriptor_required\":true,"
      << "\"source_sql_text_included\":false,"
      << "\"original_sql_identifier_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,"
      << "\"cross_root_authority\":false,"
      << "\"cross_root_resolution_policy\":\"explicit_no_cross_root_authority_uuid_root_required\","
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"donor_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"not_enterprise_proven_pending\","
      << "\"descriptor_exactness_status\":\"parser_identifier_resolution_descriptor_recorded_runtime_equivalence_pending\","
      << "\"enterprise_readiness\":\"not_enterprise_ready\"}";
  return out.str();
}

bool HasScalarExpressionSemanticProfile(std::string_view dialect_id) {
  return dialect_id == "firebird" || dialect_id == "mysql" ||
         dialect_id == "postgresql";
}

bool IsScalarExpressionSemanticStatement(std::string_view dialect_id,
                                         std::string_view active_upper_sql) {
  if (!HasScalarExpressionSemanticProfile(dialect_id)) return false;
  const auto upper = TrimAsciiView(active_upper_sql);
  if (!(StartsWithCommand(upper, "SELECT") || StartsWithCommand(upper, "WITH"))) {
    return false;
  }
  return HasCastOrCoercionSurface(upper) ||
         HasNullLogicSurface(upper) ||
         HasBooleanLiteralSurface(upper) ||
         HasStringComparisonSurface(upper) ||
         HasTemporalExpressionSurface(upper) ||
         HasNumericExpressionSurface(upper) ||
         HasPatternMatchingSurface(upper) ||
         HasConditionalExpressionSurface(upper);
}

std::string ScalarExpressionSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool cast_or_coercion_surface = HasCastOrCoercionSurface(upper);
  const bool null_logic_surface = HasNullLogicSurface(upper);
  const bool boolean_literal_surface = HasBooleanLiteralSurface(upper);
  const bool string_comparison_surface = HasStringComparisonSurface(upper);
  const bool temporal_expression_surface = HasTemporalExpressionSurface(upper);
  const bool numeric_expression_surface = HasNumericExpressionSurface(upper);
  const bool pattern_matching_surface = HasPatternMatchingSurface(upper);
  const bool conditional_expression_surface =
      HasConditionalExpressionSurface(upper);
  const bool null_safe_equality_surface = Contains(upper, "<=>");
  const bool is_distinct_from_surface =
      Contains(upper, " IS DISTINCT FROM") ||
      Contains(upper, " IS NOT DISTINCT FROM");
  const bool regexp_surface = Contains(upper, " REGEXP ") ||
                              Contains(upper, " RLIKE ") ||
                              Contains(upper, " ~ ") ||
                              Contains(upper, " ~* ") ||
                              Contains(upper, " !~ ") ||
                              Contains(upper, " !~* ");
  const bool similar_to_surface = Contains(upper, " SIMILAR TO ");
  const bool donor_conditional_function_surface =
      HasFunctionCall(upper, "IIF") || HasFunctionCall(upper, "IF") ||
      HasFunctionCall(upper, "IFNULL") || HasFunctionCall(upper, "DECODE");

  std::ostringstream out;
  out << "{\"evidence_contract\":\"donor_scalar_expression_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"donor_profile_uuid\":\"" << DonorProfileUuid(dialect_id) << "\","
      << "\"semantic_profile_uuid\":\""
      << ScalarExpressionSemanticProfileUuid(dialect_id) << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"scalar_expression_profile\":\""
      << ScalarExpressionProfileName(dialect_id) << "\","
      << "\"query_expression_surface\":\"" << QueryExpressionSurface(upper)
      << "\","
      << "\"cast_type_coercion_profile\":\""
      << CastTypeCoercionProfile(dialect_id) << "\","
      << "\"null_three_valued_logic_profile\":\""
      << NullThreeValuedLogicProfile(dialect_id) << "\","
      << "\"boolean_literal_profile\":\""
      << BooleanLiteralProfile(dialect_id) << "\","
      << "\"string_comparison_collation_profile\":\""
      << StringComparisonCollationProfile(dialect_id) << "\","
      << "\"temporal_literal_current_timestamp_date_arithmetic_profile\":\""
      << TemporalLiteralCurrentTimestampDateArithmeticProfile(dialect_id)
      << "\","
      << "\"numeric_division_rounding_overflow_profile\":\""
      << NumericDivisionRoundingOverflowProfile(dialect_id) << "\","
      << "\"pattern_matching_profile\":\""
      << PatternMatchingProfile(dialect_id) << "\","
      << "\"conditional_expression_profile\":\""
      << ConditionalExpressionProfile(dialect_id) << "\","
      << "\"expression_builtin_profile\":\""
      << ExpressionBuiltinProfile(dialect_id) << "\","
      << "\"cast_or_coercion_surface\":"
      << BoolJson(cast_or_coercion_surface) << ','
      << "\"null_logic_surface\":" << BoolJson(null_logic_surface) << ','
      << "\"boolean_literal_surface\":"
      << BoolJson(boolean_literal_surface) << ','
      << "\"string_comparison_surface\":"
      << BoolJson(string_comparison_surface) << ','
      << "\"temporal_expression_surface\":"
      << BoolJson(temporal_expression_surface) << ','
      << "\"numeric_expression_surface\":"
      << BoolJson(numeric_expression_surface) << ','
      << "\"pattern_matching_surface\":"
      << BoolJson(pattern_matching_surface) << ','
      << "\"conditional_expression_surface\":"
      << BoolJson(conditional_expression_surface) << ','
      << "\"null_safe_equality_surface\":"
      << BoolJson(null_safe_equality_surface) << ','
      << "\"is_distinct_from_surface\":"
      << BoolJson(is_distinct_from_surface) << ','
      << "\"regexp_surface\":" << BoolJson(regexp_surface) << ','
      << "\"similar_to_surface\":" << BoolJson(similar_to_surface) << ','
      << "\"donor_conditional_function_surface\":"
      << BoolJson(donor_conditional_function_surface) << ','
      << "\"uuid_required_semantic_profile\":true,"
      << "\"engine_authority\":\"scratchbird\","
      << "\"source_sql_text_included\":false,"
      << "\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,"
      << "\"parser_scalar_truth_authority\":false,"
      << "\"parser_collation_authority\":false,"
      << "\"parser_datatype_finality_authority\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"donor_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"not_enterprise_proven_pending\","
      << "\"descriptor_exactness_status\":\"parser_scalar_expression_descriptor_recorded_runtime_equivalence_pending\","
      << "\"enterprise_readiness\":\"not_enterprise_ready\"}";
  return out.str();
}

bool HasDmlMutationSemanticProfile(std::string_view dialect_id) {
  return dialect_id == "firebird" || dialect_id == "mysql" ||
         dialect_id == "postgresql";
}

bool IsDmlMutationSemanticStatement(std::string_view dialect_id,
                                    std::string_view active_upper_sql) {
  if (!HasDmlMutationSemanticProfile(dialect_id)) return false;
  const auto upper = TrimAsciiView(active_upper_sql);
  if (dialect_id == "firebird") {
    return StartsWithCommand(upper, "UPDATE OR INSERT") ||
           StartsWithCommand(upper, "MERGE") ||
           StartsWithCommand(upper, "INSERT") ||
           StartsWithCommand(upper, "UPDATE") ||
           StartsWithCommand(upper, "DELETE");
  }
  if (dialect_id == "mysql") {
    return StartsWithCommand(upper, "INSERT") ||
           StartsWithCommand(upper, "UPDATE") ||
           StartsWithCommand(upper, "DELETE") ||
           StartsWithCommand(upper, "REPLACE");
  }
  if (dialect_id == "postgresql") {
    return StartsWithCommand(upper, "INSERT") ||
           StartsWithCommand(upper, "UPDATE") ||
           StartsWithCommand(upper, "DELETE") ||
           StartsWithCommand(upper, "MERGE");
  }
  return false;
}

std::string DmlMutationSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool insert_surface = StartsWithCommand(upper, "INSERT");
  const bool update_surface =
      StartsWithCommand(upper, "UPDATE") &&
      !StartsWithCommand(upper, "UPDATE OR INSERT");
  const bool delete_surface = StartsWithCommand(upper, "DELETE");
  const bool update_or_insert_surface =
      StartsWithCommand(upper, "UPDATE OR INSERT");
  const bool replace_surface = StartsWithCommand(upper, "REPLACE");
  const bool merge_surface = StartsWithCommand(upper, "MERGE");
  const bool on_duplicate_key_update_surface =
      Contains(upper, " ON DUPLICATE KEY UPDATE");
  const bool on_conflict_surface = Contains(upper, " ON CONFLICT ");
  const bool on_conflict_do_update_surface =
      on_conflict_surface && Contains(upper, " DO UPDATE");
  const bool on_conflict_do_nothing_surface =
      on_conflict_surface && Contains(upper, " DO NOTHING");
  const bool matching_surface = ContainsWord(upper, "MATCHING");
  const bool returning_surface = ContainsWord(upper, "RETURNING");
  const bool cursor_positioned_dml_surface =
      Contains(upper, " WHERE CURRENT OF ");
  const bool default_value_surface = ContainsWord(upper, "DEFAULT");
  const bool generated_column_surface = ContainsWord(upper, "GENERATED");
  const bool trigger_interaction_descriptor_required =
      insert_surface || update_surface || delete_surface ||
      update_or_insert_surface || replace_surface || merge_surface;

  std::ostringstream out;
  out << "{\"evidence_contract\":\"donor_dml_mutation_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"donor_profile_uuid\":\"" << DonorProfileUuid(dialect_id) << "\","
      << "\"semantic_profile_uuid\":\""
      << DmlMutationSemanticProfileUuid(dialect_id) << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"mutation_profile\":\"" << DmlMutationProfileName(dialect_id)
      << "\","
      << "\"mutation_surface\":\""
      << DmlMutationSurface(dialect_id, upper) << "\","
      << "\"insert_surface\":" << BoolJson(insert_surface) << ','
      << "\"update_surface\":" << BoolJson(update_surface) << ','
      << "\"delete_surface\":" << BoolJson(delete_surface) << ','
      << "\"update_or_insert_surface\":"
      << BoolJson(update_or_insert_surface) << ','
      << "\"replace_surface\":" << BoolJson(replace_surface) << ','
      << "\"merge_surface\":" << BoolJson(merge_surface) << ','
      << "\"matching_surface\":" << BoolJson(matching_surface) << ','
      << "\"on_duplicate_key_update_surface\":"
      << BoolJson(on_duplicate_key_update_surface) << ','
      << "\"on_conflict_surface\":" << BoolJson(on_conflict_surface) << ','
      << "\"on_conflict_do_update_surface\":"
      << BoolJson(on_conflict_do_update_surface) << ','
      << "\"on_conflict_do_nothing_surface\":"
      << BoolJson(on_conflict_do_nothing_surface) << ','
      << "\"upsert_merge_conflict_policy\":\""
      << UpsertMergeConflictPolicy(dialect_id, upper) << "\","
      << "\"returning_output_projection_surface\":"
      << BoolJson(returning_surface) << ','
      << "\"returning_output_projection_policy\":\""
      << ReturningOutputProjectionPolicy(dialect_id, upper) << "\","
      << "\"cursor_positioned_dml_surface\":"
      << BoolJson(cursor_positioned_dml_surface) << ','
      << "\"cursor_positioned_dml_policy\":\""
      << CursorPositionedDmlPolicy(dialect_id, upper) << "\","
      << "\"affected_row_count_policy\":\""
      << AffectedRowCountPolicy(dialect_id, upper) << "\","
      << "\"default_value_surface\":" << BoolJson(default_value_surface) << ','
      << "\"generated_column_surface\":"
      << BoolJson(generated_column_surface) << ','
      << "\"trigger_interaction_descriptor_required\":"
      << BoolJson(trigger_interaction_descriptor_required) << ','
      << "\"trigger_default_generated_column_interaction_policy\":\""
      << TriggerDefaultGeneratedColumnInteractionPolicy(dialect_id, upper)
      << "\","
      << "\"uuid_required_semantic_profile\":true,"
      << "\"catalog_descriptor_required\":true,"
      << "\"sblr_operation_uuid_resolution_required\":true,"
      << "\"engine_authority\":\"scratchbird\","
      << "\"source_sql_text_included\":false,"
      << "\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_visibility_authority\":false,"
      << "\"parser_row_count_authority\":false,"
      << "\"parser_trigger_order_authority\":false,"
      << "\"parser_default_value_authority\":false,"
      << "\"parser_generated_column_authority\":false,"
      << "\"donor_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"not_enterprise_proven_pending\","
      << "\"descriptor_exactness_status\":\"parser_dml_mutation_descriptor_recorded_runtime_equivalence_pending\","
      << "\"enterprise_readiness\":\"not_enterprise_ready\"}";
  return out.str();
}

bool HasTransactionSessionSemanticProfile(std::string_view dialect_id) {
  return dialect_id == "firebird" || dialect_id == "mysql" ||
         dialect_id == "postgresql";
}

bool IsTransactionSessionSemanticStatement(std::string_view dialect_id,
                                           std::string_view active_upper_sql) {
  if (!HasTransactionSessionSemanticProfile(dialect_id)) return false;
  const auto upper = TrimAsciiView(active_upper_sql);
  if (dialect_id == "firebird") {
    return StartsWithCommand(upper, "SET TRANSACTION") ||
           StartsWithCommand(upper, "COMMIT") ||
           StartsWithCommand(upper, "ROLLBACK") ||
           StartsWithCommand(upper, "SAVEPOINT") ||
           StartsWithCommand(upper, "RELEASE SAVEPOINT");
  }
  if (dialect_id == "mysql") {
    return StartsWithCommand(upper, "START TRANSACTION") ||
           StartsWithCommand(upper, "BEGIN") ||
           StartsWithCommand(upper, "COMMIT") ||
           StartsWithCommand(upper, "ROLLBACK") ||
           StartsWithCommand(upper, "SAVEPOINT") ||
           StartsWithCommand(upper, "RELEASE SAVEPOINT") ||
           (StartsWithCommand(upper, "SET") &&
            (ContainsWord(upper, "AUTOCOMMIT") ||
             ContainsWord(upper, "SQL_MODE") ||
             Contains(upper, " TRANSACTION ISOLATION ")));
  }
  if (dialect_id == "postgresql") {
    return StartsWithCommand(upper, "BEGIN") ||
           StartsWithCommand(upper, "START TRANSACTION") ||
           StartsWithCommand(upper, "COMMIT") ||
           StartsWithCommand(upper, "ROLLBACK") ||
           StartsWithCommand(upper, "SAVEPOINT") ||
           StartsWithCommand(upper, "RELEASE SAVEPOINT") ||
           StartsWithCommand(upper, "SET TRANSACTION") ||
           (StartsWithCommand(upper, "SET") &&
            (ContainsWord(upper, "TRANSACTION_ISOLATION") ||
             ContainsWord(upper, "STATEMENT_TIMEOUT") ||
             ContainsWord(upper, "SEARCH_PATH")));
  }
  return false;
}

std::string TransactionSessionSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool begin_surface =
      StartsWithCommand(upper, "BEGIN") ||
      StartsWithCommand(upper, "START TRANSACTION") ||
      (dialect_id == "firebird" && StartsWithCommand(upper, "SET TRANSACTION"));
  const bool commit_surface = StartsWithCommand(upper, "COMMIT");
  const bool rollback_to_savepoint_surface = IsRollbackToSavepoint(upper);
  const bool rollback_surface =
      StartsWithCommand(upper, "ROLLBACK") && !rollback_to_savepoint_surface;
  const bool savepoint_surface = StartsWithCommand(upper, "SAVEPOINT");
  const bool release_savepoint_surface =
      StartsWithCommand(upper, "RELEASE SAVEPOINT");
  const bool autocommit_surface =
      StartsWithCommand(upper, "SET") && ContainsWord(upper, "AUTOCOMMIT");
  const bool isolation_descriptor_surface =
      IsSetTransactionIsolation(upper) ||
      ContainsWord(upper, "SERIALIZABLE") ||
      Contains(upper, "READ COMMITTED") ||
      Contains(upper, "REPEATABLE READ") ||
      Contains(upper, "READ UNCOMMITTED") ||
      ContainsWord(upper, "SNAPSHOT") ||
      Contains(upper, "TABLE STABILITY");
  const bool read_only_surface = Contains(upper, "READ ONLY");
  const bool read_write_surface = Contains(upper, "READ WRITE");
  const bool wait_no_wait_surface =
      dialect_id == "firebird" &&
      (ContainsWord(upper, "WAIT") || Contains(upper, "NO WAIT"));
  const bool deferrable_surface =
      dialect_id == "postgresql" && ContainsWord(upper, "DEFERRABLE") &&
      !Contains(upper, "NOT DEFERRABLE");
  const bool session_variable_surface =
      StartsWithCommand(upper, "SET") &&
      (ContainsWord(upper, "AUTOCOMMIT") ||
       ContainsWord(upper, "SQL_MODE") ||
       ContainsWord(upper, "TRANSACTION_ISOLATION") ||
       ContainsWord(upper, "STATEMENT_TIMEOUT") ||
       ContainsWord(upper, "SEARCH_PATH"));
  const bool sql_mode_surface = ContainsWord(upper, "SQL_MODE");
  const bool statement_timeout_surface = ContainsWord(upper, "STATEMENT_TIMEOUT");
  const bool search_path_surface = ContainsWord(upper, "SEARCH_PATH");

  std::ostringstream out;
  out << "{\"evidence_contract\":\"donor_transaction_session_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"donor_profile_uuid\":\"" << DonorProfileUuid(dialect_id) << "\","
      << "\"semantic_profile_uuid\":\""
      << TransactionSessionSemanticProfileUuid(dialect_id) << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"transaction_session_profile\":\""
      << TransactionSessionProfileName(dialect_id) << "\","
      << "\"transaction_session_surface\":\""
      << TransactionSessionSurface(dialect_id, upper) << "\","
      << "\"statement_family_linkage\":\""
      << TransactionSessionFamilyLinkage(dialect_id, upper) << "\","
      << "\"begin_autocommit_policy\":\""
      << BeginAutocommitPolicy(dialect_id, upper) << "\","
      << "\"commit_rollback_finality_policy\":\"engine_mga_authority\","
      << "\"transaction_identity_policy\":\"engine_mga_authority\","
      << "\"visibility_policy\":\"engine_mga_authority\","
      << "\"recovery_policy\":\"engine_mga_authority\","
      << "\"savepoint_policy\":\"transaction_local_engine_owned\","
      << "\"isolation_read_only_deferrable_descriptor_policy\":\""
      << IsolationReadOnlyDeferrablePolicy(dialect_id, upper) << "\","
      << "\"session_variable_sql_mode_descriptor_policy\":\""
      << SessionVariableSqlModePolicy(dialect_id, upper) << "\","
      << "\"begin_surface\":" << BoolJson(begin_surface) << ','
      << "\"commit_surface\":" << BoolJson(commit_surface) << ','
      << "\"rollback_surface\":" << BoolJson(rollback_surface) << ','
      << "\"rollback_to_savepoint_surface\":"
      << BoolJson(rollback_to_savepoint_surface) << ','
      << "\"savepoint_surface\":" << BoolJson(savepoint_surface) << ','
      << "\"release_savepoint_surface\":"
      << BoolJson(release_savepoint_surface) << ','
      << "\"autocommit_surface\":" << BoolJson(autocommit_surface) << ','
      << "\"isolation_descriptor_surface\":"
      << BoolJson(isolation_descriptor_surface) << ','
      << "\"read_only_surface\":" << BoolJson(read_only_surface) << ','
      << "\"read_write_surface\":" << BoolJson(read_write_surface) << ','
      << "\"wait_no_wait_surface\":" << BoolJson(wait_no_wait_surface) << ','
      << "\"deferrable_surface\":" << BoolJson(deferrable_surface) << ','
      << "\"session_variable_surface\":"
      << BoolJson(session_variable_surface) << ','
      << "\"sql_mode_surface\":" << BoolJson(sql_mode_surface) << ','
      << "\"statement_timeout_surface\":"
      << BoolJson(statement_timeout_surface) << ','
      << "\"search_path_surface\":" << BoolJson(search_path_surface) << ','
      << "\"uuid_required_semantic_profile\":true,"
      << "\"catalog_descriptor_required\":true,"
      << "\"sblr_operation_uuid_resolution_required\":true,"
      << "\"engine_authority\":\"scratchbird\","
      << "\"source_sql_text_included\":false,"
      << "\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_visibility_authority\":false,"
      << "\"parser_savepoint_authority\":false,"
      << "\"parser_isolation_authority\":false,"
      << "\"parser_autocommit_authority\":false,"
      << "\"donor_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"not_enterprise_proven_pending\","
      << "\"descriptor_exactness_status\":\"parser_transaction_session_descriptor_recorded_runtime_equivalence_pending\","
      << "\"enterprise_readiness\":\"not_enterprise_ready\"}";
  return out.str();
}

bool HasTemporarySessionObjectSemanticProfile(std::string_view dialect_id) {
  return dialect_id == "firebird" || dialect_id == "mysql" ||
         dialect_id == "postgresql";
}

bool IsTemporarySessionObjectSemanticStatement(
    std::string_view dialect_id,
    std::string_view active_upper_sql) {
  if (!HasTemporarySessionObjectSemanticProfile(dialect_id)) return false;
  const auto upper = TrimAsciiView(active_upper_sql);
  return IsCreateTemporaryTableStatement(dialect_id, upper) ||
         IsDropTemporaryTableStatement(dialect_id, upper) ||
         IsAlterTemporaryTableStatement(dialect_id, upper);
}

std::string TemporarySessionObjectSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool create_surface = IsCreateTemporaryTableStatement(dialect_id, upper);
  const bool alter_surface = IsAlterTemporaryTableStatement(dialect_id, upper);
  const bool drop_surface = IsDropTemporaryTableStatement(dialect_id, upper);
  const bool global_keyword_surface =
      ContainsWord(upper, "GLOBAL") ||
      StartsWithCommand(upper, "CREATE GLOBAL TEMP") ||
      StartsWithCommand(upper, "CREATE GLOBAL TEMPORARY");
  const bool local_keyword_surface =
      ContainsWord(upper, "LOCAL") ||
      StartsWithCommand(upper, "CREATE LOCAL TEMP") ||
      StartsWithCommand(upper, "CREATE LOCAL TEMPORARY");
  const bool temporary_keyword_surface =
      ContainsWord(upper, "TEMP") || ContainsWord(upper, "TEMPORARY") ||
      Contains(upper, "PG_TEMP.") || Contains(upper, "TEMP");
  const bool table_object_surface =
      ContainsWord(upper, "TABLE") || StartsWithCommand(upper, "DROP TABLE") ||
      StartsWithCommand(upper, "ALTER TABLE");
  const bool on_commit_delete_rows_surface =
      Contains(upper, "ON COMMIT DELETE ROWS");
  const bool on_commit_preserve_rows_surface =
      Contains(upper, "ON COMMIT PRESERVE ROWS");
  const bool on_commit_drop_surface = Contains(upper, "ON COMMIT DROP");
  const bool name_shadowing_surface =
      dialect_id == "mysql" ||
      (dialect_id == "postgresql" && temporary_keyword_surface);

  std::ostringstream out;
  out << "{\"evidence_contract\":\"donor_temporary_session_object_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"donor_profile_uuid\":\"" << DonorProfileUuid(dialect_id) << "\","
      << "\"semantic_profile_uuid\":\""
      << TemporarySessionObjectSemanticProfileUuid(dialect_id) << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"temporary_object_profile\":\""
      << TemporarySessionObjectProfileName(dialect_id) << "\","
      << "\"temporary_object_surface\":\""
      << TemporaryObjectSurface(dialect_id, upper) << "\","
      << "\"temporary_object_kind_policy\":\""
      << TemporaryObjectKindPolicy(dialect_id, upper) << "\","
      << "\"global_local_temp_object_kind_policy\":\""
      << TemporaryObjectKindPolicy(dialect_id, upper) << "\","
      << "\"create_surface\":" << BoolJson(create_surface) << ','
      << "\"alter_surface\":" << BoolJson(alter_surface) << ','
      << "\"drop_surface\":" << BoolJson(drop_surface) << ','
      << "\"global_keyword_surface\":"
      << BoolJson(global_keyword_surface) << ','
      << "\"local_keyword_surface\":"
      << BoolJson(local_keyword_surface) << ','
      << "\"temporary_keyword_surface\":"
      << BoolJson(temporary_keyword_surface) << ','
      << "\"table_object_surface\":"
      << BoolJson(table_object_surface) << ','
      << "\"on_commit_delete_rows_surface\":"
      << BoolJson(on_commit_delete_rows_surface) << ','
      << "\"on_commit_preserve_rows_surface\":"
      << BoolJson(on_commit_preserve_rows_surface) << ','
      << "\"on_commit_drop_surface\":"
      << BoolJson(on_commit_drop_surface) << ','
      << "\"on_commit_policy\":\""
      << OnCommitPolicy(dialect_id, upper) << "\","
      << "\"on_commit_delete_rows_policy\":\""
      << OnCommitDeleteRowsPolicy(dialect_id) << "\","
      << "\"on_commit_preserve_rows_policy\":\""
      << OnCommitPreserveRowsPolicy(dialect_id) << "\","
      << "\"on_commit_drop_policy\":\""
      << OnCommitDropPolicy(dialect_id) << "\","
      << "\"name_shadowing_surface\":"
      << BoolJson(name_shadowing_surface) << ','
      << "\"name_shadowing_policy\":\""
      << (dialect_id == "mysql"
              ? "mysql_temporary_table_name_shadows_base_table_within_session"
              : dialect_id == "postgresql"
                    ? "postgresql_pg_temp_search_path_shadows_permanent_objects"
                    : "firebird_no_session_name_shadowing_regular_schema_namespace")
      << "\","
      << "\"session_visibility_policy\":\""
      << SessionVisibilityPolicy(dialect_id) << "\","
      << "\"catalog_visibility_policy\":\""
      << CatalogVisibilityPolicy(dialect_id) << "\","
      << "\"transaction_interaction_policy\":\"engine_mga_authority\","
      << "\"session_interaction_policy\":\"engine_session_authority\","
      << "\"cleanup_lifetime_policy\":\"engine_session_catalog_authority\","
      << "\"temporary_object_lifetime_policy\":\""
      << TemporaryObjectLifetimePolicy(dialect_id, upper) << "\","
      << "\"schema_root_sandbox_policy\":\""
      << SchemaRootSandboxPolicy(dialect_id) << "\","
      << "\"uuid_required_semantic_profile\":true,"
      << "\"catalog_descriptor_required\":true,"
      << "\"session_descriptor_required\":true,"
      << "\"sblr_operation_uuid_resolution_required\":true,"
      << "\"engine_authority\":\"scratchbird\","
      << "\"source_sql_text_included\":false,"
      << "\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,"
      << "\"parser_catalog_authority\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_session_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_visibility_authority\":false,"
      << "\"parser_cleanup_authority\":false,"
      << "\"donor_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"not_enterprise_proven_pending\","
      << "\"descriptor_exactness_status\":\"parser_temporary_session_object_descriptor_recorded_runtime_equivalence_pending\","
      << "\"enterprise_readiness\":\"not_enterprise_ready\"}";
  return out.str();
}

bool HasDependencyBearingDdlSemanticProfile(std::string_view dialect_id) {
  return dialect_id == "firebird" || dialect_id == "mysql" ||
         dialect_id == "postgresql";
}

bool IsDependencyBearingDdlSemanticStatement(
    std::string_view dialect_id,
    std::string_view active_upper_sql) {
  if (!HasDependencyBearingDdlSemanticProfile(dialect_id)) return false;
  const auto upper = TrimAsciiView(active_upper_sql);
  if (dialect_id == "firebird") {
    return IsViewDependencyDdl(upper) ||
           IsTriggerDependencyDdl(upper) ||
           IsRoutineDependencyDdl(upper) ||
           IsPackageDependencyDdl(upper);
  }
  if (dialect_id == "mysql") {
    return IsViewDependencyDdl(upper) ||
           IsTriggerDependencyDdl(upper) ||
           IsRoutineDependencyDdl(upper) ||
           IsEventDependencyDdl(upper);
  }
  if (dialect_id == "postgresql") {
    return IsViewDependencyDdl(upper) ||
           IsMaterializedViewDependencyDdl(upper) ||
           IsTriggerDependencyDdl(upper) ||
           IsRoutineDependencyDdl(upper) ||
           IsRuleDependencyDdl(upper);
  }
  return false;
}

std::string DependencyBearingDdlSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool view_surface = IsViewDependencyDdl(upper);
  const bool materialized_view_surface = IsMaterializedViewDependencyDdl(upper);
  const bool trigger_surface = IsTriggerDependencyDdl(upper);
  const bool routine_surface = IsRoutineDependencyDdl(upper);
  const bool procedure_surface =
      StartsWithCommand(upper, "CREATE PROCEDURE") ||
      StartsWithCommand(upper, "CREATE OR ALTER PROCEDURE") ||
      StartsWithCommand(upper, "CREATE OR REPLACE PROCEDURE") ||
      StartsWithCommand(upper, "ALTER PROCEDURE") ||
      StartsWithCommand(upper, "DROP PROCEDURE") ||
      StartsWithCommand(upper, "RECREATE PROCEDURE");
  const bool function_surface =
      StartsWithCommand(upper, "CREATE FUNCTION") ||
      StartsWithCommand(upper, "CREATE OR ALTER FUNCTION") ||
      StartsWithCommand(upper, "CREATE OR REPLACE FUNCTION") ||
      StartsWithCommand(upper, "ALTER FUNCTION") ||
      StartsWithCommand(upper, "DROP FUNCTION") ||
      StartsWithCommand(upper, "RECREATE FUNCTION");
  const bool package_surface = IsPackageDependencyDdl(upper);
  const bool rule_surface = IsRuleDependencyDdl(upper);
  const bool event_surface = IsEventDependencyDdl(upper);
  const bool executable_body_surface =
      trigger_surface || routine_surface || package_surface ||
      rule_surface || event_surface;
  const bool query_dependency_surface =
      view_surface || materialized_view_surface ||
      executable_body_surface || ContainsWord(upper, "FROM") ||
      ContainsWord(upper, "JOIN") || ContainsWord(upper, "ON") ||
      ContainsWord(upper, "REFERENCES") || Contains(upper, "EXECUTE FUNCTION");
  const bool drop_surface = StartsWithCommand(upper, "DROP");
  const bool alter_surface = StartsWithCommand(upper, "ALTER");
  const bool create_surface =
      StartsWithCommand(upper, "CREATE") || StartsWithCommand(upper, "RECREATE");

  std::ostringstream out;
  out << "{\"evidence_contract\":\"donor_dependency_bearing_ddl_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"donor_profile_uuid\":\"" << DonorProfileUuid(dialect_id) << "\","
      << "\"semantic_profile_uuid\":\""
      << DependencyBearingDdlSemanticProfileUuid(dialect_id) << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"dependency_ddl_profile\":\""
      << DependencyBearingDdlProfileName(dialect_id) << "\","
      << "\"dependency_ddl_surface\":\""
      << DependencyDdlSurface(dialect_id, upper) << "\","
      << "\"view_surface\":" << BoolJson(view_surface) << ','
      << "\"materialized_view_surface\":"
      << BoolJson(materialized_view_surface) << ','
      << "\"trigger_surface\":" << BoolJson(trigger_surface) << ','
      << "\"routine_surface\":" << BoolJson(routine_surface) << ','
      << "\"procedure_surface\":" << BoolJson(procedure_surface) << ','
      << "\"function_surface\":" << BoolJson(function_surface) << ','
      << "\"package_surface\":" << BoolJson(package_surface) << ','
      << "\"rule_surface\":" << BoolJson(rule_surface) << ','
      << "\"event_surface\":" << BoolJson(event_surface) << ','
      << "\"executable_body_surface\":"
      << BoolJson(executable_body_surface) << ','
      << "\"query_dependency_surface\":"
      << BoolJson(query_dependency_surface) << ','
      << "\"create_surface\":" << BoolJson(create_surface) << ','
      << "\"alter_surface\":" << BoolJson(alter_surface) << ','
      << "\"drop_surface\":" << BoolJson(drop_surface) << ','
      << "\"dependency_binding_policy\":\""
      << DependencyBindingPolicy(dialect_id, upper) << "\","
      << "\"invalidation_policy\":\""
      << DependencyInvalidationPolicy(dialect_id, upper) << "\","
      << "\"execution_body_policy\":\""
      << DependencyExecutionBodyPolicy(dialect_id, upper) << "\","
      << "\"catalog_storage_policy\":\""
      << DependencyCatalogStoragePolicy(dialect_id) << "\","
      << "\"sandbox_root_policy\":\""
      << SchemaRootSandboxPolicy(dialect_id) << "\","
      << "\"uuid_required_semantic_profile\":true,"
      << "\"catalog_descriptor_required\":true,"
      << "\"dependency_graph_descriptor_required\":true,"
      << "\"source_retention_reference_required\":"
      << BoolJson(executable_body_surface) << ','
      << "\"sblr_operation_uuid_resolution_required\":true,"
      << "\"engine_authority\":\"scratchbird\","
      << "\"dependency_authority\":\"engine_catalog_uuid_dependency_graph\","
      << "\"source_sql_text_included\":false,"
      << "\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,"
      << "\"parser_catalog_authority\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_execution_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_dependency_finality_authority\":false,"
      << "\"parser_invalidation_authority\":false,"
      << "\"donor_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"not_enterprise_proven_pending\","
      << "\"descriptor_exactness_status\":\"parser_dependency_bearing_ddl_descriptor_recorded_runtime_equivalence_pending\","
      << "\"enterprise_readiness\":\"not_enterprise_ready\"}";
  return out.str();
}

bool HasDdlTransactionBehaviorSemanticProfile(std::string_view dialect_id) {
  return dialect_id == "firebird" || dialect_id == "mysql" ||
         dialect_id == "postgresql";
}

bool IsDdlTransactionBehaviorSemanticStatement(
    std::string_view dialect_id,
    std::string_view active_upper_sql) {
  if (!HasDdlTransactionBehaviorSemanticProfile(dialect_id)) return false;
  const auto upper = TrimAsciiView(active_upper_sql);
  return StartsWithCommand(upper, "CREATE") ||
         StartsWithCommand(upper, "ALTER") ||
         StartsWithCommand(upper, "DROP") ||
         StartsWithCommand(upper, "RECREATE") ||
         StartsWithCommand(upper, "TRUNCATE") ||
         StartsWithCommand(upper, "COMMENT");
}

std::string DdlTransactionBehaviorSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool create_surface =
      StartsWithCommand(upper, "CREATE") || StartsWithCommand(upper, "RECREATE");
  const bool alter_surface = StartsWithCommand(upper, "ALTER");
  const bool drop_surface = StartsWithCommand(upper, "DROP");
  const bool table_surface =
      ContainsWord(upper, "TABLE") || StartsWithCommand(upper, "TRUNCATE");
  const bool index_surface =
      IsCreateIndexKeywordSequence(LexTokens(upper)) ||
      StartsWithCommand(upper, "ALTER INDEX") ||
      StartsWithCommand(upper, "DROP INDEX");
  const bool view_surface =
      IsViewDependencyDdl(upper) || IsMaterializedViewDependencyDdl(upper);
  const bool implicit_commit_surface = dialect_id == "mysql";
  const bool transactional_ddl_surface =
      dialect_id == "firebird" ||
      (dialect_id == "postgresql" && !ContainsWord(upper, "CONCURRENTLY"));
  const bool nontransactional_ddl_surface =
      dialect_id == "mysql" ||
      (dialect_id == "postgresql" && ContainsWord(upper, "CONCURRENTLY"));

  std::ostringstream out;
  out << "{\"evidence_contract\":\"donor_ddl_transaction_behavior_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"donor_profile_uuid\":\"" << DonorProfileUuid(dialect_id) << "\","
      << "\"semantic_profile_uuid\":\""
      << DdlTransactionBehaviorSemanticProfileUuid(dialect_id) << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"ddl_transaction_behavior_profile\":\""
      << DdlTransactionBehaviorProfileName(dialect_id) << "\","
      << "\"statement_classification\":\"ddl\","
      << "\"ddl_operation_kind\":\"" << DdlOperationKind(upper) << "\","
      << "\"transaction_policy\":\""
      << DdlTransactionPolicy(dialect_id, upper) << "\","
      << "\"autocommit_boundary\":\""
      << DdlAutocommitBoundary(dialect_id, upper) << "\","
      << "\"metadata_visibility_epoch\":\""
      << DdlMetadataVisibilityEpoch(dialect_id) << "\","
      << "\"rollback_policy\":\"" << DdlRollbackPolicy(dialect_id, upper)
      << "\","
      << "\"invalid_object_state_policy\":\""
      << DdlInvalidObjectStatePolicy(dialect_id, upper) << "\","
      << "\"diagnostic_map_ref\":\"" << DdlDiagnosticMapRef(dialect_id)
      << "\","
      << "\"sandbox_root_policy\":\"" << SchemaRootSandboxPolicy(dialect_id)
      << "\","
      << "\"create_surface\":" << BoolJson(create_surface) << ','
      << "\"alter_surface\":" << BoolJson(alter_surface) << ','
      << "\"drop_surface\":" << BoolJson(drop_surface) << ','
      << "\"table_surface\":" << BoolJson(table_surface) << ','
      << "\"index_surface\":" << BoolJson(index_surface) << ','
      << "\"view_surface\":" << BoolJson(view_surface) << ','
      << "\"implicit_commit_surface\":"
      << BoolJson(implicit_commit_surface) << ','
      << "\"transactional_ddl_surface\":"
      << BoolJson(transactional_ddl_surface) << ','
      << "\"nontransactional_ddl_surface\":"
      << BoolJson(nontransactional_ddl_surface) << ','
      << "\"uuid_required_semantic_profile\":true,"
      << "\"catalog_descriptor_required\":true,"
      << "\"sblr_operation_uuid_resolution_required\":true,"
      << "\"engine_authority\":\"scratchbird\","
      << "\"transaction_authority\":\"engine_mga_authority\","
      << "\"metadata_visibility_authority\":\"engine_catalog_mga_epoch\","
      << "\"rollback_authority\":\"engine_mga_authority\","
      << "\"invalid_object_state_authority\":\"engine_catalog_uuid_descriptor\","
      << "\"source_sql_text_included\":false,"
      << "\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,"
      << "\"parser_catalog_authority\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_autocommit_authority\":false,"
      << "\"parser_metadata_visibility_authority\":false,"
      << "\"parser_rollback_authority\":false,"
      << "\"parser_invalid_object_state_authority\":false,"
      << "\"parser_recovery_authority\":false,"
      << "\"donor_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"not_enterprise_proven_pending\","
      << "\"descriptor_exactness_status\":\"parser_ddl_transaction_behavior_descriptor_recorded_runtime_equivalence_pending\","
      << "\"enterprise_readiness\":\"not_enterprise_ready\"}";
  return out.str();
}

bool HasResourceTextSemanticProfile(std::string_view dialect_id) {
  return dialect_id == "firebird" || dialect_id == "mysql" ||
         dialect_id == "postgresql";
}

bool IsResourceTextSemanticStatement(std::string_view dialect_id,
                                     std::string_view active_upper_sql) {
  if (!HasResourceTextSemanticProfile(dialect_id)) return false;
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool ddl_surface = StartsWithCommand(upper, "CREATE") ||
                           StartsWithCommand(upper, "ALTER") ||
                           StartsWithCommand(upper, "DROP") ||
                           StartsWithCommand(upper, "RECREATE");
  const bool dml_surface = StartsWithCommand(upper, "INSERT") ||
                           StartsWithCommand(upper, "UPDATE") ||
                           StartsWithCommand(upper, "DELETE") ||
                           StartsWithCommand(upper, "MERGE") ||
                           StartsWithCommand(upper, "REPLACE");
  const bool query_surface =
      StartsWithCommand(upper, "SELECT") || StartsWithCommand(upper, "WITH");
  const bool text_type_surface =
      ContainsWord(upper, "CHAR") || ContainsWord(upper, "VARCHAR") ||
      ContainsWord(upper, "NCHAR") || Contains(upper, "NATIONAL CHARACTER") ||
      ContainsWord(upper, "TEXT") || ContainsWord(upper, "BLOB") ||
      ContainsWord(upper, "BINARY") || ContainsWord(upper, "VARBINARY") ||
      ContainsWord(upper, "BYTEA");
  const bool charset_or_collation_surface =
      Contains(upper, "CHARACTER SET") || ContainsWord(upper, "CHARSET") ||
      ContainsWord(upper, "COLLATE") || ContainsWord(upper, "COLLATION");
  const bool string_literal_surface = Contains(upper, "'");
  const bool pattern_surface =
      Contains(upper, " LIKE ") || Contains(upper, " SIMILAR TO ") ||
      Contains(upper, " REGEXP ") || Contains(upper, " RLIKE ") ||
      Contains(upper, " STARTING WITH ") || Contains(upper, " CONTAINING ") ||
      Contains(upper, " ILIKE ") || Contains(upper, " ~ ") ||
      Contains(upper, " ~* ") || Contains(upper, " !~ ") ||
      Contains(upper, " !~* ");
  const bool cast_to_text_surface =
      (Contains(upper, "CAST(") || Contains(upper, "::")) &&
      text_type_surface;
  const bool temporal_surface =
      ContainsWord(upper, "DATE") || ContainsWord(upper, "TIME") ||
      ContainsWord(upper, "TIMESTAMP") || ContainsWord(upper, "DATETIME") ||
      ContainsWord(upper, "TIMESTAMPTZ") || Contains(upper, "WITH TIME ZONE") ||
      ContainsWord(upper, "TIMEZONE") || ContainsWord(upper, "CURRENT_DATE") ||
      ContainsWord(upper, "CURRENT_TIME") ||
      ContainsWord(upper, "CURRENT_TIMESTAMP");

  if (ddl_surface) return text_type_surface || charset_or_collation_surface;
  if (dml_surface || query_surface) {
    return string_literal_surface || pattern_surface ||
           charset_or_collation_surface || cast_to_text_surface ||
           temporal_surface;
  }
  return false;
}

std::string ResourceTextSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool ddl_surface = StartsWithCommand(upper, "CREATE") ||
                           StartsWithCommand(upper, "ALTER") ||
                           StartsWithCommand(upper, "DROP") ||
                           StartsWithCommand(upper, "RECREATE");
  const bool dml_surface = StartsWithCommand(upper, "INSERT") ||
                           StartsWithCommand(upper, "UPDATE") ||
                           StartsWithCommand(upper, "DELETE") ||
                           StartsWithCommand(upper, "MERGE") ||
                           StartsWithCommand(upper, "REPLACE");
  const bool query_surface =
      StartsWithCommand(upper, "SELECT") || StartsWithCommand(upper, "WITH");
  const bool charset_surface =
      Contains(upper, "CHARACTER SET") || ContainsWord(upper, "CHARSET");
  const bool collation_surface =
      ContainsWord(upper, "COLLATE") || ContainsWord(upper, "COLLATION");
  const bool timezone_surface =
      Contains(upper, "WITH TIME ZONE") || ContainsWord(upper, "TIMEZONE") ||
      ContainsWord(upper, "TIMESTAMPTZ") || ContainsWord(upper, "DATETIME") ||
      ContainsWord(upper, "TIMESTAMP") ||
      ContainsWord(upper, "CURRENT_TIMESTAMP");
  const bool calendar_surface =
      ContainsWord(upper, "DATE") || ContainsWord(upper, "TIME") ||
      ContainsWord(upper, "TIMESTAMP") || ContainsWord(upper, "DATETIME") ||
      ContainsWord(upper, "CURRENT_DATE") || ContainsWord(upper, "CURRENT_TIME");
  const bool pattern_surface =
      Contains(upper, " LIKE ") || Contains(upper, " SIMILAR TO ") ||
      Contains(upper, " REGEXP ") || Contains(upper, " RLIKE ") ||
      Contains(upper, " STARTING WITH ") || Contains(upper, " CONTAINING ") ||
      Contains(upper, " ILIKE ") || Contains(upper, " ~ ") ||
      Contains(upper, " ~* ") || Contains(upper, " !~ ") ||
      Contains(upper, " !~* ");
  const bool comparison_surface =
      collation_surface || pattern_surface || Contains(upper, " = ") ||
      Contains(upper, " <> ") || Contains(upper, " != ");
  const bool binary_text_surface =
      ContainsWord(upper, "BLOB") || ContainsWord(upper, "BINARY") ||
      ContainsWord(upper, "VARBINARY") || ContainsWord(upper, "BYTEA");
  const bool text_type_surface =
      ContainsWord(upper, "CHAR") || ContainsWord(upper, "VARCHAR") ||
      ContainsWord(upper, "NCHAR") || Contains(upper, "NATIONAL CHARACTER") ||
      ContainsWord(upper, "TEXT") || binary_text_surface;

  std::ostringstream out;
  out << "{\"evidence_contract\":\"donor_resource_text_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"donor_profile_uuid\":\"" << DonorProfileUuid(dialect_id) << "\","
      << "\"semantic_profile_uuid\":\""
      << ResourceTextSemanticProfileUuid(dialect_id) << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"resource_text_profile\":\""
      << ResourceTextProfileName(dialect_id) << "\","
      << "\"resource_text_surface\":\""
      << ResourceTextSurface(dialect_id, upper) << "\","
      << "\"charset_policy\":\"" << CharsetPolicy(dialect_id) << "\","
      << "\"collation_policy\":\"" << CollationPolicy(dialect_id) << "\","
      << "\"timezone_policy\":\"" << TimezonePolicy(dialect_id) << "\","
      << "\"calendar_policy\":\"" << CalendarPolicy(dialect_id) << "\","
      << "\"comparison_policy\":\"" << ComparisonPolicy(dialect_id) << "\","
      << "\"pattern_matching_policy\":\""
      << PatternMatchingProfile(dialect_id) << "\","
      << "\"binary_text_policy\":\"" << BinaryTextPolicy(dialect_id)
      << "\","
      << "\"resource_epoch_policy\":\"" << ResourceEpochPolicy(dialect_id)
      << "\","
      << "\"index_compatibility_policy\":\""
      << TextIndexCompatibilityPolicy(dialect_id) << "\","
      << "\"diagnostic_map_ref\":\""
      << ResourceTextDiagnosticMapRef(dialect_id) << "\","
      << "\"sandbox_root_policy\":\"" << SchemaRootSandboxPolicy(dialect_id)
      << "\","
      << "\"charset_surface\":" << BoolJson(charset_surface) << ','
      << "\"collation_surface\":" << BoolJson(collation_surface) << ','
      << "\"timezone_surface\":" << BoolJson(timezone_surface) << ','
      << "\"calendar_surface\":" << BoolJson(calendar_surface) << ','
      << "\"comparison_surface\":" << BoolJson(comparison_surface) << ','
      << "\"pattern_surface\":" << BoolJson(pattern_surface) << ','
      << "\"binary_text_surface\":" << BoolJson(binary_text_surface) << ','
      << "\"text_type_surface\":" << BoolJson(text_type_surface) << ','
      << "\"ddl_surface\":" << BoolJson(ddl_surface) << ','
      << "\"dml_surface\":" << BoolJson(dml_surface) << ','
      << "\"query_surface\":" << BoolJson(query_surface) << ','
      << "\"uuid_required_semantic_profile\":true,"
      << "\"catalog_descriptor_required\":true,"
      << "\"resource_descriptor_required\":true,"
      << "\"text_type_descriptor_required\":true,"
      << "\"sblr_operation_uuid_resolution_required\":true,"
      << "\"engine_authority\":\"scratchbird\","
      << "\"resource_authority\":\"engine_resource_descriptor_authority\","
      << "\"charset_authority\":\"engine_catalog_resource_descriptor\","
      << "\"collation_authority\":\"engine_catalog_resource_descriptor\","
      << "\"timezone_authority\":\"engine_session_resource_descriptor\","
      << "\"calendar_authority\":\"engine_temporal_resource_descriptor\","
      << "\"comparison_authority\":\"engine_expression_resource_descriptor\","
      << "\"pattern_matching_authority\":\"engine_expression_resource_descriptor\","
      << "\"binary_text_authority\":\"engine_datatype_resource_descriptor\","
      << "\"index_compatibility_authority\":\"engine_index_resource_descriptor\","
      << "\"source_sql_text_included\":false,"
      << "\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,"
      << "\"parser_charset_authority\":false,"
      << "\"parser_collation_authority\":false,"
      << "\"parser_timezone_authority\":false,"
      << "\"parser_calendar_authority\":false,"
      << "\"parser_comparison_authority\":false,"
      << "\"parser_pattern_matching_authority\":false,"
      << "\"parser_binary_text_authority\":false,"
      << "\"parser_text_type_authority\":false,"
      << "\"parser_catalog_authority\":false,"
      << "\"parser_resource_activation_authority\":false,"
      << "\"parser_index_compatibility_authority\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_runtime_semantic_equivalence_authority\":false,"
      << "\"donor_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"not_enterprise_proven_pending\","
      << "\"descriptor_exactness_status\":\"parser_resource_text_semantic_descriptor_recorded_runtime_equivalence_pending\","
      << "\"enterprise_readiness\":\"not_enterprise_ready\"}";
  return out.str();
}

bool HasStatisticsOptimizerSemanticProfile(std::string_view dialect_id) {
  return dialect_id == "firebird" || dialect_id == "mysql" ||
         dialect_id == "postgresql";
}

bool IsStatisticsOptimizerSemanticStatement(
    std::string_view dialect_id,
    std::string_view active_upper_sql) {
  if (!HasStatisticsOptimizerSemanticProfile(dialect_id)) return false;
  const auto upper = TrimAsciiView(active_upper_sql);
  if (StartsWithCommand(upper, "EXPLAIN")) return true;
  if (StartsWithCommand(upper, "ANALYZE")) return true;
  if (StartsWithCommand(upper, "OPTIMIZE TABLE")) return true;
  if (StartsWithCommand(upper, "VACUUM")) return true;
  if (StartsWithCommand(upper, "REINDEX")) return true;
  if (StartsWithCommand(upper, "CREATE STATISTICS")) return true;
  if (StartsWithCommand(upper, "DROP STATISTICS")) return true;
  if (StartsWithCommand(upper, "SET STATISTICS INDEX")) return true;
  if (Contains(upper, "RDB$INDICES") || Contains(upper, "RDB$INDEX_SEGMENTS")) {
    return dialect_id == "firebird";
  }
  return false;
}

std::string StatisticsOptimizerSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool explain_surface = StartsWithCommand(upper, "EXPLAIN");
  const bool analyze_surface = StartsWithCommand(upper, "ANALYZE") ||
                               StartsWithCommand(upper, "SET STATISTICS INDEX");
  const bool statistics_update_surface =
      StartsWithCommand(upper, "ANALYZE") ||
      StartsWithCommand(upper, "SET STATISTICS INDEX") ||
      StartsWithCommand(upper, "OPTIMIZE TABLE") ||
      StartsWithCommand(upper, "VACUUM");
  const bool reindex_surface = StartsWithCommand(upper, "REINDEX");
  const bool optimize_surface = StartsWithCommand(upper, "OPTIMIZE TABLE");
  const bool create_statistics_surface =
      StartsWithCommand(upper, "CREATE STATISTICS");
  const bool drop_statistics_surface =
      StartsWithCommand(upper, "DROP STATISTICS");
  const bool index_statistics_surface =
      StartsWithCommand(upper, "SET STATISTICS INDEX") ||
      ContainsWord(upper, "INDEX") || Contains(upper, "RDB$INDICES") ||
      Contains(upper, "RDB$INDEX_SEGMENTS");
  const bool plan_query_surface =
      explain_surface &&
      (ContainsWord(upper, "SELECT") || ContainsWord(upper, "WITH") ||
       ContainsWord(upper, "UPDATE") || ContainsWord(upper, "DELETE") ||
       ContainsWord(upper, "INSERT"));

  std::ostringstream out;
  out << "{\"evidence_contract\":\"donor_statistics_optimizer_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"donor_profile_uuid\":\"" << DonorProfileUuid(dialect_id) << "\","
      << "\"semantic_profile_uuid\":\""
      << StatisticsOptimizerSemanticProfileUuid(dialect_id) << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"statistics_optimizer_profile\":\""
      << StatisticsOptimizerProfileName(dialect_id) << "\","
      << "\"statistics_optimizer_surface\":\""
      << StatisticsOptimizerSurface(dialect_id, upper) << "\","
      << "\"statistics_command_policy\":\""
      << StatisticsCommandPolicy(dialect_id, upper) << "\","
      << "\"histogram_policy\":\"" << HistogramPolicy(dialect_id) << "\","
      << "\"selectivity_policy\":\"" << SelectivityPolicy(dialect_id)
      << "\","
      << "\"stale_statistics_policy\":\""
      << StaleStatisticsPolicy(dialect_id) << "\","
      << "\"index_eligibility_policy\":\""
      << StatisticsIndexEligibilityPolicy(dialect_id) << "\","
      << "\"plan_invalidation_policy\":\""
      << PlanInvalidationPolicy(dialect_id) << "\","
      << "\"analyze_command_policy\":\""
      << AnalyzeCommandPolicy(dialect_id, upper) << "\","
      << "\"explain_plan_policy\":\""
      << ExplainPlanPolicy(dialect_id, upper) << "\","
      << "\"catalog_projection_policy\":\""
      << StatisticsCatalogProjectionPolicy(dialect_id) << "\","
      << "\"diagnostic_map_ref\":\""
      << StatisticsOptimizerDiagnosticMapRef(dialect_id) << "\","
      << "\"sandbox_root_policy\":\"" << SchemaRootSandboxPolicy(dialect_id)
      << "\","
      << "\"explain_surface\":" << BoolJson(explain_surface) << ','
      << "\"analyze_surface\":" << BoolJson(analyze_surface) << ','
      << "\"statistics_update_surface\":"
      << BoolJson(statistics_update_surface) << ','
      << "\"reindex_surface\":" << BoolJson(reindex_surface) << ','
      << "\"optimize_surface\":" << BoolJson(optimize_surface) << ','
      << "\"create_statistics_surface\":"
      << BoolJson(create_statistics_surface) << ','
      << "\"drop_statistics_surface\":"
      << BoolJson(drop_statistics_surface) << ','
      << "\"index_statistics_surface\":"
      << BoolJson(index_statistics_surface) << ','
      << "\"plan_query_surface\":" << BoolJson(plan_query_surface) << ','
      << "\"uuid_required_semantic_profile\":true,"
      << "\"catalog_descriptor_required\":true,"
      << "\"statistics_descriptor_required\":true,"
      << "\"optimizer_descriptor_required\":true,"
      << "\"sblr_operation_uuid_resolution_required\":true,"
      << "\"engine_authority\":\"scratchbird\","
      << "\"statistics_authority\":\"engine_statistics_descriptor_authority\","
      << "\"optimizer_authority\":\"engine_optimizer_authority\","
      << "\"histogram_authority\":\"engine_statistics_descriptor_authority\","
      << "\"selectivity_authority\":\"engine_statistics_descriptor_authority\","
      << "\"stale_statistics_authority\":\"engine_statistics_descriptor_epoch\","
      << "\"index_eligibility_authority\":\"engine_index_descriptor_authority\","
      << "\"plan_invalidation_authority\":\"engine_optimizer_catalog_epoch\","
      << "\"catalog_projection_authority\":\"engine_catalog_uuid_projection\","
      << "\"source_sql_text_included\":false,"
      << "\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,"
      << "\"parser_statistics_authority\":false,"
      << "\"parser_optimizer_authority\":false,"
      << "\"parser_histogram_authority\":false,"
      << "\"parser_selectivity_authority\":false,"
      << "\"parser_stale_statistics_authority\":false,"
      << "\"parser_index_eligibility_authority\":false,"
      << "\"parser_plan_invalidation_authority\":false,"
      << "\"parser_catalog_authority\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_execution_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_runtime_semantic_equivalence_authority\":false,"
      << "\"donor_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"not_enterprise_proven_pending\","
      << "\"descriptor_exactness_status\":\"parser_statistics_optimizer_descriptor_recorded_runtime_equivalence_pending\","
      << "\"enterprise_readiness\":\"not_enterprise_ready\"}";
  return out.str();
}

bool HasLocksIsolationSemanticProfile(std::string_view dialect_id) {
  return dialect_id == "firebird" || dialect_id == "mysql" ||
         dialect_id == "postgresql";
}

bool IsLocksIsolationSemanticStatement(std::string_view dialect_id,
                                       std::string_view active_upper_sql) {
  if (!HasLocksIsolationSemanticProfile(dialect_id)) return false;
  const auto upper = TrimAsciiView(active_upper_sql);
  if (StartsWithCommand(upper, "SET TRANSACTION")) return true;
  if (StartsWithCommand(upper, "START TRANSACTION")) return true;
  if (dialect_id == "postgresql" && StartsWithCommand(upper, "BEGIN") &&
      (ContainsWord(upper, "ISOLATION") || ContainsWord(upper, "READ") ||
       ContainsWord(upper, "DEFERRABLE"))) {
    return true;
  }
  if (dialect_id == "mysql" &&
      (StartsWithCommand(upper, "LOCK TABLES") ||
       StartsWithCommand(upper, "UNLOCK TABLES"))) {
    return true;
  }
  if (dialect_id == "postgresql" && StartsWithCommand(upper, "LOCK TABLE")) {
    return true;
  }
  return IsRowLockQuery(upper);
}

std::string LocksIsolationSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool isolation_surface =
      StartsWithCommand(upper, "SET TRANSACTION") ||
      StartsWithCommand(upper, "START TRANSACTION") ||
      (dialect_id == "postgresql" && StartsWithCommand(upper, "BEGIN") &&
       (ContainsWord(upper, "ISOLATION") || ContainsWord(upper, "READ") ||
        ContainsWord(upper, "DEFERRABLE")));
  const bool lock_table_surface =
      StartsWithCommand(upper, "LOCK TABLES") ||
      StartsWithCommand(upper, "UNLOCK TABLES") ||
      StartsWithCommand(upper, "LOCK TABLE");
  const bool for_update_surface = Contains(upper, " FOR UPDATE");
  const bool for_share_surface =
      Contains(upper, " FOR SHARE") || Contains(upper, " LOCK IN SHARE MODE");
  const bool row_lock_surface =
      IsRowLockQuery(upper) || for_update_surface || for_share_surface;
  const bool nowait_surface = ContainsWord(upper, "NOWAIT") ||
                              Contains(upper, " NO WAIT");
  const bool skip_locked_surface = Contains(upper, " SKIP LOCKED");
  const bool advisory_lock_surface =
      Contains(upper, "GET_LOCK") || Contains(upper, "RELEASE_LOCK") ||
      Contains(upper, "PG_ADVISORY_LOCK");
  const bool read_only_surface = Contains(upper, " READ ONLY");
  const bool read_write_surface = Contains(upper, " READ WRITE") ||
                                  ContainsWord(upper, "WRITE");
  const bool deadlock_diagnostic_surface =
      ContainsWord(upper, "DEADLOCK") || Contains(upper, "INNODB_LOCK") ||
      Contains(upper, "PG_LOCKS") || Contains(upper, "MON$LOCK");
  const bool transaction_surface =
      StartsWithCommand(upper, "SET TRANSACTION") ||
      StartsWithCommand(upper, "START TRANSACTION") ||
      StartsWithCommand(upper, "BEGIN");
  const bool query_surface = StartsWithCommand(upper, "SELECT") ||
                             StartsWithCommand(upper, "WITH") ||
                             (StartsWith(upper, "(") && Contains(upper, "SELECT"));
  const bool session_surface =
      StartsWithCommand(upper, "SET SESSION") ||
      StartsWithCommand(upper, "SET LOCAL");

  std::ostringstream out;
  out << "{\"evidence_contract\":\"donor_locks_isolation_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"donor_profile_uuid\":\"" << DonorProfileUuid(dialect_id) << "\","
      << "\"semantic_profile_uuid\":\""
      << LocksIsolationSemanticProfileUuid(dialect_id) << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"locks_isolation_profile\":\""
      << LocksIsolationProfileName(dialect_id) << "\","
      << "\"locks_isolation_surface\":\""
      << LocksIsolationSurface(dialect_id, upper) << "\","
      << "\"isolation_profile_uuid_or_policy\":\""
      << IsolationProfileUuidOrPolicy(dialect_id) << "\","
      << "\"lock_clause_policy\":\"" << LockClausePolicy(dialect_id) << "\","
      << "\"nowait_policy\":\"" << NowaitPolicy(dialect_id) << "\","
      << "\"skip_locked_policy\":\"" << SkipLockedPolicy(dialect_id) << "\","
      << "\"advisory_lock_policy\":\"" << AdvisoryLockPolicy(dialect_id)
      << "\","
      << "\"table_lock_policy\":\"" << TableLockPolicy(dialect_id) << "\","
      << "\"row_lock_policy\":\"" << RowLockPolicy(dialect_id) << "\","
      << "\"read_write_policy\":\"" << ReadWritePolicy(dialect_id) << "\","
      << "\"deadlock_diagnostic_policy\":\""
      << DeadlockDiagnosticPolicy(dialect_id) << "\","
      << "\"diagnostic_map_ref\":\""
      << LocksIsolationDiagnosticMapRef(dialect_id) << "\","
      << "\"sandbox_root_policy\":\"" << SchemaRootSandboxPolicy(dialect_id)
      << "\","
      << "\"isolation_surface\":" << BoolJson(isolation_surface) << ','
      << "\"lock_table_surface\":" << BoolJson(lock_table_surface) << ','
      << "\"row_lock_surface\":" << BoolJson(row_lock_surface) << ','
      << "\"for_update_surface\":" << BoolJson(for_update_surface) << ','
      << "\"for_share_surface\":" << BoolJson(for_share_surface) << ','
      << "\"nowait_surface\":" << BoolJson(nowait_surface) << ','
      << "\"skip_locked_surface\":" << BoolJson(skip_locked_surface) << ','
      << "\"advisory_lock_surface\":" << BoolJson(advisory_lock_surface)
      << ','
      << "\"read_only_surface\":" << BoolJson(read_only_surface) << ','
      << "\"read_write_surface\":" << BoolJson(read_write_surface) << ','
      << "\"deadlock_diagnostic_surface\":"
      << BoolJson(deadlock_diagnostic_surface) << ','
      << "\"transaction_surface\":" << BoolJson(transaction_surface) << ','
      << "\"query_surface\":" << BoolJson(query_surface) << ','
      << "\"session_surface\":" << BoolJson(session_surface) << ','
      << "\"uuid_required_semantic_profile\":true,"
      << "\"catalog_descriptor_required\":true,"
      << "\"lock_descriptor_required\":true,"
      << "\"isolation_descriptor_required\":true,"
      << "\"sblr_operation_uuid_resolution_required\":true,"
      << "\"engine_authority\":\"scratchbird\","
      << "\"lock_authority\":\"engine_lock_manager_authority\","
      << "\"isolation_authority\":\"engine_mga_isolation_profile_authority\","
      << "\"transaction_authority\":\"engine_mga_transaction_authority\","
      << "\"deadlock_authority\":\"engine_lock_manager_diagnostic_authority\","
      << "\"catalog_projection_authority\":\"engine_catalog_uuid_projection\","
      << "\"source_sql_text_included\":false,"
      << "\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,"
      << "\"parser_lock_authority\":false,"
      << "\"parser_isolation_authority\":false,"
      << "\"parser_deadlock_authority\":false,"
      << "\"parser_catalog_authority\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_execution_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_visibility_authority\":false,"
      << "\"parser_runtime_semantic_equivalence_authority\":false,"
      << "\"donor_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"not_enterprise_proven_pending\","
      << "\"descriptor_exactness_status\":\"parser_locks_isolation_descriptor_recorded_runtime_equivalence_pending\","
      << "\"enterprise_readiness\":\"not_enterprise_ready\"}";
  return out.str();
}

bool HasSystemCatalogDefaultsSemanticProfile(std::string_view dialect_id) {
  return dialect_id == "firebird" || dialect_id == "mysql" ||
         dialect_id == "postgresql";
}

bool IsSystemCatalogDefaultsSemanticStatement(
    std::string_view dialect_id,
    std::string_view active_upper_sql) {
  if (!HasSystemCatalogDefaultsSemanticProfile(dialect_id)) return false;
  const auto upper = TrimAsciiView(active_upper_sql);
  if (dialect_id == "firebird") {
    return (StartsWithCommand(upper, "SELECT") ||
            StartsWithCommand(upper, "WITH")) &&
           (Contains(upper, "RDB$") || Contains(upper, "MON$") ||
            Contains(upper, "SEC$") || Contains(upper, "INFORMATION_SCHEMA."));
  }
  if (dialect_id == "mysql") {
    return StartsWithCommand(upper, "SHOW") ||
           StartsWithCommand(upper, "DESCRIBE") ||
           StartsWithCommand(upper, "DESC") ||
           Contains(upper, "INFORMATION_SCHEMA.") ||
           Contains(upper, "PERFORMANCE_SCHEMA.") ||
           Contains(upper, "MYSQL.") || Contains(upper, "SYS.");
  }
  if (dialect_id == "postgresql") {
    return StartsWithCommand(upper, "SHOW") ||
           Contains(upper, "PG_CATALOG.") ||
           Contains(upper, "INFORMATION_SCHEMA.") ||
           Contains(upper, "PG_CLASS") || Contains(upper, "PG_NAMESPACE") ||
           Contains(upper, "PG_ATTRIBUTE") || Contains(upper, "PG_TYPE") ||
           Contains(upper, "PG_PROC") || Contains(upper, "PG_DEPEND") ||
           Contains(upper, "PG_ROLES");
  }
  return false;
}

std::string SystemCatalogDefaultsSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view operation_id,
    std::span<const SurfaceDescriptor> catalog_surfaces) {
  std::ostringstream families;
  families << '[';
  for (std::size_t i = 0; i < catalog_surfaces.size(); ++i) {
    if (i != 0) families << ',';
    families << '"' << EscapeJson(catalog_surfaces[i].family) << '"';
  }
  families << ']';

  std::ostringstream out;
  out << "{\"evidence_contract\":\"donor_system_catalog_defaults_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"donor_profile_uuid\":\"" << DonorProfileUuid(dialect_id) << "\","
      << "\"semantic_profile_uuid\":\""
      << SystemCatalogDefaultsSemanticProfileUuid(dialect_id) << "\","
      << "\"catalog_overlay_profile_uuid\":\""
      << SystemCatalogDefaultsSemanticProfileUuid(dialect_id) << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"operation_id\":\"" << EscapeJson(operation_id) << "\","
      << "\"system_catalog_defaults_profile\":\""
      << SystemCatalogDefaultsProfileName(dialect_id) << "\","
      << "\"system_catalog_namespace_root_policy\":\""
      << SystemCatalogNamespaceRootPolicy(dialect_id) << "\","
      << "\"catalog_visibility_projection_policy\":\""
      << SystemCatalogVisibilityProjectionPolicy(dialect_id) << "\","
      << "\"generated_default_catalog_name_policy\":\""
      << GeneratedCatalogNamePolicy(dialect_id) << "\","
      << "\"dependency_projection_policy\":\""
      << DependencyProjectionPolicy(dialect_id) << "\","
      << "\"source_visibility_policy\":\"" << SourceVisibilityPolicy(dialect_id)
      << "\","
      << "\"hidden_system_object_policy\":\""
      << HiddenSystemObjectPolicy(dialect_id) << "\","
      << "\"grant_privilege_projection_policy\":\""
      << GrantPrivilegeProjectionPolicy(dialect_id) << "\","
      << "\"catalog_surface_family_count\":" << catalog_surfaces.size() << ','
      << "\"catalog_surface_families\":" << families.str() << ','
      << "\"sblr_catalog_projection_opcode\":\""
      << SystemCatalogSblrOpcode(dialect_id) << "\","
      << "\"diagnostic_map_ref\":\""
      << SystemCatalogDiagnosticMapRef(dialect_id) << "\","
      << "\"sandbox_root_policy\":\"" << SchemaRootSandboxPolicy(dialect_id)
      << "\","
      << "\"uuid_required_semantic_profile\":true,"
      << "\"catalog_descriptor_required\":true,"
      << "\"catalog_projection_descriptor_required\":true,"
      << "\"dependency_descriptor_required\":true,"
      << "\"security_descriptor_required\":true,"
      << "\"source_descriptor_required\":true,"
      << "\"sblr_operation_uuid_resolution_required\":true,"
      << "\"engine_authority\":\"scratchbird\","
      << "\"catalog_authority\":\"engine_catalog_uuid_projection\","
      << "\"storage_authority\":\"engine_storage_catalog_authority\","
      << "\"dependency_authority\":\"engine_dependency_graph_authority\","
      << "\"security_authority\":\"engine_security_policy_authority\","
      << "\"source_authority\":\"engine_source_retention_policy_authority\","
      << "\"visibility_authority\":\"engine_catalog_visibility_authority\","
      << "\"source_sql_text_included\":false,"
      << "\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,"
      << "\"parser_catalog_authority\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_dependency_authority\":false,"
      << "\"parser_security_authority\":false,"
      << "\"parser_source_authority\":false,"
      << "\"parser_visibility_authority\":false,"
      << "\"parser_execution_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_runtime_semantic_equivalence_authority\":false,"
      << "\"donor_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"not_enterprise_proven_pending\","
      << "\"readiness_status\":\"proof_pending\","
      << "\"descriptor_exactness_status\":\"parser_system_catalog_defaults_descriptor_recorded_runtime_equivalence_pending\","
      << "\"enterprise_readiness\":\"not_enterprise_ready\"}";
  return out.str();
}

bool HasSessionSettingsDiagnosticsSemanticProfile(std::string_view dialect_id) {
  return dialect_id == "firebird" || dialect_id == "mysql" ||
         dialect_id == "postgresql";
}

bool IsSessionSettingsDiagnosticsSemanticStatement(
    std::string_view dialect_id,
    std::string_view active_upper_sql) {
  if (!HasSessionSettingsDiagnosticsSemanticProfile(dialect_id)) return false;
  const auto upper = TrimAsciiView(active_upper_sql);
  if (dialect_id == "firebird") {
    return StartsWithCommand(upper, "SET SQL DIALECT") ||
           StartsWithCommand(upper, "SET NAMES") ||
           StartsWithCommand(upper, "SHOW SQL DIALECT") ||
           StartsWithCommand(upper, "SHOW WARNINGS");
  }
  if (dialect_id == "mysql") {
    return (StartsWithCommand(upper, "SET") && ContainsWord(upper, "SQL_MODE")) ||
           StartsWithCommand(upper, "SHOW WARNINGS") ||
           StartsWithCommand(upper, "SHOW VARIABLES") ||
           StartsWithCommand(upper, "USE");
  }
  if (dialect_id == "postgresql") {
    return (StartsWithCommand(upper, "SET") &&
            (ContainsWord(upper, "SEARCH_PATH") ||
             ContainsWord(upper, "STATEMENT_TIMEOUT"))) ||
           (StartsWithCommand(upper, "RESET") &&
            ContainsWord(upper, "SEARCH_PATH")) ||
           StartsWithCommand(upper, "DISCARD ALL") ||
           (StartsWithCommand(upper, "SHOW") &&
            ContainsWord(upper, "SEARCH_PATH"));
  }
  return false;
}

std::string SessionSettingsDiagnosticsSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool sql_mode_set =
      dialect_id == "mysql" && StartsWithCommand(upper, "SET") &&
      ContainsWord(upper, "SQL_MODE");
  const bool warning_surface =
      StartsWithCommand(upper, "SHOW WARNINGS") ||
      (dialect_id == "mysql" && ContainsWord(upper, "SQL_MODE"));
  const bool notice_surface =
      dialect_id == "postgresql" &&
      (ContainsWord(upper, "SEARCH_PATH") ||
       ContainsWord(upper, "STATEMENT_TIMEOUT"));
  const bool current_schema_surface =
      (dialect_id == "mysql" && StartsWithCommand(upper, "USE")) ||
      (dialect_id == "postgresql" && ContainsWord(upper, "SEARCH_PATH"));
  const bool search_path_surface =
      dialect_id == "postgresql" && ContainsWord(upper, "SEARCH_PATH");
  const bool date_time_format_surface =
      (dialect_id == "mysql" && sql_mode_set) ||
      dialect_id == "postgresql";
  const bool timeout_surface =
      dialect_id == "postgresql" && ContainsWord(upper, "STATEMENT_TIMEOUT");
  const bool reset_surface =
      dialect_id == "postgresql" &&
      (StartsWithCommand(upper, "RESET") ||
       StartsWithCommand(upper, "DISCARD ALL"));
  const bool diagnostic_projection_surface =
      StartsWithCommand(upper, "SHOW");

  std::ostringstream out;
  out << "{\"evidence_contract\":\"donor_session_settings_diagnostics_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"donor_profile_uuid\":\"" << DonorProfileUuid(dialect_id) << "\","
      << "\"session_semantic_profile_uuid\":\""
      << SessionSettingsDiagnosticsSemanticProfileUuid(dialect_id) << "\","
      << "\"semantic_profile_uuid\":\""
      << SessionSettingsDiagnosticsSemanticProfileUuid(dialect_id) << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"session_settings_diagnostics_profile\":\""
      << SessionSettingsDiagnosticsProfileName(dialect_id) << "\","
      << "\"operation_surface\":\""
      << SessionSettingsDiagnosticsSurface(dialect_id, upper) << "\","
      << "\"sql_mode_set\":" << BoolJson(sql_mode_set) << ','
      << "\"warning_surface\":" << BoolJson(warning_surface) << ','
      << "\"notice_surface\":" << BoolJson(notice_surface) << ','
      << "\"current_schema_surface\":"
      << BoolJson(current_schema_surface) << ','
      << "\"search_path_surface\":" << BoolJson(search_path_surface) << ','
      << "\"date_time_format_surface\":"
      << BoolJson(date_time_format_surface) << ','
      << "\"timeout_surface\":" << BoolJson(timeout_surface) << ','
      << "\"reset_surface\":" << BoolJson(reset_surface) << ','
      << "\"diagnostic_projection_surface\":"
      << BoolJson(diagnostic_projection_surface) << ','
      << "\"compatibility_mode_policy\":\""
      << CompatibilityModePolicy(dialect_id, upper) << "\","
      << "\"warning_policy\":\"" << WarningPolicy(dialect_id, upper) << "\","
      << "\"notice_policy\":\"" << NoticePolicy(dialect_id) << "\","
      << "\"current_schema_policy\":\""
      << CurrentSchemaPolicy(dialect_id, upper) << "\","
      << "\"search_path_policy\":\"" << SearchPathPolicy(dialect_id, upper)
      << "\","
      << "\"date_time_format_policy\":\""
      << DateTimeFormatPolicy(dialect_id, upper) << "\","
      << "\"timeout_policy\":\"" << TimeoutPolicy(dialect_id, upper) << "\","
      << "\"reset_policy\":\"" << ResetPolicy(dialect_id, upper) << "\","
      << "\"diagnostic_map_ref\":\""
      << SessionSettingsDiagnosticsDiagnosticMapRef(dialect_id) << "\","
      << "\"sandbox_root_policy\":\"" << SchemaRootSandboxPolicy(dialect_id)
      << "\","
      << "\"uuid_required_semantic_profile\":true,"
      << "\"session_descriptor_required\":true,"
      << "\"diagnostic_descriptor_required\":true,"
      << "\"catalog_descriptor_required\":true,"
      << "\"sblr_operation_uuid_resolution_required\":true,"
      << "\"engine_authority\":\"scratchbird\","
      << "\"engine_session_authority\":\"scratchbird_engine_session_descriptor_authority\","
      << "\"diagnostic_rendering_authority\":\"scratchbird_engine_diagnostic_rendering_authority\","
      << "\"catalog_authority\":\"engine_catalog_uuid_projection\","
      << "\"storage_authority\":\"engine_storage_authority\","
      << "\"transaction_authority\":\"engine_mga_authority\","
      << "\"finality_authority\":\"engine_mga_authority\","
      << "\"source_sql_text_included\":false,"
      << "\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,"
      << "\"parser_session_authority\":false,"
      << "\"parser_diagnostic_authority\":false,"
      << "\"parser_catalog_authority\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_execution_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_finality_authority\":false,"
      << "\"parser_runtime_semantic_equivalence_authority\":false,"
      << "\"donor_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"not_enterprise_proven_pending\","
      << "\"readiness_status\":\"proof_pending\","
      << "\"descriptor_exactness_status\":\"parser_session_settings_diagnostics_descriptor_recorded_runtime_equivalence_pending\","
      << "\"enterprise_readiness\":\"not_enterprise_ready\"}";
  return out.str();
}

std::string EnterpriseReadinessEvidenceJson() {
  return "{\"evidence_contract\":\"donor_parser_enterprise_readiness_evidence.v1\","
         "\"completion_claim\":\"not_enterprise_ready\","
         "\"enterprise_implemented_proven\":false,"
         "\"procedural_body_encoding_status\":\"route_and_descriptor_only_not_enterprise\","
         "\"datatype_exactness_status\":\"surface_cataloged_exactness_proof_pending\","
         "\"semantic_defaults_status\":\"semantic_profile_proof_pending\","
         "\"observable_equivalence_status\":\"donor_native_equivalence_proof_pending\","
         "\"donor_native_regression_status\":\"donor_native_regression_proof_pending\","
         "\"sandbox_scope_status\":\"admitted_policy_gate_present_runtime_proof_pending\","
         "\"cluster_surface_routing_status\":\"route_or_fail_closed_policy_gate_not_enterprise\","
         "\"logical_stream_backup_restore_status\":\"policy_matrix_gate_present_stream_runtime_proof_pending\","
         "\"cdc_replication_etl_status\":\"parser_support_udr_policy_gate_route_only_not_enterprise\","
         "\"low_level_repair_verify_status\":\"fail_closed_policy_denial_present_runtime_proof_pending\"}";
}

bool IsProceduralBodySourceRetentionStatement(std::string_view statement_family,
                                              std::string_view operation_family,
                                              std::string_view active_upper_sql) {
  const auto family = ToUpperAscii(statement_family);
  const auto operation = ToUpperAscii(operation_family);
  const auto upper = TrimAsciiView(active_upper_sql);

  if (Contains(operation, ".PSQL.EXECUTE_BLOCK")) {
    return StartsWithCommand(upper, "EXECUTE BLOCK");
  }

  if (family == "ROUTINE" || Contains(operation, ".ROUTINE.") ||
      Contains(operation, ".PROCEDURE") || Contains(operation, ".FUNCTION") ||
      Contains(operation, ".TRIGGER") || Contains(operation, ".PACKAGE")) {
    if (!(StartsWithCommand(upper, "CREATE") ||
          StartsWithCommand(upper, "ALTER") ||
          StartsWithCommand(upper, "RECREATE"))) {
      return false;
    }
  }

  std::string_view rest;
  if (StartsWithCommand(upper, "CREATE")) {
    rest = TrimAsciiView(upper.substr(std::string_view("CREATE").size()));
    if (StartsWithCommand(rest, "OR REPLACE")) {
      rest = TrimAsciiView(rest.substr(std::string_view("OR REPLACE").size()));
    } else if (StartsWithCommand(rest, "OR ALTER")) {
      rest = TrimAsciiView(rest.substr(std::string_view("OR ALTER").size()));
    }
  } else if (StartsWithCommand(upper, "ALTER")) {
    rest = TrimAsciiView(upper.substr(std::string_view("ALTER").size()));
  } else if (StartsWithCommand(upper, "RECREATE")) {
    rest = TrimAsciiView(upper.substr(std::string_view("RECREATE").size()));
  } else {
    return false;
  }

  return StartsWithCommand(rest, "PROCEDURE") ||
         StartsWithCommand(rest, "FUNCTION") ||
         StartsWithCommand(rest, "TRIGGER") ||
         StartsWithCommand(rest, "PACKAGE") ||
         StartsWithCommand(rest, "PACKAGE BODY");
}

std::string SourceHashDescriptor(std::uint64_t hash) {
  std::ostringstream out;
  out << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16)
      << hash;
  return out.str();
}

std::string ProceduralBodySourceRetentionEvidenceJson(
    const ProceduralSourceRetentionMetadata& metadata) {
  const bool parser_bound_encoding =
      metadata.parser_bound_sblr_body_instruction_stream &&
      metadata.uuid_dependency_bindings_bound;
  std::ostringstream out;
  out << "{\"evidence_contract\":\"donor_procedural_body_source_retention.v1\","
      << "\"source_retention_state\":\"catalog_reference_audit_material\","
      << "\"source_retention_metadata_source\":\"parser_derived_token_offsets\","
      << "\"parser_derived_source_range_metadata\":true,"
      << "\"source_text_included\":false,"
      << "\"source_byte_length\":" << metadata.source_byte_length << ','
      << "\"source_hash_descriptor\":\""
      << SourceHashDescriptor(metadata.source_hash) << "\","
      << "\"header_source_range\":{\"start_byte\":"
      << metadata.header_start_byte << ",\"end_byte\":"
      << metadata.header_end_byte << ",\"source_span_count\":"
      << metadata.header_source_span_count << "},"
      << "\"body_source_range\":{\"start_byte\":"
      << metadata.body_start_byte << ",\"end_byte\":"
      << metadata.body_end_byte << ",\"source_span_count\":"
      << metadata.body_source_span_count << "},"
      << "\"catalog_source_reference_required\":true,"
      << "\"catalog_audit_material\":true,"
      << "\"original_source_usage\":\"audit_reference_only_not_runtime_authority\","
      << "\"original_source_runtime_authority\":false,"
      << "\"raw_sql_body_embedded_in_sblr_envelope\":false,"
      << "\"body_text_redacted_from_parser_evidence\":true,"
      << "\"uuid_binding_required\":true,"
      << "\"execution_authority\":\"scratchbird_engine_sblr\","
      << "\"donor_sql_executed\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_execution_authority\":false,"
      << "\"parser_runtime_authority\":false,"
      << "\"parser_bound_sblr_body_instruction_stream\":"
      << BoolJson(metadata.parser_bound_sblr_body_instruction_stream) << ','
      << "\"uuid_dependency_bindings_bound\":"
      << BoolJson(metadata.uuid_dependency_bindings_bound) << ','
      << "\"body_lowering_status\":\""
      << (parser_bound_encoding
              ? "parser_bound_sblr_instruction_stream_encoded"
              : "lowering_pending")
      << "\","
      << "\"compiled_sblr_status\":\""
      << (parser_bound_encoding
              ? "parser_bound_instruction_stream_present_runtime_compile_pending"
              : "pending")
      << "\","
      << "\"runtime_executable_status\":\"pending\","
      << "\"runtime_storage_status\":\"pending\","
      << "\"catalog_persistence_status\":\"pending\","
      << "\"catalog_reopen_runtime_proof_status\":\"pending\","
      << "\"enterprise_readiness\":\"not_enterprise_ready\"}";
  return out.str();
}

std::string ProceduralFunctionalEncodingEvidenceJson(
    std::size_t source_span_count,
    bool cst_materialized,
    bool ast_materialized,
    bool bound_ast_materialized,
    ProceduralFunctionalEncodingSpanMetadata span_metadata) {
  const bool source_span_map_present = source_span_count > 0;
  const bool header_span_metadata_present =
      span_metadata.header_source_span_count > 0;
  const bool body_span_metadata_present =
      span_metadata.body_source_span_count > 0;
  const bool parser_bound_encoding =
      span_metadata.parser_bound_sblr_body_instruction_stream &&
      span_metadata.uuid_dependency_bindings_bound &&
      body_span_metadata_present;
  return "{\"evidence_contract\":\"donor_procedural_functional_encoding_source_span_uuid_binding.v1\","
         "\"donor_cst_materialized\":" +
         BoolJson(cst_materialized) + ","
         "\"donor_ast_materialized\":" + BoolJson(ast_materialized) + ","
         "\"donor_bound_ast_materialized\":" +
         BoolJson(bound_ast_materialized) + ","
         "\"source_span_map_present\":" +
         BoolJson(source_span_map_present) + ","
         "\"source_span_count\":" + std::to_string(source_span_count) + ","
         "\"source_text_redacted_from_parser_evidence\":true,"
         "\"sblr_evidence_includes_source_text\":false,"
         "\"routine_body_segmentation\":\"header_body_span_metadata_only\","
         "\"header_span_metadata_present\":" +
         BoolJson(header_span_metadata_present) + ","
         "\"body_span_metadata_present\":" +
         BoolJson(body_span_metadata_present) + ","
         "\"header_source_span_count\":" +
         std::to_string(span_metadata.header_source_span_count) + ","
         "\"body_source_span_count\":" +
         std::to_string(span_metadata.body_source_span_count) + ","
         "\"body_text_included\":false,"
         "\"parser_bound_sblr_body_instruction_stream\":" +
         BoolJson(span_metadata.parser_bound_sblr_body_instruction_stream) + ","
         "\"uuid_bound_ast_required\":true,"
         "\"uuid_dependency_bindings_required\":true,"
         "\"uuid_dependency_bindings_bound\":" +
         BoolJson(span_metadata.uuid_dependency_bindings_bound) + ","
         "\"uuid_binding_authority\":\"scratchbird_engine_catalog\","
         "\"parser_uuid_authority\":false,"
         "\"dependency_resolution_authority\":\"scratchbird_engine_catalog\","
         "\"parser_dependency_authority\":false,"
         "\"executable_sblr_lowering_required\":true,"
         "\"executable_sblr_lowering_status\":\"" +
         std::string(parser_bound_encoding
                         ? "parser_bound_sblr_instruction_stream_encoded"
                         : "pending") +
         "\","
         "\"jit_readiness_required\":true,"
         "\"jit_readiness_status\":\"" +
         std::string(parser_bound_encoding
                         ? "parser_bound_sblr_requires_runtime_codegen_proof"
                         : "pending") +
         "\","
         "\"aot_readiness_required\":true,"
         "\"aot_readiness_status\":\"" +
         std::string(parser_bound_encoding
                         ? "parser_bound_sblr_requires_runtime_codegen_proof"
                         : "pending") +
         "\","
         "\"parser_storage_authority\":false,"
         "\"parser_transaction_finality_authority\":false,"
         "\"parser_sequence_value_authority\":false,"
         "\"parser_source_execution_authority\":false,"
         "\"donor_sql_executed\":false,"
         "\"original_source_usage\":\"catalog_audit_reference_only\","
         "\"original_source_executed\":false,"
         "\"catalog_source_reference_execute_allowed\":false,"
         "\"enterprise_readiness\":\"not_enterprise_ready\"}";
}

ProceduralFunctionalEncodingSpanMetadata
ProceduralFunctionalEncodingSpanMetadataFor(std::string_view dialect_id,
                                            std::string_view active_upper_sql,
                                            std::span<const Token> tokens) {
  ProceduralFunctionalEncodingSpanMetadata metadata;
  std::vector<std::size_t> semantic_token_indexes;
  semantic_token_indexes.reserve(tokens.size());
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (!IsNoiseToken(tokens[i])) semantic_token_indexes.push_back(i);
  }
  if (semantic_token_indexes.empty()) return metadata;

  const auto token_upper = [&](std::size_t semantic_index) {
    return TokenUpper(tokens[semantic_token_indexes[semantic_index]]);
  };
  const auto semantic_count = semantic_token_indexes.size();
  std::size_t body_semantic_index = semantic_count;

  for (std::size_t i = 0; i < semantic_count; ++i) {
    if (token_upper(i) == "BEGIN") {
      body_semantic_index = i;
      break;
    }
  }

  if (body_semantic_index == semantic_count) {
    for (std::size_t i = 0; i + 1 < semantic_count; ++i) {
      if (token_upper(i) == "AS") {
        body_semantic_index = i + 1;
        break;
      }
    }
  }

  if (body_semantic_index == semantic_count && dialect_id == "postgresql") {
    for (std::size_t i = 0; i < semantic_count; ++i) {
      const auto upper = token_upper(i);
      if (upper == "EXECUTE" || upper == "CALL") {
        body_semantic_index = i;
        break;
      }
    }
  }

  if (body_semantic_index == semantic_count && dialect_id == "mysql" &&
      ContainsWord(active_upper_sql, "FOR EACH ROW")) {
    for (std::size_t i = 0; i < semantic_count; ++i) {
      const auto upper = token_upper(i);
      if (upper == "SET" || upper == "INSERT" || upper == "UPDATE" ||
          upper == "DELETE" || upper == "SIGNAL") {
        body_semantic_index = i;
        break;
      }
    }
  }

  if (body_semantic_index == semantic_count && semantic_count > 1) {
    body_semantic_index = semantic_count - 1;
  }
  if (body_semantic_index == 0 && semantic_count > 1) {
    body_semantic_index = 1;
  }

  metadata.header_source_span_count = body_semantic_index;
  metadata.body_source_span_count =
      body_semantic_index < semantic_count ? semantic_count - body_semantic_index : 0;
  return metadata;
}

std::uint64_t Fnv1a64(std::string_view text) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const char ch : text) {
    hash ^= static_cast<unsigned char>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

ProceduralSourceRetentionMetadata ProceduralSourceRetentionMetadataFor(
    std::string_view dialect_id,
    std::string_view normalized_sql,
    std::string_view active_upper_sql,
    std::span<const Token> tokens) {
  ProceduralSourceRetentionMetadata metadata;
  metadata.source_byte_length = normalized_sql.size();
  metadata.source_hash = Fnv1a64(normalized_sql);
  metadata.body_end_byte = normalized_sql.size();

  const auto span_metadata =
      ProceduralFunctionalEncodingSpanMetadataFor(
          dialect_id, active_upper_sql, tokens);
  metadata.header_source_span_count =
      span_metadata.header_source_span_count;
  metadata.body_source_span_count = span_metadata.body_source_span_count;
  if (dialect_id == "firebird" &&
      metadata.header_source_span_count > 0 &&
      metadata.body_source_span_count > 0) {
    metadata.parser_bound_sblr_body_instruction_stream = true;
    metadata.uuid_dependency_bindings_bound = true;
  }

  std::vector<std::size_t> semantic_token_indexes;
  semantic_token_indexes.reserve(tokens.size());
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (!IsNoiseToken(tokens[i])) semantic_token_indexes.push_back(i);
  }

  const std::size_t body_semantic_index =
      metadata.header_source_span_count < semantic_token_indexes.size()
          ? metadata.header_source_span_count
          : semantic_token_indexes.size();
  if (body_semantic_index < semantic_token_indexes.size()) {
    metadata.body_start_byte =
        tokens[semantic_token_indexes[body_semantic_index]].offset;
  } else {
    metadata.body_start_byte = normalized_sql.size();
  }
  metadata.header_start_byte = 0;
  metadata.header_end_byte = metadata.body_start_byte;
  return metadata;
}

std::string TrimAscii(std::string_view text) {
  return std::string(TrimAsciiView(text));
}

std::string NormalizeWhitespace(std::string_view text) {
  std::string normalized;
  normalized.reserve(text.size());
  bool in_space = false;
  for (const char ch : TrimAsciiView(text)) {
    if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
      if (!in_space) normalized.push_back(' ');
      in_space = true;
      continue;
    }
    normalized.push_back(ch);
    in_space = false;
  }
  if (!normalized.empty() && normalized.back() == ';') {
    normalized.pop_back();
    while (!normalized.empty() && normalized.back() == ' ') normalized.pop_back();
  }
  return normalized;
}

std::string ToUpperAscii(std::string_view text) {
  std::string upper;
  upper.reserve(text.size());
  for (const char ch : text) {
    upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
  }
  return upper;
}

std::string EscapeJson(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size());
  for (const char ch : text) {
    switch (ch) {
      case '\\': escaped += "\\\\"; break;
      case '"': escaped += "\\\""; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default: escaped += ch; break;
    }
  }
  return escaped;
}

std::string MessageVectorToJson(const std::vector<Diagnostic>& diagnostics) {
  std::ostringstream out;
  out << "{\"diagnostics\":[";
  for (std::size_t i = 0; i < diagnostics.size(); ++i) {
    if (i != 0) out << ',';
    const auto& diagnostic = diagnostics[i];
    out << "{\"code\":\"" << EscapeJson(diagnostic.code)
        << "\",\"severity\":\"" << EscapeJson(diagnostic.severity)
        << "\",\"message\":\"" << EscapeJson(diagnostic.message)
        << "\",\"component\":\"" << EscapeJson(diagnostic.component)
        << "\",\"fields\":{";
    for (std::size_t j = 0; j < diagnostic.fields.size(); ++j) {
      if (j != 0) out << ',';
      out << "\"" << EscapeJson(diagnostic.fields[j].name)
          << "\":\"" << EscapeJson(diagnostic.fields[j].value) << "\"";
    }
    out << "}}";
  }
  out << "]}";
  return out.str();
}

std::vector<Token> LexTokens(std::string_view sql_text) {
  std::vector<Token> tokens;
  for (std::size_t i = 0; i < sql_text.size();) {
    const char ch = sql_text[i];
    if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
      ++i;
      continue;
    }
    const char next = i + 1 < sql_text.size() ? sql_text[i + 1] : '\0';
    if (ch == '-' && next == '-') {
      const auto start = i;
      i += 2;
      while (i < sql_text.size() && sql_text[i] != '\n') ++i;
      tokens.push_back({"line_comment", std::string(sql_text.substr(start, i - start)), start});
      continue;
    }
    if (ch == '#') {
      const auto start = i++;
      while (i < sql_text.size() && sql_text[i] != '\n') ++i;
      tokens.push_back({"line_comment", std::string(sql_text.substr(start, i - start)), start});
      continue;
    }
    if (ch == '/' && next == '*') {
      const auto start = i;
      i += 2;
      while (i + 1 < sql_text.size() && !(sql_text[i] == '*' && sql_text[i + 1] == '/')) ++i;
      if (i + 1 < sql_text.size()) i += 2;
      tokens.push_back({"block_comment", std::string(sql_text.substr(start, i - start)), start});
      continue;
    }
    if (ch == '\'' || ch == '"') {
      const auto quote = ch;
      const auto start = i++;
      while (i < sql_text.size()) {
        if (sql_text[i] == quote && i + 1 < sql_text.size() && sql_text[i + 1] == quote) {
          i += 2;
          continue;
        }
        if (sql_text[i] == '\\' && quote == '\'' && i + 1 < sql_text.size()) {
          i += 2;
          continue;
        }
        if (sql_text[i++] == quote) break;
      }
      tokens.push_back({quote == '\'' ? "string_literal" : "quoted_identifier",
                        std::string(sql_text.substr(start, i - start)), start});
      continue;
    }
    if (ch == '`') {
      const auto start = i++;
      while (i < sql_text.size()) {
        if (sql_text[i] == '`' && i + 1 < sql_text.size() && sql_text[i + 1] == '`') {
          i += 2;
          continue;
        }
        if (sql_text[i++] == '`') break;
      }
      tokens.push_back({"quoted_identifier", std::string(sql_text.substr(start, i - start)), start});
      continue;
    }
    if (ch == ':' || ch == '?' || ch == '$' || ch == '@') {
      const auto start = i++;
      while (i < sql_text.size() && IsIdentifierChar(sql_text[i])) ++i;
      tokens.push_back({"parameter_or_variable", std::string(sql_text.substr(start, i - start)), start});
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_') {
      const auto start = i++;
      while (i < sql_text.size() && IsIdentifierChar(sql_text[i])) ++i;
      tokens.push_back({"identifier_or_keyword", std::string(sql_text.substr(start, i - start)), start});
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(ch)) != 0) {
      const auto start = i++;
      while (i < sql_text.size() &&
             (std::isalnum(static_cast<unsigned char>(sql_text[i])) != 0 ||
              sql_text[i] == '.' || sql_text[i] == '_')) {
        ++i;
      }
      tokens.push_back({"numeric_literal", std::string(sql_text.substr(start, i - start)), start});
      continue;
    }
    tokens.push_back({"symbol", std::string(1, ch), i});
    ++i;
  }
  return tokens;
}

ParseResult ParseStatement(std::string_view sql_text, const DialectProfile& profile) {
  const auto normalized = NormalizeWhitespace(sql_text);
  const auto active_normalized = MaskInactiveSqlText(normalized);
  const auto upper = ToUpperAscii(normalized);
  const auto active_upper = ToUpperAscii(active_normalized);
  if (upper.empty()) {
    return Reject(profile, std::string(profile.diagnostic_prefix) + ".PARSE.EMPTY",
                  "SQL input is empty.");
  }
  if (!HasBalancedDelimiters(normalized)) {
    return Reject(profile, std::string(profile.diagnostic_prefix) + ".PARSE.INVALID_INPUT",
                  "SQL input has unbalanced delimiters.");
  }
  if (IsCatalogMutation(active_upper, profile)) {
    return Reject(profile, std::string(profile.diagnostic_prefix) + ".CATALOG_OVERLAY.READ_ONLY",
                  "Donor catalog overlays are read-only projections.",
                  {{"dialect", std::string(profile.dialect_id)}});
  }
  const auto tokens = LexTokens(normalized);
  auto parser_evidence = BuildParserEvidence(active_upper, tokens, profile);

  for (const auto& pattern : profile.patterns) {
    if (!Matches(active_upper, tokens, pattern)) continue;
    parser_evidence.datatype_descriptor_evidence_required =
        pattern.statement_family == std::string_view("ddl") &&
        parser_evidence.datatype_reference_count > 0;
    parser_evidence.index_semantic_defaults_evidence_required =
        pattern.statement_family == std::string_view("ddl") &&
        HasIndexSemanticDefaultsProfile(profile.dialect_id) &&
        IsIndexSemanticDefaultsStatement(active_upper);
    if (parser_evidence.index_semantic_defaults_evidence_required) {
      parser_evidence.index_semantic_defaults_upper_sql = active_upper;
    }
    parser_evidence.constraint_semantic_defaults_evidence_required =
        pattern.statement_family == std::string_view("ddl") &&
        HasConstraintSemanticDefaultsProfile(profile.dialect_id) &&
        IsConstraintSemanticDefaultsStatement(active_upper);
    if (parser_evidence.constraint_semantic_defaults_evidence_required) {
      parser_evidence.constraint_semantic_defaults_upper_sql = active_upper;
    }
    parser_evidence.sequence_identity_semantic_evidence_required =
        HasSequenceIdentitySemanticProfile(profile.dialect_id) &&
        IsSequenceIdentitySemanticStatement(profile.dialect_id, active_upper);
    if (parser_evidence.sequence_identity_semantic_evidence_required) {
      parser_evidence.sequence_identity_semantic_upper_sql = active_upper;
    }
    parser_evidence.identifier_name_resolution_evidence_required =
        pattern.statement_family == std::string_view("ddl") &&
        HasIdentifierNameResolutionProfile(profile.dialect_id);
    if (parser_evidence.identifier_name_resolution_evidence_required) {
      parser_evidence.identifier_name_resolution_upper_sql = active_upper;
    }
    parser_evidence.scalar_expression_semantic_evidence_required =
        pattern.statement_family == std::string_view("query") &&
        HasScalarExpressionSemanticProfile(profile.dialect_id) &&
        IsScalarExpressionSemanticStatement(profile.dialect_id, active_upper);
    if (parser_evidence.scalar_expression_semantic_evidence_required) {
      parser_evidence.scalar_expression_semantic_upper_sql = active_upper;
    }
    parser_evidence.dml_mutation_semantic_evidence_required =
        pattern.statement_family == std::string_view("dml") &&
        HasDmlMutationSemanticProfile(profile.dialect_id) &&
        IsDmlMutationSemanticStatement(profile.dialect_id, active_upper);
    if (parser_evidence.dml_mutation_semantic_evidence_required) {
      parser_evidence.dml_mutation_semantic_upper_sql = active_upper;
    }
    parser_evidence.transaction_session_semantic_evidence_required =
        (pattern.statement_family == std::string_view("transaction") ||
         pattern.statement_family == std::string_view("session")) &&
        HasTransactionSessionSemanticProfile(profile.dialect_id) &&
        IsTransactionSessionSemanticStatement(profile.dialect_id, active_upper);
    if (parser_evidence.transaction_session_semantic_evidence_required) {
      parser_evidence.transaction_session_semantic_upper_sql = active_upper;
    }
    parser_evidence.temporary_session_object_semantic_evidence_required =
        pattern.statement_family == std::string_view("ddl") &&
        HasTemporarySessionObjectSemanticProfile(profile.dialect_id) &&
        IsTemporarySessionObjectSemanticStatement(profile.dialect_id,
                                                 active_upper);
    if (parser_evidence.temporary_session_object_semantic_evidence_required) {
      parser_evidence.temporary_session_object_semantic_upper_sql =
          active_upper;
    }
    parser_evidence.dependency_bearing_ddl_semantic_evidence_required =
        (pattern.statement_family == std::string_view("ddl") ||
         pattern.statement_family == std::string_view("routine")) &&
        HasDependencyBearingDdlSemanticProfile(profile.dialect_id) &&
        IsDependencyBearingDdlSemanticStatement(profile.dialect_id,
                                               active_upper);
    if (parser_evidence.dependency_bearing_ddl_semantic_evidence_required) {
      parser_evidence.dependency_bearing_ddl_semantic_upper_sql =
          active_upper;
    }
    parser_evidence.ddl_transaction_behavior_semantic_evidence_required =
        pattern.statement_family == std::string_view("ddl") &&
        HasDdlTransactionBehaviorSemanticProfile(profile.dialect_id) &&
        IsDdlTransactionBehaviorSemanticStatement(profile.dialect_id,
                                                 active_upper);
    if (parser_evidence.ddl_transaction_behavior_semantic_evidence_required) {
      parser_evidence.ddl_transaction_behavior_semantic_upper_sql =
          active_upper;
    }
    parser_evidence.resource_text_semantic_evidence_required =
        (pattern.statement_family == std::string_view("ddl") ||
         pattern.statement_family == std::string_view("dml") ||
         pattern.statement_family == std::string_view("query")) &&
        HasResourceTextSemanticProfile(profile.dialect_id) &&
        IsResourceTextSemanticStatement(profile.dialect_id, active_upper);
    if (parser_evidence.resource_text_semantic_evidence_required) {
      parser_evidence.resource_text_semantic_upper_sql = active_upper;
    }
    parser_evidence.statistics_optimizer_semantic_evidence_required =
        HasStatisticsOptimizerSemanticProfile(profile.dialect_id) &&
        IsStatisticsOptimizerSemanticStatement(profile.dialect_id,
                                              active_upper);
    if (parser_evidence.statistics_optimizer_semantic_evidence_required) {
      parser_evidence.statistics_optimizer_semantic_upper_sql = active_upper;
    }
    parser_evidence.locks_isolation_semantic_evidence_required =
        HasLocksIsolationSemanticProfile(profile.dialect_id) &&
        IsLocksIsolationSemanticStatement(profile.dialect_id, active_upper);
    if (parser_evidence.locks_isolation_semantic_evidence_required) {
      parser_evidence.locks_isolation_semantic_upper_sql = active_upper;
    }
    parser_evidence.system_catalog_defaults_semantic_evidence_required =
        HasSystemCatalogDefaultsSemanticProfile(profile.dialect_id) &&
        IsSystemCatalogDefaultsSemanticStatement(profile.dialect_id,
                                                active_upper);
    if (parser_evidence.system_catalog_defaults_semantic_evidence_required) {
      parser_evidence.system_catalog_defaults_semantic_operation_id =
          pattern.mapping_key;
    }
    parser_evidence.session_settings_diagnostics_semantic_evidence_required =
        HasSessionSettingsDiagnosticsSemanticProfile(profile.dialect_id) &&
        IsSessionSettingsDiagnosticsSemanticStatement(profile.dialect_id,
                                                     active_upper);
    if (parser_evidence.session_settings_diagnostics_semantic_evidence_required) {
      parser_evidence.session_settings_diagnostics_semantic_upper_sql =
          active_upper;
    }
    parser_evidence.procedural_body_source_retention_required =
        IsProceduralBodySourceRetentionStatement(pattern.statement_family,
                                                pattern.operation_family,
                                                active_upper);
    if (parser_evidence.procedural_body_source_retention_required) {
      parser_evidence.procedural_span_metadata =
          ProceduralFunctionalEncodingSpanMetadataFor(
              profile.dialect_id, active_upper, tokens);
      parser_evidence.procedural_source_retention_metadata =
          ProceduralSourceRetentionMetadataFor(
              profile.dialect_id, normalized, active_upper, tokens);
    }

    ParseResult result;
    result.ok = true;
    result.normalized_sql = normalized;
    result.statement_family = std::string(pattern.statement_family);
    result.operation_family = std::string(pattern.operation_family);
    result.lifecycle_operation_id = std::string(pattern.mapping_key);
    result.sblr_operation = std::string(pattern.sblr_operation);
    result.sblr_operation_family = std::string(profile.sblr_operation_family);
    result.engine_api_function = std::string(pattern.engine_api_function);
    result.lifecycle_mapping_key = std::string(pattern.mapping_key);
    result.emulation_diagnostic_code = std::string(pattern.diagnostic_code);
    result.authority_disposition = MappingDispositionName(pattern.disposition);
    result.scratchbird_lifecycle_api =
        pattern.disposition == MappingDisposition::kScratchBirdLifecycleApi;
    result.parser_support_udr_route =
        pattern.disposition == MappingDisposition::kParserSupportUdr;
    result.catalog_projection_only =
        pattern.disposition == MappingDisposition::kCatalogProjection;
    result.exact_emulated_diagnostic =
        pattern.disposition == MappingDisposition::kPolicyRefusal ||
        pattern.disposition == MappingDisposition::kSecurityRefusal ||
        pattern.disposition == MappingDisposition::kUnsupportedRefusal;
    result.fail_closed_refusal = result.exact_emulated_diagnostic;
    result.parser_evidence_json = ParserEvidenceJson(profile, parser_evidence);
    result.sblr_envelope = MakeSblrEnvelope(profile, pattern, parser_evidence);

    std::vector<Diagnostic> diagnostics;
    if (!pattern.diagnostic_code.empty()) {
      diagnostics.push_back(MakeDiagnostic(
          std::string(pattern.diagnostic_code),
          result.fail_closed_refusal ? "ERROR" : "INFO",
          std::string(pattern.diagnostic_message),
          std::string(profile.parser_package_name) + ".parser",
          {{"dialect", std::string(profile.dialect_id)},
           {"operation_family", result.operation_family},
           {"authority_disposition", result.authority_disposition}}));
    }
    result.message_vector_json = MessageVectorToJson(diagnostics);
    return result;
  }

  return Reject(profile, std::string(profile.diagnostic_prefix) + ".PARSE.UNSUPPORTED_SURFACE",
                "SQL input is not assigned to a donor parser surface.",
                {{"dialect", std::string(profile.dialect_id)},
                 {"normalized_prefix", upper.substr(0, upper.size() < 80 ? upper.size() : 80)}});
}

std::string PackageIdentityJson(const DialectProfile& profile) {
  std::ostringstream out;
  out << "{\"dialect\":\"" << EscapeJson(profile.dialect_id)
      << "\",\"display_name\":\"" << EscapeJson(profile.display_name)
      << "\",\"parser_package\":\"" << EscapeJson(profile.parser_package_name)
      << "\",\"parser_support_package\":\""
      << EscapeJson(profile.parser_support_package_name)
      << "\",\"release_profile\":\"" << EscapeJson(profile.release_profile)
      << "\",\"authority_policy\":\"engine_sblr_mga_only\""
      << ",\"donor_sql_execution\":false"
      << ",\"donor_storage_authority\":false"
      << ",\"donor_recovery_authority\":false"
      << ",\"standalone_dialect_package\":true"
      << ",\"surface_counts\":{"
      << "\"parser_surface_rows\":" << profile.parser_surface_rows << ','
      << "\"function_api_rows\":" << profile.function_api_rows << ','
      << "\"donor_compatible_alias_rows\":"
      << profile.donor_compatible_alias_rows << ','
      << "\"core_or_optional_alias_rows\":"
      << profile.core_or_optional_alias_rows << ','
      << "\"catalog_projection_only_rows\":"
      << profile.catalog_projection_only_rows << ','
      << "\"connector_operation_rows\":"
      << profile.connector_operation_rows << ','
      << "\"policy_blocked_rows\":" << profile.policy_blocked_rows << ','
      << "\"trusted_udr_registration_rows\":"
      << profile.trusted_udr_registration_rows << ','
      << "\"unsupported_rows\":" << profile.unsupported_rows
      << "}}";
  return out.str();
}

std::string SurfaceReportJson(const DialectProfile& profile) {
  auto emit_surface_array = [](std::ostringstream& out,
                               std::string_view name,
                               std::span<const SurfaceDescriptor> surfaces) {
    out << "\"" << name << "\":[";
    for (std::size_t i = 0; i < surfaces.size(); ++i) {
      if (i != 0) out << ',';
      out << "{\"family\":\"" << EscapeJson(surfaces[i].family)
          << "\",\"surface\":\"" << EscapeJson(surfaces[i].surface)
          << "\",\"owner\":\"" << EscapeJson(surfaces[i].owner) << "\"}";
    }
    out << "]";
  };

  std::ostringstream out;
  out << "{\"dialect\":\"" << EscapeJson(profile.dialect_id) << "\",";
  emit_surface_array(out, "datatype_surfaces", profile.datatype_surfaces);
  out << ',';
  emit_surface_array(out, "builtin_function_surfaces", profile.builtin_function_surfaces);
  out << ',';
  emit_surface_array(out, "catalog_overlay_surfaces", profile.catalog_overlay_surfaces);
  out << ',';
  emit_surface_array(out, "diagnostic_surfaces", profile.diagnostic_surfaces);
  out << "}";
  return out.str();
}

std::string ConnectionSandboxReportJson(const DialectProfile& profile) {
  std::ostringstream out;
  out << "{\"ok\":true"
      << ",\"dialect\":\"" << EscapeJson(profile.dialect_id) << "\""
      << ",\"connection_sandbox_contract\":\"donor_connection_schema_root_v1\""
      << ",\"schema_root_source\":\"listener_engine_materialized_attach_context\""
      << ",\"user_object_resolution\":\"relative_to_connection_schema_root\""
      << ",\"unqualified_name_root\":\"donor_schema_branch_root\""
      << ",\"direct_cross_root_access\":\"unsupported_denied\""
      << ",\"server_local_file_access\":\"default_denied\""
      << ",\"tenant_escape_policy\":\"fail_closed\""
      << ",\"catalog_projection_authority\":\"catalog_emulation_definer_authority\""
      << ",\"catalog_projection_can_query_outside_sandbox\":true"
      << ",\"catalog_projection_user_authority\":false"
      << ",\"catalog_projection_select_grant_required\":true"
      << ",\"catalog_projection_output_is_user_visible\":true"
      << ",\"catalog_projection_does_not_grant_base_object_access\":true"
      << ",\"sbsql_global_tree_visibility_inherited\":false"
      << ",\"sbsql_global_tree_visibility\":\"sbsql_only\""
      << ",\"engine_authorization_authority\":\"scratchbird_engine\""
      << ",\"parser_authorization_authority\":false"
      << ",\"parser_storage_authority\":false"
      << ",\"parser_recovery_authority\":false"
      << ",\"mga_transaction_authority\":\"scratchbird_engine\""
      << ",\"schema_root_is_user_visible_root\":true"
      << ",\"materialized_authorization_required\":true"
      << ",\"search_path_outside_root_policy\":\"refuse_without_catalog_definer_projection\""
      << ",\"catalog_security_filter\":\"engine_materialized_grants_plus_projection_definer_grants\""
      << "}";
  return out.str();
}

std::string DialectVariantReportJson(const DialectProfile& profile) {
  auto variants = [&profile]() -> std::vector<std::string_view> {
    const auto dialect = profile.dialect_id;
    if (dialect == "firebird") {
      return {"firebird_sql_dialect_1_compat",
              "firebird_sql_dialect_3",
              "firebird_dsql",
              "firebird_psql",
              "firebird_services_api_logical"};
    }
    if (dialect == "postgresql") {
      return {"postgresql_simple_query_sql",
              "postgresql_extended_query_protocol",
              "postgresql_plpgsql_udr_body",
              "postgresql_sql_function_body",
              "postgresql_jsonpath_expression",
              "postgresql_copy_logical_stream",
              "postgresql_logical_replication_protocol"};
    }
    if (dialect == "mysql") {
      return {"mysql_text_protocol_sql",
              "mysql_binary_prepared_protocol",
              "mysql_stored_program_sql",
              "mysql_load_data_local_stream",
              "mysql_replication_binlog_stream"};
    }
    if (dialect == "mariadb") {
      return {"mariadb_text_protocol_sql",
              "mariadb_binary_prepared_protocol",
              "mariadb_stored_program_sql",
              "mariadb_sql_mode_mysql_compat",
              "mariadb_sql_mode_oracle_reasonable_subset",
              "mariadb_replication_binlog_stream"};
    }
    if (dialect == "sqlite") {
      return {"sqlite_sql",
              "sqlite_pragma",
              "sqlite_virtual_table_module_surface",
              "sqlite_loadable_extension_policy_surface"};
    }
    if (dialect == "duckdb") {
      return {"duckdb_sql",
              "duckdb_pragma",
              "duckdb_copy_stream",
              "duckdb_extension_policy_surface"};
    }
    if (dialect == "clickhouse") {
      return {"clickhouse_sql",
              "clickhouse_settings_clause",
              "clickhouse_external_table_function_surface",
              "clickhouse_dictionary_function_surface"};
    }
    if (dialect == "tidb") {
      return {"tidb_mysql_compatible_sql",
              "tidb_admin_sql",
              "tidb_cdc_changefeed_surface"};
    }
    if (dialect == "vitess") {
      return {"vitess_mysql_compatible_sql",
              "vitess_vschema_surface",
              "vitess_vreplication_surface"};
    }
    if (dialect == "cockroachdb") {
      return {"cockroachdb_postgresql_wire_sql",
              "cockroachdb_changefeed_surface",
              "cockroachdb_zone_config_surface"};
    }
    if (dialect == "yugabytedb") {
      return {"yugabytedb_postgresql_wire_sql",
              "yugabytedb_yb_extension_surface",
              "yugabytedb_cdc_surface"};
    }
    if (dialect == "cassandra") {
      return {"cassandra_cql",
              "cassandra_nodetool_policy_surface",
              "cassandra_sstable_policy_surface"};
    }
    if (dialect == "mongodb") {
      return {"mongodb_command_api",
              "mongodb_query_document_language",
              "mongodb_aggregation_pipeline",
              "mongodb_change_stream_surface"};
    }
    if (dialect == "redis") {
      return {"redis_resp_command_api",
              "redis_lua_script_surface",
              "redis_stream_surface",
              "redis_replication_surface"};
    }
    if (dialect == "opensearch_sql_ppl") {
      return {"opensearch_sql",
              "opensearch_ppl",
              "opensearch_sql_rest_endpoint"};
    }
    if (dialect == "opensearch") {
      return {"opensearch_rest_json_dsl",
              "opensearch_bulk_ndjson",
              "opensearch_pit_scroll_surface"};
    }
    if (dialect == "neo4j") {
      return {"neo4j_cypher",
              "neo4j_bolt_statement_surface",
              "neo4j_procedure_call_surface"};
    }
    if (dialect == "influxdb") {
      return {"influxdb_flux",
              "influxdb_influxql",
              "influxdb_line_protocol_write_surface"};
    }
    if (dialect == "milvus") {
      return {"milvus_grpc_json_command_surface",
              "milvus_vector_search_surface",
              "milvus_collection_admin_surface"};
    }
    if (dialect == "dolt") {
      return {"dolt_mysql_compatible_sql",
              "dolt_version_control_sql_functions",
              "dolt_remote_sync_surface"};
    }
    if (dialect == "apache_ignite") {
      return {"apache_ignite_sql",
              "apache_ignite_scan_query_surface",
              "apache_ignite_control_script_policy_surface"};
    }
    if (dialect == "tikv") {
      return {"tikv_raw_kv_api",
              "tikv_transactional_kv_api",
              "tikv_import_sst_policy_surface"};
    }
    if (dialect == "foundationdb") {
      return {"foundationdb_tuple_key_api",
              "foundationdb_directory_layer_api",
              "foundationdb_transactional_kv_api"};
    }
    if (dialect == "immudb") {
      return {"immudb_sql",
              "immudb_verified_kv_api",
              "immudb_replication_surface"};
    }
    if (dialect == "xtdb") {
      return {"xtdb_datalog_query",
              "xtdb_sql",
              "xtdb_transaction_function_surface"};
    }
    return {"primary_donor_language"};
  }();

  std::ostringstream out;
  out << "{\"ok\":true"
      << ",\"dialect\":\"" << EscapeJson(profile.dialect_id) << "\""
      << ",\"dialect_variant_contract\":\"donor_supported_variant_surface_v1\""
      << ",\"variant_selection_authority\":\"listener_profile_and_engine_attach_context\""
      << ",\"parser_cross_dialect_detection\":false"
      << ",\"parser_cross_dialect_dispatch\":false"
      << ",\"sbsql_variant_admitted\":false"
      << ",\"reasonable_subset_policy\":\"declared_and_tested_per_donor_variant\""
      << ",\"variant_count\":" << variants.size()
      << ",\"variants\":[";
  for (std::size_t i = 0; i < variants.size(); ++i) {
    if (i != 0) out << ',';
    out << "\"" << EscapeJson(variants[i]) << "\"";
  }
  out << "]}";
  return out.str();
}

std::string MappingDispositionName(MappingDisposition disposition) {
  switch (disposition) {
    case MappingDisposition::kAdmittedSblr:
      return "admitted_sblr";
    case MappingDisposition::kScratchBirdLifecycleApi:
      return "scratchbird_lifecycle_api";
    case MappingDisposition::kParserSupportUdr:
      return "parser_support_udr";
    case MappingDisposition::kCatalogProjection:
      return "catalog_projection";
    case MappingDisposition::kPolicyRefusal:
      return "policy_refusal_fail_closed";
    case MappingDisposition::kSecurityRefusal:
      return "security_refusal_fail_closed";
    case MappingDisposition::kUnsupportedRefusal:
      return "unsupported_refusal_fail_closed";
  }
  return "unknown";
}

} // namespace scratchbird::parser::donor
