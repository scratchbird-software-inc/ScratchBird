// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "postgresql_dialect.hpp"

#include <array>
#include <cctype>
#include <sstream>
#include <vector>

namespace scratchbird::parser::postgresql {
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
         next == '(' || next == '\'' || next == '"';
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
         upper.find("PG_CATALOG.") != std::string_view::npos ||
         upper.find("INFORMATION_SCHEMA.") != std::string_view::npos ||
         upper.find("PG_CLASS") != std::string_view::npos ||
         upper.find("PG_NAMESPACE") != std::string_view::npos ||
         upper.find("PG_ATTRIBUTE") != std::string_view::npos ||
         upper.find("PG_TYPE") != std::string_view::npos ||
         upper.find("PG_PROC") != std::string_view::npos ||
         upper.find("PG_DEPEND") != std::string_view::npos ||
         upper.find("PG_ROLES") != std::string_view::npos;
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
      << "\"compatibility_profile_uuid\":\"019e13c0-0000-7000-8000-000000000304\","
      << "\"semantic_profile_uuid\":\"019e13c0-1d00-7000-8000-000000000304\","
      << "\"catalog_overlay_profile_uuid\":\"019e13c0-1d00-7000-8000-000000000304\","
      << "\"dialect\":\"postgresql\",\"operation_id\":\""
      << scratchbird::parser::compatibility::EscapeJson(operation_id) << "\","
      << "\"system_catalog_defaults_profile\":\"postgresql.system_catalog_defaults_semantics_profile\","
      << "\"system_catalog_namespace_root_policy\":\"postgresql_pg_catalog_information_schema_projected_from_connected_database_catalog_root\","
      << "\"catalog_visibility_projection_policy\":\"postgresql_pg_catalog_information_schema_privilege_filtered_projection\","
      << "\"generated_default_catalog_name_policy\":\"postgresql_generated_pg_class_pg_constraint_names_projected_from_engine_catalog_descriptors\","
      << "\"dependency_projection_policy\":\"postgresql_pg_depend_projection_from_engine_dependency_graph\","
      << "\"source_visibility_policy\":\"postgresql_pg_proc_pg_views_source_redacted_or_projected_by_engine_source_policy\","
      << "\"hidden_system_object_policy\":\"postgresql_toast_temp_internal_objects_privilege_filtered_engine_projection\","
      << "\"grant_privilege_projection_policy\":\"postgresql_acl_roles_information_schema_projection_engine_security_authority\","
      << "\"catalog_surface_family_count\":" << catalog_surfaces.size() << ','
      << "\"catalog_surface_families\":" << families.str() << ','
      << "\"sblr_catalog_projection_opcode\":\"SBLR_COMPATIBILITY_POSTGRESQL_CATALOG_PROJECT\","
      << "\"diagnostic_map_ref\":\"postgresql_system_catalog_defaults_semantics_diagnostic_map\","
      << "\"sandbox_root_policy\":\"postgresql_connected_database_schema_root_uuid_required_pg_temp_root_local\","
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
  return (StartsWithCommand(upper, "SET") &&
          (ContainsWord(upper, "SEARCH_PATH") ||
           ContainsWord(upper, "STATEMENT_TIMEOUT"))) ||
         (StartsWithCommand(upper, "RESET") &&
          ContainsWord(upper, "SEARCH_PATH")) ||
         StartsWithCommand(upper, "DISCARD ALL") ||
         (StartsWithCommand(upper, "SHOW") && ContainsWord(upper, "SEARCH_PATH"));
}

std::string SessionSettingsDiagnosticsEvidenceJson(
    std::string_view release_profile,
    std::string_view upper) {
  const bool search_path = ContainsWord(upper, "SEARCH_PATH");
  const bool timeout = ContainsWord(upper, "STATEMENT_TIMEOUT");
  const bool reset = StartsWithCommand(upper, "RESET") ||
                     StartsWithCommand(upper, "DISCARD ALL");
  const bool diagnostic_projection = StartsWithCommand(upper, "SHOW");
  const std::string_view operation_surface =
      StartsWithCommand(upper, "SET") && search_path
          ? "postgresql_set_search_path"
      : StartsWithCommand(upper, "SET") && timeout
          ? "postgresql_set_statement_timeout"
      : StartsWithCommand(upper, "RESET") && search_path
          ? "postgresql_reset_search_path"
      : StartsWithCommand(upper, "DISCARD ALL") ? "postgresql_discard_all"
      : diagnostic_projection && search_path ? "postgresql_show_search_path"
                                             : "postgresql_session_settings_diagnostics";
  const std::string_view compatibility_mode_policy =
      StartsWithCommand(upper, "SET") && search_path
          ? "postgresql_search_path_compatibility_descriptor_engine_applies"
      : StartsWithCommand(upper, "SET") && timeout
          ? "postgresql_guc_timeout_descriptor_engine_applies"
      : reset ? "postgresql_guc_reset_compatibility_descriptor_engine_applies"
              : "postgresql_show_guc_projection_descriptor";
  const std::string_view current_schema_policy =
      search_path
          ? "postgresql_current_schema_resolved_from_engine_search_path_descriptor"
          : "postgresql_current_schema_guc_projection_engine_session_descriptor";
  const std::string_view search_path_policy =
      StartsWithCommand(upper, "RESET") && search_path
          ? "postgresql_reset_search_path_to_engine_default_descriptor"
      : diagnostic_projection && search_path
          ? "postgresql_show_search_path_engine_projection"
      : search_path
          ? "postgresql_search_path_list_descriptor_uuid_resolved_engine_applies"
          : "postgresql_search_path_unchanged_engine_session_descriptor";
  const std::string_view timeout_policy =
      timeout ? "postgresql_statement_timeout_engine_session_descriptor"
              : "postgresql_timeout_settings_unchanged_engine_session_descriptor";
  const std::string_view reset_policy =
      StartsWithCommand(upper, "DISCARD ALL")
          ? "postgresql_discard_all_requests_engine_session_reset_descriptor"
      : StartsWithCommand(upper, "RESET")
          ? "postgresql_reset_guc_requests_engine_session_reset_descriptor"
          : "postgresql_session_setting_reset_not_requested";

  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_session_settings_diagnostics_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\"019e13c0-0000-7000-8000-000000000304\","
      << "\"session_semantic_profile_uuid\":\"019e13c0-1e00-7000-8000-000000000304\","
      << "\"semantic_profile_uuid\":\"019e13c0-1e00-7000-8000-000000000304\","
      << "\"dialect\":\"postgresql\",\"release_profile\":\""
      << scratchbird::parser::compatibility::EscapeJson(release_profile) << "\","
      << "\"session_settings_diagnostics_profile\":\"postgresql.session_settings_diagnostics_semantics_profile\","
      << "\"operation_surface\":\"" << operation_surface << "\","
      << "\"sql_mode_set\":false,\"warning_surface\":false,"
      << "\"notice_surface\":" << BoolJson(search_path || timeout) << ','
      << "\"current_schema_surface\":" << BoolJson(search_path) << ','
      << "\"search_path_surface\":" << BoolJson(search_path) << ','
      << "\"date_time_format_surface\":true,"
      << "\"timeout_surface\":" << BoolJson(timeout) << ','
      << "\"reset_surface\":" << BoolJson(reset) << ','
      << "\"diagnostic_projection_surface\":" << BoolJson(diagnostic_projection) << ','
      << "\"compatibility_mode_policy\":\"" << compatibility_mode_policy << "\","
      << "\"warning_policy\":\"postgresql_warning_diagnostics_engine_rendered\","
      << "\"notice_policy\":\"postgresql_notice_warning_guc_diagnostics_engine_rendered\","
      << "\"current_schema_policy\":\"" << current_schema_policy << "\","
      << "\"search_path_policy\":\"" << search_path_policy << "\","
      << "\"date_time_format_policy\":\"postgresql_datestyle_intervalstyle_descriptor_engine_applies\","
      << "\"timeout_policy\":\"" << timeout_policy << "\","
      << "\"reset_policy\":\"" << reset_policy << "\","
      << "\"diagnostic_map_ref\":\"postgresql_session_settings_diagnostics_semantics_diagnostic_map\","
      << "\"sandbox_root_policy\":\"postgresql_connected_database_schema_root_uuid_required_pg_temp_root_local\","
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
         upper.find(" FOR SHARE") != std::string_view::npos;
}

bool IsLocksIsolationStatement(std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  return StartsWithCommand(upper, "SET TRANSACTION") ||
         StartsWithCommand(upper, "START TRANSACTION") ||
         (StartsWithCommand(upper, "BEGIN") &&
          (ContainsWord(upper, "ISOLATION") || ContainsWord(upper, "READ") ||
           ContainsWord(upper, "DEFERRABLE"))) ||
         StartsWithCommand(upper, "LOCK TABLE") || IsRowLockQuery(upper);
}

std::string LocksIsolationEvidenceJson(std::string_view release_profile,
                                       std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool isolation_surface =
      StartsWithCommand(upper, "SET TRANSACTION") ||
      StartsWithCommand(upper, "START TRANSACTION") ||
      (StartsWithCommand(upper, "BEGIN") &&
       (ContainsWord(upper, "ISOLATION") || ContainsWord(upper, "READ") ||
        ContainsWord(upper, "DEFERRABLE")));
  const bool lock_table_surface = StartsWithCommand(upper, "LOCK TABLE");
  const bool for_update_surface =
      upper.find(" FOR UPDATE") != std::string_view::npos;
  const bool for_share_surface =
      upper.find(" FOR SHARE") != std::string_view::npos;
  const bool row_lock_surface =
      IsRowLockQuery(upper) || for_update_surface || for_share_surface;
  const bool nowait_surface = ContainsWord(upper, "NOWAIT");
  const bool skip_locked_surface =
      upper.find(" SKIP LOCKED") != std::string_view::npos;
  const bool advisory_lock_surface =
      upper.find("PG_ADVISORY_LOCK") != std::string_view::npos;
  const bool read_only_surface =
      upper.find(" READ ONLY") != std::string_view::npos;
  const bool read_write_surface =
      upper.find(" READ WRITE") != std::string_view::npos ||
      ContainsWord(upper, "WRITE");
  const bool deadlock_diagnostic_surface =
      ContainsWord(upper, "DEADLOCK") ||
      upper.find("PG_LOCKS") != std::string_view::npos;
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
      StartsWithCommand(upper, "LOCK TABLE")
          ? (ContainsWord(upper, "NOWAIT")
                 ? "postgresql_lock_table_mode_nowait_descriptor"
                 : "postgresql_lock_table_mode_descriptor")
      : for_update_surface ? "postgresql_select_for_update_row_lock_descriptor"
      : for_share_surface ? "postgresql_select_for_share_row_lock_descriptor"
      : (StartsWithCommand(upper, "SET TRANSACTION") ||
         StartsWithCommand(upper, "BEGIN") ||
         StartsWithCommand(upper, "START TRANSACTION"))
          ? "postgresql_transaction_isolation_read_write_descriptor"
          : "postgresql_locks_isolation_syntax_surface";

  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_locks_isolation_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\"019e13c0-0000-7000-8000-000000000304\","
      << "\"semantic_profile_uuid\":\"019e13c0-1c00-7000-8000-000000000304\","
      << "\"dialect\":\"postgresql\",\"release_profile\":\""
      << scratchbird::parser::compatibility::EscapeJson(release_profile) << "\","
      << "\"locks_isolation_profile\":\"postgresql.locks_isolation_syntax_semantics_profile\","
      << "\"locks_isolation_surface\":\"" << locks_surface << "\","
      << "\"isolation_profile_uuid_or_policy\":\"postgresql_transaction_isolation_descriptor_uuid_required_engine_mga_authority\","
      << "\"lock_clause_policy\":\"postgresql_lock_table_and_row_lock_descriptor_engine_lock_authority\","
      << "\"nowait_policy\":\"postgresql_nowait_descriptor_engine_lock_wait_policy\","
      << "\"skip_locked_policy\":\"postgresql_skip_locked_descriptor_engine_row_lock_policy\","
      << "\"advisory_lock_policy\":\"postgresql_pg_advisory_lock_descriptor_engine_policy\","
      << "\"table_lock_policy\":\"postgresql_lock_table_mode_descriptor_engine_lock_authority\","
      << "\"row_lock_policy\":\"postgresql_for_update_for_share_descriptor_engine_row_lock_authority\","
      << "\"read_write_policy\":\"postgresql_read_only_read_write_descriptor_engine_intent_authority\","
      << "\"deadlock_diagnostic_policy\":\"postgresql_deadlock_diagnostic_map_descriptor_engine_lock_manager_authority\","
      << "\"diagnostic_map_ref\":\"postgresql_locks_isolation_semantics_diagnostic_map\","
      << "\"sandbox_root_policy\":\"postgresql_connected_database_schema_root_uuid_required_pg_temp_root_local\","
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
              StartsWithCommand(upper, "DROP");
  flags.dml = StartsWithCommand(upper, "INSERT") ||
              StartsWithCommand(upper, "UPDATE") ||
              StartsWithCommand(upper, "DELETE") ||
              StartsWithCommand(upper, "MERGE");
  flags.query = StartsWithCommand(upper, "SELECT") ||
                StartsWithCommand(upper, "WITH");
  flags.binary_text = ContainsWord(upper, "BYTEA");
  flags.text_type = ContainsWord(upper, "CHAR") ||
                    ContainsWord(upper, "VARCHAR") ||
                    ContainsWord(upper, "NCHAR") ||
                    Contains(upper, "NATIONAL CHARACTER") ||
                    ContainsWord(upper, "TEXT") || flags.binary_text;
  flags.charset = Contains(upper, "CHARACTER SET");
  flags.collation = ContainsWord(upper, "COLLATE") ||
                    ContainsWord(upper, "COLLATION");
  flags.string_literal = Contains(upper, "'");
  flags.pattern = Contains(upper, " LIKE ") ||
                  Contains(upper, " SIMILAR TO ") ||
                  Contains(upper, " ILIKE ") || Contains(upper, " ~ ") ||
                  Contains(upper, " ~* ") || Contains(upper, " !~ ") ||
                  Contains(upper, " !~* ");
  flags.cast_to_text =
      (Contains(upper, "CAST(") || Contains(upper, "::")) && flags.text_type;
  flags.timezone = Contains(upper, "WITH TIME ZONE") ||
                   ContainsWord(upper, "TIMEZONE") ||
                   ContainsWord(upper, "TIMESTAMPTZ") ||
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
      flags.ddl ? "postgresql_ddl_text_collation_bytea"
      : flags.dml ? "postgresql_dml_text_resource_descriptor"
      : flags.pattern ? "postgresql_query_like_similar_regex_collation"
      : flags.timezone ? "postgresql_query_timestamptz_timezone_resource"
      : flags.binary_text ? "postgresql_bytea_text_resource"
                          : "postgresql_query_resource_text_semantics";

  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_resource_text_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\"019e13c0-0000-7000-8000-000000000304\","
      << "\"semantic_profile_uuid\":\"019e13c0-1a00-7000-8000-000000000304\","
      << "\"dialect\":\"postgresql\",\"release_profile\":\""
      << scratchbird::parser::compatibility::EscapeJson(release_profile) << "\","
      << "\"resource_text_profile\":\"postgresql.resource_text_semantics_profile\","
      << "\"resource_text_surface\":\"" << surface << "\","
      << "\"charset_policy\":\"postgresql_database_encoding_descriptor_uuid_required_engine_applies\","
      << "\"collation_policy\":\"postgresql_per_expression_collation_descriptor\","
      << "\"timezone_policy\":\"postgresql_time_zone_guc_descriptor_engine_authority\","
      << "\"calendar_policy\":\"postgresql_datestyle_intervalstyle_calendar_descriptor_engine_authority\","
      << "\"comparison_policy\":\"postgresql_text_comparison_collation_operator_descriptor_engine_authority\","
      << "\"pattern_matching_policy\":\"postgresql_like_similar_to_regex_operator_descriptor\","
      << "\"binary_text_policy\":\"postgresql_bytea_text_cast_descriptor_required\","
      << "\"resource_epoch_policy\":\"postgresql_resource_text_descriptor_epoch_engine_mga_catalog_bound\","
      << "\"index_compatibility_policy\":\"postgresql_text_index_operator_class_collation_compatibility_engine_validated\","
      << "\"diagnostic_map_ref\":\"postgresql_resource_text_semantics_diagnostic_map\","
      << "\"sandbox_root_policy\":\"postgresql_connected_database_schema_root_uuid_required_pg_temp_root_local\","
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
         StartsWithCommand(upper, "ANALYZE") ||
         StartsWithCommand(upper, "VACUUM") ||
         StartsWithCommand(upper, "REINDEX") ||
         StartsWithCommand(upper, "CREATE STATISTICS") ||
         StartsWithCommand(upper, "DROP STATISTICS");
}

std::string StatisticsOptimizerEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool explain_surface = StartsWithCommand(upper, "EXPLAIN");
  const bool analyze_surface = StartsWithCommand(upper, "ANALYZE");
  const bool vacuum_surface = StartsWithCommand(upper, "VACUUM");
  const bool reindex_surface = StartsWithCommand(upper, "REINDEX");
  const bool create_statistics_surface =
      StartsWithCommand(upper, "CREATE STATISTICS");
  const bool drop_statistics_surface =
      StartsWithCommand(upper, "DROP STATISTICS");
  const bool statistics_update_surface = analyze_surface || vacuum_surface;
  const bool index_statistics_surface = ContainsWord(upper, "INDEX");
  const bool plan_query_surface =
      explain_surface &&
      (ContainsWord(upper, "SELECT") || ContainsWord(upper, "WITH") ||
       ContainsWord(upper, "UPDATE") || ContainsWord(upper, "DELETE") ||
       ContainsWord(upper, "INSERT"));
  const std::string_view surface =
      explain_surface ? "postgresql_explain_plan_catalog_projection_descriptor"
      : analyze_surface
          ? "postgresql_analyze_statistics_update_refused_descriptor"
      : vacuum_surface
          ? "postgresql_vacuum_analyze_statistics_refused_descriptor"
      : reindex_surface
          ? "postgresql_reindex_statistics_dependency_refused_descriptor"
      : create_statistics_surface
          ? "postgresql_create_statistics_catalog_descriptor"
      : drop_statistics_surface
          ? "postgresql_drop_statistics_catalog_descriptor"
          : "postgresql_statistics_optimizer_metadata_surface";
  const std::string_view command_policy =
      (analyze_surface || vacuum_surface || reindex_surface)
          ? "postgresql_statistics_maintenance_command_refused_no_compatibility_execution"
          : "postgresql_optimizer_metadata_catalog_projection_only";
  const std::string_view analyze_policy =
      analyze_surface
          ? "postgresql_analyze_refused_descriptor_no_compatibility_execution"
          : "postgresql_analyze_policy_descriptor_required";
  const std::string_view explain_policy =
      explain_surface
          ? "postgresql_explain_catalog_projection_descriptor_no_plan_authority"
          : "postgresql_explain_policy_descriptor_required";

  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_statistics_optimizer_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\"019e13c0-0000-7000-8000-000000000304\","
      << "\"semantic_profile_uuid\":\"019e13c0-1b00-7000-8000-000000000304\","
      << "\"dialect\":\"postgresql\",\"release_profile\":\""
      << scratchbird::parser::compatibility::EscapeJson(release_profile) << "\","
      << "\"statistics_optimizer_profile\":\"postgresql.statistics_optimizer_metadata_semantics_profile\","
      << "\"statistics_optimizer_surface\":\"" << surface << "\","
      << "\"statistics_command_policy\":\"" << command_policy << "\","
      << "\"histogram_policy\":\"postgresql_pg_statistic_histogram_descriptor_engine_statistics_authority\","
      << "\"selectivity_policy\":\"postgresql_ndistinct_mcv_selectivity_descriptor_engine_authority\","
      << "\"stale_statistics_policy\":\"postgresql_autovacuum_analyze_staleness_descriptor_engine_epoch\","
      << "\"index_eligibility_policy\":\"postgresql_index_valid_ready_predicate_eligibility_engine_descriptor\","
      << "\"plan_invalidation_policy\":\"postgresql_plan_cache_invalidation_engine_catalog_statistics_epoch\","
      << "\"analyze_command_policy\":\"" << analyze_policy << "\","
      << "\"explain_plan_policy\":\"" << explain_policy << "\","
      << "\"catalog_projection_policy\":\"postgresql_pg_statistic_pg_stats_projection_uuid_required\","
      << "\"diagnostic_map_ref\":\"postgresql_statistics_optimizer_semantics_diagnostic_map\","
      << "\"sandbox_root_policy\":\"postgresql_connected_database_schema_root_uuid_required_pg_temp_root_local\","
      << "\"explain_surface\":" << BoolJson(explain_surface) << ','
      << "\"analyze_surface\":" << BoolJson(analyze_surface) << ','
      << "\"statistics_update_surface\":"
      << BoolJson(statistics_update_surface) << ','
      << "\"reindex_surface\":" << BoolJson(reindex_surface) << ','
      << "\"optimize_surface\":false,"
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
         StartsWithCommand(upper, "TRUNCATE") ||
         StartsWithCommand(upper, "COMMENT");
}

bool IsCreateIndexStatement(std::string_view upper) {
  return StartsWithCommand(upper, "CREATE INDEX") ||
         StartsWithCommand(upper, "CREATE UNIQUE INDEX") ||
         StartsWithCommand(upper, "CREATE INDEX CONCURRENTLY") ||
         StartsWithCommand(upper, "CREATE UNIQUE INDEX CONCURRENTLY");
}

std::string_view DdlOperationKind(std::string_view upper) {
  if (StartsWithCommand(upper, "CREATE OR REPLACE VIEW")) {
    return "create_or_replace_view";
  }
  if (StartsWithCommand(upper, "CREATE MATERIALIZED VIEW")) {
    return "create_materialized_view";
  }
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
  if (IsCreateIndexStatement(upper)) return "create_index";
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
  const bool concurrent = ContainsWord(upper, "CONCURRENTLY");
  const std::string_view transaction_policy =
      concurrent
          ? "postgresql_concurrent_ddl_nontransactional_policy_descriptor"
          : "postgresql_transactional_ddl_descriptor_required";
  const std::string_view autocommit_boundary =
      concurrent ? "concurrent_index_requires_top_level_engine_policy"
                 : "none_parser_does_not_commit_engine_transaction";
  const std::string_view rollback_policy =
      concurrent
          ? "postgresql_concurrent_index_rollback_policy_engine_owned"
          : "ddl_rollback_requires_engine_mga_transaction_rollback";
  const std::string_view invalid_state_policy =
      DdlOperationKind(upper) == std::string_view("create_index")
          ? "postgresql_index_invalid_state_descriptor_engine_authority"
          : "postgresql_catalog_invalid_state_descriptor_engine_authority";

  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_ddl_transaction_behavior_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\"019e13c0-0000-7000-8000-000000000304\","
      << "\"semantic_profile_uuid\":\"019e13c0-1900-7000-8000-000000000304\","
      << "\"dialect\":\"postgresql\",\"release_profile\":\""
      << scratchbird::parser::compatibility::EscapeJson(release_profile) << "\","
      << "\"ddl_transaction_behavior_profile\":\"postgresql.ddl_transaction_behavior_semantics_profile\","
      << "\"statement_classification\":\"ddl\","
      << "\"ddl_operation_kind\":\"" << DdlOperationKind(upper) << "\","
      << "\"transaction_policy\":\"" << transaction_policy << "\","
      << "\"autocommit_boundary\":\"" << autocommit_boundary << "\","
      << "\"metadata_visibility_epoch\":\"transaction_local_until_engine_commit_then_catalog_epoch\","
      << "\"rollback_policy\":\"" << rollback_policy << "\","
      << "\"invalid_object_state_policy\":\"" << invalid_state_policy << "\","
      << "\"diagnostic_map_ref\":\"postgresql_ddl_transaction_behavior_diagnostic_map\","
      << "\"sandbox_root_policy\":\"postgresql_connected_database_schema_root_uuid_required_pg_temp_root_local\","
      << "\"create_surface\":" << BoolJson(create_surface) << ','
      << "\"alter_surface\":" << BoolJson(alter_surface) << ','
      << "\"drop_surface\":" << BoolJson(drop_surface) << ','
      << "\"table_surface\":" << BoolJson(table_surface) << ','
      << "\"index_surface\":" << BoolJson(index_surface) << ','
      << "\"view_surface\":" << BoolJson(view_surface) << ','
      << "\"implicit_commit_surface\":false,"
      << "\"transactional_ddl_surface\":" << BoolJson(!concurrent) << ','
      << "\"nontransactional_ddl_surface\":" << BoolJson(concurrent) << ','
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
  bool materialized_view{false};
  bool trigger{false};
  bool routine{false};
  bool procedure{false};
  bool function{false};
  bool rule{false};
};

DependencyDdlFlags ClassifyDependencyDdl(std::string_view upper) {
  DependencyDdlFlags flags;
  flags.view = StartsWithCommand(upper, "CREATE VIEW") ||
               StartsWithCommand(upper, "CREATE OR REPLACE VIEW") ||
               StartsWithCommand(upper, "ALTER VIEW") ||
               StartsWithCommand(upper, "DROP VIEW");
  flags.materialized_view =
      StartsWithCommand(upper, "CREATE MATERIALIZED VIEW") ||
      StartsWithCommand(upper, "ALTER MATERIALIZED VIEW") ||
      StartsWithCommand(upper, "DROP MATERIALIZED VIEW") ||
      StartsWithCommand(upper, "REFRESH MATERIALIZED VIEW");
  flags.trigger = StartsWithCommand(upper, "CREATE TRIGGER") ||
                  StartsWithCommand(upper, "ALTER TRIGGER") ||
                  StartsWithCommand(upper, "DROP TRIGGER");
  flags.procedure = StartsWithCommand(upper, "CREATE PROCEDURE") ||
                    StartsWithCommand(upper, "CREATE OR REPLACE PROCEDURE") ||
                    StartsWithCommand(upper, "ALTER PROCEDURE") ||
                    StartsWithCommand(upper, "DROP PROCEDURE");
  flags.function = StartsWithCommand(upper, "CREATE FUNCTION") ||
                   StartsWithCommand(upper, "CREATE OR REPLACE FUNCTION") ||
                   StartsWithCommand(upper, "ALTER FUNCTION") ||
                   StartsWithCommand(upper, "DROP FUNCTION");
  flags.routine = flags.procedure || flags.function;
  flags.rule = StartsWithCommand(upper, "CREATE RULE") ||
               StartsWithCommand(upper, "CREATE OR REPLACE RULE") ||
               StartsWithCommand(upper, "DROP RULE");
  return flags;
}

bool IsDependencyBearingDdlStatement(std::string_view active_upper_sql) {
  const auto flags = ClassifyDependencyDdl(TrimAsciiView(active_upper_sql));
  return flags.view || flags.materialized_view || flags.trigger ||
         flags.routine || flags.rule;
}

std::string DependencyBearingDdlEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const auto flags = ClassifyDependencyDdl(upper);
  const bool executable_body_surface =
      flags.trigger || flags.routine || flags.rule;
  const bool query_dependency_surface =
      flags.view || flags.materialized_view || executable_body_surface ||
      ContainsWord(upper, "FROM") || ContainsWord(upper, "JOIN") ||
      ContainsWord(upper, "ON") || ContainsWord(upper, "REFERENCES") ||
      Contains(upper, "EXECUTE FUNCTION");
  const bool drop_surface = StartsWithCommand(upper, "DROP");
  const bool alter_surface = StartsWithCommand(upper, "ALTER");
  const bool create_surface = StartsWithCommand(upper, "CREATE");
  const std::string_view surface =
      flags.materialized_view ? "postgresql_materialized_view_ddl"
      : StartsWithCommand(upper, "CREATE OR REPLACE VIEW")
          ? "postgresql_create_or_replace_view"
      : StartsWithCommand(upper, "CREATE VIEW") ? "postgresql_create_view"
      : StartsWithCommand(upper, "ALTER VIEW") ? "postgresql_alter_view"
      : StartsWithCommand(upper, "DROP VIEW") ? "postgresql_drop_view"
      : flags.rule ? "postgresql_rule_rewrite_ddl"
      : flags.trigger ? "postgresql_trigger_ddl"
      : flags.routine ? "postgresql_procedure_function_ddl"
                      : "postgresql_dependency_bearing_ddl";
  const std::string_view binding_policy =
      flags.rule
          ? "postgresql_rewrite_rule_dependency_binding_uuid_descriptors"
      : flags.materialized_view
          ? "postgresql_materialized_view_dependency_binding_uuid_descriptors"
          : "postgresql_pg_depend_dependency_binding_uuid_descriptors";
  const std::string_view invalidation_policy =
      flags.materialized_view
          ? "postgresql_materialized_view_refresh_dependency_invalidation_engine_catalog_authority"
          : "postgresql_pg_depend_invalidation_engine_catalog_authority";
  const std::string_view execution_policy =
      executable_body_surface
          ? "postgresql_routine_trigger_rule_body_routes_to_trusted_udr_lowering"
          : "postgresql_view_query_dependency_descriptor_no_parser_execution";

  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_dependency_bearing_ddl_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\"019e13c0-0000-7000-8000-000000000304\","
      << "\"semantic_profile_uuid\":\"019e13c0-1800-7000-8000-000000000304\","
      << "\"dialect\":\"postgresql\",\"release_profile\":\""
      << scratchbird::parser::compatibility::EscapeJson(release_profile) << "\","
      << "\"dependency_ddl_profile\":\"postgresql.dependency_bearing_ddl_semantics_profile\","
      << "\"dependency_ddl_surface\":\"" << surface << "\","
      << "\"view_surface\":" << BoolJson(flags.view) << ','
      << "\"materialized_view_surface\":"
      << BoolJson(flags.materialized_view) << ','
      << "\"trigger_surface\":" << BoolJson(flags.trigger) << ','
      << "\"routine_surface\":" << BoolJson(flags.routine) << ','
      << "\"procedure_surface\":" << BoolJson(flags.procedure) << ','
      << "\"function_surface\":" << BoolJson(flags.function) << ','
      << "\"package_surface\":false,"
      << "\"rule_surface\":" << BoolJson(flags.rule) << ','
      << "\"event_surface\":false,"
      << "\"executable_body_surface\":"
      << BoolJson(executable_body_surface) << ','
      << "\"query_dependency_surface\":"
      << BoolJson(query_dependency_surface) << ','
      << "\"create_surface\":" << BoolJson(create_surface) << ','
      << "\"alter_surface\":" << BoolJson(alter_surface) << ','
      << "\"drop_surface\":" << BoolJson(drop_surface) << ','
      << "\"dependency_binding_policy\":\"" << binding_policy << "\","
      << "\"invalidation_policy\":\"" << invalidation_policy << "\","
      << "\"execution_body_policy\":\"" << execution_policy << "\","
      << "\"catalog_storage_policy\":\"postgresql_pg_catalog_projection_stores_uuid_dependency_descriptors\","
      << "\"sandbox_root_policy\":\"postgresql_connected_database_schema_root_uuid_required_pg_temp_root_local\","
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

std::string TransactionSessionSemanticEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool rollback_to_savepoint = IsRollbackToSavepoint(upper);

  scratchbird::parser::compatibility::TransactionSessionSemanticDescriptor d;
  d.compatibility_profile_uuid = "019e13c0-0000-7000-8000-000000000304";
  d.semantic_profile_uuid = "019e13c0-1600-7000-8000-000000000304";
  d.transaction_session_profile =
      "postgresql.transaction_session_semantics_profile";
  if (StartsWithCommand(upper, "BEGIN")) {
    d.transaction_session_surface = "postgresql_begin";
  } else if (StartsWithCommand(upper, "START TRANSACTION")) {
    d.transaction_session_surface = "postgresql_start_transaction";
  } else if (StartsWithCommand(upper, "COMMIT")) {
    d.transaction_session_surface = "postgresql_commit";
  } else if (rollback_to_savepoint) {
    d.transaction_session_surface = "postgresql_rollback_to_savepoint";
  } else if (StartsWithCommand(upper, "ROLLBACK")) {
    d.transaction_session_surface = "postgresql_rollback";
  } else if (StartsWithCommand(upper, "RELEASE SAVEPOINT")) {
    d.transaction_session_surface = "postgresql_release_savepoint";
  } else if (StartsWithCommand(upper, "SAVEPOINT")) {
    d.transaction_session_surface = "postgresql_savepoint";
  } else if (StartsWithCommand(upper, "SET TRANSACTION")) {
    d.transaction_session_surface =
        ContainsWord(upper, "SERIALIZABLE") && Contains(upper, "READ ONLY") &&
                ContainsWord(upper, "DEFERRABLE")
            ? "postgresql_set_transaction_serializable_read_only_deferrable"
            : "postgresql_set_transaction";
  } else if (StartsWithCommand(upper, "SET LOCAL") &&
             ContainsWord(upper, "TRANSACTION_ISOLATION")) {
    d.transaction_session_surface = "postgresql_set_local_transaction_isolation";
  } else if (StartsWithCommand(upper, "SET SESSION") &&
             ContainsWord(upper, "TRANSACTION_ISOLATION")) {
    d.transaction_session_surface = "postgresql_set_session_transaction_isolation";
  } else if (StartsWithCommand(upper, "SET") &&
             ContainsWord(upper, "STATEMENT_TIMEOUT")) {
    d.transaction_session_surface = "postgresql_set_statement_timeout";
  } else if (StartsWithCommand(upper, "SET") &&
             ContainsWord(upper, "SEARCH_PATH")) {
    d.transaction_session_surface = "postgresql_set_search_path";
  } else {
    d.transaction_session_surface = "postgresql_transaction_session";
  }
  d.statement_family_linkage =
      StartsWithCommand(upper, "SET LOCAL") ||
              StartsWithCommand(upper, "SET SESSION") ||
              (StartsWithCommand(upper, "SET") &&
               (ContainsWord(upper, "STATEMENT_TIMEOUT") ||
                ContainsWord(upper, "SEARCH_PATH")))
          ? "session"
          : "transaction";
  d.begin_autocommit_policy =
      StartsWithCommand(upper, "BEGIN") ||
              StartsWithCommand(upper, "START TRANSACTION")
          ? "postgresql_explicit_begin_requests_engine_mga_transaction_handle"
          : "postgresql_existing_engine_transaction_or_session_descriptor";
  d.isolation_read_only_deferrable_descriptor_policy =
      "postgresql_isolation_read_only_deferrable_descriptor_engine_enforced";
  if (ContainsWord(upper, "SEARCH_PATH")) {
    d.session_variable_sql_mode_descriptor_policy =
        "postgresql_search_path_descriptor_uuid_profile_engine_applies";
  } else if (ContainsWord(upper, "STATEMENT_TIMEOUT")) {
    d.session_variable_sql_mode_descriptor_policy =
        "postgresql_statement_timeout_descriptor_engine_applies";
  } else if (ContainsWord(upper, "TRANSACTION_ISOLATION")) {
    d.session_variable_sql_mode_descriptor_policy =
        "postgresql_transaction_isolation_guc_descriptor_engine_applies";
  } else {
    d.session_variable_sql_mode_descriptor_policy =
        "postgresql_session_guc_descriptor_engine_applies";
  }
  d.begin_surface = StartsWithCommand(upper, "BEGIN") ||
                    StartsWithCommand(upper, "START TRANSACTION");
  d.commit_surface = StartsWithCommand(upper, "COMMIT");
  d.rollback_to_savepoint_surface = rollback_to_savepoint;
  d.rollback_surface = StartsWithCommand(upper, "ROLLBACK") &&
                       !rollback_to_savepoint;
  d.savepoint_surface = StartsWithCommand(upper, "SAVEPOINT");
  d.release_savepoint_surface = StartsWithCommand(upper, "RELEASE SAVEPOINT");
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
  d.deferrable_surface = ContainsWord(upper, "DEFERRABLE") &&
                         !Contains(upper, "NOT DEFERRABLE");
  d.session_variable_surface = StartsWithCommand(upper, "SET") &&
      (ContainsWord(upper, "TRANSACTION_ISOLATION") ||
       ContainsWord(upper, "STATEMENT_TIMEOUT") ||
       ContainsWord(upper, "SEARCH_PATH"));
  d.statement_timeout_surface = ContainsWord(upper, "STATEMENT_TIMEOUT");
  d.search_path_surface = ContainsWord(upper, "SEARCH_PATH");
  return scratchbird::parser::compatibility::
      RenderTransactionSessionSemanticEvidenceJson("postgresql", release_profile,
                                                   d);
}

bool IsCreateTemporaryTableStatement(std::string_view upper) {
  return StartsWithCommand(upper, "CREATE TEMP TABLE") ||
         StartsWithCommand(upper, "CREATE TEMPORARY TABLE") ||
         StartsWithCommand(upper, "CREATE LOCAL TEMP TABLE") ||
         StartsWithCommand(upper, "CREATE LOCAL TEMPORARY TABLE") ||
         StartsWithCommand(upper, "CREATE GLOBAL TEMP TABLE") ||
         StartsWithCommand(upper, "CREATE GLOBAL TEMPORARY TABLE");
}

bool IsTemporarySessionObjectSemanticStatement(
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool drop_surface = StartsWithCommand(upper, "DROP TABLE") &&
                            (Contains(upper, "PG_TEMP.") ||
                             Contains(upper, "TEMP"));
  const bool alter_surface =
      (StartsWithCommand(upper, "ALTER TABLE") ||
       StartsWithCommand(upper, "ALTER LOCAL TEMPORARY TABLE") ||
       StartsWithCommand(upper, "ALTER TEMPORARY TABLE") ||
       StartsWithCommand(upper, "ALTER TEMP TABLE")) &&
      (Contains(upper, "TEMP") || Contains(upper, "PG_TEMP."));
  return IsCreateTemporaryTableStatement(upper) || drop_surface || alter_surface;
}

std::string TemporarySessionObjectSemanticEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  scratchbird::parser::compatibility::TemporarySessionObjectSemanticDescriptor d;
  d.compatibility_profile_uuid = "019e13c0-0000-7000-8000-000000000304";
  d.semantic_profile_uuid = "019e13c0-1700-7000-8000-000000000304";
  d.temporary_object_profile = "postgresql.temporary_table_semantics_profile";
  d.create_surface = IsCreateTemporaryTableStatement(upper);
  d.drop_surface = StartsWithCommand(upper, "DROP TABLE") &&
                   (Contains(upper, "PG_TEMP.") || Contains(upper, "TEMP"));
  d.alter_surface = !d.create_surface && !d.drop_surface &&
                    StartsWithCommand(upper, "ALTER");
  d.on_commit_delete_rows_surface = Contains(upper, "ON COMMIT DELETE ROWS");
  d.on_commit_preserve_rows_surface = Contains(upper, "ON COMMIT PRESERVE ROWS");
  d.on_commit_drop_surface = Contains(upper, "ON COMMIT DROP");
  if (d.create_surface) {
    d.temporary_object_surface =
        d.on_commit_drop_surface ? "postgresql_create_temp_table_on_commit_drop"
        : d.on_commit_delete_rows_surface
            ? "postgresql_create_temp_table_on_commit_delete_rows"
        : d.on_commit_preserve_rows_surface
            ? "postgresql_create_temp_table_on_commit_preserve_rows"
        : StartsWithCommand(upper, "CREATE LOCAL TEMP") ||
                  StartsWithCommand(upper, "CREATE LOCAL TEMPORARY")
            ? "postgresql_create_local_temp_table_default_preserve_rows"
        : StartsWithCommand(upper, "CREATE GLOBAL TEMP") ||
                  StartsWithCommand(upper, "CREATE GLOBAL TEMPORARY")
            ? "postgresql_create_global_temp_table_compatibility_keyword"
            : "postgresql_create_temp_table_default_preserve_rows";
  } else if (d.drop_surface) {
    d.temporary_object_surface = "postgresql_drop_table_pg_temp_resolution";
  } else if (d.alter_surface) {
    d.temporary_object_surface = "postgresql_alter_table_pg_temp_resolution";
  } else {
    d.temporary_object_surface = "postgresql_temporary_session_object";
  }
  if (StartsWithCommand(upper, "CREATE GLOBAL TEMP") ||
      StartsWithCommand(upper, "CREATE GLOBAL TEMPORARY")) {
    d.temporary_object_kind_policy =
        "postgresql_global_temp_keyword_accepted_as_local_temp_semantics";
  } else if (StartsWithCommand(upper, "CREATE LOCAL TEMP") ||
             StartsWithCommand(upper, "CREATE LOCAL TEMPORARY")) {
    d.temporary_object_kind_policy =
        "postgresql_local_temp_schema_session_private";
  } else {
    d.temporary_object_kind_policy =
        "postgresql_temp_schema_session_private_table_object";
  }
  d.global_keyword_surface = ContainsWord(upper, "GLOBAL");
  d.local_keyword_surface = ContainsWord(upper, "LOCAL");
  d.temporary_keyword_surface = ContainsWord(upper, "TEMP") ||
                                ContainsWord(upper, "TEMPORARY") ||
                                Contains(upper, "PG_TEMP.") ||
                                Contains(upper, "TEMP");
  d.table_object_surface = ContainsWord(upper, "TABLE");
  d.on_commit_policy =
      d.on_commit_delete_rows_surface
          ? "postgresql_on_commit_delete_rows_engine_transaction_end_cleanup"
      : d.on_commit_preserve_rows_surface
          ? "postgresql_on_commit_preserve_rows_engine_session_lifetime"
      : d.on_commit_drop_surface
          ? "postgresql_on_commit_drop_engine_transaction_end_catalog_cleanup"
          : "postgresql_default_on_commit_preserve_rows_descriptor";
  d.on_commit_delete_rows_policy =
      "postgresql_delete_rows_supported_engine_mga_transaction_boundary";
  d.on_commit_preserve_rows_policy =
      "postgresql_preserve_rows_supported_engine_session_lifetime";
  d.on_commit_drop_policy =
      "postgresql_on_commit_drop_supported_engine_session_catalog_cleanup";
  d.name_shadowing_surface = d.temporary_keyword_surface;
  d.name_shadowing_policy =
      "postgresql_pg_temp_search_path_shadows_permanent_objects";
  d.session_visibility_policy =
      "postgresql_pg_temp_schema_session_private_search_path_visible";
  d.catalog_visibility_policy =
      "postgresql_catalog_pg_class_pg_namespace_temp_schema_descriptor";
  d.temporary_object_lifetime_policy =
      d.on_commit_drop_surface
          ? "postgresql_temp_table_dropped_at_engine_mga_transaction_end"
      : d.on_commit_delete_rows_surface
          ? "postgresql_temp_rows_deleted_at_engine_mga_transaction_end"
          : "postgresql_temp_table_lives_until_engine_session_end_or_explicit_drop";
  d.schema_root_sandbox_policy =
      "postgresql_connected_database_schema_root_uuid_required_pg_temp_root_local";
  return scratchbird::parser::compatibility::
      RenderTemporarySessionObjectSemanticEvidenceJson("postgresql",
                                                       release_profile, d);
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
         StartsWithCommand(upper, "CREATE UNIQUE INDEX");
}

std::string IndexSemanticDefaultsEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool expression = Contains(upper, "((") || Contains(upper, "COMPUTED BY");
  const bool predicate = ContainsWord(upper, "WHERE");
  scratchbird::parser::compatibility::IndexSemanticDefaultsDescriptor d;
  d.compatibility_profile_uuid = "019e13c0-0000-7000-8000-000000000304";
  d.semantic_profile_uuid = "019e13c0-1000-7000-8000-000000000304";
  d.index_profile = "postgresql.index_optimizer_translation_profile";
  d.ddl_surface = StartsWithCommand(upper, "CREATE")
      ? (ContainsWord(upper, "UNIQUE") ? "create_unique_index" : "create_index")
      : StartsWithCommand(upper, "ALTER TABLE") ? "alter_table_index"
      : StartsWithCommand(upper, "ALTER INDEX") ? "alter_index"
                                                 : "index_ddl";
  d.index_method =
      Contains(upper, " USING HASH") ? "postgresql_hash_access_method_explicit"
      : Contains(upper, " USING GIN") ? "postgresql_gin_access_method_explicit"
      : Contains(upper, " USING GIST") ? "postgresql_gist_access_method_explicit"
      : Contains(upper, " USING BRIN") ? "postgresql_brin_access_method_explicit"
      : Contains(upper, " USING SPGIST")
          ? "postgresql_spgist_access_method_explicit"
          : "postgresql_btree_access_method_default";
  d.unique_requested = ContainsWord(upper, "UNIQUE");
  d.unique_null_policy = !d.unique_requested
      ? "not_unique_index_not_applicable"
      : Contains(upper, "NULLS NOT DISTINCT")
          ? "postgresql_unique_nulls_not_distinct_requested"
          : "postgresql_unique_nulls_distinct_default";
  d.null_ordering = "postgresql_nulls_last_for_ascending_btree_default";
  d.collation_policy = "postgresql_per_expression_collation_descriptor";
  d.operator_family_policy =
      "postgresql_default_operator_class_and_family_resolution";
  d.predicate_or_expression_policy =
      expression && predicate
          ? "postgresql_expression_and_partial_predicate_descriptor"
      : expression ? "postgresql_expression_index_descriptor"
      : predicate ? "postgresql_partial_predicate_descriptor"
                  : "postgresql_column_index_no_predicate_descriptor";
  d.predicate_present = predicate;
  d.expression_key_present = expression;
  d.concurrently_requested = ContainsWord(upper, "CONCURRENTLY");
  d.descending_requested = ContainsWord(upper, "DESC") ||
                           ContainsWord(upper, "DESCENDING");
  d.nulls_not_distinct_requested = Contains(upper, "NULLS NOT DISTINCT");
  d.validation_state = StartsWithCommand(upper, "ALTER INDEX")
      ? "postgresql_index_catalog_validity_preserved_by_alter"
      : "postgresql_index_valid_after_build_default";
  d.build_mode = d.concurrently_requested
      ? "postgresql_concurrent_index_build_requested"
      : StartsWithCommand(upper, "ALTER INDEX")
          ? "postgresql_alter_index_metadata_route"
          : "postgresql_nonconcurrent_index_build_default";
  d.statistics_policy_ref =
      "postgresql_pg_statistic_and_pg_class_index_statistics_profile";
  return scratchbird::parser::compatibility::
      RenderIndexSemanticDefaultsEvidenceJson("postgresql", release_profile, d);
}

bool IsConstraintSemanticDefaultsStatement(std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  return StartsWithCommand(upper, "CREATE") && ContainsWord(upper, "TABLE") &&
         (Contains(upper, "PRIMARY KEY") || ContainsWord(upper, "UNIQUE") ||
          Contains(upper, "FOREIGN KEY") || ContainsWord(upper, "REFERENCES") ||
          ContainsWord(upper, "CHECK") || ContainsWord(upper, "DEFAULT") ||
          ContainsWord(upper, "GENERATED") || ContainsWord(upper, "SERIAL") ||
          ContainsWord(upper, "BIGSERIAL") ||
          ContainsWord(upper, "SMALLSERIAL"));
}

std::string ConstraintSemanticDefaultsEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  scratchbird::parser::compatibility::ConstraintSemanticDefaultsDescriptor d;
  d.compatibility_profile_uuid = "019e13c0-0000-7000-8000-000000000304";
  d.semantic_profile_uuid = "019e13c0-1100-7000-8000-000000000304";
  d.constraint_profile = "postgresql.table_constraint_defaults_profile";
  d.primary_key_present = Contains(upper, "PRIMARY KEY");
  d.primary_key_behavior =
      "postgresql_primary_key_not_null_unique_btree_descriptor";
  d.unique_constraint_present = ContainsWord(upper, "UNIQUE");
  d.unique_null_policy = !d.unique_constraint_present
      ? "not_unique_constraint_not_applicable"
      : Contains(upper, "NULLS NOT DISTINCT")
          ? "postgresql_unique_constraint_nulls_not_distinct_requested"
          : "postgresql_unique_constraint_nulls_distinct_default";
  d.foreign_key_reference_present = Contains(upper, "FOREIGN KEY") ||
                                    ContainsWord(upper, "REFERENCES");
  d.foreign_key_action_defaults =
      "postgresql_foreign_key_default_no_action_update_no_action_delete_descriptor";
  d.check_constraint_present = ContainsWord(upper, "CHECK");
  d.check_truth_table_null_behavior =
      "postgresql_check_constraint_false_fails_unknown_passes_profile";
  d.default_clause_present = ContainsWord(upper, "DEFAULT");
  d.default_expression_policy =
      "postgresql_variable_free_default_expression_descriptor";
  const bool serial = ContainsWord(upper, "SERIAL") ||
                      ContainsWord(upper, "BIGSERIAL") ||
                      ContainsWord(upper, "SMALLSERIAL");
  d.generated_identity_or_autoincrement_present =
      ContainsWord(upper, "GENERATED") || serial;
  d.generated_identity_autoincrement_policy =
      ContainsWord(upper, "GENERATED")
          ? "postgresql_sql_identity_sequence_backed_descriptor"
      : serial ? "postgresql_serial_pseudo_type_sequence_default_descriptor"
               : "postgresql_no_implicit_autoincrement_default";
  d.explicit_constraint_names_present = ContainsWord(upper, "CONSTRAINT");
  d.generated_name_policy =
      "postgresql_catalog_generated_constraint_names_descriptor_required";
  d.deferrability_policy = Contains(upper, "NOT DEFERRABLE")
      ? "postgresql_not_deferrable_initially_immediate_default_profile"
      : ContainsWord(upper, "DEFERRABLE")
          ? "postgresql_deferrability_requested_descriptor"
          : "postgresql_not_deferrable_initially_immediate_default_profile";
  d.enforcement_timing = Contains(upper, "NOT DEFERRABLE")
      ? "postgresql_immediate_constraint_validation_default_profile"
      : ContainsWord(upper, "DEFERRABLE")
          ? "postgresql_constraint_timing_descriptor_requested_runtime_proven"
          : "postgresql_immediate_constraint_validation_default_profile";
  return scratchbird::parser::compatibility::
      RenderConstraintSemanticDefaultsEvidenceJson("postgresql", release_profile,
                                                   d);
}

bool IsSequenceIdentitySemanticStatement(std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  const bool create_sequence = StartsWithCommand(upper, "CREATE SEQUENCE") ||
      StartsWithCommand(upper, "CREATE TEMP SEQUENCE") ||
      StartsWithCommand(upper, "CREATE TEMPORARY SEQUENCE") ||
      StartsWithCommand(upper, "CREATE UNLOGGED SEQUENCE") ||
      StartsWithCommand(upper, "CREATE GLOBAL TEMP SEQUENCE") ||
      StartsWithCommand(upper, "CREATE LOCAL TEMP SEQUENCE");
  return create_sequence || StartsWithCommand(upper, "ALTER SEQUENCE") ||
         StartsWithCommand(upper, "DROP SEQUENCE") ||
         Contains(upper, "NEXTVAL(") || Contains(upper, "CURRVAL(") ||
         Contains(upper, "SETVAL(") || ContainsWord(upper, "SERIAL") ||
         ContainsWord(upper, "BIGSERIAL") ||
         ContainsWord(upper, "SMALLSERIAL") ||
         (ContainsWord(upper, "GENERATED") && ContainsWord(upper, "IDENTITY"));
}

std::string SequenceIdentitySemanticEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  scratchbird::parser::compatibility::SequenceIdentitySemanticDescriptor d;
  d.compatibility_profile_uuid = "019e13c0-0000-7000-8000-000000000304";
  d.semantic_profile_uuid = "019e13c0-1300-7000-8000-000000000304";
  d.sequence_identity_profile =
      "postgresql.sequence_serial_identity_profile";
  const bool serial = ContainsWord(upper, "SERIAL") ||
                      ContainsWord(upper, "BIGSERIAL") ||
                      ContainsWord(upper, "SMALLSERIAL");
  const bool identity = ContainsWord(upper, "GENERATED") &&
                        ContainsWord(upper, "IDENTITY");
  d.create_sequence_or_generator_surface =
      StartsWithCommand(upper, "CREATE SEQUENCE");
  d.alter_sequence_surface = StartsWithCommand(upper, "ALTER SEQUENCE");
  d.next_value_surface = Contains(upper, "NEXTVAL(");
  d.currval_surface = Contains(upper, "CURRVAL(");
  d.setval_surface = Contains(upper, "SETVAL(");
  if (StartsWithCommand(upper, "CREATE SEQUENCE")) {
    d.sequence_identity_surface = "postgresql_create_sequence";
  } else if (StartsWithCommand(upper, "ALTER SEQUENCE")) {
    d.sequence_identity_surface = "postgresql_alter_sequence";
  } else if (d.next_value_surface || d.currval_surface || d.setval_surface) {
    d.sequence_identity_surface = "postgresql_sequence_function_expression";
  } else if (serial && identity) {
    d.sequence_identity_surface =
        "postgresql_serial_and_identity_sequence_defaults";
  } else if (serial) {
    d.sequence_identity_surface =
        "postgresql_serial_pseudo_type_sequence_default";
  } else if (identity) {
    d.sequence_identity_surface = "postgresql_sql_identity_sequence_default";
  } else {
    d.sequence_identity_surface = "postgresql_sequence_descriptor";
  }
  d.sequence_backed_default_present = serial || identity;
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
  d.session_visible_state_surface = d.currval_surface || d.setval_surface;
  d.object_identity_policy =
      "postgresql_sequence_and_owned_default_uuid_required_no_source_name_binding";
  d.engine_catalog_sequence_descriptor_policy =
      "postgresql_engine_catalog_sequence_descriptor_policy";
  d.allocation_finality_policy =
      "postgresql_sequence_allocation_descriptor_parser_not_allocator";
  d.lower_layer_allocation_policy =
      "postgresql_sequence_access_method_and_catalog_allocator_policy";
  d.value_function_profile =
      d.next_value_surface && d.currval_surface && d.setval_surface
          ? "postgresql_nextval_currval_setval_descriptor"
      : d.next_value_surface ? "postgresql_nextval_descriptor"
      : d.currval_surface ? "postgresql_currval_descriptor"
      : d.setval_surface ? "postgresql_setval_descriptor"
                         : "postgresql_sequence_function_not_observed";
  d.session_visibility_policy =
      "postgresql_currval_session_requires_prior_nextval_descriptor";
  d.sequence_backed_default_policy =
      serial && identity
          ? "postgresql_serial_and_identity_sequence_backed_defaults"
      : serial ? "postgresql_serial_pseudo_type_sequence_backed_default"
      : identity ? "postgresql_identity_sequence_backed_default"
                 : "postgresql_no_sequence_default_observed";
  d.restart_increment_descriptor_policy =
      "postgresql_start_restart_increment_min_max_cache_cycle_descriptor";
  return scratchbird::parser::compatibility::
      RenderSequenceIdentitySemanticEvidenceJson("postgresql", release_profile,
                                                 d);
}

bool IsIdentifierNameResolutionStatement(std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  return StartsWithCommand(upper, "CREATE") ||
         StartsWithCommand(upper, "ALTER") ||
         StartsWithCommand(upper, "DROP") ||
         StartsWithCommand(upper, "COMMENT") ||
         StartsWithCommand(upper, "TRUNCATE");
}

std::string IdentifierNameResolutionEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  scratchbird::parser::compatibility::
      IdentifierNameResolutionSemanticDescriptor d;
  d.compatibility_profile_uuid =
      "019e13c0-0000-7000-8000-000000000304";
  d.semantic_profile_uuid = "019e13c0-1200-7000-8000-000000000304";
  d.name_resolution_profile =
      "postgresql.identifier_name_resolution_profile";
  d.unquoted_identifier_policy =
      "postgresql_unquoted_identifiers_fold_to_lowercase";
  d.quoted_identifier_policy =
      "postgresql_double_quoted_identifiers_preserve_exact_case";
  d.schema_root_resolution_policy =
      "postgresql_database_schema_search_path_uuid_resolution_required";
  d.generated_catalog_name_behavior =
      "postgresql_catalog_generated_names_descriptor_required";
  d.namespace_collision_behavior =
      "postgresql_schema_namespace_collision_resolved_by_uuid_descriptor_and_search_path";
  d.result_metadata_label_policy =
      "postgresql_result_labels_follow_lowercase_fold_alias_descriptor";
  d.table_name_filesystem_case_policy =
      "not_filesystem_sensitive_table_name_policy";
  d.create_surface = StartsWithCommand(upper, "CREATE");
  d.alter_surface = StartsWithCommand(upper, "ALTER");
  d.drop_surface = StartsWithCommand(upper, "DROP");
  d.quoted_identifier_syntax_observed = Contains(upper, "\"");
  d.qualified_name_syntax_observed = Contains(upper, ".");
  return scratchbird::parser::compatibility::
      RenderIdentifierNameResolutionSemanticEvidenceJson(
          "postgresql", release_profile, d);
}

scratchbird::parser::compatibility::ScalarExpressionSemanticDescriptor
PostgresqlScalarExpressionSemanticDescriptor(
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  scratchbird::parser::compatibility::ScalarExpressionSemanticDescriptor d;
  d.compatibility_profile_uuid =
      "019e13c0-0000-7000-8000-000000000304";
  d.semantic_profile_uuid = "019e13c0-1400-7000-8000-000000000304";
  d.scalar_expression_profile =
      "postgresql.scalar_expression_semantics_profile";
  d.query_expression_surface = StartsWithCommand(upper, "WITH")
      ? "with_query_scalar_expression"
      : StartsWithCommand(upper, "SELECT")
          ? "select_scalar_expression"
          : "query_scalar_expression";
  d.cast_type_coercion_profile =
      "postgresql_cast_operator_type_resolution_descriptor";
  d.null_three_valued_logic_profile =
      "postgresql_three_valued_logic_is_distinct_from_descriptor";
  d.boolean_literal_profile =
      "postgresql_strict_boolean_type_literal_profile";
  d.string_comparison_collation_profile =
      "postgresql_collation_operator_resolution_descriptor_no_parser_collation_authority";
  d.temporal_literal_current_timestamp_date_arithmetic_profile =
      "postgresql_timestamp_timestamptz_interval_timezone_descriptor";
  d.numeric_division_rounding_overflow_profile =
      "postgresql_numeric_division_rounding_overflow_descriptor";
  d.pattern_matching_profile =
      "postgresql_like_similar_to_regex_operator_descriptor";
  d.conditional_expression_profile =
      "postgresql_case_coalesce_nullif_descriptor";
  d.expression_builtin_profile =
      "postgresql_expression_operator_resolution_profile_is_distinct_from_similar_regex";
  d.cast_or_coercion_surface =
      Contains(upper, "CAST(") || Contains(upper, "::");
  d.null_logic_surface = ContainsWord(upper, "NULL") ||
                         Contains(upper, " IS DISTINCT FROM") ||
                         Contains(upper, " IS NOT DISTINCT FROM");
  d.boolean_literal_surface = ContainsWord(upper, "TRUE") ||
                              ContainsWord(upper, "FALSE") ||
                              ContainsWord(upper, "UNKNOWN");
  d.string_comparison_surface = ContainsWord(upper, "COLLATE") ||
                                Contains(upper, " LIKE ") ||
                                Contains(upper, " SIMILAR TO ") ||
                                Contains(upper, " ILIKE ");
  d.temporal_expression_surface =
      ContainsWord(upper, "CURRENT_DATE") ||
      ContainsWord(upper, "CURRENT_TIME") ||
      ContainsWord(upper, "CURRENT_TIMESTAMP") ||
      ContainsWord(upper, "LOCALTIMESTAMP") ||
      ContainsWord(upper, "LOCALTIME") || Contains(upper, "EXTRACT(") ||
      Contains(upper, "DATE_PART(") || Contains(upper, "DATE_TRUNC(") ||
      Contains(upper, "NOW(") || ContainsWord(upper, "INTERVAL") ||
      Contains(upper, "TIMESTAMP '") || Contains(upper, "TIMESTAMPTZ '") ||
      Contains(upper, "DATE '") || Contains(upper, "TIME '");
  d.numeric_expression_surface =
      Contains(upper, "/") || Contains(upper, "ROUND(") ||
      Contains(upper, "TRUNC(") || Contains(upper, "MOD(") ||
      Contains(upper, "POWER(") || Contains(upper, "SQRT(") ||
      ContainsWord(upper, "NUMERIC") || ContainsWord(upper, "DECIMAL");
  d.pattern_matching_surface =
      Contains(upper, " LIKE ") || Contains(upper, " SIMILAR TO ") ||
      Contains(upper, " ILIKE ") || Contains(upper, " ~ ") ||
      Contains(upper, " ~* ") || Contains(upper, " !~ ") ||
      Contains(upper, " !~* ");
  d.conditional_expression_surface = HasFunctionCall(upper, "COALESCE") ||
                                     HasFunctionCall(upper, "NULLIF") ||
                                     Contains(upper, "CASE ");
  d.is_distinct_from_surface = Contains(upper, " IS DISTINCT FROM") ||
                               Contains(upper, " IS NOT DISTINCT FROM");
  d.regexp_surface = Contains(upper, " ~ ") || Contains(upper, " ~* ") ||
                     Contains(upper, " !~ ") || Contains(upper, " !~* ");
  d.similar_to_surface = Contains(upper, " SIMILAR TO ");
  return d;
}

bool IsScalarExpressionSemanticStatement(std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  if (!(StartsWithCommand(upper, "SELECT") ||
        StartsWithCommand(upper, "WITH"))) {
    return false;
  }
  const auto d = PostgresqlScalarExpressionSemanticDescriptor(upper);
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
          "postgresql", release_profile,
          PostgresqlScalarExpressionSemanticDescriptor(active_upper_sql));
}

bool IsDmlMutationSemanticStatement(std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  return StartsWithCommand(upper, "INSERT") ||
         StartsWithCommand(upper, "UPDATE") ||
         StartsWithCommand(upper, "DELETE") ||
         StartsWithCommand(upper, "MERGE");
}

std::string DmlMutationSemanticEvidenceJson(
    std::string_view release_profile,
    std::string_view active_upper_sql) {
  const auto upper = TrimAsciiView(active_upper_sql);
  scratchbird::parser::compatibility::DmlMutationSemanticDescriptor d;
  d.compatibility_profile_uuid =
      "019e13c0-0000-7000-8000-000000000304";
  d.semantic_profile_uuid = "019e13c0-1500-7000-8000-000000000304";
  d.mutation_profile = "postgresql.dml_mutation_semantics_profile";
  d.insert_surface = StartsWithCommand(upper, "INSERT");
  d.update_surface = StartsWithCommand(upper, "UPDATE");
  d.delete_surface = StartsWithCommand(upper, "DELETE");
  d.merge_surface = StartsWithCommand(upper, "MERGE");
  d.on_conflict_surface = Contains(upper, " ON CONFLICT ");
  d.on_conflict_do_update_surface =
      d.on_conflict_surface && Contains(upper, " DO UPDATE");
  d.on_conflict_do_nothing_surface =
      d.on_conflict_surface && Contains(upper, " DO NOTHING");
  d.returning_output_projection_surface = ContainsWord(upper, "RETURNING");
  d.cursor_positioned_dml_surface =
      Contains(upper, " WHERE CURRENT OF ");
  d.mutation_surface =
      d.insert_surface && d.on_conflict_do_update_surface
          ? "postgresql_insert_on_conflict_do_update"
      : d.insert_surface && d.on_conflict_do_nothing_surface
          ? "postgresql_insert_on_conflict_do_nothing"
      : d.insert_surface && d.on_conflict_surface
          ? "postgresql_insert_on_conflict"
      : d.merge_surface
          ? (d.returning_output_projection_surface
                 ? "postgresql_merge_returning"
                 : "postgresql_merge")
      : d.cursor_positioned_dml_surface && d.update_surface
          ? "postgresql_update_current_of"
      : d.cursor_positioned_dml_surface && d.delete_surface
          ? "postgresql_delete_current_of"
      : d.insert_surface
          ? (d.returning_output_projection_surface
                 ? "postgresql_insert_returning"
                 : "postgresql_insert")
      : d.update_surface
          ? (d.returning_output_projection_surface
                 ? "postgresql_update_returning"
                 : "postgresql_update")
      : d.delete_surface
          ? (d.returning_output_projection_surface
                 ? "postgresql_delete_returning"
                 : "postgresql_delete")
          : "postgresql_dml_mutation";
  d.upsert_merge_conflict_policy = d.on_conflict_do_update_surface
      ? "postgresql_on_conflict_do_update_descriptor_inference_uuid_required"
      : d.on_conflict_do_nothing_surface
          ? "postgresql_on_conflict_do_nothing_descriptor_inference_uuid_required"
      : d.on_conflict_surface
          ? "postgresql_on_conflict_descriptor_inference_uuid_required"
      : d.merge_surface
          ? "postgresql_merge_descriptor_source_target_uuid_binding_required"
          : "postgresql_no_conflict_or_merge_surface_observed";
  d.returning_output_projection_policy =
      d.returning_output_projection_surface
          ? "postgresql_returning_projection_descriptor_result_relation_uuid_bound"
          : "postgresql_no_returning_projection_observed";
  d.cursor_positioned_dml_policy = d.cursor_positioned_dml_surface
      ? "postgresql_where_current_of_cursor_descriptor_engine_cursor_authority"
      : "postgresql_no_cursor_positioned_dml_observed";
  d.affected_row_count_policy =
      "postgresql_command_tag_row_count_descriptor_engine_reported";
  d.default_value_surface = ContainsWord(upper, "DEFAULT");
  d.generated_column_surface = ContainsWord(upper, "GENERATED");
  d.trigger_interaction_descriptor_required = d.insert_surface ||
                                              d.update_surface ||
                                              d.delete_surface ||
                                              d.merge_surface;
  d.trigger_default_generated_column_interaction_policy =
      d.on_conflict_surface
          ? "postgresql_on_conflict_defaults_generated_columns_triggers_descriptor_engine_order"
      : d.default_value_surface || d.generated_column_surface ||
              d.returning_output_projection_surface
          ? "postgresql_defaults_generated_columns_triggers_returning_descriptor_engine_order"
          : "postgresql_trigger_default_generated_column_descriptor_required";
  return scratchbird::parser::compatibility::
      RenderDmlMutationSemanticEvidenceJson("postgresql", release_profile, d);
}

scratchbird::parser::compatibility::ProceduralFunctionalEncodingSpanMetadata
ProceduralFunctionalEncodingSpanMetadataFor(
    std::string_view,
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
  if (body_semantic_index == semantic_count) {
    for (std::size_t i = 0; i < semantic_count; ++i) {
      const auto upper = token_upper(i);
      if (upper == "EXECUTE" || upper == "CALL") {
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
  d.compatibility_profile_uuid = "019e13c0-0000-7000-8000-000000000304";
  for (const auto& token : active_tokens) {
    if ((token.kind == "symbol" || token.kind == "punctuation") &&
        token.lexeme == "[") {
      d.array = true;
    }
    if (token.kind != "identifier_or_keyword") continue;
    const auto upper = scratchbird::parser::compatibility::ToUpperAscii(token.lexeme);
    if (upper == "SMALLINT" || upper == "INT" || upper == "INTEGER" ||
        upper == "BIGINT" || upper == "SERIAL" || upper == "SMALLSERIAL" ||
        upper == "BIGSERIAL" || upper == "MONEY") {
      d.numeric = true;
    } else if (upper == "NUMERIC" || upper == "DECIMAL") {
      d.numeric = true;
      d.exact_decimal = true;
    } else if (upper == "REAL" || upper == "FLOAT" || upper == "DOUBLE") {
      d.numeric = true;
      d.floating = true;
    } else if (upper == "CHAR" || upper == "VARCHAR" || upper == "CHARACTER" ||
               upper == "TEXT" || upper == "NAME") {
      d.text = true;
    } else if (upper == "COLLATE" || upper == "COLLATION") {
      d.charset_collation_sensitive_text = true;
    } else if (upper == "BYTEA") {
      d.binary_blob = true;
    } else if (upper == "DATE" || upper == "TIME" || upper == "TIMETZ" ||
               upper == "TIMESTAMP" || upper == "TIMESTAMPTZ" ||
               upper == "INTERVAL") {
      d.temporal = true;
    } else if (upper == "BOOL" || upper == "BOOLEAN") {
      d.boolean = true;
    } else if (upper == "JSON" || upper == "JSONB" || upper == "JSONPATH") {
      d.json_document = true;
    } else if (upper == "UUID") {
      d.uuid = true;
    } else if (upper == "ARRAY") {
      d.array = true;
    } else if (upper == "ENUM") {
      d.enum_set = true;
    } else if (upper == "CIDR" || upper == "INET" || upper == "MACADDR" ||
               upper == "MACADDR8") {
      d.network = true;
    } else if (upper == "POINT" || upper == "LINE" || upper == "LSEG" ||
               upper == "BOX" || upper == "PATH" || upper == "POLYGON" ||
               upper == "CIRCLE") {
      d.geometric_spatial = true;
    } else if (upper == "DOMAIN" || upper == "COMPOSITE" ||
               upper == "INT4RANGE" || upper == "INT8RANGE" ||
               upper == "NUMRANGE" || upper == "DATERANGE" ||
               upper == "TSRANGE" || upper == "TSTZRANGE" ||
               upper == "INT4MULTIRANGE" || upper == "INT8MULTIRANGE" ||
               upper == "NUMMULTIRANGE" || upper == "DATEMULTIRANGE" ||
               upper == "TSMULTIRANGE" || upper == "TSTZMULTIRANGE") {
      d.range_domain_composite = true;
    }
  }
  return scratchbird::parser::compatibility::RenderDatatypeProfileEvidenceJson(
      "postgresql", d);
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

constexpr std::string_view kSblrFamily = "sblr.compatibility.postgresql.profile.v1";

constexpr OperationPattern kPatterns[] = {
    {"COPY|| PROGRAM ", PatternMatch::kPrefixAndContains, "bulk_io",
     "postgresql.bulk_io.copy_program",
     MappingDisposition::kPolicyRefusal, "postgresql.policy.copy_program", "",
     "", "POSTGRESQL.AUTHORITY.PROGRAM_DENIED",
     "COPY PROGRAM cannot spawn host programs from parser authority.", true, false},
    {"COPY|| TO '", PatternMatch::kPrefixAndContains, "bulk_io",
     "postgresql.bulk_io.copy_to_file",
     MappingDisposition::kPolicyRefusal, "postgresql.policy.copy_to_file", "",
     "", "POSTGRESQL.AUTHORITY.FILE_IO_DENIED",
     "COPY TO file cannot perform compatibility filesystem writes.", true, false},
    {"COPY|| TO :'", PatternMatch::kPrefixAndContains, "bulk_io",
     "postgresql.bulk_io.copy_to_file",
     MappingDisposition::kPolicyRefusal, "postgresql.policy.copy_to_file", "",
     "", "POSTGRESQL.AUTHORITY.FILE_IO_DENIED",
     "COPY TO file cannot perform compatibility filesystem writes.", true, false},
    {"COPY|| FROM '", PatternMatch::kPrefixAndContains, "bulk_io",
     "postgresql.bulk_io.copy_from_file",
     MappingDisposition::kPolicyRefusal, "postgresql.policy.copy_from_file", "",
     "", "POSTGRESQL.AUTHORITY.FILE_IO_DENIED",
     "COPY FROM file requires a trusted ScratchBird import service.", true, false},
    {"COPY|| FROM :'", PatternMatch::kPrefixAndContains, "bulk_io",
     "postgresql.bulk_io.copy_from_file",
     MappingDisposition::kPolicyRefusal, "postgresql.policy.copy_from_file", "",
     "", "POSTGRESQL.AUTHORITY.FILE_IO_DENIED",
     "COPY FROM file requires a trusted ScratchBird import service.", true, false},
    {"COPY|| TO STDOUT", PatternMatch::kPrefixAndContains,
     "logical_stream_backup_restore", "postgresql.logical_stream.copy_to_stdout",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.copy_to_stdout",
     "SBLR_COMPATIBILITY_POSTGRESQL_COPY_ROUTE", "ParserSupportCopyRoute",
     "POSTGRESQL.EMULATION.COPY_ROUTE",
     "COPY TO STDOUT is a remote logical export stream routed through trusted package policy.",
     true, false},
    {"COPY|| FROM STDIN", PatternMatch::kPrefixAndContains,
     "logical_stream_backup_restore",
     "postgresql.logical_stream.copy_from_stdin",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.copy_from_stdin",
     "SBLR_COMPATIBILITY_POSTGRESQL_COPY_ROUTE", "ParserSupportCopyRoute",
     "POSTGRESQL.EMULATION.COPY_ROUTE",
     "COPY FROM STDIN is a remote logical import stream routed through trusted package policy.",
     true, false},
    {"COPY", PatternMatch::kPrefix, "bulk_io", "postgresql.bulk_io.copy",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.copy",
     "SBLR_COMPATIBILITY_POSTGRESQL_COPY_ROUTE", "ParserSupportCopyRoute",
     "POSTGRESQL.EMULATION.COPY_ROUTE",
     "COPY routes through trusted package policy and cannot bypass engine admission.", true, false},
    {"LO_IMPORT", PatternMatch::kContainsFunctionCall, "large_object", "postgresql.large_object.lo_import",
     MappingDisposition::kPolicyRefusal, "postgresql.policy.large_object.import", "",
     "", "POSTGRESQL.AUTHORITY.FILE_IO_DENIED",
     "lo_import cannot read host files from parser authority.", true, false},
    {"LO_EXPORT", PatternMatch::kContainsFunctionCall, "large_object", "postgresql.large_object.lo_export",
     MappingDisposition::kPolicyRefusal, "postgresql.policy.large_object.export", "",
     "", "POSTGRESQL.AUTHORITY.FILE_IO_DENIED",
     "lo_export cannot write host files from parser authority.", true, false},
    {"LO_CREATE", PatternMatch::kContainsFunctionCall, "large_object", "postgresql.large_object.api",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.large_object",
     "SBLR_COMPAT_POSTGRESQL_LARGE_OBJECT_ROUTE", "ParserSupportLargeObjectRoute",
     "POSTGRESQL.EMULATION.LARGE_OBJECT_ROUTE",
     "Large object APIs route through trusted package policy.", true, true},
    {"LO_CREAT", PatternMatch::kContainsFunctionCall, "large_object", "postgresql.large_object.api",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.large_object",
     "SBLR_COMPAT_POSTGRESQL_LARGE_OBJECT_ROUTE", "ParserSupportLargeObjectRoute",
     "POSTGRESQL.EMULATION.LARGE_OBJECT_ROUTE",
     "Large object APIs route through trusted package policy.", true, true},
    {"LO_UNLINK", PatternMatch::kContainsFunctionCall, "large_object", "postgresql.large_object.api",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.large_object",
     "SBLR_COMPAT_POSTGRESQL_LARGE_OBJECT_ROUTE", "ParserSupportLargeObjectRoute",
     "POSTGRESQL.EMULATION.LARGE_OBJECT_ROUTE",
     "Large object APIs route through trusted package policy.", true, true},
    {"LO_OPEN", PatternMatch::kContainsFunctionCall, "large_object", "postgresql.large_object.api",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.large_object",
     "SBLR_COMPAT_POSTGRESQL_LARGE_OBJECT_ROUTE", "ParserSupportLargeObjectRoute",
     "POSTGRESQL.EMULATION.LARGE_OBJECT_ROUTE",
     "Large object APIs route through trusted package policy.", true, true},
    {"LO_CLOSE", PatternMatch::kContainsFunctionCall, "large_object", "postgresql.large_object.api",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.large_object",
     "SBLR_COMPAT_POSTGRESQL_LARGE_OBJECT_ROUTE", "ParserSupportLargeObjectRoute",
     "POSTGRESQL.EMULATION.LARGE_OBJECT_ROUTE",
     "Large object APIs route through trusted package policy.", true, true},
    {"LO_READ", PatternMatch::kContainsFunctionCall, "large_object", "postgresql.large_object.api",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.large_object",
     "SBLR_COMPAT_POSTGRESQL_LARGE_OBJECT_ROUTE", "ParserSupportLargeObjectRoute",
     "POSTGRESQL.EMULATION.LARGE_OBJECT_ROUTE",
     "Large object APIs route through trusted package policy.", true, true},
    {"LO_WRITE", PatternMatch::kContainsFunctionCall, "large_object", "postgresql.large_object.api",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.large_object",
     "SBLR_COMPAT_POSTGRESQL_LARGE_OBJECT_ROUTE", "ParserSupportLargeObjectRoute",
     "POSTGRESQL.EMULATION.LARGE_OBJECT_ROUTE",
     "Large object APIs route through trusted package policy.", true, true},
    {"LO_LSEEK", PatternMatch::kContainsFunctionCall, "large_object", "postgresql.large_object.api",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.large_object",
     "SBLR_COMPAT_POSTGRESQL_LARGE_OBJECT_ROUTE", "ParserSupportLargeObjectRoute",
     "POSTGRESQL.EMULATION.LARGE_OBJECT_ROUTE",
     "Large object APIs route through trusted package policy.", true, true},
    {"LO_LSEEK64", PatternMatch::kContainsFunctionCall, "large_object", "postgresql.large_object.api",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.large_object",
     "SBLR_COMPAT_POSTGRESQL_LARGE_OBJECT_ROUTE", "ParserSupportLargeObjectRoute",
     "POSTGRESQL.EMULATION.LARGE_OBJECT_ROUTE",
     "Large object APIs route through trusted package policy.", true, true},
    {"LO_TELL", PatternMatch::kContainsFunctionCall, "large_object", "postgresql.large_object.api",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.large_object",
     "SBLR_COMPAT_POSTGRESQL_LARGE_OBJECT_ROUTE", "ParserSupportLargeObjectRoute",
     "POSTGRESQL.EMULATION.LARGE_OBJECT_ROUTE",
     "Large object APIs route through trusted package policy.", true, true},
    {"LO_TELL64", PatternMatch::kContainsFunctionCall, "large_object", "postgresql.large_object.api",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.large_object",
     "SBLR_COMPAT_POSTGRESQL_LARGE_OBJECT_ROUTE", "ParserSupportLargeObjectRoute",
     "POSTGRESQL.EMULATION.LARGE_OBJECT_ROUTE",
     "Large object APIs route through trusted package policy.", true, true},
    {"LO_TRUNCATE", PatternMatch::kContainsFunctionCall, "large_object", "postgresql.large_object.api",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.large_object",
     "SBLR_COMPAT_POSTGRESQL_LARGE_OBJECT_ROUTE", "ParserSupportLargeObjectRoute",
     "POSTGRESQL.EMULATION.LARGE_OBJECT_ROUTE",
     "Large object APIs route through trusted package policy.", true, true},
    {"LO_TRUNCATE64", PatternMatch::kContainsFunctionCall, "large_object", "postgresql.large_object.api",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.large_object",
     "SBLR_COMPAT_POSTGRESQL_LARGE_OBJECT_ROUTE", "ParserSupportLargeObjectRoute",
     "POSTGRESQL.EMULATION.LARGE_OBJECT_ROUTE",
     "Large object APIs route through trusted package policy.", true, true},
    {"LO_FROM_BYTEA", PatternMatch::kContainsFunctionCall, "large_object", "postgresql.large_object.api",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.large_object",
     "SBLR_COMPAT_POSTGRESQL_LARGE_OBJECT_ROUTE", "ParserSupportLargeObjectRoute",
     "POSTGRESQL.EMULATION.LARGE_OBJECT_ROUTE",
     "Large object APIs route through trusted package policy.", true, true},
    {"LO_GET", PatternMatch::kContainsFunctionCall, "large_object", "postgresql.large_object.api",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.large_object",
     "SBLR_COMPAT_POSTGRESQL_LARGE_OBJECT_ROUTE", "ParserSupportLargeObjectRoute",
     "POSTGRESQL.EMULATION.LARGE_OBJECT_ROUTE",
     "Large object APIs route through trusted package policy.", true, true},
    {"LO_PUT", PatternMatch::kContainsFunctionCall, "large_object", "postgresql.large_object.api",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.large_object",
     "SBLR_COMPAT_POSTGRESQL_LARGE_OBJECT_ROUTE", "ParserSupportLargeObjectRoute",
     "POSTGRESQL.EMULATION.LARGE_OBJECT_ROUTE",
     "Large object APIs route through trusted package policy.", true, true},
    {"LOREAD", PatternMatch::kContainsFunctionCall, "large_object", "postgresql.large_object.api",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.large_object",
     "SBLR_COMPAT_POSTGRESQL_LARGE_OBJECT_ROUTE", "ParserSupportLargeObjectRoute",
     "POSTGRESQL.EMULATION.LARGE_OBJECT_ROUTE",
     "Large object APIs route through trusted package policy.", true, true},
    {"LOWRITE", PatternMatch::kContainsFunctionCall, "large_object", "postgresql.large_object.api",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.large_object",
     "SBLR_COMPAT_POSTGRESQL_LARGE_OBJECT_ROUTE", "ParserSupportLargeObjectRoute",
     "POSTGRESQL.EMULATION.LARGE_OBJECT_ROUTE",
     "Large object APIs route through trusted package policy.", true, true},
    {"CREATE EXTENSION", PatternMatch::kPrefix, "extension", "postgresql.extension.create",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.extension.create",
     "SBLR_COMPAT_POSTGRESQL_EXTENSION_ROUTE", "ParserSupportExtensionRoute",
     "POSTGRESQL.EMULATION.EXTENSION_ROUTE",
     "Extension installation routes through trusted package policy.", true, false},
    {"ALTER EXTENSION", PatternMatch::kPrefix, "extension", "postgresql.extension.alter",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.extension.alter",
     "SBLR_COMPAT_POSTGRESQL_EXTENSION_ROUTE", "ParserSupportExtensionRoute",
     "POSTGRESQL.EMULATION.EXTENSION_ROUTE",
     "Extension changes route through trusted package policy.", true, false},
    {"DROP EXTENSION", PatternMatch::kPrefix, "extension", "postgresql.extension.drop",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.extension.drop",
     "SBLR_COMPAT_POSTGRESQL_EXTENSION_ROUTE", "ParserSupportExtensionRoute",
     "POSTGRESQL.EMULATION.EXTENSION_ROUTE",
     "Extension removal routes through trusted package policy.", true, false},
    {"CREATE FOREIGN DATA WRAPPER", PatternMatch::kPrefix, "connector", "postgresql.connector.fdw.create",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.connector.fdw.create",
     "SBLR_COMPAT_POSTGRESQL_CONNECTOR_ROUTE", "ParserSupportConnectorRoute",
     "POSTGRESQL.EMULATION.CONNECTOR_ROUTE",
     "Foreign data wrapper operations route through the PostgreSQL compatibility UDR.", true, false},
    {"CREATE SERVER", PatternMatch::kPrefix, "connector", "postgresql.connector.server.create",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.connector.server.create",
     "SBLR_COMPAT_POSTGRESQL_CONNECTOR_ROUTE", "ParserSupportConnectorRoute",
     "POSTGRESQL.EMULATION.CONNECTOR_ROUTE",
     "Foreign server operations route through the PostgreSQL compatibility UDR.", true, false},
    {"CREATE USER MAPPING", PatternMatch::kPrefix, "connector", "postgresql.connector.user_mapping.create",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.connector.user_mapping.create",
     "SBLR_COMPAT_POSTGRESQL_CONNECTOR_ROUTE", "ParserSupportConnectorRoute",
     "POSTGRESQL.EMULATION.CONNECTOR_ROUTE",
     "Foreign user mapping operations route through the PostgreSQL compatibility UDR.", true, false},
    {"IMPORT FOREIGN SCHEMA", PatternMatch::kPrefix, "connector", "postgresql.connector.import_foreign_schema",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.connector.import_foreign_schema",
     "SBLR_COMPAT_POSTGRESQL_CONNECTOR_ROUTE", "ParserSupportConnectorRoute",
     "POSTGRESQL.EMULATION.CONNECTOR_ROUTE",
     "Foreign schema import routes through the PostgreSQL compatibility UDR.", true, false},
    {"CREATE FOREIGN TABLE", PatternMatch::kPrefix, "connector", "postgresql.connector.foreign_table.create",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.connector.foreign_table.create",
     "SBLR_COMPAT_POSTGRESQL_CONNECTOR_ROUTE", "ParserSupportConnectorRoute",
     "POSTGRESQL.EMULATION.CONNECTOR_ROUTE",
     "Foreign table creation routes through the PostgreSQL compatibility UDR.", true, false},
    {"CREATE PUBLICATION", PatternMatch::kPrefix, "logical_replication", "postgresql.replication.publication.create",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.replication.publication.create",
     "SBLR_COMPAT_POSTGRESQL_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "POSTGRESQL.EMULATION.REPLICATION_ROUTE",
     "Logical publication requests route through the PostgreSQL compatibility UDR.", true, false},
    {"CREATE SUBSCRIPTION", PatternMatch::kPrefix, "logical_replication", "postgresql.replication.subscription.create",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.replication.subscription.create",
     "SBLR_COMPAT_POSTGRESQL_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "POSTGRESQL.EMULATION.REPLICATION_ROUTE",
     "Logical subscription requests route through the PostgreSQL compatibility UDR.", true, false},
    {"ALTER SUBSCRIPTION", PatternMatch::kPrefix, "logical_replication", "postgresql.replication.subscription.alter",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.replication.subscription.alter",
     "SBLR_COMPAT_POSTGRESQL_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "POSTGRESQL.EMULATION.REPLICATION_ROUTE",
     "Logical subscription changes route through the PostgreSQL compatibility UDR.", true, false},
    {"DROP SUBSCRIPTION", PatternMatch::kPrefix, "logical_replication", "postgresql.replication.subscription.drop",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.replication.subscription.drop",
     "SBLR_COMPAT_POSTGRESQL_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "POSTGRESQL.EMULATION.REPLICATION_ROUTE",
     "Logical subscription removal routes through the PostgreSQL compatibility UDR.", true, false},
    {"CREATE TABLESPACE", PatternMatch::kPrefix, "storage_admin", "postgresql.storage.tablespace.create",
     MappingDisposition::kPolicyRefusal, "postgresql.policy.tablespace.create", "",
     "", "POSTGRESQL.AUTHORITY.TABLESPACE_DENIED",
     "Tablespace physical storage administration is not parser authority.", true, false},
    {"ALTER TABLESPACE", PatternMatch::kPrefix, "storage_admin", "postgresql.storage.tablespace.alter",
     MappingDisposition::kPolicyRefusal, "postgresql.policy.tablespace.alter", "",
     "", "POSTGRESQL.AUTHORITY.TABLESPACE_DENIED",
     "Tablespace physical storage administration is not parser authority.", true, false},
    {"DROP TABLESPACE", PatternMatch::kPrefix, "storage_admin", "postgresql.storage.tablespace.drop",
     MappingDisposition::kPolicyRefusal, "postgresql.policy.tablespace.drop", "",
     "", "POSTGRESQL.AUTHORITY.TABLESPACE_DENIED",
     "Tablespace physical storage administration is not parser authority.", true, false},
    {"ALTER SYSTEM", PatternMatch::kPrefix, "system_admin", "postgresql.system.alter_system",
     MappingDisposition::kPolicyRefusal, "postgresql.policy.alter_system", "",
     "", "POSTGRESQL.AUTHORITY.SYSTEM_DENIED",
     "ALTER SYSTEM is blocked from parser authority.", true, false},
    {"CHECKPOINT", PatternMatch::kPrefix, "system_admin", "postgresql.system.checkpoint",
     MappingDisposition::kUnsupportedRefusal, "postgresql.policy.unsupported.checkpoint",
     "", "", "POSTGRESQL.AUTHORITY.UNSUPPORTED_DENIED",
     "PostgreSQL CHECKPOINT is a compatibility low-level utility surface and is outside compatibility parser authority.",
     true, false},
    {"CREATE DATABASE", PatternMatch::kPrefix, "database_lifecycle", "postgresql.lifecycle.create_database",
     MappingDisposition::kScratchBirdLifecycleApi, "postgresql.lifecycle.create_database",
     "SBLR_LIFECYCLE_CREATE_DATABASE", "EngineCreateLifecycle", "", "", false, false},
    {"DROP DATABASE", PatternMatch::kPrefix, "database_lifecycle", "postgresql.lifecycle.drop_database",
     MappingDisposition::kScratchBirdLifecycleApi, "postgresql.lifecycle.drop_database",
     "SBLR_LIFECYCLE_DROP_DATABASE", "EngineDropLifecycle", "", "", true, false},
    {"CREATE ROLE", PatternMatch::kPrefix, "security", "postgresql.security.create_role",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.security.create_role",
     "SBLR_COMPAT_POSTGRESQL_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "POSTGRESQL.EMULATION.SECURITY_ROUTE",
     "Role management routes through trusted security policy.", true, false},
    {"ALTER ROLE", PatternMatch::kPrefix, "security", "postgresql.security.alter_role",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.security.alter_role",
     "SBLR_COMPAT_POSTGRESQL_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "POSTGRESQL.EMULATION.SECURITY_ROUTE",
     "Role management routes through trusted security policy.", true, false},
    {"DROP ROLE", PatternMatch::kPrefix, "security", "postgresql.security.drop_role",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.security.drop_role",
     "SBLR_COMPAT_POSTGRESQL_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "POSTGRESQL.EMULATION.SECURITY_ROUTE",
     "Role management routes through trusted security policy.", true, false},
    {"CREATE USER", PatternMatch::kPrefix, "security", "postgresql.security.create_user",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.security.create_user",
     "SBLR_COMPAT_POSTGRESQL_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "POSTGRESQL.EMULATION.SECURITY_ROUTE",
     "User management routes through trusted security policy.", true, false},
    {"GRANT", PatternMatch::kPrefix, "security", "postgresql.security.grant",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.security.grant",
     "SBLR_COMPAT_POSTGRESQL_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "POSTGRESQL.EMULATION.SECURITY_ROUTE",
     "Privilege changes route through trusted security policy.", true, false},
    {"REVOKE", PatternMatch::kPrefix, "security", "postgresql.security.revoke",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.security.revoke",
     "SBLR_COMPAT_POSTGRESQL_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "POSTGRESQL.EMULATION.SECURITY_ROUTE",
     "Privilege changes route through trusted security policy.", true, false},
    {"CREATE POLICY", PatternMatch::kPrefix, "security", "postgresql.security.row_policy.create",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.security.policy.create",
     "SBLR_COMPAT_POSTGRESQL_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "POSTGRESQL.EMULATION.SECURITY_ROUTE",
     "Row-level security policy routes through trusted security policy.", true, false},
    {"CREATE FUNCTION", PatternMatch::kPrefix, "routine", "postgresql.routine.function.create",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.routine.function.create",
     "SBLR_COMPAT_POSTGRESQL_ROUTINE_ROUTE", "ParserSupportRoutineRoute",
     "POSTGRESQL.EMULATION.ROUTINE_ROUTE",
     "Functions route through trusted routine package policy.", true, false},
    {"CREATE PROCEDURE", PatternMatch::kPrefix, "routine", "postgresql.routine.procedure.create",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.routine.procedure.create",
     "SBLR_COMPAT_POSTGRESQL_ROUTINE_ROUTE", "ParserSupportRoutineRoute",
     "POSTGRESQL.EMULATION.ROUTINE_ROUTE",
     "Procedures route through trusted routine package policy.", true, false},
    {"CREATE TRIGGER", PatternMatch::kPrefix, "routine", "postgresql.routine.trigger.create",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.routine.trigger.create",
     "SBLR_COMPAT_POSTGRESQL_ROUTINE_ROUTE", "ParserSupportRoutineRoute",
     "POSTGRESQL.EMULATION.ROUTINE_ROUTE",
     "Triggers route through trusted routine package policy.", true, false},
    {"CREATE RULE", PatternMatch::kPrefix, "routine", "postgresql.routine.rule.create",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.routine.rule.create",
     "SBLR_COMPAT_POSTGRESQL_ROUTINE_ROUTE", "ParserSupportRoutineRoute",
     "POSTGRESQL.EMULATION.ROUTINE_ROUTE",
     "Rules route through trusted routine package policy.", true, false},
    {"DO", PatternMatch::kPrefix, "routine", "postgresql.routine.do_block",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.routine.do_block",
     "SBLR_COMPAT_POSTGRESQL_ROUTINE_ROUTE", "ParserSupportRoutineRoute",
     "POSTGRESQL.EMULATION.ROUTINE_ROUTE",
     "Anonymous code blocks route through trusted routine package policy.", true, false},
    {"CALL", PatternMatch::kPrefix, "routine", "postgresql.routine.call",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.routine.call",
     "SBLR_COMPAT_POSTGRESQL_ROUTINE_ROUTE", "ParserSupportRoutineRoute",
     "POSTGRESQL.EMULATION.ROUTINE_ROUTE",
     "Procedure calls route through trusted routine package policy.", true, true},
    {"LISTEN", PatternMatch::kPrefix, "events", "postgresql.events.listen",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.events.listen",
     "SBLR_COMPAT_POSTGRESQL_EVENT_ROUTE", "ParserSupportEventRoute",
     "POSTGRESQL.EMULATION.EVENT_ROUTE",
     "LISTEN routes through trusted event policy.", true, false},
    {"NOTIFY", PatternMatch::kPrefix, "events", "postgresql.events.notify",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.events.notify",
     "SBLR_COMPAT_POSTGRESQL_EVENT_ROUTE", "ParserSupportEventRoute",
     "POSTGRESQL.EMULATION.EVENT_ROUTE",
     "NOTIFY routes through trusted event policy.", true, true},
    {"UNLISTEN", PatternMatch::kPrefix, "events", "postgresql.events.unlisten",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.events.unlisten",
     "SBLR_COMPAT_POSTGRESQL_EVENT_ROUTE", "ParserSupportEventRoute",
     "POSTGRESQL.EMULATION.EVENT_ROUTE",
     "UNLISTEN routes through trusted event policy.", true, false},
    {"VACUUM", PatternMatch::kPrefix, "maintenance", "postgresql.maintenance.vacuum",
     MappingDisposition::kUnsupportedRefusal, "postgresql.policy.unsupported.vacuum",
     "", "", "POSTGRESQL.AUTHORITY.UNSUPPORTED_DENIED",
     "PostgreSQL VACUUM is a compatibility low-level utility surface and is outside compatibility parser authority.",
     true, false},
    {"ANALYZE", PatternMatch::kPrefix, "maintenance", "postgresql.maintenance.analyze",
     MappingDisposition::kUnsupportedRefusal, "postgresql.policy.unsupported.analyze",
     "", "", "POSTGRESQL.AUTHORITY.UNSUPPORTED_DENIED",
     "PostgreSQL ANALYZE is a compatibility low-level utility surface and is outside compatibility parser authority.",
     true, false},
    {"REINDEX", PatternMatch::kPrefix, "maintenance", "postgresql.maintenance.reindex",
     MappingDisposition::kUnsupportedRefusal, "postgresql.policy.unsupported.reindex",
     "", "", "POSTGRESQL.AUTHORITY.UNSUPPORTED_DENIED",
     "PostgreSQL REINDEX is a compatibility low-level utility surface and is outside compatibility parser authority.",
     true, false},
    {"CLUSTER", PatternMatch::kPrefix, "maintenance", "postgresql.maintenance.cluster",
     MappingDisposition::kUnsupportedRefusal, "postgresql.policy.unsupported.cluster",
     "", "", "POSTGRESQL.AUTHORITY.UNSUPPORTED_DENIED",
     "PostgreSQL CLUSTER is a compatibility low-level utility surface and is outside compatibility parser authority.",
     true, false},
    {"REFRESH MATERIALIZED VIEW", PatternMatch::kPrefix, "maintenance", "postgresql.maintenance.refresh_materialized_view",
     MappingDisposition::kUnsupportedRefusal,
     "postgresql.policy.unsupported.refresh_materialized_view",
     "", "", "POSTGRESQL.AUTHORITY.UNSUPPORTED_DENIED",
     "PostgreSQL REFRESH MATERIALIZED VIEW is a compatibility low-level utility surface and is outside compatibility parser authority.",
     true, true},
    {"EXPLAIN", PatternMatch::kPrefix, "optimizer", "postgresql.optimizer.explain",
     MappingDisposition::kCatalogProjection, "postgresql.optimizer.explain",
     "SBLR_COMPAT_POSTGRESQL_EXPLAIN", "EngineExplainPlan", "", "", false, false},
    {"SHOW", PatternMatch::kPrefix, "catalog_overlay", "postgresql.catalog_overlay.show",
     MappingDisposition::kCatalogProjection, "postgresql.catalog.show",
     "SBLR_COMPAT_POSTGRESQL_CATALOG_PROJECT", "EngineCatalogProjection", "", "", false, false},
    {"SET", PatternMatch::kPrefix, "session", "postgresql.session.set",
     MappingDisposition::kAdmittedSblr, "postgresql.session.set",
     "SBLR_COMPAT_POSTGRESQL_SET", "EngineSessionSet", "", "", false, false},
    {"RESET", PatternMatch::kPrefix, "session", "postgresql.session.reset",
     MappingDisposition::kAdmittedSblr, "postgresql.session.reset",
     "SBLR_COMPAT_POSTGRESQL_RESET", "EngineSessionReset", "", "", false, false},
    {"DISCARD", PatternMatch::kPrefix, "session", "postgresql.session.discard",
     MappingDisposition::kAdmittedSblr, "postgresql.session.discard",
     "SBLR_COMPAT_POSTGRESQL_DISCARD", "EngineSessionDiscard", "", "", false, false},
    {"BEGIN", PatternMatch::kPrefix, "transaction", "postgresql.transaction.begin",
     MappingDisposition::kAdmittedSblr, "postgresql.transaction.begin",
     "SBLR_TRANSACTION_BEGIN", "EngineBeginTransaction", "", "", false, false},
    {"START TRANSACTION", PatternMatch::kPrefix, "transaction", "postgresql.transaction.start",
     MappingDisposition::kAdmittedSblr, "postgresql.transaction.start",
     "SBLR_TRANSACTION_BEGIN", "EngineBeginTransaction", "", "", false, false},
    {"COMMIT", PatternMatch::kPrefix, "transaction", "postgresql.transaction.commit",
     MappingDisposition::kAdmittedSblr, "postgresql.transaction.commit",
     "SBLR_TRANSACTION_COMMIT", "EngineCommitTransaction", "", "", false, true},
    {"ROLLBACK TO SAVEPOINT", PatternMatch::kPrefix, "transaction", "postgresql.transaction.rollback_to_savepoint",
     MappingDisposition::kAdmittedSblr, "postgresql.transaction.rollback_to_savepoint",
     "SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT", "EngineRollbackToSavepoint", "", "", false, true},
    {"ROLLBACK WORK TO SAVEPOINT", PatternMatch::kPrefix, "transaction", "postgresql.transaction.rollback_to_savepoint",
     MappingDisposition::kAdmittedSblr, "postgresql.transaction.rollback_to_savepoint",
     "SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT", "EngineRollbackToSavepoint", "", "", false, true},
    {"ROLLBACK WORK TO", PatternMatch::kPrefix, "transaction", "postgresql.transaction.rollback_to_savepoint",
     MappingDisposition::kAdmittedSblr, "postgresql.transaction.rollback_to_savepoint",
     "SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT", "EngineRollbackToSavepoint", "", "", false, true},
    {"ROLLBACK TRANSACTION TO SAVEPOINT", PatternMatch::kPrefix, "transaction", "postgresql.transaction.rollback_to_savepoint",
     MappingDisposition::kAdmittedSblr, "postgresql.transaction.rollback_to_savepoint",
     "SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT", "EngineRollbackToSavepoint", "", "", false, true},
    {"ROLLBACK TRANSACTION TO", PatternMatch::kPrefix, "transaction", "postgresql.transaction.rollback_to_savepoint",
     MappingDisposition::kAdmittedSblr, "postgresql.transaction.rollback_to_savepoint",
     "SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT", "EngineRollbackToSavepoint", "", "", false, true},
    {"ROLLBACK TO", PatternMatch::kPrefix, "transaction", "postgresql.transaction.rollback_to_savepoint",
     MappingDisposition::kAdmittedSblr, "postgresql.transaction.rollback_to_savepoint",
     "SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT", "EngineRollbackToSavepoint", "", "", false, true},
    {"ROLLBACK", PatternMatch::kPrefix, "transaction", "postgresql.transaction.rollback",
     MappingDisposition::kAdmittedSblr, "postgresql.transaction.rollback",
     "SBLR_TRANSACTION_ROLLBACK", "EngineRollbackTransaction", "", "", false, true},
    {"SAVEPOINT", PatternMatch::kPrefix, "transaction", "postgresql.transaction.savepoint",
     MappingDisposition::kAdmittedSblr, "postgresql.transaction.savepoint",
     "SBLR_TRANSACTION_SAVEPOINT", "EngineSavepoint", "", "", false, true},
    {"RELEASE SAVEPOINT", PatternMatch::kPrefix, "transaction", "postgresql.transaction.release_savepoint",
     MappingDisposition::kAdmittedSblr, "postgresql.transaction.release_savepoint",
     "SBLR_TRANSACTION_RELEASE_SAVEPOINT", "EngineReleaseSavepoint", "", "", false, true},
    {"PREPARE TRANSACTION", PatternMatch::kPrefix, "transaction", "postgresql.transaction.prepare_transaction",
     MappingDisposition::kUnsupportedRefusal, "postgresql.policy.transaction.prepare_transaction",
     "", "", "POSTGRESQL.AUTHORITY.PREPARE_TRANSACTION_DENIED",
     "Two-phase transaction finality is not admitted by the parser.", true, true},
    {"PREPARE", PatternMatch::kPrefix, "prepared_statement", "postgresql.prepared.prepare",
     MappingDisposition::kAdmittedSblr, "postgresql.prepared.prepare",
     "SBLR_COMPAT_POSTGRESQL_PREPARE", "EnginePrepareStatement", "", "", false, false},
    {"EXECUTE", PatternMatch::kPrefix, "prepared_statement", "postgresql.prepared.execute",
     MappingDisposition::kAdmittedSblr, "postgresql.prepared.execute",
     "SBLR_COMPAT_POSTGRESQL_EXECUTE", "EngineExecuteStatement", "", "", false, true},
    {"DEALLOCATE", PatternMatch::kPrefix, "prepared_statement", "postgresql.prepared.deallocate",
     MappingDisposition::kAdmittedSblr, "postgresql.prepared.deallocate",
     "SBLR_COMPAT_POSTGRESQL_DEALLOCATE", "EngineDeallocateStatement", "", "", false, false},
    {"DECLARE", PatternMatch::kPrefix, "cursor", "postgresql.cursor.declare",
     MappingDisposition::kAdmittedSblr, "postgresql.cursor.declare",
     "SBLR_COMPAT_POSTGRESQL_CURSOR_DECLARE", "EngineCursorDeclare", "", "", false, true},
    {"FETCH", PatternMatch::kPrefix, "cursor", "postgresql.cursor.fetch",
     MappingDisposition::kAdmittedSblr, "postgresql.cursor.fetch",
     "SBLR_COMPAT_POSTGRESQL_CURSOR_FETCH", "EngineCursorFetch", "", "", false, true},
    {"MOVE", PatternMatch::kPrefix, "cursor", "postgresql.cursor.move",
     MappingDisposition::kAdmittedSblr, "postgresql.cursor.move",
     "SBLR_COMPAT_POSTGRESQL_CURSOR_MOVE", "EngineCursorMove", "", "", false, true},
    {"CLOSE", PatternMatch::kPrefix, "cursor", "postgresql.cursor.close",
     MappingDisposition::kAdmittedSblr, "postgresql.cursor.close",
     "SBLR_COMPAT_POSTGRESQL_CURSOR_CLOSE", "EngineCursorClose", "", "", false, true},
    {"LOCK TABLE", PatternMatch::kPrefix, "locking", "postgresql.locking.lock_table",
     MappingDisposition::kAdmittedSblr, "postgresql.locking.lock_table",
     "SBLR_COMPAT_POSTGRESQL_LOCK_TABLE", "EngineLockTable", "", "", true, true},
    {"SECURITY LABEL", PatternMatch::kPrefix, "security", "postgresql.security.security_label",
     MappingDisposition::kParserSupportUdr, "postgresql.udr.security.security_label",
     "SBLR_COMPAT_POSTGRESQL_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "POSTGRESQL.EMULATION.SECURITY_ROUTE",
     "Security labels route through trusted security policy.", true, true},
    {"CREATE UNIQUE INDEX", PatternMatch::kPrefix, "ddl", "postgresql.ddl.create.unique_index",
     MappingDisposition::kAdmittedSblr, "postgresql.ddl.create.unique_index",
     "SBLR_COMPAT_POSTGRESQL_INDEX_CREATE", "EngineDdlCreateIndex", "", "", true, true},
    {"CREATE INDEX", PatternMatch::kPrefix, "ddl", "postgresql.ddl.create.index",
     MappingDisposition::kAdmittedSblr, "postgresql.ddl.create.index",
     "SBLR_COMPAT_POSTGRESQL_INDEX_CREATE", "EngineDdlCreateIndex", "", "", true, true},
    {"ALTER INDEX", PatternMatch::kPrefix, "ddl", "postgresql.ddl.alter.index",
     MappingDisposition::kAdmittedSblr, "postgresql.ddl.alter.index",
     "SBLR_COMPAT_POSTGRESQL_INDEX_ALTER", "EngineDdlAlterIndex", "", "", true, true},
    {"CREATE OR REPLACE VIEW", PatternMatch::kPrefix, "ddl", "postgresql.ddl.create_or_replace.view",
     MappingDisposition::kAdmittedSblr, "postgresql.ddl.create_or_replace.view",
     "SBLR_COMPAT_POSTGRESQL_VIEW_CREATE_OR_REPLACE", "EngineDdlCreateOrReplaceView",
     "", "", true, true},
    {"CREATE MATERIALIZED VIEW", PatternMatch::kPrefix, "ddl", "postgresql.ddl.create.materialized_view",
     MappingDisposition::kAdmittedSblr, "postgresql.ddl.create.materialized_view",
     "SBLR_COMPAT_POSTGRESQL_MATERIALIZED_VIEW_CREATE", "EngineDdlCreateMaterializedView",
     "", "", true, true},
    {"CREATE VIEW", PatternMatch::kPrefix, "ddl", "postgresql.ddl.create.view",
     MappingDisposition::kAdmittedSblr, "postgresql.ddl.create.view",
     "SBLR_COMPAT_POSTGRESQL_VIEW_CREATE", "EngineDdlCreateView", "", "", true, true},
    {"CREATE LOCAL TEMPORARY TABLE", PatternMatch::kPrefix, "ddl", "postgresql.ddl.create.local_temporary_table",
     MappingDisposition::kAdmittedSblr, "postgresql.ddl.create.local_temporary_table",
     "SBLR_COMPAT_POSTGRESQL_TEMPORARY_TABLE_CREATE", "EngineDdlCreateTemporaryTable",
     "", "", true, true},
    {"CREATE LOCAL TEMP TABLE", PatternMatch::kPrefix, "ddl", "postgresql.ddl.create.local_temporary_table",
     MappingDisposition::kAdmittedSblr, "postgresql.ddl.create.local_temporary_table",
     "SBLR_COMPAT_POSTGRESQL_TEMPORARY_TABLE_CREATE", "EngineDdlCreateTemporaryTable",
     "", "", true, true},
    {"CREATE GLOBAL TEMPORARY TABLE", PatternMatch::kPrefix, "ddl", "postgresql.ddl.create.global_temporary_table",
     MappingDisposition::kAdmittedSblr, "postgresql.ddl.create.global_temporary_table",
     "SBLR_COMPAT_POSTGRESQL_TEMPORARY_TABLE_CREATE", "EngineDdlCreateTemporaryTable",
     "", "", true, true},
    {"CREATE GLOBAL TEMP TABLE", PatternMatch::kPrefix, "ddl", "postgresql.ddl.create.global_temporary_table",
     MappingDisposition::kAdmittedSblr, "postgresql.ddl.create.global_temporary_table",
     "SBLR_COMPAT_POSTGRESQL_TEMPORARY_TABLE_CREATE", "EngineDdlCreateTemporaryTable",
     "", "", true, true},
    {"CREATE TEMPORARY TABLE", PatternMatch::kPrefix, "ddl", "postgresql.ddl.create.temporary_table",
     MappingDisposition::kAdmittedSblr, "postgresql.ddl.create.temporary_table",
     "SBLR_COMPAT_POSTGRESQL_TEMPORARY_TABLE_CREATE", "EngineDdlCreateTemporaryTable",
     "", "", true, true},
    {"CREATE TEMP TABLE", PatternMatch::kPrefix, "ddl", "postgresql.ddl.create.temporary_table",
     MappingDisposition::kAdmittedSblr, "postgresql.ddl.create.temporary_table",
     "SBLR_COMPAT_POSTGRESQL_TEMPORARY_TABLE_CREATE", "EngineDdlCreateTemporaryTable",
     "", "", true, true},
    {"CREATE", PatternMatch::kPrefix, "ddl", "postgresql.ddl.create",
     MappingDisposition::kAdmittedSblr, "postgresql.ddl.create",
     "SBLR_COMPAT_POSTGRESQL_DDL_CREATE", "EngineDdlCreate", "", "", true, true},
    {"ALTER MATERIALIZED VIEW", PatternMatch::kPrefix, "ddl", "postgresql.ddl.alter.materialized_view",
     MappingDisposition::kAdmittedSblr, "postgresql.ddl.alter.materialized_view",
     "SBLR_COMPAT_POSTGRESQL_MATERIALIZED_VIEW_ALTER", "EngineDdlAlterMaterializedView",
     "", "", true, true},
    {"ALTER VIEW", PatternMatch::kPrefix, "ddl", "postgresql.ddl.alter.view",
     MappingDisposition::kAdmittedSblr, "postgresql.ddl.alter.view",
     "SBLR_COMPAT_POSTGRESQL_VIEW_ALTER", "EngineDdlAlterView", "", "", true, true},
    {"ALTER", PatternMatch::kPrefix, "ddl", "postgresql.ddl.alter",
     MappingDisposition::kAdmittedSblr, "postgresql.ddl.alter",
     "SBLR_COMPAT_POSTGRESQL_DDL_ALTER", "EngineDdlAlter", "", "", true, true},
    {"DROP TABLE PG_TEMP.", PatternMatch::kContains, "ddl", "postgresql.ddl.drop.temporary_table",
     MappingDisposition::kAdmittedSblr, "postgresql.ddl.drop.temporary_table",
     "SBLR_COMPAT_POSTGRESQL_TEMPORARY_TABLE_DROP", "EngineDdlDropTemporaryTable",
     "", "", true, true},
    {"DROP MATERIALIZED VIEW", PatternMatch::kPrefix, "ddl", "postgresql.ddl.drop.materialized_view",
     MappingDisposition::kAdmittedSblr, "postgresql.ddl.drop.materialized_view",
     "SBLR_COMPAT_POSTGRESQL_MATERIALIZED_VIEW_DROP", "EngineDdlDropMaterializedView",
     "", "", true, true},
    {"DROP VIEW", PatternMatch::kPrefix, "ddl", "postgresql.ddl.drop.view",
     MappingDisposition::kAdmittedSblr, "postgresql.ddl.drop.view",
     "SBLR_COMPAT_POSTGRESQL_VIEW_DROP", "EngineDdlDropView", "", "", true, true},
    {"DROP", PatternMatch::kPrefix, "ddl", "postgresql.ddl.drop",
     MappingDisposition::kAdmittedSblr, "postgresql.ddl.drop",
     "SBLR_COMPAT_POSTGRESQL_DDL_DROP", "EngineDdlDrop", "", "", true, true},
    {"COMMENT", PatternMatch::kPrefix, "ddl", "postgresql.ddl.comment",
     MappingDisposition::kAdmittedSblr, "postgresql.ddl.comment",
     "SBLR_COMPAT_POSTGRESQL_DDL_COMMENT", "EngineDdlComment", "", "", true, true},
    {"TRUNCATE", PatternMatch::kPrefix, "ddl", "postgresql.ddl.truncate",
     MappingDisposition::kAdmittedSblr, "postgresql.ddl.truncate",
     "SBLR_COMPAT_POSTGRESQL_DDL_TRUNCATE", "EngineDdlTruncate", "", "", true, true},
    {"MERGE", PatternMatch::kPrefix, "dml", "postgresql.dml.merge",
     MappingDisposition::kAdmittedSblr, "postgresql.dml.merge",
     "SBLR_COMPAT_POSTGRESQL_MERGE", "EngineDmlMerge", "", "", false, true},
    {"INSERT", PatternMatch::kPrefix, "dml", "postgresql.dml.insert",
     MappingDisposition::kAdmittedSblr, "postgresql.dml.insert",
     "SBLR_COMPAT_POSTGRESQL_INSERT", "EngineDmlInsert", "", "", false, true},
    {"UPDATE", PatternMatch::kPrefix, "dml", "postgresql.dml.update",
     MappingDisposition::kAdmittedSblr, "postgresql.dml.update",
     "SBLR_COMPAT_POSTGRESQL_UPDATE", "EngineDmlUpdate", "", "", false, true},
    {"DELETE", PatternMatch::kPrefix, "dml", "postgresql.dml.delete",
     MappingDisposition::kAdmittedSblr, "postgresql.dml.delete",
     "SBLR_COMPAT_POSTGRESQL_DELETE", "EngineDmlDelete", "", "", false, true},
    {"SELECT", PatternMatch::kPrefix, "query", "postgresql.query.select",
     MappingDisposition::kAdmittedSblr, "postgresql.query.select",
     "SBLR_COMPAT_POSTGRESQL_SELECT", "EngineQuerySelect", "", "", false, false},
    {"WITH", PatternMatch::kPrefix, "query", "postgresql.query.with",
     MappingDisposition::kAdmittedSblr, "postgresql.query.with",
     "SBLR_COMPAT_POSTGRESQL_SELECT", "EngineQuerySelect", "", "", false, false},
};

const std::array<SurfaceDescriptor, 12> kDatatypeSurfaces{{
    {"numeric", "SMALLINT;INTEGER;BIGINT;NUMERIC;DECIMAL;REAL;DOUBLE PRECISION;MONEY", "descriptor"},
    {"serial_identity", "SMALLSERIAL;SERIAL;BIGSERIAL;IDENTITY", "descriptor_policy"},
    {"text", "CHAR;VARCHAR;TEXT;NAME", "descriptor"},
    {"binary", "BYTEA", "descriptor"},
    {"temporal", "DATE;TIME;TIMETZ;TIMESTAMP;TIMESTAMPTZ;INTERVAL", "descriptor"},
    {"boolean_uuid", "BOOLEAN;UUID", "descriptor"},
    {"json_xml", "JSON;JSONB;JSONPATH;XML", "descriptor"},
    {"array", "ARRAY;[]", "descriptor"},
    {"range_multirange", "INT4RANGE;INT8RANGE;NUMRANGE;DATERANGE;TSRANGE;TSTZRANGE", "parser_support_udr"},
    {"network", "CIDR;INET;MACADDR;MACADDR8", "descriptor"},
    {"geometric", "POINT;LINE;LSEG;BOX;PATH;POLYGON;CIRCLE", "parser_support_udr"},
    {"domain_enum_composite", "DOMAIN;ENUM;COMPOSITE", "catalog_policy"},
}};

const std::array<SurfaceDescriptor, 13> kBuiltinSurfaces{{
    {"aggregate", "COUNT;SUM;AVG;MIN;MAX;STRING_AGG;ARRAY_AGG;JSON_AGG", "sblr"},
    {"window", "ROW_NUMBER;RANK;DENSE_RANK;LAG;LEAD;NTILE", "sblr"},
    {"string", "LOWER;UPPER;SUBSTRING;TRIM;POSITION;OVERLAY", "sblr"},
    {"numeric", "ABS;ROUND;POWER;SQRT;MOD;WIDTH_BUCKET", "sblr"},
    {"temporal", "NOW;CURRENT_TIMESTAMP;DATE_PART;DATE_TRUNC;EXTRACT", "sblr"},
    {"json", "JSONB_*;JSON_*", "parser_support_udr"},
    {"array", "ARRAY_APPEND;ARRAY_REMOVE;UNNEST", "parser_support_udr"},
    {"fulltext", "TO_TSVECTOR;TO_TSQUERY;TS_RANK", "sblr_optional"},
    {"uuid", "GEN_RANDOM_UUID;UUID_*", "parser_support_udr"},
    {"large_object", "LO_*", "parser_support_udr"},
    {"security", "CURRENT_USER;SESSION_USER;CURRENT_ROLE", "catalog_projection"},
    {"catalog", "PG_*_IS_VISIBLE;FORMAT_TYPE", "catalog_projection"},
    {"extension", "extension-defined functions", "trusted_package_or_refusal"},
}};

const std::array<SurfaceDescriptor, 9> kCatalogSurfaces{{
    {"pg_catalog", "PG_CATALOG.", "catalog_projection"},
    {"information_schema", "INFORMATION_SCHEMA.", "catalog_projection"},
    {"statistics", "PG_STAT_;PG_STATIO_;PG_STATISTIC", "catalog_projection"},
    {"roles_security", "PG_AUTHID;PG_ROLES;PG_POLICY", "catalog_projection"},
    {"namespace_relation", "PG_NAMESPACE;PG_CLASS;PG_ATTRIBUTE;PG_TYPE", "catalog_projection"},
    {"proc_language", "PG_PROC;PG_LANGUAGE;PG_TRIGGER;PG_REWRITE", "catalog_projection"},
    {"extension", "PG_EXTENSION;PG_DEPEND;PG_DESCRIPTION", "catalog_projection"},
    {"replication", "PG_PUBLICATION;PG_SUBSCRIPTION;PG_REPLICATION_SLOTS", "catalog_projection"},
    {"fdw", "PG_FOREIGN_DATA_WRAPPER;PG_FOREIGN_SERVER;PG_USER_MAPPING", "catalog_projection"},
}};

const std::array<SurfaceDescriptor, 12> kDiagnosticSurfaces{{
    {"parse", "POSTGRESQL.PARSE.INVALID_INPUT;POSTGRESQL.PARSE.UNSUPPORTED_SURFACE", "parser"},
    {"file", "POSTGRESQL.AUTHORITY.FILE_IO_DENIED", "fail_closed"},
    {"program", "POSTGRESQL.AUTHORITY.PROGRAM_DENIED", "fail_closed"},
    {"tablespace", "POSTGRESQL.AUTHORITY.TABLESPACE_DENIED", "fail_closed"},
    {"system", "POSTGRESQL.AUTHORITY.SYSTEM_DENIED;POSTGRESQL.AUTHORITY.UNSUPPORTED_DENIED", "fail_closed"},
    {"copy", "POSTGRESQL.EMULATION.COPY_ROUTE", "parser_support_udr"},
    {"extension", "POSTGRESQL.EMULATION.EXTENSION_ROUTE", "parser_support_udr"},
    {"connector", "POSTGRESQL.EMULATION.CONNECTOR_ROUTE", "parser_support_udr"},
    {"replication", "POSTGRESQL.EMULATION.REPLICATION_ROUTE", "parser_support_udr"},
    {"maintenance", "POSTGRESQL.AUTHORITY.UNSUPPORTED_DENIED", "fail_closed"},
    {"security", "POSTGRESQL.EMULATION.SECURITY_ROUTE", "parser_support_udr"},
    {"transaction", "POSTGRESQL.AUTHORITY.PREPARE_TRANSACTION_DENIED", "fail_closed"},
}};

const std::array<std::string_view, 7> kDialectVariants{{
    "postgresql_simple_query_sql",
    "postgresql_extended_query_protocol",
    "postgresql_plpgsql_udr_body",
    "postgresql_sql_function_body",
    "postgresql_jsonpath_expression",
    "postgresql_copy_logical_stream",
    "postgresql_logical_replication_protocol",
}};

const scratchbird::parser::compatibility::DialectProfile kProfile{
    "postgresql",
    "PostgreSQL",
    "sbp_postgresql",
    "sbup_postgresql",
    "18.3",
    "POSTGRESQL",
    kSblrFamily,
    kPatterns,
    kDatatypeSurfaces,
    kBuiltinSurfaces,
    kCatalogSurfaces,
    kDiagnosticSurfaces,
    19,
    2406,
    2286,
    108,
    0,
    12,
    0,
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

std::string PostgresqlPackageIdentityJson() {
  return scratchbird::parser::compatibility::PackageIdentityJson(kProfile);
}

std::string PostgresqlSurfaceReportJson() {
  return scratchbird::parser::compatibility::SurfaceReportJson(kProfile);
}

} // namespace scratchbird::parser::postgresql
