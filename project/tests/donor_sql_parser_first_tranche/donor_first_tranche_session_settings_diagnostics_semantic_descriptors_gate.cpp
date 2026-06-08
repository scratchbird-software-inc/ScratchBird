// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_dialect.hpp"
#include "mysql_dialect.hpp"
#include "postgresql_dialect.hpp"
#include "sbu_firebird_parser_support.hpp"
#include "sbu_mysql_parser_support.hpp"
#include "sbu_postgresql_parser_support.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

struct Expected {
  std::string_view dialect;
  std::string_view release_profile;
  std::string_view donor_profile_uuid;
  std::string_view session_semantic_profile_uuid;
  std::string_view profile;
  std::string_view statement_family;
  std::string_view operation_family;
  std::string_view operation_surface;
  std::string_view compatibility_mode_policy;
  std::string_view warning_policy;
  std::string_view notice_policy;
  std::string_view current_schema_policy;
  std::string_view search_path_policy;
  std::string_view date_time_format_policy;
  std::string_view timeout_policy;
  std::string_view reset_policy;
  std::string_view diagnostic_map_ref;
  std::string_view sandbox_root_policy;
  bool sql_mode_set{false};
  bool warning_surface{false};
  bool notice_surface{false};
  bool current_schema_surface{false};
  bool search_path_surface{false};
  bool date_time_format_surface{false};
  bool timeout_surface{false};
  bool reset_surface{false};
  bool diagnostic_projection_surface{false};
};

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool Expect(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

bool ExpectField(std::string_view json,
                 std::string_view field,
                 std::string_view value,
                 std::string_view label) {
  return Expect(Contains(json, "\"" + std::string(field) + "\":\"" +
                                  std::string(value) + "\""),
                std::string(label) + " missing field " + std::string(field) +
                    "=" + std::string(value));
}

bool ExpectBool(std::string_view json,
                std::string_view field,
                bool value,
                std::string_view label) {
  return Expect(Contains(json, "\"" + std::string(field) + "\":" +
                                  std::string(value ? "true" : "false")),
                std::string(label) + " missing bool " + std::string(field));
}

bool ExpectNoMysqlLts(std::string_view payload, std::string_view label) {
  bool ok = true;
  ok &= Expect(!Contains(payload, "mysql_lts"),
               std::string(label) + " emitted mysql_lts runtime evidence");
  ok &= Expect(!Contains(payload, "\"dialect\":\"mysql_lts\""),
               std::string(label) + " emitted mysql_lts dialect evidence");
  return ok;
}

bool ExpectAuthorityFalse(std::string_view payload, std::string_view label) {
  bool ok = true;
  ok &= ExpectBool(payload, "parser_session_authority", false, label);
  ok &= ExpectBool(payload, "parser_diagnostic_authority", false, label);
  ok &= ExpectBool(payload, "parser_catalog_authority", false, label);
  ok &= ExpectBool(payload, "parser_storage_authority", false, label);
  ok &= ExpectBool(payload, "parser_execution_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_finality_authority", false,
                   label);
  ok &= ExpectBool(payload, "parser_finality_authority", false, label);
  ok &= ExpectBool(payload, "parser_runtime_semantic_equivalence_authority",
                   false, label);
  ok &= Expect(!Contains(payload, "parser_session_authority\":true"),
               std::string(label) + " granted parser session authority");
  ok &= Expect(!Contains(payload, "parser_diagnostic_authority\":true"),
               std::string(label) + " granted parser diagnostic authority");
  ok &= Expect(!Contains(payload, "parser_catalog_authority\":true"),
               std::string(label) + " granted parser catalog authority");
  ok &= Expect(!Contains(payload, "parser_storage_authority\":true"),
               std::string(label) + " granted parser storage authority");
  ok &= Expect(!Contains(payload, "parser_transaction_authority\":true"),
               std::string(label) + " granted parser transaction authority");
  ok &= Expect(!Contains(payload,
                         "parser_transaction_finality_authority\":true"),
               std::string(label) +
                   " granted parser transaction finality authority");
  ok &= Expect(!Contains(payload, "parser_finality_authority\":true"),
               std::string(label) + " granted parser finality authority");
  ok &= Expect(!Contains(payload,
                         "parser_runtime_semantic_equivalence_authority\":true"),
               std::string(label) +
                   " granted parser runtime equivalence authority");
  return ok;
}

bool ExpectReadiness(std::string_view payload, std::string_view label) {
  bool ok = true;
  ok &= ExpectField(payload, "runtime_semantic_equivalence",
                    "not_enterprise_proven_pending", label);
  ok &= ExpectField(payload, "readiness_status", "proof_pending", label);
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready",
                    label);
  ok &= Expect(Contains(payload, "enterprise_implemented_proven\":false"),
               std::string(label) + " missing enterprise implementation proof");
  return ok;
}

bool ExpectSessionSettingsDiagnosticsEvidence(std::string_view payload,
                                              const Expected& expected,
                                              std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload,
                        "\"session_settings_diagnostics_semantic_evidence\":{"),
               std::string(label) +
                   " missing session settings diagnostics evidence");
  ok &= ExpectField(
      payload, "evidence_contract",
      "donor_session_settings_diagnostics_semantic_descriptor_evidence.v1",
      label);
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required", label);
  ok &= ExpectField(payload, "donor_profile_uuid",
                    expected.donor_profile_uuid, label);
  ok &= ExpectField(payload, "session_semantic_profile_uuid",
                    expected.session_semantic_profile_uuid, label);
  ok &= ExpectField(payload, "semantic_profile_uuid",
                    expected.session_semantic_profile_uuid, label);
  ok &= ExpectField(payload, "dialect", expected.dialect, label);
  ok &= ExpectField(payload, "release_profile", expected.release_profile,
                    label);
  ok &= ExpectField(payload, "session_settings_diagnostics_profile",
                    expected.profile, label);
  ok &= ExpectField(payload, "operation_surface", expected.operation_surface,
                    label);
  ok &= ExpectBool(payload, "sql_mode_set", expected.sql_mode_set, label);
  ok &= ExpectBool(payload, "warning_surface", expected.warning_surface, label);
  ok &= ExpectBool(payload, "notice_surface", expected.notice_surface, label);
  ok &= ExpectBool(payload, "current_schema_surface",
                   expected.current_schema_surface, label);
  ok &= ExpectBool(payload, "search_path_surface",
                   expected.search_path_surface, label);
  ok &= ExpectBool(payload, "date_time_format_surface",
                   expected.date_time_format_surface, label);
  ok &= ExpectBool(payload, "timeout_surface", expected.timeout_surface, label);
  ok &= ExpectBool(payload, "reset_surface", expected.reset_surface, label);
  ok &= ExpectBool(payload, "diagnostic_projection_surface",
                   expected.diagnostic_projection_surface, label);
  ok &= ExpectField(payload, "compatibility_mode_policy",
                    expected.compatibility_mode_policy, label);
  ok &= ExpectField(payload, "warning_policy", expected.warning_policy, label);
  ok &= ExpectField(payload, "notice_policy", expected.notice_policy, label);
  ok &= ExpectField(payload, "current_schema_policy",
                    expected.current_schema_policy, label);
  ok &= ExpectField(payload, "search_path_policy", expected.search_path_policy,
                    label);
  ok &= ExpectField(payload, "date_time_format_policy",
                    expected.date_time_format_policy, label);
  ok &= ExpectField(payload, "timeout_policy", expected.timeout_policy, label);
  ok &= ExpectField(payload, "reset_policy", expected.reset_policy, label);
  ok &= ExpectField(payload, "diagnostic_map_ref",
                    expected.diagnostic_map_ref, label);
  ok &= ExpectField(payload, "sandbox_root_policy",
                    expected.sandbox_root_policy, label);
  ok &= ExpectBool(payload, "uuid_required_semantic_profile", true, label);
  ok &= ExpectBool(payload, "session_descriptor_required", true, label);
  ok &= ExpectBool(payload, "diagnostic_descriptor_required", true, label);
  ok &= ExpectBool(payload, "catalog_descriptor_required", true, label);
  ok &= ExpectBool(payload, "sblr_operation_uuid_resolution_required", true,
                   label);
  ok &= ExpectField(payload, "engine_authority", "scratchbird", label);
  ok &= ExpectField(payload, "engine_session_authority",
                    "scratchbird_engine_session_descriptor_authority", label);
  ok &= ExpectField(payload, "diagnostic_rendering_authority",
                    "scratchbird_engine_diagnostic_rendering_authority",
                    label);
  ok &= ExpectField(payload, "catalog_authority",
                    "engine_catalog_uuid_projection", label);
  ok &= ExpectField(payload, "storage_authority", "engine_storage_authority",
                    label);
  ok &= ExpectField(payload, "transaction_authority", "engine_mga_authority",
                    label);
  ok &= ExpectField(payload, "finality_authority", "engine_mga_authority",
                    label);
  ok &= ExpectBool(payload, "source_sql_text_included", false, label);
  ok &= ExpectBool(payload, "literal_text_included", false, label);
  ok &= ExpectBool(payload, "object_name_text_included", false, label);
  ok &= ExpectBool(payload, "quoted_identifier_text_included", false, label);
  ok &= ExpectBool(payload, "sblr_embeds_source_identifiers", false, label);
  ok &= ExpectAuthorityFalse(payload, label);
  ok &= ExpectBool(payload, "donor_sql_executed", false, label);
  ok &= ExpectReadiness(payload, label);
  ok &= ExpectField(
      payload, "descriptor_exactness_status",
      "parser_session_settings_diagnostics_descriptor_recorded_runtime_equivalence_pending",
      label);
  ok &= ExpectNoMysqlLts(payload, label);
  return ok;
}

bool ExpectEnvelope(std::string_view payload,
                    const Expected& expected,
                    std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload, "\"envelope\":\"SBLRExecutionEnvelope.v3\""),
               std::string(label) + " missing SBLR envelope");
  ok &= ExpectField(payload, "dialect", expected.dialect, label);
  ok &= ExpectField(payload, "statement_family", expected.statement_family,
                    label);
  ok &= ExpectField(payload, "operation_family", expected.operation_family,
                    label);
  ok &= ExpectField(payload, "engine_authority", "scratchbird", label);
  ok &= ExpectBool(payload, "donor_engine_sql_executed", false, label);
  ok &= ExpectBool(payload, "sql_text_included", false, label);
  ok &= ExpectField(payload, "completion_claim", "not_enterprise_ready",
                    label);
  ok &= ExpectBool(payload, "enterprise_implemented_proven", false, label);
  ok &= ExpectSessionSettingsDiagnosticsEvidence(payload, expected, label);
  return ok;
}

template <typename Result>
bool ExpectDirectResult(const Result& result,
                        const Expected& expected,
                        std::string_view label) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " did not parse: " +
                             result.message_vector_json);
  ok &= Expect(result.statement_family == expected.statement_family,
               std::string(label) + " statement family mismatch: " +
                   result.statement_family);
  ok &= Expect(result.operation_family == expected.operation_family,
               std::string(label) + " operation family mismatch: " +
                   result.operation_family);
  ok &= ExpectSessionSettingsDiagnosticsEvidence(result.parser_evidence_json,
                                                expected, label);
  ok &= ExpectEnvelope(result.sblr_envelope, expected, label);
  return ok;
}

template <typename UdrResult>
bool ExpectUdrResult(const UdrResult& result,
                     const Expected& expected,
                     std::string_view label) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " UDR parse failed: " +
                             result.message_vector_json);
  ok &= ExpectEnvelope(result.payload, expected, label);
  return ok;
}

template <typename DirectParser, typename UdrParser>
bool RunCase(DirectParser direct_parser,
             UdrParser udr_parser,
             std::string_view sql,
             const Expected& expected,
             std::string_view label) {
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  bool ok = true;
  ok &= ExpectDirectResult(direct_parser(sql), expected,
                           std::string(label) + " direct");
  ok &= ExpectUdrResult(udr_parser(sql, trusted), expected,
                        std::string(label) + " UDR");
  return ok;
}

Expected FirebirdExpected(std::string_view operation_surface,
                          std::string_view compatibility_mode_policy,
                          bool diagnostic_projection_surface) {
  return {"firebird",
          "5.0.4",
          "019e13c0-0000-7000-8000-000000000302",
          "019e13c0-1e00-7000-8000-000000000302",
          "firebird.session_settings_diagnostics_semantics_profile",
          "isql_frontend",
          operation_surface == "firebird_show_sql_dialect"
              ? "firebird.isql.show"
              : "firebird.isql.set",
          operation_surface,
          compatibility_mode_policy,
          "firebird_status_vector_warning_diagnostics_engine_rendered",
          "firebird_status_vector_notice_mapping_engine_rendered",
          "firebird_current_schema_context_engine_session_descriptor",
          "firebird_no_search_path_single_attachment_schema_context",
          "firebird_date_time_format_stable_dialect_descriptor",
          "firebird_no_statement_timeout_session_setting_descriptor",
          "firebird_session_setting_reset_not_requested",
          "firebird_session_settings_diagnostics_semantics_diagnostic_map",
          "firebird_donor_schema_root_uuid_required_no_cross_root_temp_access",
          false,
          false,
          false,
          false,
          false,
          false,
          false,
          false,
          diagnostic_projection_surface};
}

Expected MysqlExpected(std::string_view statement_family,
                       std::string_view operation_family,
                       std::string_view operation_surface,
                       std::string_view compatibility_mode_policy,
                       std::string_view warning_policy,
                       std::string_view current_schema_policy,
                       bool sql_mode_set,
                       bool warning_surface,
                       bool current_schema_surface,
                       bool date_time_format_surface,
                       bool diagnostic_projection_surface) {
  return {"mysql",
          "9.7.0",
          "019e13c0-0000-7000-8000-000000000303",
          "019e13c0-1e00-7000-8000-000000000303",
          "mysql.session_settings_diagnostics_semantics_profile",
          statement_family,
          operation_family,
          operation_surface,
          compatibility_mode_policy,
          warning_policy,
          "mysql_notes_warnings_errors_diagnostic_area_engine_rendered",
          current_schema_policy,
          "mysql_no_search_path_current_database_descriptor_only",
          date_time_format_surface
              ? "mysql_sql_mode_date_time_format_descriptor_engine_applies"
              : "mysql_date_time_format_descriptor_engine_session_defaults",
          "mysql_timeout_settings_not_first_tranche_descriptor_only",
          "mysql_session_setting_reset_not_requested",
          "mysql_session_settings_diagnostics_semantics_diagnostic_map",
          "mysql_connected_database_root_uuid_required_temp_shadowing_root_local",
          sql_mode_set,
          warning_surface,
          false,
          current_schema_surface,
          false,
          date_time_format_surface,
          false,
          false,
          diagnostic_projection_surface};
}

Expected PostgresqlExpected(std::string_view statement_family,
                            std::string_view operation_family,
                            std::string_view operation_surface,
                            std::string_view compatibility_mode_policy,
                            std::string_view current_schema_policy,
                            std::string_view search_path_policy,
                            std::string_view timeout_policy,
                            std::string_view reset_policy,
                            bool notice_surface,
                            bool current_schema_surface,
                            bool search_path_surface,
                            bool timeout_surface,
                            bool reset_surface,
                            bool diagnostic_projection_surface) {
  return {"postgresql",
          "18.3",
          "019e13c0-0000-7000-8000-000000000304",
          "019e13c0-1e00-7000-8000-000000000304",
          "postgresql.session_settings_diagnostics_semantics_profile",
          statement_family,
          operation_family,
          operation_surface,
          compatibility_mode_policy,
          "postgresql_warning_diagnostics_engine_rendered",
          "postgresql_notice_warning_guc_diagnostics_engine_rendered",
          current_schema_policy,
          search_path_policy,
          "postgresql_datestyle_intervalstyle_descriptor_engine_applies",
          timeout_policy,
          reset_policy,
          "postgresql_session_settings_diagnostics_semantics_diagnostic_map",
          "postgresql_connected_database_schema_root_uuid_required_pg_temp_root_local",
          false,
          false,
          notice_surface,
          current_schema_surface,
          search_path_surface,
          true,
          timeout_surface,
          reset_surface,
          diagnostic_projection_surface};
}

bool FirebirdChecks() {
  using scratchbird::parser::firebird::ParseStatement;
  using scratchbird::udr::firebird_parser_support::sbu_firebird_parse_to_sblr;
  bool ok = true;
  ok &= RunCase(
      ParseStatement, sbu_firebird_parse_to_sblr, "set sql dialect 3",
      FirebirdExpected(
          "firebird_set_sql_dialect",
          "firebird_sql_dialect_session_descriptor_engine_applies", false),
      "firebird set sql dialect");
  ok &= RunCase(
      ParseStatement, sbu_firebird_parse_to_sblr, "set names utf8",
      FirebirdExpected(
          "firebird_set_names",
          "firebird_character_set_session_descriptor_engine_applies", false),
      "firebird set names");
  ok &= RunCase(
      ParseStatement, sbu_firebird_parse_to_sblr, "show sql dialect",
      FirebirdExpected(
          "firebird_show_sql_dialect",
          "firebird_isql_show_session_descriptor_projection", true),
      "firebird show sql dialect");
  return ok;
}

bool MysqlChecks() {
  using scratchbird::parser::mysql::ParseStatement;
  using scratchbird::udr::mysql_parser_support::sbu_mysql_parse_to_sblr;
  bool ok = true;
  ok &= RunCase(
      ParseStatement, sbu_mysql_parse_to_sblr,
      "set sql_mode = 'ANSI_QUOTES'",
      MysqlExpected(
          "session", "mysql.session.set", "mysql_set_sql_mode",
          "mysql_sql_mode_compatibility_descriptor_engine_applies",
          "mysql_warning_count_diagnostic_area_engine_rendered",
          "mysql_default_database_engine_session_descriptor", true, true,
          false, true, false),
      "mysql set sql_mode");
  ok &= RunCase(
      ParseStatement, sbu_mysql_parse_to_sblr, "show warnings",
      MysqlExpected(
          "catalog_overlay", "mysql.catalog_overlay.show",
          "mysql_show_warnings", "mysql_show_compatibility_projection_descriptor",
          "mysql_show_warnings_diagnostic_rows_engine_rendered",
          "mysql_default_database_engine_session_descriptor", false, true,
          false, false, true),
      "mysql show warnings");
  ok &= RunCase(
      ParseStatement, sbu_mysql_parse_to_sblr,
      "show variables like 'sql_mode'",
      MysqlExpected(
          "catalog_overlay", "mysql.catalog_overlay.show",
          "mysql_show_variables",
          "mysql_show_compatibility_projection_descriptor",
          "mysql_warning_count_diagnostic_area_engine_rendered",
          "mysql_default_database_engine_session_descriptor", false, false,
          false, false, true),
      "mysql show variables sql_mode");
  ok &= RunCase(
      ParseStatement, sbu_mysql_parse_to_sblr, "use inventory",
      MysqlExpected(
          "session", "mysql.session.use_database", "mysql_use_database",
          "mysql_default_schema_compatibility_descriptor_engine_applies",
          "mysql_warning_count_diagnostic_area_engine_rendered",
          "mysql_use_database_sets_current_schema_engine_session_descriptor",
          false, false, true, false, false),
      "mysql use database");
  return ok;
}

bool PostgresqlChecks() {
  using scratchbird::parser::postgresql::ParseStatement;
  using scratchbird::udr::postgresql_parser_support::
      sbu_postgresql_parse_to_sblr;
  bool ok = true;
  ok &= RunCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "set search_path to public, pg_catalog",
      PostgresqlExpected(
          "session", "postgresql.session.set", "postgresql_set_search_path",
          "postgresql_search_path_compatibility_descriptor_engine_applies",
          "postgresql_current_schema_resolved_from_engine_search_path_descriptor",
          "postgresql_search_path_list_descriptor_uuid_resolved_engine_applies",
          "postgresql_timeout_settings_unchanged_engine_session_descriptor",
          "postgresql_session_setting_reset_not_requested", true, true, true,
          false, false, false),
      "postgresql set search_path");
  ok &= RunCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "set statement_timeout = '5s'",
      PostgresqlExpected(
          "session", "postgresql.session.set",
          "postgresql_set_statement_timeout",
          "postgresql_guc_timeout_descriptor_engine_applies",
          "postgresql_current_schema_guc_projection_engine_session_descriptor",
          "postgresql_search_path_unchanged_engine_session_descriptor",
          "postgresql_statement_timeout_engine_session_descriptor",
          "postgresql_session_setting_reset_not_requested", true, false,
          false, true, false, false),
      "postgresql set statement_timeout");
  ok &= RunCase(
      ParseStatement, sbu_postgresql_parse_to_sblr, "reset search_path",
      PostgresqlExpected(
          "session", "postgresql.session.reset",
          "postgresql_reset_search_path",
          "postgresql_guc_reset_compatibility_descriptor_engine_applies",
          "postgresql_current_schema_resolved_from_engine_search_path_descriptor",
          "postgresql_reset_search_path_to_engine_default_descriptor",
          "postgresql_timeout_settings_unchanged_engine_session_descriptor",
          "postgresql_reset_guc_requests_engine_session_reset_descriptor",
          true, true, true, false, true, false),
      "postgresql reset search_path");
  ok &= RunCase(
      ParseStatement, sbu_postgresql_parse_to_sblr, "discard all",
      PostgresqlExpected(
          "session", "postgresql.session.discard", "postgresql_discard_all",
          "postgresql_guc_reset_compatibility_descriptor_engine_applies",
          "postgresql_current_schema_guc_projection_engine_session_descriptor",
          "postgresql_search_path_unchanged_engine_session_descriptor",
          "postgresql_timeout_settings_unchanged_engine_session_descriptor",
          "postgresql_discard_all_requests_engine_session_reset_descriptor",
          false, false, false, false, true, false),
      "postgresql discard all");
  ok &= RunCase(
      ParseStatement, sbu_postgresql_parse_to_sblr, "show search_path",
      PostgresqlExpected(
          "catalog_overlay", "postgresql.catalog_overlay.show",
          "postgresql_show_search_path",
          "postgresql_show_guc_projection_descriptor",
          "postgresql_current_schema_resolved_from_engine_search_path_descriptor",
          "postgresql_show_search_path_engine_projection",
          "postgresql_timeout_settings_unchanged_engine_session_descriptor",
          "postgresql_session_setting_reset_not_requested", true, true, true,
          false, false, true),
      "postgresql show search_path");
  return ok;
}

} // namespace

int main() {
  bool ok = true;
  ok &= FirebirdChecks();
  ok &= MysqlChecks();
  ok &= PostgresqlChecks();
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
