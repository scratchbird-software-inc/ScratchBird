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
                 std::string(label) + " leaked SQL/object name marker: " +
                     std::string(marker));
  }
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
  ok &= ExpectField(payload, "completion_claim", "not_enterprise_ready", label);
  ok &= Expect(Contains(payload, "enterprise_implemented_proven\":false"),
               std::string(label) + " missing enterprise implementation proof");
  return ok;
}

bool ExpectIdentifierEvidence(std::string_view payload,
                              std::string_view dialect,
                              std::string_view release_profile,
                              std::string_view donor_profile_uuid,
                              std::string_view semantic_profile_uuid,
                              std::string_view name_resolution_profile,
                              std::string_view unquoted_policy,
                              std::string_view quoted_policy,
                              std::string_view schema_root_policy,
                              std::string_view generated_name_behavior,
                              std::string_view namespace_collision_behavior,
                              std::string_view result_label_policy,
                              std::string_view table_case_policy,
                              bool release_profile_variant_bound_to_base_donor,
                              std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload, "\"identifier_name_resolution_evidence\":{"),
               std::string(label) + " missing identifier evidence");
  ok &= ExpectField(payload, "evidence_contract",
                    "donor_identifier_name_resolution_evidence.v1", label);
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required", label);
  ok &= ExpectField(payload, "donor_profile_uuid", donor_profile_uuid, label);
  ok &= ExpectField(payload, "semantic_profile_uuid", semantic_profile_uuid, label);
  ok &= ExpectField(payload, "dialect", dialect, label);
  ok &= ExpectField(payload, "release_profile", release_profile, label);
  ok &= ExpectField(payload, "name_resolution_profile",
                    name_resolution_profile, label);
  ok &= ExpectField(payload, "unquoted_identifier_policy",
                    unquoted_policy, label);
  ok &= ExpectField(payload, "quoted_identifier_policy",
                    quoted_policy, label);
  ok &= ExpectField(payload, "schema_root_resolution_policy",
                    schema_root_policy, label);
  ok &= ExpectField(payload, "generated_catalog_name_behavior",
                    generated_name_behavior, label);
  ok &= ExpectField(payload, "namespace_collision_behavior",
                    namespace_collision_behavior, label);
  ok &= ExpectField(payload, "result_metadata_label_policy",
                    result_label_policy, label);
  ok &= ExpectField(payload, "table_name_filesystem_case_policy",
                    table_case_policy, label);
  ok &= ExpectBool(payload, "release_profile_variant_bound_to_base_donor",
                   release_profile_variant_bound_to_base_donor, label);
  ok &= ExpectBool(payload, "create_surface", true, label);
  ok &= ExpectBool(payload, "quoted_identifier_syntax_observed", true, label);
  ok &= ExpectBool(payload, "qualified_name_syntax_observed", true, label);
  ok &= ExpectBool(payload, "uuid_descriptor_resolution_required", true, label);
  ok &= ExpectBool(payload, "catalog_descriptor_required", true, label);
  ok &= ExpectBool(payload, "source_sql_text_included", false, label);
  ok &= ExpectBool(payload, "original_sql_identifier_text_included", false, label);
  ok &= ExpectBool(payload, "object_name_text_included", false, label);
  ok &= ExpectBool(payload, "sblr_embeds_source_identifiers", false, label);
  ok &= ExpectBool(payload, "cross_root_authority", false, label);
  ok &= ExpectField(payload, "cross_root_resolution_policy",
                    "explicit_no_cross_root_authority_uuid_root_required", label);
  ok &= ExpectBool(payload, "parser_storage_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_authority", false, label);
  ok &= ExpectBool(payload, "donor_sql_executed", false, label);
  ok &= ExpectField(payload, "runtime_semantic_equivalence",
                    "not_enterprise_proven_pending", label);
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready", label);
  return ok;
}

template <typename Result, typename ProfileExpectation>
bool ExpectDirectResult(const Result& result,
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
bool ExpectUdrResult(const UdrResult& result,
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
    return ExpectIdentifierEvidence(
        payload, "firebird", "5.0.4",
        "019e13c0-0000-7000-8000-000000000302",
        "019e13c0-1200-7000-8000-000000000302",
        "firebird.identifier_name_resolution_profile",
        "firebird_unquoted_identifiers_fold_to_uppercase",
        "firebird_double_quoted_identifiers_preserve_exact_case",
        "firebird_single_database_root_uuid_catalog_resolution_required",
        "firebird_rdb_generated_names_catalog_descriptor_required",
        "firebird_catalog_namespace_collision_resolved_by_uuid_descriptor",
        "firebird_result_labels_follow_identifier_fold_alias_descriptor",
        "not_filesystem_sensitive_table_name_policy", false, label);
  };
}

auto MysqlProfile() {
  return [](std::string_view payload, std::string_view label) {
    bool ok = ExpectIdentifierEvidence(
        payload, "mysql", "9.7.0",
        "019e13c0-0000-7000-8000-000000000303",
        "019e13c0-1200-7000-8000-000000000303",
        "mysql.identifier_name_resolution_profile",
        "mysql_unquoted_identifiers_preserve_spelling_table_name_case_bound_by_lower_case_table_names",
        "mysql_quoted_identifiers_preserve_exact_case_backtick_default_ansi_quotes_profile_bound",
        "mysql_database_schema_root_uuid_resolution_required_no_filesystem_authority",
        "mysql_engine_generated_names_descriptor_required_lower_case_table_names_bound",
        "mysql_schema_table_namespace_collision_resolved_by_uuid_descriptor_and_lctn_profile",
        "mysql_result_labels_preserve_alias_spelling_descriptor",
        "mysql_lower_case_table_names_filesystem_sensitive_bound_descriptor",
        true, label);
    ok &= Expect(!Contains(payload, "\"dialect\":\"mysql_lts\"") &&
                     !Contains(payload, "mysql_lts"),
                 std::string(label) + " treated MySQL LTS as a parser donor");
    return ok;
  };
}

auto PostgresqlProfile() {
  return [](std::string_view payload, std::string_view label) {
    return ExpectIdentifierEvidence(
        payload, "postgresql", "18.3",
        "019e13c0-0000-7000-8000-000000000304",
        "019e13c0-1200-7000-8000-000000000304",
        "postgresql.identifier_name_resolution_profile",
        "postgresql_unquoted_identifiers_fold_to_lowercase",
        "postgresql_double_quoted_identifiers_preserve_exact_case",
        "postgresql_database_schema_search_path_uuid_resolution_required",
        "postgresql_catalog_generated_names_descriptor_required",
        "postgresql_schema_namespace_collision_resolved_by_uuid_descriptor_and_search_path",
        "postgresql_result_labels_follow_lowercase_fold_alias_descriptor",
        "not_filesystem_sensitive_table_name_policy", false, label);
  };
}

bool FirebirdChecks() {
  using scratchbird::parser::firebird::ParseStatement;
  using scratchbird::udr::firebird_parser_support::sbu_firebird_parse_to_sblr;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  constexpr std::string_view ddl =
      "create table FbRoot.\"ExactFireName\" ("
      "PlainCol integer, \"ExactFireColumn\" varchar(20))";
  const std::initializer_list<std::string_view> markers = {
      "FbRoot", "FBROOT", "ExactFireName", "EXACTFIRENAME", "PlainCol",
      "PLAINCOL", "ExactFireColumn", "EXACTFIRECOLUMN"};
  bool ok = true;
  ok &= ExpectDirectResult(ParseStatement(ddl), "firebird",
                           "firebird.ddl.create", markers,
                           "firebird direct create table identifiers",
                           FirebirdProfile());
  ok &= ExpectUdrResult(sbu_firebird_parse_to_sblr(ddl, trusted), "firebird",
                        "firebird.ddl.create", markers,
                        "firebird UDR create table identifiers",
                        FirebirdProfile());
  return ok;
}

bool MysqlChecks() {
  using scratchbird::parser::mysql::ParseStatement;
  using scratchbird::udr::mysql_parser_support::sbu_mysql_parse_to_sblr;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  constexpr std::string_view ddl =
      "create table MixDb.`ExactMysqlName` ("
      "PlainCol int, `ExactMysqlColumn` varchar(20))";
  const std::initializer_list<std::string_view> markers = {
      "MixDb", "MIXDB", "ExactMysqlName", "EXACTMYSQLNAME", "PlainCol",
      "PLAINCOL", "ExactMysqlColumn", "EXACTMYSQLCOLUMN"};
  bool ok = true;
  ok &= ExpectDirectResult(ParseStatement(ddl), "mysql",
                           "mysql.ddl.create", markers,
                           "mysql direct create table identifiers",
                           MysqlProfile());
  ok &= ExpectUdrResult(sbu_mysql_parse_to_sblr(ddl, trusted), "mysql",
                        "mysql.ddl.create", markers,
                        "mysql UDR create table identifiers",
                        MysqlProfile());
  return ok;
}

bool PostgresqlChecks() {
  using scratchbird::parser::postgresql::ParseStatement;
  using scratchbird::udr::postgresql_parser_support::sbu_postgresql_parse_to_sblr;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  constexpr std::string_view ddl =
      "create table PgRoot.\"ExactPgName\" ("
      "PlainCol integer, \"ExactPgColumn\" text)";
  const std::initializer_list<std::string_view> markers = {
      "PgRoot", "PGROOT", "ExactPgName", "EXACTPGNAME", "PlainCol",
      "PLAINCOL", "ExactPgColumn", "EXACTPGCOLUMN"};
  bool ok = true;
  ok &= ExpectDirectResult(ParseStatement(ddl), "postgresql",
                           "postgresql.ddl.create", markers,
                           "postgresql direct create table identifiers",
                           PostgresqlProfile());
  ok &= ExpectUdrResult(sbu_postgresql_parse_to_sblr(ddl, trusted),
                        "postgresql", "postgresql.ddl.create", markers,
                        "postgresql UDR create table identifiers",
                        PostgresqlProfile());
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
