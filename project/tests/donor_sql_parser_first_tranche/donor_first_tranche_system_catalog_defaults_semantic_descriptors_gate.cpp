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
  std::string_view donor_profile_uuid;
  std::string_view semantic_profile_uuid;
  std::string_view profile;
  std::string_view namespace_root_policy;
  std::string_view visibility_projection_policy;
  std::string_view generated_name_policy;
  std::string_view dependency_projection_policy;
  std::string_view source_visibility_policy;
  std::string_view hidden_system_object_policy;
  std::string_view grant_privilege_projection_policy;
  std::string_view sblr_opcode;
  std::string_view diagnostic_map_ref;
  std::string_view sandbox_root_policy;
  std::string_view statement_family;
  std::string_view operation_family;
  std::string_view operation_id;
  std::string_view family_count;
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

bool ExpectNumber(std::string_view json,
                  std::string_view field,
                  std::string_view value,
                  std::string_view label) {
  return Expect(Contains(json, "\"" + std::string(field) + "\":" +
                                  std::string(value)),
                std::string(label) + " missing number " +
                    std::string(field) + "=" + std::string(value));
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
  ok &= ExpectBool(payload, "parser_catalog_authority", false, label);
  ok &= ExpectBool(payload, "parser_storage_authority", false, label);
  ok &= ExpectBool(payload, "parser_dependency_authority", false, label);
  ok &= ExpectBool(payload, "parser_security_authority", false, label);
  ok &= ExpectBool(payload, "parser_source_authority", false, label);
  ok &= ExpectBool(payload, "parser_visibility_authority", false, label);
  ok &= ExpectBool(payload, "parser_execution_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_finality_authority", false,
                   label);
  ok &= ExpectBool(payload, "parser_runtime_semantic_equivalence_authority",
                   false, label);
  ok &= Expect(!Contains(payload, "parser_catalog_authority\":true"),
               std::string(label) + " granted parser catalog authority");
  ok &= Expect(!Contains(payload, "parser_storage_authority\":true"),
               std::string(label) + " granted parser storage authority");
  ok &= Expect(!Contains(payload, "parser_dependency_authority\":true"),
               std::string(label) + " granted parser dependency authority");
  ok &= Expect(!Contains(payload, "parser_security_authority\":true"),
               std::string(label) + " granted parser security authority");
  ok &= Expect(!Contains(payload, "parser_source_authority\":true"),
               std::string(label) + " granted parser source authority");
  return ok;
}

bool ExpectSystemCatalogEvidence(std::string_view payload,
                                 const Expected& expected,
                                 std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload,
                        "\"system_catalog_defaults_semantic_evidence\":{"),
               std::string(label) + " missing system catalog evidence");
  ok &= ExpectField(
      payload, "evidence_contract",
      "donor_system_catalog_defaults_semantic_descriptor_evidence.v1", label);
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required", label);
  ok &= ExpectField(payload, "donor_profile_uuid",
                    expected.donor_profile_uuid, label);
  ok &= ExpectField(payload, "semantic_profile_uuid",
                    expected.semantic_profile_uuid, label);
  ok &= ExpectField(payload, "catalog_overlay_profile_uuid",
                    expected.semantic_profile_uuid, label);
  ok &= ExpectField(payload, "dialect", expected.dialect, label);
  ok &= ExpectField(payload, "operation_id", expected.operation_id, label);
  ok &= ExpectField(payload, "system_catalog_defaults_profile",
                    expected.profile, label);
  ok &= ExpectField(payload, "system_catalog_namespace_root_policy",
                    expected.namespace_root_policy, label);
  ok &= ExpectField(payload, "catalog_visibility_projection_policy",
                    expected.visibility_projection_policy, label);
  ok &= ExpectField(payload, "generated_default_catalog_name_policy",
                    expected.generated_name_policy, label);
  ok &= ExpectField(payload, "dependency_projection_policy",
                    expected.dependency_projection_policy, label);
  ok &= ExpectField(payload, "source_visibility_policy",
                    expected.source_visibility_policy, label);
  ok &= ExpectField(payload, "hidden_system_object_policy",
                    expected.hidden_system_object_policy, label);
  ok &= ExpectField(payload, "grant_privilege_projection_policy",
                    expected.grant_privilege_projection_policy, label);
  ok &= ExpectNumber(payload, "catalog_surface_family_count",
                     expected.family_count, label);
  ok &= Expect(Contains(payload, "\"catalog_surface_families\":["),
               std::string(label) + " missing catalog surface families");
  ok &= ExpectField(payload, "sblr_catalog_projection_opcode",
                    expected.sblr_opcode, label);
  ok &= ExpectField(payload, "diagnostic_map_ref",
                    expected.diagnostic_map_ref, label);
  ok &= ExpectField(payload, "sandbox_root_policy",
                    expected.sandbox_root_policy, label);
  ok &= ExpectBool(payload, "uuid_required_semantic_profile", true, label);
  ok &= ExpectBool(payload, "catalog_descriptor_required", true, label);
  ok &= ExpectBool(payload, "catalog_projection_descriptor_required", true,
                   label);
  ok &= ExpectBool(payload, "dependency_descriptor_required", true, label);
  ok &= ExpectBool(payload, "security_descriptor_required", true, label);
  ok &= ExpectBool(payload, "source_descriptor_required", true, label);
  ok &= ExpectBool(payload, "sblr_operation_uuid_resolution_required", true,
                   label);
  ok &= ExpectField(payload, "engine_authority", "scratchbird", label);
  ok &= ExpectField(payload, "catalog_authority",
                    "engine_catalog_uuid_projection", label);
  ok &= ExpectField(payload, "storage_authority",
                    "engine_storage_catalog_authority", label);
  ok &= ExpectField(payload, "dependency_authority",
                    "engine_dependency_graph_authority", label);
  ok &= ExpectField(payload, "security_authority",
                    "engine_security_policy_authority", label);
  ok &= ExpectField(payload, "source_authority",
                    "engine_source_retention_policy_authority", label);
  ok &= ExpectField(payload, "visibility_authority",
                    "engine_catalog_visibility_authority", label);
  ok &= ExpectBool(payload, "source_sql_text_included", false, label);
  ok &= ExpectBool(payload, "literal_text_included", false, label);
  ok &= ExpectBool(payload, "object_name_text_included", false, label);
  ok &= ExpectBool(payload, "quoted_identifier_text_included", false, label);
  ok &= ExpectBool(payload, "sblr_embeds_source_identifiers", false, label);
  ok &= ExpectAuthorityFalse(payload, label);
  ok &= ExpectBool(payload, "donor_sql_executed", false, label);
  ok &= ExpectField(payload, "runtime_semantic_equivalence",
                    "not_enterprise_proven_pending", label);
  ok &= ExpectField(payload, "readiness_status", "proof_pending", label);
  ok &= ExpectField(
      payload, "descriptor_exactness_status",
      "parser_system_catalog_defaults_descriptor_recorded_runtime_equivalence_pending",
      label);
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready",
                    label);
  ok &= Expect(Contains(payload, "enterprise_implemented_proven\":false"),
               std::string(label) + " missing enterprise implementation proof");
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
  ok &= ExpectSystemCatalogEvidence(payload, expected, label);
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
  ok &= ExpectSystemCatalogEvidence(result.parser_evidence_json, expected,
                                    label);
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

Expected FirebirdExpected(std::string_view operation_family) {
  return {"firebird",
          "019e13c0-0000-7000-8000-000000000302",
          "019e13c0-1d00-7000-8000-000000000302",
          "firebird.system_catalog_defaults_semantics_profile",
          "firebird_rdb_mon_sec_information_schema_projected_from_engine_catalog_uuid_root",
          "firebird_system_relations_visible_through_engine_privilege_filtered_projection",
          "firebird_generated_rdb_names_projected_as_catalog_descriptors_not_parser_names",
          "firebird_rdb_dependencies_projected_from_engine_dependency_graph",
          "firebird_rdb_source_columns_redacted_or_projected_by_engine_source_retention_policy",
          "firebird_rdb_system_flag_hidden_objects_privilege_filtered_engine_projection",
          "firebird_rdb_user_privileges_sec_projection_engine_security_authority",
          "SBLR_DONOR_FIREBIRD_CATALOG_PROJECT",
          "firebird_system_catalog_defaults_semantics_diagnostic_map",
          "firebird_donor_schema_root_uuid_required_no_cross_root_temp_access",
          "catalog_overlay",
          operation_family,
          operation_family,
          "7"};
}

Expected MysqlExpected(std::string_view statement_family,
                       std::string_view operation_family,
                       std::string_view operation_id) {
  return {"mysql",
          "019e13c0-0000-7000-8000-000000000303",
          "019e13c0-1d00-7000-8000-000000000303",
          "mysql.system_catalog_defaults_semantics_profile",
          "mysql_information_schema_mysql_performance_schema_sys_projected_from_connected_catalog_root",
          "mysql_show_describe_information_schema_privilege_filtered_projection",
          "mysql_generated_constraint_index_names_projected_from_engine_dictionary_descriptors",
          "mysql_information_schema_dependencies_projected_without_parser_dependency_authority",
          "mysql_routine_trigger_view_source_redacted_or_projected_by_engine_source_policy",
          "mysql_data_dictionary_hidden_objects_privilege_filtered_engine_projection",
          "mysql_grants_information_schema_projection_engine_security_authority",
          "SBLR_DONOR_MYSQL_CATALOG_PROJECT",
          "mysql_system_catalog_defaults_semantics_diagnostic_map",
          "mysql_connected_database_root_uuid_required_temp_shadowing_root_local",
          statement_family,
          operation_family,
          operation_id,
          "8"};
}

Expected PostgresqlExpected(std::string_view operation_id) {
  return {"postgresql",
          "019e13c0-0000-7000-8000-000000000304",
          "019e13c0-1d00-7000-8000-000000000304",
          "postgresql.system_catalog_defaults_semantics_profile",
          "postgresql_pg_catalog_information_schema_projected_from_connected_database_catalog_root",
          "postgresql_pg_catalog_information_schema_privilege_filtered_projection",
          "postgresql_generated_pg_class_pg_constraint_names_projected_from_engine_catalog_descriptors",
          "postgresql_pg_depend_projection_from_engine_dependency_graph",
          "postgresql_pg_proc_pg_views_source_redacted_or_projected_by_engine_source_policy",
          "postgresql_toast_temp_internal_objects_privilege_filtered_engine_projection",
          "postgresql_acl_roles_information_schema_projection_engine_security_authority",
          "SBLR_DONOR_POSTGRESQL_CATALOG_PROJECT",
          "postgresql_system_catalog_defaults_semantics_diagnostic_map",
          "postgresql_connected_database_schema_root_uuid_required_pg_temp_root_local",
          "query",
          "postgresql.query.select",
          operation_id,
          "9"};
}

bool FirebirdChecks() {
  using scratchbird::parser::firebird::ParseStatement;
  using scratchbird::udr::firebird_parser_support::sbu_firebird_parse_to_sblr;
  bool ok = true;
  ok &= RunCase(ParseStatement, sbu_firebird_parse_to_sblr,
                "select rdb$relation_name from rdb$relations",
                FirebirdExpected("firebird.catalog_overlay.rdb_core"),
                "firebird rdb relations catalog defaults descriptor");
  ok &= RunCase(ParseStatement, sbu_firebird_parse_to_sblr,
                "select rdb$field_name from rdb$fields",
                FirebirdExpected("firebird.catalog_overlay.rdb_core"),
                "firebird rdb fields catalog defaults descriptor");
  return ok;
}

bool MysqlChecks() {
  using scratchbird::parser::mysql::ParseStatement;
  using scratchbird::udr::mysql_parser_support::sbu_mysql_parse_to_sblr;
  bool ok = true;
  ok &= RunCase(ParseStatement, sbu_mysql_parse_to_sblr, "show tables",
                MysqlExpected("catalog_overlay", "mysql.catalog_overlay.show",
                              "mysql.catalog.show"),
                "mysql show tables catalog defaults descriptor");
  ok &= RunCase(ParseStatement, sbu_mysql_parse_to_sblr,
                "describe inventory",
                MysqlExpected("catalog_overlay",
                              "mysql.catalog_overlay.describe",
                              "mysql.catalog.describe"),
                "mysql describe catalog defaults descriptor");
  ok &= RunCase(ParseStatement, sbu_mysql_parse_to_sblr,
                "select table_name from information_schema.tables",
                MysqlExpected("query", "mysql.query.select",
                              "mysql.query.select"),
                "mysql information_schema catalog defaults descriptor");
  return ok;
}

bool PostgresqlChecks() {
  using scratchbird::parser::postgresql::ParseStatement;
  using scratchbird::udr::postgresql_parser_support::
      sbu_postgresql_parse_to_sblr;
  bool ok = true;
  ok &= RunCase(ParseStatement, sbu_postgresql_parse_to_sblr,
                "select relname from pg_catalog.pg_class",
                PostgresqlExpected("postgresql.query.select"),
                "postgresql pg_catalog catalog defaults descriptor");
  ok &= RunCase(ParseStatement, sbu_postgresql_parse_to_sblr,
                "select table_name from information_schema.tables",
                PostgresqlExpected("postgresql.query.select"),
                "postgresql information_schema catalog defaults descriptor");
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
