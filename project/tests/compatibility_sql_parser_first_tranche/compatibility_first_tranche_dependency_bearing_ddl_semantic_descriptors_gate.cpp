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

struct DependencyExpected {
  std::string_view dialect;
  std::string_view release_profile;
  std::string_view compatibility_profile_uuid;
  std::string_view semantic_profile_uuid;
  std::string_view dependency_ddl_profile;
  std::string_view statement_family;
  std::string_view operation_family;
  std::string_view dependency_ddl_surface;
  std::string_view dependency_binding_policy;
  std::string_view invalidation_policy;
  std::string_view execution_body_policy;
  std::string_view catalog_storage_policy;
  std::string_view sandbox_root_policy;
  std::string_view sblr_operation;
  std::string_view engine_api_function;
  bool view_surface{false};
  bool materialized_view_surface{false};
  bool trigger_surface{false};
  bool routine_surface{false};
  bool procedure_surface{false};
  bool function_surface{false};
  bool package_surface{false};
  bool rule_surface{false};
  bool event_surface{false};
  bool executable_body_surface{false};
  bool query_dependency_surface{false};
  bool create_surface{false};
  bool alter_surface{false};
  bool drop_surface{false};
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
                     " leaked dependency-bearing DDL source marker: " +
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
                             const DependencyExpected& expected,
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

bool ExpectDependencyEvidence(std::string_view payload,
                              const DependencyExpected& expected,
                              std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload,
                        "\"dependency_bearing_ddl_semantic_evidence\":{"),
               std::string(label) + " missing dependency-bearing DDL evidence");
  ok &= ExpectField(
      payload, "evidence_contract",
      "compatibility_dependency_bearing_ddl_semantic_descriptor_evidence.v1", label);
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required", label);
  ok &= ExpectField(payload, "compatibility_profile_uuid",
                    expected.compatibility_profile_uuid, label);
  ok &= ExpectField(payload, "semantic_profile_uuid",
                    expected.semantic_profile_uuid, label);
  ok &= ExpectField(payload, "dialect", expected.dialect, label);
  ok &= ExpectField(payload, "release_profile", expected.release_profile,
                    label);
  ok &= ExpectField(payload, "dependency_ddl_profile",
                    expected.dependency_ddl_profile, label);
  ok &= ExpectField(payload, "dependency_ddl_surface",
                    expected.dependency_ddl_surface, label);
  ok &= ExpectBool(payload, "view_surface", expected.view_surface, label);
  ok &= ExpectBool(payload, "materialized_view_surface",
                   expected.materialized_view_surface, label);
  ok &= ExpectBool(payload, "trigger_surface", expected.trigger_surface,
                   label);
  ok &= ExpectBool(payload, "routine_surface", expected.routine_surface,
                   label);
  ok &= ExpectBool(payload, "procedure_surface", expected.procedure_surface,
                   label);
  ok &= ExpectBool(payload, "function_surface", expected.function_surface,
                   label);
  ok &= ExpectBool(payload, "package_surface", expected.package_surface,
                   label);
  ok &= ExpectBool(payload, "rule_surface", expected.rule_surface, label);
  ok &= ExpectBool(payload, "event_surface", expected.event_surface, label);
  ok &= ExpectBool(payload, "executable_body_surface",
                   expected.executable_body_surface, label);
  ok &= ExpectBool(payload, "query_dependency_surface",
                   expected.query_dependency_surface, label);
  ok &= ExpectBool(payload, "create_surface", expected.create_surface, label);
  ok &= ExpectBool(payload, "alter_surface", expected.alter_surface, label);
  ok &= ExpectBool(payload, "drop_surface", expected.drop_surface, label);
  ok &= ExpectField(payload, "dependency_binding_policy",
                    expected.dependency_binding_policy, label);
  ok &= ExpectField(payload, "invalidation_policy",
                    expected.invalidation_policy, label);
  ok &= ExpectField(payload, "execution_body_policy",
                    expected.execution_body_policy, label);
  ok &= ExpectField(payload, "catalog_storage_policy",
                    expected.catalog_storage_policy, label);
  ok &= ExpectField(payload, "sandbox_root_policy",
                    expected.sandbox_root_policy, label);
  ok &= ExpectBool(payload, "uuid_required_semantic_profile", true, label);
  ok &= ExpectBool(payload, "catalog_descriptor_required", true, label);
  ok &= ExpectBool(payload, "dependency_graph_descriptor_required", true,
                   label);
  ok &= ExpectBool(payload, "source_retention_reference_required",
                   expected.executable_body_surface, label);
  ok &= ExpectBool(payload, "sblr_operation_uuid_resolution_required", true,
                   label);
  ok &= ExpectField(payload, "engine_authority", "scratchbird", label);
  ok &= ExpectField(payload, "dependency_authority",
                    "engine_catalog_uuid_dependency_graph", label);
  ok &= ExpectBool(payload, "source_sql_text_included", false, label);
  ok &= ExpectBool(payload, "literal_text_included", false, label);
  ok &= ExpectBool(payload, "object_name_text_included", false, label);
  ok &= ExpectBool(payload, "quoted_identifier_text_included", false, label);
  ok &= ExpectBool(payload, "sblr_embeds_source_identifiers", false, label);
  ok &= ExpectBool(payload, "parser_catalog_authority", false, label);
  ok &= ExpectBool(payload, "parser_storage_authority", false, label);
  ok &= ExpectBool(payload, "parser_execution_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_finality_authority", false,
                   label);
  ok &= ExpectBool(payload, "parser_dependency_finality_authority", false,
                   label);
  ok &= ExpectBool(payload, "parser_invalidation_authority", false, label);
  ok &= ExpectBool(payload, "compatibility_sql_executed", false, label);
  ok &= ExpectField(payload, "runtime_semantic_equivalence",
                    "not_enterprise_proven_pending", label);
  ok &= ExpectField(
      payload, "descriptor_exactness_status",
      "parser_dependency_bearing_ddl_descriptor_recorded_runtime_equivalence_pending",
      label);
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready",
                    label);
  ok &= Expect(!Contains(payload, "parser_catalog_authority\":true"),
               std::string(label) + " granted parser catalog authority");
  ok &= Expect(!Contains(payload, "parser_storage_authority\":true"),
               std::string(label) + " granted parser storage authority");
  ok &= Expect(!Contains(payload, "parser_execution_authority\":true"),
               std::string(label) + " granted parser execution authority");
  ok &= Expect(!Contains(payload, "parser_dependency_finality_authority\":true"),
               std::string(label) + " granted parser dependency authority");
  ok &= Expect(!Contains(payload, "compatibility_sql_executed\":true"),
               std::string(label) + " executed compatibility SQL");
  return ok;
}

template <typename Result>
bool ExpectDirectDependencyResult(
    const Result& result,
    const DependencyExpected& expected,
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
  ok &= ExpectDependencyEvidence(result.parser_evidence_json, expected, label);
  ok &= ExpectEnvelopeAuthority(result.sblr_envelope, expected, label);
  ok &= ExpectDependencyEvidence(result.sblr_envelope, expected, label);
  ok &= ExpectNoMarkers(result.parser_evidence_json, markers, label);
  ok &= ExpectNoMarkers(result.sblr_envelope, markers, label);
  ok &= ExpectNoMysqlLts(result.parser_evidence_json, label);
  ok &= ExpectNoMysqlLts(result.sblr_envelope, label);
  return ok;
}

template <typename UdrResult>
bool ExpectUdrDependencyResult(const UdrResult& result,
                               const DependencyExpected& expected,
                               std::span<const std::string_view> markers,
                               std::string_view label) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " UDR parse failed: " +
                             result.message_vector_json);
  ok &= ExpectEnvelopeAuthority(result.payload, expected, label);
  ok &= ExpectDependencyEvidence(result.payload, expected, label);
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
bool RunDependencyCase(DirectParser direct_parser,
                       UdrParser udr_parser,
                       std::string_view sql,
                       const DependencyExpected& expected,
                       std::span<const std::string_view> markers,
                       std::string_view label) {
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  bool ok = true;
  ok &= ExpectDirectDependencyResult(direct_parser(sql), expected, markers,
                                     std::string(label) + " direct");
  ok &= ExpectUdrDependencyResult(udr_parser(sql, trusted), expected, markers,
                                  std::string(label) + " UDR");
  return ok;
}

std::span<const std::string_view> FirebirdMarkers() {
  static constexpr std::string_view markers[] = {
      "fb_dep_view_marker", "FB_DEP_VIEW_MARKER",
      "fb_dep_trigger_marker", "FB_DEP_TRIGGER_MARKER",
      "fb_dep_body_secret", "FB_DEP_BODY_SECRET"};
  return markers;
}

std::span<const std::string_view> MysqlMarkers() {
  static constexpr std::string_view markers[] = {
      "mysql_dep_view_marker", "MYSQL_DEP_VIEW_MARKER",
      "mysql_dep_event_marker", "MYSQL_DEP_EVENT_MARKER"};
  return markers;
}

std::span<const std::string_view> PostgresqlMarkers() {
  static constexpr std::string_view markers[] = {
      "pg_dep_mv_marker", "PG_DEP_MV_MARKER",
      "pg_dep_rule_marker", "PG_DEP_RULE_MARKER"};
  return markers;
}

bool FirebirdChecks() {
  using scratchbird::parser::firebird::ParseStatement;
  using scratchbird::udr::firebird_parser_support::sbu_firebird_parse_to_sblr;
  bool ok = true;
  ok &= RunDependencyCase(
      ParseStatement, sbu_firebird_parse_to_sblr,
      "create view fb_dep_view_marker as select id from base_table",
      {"firebird",
       "5.0.4",
       "019e13c0-0000-7000-8000-000000000302",
       "019e13c0-1800-7000-8000-000000000302",
       "firebird.dependency_bearing_ddl_semantics_profile",
       "ddl",
       "firebird.ddl.create.view",
       "firebird_create_view",
       "firebird_rdb_dependency_binding_uuid_catalog_descriptors",
       "firebird_metadata_dependency_invalidation_engine_catalog_authority",
       "firebird_view_query_dependency_descriptor_no_parser_execution",
       "firebird_rdb_catalog_projection_stores_uuid_dependency_descriptors",
       "firebird_compatibility_schema_root_uuid_required_no_cross_root_temp_access",
       "",
       "",
       true, false, false, false, false, false, false, false, false,
       false, true, true, false, false},
      FirebirdMarkers(), "firebird create view dependency");
  ok &= RunDependencyCase(
      ParseStatement, sbu_firebird_parse_to_sblr,
      "create trigger fb_dep_trigger_marker for t before insert as begin "
      "post_event 'fb_dep_body_secret'; end",
      {"firebird",
       "5.0.4",
       "019e13c0-0000-7000-8000-000000000302",
       "019e13c0-1800-7000-8000-000000000302",
       "firebird.dependency_bearing_ddl_semantics_profile",
       "ddl",
       "firebird.ddl.create.trigger",
       "firebird_trigger_ddl",
       "firebird_trigger_relation_event_dependency_binding_uuid_descriptors",
       "firebird_metadata_dependency_invalidation_engine_catalog_authority",
       "firebird_psql_body_stored_as_catalog_reference_and_lowered_to_sblr_uuid",
       "firebird_rdb_catalog_projection_stores_uuid_dependency_descriptors",
       "firebird_compatibility_schema_root_uuid_required_no_cross_root_temp_access",
       "",
       "",
       false, false, true, false, false, false, false, false, false,
       true, true, true, false, false},
      FirebirdMarkers(), "firebird create trigger dependency");
  return ok;
}

bool MysqlChecks() {
  using scratchbird::parser::mysql::ParseStatement;
  using scratchbird::udr::mysql_parser_support::sbu_mysql_parse_to_sblr;
  bool ok = true;
  ok &= RunDependencyCase(
      ParseStatement, sbu_mysql_parse_to_sblr,
      "create or replace view mysql_dep_view_marker as select id from base_table",
      {"mysql",
       "9.7.0",
       "019e13c0-0000-7000-8000-000000000303",
       "019e13c0-1800-7000-8000-000000000303",
       "mysql.dependency_bearing_ddl_semantics_profile",
       "ddl",
       "mysql.ddl.create_or_replace.view",
       "mysql_create_or_replace_view",
       "mysql_routine_view_dependency_binding_uuid_descriptors",
       "mysql_metadata_dependency_invalidation_engine_catalog_authority",
       "mysql_view_definition_descriptor_no_parser_execution",
       "mysql_information_schema_catalog_projection_stores_uuid_dependency_descriptors",
       "mysql_connected_database_root_uuid_required_temp_shadowing_root_local",
       "SBLR_COMPATIBILITY_MYSQL_VIEW_CREATE_OR_REPLACE",
       "EngineDdlCreateOrReplaceView",
       true, false, false, false, false, false, false, false, false,
       false, true, true, false, false},
      MysqlMarkers(), "mysql create or replace view dependency");
  ok &= RunDependencyCase(
      ParseStatement, sbu_mysql_parse_to_sblr,
      "create event mysql_dep_event_marker on schedule every 1 day do select 1",
      {"mysql",
       "9.7.0",
       "019e13c0-0000-7000-8000-000000000303",
       "019e13c0-1800-7000-8000-000000000303",
       "mysql.dependency_bearing_ddl_semantics_profile",
       "routine",
       "mysql.routine.event.create",
       "mysql_event_scheduler_ddl",
       "mysql_event_scheduler_dependency_binding_uuid_descriptors",
       "mysql_metadata_dependency_invalidation_engine_catalog_authority",
       "mysql_routine_trigger_event_body_routes_to_trusted_udr_lowering",
       "mysql_information_schema_catalog_projection_stores_uuid_dependency_descriptors",
       "mysql_connected_database_root_uuid_required_temp_shadowing_root_local",
       "SBLR_COMPATIBILITY_MYSQL_ROUTINE_ROUTE",
       "ParserSupportRoutineRoute",
       false, false, false, false, false, false, false, false, true,
       true, true, true, false, false},
      MysqlMarkers(), "mysql create event dependency");
  return ok;
}

bool PostgresqlChecks() {
  using scratchbird::parser::postgresql::ParseStatement;
  using scratchbird::udr::postgresql_parser_support::
      sbu_postgresql_parse_to_sblr;
  bool ok = true;
  ok &= RunDependencyCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "create materialized view pg_dep_mv_marker as select id from base_table",
      {"postgresql",
       "18.3",
       "019e13c0-0000-7000-8000-000000000304",
       "019e13c0-1800-7000-8000-000000000304",
       "postgresql.dependency_bearing_ddl_semantics_profile",
       "ddl",
       "postgresql.ddl.create.materialized_view",
       "postgresql_materialized_view_ddl",
       "postgresql_materialized_view_dependency_binding_uuid_descriptors",
       "postgresql_materialized_view_refresh_dependency_invalidation_engine_catalog_authority",
       "postgresql_view_query_dependency_descriptor_no_parser_execution",
       "postgresql_pg_catalog_projection_stores_uuid_dependency_descriptors",
       "postgresql_connected_database_schema_root_uuid_required_pg_temp_root_local",
       "SBLR_COMPATIBILITY_POSTGRESQL_MATERIALIZED_VIEW_CREATE",
       "EngineDdlCreateMaterializedView",
       false, true, false, false, false, false, false, false, false,
       false, true, true, false, false},
      PostgresqlMarkers(), "postgresql create materialized view dependency");
  ok &= RunDependencyCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "create rule pg_dep_rule_marker as on insert to t do instead nothing",
      {"postgresql",
       "18.3",
       "019e13c0-0000-7000-8000-000000000304",
       "019e13c0-1800-7000-8000-000000000304",
       "postgresql.dependency_bearing_ddl_semantics_profile",
       "routine",
       "postgresql.routine.rule.create",
       "postgresql_rule_rewrite_ddl",
       "postgresql_rewrite_rule_dependency_binding_uuid_descriptors",
       "postgresql_pg_depend_invalidation_engine_catalog_authority",
       "postgresql_routine_trigger_rule_body_routes_to_trusted_udr_lowering",
       "postgresql_pg_catalog_projection_stores_uuid_dependency_descriptors",
       "postgresql_connected_database_schema_root_uuid_required_pg_temp_root_local",
       "SBLR_COMPATIBILITY_POSTGRESQL_ROUTINE_ROUTE",
       "ParserSupportRoutineRoute",
       false, false, false, false, false, false, false, true, false,
       true, true, true, false, false},
      PostgresqlMarkers(), "postgresql create rule dependency");
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
