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

struct ResourceTextExpected {
  std::string_view dialect;
  std::string_view release_profile;
  std::string_view compatibility_profile_uuid;
  std::string_view semantic_profile_uuid;
  std::string_view resource_text_profile;
  std::string_view resource_text_surface;
  std::string_view charset_policy;
  std::string_view collation_policy;
  std::string_view timezone_policy;
  std::string_view calendar_policy;
  std::string_view comparison_policy;
  std::string_view pattern_matching_policy;
  std::string_view binary_text_policy;
  std::string_view resource_epoch_policy;
  std::string_view index_compatibility_policy;
  std::string_view diagnostic_map_ref;
  std::string_view sandbox_root_policy;
  std::string_view statement_family;
  std::string_view operation_family;
  bool charset_surface{false};
  bool collation_surface{false};
  bool timezone_surface{false};
  bool calendar_surface{false};
  bool comparison_surface{false};
  bool pattern_surface{false};
  bool binary_text_surface{false};
  bool text_type_surface{false};
  bool ddl_surface{false};
  bool dml_surface{false};
  bool query_surface{false};
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
                     " leaked resource/text source marker: " +
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
                             const ResourceTextExpected& expected,
                             std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload, "\"envelope\":\"SBLRExecutionEnvelope.v3\""),
               std::string(label) + " missing SBLR envelope");
  ok &= ExpectField(payload, "dialect", expected.dialect, label);
  ok &= ExpectField(payload, "statement_family", expected.statement_family,
                    label);
  ok &= ExpectField(payload, "operation_family", expected.operation_family,
                    label);
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required", label);
  ok &= ExpectField(payload, "engine_authority", "scratchbird", label);
  ok &= ExpectBool(payload, "reference_engine_sql_executed", false, label);
  ok &= ExpectBool(payload, "sql_text_included", false, label);
  ok &= ExpectReadinessComplete(payload, label);
  return ok;
}

bool ExpectResourceTextEvidence(std::string_view payload,
                                const ResourceTextExpected& expected,
                                std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload, "\"resource_text_semantic_evidence\":{"),
               std::string(label) +
                   " missing resource/text semantic evidence");
  ok &= ExpectField(
      payload, "evidence_contract",
      "compatibility_resource_text_semantic_descriptor_evidence.v1", label);
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required", label);
  ok &= ExpectField(payload, "compatibility_profile_uuid",
                    expected.compatibility_profile_uuid, label);
  ok &= ExpectField(payload, "semantic_profile_uuid",
                    expected.semantic_profile_uuid, label);
  ok &= ExpectField(payload, "dialect", expected.dialect, label);
  ok &= ExpectField(payload, "release_profile", expected.release_profile,
                    label);
  ok &= ExpectField(payload, "resource_text_profile",
                    expected.resource_text_profile, label);
  ok &= ExpectField(payload, "resource_text_surface",
                    expected.resource_text_surface, label);
  ok &= ExpectField(payload, "charset_policy", expected.charset_policy, label);
  ok &= ExpectField(payload, "collation_policy", expected.collation_policy,
                    label);
  ok &= ExpectField(payload, "timezone_policy", expected.timezone_policy,
                    label);
  ok &= ExpectField(payload, "calendar_policy", expected.calendar_policy,
                    label);
  ok &= ExpectField(payload, "comparison_policy", expected.comparison_policy,
                    label);
  ok &= ExpectField(payload, "pattern_matching_policy",
                    expected.pattern_matching_policy, label);
  ok &= ExpectField(payload, "binary_text_policy",
                    expected.binary_text_policy, label);
  ok &= ExpectField(payload, "resource_epoch_policy",
                    expected.resource_epoch_policy, label);
  ok &= ExpectField(payload, "index_compatibility_policy",
                    expected.index_compatibility_policy, label);
  ok &= ExpectField(payload, "diagnostic_map_ref",
                    expected.diagnostic_map_ref, label);
  ok &= ExpectField(payload, "sandbox_root_policy",
                    expected.sandbox_root_policy, label);
  ok &= ExpectBool(payload, "charset_surface", expected.charset_surface, label);
  ok &= ExpectBool(payload, "collation_surface", expected.collation_surface,
                   label);
  ok &= ExpectBool(payload, "timezone_surface", expected.timezone_surface,
                   label);
  ok &= ExpectBool(payload, "calendar_surface", expected.calendar_surface,
                   label);
  ok &= ExpectBool(payload, "comparison_surface", expected.comparison_surface,
                   label);
  ok &= ExpectBool(payload, "pattern_surface", expected.pattern_surface,
                   label);
  ok &= ExpectBool(payload, "binary_text_surface",
                   expected.binary_text_surface, label);
  ok &= ExpectBool(payload, "text_type_surface", expected.text_type_surface,
                   label);
  ok &= ExpectBool(payload, "ddl_surface", expected.ddl_surface, label);
  ok &= ExpectBool(payload, "dml_surface", expected.dml_surface, label);
  ok &= ExpectBool(payload, "query_surface", expected.query_surface, label);
  ok &= ExpectBool(payload, "uuid_required_semantic_profile", true, label);
  ok &= ExpectBool(payload, "catalog_descriptor_required", true, label);
  ok &= ExpectBool(payload, "resource_descriptor_required", true, label);
  ok &= ExpectBool(payload, "text_type_descriptor_required", true, label);
  ok &= ExpectBool(payload, "sblr_operation_uuid_resolution_required", true,
                   label);
  ok &= ExpectField(payload, "engine_authority", "scratchbird", label);
  ok &= ExpectField(payload, "resource_authority",
                    "engine_resource_descriptor_authority", label);
  ok &= ExpectField(payload, "charset_authority",
                    "engine_catalog_resource_descriptor", label);
  ok &= ExpectField(payload, "collation_authority",
                    "engine_catalog_resource_descriptor", label);
  ok &= ExpectField(payload, "timezone_authority",
                    "engine_session_resource_descriptor", label);
  ok &= ExpectField(payload, "calendar_authority",
                    "engine_temporal_resource_descriptor", label);
  ok &= ExpectField(payload, "comparison_authority",
                    "engine_expression_resource_descriptor", label);
  ok &= ExpectField(payload, "pattern_matching_authority",
                    "engine_expression_resource_descriptor", label);
  ok &= ExpectField(payload, "binary_text_authority",
                    "engine_datatype_resource_descriptor", label);
  ok &= ExpectField(payload, "index_compatibility_authority",
                    "engine_index_resource_descriptor", label);
  ok &= ExpectBool(payload, "source_sql_text_included", false, label);
  ok &= ExpectBool(payload, "literal_text_included", false, label);
  ok &= ExpectBool(payload, "object_name_text_included", false, label);
  ok &= ExpectBool(payload, "quoted_identifier_text_included", false, label);
  ok &= ExpectBool(payload, "sblr_embeds_source_identifiers", false, label);
  ok &= ExpectBool(payload, "parser_charset_authority", false, label);
  ok &= ExpectBool(payload, "parser_collation_authority", false, label);
  ok &= ExpectBool(payload, "parser_timezone_authority", false, label);
  ok &= ExpectBool(payload, "parser_calendar_authority", false, label);
  ok &= ExpectBool(payload, "parser_comparison_authority", false, label);
  ok &= ExpectBool(payload, "parser_pattern_matching_authority", false, label);
  ok &= ExpectBool(payload, "parser_binary_text_authority", false, label);
  ok &= ExpectBool(payload, "parser_text_type_authority", false, label);
  ok &= ExpectBool(payload, "parser_catalog_authority", false, label);
  ok &= ExpectBool(payload, "parser_resource_activation_authority", false,
                   label);
  ok &= ExpectBool(payload, "parser_index_compatibility_authority", false,
                   label);
  ok &= ExpectBool(payload, "parser_storage_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_finality_authority", false,
                   label);
  ok &= ExpectBool(payload, "parser_runtime_semantic_equivalence_authority",
                   false, label);
  ok &= ExpectBool(payload, "compatibility_sql_executed", false, label);
  ok &= ExpectField(payload, "runtime_semantic_equivalence",
                    "not_enterprise_proven_pending", label);
  ok &= ExpectField(
      payload, "descriptor_exactness_status",
      "parser_resource_text_semantic_descriptor_recorded_runtime_equivalence_pending",
      label);
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready",
                    label);
  ok &= Expect(!Contains(payload, "parser_charset_authority\":true"),
               std::string(label) + " granted parser charset authority");
  ok &= Expect(!Contains(payload, "parser_collation_authority\":true"),
               std::string(label) + " granted parser collation authority");
  ok &= Expect(!Contains(payload, "parser_resource_activation_authority\":true"),
               std::string(label) +
                   " granted parser resource activation authority");
  ok &= Expect(!Contains(payload, "parser_index_compatibility_authority\":true"),
               std::string(label) +
                   " granted parser index compatibility authority");
  ok &= Expect(!Contains(payload, "compatibility_sql_executed\":true"),
               std::string(label) + " executed compatibility SQL");
  return ok;
}

template <typename Result>
bool ExpectDirectResult(const Result& result,
                        const ResourceTextExpected& expected,
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
  ok &= ExpectResourceTextEvidence(result.parser_evidence_json, expected,
                                   label);
  ok &= ExpectEnvelopeAuthority(result.sblr_envelope, expected, label);
  ok &= ExpectResourceTextEvidence(result.sblr_envelope, expected, label);
  ok &= ExpectNoMarkers(result.parser_evidence_json, markers, label);
  ok &= ExpectNoMarkers(result.sblr_envelope, markers, label);
  ok &= ExpectNoMysqlLts(result.parser_evidence_json, label);
  ok &= ExpectNoMysqlLts(result.sblr_envelope, label);
  return ok;
}

template <typename UdrResult>
bool ExpectUdrResult(const UdrResult& result,
                     const ResourceTextExpected& expected,
                     std::span<const std::string_view> markers,
                     std::string_view label) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " UDR parse failed: " +
                             result.message_vector_json);
  ok &= ExpectEnvelopeAuthority(result.payload, expected, label);
  ok &= ExpectResourceTextEvidence(result.payload, expected, label);
  ok &= ExpectNoMarkers(result.payload, markers, label);
  ok &= ExpectNoMysqlLts(result.payload, label);
  return ok;
}

template <typename DirectParser, typename UdrParser>
bool RunCase(DirectParser direct_parser,
             UdrParser udr_parser,
             std::string_view sql,
             const ResourceTextExpected& expected,
             std::span<const std::string_view> markers,
             std::string_view label) {
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  bool ok = true;
  ok &= ExpectDirectResult(direct_parser(sql), expected, markers,
                           std::string(label) + " direct");
  ok &= ExpectUdrResult(udr_parser(sql, trusted), expected, markers,
                        std::string(label) + " UDR");
  return ok;
}

std::span<const std::string_view> FirebirdDdlMarkers() {
  static constexpr std::string_view markers[] = {
      "fb_rt_marker", "FB_RT_MARKER", "unicode_ci", "UNICODE_CI"};
  return markers;
}

std::span<const std::string_view> FirebirdQueryMarkers() {
  static constexpr std::string_view markers[] = {
      "fb_rt_literal_marker", "FB_RT_LITERAL_MARKER"};
  return markers;
}

std::span<const std::string_view> MysqlDdlMarkers() {
  static constexpr std::string_view markers[] = {
      "mysql_rt_marker", "MYSQL_RT_MARKER", "utf8mb4_0900_ai_ci",
      "UTF8MB4_0900_AI_CI"};
  return markers;
}

std::span<const std::string_view> MysqlQueryMarkers() {
  static constexpr std::string_view markers[] = {
      "mysql_rt_literal_marker", "MYSQL_RT_LITERAL_MARKER"};
  return markers;
}

std::span<const std::string_view> PostgresqlDdlMarkers() {
  static constexpr std::string_view markers[] = {
      "pg_rt_marker", "PG_RT_MARKER"};
  return markers;
}

std::span<const std::string_view> PostgresqlQueryMarkers() {
  static constexpr std::string_view markers[] = {
      "pg_rt_literal_marker", "PG_RT_LITERAL_MARKER"};
  return markers;
}

bool FirebirdChecks() {
  using scratchbird::parser::firebird::ParseStatement;
  using scratchbird::udr::firebird_parser_support::sbu_firebird_parse_to_sblr;
  bool ok = true;
  ok &= RunCase(
      ParseStatement, sbu_firebird_parse_to_sblr,
      "create table fb_rt_marker (name varchar(40) character set utf8 collate unicode_ci)",
      {"firebird",
       "5.0.4",
       "019e13c0-0000-7000-8000-000000000302",
       "019e13c0-1a00-7000-8000-000000000302",
       "firebird.resource_text_semantics_profile",
       "firebird_ddl_charset_collation_text_blob",
       "firebird_character_set_descriptor_uuid_required_engine_applies",
       "firebird_column_charset_collation_descriptor",
       "firebird_session_timezone_descriptor_engine_authority",
       "firebird_temporal_calendar_descriptor_engine_authority",
       "firebird_text_comparison_charset_collation_descriptor_engine_authority",
       "firebird_like_similar_to_containing_starting_with_descriptor",
       "firebird_blob_sub_type_binary_text_descriptor_required",
       "firebird_resource_text_descriptor_epoch_engine_mga_catalog_bound",
       "firebird_text_index_charset_collation_compatibility_engine_validated",
       "firebird_resource_text_semantics_diagnostic_map",
       "firebird_compatibility_schema_root_uuid_required_no_cross_root_temp_access",
       "ddl",
       "firebird.ddl.create",
       true, true, false, false, true, false, false, true, true, false,
       false},
      FirebirdDdlMarkers(), "firebird resource/text DDL");
  ok &= RunCase(
      ParseStatement, sbu_firebird_parse_to_sblr,
      "select * from r where name containing 'fb_rt_literal_marker'",
      {"firebird",
       "5.0.4",
       "019e13c0-0000-7000-8000-000000000302",
       "019e13c0-1a00-7000-8000-000000000302",
       "firebird.resource_text_semantics_profile",
       "firebird_query_like_similar_containing",
       "firebird_character_set_descriptor_uuid_required_engine_applies",
       "firebird_column_charset_collation_descriptor",
       "firebird_session_timezone_descriptor_engine_authority",
       "firebird_temporal_calendar_descriptor_engine_authority",
       "firebird_text_comparison_charset_collation_descriptor_engine_authority",
       "firebird_like_similar_to_containing_starting_with_descriptor",
       "firebird_blob_sub_type_binary_text_descriptor_required",
       "firebird_resource_text_descriptor_epoch_engine_mga_catalog_bound",
       "firebird_text_index_charset_collation_compatibility_engine_validated",
       "firebird_resource_text_semantics_diagnostic_map",
       "firebird_compatibility_schema_root_uuid_required_no_cross_root_temp_access",
       "query",
       "firebird.query.select",
       false, false, false, false, true, true, false, false, false, false,
       true},
      FirebirdQueryMarkers(), "firebird resource/text query");
  return ok;
}

bool MysqlChecks() {
  using scratchbird::parser::mysql::ParseStatement;
  using scratchbird::udr::mysql_parser_support::sbu_mysql_parse_to_sblr;
  bool ok = true;
  ok &= RunCase(
      ParseStatement, sbu_mysql_parse_to_sblr,
      "create table mysql_rt_marker (name varchar(40) character set utf8mb4 collate utf8mb4_0900_ai_ci)",
      {"mysql",
       "9.7.0",
       "019e13c0-0000-7000-8000-000000000303",
       "019e13c0-1a00-7000-8000-000000000303",
       "mysql.resource_text_semantics_profile",
       "mysql_ddl_charset_collation_text_binary",
       "mysql_character_set_descriptor_uuid_required_engine_applies",
       "mysql_character_set_collation_weight_string_descriptor",
       "mysql_session_time_zone_descriptor_engine_authority",
       "mysql_temporal_calendar_sql_mode_descriptor_engine_authority",
       "mysql_text_comparison_charset_collation_coercibility_descriptor_engine_authority",
       "mysql_like_regexp_rlike_collation_descriptor",
       "mysql_binary_varbinary_blob_text_descriptor_required",
       "mysql_resource_text_descriptor_epoch_engine_catalog_bound",
       "mysql_text_index_prefix_charset_collation_compatibility_engine_validated",
       "mysql_resource_text_semantics_diagnostic_map",
       "mysql_connected_database_root_uuid_required_temp_shadowing_root_local",
       "ddl",
       "mysql.ddl.create",
       true, true, false, false, true, false, false, true, true, false,
       false},
      MysqlDdlMarkers(), "mysql resource/text DDL");
  ok &= RunCase(
      ParseStatement, sbu_mysql_parse_to_sblr,
      "select * from t where name regexp 'mysql_rt_literal_marker'",
      {"mysql",
       "9.7.0",
       "019e13c0-0000-7000-8000-000000000303",
       "019e13c0-1a00-7000-8000-000000000303",
       "mysql.resource_text_semantics_profile",
       "mysql_query_like_regexp_rlike",
       "mysql_character_set_descriptor_uuid_required_engine_applies",
       "mysql_character_set_collation_weight_string_descriptor",
       "mysql_session_time_zone_descriptor_engine_authority",
       "mysql_temporal_calendar_sql_mode_descriptor_engine_authority",
       "mysql_text_comparison_charset_collation_coercibility_descriptor_engine_authority",
       "mysql_like_regexp_rlike_collation_descriptor",
       "mysql_binary_varbinary_blob_text_descriptor_required",
       "mysql_resource_text_descriptor_epoch_engine_catalog_bound",
       "mysql_text_index_prefix_charset_collation_compatibility_engine_validated",
       "mysql_resource_text_semantics_diagnostic_map",
       "mysql_connected_database_root_uuid_required_temp_shadowing_root_local",
       "query",
       "mysql.query.select",
       false, false, false, false, true, true, false, false, false, false,
       true},
      MysqlQueryMarkers(), "mysql resource/text query");
  return ok;
}

bool PostgresqlChecks() {
  using scratchbird::parser::postgresql::ParseStatement;
  using scratchbird::udr::postgresql_parser_support::
      sbu_postgresql_parse_to_sblr;
  bool ok = true;
  ok &= RunCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "create table pg_rt_marker (name text collate \"C\")",
      {"postgresql",
       "18.3",
       "019e13c0-0000-7000-8000-000000000304",
       "019e13c0-1a00-7000-8000-000000000304",
       "postgresql.resource_text_semantics_profile",
       "postgresql_ddl_text_collation_bytea",
       "postgresql_database_encoding_descriptor_uuid_required_engine_applies",
       "postgresql_per_expression_collation_descriptor",
       "postgresql_time_zone_guc_descriptor_engine_authority",
       "postgresql_datestyle_intervalstyle_calendar_descriptor_engine_authority",
       "postgresql_text_comparison_collation_operator_descriptor_engine_authority",
       "postgresql_like_similar_to_regex_operator_descriptor",
       "postgresql_bytea_text_cast_descriptor_required",
       "postgresql_resource_text_descriptor_epoch_engine_mga_catalog_bound",
       "postgresql_text_index_operator_class_collation_compatibility_engine_validated",
       "postgresql_resource_text_semantics_diagnostic_map",
       "postgresql_connected_database_schema_root_uuid_required_pg_temp_root_local",
       "ddl",
       "postgresql.ddl.create",
       false, true, false, false, true, false, false, true, true, false,
       false},
      PostgresqlDdlMarkers(), "postgresql resource/text DDL");
  ok &= RunCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "select * from t where name like 'pg_rt_literal_marker' collate \"C\"",
      {"postgresql",
       "18.3",
       "019e13c0-0000-7000-8000-000000000304",
       "019e13c0-1a00-7000-8000-000000000304",
       "postgresql.resource_text_semantics_profile",
       "postgresql_query_like_similar_regex_collation",
       "postgresql_database_encoding_descriptor_uuid_required_engine_applies",
       "postgresql_per_expression_collation_descriptor",
       "postgresql_time_zone_guc_descriptor_engine_authority",
       "postgresql_datestyle_intervalstyle_calendar_descriptor_engine_authority",
       "postgresql_text_comparison_collation_operator_descriptor_engine_authority",
       "postgresql_like_similar_to_regex_operator_descriptor",
       "postgresql_bytea_text_cast_descriptor_required",
       "postgresql_resource_text_descriptor_epoch_engine_mga_catalog_bound",
       "postgresql_text_index_operator_class_collation_compatibility_engine_validated",
       "postgresql_resource_text_semantics_diagnostic_map",
       "postgresql_connected_database_schema_root_uuid_required_pg_temp_root_local",
       "query",
       "postgresql.query.select",
       false, true, false, false, true, true, false, false, false, false,
       true},
      PostgresqlQueryMarkers(), "postgresql resource/text query");
  return ok;
}

} // namespace

int main() {
  bool ok = true;
  ok &= FirebirdChecks();
  ok &= MysqlChecks();
  ok &= PostgresqlChecks();
  if (!ok) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
