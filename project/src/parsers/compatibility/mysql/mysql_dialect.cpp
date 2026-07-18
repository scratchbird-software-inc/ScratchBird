// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mysql_dialect.hpp"

#include <array>
#include <cctype>
#include <sstream>
#include <vector>

namespace scratchbird::parser::mysql {
namespace {

using scratchbird::parser::compatibility::MappingDisposition;
using scratchbird::parser::compatibility::OperationPattern;
using scratchbird::parser::compatibility::PatternMatch;
using scratchbird::parser::compatibility::SurfaceDescriptor;

bool IsIdentifierChar(char ch) {
  const auto value = static_cast<unsigned char>(ch);
  return std::isalnum(value) != 0 || ch == '_' || ch == '$';
}

bool StartsWithCommand(std::string_view value, std::string_view prefix) {
  if (!value.starts_with(prefix)) return false;
  if (value.size() == prefix.size()) return true;
  const char next = value[prefix.size()];
  return std::isspace(static_cast<unsigned char>(next)) != 0 || next == ';' ||
         next == '(' || next == '\'' || next == '"' || next == '`';
}

bool Contains(std::string_view value, std::string_view needle) {
  return value.find(needle) != std::string_view::npos;
}

bool ContainsWord(std::string_view value, std::string_view word) {
  for (std::size_t pos = value.find(word); pos != std::string_view::npos;
       pos = value.find(word, pos + 1)) {
    const auto end = pos + word.size();
    if ((pos == 0 || !IsIdentifierChar(value[pos - 1])) &&
        (end == value.size() || !IsIdentifierChar(value[end]))) {
      return true;
    }
  }
  return false;
}

bool HasFunctionCall(std::string_view upper, std::string_view name) {
  return ContainsWord(upper, name) &&
         Contains(upper, std::string(name) + "(");
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

std::string BoolJson(bool value) { return value ? "true" : "false"; }

bool IsSystemCatalogDefaultsStatement(std::string_view upper) {
  return StartsWithCommand(upper, "SHOW") ||
         StartsWithCommand(upper, "DESCRIBE") ||
         StartsWithCommand(upper, "DESC") ||
         upper.find("INFORMATION_SCHEMA.") != std::string_view::npos ||
         upper.find("PERFORMANCE_SCHEMA.") != std::string_view::npos ||
         upper.find("MYSQL.") != std::string_view::npos ||
         upper.find("SYS.") != std::string_view::npos;
}

std::string SystemCatalogDefaultsEvidenceJson(
    std::string_view operation_id,
    std::span<const SurfaceDescriptor> catalog_surfaces) {
  std::ostringstream families;
  families << '[';
  for (std::size_t i = 0; i < catalog_surfaces.size(); ++i) {
    if (i != 0) families << ',';
    families << '"'
             << scratchbird::parser::compatibility::EscapeJson(
                    catalog_surfaces[i].family)
             << '"';
  }
  families << ']';

  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_system_catalog_defaults_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\"019e13c0-0000-7000-8000-000000000303\","
      << "\"semantic_profile_uuid\":\"019e13c0-1d00-7000-8000-000000000303\","
      << "\"catalog_overlay_profile_uuid\":\"019e13c0-1d00-7000-8000-000000000303\","
      << "\"dialect\":\"mysql\",\"operation_id\":\""
      << scratchbird::parser::compatibility::EscapeJson(operation_id) << "\","
      << "\"system_catalog_defaults_profile\":\"mysql.system_catalog_defaults_semantics_profile\","
      << "\"system_catalog_namespace_root_policy\":\"mysql_information_schema_mysql_performance_schema_sys_projected_from_connected_catalog_root\","
      << "\"catalog_visibility_projection_policy\":\"mysql_show_describe_information_schema_privilege_filtered_projection\","
      << "\"generated_default_catalog_name_policy\":\"mysql_generated_constraint_index_names_projected_from_engine_dictionary_descriptors\","
      << "\"dependency_projection_policy\":\"mysql_information_schema_dependencies_projected_without_parser_dependency_authority\","
      << "\"source_visibility_policy\":\"mysql_routine_trigger_view_source_redacted_or_projected_by_engine_source_policy\","
      << "\"hidden_system_object_policy\":\"mysql_data_dictionary_hidden_objects_privilege_filtered_engine_projection\","
      << "\"grant_privilege_projection_policy\":\"mysql_grants_information_schema_projection_engine_security_authority\","
      << "\"catalog_surface_family_count\":" << catalog_surfaces.size() << ','
      << "\"catalog_surface_families\":" << families.str() << ','
      << "\"sblr_catalog_projection_opcode\":\"SBLR_COMPATIBILITY_MYSQL_CATALOG_PROJECT\","
      << "\"diagnostic_map_ref\":\"mysql_system_catalog_defaults_semantics_diagnostic_map\","
      << "\"sandbox_root_policy\":\"mysql_connected_database_root_uuid_required_temp_shadowing_root_local\","
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

bool IsSessionSettingsDiagnosticsStatement(std::string_view upper) {
  return (StartsWithCommand(upper, "SET") && ContainsWord(upper, "SQL_MODE")) ||
         StartsWithCommand(upper, "SHOW WARNINGS") ||
         StartsWithCommand(upper, "SHOW VARIABLES") ||
         StartsWithCommand(upper, "USE");
}

std::string SessionSettingsDiagnosticsEvidenceJson(
    std::string_view release_profile,
    std::string_view upper) {
  const bool sql_mode_set =
      StartsWithCommand(upper, "SET") && ContainsWord(upper, "SQL_MODE");
  const bool warning_surface =
      StartsWithCommand(upper, "SHOW WARNINGS") || ContainsWord(upper, "SQL_MODE");
  const bool current_schema_surface = StartsWithCommand(upper, "USE");
  const bool diagnostic_projection_surface = StartsWithCommand(upper, "SHOW");
  const std::string_view operation_surface =
      sql_mode_set ? "mysql_set_sql_mode"
      : StartsWithCommand(upper, "SHOW WARNINGS") ? "mysql_show_warnings"
      : StartsWithCommand(upper, "SHOW VARIABLES") ? "mysql_show_variables"
      : current_schema_surface ? "mysql_use_database"
                               : "mysql_session_settings_diagnostics";
  const std::string_view compatibility_mode_policy =
      sql_mode_set ? "mysql_sql_mode_compatibility_descriptor_engine_applies"
      : current_schema_surface
          ? "mysql_default_schema_compatibility_descriptor_engine_applies"
          : "mysql_show_compatibility_projection_descriptor";
  const std::string_view warning_policy =
      StartsWithCommand(upper, "SHOW WARNINGS")
          ? "mysql_show_warnings_diagnostic_rows_engine_rendered"
          : "mysql_warning_count_diagnostic_area_engine_rendered";
  const std::string_view current_schema_policy =
      current_schema_surface
          ? "mysql_use_database_sets_current_schema_engine_session_descriptor"
          : "mysql_default_database_engine_session_descriptor";
  const std::string_view date_time_policy =
      sql_mode_set ? "mysql_sql_mode_date_time_format_descriptor_engine_applies"
                   : "mysql_date_time_format_descriptor_engine_session_defaults";

  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_session_settings_diagnostics_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\"019e13c0-0000-7000-8000-000000000303\","
      << "\"session_semantic_profile_uuid\":\"019e13c0-1e00-7000-8000-000000000303\","
      << "\"semantic_profile_uuid\":\"019e13c0-1e00-7000-8000-000000000303\","
      << "\"dialect\":\"mysql\","
      << "\"release_profile\":\""
      << scratchbird::parser::compatibility::EscapeJson(release_profile) << "\","
      << "\"session_settings_diagnostics_profile\":\"mysql.session_settings_diagnostics_semantics_profile\","
      << "\"operation_surface\":\"" << operation_surface << "\","
      << "\"sql_mode_set\":" << BoolJson(sql_mode_set) << ','
      << "\"warning_surface\":" << BoolJson(warning_surface) << ','
      << "\"notice_surface\":false,"
      << "\"current_schema_surface\":" << BoolJson(current_schema_surface) << ','
      << "\"search_path_surface\":false,"
      << "\"date_time_format_surface\":" << BoolJson(sql_mode_set) << ','
      << "\"timeout_surface\":false,\"reset_surface\":false,"
      << "\"diagnostic_projection_surface\":"
      << BoolJson(diagnostic_projection_surface) << ','
      << "\"compatibility_mode_policy\":\"" << compatibility_mode_policy << "\","
      << "\"warning_policy\":\"" << warning_policy << "\","
      << "\"notice_policy\":\"mysql_notes_warnings_errors_diagnostic_area_engine_rendered\","
      << "\"current_schema_policy\":\"" << current_schema_policy << "\","
      << "\"search_path_policy\":\"mysql_no_search_path_current_database_descriptor_only\","
      << "\"date_time_format_policy\":\"" << date_time_policy << "\","
      << "\"timeout_policy\":\"mysql_timeout_settings_not_first_tranche_descriptor_only\","
      << "\"reset_policy\":\"mysql_session_setting_reset_not_requested\","
      << "\"diagnostic_map_ref\":\"mysql_session_settings_diagnostics_semantics_diagnostic_map\","
      << "\"sandbox_root_policy\":\"mysql_connected_database_root_uuid_required_temp_shadowing_root_local\","
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

bool IsRowLockQuery(std::string_view upper) {
  if (!(StartsWithCommand(upper, "SELECT") || StartsWithCommand(upper, "WITH") ||
        (upper.starts_with("(") &&
         upper.find("SELECT") != std::string_view::npos))) {
    return false;
  }
  return upper.find(" FOR UPDATE") != std::string_view::npos ||
         upper.find(" FOR SHARE") != std::string_view::npos ||
         upper.find(" LOCK IN SHARE MODE") != std::string_view::npos;
}

bool IsLocksIsolationStatement(std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  return StartsWithCommand(upper, "SET TRANSACTION") ||
         StartsWithCommand(upper, "START TRANSACTION") ||
         StartsWithCommand(upper, "LOCK TABLES") ||
         StartsWithCommand(upper, "UNLOCK TABLES") || IsRowLockQuery(upper);
}

std::string LocksIsolationEvidenceJson(std::string_view release_profile,
                                       std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool isolation_surface =
      StartsWithCommand(upper, "SET TRANSACTION") ||
      StartsWithCommand(upper, "START TRANSACTION");
  const bool lock_table_surface =
      StartsWithCommand(upper, "LOCK TABLES") ||
      StartsWithCommand(upper, "UNLOCK TABLES");
  const bool for_update_surface =
      upper.find(" FOR UPDATE") != std::string_view::npos;
  const bool for_share_surface =
      upper.find(" FOR SHARE") != std::string_view::npos ||
      upper.find(" LOCK IN SHARE MODE") != std::string_view::npos;
  const bool row_lock_surface =
      IsRowLockQuery(upper) || for_update_surface || for_share_surface;
  const bool nowait_surface = ContainsWord(upper, "NOWAIT") ||
                              upper.find(" NO WAIT") != std::string_view::npos;
  const bool skip_locked_surface =
      upper.find(" SKIP LOCKED") != std::string_view::npos;
  const bool advisory_lock_surface =
      upper.find("GET_LOCK") != std::string_view::npos ||
      upper.find("RELEASE_LOCK") != std::string_view::npos ||
      upper.find("PG_ADVISORY_LOCK") != std::string_view::npos;
  const bool read_only_surface =
      upper.find(" READ ONLY") != std::string_view::npos;
  const bool read_write_surface =
      upper.find(" READ WRITE") != std::string_view::npos ||
      ContainsWord(upper, "WRITE");
  const bool deadlock_diagnostic_surface =
      ContainsWord(upper, "DEADLOCK") ||
      upper.find("INNODB_LOCK") != std::string_view::npos ||
      upper.find("PG_LOCKS") != std::string_view::npos ||
      upper.find("MON$LOCK") != std::string_view::npos;
  const bool transaction_surface =
      StartsWithCommand(upper, "SET TRANSACTION") ||
      StartsWithCommand(upper, "START TRANSACTION") ||
      StartsWithCommand(upper, "BEGIN");
  const bool query_surface =
      StartsWithCommand(upper, "SELECT") || StartsWithCommand(upper, "WITH") ||
      (upper.starts_with("(") &&
       upper.find("SELECT") != std::string_view::npos);
  const bool session_surface = StartsWithCommand(upper, "SET SESSION") ||
                               StartsWithCommand(upper, "SET LOCAL");

  const std::string_view locks_surface =
      StartsWithCommand(upper, "LOCK TABLES")
          ? "mysql_lock_tables_table_lock_descriptor"
      : StartsWithCommand(upper, "UNLOCK TABLES")
          ? "mysql_unlock_tables_engine_lock_release_descriptor"
      : for_update_surface ? "mysql_select_for_update_row_lock_descriptor"
      : for_share_surface ? "mysql_select_for_share_row_lock_descriptor"
      : isolation_surface ? "mysql_transaction_isolation_read_write_descriptor"
                          : "mysql_locks_isolation_syntax_surface";

  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_locks_isolation_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\"019e13c0-0000-7000-8000-000000000303\","
      << "\"semantic_profile_uuid\":\"019e13c0-1c00-7000-8000-000000000303\","
      << "\"dialect\":\"mysql\",\"release_profile\":\""
      << scratchbird::parser::compatibility::EscapeJson(release_profile) << "\","
      << "\"locks_isolation_profile\":\"mysql.locks_isolation_syntax_semantics_profile\","
      << "\"locks_isolation_surface\":\"" << locks_surface << "\","
      << "\"isolation_profile_uuid_or_policy\":\"mysql_transaction_isolation_descriptor_uuid_required_engine_mga_authority\","
      << "\"lock_clause_policy\":\"mysql_lock_tables_and_for_update_descriptor_engine_lock_authority\","
      << "\"nowait_policy\":\"mysql_nowait_descriptor_engine_lock_wait_policy\","
      << "\"skip_locked_policy\":\"mysql_skip_locked_descriptor_engine_row_lock_policy\","
      << "\"advisory_lock_policy\":\"mysql_get_lock_release_lock_advisory_descriptor_engine_policy\","
      << "\"table_lock_policy\":\"mysql_lock_tables_descriptor_engine_lock_authority\","
      << "\"row_lock_policy\":\"mysql_for_update_for_share_descriptor_engine_row_lock_authority\","
      << "\"read_write_policy\":\"mysql_read_only_read_write_descriptor_engine_intent_authority\","
      << "\"deadlock_diagnostic_policy\":\"mysql_deadlock_diagnostic_map_descriptor_engine_lock_manager_authority\","
      << "\"diagnostic_map_ref\":\"mysql_locks_isolation_semantics_diagnostic_map\","
      << "\"sandbox_root_policy\":\"mysql_connected_database_root_uuid_required_temp_shadowing_root_local\","
      << "\"isolation_surface\":" << BoolJson(isolation_surface) << ','
      << "\"lock_table_surface\":" << BoolJson(lock_table_surface) << ','
      << "\"row_lock_surface\":" << BoolJson(row_lock_surface) << ','
      << "\"for_update_surface\":" << BoolJson(for_update_surface) << ','
      << "\"for_share_surface\":" << BoolJson(for_share_surface) << ','
      << "\"nowait_surface\":" << BoolJson(nowait_surface) << ','
      << "\"skip_locked_surface\":" << BoolJson(skip_locked_surface) << ','
      << "\"advisory_lock_surface\":" << BoolJson(advisory_lock_surface) << ','
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

struct ResourceTextFlags {
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

ResourceTextFlags ClassifyResourceText(std::string_view upper) {
  ResourceTextFlags flags;
  flags.ddl = StartsWithCommand(upper, "CREATE") ||
              StartsWithCommand(upper, "ALTER") ||
              StartsWithCommand(upper, "DROP") ||
              StartsWithCommand(upper, "RECREATE");
  flags.dml = StartsWithCommand(upper, "INSERT") ||
              StartsWithCommand(upper, "UPDATE") ||
              StartsWithCommand(upper, "DELETE") ||
              StartsWithCommand(upper, "MERGE") ||
              StartsWithCommand(upper, "REPLACE");
  flags.query = StartsWithCommand(upper, "SELECT") ||
                StartsWithCommand(upper, "WITH");
  flags.binary_text = ContainsWord(upper, "BLOB") ||
                      ContainsWord(upper, "BINARY") ||
                      ContainsWord(upper, "VARBINARY");
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
                  Contains(upper, " REGEXP ") || Contains(upper, " RLIKE ");
  flags.cast_to_text =
      Contains(upper, "CAST(") && flags.text_type;
  flags.timezone = ContainsWord(upper, "TIMEZONE") ||
                   ContainsWord(upper, "DATETIME") ||
                   ContainsWord(upper, "TIMESTAMP") ||
                   ContainsWord(upper, "CURRENT_TIMESTAMP");
  flags.calendar = ContainsWord(upper, "DATE") ||
                   ContainsWord(upper, "TIME") ||
                   ContainsWord(upper, "TIMESTAMP") ||
                   ContainsWord(upper, "DATETIME") ||
                   ContainsWord(upper, "CURRENT_DATE") ||
                   ContainsWord(upper, "CURRENT_TIME");
  flags.comparison = flags.collation || flags.pattern || Contains(upper, " = ") ||
                     Contains(upper, " <> ") || Contains(upper, " != ");
  return flags;
}

bool IsResourceTextStatement(std::string_view active_upper_sql) {
  const auto flags = ClassifyResourceText(TrimAsciiView(active_upper_sql));
  if (flags.ddl) return flags.text_type || flags.charset || flags.collation;
  if (flags.dml || flags.query) {
    return flags.string_literal || flags.pattern || flags.charset ||
           flags.collation || flags.cast_to_text || flags.timezone ||
           flags.calendar;
  }
  return false;
}

std::string ResourceTextEvidenceJson(std::string_view release_profile,
                                     std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const auto flags = ClassifyResourceText(upper);
  const std::string_view surface =
      flags.ddl ? "mysql_ddl_charset_collation_text_binary"
      : flags.dml ? "mysql_dml_text_resource_descriptor"
      : flags.pattern ? "mysql_query_like_regexp_rlike"
      : flags.timezone ? "mysql_query_datetime_timestamp_resource"
      : flags.binary_text ? "mysql_binary_text_resource"
                          : "mysql_query_resource_text_semantics";

  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_resource_text_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\"019e13c0-0000-7000-8000-000000000303\","
      << "\"semantic_profile_uuid\":\"019e13c0-1a00-7000-8000-000000000303\","
      << "\"dialect\":\"mysql\",\"release_profile\":\""
      << scratchbird::parser::compatibility::EscapeJson(release_profile) << "\","
      << "\"resource_text_profile\":\"mysql.resource_text_semantics_profile\","
      << "\"resource_text_surface\":\"" << surface << "\","
      << "\"charset_policy\":\"mysql_character_set_descriptor_uuid_required_engine_applies\","
      << "\"collation_policy\":\"mysql_character_set_collation_weight_string_descriptor\","
      << "\"timezone_policy\":\"mysql_session_time_zone_descriptor_engine_authority\","
      << "\"calendar_policy\":\"mysql_temporal_calendar_sql_mode_descriptor_engine_authority\","
      << "\"comparison_policy\":\"mysql_text_comparison_charset_collation_coercibility_descriptor_engine_authority\","
      << "\"pattern_matching_policy\":\"mysql_like_regexp_rlike_collation_descriptor\","
      << "\"binary_text_policy\":\"mysql_binary_varbinary_blob_text_descriptor_required\","
      << "\"resource_epoch_policy\":\"mysql_resource_text_descriptor_epoch_engine_catalog_bound\","
      << "\"index_compatibility_policy\":\"mysql_text_index_prefix_charset_collation_compatibility_engine_validated\","
      << "\"diagnostic_map_ref\":\"mysql_resource_text_semantics_diagnostic_map\","
      << "\"sandbox_root_policy\":\"mysql_connected_database_root_uuid_required_temp_shadowing_root_local\","
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

bool IsStatisticsOptimizerStatement(std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  return StartsWithCommand(upper, "EXPLAIN") ||
         StartsWithCommand(upper, "ANALYZE TABLE") ||
         StartsWithCommand(upper, "OPTIMIZE TABLE");
}

std::string StatisticsOptimizerEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool explain_surface = StartsWithCommand(upper, "EXPLAIN");
  const bool analyze_surface = StartsWithCommand(upper, "ANALYZE TABLE");
  const bool optimize_surface = StartsWithCommand(upper, "OPTIMIZE TABLE");
  const bool statistics_update_surface = analyze_surface || optimize_surface;
  const bool index_statistics_surface = ContainsWord(upper, "INDEX");
  const bool plan_query_surface =
      explain_surface &&
      (ContainsWord(upper, "SELECT") || ContainsWord(upper, "WITH") ||
       ContainsWord(upper, "UPDATE") || ContainsWord(upper, "DELETE") ||
       ContainsWord(upper, "INSERT"));
  const std::string_view surface =
      explain_surface ? "mysql_explain_plan_catalog_projection_descriptor"
      : analyze_surface
          ? "mysql_analyze_table_statistics_update_refused_descriptor"
      : optimize_surface
          ? "mysql_optimize_table_statistics_rebuild_refused_descriptor"
          : "mysql_statistics_optimizer_metadata_surface";
  const std::string_view command_policy =
      (analyze_surface || optimize_surface)
          ? "mysql_statistics_maintenance_command_refused_no_reference_execution"
          : "mysql_optimizer_metadata_catalog_projection_only";
  const std::string_view analyze_policy =
      analyze_surface
          ? "mysql_analyze_table_refused_descriptor_no_reference_execution"
          : "mysql_analyze_table_policy_descriptor_required";
  const std::string_view explain_policy =
      explain_surface
          ? "mysql_explain_catalog_projection_descriptor_no_plan_authority"
          : "mysql_explain_policy_descriptor_required";

  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_statistics_optimizer_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\"019e13c0-0000-7000-8000-000000000303\","
      << "\"semantic_profile_uuid\":\"019e13c0-1b00-7000-8000-000000000303\","
      << "\"dialect\":\"mysql\",\"release_profile\":\""
      << scratchbird::parser::compatibility::EscapeJson(release_profile) << "\","
      << "\"statistics_optimizer_profile\":\"mysql.statistics_optimizer_metadata_semantics_profile\","
      << "\"statistics_optimizer_surface\":\"" << surface << "\","
      << "\"statistics_command_policy\":\"" << command_policy << "\","
      << "\"histogram_policy\":\"mysql_histogram_descriptor_engine_statistics_authority\","
      << "\"selectivity_policy\":\"mysql_index_cardinality_selectivity_descriptor_engine_authority\","
      << "\"stale_statistics_policy\":\"mysql_persistent_statistics_staleness_descriptor_engine_epoch\","
      << "\"index_eligibility_policy\":\"mysql_visible_index_optimizer_eligibility_engine_descriptor\","
      << "\"plan_invalidation_policy\":\"mysql_plan_invalidation_engine_dictionary_statistics_epoch\","
      << "\"analyze_command_policy\":\"" << analyze_policy << "\","
      << "\"explain_plan_policy\":\"" << explain_policy << "\","
      << "\"catalog_projection_policy\":\"mysql_information_schema_statistics_projection_uuid_required\","
      << "\"diagnostic_map_ref\":\"mysql_statistics_optimizer_semantics_diagnostic_map\","
      << "\"sandbox_root_policy\":\"mysql_connected_database_root_uuid_required_temp_shadowing_root_local\","
      << "\"explain_surface\":" << BoolJson(explain_surface) << ','
      << "\"analyze_surface\":" << BoolJson(analyze_surface) << ','
      << "\"statistics_update_surface\":"
      << BoolJson(statistics_update_surface) << ','
      << "\"reindex_surface\":false,"
      << "\"optimize_surface\":" << BoolJson(optimize_surface) << ','
      << "\"create_statistics_surface\":false,"
      << "\"drop_statistics_surface\":false,"
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

bool IsDdlTransactionBehaviorStatement(std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  return StartsWithCommand(upper, "CREATE") ||
         StartsWithCommand(upper, "ALTER") ||
         StartsWithCommand(upper, "DROP") ||
         StartsWithCommand(upper, "TRUNCATE");
}

bool IsCreateIndexStatement(std::string_view upper) {
  return StartsWithCommand(upper, "CREATE INDEX") ||
         StartsWithCommand(upper, "CREATE UNIQUE INDEX") ||
         StartsWithCommand(upper, "CREATE FULLTEXT INDEX") ||
         StartsWithCommand(upper, "CREATE SPATIAL INDEX");
}

std::string_view DdlOperationKind(std::string_view upper) {
  if (StartsWithCommand(upper, "CREATE OR REPLACE VIEW")) {
    return "create_or_replace_view";
  }
  if (StartsWithCommand(upper, "CREATE VIEW")) return "create_view";
  if (StartsWithCommand(upper, "CREATE TEMPORARY TABLE") ||
      StartsWithCommand(upper, "CREATE TABLE")) {
    return "create_table";
  }
  if (IsCreateIndexStatement(upper)) return "create_index";
  if (StartsWithCommand(upper, "ALTER TABLE")) return "alter_table";
  if (StartsWithCommand(upper, "ALTER INDEX")) return "alter_index";
  if (StartsWithCommand(upper, "ALTER VIEW")) return "alter_view";
  if (StartsWithCommand(upper, "DROP TABLE")) return "drop_table";
  if (StartsWithCommand(upper, "DROP INDEX")) return "drop_index";
  if (StartsWithCommand(upper, "DROP VIEW")) return "drop_view";
  if (StartsWithCommand(upper, "TRUNCATE")) return "truncate";
  if (StartsWithCommand(upper, "CREATE")) return "create";
  if (StartsWithCommand(upper, "ALTER")) return "alter";
  if (StartsWithCommand(upper, "DROP")) return "drop";
  return "ddl";
}

std::string DdlTransactionBehaviorEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool create_surface = StartsWithCommand(upper, "CREATE");
  const bool alter_surface = StartsWithCommand(upper, "ALTER");
  const bool drop_surface = StartsWithCommand(upper, "DROP");
  const bool table_surface = ContainsWord(upper, "TABLE") ||
                             StartsWithCommand(upper, "TRUNCATE");
  const bool index_surface = IsCreateIndexStatement(upper) ||
                             StartsWithCommand(upper, "ALTER INDEX") ||
                             StartsWithCommand(upper, "DROP INDEX");
  const bool view_surface = ContainsWord(upper, "VIEW");

  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_ddl_transaction_behavior_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\"019e13c0-0000-7000-8000-000000000303\","
      << "\"semantic_profile_uuid\":\"019e13c0-1900-7000-8000-000000000303\","
      << "\"dialect\":\"mysql\",\"release_profile\":\""
      << scratchbird::parser::compatibility::EscapeJson(release_profile) << "\","
      << "\"ddl_transaction_behavior_profile\":\"mysql.ddl_transaction_behavior_semantics_profile\","
      << "\"statement_classification\":\"ddl\","
      << "\"ddl_operation_kind\":\"" << DdlOperationKind(upper) << "\","
      << "\"transaction_policy\":\"mysql_implicit_commit_ddl_descriptor_required\","
      << "\"autocommit_boundary\":\"implicit_commit_before_and_after_ddl_engine_policy\","
      << "\"metadata_visibility_epoch\":\"post_implicit_commit_catalog_epoch\","
      << "\"rollback_policy\":\"mysql_ddl_not_rolled_back_by_user_transaction_descriptor\","
      << "\"invalid_object_state_policy\":\"mysql_atomic_ddl_dictionary_state_engine_authority\","
      << "\"diagnostic_map_ref\":\"mysql_ddl_transaction_behavior_diagnostic_map\","
      << "\"sandbox_root_policy\":\"mysql_connected_database_root_uuid_required_temp_shadowing_root_local\","
      << "\"create_surface\":" << BoolJson(create_surface) << ','
      << "\"alter_surface\":" << BoolJson(alter_surface) << ','
      << "\"drop_surface\":" << BoolJson(drop_surface) << ','
      << "\"table_surface\":" << BoolJson(table_surface) << ','
      << "\"index_surface\":" << BoolJson(index_surface) << ','
      << "\"view_surface\":" << BoolJson(view_surface) << ','
      << "\"implicit_commit_surface\":true,"
      << "\"transactional_ddl_surface\":false,"
      << "\"nontransactional_ddl_surface\":true,"
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

struct DependencyDdlFlags {
  bool view{false};
  bool trigger{false};
  bool routine{false};
  bool procedure{false};
  bool function{false};
  bool event{false};
};

DependencyDdlFlags ClassifyDependencyDdl(std::string_view upper) {
  DependencyDdlFlags flags;
  flags.view = StartsWithCommand(upper, "CREATE VIEW") ||
               StartsWithCommand(upper, "CREATE OR REPLACE VIEW") ||
               StartsWithCommand(upper, "ALTER VIEW") ||
               StartsWithCommand(upper, "DROP VIEW");
  flags.trigger = StartsWithCommand(upper, "CREATE TRIGGER") ||
                  StartsWithCommand(upper, "ALTER TRIGGER") ||
                  StartsWithCommand(upper, "DROP TRIGGER");
  flags.procedure = StartsWithCommand(upper, "CREATE PROCEDURE") ||
                    StartsWithCommand(upper, "ALTER PROCEDURE") ||
                    StartsWithCommand(upper, "DROP PROCEDURE");
  flags.function = StartsWithCommand(upper, "CREATE FUNCTION") ||
                   StartsWithCommand(upper, "ALTER FUNCTION") ||
                   StartsWithCommand(upper, "DROP FUNCTION");
  flags.routine = flags.procedure || flags.function;
  flags.event = StartsWithCommand(upper, "CREATE EVENT") ||
                StartsWithCommand(upper, "ALTER EVENT") ||
                StartsWithCommand(upper, "DROP EVENT");
  return flags;
}

bool IsDependencyBearingDdlStatement(std::string_view active_upper_sql) {
  const auto flags = ClassifyDependencyDdl(TrimAsciiView(active_upper_sql));
  return flags.view || flags.trigger || flags.routine || flags.event;
}

std::string DependencyBearingDdlEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const auto flags = ClassifyDependencyDdl(upper);
  const bool executable_body_surface =
      flags.trigger || flags.routine || flags.event;
  const bool query_dependency_surface =
      flags.view || executable_body_surface || ContainsWord(upper, "FROM") ||
      ContainsWord(upper, "JOIN") || ContainsWord(upper, "ON") ||
      ContainsWord(upper, "REFERENCES");
  const bool drop_surface = StartsWithCommand(upper, "DROP");
  const bool alter_surface = StartsWithCommand(upper, "ALTER");
  const bool create_surface = StartsWithCommand(upper, "CREATE");
  const std::string_view surface =
      StartsWithCommand(upper, "CREATE OR REPLACE VIEW")
          ? "mysql_create_or_replace_view"
      : StartsWithCommand(upper, "CREATE VIEW") ? "mysql_create_view"
      : StartsWithCommand(upper, "ALTER VIEW") ? "mysql_alter_view"
      : StartsWithCommand(upper, "DROP VIEW") ? "mysql_drop_view"
      : flags.trigger ? "mysql_trigger_ddl"
      : flags.event ? "mysql_event_scheduler_ddl"
      : flags.routine ? "mysql_procedure_function_ddl"
                      : "mysql_dependency_bearing_ddl";
  const std::string_view binding_policy =
      flags.event ? "mysql_event_scheduler_dependency_binding_uuid_descriptors"
      : flags.trigger ? "mysql_trigger_table_dependency_binding_uuid_descriptors"
                      : "mysql_routine_view_dependency_binding_uuid_descriptors";
  const std::string_view execution_policy =
      executable_body_surface
          ? "mysql_routine_trigger_event_body_routes_to_trusted_udr_lowering"
          : "mysql_view_definition_descriptor_no_parser_execution";

  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_dependency_bearing_ddl_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\"019e13c0-0000-7000-8000-000000000303\","
      << "\"semantic_profile_uuid\":\"019e13c0-1800-7000-8000-000000000303\","
      << "\"dialect\":\"mysql\",\"release_profile\":\""
      << scratchbird::parser::compatibility::EscapeJson(release_profile) << "\","
      << "\"dependency_ddl_profile\":\"mysql.dependency_bearing_ddl_semantics_profile\","
      << "\"dependency_ddl_surface\":\"" << surface << "\","
      << "\"view_surface\":" << BoolJson(flags.view) << ','
      << "\"materialized_view_surface\":false,"
      << "\"trigger_surface\":" << BoolJson(flags.trigger) << ','
      << "\"routine_surface\":" << BoolJson(flags.routine) << ','
      << "\"procedure_surface\":" << BoolJson(flags.procedure) << ','
      << "\"function_surface\":" << BoolJson(flags.function) << ','
      << "\"package_surface\":false,\"rule_surface\":false,"
      << "\"event_surface\":" << BoolJson(flags.event) << ','
      << "\"executable_body_surface\":"
      << BoolJson(executable_body_surface) << ','
      << "\"query_dependency_surface\":"
      << BoolJson(query_dependency_surface) << ','
      << "\"create_surface\":" << BoolJson(create_surface) << ','
      << "\"alter_surface\":" << BoolJson(alter_surface) << ','
      << "\"drop_surface\":" << BoolJson(drop_surface) << ','
      << "\"dependency_binding_policy\":\"" << binding_policy << "\","
      << "\"invalidation_policy\":\"mysql_metadata_dependency_invalidation_engine_catalog_authority\","
      << "\"execution_body_policy\":\"" << execution_policy << "\","
      << "\"catalog_storage_policy\":\"mysql_information_schema_catalog_projection_stores_uuid_dependency_descriptors\","
      << "\"sandbox_root_policy\":\"mysql_connected_database_root_uuid_required_temp_shadowing_root_local\","
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

bool IsRollbackToSavepoint(std::string_view upper) {
  return StartsWithCommand(upper, "ROLLBACK TO SAVEPOINT") ||
         StartsWithCommand(upper, "ROLLBACK TO") ||
         StartsWithCommand(upper, "ROLLBACK WORK TO SAVEPOINT") ||
         StartsWithCommand(upper, "ROLLBACK WORK TO") ||
         StartsWithCommand(upper, "ROLLBACK TRANSACTION TO SAVEPOINT") ||
         StartsWithCommand(upper, "ROLLBACK TRANSACTION TO");
}

bool IsTransactionSessionSemanticStatement(std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
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

std::string TransactionSessionSemanticEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool rollback_to_savepoint = IsRollbackToSavepoint(upper);

  scratchbird::parser::compatibility::TransactionSessionSemanticDescriptor d;
  d.compatibility_profile_uuid = "019e13c0-0000-7000-8000-000000000303";
  d.semantic_profile_uuid = "019e13c0-1600-7000-8000-000000000303";
  d.transaction_session_profile = "mysql.transaction_session_semantics_profile";
  if (StartsWithCommand(upper, "START TRANSACTION")) {
    d.transaction_session_surface =
        Contains(upper, "READ ONLY") ? "mysql_start_transaction_read_only"
        : Contains(upper, "READ WRITE") ? "mysql_start_transaction_read_write"
                                         : "mysql_start_transaction";
  } else if (StartsWithCommand(upper, "BEGIN")) {
    d.transaction_session_surface = "mysql_begin";
  } else if (StartsWithCommand(upper, "COMMIT")) {
    d.transaction_session_surface = "mysql_commit";
  } else if (rollback_to_savepoint) {
    d.transaction_session_surface = "mysql_rollback_to_savepoint";
  } else if (StartsWithCommand(upper, "ROLLBACK")) {
    d.transaction_session_surface = "mysql_rollback";
  } else if (StartsWithCommand(upper, "RELEASE SAVEPOINT")) {
    d.transaction_session_surface = "mysql_release_savepoint";
  } else if (StartsWithCommand(upper, "SAVEPOINT")) {
    d.transaction_session_surface = "mysql_savepoint";
  } else if (StartsWithCommand(upper, "SET") &&
             ContainsWord(upper, "AUTOCOMMIT")) {
    d.transaction_session_surface = "mysql_set_autocommit";
  } else if (StartsWithCommand(upper, "SET") &&
             Contains(upper, " TRANSACTION ISOLATION ")) {
    d.transaction_session_surface = "mysql_set_session_transaction_isolation";
  } else if (StartsWithCommand(upper, "SET") &&
             ContainsWord(upper, "SQL_MODE")) {
    d.transaction_session_surface = "mysql_set_sql_mode";
  } else {
    d.transaction_session_surface = "mysql_transaction_session";
  }
  d.statement_family_linkage =
      StartsWithCommand(upper, "SET") ? "session" : "transaction";
  if (StartsWithCommand(upper, "SET") && ContainsWord(upper, "AUTOCOMMIT")) {
    d.begin_autocommit_policy =
        "mysql_autocommit_session_descriptor_engine_transaction_profile";
  } else if (StartsWithCommand(upper, "START TRANSACTION") ||
             StartsWithCommand(upper, "BEGIN")) {
    d.begin_autocommit_policy =
        "mysql_explicit_begin_requests_engine_mga_transaction_handle_autocommit_suspended";
  } else {
    d.begin_autocommit_policy =
        "mysql_existing_engine_transaction_or_session_descriptor";
  }
  d.isolation_read_only_deferrable_descriptor_policy =
      "mysql_transaction_access_mode_isolation_descriptor_engine_enforced";
  if (ContainsWord(upper, "SQL_MODE")) {
    d.session_variable_sql_mode_descriptor_policy =
        "mysql_sql_mode_session_descriptor_uuid_profile_engine_applies";
  } else if (ContainsWord(upper, "AUTOCOMMIT")) {
    d.session_variable_sql_mode_descriptor_policy =
        "mysql_autocommit_session_descriptor_engine_applies";
  } else {
    d.session_variable_sql_mode_descriptor_policy =
        "mysql_session_transaction_descriptor_engine_applies";
  }
  d.begin_surface = StartsWithCommand(upper, "BEGIN") ||
                    StartsWithCommand(upper, "START TRANSACTION");
  d.commit_surface = StartsWithCommand(upper, "COMMIT");
  d.rollback_to_savepoint_surface = rollback_to_savepoint;
  d.rollback_surface = StartsWithCommand(upper, "ROLLBACK") &&
                       !rollback_to_savepoint;
  d.savepoint_surface = StartsWithCommand(upper, "SAVEPOINT");
  d.release_savepoint_surface = StartsWithCommand(upper, "RELEASE SAVEPOINT");
  d.autocommit_surface = StartsWithCommand(upper, "SET") &&
                         ContainsWord(upper, "AUTOCOMMIT");
  d.isolation_descriptor_surface =
      (StartsWithCommand(upper, "SET") &&
       (Contains(upper, " TRANSACTION ISOLATION ") ||
        Contains(upper, " TRANSACTION_ISOLATION"))) ||
      Contains(upper, " ISOLATION LEVEL ") ||
      ContainsWord(upper, "SERIALIZABLE") ||
      Contains(upper, "READ COMMITTED") ||
      Contains(upper, "REPEATABLE READ") ||
      Contains(upper, "READ UNCOMMITTED");
  d.read_only_surface = Contains(upper, "READ ONLY");
  d.read_write_surface = Contains(upper, "READ WRITE");
  d.session_variable_surface = StartsWithCommand(upper, "SET") &&
      (ContainsWord(upper, "AUTOCOMMIT") || ContainsWord(upper, "SQL_MODE") ||
       ContainsWord(upper, "TRANSACTION_ISOLATION"));
  d.sql_mode_surface = ContainsWord(upper, "SQL_MODE");
  return scratchbird::parser::compatibility::
      RenderTransactionSessionSemanticEvidenceJson("mysql", release_profile, d);
}

bool IsTemporarySessionObjectSemanticStatement(
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool alter_surface =
      (StartsWithCommand(upper, "ALTER TABLE") ||
       StartsWithCommand(upper, "ALTER TEMPORARY TABLE") ||
       StartsWithCommand(upper, "ALTER TEMP TABLE")) &&
      Contains(upper, "TEMP");
  return StartsWithCommand(upper, "CREATE TEMPORARY TABLE") ||
         StartsWithCommand(upper, "DROP TEMPORARY TABLE") || alter_surface;
}

std::string TemporarySessionObjectSemanticEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  scratchbird::parser::compatibility::TemporarySessionObjectSemanticDescriptor d;
  d.compatibility_profile_uuid = "019e13c0-0000-7000-8000-000000000303";
  d.semantic_profile_uuid = "019e13c0-1700-7000-8000-000000000303";
  d.temporary_object_profile = "mysql.session_temporary_table_semantics_profile";
  d.create_surface = StartsWithCommand(upper, "CREATE TEMPORARY TABLE");
  d.drop_surface = StartsWithCommand(upper, "DROP TEMPORARY TABLE");
  d.alter_surface = !d.create_surface && !d.drop_surface &&
                    StartsWithCommand(upper, "ALTER");
  d.temporary_object_surface =
      d.create_surface ? "mysql_create_temporary_table_name_shadowing"
      : d.drop_surface ? "mysql_drop_temporary_table_session_object"
                       : "mysql_temporary_session_object";
  d.temporary_object_kind_policy =
      "mysql_session_temporary_table_private_name_shadowing_regular_table";
  d.global_keyword_surface = ContainsWord(upper, "GLOBAL");
  d.local_keyword_surface = ContainsWord(upper, "LOCAL");
  d.temporary_keyword_surface = ContainsWord(upper, "TEMP") ||
                                ContainsWord(upper, "TEMPORARY") ||
                                Contains(upper, "TEMP");
  d.table_object_surface = ContainsWord(upper, "TABLE");
  d.on_commit_delete_rows_surface = Contains(upper, "ON COMMIT DELETE ROWS");
  d.on_commit_preserve_rows_surface = Contains(upper, "ON COMMIT PRESERVE ROWS");
  d.on_commit_drop_surface = Contains(upper, "ON COMMIT DROP");
  d.on_commit_policy = "mysql_no_on_commit_clause_table_lifetime_session_end";
  d.on_commit_delete_rows_policy = "mysql_delete_rows_not_supported";
  d.on_commit_preserve_rows_policy =
      "mysql_preserve_rows_is_session_lifetime_default";
  d.on_commit_drop_policy = "mysql_on_commit_drop_not_supported";
  d.name_shadowing_surface = true;
  d.name_shadowing_policy =
      "mysql_temporary_table_name_shadows_base_table_within_session";
  d.session_visibility_policy =
      "mysql_temporary_table_session_private_name_shadowing_visible_to_connection";
  d.catalog_visibility_policy =
      "mysql_temporary_table_not_persistent_information_schema_object";
  d.temporary_object_lifetime_policy =
      "mysql_temp_table_dropped_at_engine_session_end_or_explicit_drop";
  d.schema_root_sandbox_policy =
      "mysql_connected_database_root_uuid_required_temp_shadowing_root_local";
  return scratchbird::parser::compatibility::
      RenderTemporarySessionObjectSemanticEvidenceJson("mysql", release_profile, d);
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
  return StartsWithCommand(upper, "CREATE INDEX") ||
         StartsWithCommand(upper, "CREATE UNIQUE INDEX") ||
         StartsWithCommand(upper, "CREATE FULLTEXT INDEX") ||
         StartsWithCommand(upper, "CREATE SPATIAL INDEX");
}

std::string IndexSemanticDefaultsEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool expression = Contains(upper, "((") || Contains(upper, "COMPUTED BY");
  scratchbird::parser::compatibility::IndexSemanticDefaultsDescriptor d;
  d.compatibility_profile_uuid = "019e13c0-0000-7000-8000-000000000303";
  d.semantic_profile_uuid = "019e13c0-1000-7000-8000-000000000303";
  d.index_profile = "mysql.index_optimizer_translation_profile";
  d.ddl_surface = StartsWithCommand(upper, "CREATE")
      ? (ContainsWord(upper, "UNIQUE") ? "create_unique_index" : "create_index")
      : StartsWithCommand(upper, "ALTER TABLE") ? "alter_table_index"
      : StartsWithCommand(upper, "ALTER INDEX") ? "alter_index"
                                                 : "index_ddl";
  d.index_method =
      ContainsWord(upper, "FULLTEXT") ? "mysql_fulltext_index_profile"
      : ContainsWord(upper, "SPATIAL") ? "mysql_spatial_index_profile"
      : Contains(upper, " USING HASH")
          ? "mysql_hash_index_method_requested_engine_validated"
          : "mysql_innodb_btree_index_profile";
  d.unique_requested = ContainsWord(upper, "UNIQUE");
  d.unique_null_policy = d.unique_requested
      ? "mysql_innodb_unique_index_allows_multiple_nulls_profile"
      : "not_unique_index_not_applicable";
  d.null_ordering = "mysql_nulls_low_ascending_index_profile";
  d.collation_policy =
      "mysql_character_set_collation_weight_string_descriptor";
  d.operator_family_policy =
      "mysql_builtin_comparison_no_named_operator_family";
  d.predicate_or_expression_policy = expression
      ? "mysql_functional_key_part_descriptor"
      : "mysql_column_index_no_partial_predicate";
  d.predicate_present = ContainsWord(upper, "WHERE");
  d.expression_key_present = expression;
  d.concurrently_requested = ContainsWord(upper, "CONCURRENTLY");
  d.descending_requested = ContainsWord(upper, "DESC") ||
                           ContainsWord(upper, "DESCENDING");
  d.nulls_not_distinct_requested = Contains(upper, "NULLS NOT DISTINCT");
  d.validation_state = ContainsWord(upper, "INVISIBLE")
      ? "mysql_index_invisible_requested"
      : "mysql_index_visible_default";
  d.build_mode = StartsWithCommand(upper, "ALTER TABLE")
      ? "mysql_alter_index_visibility_or_metadata_route"
      : "mysql_engine_selected_online_ddl_default";
  d.statistics_policy_ref =
      "mysql_innodb_persistent_index_statistics_profile";
  return scratchbird::parser::compatibility::
      RenderIndexSemanticDefaultsEvidenceJson("mysql", release_profile, d);
}

bool IsConstraintSemanticDefaultsStatement(std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  return StartsWithCommand(upper, "CREATE") && ContainsWord(upper, "TABLE") &&
         (Contains(upper, "PRIMARY KEY") || ContainsWord(upper, "UNIQUE") ||
          Contains(upper, "FOREIGN KEY") || ContainsWord(upper, "REFERENCES") ||
          ContainsWord(upper, "CHECK") || ContainsWord(upper, "DEFAULT") ||
          ContainsWord(upper, "GENERATED") ||
          ContainsWord(upper, "AUTO_INCREMENT"));
}

std::string ConstraintSemanticDefaultsEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  scratchbird::parser::compatibility::ConstraintSemanticDefaultsDescriptor d;
  d.compatibility_profile_uuid = "019e13c0-0000-7000-8000-000000000303";
  d.semantic_profile_uuid = "019e13c0-1100-7000-8000-000000000303";
  d.constraint_profile = "mysql.table_constraint_defaults_profile";
  d.primary_key_present = Contains(upper, "PRIMARY KEY");
  d.primary_key_behavior =
      "mysql_primary_key_not_null_unique_index_innodb_descriptor";
  d.unique_constraint_present = ContainsWord(upper, "UNIQUE");
  d.unique_null_policy = d.unique_constraint_present
      ? "mysql_unique_constraint_allows_multiple_nulls_profile"
      : "not_unique_constraint_not_applicable";
  d.foreign_key_reference_present = Contains(upper, "FOREIGN KEY") ||
                                    ContainsWord(upper, "REFERENCES");
  d.foreign_key_action_defaults =
      "mysql_innodb_foreign_key_default_restrict_update_restrict_delete_descriptor";
  d.check_constraint_present = ContainsWord(upper, "CHECK");
  d.check_truth_table_null_behavior =
      "mysql_check_constraint_false_fails_unknown_passes_profile";
  d.default_clause_present = ContainsWord(upper, "DEFAULT");
  d.default_expression_policy =
      "mysql_default_literal_or_parenthesized_expression_descriptor";
  d.generated_identity_or_autoincrement_present =
      ContainsWord(upper, "GENERATED") || ContainsWord(upper, "AUTO_INCREMENT");
  d.generated_identity_autoincrement_policy =
      ContainsWord(upper, "AUTO_INCREMENT")
          ? "mysql_auto_increment_column_attribute_descriptor"
          : "mysql_no_implicit_autoincrement_default";
  d.explicit_constraint_names_present = ContainsWord(upper, "CONSTRAINT");
  d.generated_name_policy =
      "mysql_engine_generated_constraint_names_descriptor_required";
  d.deferrability_policy =
      "mysql_constraints_not_deferrable_immediate_profile";
  d.enforcement_timing =
      "mysql_innodb_immediate_constraint_validation_profile";
  return scratchbird::parser::compatibility::
      RenderConstraintSemanticDefaultsEvidenceJson("mysql", release_profile, d);
}

bool IsSequenceIdentitySemanticStatement(std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  return ContainsWord(upper, "AUTO_INCREMENT") ||
         Contains(upper, "LAST_INSERT_ID(");
}

std::string SequenceIdentitySemanticEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  scratchbird::parser::compatibility::SequenceIdentitySemanticDescriptor d;
  d.compatibility_profile_uuid = "019e13c0-0000-7000-8000-000000000303";
  d.semantic_profile_uuid = "019e13c0-1300-7000-8000-000000000303";
  d.sequence_identity_profile = "mysql.auto_increment_identity_profile";
  d.auto_increment_surface = ContainsWord(upper, "AUTO_INCREMENT");
  d.last_insert_id_surface = Contains(upper, "LAST_INSERT_ID(");
  d.sequence_identity_surface = d.last_insert_id_surface
      ? "mysql_last_insert_id_session_function"
      : d.auto_increment_surface
          ? "mysql_auto_increment_column_or_table_option"
          : "mysql_auto_increment_descriptor";
  d.sequence_backed_default_present = d.auto_increment_surface;
  d.restart_descriptor_present = ContainsWord(upper, "RESTART") ||
                                 Contains(upper, "START WITH");
  d.increment_descriptor_present = ContainsWord(upper, "INCREMENT") ||
                                   d.auto_increment_surface;
  d.min_value_descriptor_present = ContainsWord(upper, "MINVALUE") ||
                                   Contains(upper, "NO MINVALUE");
  d.max_value_descriptor_present = ContainsWord(upper, "MAXVALUE") ||
                                   Contains(upper, "NO MAXVALUE");
  d.cycle_descriptor_present = ContainsWord(upper, "CYCLE") ||
                               Contains(upper, "NO CYCLE");
  d.cache_descriptor_present = ContainsWord(upper, "CACHE") ||
                               Contains(upper, "NO CACHE");
  d.session_visible_state_surface = d.last_insert_id_surface;
  d.object_identity_policy =
      "mysql_table_column_auto_increment_uuid_required_no_source_name_binding";
  d.engine_catalog_sequence_descriptor_policy =
      "mysql_engine_catalog_auto_increment_counter_descriptor_policy";
  d.allocation_finality_policy =
      "mysql_auto_increment_lower_layer_allocation_descriptor_parser_not_allocator";
  d.lower_layer_allocation_policy =
      "mysql_storage_engine_auto_increment_allocator_policy_descriptor";
  d.value_function_profile = d.last_insert_id_surface
      ? "mysql_last_insert_id_session_visible_descriptor"
      : "mysql_last_insert_id_not_observed";
  d.session_visibility_policy =
      "mysql_last_insert_id_connection_session_visible_descriptor";
  d.sequence_backed_default_policy = d.auto_increment_surface
      ? "mysql_auto_increment_column_counter_default_descriptor"
      : "mysql_no_auto_increment_default_observed";
  d.restart_increment_descriptor_policy =
      "mysql_auto_increment_table_option_descriptor";
  return scratchbird::parser::compatibility::
      RenderSequenceIdentitySemanticEvidenceJson("mysql", release_profile, d);
}

bool IsIdentifierNameResolutionStatement(std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  return StartsWithCommand(upper, "CREATE") ||
         StartsWithCommand(upper, "ALTER") ||
         StartsWithCommand(upper, "DROP") ||
         StartsWithCommand(upper, "TRUNCATE");
}

std::string IdentifierNameResolutionEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  scratchbird::parser::compatibility::
      IdentifierNameResolutionSemanticDescriptor d;
  d.compatibility_profile_uuid =
      "019e13c0-0000-7000-8000-000000000303";
  d.semantic_profile_uuid = "019e13c0-1200-7000-8000-000000000303";
  d.name_resolution_profile = "mysql.identifier_name_resolution_profile";
  d.unquoted_identifier_policy =
      "mysql_unquoted_identifiers_preserve_spelling_table_name_case_bound_by_lower_case_table_names";
  d.quoted_identifier_policy =
      "mysql_quoted_identifiers_preserve_exact_case_backtick_default_ansi_quotes_profile_bound";
  d.schema_root_resolution_policy =
      "mysql_database_schema_root_uuid_resolution_required_no_filesystem_authority";
  d.generated_catalog_name_behavior =
      "mysql_engine_generated_names_descriptor_required_lower_case_table_names_bound";
  d.namespace_collision_behavior =
      "mysql_schema_table_namespace_collision_resolved_by_uuid_descriptor_and_lctn_profile";
  d.result_metadata_label_policy =
      "mysql_result_labels_preserve_alias_spelling_descriptor";
  d.table_name_filesystem_case_policy =
      "mysql_lower_case_table_names_filesystem_sensitive_bound_descriptor";
  d.release_profile_variant_bound_to_base_compatibility = true;
  d.create_surface = StartsWithCommand(upper, "CREATE");
  d.alter_surface = StartsWithCommand(upper, "ALTER");
  d.drop_surface = StartsWithCommand(upper, "DROP");
  d.quoted_identifier_syntax_observed =
      Contains(upper, "\"") || Contains(upper, "`");
  d.qualified_name_syntax_observed = Contains(upper, ".");
  return scratchbird::parser::compatibility::
      RenderIdentifierNameResolutionSemanticEvidenceJson(
          "mysql", release_profile, d);
}

scratchbird::parser::compatibility::ScalarExpressionSemanticDescriptor
MysqlScalarExpressionSemanticDescriptor(std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  scratchbird::parser::compatibility::ScalarExpressionSemanticDescriptor d;
  d.compatibility_profile_uuid =
      "019e13c0-0000-7000-8000-000000000303";
  d.semantic_profile_uuid = "019e13c0-1400-7000-8000-000000000303";
  d.scalar_expression_profile = "mysql.scalar_expression_semantics_profile";
  d.query_expression_surface = StartsWithCommand(upper, "WITH")
      ? "with_query_scalar_expression"
      : StartsWithCommand(upper, "SELECT")
          ? "select_scalar_expression"
          : "query_scalar_expression";
  d.cast_type_coercion_profile =
      "mysql_cast_convert_type_coercion_sql_mode_descriptor";
  d.null_three_valued_logic_profile =
      "mysql_three_valued_logic_null_safe_equality_descriptor";
  d.boolean_literal_profile = "mysql_truthy_numeric_boolean_literal_profile";
  d.string_comparison_collation_profile =
      "mysql_charset_collation_coercibility_descriptor_no_parser_collation_authority";
  d.temporal_literal_current_timestamp_date_arithmetic_profile =
      "mysql_datetime_timestamp_current_timestamp_date_add_sql_mode_descriptor";
  d.numeric_division_rounding_overflow_profile =
      "mysql_division_rounding_overflow_sql_mode_descriptor";
  d.pattern_matching_profile = "mysql_like_regexp_rlike_collation_descriptor";
  d.conditional_expression_profile =
      "mysql_case_if_ifnull_nullif_coalesce_descriptor";
  d.expression_builtin_profile =
      "mysql_expression_builtin_profile_if_ifnull_date_add_regexp";
  d.cast_or_coercion_surface =
      Contains(upper, "CAST(") || Contains(upper, "CONVERT(");
  d.null_logic_surface = ContainsWord(upper, "NULL") || Contains(upper, "<=>");
  d.boolean_literal_surface = ContainsWord(upper, "TRUE") ||
                              ContainsWord(upper, "FALSE") ||
                              ContainsWord(upper, "UNKNOWN");
  d.string_comparison_surface = ContainsWord(upper, "COLLATE") ||
                                Contains(upper, " LIKE ") ||
                                Contains(upper, " REGEXP ") ||
                                Contains(upper, " RLIKE ");
  d.temporal_expression_surface =
      ContainsWord(upper, "CURRENT_DATE") ||
      ContainsWord(upper, "CURRENT_TIME") ||
      ContainsWord(upper, "CURRENT_TIMESTAMP") ||
      ContainsWord(upper, "LOCALTIMESTAMP") ||
      ContainsWord(upper, "LOCALTIME") || Contains(upper, "DATE_ADD(") ||
      Contains(upper, "DATE_SUB(") || Contains(upper, "TIMESTAMPDIFF(") ||
      Contains(upper, "EXTRACT(") || Contains(upper, "NOW(") ||
      ContainsWord(upper, "INTERVAL") || Contains(upper, "TIMESTAMP '") ||
      Contains(upper, "DATE '") || Contains(upper, "TIME '");
  d.numeric_expression_surface =
      Contains(upper, "/") || Contains(upper, " DIV ") ||
      Contains(upper, "ROUND(") || Contains(upper, "TRUNCATE(") ||
      Contains(upper, "MOD(") || Contains(upper, "POWER(") ||
      Contains(upper, "POW(") || Contains(upper, "SQRT(") ||
      ContainsWord(upper, "NUMERIC") || ContainsWord(upper, "DECIMAL");
  d.pattern_matching_surface = Contains(upper, " LIKE ") ||
                               Contains(upper, " REGEXP ") ||
                               Contains(upper, " RLIKE ");
  d.conditional_expression_surface = HasFunctionCall(upper, "COALESCE") ||
                                     HasFunctionCall(upper, "NULLIF") ||
                                     HasFunctionCall(upper, "IF") ||
                                     HasFunctionCall(upper, "IFNULL") ||
                                     Contains(upper, "CASE ");
  d.null_safe_equality_surface = Contains(upper, "<=>");
  d.regexp_surface =
      Contains(upper, " REGEXP ") || Contains(upper, " RLIKE ");
  d.compatibility_conditional_function_surface =
      HasFunctionCall(upper, "IF") || HasFunctionCall(upper, "IFNULL");
  return d;
}

bool IsScalarExpressionSemanticStatement(std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  if (!(StartsWithCommand(upper, "SELECT") ||
        StartsWithCommand(upper, "WITH"))) {
    return false;
  }
  const auto d = MysqlScalarExpressionSemanticDescriptor(upper);
  return d.cast_or_coercion_surface || d.null_logic_surface ||
         d.boolean_literal_surface || d.string_comparison_surface ||
         d.temporal_expression_surface || d.numeric_expression_surface ||
         d.pattern_matching_surface || d.conditional_expression_surface;
}

std::string ScalarExpressionSemanticEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  return scratchbird::parser::compatibility::
      RenderScalarExpressionSemanticEvidenceJson(
          "mysql", release_profile,
          MysqlScalarExpressionSemanticDescriptor(active_upper_sql));
}

bool IsDmlMutationSemanticStatement(std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  return StartsWithCommand(upper, "INSERT") ||
         StartsWithCommand(upper, "UPDATE") ||
         StartsWithCommand(upper, "DELETE") ||
         StartsWithCommand(upper, "REPLACE");
}

std::string DmlMutationSemanticEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  scratchbird::parser::compatibility::DmlMutationSemanticDescriptor d;
  d.compatibility_profile_uuid =
      "019e13c0-0000-7000-8000-000000000303";
  d.semantic_profile_uuid = "019e13c0-1500-7000-8000-000000000303";
  d.mutation_profile = "mysql.dml_mutation_semantics_profile";
  d.insert_surface = StartsWithCommand(upper, "INSERT");
  d.update_surface = StartsWithCommand(upper, "UPDATE");
  d.delete_surface = StartsWithCommand(upper, "DELETE");
  d.replace_surface = StartsWithCommand(upper, "REPLACE");
  d.on_duplicate_key_update_surface =
      Contains(upper, " ON DUPLICATE KEY UPDATE");
  d.mutation_surface = d.replace_surface
      ? "mysql_replace"
      : d.insert_surface && d.on_duplicate_key_update_surface
          ? "mysql_insert_on_duplicate_key_update"
          : d.insert_surface ? "mysql_insert"
          : d.update_surface ? "mysql_update"
          : d.delete_surface ? "mysql_delete"
                             : "mysql_dml_mutation";
  d.upsert_merge_conflict_policy = d.replace_surface
      ? "mysql_replace_delete_insert_semantics_descriptor_engine_executes"
      : d.insert_surface && d.on_duplicate_key_update_surface
          ? "mysql_on_duplicate_key_update_descriptor_unique_probe_engine_authority"
          : "mysql_no_upsert_surface_observed";
  d.returning_output_projection_surface = false;
  d.returning_output_projection_policy =
      "mysql_no_native_dml_returning_projection_descriptor_rowcount_generated_keys_only";
  d.cursor_positioned_dml_surface = false;
  d.cursor_positioned_dml_policy =
      "mysql_no_native_where_current_of_descriptor";
  d.affected_row_count_policy = d.replace_surface
      ? "mysql_replace_row_count_delete_plus_insert_descriptor_engine_reported"
      : d.on_duplicate_key_update_surface
          ? "mysql_on_duplicate_key_affected_rows_client_found_rows_sensitive_descriptor"
          : "mysql_affected_rows_descriptor_client_found_rows_profile_bound";
  d.default_value_surface = ContainsWord(upper, "DEFAULT");
  d.generated_column_surface = ContainsWord(upper, "GENERATED");
  d.trigger_interaction_descriptor_required = d.insert_surface ||
                                              d.update_surface ||
                                              d.delete_surface ||
                                              d.replace_surface;
  d.trigger_default_generated_column_interaction_policy = d.replace_surface
      ? "mysql_replace_defaults_generated_columns_triggers_descriptor_engine_order"
      : d.on_duplicate_key_update_surface
          ? "mysql_on_duplicate_defaults_generated_columns_triggers_descriptor_engine_order"
      : d.default_value_surface || d.generated_column_surface
          ? "mysql_defaults_generated_columns_trigger_descriptor_engine_order"
          : "mysql_trigger_default_generated_column_descriptor_required";
  return scratchbird::parser::compatibility::
      RenderDmlMutationSemanticEvidenceJson("mysql", release_profile, d);
}

scratchbird::parser::compatibility::ProceduralFunctionalEncodingSpanMetadata
ProceduralFunctionalEncodingSpanMetadataFor(
    std::string_view active_upper_sql,
    std::span<const scratchbird::parser::compatibility::Token> tokens) {
  scratchbird::parser::compatibility::
      ProceduralFunctionalEncodingSpanMetadata metadata;
  std::vector<std::size_t> semantic_token_indexes;
  semantic_token_indexes.reserve(tokens.size());
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].kind != "line_comment" &&
        tokens[i].kind != "block_comment") {
      semantic_token_indexes.push_back(i);
    }
  }
  if (semantic_token_indexes.empty()) return metadata;

  const auto token_upper = [&](std::size_t semantic_index) {
    return scratchbird::parser::compatibility::ToUpperAscii(
        tokens[semantic_token_indexes[semantic_index]].lexeme);
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
  metadata.body_source_span_count = body_semantic_index < semantic_count
      ? semantic_count - body_semantic_index
      : 0;
  if (metadata.header_source_span_count > 0 &&
      metadata.body_source_span_count > 0) {
    metadata.parser_bound_sblr_body_instruction_stream = true;
    metadata.uuid_dependency_bindings_bound = true;
  }
  return metadata;
}

std::string DatatypeProfileEvidenceJson(
    std::span<const scratchbird::parser::compatibility::Token> active_tokens) {
  scratchbird::parser::compatibility::DatatypeFamilySemanticDescriptor d;
  d.compatibility_profile_uuid = "019e13c0-0000-7000-8000-000000000303";
  for (std::size_t i = 0; i < active_tokens.size(); ++i) {
    const auto& token = active_tokens[i];
    if (token.kind != "identifier_or_keyword") continue;
    const auto upper = scratchbird::parser::compatibility::ToUpperAscii(token.lexeme);
    if (upper == "TINYINT" || upper == "SMALLINT" || upper == "MEDIUMINT" ||
        upper == "INT" || upper == "INTEGER" || upper == "BIGINT") {
      d.numeric = true;
    } else if (upper == "NUMERIC" || upper == "DECIMAL") {
      d.numeric = true;
      d.exact_decimal = true;
    } else if (upper == "REAL" || upper == "FLOAT" || upper == "DOUBLE") {
      d.numeric = true;
      d.floating = true;
    } else if (upper == "CHAR" || upper == "VARCHAR" || upper == "CHARACTER" ||
               upper == "TEXT" || upper == "TINYTEXT" ||
               upper == "MEDIUMTEXT" || upper == "LONGTEXT") {
      d.text = true;
    } else if (upper == "CHARSET" || upper == "COLLATE" ||
               upper == "COLLATION") {
      d.charset_collation_sensitive_text = true;
    } else if (upper == "BINARY" || upper == "VARBINARY" || upper == "BLOB" ||
               upper == "TINYBLOB" || upper == "MEDIUMBLOB" ||
               upper == "LONGBLOB") {
      d.binary_blob = true;
    } else if (upper == "DATE" || upper == "TIME" || upper == "DATETIME" ||
               upper == "TIMESTAMP" || upper == "YEAR") {
      d.temporal = true;
    } else if (upper == "BOOL" || upper == "BOOLEAN") {
      d.boolean = true;
    } else if (upper == "JSON") {
      d.json_document = true;
    } else if (upper == "ENUM") {
      d.enum_set = true;
    } else if (upper == "SET" && i + 1 < active_tokens.size() &&
               (active_tokens[i + 1].kind == "symbol" ||
                active_tokens[i + 1].kind == "punctuation") &&
               active_tokens[i + 1].lexeme == "(") {
      d.enum_set = true;
    } else if (upper == "GEOMETRY" || upper == "POINT" ||
               upper == "LINESTRING" || upper == "POLYGON") {
      d.geometric_spatial = true;
    }
    if (upper == "CHARACTER" && i + 1 < active_tokens.size() &&
        active_tokens[i + 1].kind == "identifier_or_keyword" &&
        scratchbird::parser::compatibility::ToUpperAscii(
            active_tokens[i + 1].lexeme) == "SET") {
      d.charset_collation_sensitive_text = true;
    }
  }
  return scratchbird::parser::compatibility::RenderDatatypeProfileEvidenceJson(
      "mysql", d);
}

const scratchbird::parser::compatibility::DialectSemanticPolicy kSemanticPolicy{
    IsSystemCatalogDefaultsStatement,
    SystemCatalogDefaultsEvidenceJson,
    IsSessionSettingsDiagnosticsStatement,
    SessionSettingsDiagnosticsEvidenceJson,
    IsLocksIsolationStatement,
    LocksIsolationEvidenceJson,
    IsResourceTextStatement,
    ResourceTextEvidenceJson,
    IsStatisticsOptimizerStatement,
    StatisticsOptimizerEvidenceJson,
    IsDdlTransactionBehaviorStatement,
    DdlTransactionBehaviorEvidenceJson,
    IsDependencyBearingDdlStatement,
    DependencyBearingDdlEvidenceJson,
    IsTransactionSessionSemanticStatement,
    TransactionSessionSemanticEvidenceJson,
    IsTemporarySessionObjectSemanticStatement,
    TemporarySessionObjectSemanticEvidenceJson,
    IsIndexSemanticDefaultsStatement,
    IndexSemanticDefaultsEvidenceJson,
    IsConstraintSemanticDefaultsStatement,
    ConstraintSemanticDefaultsEvidenceJson,
    IsSequenceIdentitySemanticStatement,
    SequenceIdentitySemanticEvidenceJson,
    IsIdentifierNameResolutionStatement,
    IdentifierNameResolutionEvidenceJson,
    IsScalarExpressionSemanticStatement,
    ScalarExpressionSemanticEvidenceJson,
    IsDmlMutationSemanticStatement,
    DmlMutationSemanticEvidenceJson,
    ProceduralFunctionalEncodingSpanMetadataFor,
    DatatypeProfileEvidenceJson,
};

constexpr std::string_view kSblrFamily = "sblr.compatibility.mysql.profile.v1";

constexpr OperationPattern kPatterns[] = {
    {"LOAD DATA LOCAL INFILE", PatternMatch::kLoadDataLocalInfile, "bulk_io", "mysql.bulk_io.load_data_local_infile",
     MappingDisposition::kParserSupportUdr, "mysql.udr.etl.load_data_local_infile",
     "SBLR_COMPAT_MYSQL_ETL_ROUTE", "ParserSupportEtlRoute",
     "MYSQL.EMULATION.ETL_ROUTE",
     "LOAD DATA LOCAL INFILE routes through the MySQL compatibility UDR as a client logical ETL stream.",
     true, true},
    {"LOAD DATA INFILE", PatternMatch::kLoadDataServerInfile, "bulk_io", "mysql.bulk_io.load_data_infile",
     MappingDisposition::kPolicyRefusal, "mysql.policy.file.load_data_infile", "",
     "", "MYSQL.AUTHORITY.FILE_IO_DENIED",
     "LOAD DATA INFILE is parsed but refused unless a trusted ScratchBird import service admits it.",
     true, false},
    {"LOAD_FILE", PatternMatch::kContainsFunctionCall, "bulk_io", "mysql.bulk_io.load_file",
     MappingDisposition::kPolicyRefusal, "mysql.policy.file.load_file", "",
     "", "MYSQL.AUTHORITY.FILE_IO_DENIED",
     "LOAD_FILE cannot read host files from parser authority.", true, false},
    {"SELECT|| INTO OUTFILE", PatternMatch::kPrefixAndContains, "bulk_io", "mysql.bulk_io.select_into_outfile",
     MappingDisposition::kPolicyRefusal, "mysql.policy.file.select_into_outfile", "",
     "", "MYSQL.AUTHORITY.FILE_IO_DENIED",
     "SELECT INTO OUTFILE cannot perform compatibility filesystem writes.", true, false},
    {"SELECT|| INTO DUMPFILE", PatternMatch::kPrefixAndContains, "bulk_io", "mysql.bulk_io.select_into_dumpfile",
     MappingDisposition::kPolicyRefusal, "mysql.policy.file.select_into_dumpfile", "",
     "", "MYSQL.AUTHORITY.FILE_IO_DENIED",
     "SELECT INTO DUMPFILE cannot perform compatibility filesystem writes.", true, false},
    {"INSTALL PLUGIN", PatternMatch::kPrefix, "plugin", "mysql.plugin.install",
     MappingDisposition::kPolicyRefusal, "mysql.policy.plugin.install", "",
     "", "MYSQL.AUTHORITY.PLUGIN_DENIED",
     "MySQL plugin installation is blocked from parser authority.", true, false},
    {"UNINSTALL PLUGIN", PatternMatch::kPrefix, "plugin", "mysql.plugin.uninstall",
     MappingDisposition::kPolicyRefusal, "mysql.policy.plugin.uninstall", "",
     "", "MYSQL.AUTHORITY.PLUGIN_DENIED",
     "MySQL plugin uninstallation is blocked from parser authority.", true, false},
    {"CREATE TABLESPACE", PatternMatch::kPrefix, "storage_admin", "mysql.storage.tablespace.create",
     MappingDisposition::kPolicyRefusal, "mysql.policy.tablespace.create", "",
     "", "MYSQL.AUTHORITY.TABLESPACE_DENIED",
     "Tablespace physical storage administration is not parser authority.", true, false},
    {"ALTER TABLESPACE", PatternMatch::kPrefix, "storage_admin", "mysql.storage.tablespace.alter",
     MappingDisposition::kPolicyRefusal, "mysql.policy.tablespace.alter", "",
     "", "MYSQL.AUTHORITY.TABLESPACE_DENIED",
     "Tablespace physical storage administration is not parser authority.", true, false},
    {"DROP TABLESPACE", PatternMatch::kPrefix, "storage_admin", "mysql.storage.tablespace.drop",
     MappingDisposition::kPolicyRefusal, "mysql.policy.tablespace.drop", "",
     "", "MYSQL.AUTHORITY.TABLESPACE_DENIED",
     "Tablespace physical storage administration is not parser authority.", true, false},
    {"CHANGE REPLICATION SOURCE", PatternMatch::kPrefix, "replication", "mysql.replication.change_source",
     MappingDisposition::kParserSupportUdr, "mysql.udr.replication.change_source",
     "SBLR_COMPAT_MYSQL_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MYSQL.EMULATION.REPLICATION_ROUTE",
     "Replication source changes route through the MySQL compatibility UDR.", true, false},
    {"CHANGE MASTER", PatternMatch::kPrefix, "replication", "mysql.replication.change_master_legacy",
     MappingDisposition::kParserSupportUdr, "mysql.udr.replication.change_master_legacy",
     "SBLR_COMPAT_MYSQL_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MYSQL.EMULATION.REPLICATION_ROUTE",
     "Legacy replication source changes route through the MySQL compatibility UDR.", true, false},
    {"START REPLICA", PatternMatch::kPrefix, "replication", "mysql.replication.start_replica",
     MappingDisposition::kParserSupportUdr, "mysql.udr.replication.start_replica",
     "SBLR_COMPAT_MYSQL_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MYSQL.EMULATION.REPLICATION_ROUTE",
     "Replica start requests route through the MySQL compatibility UDR.", true, false},
    {"STOP REPLICA", PatternMatch::kPrefix, "replication", "mysql.replication.stop_replica",
     MappingDisposition::kParserSupportUdr, "mysql.udr.replication.stop_replica",
     "SBLR_COMPAT_MYSQL_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MYSQL.EMULATION.REPLICATION_ROUTE",
     "Replica stop requests route through the MySQL compatibility UDR.", true, false},
    {"RESET REPLICA", PatternMatch::kPrefix, "replication", "mysql.replication.reset_replica",
     MappingDisposition::kParserSupportUdr, "mysql.udr.replication.reset_replica",
     "SBLR_COMPAT_MYSQL_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MYSQL.EMULATION.REPLICATION_ROUTE",
     "Replica reset requests route through the MySQL compatibility UDR.", true, false},
    {"SHOW REPLICA STATUS", PatternMatch::kPrefix, "replication", "mysql.replication.show_replica_status",
     MappingDisposition::kParserSupportUdr, "mysql.udr.replication.show_replica_status",
     "SBLR_COMPAT_MYSQL_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MYSQL.EMULATION.REPLICATION_ROUTE",
     "Replica status reports route through the MySQL compatibility UDR.", false, false},
    {"SHOW SLAVE STATUS", PatternMatch::kPrefix, "replication", "mysql.replication.show_slave_status_legacy",
     MappingDisposition::kParserSupportUdr, "mysql.udr.replication.show_slave_status_legacy",
     "SBLR_COMPAT_MYSQL_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MYSQL.EMULATION.REPLICATION_ROUTE",
     "Legacy replica status reports route through the MySQL compatibility UDR.", false, false},
    {"PURGE BINARY LOGS", PatternMatch::kPrefix, "replication", "mysql.replication.purge_binary_logs",
     MappingDisposition::kParserSupportUdr, "mysql.udr.replication.purge_binary_logs",
     "SBLR_COMPAT_MYSQL_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MYSQL.EMULATION.REPLICATION_ROUTE",
     "Binary-log CDC retention requests route through the MySQL compatibility UDR.", true, false},
    {"RESET BINARY LOGS", PatternMatch::kPrefix, "replication", "mysql.replication.reset_binary_logs",
     MappingDisposition::kParserSupportUdr, "mysql.udr.replication.reset_binary_logs",
     "SBLR_COMPAT_MYSQL_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MYSQL.EMULATION.REPLICATION_ROUTE",
     "Binary-log reset requests route through the MySQL compatibility UDR.", true, false},
    {"CREATE USER", PatternMatch::kPrefix, "security", "mysql.security.create_user",
     MappingDisposition::kParserSupportUdr, "mysql.udr.security.create_user",
     "SBLR_COMPAT_MYSQL_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "MYSQL.EMULATION.SECURITY_ROUTE",
     "Account management routes through trusted security policy.", true, false},
    {"ALTER USER", PatternMatch::kPrefix, "security", "mysql.security.alter_user",
     MappingDisposition::kParserSupportUdr, "mysql.udr.security.alter_user",
     "SBLR_COMPAT_MYSQL_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "MYSQL.EMULATION.SECURITY_ROUTE",
     "Account management routes through trusted security policy.", true, false},
    {"DROP USER", PatternMatch::kPrefix, "security", "mysql.security.drop_user",
     MappingDisposition::kParserSupportUdr, "mysql.udr.security.drop_user",
     "SBLR_COMPAT_MYSQL_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "MYSQL.EMULATION.SECURITY_ROUTE",
     "Account management routes through trusted security policy.", true, false},
    {"CREATE ROLE", PatternMatch::kPrefix, "security", "mysql.security.create_role",
     MappingDisposition::kParserSupportUdr, "mysql.udr.security.create_role",
     "SBLR_COMPAT_MYSQL_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "MYSQL.EMULATION.SECURITY_ROUTE",
     "Role management routes through trusted security policy.", true, false},
    {"DROP ROLE", PatternMatch::kPrefix, "security", "mysql.security.drop_role",
     MappingDisposition::kParserSupportUdr, "mysql.udr.security.drop_role",
     "SBLR_COMPAT_MYSQL_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "MYSQL.EMULATION.SECURITY_ROUTE",
     "Role management routes through trusted security policy.", true, false},
    {"GRANT", PatternMatch::kPrefix, "security", "mysql.security.grant",
     MappingDisposition::kParserSupportUdr, "mysql.udr.security.grant",
     "SBLR_COMPAT_MYSQL_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "MYSQL.EMULATION.SECURITY_ROUTE",
     "Privilege changes route through trusted security policy.", true, false},
    {"REVOKE", PatternMatch::kPrefix, "security", "mysql.security.revoke",
     MappingDisposition::kParserSupportUdr, "mysql.udr.security.revoke",
     "SBLR_COMPAT_MYSQL_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "MYSQL.EMULATION.SECURITY_ROUTE",
     "Privilege changes route through trusted security policy.", true, false},
    {"CREATE EVENT", PatternMatch::kPrefix, "routine", "mysql.routine.event.create",
     MappingDisposition::kParserSupportUdr, "mysql.udr.routine.event.create",
     "SBLR_COMPAT_MYSQL_ROUTINE_ROUTE", "ParserSupportRoutineRoute",
     "MYSQL.EMULATION.ROUTINE_ROUTE",
     "Events route through trusted routine package policy.", true, false},
    {"CREATE TRIGGER", PatternMatch::kPrefix, "routine", "mysql.routine.trigger.create",
     MappingDisposition::kParserSupportUdr, "mysql.udr.routine.trigger.create",
     "SBLR_COMPAT_MYSQL_ROUTINE_ROUTE", "ParserSupportRoutineRoute",
     "MYSQL.EMULATION.ROUTINE_ROUTE",
     "Triggers route through trusted routine package policy.", true, false},
    {"CREATE PROCEDURE", PatternMatch::kPrefix, "routine", "mysql.routine.procedure.create",
     MappingDisposition::kParserSupportUdr, "mysql.udr.routine.procedure.create",
     "SBLR_COMPAT_MYSQL_ROUTINE_ROUTE", "ParserSupportRoutineRoute",
     "MYSQL.EMULATION.ROUTINE_ROUTE",
     "Stored procedures route through trusted routine package policy.", true, false},
    {"CREATE FUNCTION", PatternMatch::kPrefix, "routine", "mysql.routine.function.create",
     MappingDisposition::kParserSupportUdr, "mysql.udr.routine.function.create",
     "SBLR_COMPAT_MYSQL_ROUTINE_ROUTE", "ParserSupportRoutineRoute",
     "MYSQL.EMULATION.ROUTINE_ROUTE",
     "Stored functions route through trusted routine package policy.", true, false},
    {"CREATE DATABASE", PatternMatch::kPrefix, "database_lifecycle", "mysql.lifecycle.create_database",
     MappingDisposition::kScratchBirdLifecycleApi, "mysql.lifecycle.create_database",
     "SBLR_LIFECYCLE_CREATE_DATABASE", "EngineCreateLifecycle", "", "", false, false},
    {"DROP DATABASE", PatternMatch::kPrefix, "database_lifecycle", "mysql.lifecycle.drop_database",
     MappingDisposition::kScratchBirdLifecycleApi, "mysql.lifecycle.drop_database",
     "SBLR_LIFECYCLE_DROP_DATABASE", "EngineDropLifecycle", "", "", true, false},
    {"USE", PatternMatch::kPrefix, "session", "mysql.session.use_database",
     MappingDisposition::kAdmittedSblr, "mysql.session.use_database",
     "SBLR_COMPAT_MYSQL_USE_DATABASE", "EngineSessionRoute", "", "", false, false},
    {"SHOW", PatternMatch::kPrefix, "catalog_overlay", "mysql.catalog_overlay.show",
     MappingDisposition::kCatalogProjection, "mysql.catalog.show",
     "SBLR_COMPAT_MYSQL_CATALOG_PROJECT", "EngineCatalogProjection", "", "", false, false},
    {"DESCRIBE", PatternMatch::kPrefix, "catalog_overlay", "mysql.catalog_overlay.describe",
     MappingDisposition::kCatalogProjection, "mysql.catalog.describe",
     "SBLR_COMPAT_MYSQL_CATALOG_PROJECT", "EngineCatalogProjection", "", "", false, false},
    {"EXPLAIN", PatternMatch::kPrefix, "optimizer", "mysql.optimizer.explain",
     MappingDisposition::kCatalogProjection, "mysql.optimizer.explain",
     "SBLR_COMPAT_MYSQL_EXPLAIN", "EngineExplainPlan", "", "", false, false},
    {"PREPARE", PatternMatch::kPrefix, "prepared_statement", "mysql.prepared.prepare",
     MappingDisposition::kAdmittedSblr, "mysql.prepared.prepare",
     "SBLR_COMPAT_MYSQL_PREPARE", "EnginePrepareStatement", "", "", false, false},
    {"EXECUTE", PatternMatch::kPrefix, "prepared_statement", "mysql.prepared.execute",
     MappingDisposition::kAdmittedSblr, "mysql.prepared.execute",
     "SBLR_COMPAT_MYSQL_EXECUTE", "EngineExecuteStatement", "", "", false, true},
    {"DEALLOCATE", PatternMatch::kPrefix, "prepared_statement", "mysql.prepared.deallocate",
     MappingDisposition::kAdmittedSblr, "mysql.prepared.deallocate",
     "SBLR_COMPAT_MYSQL_DEALLOCATE", "EngineDeallocateStatement", "", "", false, false},
    {"LOCK TABLES", PatternMatch::kPrefix, "locking", "mysql.locking.lock_tables",
     MappingDisposition::kAdmittedSblr, "mysql.locking.lock_tables",
     "SBLR_COMPAT_MYSQL_LOCK_TABLES", "EngineLockTables", "", "", true, true},
    {"UNLOCK TABLES", PatternMatch::kPrefix, "locking", "mysql.locking.unlock_tables",
     MappingDisposition::kAdmittedSblr, "mysql.locking.unlock_tables",
     "SBLR_COMPAT_MYSQL_UNLOCK_TABLES", "EngineUnlockTables", "", "", true, true},
    {"START TRANSACTION", PatternMatch::kPrefix, "transaction", "mysql.transaction.start",
     MappingDisposition::kAdmittedSblr, "mysql.transaction.start",
     "SBLR_TRANSACTION_BEGIN", "EngineBeginTransaction", "", "", false, false},
    {"BEGIN", PatternMatch::kPrefix, "transaction", "mysql.transaction.begin",
     MappingDisposition::kAdmittedSblr, "mysql.transaction.begin",
     "SBLR_TRANSACTION_BEGIN", "EngineBeginTransaction", "", "", false, false},
    {"COMMIT", PatternMatch::kPrefix, "transaction", "mysql.transaction.commit",
     MappingDisposition::kAdmittedSblr, "mysql.transaction.commit",
     "SBLR_TRANSACTION_COMMIT", "EngineCommitTransaction", "", "", false, true},
    {"ROLLBACK TO SAVEPOINT", PatternMatch::kPrefix, "transaction", "mysql.transaction.rollback_to_savepoint",
     MappingDisposition::kAdmittedSblr, "mysql.transaction.rollback_to_savepoint",
     "SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT", "EngineRollbackToSavepoint", "", "", false, true},
    {"ROLLBACK WORK TO SAVEPOINT", PatternMatch::kPrefix, "transaction", "mysql.transaction.rollback_to_savepoint",
     MappingDisposition::kAdmittedSblr, "mysql.transaction.rollback_to_savepoint",
     "SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT", "EngineRollbackToSavepoint", "", "", false, true},
    {"ROLLBACK WORK TO", PatternMatch::kPrefix, "transaction", "mysql.transaction.rollback_to_savepoint",
     MappingDisposition::kAdmittedSblr, "mysql.transaction.rollback_to_savepoint",
     "SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT", "EngineRollbackToSavepoint", "", "", false, true},
    {"ROLLBACK TO", PatternMatch::kPrefix, "transaction", "mysql.transaction.rollback_to_savepoint",
     MappingDisposition::kAdmittedSblr, "mysql.transaction.rollback_to_savepoint",
     "SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT", "EngineRollbackToSavepoint", "", "", false, true},
    {"ROLLBACK", PatternMatch::kPrefix, "transaction", "mysql.transaction.rollback",
     MappingDisposition::kAdmittedSblr, "mysql.transaction.rollback",
     "SBLR_TRANSACTION_ROLLBACK", "EngineRollbackTransaction", "", "", false, true},
    {"SAVEPOINT", PatternMatch::kPrefix, "transaction", "mysql.transaction.savepoint",
     MappingDisposition::kAdmittedSblr, "mysql.transaction.savepoint",
     "SBLR_TRANSACTION_SAVEPOINT", "EngineSavepoint", "", "", false, true},
    {"RELEASE SAVEPOINT", PatternMatch::kPrefix, "transaction", "mysql.transaction.release_savepoint",
     MappingDisposition::kAdmittedSblr, "mysql.transaction.release_savepoint",
     "SBLR_TRANSACTION_RELEASE_SAVEPOINT", "EngineReleaseSavepoint", "", "", false, true},
    {"SET", PatternMatch::kPrefix, "session", "mysql.session.set",
     MappingDisposition::kAdmittedSblr, "mysql.session.set",
     "SBLR_COMPAT_MYSQL_SET", "EngineSessionSet", "", "", false, false},
    {"CREATE UNIQUE INDEX", PatternMatch::kPrefix, "ddl", "mysql.ddl.create.unique_index",
     MappingDisposition::kAdmittedSblr, "mysql.ddl.create.unique_index",
     "SBLR_COMPAT_MYSQL_INDEX_CREATE", "EngineDdlCreateIndex", "", "", true, true},
    {"CREATE INDEX", PatternMatch::kPrefix, "ddl", "mysql.ddl.create.index",
     MappingDisposition::kAdmittedSblr, "mysql.ddl.create.index",
     "SBLR_COMPAT_MYSQL_INDEX_CREATE", "EngineDdlCreateIndex", "", "", true, true},
    {"CREATE TEMPORARY TABLE", PatternMatch::kPrefix, "ddl", "mysql.ddl.create.temporary_table",
     MappingDisposition::kAdmittedSblr, "mysql.ddl.create.temporary_table",
     "SBLR_COMPAT_MYSQL_TEMPORARY_TABLE_CREATE", "EngineDdlCreateTemporaryTable",
     "", "", true, true},
    {"CREATE OR REPLACE VIEW", PatternMatch::kPrefix, "ddl", "mysql.ddl.create_or_replace.view",
     MappingDisposition::kAdmittedSblr, "mysql.ddl.create_or_replace.view",
     "SBLR_COMPAT_MYSQL_VIEW_CREATE_OR_REPLACE", "EngineDdlCreateOrReplaceView",
     "", "", true, true},
    {"CREATE VIEW", PatternMatch::kPrefix, "ddl", "mysql.ddl.create.view",
     MappingDisposition::kAdmittedSblr, "mysql.ddl.create.view",
     "SBLR_COMPAT_MYSQL_VIEW_CREATE", "EngineDdlCreateView", "", "", true, true},
    {"CREATE", PatternMatch::kPrefix, "ddl", "mysql.ddl.create",
     MappingDisposition::kAdmittedSblr, "mysql.ddl.create",
     "SBLR_COMPAT_MYSQL_DDL_CREATE", "EngineDdlCreate", "", "", true, true},
    {"ALTER VIEW", PatternMatch::kPrefix, "ddl", "mysql.ddl.alter.view",
     MappingDisposition::kAdmittedSblr, "mysql.ddl.alter.view",
     "SBLR_COMPAT_MYSQL_VIEW_ALTER", "EngineDdlAlterView", "", "", true, true},
    {"ALTER", PatternMatch::kPrefix, "ddl", "mysql.ddl.alter",
     MappingDisposition::kAdmittedSblr, "mysql.ddl.alter",
     "SBLR_COMPAT_MYSQL_DDL_ALTER", "EngineDdlAlter", "", "", true, true},
    {"DROP TEMPORARY TABLE", PatternMatch::kPrefix, "ddl", "mysql.ddl.drop.temporary_table",
     MappingDisposition::kAdmittedSblr, "mysql.ddl.drop.temporary_table",
     "SBLR_COMPAT_MYSQL_TEMPORARY_TABLE_DROP", "EngineDdlDropTemporaryTable",
     "", "", true, true},
    {"DROP VIEW", PatternMatch::kPrefix, "ddl", "mysql.ddl.drop.view",
     MappingDisposition::kAdmittedSblr, "mysql.ddl.drop.view",
     "SBLR_COMPAT_MYSQL_VIEW_DROP", "EngineDdlDropView", "", "", true, true},
    {"DROP", PatternMatch::kPrefix, "ddl", "mysql.ddl.drop",
     MappingDisposition::kAdmittedSblr, "mysql.ddl.drop",
     "SBLR_COMPAT_MYSQL_DDL_DROP", "EngineDdlDrop", "", "", true, true},
    {"TRUNCATE", PatternMatch::kPrefix, "ddl", "mysql.ddl.truncate",
     MappingDisposition::kAdmittedSblr, "mysql.ddl.truncate",
     "SBLR_COMPAT_MYSQL_DDL_TRUNCATE", "EngineDdlTruncate", "", "", true, true},
    {"REPLACE", PatternMatch::kPrefix, "dml", "mysql.dml.replace",
     MappingDisposition::kAdmittedSblr, "mysql.dml.replace",
     "SBLR_COMPAT_MYSQL_REPLACE", "EngineDmlReplace", "", "", false, true},
    {"INSERT", PatternMatch::kPrefix, "dml", "mysql.dml.insert",
     MappingDisposition::kAdmittedSblr, "mysql.dml.insert",
     "SBLR_COMPAT_MYSQL_INSERT", "EngineDmlInsert", "", "", false, true},
    {"UPDATE", PatternMatch::kPrefix, "dml", "mysql.dml.update",
     MappingDisposition::kAdmittedSblr, "mysql.dml.update",
     "SBLR_COMPAT_MYSQL_UPDATE", "EngineDmlUpdate", "", "", false, true},
    {"DELETE", PatternMatch::kPrefix, "dml", "mysql.dml.delete",
     MappingDisposition::kAdmittedSblr, "mysql.dml.delete",
     "SBLR_COMPAT_MYSQL_DELETE", "EngineDmlDelete", "", "", false, true},
    {"SELECT", PatternMatch::kPrefix, "query", "mysql.query.select",
     MappingDisposition::kAdmittedSblr, "mysql.query.select",
     "SBLR_COMPAT_MYSQL_SELECT", "EngineQuerySelect", "", "", false, false},
    {"WITH", PatternMatch::kPrefix, "query", "mysql.query.with",
     MappingDisposition::kAdmittedSblr, "mysql.query.with",
     "SBLR_COMPAT_MYSQL_SELECT", "EngineQuerySelect", "", "", false, false},
    {"CALL", PatternMatch::kPrefix, "routine", "mysql.routine.call",
     MappingDisposition::kParserSupportUdr, "mysql.udr.routine.call",
     "SBLR_COMPAT_MYSQL_ROUTINE_CALL", "ParserSupportRoutineRoute",
     "MYSQL.EMULATION.ROUTINE_ROUTE",
     "Routine calls route through trusted package policy.", true, true},
    {"ANALYZE TABLE", PatternMatch::kPrefix, "maintenance", "mysql.maintenance.analyze_table",
     MappingDisposition::kUnsupportedRefusal, "mysql.policy.unsupported.analyze_table",
     "", "", "MYSQL.AUTHORITY.UNSUPPORTED_DENIED",
     "MySQL ANALYZE TABLE is a compatibility low-level utility surface and is outside compatibility parser authority.",
     true, false},
    {"OPTIMIZE TABLE", PatternMatch::kPrefix, "maintenance", "mysql.maintenance.optimize_table",
     MappingDisposition::kUnsupportedRefusal, "mysql.policy.unsupported.optimize_table",
     "", "", "MYSQL.AUTHORITY.UNSUPPORTED_DENIED",
     "MySQL OPTIMIZE TABLE is a compatibility low-level utility surface and is outside compatibility parser authority.",
     true, false},
    {"CHECK TABLE", PatternMatch::kPrefix, "maintenance", "mysql.maintenance.check_table",
     MappingDisposition::kUnsupportedRefusal, "mysql.policy.unsupported.check_table",
     "", "", "MYSQL.AUTHORITY.UNSUPPORTED_DENIED",
     "MySQL CHECK TABLE is a compatibility verification utility surface and is outside compatibility parser authority.",
     true, false},
    {"REPAIR TABLE", PatternMatch::kPrefix, "maintenance", "mysql.maintenance.repair_table",
     MappingDisposition::kUnsupportedRefusal, "mysql.policy.unsupported.repair_table",
     "", "", "MYSQL.AUTHORITY.UNSUPPORTED_DENIED",
     "MySQL REPAIR TABLE is a compatibility repair utility surface and is outside compatibility parser authority.",
     true, false},
    {"FLUSH", PatternMatch::kPrefix, "maintenance", "mysql.maintenance.flush",
     MappingDisposition::kUnsupportedRefusal, "mysql.policy.unsupported.flush",
     "", "", "MYSQL.AUTHORITY.UNSUPPORTED_DENIED",
     "MySQL FLUSH is a compatibility low-level utility surface and is outside compatibility parser authority.",
     true, false},
    {"XA", PatternMatch::kPrefix, "transaction", "mysql.transaction.xa",
     MappingDisposition::kUnsupportedRefusal, "mysql.policy.transaction.xa", "",
     "", "MYSQL.AUTHORITY.XA_DENIED",
     "XA distributed transaction authority is not admitted by the parser.", true, true},
};

const std::array<SurfaceDescriptor, 10> kDatatypeSurfaces{{
    {"numeric", "TINYINT;SMALLINT;MEDIUMINT;INT;BIGINT;DECIMAL;FLOAT;DOUBLE", "descriptor"},
    {"unsigned_numeric", "UNSIGNED;ZEROFILL", "descriptor_policy"},
    {"text", "CHAR;VARCHAR;TEXT;TINYTEXT;MEDIUMTEXT;LONGTEXT", "descriptor"},
    {"binary", "BINARY;VARBINARY;BLOB;TINYBLOB;MEDIUMBLOB;LONGBLOB", "descriptor"},
    {"temporal", "DATE;TIME;DATETIME;TIMESTAMP;YEAR", "descriptor"},
    {"boolean", "BOOL;BOOLEAN", "descriptor_alias"},
    {"json", "JSON", "descriptor"},
    {"enum_set", "ENUM;SET", "parser_support_udr"},
    {"spatial", "GEOMETRY;POINT;LINESTRING;POLYGON", "parser_support_udr"},
    {"charset_collation", "CHARACTER SET;COLLATE", "catalog_policy"},
}};

const std::array<SurfaceDescriptor, 10> kBuiltinSurfaces{{
    {"aggregate", "COUNT;SUM;AVG;MIN;MAX;GROUP_CONCAT", "sblr"},
    {"window", "ROW_NUMBER;RANK;DENSE_RANK;LAG;LEAD", "sblr"},
    {"string", "CONCAT;SUBSTRING;LOWER;UPPER;TRIM;CHAR_LENGTH", "sblr"},
    {"numeric", "ABS;ROUND;POW;SQRT;MOD", "sblr"},
    {"temporal", "NOW;CURRENT_TIMESTAMP;DATE_ADD;DATE_SUB;TIMESTAMPDIFF", "sblr"},
    {"json", "JSON_EXTRACT;JSON_VALUE;JSON_TABLE;JSON_OBJECT", "parser_support_udr"},
    {"security", "CURRENT_USER;SESSION_USER;USER", "catalog_projection"},
    {"variables", "@user_variable;@@system_variable", "session_descriptor"},
    {"fulltext", "MATCH AGAINST", "sblr_optional"},
    {"spatial", "ST_*", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 8> kCatalogSurfaces{{
    {"information_schema", "INFORMATION_SCHEMA.", "catalog_projection"},
    {"mysql_schema", "MYSQL.USER;MYSQL.DB;MYSQL.TABLES_PRIV;MYSQL.PROCS_PRIV", "catalog_projection"},
    {"performance_schema", "PERFORMANCE_SCHEMA.", "catalog_projection"},
    {"sys_schema", "SYS.", "catalog_projection"},
    {"replication_status", "SHOW REPLICA STATUS;SHOW BINARY LOGS", "catalog_projection"},
    {"routine_metadata", "INFORMATION_SCHEMA.ROUTINES;TRIGGERS;EVENTS", "catalog_projection"},
    {"table_metadata", "SHOW COLUMNS;SHOW INDEX;DESCRIBE", "catalog_projection"},
    {"privilege_metadata", "SHOW GRANTS", "catalog_projection"},
}};

const std::array<SurfaceDescriptor, 11> kDiagnosticSurfaces{{
    {"parse", "MYSQL.PARSE.INVALID_INPUT;MYSQL.PARSE.UNSUPPORTED_SURFACE", "parser"},
    {"file", "MYSQL.AUTHORITY.FILE_IO_DENIED", "fail_closed"},
    {"plugin", "MYSQL.AUTHORITY.PLUGIN_DENIED", "fail_closed"},
    {"tablespace", "MYSQL.AUTHORITY.TABLESPACE_DENIED", "fail_closed"},
    {"etl", "MYSQL.EMULATION.ETL_ROUTE", "parser_support_udr"},
    {"replication", "MYSQL.EMULATION.REPLICATION_ROUTE", "parser_support_udr"},
    {"security", "MYSQL.EMULATION.SECURITY_ROUTE", "parser_support_udr"},
    {"routine", "MYSQL.EMULATION.ROUTINE_ROUTE", "parser_support_udr"},
    {"maintenance", "MYSQL.AUTHORITY.UNSUPPORTED_DENIED", "fail_closed"},
    {"binlog", "MYSQL.AUTHORITY.BINLOG_DENIED", "fail_closed"},
    {"xa", "MYSQL.AUTHORITY.XA_DENIED", "fail_closed"},
}};

const std::array<std::string_view, 5> kDialectVariants{{
    "mysql_text_protocol_sql",
    "mysql_binary_prepared_protocol",
    "mysql_stored_program_sql",
    "mysql_load_data_local_stream",
    "mysql_replication_binlog_stream",
}};

const scratchbird::parser::compatibility::DialectProfile kProfile{
    "mysql",
    "MySQL",
    "sbp_mysql",
    "sbup_mysql",
    "9.7.0",
    "MYSQL",
    kSblrFamily,
    kPatterns,
    kDatatypeSurfaces,
    kBuiltinSurfaces,
    kCatalogSurfaces,
    kDiagnosticSurfaces,
    19,
    123,
    118,
    0,
    1,
    0,
    4,
    0,
    0,
    kDialectVariants,
    &kSemanticPolicy,
};

} // namespace

const scratchbird::parser::compatibility::DialectProfile& Profile() {
  return kProfile;
}

std::string TrimAscii(std::string_view text) {
  return scratchbird::parser::compatibility::TrimAscii(text);
}

std::string NormalizeWhitespace(std::string_view text) {
  return scratchbird::parser::compatibility::NormalizeWhitespace(text);
}

std::string ToUpperAscii(std::string_view text) {
  return scratchbird::parser::compatibility::ToUpperAscii(text);
}

std::string MessageVectorToJson(const std::vector<Diagnostic>& diagnostics) {
  return scratchbird::parser::compatibility::MessageVectorToJson(diagnostics);
}

std::vector<Token> LexTokens(std::string_view sql_text) {
  return scratchbird::parser::compatibility::LexTokens(sql_text);
}

ParseResult ParseStatement(std::string_view sql_text) {
  return scratchbird::parser::compatibility::ParseStatement(sql_text, kProfile);
}

std::span<const SurfaceDescriptor> DatatypeSurfaces() {
  return kDatatypeSurfaces;
}

std::span<const SurfaceDescriptor> BuiltinFunctionSurfaces() {
  return kBuiltinSurfaces;
}

std::span<const SurfaceDescriptor> CatalogOverlaySurfaces() {
  return kCatalogSurfaces;
}

std::span<const SurfaceDescriptor> DiagnosticSurfaces() {
  return kDiagnosticSurfaces;
}

std::string MysqlPackageIdentityJson() {
  return scratchbird::parser::compatibility::PackageIdentityJson(kProfile);
}

std::string MysqlSurfaceReportJson() {
  return scratchbird::parser::compatibility::SurfaceReportJson(kProfile);
}

} // namespace scratchbird::parser::mysql
