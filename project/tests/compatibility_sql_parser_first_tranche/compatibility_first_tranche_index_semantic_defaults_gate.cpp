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
                    "compatibility_native_equivalence_proof_pending", label);
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
  ok &= ExpectBool(payload, "reference_engine_sql_executed", false, label);
  ok &= ExpectBool(payload, "sql_text_included", false, label);
  ok &= ExpectReadinessComplete(payload, label);
  return ok;
}

bool ExpectIndexEvidence(std::string_view payload,
                         std::string_view dialect,
                         std::string_view release_profile,
                         std::string_view compatibility_profile_uuid,
                         std::string_view semantic_profile_uuid,
                         std::string_view ddl_surface,
                         std::string_view index_method,
                         bool unique_requested,
                         std::string_view unique_null_policy,
                         std::string_view null_ordering,
                         std::string_view collation_policy,
                         std::string_view operator_family_policy,
                         std::string_view predicate_or_expression_policy,
                         std::string_view validation_state,
                         std::string_view build_mode,
                         std::string_view statistics_policy_ref,
                         std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload, "\"index_semantic_defaults_evidence\":{"),
               std::string(label) + " missing index semantic evidence");
  ok &= ExpectField(payload, "evidence_contract",
                    "compatibility_index_semantic_defaults_evidence.v1", label);
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required", label);
  ok &= ExpectField(payload, "compatibility_profile_uuid", compatibility_profile_uuid, label);
  ok &= ExpectField(payload, "semantic_profile_uuid", semantic_profile_uuid, label);
  ok &= ExpectField(payload, "dialect", dialect, label);
  ok &= ExpectField(payload, "release_profile", release_profile, label);
  ok &= ExpectField(payload, "ddl_surface", ddl_surface, label);
  ok &= ExpectField(payload, "index_method", index_method, label);
  ok &= ExpectBool(payload, "unique_requested", unique_requested, label);
  ok &= ExpectField(payload, "unique_null_policy", unique_null_policy, label);
  ok &= ExpectField(payload, "null_ordering", null_ordering, label);
  ok &= ExpectField(payload, "collation_policy", collation_policy, label);
  ok &= ExpectField(payload, "operator_family_policy",
                    operator_family_policy, label);
  ok &= ExpectField(payload, "predicate_or_expression_policy",
                    predicate_or_expression_policy, label);
  ok &= ExpectField(payload, "validation_state", validation_state, label);
  ok &= ExpectField(payload, "build_mode", build_mode, label);
  ok &= ExpectField(payload, "statistics_policy_ref",
                    statistics_policy_ref, label);
  ok &= ExpectBool(payload, "catalog_descriptor_required", true, label);
  ok &= ExpectBool(payload, "generic_index_default_allowed", false, label);
  ok &= ExpectBool(payload, "parser_storage_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_authority", false, label);
  ok &= ExpectBool(payload, "compatibility_sql_executed", false, label);
  ok &= ExpectField(payload, "runtime_semantic_equivalence",
                    "not_enterprise_proven_pending", label);
  ok &= ExpectField(payload, "descriptor_exactness_status",
                    "parser_descriptor_defaults_recorded_runtime_equivalence_pending",
                    label);
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready", label);
  return ok;
}

template <typename Result, typename ProfileExpectation>
bool ExpectDirectIndexResult(const Result& result,
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
  ok &= Expect(Contains(result.parser_evidence_json, "\"statement_kind\":\"CREATE_INDEX\"") ||
                   Contains(result.parser_evidence_json, "\"statement_kind\":\"ALTER_INDEX\""),
               std::string(label) + " was not classified as index DDL");
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
bool ExpectUdrIndexResult(const UdrResult& result,
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

auto FirebirdCreateProfile(bool unique) {
  return [unique](std::string_view payload, std::string_view label) {
    return ExpectIndexEvidence(
        payload, "firebird", "5.0.4",
        "019e13c0-0000-7000-8000-000000000302",
        "019e13c0-1000-7000-8000-000000000302",
        unique ? "create_unique_index" : "create_index",
        "firebird_btree_ascending_index_profile", unique,
        unique ? "firebird_unique_index_nulls_are_distinct_profile"
               : "not_unique_index_not_applicable",
        "firebird_nulls_first_for_ascending_index_profile",
        "firebird_column_charset_collation_descriptor",
        "firebird_builtin_comparison_no_named_operator_family",
        "firebird_column_index_no_partial_predicate",
        "firebird_index_active_default",
        "firebird_immediate_index_build_default",
        "firebird_index_selectivity_statistics_profile", label);
  };
}

auto MysqlCreateProfile(bool unique) {
  return [unique](std::string_view payload, std::string_view label) {
    bool ok = ExpectIndexEvidence(
        payload, "mysql", "9.7.0",
        "019e13c0-0000-7000-8000-000000000303",
        "019e13c0-1000-7000-8000-000000000303",
        unique ? "create_unique_index" : "create_index",
        "mysql_innodb_btree_index_profile", unique,
        unique ? "mysql_innodb_unique_index_allows_multiple_nulls_profile"
               : "not_unique_index_not_applicable",
        "mysql_nulls_low_ascending_index_profile",
        "mysql_character_set_collation_weight_string_descriptor",
        "mysql_builtin_comparison_no_named_operator_family",
        "mysql_column_index_no_partial_predicate",
        "mysql_index_visible_default",
        "mysql_engine_selected_online_ddl_default",
        "mysql_innodb_persistent_index_statistics_profile", label);
    ok &= Expect(!Contains(payload, "\"dialect\":\"mysql_lts\"") &&
                     !Contains(payload, "mysql_lts.index_optimizer"),
                 std::string(label) + " treated MySQL LTS as a parser compatibility");
    return ok;
  };
}

auto PostgresqlCreateProfile(bool unique) {
  return [unique](std::string_view payload, std::string_view label) {
    return ExpectIndexEvidence(
        payload, "postgresql", "18.3",
        "019e13c0-0000-7000-8000-000000000304",
        "019e13c0-1000-7000-8000-000000000304",
        unique ? "create_unique_index" : "create_index",
        "postgresql_btree_access_method_default", unique,
        unique ? "postgresql_unique_nulls_distinct_default"
               : "not_unique_index_not_applicable",
        "postgresql_nulls_last_for_ascending_btree_default",
        "postgresql_per_expression_collation_descriptor",
        "postgresql_default_operator_class_and_family_resolution",
        "postgresql_column_index_no_predicate_descriptor",
        "postgresql_index_valid_after_build_default",
        "postgresql_nonconcurrent_index_build_default",
        "postgresql_pg_statistic_and_pg_class_index_statistics_profile", label);
  };
}

bool FirebirdChecks() {
  using scratchbird::parser::firebird::ParseStatement;
  using scratchbird::udr::firebird_parser_support::sbu_firebird_parse_to_sblr;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  bool ok = true;
  ok &= ExpectDirectIndexResult(
      ParseStatement("create index fb_sem_idx on fb_sem_table (fb_sem_col)"),
      "firebird", "firebird.ddl.create.index",
      {"fb_sem_idx", "fb_sem_table", "fb_sem_col"}, "firebird direct create index",
      FirebirdCreateProfile(false));
  ok &= ExpectDirectIndexResult(
      ParseStatement("create unique index fb_sem_unique_idx on fb_sem_unique_table (fb_sem_unique_col)"),
      "firebird", "firebird.ddl.create.unique_index",
      {"fb_sem_unique_idx", "fb_sem_unique_table", "fb_sem_unique_col"},
      "firebird direct create unique index", FirebirdCreateProfile(true));
  ok &= ExpectUdrIndexResult(
      sbu_firebird_parse_to_sblr(
          "create index fb_udr_sem_idx on fb_udr_sem_table (fb_udr_sem_col)",
          trusted),
      "firebird", "firebird.ddl.create.index",
      {"fb_udr_sem_idx", "fb_udr_sem_table", "fb_udr_sem_col"},
      "firebird UDR create index", FirebirdCreateProfile(false));
  ok &= ExpectUdrIndexResult(
      sbu_firebird_parse_to_sblr(
          "create unique index fb_udr_sem_unique_idx on fb_udr_sem_unique_table (fb_udr_sem_unique_col)",
          trusted),
      "firebird", "firebird.ddl.create.unique_index",
      {"fb_udr_sem_unique_idx", "fb_udr_sem_unique_table",
       "fb_udr_sem_unique_col"},
      "firebird UDR create unique index", FirebirdCreateProfile(true));
  return ok;
}

bool MysqlChecks() {
  using scratchbird::parser::mysql::ParseStatement;
  using scratchbird::udr::mysql_parser_support::sbu_mysql_parse_to_sblr;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  bool ok = true;
  ok &= ExpectDirectIndexResult(
      ParseStatement("create index mysql_sem_idx on mysql_sem_table (mysql_sem_col)"),
      "mysql", "mysql.ddl.create.index",
      {"mysql_sem_idx", "mysql_sem_table", "mysql_sem_col"},
      "mysql direct create index", MysqlCreateProfile(false));
  ok &= ExpectDirectIndexResult(
      ParseStatement("create unique index mysql_sem_unique_idx on mysql_sem_unique_table (mysql_sem_unique_col)"),
      "mysql", "mysql.ddl.create.unique_index",
      {"mysql_sem_unique_idx", "mysql_sem_unique_table", "mysql_sem_unique_col"},
      "mysql direct create unique index", MysqlCreateProfile(true));
  ok &= ExpectUdrIndexResult(
      sbu_mysql_parse_to_sblr(
          "create index mysql_udr_sem_idx on mysql_udr_sem_table (mysql_udr_sem_col)",
          trusted),
      "mysql", "mysql.ddl.create.index",
      {"mysql_udr_sem_idx", "mysql_udr_sem_table", "mysql_udr_sem_col"},
      "mysql UDR create index", MysqlCreateProfile(false));
  ok &= ExpectUdrIndexResult(
      sbu_mysql_parse_to_sblr(
          "create unique index mysql_udr_sem_unique_idx on mysql_udr_sem_unique_table (mysql_udr_sem_unique_col)",
          trusted),
      "mysql", "mysql.ddl.create.unique_index",
      {"mysql_udr_sem_unique_idx", "mysql_udr_sem_unique_table",
       "mysql_udr_sem_unique_col"},
      "mysql UDR create unique index", MysqlCreateProfile(true));
  return ok;
}

bool PostgresqlChecks() {
  using scratchbird::parser::postgresql::ParseStatement;
  using scratchbird::udr::postgresql_parser_support::sbu_postgresql_parse_to_sblr;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  bool ok = true;
  ok &= ExpectDirectIndexResult(
      ParseStatement("create index pg_sem_idx on pg_sem_table (pg_sem_col)"),
      "postgresql", "postgresql.ddl.create.index",
      {"pg_sem_idx", "pg_sem_table", "pg_sem_col"},
      "postgresql direct create index", PostgresqlCreateProfile(false));
  ok &= ExpectDirectIndexResult(
      ParseStatement("create unique index pg_sem_unique_idx on pg_sem_unique_table (pg_sem_unique_col)"),
      "postgresql", "postgresql.ddl.create.unique_index",
      {"pg_sem_unique_idx", "pg_sem_unique_table", "pg_sem_unique_col"},
      "postgresql direct create unique index", PostgresqlCreateProfile(true));
  ok &= ExpectUdrIndexResult(
      sbu_postgresql_parse_to_sblr(
          "create index pg_udr_sem_idx on pg_udr_sem_table (pg_udr_sem_col)",
          trusted),
      "postgresql", "postgresql.ddl.create.index",
      {"pg_udr_sem_idx", "pg_udr_sem_table", "pg_udr_sem_col"},
      "postgresql UDR create index", PostgresqlCreateProfile(false));
  ok &= ExpectUdrIndexResult(
      sbu_postgresql_parse_to_sblr(
          "create unique index pg_udr_sem_unique_idx on pg_udr_sem_unique_table (pg_udr_sem_unique_col)",
          trusted),
      "postgresql", "postgresql.ddl.create.unique_index",
      {"pg_udr_sem_unique_idx", "pg_udr_sem_unique_table",
       "pg_udr_sem_unique_col"},
      "postgresql UDR create unique index", PostgresqlCreateProfile(true));
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
