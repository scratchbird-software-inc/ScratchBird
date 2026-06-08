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

struct DdlTxnExpected {
  std::string_view dialect;
  std::string_view release_profile;
  std::string_view donor_profile_uuid;
  std::string_view semantic_profile_uuid;
  std::string_view ddl_transaction_behavior_profile;
  std::string_view statement_family;
  std::string_view operation_family;
  std::string_view ddl_operation_kind;
  std::string_view transaction_policy;
  std::string_view autocommit_boundary;
  std::string_view metadata_visibility_epoch;
  std::string_view rollback_policy;
  std::string_view invalid_object_state_policy;
  std::string_view diagnostic_map_ref;
  std::string_view sandbox_root_policy;
  std::string_view sblr_operation;
  std::string_view engine_api_function;
  bool create_surface{false};
  bool alter_surface{false};
  bool drop_surface{false};
  bool table_surface{false};
  bool index_surface{false};
  bool view_surface{false};
  bool implicit_commit_surface{false};
  bool transactional_ddl_surface{false};
  bool nontransactional_ddl_surface{false};
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
                     " leaked DDL transaction source marker: " +
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
                             const DdlTxnExpected& expected,
                             std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload, "\"envelope\":\"SBLRExecutionEnvelope.v3\""),
               std::string(label) + " missing SBLR envelope");
  ok &= ExpectField(payload, "dialect", expected.dialect, label);
  ok &= ExpectField(payload, "operation_family", expected.operation_family,
                    label);
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required", label);
  ok &= ExpectField(payload, "engine_authority", "scratchbird", label);
  ok &= ExpectBool(payload, "donor_engine_sql_executed", false, label);
  ok &= ExpectBool(payload, "sql_text_included", false, label);
  ok &= ExpectReadinessComplete(payload, label);
  return ok;
}

bool ExpectDdlTxnEvidence(std::string_view payload,
                          const DdlTxnExpected& expected,
                          std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload,
                        "\"ddl_transaction_behavior_semantic_evidence\":{"),
               std::string(label) + " missing DDL transaction behavior evidence");
  ok &= ExpectField(
      payload, "evidence_contract",
      "donor_ddl_transaction_behavior_semantic_descriptor_evidence.v1", label);
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required", label);
  ok &= ExpectField(payload, "donor_profile_uuid",
                    expected.donor_profile_uuid, label);
  ok &= ExpectField(payload, "semantic_profile_uuid",
                    expected.semantic_profile_uuid, label);
  ok &= ExpectField(payload, "dialect", expected.dialect, label);
  ok &= ExpectField(payload, "release_profile", expected.release_profile,
                    label);
  ok &= ExpectField(payload, "ddl_transaction_behavior_profile",
                    expected.ddl_transaction_behavior_profile, label);
  ok &= ExpectField(payload, "statement_classification", "ddl", label);
  ok &= ExpectField(payload, "ddl_operation_kind",
                    expected.ddl_operation_kind, label);
  ok &= ExpectField(payload, "transaction_policy",
                    expected.transaction_policy, label);
  ok &= ExpectField(payload, "autocommit_boundary",
                    expected.autocommit_boundary, label);
  ok &= ExpectField(payload, "metadata_visibility_epoch",
                    expected.metadata_visibility_epoch, label);
  ok &= ExpectField(payload, "rollback_policy", expected.rollback_policy,
                    label);
  ok &= ExpectField(payload, "invalid_object_state_policy",
                    expected.invalid_object_state_policy, label);
  ok &= ExpectField(payload, "diagnostic_map_ref",
                    expected.diagnostic_map_ref, label);
  ok &= ExpectField(payload, "sandbox_root_policy",
                    expected.sandbox_root_policy, label);
  ok &= ExpectBool(payload, "create_surface", expected.create_surface, label);
  ok &= ExpectBool(payload, "alter_surface", expected.alter_surface, label);
  ok &= ExpectBool(payload, "drop_surface", expected.drop_surface, label);
  ok &= ExpectBool(payload, "table_surface", expected.table_surface, label);
  ok &= ExpectBool(payload, "index_surface", expected.index_surface, label);
  ok &= ExpectBool(payload, "view_surface", expected.view_surface, label);
  ok &= ExpectBool(payload, "implicit_commit_surface",
                   expected.implicit_commit_surface, label);
  ok &= ExpectBool(payload, "transactional_ddl_surface",
                   expected.transactional_ddl_surface, label);
  ok &= ExpectBool(payload, "nontransactional_ddl_surface",
                   expected.nontransactional_ddl_surface, label);
  ok &= ExpectBool(payload, "uuid_required_semantic_profile", true, label);
  ok &= ExpectBool(payload, "catalog_descriptor_required", true, label);
  ok &= ExpectBool(payload, "sblr_operation_uuid_resolution_required", true,
                   label);
  ok &= ExpectField(payload, "engine_authority", "scratchbird", label);
  ok &= ExpectField(payload, "transaction_authority",
                    "engine_mga_authority", label);
  ok &= ExpectField(payload, "metadata_visibility_authority",
                    "engine_catalog_mga_epoch", label);
  ok &= ExpectField(payload, "rollback_authority", "engine_mga_authority",
                    label);
  ok &= ExpectField(payload, "invalid_object_state_authority",
                    "engine_catalog_uuid_descriptor", label);
  ok &= ExpectBool(payload, "source_sql_text_included", false, label);
  ok &= ExpectBool(payload, "literal_text_included", false, label);
  ok &= ExpectBool(payload, "object_name_text_included", false, label);
  ok &= ExpectBool(payload, "quoted_identifier_text_included", false, label);
  ok &= ExpectBool(payload, "sblr_embeds_source_identifiers", false, label);
  ok &= ExpectBool(payload, "parser_catalog_authority", false, label);
  ok &= ExpectBool(payload, "parser_storage_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_finality_authority", false,
                   label);
  ok &= ExpectBool(payload, "parser_autocommit_authority", false, label);
  ok &= ExpectBool(payload, "parser_metadata_visibility_authority", false,
                   label);
  ok &= ExpectBool(payload, "parser_rollback_authority", false, label);
  ok &= ExpectBool(payload, "parser_invalid_object_state_authority", false,
                   label);
  ok &= ExpectBool(payload, "parser_recovery_authority", false, label);
  ok &= ExpectBool(payload, "donor_sql_executed", false, label);
  ok &= ExpectField(payload, "runtime_semantic_equivalence",
                    "not_enterprise_proven_pending", label);
  ok &= ExpectField(
      payload, "descriptor_exactness_status",
      "parser_ddl_transaction_behavior_descriptor_recorded_runtime_equivalence_pending",
      label);
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready",
                    label);
  ok &= Expect(!Contains(payload, "parser_catalog_authority\":true"),
               std::string(label) + " granted parser catalog authority");
  ok &= Expect(!Contains(payload, "parser_storage_authority\":true"),
               std::string(label) + " granted parser storage authority");
  ok &= Expect(!Contains(payload, "parser_transaction_authority\":true"),
               std::string(label) + " granted parser transaction authority");
  ok &= Expect(!Contains(payload, "parser_autocommit_authority\":true"),
               std::string(label) + " granted parser autocommit authority");
  ok &= Expect(!Contains(payload, "parser_recovery_authority\":true"),
               std::string(label) + " granted parser recovery authority");
  ok &= Expect(!Contains(payload, "donor_sql_executed\":true"),
               std::string(label) + " executed donor SQL");
  return ok;
}

template <typename Result>
bool ExpectDirectDdlTxnResult(const Result& result,
                              const DdlTxnExpected& expected,
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
  ok &= ExpectDdlTxnEvidence(result.parser_evidence_json, expected, label);
  ok &= ExpectEnvelopeAuthority(result.sblr_envelope, expected, label);
  ok &= ExpectDdlTxnEvidence(result.sblr_envelope, expected, label);
  ok &= ExpectNoMarkers(result.parser_evidence_json, markers, label);
  ok &= ExpectNoMarkers(result.sblr_envelope, markers, label);
  ok &= ExpectNoMysqlLts(result.parser_evidence_json, label);
  ok &= ExpectNoMysqlLts(result.sblr_envelope, label);
  return ok;
}

template <typename UdrResult>
bool ExpectUdrDdlTxnResult(const UdrResult& result,
                           const DdlTxnExpected& expected,
                           std::span<const std::string_view> markers,
                           std::string_view label) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " UDR parse failed: " +
                             result.message_vector_json);
  ok &= ExpectEnvelopeAuthority(result.payload, expected, label);
  ok &= ExpectDdlTxnEvidence(result.payload, expected, label);
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
bool RunDdlTxnCase(DirectParser direct_parser,
                   UdrParser udr_parser,
                   std::string_view sql,
                   const DdlTxnExpected& expected,
                   std::span<const std::string_view> markers,
                   std::string_view label) {
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  bool ok = true;
  ok &= ExpectDirectDdlTxnResult(direct_parser(sql), expected, markers,
                                 std::string(label) + " direct");
  ok &= ExpectUdrDdlTxnResult(udr_parser(sql, trusted), expected, markers,
                              std::string(label) + " UDR");
  return ok;
}

std::span<const std::string_view> FirebirdMarkers() {
  static constexpr std::string_view markers[] = {
      "fb_ddl_txn_create_marker", "FB_DDL_TXN_CREATE_MARKER",
      "fb_ddl_txn_drop_marker", "FB_DDL_TXN_DROP_MARKER"};
  return markers;
}

std::span<const std::string_view> MysqlMarkers() {
  static constexpr std::string_view markers[] = {
      "mysql_ddl_txn_create_marker", "MYSQL_DDL_TXN_CREATE_MARKER",
      "mysql_ddl_txn_alter_marker", "MYSQL_DDL_TXN_ALTER_MARKER"};
  return markers;
}

std::span<const std::string_view> PostgresqlMarkers() {
  static constexpr std::string_view markers[] = {
      "pg_ddl_txn_create_marker", "PG_DDL_TXN_CREATE_MARKER",
      "pg_ddl_txn_index_marker", "PG_DDL_TXN_INDEX_MARKER"};
  return markers;
}

bool FirebirdChecks() {
  using scratchbird::parser::firebird::ParseStatement;
  using scratchbird::udr::firebird_parser_support::sbu_firebird_parse_to_sblr;
  bool ok = true;
  ok &= RunDdlTxnCase(
      ParseStatement, sbu_firebird_parse_to_sblr,
      "create table fb_ddl_txn_create_marker (id integer not null)",
      {"firebird",
       "5.0.4",
       "019e13c0-0000-7000-8000-000000000302",
       "019e13c0-1900-7000-8000-000000000302",
       "firebird.ddl_transaction_behavior_semantics_profile",
       "ddl",
       "firebird.ddl.create",
       "create_table",
       "firebird_transactional_ddl_engine_mga_descriptor_required",
       "none_parser_does_not_commit_engine_transaction",
       "transaction_local_until_engine_commit_then_catalog_epoch",
       "ddl_rollback_requires_engine_mga_transaction_rollback",
       "firebird_metadata_invalid_state_catalog_descriptor_engine_authority",
       "firebird_ddl_transaction_behavior_diagnostic_map",
       "firebird_donor_schema_root_uuid_required_no_cross_root_temp_access",
       "",
       "",
       true, false, false, true, false, false, false, true, false},
      FirebirdMarkers(), "firebird create table DDL transaction behavior");
  ok &= RunDdlTxnCase(
      ParseStatement, sbu_firebird_parse_to_sblr,
      "drop table fb_ddl_txn_drop_marker",
      {"firebird",
       "5.0.4",
       "019e13c0-0000-7000-8000-000000000302",
       "019e13c0-1900-7000-8000-000000000302",
       "firebird.ddl_transaction_behavior_semantics_profile",
       "ddl",
       "firebird.ddl.drop",
       "drop_table",
       "firebird_transactional_ddl_engine_mga_descriptor_required",
       "none_parser_does_not_commit_engine_transaction",
       "transaction_local_until_engine_commit_then_catalog_epoch",
       "ddl_rollback_requires_engine_mga_transaction_rollback",
       "firebird_metadata_invalid_state_catalog_descriptor_engine_authority",
       "firebird_ddl_transaction_behavior_diagnostic_map",
       "firebird_donor_schema_root_uuid_required_no_cross_root_temp_access",
       "",
       "",
       false, false, true, true, false, false, false, true, false},
      FirebirdMarkers(), "firebird drop table DDL transaction behavior");
  return ok;
}

bool MysqlChecks() {
  using scratchbird::parser::mysql::ParseStatement;
  using scratchbird::udr::mysql_parser_support::sbu_mysql_parse_to_sblr;
  bool ok = true;
  ok &= RunDdlTxnCase(
      ParseStatement, sbu_mysql_parse_to_sblr,
      "create table mysql_ddl_txn_create_marker (id int primary key)",
      {"mysql",
       "9.7.0",
       "019e13c0-0000-7000-8000-000000000303",
       "019e13c0-1900-7000-8000-000000000303",
       "mysql.ddl_transaction_behavior_semantics_profile",
       "ddl",
       "mysql.ddl.create",
       "create_table",
       "mysql_implicit_commit_ddl_descriptor_required",
       "implicit_commit_before_and_after_ddl_engine_policy",
       "post_implicit_commit_catalog_epoch",
       "mysql_ddl_not_rolled_back_by_user_transaction_descriptor",
       "mysql_atomic_ddl_dictionary_state_engine_authority",
       "mysql_ddl_transaction_behavior_diagnostic_map",
       "mysql_connected_database_root_uuid_required_temp_shadowing_root_local",
       "SBLR_DONOR_MYSQL_DDL_CREATE",
       "EngineDdlCreate",
       true, false, false, true, false, false, true, false, true},
      MysqlMarkers(), "mysql create table DDL transaction behavior");
  ok &= RunDdlTxnCase(
      ParseStatement, sbu_mysql_parse_to_sblr,
      "alter table mysql_ddl_txn_alter_marker add column name varchar(40)",
      {"mysql",
       "9.7.0",
       "019e13c0-0000-7000-8000-000000000303",
       "019e13c0-1900-7000-8000-000000000303",
       "mysql.ddl_transaction_behavior_semantics_profile",
       "ddl",
       "mysql.ddl.alter",
       "alter_table",
       "mysql_implicit_commit_ddl_descriptor_required",
       "implicit_commit_before_and_after_ddl_engine_policy",
       "post_implicit_commit_catalog_epoch",
       "mysql_ddl_not_rolled_back_by_user_transaction_descriptor",
       "mysql_atomic_ddl_dictionary_state_engine_authority",
       "mysql_ddl_transaction_behavior_diagnostic_map",
       "mysql_connected_database_root_uuid_required_temp_shadowing_root_local",
       "SBLR_DONOR_MYSQL_DDL_ALTER",
       "EngineDdlAlter",
       false, true, false, true, false, false, true, false, true},
      MysqlMarkers(), "mysql alter table DDL transaction behavior");
  return ok;
}

bool PostgresqlChecks() {
  using scratchbird::parser::postgresql::ParseStatement;
  using scratchbird::udr::postgresql_parser_support::
      sbu_postgresql_parse_to_sblr;
  bool ok = true;
  ok &= RunDdlTxnCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "create table pg_ddl_txn_create_marker (id integer primary key)",
      {"postgresql",
       "18.3",
       "019e13c0-0000-7000-8000-000000000304",
       "019e13c0-1900-7000-8000-000000000304",
       "postgresql.ddl_transaction_behavior_semantics_profile",
       "ddl",
       "postgresql.ddl.create",
       "create_table",
       "postgresql_transactional_ddl_descriptor_required",
       "none_parser_does_not_commit_engine_transaction",
       "transaction_local_until_engine_commit_then_catalog_epoch",
       "ddl_rollback_requires_engine_mga_transaction_rollback",
       "postgresql_catalog_invalid_state_descriptor_engine_authority",
       "postgresql_ddl_transaction_behavior_diagnostic_map",
       "postgresql_connected_database_schema_root_uuid_required_pg_temp_root_local",
       "SBLR_DONOR_POSTGRESQL_DDL_CREATE",
       "EngineDdlCreate",
       true, false, false, true, false, false, false, true, false},
      PostgresqlMarkers(), "postgresql create table DDL transaction behavior");
  ok &= RunDdlTxnCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "create index pg_ddl_txn_index_marker on pg_ddl_txn_create_marker (id)",
      {"postgresql",
       "18.3",
       "019e13c0-0000-7000-8000-000000000304",
       "019e13c0-1900-7000-8000-000000000304",
       "postgresql.ddl_transaction_behavior_semantics_profile",
       "ddl",
       "postgresql.ddl.create.index",
       "create_index",
       "postgresql_transactional_ddl_descriptor_required",
       "none_parser_does_not_commit_engine_transaction",
       "transaction_local_until_engine_commit_then_catalog_epoch",
       "ddl_rollback_requires_engine_mga_transaction_rollback",
       "postgresql_index_invalid_state_descriptor_engine_authority",
       "postgresql_ddl_transaction_behavior_diagnostic_map",
       "postgresql_connected_database_schema_root_uuid_required_pg_temp_root_local",
       "SBLR_DONOR_POSTGRESQL_INDEX_CREATE",
       "EngineDdlCreateIndex",
       true, false, false, false, true, false, false, true, false},
      PostgresqlMarkers(), "postgresql create index DDL transaction behavior");
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
