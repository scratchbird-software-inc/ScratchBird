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

struct LocksIsolationExpected {
  std::string_view dialect;
  std::string_view release_profile;
  std::string_view donor_profile_uuid;
  std::string_view semantic_profile_uuid;
  std::string_view locks_isolation_profile;
  std::string_view locks_isolation_surface;
  std::string_view isolation_profile_uuid_or_policy;
  std::string_view lock_clause_policy;
  std::string_view nowait_policy;
  std::string_view skip_locked_policy;
  std::string_view advisory_lock_policy;
  std::string_view table_lock_policy;
  std::string_view row_lock_policy;
  std::string_view read_write_policy;
  std::string_view deadlock_diagnostic_policy;
  std::string_view diagnostic_map_ref;
  std::string_view sandbox_root_policy;
  std::string_view statement_family;
  std::string_view operation_family;
  bool isolation_surface{false};
  bool lock_table_surface{false};
  bool row_lock_surface{false};
  bool for_update_surface{false};
  bool for_share_surface{false};
  bool nowait_surface{false};
  bool skip_locked_surface{false};
  bool advisory_lock_surface{false};
  bool read_only_surface{false};
  bool read_write_surface{false};
  bool deadlock_diagnostic_surface{false};
  bool transaction_surface{false};
  bool query_surface{false};
  bool session_surface{false};
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
                     " leaked locks/isolation source marker: " +
                     std::string(marker));
  }
  return ok;
}

bool ExpectNoMysqlLts(std::string_view payload, std::string_view label) {
  bool ok = true;
  ok &= Expect(!Contains(payload, "mysql_lts"),
               std::string(label) + " emitted mysql_lts runtime evidence");
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
                             const LocksIsolationExpected& expected,
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
  ok &= ExpectBool(payload, "donor_engine_sql_executed", false, label);
  ok &= ExpectBool(payload, "sql_text_included", false, label);
  ok &= ExpectReadinessComplete(payload, label);
  return ok;
}

bool ExpectLocksIsolationEvidence(std::string_view payload,
                                  const LocksIsolationExpected& expected,
                                  std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload, "\"locks_isolation_semantic_evidence\":{"),
               std::string(label) +
                   " missing locks/isolation semantic evidence");
  ok &= ExpectField(
      payload, "evidence_contract",
      "donor_locks_isolation_semantic_descriptor_evidence.v1", label);
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required", label);
  ok &= ExpectField(payload, "donor_profile_uuid",
                    expected.donor_profile_uuid, label);
  ok &= ExpectField(payload, "semantic_profile_uuid",
                    expected.semantic_profile_uuid, label);
  ok &= ExpectField(payload, "dialect", expected.dialect, label);
  ok &= ExpectField(payload, "release_profile", expected.release_profile,
                    label);
  ok &= ExpectField(payload, "locks_isolation_profile",
                    expected.locks_isolation_profile, label);
  ok &= ExpectField(payload, "locks_isolation_surface",
                    expected.locks_isolation_surface, label);
  ok &= ExpectField(payload, "isolation_profile_uuid_or_policy",
                    expected.isolation_profile_uuid_or_policy, label);
  ok &= ExpectField(payload, "lock_clause_policy",
                    expected.lock_clause_policy, label);
  ok &= ExpectField(payload, "nowait_policy", expected.nowait_policy, label);
  ok &= ExpectField(payload, "skip_locked_policy",
                    expected.skip_locked_policy, label);
  ok &= ExpectField(payload, "advisory_lock_policy",
                    expected.advisory_lock_policy, label);
  ok &= ExpectField(payload, "table_lock_policy",
                    expected.table_lock_policy, label);
  ok &= ExpectField(payload, "row_lock_policy", expected.row_lock_policy,
                    label);
  ok &= ExpectField(payload, "read_write_policy", expected.read_write_policy,
                    label);
  ok &= ExpectField(payload, "deadlock_diagnostic_policy",
                    expected.deadlock_diagnostic_policy, label);
  ok &= ExpectField(payload, "diagnostic_map_ref",
                    expected.diagnostic_map_ref, label);
  ok &= ExpectField(payload, "sandbox_root_policy",
                    expected.sandbox_root_policy, label);
  ok &= ExpectBool(payload, "isolation_surface", expected.isolation_surface,
                   label);
  ok &= ExpectBool(payload, "lock_table_surface",
                   expected.lock_table_surface, label);
  ok &= ExpectBool(payload, "row_lock_surface", expected.row_lock_surface,
                   label);
  ok &= ExpectBool(payload, "for_update_surface",
                   expected.for_update_surface, label);
  ok &= ExpectBool(payload, "for_share_surface", expected.for_share_surface,
                   label);
  ok &= ExpectBool(payload, "nowait_surface", expected.nowait_surface, label);
  ok &= ExpectBool(payload, "skip_locked_surface",
                   expected.skip_locked_surface, label);
  ok &= ExpectBool(payload, "advisory_lock_surface",
                   expected.advisory_lock_surface, label);
  ok &= ExpectBool(payload, "read_only_surface", expected.read_only_surface,
                   label);
  ok &= ExpectBool(payload, "read_write_surface", expected.read_write_surface,
                   label);
  ok &= ExpectBool(payload, "deadlock_diagnostic_surface",
                   expected.deadlock_diagnostic_surface, label);
  ok &= ExpectBool(payload, "transaction_surface",
                   expected.transaction_surface, label);
  ok &= ExpectBool(payload, "query_surface", expected.query_surface, label);
  ok &= ExpectBool(payload, "session_surface", expected.session_surface, label);
  ok &= ExpectBool(payload, "uuid_required_semantic_profile", true, label);
  ok &= ExpectBool(payload, "catalog_descriptor_required", true, label);
  ok &= ExpectBool(payload, "lock_descriptor_required", true, label);
  ok &= ExpectBool(payload, "isolation_descriptor_required", true, label);
  ok &= ExpectBool(payload, "sblr_operation_uuid_resolution_required", true,
                   label);
  ok &= ExpectField(payload, "engine_authority", "scratchbird", label);
  ok &= ExpectField(payload, "lock_authority",
                    "engine_lock_manager_authority", label);
  ok &= ExpectField(payload, "isolation_authority",
                    "engine_mga_isolation_profile_authority", label);
  ok &= ExpectField(payload, "transaction_authority",
                    "engine_mga_transaction_authority", label);
  ok &= ExpectField(payload, "deadlock_authority",
                    "engine_lock_manager_diagnostic_authority", label);
  ok &= ExpectField(payload, "catalog_projection_authority",
                    "engine_catalog_uuid_projection", label);
  ok &= ExpectBool(payload, "source_sql_text_included", false, label);
  ok &= ExpectBool(payload, "literal_text_included", false, label);
  ok &= ExpectBool(payload, "object_name_text_included", false, label);
  ok &= ExpectBool(payload, "quoted_identifier_text_included", false, label);
  ok &= ExpectBool(payload, "sblr_embeds_source_identifiers", false, label);
  ok &= ExpectBool(payload, "parser_lock_authority", false, label);
  ok &= ExpectBool(payload, "parser_isolation_authority", false, label);
  ok &= ExpectBool(payload, "parser_deadlock_authority", false, label);
  ok &= ExpectBool(payload, "parser_catalog_authority", false, label);
  ok &= ExpectBool(payload, "parser_storage_authority", false, label);
  ok &= ExpectBool(payload, "parser_execution_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_finality_authority", false,
                   label);
  ok &= ExpectBool(payload, "parser_visibility_authority", false, label);
  ok &= ExpectBool(payload, "parser_runtime_semantic_equivalence_authority",
                   false, label);
  ok &= ExpectBool(payload, "donor_sql_executed", false, label);
  ok &= ExpectField(payload, "runtime_semantic_equivalence",
                    "not_enterprise_proven_pending", label);
  ok &= ExpectField(
      payload, "descriptor_exactness_status",
      "parser_locks_isolation_descriptor_recorded_runtime_equivalence_pending",
      label);
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready",
                    label);
  ok &= Expect(!Contains(payload, "parser_lock_authority\":true"),
               std::string(label) + " granted parser lock authority");
  ok &= Expect(!Contains(payload, "parser_isolation_authority\":true"),
               std::string(label) + " granted parser isolation authority");
  ok &= Expect(!Contains(payload, "parser_deadlock_authority\":true"),
               std::string(label) + " granted parser deadlock authority");
  ok &= Expect(!Contains(payload, "donor_sql_executed\":true"),
               std::string(label) + " executed donor SQL");
  return ok;
}

template <typename Result>
bool ExpectDirectResult(const Result& result,
                        const LocksIsolationExpected& expected,
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
  ok &= ExpectLocksIsolationEvidence(result.parser_evidence_json, expected,
                                     label);
  ok &= ExpectEnvelopeAuthority(result.sblr_envelope, expected, label);
  ok &= ExpectLocksIsolationEvidence(result.sblr_envelope, expected, label);
  ok &= ExpectNoMarkers(result.parser_evidence_json, markers, label);
  ok &= ExpectNoMarkers(result.sblr_envelope, markers, label);
  ok &= ExpectNoMysqlLts(result.parser_evidence_json, label);
  ok &= ExpectNoMysqlLts(result.sblr_envelope, label);
  return ok;
}

template <typename UdrResult>
bool ExpectUdrResult(const UdrResult& result,
                     const LocksIsolationExpected& expected,
                     std::span<const std::string_view> markers,
                     std::string_view label) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " UDR parse failed: " +
                             result.message_vector_json);
  ok &= ExpectEnvelopeAuthority(result.payload, expected, label);
  ok &= ExpectLocksIsolationEvidence(result.payload, expected, label);
  ok &= ExpectNoMarkers(result.payload, markers, label);
  ok &= ExpectNoMysqlLts(result.payload, label);
  return ok;
}

template <typename DirectParser, typename UdrParser>
bool RunCase(DirectParser direct_parser,
             UdrParser udr_parser,
             std::string_view sql,
             const LocksIsolationExpected& expected,
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

std::span<const std::string_view> FirebirdMarkers() {
  static constexpr std::string_view markers[] = {"fb_lock_marker",
                                                 "FB_LOCK_MARKER"};
  return markers;
}

std::span<const std::string_view> MysqlMarkers() {
  static constexpr std::string_view markers[] = {"mysql_lock_marker",
                                                 "MYSQL_LOCK_MARKER"};
  return markers;
}

std::span<const std::string_view> PostgresqlMarkers() {
  static constexpr std::string_view markers[] = {"pg_lock_marker",
                                                 "PG_LOCK_MARKER"};
  return markers;
}

bool FirebirdChecks() {
  using scratchbird::parser::firebird::ParseStatement;
  using scratchbird::udr::firebird_parser_support::sbu_firebird_parse_to_sblr;
  return RunCase(
      ParseStatement, sbu_firebird_parse_to_sblr,
      "select * from fb_lock_marker for update",
      {"firebird",
       "5.0.4",
       "019e13c0-0000-7000-8000-000000000302",
       "019e13c0-1c00-7000-8000-000000000302",
       "firebird.locks_isolation_syntax_semantics_profile",
       "firebird_select_for_update_row_lock_descriptor",
       "firebird_tpb_isolation_descriptor_uuid_required_engine_mga_authority",
       "firebird_for_update_wait_no_wait_descriptor_engine_lock_authority",
       "firebird_nowait_tpb_descriptor_engine_lock_wait_policy",
       "firebird_skip_locked_not_supported_descriptor_refusal_policy",
       "firebird_no_advisory_lock_surface_descriptor_refusal_policy",
       "firebird_explicit_table_lock_not_supported_descriptor_refusal_policy",
       "firebird_for_update_descriptor_engine_cursor_lock_authority",
       "firebird_read_only_read_write_tpb_descriptor_engine_intent_authority",
       "firebird_deadlock_diagnostic_map_descriptor_engine_lock_manager_authority",
       "firebird_locks_isolation_semantics_diagnostic_map",
       "firebird_donor_schema_root_uuid_required_no_cross_root_temp_access",
       "query",
       "firebird.query.cursor.for_update",
       false, false, true, true, false, false, false, false, false, false,
       false, false, true, false},
      FirebirdMarkers(), "firebird locks/isolation descriptor");
}

bool MysqlChecks() {
  using scratchbird::parser::mysql::ParseStatement;
  using scratchbird::udr::mysql_parser_support::sbu_mysql_parse_to_sblr;
  return RunCase(
      ParseStatement, sbu_mysql_parse_to_sblr,
      "lock tables mysql_lock_marker read",
      {"mysql",
       "9.7.0",
       "019e13c0-0000-7000-8000-000000000303",
       "019e13c0-1c00-7000-8000-000000000303",
       "mysql.locks_isolation_syntax_semantics_profile",
       "mysql_lock_tables_table_lock_descriptor",
       "mysql_transaction_isolation_descriptor_uuid_required_engine_mga_authority",
       "mysql_lock_tables_and_for_update_descriptor_engine_lock_authority",
       "mysql_nowait_descriptor_engine_lock_wait_policy",
       "mysql_skip_locked_descriptor_engine_row_lock_policy",
       "mysql_get_lock_release_lock_advisory_descriptor_engine_policy",
       "mysql_lock_tables_descriptor_engine_lock_authority",
       "mysql_for_update_for_share_descriptor_engine_row_lock_authority",
       "mysql_read_only_read_write_descriptor_engine_intent_authority",
       "mysql_deadlock_diagnostic_map_descriptor_engine_lock_manager_authority",
       "mysql_locks_isolation_semantics_diagnostic_map",
       "mysql_connected_database_root_uuid_required_temp_shadowing_root_local",
       "locking",
       "mysql.locking.lock_tables",
       false, true, false, false, false, false, false, false, false, false,
       false, false, false, false},
      MysqlMarkers(), "mysql locks/isolation descriptor");
}

bool PostgresqlLockTableChecks() {
  using scratchbird::parser::postgresql::ParseStatement;
  using scratchbird::udr::postgresql_parser_support::
      sbu_postgresql_parse_to_sblr;
  return RunCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "lock table pg_lock_marker in share mode nowait",
      {"postgresql",
       "18.3",
       "019e13c0-0000-7000-8000-000000000304",
       "019e13c0-1c00-7000-8000-000000000304",
       "postgresql.locks_isolation_syntax_semantics_profile",
       "postgresql_lock_table_mode_nowait_descriptor",
       "postgresql_transaction_isolation_descriptor_uuid_required_engine_mga_authority",
       "postgresql_lock_table_and_row_lock_descriptor_engine_lock_authority",
       "postgresql_nowait_descriptor_engine_lock_wait_policy",
       "postgresql_skip_locked_descriptor_engine_row_lock_policy",
       "postgresql_pg_advisory_lock_descriptor_engine_policy",
       "postgresql_lock_table_mode_descriptor_engine_lock_authority",
       "postgresql_for_update_for_share_descriptor_engine_row_lock_authority",
       "postgresql_read_only_read_write_descriptor_engine_intent_authority",
       "postgresql_deadlock_diagnostic_map_descriptor_engine_lock_manager_authority",
       "postgresql_locks_isolation_semantics_diagnostic_map",
       "postgresql_connected_database_schema_root_uuid_required_pg_temp_root_local",
       "locking",
       "postgresql.locking.lock_table",
       false, true, false, false, false, true, false, false, false, false,
       false, false, false, false},
      PostgresqlMarkers(), "postgresql lock table descriptor");
}

bool PostgresqlRowLockChecks() {
  using scratchbird::parser::postgresql::ParseStatement;
  using scratchbird::udr::postgresql_parser_support::
      sbu_postgresql_parse_to_sblr;
  return RunCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "select * from pg_lock_marker for update skip locked",
      {"postgresql",
       "18.3",
       "019e13c0-0000-7000-8000-000000000304",
       "019e13c0-1c00-7000-8000-000000000304",
       "postgresql.locks_isolation_syntax_semantics_profile",
       "postgresql_select_for_update_row_lock_descriptor",
       "postgresql_transaction_isolation_descriptor_uuid_required_engine_mga_authority",
       "postgresql_lock_table_and_row_lock_descriptor_engine_lock_authority",
       "postgresql_nowait_descriptor_engine_lock_wait_policy",
       "postgresql_skip_locked_descriptor_engine_row_lock_policy",
       "postgresql_pg_advisory_lock_descriptor_engine_policy",
       "postgresql_lock_table_mode_descriptor_engine_lock_authority",
       "postgresql_for_update_for_share_descriptor_engine_row_lock_authority",
       "postgresql_read_only_read_write_descriptor_engine_intent_authority",
       "postgresql_deadlock_diagnostic_map_descriptor_engine_lock_manager_authority",
       "postgresql_locks_isolation_semantics_diagnostic_map",
       "postgresql_connected_database_schema_root_uuid_required_pg_temp_root_local",
       "query",
       "postgresql.query.select",
       false, false, true, true, false, false, true, false, false, false,
       false, false, true, false},
      PostgresqlMarkers(), "postgresql row lock descriptor");
}

} // namespace

int main() {
  bool ok = true;
  ok &= FirebirdChecks();
  ok &= MysqlChecks();
  ok &= PostgresqlLockTableChecks();
  ok &= PostgresqlRowLockChecks();
  if (!ok) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
