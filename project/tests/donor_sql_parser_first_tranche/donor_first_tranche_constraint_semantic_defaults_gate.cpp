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
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>

namespace {

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
                     std::initializer_list<std::string_view> markers,
                     std::string_view label) {
  bool ok = true;
  for (const auto marker : markers) {
    ok &= Expect(!Contains(payload, marker),
                 std::string(label) + " leaked SQL text marker: " +
                     std::string(marker));
  }
  return ok;
}

bool ExpectReadinessComplete(std::string_view payload, std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload, "\"enterprise_readiness_evidence\":{"),
               std::string(label) + " missing readiness evidence");
  ok &= ExpectField(payload, "completion_claim", "not_enterprise_ready", label);
  ok &= ExpectBool(payload, "enterprise_implemented_proven", false, label);
  ok &= ExpectField(payload, "semantic_defaults_status",
                    "semantic_profile_proof_pending", label);
  ok &= ExpectField(payload, "observable_equivalence_status",
                    "donor_native_equivalence_proof_pending", label);
  ok &= Expect(Contains(payload, "enterprise_implemented_proven\":false"),
               std::string(label) + " missing enterprise implementation proof");
  return ok;
}

bool ExpectEnvelopeAuthority(std::string_view payload,
                             std::string_view dialect,
                             std::string_view operation_family,
                             std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload, "\"envelope\":\"SBLRExecutionEnvelope.v3\""),
               std::string(label) + " missing SBLR envelope");
  ok &= ExpectField(payload, "dialect", dialect, label);
  ok &= ExpectField(payload, "operation_family", operation_family, label);
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required", label);
  ok &= ExpectField(payload, "engine_authority", "scratchbird", label);
  ok &= ExpectBool(payload, "donor_engine_sql_executed", false, label);
  ok &= ExpectBool(payload, "sql_text_included", false, label);
  ok &= ExpectReadinessComplete(payload, label);
  return ok;
}

bool ExpectConstraintEvidence(std::string_view payload,
                              std::string_view dialect,
                              std::string_view release_profile,
                              std::string_view donor_profile_uuid,
                              std::string_view semantic_profile_uuid,
                              std::string_view constraint_profile,
                              std::string_view primary_key_behavior,
                              std::string_view unique_null_policy,
                              std::string_view fk_action_defaults,
                              std::string_view check_null_behavior,
                              std::string_view default_expression_policy,
                              std::string_view generated_policy,
                              std::string_view generated_name_policy,
                              std::string_view deferrability_policy,
                              std::string_view enforcement_timing,
                              std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload, "\"constraint_semantic_defaults_evidence\":{"),
               std::string(label) + " missing constraint semantic evidence");
  ok &= ExpectField(payload, "evidence_contract",
                    "donor_constraint_semantic_defaults_evidence.v1", label);
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required", label);
  ok &= ExpectField(payload, "donor_profile_uuid", donor_profile_uuid, label);
  ok &= ExpectField(payload, "semantic_profile_uuid", semantic_profile_uuid, label);
  ok &= ExpectField(payload, "dialect", dialect, label);
  ok &= ExpectField(payload, "release_profile", release_profile, label);
  ok &= ExpectField(payload, "constraint_profile", constraint_profile, label);
  ok &= ExpectField(payload, "ddl_surface", "create_table", label);
  ok &= ExpectBool(payload, "primary_key_present", true, label);
  ok &= ExpectField(payload, "primary_key_behavior", primary_key_behavior, label);
  ok &= ExpectBool(payload, "unique_constraint_present", true, label);
  ok &= ExpectField(payload, "unique_null_policy", unique_null_policy, label);
  ok &= ExpectBool(payload, "foreign_key_reference_present", true, label);
  ok &= ExpectField(payload, "foreign_key_action_defaults",
                    fk_action_defaults, label);
  ok &= ExpectBool(payload, "check_constraint_present", true, label);
  ok &= ExpectField(payload, "check_truth_table_null_behavior",
                    check_null_behavior, label);
  ok &= ExpectBool(payload, "default_clause_present", true, label);
  ok &= ExpectField(payload, "default_expression_policy",
                    default_expression_policy, label);
  ok &= ExpectBool(payload, "generated_identity_or_autoincrement_present",
                   true, label);
  ok &= ExpectField(payload, "generated_identity_autoincrement_policy",
                    generated_policy, label);
  ok &= ExpectBool(payload, "explicit_constraint_names_present", false, label);
  ok &= ExpectField(payload, "generated_name_policy",
                    generated_name_policy, label);
  ok &= ExpectField(payload, "deferrability_policy",
                    deferrability_policy, label);
  ok &= ExpectField(payload, "enforcement_timing", enforcement_timing, label);
  ok &= ExpectBool(payload, "catalog_descriptor_required", true, label);
  ok &= ExpectBool(payload, "generic_constraint_default_allowed", false, label);
  ok &= ExpectBool(payload, "parser_storage_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_authority", false, label);
  ok &= ExpectBool(payload, "donor_sql_executed", false, label);
  ok &= ExpectField(payload, "runtime_semantic_equivalence",
                    "not_enterprise_proven_pending", label);
  ok &= ExpectField(payload, "descriptor_exactness_status",
                    "parser_constraint_defaults_recorded_runtime_equivalence_pending",
                    label);
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready", label);
  return ok;
}

template <typename Result, typename ProfileExpectation>
bool ExpectDirectConstraintResult(const Result& result,
                                  std::string_view dialect,
                                  std::string_view operation_family,
                                  std::initializer_list<std::string_view> markers,
                                  std::string_view label,
                                  ProfileExpectation expect_profile) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " did not parse");
  ok &= Expect(result.operation_family == operation_family,
               std::string(label) + " operation family mismatch: " +
                   result.operation_family);
  ok &= Expect(Contains(result.parser_evidence_json,
                        "\"statement_kind\":\"CREATE_TABLE\""),
               std::string(label) + " was not classified as CREATE TABLE DDL");
  ok &= ExpectField(result.parser_evidence_json, "dialect", dialect, label);
  ok &= ExpectBool(result.parser_evidence_json, "source_text_redacted", true, label);
  ok &= ExpectBool(result.parser_evidence_json, "descriptor_uuid_required", true, label);
  ok &= ExpectBool(result.parser_evidence_json,
                   "parser_transaction_finality_authority", false, label);
  ok &= ExpectBool(result.parser_evidence_json, "parser_storage_authority",
                   false, label);
  ok &= expect_profile(result.parser_evidence_json, label);
  ok &= ExpectEnvelopeAuthority(result.sblr_envelope, dialect,
                                operation_family, label);
  ok &= expect_profile(result.sblr_envelope, label);
  ok &= ExpectNoMarkers(result.parser_evidence_json, markers, label);
  ok &= ExpectNoMarkers(result.sblr_envelope, markers, label);
  return ok;
}

template <typename UdrResult, typename ProfileExpectation>
bool ExpectUdrConstraintResult(const UdrResult& result,
                               std::string_view dialect,
                               std::string_view operation_family,
                               std::initializer_list<std::string_view> markers,
                               std::string_view label,
                               ProfileExpectation expect_profile) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " UDR parse failed: " +
                             result.message_vector_json);
  ok &= ExpectEnvelopeAuthority(result.payload, dialect, operation_family, label);
  ok &= expect_profile(result.payload, label);
  ok &= ExpectNoMarkers(result.payload, markers, label);
  return ok;
}

auto FirebirdProfile() {
  return [](std::string_view payload, std::string_view label) {
    return ExpectConstraintEvidence(
        payload, "firebird", "5.0.4",
        "019e13c0-0000-7000-8000-000000000302",
        "019e13c0-1100-7000-8000-000000000302",
        "firebird.table_constraint_defaults_profile",
        "firebird_primary_key_not_null_unique_index_descriptor",
        "firebird_unique_constraint_nulls_are_distinct_profile",
        "firebird_foreign_key_default_no_action_update_no_action_delete_descriptor",
        "firebird_check_constraint_false_fails_unknown_passes_profile",
        "firebird_default_expression_descriptor_runtime_equivalence_pending",
        "firebird_generated_identity_sequence_backed_descriptor",
        "firebird_system_generated_constraint_names_rdb_descriptor_required",
        "firebird_constraints_not_deferrable_immediate_profile",
        "firebird_immediate_constraint_validation_profile", label);
  };
}

auto MysqlProfile() {
  return [](std::string_view payload, std::string_view label) {
    bool ok = ExpectConstraintEvidence(
        payload, "mysql", "9.7.0",
        "019e13c0-0000-7000-8000-000000000303",
        "019e13c0-1100-7000-8000-000000000303",
        "mysql.table_constraint_defaults_profile",
        "mysql_primary_key_not_null_unique_index_innodb_descriptor",
        "mysql_unique_constraint_allows_multiple_nulls_profile",
        "mysql_innodb_foreign_key_default_restrict_update_restrict_delete_descriptor",
        "mysql_check_constraint_false_fails_unknown_passes_profile",
        "mysql_default_literal_or_parenthesized_expression_descriptor",
        "mysql_auto_increment_column_attribute_descriptor",
        "mysql_engine_generated_constraint_names_descriptor_required",
        "mysql_constraints_not_deferrable_immediate_profile",
        "mysql_innodb_immediate_constraint_validation_profile", label);
    ok &= Expect(!Contains(payload, "\"dialect\":\"mysql_lts\"") &&
                     !Contains(payload, "mysql_lts.table_constraint"),
                 std::string(label) + " treated MySQL LTS as a parser donor");
    return ok;
  };
}

auto PostgresqlProfile() {
  return [](std::string_view payload, std::string_view label) {
    return ExpectConstraintEvidence(
        payload, "postgresql", "18.3",
        "019e13c0-0000-7000-8000-000000000304",
        "019e13c0-1100-7000-8000-000000000304",
        "postgresql.table_constraint_defaults_profile",
        "postgresql_primary_key_not_null_unique_btree_descriptor",
        "postgresql_unique_constraint_nulls_distinct_default",
        "postgresql_foreign_key_default_no_action_update_no_action_delete_descriptor",
        "postgresql_check_constraint_false_fails_unknown_passes_profile",
        "postgresql_variable_free_default_expression_descriptor",
        "postgresql_sql_identity_sequence_backed_descriptor",
        "postgresql_catalog_generated_constraint_names_descriptor_required",
        "postgresql_not_deferrable_initially_immediate_default_profile",
        "postgresql_immediate_constraint_validation_default_profile", label);
  };
}

bool FirebirdChecks() {
  using scratchbird::parser::firebird::ParseStatement;
  using scratchbird::udr::firebird_parser_support::sbu_firebird_parse_to_sblr;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  constexpr std::string_view ddl =
      "create table fb_constraints_child ("
      "fb_constraints_id integer generated by default as identity primary key,"
      "fb_parent_ref integer,"
      "fb_constraint_code varchar(20) unique,"
      "fb_constraint_amount integer default 1,"
      "check (fb_constraint_amount > 0),"
      "foreign key (fb_parent_ref) references fb_constraints_parent(fb_constraints_id))";
  const std::initializer_list<std::string_view> markers = {
      "fb_constraints_child", "fb_constraints_id", "fb_parent_ref",
      "fb_constraint_code", "fb_constraint_amount", "fb_constraints_parent"};
  bool ok = true;
  ok &= ExpectDirectConstraintResult(ParseStatement(ddl), "firebird",
                                     "firebird.ddl.create", markers,
                                     "firebird direct create table constraints",
                                     FirebirdProfile());
  ok &= ExpectUdrConstraintResult(sbu_firebird_parse_to_sblr(ddl, trusted),
                                  "firebird", "firebird.ddl.create", markers,
                                  "firebird UDR create table constraints",
                                  FirebirdProfile());
  return ok;
}

bool MysqlChecks() {
  using scratchbird::parser::mysql::ParseStatement;
  using scratchbird::udr::mysql_parser_support::sbu_mysql_parse_to_sblr;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  constexpr std::string_view ddl =
      "create table mysql_constraints_child ("
      "mysql_constraints_id int auto_increment primary key,"
      "mysql_parent_ref int,"
      "mysql_constraint_code varchar(20) unique,"
      "mysql_constraint_amount int default 1,"
      "check (mysql_constraint_amount > 0),"
      "foreign key (mysql_parent_ref) references mysql_constraints_parent(mysql_constraints_id))";
  const std::initializer_list<std::string_view> markers = {
      "mysql_constraints_child", "mysql_constraints_id", "mysql_parent_ref",
      "mysql_constraint_code", "mysql_constraint_amount",
      "mysql_constraints_parent"};
  bool ok = true;
  ok &= ExpectDirectConstraintResult(ParseStatement(ddl), "mysql",
                                     "mysql.ddl.create", markers,
                                     "mysql direct create table constraints",
                                     MysqlProfile());
  ok &= ExpectUdrConstraintResult(sbu_mysql_parse_to_sblr(ddl, trusted),
                                  "mysql", "mysql.ddl.create", markers,
                                  "mysql UDR create table constraints",
                                  MysqlProfile());
  return ok;
}

bool PostgresqlChecks() {
  using scratchbird::parser::postgresql::ParseStatement;
  using scratchbird::udr::postgresql_parser_support::sbu_postgresql_parse_to_sblr;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  constexpr std::string_view ddl =
      "create table pg_constraints_child ("
      "pg_constraints_id integer generated by default as identity primary key,"
      "pg_parent_ref integer,"
      "pg_constraint_code text unique,"
      "pg_constraint_amount integer default 1,"
      "check (pg_constraint_amount > 0),"
      "foreign key (pg_parent_ref) references pg_constraints_parent(pg_constraints_id))";
  const std::initializer_list<std::string_view> markers = {
      "pg_constraints_child", "pg_constraints_id", "pg_parent_ref",
      "pg_constraint_code", "pg_constraint_amount", "pg_constraints_parent"};
  bool ok = true;
  ok &= ExpectDirectConstraintResult(ParseStatement(ddl), "postgresql",
                                     "postgresql.ddl.create", markers,
                                     "postgresql direct create table constraints",
                                     PostgresqlProfile());
  ok &= ExpectUdrConstraintResult(sbu_postgresql_parse_to_sblr(ddl, trusted),
                                  "postgresql", "postgresql.ddl.create", markers,
                                  "postgresql UDR create table constraints",
                                  PostgresqlProfile());

  constexpr std::string_view not_deferrable_ddl =
      "create table pg_constraints_not_deferrable ("
      "id integer primary key,"
      "parent_id integer references pg_constraints_parent(id) not deferrable)";
  const auto not_deferrable = ParseStatement(not_deferrable_ddl);
  ok &= Expect(not_deferrable.ok,
               "postgresql explicit not deferrable DDL did not parse");
  ok &= ExpectField(not_deferrable.sblr_envelope, "deferrability_policy",
                    "postgresql_not_deferrable_initially_immediate_default_profile",
                    "postgresql explicit not deferrable");
  ok &= ExpectField(not_deferrable.sblr_envelope, "enforcement_timing",
                    "postgresql_immediate_constraint_validation_default_profile",
                    "postgresql explicit not deferrable");

  constexpr std::string_view deferrable_ddl =
      "create table pg_constraints_deferrable ("
      "id integer primary key,"
      "parent_id integer references pg_constraints_parent(id) "
      "deferrable initially deferred)";
  const auto deferrable = ParseStatement(deferrable_ddl);
  ok &= Expect(deferrable.ok,
               "postgresql explicit deferrable DDL did not parse");
  ok &= ExpectField(deferrable.sblr_envelope, "deferrability_policy",
                    "postgresql_deferrability_requested_descriptor",
                    "postgresql explicit deferrable");
  ok &= ExpectField(deferrable.sblr_envelope, "enforcement_timing",
                    "postgresql_constraint_timing_descriptor_requested_runtime_proven",
                    "postgresql explicit deferrable");
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
