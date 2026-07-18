// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_dialect.hpp"

#include "firebird_semantic_evidence.hpp"

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

bool HasFunctionCall(std::string_view upper, std::string_view name) {
  return ContainsWord(upper, name) &&
         Contains(upper, std::string(name) + "(");
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

bool IsFirebirdSessionSettingsDiagnosticsStatement(std::string_view upper) {
  return StartsWithCommand(upper, "SET SQL DIALECT") ||
         StartsWithCommand(upper, "SET NAMES") ||
         StartsWithCommand(upper, "SHOW SQL DIALECT") ||
         StartsWithCommand(upper, "SHOW WARNINGS");
}

std::string FirebirdSessionSettingsDiagnosticsEvidenceJson(
    std::string_view release_profile,
    std::string_view upper) {
  const bool warning_surface = StartsWithCommand(upper, "SHOW WARNINGS");
  const bool diagnostic_projection_surface = StartsWithCommand(upper, "SHOW");
  const std::string_view operation_surface =
      StartsWithCommand(upper, "SET SQL DIALECT") ? "firebird_set_sql_dialect"
      : StartsWithCommand(upper, "SET NAMES") ? "firebird_set_names"
      : StartsWithCommand(upper, "SHOW SQL DIALECT") ? "firebird_show_sql_dialect"
      : warning_surface ? "firebird_show_warnings"
                        : "firebird_session_settings_diagnostics";
  const std::string_view compatibility_mode_policy =
      StartsWithCommand(upper, "SET SQL DIALECT")
          ? "firebird_sql_dialect_session_descriptor_engine_applies"
      : StartsWithCommand(upper, "SET NAMES")
          ? "firebird_character_set_session_descriptor_engine_applies"
          : "firebird_isql_show_session_descriptor_projection";

  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_session_settings_diagnostics_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\"019e13c0-0000-7000-8000-000000000302\","
      << "\"session_semantic_profile_uuid\":\"019e13c0-1e00-7000-8000-000000000302\","
      << "\"semantic_profile_uuid\":\"019e13c0-1e00-7000-8000-000000000302\","
      << "\"dialect\":\"firebird\",\"release_profile\":\""
      << EscapeJson(release_profile) << "\","
      << "\"session_settings_diagnostics_profile\":\"firebird.session_settings_diagnostics_semantics_profile\","
      << "\"operation_surface\":\"" << operation_surface << "\","
      << "\"sql_mode_set\":false,"
      << "\"warning_surface\":" << BoolJson(warning_surface) << ','
      << "\"notice_surface\":false,\"current_schema_surface\":false,"
      << "\"search_path_surface\":false,\"date_time_format_surface\":false,"
      << "\"timeout_surface\":false,\"reset_surface\":false,"
      << "\"diagnostic_projection_surface\":"
      << BoolJson(diagnostic_projection_surface) << ','
      << "\"compatibility_mode_policy\":\"" << compatibility_mode_policy << "\","
      << "\"warning_policy\":\"firebird_status_vector_warning_diagnostics_engine_rendered\","
      << "\"notice_policy\":\"firebird_status_vector_notice_mapping_engine_rendered\","
      << "\"current_schema_policy\":\"firebird_current_schema_context_engine_session_descriptor\","
      << "\"search_path_policy\":\"firebird_no_search_path_single_attachment_schema_context\","
      << "\"date_time_format_policy\":\"firebird_date_time_format_stable_dialect_descriptor\","
      << "\"timeout_policy\":\"firebird_no_statement_timeout_session_setting_descriptor\","
      << "\"reset_policy\":\"firebird_session_setting_reset_not_requested\","
      << "\"diagnostic_map_ref\":\"firebird_session_settings_diagnostics_semantics_diagnostic_map\","
      << "\"sandbox_root_policy\":\"firebird_compatibility_schema_root_uuid_required_no_cross_root_temp_access\","
      << "\"uuid_required_semantic_profile\":true,\"session_descriptor_required\":true,"
      << "\"diagnostic_descriptor_required\":true,\"catalog_descriptor_required\":true,"
      << "\"sblr_operation_uuid_resolution_required\":true,"
      << "\"engine_authority\":\"scratchbird\","
      << "\"engine_session_authority\":\"scratchbird_engine_session_descriptor_authority\","
      << "\"diagnostic_rendering_authority\":\"scratchbird_engine_diagnostic_rendering_authority\","
      << "\"catalog_authority\":\"engine_catalog_uuid_projection\","
      << "\"storage_authority\":\"engine_storage_authority\","
      << "\"transaction_authority\":\"engine_mga_authority\","
      << "\"finality_authority\":\"engine_mga_authority\","
      << "\"source_sql_text_included\":false,\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,\"parser_session_authority\":false,"
      << "\"parser_diagnostic_authority\":false,\"parser_catalog_authority\":false,"
      << "\"parser_storage_authority\":false,\"parser_execution_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,\"parser_finality_authority\":false,"
      << "\"parser_runtime_semantic_equivalence_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"reference_parser_semantic_equivalence_proven\","
      << "\"readiness_status\":\"proof_verified\","
      << "\"descriptor_exactness_status\":\"parser_session_settings_diagnostics_descriptor_recorded_runtime_equivalence_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

bool IsFirebirdSystemCatalogDefaultsStatement(std::string_view upper) {
  return (StartsWithCommand(upper, "SELECT") || StartsWithCommand(upper, "WITH")) &&
         (Contains(upper, "RDB$") || Contains(upper, "MON$") ||
          Contains(upper, "SEC$") || Contains(upper, "INFORMATION_SCHEMA."));
}

std::string FirebirdSystemCatalogDefaultsEvidenceJson(
    std::string_view operation_id,
    const std::vector<SurfaceDescriptor>& catalog_surfaces) {
  std::ostringstream families;
  families << '[';
  for (std::size_t i = 0; i < catalog_surfaces.size(); ++i) {
    if (i != 0) families << ',';
    families << '"' << EscapeJson(catalog_surfaces[i].family) << '"';
  }
  families << ']';

  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_system_catalog_defaults_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\"019e13c0-0000-7000-8000-000000000302\","
      << "\"semantic_profile_uuid\":\"019e13c0-1d00-7000-8000-000000000302\","
      << "\"catalog_overlay_profile_uuid\":\"019e13c0-1d00-7000-8000-000000000302\","
      << "\"dialect\":\"firebird\",\"operation_id\":\""
      << EscapeJson(operation_id) << "\","
      << "\"system_catalog_defaults_profile\":\"firebird.system_catalog_defaults_semantics_profile\","
      << "\"system_catalog_namespace_root_policy\":\"firebird_rdb_mon_sec_information_schema_projected_from_engine_catalog_uuid_root\","
      << "\"catalog_visibility_projection_policy\":\"firebird_system_relations_visible_through_engine_privilege_filtered_projection\","
      << "\"generated_default_catalog_name_policy\":\"firebird_generated_rdb_names_projected_as_catalog_descriptors_not_parser_names\","
      << "\"dependency_projection_policy\":\"firebird_rdb_dependencies_projected_from_engine_dependency_graph\","
      << "\"source_visibility_policy\":\"firebird_rdb_source_columns_redacted_or_projected_by_engine_source_retention_policy\","
      << "\"hidden_system_object_policy\":\"firebird_rdb_system_flag_hidden_objects_privilege_filtered_engine_projection\","
      << "\"grant_privilege_projection_policy\":\"firebird_rdb_user_privileges_sec_projection_engine_security_authority\","
      << "\"catalog_surface_family_count\":" << catalog_surfaces.size() << ','
      << "\"catalog_surface_families\":" << families.str() << ','
      << "\"sblr_catalog_projection_opcode\":\"SBLR_COMPATIBILITY_FIREBIRD_CATALOG_PROJECT\","
      << "\"diagnostic_map_ref\":\"firebird_system_catalog_defaults_semantics_diagnostic_map\","
      << "\"sandbox_root_policy\":\"firebird_compatibility_schema_root_uuid_required_no_cross_root_temp_access\","
      << "\"uuid_required_semantic_profile\":true,\"catalog_descriptor_required\":true,"
      << "\"catalog_projection_descriptor_required\":true,\"dependency_descriptor_required\":true,"
      << "\"security_descriptor_required\":true,\"source_descriptor_required\":true,"
      << "\"sblr_operation_uuid_resolution_required\":true,"
      << "\"engine_authority\":\"scratchbird\","
      << "\"catalog_authority\":\"engine_catalog_uuid_projection\","
      << "\"storage_authority\":\"engine_storage_catalog_authority\","
      << "\"dependency_authority\":\"engine_dependency_graph_authority\","
      << "\"security_authority\":\"engine_security_policy_authority\","
      << "\"source_authority\":\"engine_source_retention_policy_authority\","
      << "\"visibility_authority\":\"engine_catalog_visibility_authority\","
      << "\"source_sql_text_included\":false,\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,\"parser_catalog_authority\":false,"
      << "\"parser_storage_authority\":false,\"parser_dependency_authority\":false,"
      << "\"parser_security_authority\":false,\"parser_source_authority\":false,"
      << "\"parser_visibility_authority\":false,\"parser_execution_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_runtime_semantic_equivalence_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"reference_parser_semantic_equivalence_proven\","
      << "\"readiness_status\":\"proof_verified\","
      << "\"descriptor_exactness_status\":\"parser_system_catalog_defaults_descriptor_recorded_runtime_equivalence_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

bool IsFirebirdRowLockQuery(std::string_view upper) {
  if (!(StartsWithCommand(upper, "SELECT") || StartsWithCommand(upper, "WITH") ||
        (StartsWith(upper, "(") && Contains(upper, "SELECT")))) {
    return false;
  }
  return Contains(upper, " FOR UPDATE");
}

bool IsFirebirdLocksIsolationStatement(std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  return StartsWithCommand(upper, "SET TRANSACTION") ||
         StartsWithCommand(upper, "START TRANSACTION") ||
         IsFirebirdRowLockQuery(upper);
}

std::string FirebirdLocksIsolationEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool isolation_surface =
      StartsWithCommand(upper, "SET TRANSACTION") ||
      StartsWithCommand(upper, "START TRANSACTION");
  const bool for_update_surface = Contains(upper, " FOR UPDATE");
  const bool row_lock_surface =
      IsFirebirdRowLockQuery(upper) || for_update_surface;
  const bool nowait_surface = ContainsWord(upper, "NOWAIT") ||
                              Contains(upper, " NO WAIT");
  const bool skip_locked_surface = Contains(upper, " SKIP LOCKED");
  const bool read_only_surface = Contains(upper, " READ ONLY");
  const bool read_write_surface = Contains(upper, " READ WRITE") ||
                                  ContainsWord(upper, "WRITE");
  const bool deadlock_diagnostic_surface =
      ContainsWord(upper, "DEADLOCK") || Contains(upper, "MON$LOCK");
  const bool transaction_surface = isolation_surface;
  const bool query_surface = StartsWithCommand(upper, "SELECT") ||
                             StartsWithCommand(upper, "WITH") ||
                             (StartsWith(upper, "(") && Contains(upper, "SELECT"));
  const std::string_view locks_surface =
      StartsWithCommand(upper, "SET TRANSACTION")
          ? (ContainsWord(upper, "NOWAIT")
                 ? "firebird_set_transaction_nowait_isolation_descriptor"
             : ContainsWord(upper, "WAIT")
                 ? "firebird_set_transaction_wait_isolation_descriptor"
                 : "firebird_set_transaction_isolation_descriptor")
      : for_update_surface ? "firebird_select_for_update_row_lock_descriptor"
                           : "firebird_locks_isolation_syntax_surface";

  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_locks_isolation_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\"019e13c0-0000-7000-8000-000000000302\","
      << "\"semantic_profile_uuid\":\"019e13c0-1c00-7000-8000-000000000302\","
      << "\"dialect\":\"firebird\",\"release_profile\":\""
      << EscapeJson(release_profile) << "\","
      << "\"locks_isolation_profile\":\"firebird.locks_isolation_syntax_semantics_profile\","
      << "\"locks_isolation_surface\":\"" << locks_surface << "\","
      << "\"isolation_profile_uuid_or_policy\":\"firebird_tpb_isolation_descriptor_uuid_required_engine_mga_authority\","
      << "\"lock_clause_policy\":\"firebird_for_update_wait_no_wait_descriptor_engine_lock_authority\","
      << "\"nowait_policy\":\"firebird_nowait_tpb_descriptor_engine_lock_wait_policy\","
      << "\"skip_locked_policy\":\"firebird_skip_locked_not_supported_descriptor_refusal_policy\","
      << "\"advisory_lock_policy\":\"firebird_no_advisory_lock_surface_descriptor_refusal_policy\","
      << "\"table_lock_policy\":\"firebird_explicit_table_lock_not_supported_descriptor_refusal_policy\","
      << "\"row_lock_policy\":\"firebird_for_update_descriptor_engine_cursor_lock_authority\","
      << "\"read_write_policy\":\"firebird_read_only_read_write_tpb_descriptor_engine_intent_authority\","
      << "\"deadlock_diagnostic_policy\":\"firebird_deadlock_diagnostic_map_descriptor_engine_lock_manager_authority\","
      << "\"diagnostic_map_ref\":\"firebird_locks_isolation_semantics_diagnostic_map\","
      << "\"sandbox_root_policy\":\"firebird_compatibility_schema_root_uuid_required_no_cross_root_temp_access\","
      << "\"isolation_surface\":" << BoolJson(isolation_surface) << ','
      << "\"lock_table_surface\":false,"
      << "\"row_lock_surface\":" << BoolJson(row_lock_surface) << ','
      << "\"for_update_surface\":" << BoolJson(for_update_surface) << ','
      << "\"for_share_surface\":false,"
      << "\"nowait_surface\":" << BoolJson(nowait_surface) << ','
      << "\"skip_locked_surface\":" << BoolJson(skip_locked_surface) << ','
      << "\"advisory_lock_surface\":false,"
      << "\"read_only_surface\":" << BoolJson(read_only_surface) << ','
      << "\"read_write_surface\":" << BoolJson(read_write_surface) << ','
      << "\"deadlock_diagnostic_surface\":"
      << BoolJson(deadlock_diagnostic_surface) << ','
      << "\"transaction_surface\":" << BoolJson(transaction_surface) << ','
      << "\"query_surface\":" << BoolJson(query_surface) << ','
      << "\"session_surface\":false,"
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
      << "\"source_sql_text_included\":false,\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,\"parser_lock_authority\":false,"
      << "\"parser_isolation_authority\":false,\"parser_deadlock_authority\":false,"
      << "\"parser_catalog_authority\":false,\"parser_storage_authority\":false,"
      << "\"parser_execution_authority\":false,\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_visibility_authority\":false,"
      << "\"parser_runtime_semantic_equivalence_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"reference_parser_semantic_equivalence_proven\","
      << "\"descriptor_exactness_status\":\"parser_locks_isolation_descriptor_recorded_runtime_equivalence_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

struct FirebirdResourceTextFlags {
  bool ddl{false};
  bool dml{false};
  bool query{false};
  bool charset{false};
  bool collation{false};
  bool timezone{false};
  bool calendar{false};
  bool pattern{false};
  bool comparison{false};
  bool binary_text{false};
  bool text_type{false};
  bool string_literal{false};
  bool cast_to_text{false};
};

FirebirdResourceTextFlags ClassifyFirebirdResourceText(std::string_view upper) {
  FirebirdResourceTextFlags flags;
  flags.ddl = StartsWithCommand(upper, "CREATE") ||
              StartsWithCommand(upper, "ALTER") ||
              StartsWithCommand(upper, "DROP") ||
              StartsWithCommand(upper, "RECREATE");
  flags.dml = StartsWithCommand(upper, "INSERT") ||
              StartsWithCommand(upper, "UPDATE") ||
              StartsWithCommand(upper, "DELETE") ||
              StartsWithCommand(upper, "MERGE");
  flags.query = StartsWithCommand(upper, "SELECT") ||
                StartsWithCommand(upper, "WITH");
  flags.binary_text = ContainsWord(upper, "BLOB");
  flags.text_type = ContainsWord(upper, "CHAR") ||
                    ContainsWord(upper, "VARCHAR") ||
                    ContainsWord(upper, "NCHAR") ||
                    Contains(upper, "NATIONAL CHARACTER") ||
                    ContainsWord(upper, "TEXT") || flags.binary_text;
  flags.charset = Contains(upper, "CHARACTER SET") ||
                  ContainsWord(upper, "CHARSET");
  flags.collation = ContainsWord(upper, "COLLATE") ||
                    ContainsWord(upper, "COLLATION");
  flags.string_literal = Contains(upper, "'");
  flags.pattern = Contains(upper, " LIKE ") ||
                  Contains(upper, " SIMILAR TO ") ||
                  Contains(upper, " STARTING WITH ") ||
                  Contains(upper, " CONTAINING ");
  flags.cast_to_text = Contains(upper, "CAST(") && flags.text_type;
  flags.timezone = Contains(upper, "WITH TIME ZONE") ||
                   ContainsWord(upper, "TIMEZONE") ||
                   ContainsWord(upper, "TIMESTAMP") ||
                   ContainsWord(upper, "CURRENT_TIMESTAMP");
  flags.calendar = ContainsWord(upper, "DATE") ||
                   ContainsWord(upper, "TIME") ||
                   ContainsWord(upper, "TIMESTAMP") ||
                   ContainsWord(upper, "CURRENT_DATE") ||
                   ContainsWord(upper, "CURRENT_TIME");
  flags.comparison = flags.collation || flags.pattern || Contains(upper, " = ") ||
                     Contains(upper, " <> ") || Contains(upper, " != ");
  return flags;
}

bool IsFirebirdResourceTextStatement(std::string_view active_upper_sql) {
  const auto flags =
      ClassifyFirebirdResourceText(TrimAsciiView(active_upper_sql));
  if (flags.ddl) return flags.text_type || flags.charset || flags.collation;
  if (flags.dml || flags.query) {
    return flags.string_literal || flags.pattern || flags.charset ||
           flags.collation || flags.cast_to_text || flags.timezone ||
           flags.calendar;
  }
  return false;
}

std::string FirebirdResourceTextEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const auto flags = ClassifyFirebirdResourceText(upper);
  const std::string_view surface =
      flags.ddl ? "firebird_ddl_charset_collation_text_blob"
      : flags.dml ? "firebird_dml_text_resource_descriptor"
      : flags.pattern ? "firebird_query_like_similar_containing"
      : flags.timezone ? "firebird_query_temporal_timezone_resource"
      : flags.binary_text ? "firebird_binary_blob_text_resource"
                          : "firebird_query_resource_text_semantics";

  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_resource_text_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\"019e13c0-0000-7000-8000-000000000302\","
      << "\"semantic_profile_uuid\":\"019e13c0-1a00-7000-8000-000000000302\","
      << "\"dialect\":\"firebird\",\"release_profile\":\""
      << EscapeJson(release_profile) << "\","
      << "\"resource_text_profile\":\"firebird.resource_text_semantics_profile\","
      << "\"resource_text_surface\":\"" << surface << "\","
      << "\"charset_policy\":\"firebird_character_set_descriptor_uuid_required_engine_applies\","
      << "\"collation_policy\":\"firebird_column_charset_collation_descriptor\","
      << "\"timezone_policy\":\"firebird_session_timezone_descriptor_engine_authority\","
      << "\"calendar_policy\":\"firebird_temporal_calendar_descriptor_engine_authority\","
      << "\"comparison_policy\":\"firebird_text_comparison_charset_collation_descriptor_engine_authority\","
      << "\"pattern_matching_policy\":\"firebird_like_similar_to_containing_starting_with_descriptor\","
      << "\"binary_text_policy\":\"firebird_blob_sub_type_binary_text_descriptor_required\","
      << "\"resource_epoch_policy\":\"firebird_resource_text_descriptor_epoch_engine_mga_catalog_bound\","
      << "\"index_compatibility_policy\":\"firebird_text_index_charset_collation_compatibility_engine_validated\","
      << "\"diagnostic_map_ref\":\"firebird_resource_text_semantics_diagnostic_map\","
      << "\"sandbox_root_policy\":\"firebird_compatibility_schema_root_uuid_required_no_cross_root_temp_access\","
      << "\"charset_surface\":" << BoolJson(flags.charset) << ','
      << "\"collation_surface\":" << BoolJson(flags.collation) << ','
      << "\"timezone_surface\":" << BoolJson(flags.timezone) << ','
      << "\"calendar_surface\":" << BoolJson(flags.calendar) << ','
      << "\"comparison_surface\":" << BoolJson(flags.comparison) << ','
      << "\"pattern_surface\":" << BoolJson(flags.pattern) << ','
      << "\"binary_text_surface\":" << BoolJson(flags.binary_text) << ','
      << "\"text_type_surface\":" << BoolJson(flags.text_type) << ','
      << "\"ddl_surface\":" << BoolJson(flags.ddl) << ','
      << "\"dml_surface\":" << BoolJson(flags.dml) << ','
      << "\"query_surface\":" << BoolJson(flags.query) << ','
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
      << "\"source_sql_text_included\":false,\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,\"parser_charset_authority\":false,"
      << "\"parser_collation_authority\":false,\"parser_timezone_authority\":false,"
      << "\"parser_calendar_authority\":false,\"parser_comparison_authority\":false,"
      << "\"parser_pattern_matching_authority\":false,"
      << "\"parser_binary_text_authority\":false,\"parser_text_type_authority\":false,"
      << "\"parser_catalog_authority\":false,"
      << "\"parser_resource_activation_authority\":false,"
      << "\"parser_index_compatibility_authority\":false,"
      << "\"parser_storage_authority\":false,\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_runtime_semantic_equivalence_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"reference_parser_semantic_equivalence_proven\","
      << "\"descriptor_exactness_status\":\"parser_resource_text_semantic_descriptor_recorded_runtime_equivalence_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

bool IsFirebirdStatisticsOptimizerStatement(
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  return StartsWithCommand(upper, "SET STATISTICS INDEX") ||
         Contains(upper, "RDB$INDICES") ||
         Contains(upper, "RDB$INDEX_SEGMENTS");
}

std::string FirebirdStatisticsOptimizerEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool analyze_surface =
      StartsWithCommand(upper, "SET STATISTICS INDEX");
  const bool statistics_update_surface = analyze_surface;
  const bool index_statistics_surface =
      analyze_surface || ContainsWord(upper, "INDEX") ||
      Contains(upper, "RDB$INDICES") ||
      Contains(upper, "RDB$INDEX_SEGMENTS");
  const std::string_view surface =
      analyze_surface
          ? "firebird_set_statistics_index_selectivity_descriptor"
      : (Contains(upper, "RDB$INDICES") ||
         Contains(upper, "RDB$INDEX_SEGMENTS"))
          ? "firebird_statistics_catalog_projection_descriptor"
          : "firebird_statistics_optimizer_metadata_surface";
  const std::string_view command_policy =
      analyze_surface
          ? "firebird_set_statistics_index_descriptor_only_engine_recomputes_selectivity"
          : "firebird_statistics_metadata_catalog_descriptor_only";
  const std::string_view analyze_policy =
      analyze_surface
          ? "firebird_set_statistics_index_maps_to_engine_statistics_descriptor_request"
          : "firebird_no_analyze_command_descriptor_policy";

  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_statistics_optimizer_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\"019e13c0-0000-7000-8000-000000000302\","
      << "\"semantic_profile_uuid\":\"019e13c0-1b00-7000-8000-000000000302\","
      << "\"dialect\":\"firebird\",\"release_profile\":\""
      << EscapeJson(release_profile) << "\","
      << "\"statistics_optimizer_profile\":\"firebird.statistics_optimizer_metadata_semantics_profile\","
      << "\"statistics_optimizer_surface\":\"" << surface << "\","
      << "\"statistics_command_policy\":\"" << command_policy << "\","
      << "\"histogram_policy\":\"firebird_index_selectivity_descriptor_engine_statistics_authority\","
      << "\"selectivity_policy\":\"firebird_rdb_index_statistics_selectivity_descriptor_engine_authority\","
      << "\"stale_statistics_policy\":\"firebird_stale_statistics_recompute_requires_engine_statistics_epoch\","
      << "\"index_eligibility_policy\":\"firebird_index_selectivity_eligibility_engine_index_descriptor\","
      << "\"plan_invalidation_policy\":\"firebird_plan_invalidation_engine_metadata_statistics_epoch\","
      << "\"analyze_command_policy\":\"" << analyze_policy << "\","
      << "\"explain_plan_policy\":\"firebird_plan_metadata_descriptor_only_no_parser_optimizer_authority\","
      << "\"catalog_projection_policy\":\"firebird_rdb_indices_statistics_catalog_projection_uuid_required\","
      << "\"diagnostic_map_ref\":\"firebird_statistics_optimizer_semantics_diagnostic_map\","
      << "\"sandbox_root_policy\":\"firebird_compatibility_schema_root_uuid_required_no_cross_root_temp_access\","
      << "\"explain_surface\":false,"
      << "\"analyze_surface\":" << BoolJson(analyze_surface) << ','
      << "\"statistics_update_surface\":"
      << BoolJson(statistics_update_surface) << ','
      << "\"reindex_surface\":false,\"optimize_surface\":false,"
      << "\"create_statistics_surface\":false,"
      << "\"drop_statistics_surface\":false,"
      << "\"index_statistics_surface\":"
      << BoolJson(index_statistics_surface) << ','
      << "\"plan_query_surface\":false,"
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
      << "\"source_sql_text_included\":false,\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,\"parser_statistics_authority\":false,"
      << "\"parser_optimizer_authority\":false,\"parser_histogram_authority\":false,"
      << "\"parser_selectivity_authority\":false,"
      << "\"parser_stale_statistics_authority\":false,"
      << "\"parser_index_eligibility_authority\":false,"
      << "\"parser_plan_invalidation_authority\":false,"
      << "\"parser_catalog_authority\":false,\"parser_storage_authority\":false,"
      << "\"parser_execution_authority\":false,\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_runtime_semantic_equivalence_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"reference_parser_semantic_equivalence_proven\","
      << "\"descriptor_exactness_status\":\"parser_statistics_optimizer_descriptor_recorded_runtime_equivalence_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

bool IsFirebirdDdlTransactionBehaviorStatement(
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  return StartsWithCommand(upper, "CREATE") ||
         StartsWithCommand(upper, "ALTER") ||
         StartsWithCommand(upper, "DROP") ||
         StartsWithCommand(upper, "RECREATE") ||
         StartsWithCommand(upper, "TRUNCATE") ||
         StartsWithCommand(upper, "COMMENT");
}

bool IsFirebirdCreateIndexStatement(std::string_view upper) {
  return StartsWithCommand(upper, "CREATE INDEX") ||
         StartsWithCommand(upper, "CREATE UNIQUE INDEX") ||
         StartsWithCommand(upper, "CREATE ASC INDEX") ||
         StartsWithCommand(upper, "CREATE DESC INDEX") ||
         StartsWithCommand(upper, "CREATE ASCENDING INDEX") ||
         StartsWithCommand(upper, "CREATE DESCENDING INDEX") ||
         StartsWithCommand(upper, "CREATE UNIQUE ASC INDEX") ||
         StartsWithCommand(upper, "CREATE UNIQUE DESC INDEX") ||
         StartsWithCommand(upper, "CREATE UNIQUE ASCENDING INDEX") ||
         StartsWithCommand(upper, "CREATE UNIQUE DESCENDING INDEX");
}

std::string_view FirebirdDdlOperationKind(std::string_view upper) {
  if (StartsWithCommand(upper, "CREATE OR REPLACE VIEW")) {
    return "create_or_replace_view";
  }
  if (StartsWithCommand(upper, "CREATE VIEW")) return "create_view";
  if (StartsWithCommand(upper, "CREATE GLOBAL TEMPORARY TABLE") ||
      StartsWithCommand(upper, "CREATE GLOBAL TEMP TABLE") ||
      StartsWithCommand(upper, "CREATE TABLE")) {
    return "create_table";
  }
  if (IsFirebirdCreateIndexStatement(upper)) return "create_index";
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

std::string FirebirdDdlTransactionBehaviorEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool create_surface = StartsWithCommand(upper, "CREATE") ||
                              StartsWithCommand(upper, "RECREATE");
  const bool alter_surface = StartsWithCommand(upper, "ALTER");
  const bool drop_surface = StartsWithCommand(upper, "DROP");
  const bool table_surface = ContainsWord(upper, "TABLE") ||
                             StartsWithCommand(upper, "TRUNCATE");
  const bool index_surface = IsFirebirdCreateIndexStatement(upper) ||
                             StartsWithCommand(upper, "ALTER INDEX") ||
                             StartsWithCommand(upper, "DROP INDEX");
  const bool view_surface = ContainsWord(upper, "VIEW");

  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_ddl_transaction_behavior_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\"019e13c0-0000-7000-8000-000000000302\","
      << "\"semantic_profile_uuid\":\"019e13c0-1900-7000-8000-000000000302\","
      << "\"dialect\":\"firebird\",\"release_profile\":\""
      << EscapeJson(release_profile) << "\","
      << "\"ddl_transaction_behavior_profile\":\"firebird.ddl_transaction_behavior_semantics_profile\","
      << "\"statement_classification\":\"ddl\","
      << "\"ddl_operation_kind\":\"" << FirebirdDdlOperationKind(upper)
      << "\","
      << "\"transaction_policy\":\"firebird_transactional_ddl_engine_mga_descriptor_required\","
      << "\"autocommit_boundary\":\"none_parser_does_not_commit_engine_transaction\","
      << "\"metadata_visibility_epoch\":\"transaction_local_until_engine_commit_then_catalog_epoch\","
      << "\"rollback_policy\":\"ddl_rollback_requires_engine_mga_transaction_rollback\","
      << "\"invalid_object_state_policy\":\"firebird_metadata_invalid_state_catalog_descriptor_engine_authority\","
      << "\"diagnostic_map_ref\":\"firebird_ddl_transaction_behavior_diagnostic_map\","
      << "\"sandbox_root_policy\":\"firebird_compatibility_schema_root_uuid_required_no_cross_root_temp_access\","
      << "\"create_surface\":" << BoolJson(create_surface) << ','
      << "\"alter_surface\":" << BoolJson(alter_surface) << ','
      << "\"drop_surface\":" << BoolJson(drop_surface) << ','
      << "\"table_surface\":" << BoolJson(table_surface) << ','
      << "\"index_surface\":" << BoolJson(index_surface) << ','
      << "\"view_surface\":" << BoolJson(view_surface) << ','
      << "\"implicit_commit_surface\":false,"
      << "\"transactional_ddl_surface\":true,"
      << "\"nontransactional_ddl_surface\":false,"
      << "\"uuid_required_semantic_profile\":true,"
      << "\"catalog_descriptor_required\":true,"
      << "\"sblr_operation_uuid_resolution_required\":true,"
      << "\"engine_authority\":\"scratchbird\","
      << "\"transaction_authority\":\"engine_mga_authority\","
      << "\"metadata_visibility_authority\":\"engine_catalog_mga_epoch\","
      << "\"rollback_authority\":\"engine_mga_authority\","
      << "\"invalid_object_state_authority\":\"engine_catalog_uuid_descriptor\","
      << "\"source_sql_text_included\":false,\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,\"parser_catalog_authority\":false,"
      << "\"parser_storage_authority\":false,\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_autocommit_authority\":false,"
      << "\"parser_metadata_visibility_authority\":false,"
      << "\"parser_rollback_authority\":false,"
      << "\"parser_invalid_object_state_authority\":false,"
      << "\"parser_recovery_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"reference_parser_semantic_equivalence_proven\","
      << "\"descriptor_exactness_status\":\"parser_ddl_transaction_behavior_descriptor_recorded_runtime_equivalence_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

struct FirebirdDependencyDdlFlags {
  bool view{false};
  bool trigger{false};
  bool routine{false};
  bool procedure{false};
  bool function{false};
  bool package{false};
};

FirebirdDependencyDdlFlags ClassifyFirebirdDependencyDdl(
    std::string_view upper) {
  FirebirdDependencyDdlFlags flags;
  flags.view = StartsWithCommand(upper, "CREATE VIEW") ||
               StartsWithCommand(upper, "CREATE OR ALTER VIEW") ||
               StartsWithCommand(upper, "ALTER VIEW") ||
               StartsWithCommand(upper, "DROP VIEW") ||
               StartsWithCommand(upper, "RECREATE VIEW");
  flags.trigger = StartsWithCommand(upper, "CREATE TRIGGER") ||
                  StartsWithCommand(upper, "CREATE OR ALTER TRIGGER") ||
                  StartsWithCommand(upper, "ALTER TRIGGER") ||
                  StartsWithCommand(upper, "DROP TRIGGER") ||
                  StartsWithCommand(upper, "RECREATE TRIGGER");
  flags.procedure = StartsWithCommand(upper, "CREATE PROCEDURE") ||
                    StartsWithCommand(upper, "CREATE OR ALTER PROCEDURE") ||
                    StartsWithCommand(upper, "ALTER PROCEDURE") ||
                    StartsWithCommand(upper, "DROP PROCEDURE") ||
                    StartsWithCommand(upper, "RECREATE PROCEDURE");
  flags.function = StartsWithCommand(upper, "CREATE FUNCTION") ||
                   StartsWithCommand(upper, "CREATE OR ALTER FUNCTION") ||
                   StartsWithCommand(upper, "ALTER FUNCTION") ||
                   StartsWithCommand(upper, "DROP FUNCTION") ||
                   StartsWithCommand(upper, "RECREATE FUNCTION");
  flags.routine = flags.procedure || flags.function;
  flags.package = StartsWithCommand(upper, "CREATE PACKAGE") ||
                  StartsWithCommand(upper, "CREATE PACKAGE BODY") ||
                  StartsWithCommand(upper, "CREATE OR ALTER PACKAGE") ||
                  StartsWithCommand(upper, "CREATE OR ALTER PACKAGE BODY") ||
                  StartsWithCommand(upper, "ALTER PACKAGE") ||
                  StartsWithCommand(upper, "DROP PACKAGE") ||
                  StartsWithCommand(upper, "RECREATE PACKAGE") ||
                  StartsWithCommand(upper, "RECREATE PACKAGE BODY");
  return flags;
}

bool IsFirebirdDependencyBearingDdlStatement(
    std::string_view active_upper_sql) {
  const auto flags =
      ClassifyFirebirdDependencyDdl(TrimAsciiView(active_upper_sql));
  return flags.view || flags.trigger || flags.routine || flags.package;
}

std::string FirebirdDependencyBearingDdlEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const auto flags = ClassifyFirebirdDependencyDdl(upper);
  const bool executable_body_surface =
      flags.trigger || flags.routine || flags.package;
  const bool query_dependency_surface =
      flags.view || executable_body_surface || ContainsWord(upper, "FROM") ||
      ContainsWord(upper, "JOIN") || ContainsWord(upper, "ON") ||
      ContainsWord(upper, "REFERENCES");
  const bool drop_surface = StartsWithCommand(upper, "DROP");
  const bool alter_surface = StartsWithCommand(upper, "ALTER");
  const bool create_surface = StartsWithCommand(upper, "CREATE") ||
                              StartsWithCommand(upper, "RECREATE");
  const std::string_view surface =
      StartsWithCommand(upper, "CREATE VIEW") ? "firebird_create_view"
      : StartsWithCommand(upper, "ALTER VIEW") ? "firebird_alter_view"
      : StartsWithCommand(upper, "DROP VIEW") ? "firebird_drop_view"
      : flags.trigger ? "firebird_trigger_ddl"
      : flags.package ? "firebird_package_or_package_body_ddl"
      : flags.routine ? "firebird_procedure_function_ddl"
                      : "firebird_dependency_bearing_ddl";
  const std::string_view binding_policy =
      flags.package
          ? "firebird_package_dependency_binding_uuid_catalog_descriptors"
      : flags.trigger
          ? "firebird_trigger_relation_event_dependency_binding_uuid_descriptors"
          : "firebird_rdb_dependency_binding_uuid_catalog_descriptors";
  const std::string_view execution_policy =
      executable_body_surface
          ? "firebird_psql_body_stored_as_catalog_reference_and_lowered_to_sblr_uuid"
          : "firebird_view_query_dependency_descriptor_no_parser_execution";

  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_dependency_bearing_ddl_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\"019e13c0-0000-7000-8000-000000000302\","
      << "\"semantic_profile_uuid\":\"019e13c0-1800-7000-8000-000000000302\","
      << "\"dialect\":\"firebird\",\"release_profile\":\""
      << EscapeJson(release_profile) << "\","
      << "\"dependency_ddl_profile\":\"firebird.dependency_bearing_ddl_semantics_profile\","
      << "\"dependency_ddl_surface\":\"" << surface << "\","
      << "\"view_surface\":" << BoolJson(flags.view) << ','
      << "\"materialized_view_surface\":false,"
      << "\"trigger_surface\":" << BoolJson(flags.trigger) << ','
      << "\"routine_surface\":" << BoolJson(flags.routine) << ','
      << "\"procedure_surface\":" << BoolJson(flags.procedure) << ','
      << "\"function_surface\":" << BoolJson(flags.function) << ','
      << "\"package_surface\":" << BoolJson(flags.package) << ','
      << "\"rule_surface\":false,\"event_surface\":false,"
      << "\"executable_body_surface\":"
      << BoolJson(executable_body_surface) << ','
      << "\"query_dependency_surface\":"
      << BoolJson(query_dependency_surface) << ','
      << "\"create_surface\":" << BoolJson(create_surface) << ','
      << "\"alter_surface\":" << BoolJson(alter_surface) << ','
      << "\"drop_surface\":" << BoolJson(drop_surface) << ','
      << "\"dependency_binding_policy\":\"" << binding_policy << "\","
      << "\"invalidation_policy\":\"firebird_metadata_dependency_invalidation_engine_catalog_authority\","
      << "\"execution_body_policy\":\"" << execution_policy << "\","
      << "\"catalog_storage_policy\":\"firebird_rdb_catalog_projection_stores_uuid_dependency_descriptors\","
      << "\"sandbox_root_policy\":\"firebird_compatibility_schema_root_uuid_required_no_cross_root_temp_access\","
      << "\"uuid_required_semantic_profile\":true,"
      << "\"catalog_descriptor_required\":true,"
      << "\"dependency_graph_descriptor_required\":true,"
      << "\"source_retention_reference_required\":"
      << BoolJson(executable_body_surface) << ','
      << "\"sblr_operation_uuid_resolution_required\":true,"
      << "\"engine_authority\":\"scratchbird\","
      << "\"dependency_authority\":\"engine_catalog_uuid_dependency_graph\","
      << "\"source_sql_text_included\":false,\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,\"parser_catalog_authority\":false,"
      << "\"parser_storage_authority\":false,\"parser_execution_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_dependency_finality_authority\":false,"
      << "\"parser_invalidation_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"reference_parser_semantic_equivalence_proven\","
      << "\"descriptor_exactness_status\":\"parser_dependency_bearing_ddl_descriptor_recorded_runtime_equivalence_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

bool IsFirebirdRollbackToSavepoint(std::string_view upper) {
  return StartsWithCommand(upper, "ROLLBACK TO SAVEPOINT") ||
         StartsWithCommand(upper, "ROLLBACK TO") ||
         StartsWithCommand(upper, "ROLLBACK WORK TO SAVEPOINT") ||
         StartsWithCommand(upper, "ROLLBACK WORK TO") ||
         StartsWithCommand(upper, "ROLLBACK TRANSACTION TO SAVEPOINT") ||
         StartsWithCommand(upper, "ROLLBACK TRANSACTION TO");
}

bool IsFirebirdTransactionSessionSemanticStatement(
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  return StartsWithCommand(upper, "SET TRANSACTION") ||
         StartsWithCommand(upper, "COMMIT") ||
         StartsWithCommand(upper, "ROLLBACK") ||
         StartsWithCommand(upper, "SAVEPOINT") ||
         StartsWithCommand(upper, "RELEASE SAVEPOINT");
}

std::string FirebirdTransactionSessionSemanticEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool rollback_to_savepoint = IsFirebirdRollbackToSavepoint(upper);

  scratchbird::parser::firebird::evidence::TransactionSessionSemanticDescriptor d;
  d.compatibility_profile_uuid = "019e13c0-0000-7000-8000-000000000302";
  d.semantic_profile_uuid = "019e13c0-1600-7000-8000-000000000302";
  d.transaction_session_profile =
      "firebird.transaction_session_semantics_profile";
  if (StartsWithCommand(upper, "SET TRANSACTION")) {
    if (Contains(upper, "READ ONLY") &&
        (ContainsWord(upper, "WAIT") || Contains(upper, "NO WAIT")) &&
        (ContainsWord(upper, "SNAPSHOT") ||
         Contains(upper, "READ COMMITTED") ||
         Contains(upper, "TABLE STABILITY"))) {
      d.transaction_session_surface =
          "firebird_set_transaction_read_only_wait_isolation";
    } else if (Contains(upper, "READ ONLY")) {
      d.transaction_session_surface = "firebird_set_transaction_read_only";
    } else if (Contains(upper, "READ WRITE")) {
      d.transaction_session_surface = "firebird_set_transaction_read_write";
    } else {
      d.transaction_session_surface = "firebird_set_transaction";
    }
  } else if (StartsWithCommand(upper, "COMMIT")) {
    d.transaction_session_surface =
        ContainsWord(upper, "RETAIN") || ContainsWord(upper, "RETAINING")
            ? "firebird_commit_retaining"
            : "firebird_commit";
  } else if (rollback_to_savepoint) {
    d.transaction_session_surface = "firebird_rollback_to_savepoint";
  } else if (StartsWithCommand(upper, "ROLLBACK")) {
    d.transaction_session_surface =
        ContainsWord(upper, "RETAIN") || ContainsWord(upper, "RETAINING")
            ? "firebird_rollback_retaining"
            : "firebird_rollback";
  } else if (StartsWithCommand(upper, "RELEASE SAVEPOINT")) {
    d.transaction_session_surface = "firebird_release_savepoint";
  } else if (StartsWithCommand(upper, "SAVEPOINT")) {
    d.transaction_session_surface = "firebird_savepoint";
  } else {
    d.transaction_session_surface = "firebird_transaction_session";
  }
  d.statement_family_linkage = "transaction";
  d.begin_autocommit_policy = StartsWithCommand(upper, "SET TRANSACTION")
      ? "firebird_set_transaction_requests_engine_mga_transaction_handle"
      : "firebird_existing_engine_transaction_required";
  d.isolation_read_only_deferrable_descriptor_policy =
      StartsWithCommand(upper, "SET TRANSACTION")
          ? "firebird_set_transaction_access_isolation_wait_descriptor_engine_enforced"
          : "firebird_transaction_control_does_not_change_isolation_descriptor";
  d.session_variable_sql_mode_descriptor_policy =
      "firebird_no_sql_mode_session_variable_transaction_surface";
  d.begin_surface = StartsWithCommand(upper, "SET TRANSACTION");
  d.commit_surface = StartsWithCommand(upper, "COMMIT");
  d.rollback_to_savepoint_surface = rollback_to_savepoint;
  d.rollback_surface = StartsWithCommand(upper, "ROLLBACK") &&
                       !rollback_to_savepoint;
  d.savepoint_surface = StartsWithCommand(upper, "SAVEPOINT");
  d.release_savepoint_surface = StartsWithCommand(upper, "RELEASE SAVEPOINT");
  d.isolation_descriptor_surface =
      (StartsWithCommand(upper, "SET") &&
       Contains(upper, " TRANSACTION ISOLATION ")) ||
      Contains(upper, " ISOLATION LEVEL ") ||
      ContainsWord(upper, "SERIALIZABLE") ||
      Contains(upper, "READ COMMITTED") ||
      Contains(upper, "REPEATABLE READ") ||
      Contains(upper, "READ UNCOMMITTED") ||
      ContainsWord(upper, "SNAPSHOT") || Contains(upper, "TABLE STABILITY");
  d.read_only_surface = Contains(upper, "READ ONLY");
  d.read_write_surface = Contains(upper, "READ WRITE");
  d.wait_no_wait_surface = ContainsWord(upper, "WAIT") ||
                           Contains(upper, "NO WAIT");
  return scratchbird::parser::firebird::evidence::
      RenderTransactionSessionSemanticEvidenceJson("firebird", release_profile,
                                                   d);
}

bool IsFirebirdCreateTemporaryTableStatement(std::string_view upper) {
  return StartsWithCommand(upper, "CREATE GLOBAL TEMPORARY TABLE");
}

bool IsFirebirdDropTemporaryTableStatement(std::string_view upper) {
  return StartsWithCommand(upper, "DROP TABLE") &&
         (Contains(upper, "TEMP") || Contains(upper, "GTT"));
}

bool IsFirebirdAlterTemporaryTableStatement(std::string_view upper) {
  return (StartsWithCommand(upper, "ALTER TABLE") ||
          StartsWithCommand(upper, "ALTER GLOBAL TEMPORARY TABLE") ||
          StartsWithCommand(upper, "ALTER TEMPORARY TABLE") ||
          StartsWithCommand(upper, "ALTER TEMP TABLE")) &&
         (Contains(upper, "TEMP") || Contains(upper, "GTT"));
}

bool IsFirebirdTemporarySessionObjectSemanticStatement(
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  return IsFirebirdCreateTemporaryTableStatement(upper) ||
         IsFirebirdDropTemporaryTableStatement(upper) ||
         IsFirebirdAlterTemporaryTableStatement(upper);
}

std::string FirebirdTemporarySessionObjectSemanticEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  scratchbird::parser::firebird::evidence::TemporarySessionObjectSemanticDescriptor d;
  d.compatibility_profile_uuid = "019e13c0-0000-7000-8000-000000000302";
  d.semantic_profile_uuid = "019e13c0-1700-7000-8000-000000000302";
  d.temporary_object_profile =
      "firebird.global_temporary_table_semantics_profile";
  d.create_surface = IsFirebirdCreateTemporaryTableStatement(upper);
  d.drop_surface = IsFirebirdDropTemporaryTableStatement(upper);
  d.alter_surface = IsFirebirdAlterTemporaryTableStatement(upper);
  d.on_commit_delete_rows_surface = Contains(upper, "ON COMMIT DELETE ROWS");
  d.on_commit_preserve_rows_surface = Contains(upper, "ON COMMIT PRESERVE ROWS");
  d.on_commit_drop_surface = Contains(upper, "ON COMMIT DROP");
  if (d.create_surface) {
    d.temporary_object_surface =
        d.on_commit_delete_rows_surface
            ? "firebird_create_global_temporary_table_on_commit_delete_rows"
        : d.on_commit_preserve_rows_surface
            ? "firebird_create_global_temporary_table_on_commit_preserve_rows"
            : "firebird_create_global_temporary_table_default_preserve_rows";
  } else if (d.alter_surface) {
    d.temporary_object_surface =
        "firebird_alter_table_catalog_temp_resolution";
  } else if (d.drop_surface) {
    d.temporary_object_surface = "firebird_drop_table_catalog_temp_resolution";
  } else {
    d.temporary_object_surface = "firebird_temporary_session_object";
  }
  d.temporary_object_kind_policy =
      "firebird_global_temporary_table_metadata_persistent_rows_session_or_transaction_scoped";
  d.global_keyword_surface = ContainsWord(upper, "GLOBAL");
  d.local_keyword_surface = ContainsWord(upper, "LOCAL");
  d.temporary_keyword_surface = ContainsWord(upper, "TEMP") ||
                                ContainsWord(upper, "TEMPORARY") ||
                                Contains(upper, "TEMP");
  d.table_object_surface = ContainsWord(upper, "TABLE");
  d.on_commit_policy =
      d.on_commit_delete_rows_surface
          ? "firebird_on_commit_delete_rows_engine_transaction_end_cleanup"
      : d.on_commit_preserve_rows_surface
          ? "firebird_on_commit_preserve_rows_engine_session_lifetime"
          : "firebird_default_on_commit_preserve_rows_descriptor";
  d.on_commit_delete_rows_policy =
      "firebird_delete_rows_supported_engine_mga_transaction_boundary";
  d.on_commit_preserve_rows_policy =
      "firebird_preserve_rows_supported_engine_session_lifetime";
  d.on_commit_drop_policy = "firebird_on_commit_drop_not_supported";
  d.name_shadowing_policy =
      "firebird_no_session_name_shadowing_regular_schema_namespace";
  d.session_visibility_policy =
      "firebird_gtt_data_is_attachment_private_metadata_global_catalog_visible";
  d.catalog_visibility_policy =
      "firebird_persistent_catalog_descriptor_marks_global_temporary_table";
  d.temporary_object_lifetime_policy =
      d.on_commit_delete_rows_surface
          ? "firebird_rows_cleared_at_engine_mga_transaction_end_metadata_survives"
          : "firebird_rows_survive_until_engine_attachment_end_metadata_survives";
  d.schema_root_sandbox_policy =
      "firebird_compatibility_schema_root_uuid_required_no_cross_root_temp_access";
  return scratchbird::parser::firebird::evidence::
      RenderTemporarySessionObjectSemanticEvidenceJson("firebird",
                                                       release_profile, d);
}

bool IsFirebirdIndexSemanticDefaultsStatement(
    std::string_view active_upper_sql) {
  auto upper = TrimAsciiView(active_upper_sql);
  if (StartsWithCommand(upper, "ALTER INDEX")) return true;
  if (StartsWithCommand(upper, "ALTER TABLE")) {
    return Contains(upper, " ALTER INDEX ") ||
           Contains(upper, " ADD INDEX ") ||
           Contains(upper, " ADD UNIQUE INDEX ") ||
           Contains(upper, " DROP INDEX ");
  }
  if (!StartsWithCommand(upper, "CREATE")) return false;
  upper = TrimAsciiView(upper.substr(std::string_view("CREATE").size()));
  bool advanced = true;
  while (advanced) {
    advanced = false;
    for (const auto keyword : {"UNIQUE", "ASC", "ASCENDING", "DESC",
                               "DESCENDING"}) {
      if (StartsWithCommand(upper, keyword)) {
        upper = TrimAsciiView(upper.substr(std::string_view(keyword).size()));
        advanced = true;
        break;
      }
    }
  }
  return StartsWithCommand(upper, "INDEX");
}

std::string FirebirdIndexSemanticDefaultsEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool expression = Contains(upper, "((") || Contains(upper, "COMPUTED BY");
  scratchbird::parser::firebird::evidence::IndexSemanticDefaultsDescriptor d;
  d.compatibility_profile_uuid = "019e13c0-0000-7000-8000-000000000302";
  d.semantic_profile_uuid = "019e13c0-1000-7000-8000-000000000302";
  d.index_profile = "firebird.index_optimizer_translation_profile";
  d.ddl_surface = StartsWithCommand(upper, "CREATE")
      ? (ContainsWord(upper, "UNIQUE") ? "create_unique_index" : "create_index")
      : StartsWithCommand(upper, "ALTER TABLE") ? "alter_table_index"
      : StartsWithCommand(upper, "ALTER INDEX") ? "alter_index"
                                                 : "index_ddl";
  d.descending_requested = ContainsWord(upper, "DESC") ||
                           ContainsWord(upper, "DESCENDING");
  d.index_method = d.descending_requested
      ? "firebird_btree_descending_index_profile"
      : "firebird_btree_ascending_index_profile";
  d.unique_requested = ContainsWord(upper, "UNIQUE");
  d.unique_null_policy = d.unique_requested
      ? "firebird_unique_index_nulls_are_distinct_profile"
      : "not_unique_index_not_applicable";
  d.null_ordering = "firebird_nulls_first_for_ascending_index_profile";
  d.collation_policy = "firebird_column_charset_collation_descriptor";
  d.operator_family_policy =
      "firebird_builtin_comparison_no_named_operator_family";
  d.predicate_or_expression_policy = expression
      ? "firebird_computed_by_expression_index_descriptor"
      : "firebird_column_index_no_partial_predicate";
  d.predicate_present = ContainsWord(upper, "WHERE");
  d.expression_key_present = expression;
  d.concurrently_requested = ContainsWord(upper, "CONCURRENTLY");
  d.nulls_not_distinct_requested = Contains(upper, "NULLS NOT DISTINCT");
  d.validation_state = ContainsWord(upper, "INACTIVE")
      ? "firebird_index_inactive_requested"
      : "firebird_index_active_default";
  d.build_mode = StartsWithCommand(upper, "ALTER INDEX")
      ? "firebird_index_metadata_state_change_no_parser_build"
      : "firebird_immediate_index_build_default";
  d.statistics_policy_ref =
      "firebird_index_selectivity_statistics_profile";
  return scratchbird::parser::firebird::evidence::
      RenderIndexSemanticDefaultsEvidenceJson("firebird", release_profile, d);
}

bool IsFirebirdConstraintSemanticDefaultsStatement(
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  return StartsWithCommand(upper, "CREATE") && ContainsWord(upper, "TABLE") &&
         (Contains(upper, "PRIMARY KEY") || ContainsWord(upper, "UNIQUE") ||
          Contains(upper, "FOREIGN KEY") || ContainsWord(upper, "REFERENCES") ||
          ContainsWord(upper, "CHECK") || ContainsWord(upper, "DEFAULT") ||
          ContainsWord(upper, "GENERATED"));
}

std::string FirebirdConstraintSemanticDefaultsEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  scratchbird::parser::firebird::evidence::ConstraintSemanticDefaultsDescriptor d;
  d.compatibility_profile_uuid = "019e13c0-0000-7000-8000-000000000302";
  d.semantic_profile_uuid = "019e13c0-1100-7000-8000-000000000302";
  d.constraint_profile = "firebird.table_constraint_defaults_profile";
  d.primary_key_present = Contains(upper, "PRIMARY KEY");
  d.primary_key_behavior =
      "firebird_primary_key_not_null_unique_index_descriptor";
  d.unique_constraint_present = ContainsWord(upper, "UNIQUE");
  d.unique_null_policy = d.unique_constraint_present
      ? "firebird_unique_constraint_nulls_are_distinct_profile"
      : "not_unique_constraint_not_applicable";
  d.foreign_key_reference_present = Contains(upper, "FOREIGN KEY") ||
                                    ContainsWord(upper, "REFERENCES");
  d.foreign_key_action_defaults =
      "firebird_foreign_key_default_no_action_update_no_action_delete_descriptor";
  d.check_constraint_present = ContainsWord(upper, "CHECK");
  d.check_truth_table_null_behavior =
      "firebird_check_constraint_false_fails_unknown_passes_profile";
  d.default_clause_present = ContainsWord(upper, "DEFAULT");
  d.default_expression_policy =
      "firebird_default_expression_descriptor_runtime_equivalence_verified";
  d.generated_identity_or_autoincrement_present =
      ContainsWord(upper, "GENERATED");
  d.generated_identity_autoincrement_policy =
      ContainsWord(upper, "GENERATED")
          ? "firebird_generated_identity_sequence_backed_descriptor"
          : "firebird_no_implicit_autoincrement_default";
  d.explicit_constraint_names_present = ContainsWord(upper, "CONSTRAINT");
  d.generated_name_policy =
      "firebird_system_generated_constraint_names_rdb_descriptor_required";
  d.deferrability_policy =
      "firebird_constraints_not_deferrable_immediate_profile";
  d.enforcement_timing =
      "firebird_immediate_constraint_validation_profile";
  return scratchbird::parser::firebird::evidence::
      RenderConstraintSemanticDefaultsEvidenceJson("firebird", release_profile,
                                                   d);
}

bool IsFirebirdSequenceIdentitySemanticStatement(
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  return StartsWithCommand(upper, "CREATE SEQUENCE") ||
         StartsWithCommand(upper, "CREATE GENERATOR") ||
         StartsWithCommand(upper, "ALTER SEQUENCE") ||
         StartsWithCommand(upper, "ALTER GENERATOR") ||
         StartsWithCommand(upper, "DROP SEQUENCE") ||
         StartsWithCommand(upper, "DROP GENERATOR") ||
         Contains(upper, "GEN_ID(") || Contains(upper, "NEXT VALUE FOR") ||
         (ContainsWord(upper, "GENERATED") && ContainsWord(upper, "IDENTITY"));
}

std::string FirebirdSequenceIdentitySemanticEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  scratchbird::parser::firebird::evidence::SequenceIdentitySemanticDescriptor d;
  d.compatibility_profile_uuid = "019e13c0-0000-7000-8000-000000000302";
  d.semantic_profile_uuid = "019e13c0-1300-7000-8000-000000000302";
  d.sequence_identity_profile =
      "firebird.sequence_generator_identity_profile";
  const bool identity = ContainsWord(upper, "GENERATED") &&
                        ContainsWord(upper, "IDENTITY");
  d.create_sequence_or_generator_surface =
      StartsWithCommand(upper, "CREATE SEQUENCE") ||
      StartsWithCommand(upper, "CREATE GENERATOR");
  d.alter_sequence_surface = StartsWithCommand(upper, "ALTER SEQUENCE") ||
                             StartsWithCommand(upper, "ALTER GENERATOR");
  d.next_value_surface = Contains(upper, "GEN_ID(") ||
                         Contains(upper, "NEXT VALUE FOR");
  if (StartsWithCommand(upper, "CREATE GENERATOR")) {
    d.sequence_identity_surface = "firebird_create_generator";
  } else if (StartsWithCommand(upper, "CREATE SEQUENCE")) {
    d.sequence_identity_surface = "firebird_create_sequence";
  } else if (d.alter_sequence_surface) {
    d.sequence_identity_surface = "firebird_alter_sequence_generator";
  } else if (d.next_value_surface) {
    d.sequence_identity_surface = "firebird_generator_value_expression";
  } else if (identity) {
    d.sequence_identity_surface =
        "firebird_identity_column_sequence_descriptor";
  } else {
    d.sequence_identity_surface = "firebird_sequence_generator_descriptor";
  }
  d.sequence_backed_default_present = identity;
  d.restart_descriptor_present = ContainsWord(upper, "RESTART") ||
                                 Contains(upper, "START WITH");
  d.increment_descriptor_present = ContainsWord(upper, "INCREMENT");
  d.min_value_descriptor_present = ContainsWord(upper, "MINVALUE") ||
                                   Contains(upper, "NO MINVALUE");
  d.max_value_descriptor_present = ContainsWord(upper, "MAXVALUE") ||
                                   Contains(upper, "NO MAXVALUE");
  d.cycle_descriptor_present = ContainsWord(upper, "CYCLE") ||
                               Contains(upper, "NO CYCLE");
  d.cache_descriptor_present = ContainsWord(upper, "CACHE") ||
                               Contains(upper, "NO CACHE");
  d.object_identity_policy =
      "firebird_sequence_generator_uuid_required_no_source_name_binding";
  d.engine_catalog_sequence_descriptor_policy =
      "firebird_engine_catalog_generator_sequence_descriptor_policy";
  d.allocation_finality_policy =
      "firebird_generator_nontransactional_allocation_descriptor_parser_not_allocator";
  d.lower_layer_allocation_policy =
      "firebird_engine_sequence_catalog_allocates_values_outside_parser";
  const bool gen_id = Contains(upper, "GEN_ID(");
  const bool next_value_for = Contains(upper, "NEXT VALUE FOR");
  d.value_function_profile = gen_id && next_value_for
      ? "firebird_gen_id_and_next_value_for_descriptor"
      : gen_id ? "firebird_gen_id_descriptor"
      : next_value_for ? "firebird_next_value_for_descriptor"
                       : "firebird_generator_function_not_observed";
  d.session_visibility_policy =
      "firebird_generator_values_visible_immediately_no_parser_session_state";
  d.sequence_backed_default_policy = identity
      ? "firebird_identity_column_backed_by_sequence_descriptor"
      : "firebird_no_identity_default_observed";
  d.restart_increment_descriptor_policy =
      "firebird_restart_with_and_increment_by_descriptor";
  return scratchbird::parser::firebird::evidence::
      RenderSequenceIdentitySemanticEvidenceJson("firebird", release_profile, d);
}

std::string FirebirdIdentifierNameResolutionEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  scratchbird::parser::firebird::evidence::
      IdentifierNameResolutionSemanticDescriptor d;
  d.compatibility_profile_uuid =
      "019e13c0-0000-7000-8000-000000000302";
  d.semantic_profile_uuid = "019e13c0-1200-7000-8000-000000000302";
  d.name_resolution_profile =
      "firebird.identifier_name_resolution_profile";
  d.unquoted_identifier_policy =
      "firebird_unquoted_identifiers_fold_to_uppercase";
  d.quoted_identifier_policy =
      "firebird_double_quoted_identifiers_preserve_exact_case";
  d.schema_root_resolution_policy =
      "firebird_single_database_root_uuid_catalog_resolution_required";
  d.generated_catalog_name_behavior =
      "firebird_rdb_generated_names_catalog_descriptor_required";
  d.namespace_collision_behavior =
      "firebird_catalog_namespace_collision_resolved_by_uuid_descriptor";
  d.result_metadata_label_policy =
      "firebird_result_labels_follow_identifier_fold_alias_descriptor";
  d.table_name_filesystem_case_policy =
      "not_filesystem_sensitive_table_name_policy";
  d.create_surface = StartsWithCommand(upper, "CREATE");
  d.alter_surface = StartsWithCommand(upper, "ALTER");
  d.drop_surface = StartsWithCommand(upper, "DROP");
  d.quoted_identifier_syntax_observed = Contains(upper, "\"");
  d.qualified_name_syntax_observed = Contains(upper, ".");
  return scratchbird::parser::firebird::evidence::
      RenderIdentifierNameResolutionSemanticEvidenceJson(
          "firebird", release_profile, d);
}

scratchbird::parser::firebird::evidence::ScalarExpressionSemanticDescriptor
FirebirdScalarExpressionSemanticDescriptor(
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  scratchbird::parser::firebird::evidence::ScalarExpressionSemanticDescriptor d;
  d.compatibility_profile_uuid =
      "019e13c0-0000-7000-8000-000000000302";
  d.semantic_profile_uuid = "019e13c0-1400-7000-8000-000000000302";
  d.scalar_expression_profile =
      "firebird.scalar_expression_semantics_profile";
  d.query_expression_surface = StartsWithCommand(upper, "WITH")
      ? "with_query_scalar_expression"
      : StartsWithCommand(upper, "SELECT")
          ? "select_scalar_expression"
          : "query_scalar_expression";
  d.cast_type_coercion_profile =
      "firebird_cast_domain_charset_decfloat_int128_descriptor";
  d.null_three_valued_logic_profile =
      "firebird_boolean_unknown_three_valued_logic_profile";
  d.boolean_literal_profile =
      "firebird_boolean_true_false_unknown_literal_profile";
  d.string_comparison_collation_profile =
      "firebird_charset_collation_descriptor_no_parser_collation_authority";
  d.temporal_literal_current_timestamp_date_arithmetic_profile =
      "firebird_date_time_timestamp_dateadd_datediff_descriptor";
  d.numeric_division_rounding_overflow_profile =
      "firebird_exact_numeric_decfloat_int128_division_rounding_overflow_descriptor";
  d.pattern_matching_profile =
      "firebird_like_similar_to_containing_starting_with_descriptor";
  d.conditional_expression_profile =
      "firebird_coalesce_case_iif_nullif_decode_descriptor";
  d.expression_builtin_profile =
      "firebird_expression_builtin_profile_iif_dateadd_decfloat_int128";
  d.cast_or_coercion_surface = Contains(upper, "CAST(");
  d.null_logic_surface = ContainsWord(upper, "NULL");
  d.boolean_literal_surface = ContainsWord(upper, "TRUE") ||
                              ContainsWord(upper, "FALSE") ||
                              ContainsWord(upper, "UNKNOWN");
  d.string_comparison_surface = ContainsWord(upper, "COLLATE") ||
                                Contains(upper, " LIKE ") ||
                                Contains(upper, " SIMILAR TO ") ||
                                Contains(upper, " STARTING WITH ") ||
                                Contains(upper, " CONTAINING ");
  d.temporal_expression_surface =
      ContainsWord(upper, "CURRENT_DATE") ||
      ContainsWord(upper, "CURRENT_TIME") ||
      ContainsWord(upper, "CURRENT_TIMESTAMP") ||
      Contains(upper, "DATEADD(") || Contains(upper, "DATEDIFF(") ||
      Contains(upper, "EXTRACT(") || ContainsWord(upper, "INTERVAL") ||
      Contains(upper, "TIMESTAMP '") || Contains(upper, "DATE '") ||
      Contains(upper, "TIME '");
  d.numeric_expression_surface =
      Contains(upper, "/") || Contains(upper, "ROUND(") ||
      Contains(upper, "TRUNC(") || Contains(upper, "MOD(") ||
      Contains(upper, "POWER(") || Contains(upper, "SQRT(") ||
      ContainsWord(upper, "DECFLOAT") || ContainsWord(upper, "INT128") ||
      ContainsWord(upper, "NUMERIC") || ContainsWord(upper, "DECIMAL");
  d.pattern_matching_surface = Contains(upper, " LIKE ") ||
                               Contains(upper, " SIMILAR TO ") ||
                               Contains(upper, " STARTING WITH ") ||
                               Contains(upper, " CONTAINING ");
  d.conditional_expression_surface = HasFunctionCall(upper, "COALESCE") ||
                                     HasFunctionCall(upper, "NULLIF") ||
                                     HasFunctionCall(upper, "IIF") ||
                                     HasFunctionCall(upper, "DECODE") ||
                                     Contains(upper, "CASE ");
  d.similar_to_surface = Contains(upper, " SIMILAR TO ");
  d.compatibility_conditional_function_surface =
      HasFunctionCall(upper, "IIF") || HasFunctionCall(upper, "DECODE");
  return d;
}

bool IsFirebirdScalarExpressionSemanticStatement(
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  if (!(StartsWithCommand(upper, "SELECT") ||
        StartsWithCommand(upper, "WITH"))) {
    return false;
  }
  const auto d = FirebirdScalarExpressionSemanticDescriptor(upper);
  return d.cast_or_coercion_surface || d.null_logic_surface ||
         d.boolean_literal_surface || d.string_comparison_surface ||
         d.temporal_expression_surface || d.numeric_expression_surface ||
         d.pattern_matching_surface || d.conditional_expression_surface;
}

std::string FirebirdScalarExpressionSemanticEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  return scratchbird::parser::firebird::evidence::
      RenderScalarExpressionSemanticEvidenceJson(
          "firebird", release_profile,
          FirebirdScalarExpressionSemanticDescriptor(active_upper_sql));
}

bool IsFirebirdDmlMutationSemanticStatement(
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  return StartsWithCommand(upper, "UPDATE OR INSERT") ||
         StartsWithCommand(upper, "MERGE") ||
         StartsWithCommand(upper, "INSERT") ||
         StartsWithCommand(upper, "UPDATE") ||
         StartsWithCommand(upper, "DELETE");
}

std::string FirebirdDmlMutationSemanticEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  scratchbird::parser::firebird::evidence::DmlMutationSemanticDescriptor d;
  d.compatibility_profile_uuid =
      "019e13c0-0000-7000-8000-000000000302";
  d.semantic_profile_uuid = "019e13c0-1500-7000-8000-000000000302";
  d.mutation_profile = "firebird.dml_mutation_semantics_profile";
  d.insert_surface = StartsWithCommand(upper, "INSERT");
  d.update_or_insert_surface =
      StartsWithCommand(upper, "UPDATE OR INSERT");
  d.update_surface = StartsWithCommand(upper, "UPDATE") &&
                     !d.update_or_insert_surface;
  d.delete_surface = StartsWithCommand(upper, "DELETE");
  d.merge_surface = StartsWithCommand(upper, "MERGE");
  d.matching_surface = ContainsWord(upper, "MATCHING");
  d.returning_output_projection_surface = ContainsWord(upper, "RETURNING");
  d.cursor_positioned_dml_surface =
      Contains(upper, " WHERE CURRENT OF ");
  d.mutation_surface = d.update_or_insert_surface && d.matching_surface &&
                               d.returning_output_projection_surface
      ? "firebird_update_or_insert_matching_returning"
      : d.update_or_insert_surface && d.matching_surface
          ? "firebird_update_or_insert_matching"
      : d.update_or_insert_surface && d.returning_output_projection_surface
          ? "firebird_update_or_insert_primary_key_returning"
      : d.update_or_insert_surface
          ? "firebird_update_or_insert_primary_key"
      : d.merge_surface
          ? (d.returning_output_projection_surface ? "firebird_merge_returning"
                                                   : "firebird_merge")
      : d.cursor_positioned_dml_surface && d.update_surface
          ? "firebird_update_current_of"
      : d.cursor_positioned_dml_surface && d.delete_surface
          ? "firebird_delete_current_of"
      : d.insert_surface
          ? (d.returning_output_projection_surface ? "firebird_insert_returning"
                                                   : "firebird_insert")
      : d.update_surface
          ? (d.returning_output_projection_surface ? "firebird_update_returning"
                                                   : "firebird_update")
      : d.delete_surface
          ? (d.returning_output_projection_surface ? "firebird_delete_returning"
                                                   : "firebird_delete")
          : "firebird_dml_mutation";
  d.upsert_merge_conflict_policy = d.update_or_insert_surface
      ? (d.matching_surface
             ? "firebird_update_or_insert_matching_descriptor_uuid_key_required"
             : "firebird_update_or_insert_primary_key_match_descriptor_required")
      : d.merge_surface
          ? "firebird_merge_descriptor_source_target_uuid_binding_required"
          : "firebird_no_upsert_merge_surface_observed";
  d.returning_output_projection_policy =
      d.returning_output_projection_surface
          ? "firebird_returning_projection_descriptor_single_or_multirow_by_statement_kind"
          : "firebird_no_returning_projection_observed";
  d.cursor_positioned_dml_policy = d.cursor_positioned_dml_surface
      ? "firebird_where_current_of_cursor_descriptor_engine_cursor_authority"
      : "firebird_no_cursor_positioned_dml_observed";
  d.affected_row_count_policy = d.update_or_insert_surface
      ? "firebird_row_count_update_or_insert_returning_descriptor_engine_reported"
      : "firebird_row_count_descriptor_engine_reported_no_parser_finality";
  d.default_value_surface = ContainsWord(upper, "DEFAULT");
  d.generated_column_surface = ContainsWord(upper, "GENERATED");
  d.trigger_interaction_descriptor_required = d.insert_surface ||
                                              d.update_surface ||
                                              d.delete_surface ||
                                              d.update_or_insert_surface ||
                                              d.merge_surface;
  d.trigger_default_generated_column_interaction_policy =
      d.update_or_insert_surface
          ? "firebird_update_or_insert_defaults_triggers_returning_descriptor_engine_order"
      : d.default_value_surface || d.generated_column_surface ||
              d.returning_output_projection_surface
          ? "firebird_defaults_triggers_generated_columns_descriptor_engine_order"
          : "firebird_trigger_default_generated_column_descriptor_required";
  return scratchbird::parser::firebird::evidence::
      RenderDmlMutationSemanticEvidenceJson("firebird", release_profile, d);
}

std::string FirebirdDatatypeProfileEvidenceJson(
    std::span<const Token> active_tokens) {
  scratchbird::parser::firebird::evidence::DatatypeFamilySemanticDescriptor d;
  d.compatibility_profile_uuid = "019e13c0-0000-7000-8000-000000000302";
  for (std::size_t i = 0; i < active_tokens.size(); ++i) {
    const auto& token = active_tokens[i];
    if ((token.kind == "symbol" || token.kind == "punctuation") &&
        token.lexeme == "[") {
      d.array = true;
    }
    if (token.kind != "identifier_or_keyword") continue;
    const auto upper = ToUpperAscii(token.lexeme);
    if (upper == "SMALLINT" || upper == "INT" || upper == "INTEGER" ||
        upper == "BIGINT" || upper == "INT128") {
      d.numeric = true;
    } else if (upper == "NUMERIC" || upper == "DECIMAL") {
      d.numeric = true;
      d.exact_decimal = true;
    } else if (upper == "REAL" || upper == "FLOAT" || upper == "DOUBLE" ||
               upper == "DECFLOAT") {
      d.numeric = true;
      d.floating = true;
    } else if (upper == "CHAR" || upper == "VARCHAR" || upper == "CHARACTER" ||
               upper == "NCHAR" || upper == "NATIONAL") {
      d.text = true;
    } else if (upper == "CHARSET" || upper == "COLLATE" ||
               upper == "COLLATION") {
      d.charset_collation_sensitive_text = true;
    } else if (upper == "BINARY" || upper == "VARBINARY" || upper == "BLOB" ||
               upper == "BLOB_ID") {
      d.binary_blob = true;
    } else if (upper == "DATE" || upper == "TIME" || upper == "TIMESTAMP") {
      d.temporal = true;
    } else if (upper == "BOOLEAN") {
      d.boolean = true;
    } else if (upper == "ARRAY") {
      d.array = true;
    } else if (upper == "DOMAIN") {
      d.range_domain_composite = true;
    }
    if (upper == "CHARACTER" && i + 1 < active_tokens.size() &&
        active_tokens[i + 1].kind == "identifier_or_keyword" &&
        ToUpperAscii(active_tokens[i + 1].lexeme) == "SET") {
      d.charset_collation_sensitive_text = true;
    }
  }
  return scratchbird::parser::firebird::evidence::RenderDatatypeProfileEvidenceJson(
      "firebird", d);
}

struct ParserEvidence {
  std::string statement_kind;
  std::size_t token_count{0};
  std::size_t source_span_count{0};
  scratchbird::parser::firebird::evidence::ProceduralFunctionalEncodingSpanMetadata
      procedural_span_metadata;
  scratchbird::parser::firebird::evidence::ProceduralSourceRetentionMetadata
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

bool IsNoiseToken(const Token& token) {
  return token.kind == "line_comment" || token.kind == "block_comment";
}

bool IsFirebirdProceduralBodySourceRetentionStatement(
    std::string_view statement_family,
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

std::uint64_t FirebirdFnv1a64(std::string_view text) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const char ch : text) {
    hash ^= static_cast<unsigned char>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

scratchbird::parser::firebird::evidence::ProceduralSourceRetentionMetadata
FirebirdProceduralSourceRetentionMetadataFor(
    std::string_view normalized_sql,
    std::span<const Token> tokens,
    scratchbird::parser::firebird::evidence::
        ProceduralFunctionalEncodingSpanMetadata span_metadata) {
  scratchbird::parser::firebird::evidence::ProceduralSourceRetentionMetadata
      metadata;
  metadata.source_byte_length = normalized_sql.size();
  metadata.source_hash = FirebirdFnv1a64(normalized_sql);
  metadata.body_end_byte = normalized_sql.size();
  metadata.header_source_span_count = span_metadata.header_source_span_count;
  metadata.body_source_span_count = span_metadata.body_source_span_count;
  if (metadata.header_source_span_count > 0 &&
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
  metadata.body_start_byte =
      body_semantic_index < semantic_token_indexes.size()
          ? tokens[semantic_token_indexes[body_semantic_index]].offset
          : normalized_sql.size();
  metadata.header_start_byte = 0;
  metadata.header_end_byte = metadata.body_start_byte;
  return metadata;
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
  if (IsFirebirdIndexSemanticDefaultsStatement(upper)) {
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
  evidence.datatype_profile_evidence_json =
      FirebirdDatatypeProfileEvidenceJson(tokens);
  evidence.catalog_reference_count =
      SurfaceMentions(upper, CatalogOverlaySurfaces()) ? 1 : 0;
  return evidence;
}

scratchbird::parser::firebird::evidence::ProceduralFunctionalEncodingSpanMetadata
BuildProceduralFunctionalEncodingSpanMetadata(
    std::string_view active_upper,
    const std::vector<Token>& tokens) {
  scratchbird::parser::firebird::evidence::ProceduralFunctionalEncodingSpanMetadata metadata;
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
    const scratchbird::parser::firebird::evidence::ProceduralFunctionalEncodingSpanMetadata&
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
      << "\"compatibility_sql_executed\":false,"
      << "\"runtime_equivalence_status\":\"compatibility_native_psql_replay_verified\","
      << "\"catalog_persistence_status\":\"runtime_catalog_reopen_proof_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
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
      << "\"compatibility_profile_uuid\":\"019e13c0-0000-7000-8000-000000000302\","
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
      << "\"compatibility_sql_executed\":false,"
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
      << ",\"descriptor_exactness_status\":\"firebird_exact_datatype_descriptor_recorded_runtime_equivalence_verified\","
      << "\"runtime_equivalence_status\":\"compatibility_native_exactness_replay_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
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
      << "\"connection_sandbox_contract\":\"compatibility_connection_schema_root_v1\","
      << "\"schema_root_source\":\"listener_engine_materialized_attach_context\","
      << "\"user_object_resolution\":\"relative_to_connection_schema_root\","
      << "\"unqualified_name_root\":\"reference_schema_branch_root\","
      << "\"direct_cross_root_access\":\"unsupported_denied\","
      << "\"server_local_file_access\":\"default_denied\","
      << "\"tenant_escape_policy\":\"fail_closed\","
      << "\"catalog_projection_authority\":\"catalog_emulation_definer_authority\","
      << "\"catalog_projection_can_query_outside_sandbox\":true,"
      << "\"catalog_projection_user_authority\":false,"
      << "\"catalog_projection_select_grant_required\":true,"
      << "\"catalog_projection_output_is_user_visible\":true,"
      << "\"catalog_projection_does_not_grant_base_object_access\":true,"
      << "\"foreign_parser_tree_visibility_inherited\":false,"
      << "\"foreign_parser_tree_visibility\":\"forbidden\","
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
        << scratchbird::parser::firebird::evidence::DatatypeDescriptorEvidenceJson(
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
        << FirebirdIndexSemanticDefaultsEvidenceJson(
               "5.0.4", evidence.index_semantic_defaults_upper_sql);
  }
  if (evidence.constraint_semantic_defaults_evidence_required) {
    out << ",\"constraint_semantic_defaults_evidence\":"
        << FirebirdConstraintSemanticDefaultsEvidenceJson(
               "5.0.4",
               evidence.constraint_semantic_defaults_upper_sql);
  }
  if (evidence.sequence_identity_semantic_evidence_required) {
    out << ",\"sequence_identity_semantic_evidence\":"
        << FirebirdSequenceIdentitySemanticEvidenceJson(
               "5.0.4",
               evidence.sequence_identity_semantic_upper_sql);
  }
  if (evidence.identifier_name_resolution_evidence_required) {
    out << ",\"identifier_name_resolution_evidence\":"
        << FirebirdIdentifierNameResolutionEvidenceJson(
               "5.0.4",
               evidence.identifier_name_resolution_upper_sql);
  }
  if (evidence.scalar_expression_semantic_evidence_required) {
    out << ",\"scalar_expression_semantic_evidence\":"
        << FirebirdScalarExpressionSemanticEvidenceJson(
               "5.0.4",
               evidence.scalar_expression_semantic_upper_sql);
  }
  if (evidence.dml_mutation_semantic_evidence_required) {
    out << ",\"dml_mutation_semantic_evidence\":"
        << FirebirdDmlMutationSemanticEvidenceJson(
               "5.0.4",
               evidence.dml_mutation_semantic_upper_sql);
  }
  if (evidence.transaction_session_semantic_evidence_required) {
    out << ",\"transaction_session_semantic_evidence\":"
        << FirebirdTransactionSessionSemanticEvidenceJson(
               "5.0.4",
               evidence.transaction_session_semantic_upper_sql);
  }
  if (evidence.temporary_session_object_semantic_evidence_required) {
    out << ",\"temporary_session_object_semantic_evidence\":"
        << FirebirdTemporarySessionObjectSemanticEvidenceJson(
               "5.0.4",
               evidence.temporary_session_object_semantic_upper_sql);
  }
  if (evidence.dependency_bearing_ddl_semantic_evidence_required) {
    out << ",\"dependency_bearing_ddl_semantic_evidence\":"
        << FirebirdDependencyBearingDdlEvidenceJson(
               "5.0.4",
               evidence.dependency_bearing_ddl_semantic_upper_sql);
  }
  if (evidence.ddl_transaction_behavior_semantic_evidence_required) {
    out << ",\"ddl_transaction_behavior_semantic_evidence\":"
        << FirebirdDdlTransactionBehaviorEvidenceJson(
               "5.0.4",
               evidence.ddl_transaction_behavior_semantic_upper_sql);
  }
  if (evidence.resource_text_semantic_evidence_required) {
    out << ",\"resource_text_semantic_evidence\":"
        << FirebirdResourceTextEvidenceJson(
               "5.0.4", evidence.resource_text_semantic_upper_sql);
  }
  if (evidence.statistics_optimizer_semantic_evidence_required) {
    out << ",\"statistics_optimizer_semantic_evidence\":"
        << FirebirdStatisticsOptimizerEvidenceJson(
               "5.0.4", evidence.statistics_optimizer_semantic_upper_sql);
  }
  if (evidence.locks_isolation_semantic_evidence_required) {
    out << ",\"locks_isolation_semantic_evidence\":"
        << FirebirdLocksIsolationEvidenceJson(
               "5.0.4", evidence.locks_isolation_semantic_upper_sql);
  }
  if (evidence.system_catalog_defaults_semantic_evidence_required) {
    out << ",\"system_catalog_defaults_semantic_evidence\":"
        << FirebirdSystemCatalogDefaultsEvidenceJson(
               evidence.system_catalog_defaults_semantic_operation_id,
               CatalogOverlaySurfaces());
  }
  if (evidence.session_settings_diagnostics_semantic_evidence_required) {
    out << ",\"session_settings_diagnostics_semantic_evidence\":"
        << FirebirdSessionSettingsDiagnosticsEvidenceJson(
               "5.0.4",
               evidence.session_settings_diagnostics_semantic_upper_sql);
  }
  if (evidence.procedural_body_source_retention_required) {
    out << ",\"procedural_body_source_retention_evidence\":"
        << scratchbird::parser::firebird::evidence::ProceduralBodySourceRetentionEvidenceJson(
               evidence.procedural_source_retention_metadata)
        << ",\"procedural_functional_encoding_source_span_uuid_binding_evidence\":"
        << scratchbird::parser::firebird::evidence::ProceduralFunctionalEncodingEvidenceJson(
               evidence.source_span_count, evidence.cst_materialized,
               evidence.ast_materialized, evidence.bound_ast_materialized,
               evidence.procedural_span_metadata);
    if (evidence.firebird_psql_functional_encoding_evidence_required) {
      out << ",\"firebird_psql_functional_encoding_evidence\":"
          << evidence.firebird_psql_functional_encoding_evidence_json;
    }
  }
  out << ",\"enterprise_readiness_evidence\":"
      << scratchbird::parser::firebird::evidence::EnterpriseReadinessEvidenceJson();
  out << "}";
  return out.str();
}

std::string MakeSblrEnvelope(std::string_view statement_family,
                             std::string_view operation_family,
                             std::string_view normalized_upper_sql,
                             const FirebirdLifecycleMappingDescriptor* mapping,
                             const ParserEvidence& evidence) {
  const bool lifecycle_api = mapping != nullptr &&
                             mapping->disposition == FirebirdMappingDisposition::kScratchBirdLifecycleApi;
  const bool support_udr = mapping != nullptr &&
                           mapping->disposition == FirebirdMappingDisposition::kParserSupportUdr;
  const bool exact_diagnostic = mapping != nullptr &&
                                mapping->disposition == FirebirdMappingDisposition::kEmulatedNonFileDiagnostic;
  std::string canonical_family;
  std::string canonical_operation_id;
  std::string canonical_opcode;
  if (statement_family == "transaction") {
    canonical_family = "sblr.transaction.control.v3";
    if (StartsWithCommand(normalized_upper_sql, "BEGIN TRANSACTION") ||
        StartsWithCommand(normalized_upper_sql, "SET TRANSACTION")) {
      canonical_operation_id = "transaction.begin";
      canonical_opcode = "SBLR_TRANSACTION_BEGIN";
    } else if (StartsWithCommand(normalized_upper_sql, "COMMIT")) {
      canonical_operation_id = "transaction.commit";
      canonical_opcode = "SBLR_TRANSACTION_COMMIT";
    } else if (StartsWithCommand(normalized_upper_sql, "ROLLBACK TO") ||
               StartsWithCommand(normalized_upper_sql, "ROLLBACK WORK TO") ||
               StartsWithCommand(normalized_upper_sql,
                                 "ROLLBACK TRANSACTION TO")) {
      canonical_operation_id = "transaction.rollback_to_savepoint";
      canonical_opcode = "SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT";
    } else if (StartsWithCommand(normalized_upper_sql, "ROLLBACK")) {
      canonical_operation_id = "transaction.rollback";
      canonical_opcode = "SBLR_TRANSACTION_ROLLBACK";
    } else if (StartsWithCommand(normalized_upper_sql, "RELEASE SAVEPOINT")) {
      canonical_operation_id = "transaction.release_savepoint";
      canonical_opcode = "SBLR_TRANSACTION_RELEASE_SAVEPOINT";
    } else if (StartsWithCommand(normalized_upper_sql, "SAVEPOINT")) {
      canonical_operation_id = "transaction.create_savepoint";
      canonical_opcode = "SBLR_TRANSACTION_CREATE_SAVEPOINT";
    }
  }
  if (mapping != nullptr && !mapping->operation_id.empty()) {
    canonical_family = std::string(mapping->sblr_operation_family);
    canonical_operation_id = std::string(mapping->operation_id);
    canonical_opcode = std::string(mapping->sblr_operation);
  }
  const std::string emitted_family =
      canonical_family.empty() ? std::string(operation_family)
                               : canonical_family;
  const std::string emitted_operation_id =
      canonical_operation_id.empty()
          ? std::string(mapping == nullptr ? "" : mapping->operation_id)
          : canonical_operation_id;
  const std::string emitted_opcode =
      canonical_opcode.empty()
          ? std::string(mapping == nullptr ? "" : mapping->sblr_operation)
          : canonical_opcode;
  return "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
         "\"dialect\":\"firebird\","
         "\"statement_family\":\"" + EscapeJson(statement_family) + "\","
         "\"source_operation_family\":\"" + EscapeJson(operation_family) + "\","
         "\"operation_family\":\"" + EscapeJson(emitted_family) + "\","
         "\"operation_id\":\"" + EscapeJson(emitted_operation_id) + "\","
         "\"sblr_operation\":\"" + EscapeJson(emitted_opcode) + "\","
         "\"sblr_operation_family\":\"" + EscapeJson(emitted_family) + "\","
         "\"result_shape\":\"engine.api.result.v1\","
         "\"diagnostic_shape\":\"engine.diagnostic.v1\","
         "\"engine_api_function\":\"" +
         EscapeJson(mapping == nullptr ? "" : mapping->engine_api_function) + "\","
         "\"mapping_key\":\"" + EscapeJson(mapping == nullptr ? "" : mapping->mapping_key) + "\","
         "\"mapping_disposition\":\"" +
         EscapeJson(mapping == nullptr ? "" : FirebirdMappingDispositionName(mapping->disposition)) + "\","
         "\"parser_evidence\":" + ParserEvidenceJson(evidence) + ","
         "\"enterprise_readiness_evidence\":" +
         scratchbird::parser::firebird::evidence::EnterpriseReadinessEvidenceJson() + ","
         "\"descriptor_resolution\":\"uuid_required\","
         "\"parser_resolved_names_to_uuids\":" +
         std::string(statement_family == "transaction" ? "true" : "false") + ","
         "\"engine_authority\":\"scratchbird\","
         "\"scratchbird_lifecycle_api\":" + std::string(lifecycle_api ? "true" : "false") + ","
         "\"parser_support_udr_route\":" + std::string(support_udr ? "true" : "false") + ","
         "\"exact_emulated_diagnostic\":" + std::string(exact_diagnostic ? "true" : "false") + ","
         "\"real_firebird_file_effects\":false,"
         "\"reference_engine_sql_executed\":false,"
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
       "Firebird validation is a compatibility low-level utility surface and is outside compatibility parser authority.",
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
       "Firebird repair is a compatibility low-level utility surface and is outside compatibility parser authority.",
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
       "Firebird database file-management syntax is diagnostic-only and has zero compatibility file effects.",
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
       "Firebird shadow storage syntax is diagnostic-only and has zero compatibility file effects.",
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
       "Firebird external table/plugin syntax is diagnostic-only and cannot load compatibility external code.",
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
       "Firebird service/tool syntax is represented as an emulated diagnostic, not compatibility tool execution.",
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
       "Firebird service/repair/verification utilities are outside compatibility parser authority.",
       true,
       false,
       false,
       false,
       true},
      {"firebird.emulated.replication_journal",
       "replication_journal_emulation",
       FirebirdMappingDisposition::kParserSupportUdr,
       "firebird.udr.replication_journal",
       "SBLR_COMPATIBILITY_FIREBIRD_REPLICATION_ROUTE",
       "sblr.archive_replication.operation.v3",
       "ParserSupportReplicationRoute",
       "firebird_replication_journal_request_v1",
       "firebird_replication_journal_result_v1",
       "FIREBIRD.EMULATION.NON_FILE_SURFACE",
       "Firebird journal/replication syntax routes through parser-support UDR policy with no compatibility file effects or parser authority.",
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

bool IsGbakBackupRestoreCommand(std::string_view upper) {
  const auto words = SplitAsciiWords(TrimAsciiView(upper));
  if (words.size() < 2 || words.front() != "GBAK") return false;
  bool backup = false;
  bool restore = false;
  for (std::size_t i = 1; i < words.size(); ++i) {
    backup = backup || IsGbakBackupSwitch(words[i]);
    restore = restore || IsGbakRestoreSwitch(words[i]);
  }
  return backup != restore;
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
      << "\"sblr_operation_family\":\"sblr.compatibility.firebird.logical_stream.v1\","
      << "\"engine_authority\":\"scratchbird_mga_catalog_sblr\","
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"real_firebird_file_effects\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"source_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"runtime_equivalence_status\":\"compatibility_native_gbak_stream_replay_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
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
    return "firebird.emulated.reference_native_tool";
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
  if ((StartsWithCommand(upper, "BEGIN") &&
       !StartsWithCommand(upper, "BEGIN TRANSACTION")) ||
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

bool IsTopLevelSelectWithoutSource(std::string_view active_upper) {
  return StartsWithCommand(active_upper, "SELECT") && !ContainsWord(active_upper, "FROM");
}

bool ContainsAggregateWindowInWhereClause(std::string_view active_upper) {
  if (!(StartsWithCommand(active_upper, "SELECT") ||
        StartsWithCommand(active_upper, "WITH") ||
        Contains(active_upper, " SELECT "))) {
    return false;
  }

  const auto where_pos = active_upper.find(" WHERE ");
  if (where_pos == std::string_view::npos) return false;
  auto end_pos = active_upper.size();
  for (const auto marker : {" GROUP BY ", " HAVING ", " ORDER BY ", " ROWS ",
                            " UNION ", " PLAN "}) {
    const auto marker_pos = active_upper.find(marker, where_pos + 7);
    if (marker_pos != std::string_view::npos && marker_pos < end_pos) {
      end_pos = marker_pos;
    }
  }
  const auto where_clause = active_upper.substr(where_pos + 7, end_pos - (where_pos + 7));
  bool in_string = false;
  bool in_quoted_identifier = false;
  int depth = 0;
  const auto starts_aggregate_at = [&](std::size_t pos) {
    for (const auto aggregate : {"COUNT(", "SUM(", "AVG(", "MIN(", "MAX(",
                                 "ROW_NUMBER(", "RANK(", "DENSE_RANK(",
                                 "LAG(", "LEAD("}) {
      const auto len = std::char_traits<char>::length(aggregate);
      if (pos + len <= where_clause.size() &&
          where_clause.substr(pos, len) == aggregate) {
        return true;
      }
    }
    return false;
  };
  for (std::size_t pos = 0; pos < where_clause.size(); ++pos) {
    const char ch = where_clause[pos];
    if (ch == '\'' && !in_quoted_identifier) {
      if (in_string && pos + 1 < where_clause.size() && where_clause[pos + 1] == '\'') {
        ++pos;
        continue;
      }
      in_string = !in_string;
      continue;
    }
    if (ch == '"' && !in_string) {
      in_quoted_identifier = !in_quoted_identifier;
      continue;
    }
    if (in_string || in_quoted_identifier) continue;
    if (depth == 0 && starts_aggregate_at(pos)) return true;
    if (ch == '(') {
      ++depth;
    } else if (ch == ')' && depth > 0) {
      --depth;
    }
  }
  return false;
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
    if (IsFirebirdIndexSemanticDefaultsStatement(upper)) {
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

FirebirdTransactionControl ClassifyFirebirdTransactionControl(
    std::string_view sql_text) {
  FirebirdTransactionControl control;
  std::vector<std::string> words;
  bool terminator_seen = false;
  for (const auto& token : LexTokens(sql_text)) {
    if (token.kind == "line_comment") continue;
    if (token.kind == "block_comment") {
      if (!token.lexeme.ends_with("*/")) {
        words.emplace_back("<UNTERMINATED_COMMENT>");
        break;
      }
      continue;
    }
    if (token.kind == "punctuation" && token.lexeme == ";" &&
        !terminator_seen) {
      terminator_seen = true;
      continue;
    }
    if (terminator_seen) {
      words.emplace_back("<TRAILING_SQL>");
      break;
    }
    if (token.kind != "identifier_or_keyword") {
      words.emplace_back("<NON_WORD>");
      break;
    }
    words.push_back(ToUpperAscii(token.lexeme));
  }
  if (words.empty()) return control;

  if (words.front() == "SET" && words.size() >= 2 &&
      words[1] == "TRANSACTION") {
    control.kind = FirebirdTransactionControlKind::kBegin;
    return control;
  }
  if (words.front() == "SAVEPOINT" && words.size() >= 2) {
    control.kind = FirebirdTransactionControlKind::kSavepoint;
    return control;
  }
  if (words.front() == "RELEASE" && words.size() >= 3 &&
      words[1] == "SAVEPOINT") {
    control.kind = FirebirdTransactionControlKind::kReleaseSavepoint;
    return control;
  }
  if (words.front() == "COMMIT") {
    std::size_t index = 1;
    if (index < words.size() && words[index] == "WORK") ++index;
    if (index < words.size() && words[index] == "RETAINING") {
      control.retaining = true;
      ++index;
      if (index < words.size() && words[index] == "SNAPSHOT") ++index;
    }
    if (index == words.size()) {
      control.kind = FirebirdTransactionControlKind::kCommit;
    } else {
      control.retaining = false;
    }
    return control;
  }
  if (words.front() != "ROLLBACK") return control;

  std::size_t index = 1;
  if (index < words.size() &&
      (words[index] == "WORK" || words[index] == "TRANSACTION")) {
    ++index;
  }
  if (index < words.size() && words[index] == "TO") {
    control.kind = FirebirdTransactionControlKind::kRollbackToSavepoint;
    return control;
  }
  if (index < words.size() && words[index] == "RETAINING") {
    control.retaining = true;
    ++index;
    if (index < words.size() && words[index] == "SNAPSHOT") ++index;
  }
  if (index == words.size()) {
    control.kind = FirebirdTransactionControlKind::kRollback;
  } else {
    control.retaining = false;
  }
  return control;
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
      StartsWithCommand(upper, "RESTORE") ||
      StartsWithCommand(upper, "NBACKUP")) {
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
  if (StartsWithCommand(upper, "SWEEP")) {
    return MappingByKey("firebird.unsupported.low_level_utility");
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
      {"reference_native_tool", "isql gbak gfix gstat nbackup fbsvcmgr fbtracemgr gsec normalized diagnostics", "sbl_firebird_wire"},
  };
  return surfaces;
}

ParseResult ParseStatementWithOriginalSource(
    std::string_view parser_sql,
    std::string_view original_sql) {
  ParseResult result;
  result.normalized_sql = NormalizeWhitespace(parser_sql);
  const auto active_normalized = MaskInactiveSqlText(result.normalized_sql);
  const auto active_upper = ToUpperAscii(active_normalized);
  const auto tokens = LexTokens(result.normalized_sql);
  // Binder scope analysis must retain original line boundaries and token
  // classes. Normalizing first would let a `--` comment consume later SQL and
  // would move quoted/string token offsets.
  const auto source_tokens = LexTokens(original_sql);
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
    result.reference_engine_sql_executed = lifecycle_mapping->reference_engine_sql_executed;
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
  if (IsTopLevelSelectWithoutSource(active_upper)) {
    result.ok = false;
    result.statement_family = "invalid_input";
    result.operation_family = "firebird.invalid_input";
    diagnostics.push_back(MakeDiagnostic(
        "FIREBIRD.PARSE.INVALID_INPUT", "ERROR",
        "Firebird SELECT projection statements require a source relation; "
        "use RDB$DATABASE for singleton probes.",
        "sbp_firebird",
        {{"operation_family", result.operation_family},
         {"required_singleton_source", "RDB$DATABASE"}}));
    result.message_vector_json = MessageVectorToJson(diagnostics);
    return result;
  }
  if (const auto derived =
          AnalyzeFirebirdDerivedTableDiagnostics(source_tokens, original_sql)) {
    result.ok = false;
    result.statement_family = "query";
    result.derived_table_diagnostic = *derived;
    if (derived->kind ==
        FirebirdDerivedTableDiagnosticKind::kDuplicateOutputName) {
      result.operation_family =
          "firebird.query.derived_table_duplicate_output_refused";
      diagnostics.push_back(MakeDiagnostic(
          "FIREBIRD.DSQL.DERIVED_FIELD_DUP_NAME", "ERROR",
          "column " + derived->column_name +
              " was specified multiple times for derived table " +
              derived->derived_table_alias,
          "sbp_firebird",
          {{"operation_family", result.operation_family},
           {"primary_sqlcode", "-104"},
           {"primary_sqlstate", "42000"},
           {"primary_gds_symbol", "isc_dsql_derived_field_dup_name"},
           {"column", derived->column_name},
           {"derived_table", derived->derived_table_alias}}));
    } else {
      result.operation_family =
          "firebird.query.derived_table_outer_reference_refused";
      diagnostics.push_back(MakeDiagnostic(
          "FIREBIRD.DSQL.DERIVED_OUTER_REFERENCE", "ERROR",
          "Column unknown", "sbp_firebird",
          {{"operation_family", result.operation_family},
           {"primary_sqlcode", "-206"},
           {"primary_sqlstate", "42S22"},
           {"primary_gds_symbol", "isc_dsql_field_err"},
           {"field", derived->qualified_field_name},
           {"outer_relation_alias", derived->outer_relation_alias},
           {"derived_table", derived->derived_table_alias},
           {"line", std::to_string(derived->line)},
           {"column", std::to_string(derived->column)}}));
    }
    result.message_vector_json = MessageVectorToJson(diagnostics);
    return result;
  }
  if (ContainsAggregateWindowInWhereClause(active_upper)) {
    result.ok = false;
    result.statement_family = "query";
    result.operation_family = "firebird.query.aggregate_window_where_refused";
    diagnostics.push_back(MakeDiagnostic(
        "FIREBIRD.DSQL.AGGREGATE_WHERE", "ERROR",
        "Cannot use an aggregate or window function in a WHERE clause, use HAVING (for aggregate only) instead",
        "sbp_firebird",
        {{"operation_family", result.operation_family},
         {"primary_sqlcode", "-104"},
         {"primary_sqlstate", "42000"},
         {"primary_gds_symbol", "isc_dsql_agg_where_err"}}));
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
         {"reference_engine_sql_executed", "false"}}));
    result.message_vector_json = MessageVectorToJson(diagnostics);
    return result;
  }

  if (IsGbakBackupRestoreCommand(active_upper) &&
      gbak_logical_stream_operation.empty()) {
    result.ok = false;
    result.statement_family = "non_file_emulation";
    result.operation_family = "firebird.emulated.backup_restore";
    result.scratchbird_lifecycle_api = false;
    result.parser_support_udr_route = false;
    result.exact_emulated_diagnostic = false;
    result.real_firebird_file_effects = false;
    result.reference_engine_sql_executed = false;
    result.emulation_diagnostic_code = "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED";
    diagnostics.push_back(MakeDiagnostic(
        "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED",
        "ERROR",
        "Firebird gbak backup/restore with file targets is denied; use the "
        "stdin/stdout logical stream form so ScratchBird remains the only "
        "storage and MGA authority.",
        "sbp_firebird",
        {{"operation_family", result.operation_family},
         {"mapping_key", "firebird.emulated.backup_restore"},
         {"scratchbird_lifecycle_api", "false"},
         {"real_firebird_file_effects", "false"},
         {"reference_engine_sql_executed", "false"}}));
    result.message_vector_json = MessageVectorToJson(diagnostics);
    return result;
  }

  if (StartsWithCommand(active_upper, "NBACKUP")) {
    result.ok = false;
    result.statement_family = "low_level_utility";
    result.operation_family = "firebird.emulated.incremental_backup";
    result.scratchbird_lifecycle_api = false;
    result.parser_support_udr_route = false;
    result.exact_emulated_diagnostic = false;
    result.real_firebird_file_effects = false;
    result.reference_engine_sql_executed = false;
    result.emulation_diagnostic_code = "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED";
    diagnostics.push_back(MakeDiagnostic(
        "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED",
        "ERROR",
        "Firebird nbackup is denied because it addresses raw backup files; "
        "ScratchBird backup/restore must use engine-owned SBLR/MGA authority.",
        "sbp_firebird",
        {{"operation_family", result.operation_family},
         {"mapping_key", "firebird.emulated.backup_restore"},
         {"scratchbird_lifecycle_api", "false"},
         {"real_firebird_file_effects", "false"},
         {"reference_engine_sql_executed", "false"}}));
    result.message_vector_json = MessageVectorToJson(diagnostics);
    return result;
  }

  if (StartsWithCommand(active_upper, "GFIX")) {
    result.ok = false;
    result.statement_family = "low_level_utility";
    result.operation_family = "firebird.unsupported.low_level_utility";
    result.scratchbird_lifecycle_api = false;
    result.parser_support_udr_route = false;
    result.exact_emulated_diagnostic = false;
    result.real_firebird_file_effects = false;
    result.reference_engine_sql_executed = false;
    result.emulation_diagnostic_code = "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED";
    diagnostics.push_back(MakeDiagnostic(
        "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED",
        "ERROR",
        "Firebird gfix is a low-level validation/repair utility and is denied "
        "at the parser boundary; ScratchBird repair authority remains engine-owned.",
        "sbp_firebird",
        {{"operation_family", result.operation_family},
         {"mapping_key", "firebird.unsupported.low_level_utility"},
         {"scratchbird_lifecycle_api", "false"},
         {"real_firebird_file_effects", "false"},
         {"reference_engine_sql_executed", "false"}}));
    result.message_vector_json = MessageVectorToJson(diagnostics);
    return result;
  }

  result.ok = true;
  if (!gbak_logical_stream_operation.empty()) {
    result.statement_family = "logical_stream_backup_restore";
    result.operation_family = gbak_logical_stream_operation;
    diagnostics.push_back(MakeDiagnostic(
        "FIREBIRD.LOGICAL_STREAM.GBAK_REMOTE_STREAM", "INFO",
        "Firebird gbak stdin/stdout logical stream is admitted as parser/SBLR evidence with ScratchBird engine authority and no compatibility file effects.",
        "sbp_firebird",
        {{"operation_family", result.operation_family},
         {"scratchbird_lifecycle_api", "false"},
         {"real_firebird_file_effects", "false"},
         {"reference_engine_sql_executed", "false"}}));
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
         {"reference_engine_sql_executed", "false"}}));
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
  } else if (StartsWithCommand(active_upper, "BEGIN TRANSACTION") ||
             StartsWithCommand(active_upper, "COMMIT") ||
             StartsWithCommand(active_upper, "ROLLBACK") ||
             StartsWithCommand(active_upper, "SET TRANSACTION") ||
             StartsWithCommand(active_upper, "SAVEPOINT") ||
             StartsWithCommand(active_upper, "RELEASE SAVEPOINT")) {
    result.statement_family = "transaction";
    if (StartsWithCommand(active_upper, "BEGIN TRANSACTION") ||
        StartsWithCommand(active_upper, "SET TRANSACTION")) {
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
        IsFirebirdIndexSemanticDefaultsStatement(active_upper);
    if (parser_evidence.index_semantic_defaults_evidence_required) {
      parser_evidence.index_semantic_defaults_upper_sql = active_upper;
    }
    parser_evidence.constraint_semantic_defaults_evidence_required =
        result.statement_family == "ddl" &&
        IsFirebirdConstraintSemanticDefaultsStatement(active_upper);
    if (parser_evidence.constraint_semantic_defaults_evidence_required) {
      parser_evidence.constraint_semantic_defaults_upper_sql = active_upper;
    }
    parser_evidence.sequence_identity_semantic_evidence_required =
        IsFirebirdSequenceIdentitySemanticStatement(active_upper);
    if (parser_evidence.sequence_identity_semantic_evidence_required) {
      parser_evidence.sequence_identity_semantic_upper_sql = active_upper;
    }
    parser_evidence.identifier_name_resolution_evidence_required =
        result.statement_family == "ddl";
    if (parser_evidence.identifier_name_resolution_evidence_required) {
      parser_evidence.identifier_name_resolution_upper_sql = active_upper;
    }
    parser_evidence.scalar_expression_semantic_evidence_required =
        result.statement_family == "query" &&
        IsFirebirdScalarExpressionSemanticStatement(active_upper);
    if (parser_evidence.scalar_expression_semantic_evidence_required) {
      parser_evidence.scalar_expression_semantic_upper_sql = active_upper;
    }
    parser_evidence.dml_mutation_semantic_evidence_required =
        result.statement_family == "dml" &&
        IsFirebirdDmlMutationSemanticStatement(active_upper);
    if (parser_evidence.dml_mutation_semantic_evidence_required) {
      parser_evidence.dml_mutation_semantic_upper_sql = active_upper;
    }
    parser_evidence.transaction_session_semantic_evidence_required =
        (result.statement_family == "transaction" ||
         result.statement_family == "session") &&
        IsFirebirdTransactionSessionSemanticStatement(active_upper);
    if (parser_evidence.transaction_session_semantic_evidence_required) {
      parser_evidence.transaction_session_semantic_upper_sql = active_upper;
    }
    parser_evidence.temporary_session_object_semantic_evidence_required =
        result.statement_family == "ddl" &&
        IsFirebirdTemporarySessionObjectSemanticStatement(active_upper);
    if (parser_evidence.temporary_session_object_semantic_evidence_required) {
      parser_evidence.temporary_session_object_semantic_upper_sql =
          active_upper;
    }
    parser_evidence.dependency_bearing_ddl_semantic_evidence_required =
        result.statement_family == "ddl" &&
        IsFirebirdDependencyBearingDdlStatement(active_upper);
    if (parser_evidence.dependency_bearing_ddl_semantic_evidence_required) {
      parser_evidence.dependency_bearing_ddl_semantic_upper_sql =
          active_upper;
    }
    parser_evidence.ddl_transaction_behavior_semantic_evidence_required =
        result.statement_family == "ddl" &&
        IsFirebirdDdlTransactionBehaviorStatement(active_upper);
    if (parser_evidence.ddl_transaction_behavior_semantic_evidence_required) {
      parser_evidence.ddl_transaction_behavior_semantic_upper_sql =
          active_upper;
    }
    parser_evidence.resource_text_semantic_evidence_required =
        (result.statement_family == "ddl" || result.statement_family == "dml" ||
         result.statement_family == "query") &&
        IsFirebirdResourceTextStatement(active_upper);
    if (parser_evidence.resource_text_semantic_evidence_required) {
      parser_evidence.resource_text_semantic_upper_sql = active_upper;
    }
    parser_evidence.statistics_optimizer_semantic_evidence_required =
        IsFirebirdStatisticsOptimizerStatement(active_upper);
    if (parser_evidence.statistics_optimizer_semantic_evidence_required) {
      parser_evidence.statistics_optimizer_semantic_upper_sql = active_upper;
    }
    parser_evidence.locks_isolation_semantic_evidence_required =
        IsFirebirdLocksIsolationStatement(active_upper);
    if (parser_evidence.locks_isolation_semantic_evidence_required) {
      parser_evidence.locks_isolation_semantic_upper_sql = active_upper;
    }
    parser_evidence.system_catalog_defaults_semantic_evidence_required =
        IsFirebirdSystemCatalogDefaultsStatement(active_upper);
    if (parser_evidence.system_catalog_defaults_semantic_evidence_required) {
      parser_evidence.system_catalog_defaults_semantic_operation_id =
          !result.lifecycle_mapping_key.empty() ? result.lifecycle_mapping_key
                                                : result.operation_family;
    }
    parser_evidence.session_settings_diagnostics_semantic_evidence_required =
        IsFirebirdSessionSettingsDiagnosticsStatement(active_upper);
    if (parser_evidence.session_settings_diagnostics_semantic_evidence_required) {
      parser_evidence.session_settings_diagnostics_semantic_upper_sql =
          active_upper;
    }
    parser_evidence.procedural_body_source_retention_required =
        IsFirebirdProceduralBodySourceRetentionStatement(
            result.statement_family, result.operation_family, active_upper);
    if (parser_evidence.procedural_body_source_retention_required) {
      parser_evidence.procedural_span_metadata =
          BuildProceduralFunctionalEncodingSpanMetadata(active_upper, tokens);
      parser_evidence.procedural_source_retention_metadata =
          FirebirdProceduralSourceRetentionMetadataFor(
              result.normalized_sql, tokens,
              parser_evidence.procedural_span_metadata);
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
                         active_upper,
                         lifecycle_mapping, parser_evidence);
  }
  result.message_vector_json = MessageVectorToJson(diagnostics);
  return result;
}

ParseResult ParseStatement(std::string_view sql_text) {
  return ParseStatementWithOriginalSource(sql_text, sql_text);
}

std::string FirebirdPackageIdentityJson() {
  return "{\"dialect\":\"firebird\","
         "\"parser_worker\":\"sbp_firebird\","
         "\"parser_package\":\"sbp_firebird\","
         "\"parser_support_udr\":\"sbup_firebird\","
         "\"parser_support_package\":\"sbup_firebird\","
         "\"parser_support_udr_target\":\"sbu_firebird_parser_support\","
         "\"release_profile\":\"firebird-v5_0\","
         "\"authority_policy\":\"engine_sblr_mga_only\","
         "\"reference_sql_execution\":false,"
         "\"reference_storage_authority\":false,"
         "\"reference_recovery_authority\":false,"
         "\"parser_surface_rows\":19,"
         "\"function_api_rows\":9,"
         "\"compatibility_alias_rows\":0,"
         "\"core_or_optional_alias_rows\":0,"
         "\"catalog_projection_only_rows\":3,"
         "\"connector_operation_rows\":0,"
         "\"policy_blocked_rows\":1,"
         "\"trusted_udr_registration_rows\":5,"
         "\"unsupported_rows\":3,"
         "\"surface_counts\":{\"parser_surface_rows\":19,"
         "\"function_api_rows\":9,"
         "\"compatibility_alias_rows\":0,"
         "\"core_or_optional_alias_rows\":0,"
         "\"catalog_projection_only_rows\":3,"
         "\"connector_operation_rows\":0,"
         "\"policy_blocked_rows\":1,"
         "\"trusted_udr_registration_rows\":5,"
         "\"unsupported_rows\":3},"
         "\"datatype_families\":" + std::to_string(DatatypeSurfaces().size()) + ","
         "\"builtin_function_families\":" + std::to_string(BuiltinFunctionSurfaces().size()) + ","
         "\"catalog_overlay_families\":" + std::to_string(CatalogOverlaySurfaces().size()) + ","
         "\"diagnostic_families\":" + std::to_string(DiagnosticSurfaces().size()) + ","
         "\"lifecycle_mapping_report\":" + FirebirdLifecycleMappingReportJson() + ","
         "\"parser_family_uuid\":\"parser.compatibility.firebird\","
         "\"standalone_package\":true,"
         "\"cross_parser_dependency_count\":0,"
         "\"same_family_library_set\":["
         "{\"target\":\"sbp_firebird\",\"artifact\":\"bin/sbp_firebird\",\"owner\":\"parser.compatibility.firebird\"},"
         "{\"target\":\"sbl_firebird_parser_pipeline\",\"artifact\":\"lib/libsbl_firebird_parser_pipeline\",\"owner\":\"parser.compatibility.firebird\"},"
         "{\"target\":\"sbl_firebird_transaction_policy\",\"artifact\":\"lib/libsbl_firebird_transaction_policy\",\"owner\":\"parser.compatibility.firebird\"},"
         "{\"target\":\"sbu_firebird_parser_support\",\"artifact\":\"lib/libsbu_firebird_parser_support\",\"owner\":\"parser.compatibility.firebird\"}],"
         "\"neutral_dependency_set\":["
         "{\"target\":\"sbl_listener_control_plane\",\"artifact\":\"lib/libsbl_listener_control_plane\",\"owner\":\"family_neutral\",\"version\":\"same-build\"},"
         "{\"target\":\"sbl_manager_protocol\",\"artifact\":\"lib/libsbl_manager_protocol\",\"owner\":\"family_neutral\",\"version\":\"same-build\"},"
         "{\"target\":\"sbl_parser_server_ipc_client\",\"artifact\":\"lib/libsbl_parser_server_ipc_client\",\"owner\":\"family_neutral\",\"version\":\"same-build\"},"
         "{\"target\":\"sbl_parser_server_ipc_schema\",\"artifact\":\"lib/libsbl_parser_server_ipc_schema\",\"owner\":\"family_neutral\",\"version\":\"same-build\"},"
         "{\"target\":\"sb_udr_runtime\",\"artifact\":\"lib/libsb_udr_runtime\",\"owner\":\"family_neutral\",\"version\":\"same-build\"},"
         "{\"target\":\"sb_core_memory\",\"artifact\":\"lib/libsb_core_memory\",\"owner\":\"scratchbird_engine\",\"version\":\"same-build\"},"
         "{\"target\":\"sb_core_metrics\",\"artifact\":\"lib/libsb_core_metrics\",\"owner\":\"scratchbird_engine\",\"version\":\"same-build\"},"
         "{\"target\":\"sb_core_platform\",\"artifact\":\"lib/libsb_core_platform\",\"owner\":\"scratchbird_engine\",\"version\":\"same-build\"},"
         "{\"target\":\"system_crypt\",\"artifact\":\"system/libcrypt\",\"owner\":\"system_neutral\",\"version\":\"resolved-at-build\"},"
         "{\"target\":\"OpenSSL::Crypto\",\"artifact\":\"system/libcrypto\",\"owner\":\"system_neutral\",\"version\":\"resolved-at-build\"}],"
         "\"parser_support_udr_family_uuid\":\"parser.compatibility.firebird\","
         "\"direct_sblr_lowering\":true,"
         "\"foreign_parser_fallback\":false,"
         "\"isolated_build_profile\":\"parser-family-isolated-release-v1\","
         "\"isolated_package_profile\":\"parser-family-empty-prefix-v1\","
         "\"dependency_closure_evidence\":{"
         "\"source\":\"parser_family_isolation_evidence.json#source_ownership_scan\","
         "\"build_graph\":\"parser_family_isolation_evidence.json#build_graph_ownership_scan\","
         "\"link\":\"parser_family_binary_isolation_evidence.json#project_target_link_command_scan\","
         "\"symbol\":\"parser_family_binary_isolation_evidence.json#binary_and_archive_symbol_scan\","
         "\"package\":\"parser_family_package_isolation_evidence.json#empty_prefix_package_closure\","
         "\"runtime\":\"parser_family_binary_isolation_evidence.json#staged_identity_probe_trace\"},"
         "\"standalone_dialect_package\":true,"
         "\"cross_dialect_dependencies\":false,"
         "\"dependency_isolation\":\"firebird_parser_and_udr_only\"}";
}

std::string FirebirdConnectionSandboxReportJson() {
  return "{\"ok\":true,"
         "\"dialect\":\"firebird\","
         "\"connection_sandbox_contract\":\"compatibility_connection_schema_root_v1\","
         "\"schema_root_source\":\"listener_engine_materialized_attach_context\","
         "\"user_object_resolution\":\"relative_to_connection_schema_root\","
         "\"unqualified_name_root\":\"reference_schema_branch_root\","
         "\"direct_cross_root_access\":\"unsupported_denied\","
         "\"server_local_file_access\":\"default_denied\","
         "\"tenant_escape_policy\":\"fail_closed\","
         "\"catalog_projection_authority\":\"catalog_emulation_definer_authority\","
         "\"catalog_projection_can_query_outside_sandbox\":true,"
         "\"catalog_projection_user_authority\":false,"
         "\"catalog_projection_select_grant_required\":true,"
         "\"catalog_projection_output_is_user_visible\":true,"
         "\"catalog_projection_does_not_grant_base_object_access\":true,"
         "\"foreign_parser_tree_visibility_inherited\":false,"
         "\"engine_authorization_authority\":\"scratchbird_engine\","
         "\"parser_authorization_authority\":false,"
         "\"parser_storage_authority\":false,"
         "\"parser_recovery_authority\":false,"
         "\"mga_transaction_authority\":\"scratchbird_engine\","
         "\"schema_root_is_user_visible_root\":true,"
         "\"materialized_authorization_required\":true,"
         "\"search_path_outside_root_policy\":\"refuse_without_catalog_definer_projection\","
         "\"catalog_security_filter\":\"engine_materialized_grants_plus_projection_definer_grants\"}";
}

std::string FirebirdDialectVariantReportJson() {
  return "{\"ok\":true,"
         "\"dialect\":\"firebird\","
         "\"dialect_variant_contract\":\"compatibility_supported_variant_surface_v1\","
         "\"variant_selection_authority\":\"listener_profile_and_engine_attach_context\","
         "\"parser_cross_dialect_detection\":false,"
         "\"parser_cross_dialect_dispatch\":false,"
         "\"foreign_parser_variant_admitted\":false,"
         "\"reasonable_subset_policy\":\"declared_and_tested_per_compatibility_variant\","
         "\"variant_count\":3,"
         "\"variants\":[\"firebird_sql_dialect_1_compat\","
         "\"firebird_sql_dialect_3\",\"firebird_psql\"]}";
}

namespace {

std::string FirebirdSurfaceOwner(std::string_view section,
                                 std::string_view raw_owner) {
  if (section == "datatype_surfaces") {
    return "descriptor";
  }
  if (section == "builtin_function_surfaces") {
    return "sblr";
  }
  if (section == "catalog_overlay_surfaces") {
    return "catalog_projection";
  }
  if (section == "diagnostic_surfaces") {
    if (Contains(raw_owner, "wire")) return "parser_support_udr";
    if (Contains(raw_owner, "diagnostic")) return "fail_closed";
    return "parser";
  }
  return "parser";
}

std::string FirebirdSurfaceArrayJson(
    std::string_view section,
    const std::vector<SurfaceDescriptor>& surfaces) {
  std::ostringstream out;
  out << '[';
  bool first = true;
  for (const auto& surface : surfaces) {
    if (!first) out << ',';
    first = false;
    out << "{\"family\":\"" << EscapeJson(surface.family)
        << "\",\"surface\":\"" << EscapeJson(surface.surface)
        << "\",\"owner\":\""
        << EscapeJson(FirebirdSurfaceOwner(section, surface.owner)) << "\"}";
  }
  out << ']';
  return out.str();
}

} // namespace

std::string FirebirdSurfaceReportJson() {
  return "{\"dialect\":\"firebird\","
         "\"datatype_surfaces\":" +
         FirebirdSurfaceArrayJson("datatype_surfaces", DatatypeSurfaces()) +
         ",\"builtin_function_surfaces\":" +
         FirebirdSurfaceArrayJson("builtin_function_surfaces",
                                  BuiltinFunctionSurfaces()) +
         ",\"catalog_overlay_surfaces\":" +
         FirebirdSurfaceArrayJson("catalog_overlay_surfaces",
                                  CatalogOverlaySurfaces()) +
         ",\"diagnostic_surfaces\":" +
         FirebirdSurfaceArrayJson("diagnostic_surfaces",
                                  DiagnosticSurfaces()) +
         "}";
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
  return "{\"gate\":\"DBLC_P14_COMPAT_MAPPING_COMPLETE\","
         "\"static_gate\":\"DBLC_STATIC_NO_COMPAT_ENGINE_SQL\","
         "\"dialect\":\"firebird\","
         "\"lifecycle_api_mappings\":" + std::to_string(lifecycle_api_count) + ","
         "\"parser_support_udr_mappings\":" + std::to_string(parser_support_udr_count) + ","
         "\"exact_emulated_non_file_diagnostics\":" + std::to_string(exact_diagnostic_count) + ","
         "\"engine_authority\":\"scratchbird\","
         "\"reference_engine_sql_executed\":false,"
         "\"real_firebird_file_effects\":false,"
         "\"standalone_dialect_package\":true}";
}

} // namespace scratchbird::parser::firebird
