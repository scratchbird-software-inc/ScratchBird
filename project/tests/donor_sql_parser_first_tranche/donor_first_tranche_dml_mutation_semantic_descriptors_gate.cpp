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
#include <span>
#include <string>
#include <string_view>

namespace {

struct DmlExpected {
  std::string_view dialect;
  std::string_view release_profile;
  std::string_view donor_profile_uuid;
  std::string_view semantic_profile_uuid;
  std::string_view mutation_profile;
  std::string_view operation_family;
  std::string_view mutation_surface;
  std::string_view upsert_merge_conflict_policy;
  std::string_view returning_output_projection_policy;
  std::string_view cursor_positioned_dml_policy;
  std::string_view affected_row_count_policy;
  std::string_view trigger_default_generated_column_interaction_policy;
  bool insert_surface{false};
  bool update_surface{false};
  bool delete_surface{false};
  bool update_or_insert_surface{false};
  bool replace_surface{false};
  bool merge_surface{false};
  bool matching_surface{false};
  bool on_duplicate_key_update_surface{false};
  bool on_conflict_surface{false};
  bool on_conflict_do_update_surface{false};
  bool on_conflict_do_nothing_surface{false};
  bool returning_output_projection_surface{false};
  bool cursor_positioned_dml_surface{false};
  bool default_value_surface{false};
  bool generated_column_surface{false};
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
                 std::string(label) + " leaked DML source marker: " +
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
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready", label);
  ok &= Expect(Contains(payload, "enterprise_implemented_proven\":false"),
               std::string(label) + " missing enterprise implementation proof");
  return ok;
}

bool ExpectEnvelopeAuthority(std::string_view payload,
                             const DmlExpected& expected,
                             std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload, "\"envelope\":\"SBLRExecutionEnvelope.v3\""),
               std::string(label) + " missing SBLR envelope");
  ok &= ExpectField(payload, "dialect", expected.dialect, label);
  ok &= ExpectField(payload, "operation_family", expected.operation_family, label);
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required", label);
  ok &= ExpectField(payload, "engine_authority", "scratchbird", label);
  ok &= ExpectBool(payload, "donor_engine_sql_executed", false, label);
  ok &= ExpectBool(payload, "sql_text_included", false, label);
  ok &= ExpectReadinessComplete(payload, label);
  return ok;
}

bool ExpectDmlEvidence(std::string_view payload,
                       const DmlExpected& expected,
                       std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload, "\"dml_mutation_semantic_evidence\":{"),
               std::string(label) + " missing DML mutation evidence");
  ok &= ExpectField(payload, "evidence_contract",
                    "donor_dml_mutation_semantic_descriptor_evidence.v1",
                    label);
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required", label);
  ok &= ExpectField(payload, "donor_profile_uuid",
                    expected.donor_profile_uuid, label);
  ok &= ExpectField(payload, "semantic_profile_uuid",
                    expected.semantic_profile_uuid, label);
  ok &= ExpectField(payload, "dialect", expected.dialect, label);
  ok &= ExpectField(payload, "release_profile", expected.release_profile, label);
  ok &= ExpectField(payload, "mutation_profile",
                    expected.mutation_profile, label);
  ok &= ExpectField(payload, "mutation_surface",
                    expected.mutation_surface, label);
  ok &= ExpectBool(payload, "insert_surface",
                   expected.insert_surface, label);
  ok &= ExpectBool(payload, "update_surface",
                   expected.update_surface, label);
  ok &= ExpectBool(payload, "delete_surface",
                   expected.delete_surface, label);
  ok &= ExpectBool(payload, "update_or_insert_surface",
                   expected.update_or_insert_surface, label);
  ok &= ExpectBool(payload, "replace_surface",
                   expected.replace_surface, label);
  ok &= ExpectBool(payload, "merge_surface",
                   expected.merge_surface, label);
  ok &= ExpectBool(payload, "matching_surface",
                   expected.matching_surface, label);
  ok &= ExpectBool(payload, "on_duplicate_key_update_surface",
                   expected.on_duplicate_key_update_surface, label);
  ok &= ExpectBool(payload, "on_conflict_surface",
                   expected.on_conflict_surface, label);
  ok &= ExpectBool(payload, "on_conflict_do_update_surface",
                   expected.on_conflict_do_update_surface, label);
  ok &= ExpectBool(payload, "on_conflict_do_nothing_surface",
                   expected.on_conflict_do_nothing_surface, label);
  ok &= ExpectField(payload, "upsert_merge_conflict_policy",
                    expected.upsert_merge_conflict_policy, label);
  ok &= ExpectBool(payload, "returning_output_projection_surface",
                   expected.returning_output_projection_surface, label);
  ok &= ExpectField(payload, "returning_output_projection_policy",
                    expected.returning_output_projection_policy, label);
  ok &= ExpectBool(payload, "cursor_positioned_dml_surface",
                   expected.cursor_positioned_dml_surface, label);
  ok &= ExpectField(payload, "cursor_positioned_dml_policy",
                    expected.cursor_positioned_dml_policy, label);
  ok &= ExpectField(payload, "affected_row_count_policy",
                    expected.affected_row_count_policy, label);
  ok &= ExpectBool(payload, "default_value_surface",
                   expected.default_value_surface, label);
  ok &= ExpectBool(payload, "generated_column_surface",
                   expected.generated_column_surface, label);
  ok &= ExpectBool(payload, "trigger_interaction_descriptor_required",
                   true, label);
  ok &= ExpectField(
      payload, "trigger_default_generated_column_interaction_policy",
      expected.trigger_default_generated_column_interaction_policy, label);
  ok &= ExpectBool(payload, "uuid_required_semantic_profile", true, label);
  ok &= ExpectBool(payload, "catalog_descriptor_required", true, label);
  ok &= ExpectBool(payload, "sblr_operation_uuid_resolution_required", true, label);
  ok &= ExpectField(payload, "engine_authority", "scratchbird", label);
  ok &= ExpectBool(payload, "source_sql_text_included", false, label);
  ok &= ExpectBool(payload, "literal_text_included", false, label);
  ok &= ExpectBool(payload, "object_name_text_included", false, label);
  ok &= ExpectBool(payload, "quoted_identifier_text_included", false, label);
  ok &= ExpectBool(payload, "sblr_embeds_source_identifiers", false, label);
  ok &= ExpectBool(payload, "parser_storage_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_finality_authority", false, label);
  ok &= ExpectBool(payload, "parser_visibility_authority", false, label);
  ok &= ExpectBool(payload, "parser_row_count_authority", false, label);
  ok &= ExpectBool(payload, "parser_trigger_order_authority", false, label);
  ok &= ExpectBool(payload, "parser_default_value_authority", false, label);
  ok &= ExpectBool(payload, "parser_generated_column_authority", false, label);
  ok &= ExpectBool(payload, "donor_sql_executed", false, label);
  ok &= ExpectField(payload, "runtime_semantic_equivalence",
                    "not_enterprise_proven_pending", label);
  ok &= ExpectField(
      payload, "descriptor_exactness_status",
      "parser_dml_mutation_descriptor_recorded_runtime_equivalence_pending",
      label);
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready", label);
  ok &= Expect(!Contains(payload, "parser_storage_authority\":true"),
               std::string(label) + " granted parser storage authority");
  ok &= Expect(!Contains(payload, "parser_transaction_authority\":true"),
               std::string(label) + " granted parser transaction authority");
  ok &= Expect(!Contains(payload, "parser_transaction_finality_authority\":true"),
               std::string(label) + " granted parser transaction finality authority");
  ok &= Expect(!Contains(payload, "donor_sql_executed\":true"),
               std::string(label) + " executed donor SQL");
  return ok;
}

template <typename Result>
bool ExpectDirectDmlResult(const Result& result,
                           const DmlExpected& expected,
                           std::span<const std::string_view> markers,
                           std::string_view label) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " did not parse: " +
                             result.message_vector_json);
  ok &= Expect(result.statement_family == "dml",
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
  ok &= ExpectDmlEvidence(result.parser_evidence_json, expected, label);
  ok &= ExpectEnvelopeAuthority(result.sblr_envelope, expected, label);
  ok &= ExpectDmlEvidence(result.sblr_envelope, expected, label);
  ok &= ExpectNoMarkers(result.parser_evidence_json, markers, label);
  ok &= ExpectNoMarkers(result.sblr_envelope, markers, label);
  ok &= ExpectNoMysqlLts(result.parser_evidence_json, label);
  ok &= ExpectNoMysqlLts(result.sblr_envelope, label);
  return ok;
}

template <typename UdrResult>
bool ExpectUdrDmlResult(const UdrResult& result,
                        const DmlExpected& expected,
                        std::span<const std::string_view> markers,
                        std::string_view label) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " UDR parse failed: " +
                             result.message_vector_json);
  ok &= ExpectEnvelopeAuthority(result.payload, expected, label);
  ok &= ExpectDmlEvidence(result.payload, expected, label);
  ok &= ExpectNoMarkers(result.payload, markers, label);
  ok &= ExpectNoMysqlLts(result.payload, label);
  return ok;
}

template <typename DirectParser, typename UdrParser>
bool RunDmlCase(DirectParser direct_parser,
                UdrParser udr_parser,
                std::string_view sql,
                const DmlExpected& expected,
                std::span<const std::string_view> markers,
                std::string_view label) {
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  bool ok = true;
  ok &= ExpectDirectDmlResult(direct_parser(sql), expected, markers,
                              std::string(label) + " direct");
  ok &= ExpectUdrDmlResult(udr_parser(sql, trusted), expected, markers,
                           std::string(label) + " UDR");
  return ok;
}

std::span<const std::string_view> FirebirdMarkers() {
  static constexpr std::string_view markers[] = {
      "fb_dml_secret_marker", "FB_DML_SECRET_MARKER",
      "fb_dml_literal_marker", "FB_DML_LITERAL_MARKER",
      "fb_cursor_secret_marker", "FB_CURSOR_SECRET_MARKER"};
  return markers;
}

std::span<const std::string_view> MysqlMarkers() {
  static constexpr std::string_view markers[] = {
      "mysql_dml_secret_marker", "MYSQL_DML_SECRET_MARKER",
      "mysql_dml_literal_marker", "MYSQL_DML_LITERAL_MARKER"};
  return markers;
}

std::span<const std::string_view> PostgresqlMarkers() {
  static constexpr std::string_view markers[] = {
      "pg_dml_secret_marker", "PG_DML_SECRET_MARKER",
      "pg_dml_literal_marker", "PG_DML_LITERAL_MARKER",
      "pg_cursor_secret_marker", "PG_CURSOR_SECRET_MARKER"};
  return markers;
}

bool FirebirdChecks() {
  using scratchbird::parser::firebird::ParseStatement;
  using scratchbird::udr::firebird_parser_support::sbu_firebird_parse_to_sblr;
  bool ok = true;
  ok &= RunDmlCase(
      ParseStatement, sbu_firebird_parse_to_sblr,
      "update or insert into fb_dml_secret_marker (id, val) "
      "values (?, default) matching (id) returning id, val",
      {"firebird", "5.0.4",
       "019e13c0-0000-7000-8000-000000000302",
       "019e13c0-1500-7000-8000-000000000302",
       "firebird.dml_mutation_semantics_profile",
       "firebird.dml.update_or_insert.returning",
       "firebird_update_or_insert_matching_returning",
       "firebird_update_or_insert_matching_descriptor_uuid_key_required",
       "firebird_returning_projection_descriptor_single_or_multirow_by_statement_kind",
       "firebird_no_cursor_positioned_dml_observed",
       "firebird_row_count_update_or_insert_returning_descriptor_engine_reported",
       "firebird_update_or_insert_defaults_triggers_returning_descriptor_engine_order",
       false, false, false, true, false, false, true, false, false, false,
       false, true, false, true, false},
      FirebirdMarkers(), "firebird update or insert matching returning");
  ok &= RunDmlCase(
      ParseStatement, sbu_firebird_parse_to_sblr,
      "merge into fb_dml_secret_marker t using fb_dml_secret_marker_source s "
      "on (t.id = s.id) when matched then update set val = s.val "
      "when not matched then insert (id, val) values (s.id, "
      "'fb_dml_literal_marker') returning id",
      {"firebird", "5.0.4",
       "019e13c0-0000-7000-8000-000000000302",
       "019e13c0-1500-7000-8000-000000000302",
       "firebird.dml_mutation_semantics_profile",
       "firebird.dml.merge.returning",
       "firebird_merge_returning",
       "firebird_merge_descriptor_source_target_uuid_binding_required",
       "firebird_returning_projection_descriptor_single_or_multirow_by_statement_kind",
       "firebird_no_cursor_positioned_dml_observed",
       "firebird_row_count_descriptor_engine_reported_no_parser_finality",
       "firebird_defaults_triggers_generated_columns_descriptor_engine_order",
       false, false, false, false, false, true, false, false, false, false,
       false, true, false, false, false},
      FirebirdMarkers(), "firebird merge returning");
  ok &= RunDmlCase(
      ParseStatement, sbu_firebird_parse_to_sblr,
      "insert into fb_dml_secret_marker (id, val) values (?, "
      "'fb_dml_literal_marker') returning id",
      {"firebird", "5.0.4",
       "019e13c0-0000-7000-8000-000000000302",
       "019e13c0-1500-7000-8000-000000000302",
       "firebird.dml_mutation_semantics_profile",
       "firebird.dml.insert.returning", "firebird_insert_returning",
       "firebird_no_upsert_merge_surface_observed",
       "firebird_returning_projection_descriptor_single_or_multirow_by_statement_kind",
       "firebird_no_cursor_positioned_dml_observed",
       "firebird_row_count_descriptor_engine_reported_no_parser_finality",
       "firebird_defaults_triggers_generated_columns_descriptor_engine_order",
       true, false, false, false, false, false, false, false, false, false,
       false, true, false, false, false},
      FirebirdMarkers(), "firebird insert returning");
  ok &= RunDmlCase(
      ParseStatement, sbu_firebird_parse_to_sblr,
      "update fb_dml_secret_marker set val = default returning id",
      {"firebird", "5.0.4",
       "019e13c0-0000-7000-8000-000000000302",
       "019e13c0-1500-7000-8000-000000000302",
       "firebird.dml_mutation_semantics_profile",
       "firebird.dml.update.returning", "firebird_update_returning",
       "firebird_no_upsert_merge_surface_observed",
       "firebird_returning_projection_descriptor_single_or_multirow_by_statement_kind",
       "firebird_no_cursor_positioned_dml_observed",
       "firebird_row_count_descriptor_engine_reported_no_parser_finality",
       "firebird_defaults_triggers_generated_columns_descriptor_engine_order",
       false, true, false, false, false, false, false, false, false, false,
       false, true, false, true, false},
      FirebirdMarkers(), "firebird update returning");
  ok &= RunDmlCase(
      ParseStatement, sbu_firebird_parse_to_sblr,
      "delete from fb_dml_secret_marker where id = ? returning id",
      {"firebird", "5.0.4",
       "019e13c0-0000-7000-8000-000000000302",
       "019e13c0-1500-7000-8000-000000000302",
       "firebird.dml_mutation_semantics_profile",
       "firebird.dml.delete.returning", "firebird_delete_returning",
       "firebird_no_upsert_merge_surface_observed",
       "firebird_returning_projection_descriptor_single_or_multirow_by_statement_kind",
       "firebird_no_cursor_positioned_dml_observed",
       "firebird_row_count_descriptor_engine_reported_no_parser_finality",
       "firebird_defaults_triggers_generated_columns_descriptor_engine_order",
       false, false, true, false, false, false, false, false, false, false,
       false, true, false, false, false},
      FirebirdMarkers(), "firebird delete returning");
  ok &= RunDmlCase(
      ParseStatement, sbu_firebird_parse_to_sblr,
      "update fb_dml_secret_marker set val = ? where current of "
      "fb_cursor_secret_marker",
      {"firebird", "5.0.4",
       "019e13c0-0000-7000-8000-000000000302",
       "019e13c0-1500-7000-8000-000000000302",
       "firebird.dml_mutation_semantics_profile",
       "firebird.dml.cursor.update_current_of", "firebird_update_current_of",
       "firebird_no_upsert_merge_surface_observed",
       "firebird_no_returning_projection_observed",
       "firebird_where_current_of_cursor_descriptor_engine_cursor_authority",
       "firebird_row_count_descriptor_engine_reported_no_parser_finality",
       "firebird_trigger_default_generated_column_descriptor_required",
       false, true, false, false, false, false, false, false, false, false,
       false, false, true, false, false},
      FirebirdMarkers(), "firebird update current of");
  ok &= RunDmlCase(
      ParseStatement, sbu_firebird_parse_to_sblr,
      "delete from fb_dml_secret_marker where current of "
      "fb_cursor_secret_marker",
      {"firebird", "5.0.4",
       "019e13c0-0000-7000-8000-000000000302",
       "019e13c0-1500-7000-8000-000000000302",
       "firebird.dml_mutation_semantics_profile",
       "firebird.dml.cursor.delete_current_of", "firebird_delete_current_of",
       "firebird_no_upsert_merge_surface_observed",
       "firebird_no_returning_projection_observed",
       "firebird_where_current_of_cursor_descriptor_engine_cursor_authority",
       "firebird_row_count_descriptor_engine_reported_no_parser_finality",
       "firebird_trigger_default_generated_column_descriptor_required",
       false, false, true, false, false, false, false, false, false, false,
       false, false, true, false, false},
      FirebirdMarkers(), "firebird delete current of");
  return ok;
}

bool MysqlChecks() {
  using scratchbird::parser::mysql::ParseStatement;
  using scratchbird::udr::mysql_parser_support::sbu_mysql_parse_to_sblr;
  bool ok = true;
  ok &= RunDmlCase(
      ParseStatement, sbu_mysql_parse_to_sblr,
      "insert into mysql_dml_secret_marker (id, val) values "
      "(default, 'mysql_dml_literal_marker') on duplicate key update "
      "val = values(val)",
      {"mysql", "9.7.0",
       "019e13c0-0000-7000-8000-000000000303",
       "019e13c0-1500-7000-8000-000000000303",
       "mysql.dml_mutation_semantics_profile", "mysql.dml.insert",
       "mysql_insert_on_duplicate_key_update",
       "mysql_on_duplicate_key_update_descriptor_unique_probe_engine_authority",
       "mysql_no_native_dml_returning_projection_descriptor_rowcount_generated_keys_only",
       "mysql_no_native_where_current_of_descriptor",
       "mysql_on_duplicate_key_affected_rows_client_found_rows_sensitive_descriptor",
       "mysql_on_duplicate_defaults_generated_columns_triggers_descriptor_engine_order",
       true, false, false, false, false, false, false, true, false, false,
       false, false, false, true, false},
      MysqlMarkers(), "mysql insert on duplicate key update");
  ok &= RunDmlCase(
      ParseStatement, sbu_mysql_parse_to_sblr,
      "replace into mysql_dml_secret_marker (id, val) values "
      "(default, 'mysql_dml_literal_marker')",
      {"mysql", "9.7.0",
       "019e13c0-0000-7000-8000-000000000303",
       "019e13c0-1500-7000-8000-000000000303",
       "mysql.dml_mutation_semantics_profile", "mysql.dml.replace",
       "mysql_replace",
       "mysql_replace_delete_insert_semantics_descriptor_engine_executes",
       "mysql_no_native_dml_returning_projection_descriptor_rowcount_generated_keys_only",
       "mysql_no_native_where_current_of_descriptor",
       "mysql_replace_row_count_delete_plus_insert_descriptor_engine_reported",
       "mysql_replace_defaults_generated_columns_triggers_descriptor_engine_order",
       false, false, false, false, true, false, false, false, false, false,
       false, false, false, true, false},
      MysqlMarkers(), "mysql replace");
  ok &= RunDmlCase(
      ParseStatement, sbu_mysql_parse_to_sblr,
      "update mysql_dml_secret_marker set generated_col = default "
      "where id = ?",
      {"mysql", "9.7.0",
       "019e13c0-0000-7000-8000-000000000303",
       "019e13c0-1500-7000-8000-000000000303",
       "mysql.dml_mutation_semantics_profile", "mysql.dml.update",
       "mysql_update", "mysql_no_upsert_surface_observed",
       "mysql_no_native_dml_returning_projection_descriptor_rowcount_generated_keys_only",
       "mysql_no_native_where_current_of_descriptor",
       "mysql_affected_rows_descriptor_client_found_rows_profile_bound",
       "mysql_defaults_generated_columns_trigger_descriptor_engine_order",
       false, true, false, false, false, false, false, false, false, false,
       false, false, false, true, false},
      MysqlMarkers(), "mysql update default interaction");
  ok &= RunDmlCase(
      ParseStatement, sbu_mysql_parse_to_sblr,
      "delete from mysql_dml_secret_marker where id = ?",
      {"mysql", "9.7.0",
       "019e13c0-0000-7000-8000-000000000303",
       "019e13c0-1500-7000-8000-000000000303",
       "mysql.dml_mutation_semantics_profile", "mysql.dml.delete",
       "mysql_delete", "mysql_no_upsert_surface_observed",
       "mysql_no_native_dml_returning_projection_descriptor_rowcount_generated_keys_only",
       "mysql_no_native_where_current_of_descriptor",
       "mysql_affected_rows_descriptor_client_found_rows_profile_bound",
       "mysql_trigger_default_generated_column_descriptor_required",
       false, false, true, false, false, false, false, false, false, false,
       false, false, false, false, false},
      MysqlMarkers(), "mysql delete");
  return ok;
}

bool PostgresqlChecks() {
  using scratchbird::parser::postgresql::ParseStatement;
  using scratchbird::udr::postgresql_parser_support::sbu_postgresql_parse_to_sblr;
  bool ok = true;
  ok &= RunDmlCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "insert into pg_dml_secret_marker (id, val) values "
      "(default, 'pg_dml_literal_marker') on conflict (id) do update "
      "set val = excluded.val returning id",
      {"postgresql", "18.3",
       "019e13c0-0000-7000-8000-000000000304",
       "019e13c0-1500-7000-8000-000000000304",
       "postgresql.dml_mutation_semantics_profile", "postgresql.dml.insert",
       "postgresql_insert_on_conflict_do_update",
       "postgresql_on_conflict_do_update_descriptor_inference_uuid_required",
       "postgresql_returning_projection_descriptor_result_relation_uuid_bound",
       "postgresql_no_cursor_positioned_dml_observed",
       "postgresql_command_tag_row_count_descriptor_engine_reported",
       "postgresql_on_conflict_defaults_generated_columns_triggers_descriptor_engine_order",
       true, false, false, false, false, false, false, false, true, true,
       false, true, false, true, false},
      PostgresqlMarkers(), "postgresql insert on conflict do update returning");
  ok &= RunDmlCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "insert into pg_dml_secret_marker (id, val) values "
      "(?, 'pg_dml_literal_marker') on conflict (id) do nothing returning id",
      {"postgresql", "18.3",
       "019e13c0-0000-7000-8000-000000000304",
       "019e13c0-1500-7000-8000-000000000304",
       "postgresql.dml_mutation_semantics_profile", "postgresql.dml.insert",
       "postgresql_insert_on_conflict_do_nothing",
       "postgresql_on_conflict_do_nothing_descriptor_inference_uuid_required",
       "postgresql_returning_projection_descriptor_result_relation_uuid_bound",
       "postgresql_no_cursor_positioned_dml_observed",
       "postgresql_command_tag_row_count_descriptor_engine_reported",
       "postgresql_on_conflict_defaults_generated_columns_triggers_descriptor_engine_order",
       true, false, false, false, false, false, false, false, true, false,
       true, true, false, false, false},
      PostgresqlMarkers(), "postgresql insert on conflict do nothing returning");
  ok &= RunDmlCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "merge into pg_dml_secret_marker t using pg_dml_secret_marker_source s "
      "on t.id = s.id when matched then update set val = s.val "
      "when not matched then insert (id, val) values (s.id, "
      "'pg_dml_literal_marker') returning id",
      {"postgresql", "18.3",
       "019e13c0-0000-7000-8000-000000000304",
       "019e13c0-1500-7000-8000-000000000304",
       "postgresql.dml_mutation_semantics_profile", "postgresql.dml.merge",
       "postgresql_merge_returning",
       "postgresql_merge_descriptor_source_target_uuid_binding_required",
       "postgresql_returning_projection_descriptor_result_relation_uuid_bound",
       "postgresql_no_cursor_positioned_dml_observed",
       "postgresql_command_tag_row_count_descriptor_engine_reported",
       "postgresql_defaults_generated_columns_triggers_returning_descriptor_engine_order",
       false, false, false, false, false, true, false, false, false, false,
       false, true, false, false, false},
      PostgresqlMarkers(), "postgresql merge returning");
  ok &= RunDmlCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "update pg_dml_secret_marker set generated_col = default "
      "where id = ? returning id",
      {"postgresql", "18.3",
       "019e13c0-0000-7000-8000-000000000304",
       "019e13c0-1500-7000-8000-000000000304",
       "postgresql.dml_mutation_semantics_profile", "postgresql.dml.update",
       "postgresql_update_returning",
       "postgresql_no_conflict_or_merge_surface_observed",
       "postgresql_returning_projection_descriptor_result_relation_uuid_bound",
       "postgresql_no_cursor_positioned_dml_observed",
       "postgresql_command_tag_row_count_descriptor_engine_reported",
       "postgresql_defaults_generated_columns_triggers_returning_descriptor_engine_order",
       false, true, false, false, false, false, false, false, false, false,
       false, true, false, true, false},
      PostgresqlMarkers(), "postgresql update returning default interaction");
  ok &= RunDmlCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "delete from pg_dml_secret_marker where id = ? returning id",
      {"postgresql", "18.3",
       "019e13c0-0000-7000-8000-000000000304",
       "019e13c0-1500-7000-8000-000000000304",
       "postgresql.dml_mutation_semantics_profile", "postgresql.dml.delete",
       "postgresql_delete_returning",
       "postgresql_no_conflict_or_merge_surface_observed",
       "postgresql_returning_projection_descriptor_result_relation_uuid_bound",
       "postgresql_no_cursor_positioned_dml_observed",
       "postgresql_command_tag_row_count_descriptor_engine_reported",
       "postgresql_defaults_generated_columns_triggers_returning_descriptor_engine_order",
       false, false, true, false, false, false, false, false, false, false,
       false, true, false, false, false},
      PostgresqlMarkers(), "postgresql delete returning");
  ok &= RunDmlCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "update pg_dml_secret_marker set val = ? where current of "
      "pg_cursor_secret_marker",
      {"postgresql", "18.3",
       "019e13c0-0000-7000-8000-000000000304",
       "019e13c0-1500-7000-8000-000000000304",
       "postgresql.dml_mutation_semantics_profile", "postgresql.dml.update",
       "postgresql_update_current_of",
       "postgresql_no_conflict_or_merge_surface_observed",
       "postgresql_no_returning_projection_observed",
       "postgresql_where_current_of_cursor_descriptor_engine_cursor_authority",
       "postgresql_command_tag_row_count_descriptor_engine_reported",
       "postgresql_trigger_default_generated_column_descriptor_required",
       false, true, false, false, false, false, false, false, false, false,
       false, false, true, false, false},
      PostgresqlMarkers(), "postgresql update current of");
  ok &= RunDmlCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "delete from pg_dml_secret_marker where current of "
      "pg_cursor_secret_marker",
      {"postgresql", "18.3",
       "019e13c0-0000-7000-8000-000000000304",
       "019e13c0-1500-7000-8000-000000000304",
       "postgresql.dml_mutation_semantics_profile", "postgresql.dml.delete",
       "postgresql_delete_current_of",
       "postgresql_no_conflict_or_merge_surface_observed",
       "postgresql_no_returning_projection_observed",
       "postgresql_where_current_of_cursor_descriptor_engine_cursor_authority",
       "postgresql_command_tag_row_count_descriptor_engine_reported",
       "postgresql_trigger_default_generated_column_descriptor_required",
       false, false, true, false, false, false, false, false, false, false,
       false, false, true, false, false},
      PostgresqlMarkers(), "postgresql delete current of");
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
