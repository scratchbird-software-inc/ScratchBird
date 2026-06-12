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
#include <span>
#include <string>
#include <string_view>

namespace {

struct TempExpected {
  std::string_view dialect;
  std::string_view release_profile;
  std::string_view compatibility_profile_uuid;
  std::string_view semantic_profile_uuid;
  std::string_view temporary_object_profile;
  std::string_view statement_family;
  std::string_view operation_family;
  std::string_view temporary_object_surface;
  std::string_view temporary_object_kind_policy;
  std::string_view on_commit_policy;
  std::string_view on_commit_delete_rows_policy;
  std::string_view on_commit_preserve_rows_policy;
  std::string_view on_commit_drop_policy;
  std::string_view name_shadowing_policy;
  std::string_view session_visibility_policy;
  std::string_view catalog_visibility_policy;
  std::string_view temporary_object_lifetime_policy;
  std::string_view schema_root_sandbox_policy;
  std::string_view sblr_operation;
  std::string_view engine_api_function;
  bool create_surface{false};
  bool alter_surface{false};
  bool drop_surface{false};
  bool global_keyword_surface{false};
  bool local_keyword_surface{false};
  bool temporary_keyword_surface{false};
  bool table_object_surface{false};
  bool on_commit_delete_rows_surface{false};
  bool on_commit_preserve_rows_surface{false};
  bool on_commit_drop_surface{false};
  bool name_shadowing_surface{false};
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

bool ExpectNoMarkers(std::string_view payload,
                     std::span<const std::string_view> markers,
                     std::string_view label) {
  bool ok = true;
  for (const auto marker : markers) {
    ok &= Expect(!Contains(payload, marker),
                 std::string(label) +
                     " leaked temporary object source marker: " +
                     std::string(marker));
  }
  return ok;
}

bool ExpectNoMysqlLts(std::string_view payload, std::string_view label) {
  bool ok = true;
  ok &= Expect(!Contains(payload, "mysql_lts"),
               std::string(label) + " emitted mysql_lts runtime evidence");
  ok &= Expect(!Contains(payload, "dialect:mysql_lts"),
               std::string(label) + " emitted dialect:mysql_lts");
  ok &= Expect(!Contains(payload, "\"dialect\":\"mysql_lts\""),
               std::string(label) + " emitted dialect mysql_lts");
  return ok;
}

bool ExpectReadinessComplete(std::string_view payload, std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload, "\"enterprise_readiness_evidence\":{"),
               std::string(label) + " missing readiness evidence");
  ok &= ExpectField(payload, "completion_claim", "not_enterprise_ready", label);
  ok &= ExpectBool(payload, "enterprise_implemented_proven", false, label);
  ok &= ExpectField(payload, "runtime_semantic_equivalence",
                    "not_enterprise_proven_pending", label);
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready",
                    label);
  ok &= Expect(Contains(payload, "enterprise_implemented_proven\":false"),
               std::string(label) + " missing enterprise implementation proof");
  return ok;
}

bool ExpectEnvelopeAuthority(std::string_view payload,
                             const TempExpected& expected,
                             std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload, "\"envelope\":\"SBLRExecutionEnvelope.v3\""),
               std::string(label) + " missing SBLR envelope");
  ok &= ExpectField(payload, "dialect", expected.dialect, label);
  ok &= ExpectField(payload, "operation_family", expected.operation_family,
                    label);
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required", label);
  ok &= ExpectField(payload, "engine_authority", "scratchbird", label);
  ok &= ExpectBool(payload, "reference_engine_sql_executed", false, label);
  ok &= ExpectBool(payload, "sql_text_included", false, label);
  ok &= ExpectReadinessComplete(payload, label);
  return ok;
}

bool ExpectTemporarySessionObjectEvidence(std::string_view payload,
                                          const TempExpected& expected,
                                          std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload,
                        "\"temporary_session_object_semantic_evidence\":{"),
               std::string(label) +
                   " missing temporary/session object evidence");
  ok &= ExpectField(
      payload, "evidence_contract",
      "compatibility_temporary_session_object_semantic_descriptor_evidence.v1", label);
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required", label);
  ok &= ExpectField(payload, "compatibility_profile_uuid",
                    expected.compatibility_profile_uuid, label);
  ok &= ExpectField(payload, "semantic_profile_uuid",
                    expected.semantic_profile_uuid, label);
  ok &= ExpectField(payload, "dialect", expected.dialect, label);
  ok &= ExpectField(payload, "release_profile", expected.release_profile,
                    label);
  ok &= ExpectField(payload, "temporary_object_profile",
                    expected.temporary_object_profile, label);
  ok &= ExpectField(payload, "temporary_object_surface",
                    expected.temporary_object_surface, label);
  ok &= ExpectField(payload, "temporary_object_kind_policy",
                    expected.temporary_object_kind_policy, label);
  ok &= ExpectField(payload, "global_local_temp_object_kind_policy",
                    expected.temporary_object_kind_policy, label);
  ok &= ExpectBool(payload, "create_surface", expected.create_surface, label);
  ok &= ExpectBool(payload, "alter_surface", expected.alter_surface, label);
  ok &= ExpectBool(payload, "drop_surface", expected.drop_surface, label);
  ok &= ExpectBool(payload, "global_keyword_surface",
                   expected.global_keyword_surface, label);
  ok &= ExpectBool(payload, "local_keyword_surface",
                   expected.local_keyword_surface, label);
  ok &= ExpectBool(payload, "temporary_keyword_surface",
                   expected.temporary_keyword_surface, label);
  ok &= ExpectBool(payload, "table_object_surface",
                   expected.table_object_surface, label);
  ok &= ExpectBool(payload, "on_commit_delete_rows_surface",
                   expected.on_commit_delete_rows_surface, label);
  ok &= ExpectBool(payload, "on_commit_preserve_rows_surface",
                   expected.on_commit_preserve_rows_surface, label);
  ok &= ExpectBool(payload, "on_commit_drop_surface",
                   expected.on_commit_drop_surface, label);
  ok &= ExpectField(payload, "on_commit_policy", expected.on_commit_policy,
                    label);
  ok &= ExpectField(payload, "on_commit_delete_rows_policy",
                    expected.on_commit_delete_rows_policy, label);
  ok &= ExpectField(payload, "on_commit_preserve_rows_policy",
                    expected.on_commit_preserve_rows_policy, label);
  ok &= ExpectField(payload, "on_commit_drop_policy",
                    expected.on_commit_drop_policy, label);
  ok &= ExpectBool(payload, "name_shadowing_surface",
                   expected.name_shadowing_surface, label);
  ok &= ExpectField(payload, "name_shadowing_policy",
                    expected.name_shadowing_policy, label);
  ok &= ExpectField(payload, "session_visibility_policy",
                    expected.session_visibility_policy, label);
  ok &= ExpectField(payload, "catalog_visibility_policy",
                    expected.catalog_visibility_policy, label);
  ok &= ExpectField(payload, "transaction_interaction_policy",
                    "engine_mga_authority", label);
  ok &= ExpectField(payload, "session_interaction_policy",
                    "engine_session_authority", label);
  ok &= ExpectField(payload, "cleanup_lifetime_policy",
                    "engine_session_catalog_authority", label);
  ok &= ExpectField(payload, "temporary_object_lifetime_policy",
                    expected.temporary_object_lifetime_policy, label);
  ok &= ExpectField(payload, "schema_root_sandbox_policy",
                    expected.schema_root_sandbox_policy, label);
  ok &= ExpectBool(payload, "uuid_required_semantic_profile", true, label);
  ok &= ExpectBool(payload, "catalog_descriptor_required", true, label);
  ok &= ExpectBool(payload, "session_descriptor_required", true, label);
  ok &= ExpectBool(payload, "sblr_operation_uuid_resolution_required", true,
                   label);
  ok &= ExpectField(payload, "engine_authority", "scratchbird", label);
  ok &= ExpectBool(payload, "source_sql_text_included", false, label);
  ok &= ExpectBool(payload, "literal_text_included", false, label);
  ok &= ExpectBool(payload, "object_name_text_included", false, label);
  ok &= ExpectBool(payload, "quoted_identifier_text_included", false, label);
  ok &= ExpectBool(payload, "sblr_embeds_source_identifiers", false, label);
  ok &= ExpectBool(payload, "parser_catalog_authority", false, label);
  ok &= ExpectBool(payload, "parser_storage_authority", false, label);
  ok &= ExpectBool(payload, "parser_session_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_finality_authority", false,
                   label);
  ok &= ExpectBool(payload, "parser_visibility_authority", false, label);
  ok &= ExpectBool(payload, "parser_cleanup_authority", false, label);
  ok &= ExpectBool(payload, "compatibility_sql_executed", false, label);
  ok &= ExpectField(payload, "runtime_semantic_equivalence",
                    "not_enterprise_proven_pending", label);
  ok &= ExpectField(
      payload, "descriptor_exactness_status",
      "parser_temporary_session_object_descriptor_recorded_runtime_equivalence_pending",
      label);
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready",
                    label);
  ok &= Expect(!Contains(payload, "parser_catalog_authority\":true"),
               std::string(label) + " granted parser catalog authority");
  ok &= Expect(!Contains(payload, "parser_storage_authority\":true"),
               std::string(label) + " granted parser storage authority");
  ok &= Expect(!Contains(payload, "parser_session_authority\":true"),
               std::string(label) + " granted parser session authority");
  ok &= Expect(!Contains(payload, "parser_transaction_authority\":true"),
               std::string(label) + " granted parser transaction authority");
  ok &= Expect(!Contains(payload, "compatibility_sql_executed\":true"),
               std::string(label) + " executed compatibility SQL");
  return ok;
}

template <typename Result>
bool ExpectDirectTemporaryResult(
    const Result& result,
    const TempExpected& expected,
    std::span<const std::string_view> markers,
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
  if (!expected.sblr_operation.empty()) {
    ok &= Expect(result.sblr_operation == expected.sblr_operation,
                 std::string(label) + " SBLR operation mismatch: " +
                     result.sblr_operation);
    ok &= Expect(result.engine_api_function == expected.engine_api_function,
                 std::string(label) + " engine API mismatch: " +
                     result.engine_api_function);
  }
  ok &= ExpectField(result.parser_evidence_json, "dialect", expected.dialect,
                    label);
  ok &= ExpectBool(result.parser_evidence_json, "source_text_redacted", true,
                   label);
  ok &= ExpectBool(result.parser_evidence_json, "descriptor_uuid_required",
                   true, label);
  ok &= ExpectBool(result.parser_evidence_json,
                   "parser_transaction_finality_authority", false, label);
  ok &= ExpectBool(result.parser_evidence_json, "parser_storage_authority",
                   false, label);
  ok &= ExpectTemporarySessionObjectEvidence(result.parser_evidence_json,
                                             expected, label);
  ok &= ExpectEnvelopeAuthority(result.sblr_envelope, expected, label);
  ok &= ExpectTemporarySessionObjectEvidence(result.sblr_envelope, expected,
                                             label);
  ok &= ExpectNoMarkers(result.parser_evidence_json, markers, label);
  ok &= ExpectNoMarkers(result.sblr_envelope, markers, label);
  ok &= ExpectNoMysqlLts(result.parser_evidence_json, label);
  ok &= ExpectNoMysqlLts(result.sblr_envelope, label);
  return ok;
}

template <typename UdrResult>
bool ExpectUdrTemporaryResult(const UdrResult& result,
                              const TempExpected& expected,
                              std::span<const std::string_view> markers,
                              std::string_view label) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " UDR parse failed: " +
                             result.message_vector_json);
  ok &= ExpectEnvelopeAuthority(result.payload, expected, label);
  ok &= ExpectTemporarySessionObjectEvidence(result.payload, expected, label);
  if (!expected.sblr_operation.empty()) {
    ok &= ExpectField(result.payload, "sblr_operation",
                      expected.sblr_operation, label);
    ok &= ExpectField(result.payload, "engine_api_function",
                      expected.engine_api_function, label);
  }
  ok &= ExpectNoMarkers(result.payload, markers, label);
  ok &= ExpectNoMysqlLts(result.payload, label);
  return ok;
}

template <typename DirectParser, typename UdrParser>
bool RunTemporaryCase(DirectParser direct_parser,
                      UdrParser udr_parser,
                      std::string_view sql,
                      const TempExpected& expected,
                      std::span<const std::string_view> markers,
                      std::string_view label) {
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  bool ok = true;
  ok &= ExpectDirectTemporaryResult(direct_parser(sql), expected, markers,
                                    std::string(label) + " direct");
  ok &= ExpectUdrTemporaryResult(udr_parser(sql, trusted), expected, markers,
                                 std::string(label) + " UDR");
  return ok;
}

std::span<const std::string_view> FirebirdMarkers() {
  static constexpr std::string_view markers[] = {
      "fb_temp_secret_marker", "FB_TEMP_SECRET_MARKER"};
  return markers;
}

std::span<const std::string_view> MysqlMarkers() {
  static constexpr std::string_view markers[] = {
      "mysql_temp_secret_marker", "MYSQL_TEMP_SECRET_MARKER"};
  return markers;
}

std::span<const std::string_view> PostgresqlMarkers() {
  static constexpr std::string_view markers[] = {
      "pg_temp_secret_marker", "PG_TEMP_SECRET_MARKER",
      "pg_temp_drop_secret_marker", "PG_TEMP_DROP_SECRET_MARKER"};
  return markers;
}

TempExpected FirebirdBase(std::string_view surface,
                          std::string_view on_commit_policy,
                          std::string_view lifetime_policy,
                          bool delete_rows,
                          bool preserve_rows) {
  return {"firebird",
          "5.0.4",
          "019e13c0-0000-7000-8000-000000000302",
          "019e13c0-1700-7000-8000-000000000302",
          "firebird.global_temporary_table_semantics_profile",
          "ddl",
          "firebird.ddl.create.global_temporary_table",
          surface,
          "firebird_global_temporary_table_metadata_persistent_rows_session_or_transaction_scoped",
          on_commit_policy,
          "firebird_delete_rows_supported_engine_mga_transaction_boundary",
          "firebird_preserve_rows_supported_engine_session_lifetime",
          "firebird_on_commit_drop_not_supported",
          "firebird_no_session_name_shadowing_regular_schema_namespace",
          "firebird_gtt_data_is_attachment_private_metadata_global_catalog_visible",
          "firebird_persistent_catalog_descriptor_marks_global_temporary_table",
          lifetime_policy,
          "firebird_compatibility_schema_root_uuid_required_no_cross_root_temp_access",
          "",
          "",
          true,
          false,
          false,
          true,
          false,
          true,
          true,
          delete_rows,
          preserve_rows,
          false,
          false};
}

bool FirebirdChecks() {
  using scratchbird::parser::firebird::ParseStatement;
  using scratchbird::udr::firebird_parser_support::sbu_firebird_parse_to_sblr;
  bool ok = true;
  ok &= RunTemporaryCase(
      ParseStatement, sbu_firebird_parse_to_sblr,
      "create global temporary table fb_temp_secret_marker (id integer) "
      "on commit delete rows",
      FirebirdBase(
          "firebird_create_global_temporary_table_on_commit_delete_rows",
          "firebird_on_commit_delete_rows_engine_transaction_end_cleanup",
          "firebird_rows_cleared_at_engine_mga_transaction_end_metadata_survives",
          true, false),
      FirebirdMarkers(), "firebird gtt on commit delete rows");
  ok &= RunTemporaryCase(
      ParseStatement, sbu_firebird_parse_to_sblr,
      "create global temporary table fb_temp_secret_marker (id integer) "
      "on commit preserve rows",
      FirebirdBase(
          "firebird_create_global_temporary_table_on_commit_preserve_rows",
          "firebird_on_commit_preserve_rows_engine_session_lifetime",
          "firebird_rows_survive_until_engine_attachment_end_metadata_survives",
          false, true),
      FirebirdMarkers(), "firebird gtt on commit preserve rows");
  return ok;
}

bool MysqlChecks() {
  using scratchbird::parser::mysql::ParseStatement;
  using scratchbird::udr::mysql_parser_support::sbu_mysql_parse_to_sblr;
  bool ok = true;
  const TempExpected create_expected{
      "mysql",
      "9.7.0",
      "019e13c0-0000-7000-8000-000000000303",
      "019e13c0-1700-7000-8000-000000000303",
      "mysql.session_temporary_table_semantics_profile",
      "ddl",
      "mysql.ddl.create.temporary_table",
      "mysql_create_temporary_table_name_shadowing",
      "mysql_session_temporary_table_private_name_shadowing_regular_table",
      "mysql_no_on_commit_clause_table_lifetime_session_end",
      "mysql_delete_rows_not_supported",
      "mysql_preserve_rows_is_session_lifetime_default",
      "mysql_on_commit_drop_not_supported",
      "mysql_temporary_table_name_shadows_base_table_within_session",
      "mysql_temporary_table_session_private_name_shadowing_visible_to_connection",
      "mysql_temporary_table_not_persistent_information_schema_object",
      "mysql_temp_table_dropped_at_engine_session_end_or_explicit_drop",
      "mysql_connected_database_root_uuid_required_temp_shadowing_root_local",
      "SBLR_COMPATIBILITY_MYSQL_TEMPORARY_TABLE_CREATE",
      "EngineDdlCreateTemporaryTable",
      true,
      false,
      false,
      false,
      false,
      true,
      true,
      false,
      false,
      false,
      true};
  ok &= RunTemporaryCase(
      ParseStatement, sbu_mysql_parse_to_sblr,
      "create temporary table mysql_temp_secret_marker (id int)",
      create_expected, MysqlMarkers(), "mysql create temporary table");

  TempExpected drop_expected = create_expected;
  drop_expected.operation_family = "mysql.ddl.drop.temporary_table";
  drop_expected.temporary_object_surface =
      "mysql_drop_temporary_table_session_object";
  drop_expected.sblr_operation = "SBLR_COMPATIBILITY_MYSQL_TEMPORARY_TABLE_DROP";
  drop_expected.engine_api_function = "EngineDdlDropTemporaryTable";
  drop_expected.create_surface = false;
  drop_expected.drop_surface = true;
  ok &= RunTemporaryCase(
      ParseStatement, sbu_mysql_parse_to_sblr,
      "drop temporary table mysql_temp_secret_marker",
      drop_expected, MysqlMarkers(), "mysql drop temporary table");
  return ok;
}

bool PostgresqlChecks() {
  using scratchbird::parser::postgresql::ParseStatement;
  using scratchbird::udr::postgresql_parser_support::
      sbu_postgresql_parse_to_sblr;
  bool ok = true;
  const TempExpected create_drop_expected{
      "postgresql",
      "18.3",
      "019e13c0-0000-7000-8000-000000000304",
      "019e13c0-1700-7000-8000-000000000304",
      "postgresql.temporary_table_semantics_profile",
      "ddl",
      "postgresql.ddl.create.temporary_table",
      "postgresql_create_temp_table_on_commit_drop",
      "postgresql_temp_schema_session_private_table_object",
      "postgresql_on_commit_drop_engine_transaction_end_catalog_cleanup",
      "postgresql_delete_rows_supported_engine_mga_transaction_boundary",
      "postgresql_preserve_rows_supported_engine_session_lifetime",
      "postgresql_on_commit_drop_supported_engine_session_catalog_cleanup",
      "postgresql_pg_temp_search_path_shadows_permanent_objects",
      "postgresql_pg_temp_schema_session_private_search_path_visible",
      "postgresql_catalog_pg_class_pg_namespace_temp_schema_descriptor",
      "postgresql_temp_table_dropped_at_engine_mga_transaction_end",
      "postgresql_connected_database_schema_root_uuid_required_pg_temp_root_local",
      "SBLR_COMPATIBILITY_POSTGRESQL_TEMPORARY_TABLE_CREATE",
      "EngineDdlCreateTemporaryTable",
      true,
      false,
      false,
      false,
      false,
      true,
      true,
      false,
      false,
      true,
      true};
  ok &= RunTemporaryCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "create temp table pg_temp_secret_marker (id integer) on commit drop",
      create_drop_expected, PostgresqlMarkers(),
      "postgresql create temp table on commit drop");

  TempExpected local_expected = create_drop_expected;
  local_expected.operation_family =
      "postgresql.ddl.create.local_temporary_table";
  local_expected.temporary_object_surface =
      "postgresql_create_temp_table_on_commit_preserve_rows";
  local_expected.temporary_object_kind_policy =
      "postgresql_local_temp_schema_session_private";
  local_expected.global_keyword_surface = false;
  local_expected.local_keyword_surface = true;
  local_expected.on_commit_policy =
      "postgresql_on_commit_preserve_rows_engine_session_lifetime";
  local_expected.temporary_object_lifetime_policy =
      "postgresql_temp_table_lives_until_engine_session_end_or_explicit_drop";
  local_expected.on_commit_drop_surface = false;
  local_expected.on_commit_preserve_rows_surface = true;
  ok &= RunTemporaryCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "create local temporary table pg_temp_secret_marker (id integer) "
      "on commit preserve rows",
      local_expected, PostgresqlMarkers(),
      "postgresql create local temporary table preserve rows");

  TempExpected drop_expected = create_drop_expected;
  drop_expected.operation_family = "postgresql.ddl.drop.temporary_table";
  drop_expected.temporary_object_surface =
      "postgresql_drop_table_pg_temp_resolution";
  drop_expected.temporary_object_kind_policy =
      "postgresql_temp_schema_session_private_table_object";
  drop_expected.on_commit_policy =
      "postgresql_default_on_commit_preserve_rows_descriptor";
  drop_expected.temporary_object_lifetime_policy =
      "postgresql_temp_table_lives_until_engine_session_end_or_explicit_drop";
  drop_expected.sblr_operation = "SBLR_COMPATIBILITY_POSTGRESQL_TEMPORARY_TABLE_DROP";
  drop_expected.engine_api_function = "EngineDdlDropTemporaryTable";
  drop_expected.create_surface = false;
  drop_expected.drop_surface = true;
  drop_expected.on_commit_drop_surface = false;
  drop_expected.temporary_keyword_surface = true;
  ok &= RunTemporaryCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "drop table pg_temp.pg_temp_drop_secret_marker",
      drop_expected, PostgresqlMarkers(),
      "postgresql drop pg_temp table");
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
