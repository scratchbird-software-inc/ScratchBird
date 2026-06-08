// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_dialect.hpp"

#include "donor_dialect.hpp"

#include <array>
#include <cctype>
#include <sstream>

namespace scratchbird::parser::firebird {
namespace {

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool IsCommandBoundary(char ch) {
  return std::isspace(static_cast<unsigned char>(ch)) != 0 ||
         ch == ';' || ch == '(' || ch == '\'' || ch == '"';
}

bool StartsWithCommand(std::string_view value, std::string_view prefix) {
  if (!StartsWith(value, prefix)) return false;
  return value.size() == prefix.size() || IsCommandBoundary(value[prefix.size()]);
}

bool Contains(std::string_view value, std::string_view needle) {
  return value.find(needle) != std::string_view::npos;
}

bool IsIdentifierChar(char ch) {
  const auto c = static_cast<unsigned char>(ch);
  return std::isalnum(c) != 0 || ch == '_' || ch == '$';
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

char QQuoteCloseDelimiter(char open) {
  switch (open) {
    case '{': return '}';
    case '[': return ']';
    case '(': return ')';
    case '<': return '>';
    default: return open;
  }
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
    if ((ch == 'q' || ch == 'Q') && next == '\'' && i + 2 < text.size() &&
        text[i + 2] != '\'') {
      const char close = QQuoteCloseDelimiter(text[i + 2]);
      masked.push_back(ch);
      masked.push_back('\'');
      masked.push_back(text[i + 2]);
      i += 3;
      while (i < text.size()) {
        if (i + 1 < text.size() && text[i] == close && text[i + 1] == '\'') {
          masked.push_back(close);
          masked.push_back('\'');
          i += 2;
          break;
        }
        masked.push_back(' ');
        ++i;
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

struct ParserEvidence {
  std::string statement_kind;
  std::size_t token_count{0};
  std::size_t source_span_count{0};
  scratchbird::parser::donor::ProceduralFunctionalEncodingSpanMetadata
      procedural_span_metadata;
  scratchbird::parser::donor::ProceduralSourceRetentionMetadata
      procedural_source_retention_metadata;
  std::string firebird_psql_functional_encoding_evidence_json;
  std::size_t clause_count{0};
  std::size_t parameter_count{0};
  std::size_t object_reference_count{0};
  std::size_t function_reference_count{0};
  std::size_t datatype_reference_count{0};
  std::size_t catalog_reference_count{0};
  std::string datatype_profile_evidence_json;
  std::string firebird_exact_datatype_domain_evidence_json;
  std::string firebird_gbak_logical_stream_evidence_json;
  std::string firebird_connection_sandbox_evidence_json;
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
  bool firebird_exact_datatype_domain_evidence_required{false};
  bool firebird_gbak_logical_stream_evidence_required{false};
  bool firebird_connection_sandbox_evidence_required{false};
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
  bool firebird_psql_functional_encoding_evidence_required{false};
};

std::span<const scratchbird::parser::donor::SurfaceDescriptor>
DonorCatalogOverlaySurfaces() {
  static const std::vector<scratchbird::parser::donor::SurfaceDescriptor>
      surfaces = [] {
        std::vector<scratchbird::parser::donor::SurfaceDescriptor> converted;
        for (const auto& surface :
             scratchbird::parser::firebird::CatalogOverlaySurfaces()) {
          converted.push_back({surface.family, surface.surface, surface.owner});
        }
        return converted;
      }();
  return surfaces;
}

bool IsNoiseToken(const Token& token) {
  return token.kind == "line_comment" || token.kind == "block_comment";
}

bool IsIdentifierToken(const Token& token) {
  return token.kind == "identifier_or_keyword";
}

std::string TokenUpper(const Token& token) {
  return ToUpperAscii(token.lexeme);
}

bool IsClauseKeyword(std::string_view upper) {
  return upper == "SELECT" || upper == "WITH" || upper == "FROM" ||
         upper == "WHERE" || upper == "GROUP" || upper == "HAVING" ||
         upper == "ORDER" || upper == "ROWS" || upper == "RETURNING" ||
         upper == "VALUES" || upper == "SET" || upper == "JOIN" ||
         upper == "ON" || upper == "INTO" || upper == "DATABASE" ||
         upper == "TABLE" || upper == "DOMAIN" || upper == "INDEX" ||
         upper == "VIEW" || upper == "PROCEDURE" || upper == "FUNCTION" ||
         upper == "TRIGGER" || upper == "ROLE" || upper == "USER" ||
         upper == "EXCEPTION" || upper == "PACKAGE";
}

bool IntroducesObjectReference(std::string_view upper) {
  return upper == "FROM" || upper == "JOIN" || upper == "UPDATE" ||
         upper == "INTO" || upper == "DATABASE" || upper == "TABLE" ||
         upper == "DOMAIN" || upper == "INDEX" || upper == "VIEW" ||
         upper == "PROCEDURE" || upper == "FUNCTION" ||
         upper == "TRIGGER" || upper == "ROLE" || upper == "USER" ||
         upper == "EXCEPTION" || upper == "PACKAGE" || upper == "GENERATOR" ||
         upper == "SEQUENCE";
}

bool IsBuiltinSqlKeyword(std::string_view upper) {
  return upper == "SELECT" || upper == "WITH" || upper == "FROM" ||
         upper == "WHERE" || upper == "GROUP" || upper == "BY" ||
         upper == "HAVING" || upper == "ORDER" || upper == "ROWS" ||
         upper == "INSERT" || upper == "UPDATE" || upper == "DELETE" ||
         upper == "CREATE" || upper == "ALTER" || upper == "DROP" ||
         upper == "TABLE" || upper == "INDEX" || upper == "VIEW" ||
         upper == "DATABASE" || upper == "VALUES" || upper == "INTO" ||
         upper == "SET" || upper == "AND" || upper == "OR" ||
         upper == "NOT" || upper == "NULL" || upper == "TRUE" ||
         upper == "FALSE" || upper == "UNKNOWN" || upper == "CASE" ||
         upper == "WHEN" || upper == "THEN" || upper == "ELSE" ||
         upper == "END" || upper == "AS" || upper == "ON" ||
         upper == "JOIN" || upper == "LEFT" || upper == "RIGHT" ||
         upper == "INNER" || upper == "OUTER" || upper == "FULL";
}

bool SurfaceMentions(std::string_view upper,
                     const std::vector<SurfaceDescriptor>& surfaces) {
  for (const auto& surface : surfaces) {
    std::size_t begin = 0;
    while (begin < surface.surface.size()) {
      std::size_t end = surface.surface.find(' ', begin);
      if (end == std::string_view::npos) end = surface.surface.size();
      const auto raw = TrimAsciiView(
          std::string_view(surface.surface).substr(begin, end - begin));
      const auto token = ToUpperAscii(raw);
      if (!token.empty() && token != "OPTIONAL" &&
          token != "COMPATIBILITY" && token != "VIEWS" &&
          (ContainsWord(upper, token) || Contains(upper, token))) {
        return true;
      }
      if (end == surface.surface.size()) break;
      begin = end + 1;
    }
  }
  return false;
}

std::string StatementKindFromTokens(const std::vector<Token>& tokens) {
  std::vector<std::string> keywords;
  for (const auto& token : tokens) {
    if (IsNoiseToken(token)) continue;
    if (IsIdentifierToken(token)) keywords.push_back(TokenUpper(token));
    if (keywords.size() >= 3) break;
  }
  if (keywords.empty()) return "unknown";
  if (keywords.size() >= 2) {
    if (keywords[0] == "CREATE" || keywords[0] == "ALTER" ||
        keywords[0] == "DROP") {
      return keywords[0] + "_" + keywords[1];
    }
    if (keywords[0] == "SET" && keywords[1] == "TRANSACTION") {
      return "SET_TRANSACTION";
    }
    if (keywords[0] == "EXECUTE" && keywords[1] == "BLOCK") {
      return "EXECUTE_BLOCK";
    }
  }
  return keywords[0];
}

ParserEvidence BuildParserEvidence(std::string_view upper,
                                   const std::vector<Token>& tokens) {
  ParserEvidence evidence;
  evidence.statement_kind = StatementKindFromTokens(tokens);
  if (scratchbird::parser::donor::IsIndexSemanticDefaultsStatement(upper)) {
    evidence.statement_kind = StartsWithCommand(TrimAsciiView(upper), "ALTER")
                                  ? "ALTER_INDEX"
                                  : "CREATE_INDEX";
  }
  evidence.token_count = tokens.size();
  evidence.source_span_count = tokens.empty() ? 0 : tokens.size();
  evidence.cst_materialized = !tokens.empty();
  evidence.ast_materialized = evidence.cst_materialized &&
                              evidence.statement_kind != "unknown";
  evidence.bound_ast_materialized = evidence.ast_materialized;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const auto& token = tokens[i];
    if (IsNoiseToken(token)) continue;
    if (token.kind == "parameter") {
      ++evidence.parameter_count;
      continue;
    }
    if (!IsIdentifierToken(token)) continue;
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
    if (i + 1 < tokens.size() && tokens[i + 1].kind == "punctuation" &&
        tokens[i + 1].lexeme == "(" && !IsBuiltinSqlKeyword(upper_token)) {
      ++evidence.function_reference_count;
    }
  }
  evidence.datatype_reference_count =
      SurfaceMentions(upper, DatatypeSurfaces()) ? 1 : 0;
  std::vector<scratchbird::parser::donor::Token> donor_tokens;
  donor_tokens.reserve(tokens.size());
  for (const auto& token : tokens) {
    donor_tokens.push_back({token.kind, token.lexeme, token.offset});
  }
  evidence.datatype_profile_evidence_json =
      scratchbird::parser::donor::DatatypeProfileEvidenceJson("firebird",
                                                              donor_tokens);
  evidence.catalog_reference_count =
      SurfaceMentions(upper, CatalogOverlaySurfaces()) ? 1 : 0;
  return evidence;
}

scratchbird::parser::donor::ProceduralFunctionalEncodingSpanMetadata
BuildProceduralFunctionalEncodingSpanMetadata(
    std::string_view active_upper,
    const std::vector<Token>& tokens) {
  scratchbird::parser::donor::ProceduralFunctionalEncodingSpanMetadata metadata;
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
  if (body_semantic_index == semantic_count &&
      ContainsWord(active_upper, "FOR EACH ROW")) {
    for (std::size_t i = 0; i < semantic_count; ++i) {
      if (token_upper(i) == "SET") {
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
      body_semantic_index < semantic_count ? semantic_count - body_semantic_index
                                           : 0;
  if (metadata.header_source_span_count > 0 &&
      metadata.body_source_span_count > 0) {
    metadata.parser_bound_sblr_body_instruction_stream = true;
    metadata.uuid_dependency_bindings_bound = true;
  }
  return metadata;
}

std::string FirebirdPsqlFunctionalEncodingEvidenceJson(
    std::string_view operation_family,
    std::string_view active_upper,
    const scratchbird::parser::donor::ProceduralFunctionalEncodingSpanMetadata&
        span_metadata) {
  const bool is_procedure = Contains(operation_family, ".procedure");
  const bool is_function = Contains(operation_family, ".function");
  const bool is_trigger = Contains(operation_family, ".trigger");
  const bool is_package = Contains(operation_family, ".package");
  const bool is_package_body = Contains(operation_family, ".package_body");
  const bool is_execute_block = Contains(operation_family, ".execute_block");
  const bool has_begin_end = ContainsWord(active_upper, "BEGIN") &&
                             ContainsWord(active_upper, "END");
  const bool has_suspend = ContainsWord(active_upper, "SUSPEND");
  const bool has_post_event = ContainsWord(active_upper, "POST_EVENT");
  const bool has_execute_statement =
      ContainsWord(active_upper, "EXECUTE") && ContainsWord(active_upper, "STATEMENT");
  const bool has_return = ContainsWord(active_upper, "RETURN");
  const bool has_exception_handler = ContainsWord(active_upper, "WHEN") ||
                                     ContainsWord(active_upper, "EXCEPTION");
  const bool has_loop_or_cursor = ContainsWord(active_upper, "FOR") ||
                                  ContainsWord(active_upper, "WHILE") ||
                                  ContainsWord(active_upper, "CURSOR");
  const bool has_autonomous = ContainsWord(active_upper, "AUTONOMOUS") &&
                              ContainsWord(active_upper, "TRANSACTION");
  const bool has_assignment = Contains(active_upper, "=") &&
                              !Contains(active_upper, "==");
  const std::size_t encoded_instruction_count =
      (has_begin_end ? 1u : 0u) + (has_suspend ? 1u : 0u) +
      (has_post_event ? 1u : 0u) + (has_execute_statement ? 1u : 0u) +
      (has_return ? 1u : 0u) + (has_exception_handler ? 1u : 0u) +
      (has_loop_or_cursor ? 1u : 0u) + (has_autonomous ? 1u : 0u) +
      (has_assignment ? 1u : 0u);

  std::ostringstream out;
  out << "{\"evidence_contract\":\"firebird_psql_functional_encoding.v1\","
      << "\"dialect\":\"firebird\","
      << "\"operation_family\":\"" << EscapeJson(operation_family) << "\","
      << "\"routine_kind\":{\"procedure\":" << BoolJson(is_procedure)
      << ",\"function\":" << BoolJson(is_function)
      << ",\"trigger\":" << BoolJson(is_trigger)
      << ",\"package\":" << BoolJson(is_package)
      << ",\"package_body\":" << BoolJson(is_package_body)
      << ",\"execute_block\":" << BoolJson(is_execute_block) << "},"
      << "\"source_text_included\":false,"
      << "\"body_text_included\":false,"
      << "\"parser_bound_sblr_body_instruction_stream\":"
      << BoolJson(span_metadata.parser_bound_sblr_body_instruction_stream)
      << ",\"uuid_dependency_bindings_bound\":"
      << BoolJson(span_metadata.uuid_dependency_bindings_bound)
      << ",\"header_source_span_count\":"
      << span_metadata.header_source_span_count
      << ",\"body_source_span_count\":"
      << span_metadata.body_source_span_count
      << ",\"encoded_instruction_count\":" << encoded_instruction_count
      << ",\"encoded_instruction_families\":{\"block_boundary\":"
      << BoolJson(has_begin_end)
      << ",\"row_yield\":" << BoolJson(has_suspend)
      << ",\"event_signal\":" << BoolJson(has_post_event)
      << ",\"dynamic_statement\":" << BoolJson(has_execute_statement)
      << ",\"routine_result\":" << BoolJson(has_return)
      << ",\"error_handler\":" << BoolJson(has_exception_handler)
      << ",\"iterator_or_cursor\":" << BoolJson(has_loop_or_cursor)
      << ",\"autonomous_unit\":" << BoolJson(has_autonomous)
      << ",\"assignment\":" << BoolJson(has_assignment) << "},"
      << "\"functional_encoding_status\":\"firebird_psql_parser_bound_sblr_encoded\","
      << "\"execution_authority\":\"scratchbird_engine_sblr\","
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_sequence_value_authority\":false,"
      << "\"donor_sql_executed\":false,"
      << "\"runtime_equivalence_status\":\"pending_donor_native_psql_replay\","
      << "\"catalog_persistence_status\":\"pending_runtime_catalog_reopen_proof\","
      << "\"enterprise_readiness\":\"not_enterprise_ready\"}";
  return out.str();
}

std::string FirebirdExactDatatypeDomainEvidenceJson(
    std::string_view operation_family,
    std::string_view active_upper) {
  const bool domain_descriptor_bound = ContainsWord(active_upper, "DOMAIN");
  const bool table_column_descriptor_bound =
      ContainsWord(active_upper, "TABLE") &&
      (ContainsWord(active_upper, "CREATE") ||
       ContainsWord(active_upper, "ALTER") ||
       ContainsWord(active_upper, "RECREATE"));
  const bool exact_numeric_descriptor_bound =
      ContainsWord(active_upper, "SMALLINT") ||
      ContainsWord(active_upper, "INTEGER") ||
      ContainsWord(active_upper, "BIGINT") ||
      ContainsWord(active_upper, "INT128") ||
      ContainsWord(active_upper, "NUMERIC") ||
      ContainsWord(active_upper, "DECIMAL");
  const bool numeric_precision_scale_descriptor_bound =
      Contains(active_upper, "NUMERIC(") ||
      Contains(active_upper, "DECIMAL(");
  const bool floating_descriptor_bound =
      ContainsWord(active_upper, "FLOAT") ||
      ContainsWord(active_upper, "DOUBLE") ||
      ContainsWord(active_upper, "REAL") ||
      ContainsWord(active_upper, "DECFLOAT");
  const bool text_descriptor_bound =
      ContainsWord(active_upper, "CHAR") ||
      ContainsWord(active_upper, "VARCHAR") ||
      ContainsWord(active_upper, "NCHAR") ||
      ContainsWord(active_upper, "CHARACTER");
  const bool charset_descriptor_bound =
      Contains(active_upper, "CHARACTER SET");
  const bool collation_descriptor_bound = ContainsWord(active_upper, "COLLATE");
  const bool blob_descriptor_bound = ContainsWord(active_upper, "BLOB");
  const bool blob_subtype_descriptor_bound =
      blob_descriptor_bound &&
      (Contains(active_upper, "SUB_TYPE") || Contains(active_upper, "SUB TYPE"));
  const bool blob_segment_size_descriptor_bound =
      blob_descriptor_bound && Contains(active_upper, "SEGMENT SIZE");
  const bool temporal_descriptor_bound =
      ContainsWord(active_upper, "DATE") ||
      ContainsWord(active_upper, "TIME") ||
      ContainsWord(active_upper, "TIMESTAMP");
  const bool temporal_timezone_descriptor_bound =
      temporal_descriptor_bound && Contains(active_upper, "WITH TIME ZONE");
  const bool boolean_descriptor_bound =
      ContainsWord(active_upper, "BOOLEAN") ||
      ContainsWord(active_upper, "TRUE") ||
      ContainsWord(active_upper, "FALSE") ||
      ContainsWord(active_upper, "UNKNOWN");
  const bool array_bounds_descriptor_bound =
      Contains(active_upper, "[") || ContainsWord(active_upper, "ARRAY");
  const bool nullability_descriptor_bound =
      ContainsWord(active_upper, "NULL");
  const bool default_descriptor_bound = ContainsWord(active_upper, "DEFAULT");
  const bool check_constraint_descriptor_bound =
      ContainsWord(active_upper, "CHECK");
  const bool computed_expression_descriptor_bound =
      Contains(active_upper, "COMPUTED BY") ||
      Contains(active_upper, "GENERATED ALWAYS AS");
  const bool cast_descriptor_bound = Contains(active_upper, "CAST(");
  const std::size_t descriptor_family_count =
      static_cast<std::size_t>(domain_descriptor_bound) +
      static_cast<std::size_t>(table_column_descriptor_bound) +
      static_cast<std::size_t>(exact_numeric_descriptor_bound) +
      static_cast<std::size_t>(numeric_precision_scale_descriptor_bound) +
      static_cast<std::size_t>(floating_descriptor_bound) +
      static_cast<std::size_t>(text_descriptor_bound) +
      static_cast<std::size_t>(charset_descriptor_bound) +
      static_cast<std::size_t>(collation_descriptor_bound) +
      static_cast<std::size_t>(blob_descriptor_bound) +
      static_cast<std::size_t>(blob_subtype_descriptor_bound) +
      static_cast<std::size_t>(blob_segment_size_descriptor_bound) +
      static_cast<std::size_t>(temporal_descriptor_bound) +
      static_cast<std::size_t>(temporal_timezone_descriptor_bound) +
      static_cast<std::size_t>(boolean_descriptor_bound) +
      static_cast<std::size_t>(array_bounds_descriptor_bound) +
      static_cast<std::size_t>(nullability_descriptor_bound) +
      static_cast<std::size_t>(default_descriptor_bound) +
      static_cast<std::size_t>(check_constraint_descriptor_bound) +
      static_cast<std::size_t>(computed_expression_descriptor_bound) +
      static_cast<std::size_t>(cast_descriptor_bound);

  std::ostringstream out;
  out << "{\"evidence_contract\":\"firebird_exact_datatype_domain_descriptor_evidence.v1\","
      << "\"dialect\":\"firebird\","
      << "\"operation_family\":\"" << EscapeJson(operation_family) << "\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"donor_profile_uuid\":\"019e13c0-0000-7000-8000-000000000302\","
      << "\"descriptor_authority\":\"scratchbird_engine_catalog\","
      << "\"execution_authority\":\"scratchbird_engine_sblr\","
      << "\"sblr_operation_uuid_resolution_required\":true,"
      << "\"catalog_descriptor_required\":true,"
      << "\"generic_text_fallback_allowed\":false,"
      << "\"source_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"literal_text_included\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"donor_sql_executed\":false,"
      << "\"descriptor_family_count\":" << descriptor_family_count << ','
      << "\"domain_descriptor_bound\":"
      << BoolJson(domain_descriptor_bound)
      << ",\"table_column_descriptor_bound\":"
      << BoolJson(table_column_descriptor_bound)
      << ",\"exact_numeric_descriptor_bound\":"
      << BoolJson(exact_numeric_descriptor_bound)
      << ",\"numeric_precision_scale_descriptor_bound\":"
      << BoolJson(numeric_precision_scale_descriptor_bound)
      << ",\"floating_descriptor_bound\":"
      << BoolJson(floating_descriptor_bound)
      << ",\"text_descriptor_bound\":" << BoolJson(text_descriptor_bound)
      << ",\"charset_descriptor_bound\":"
      << BoolJson(charset_descriptor_bound)
      << ",\"collation_descriptor_bound\":"
      << BoolJson(collation_descriptor_bound)
      << ",\"blob_descriptor_bound\":"
      << BoolJson(blob_descriptor_bound)
      << ",\"blob_subtype_descriptor_bound\":"
      << BoolJson(blob_subtype_descriptor_bound)
      << ",\"blob_segment_size_descriptor_bound\":"
      << BoolJson(blob_segment_size_descriptor_bound)
      << ",\"temporal_descriptor_bound\":"
      << BoolJson(temporal_descriptor_bound)
      << ",\"temporal_timezone_descriptor_bound\":"
      << BoolJson(temporal_timezone_descriptor_bound)
      << ",\"boolean_descriptor_bound\":"
      << BoolJson(boolean_descriptor_bound)
      << ",\"array_bounds_descriptor_bound\":"
      << BoolJson(array_bounds_descriptor_bound)
      << ",\"nullability_descriptor_bound\":"
      << BoolJson(nullability_descriptor_bound)
      << ",\"default_descriptor_bound\":"
      << BoolJson(default_descriptor_bound)
      << ",\"check_constraint_descriptor_bound\":"
      << BoolJson(check_constraint_descriptor_bound)
      << ",\"computed_expression_descriptor_bound\":"
      << BoolJson(computed_expression_descriptor_bound)
      << ",\"cast_descriptor_bound\":"
      << BoolJson(cast_descriptor_bound)
      << ",\"descriptor_exactness_status\":\"firebird_exact_datatype_descriptor_recorded_runtime_equivalence_pending\","
      << "\"runtime_equivalence_status\":\"pending_donor_native_exactness_replay\","
      << "\"enterprise_readiness\":\"not_enterprise_ready\"}";
  return out.str();
}

std::string FirebirdConnectionSandboxEvidenceJson(
    std::string_view statement_family,
    std::string_view operation_family) {
  std::ostringstream out;
  out << "{\"evidence_contract\":\"firebird_connection_sandbox_evidence.v1\","
      << "\"dialect\":\"firebird\","
      << "\"statement_family\":\"" << EscapeJson(statement_family) << "\","
      << "\"operation_family\":\"" << EscapeJson(operation_family) << "\","
      << "\"connection_sandbox_contract\":\"donor_connection_schema_root_v1\","
      << "\"schema_root_source\":\"listener_engine_materialized_attach_context\","
      << "\"user_object_resolution\":\"relative_to_connection_schema_root\","
      << "\"unqualified_name_root\":\"donor_schema_branch_root\","
      << "\"direct_cross_root_access\":\"unsupported_denied\","
      << "\"server_local_file_access\":\"default_denied\","
      << "\"tenant_escape_policy\":\"fail_closed\","
      << "\"catalog_projection_authority\":\"catalog_emulation_definer_authority\","
      << "\"catalog_projection_can_query_outside_sandbox\":true,"
      << "\"catalog_projection_user_authority\":false,"
      << "\"catalog_projection_select_grant_required\":true,"
      << "\"catalog_projection_output_is_user_visible\":true,"
      << "\"catalog_projection_does_not_grant_base_object_access\":true,"
      << "\"sbsql_global_tree_visibility_inherited\":false,"
      << "\"sbsql_global_tree_visibility\":\"sbsql_only\","
      << "\"engine_authorization_authority\":\"scratchbird_engine\","
      << "\"parser_authorization_authority\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_recovery_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"mga_transaction_authority\":\"scratchbird_engine\","
      << "\"schema_root_is_user_visible_root\":true,"
      << "\"materialized_authorization_required\":true,"
      << "\"search_path_outside_root_policy\":\"refuse_without_catalog_definer_projection\","
      << "\"catalog_security_filter\":\"engine_materialized_grants_plus_projection_definer_grants\","
      << "\"source_text_included\":false}";
  return out.str();
}

std::string ParserEvidenceJson(const ParserEvidence& evidence) {
  std::ostringstream out;
  out << "{\"dialect\":\"firebird\","
      << "\"statement_kind\":\"" << EscapeJson(evidence.statement_kind)
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
  if (evidence.firebird_connection_sandbox_evidence_required) {
    out << ",\"firebird_connection_sandbox_evidence\":"
        << evidence.firebird_connection_sandbox_evidence_json;
  }
  if (evidence.datatype_descriptor_evidence_required) {
    out << ",\"datatype_descriptor_evidence\":"
        << scratchbird::parser::donor::DatatypeDescriptorEvidenceJson(
               evidence.datatype_reference_count);
    if (!evidence.datatype_profile_evidence_json.empty()) {
      out << ",\"datatype_profile_evidence\":"
          << evidence.datatype_profile_evidence_json;
    }
    if (evidence.firebird_exact_datatype_domain_evidence_required) {
      out << ",\"firebird_exact_datatype_domain_evidence\":"
          << evidence.firebird_exact_datatype_domain_evidence_json;
    }
  }
  if (evidence.firebird_gbak_logical_stream_evidence_required) {
    out << ",\"firebird_gbak_logical_stream_evidence\":"
        << evidence.firebird_gbak_logical_stream_evidence_json;
  }
  if (evidence.index_semantic_defaults_evidence_required) {
    out << ",\"index_semantic_defaults_evidence\":"
        << scratchbird::parser::donor::IndexSemanticDefaultsEvidenceJson(
               "firebird", "5.0.4", evidence.index_semantic_defaults_upper_sql);
  }
  if (evidence.constraint_semantic_defaults_evidence_required) {
    out << ",\"constraint_semantic_defaults_evidence\":"
        << scratchbird::parser::donor::ConstraintSemanticDefaultsEvidenceJson(
               "firebird", "5.0.4",
               evidence.constraint_semantic_defaults_upper_sql);
  }
  if (evidence.sequence_identity_semantic_evidence_required) {
    out << ",\"sequence_identity_semantic_evidence\":"
        << scratchbird::parser::donor::SequenceIdentitySemanticEvidenceJson(
               "firebird", "5.0.4",
               evidence.sequence_identity_semantic_upper_sql);
  }
  if (evidence.identifier_name_resolution_evidence_required) {
    out << ",\"identifier_name_resolution_evidence\":"
        << scratchbird::parser::donor::IdentifierNameResolutionEvidenceJson(
               "firebird", "5.0.4",
               evidence.identifier_name_resolution_upper_sql);
  }
  if (evidence.scalar_expression_semantic_evidence_required) {
    out << ",\"scalar_expression_semantic_evidence\":"
        << scratchbird::parser::donor::ScalarExpressionSemanticEvidenceJson(
               "firebird", "5.0.4",
               evidence.scalar_expression_semantic_upper_sql);
  }
  if (evidence.dml_mutation_semantic_evidence_required) {
    out << ",\"dml_mutation_semantic_evidence\":"
        << scratchbird::parser::donor::DmlMutationSemanticEvidenceJson(
               "firebird", "5.0.4",
               evidence.dml_mutation_semantic_upper_sql);
  }
  if (evidence.transaction_session_semantic_evidence_required) {
    out << ",\"transaction_session_semantic_evidence\":"
        << scratchbird::parser::donor::TransactionSessionSemanticEvidenceJson(
               "firebird", "5.0.4",
               evidence.transaction_session_semantic_upper_sql);
  }
  if (evidence.temporary_session_object_semantic_evidence_required) {
    out << ",\"temporary_session_object_semantic_evidence\":"
        << scratchbird::parser::donor::
               TemporarySessionObjectSemanticEvidenceJson(
                   "firebird", "5.0.4",
                   evidence.temporary_session_object_semantic_upper_sql);
  }
  if (evidence.dependency_bearing_ddl_semantic_evidence_required) {
    out << ",\"dependency_bearing_ddl_semantic_evidence\":"
        << scratchbird::parser::donor::
               DependencyBearingDdlSemanticEvidenceJson(
                   "firebird", "5.0.4",
                   evidence.dependency_bearing_ddl_semantic_upper_sql);
  }
  if (evidence.ddl_transaction_behavior_semantic_evidence_required) {
    out << ",\"ddl_transaction_behavior_semantic_evidence\":"
        << scratchbird::parser::donor::
               DdlTransactionBehaviorSemanticEvidenceJson(
                   "firebird", "5.0.4",
                   evidence.ddl_transaction_behavior_semantic_upper_sql);
  }
  if (evidence.resource_text_semantic_evidence_required) {
    out << ",\"resource_text_semantic_evidence\":"
        << scratchbird::parser::donor::ResourceTextSemanticEvidenceJson(
               "firebird", "5.0.4",
               evidence.resource_text_semantic_upper_sql);
  }
  if (evidence.statistics_optimizer_semantic_evidence_required) {
    out << ",\"statistics_optimizer_semantic_evidence\":"
        << scratchbird::parser::donor::
               StatisticsOptimizerSemanticEvidenceJson(
                   "firebird", "5.0.4",
                   evidence.statistics_optimizer_semantic_upper_sql);
  }
  if (evidence.locks_isolation_semantic_evidence_required) {
    out << ",\"locks_isolation_semantic_evidence\":"
        << scratchbird::parser::donor::
               LocksIsolationSemanticEvidenceJson(
                   "firebird", "5.0.4",
                   evidence.locks_isolation_semantic_upper_sql);
  }
  if (evidence.system_catalog_defaults_semantic_evidence_required) {
    out << ",\"system_catalog_defaults_semantic_evidence\":"
        << scratchbird::parser::donor::
               SystemCatalogDefaultsSemanticEvidenceJson(
                   "firebird",
                   evidence.system_catalog_defaults_semantic_operation_id,
                   DonorCatalogOverlaySurfaces());
  }
  if (evidence.session_settings_diagnostics_semantic_evidence_required) {
    out << ",\"session_settings_diagnostics_semantic_evidence\":"
        << scratchbird::parser::donor::
               SessionSettingsDiagnosticsSemanticEvidenceJson(
                   "firebird", "5.0.4",
                   evidence.session_settings_diagnostics_semantic_upper_sql);
  }
  if (evidence.procedural_body_source_retention_required) {
    out << ",\"procedural_body_source_retention_evidence\":"
        << scratchbird::parser::donor::ProceduralBodySourceRetentionEvidenceJson(
               evidence.procedural_source_retention_metadata)
        << ",\"procedural_functional_encoding_source_span_uuid_binding_evidence\":"
        << scratchbird::parser::donor::ProceduralFunctionalEncodingEvidenceJson(
               evidence.source_span_count, evidence.cst_materialized,
               evidence.ast_materialized, evidence.bound_ast_materialized,
               evidence.procedural_span_metadata);
    if (evidence.firebird_psql_functional_encoding_evidence_required) {
      out << ",\"firebird_psql_functional_encoding_evidence\":"
          << evidence.firebird_psql_functional_encoding_evidence_json;
    }
  }
  out << ",\"enterprise_readiness_evidence\":"
      << scratchbird::parser::donor::EnterpriseReadinessEvidenceJson();
  out << "}";
  return out.str();
}

std::string MakeSblrEnvelope(std::string_view statement_family,
                             std::string_view operation_family,
                             const FirebirdLifecycleMappingDescriptor* mapping,
                             const ParserEvidence& evidence) {
  const bool lifecycle_api = mapping != nullptr &&
                             mapping->disposition == FirebirdMappingDisposition::kScratchBirdLifecycleApi;
  const bool support_udr = mapping != nullptr &&
                           mapping->disposition == FirebirdMappingDisposition::kParserSupportUdr;
  const bool exact_diagnostic = mapping != nullptr &&
                                mapping->disposition == FirebirdMappingDisposition::kEmulatedNonFileDiagnostic;
  return "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
         "\"dialect\":\"firebird\","
         "\"statement_family\":\"" + EscapeJson(statement_family) + "\","
         "\"operation_family\":\"" + EscapeJson(operation_family) + "\","
         "\"operation_id\":\"" + EscapeJson(mapping == nullptr ? "" : mapping->operation_id) + "\","
         "\"sblr_operation\":\"" + EscapeJson(mapping == nullptr ? "" : mapping->sblr_operation) + "\","
         "\"sblr_operation_family\":\"" +
         EscapeJson(mapping == nullptr ? "" : mapping->sblr_operation_family) + "\","
         "\"engine_api_function\":\"" +
         EscapeJson(mapping == nullptr ? "" : mapping->engine_api_function) + "\","
         "\"mapping_key\":\"" + EscapeJson(mapping == nullptr ? "" : mapping->mapping_key) + "\","
         "\"mapping_disposition\":\"" +
         EscapeJson(mapping == nullptr ? "" : FirebirdMappingDispositionName(mapping->disposition)) + "\","
         "\"parser_evidence\":" + ParserEvidenceJson(evidence) + ","
         "\"enterprise_readiness_evidence\":" +
         scratchbird::parser::donor::EnterpriseReadinessEvidenceJson() + ","
         "\"descriptor_resolution\":\"uuid_required\","
         "\"engine_authority\":\"scratchbird\","
         "\"scratchbird_lifecycle_api\":" + std::string(lifecycle_api ? "true" : "false") + ","
         "\"parser_support_udr_route\":" + std::string(support_udr ? "true" : "false") + ","
         "\"exact_emulated_diagnostic\":" + std::string(exact_diagnostic ? "true" : "false") + ","
         "\"real_firebird_file_effects\":false,"
         "\"donor_engine_sql_executed\":false,"
         "\"finite_subset\":true,"
         "\"full_declared_surface_assignment\":true,"
         "\"sql_text_included\":false}";
}

constexpr std::string_view kFirebirdLifecycleFamily = "sblr.management.runtime_operation.v3";

const std::array<FirebirdLifecycleMappingDescriptor, 13>& FirebirdMappingStorage() {
  static constexpr std::array<FirebirdLifecycleMappingDescriptor, 13> mappings{{
      {"firebird.lifecycle.create_database",
       "database_lifecycle",
       FirebirdMappingDisposition::kScratchBirdLifecycleApi,
       "lifecycle.create_database",
       "SBLR_LIFECYCLE_CREATE_DATABASE",
       kFirebirdLifecycleFamily,
       "EngineCreateLifecycle",
       "EngineCreateLifecycleRequest",
       "EngineCreateLifecycleResult",
       "FIREBIRD.EMULATION.NON_FILE_SURFACE",
       "Firebird CREATE DATABASE maps to ScratchBird engine lifecycle create authority.",
       false,
       false,
       false,
       false,
       false},
      {"firebird.lifecycle.drop_database",
       "database_lifecycle",
       FirebirdMappingDisposition::kScratchBirdLifecycleApi,
       "lifecycle.drop_database",
       "SBLR_LIFECYCLE_DROP_DATABASE",
       kFirebirdLifecycleFamily,
       "EngineDropLifecycle",
       "EngineDropLifecycleRequest",
       "EngineDropLifecycleResult",
       "FIREBIRD.EMULATION.NON_FILE_SURFACE",
       "Firebird DROP DATABASE maps to ScratchBird engine lifecycle safe-drop authority.",
       true,
       false,
       false,
       false,
       false},
      {"firebird.lifecycle.attach_database",
       "database_lifecycle",
       FirebirdMappingDisposition::kScratchBirdLifecycleApi,
       "lifecycle.attach_database",
       "SBLR_LIFECYCLE_ATTACH_DATABASE",
       kFirebirdLifecycleFamily,
       "EngineAttachLifecycle",
       "EngineAttachLifecycleRequest",
       "EngineAttachLifecycleResult",
       "FIREBIRD.EMULATION.NON_FILE_SURFACE",
       "Firebird CONNECT maps to ScratchBird engine lifecycle attach authority.",
       true,
       false,
       false,
       false,
       false},
      {"firebird.lifecycle.detach_database",
       "database_lifecycle",
       FirebirdMappingDisposition::kScratchBirdLifecycleApi,
       "lifecycle.detach_database",
       "SBLR_LIFECYCLE_DETACH_DATABASE",
       kFirebirdLifecycleFamily,
       "EngineDetachLifecycle",
       "EngineDetachLifecycleRequest",
       "EngineDetachLifecycleResult",
       "FIREBIRD.EMULATION.NON_FILE_SURFACE",
       "Firebird DISCONNECT maps to ScratchBird engine lifecycle detach authority.",
       true,
       false,
       false,
       false,
       false},
      {"firebird.lifecycle.verify_database",
       "low_level_utility",
       FirebirdMappingDisposition::kEmulatedNonFileDiagnostic,
       "",
       "",
       "",
       "",
       "",
       "",
       "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED",
       "Firebird validation is a donor low-level utility surface and is outside donor parser authority.",
       true,
       false,
       false,
       false,
       true},
      {"firebird.lifecycle.repair_database",
       "low_level_utility",
       FirebirdMappingDisposition::kEmulatedNonFileDiagnostic,
       "",
       "",
       "",
       "",
       "",
       "",
       "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED",
       "Firebird repair is a donor low-level utility surface and is outside donor parser authority.",
       true,
       false,
       false,
       false,
       true},
      {"firebird.emulated.database_file_management",
       "database_file_emulation",
       FirebirdMappingDisposition::kEmulatedNonFileDiagnostic,
       "",
       "",
       "",
       "",
       "",
       "",
       "FIREBIRD.EMULATION.NON_FILE_SURFACE",
       "Firebird database file-management syntax is diagnostic-only and has zero donor file effects.",
       true,
       false,
       false,
       false,
       true},
      {"firebird.emulated.shadow_storage",
       "shadow_file_emulation",
       FirebirdMappingDisposition::kEmulatedNonFileDiagnostic,
       "",
       "",
       "",
       "",
       "",
       "",
       "FIREBIRD.EMULATION.NON_FILE_SURFACE",
       "Firebird shadow storage syntax is diagnostic-only and has zero donor file effects.",
       true,
       false,
       false,
       false,
       true},
      {"firebird.emulated.backup_restore",
       "backup_restore_file_emulation",
       FirebirdMappingDisposition::kEmulatedNonFileDiagnostic,
       "",
       "",
       "",
       "",
       "",
       "",
       "FIREBIRD.EMULATION.NON_FILE_SURFACE",
       "Firebird backup/restore syntax is diagnostic-only here and must route through ScratchBird management authority.",
       true,
       false,
       false,
       false,
       true},
      {"firebird.emulated.external_plugin",
       "external_plugin_emulation",
       FirebirdMappingDisposition::kEmulatedNonFileDiagnostic,
       "",
       "",
       "",
       "",
       "",
       "",
       "FIREBIRD.EMULATION.NON_FILE_SURFACE",
       "Firebird external table/plugin syntax is diagnostic-only and cannot load donor external code.",
       true,
       false,
       false,
       false,
       true},
      {"firebird.emulated.service_api",
       "service_tool_emulation",
       FirebirdMappingDisposition::kEmulatedNonFileDiagnostic,
       "",
       "",
       "",
       "",
       "",
       "",
       "FIREBIRD.EMULATION.NON_FILE_SURFACE",
       "Firebird service/tool syntax is represented as an emulated diagnostic, not donor tool execution.",
       true,
       false,
       false,
       false,
       true},
      {"firebird.unsupported.low_level_utility",
       "low_level_utility",
       FirebirdMappingDisposition::kEmulatedNonFileDiagnostic,
       "",
       "",
       "",
       "",
       "",
       "",
       "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED",
       "Firebird service/repair/verification utilities are outside donor parser authority.",
       true,
       false,
       false,
       false,
       true},
      {"firebird.emulated.replication_journal",
       "replication_journal_emulation",
       FirebirdMappingDisposition::kParserSupportUdr,
       "firebird.udr.replication_journal",
       "SBLR_DONOR_FIREBIRD_REPLICATION_ROUTE",
       "sblr.archive_replication.operation.v3",
       "ParserSupportReplicationRoute",
       "firebird_replication_journal_request_v1",
       "firebird_replication_journal_result_v1",
       "FIREBIRD.EMULATION.NON_FILE_SURFACE",
       "Firebird journal/replication syntax routes through parser-support UDR policy with no donor file effects or parser authority.",
       true,
       false,
       false,
       false,
       true},
  }};
  return mappings;
}

const FirebirdLifecycleMappingDescriptor* MappingByKey(std::string_view mapping_key) {
  for (const auto& mapping : FirebirdMappingStorage()) {
    if (mapping.mapping_key == mapping_key) return &mapping;
  }
  return nullptr;
}

std::string_view StripGbakCommandPunctuation(std::string_view token) {
  while (!token.empty() && token.back() == ';') {
    token.remove_suffix(1);
  }
  return token;
}

std::vector<std::string_view> SplitAsciiWords(std::string_view text) {
  std::vector<std::string_view> words;
  std::size_t begin = 0;
  while (begin < text.size()) {
    while (begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
      ++begin;
    }
    if (begin >= text.size()) break;
    std::size_t end = begin + 1;
    while (end < text.size() &&
           std::isspace(static_cast<unsigned char>(text[end])) == 0) {
      ++end;
    }
    words.push_back(StripGbakCommandPunctuation(text.substr(begin, end - begin)));
    begin = end;
  }
  return words;
}

bool IsGbakBackupSwitch(std::string_view word) {
  return word == "-B" || word == "-BACKUP" || word == "-BACKUP_DATABASE";
}

bool IsGbakRestoreSwitch(std::string_view word) {
  return word == "-R" || word == "-RESTORE" || word == "-RESTORE_DATABASE" ||
         word == "-RECREATE" || word == "-RECREATE_DATABASE" ||
         word == "-C" || word == "-CREATE" || word == "-CREATE_DATABASE" ||
         word == "-REP" || word == "-REPLACE" || word == "-REPLACE_DATABASE";
}

bool IsGbakRecreateSwitch(std::string_view word) {
  return word == "-RECREATE" || word == "-RECREATE_DATABASE";
}

bool IsGbakSwitchWithValue(std::string_view word) {
  return word == "-BUFFERS" || word == "-BU" ||
         word == "-CRYPT" ||
         word == "-FACTOR" || word == "-FA" ||
         word == "-FETCH_PASSWORD" ||
         word == "-FIX_FSS_DATA" ||
         word == "-FIX_FSS_METADATA" ||
         word == "-INCLUDE_DATA" ||
         word == "-KEYHOLDER" ||
         word == "-KEYNAME" ||
         word == "-MODE" ||
         word == "-PAGE_SIZE" || word == "-P" ||
         word == "-PARALLEL" || word == "-PAR" ||
         word == "-PASSWORD" || word == "-PAS" ||
         word == "-REPLICA" ||
         word == "-ROLE" || word == "-RO" ||
         word == "-SERVICE" || word == "-SE" ||
         word == "-SKIP_BAD_DATA" ||
         word == "-SKIP_DATA" ||
         word == "-STATISTICS" || word == "-ST" ||
         word == "-USER" ||
         word == "-VERBINT" ||
         word == "-Y";
}

bool IsGbakFlagSwitch(std::string_view word) {
  return word == "-CONVERT" ||
         word == "-DIRECT_IO" || word == "-D" ||
         word == "-EXPAND" || word == "-E" ||
         word == "-GARBAGE_COLLECT" || word == "-G" ||
         word == "-IGNORE" || word == "-IG" ||
         word == "-INACTIVE" || word == "-I" ||
         word == "-KILL" || word == "-K" ||
         word == "-LIMBO" || word == "-L" ||
         word == "-METADATA" || word == "-META_DATA" || word == "-M" ||
         word == "-NO_VALIDITY" || word == "-N" ||
         word == "-NODBTRIGGERS" ||
         word == "-NT" ||
         word == "-OLD_DESCRIPTIONS" || word == "-OL" ||
         word == "-ONE_AT_A_TIME" || word == "-O" ||
         word == "-TRANSPORTABLE" || word == "-T" ||
         word == "-TRUSTED" ||
         word == "-UNPROTECTED" ||
         word == "-USE_ALL_SPACE" || word == "-US" ||
         word == "-VERBOSE" || word == "-VERIFY" || word == "-V" ||
         word == "-Z" ||
         word == "-ZIP";
}

std::string ClassifyGbakLogicalStreamOperation(std::string_view upper) {
  const auto words = SplitAsciiWords(TrimAsciiView(upper));
  if (words.size() < 4 || words.front() != "GBAK") return {};

  std::size_t operation_index = words.size();
  bool backup = false;
  bool restore = false;
  for (std::size_t i = 1; i < words.size(); ++i) {
    if (IsGbakBackupSwitch(words[i])) {
      if (operation_index != words.size()) return {};
      operation_index = i;
      backup = true;
      continue;
    }
    if (IsGbakRestoreSwitch(words[i])) {
      if (operation_index != words.size()) return {};
      operation_index = i;
      restore = true;
      continue;
    }
  }
  if (operation_index == words.size() || backup == restore) return {};

  std::vector<std::string_view> positional_after_operation;
  for (std::size_t i = operation_index + 1; i < words.size(); ++i) {
    if (restore && IsGbakRecreateSwitch(words[operation_index]) &&
        i == operation_index + 1 && words[i] == "OVERWRITE") {
      continue;
    }
    if (!words[i].empty() && words[i].front() == '-') {
      if (IsGbakSwitchWithValue(words[i])) {
        if (i + 1 >= words.size()) return {};
        ++i;
        continue;
      }
      if (IsGbakFlagSwitch(words[i])) {
        continue;
      }
      return {};
    }
    positional_after_operation.push_back(words[i]);
  }
  if (positional_after_operation.size() != 2) return {};

  if (backup && positional_after_operation[1] == "STDOUT") {
    return "firebird.logical_stream.gbak_backup";
  }
  if (restore && positional_after_operation[0] == "STDIN") {
    return "firebird.logical_stream.gbak_restore";
  }
  return {};
}

std::string FirebirdGbakLogicalStreamEvidenceJson(
    std::string_view operation_family,
    std::string_view active_upper) {
  const auto words = SplitAsciiWords(TrimAsciiView(active_upper));
  const bool backup_stream =
      operation_family == "firebird.logical_stream.gbak_backup";
  const bool restore_stream =
      operation_family == "firebird.logical_stream.gbak_restore";
  bool stdin_stream_bound = false;
  bool stdout_stream_bound = false;
  bool metadata_only_requested = false;
  bool data_filter_requested = false;
  bool transportable_requested = false;
  bool verbose_requested = false;
  bool parallel_requested = false;
  bool service_requested = false;
  bool recreate_requested = false;
  bool replace_requested = false;

  for (std::size_t i = 0; i < words.size(); ++i) {
    const auto word = words[i];
    stdin_stream_bound = stdin_stream_bound || word == "STDIN";
    stdout_stream_bound = stdout_stream_bound || word == "STDOUT";
    metadata_only_requested =
        metadata_only_requested || word == "-METADATA" ||
        word == "-META_DATA" || word == "-M";
    data_filter_requested =
        data_filter_requested || word == "-INCLUDE_DATA" ||
        word == "-SKIP_DATA";
    transportable_requested =
        transportable_requested || word == "-TRANSPORTABLE" || word == "-T";
    verbose_requested =
        verbose_requested || word == "-VERBOSE" || word == "-V";
    parallel_requested =
        parallel_requested || word == "-PARALLEL" || word == "-PAR";
    service_requested =
        service_requested || word == "-SERVICE" || word == "-SE";
    recreate_requested =
        recreate_requested || word == "-RECREATE" ||
        word == "-RECREATE_DATABASE" || word == "-C" ||
        word == "-CREATE" || word == "-CREATE_DATABASE";
    replace_requested =
        replace_requested || word == "-REP" || word == "-REPLACE" ||
        word == "-REPLACE_DATABASE";
  }

  std::ostringstream out;
  out << "{\"evidence_contract\":\"firebird_gbak_logical_stream_evidence.v1\","
      << "\"dialect\":\"firebird\","
      << "\"operation_family\":\"" << EscapeJson(operation_family) << "\","
      << "\"stream_tool\":\"gbak\","
      << "\"stream_direction\":\""
      << (backup_stream ? "outbound_backup" : "inbound_restore") << "\","
      << "\"remote_client_stream\":true,"
      << "\"stdin_stream_bound\":" << BoolJson(stdin_stream_bound) << ','
      << "\"stdout_stream_bound\":" << BoolJson(stdout_stream_bound) << ','
      << "\"backup_stream\":" << BoolJson(backup_stream) << ','
      << "\"restore_stream\":" << BoolJson(restore_stream) << ','
      << "\"metadata_only_requested\":"
      << BoolJson(metadata_only_requested) << ','
      << "\"data_filter_requested\":"
      << BoolJson(data_filter_requested) << ','
      << "\"transportable_requested\":"
      << BoolJson(transportable_requested) << ','
      << "\"verbose_requested\":" << BoolJson(verbose_requested) << ','
      << "\"parallel_requested\":" << BoolJson(parallel_requested) << ','
      << "\"service_requested\":" << BoolJson(service_requested) << ','
      << "\"recreate_requested\":" << BoolJson(recreate_requested) << ','
      << "\"replace_requested\":" << BoolJson(replace_requested) << ','
      << "\"single_connected_legacy_database_scope\":true,"
      << "\"server_local_file_access\":\"default_denied\","
      << "\"physical_page_copy_allowed\":false,"
      << "\"nbackup_allowed\":false,"
      << "\"raw_database_file_restore_allowed\":false,"
      << "\"raw_database_file_backup_allowed\":false,"
      << "\"logical_metadata_stream_supported\":true,"
      << "\"logical_data_stream_supported\":true,"
      << "\"sblr_requirement\":\"required_logical_stream_backup_restore_surface\","
      << "\"sblr_operation_family\":\"sblr.donor.firebird.logical_stream.v1\","
      << "\"engine_authority\":\"scratchbird_mga_catalog_sblr\","
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"real_firebird_file_effects\":false,"
      << "\"donor_sql_executed\":false,"
      << "\"source_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"runtime_equivalence_status\":\"pending_donor_native_gbak_stream_replay\","
      << "\"enterprise_readiness\":\"not_enterprise_ready\"}";
  return out.str();
}

std::string ClassifyNonFileOperation(std::string_view upper) {
  if (!ClassifyGbakLogicalStreamOperation(upper).empty()) {
    return {};
  }
  if (Contains(upper, "RAW DEVICE")) {
    return "firebird.emulated.shadow_raw_storage";
  }
  if (StartsWithCommand(upper, "CREATE DATABASE") ||
      StartsWithCommand(upper, "DROP DATABASE") ||
      StartsWithCommand(upper, "ALTER DATABASE")) {
    return "firebird.emulated.database_lifecycle";
  }
  if (StartsWithCommand(upper, "CREATE SHADOW") ||
      StartsWithCommand(upper, "DROP SHADOW") ||
      StartsWithCommand(upper, "ALTER SHADOW")) {
    return "firebird.emulated.shadow_storage";
  }
  if (StartsWithCommand(upper, "CREATE EXTERNAL TABLE")) {
    return "firebird.emulated.external_table_authority";
  }
  if (StartsWithCommand(upper, "CREATE EXTERNAL FUNCTION") ||
      StartsWithCommand(upper, "DECLARE EXTERNAL FUNCTION") ||
      StartsWithCommand(upper, "CREATE FILTER") ||
      StartsWithCommand(upper, "DECLARE FILTER") ||
      StartsWithCommand(upper, "DROP FILTER") ||
      StartsWithCommand(upper, "CREATE EXTERNAL ENGINE") ||
      StartsWithCommand(upper, "ALTER EXTERNAL ENGINE") ||
      StartsWithCommand(upper, "DROP EXTERNAL ENGINE")) {
    return "firebird.emulated.plugin_external_engine";
  }
  if (StartsWithCommand(upper, "CREATE MAPPING") ||
      StartsWithCommand(upper, "ALTER MAPPING") ||
      StartsWithCommand(upper, "DROP MAPPING")) {
    return "firebird.emulated.security_projection";
  }
  if (StartsWithCommand(upper, "CREATE JOURNAL") ||
      StartsWithCommand(upper, "ALTER JOURNAL") ||
      StartsWithCommand(upper, "DROP JOURNAL") ||
      StartsWithCommand(upper, "ARCHIVE") ||
      StartsWithCommand(upper, "REPLICATION") ||
      StartsWithCommand(upper, "CREATE REPLICA") ||
      StartsWithCommand(upper, "ALTER REPLICA") ||
      StartsWithCommand(upper, "DROP REPLICA")) {
    return "firebird.emulated.replication_journal";
  }
  if (StartsWithCommand(upper, "BACKUP") ||
      StartsWithCommand(upper, "RESTORE")) {
    return "firebird.emulated.backup_restore";
  }
  if (StartsWithCommand(upper, "NBACKUP")) {
    return "firebird.emulated.incremental_backup";
  }
  if (StartsWithCommand(upper, "VALIDATE") ||
      StartsWithCommand(upper, "REPAIR") ||
      StartsWithCommand(upper, "SWEEP")) {
    return "firebird.emulated.validation_repair_sweep";
  }
  if (StartsWithCommand(upper, "TRACE")) {
    return "firebird.emulated.trace_monitoring";
  }
  if (StartsWithCommand(upper, "SERVICE")) {
    return "firebird.emulated.service_api";
  }
  if (StartsWithCommand(upper, "GBAK") ||
      StartsWithCommand(upper, "GFIX") ||
      StartsWithCommand(upper, "GSTAT") ||
      StartsWithCommand(upper, "GSEC") ||
      StartsWithCommand(upper, "GPRE") ||
      StartsWithCommand(upper, "GSPLIT") ||
      StartsWithCommand(upper, "FBGUARD") ||
      StartsWithCommand(upper, "FB_LOCK_PRINT") ||
      StartsWithCommand(upper, "FBSVCMGR") ||
      StartsWithCommand(upper, "FBTRACEMGR")) {
    return "firebird.emulated.donor_native_tool";
  }
  return {};
}

bool ReferencesCatalogOverlay(std::string_view upper) {
  return Contains(upper, "RDB$") || Contains(upper, "MON$") ||
         Contains(upper, "SEC$") || Contains(upper, "INFORMATION_SCHEMA.");
}

std::string_view ConsumeLeadingKeyword(std::string_view value,
                                       std::string_view keyword) {
  value = TrimAsciiView(value);
  if (!StartsWithCommand(value, keyword)) return {};
  return TrimAsciiView(value.substr(keyword.size()));
}

std::string FirstIdentifier(std::string_view value) {
  value = TrimAsciiView(value);
  if (value.empty()) return {};
  if (value.front() == '"') {
    std::string identifier;
    for (std::size_t i = 1; i < value.size(); ++i) {
      if (value[i] == '"' && i + 1 < value.size() && value[i + 1] == '"') {
        identifier.push_back('"');
        ++i;
        continue;
      }
      if (value[i] == '"') break;
      identifier.push_back(value[i]);
    }
    return identifier;
  }
  std::size_t end = 0;
  while (end < value.size()) {
    const char ch = value[end];
    if (!IsIdentifierChar(ch) && ch != '.') break;
    ++end;
  }
  return std::string(value.substr(0, end));
}

bool IsCatalogOverlayTarget(std::string_view name) {
  const auto upper = ToUpperAscii(name);
  return StartsWith(upper, "RDB$") || StartsWith(upper, "MON$") ||
         StartsWith(upper, "SEC$") ||
         StartsWith(upper, "INFORMATION_SCHEMA.");
}

bool MutatesCatalogOverlay(std::string_view upper) {
  if (!ReferencesCatalogOverlay(upper)) return false;
  if (auto rest = ConsumeLeadingKeyword(upper, "INSERT"); !rest.empty()) {
    rest = ConsumeLeadingKeyword(rest, "INTO");
    return !rest.empty() && IsCatalogOverlayTarget(FirstIdentifier(rest));
  }
  if (auto rest = ConsumeLeadingKeyword(upper, "UPDATE"); !rest.empty()) {
    return IsCatalogOverlayTarget(FirstIdentifier(rest));
  }
  if (auto rest = ConsumeLeadingKeyword(upper, "DELETE"); !rest.empty()) {
    rest = ConsumeLeadingKeyword(rest, "FROM");
    return !rest.empty() && IsCatalogOverlayTarget(FirstIdentifier(rest));
  }
  if (auto rest = ConsumeLeadingKeyword(upper, "MERGE"); !rest.empty()) {
    rest = ConsumeLeadingKeyword(rest, "INTO");
    return !rest.empty() && IsCatalogOverlayTarget(FirstIdentifier(rest));
  }
  return false;
}

bool HasBalancedParentheses(std::string_view text) {
  int depth = 0;
  bool in_string = false;
  bool in_quoted_identifier = false;
  auto q_quote_close = [](char open) {
    switch (open) {
      case '{': return '}';
      case '[': return ']';
      case '(': return ')';
      case '<': return '>';
      default: return open;
    }
  };
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (!in_string && !in_quoted_identifier &&
        (ch == 'q' || ch == 'Q') && i + 2 < text.size() &&
        text[i + 1] == '\'' && text[i + 2] != '\'') {
      const char delimiter = q_quote_close(text[i + 2]);
      i += 3;
      while (i + 1 < text.size()) {
        if (text[i] == delimiter && text[i + 1] == '\'') {
          ++i;
          break;
        }
        ++i;
      }
      continue;
    }
    if (in_string) {
      if (ch == '\'' && i + 1 < text.size() && text[i + 1] == '\'') {
        ++i;
        continue;
      }
      if (ch == '\'') in_string = false;
      continue;
    }
    if (in_quoted_identifier) {
      if (ch == '"' && i + 1 < text.size() && text[i + 1] == '"') {
        ++i;
        continue;
      }
      if (ch == '"') in_quoted_identifier = false;
      continue;
    }
    if (ch == '\'') {
      in_string = true;
      continue;
    }
    if (ch == '"') {
      in_quoted_identifier = true;
      continue;
    }
    if (ch == '(') {
      ++depth;
      continue;
    }
    if (ch == ')') {
      --depth;
      if (depth < 0) return false;
    }
  }
  return depth == 0 && !in_string && !in_quoted_identifier;
}

std::string ClassifyExpressionOperation(std::string_view upper) {
  if (!(StartsWithCommand(upper, "SELECT") || StartsWithCommand(upper, "WITH"))) return {};
  if (Contains(upper, "GEN_ID(")) {
    return "firebird.expression.generator.gen_id";
  }
  if (Contains(upper, "NEXT VALUE FOR")) {
    return "firebird.expression.generator.next_value_for";
  }
  if (Contains(upper, "RDB$GET_CONTEXT(")) {
    return "firebird.expression.context.get";
  }
  if (Contains(upper, "RDB$SET_CONTEXT(")) {
    return "firebird.expression.context.set";
  }
  if (Contains(upper, "CURRENT_CONNECTION") ||
      Contains(upper, "CURRENT_TRANSACTION") ||
      Contains(upper, "CURRENT_ROLE") ||
      Contains(upper, "CURRENT_USER") ||
      Contains(upper, "CURRENT_SCHEMA")) {
    return "firebird.expression.context.variable";
  }
  if (Contains(upper, "UUID_TO_CHAR(")) {
    return "firebird.expression.uuid.uuid_to_char";
  }
  if (Contains(upper, "CHAR_TO_UUID(")) {
    return "firebird.expression.uuid.char_to_uuid";
  }
  if (Contains(upper, "HASH(")) {
    return "firebird.expression.hash.hash";
  }
  if (Contains(upper, "DATEADD(") || Contains(upper, "DATEDIFF(") ||
      Contains(upper, "EXTRACT(") || Contains(upper, "CURRENT_DATE") ||
      Contains(upper, "CURRENT_TIME") || Contains(upper, "CURRENT_TIMESTAMP")) {
    return "firebird.expression.temporal";
  }
  if (Contains(upper, "COUNT(") || Contains(upper, "SUM(") ||
      Contains(upper, "AVG(") || Contains(upper, "ROW_NUMBER(") ||
      Contains(upper, "RANK(") || Contains(upper, "LAG(") ||
      Contains(upper, "LEAD(")) {
    return "firebird.expression.aggregate_window";
  }
  if (Contains(upper, "COALESCE(") || Contains(upper, "NULLIF(") ||
      Contains(upper, "IIF(") || Contains(upper, "DECODE(") ||
      Contains(upper, "CASE ")) {
    return "firebird.expression.conditional";
  }
  if (Contains(upper, "UPPER(") || Contains(upper, "LOWER(") ||
      Contains(upper, "TRIM(") || Contains(upper, "SUBSTRING(") ||
      Contains(upper, "CHAR_LENGTH(") || Contains(upper, "OCTET_LENGTH(")) {
    return "firebird.expression.string";
  }
  if (Contains(upper, "ABS(") || Contains(upper, "ROUND(") ||
      Contains(upper, "POWER(") || Contains(upper, "SQRT(") ||
      Contains(upper, "MOD(")) {
    return "firebird.expression.numeric";
  }
  return {};
}

std::string ClassifyDatatypeOperation(std::string_view upper) {
  if (StartsWithCommand(upper, "CREATE DOMAIN")) {
    return "firebird.datatype.domain.create";
  }
  if (StartsWithCommand(upper, "ALTER DOMAIN")) {
    return "firebird.datatype.domain.alter";
  }
  if (StartsWithCommand(upper, "DROP DOMAIN")) {
    return "firebird.datatype.domain.drop";
  }
  if (StartsWithCommand(upper, "SELECT") || StartsWithCommand(upper, "WITH")) {
    if (Contains(upper, "CAST(")) return "firebird.datatype.cast";
    if (Contains(upper, "DATE '") || Contains(upper, "TIME '") ||
        Contains(upper, "TIMESTAMP '")) {
      return "firebird.datatype.temporal_literal";
    }
    if (Contains(upper, "DECFLOAT") || Contains(upper, "INT128") ||
        Contains(upper, "NUMERIC(") || Contains(upper, "DECIMAL(")) {
      return "firebird.datatype.exact_numeric_descriptor";
    }
    if (Contains(upper, "BLOB") || Contains(upper, "SUB_TYPE") ||
        Contains(upper, "CHARACTER SET") || Contains(upper, "COLLATE ")) {
      return "firebird.datatype.text_blob_descriptor";
    }
    if (Contains(upper, " TRUE") || Contains(upper, " FALSE") ||
        Contains(upper, " UNKNOWN")) {
      return "firebird.datatype.boolean_literal";
    }
  }
  return {};
}

std::string ClassifyCatalogOverlayOperation(std::string_view upper) {
  if (!(StartsWithCommand(upper, "SELECT") || StartsWithCommand(upper, "WITH"))) return {};
  if (Contains(upper, "INFORMATION_SCHEMA.")) {
    return "firebird.catalog_overlay.information_schema";
  }
  if (Contains(upper, "MON$")) {
    return "firebird.catalog_overlay.monitoring";
  }
  if (Contains(upper, "SEC$") ||
      Contains(upper, "RDB$USER_PRIVILEGES") ||
      Contains(upper, "RDB$ROLES") ||
      Contains(upper, "RDB$AUTH_MAPPING")) {
    return "firebird.catalog_overlay.security";
  }
  if (Contains(upper, "RDB$INDICES") ||
      Contains(upper, "RDB$INDEX_SEGMENTS") ||
      Contains(upper, "RDB$RELATION_CONSTRAINTS") ||
      Contains(upper, "RDB$REF_CONSTRAINTS") ||
      Contains(upper, "RDB$CHECK_CONSTRAINTS")) {
    return "firebird.catalog_overlay.constraints_indexes";
  }
  if (Contains(upper, "RDB$PROCEDURES") ||
      Contains(upper, "RDB$FUNCTIONS") ||
      Contains(upper, "RDB$TRIGGERS") ||
      Contains(upper, "RDB$PACKAGES") ||
      Contains(upper, "RDB$DEPENDENCIES")) {
    return "firebird.catalog_overlay.routines_triggers_packages";
  }
  if (Contains(upper, "RDB$EXCEPTIONS") ||
      Contains(upper, "RDB$CHARACTER_SETS") ||
      Contains(upper, "RDB$COLLATIONS") ||
      Contains(upper, "RDB$FILTERS")) {
    return "firebird.catalog_overlay.exceptions_collations_charsets";
  }
  if (Contains(upper, "RDB$")) {
    return "firebird.catalog_overlay.rdb_core";
  }
  return {};
}

std::string ClassifyStatisticsOptimizerOperation(std::string_view upper) {
  if (StartsWithCommand(upper, "SET STATISTICS INDEX")) {
    return "firebird.statistics.set_index_statistics";
  }
  return {};
}

std::string ClassifyPsqlOperation(std::string_view upper) {
  if (StartsWithCommand(upper, "EXECUTE BLOCK")) {
    return "firebird.psql.execute_block";
  }
  if (StartsWithCommand(upper, "EXECUTE STATEMENT")) {
    return "firebird.psql.execute_statement";
  }
  if (StartsWithCommand(upper, "BEGIN") ||
      StartsWithCommand(upper, "END") ||
      StartsWithCommand(upper, "SUSPEND") ||
      StartsWithCommand(upper, "DECLARE") ||
      StartsWithCommand(upper, "IN AUTONOMOUS TRANSACTION") ||
      StartsWithCommand(upper, "WHEN") ||
      StartsWithCommand(upper, "FOR")) {
    return "firebird.psql.block_fragment";
  }
  if (Contains(upper, "=") && !Contains(upper, "==") &&
      !(StartsWithCommand(upper, "SELECT") ||
        StartsWithCommand(upper, "WITH") ||
        StartsWithCommand(upper, "INSERT") ||
        StartsWithCommand(upper, "UPDATE") ||
        StartsWithCommand(upper, "DELETE") ||
        StartsWithCommand(upper, "MERGE") ||
        StartsWithCommand(upper, "CREATE") ||
        StartsWithCommand(upper, "ALTER") ||
        StartsWithCommand(upper, "DROP") ||
        StartsWithCommand(upper, "RECREATE") ||
        StartsWithCommand(upper, "SET"))) {
    return "firebird.psql.assignment_fragment";
  }
  return {};
}

std::string ClassifyQueryOperation(std::string_view upper) {
  if (!(StartsWithCommand(upper, "SELECT") ||
        StartsWithCommand(upper, "WITH") ||
        (StartsWith(upper, "(") && Contains(upper, "SELECT")))) {
    return {};
  }
  if (ContainsWord(upper, "FIRST") || ContainsWord(upper, "SKIP") ||
      ContainsWord(upper, "ROWS")) {
    return "firebird.query.select.first_skip_rows";
  }
  if (Contains(upper, " FOR UPDATE")) {
    return "firebird.query.cursor.for_update";
  }
  return "firebird.query.select";
}

std::string ClassifyDmlOperation(std::string_view upper) {
  if (Contains(upper, " WHERE CURRENT OF ")) {
    if (StartsWithCommand(upper, "UPDATE")) {
      return "firebird.dml.cursor.update_current_of";
    }
    if (StartsWithCommand(upper, "DELETE")) {
      return "firebird.dml.cursor.delete_current_of";
    }
  }
  if (StartsWithCommand(upper, "INSERT")) {
    return ContainsWord(upper, "RETURNING") ? "firebird.dml.insert.returning"
                                            : "firebird.dml.insert";
  }
  if (StartsWithCommand(upper, "UPDATE OR INSERT")) {
    return ContainsWord(upper, "RETURNING")
               ? "firebird.dml.update_or_insert.returning"
               : "firebird.dml.update_or_insert";
  }
  if (StartsWithCommand(upper, "UPDATE")) {
    return ContainsWord(upper, "RETURNING") ? "firebird.dml.update.returning"
                                            : "firebird.dml.update";
  }
  if (StartsWithCommand(upper, "DELETE")) {
    return ContainsWord(upper, "RETURNING") ? "firebird.dml.delete.returning"
                                            : "firebird.dml.delete";
  }
  if (StartsWithCommand(upper, "MERGE")) {
    return ContainsWord(upper, "RETURNING") ? "firebird.dml.merge.returning"
                                            : "firebird.dml.merge";
  }
  if (StartsWithCommand(upper, "EXECUTE PROCEDURE")) {
    return "firebird.dml.execute_procedure";
  }
  if (StartsWithCommand(upper, "CALL")) return "firebird.dml.call";
  return {};
}

std::string ClassifyRoutineDdlOperation(std::string_view verb,
                                        std::string_view rest) {
  if (StartsWithCommand(rest, "PACKAGE BODY")) {
    return "firebird.ddl." + std::string(verb) + ".package_body";
  }
  if (StartsWithCommand(rest, "PROCEDURE")) {
    return "firebird.ddl." + std::string(verb) + ".procedure";
  }
  if (StartsWithCommand(rest, "FUNCTION")) {
    return "firebird.ddl." + std::string(verb) + ".function";
  }
  if (StartsWithCommand(rest, "PACKAGE")) {
    return "firebird.ddl." + std::string(verb) + ".package";
  }
  if (StartsWithCommand(rest, "TRIGGER")) {
    return "firebird.ddl." + std::string(verb) + ".trigger";
  }
  if (StartsWithCommand(rest, "EXCEPTION")) {
    return "firebird.ddl." + std::string(verb) + ".exception";
  }
  if (StartsWithCommand(rest, "SEQUENCE") || StartsWithCommand(rest, "GENERATOR")) {
    return "firebird.ddl." + std::string(verb) + ".sequence";
  }
  if (StartsWithCommand(rest, "ROLE")) {
    return "firebird.ddl." + std::string(verb) + ".role";
  }
  if (StartsWithCommand(rest, "USER")) {
    return "firebird.ddl." + std::string(verb) + ".user";
  }
  return {};
}

std::string ClassifyDdlOperation(std::string_view upper) {
  if (StartsWithCommand(upper, "CREATE OR ALTER")) {
    const auto rest = upper.substr(std::string_view("CREATE OR ALTER").size());
    if (const auto routine = ClassifyRoutineDdlOperation("create_or_alter", TrimAsciiView(rest));
        !routine.empty()) {
      return routine;
    }
    return "firebird.ddl.create_or_alter";
  }
  if (StartsWithCommand(upper, "CREATE")) {
    const auto rest = upper.substr(std::string_view("CREATE").size());
    if (StartsWithCommand(TrimAsciiView(rest), "GLOBAL TEMPORARY TABLE")) {
      return "firebird.ddl.create.global_temporary_table";
    }
    if (StartsWithCommand(TrimAsciiView(rest), "VIEW")) {
      return "firebird.ddl.create.view";
    }
    if (scratchbird::parser::donor::IsIndexSemanticDefaultsStatement(upper)) {
      return ContainsWord(rest, "UNIQUE") ? "firebird.ddl.create.unique_index"
                                          : "firebird.ddl.create.index";
    }
    if (const auto routine = ClassifyRoutineDdlOperation("create", TrimAsciiView(rest));
        !routine.empty()) {
      return routine;
    }
    return "firebird.ddl.create";
  }
  if (StartsWithCommand(upper, "ALTER")) {
    const auto rest = upper.substr(std::string_view("ALTER").size());
    if (StartsWithCommand(TrimAsciiView(rest), "INDEX")) {
      return "firebird.ddl.alter.index";
    }
    if (const auto routine = ClassifyRoutineDdlOperation("alter", TrimAsciiView(rest));
        !routine.empty()) {
      return routine;
    }
    return "firebird.ddl.alter";
  }
  if (StartsWithCommand(upper, "DROP")) {
    const auto rest = upper.substr(std::string_view("DROP").size());
    if (const auto routine = ClassifyRoutineDdlOperation("drop", TrimAsciiView(rest));
        !routine.empty()) {
      return routine;
    }
    return "firebird.ddl.drop";
  }
  if (StartsWithCommand(upper, "RECREATE")) {
    const auto rest = upper.substr(std::string_view("RECREATE").size());
    if (const auto routine =
            ClassifyRoutineDdlOperation("recreate", TrimAsciiView(rest));
        !routine.empty()) {
      return routine;
    }
    return "firebird.ddl.recreate";
  }
  if (StartsWithCommand(upper, "COMMENT")) return "firebird.ddl.comment";
  if (StartsWithCommand(upper, "GRANT")) return "firebird.ddl.grant";
  if (StartsWithCommand(upper, "REVOKE")) return "firebird.ddl.revoke";
  return {};
}

std::string ClassifyIsqlOperation(std::string_view upper) {
  if (upper == ";" || upper == "!") return "firebird.isql.noop";
  if (StartsWith(upper, "(") && ContainsWord(upper, "STOP")) {
    return "firebird.isql.input_data_block";
  }
  if (StartsWithCommand(upper, "CONNECT")) return "firebird.isql.connect";
  if (StartsWithCommand(upper, "DISCONNECT")) return "firebird.isql.disconnect";
  if (StartsWithCommand(upper, "SET") &&
      !StartsWithCommand(upper, "SET TRANSACTION")) {
    return "firebird.isql.set";
  }
  if (StartsWithCommand(upper, "SHOW")) return "firebird.isql.show";
  if (StartsWithCommand(upper, "EXTRACT")) return "firebird.isql.extract";
  if (StartsWithCommand(upper, "IN")) return "firebird.isql.input";
  if (StartsWithCommand(upper, "OUT")) return "firebird.isql.output";
  if (StartsWithCommand(upper, "INPUT")) return "firebird.isql.input";
  if (StartsWithCommand(upper, "OUTPUT")) return "firebird.isql.output";
  if (StartsWithCommand(upper, "HELP")) return "firebird.isql.help";
  if (StartsWithCommand(upper, "QUIT") || StartsWithCommand(upper, "EXIT")) {
    return "firebird.isql.exit";
  }
  if (StartsWithCommand(upper, "BLOBDUMP") ||
      StartsWithCommand(upper, "BLOBVIEW") ||
      StartsWithCommand(upper, "EDIT") ||
      StartsWithCommand(upper, "SHELL")) {
    return "firebird.isql.frontend_utility";
  }
  return {};
}

} // namespace

std::string TrimAscii(std::string_view text) {
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
  return std::string(text.substr(begin, end - begin));
}

std::string NormalizeWhitespace(std::string_view text) {
  const auto trimmed = TrimAscii(text);
  std::string normalized;
  normalized.reserve(trimmed.size());
  bool previous_space = false;
  for (const char ch : trimmed) {
    if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
      if (!previous_space) normalized.push_back(' ');
      previous_space = true;
      continue;
    }
    normalized.push_back(ch);
    previous_space = false;
  }
  return normalized;
}

std::string ToUpperAscii(std::string_view text) {
  std::string upper;
  upper.reserve(text.size());
  for (const char ch : text) {
    upper.push_back(static_cast<char>(
        std::toupper(static_cast<unsigned char>(ch))));
  }
  return upper;
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
    for (std::size_t f = 0; f < diagnostic.fields.size(); ++f) {
      if (f != 0) out << ',';
      out << "\"" << EscapeJson(diagnostic.fields[f].name) << "\":\""
          << EscapeJson(diagnostic.fields[f].value) << "\"";
    }
    out << "}}";
  }
  out << "]}";
  return out.str();
}

std::vector<Token> LexTokens(std::string_view sql_text) {
  std::vector<Token> tokens;
  std::size_t i = 0;
  while (i < sql_text.size()) {
    const auto ch = sql_text[i];
    if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
      ++i;
      continue;
    }
    if (ch == '-' && i + 1 < sql_text.size() && sql_text[i + 1] == '-') {
      const auto begin = i;
      i += 2;
      while (i < sql_text.size() && sql_text[i] != '\n') ++i;
      tokens.push_back({"line_comment", std::string(sql_text.substr(begin, i - begin)), begin});
      continue;
    }
    if (ch == '/' && i + 1 < sql_text.size() && sql_text[i + 1] == '*') {
      const auto begin = i;
      i += 2;
      while (i + 1 < sql_text.size() && !(sql_text[i] == '*' && sql_text[i + 1] == '/')) {
        ++i;
      }
      if (i + 1 < sql_text.size()) i += 2;
      tokens.push_back({"block_comment", std::string(sql_text.substr(begin, i - begin)), begin});
      continue;
    }
    if (ch == '"') {
      const auto begin = i++;
      while (i < sql_text.size()) {
        if (sql_text[i] == '"' && i + 1 < sql_text.size() && sql_text[i + 1] == '"') {
          i += 2;
          continue;
        }
        if (sql_text[i++] == '"') break;
      }
      tokens.push_back({"quoted_identifier", std::string(sql_text.substr(begin, i - begin)), begin});
      continue;
    }
    if (ch == '\'') {
      const auto begin = i++;
      while (i < sql_text.size()) {
        if (sql_text[i] == '\'' && i + 1 < sql_text.size() && sql_text[i + 1] == '\'') {
          i += 2;
          continue;
        }
        if (sql_text[i++] == '\'') break;
      }
      tokens.push_back({"string_literal", std::string(sql_text.substr(begin, i - begin)), begin});
      continue;
    }
    if (ch == '?' || ch == ':') {
      const auto begin = i++;
      if (ch == ':') {
        while (i < sql_text.size()) {
          const auto c = static_cast<unsigned char>(sql_text[i]);
          if (std::isalnum(c) == 0 && sql_text[i] != '_' && sql_text[i] != '$') break;
          ++i;
        }
      }
      tokens.push_back({"parameter", std::string(sql_text.substr(begin, i - begin)), begin});
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(ch)) != 0) {
      const auto begin = i++;
      bool seen_dot = false;
      while (i < sql_text.size()) {
        const auto c = static_cast<unsigned char>(sql_text[i]);
        if (std::isdigit(c) != 0) {
          ++i;
          continue;
        }
        if (!seen_dot && sql_text[i] == '.') {
          seen_dot = true;
          ++i;
          continue;
        }
        break;
      }
      tokens.push_back({"numeric_literal", std::string(sql_text.substr(begin, i - begin)), begin});
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '$') {
      const auto begin = i++;
      while (i < sql_text.size()) {
        const auto c = static_cast<unsigned char>(sql_text[i]);
        if (std::isalnum(c) == 0 && sql_text[i] != '_' && sql_text[i] != '$') break;
        ++i;
      }
      tokens.push_back({"identifier_or_keyword",
                        std::string(sql_text.substr(begin, i - begin)), begin});
      continue;
    }
    tokens.push_back({"punctuation", std::string(sql_text.substr(i, 1)), i});
    ++i;
  }
  return tokens;
}

bool IsNonFileEmulatedOperation(std::string_view normalized_upper_sql) {
  return !ClassifyNonFileOperation(normalized_upper_sql).empty();
}

std::span<const FirebirdLifecycleMappingDescriptor> FirebirdLifecycleMappings() {
  const auto& mappings = FirebirdMappingStorage();
  return {mappings.data(), mappings.size()};
}

const FirebirdLifecycleMappingDescriptor* FindFirebirdLifecycleMappingByOperationId(
    std::string_view operation_id) {
  for (const auto& mapping : FirebirdLifecycleMappings()) {
    if (mapping.operation_id == operation_id) return &mapping;
  }
  return nullptr;
}

const FirebirdLifecycleMappingDescriptor* MapFirebirdLifecycleCommand(
    std::string_view normalized_upper_sql) {
  const auto upper = TrimAsciiView(normalized_upper_sql);
  if (!ClassifyGbakLogicalStreamOperation(upper).empty()) {
    return nullptr;
  }
  if (StartsWithCommand(upper, "CREATE DATABASE")) {
    return MappingByKey("firebird.lifecycle.create_database");
  }
  if (StartsWithCommand(upper, "DROP DATABASE")) {
    return MappingByKey("firebird.lifecycle.drop_database");
  }
  if (StartsWithCommand(upper, "CONNECT")) {
    return MappingByKey("firebird.lifecycle.attach_database");
  }
  if (StartsWithCommand(upper, "DISCONNECT")) {
    return MappingByKey("firebird.lifecycle.detach_database");
  }
  if (StartsWithCommand(upper, "VALIDATE")) {
    return MappingByKey("firebird.lifecycle.verify_database");
  }
  if (StartsWithCommand(upper, "REPAIR")) {
    return MappingByKey("firebird.lifecycle.repair_database");
  }
  if (StartsWithCommand(upper, "SWEEP") ||
      StartsWithCommand(upper, "SERVICE") ||
      StartsWithCommand(upper, "GBAK") ||
      StartsWithCommand(upper, "GFIX") ||
      StartsWithCommand(upper, "GSTAT") ||
      StartsWithCommand(upper, "NBACKUP") ||
      StartsWithCommand(upper, "GSEC") ||
      StartsWithCommand(upper, "GPRE") ||
      StartsWithCommand(upper, "GSPLIT") ||
      StartsWithCommand(upper, "FBGUARD") ||
      StartsWithCommand(upper, "FB_LOCK_PRINT") ||
      StartsWithCommand(upper, "FBSVCMGR") ||
      StartsWithCommand(upper, "FBTRACEMGR")) {
    return MappingByKey("firebird.unsupported.low_level_utility");
  }
  if (StartsWithCommand(upper, "ALTER DATABASE")) {
    return MappingByKey("firebird.emulated.database_file_management");
  }
  if (StartsWithCommand(upper, "CREATE SHADOW") ||
      StartsWithCommand(upper, "ALTER SHADOW") ||
      StartsWithCommand(upper, "DROP SHADOW") ||
      Contains(upper, "RAW DEVICE")) {
    return MappingByKey("firebird.emulated.shadow_storage");
  }
  if (StartsWithCommand(upper, "BACKUP") ||
      StartsWithCommand(upper, "RESTORE")) {
    return MappingByKey("firebird.emulated.backup_restore");
  }
  if (StartsWithCommand(upper, "CREATE EXTERNAL TABLE") ||
      StartsWithCommand(upper, "CREATE EXTERNAL FUNCTION") ||
      StartsWithCommand(upper, "DECLARE EXTERNAL FUNCTION") ||
      StartsWithCommand(upper, "CREATE FILTER") ||
      StartsWithCommand(upper, "DECLARE FILTER") ||
      StartsWithCommand(upper, "DROP FILTER") ||
      StartsWithCommand(upper, "CREATE EXTERNAL ENGINE") ||
      StartsWithCommand(upper, "ALTER EXTERNAL ENGINE") ||
      StartsWithCommand(upper, "DROP EXTERNAL ENGINE")) {
    return MappingByKey("firebird.emulated.external_plugin");
  }
  if (StartsWithCommand(upper, "SERVICE") ||
      StartsWithCommand(upper, "GBAK") ||
      StartsWithCommand(upper, "GFIX") ||
      StartsWithCommand(upper, "GSTAT") ||
      StartsWithCommand(upper, "GSEC") ||
      StartsWithCommand(upper, "GPRE") ||
      StartsWithCommand(upper, "GSPLIT") ||
      StartsWithCommand(upper, "FBGUARD") ||
      StartsWithCommand(upper, "FB_LOCK_PRINT") ||
      StartsWithCommand(upper, "FBSVCMGR") ||
      StartsWithCommand(upper, "FBTRACEMGR")) {
    return MappingByKey("firebird.emulated.service_api");
  }
  if (StartsWithCommand(upper, "CREATE JOURNAL") ||
      StartsWithCommand(upper, "ALTER JOURNAL") ||
      StartsWithCommand(upper, "DROP JOURNAL") ||
      StartsWithCommand(upper, "ARCHIVE") ||
      StartsWithCommand(upper, "REPLICATION") ||
      StartsWithCommand(upper, "CREATE REPLICA") ||
      StartsWithCommand(upper, "ALTER REPLICA") ||
      StartsWithCommand(upper, "DROP REPLICA")) {
    return MappingByKey("firebird.emulated.replication_journal");
  }
  return nullptr;
}

std::string_view FirebirdMappingDispositionName(FirebirdMappingDisposition disposition) {
  switch (disposition) {
    case FirebirdMappingDisposition::kScratchBirdLifecycleApi:
      return "scratchbird_lifecycle_api";
    case FirebirdMappingDisposition::kParserSupportUdr:
      return "parser_support_udr";
    case FirebirdMappingDisposition::kEmulatedNonFileDiagnostic:
      return "emulated_non_file_diagnostic";
  }
  return "emulated_non_file_diagnostic";
}

const std::vector<SurfaceDescriptor>& DatatypeSurfaces() {
  static const std::vector<SurfaceDescriptor> surfaces = {
      {"exact_numeric", "SMALLINT INTEGER BIGINT INT128 NUMERIC DECIMAL DECFLOAT FLOAT DOUBLE", "sbl_firebird_dialect"},
      {"character", "CHAR VARCHAR NCHAR NATIONAL CHARACTER CHARACTER SET COLLATE", "sbl_firebird_dialect"},
      {"binary_blob", "BINARY VARBINARY BLOB SUB_TYPE SEGMENT SIZE BLOB_ID", "sbl_firebird_dialect"},
      {"temporal", "DATE TIME TIMESTAMP TIME WITH TIME ZONE TIMESTAMP WITH TIME ZONE", "sbl_firebird_dialect"},
      {"boolean", "BOOLEAN TRUE FALSE UNKNOWN", "sbl_firebird_dialect"},
      {"domain", "CREATE DOMAIN ALTER DOMAIN DEFAULT CHECK NOT NULL COLLATE", "sbl_firebird_dialect"},
      {"array", "ARRAY dimensions slices descriptors", "sbl_firebird_wire"},
      {"pseudo_system", "RDB$DB_KEY ROW_COUNT SQLCODE SQLSTATE GDSCODE context variables", "sbl_firebird_dialect"},
  };
  return surfaces;
}

const std::vector<SurfaceDescriptor>& BuiltinFunctionSurfaces() {
  static const std::vector<SurfaceDescriptor> surfaces = {
      {"string", "ASCII_CHAR ASCII_VAL BIT_LENGTH CHAR_LENGTH OCTET_LENGTH OVERLAY POSITION REPLACE SUBSTRING TRIM UPPER LOWER", "sbl_firebird_dialect"},
      {"numeric", "ABS CEIL CEILING EXP FLOOR LN LOG LOG10 MOD POWER RAND ROUND SIGN SQRT TRUNC", "sbl_firebird_dialect"},
      {"temporal", "CURRENT_DATE CURRENT_TIME CURRENT_TIMESTAMP DATEADD DATEDIFF EXTRACT LOCALTIME LOCALTIMESTAMP", "sbl_firebird_dialect"},
      {"aggregate_window", "COUNT SUM AVG MIN MAX LIST EVERY ANY_VALUE RANK DENSE_RANK ROW_NUMBER FIRST_VALUE LAST_VALUE LAG LEAD", "sbl_firebird_dialect"},
      {"conditional", "COALESCE NULLIF IIF DECODE CASE", "sbl_firebird_dialect"},
      {"context", "CURRENT_CONNECTION CURRENT_ROLE CURRENT_TRANSACTION CURRENT_USER CURRENT_SCHEMA RDB$GET_CONTEXT RDB$SET_CONTEXT", "sbl_firebird_dialect"},
      {"generator_sequence", "GEN_ID NEXT VALUE FOR CREATE SEQUENCE ALTER SEQUENCE RESTART WITH", "sbl_firebird_dialect"},
      {"hash_crypto_uuid", "HASH UUID_TO_CHAR CHAR_TO_UUID cryptographic plugin-visible functions", "sbl_firebird_dialect"},
  };
  return surfaces;
}

const std::vector<SurfaceDescriptor>& CatalogOverlaySurfaces() {
  static const std::vector<SurfaceDescriptor> surfaces = {
      {"rdb_core", "RDB$DATABASE RDB$RELATIONS RDB$RELATION_FIELDS RDB$FIELDS RDB$TYPES", "sbl_firebird_catalog_overlay"},
      {"constraints_indexes", "RDB$INDICES RDB$INDEX_SEGMENTS RDB$RELATION_CONSTRAINTS RDB$REF_CONSTRAINTS RDB$CHECK_CONSTRAINTS", "sbl_firebird_catalog_overlay"},
      {"routines_triggers_packages", "RDB$PROCEDURES RDB$FUNCTIONS RDB$TRIGGERS RDB$PACKAGES RDB$DEPENDENCIES", "sbl_firebird_catalog_overlay"},
      {"security", "RDB$USER_PRIVILEGES RDB$ROLES RDB$AUTH_MAPPING SEC$USERS SEC$USER_ATTRIBUTES", "sbl_firebird_catalog_overlay"},
      {"monitoring", "MON$DATABASE MON$ATTACHMENTS MON$TRANSACTIONS MON$STATEMENTS MON$CALL_STACK MON$IO_STATS MON$RECORD_STATS", "sbl_firebird_catalog_overlay"},
      {"exceptions_collations_charsets", "RDB$EXCEPTIONS RDB$CHARACTER_SETS RDB$COLLATIONS RDB$FILTERS", "sbl_firebird_catalog_overlay"},
      {"information_schema", "optional INFORMATION_SCHEMA compatibility views", "sbl_firebird_catalog_overlay"},
  };
  return surfaces;
}

const std::vector<SurfaceDescriptor>& DiagnosticSurfaces() {
  static const std::vector<SurfaceDescriptor> surfaces = {
      {"parse_lex_syntax", "lexer parser CST AST invalid input", "sbl_firebird_diagnostic"},
      {"binder_resolution", "name resolution UUID cache descriptor hidden-vs-missing privilege projection", "sbl_firebird_diagnostic"},
      {"datatype_cast", "conversion overflow truncation charset collation array blob domain check", "sbl_firebird_diagnostic"},
      {"psql_dynamic_sql", "PSQL handlers exceptions execute statement dynamic SQL UDR parse path", "sbl_firebird_diagnostic"},
      {"non_file_authority", "create database shadow backup restore external table trace plugin service file-effect attempts FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED", "sbl_firebird_diagnostic"},
      {"wire_service_api", "DPB TPB SPB BPB BLR SQLDA wire frame service operation diagnostics", "sbl_firebird_wire"},
      {"donor_native_tool", "isql gbak gfix gstat nbackup fbsvcmgr fbtracemgr gsec normalized diagnostics", "sbl_firebird_wire"},
  };
  return surfaces;
}

ParseResult ParseStatement(std::string_view sql_text) {
  ParseResult result;
  result.normalized_sql = NormalizeWhitespace(sql_text);
  const auto active_normalized = MaskInactiveSqlText(result.normalized_sql);
  const auto active_upper = ToUpperAscii(active_normalized);
  const auto tokens = LexTokens(result.normalized_sql);
  auto parser_evidence = BuildParserEvidence(active_upper, tokens);
  result.parser_evidence_json = ParserEvidenceJson(parser_evidence);
  const auto gbak_logical_stream_operation =
      ClassifyGbakLogicalStreamOperation(active_upper);
  const auto* lifecycle_mapping = MapFirebirdLifecycleCommand(active_upper);
  if (lifecycle_mapping != nullptr) {
    result.lifecycle_operation_id = std::string(lifecycle_mapping->operation_id);
    result.sblr_operation = std::string(lifecycle_mapping->sblr_operation);
    result.sblr_operation_family = std::string(lifecycle_mapping->sblr_operation_family);
    result.engine_api_function = std::string(lifecycle_mapping->engine_api_function);
    result.lifecycle_mapping_key = std::string(lifecycle_mapping->mapping_key);
    result.emulation_diagnostic_code = std::string(lifecycle_mapping->diagnostic_code);
    result.scratchbird_lifecycle_api =
        lifecycle_mapping->disposition == FirebirdMappingDisposition::kScratchBirdLifecycleApi;
    result.parser_support_udr_route =
        lifecycle_mapping->disposition == FirebirdMappingDisposition::kParserSupportUdr;
    result.exact_emulated_diagnostic =
        lifecycle_mapping->disposition == FirebirdMappingDisposition::kEmulatedNonFileDiagnostic;
    result.real_firebird_file_effects = lifecycle_mapping->produces_file_effects;
    result.donor_engine_sql_executed = lifecycle_mapping->donor_engine_sql_executed;
  }
  std::vector<Diagnostic> diagnostics;

  if (result.normalized_sql.empty()) {
    diagnostics.push_back(MakeDiagnostic(
        "FIREBIRD.PARSE.EMPTY_INPUT", "ERROR",
        "Firebird parser input is empty.", "sbp_firebird"));
    result.message_vector_json = MessageVectorToJson(diagnostics);
    return result;
  }

  if (MutatesCatalogOverlay(active_upper)) {
    result.ok = false;
    result.statement_family = "catalog_overlay";
    result.operation_family = "firebird.catalog_overlay.read_only_violation";
    diagnostics.push_back(MakeDiagnostic(
        "FIREBIRD.CATALOG_OVERLAY.READ_ONLY", "ERROR",
        "Firebird catalog overlay rows are projected from ScratchBird authority and cannot be mutated directly.",
        "sbp_firebird",
        {{"operation_family", result.operation_family}}));
    result.message_vector_json = MessageVectorToJson(diagnostics);
    return result;
  }
  if (!HasBalancedParentheses(result.normalized_sql)) {
    result.ok = false;
    result.statement_family = "invalid_input";
    result.operation_family = "firebird.invalid_input";
    diagnostics.push_back(MakeDiagnostic(
        "FIREBIRD.PARSE.INVALID_INPUT", "ERROR",
        "Firebird parser input has unterminated expression delimiters.",
        "sbp_firebird"));
    result.message_vector_json = MessageVectorToJson(diagnostics);
    return result;
  }

  if (lifecycle_mapping != nullptr &&
      lifecycle_mapping->diagnostic_code == "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED") {
    result.ok = false;
    result.statement_family = "low_level_utility";
    if (const auto operation = ClassifyNonFileOperation(active_upper); !operation.empty()) {
      result.operation_family = operation;
    } else {
      result.operation_family = "firebird.low_level_utility.unsupported";
    }
    diagnostics.push_back(MakeDiagnostic(
        std::string(lifecycle_mapping->diagnostic_code),
        "ERROR",
        std::string(lifecycle_mapping->diagnostic_message),
        "sbp_firebird",
        {{"operation_family", result.operation_family},
         {"mapping_key", result.lifecycle_mapping_key},
         {"real_firebird_file_effects", "false"},
         {"donor_engine_sql_executed", "false"}}));
    result.message_vector_json = MessageVectorToJson(diagnostics);
    return result;
  }

  result.ok = true;
  if (!gbak_logical_stream_operation.empty()) {
    result.statement_family = "logical_stream_backup_restore";
    result.operation_family = gbak_logical_stream_operation;
    diagnostics.push_back(MakeDiagnostic(
        "FIREBIRD.LOGICAL_STREAM.GBAK_REMOTE_STREAM", "INFO",
        "Firebird gbak stdin/stdout logical stream is admitted as parser/SBLR evidence with ScratchBird engine authority and no donor file effects.",
        "sbp_firebird",
        {{"operation_family", result.operation_family},
         {"scratchbird_lifecycle_api", "false"},
         {"real_firebird_file_effects", "false"},
         {"donor_engine_sql_executed", "false"}}));
  } else if (const auto operation = ClassifyNonFileOperation(active_upper); !operation.empty()) {
    result.statement_family = "non_file_emulation";
    result.operation_family = operation;
    diagnostics.push_back(MakeDiagnostic(
        lifecycle_mapping == nullptr ? "FIREBIRD.EMULATION.NON_FILE_SURFACE"
                                     : std::string(lifecycle_mapping->diagnostic_code),
        "INFO",
        lifecycle_mapping == nullptr
            ? "Firebird file/storage/admin surface is admitted as ScratchBird emulation with zero real Firebird file effects."
            : std::string(lifecycle_mapping->diagnostic_message),
        "sbp_firebird",
        {{"operation_family", result.operation_family},
         {"mapping_key", result.lifecycle_mapping_key},
         {"lifecycle_operation_id", result.lifecycle_operation_id},
         {"sblr_operation", result.sblr_operation},
         {"engine_api_function", result.engine_api_function},
         {"real_firebird_file_effects", "false"},
         {"donor_engine_sql_executed", "false"}}));
  } else if (const auto operation = ClassifyPsqlOperation(active_upper); !operation.empty()) {
    result.statement_family = "psql";
    result.operation_family = operation;
  } else if (const auto operation = ClassifyDatatypeOperation(active_upper); !operation.empty()) {
    result.statement_family = StartsWithCommand(active_upper, "SELECT") ||
                                      StartsWithCommand(active_upper, "WITH")
                                  ? "query"
                                  : "datatype";
    result.operation_family = operation;
  } else if (const auto operation = ClassifyExpressionOperation(active_upper); !operation.empty()) {
    result.statement_family = "query";
    result.operation_family = operation;
  } else if (const auto operation = ClassifyCatalogOverlayOperation(active_upper); !operation.empty()) {
    result.statement_family = "catalog_overlay";
    result.operation_family = operation;
    diagnostics.push_back(MakeDiagnostic(
        "FIREBIRD.CATALOG_OVERLAY.PROJECTION", "INFO",
        "Firebird catalog object is projected from ScratchBird UUID-backed metadata.",
        "sbp_firebird",
        {{"operation_family", result.operation_family}}));
  } else if (const auto operation = ClassifyStatisticsOptimizerOperation(active_upper);
             !operation.empty()) {
    result.statement_family = "optimizer";
    result.operation_family = operation;
  } else if (const auto operation = ClassifyQueryOperation(active_upper); !operation.empty()) {
    result.statement_family = "query";
    result.operation_family = operation;
  } else if (const auto operation = ClassifyDmlOperation(active_upper); !operation.empty()) {
    result.statement_family = "dml";
    result.operation_family = operation;
  } else if (const auto operation = ClassifyDdlOperation(active_upper); !operation.empty()) {
    result.statement_family = "ddl";
    result.operation_family = operation;
  } else if (StartsWithCommand(active_upper, "COMMIT") ||
             StartsWithCommand(active_upper, "ROLLBACK") ||
             StartsWithCommand(active_upper, "SET TRANSACTION") ||
             StartsWithCommand(active_upper, "SAVEPOINT") ||
             StartsWithCommand(active_upper, "RELEASE SAVEPOINT")) {
    result.statement_family = "transaction";
    if (StartsWithCommand(active_upper, "SET TRANSACTION")) {
      result.operation_family = "firebird.transaction.set_transaction";
    } else if (StartsWithCommand(active_upper, "ROLLBACK TO SAVEPOINT") ||
               StartsWithCommand(active_upper, "ROLLBACK TO") ||
               StartsWithCommand(active_upper, "ROLLBACK WORK TO SAVEPOINT") ||
               StartsWithCommand(active_upper, "ROLLBACK WORK TO") ||
               StartsWithCommand(active_upper, "ROLLBACK TRANSACTION TO SAVEPOINT") ||
               StartsWithCommand(active_upper, "ROLLBACK TRANSACTION TO")) {
      result.operation_family = "firebird.transaction.rollback_to_savepoint";
    } else if (StartsWithCommand(active_upper, "SAVEPOINT") ||
               StartsWithCommand(active_upper, "RELEASE SAVEPOINT")) {
      result.operation_family = "firebird.transaction.savepoint";
    } else {
      result.operation_family = "firebird.transaction.control";
    }
  } else if (const auto operation = ClassifyIsqlOperation(active_upper); !operation.empty()) {
    result.statement_family = "isql_frontend";
    result.operation_family = operation;
  } else {
    result.ok = false;
    result.statement_family = "invalid_input";
    result.operation_family = "firebird.invalid_input";
    diagnostics.push_back(MakeDiagnostic(
        "FIREBIRD.PARSE.INVALID_INPUT", "ERROR",
        "Input is not recognized by the current Firebird parser registry seed.",
        "sbp_firebird"));
  }

  if (result.ok) {
    parser_evidence.firebird_connection_sandbox_evidence_required = true;
    parser_evidence.firebird_connection_sandbox_evidence_json =
        FirebirdConnectionSandboxEvidenceJson(result.statement_family,
                                             result.operation_family);
    parser_evidence.datatype_descriptor_evidence_required =
        (result.statement_family == "ddl" ||
         result.statement_family == "datatype" ||
         StartsWith(result.operation_family, "firebird.datatype.")) &&
        parser_evidence.datatype_reference_count > 0;
    if (parser_evidence.datatype_descriptor_evidence_required) {
      parser_evidence.firebird_exact_datatype_domain_evidence_required = true;
      parser_evidence.firebird_exact_datatype_domain_evidence_json =
          FirebirdExactDatatypeDomainEvidenceJson(result.operation_family,
                                                 active_upper);
    }
    if (result.statement_family == "logical_stream_backup_restore" &&
        !gbak_logical_stream_operation.empty()) {
      parser_evidence.firebird_gbak_logical_stream_evidence_required = true;
      parser_evidence.firebird_gbak_logical_stream_evidence_json =
          FirebirdGbakLogicalStreamEvidenceJson(result.operation_family,
                                               active_upper);
    }
    parser_evidence.index_semantic_defaults_evidence_required =
        result.statement_family == "ddl" &&
        scratchbird::parser::donor::IsIndexSemanticDefaultsStatement(active_upper);
    if (parser_evidence.index_semantic_defaults_evidence_required) {
      parser_evidence.index_semantic_defaults_upper_sql = active_upper;
    }
    parser_evidence.constraint_semantic_defaults_evidence_required =
        result.statement_family == "ddl" &&
        scratchbird::parser::donor::IsConstraintSemanticDefaultsStatement(
            active_upper);
    if (parser_evidence.constraint_semantic_defaults_evidence_required) {
      parser_evidence.constraint_semantic_defaults_upper_sql = active_upper;
    }
    parser_evidence.sequence_identity_semantic_evidence_required =
        scratchbird::parser::donor::IsSequenceIdentitySemanticStatement(
            "firebird", active_upper);
    if (parser_evidence.sequence_identity_semantic_evidence_required) {
      parser_evidence.sequence_identity_semantic_upper_sql = active_upper;
    }
    parser_evidence.identifier_name_resolution_evidence_required =
        result.statement_family == "ddl" &&
        scratchbird::parser::donor::HasIdentifierNameResolutionProfile(
            "firebird");
    if (parser_evidence.identifier_name_resolution_evidence_required) {
      parser_evidence.identifier_name_resolution_upper_sql = active_upper;
    }
    parser_evidence.scalar_expression_semantic_evidence_required =
        result.statement_family == "query" &&
        scratchbird::parser::donor::HasScalarExpressionSemanticProfile(
            "firebird") &&
        scratchbird::parser::donor::IsScalarExpressionSemanticStatement(
            "firebird", active_upper);
    if (parser_evidence.scalar_expression_semantic_evidence_required) {
      parser_evidence.scalar_expression_semantic_upper_sql = active_upper;
    }
    parser_evidence.dml_mutation_semantic_evidence_required =
        result.statement_family == "dml" &&
        scratchbird::parser::donor::IsDmlMutationSemanticStatement(
            "firebird", active_upper);
    if (parser_evidence.dml_mutation_semantic_evidence_required) {
      parser_evidence.dml_mutation_semantic_upper_sql = active_upper;
    }
    parser_evidence.transaction_session_semantic_evidence_required =
        (result.statement_family == "transaction" ||
         result.statement_family == "session") &&
        scratchbird::parser::donor::IsTransactionSessionSemanticStatement(
            "firebird", active_upper);
    if (parser_evidence.transaction_session_semantic_evidence_required) {
      parser_evidence.transaction_session_semantic_upper_sql = active_upper;
    }
    parser_evidence.temporary_session_object_semantic_evidence_required =
        result.statement_family == "ddl" &&
        scratchbird::parser::donor::
            IsTemporarySessionObjectSemanticStatement("firebird",
                                                     active_upper);
    if (parser_evidence.temporary_session_object_semantic_evidence_required) {
      parser_evidence.temporary_session_object_semantic_upper_sql =
          active_upper;
    }
    parser_evidence.dependency_bearing_ddl_semantic_evidence_required =
        result.statement_family == "ddl" &&
        scratchbird::parser::donor::
            IsDependencyBearingDdlSemanticStatement("firebird",
                                                   active_upper);
    if (parser_evidence.dependency_bearing_ddl_semantic_evidence_required) {
      parser_evidence.dependency_bearing_ddl_semantic_upper_sql =
          active_upper;
    }
    parser_evidence.ddl_transaction_behavior_semantic_evidence_required =
        result.statement_family == "ddl" &&
        scratchbird::parser::donor::IsDdlTransactionBehaviorSemanticStatement(
            "firebird", active_upper);
    if (parser_evidence.ddl_transaction_behavior_semantic_evidence_required) {
      parser_evidence.ddl_transaction_behavior_semantic_upper_sql =
          active_upper;
    }
    parser_evidence.resource_text_semantic_evidence_required =
        (result.statement_family == "ddl" || result.statement_family == "dml" ||
         result.statement_family == "query") &&
        scratchbird::parser::donor::HasResourceTextSemanticProfile(
            "firebird") &&
        scratchbird::parser::donor::IsResourceTextSemanticStatement(
            "firebird", active_upper);
    if (parser_evidence.resource_text_semantic_evidence_required) {
      parser_evidence.resource_text_semantic_upper_sql = active_upper;
    }
    parser_evidence.statistics_optimizer_semantic_evidence_required =
        scratchbird::parser::donor::HasStatisticsOptimizerSemanticProfile(
            "firebird") &&
        scratchbird::parser::donor::IsStatisticsOptimizerSemanticStatement(
            "firebird", active_upper);
    if (parser_evidence.statistics_optimizer_semantic_evidence_required) {
      parser_evidence.statistics_optimizer_semantic_upper_sql = active_upper;
    }
    parser_evidence.locks_isolation_semantic_evidence_required =
        scratchbird::parser::donor::HasLocksIsolationSemanticProfile(
            "firebird") &&
        scratchbird::parser::donor::IsLocksIsolationSemanticStatement(
            "firebird", active_upper);
    if (parser_evidence.locks_isolation_semantic_evidence_required) {
      parser_evidence.locks_isolation_semantic_upper_sql = active_upper;
    }
    parser_evidence.system_catalog_defaults_semantic_evidence_required =
        scratchbird::parser::donor::
            HasSystemCatalogDefaultsSemanticProfile("firebird") &&
        scratchbird::parser::donor::
            IsSystemCatalogDefaultsSemanticStatement("firebird", active_upper);
    if (parser_evidence.system_catalog_defaults_semantic_evidence_required) {
      parser_evidence.system_catalog_defaults_semantic_operation_id =
          !result.lifecycle_mapping_key.empty() ? result.lifecycle_mapping_key
                                                : result.operation_family;
    }
    parser_evidence.session_settings_diagnostics_semantic_evidence_required =
        scratchbird::parser::donor::
            HasSessionSettingsDiagnosticsSemanticProfile("firebird") &&
        scratchbird::parser::donor::
            IsSessionSettingsDiagnosticsSemanticStatement("firebird",
                                                         active_upper);
    if (parser_evidence.session_settings_diagnostics_semantic_evidence_required) {
      parser_evidence.session_settings_diagnostics_semantic_upper_sql =
          active_upper;
    }
    parser_evidence.procedural_body_source_retention_required =
        scratchbird::parser::donor::IsProceduralBodySourceRetentionStatement(
            result.statement_family, result.operation_family, active_upper);
    if (parser_evidence.procedural_body_source_retention_required) {
      parser_evidence.procedural_span_metadata =
          BuildProceduralFunctionalEncodingSpanMetadata(active_upper, tokens);
      std::vector<scratchbird::parser::donor::Token> donor_tokens;
      donor_tokens.reserve(tokens.size());
      for (const auto& token : tokens) {
        donor_tokens.push_back({token.kind, token.lexeme, token.offset});
      }
      parser_evidence.procedural_source_retention_metadata =
          scratchbird::parser::donor::ProceduralSourceRetentionMetadataFor(
              "firebird", result.normalized_sql, active_upper, donor_tokens);
      parser_evidence.firebird_psql_functional_encoding_evidence_required =
          true;
      parser_evidence.firebird_psql_functional_encoding_evidence_json =
          FirebirdPsqlFunctionalEncodingEvidenceJson(
              result.operation_family, active_upper,
              parser_evidence.procedural_span_metadata);
    }
    result.parser_evidence_json = ParserEvidenceJson(parser_evidence);
    result.sblr_envelope =
        MakeSblrEnvelope(result.statement_family, result.operation_family,
                         lifecycle_mapping, parser_evidence);
  }
  result.message_vector_json = MessageVectorToJson(diagnostics);
  return result;
}

std::string FirebirdPackageIdentityJson() {
  return "{\"dialect\":\"firebird\","
         "\"parser_worker\":\"sbp_firebird\","
         "\"parser_support_udr\":\"sbup_firebird\","
         "\"parser_support_udr_target\":\"sbu_firebird_parser_support\","
         "\"release_profile\":\"firebird-v5_0\","
         "\"parser_surface_rows\":19,"
         "\"function_api_rows\":9,"
         "\"donor_compatible_alias_rows\":0,"
         "\"core_or_optional_alias_rows\":0,"
         "\"catalog_projection_only_rows\":3,"
         "\"connector_operation_rows\":0,"
         "\"policy_blocked_rows\":1,"
         "\"trusted_udr_registration_rows\":5,"
         "\"unsupported_rows\":3,"
         "\"datatype_families\":" + std::to_string(DatatypeSurfaces().size()) + ","
         "\"builtin_function_families\":" + std::to_string(BuiltinFunctionSurfaces().size()) + ","
         "\"catalog_overlay_families\":" + std::to_string(CatalogOverlaySurfaces().size()) + ","
         "\"diagnostic_families\":" + std::to_string(DiagnosticSurfaces().size()) + ","
         "\"lifecycle_mapping_report\":" + FirebirdLifecycleMappingReportJson() + ","
         "\"standalone_dialect_package\":true,"
         "\"cross_dialect_dependencies\":false,"
         "\"dependency_isolation\":\"firebird_parser_and_udr_only\"}";
}

std::string FirebirdLifecycleMappingReportJson() {
  std::size_t lifecycle_api_count = 0;
  std::size_t parser_support_udr_count = 0;
  std::size_t exact_diagnostic_count = 0;
  for (const auto& mapping : FirebirdLifecycleMappings()) {
    if (mapping.disposition == FirebirdMappingDisposition::kScratchBirdLifecycleApi) {
      ++lifecycle_api_count;
    } else if (mapping.disposition == FirebirdMappingDisposition::kParserSupportUdr) {
      ++parser_support_udr_count;
    } else if (mapping.disposition == FirebirdMappingDisposition::kEmulatedNonFileDiagnostic &&
               mapping.exact_emulated_diagnostic) {
      ++exact_diagnostic_count;
    }
  }
  return "{\"gate\":\"DBLC_P14_DONOR_MAPPING_COMPLETE\","
         "\"static_gate\":\"DBLC_STATIC_NO_DONOR_ENGINE_SQL\","
         "\"dialect\":\"firebird\","
         "\"lifecycle_api_mappings\":" + std::to_string(lifecycle_api_count) + ","
         "\"parser_support_udr_mappings\":" + std::to_string(parser_support_udr_count) + ","
         "\"exact_emulated_non_file_diagnostics\":" + std::to_string(exact_diagnostic_count) + ","
         "\"engine_authority\":\"scratchbird\","
         "\"donor_engine_sql_executed\":false,"
         "\"real_firebird_file_effects\":false,"
         "\"standalone_dialect_package\":true}";
}

} // namespace scratchbird::parser::firebird
